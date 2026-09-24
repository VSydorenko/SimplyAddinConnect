//
// @file tests/uapki_fiscal_emulator.cpp
// @brief HTTP-оракул ЕЦП: грає «приймаючу сторону» (сервер ДПС/ЄВПЕЗ) для тестування
//        крипто-стека UAPKI з РЕАЛЬНОЇ 1С.
//
//        UAPKI — не мережевий пристрій, а in-process крипто-бібліотека, тож «емулювати»
//        тут нічого: консоль виступає ОРАКУЛОМ. 1С підписує документ компонентою й шле
//        CMS по HTTP; консоль незалежним шляхом інтеграції (process()/json_free()
//        статично злінкованого ядра) перевіряє підпис і повертає підписану квитанцію,
//        яку 1С має розгорнути назад. Це cross-path-валідація шарів інтеграції
//        (маршалінг/серіалізація), а не незалежна крипто-реалізація — рушій той самий.
//
//        Це окремий консольний exe: src/core/pch.h НЕ підключається, макроси REPORT_*
//        не застосовні, звичайний printf дозволено (правило tests/).
//
// Ендпоінти:
//   GET  /ping                                — живість
//   POST /doc      тіло = DER CMS             — VERIFY + підписана квитанція (DER CMS)
//   POST /verify   тіло = DER CMS             — VERIFY, відповідь JSON (завжди)
//   GET  /reference?type=check|zrep|ticket    — еталонний підписаний документ (DER CMS)
//
// CLI: uapki_fiscal_emulator [port] [--key <p12>] [--pass <pwd>] [--providers <dir>]
//                            [--data <dir>] [--samples <dir>] [--canned] [--self-test]
// Коди повернення: 0 — штатно; 1 — не піднявся порт / провал self-test; 2 — CLI/bootstrap.
//

// NOMINMAX/WIN32_LEAN_AND_MEAN — до БУДЬ-ЯКИХ інклюдів: MiniHttpServer.h тягне
// winsock2.h, а той далі windows.h, тож задати їх пізніше вже не вийде.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// Власний мінімальний HTTP-сервер — ПЕРЕД windows.h (winsock2 має потрапити
// раніше, інакше windows.h підтягне старий winsock.h і посиплються редефініції
// сокет-типів). Раніше тут стояв ix::HttpServer; ixwebsocket видалено з репозиторію.
#include "support/MiniHttpServer.h"
#include "support/UapkiOracle.h"

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW (windows.h не тягне його при WIN32_LEAN_AND_MEAN)

#pragma comment(lib, "shell32.lib")  // CommandLineToArgvW

using nlohmann::json;

// Дефолтний каталог тест-даних (certs/ + crls/ + test-diia.p12), впаяний CMake.
#ifndef HOST_DATA_DIR
#  define HOST_DATA_DIR ""
#endif

// Підкаталог зі зразками ПРРО всередині prro_docs (кирилиця у wide-літералі коректна
// завдяки /utf-8: MSVC читає вихідник як UTF-8 і кодує L"" в UTF-16).
static const wchar_t* SAMPLES_SUBDIR =
    L"\\Єдине вікно подання електронної звітності\\Приклади\\Приклади з КЕП";

// Дані позитивного self-test (шукаються всередині DER, щоб точково зіпсувати вміст).
static const char* SELFTEST_DATA = "hello world";

// Ліміт тіла запиту (спека §3.2: норматив ЄВПЕЗ обмежує пакет 200 КБ, беремо із запасом).
static const size_t MAX_BODY_BYTES = 1024 * 1024;

// ============================================================================
// Крипто — у спільному модулі tests/support/UapkiOracle (спека prro_fs_emulator §9).
// Перехідники зберігають імена, під якими крипто кличеться в цьому файлі.
// ============================================================================
using oracle::VerifyOutcome;
using oracle::u8to16;
using oracle::w2u8;
using oracle::b64decode;
static VerifyOutcome verifyCms(const std::string& der) { return oracle::Verify(der); }
static std::string   signData(const std::string& raw)  { return oracle::Sign(raw); }
static void          removeWorkDir()                   { oracle::Shutdown(); }


// ============================================================================
// Файли й шляхи
// ============================================================================
static bool dirExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD a = GetFileAttributesW(path.c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && ((a & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

// Каталог самого exe. Саме туди build кладе cm-pkcs12_*.dll (bin/Release), тож це
// найнадійніший дефолт для --providers: exe і провайдер завжди їдуть разом, а
// compile-time шлях до bin/Release зламався б при копіюванні збірки кудись інде.
static std::wstring exeDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring p(buf, n);
    const size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : p.substr(0, pos);
}

static bool readFileBin(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (LONGLONG)MAX_BODY_BYTES) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    bool ok = true;
    if (!out.empty()) {
        DWORD rd = 0;
        ok = (ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr) != 0) && (rd == out.size());
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}


// ============================================================================
// Конфіг і глобальний стан
// ============================================================================
struct Config {
    // 8099, а НЕ 8080: останній майже завжди зайнятий (Tomcat/Jenkins/dev-сервери), і тоді
    // автопідбір піднімав оракула на сусідньому порту, а обробка 1С зі своїм дефолтом стукала
    // в чужий сервіс і отримувала таймаут. Це значення має збігатися з реквізитом
    // «Порт консолі» тестової обробки.
    int          port = 8099;
    std::wstring keyPath;        // .p12-контейнер тест-ключа
    std::wstring providersDir;   // каталог з cm-pkcs12_*.dll
    std::wstring dataDir;        // certs/ + crls/ + test-diia.p12
    std::wstring samplesDir;     // «Приклади з КЕП» (для --canned); може бути порожнім
    std::string  pass = "testpassword";
    bool         canned   = false;
    bool         selfTest = false;
};

// Запит на завершення від ctrl-хендлера: головний потік чекає на цій парі з ПРЕДИКАТОМ
// (див. коментар перед очікуванням у main()).
static std::mutex               g_stopMx;
static std::condition_variable  g_stopCv;
static bool                     g_stopRequested = false;   // guarded by g_stopMx

// Ctrl-C / закриття вікна: мінімум роботи — розбудити головний потік (щоб штатний шлях
// завершення лишався досяжним) і прибрати temp-каталог; далі повертаємо FALSE, щоб
// спрацював дефолтний термінатор процесу.
static BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
        { std::lock_guard<std::mutex> lk(g_stopMx); g_stopRequested = true; }
        g_stopCv.notify_all();
        removeWorkDir();
    }
    return FALSE;
}


// Мінімальне XML-екранування (статуси UAPKI — ASCII-константи, але текст помилки
// потрапляє в XML квитанції, тож екрануємо на загальних підставах).
static std::string xmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

// ticket01-подібний каркас квитанції ДПС. Оголошене кодування — windows-1251 (як у
// нормативі), тому текст помилки тримаємо в ASCII: інакше UTF-8-байти суперечили б
// декларації й документ став би невалідним XML для приймача.
static std::string buildTicketXml(int errorCode, const std::string& errorTextAscii) {
    std::string x = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\r\n<TICKET>";
    x += "<UID>00000000-0000-0000-0000-000000000001</UID>";
    x += "<ORDERDATE>22072026</ORDERDATE><ORDERTIME>120000</ORDERTIME>";
    x += "<ERRORCODE>" + std::to_string(errorCode) + "</ERRORCODE>";
    x += "<ERRORTEXT>" + xmlEscape(errorTextAscii) + "</ERRORTEXT>";
    x += "<VER>1</VER></TICKET>";
    return x;
}


// ============================================================================
// HTTP-хелпери
// ============================================================================
static const char* httpReason(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 500: return "Internal Server Error";
        default:  return "Status";
    }
}

static minihttp::Response httpResp(int code, const std::string& ctype, const std::string& body) {
    std::printf("  → %d %s (%zu B)\n", code, httpReason(code), body.size());
    std::fflush(stdout);
    minihttp::Response r;
    r.code        = code;
    r.reason      = httpReason(code);
    r.contentType = ctype;
    r.body        = body;
    return r;
}

// Значення параметра query-рядка: рівно до '&' або до кінця (не «наосліп до кінця»).
static std::string queryValue(const std::string& query, const std::string& key) {
    const std::string pref = key + "=";
    size_t pos = 0;
    while (pos < query.size()) {
        size_t end = query.find('&', pos);
        if (end == std::string::npos) end = query.size();
        if (end - pos >= pref.size() && query.compare(pos, pref.size(), pref) == 0)
            return query.substr(pos + pref.size(), end - pos - pref.size());
        pos = end + 1;
    }
    return std::string();
}

// Безпечний для консолі прев'ю-рядок: керівні байти -> пробіл, обрізка без розриву UTF-8.
static std::string previewText(const std::string& s, size_t maxLen) {
    size_t n = (s.size() < maxLen) ? s.size() : maxLen;
    while (n > 0 && n < s.size() && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
    std::string out;
    out.reserve(n + 3);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)s[i];
        out.push_back((c < 0x20 || c == 0x7F) ? ' ' : (char)c);
    }
    if (n < s.size()) out += "...";
    return out;
}


// ============================================================================
// Зовнішній суддя self-test'у: iit_verify_x86.exe окремим процесом
// ============================================================================
// Досі self-test замикав коло сам на себе: signData нашим стеком -> verifyCms нашим
// же. Ця функція розмикає коло — віддає CMS НЕЗАЛЕЖНОМУ двигуну (нативна бібліотека
// АТ «ІІТ», інша кодова база; докладніше tests/iit_verify.cpp).
// Повертає код виходу iit_verify: 0 валідний | 1 невалідний | 2 помилка | 3 SKIP.
// Не знайшли арбітра, не змогли записати тимчасовий файл чи запустити процес ->
// 3 (SKIP), НІКОЛИ не мовчазний успіх.
static int judgeByIit(const std::string& cms) {
    wchar_t tmpDir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tmpDir) == 0) return 3;

    // PID + монотонний лічильник процесу: self-test кличе цю функцію мінімум двічі
    // (good/bad) з ОДНОГО процесу, а x86- і x64-варіанти оракула самостійні процеси
    // з різними PID, тож цього досить для унікальності імені без COM/GUID-залежності.
    // Фіксоване ім'я тут уже кусало гонкою на паралельних прогонах (нотатка в брифі).
    static volatile LONG s_seq = 0;
    const LONG seq = InterlockedIncrement(&s_seq);
    const std::wstring tmp = std::wstring(tmpDir) + L"sac_judge_"
                           + std::to_wstring(GetCurrentProcessId()) + L"_"
                           + std::to_wstring(seq) + L".p7s";

    {
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return 3;
        DWORD w = 0;
        const bool ok = WriteFile(h, cms.data(), (DWORD)cms.size(), &w, nullptr) != 0
                     && w == cms.size();
        CloseHandle(h);
        if (!ok) { DeleteFileW(tmp.c_str()); return 3; }
    }

    // Арбітр лежить поруч з оракулом (bin/Release, ціль лише x86 — EUSignCP.dll
    // 32-бітна). Не знайшли -> SKIP, а не мовчазний PASS.
    const std::wstring exe = exeDir() + L"\\iit_verify_x86.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(tmp.c_str());
        return 3;
    }

    std::wstring cmd = L"\"" + exe + L"\" \"" + tmp + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW;   // CREATE_NO_WINDOW + SW_HIDE: без блимання вікна
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DeleteFileW(tmp.c_str());
        return 3;
    }

    // Таймаут 120 с (як у tests/support/IitStore.cpp — холодний старт арбітра тягне
    // бандл ЦЗО ~9 с, запас достатній). На таймауті TerminateProcess ОБОВ'ЯЗКОВИЙ:
    // інакше завислий дочірній процес тримає tmp відкритим, і DeleteFileW нижче
    // мовчки провалиться, лишивши тимчасовий файл (пастка вже ловилась у задачі 7).
    int rc = 3;
    if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);   // доочікування звільнення tmp
    } else {
        DWORD ec = 3;
        GetExitCodeProcess(pi.hProcess, &ec);
        rc = (int)ec;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DeleteFileW(tmp.c_str());   // прибрати на ВСІХ гілках
    return rc;
}


// ============================================================================
// --self-test (без 1С і без піднятого сервера)
// ============================================================================
static int runSelfTest(const Config& cfg) {
    int fails = 0;
    auto check = [&fails](bool ok, const char* msg) {
        std::printf("  %s: %s\n", ok ? "ok" : "FAIL", msg);
        if (!ok) ++fails;
    };

    std::printf("\n==== self-test (ключ: %s) ====\n", w2u8(cfg.keyPath).c_str());

    // --- Позитив: свіжий CMS має прийматись, вміст — збігатися з входом ---
    const std::string input = SELFTEST_DATA;
    const std::string sig   = signData(input);
    check(!sig.empty(), "signData повернув підпис");

    const VerifyOutcome good = verifyCms(sig);
    check(good.accepted && good.status == "TOTAL-VALID", "валідний CMS accepted (TOTAL-VALID)");
    std::string content;
    check(b64decode(good.contentB64, content) && content == input,
          "витягнутий content.bytes == вхідні дані");
    // includeCert:true => сертифікат підписувача мусить бути ВКЛАДЕНИЙ у CMS.
    check(good.certEmbedded, "сертифікат підписувача вкладено (certIds містить signerCertId)");

    // --- Негатив 1: точково псуємо ВКЛАДЕНИЙ вміст (enveloping => він лежить у DER
    //     як є). Саме цей кейс ловить холістичний критерій: statusSignature лишиться
    //     "VALID", а status/validDigests зламаються.
    std::string bad = sig;
    const size_t pos = bad.find(input);
    check(pos != std::string::npos, "вкладений вміст знайдено всередині DER");
    if (pos != std::string::npos)      bad[pos] = (char)(bad[pos] ^ 0x20);   // 'h' -> 'H'
    else if (bad.size() > 40)          bad[bad.size() / 2] = (char)(bad[bad.size() / 2] ^ 0x01);
    const VerifyOutcome nok = verifyCms(bad);
    check(!nok.accepted, "CMS зі зіпсованим вмістом ВІДХИЛЕНО (не accepted)");

    // Замикаємо коло: досі self-test підписував нашим стеком і перевіряв нашим же
    // (verifyCms вище). Негативні кейси цінні, але доводять лише, що НАШ верифікатор
    // помічає псування — не що наш ПІДПИС прийнятний для СТОРОННЬОГО двигуна.
    // Тепер той самий good/bad CMS судить iit_verify_x86.exe (нативна бібліотека
    // АТ «ІІТ», інша кодова база за наш uapki-стек — детальніше в tests/iit_verify.cpp).
    //
    // ВИМІР (2026-09-03, ключ tests/data/test-diia.p12): і good, і bad CMS дають від
    // арбітра ОДНАКОВИЙ вердикт {"status":"INVALID","code":52,
    // "desc":"Сертифікат не чинний за строком дії або закінчився строк дії відповідного
    // особистого ключа"} (exit=1). EUVerifyDataInternal перевіряє строк дії сертифіката
    // РАНІШЕ за дайджести/ланцюг довіри, тож зіпсований вміст ніколи не встигає стати
    // причиною відмови — тестовий сертифікат прострочений незалежно від вмісту CMS.
    // Отже для ЦЬОГО ключа зовнішній суддя НЕ РОЗРІЗНЯЄ good/bad: check(accepted)
    // тут або завжди падав би (=BLOCKED self-test на кожному прогоні), або, якби хтось
    // послабив умову до «просто відхилено», завжди мовчки проходив би незалежно від
    // того, чи справді підпис прийнятний, — рівно той тихий PASS, проти якого ця
    // перевірка й задумана. Тому: НЕ стверджуємо ані «прийнято», ані «відхилено саме
    // через довіру» — чесно рахуємо перевірку неінформативною для test-diia.p12 і
    // віддаємо skip з названою причиною, а не вигадану умову, що завжди проходить.
    const int jGood = judgeByIit(sig);
    const int jBad  = judgeByIit(bad);
    if (jGood == 3 || jBad == 3) {
        std::printf("  skip: арбітр ІІТ недоступний — зовнішня перевірка не виконана "
                    "(good exit=%d, bad exit=%d)\n", jGood, jBad);
    } else if (jGood == 0) {
        // Арбітр ПРИЙМАЄ підпис цим ключем -> перевірка інформативна, ставимо
        // саме ті умови, заради яких цей рівень і існує.
        check(jGood == 0, "валідний CMS ПРИЙНЯТО зовнішнім двигуном (ІІТ)");
        // Саме ==1 (INVALID), а не !=0: exit 2 — це внутрішній збій арбітра, і зараховувати
        // його як «відхилено зовнішнім двигуном» означало б той самий тихий PASS, проти
        // якого ця перевірка й існує. Коди iit_verify: 0=VALID, 1=INVALID, 2=ERROR, 3=SKIP.
        check(jBad == 1, "CMS зі зіпсованим вмістом ВІДХИЛЕНО і зовнішнім двигуном (exit=1)");
    } else if (jGood == jBad) {
        std::printf("  skip: арбітр ІІТ не розрізняє good/bad для ключа test-diia.p12 — "
                    "обидва CMS дають однаковий exit=%d (сертифікат тестового ключа "
                    "непридатний для арбітра незалежно від вмісту CMS, а не через "
                    "довіру до ЦСК); зовнішня перевірка для цього ключа неінформативна\n",
                    jGood);
    } else {
        // Позитив арбітр відхилив (ключ непридатний), але негатив дав ІНШИЙ
        // результат — принаймні негативна гілка інформативна, позитивну чесно
        // рахуємо неперевіреною (а не тихо зеленою).
        std::printf("  skip: позитивна перевірка неможлива — арбітр відхилив good CMS "
                    "(exit=%d), сертифікат test-diia.p12 непридатний для арбітра\n", jGood);
        // Саме ==1 (INVALID), а не !=0: exit 2 — це внутрішній збій арбітра, і зараховувати
        // його як «відхилено зовнішнім двигуном» означало б той самий тихий PASS, проти
        // якого ця перевірка й існує. Коди iit_verify: 0=VALID, 1=INVALID, 2=ERROR, 3=SKIP.
        check(jBad == 1, "CMS зі зіпсованим вмістом ВІДХИЛЕНО і зовнішнім двигуном (exit=1)");
    }

    // --- Негатив 2: взагалі не CMS -> не accepted і без падіння ---
    const VerifyOutcome junk = verifyCms(std::string("\x01\x02\x03not-a-cms", 12));
    check(!junk.accepted, "malformed CMS ВІДХИЛЕНО без падіння");

    // --- Round-trip квитанції: підписати ticket XML і перевірити назад ---
    const std::string tkt = signData(buildTicketXml(0, ""));
    check(!tkt.empty(), "квитанцію підписано");
    const VerifyOutcome tv = verifyCms(tkt);
    check(tv.accepted, "квитанція VERIFY-иться назад як TOTAL-VALID");
    std::string tc;
    check(b64decode(tv.contentB64, tc) && tc.find("<TICKET>") != std::string::npos,
          "content квитанції містить <TICKET>");

    std::printf("\n==== self-test: fails=%d ====\n", fails);
    return (fails > 0) ? 1 : 0;
}


// ============================================================================
// CLI
// ============================================================================
static void usage() {
    std::printf(
        "uapki_fiscal_emulator [port] [--key <p12>] [--pass <pwd>] [--providers <dir>]\n"
        "                      [--data <dir>] [--samples <dir>] [--canned] [--self-test]\n"
        "  port        порт HTTP-оракула (деф. 8099; 8080 не беремо — зазвичай зайнятий)\n"
        "  --key       PKCS#12-контейнер тест-ключа (деф. <data>\\test-diia.p12)\n"
        "  --pass      пароль контейнера (деф. testpassword)\n"
        "  --providers каталог із cm-pkcs12_*.dll (деф. каталог цього exe)\n"
        "  --data      каталог тест-даних certs/ + crls/ (деф. compile-time tests/data)\n"
        "  --samples   каталог зразків ПРРО (.signed) для --canned\n"
        "              (деф. %%PRRO_DOCS_DIR%% або каталог prro_docs поруч із репозиторієм;\n"
        "               самі зразки — github.com/VSydorenko/prro_docs)\n"
        "  --canned    /reference віддає канонічний .signed замість свіжо-підписаного\n"
        "  --self-test прогнати вбудовані перевірки (сервер НЕ піднімається) і вийти\n");
}


// Чи належить консольне вікно ЛИШЕ нам. Якщо так — при виході воно зникне разом із процесом,
// і повідомлення про помилку прочитати неможливо (типовий запуск подвійним кліком із Провідника).
// Тоді перед виходом тримаємо вікно до Enter. Запуск із cmd/PowerShell/CI цього не робить.
static bool ownsConsoleWindow() {
    DWORD pids[4] = { 0 };
    const DWORD n = GetConsoleProcessList(pids, 4);
    return (n <= 1);
}

static void pauseIfOwnConsole() {
    if (!ownsConsoleWindow()) return;
    std::printf("\nНатисніть Enter, щоб закрити вікно...");
    std::fflush(stdout);
    (void)std::getchar();
}


// ============================================================================
// main
// ============================================================================
int main() {
    SetConsoleOutputCP(CP_UTF8);   // кирилиця в консолі замість крякозябрів

    // char** argv на Windows приходить у системному ANSI, тож кириличні шляхи
    // (напр. «Приклади з КЕП») спотворилися б. Беремо широкі аргументи напряму.
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) { std::printf("FAIL: CommandLineToArgvW\n"); return 2; }

    Config cfg;
    bool bad        = false;
    bool portSet    = false;
    bool samplesSet = false;

    for (int i = 1; i < argc && !bad; ++i) {
        const std::wstring a = wargv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 < argc) return std::wstring(wargv[++i]);
            std::printf("Аргумент %s потребує значення\n", w2u8(a).c_str());
            bad = true;
            return std::wstring();
        };
        if      (a == L"--key")       cfg.keyPath      = next();
        else if (a == L"--pass")      cfg.pass         = w2u8(next());
        else if (a == L"--providers") cfg.providersDir = next();
        else if (a == L"--data")      cfg.dataDir      = next();
        else if (a == L"--samples") { cfg.samplesDir   = next(); samplesSet = true; }
        else if (a == L"--canned")    cfg.canned       = true;
        else if (a == L"--self-test") cfg.selfTest     = true;
        else if (!a.empty() && a[0] != L'-' && !portSet) { cfg.port = _wtoi(a.c_str()); portSet = true; }
        else {
            std::printf("Невідомий аргумент: %s\n", w2u8(a).c_str());
            bad = true;
        }
    }
    LocalFree(wargv);

    if (bad) { usage(); pauseIfOwnConsole(); return 2; }
    if (cfg.port <= 0 || cfg.port > 65535) {
        std::printf("Некоректний порт: %d\n", cfg.port);
        usage();
        pauseIfOwnConsole();
        return 2;
    }

    // Дефолти шляхів.
    if (cfg.dataDir.empty())      cfg.dataDir      = u8to16(HOST_DATA_DIR);
    if (cfg.keyPath.empty())      cfg.keyPath      = cfg.dataDir + L"\\test-diia.p12";
    if (cfg.providersDir.empty()) cfg.providersDir = exeDir();
    if (!samplesSet) {
        // Зразки ПРРО лежать в окремому репозиторії github.com/VSydorenko/prro_docs.
        // Порядок: 1) PRRO_DOCS_DIR; 2) каталог prro_docs ПОРУЧ із репозиторієм —
        // exe лежить у <repo>/bin/Release, тож сусід це три рівні вгору. Фолбек
        // best-effort: якщо exe кудись скопіювали, спрацює лише змінна оточення.
        std::wstring root;
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"PRRO_DOCS_DIR", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            root = buf;
        }
        else {
            const std::wstring dir = exeDir();
            if (!dir.empty()) {
                const std::wstring sibling = dir + L"\\..\\..\\..\\prro_docs";
                if (dirExists(sibling)) root = sibling;
            }
        }
        if (!root.empty()) cfg.samplesDir = root + SAMPLES_SUBDIR;
    }

    // Пароль і параметри OPEN у трейс НЕ потрапляють (спека §3.5).
    std::printf("uapki_fiscal_emulator\n  port=%d\n  data=%s\n  key=%s\n  providers=%s\n"
                "  samples=%s\n  canned=%s\n",
                cfg.port, w2u8(cfg.dataDir).c_str(), w2u8(cfg.keyPath).c_str(),
                w2u8(cfg.providersDir).c_str(),
                cfg.samplesDir.empty() ? "(немає)" : w2u8(cfg.samplesDir).c_str(),
                cfg.canned ? "так" : "ні");
    if (cfg.canned) {
        // Причина встановлена точно: канонічні документи ДПС мають позначку часу (CAdES-T),
        // а в тестовому наборі немає сертифіката TSP-СЕРВЕРА (у детальній відповіді VERIFY
        // видно "expectedCerts":[{"entity":"TSP",...}]). Сертифікат ПІДПИСУВАЧА при цьому
        // знаходиться: signerCertId є, statusSignature=VALID, validDigests=true, але
        // загальний status=INDETERMINATE → errorCode=4161 CERT_NOT_FOUND.
        std::printf(
            "  УВАГА: канонічні зразки ДПС НЕ проходять VERIFY (errorCode=4161 CERT_NOT_FOUND) —\n"
            "  ні цим оракулом, ні компонентою в 1С: документи мають позначку часу (CAdES-T),\n"
            "  а сертифіката TSP-сервера в тестовому наборі немає (підпис і дайджести при цьому\n"
            "  валідні, статус — INDETERMINATE). --canned придатний лише для перевірки РОЗБОРУ\n"
            "  чужого CMS; позитивний round-trip / крос-валідація — БЕЗ --canned (свіжий підпис\n"
            "  тест-ключем).\n");
    }
    std::fflush(stdout);

    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    if (!oracle::Init(oracle::OracleConfig{ cfg.providersDir, cfg.dataDir, cfg.keyPath, cfg.pass })) {
        std::printf("Bootstrap не вдався — вихід\n");
        removeWorkDir();
        pauseIfOwnConsole();
        return 2;
    }

    // --self-test: сервер не піднімаємо взагалі.
    if (cfg.selfTest) {
        const int rc = runSelfTest(cfg);
        removeWorkDir();
        return rc;
    }

    if (!minihttp::InitNetwork()) {
        std::printf("Не вдалося ініціалізувати Winsock\n");
        removeWorkDir();
        pauseIfOwnConsole();
        return 1;
    }

    auto handler =
        [&cfg](const minihttp::Request& req) -> minihttp::Response {
            // Трейс: лише метод/шлях/розмір — жодних параметрів крипто й пароля.
            std::printf("← %s %s (%zu B)\n", req.method.c_str(), req.uri.c_str(), req.body.size());
            std::fflush(stdout);

            if (req.body.size() > MAX_BODY_BYTES)
                return httpResp(413, "text/plain; charset=utf-8", "body over 1 MiB limit");

            // Шлях і query — окремо; маршрутизація точним збігом шляху.
            std::string path = req.uri;
            std::string query;
            const size_t qm = path.find('?');
            if (qm != std::string::npos) { query = path.substr(qm + 1); path.resize(qm); }

            if (req.method == "GET" && path == "/ping")
                return httpResp(200, "text/plain; charset=utf-8", "uapki_fiscal_emulator alive");

            // --- POST /doc: перевірити підпис 1С і відповісти підписаною квитанцією ---
            if (req.method == "POST" && path == "/doc") {
                if (req.body.empty())
                    return httpResp(400, "text/plain; charset=utf-8", "empty body");
                const VerifyOutcome o = verifyCms(req.body);
                std::string content;
                b64decode(o.contentB64, content);
                std::printf("  /doc: %s status=%s certEmbedded=%s signer=%s\n  вміст: %s\n",
                            o.accepted ? "ACCEPTED" : "REJECTED",
                            o.status.empty() ? "(немає)" : o.status.c_str(),
                            o.certEmbedded ? "так" : "ні",
                            o.signerCertId.empty() ? "(немає)" : o.signerCertId.substr(0, 16).c_str(),
                            previewText(content, 200).c_str());
                if (o.errorCode != 0)
                    std::printf("  причина: VERIFY errorCode=%ld %s\n", o.errorCode, o.errorText.c_str());
                std::fflush(stdout);

                // 0 = Ok, 9 = DocumentValidationError (спрощена семантика ЄВПЕЗ).
                const int code = o.accepted ? 0 : 9;
                const std::string text = o.accepted
                    ? std::string()
                    : ("Signature rejected: " + (o.status.empty() ? std::string("NOT-A-CMS") : o.status));
                const std::string ticketDer = signData(buildTicketXml(code, text));
                if (ticketDer.empty())
                    return httpResp(500, "text/plain; charset=utf-8", "cannot sign ticket");
                return httpResp(o.accepted ? 200 : 422, "application/octet-stream", ticketDer);
            }

            // --- POST /verify: вердикт JSON-ом (і при 200, і при 422) ---
            if (req.method == "POST" && path == "/verify") {
                if (req.body.empty())
                    return httpResp(400, "text/plain; charset=utf-8", "empty body");
                const VerifyOutcome o = verifyCms(req.body);
                const json out{
                    {"accepted",            o.accepted},
                    {"status",              o.status},
                    {"statusSignature",     o.statusSig},
                    {"statusMessageDigest", o.statusMd},
                    {"validSignatures",     o.validSig},
                    {"validDigests",        o.validDig},
                    {"certEmbedded",        o.certEmbedded},
                    {"signerCertId",        o.signerCertId},
                    {"contentBase64",       o.contentB64},
                    {"errorCode",           o.errorCode},
                    {"errorText",           o.errorText}
                };
                std::printf("  /verify: %s status=%s certEmbedded=%s\n",
                            o.accepted ? "ACCEPTED" : "REJECTED",
                            o.status.empty() ? "(немає)" : o.status.c_str(),
                            o.certEmbedded ? "так" : "ні");
                if (o.errorCode != 0)
                    std::printf("  причина: VERIFY errorCode=%ld %s\n", o.errorCode, o.errorText.c_str());
                std::fflush(stdout);
                return httpResp(o.accepted ? 200 : 422, "application/json; charset=utf-8", out.dump());
            }

            // --- GET /reference: еталонний підписаний документ ---
            if (req.method == "GET" && path == "/reference") {
                std::string type = queryValue(query, "type");
                if (type.empty()) type = "check";
                if (type != "check" && type != "zrep" && type != "ticket")
                    return httpResp(404, "text/plain; charset=utf-8",
                                    "unknown type (check|zrep|ticket)");

                std::string der;
                if (cfg.canned) {
                    // Канонічних зразків рівно два; файлу-квитанції в «Прикладах з КЕП» НЕМАЄ,
                    // тож на ticket чесно віддаємо 404, а не підсовуємо чек.
                    if (type == "ticket")
                        return httpResp(404, "text/plain; charset=utf-8",
                                        "canned ticket sample does not exist "
                                        "(use without --canned for a freshly signed ticket)");
                    if (cfg.samplesDir.empty())
                        return httpResp(404, "text/plain; charset=utf-8", "samples dir not set");
                    const std::wstring file = cfg.samplesDir
                        + ((type == "zrep") ? L"\\Z-звіт.xml.signed" : L"\\чек.xml.signed");
                    if (!readFileBin(file, der) || der.empty())
                        return httpResp(404, "text/plain; charset=utf-8", "sample file not found");
                } else {
                    const std::string doc = (type == "zrep")   ? "<ZREP>reference</ZREP>"
                                          : (type == "ticket") ? buildTicketXml(0, "")
                                                               : "<CHECK>reference</CHECK>";
                    der = signData(doc);
                    if (der.empty())
                        return httpResp(500, "text/plain; charset=utf-8", "cannot sign reference");
                }
                std::printf("  /reference: type=%s джерело=%s\n",
                            type.c_str(), cfg.canned ? "канонічний .signed" : "свіжий підпис");
                std::fflush(stdout);
                return httpResp(200, "application/octet-stream", der);
            }

            return httpResp(404, "text/plain; charset=utf-8", "unknown endpoint");
        };

    // Якщо порт НЕ заданий явно і зайнятий — беремо наступний вільний, щоб запуск подвійним
    // кліком просто працював; якщо заданий явно — поважаємо вибір і не підмінюємо його мовчки.
    // УВАГА: підміна порту означає, що обробка 1С зі своїм дефолтом стукатиме не туди, тому
    // нижче про це друкується явне попередження.
    std::unique_ptr<minihttp::Server> server;
    int         chosenPort = 0;
    std::string listenErr;
    const int   attempts = portSet ? 1 : 11;
    for (int i = 0; i < attempts; ++i) {
        const int tryPort = cfg.port + i;
        if (tryPort > 65535) break;
        std::unique_ptr<minihttp::Server> s(new minihttp::Server(tryPort, "127.0.0.1"));
        s->SetHandler(handler);
        const std::pair<bool, std::string> res = s->Listen();
        if (res.first) { server = std::move(s); chosenPort = tryPort; break; }
        listenErr = res.second;
        if (i == 0) {
            std::printf("Порт %d зайнятий (%s)\n", tryPort,
                        listenErr.empty() ? "немає деталей" : listenErr.c_str());
            if (attempts > 1) std::printf("Шукаю вільний порт…\n");
        }
    }
    if (!server) {
        std::printf("Не вдалося зайняти порт %d: %s\n", cfg.port,
                    listenErr.empty() ? "невідома помилка" : listenErr.c_str());
        std::printf("Підказка: запустіть з іншим портом, напр. «uapki_fiscal_emulator%s.exe 8090»\n",
#ifdef _WIN64
                    "_x64"
#else
                    "_x86"
#endif
        );
        minihttp::ShutdownNetwork();
        removeWorkDir();
        pauseIfOwnConsole();
        return 1;
    }
    server->Start();
    std::printf("\nuapki_fiscal_emulator слухає 127.0.0.1:%d — Ctrl-C для виходу\n", chosenPort);
    if (chosenPort != cfg.port) {
        std::printf("УВАГА: дефолтний порт %d був зайнятий. У полі «Порт консолі» тестової обробки\n"
                    "       1С вкажіть %d, інакше кнопки HTTP-обміну не достукаються.\n",
                    cfg.port, chosenPort);
    }
    std::fflush(stdout);

    // Чекаємо з ПРЕДИКАТОМ: стандарт дозволяє спурйозні пробудження, і без предиката
    // оракул тихо завершився б посеред сесії 1С. (Саме на цьому спотикався
    // ix::SocketServer::wait(), тому чекання лишається тут, а не всередині сервера.)
    // Прапорець виставляє ctrlHandler.
    {
        std::unique_lock<std::mutex> lk(g_stopMx);
        g_stopCv.wait(lk, [] { return g_stopRequested; });
    }

    server->Stop();
    minihttp::ShutdownNetwork();
    removeWorkDir();
    return 0;
}
