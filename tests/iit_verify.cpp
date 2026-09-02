///
/// @file tests/iit_verify.cpp
/// @brief iit_verify — НЕЗАЛЕЖНИЙ арбітр підпису на нативній бібліотеці АТ «ІІТ».
///
/// НАВІЩО. Рівні native_host перевіряють наш підпис НАШИМ ЖЕ VERIFY: підписали
/// UAPKI — перевірили UAPKI. При системній помилці (спільній для підпису й
/// перевірки) обидва рівні пройдуть зелено, нічого не помітивши. Потрібен двигун
/// ІНШОГО ПОХОДЖЕННЯ. UAPKI є форком Cryptonite (extern/uapki/README.md:4), тож
/// жодна Cryptonite-похідна бібліотека арбітром бути не може — спільний предок дає
/// спільні системні помилки. АТ «ІІТ» має власну кодову базу, тому її EUSignCP.dll
/// і є другою, незалежною думкою про наш підпис.
///
/// ЛІЦЕНЗІЯ. EUSignCP.dll у репозиторій НЕ копіюється — вантажиться з місця
/// встановлення через LoadLibraryW+GetProcAddress. Ліцензія ІІТ §2.2: складові
/// неподільного продукту не можна поділяти для використання на кількох комп'ютерах.
///
/// ТІЛЬКИ x86. EUSignCP.dll 1.3.1.224 — 32-бітна (dumpbin /headers → 14C machine),
/// x64-процес її не завантажить. Ціль оголошена в tests/CMakeLists.txt ДО гейта
/// BUILD_WITH_UAPKI: UAPKI цьому арбітру не потрібен — у незалежності від нашого
/// крипто-стека весь його сенс.
///
/// ІЗОЛЯЦІЯ. Усі налаштування бібліотеки перенаправлено у власну гілку реєстру
/// HKCU\Software\SimplyAddinConnect\IitVerify (EUSetSettingsRegPath ДО EUInitialize),
/// а сховище довіри — у власний каталог %LOCALAPPDATA%\SimplyAddinConnect\iit-store.
/// Інакше офлайн-режим і файлове сховище цього тесту приземлилися б на ІНСТАЛЯЦІЮ
/// ІІТ КОРИСТУВАЧА: ми змінили б чужий софт побічним ефектом власного прогону.
///
/// КОДИ ВИХОДУ (контракт для задач 8-11, не міняти):
///   0 — підпис валідний | 1 — підпис невалідний | 2 — помилка використання/внутрішня
///   3 — SKIP (немає ІІТ або сховища довіри).
/// stdout — РІВНО ОДИН рядок JSON, у двох формах:
///   вердикт  : {"status":"VALID|INVALID","code":…,"desc":…,"subject":…,
///               "timeStamp":true|false,"contentLen":…}
///   службовий: {"status":"SKIP|ERROR","detail":…,"code":…}
///

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW (потрібен явно)
#include <cstdio>
#include <string>
#include <vector>

#include "support/IitStore.h"  // сховище довіри ІІТ у %LOCALAPPDATA%: кеш бандла ЦЗО + маркер імпорту

#pragma comment(lib, "shell32.lib")  // CommandLineToArgvW

// ---------------------------------------------------------------------------
// Прототипи потрібних експортів EUSignCP.dll.
//
// EULoad/EUUnload/EUGetInterface у DLL НЕ експортуються — це glue із SDK ІІТ,
// якого в інсталяції кінцевого користувача немає. Тому зв'язуємось виключно через
// GetProcAddress по іменах (звірено dumpbin /exports, 630 експортів, імена
// недекоровані).
//
// ДЖЕРЕЛО СИГНАТУР — настанова АТ «ІІТ» (додаток з описом інтерфейсу бібліотеки
// підпису, EUSignMSWCPPAppendixA): звірено поле за полем, аргумент за аргументом.
// Заголовка EUSignCP.h у постачанні кінцевого користувача НЕМАЄ, тож перевірити
// прототип «на місці» неможливо — саме тому кожен нижче має посилання на джерело.
//
// Угода виклику — __stdcall (WINAPI), як і в решті API ІІТ. УВАГА: при __stdcall
// стек чистить ВИКЛИКАНА сторона, тож помилка в КІЛЬКОСТІ параметрів псує стек
// МОВЧКИ — ні помилки збірки, ні винятку, лише зіпсовані дані десь далі.
// НЕ «спрощуй» ці прототипи, прибираючи «зайвий» аргумент: два з них уже були
// коротшими на один параметр (EUSetFileStoreSettings, EUGetErrorLangDesc), і саме
// настанова показала, що це помилка. Правити — лише звірившись із настановою ІІТ.
// PSTR у настанові = char* (не const): рядки передаються модифікованими буферами.
// ---------------------------------------------------------------------------

/// Відомості про підпис, які повертає EUVerifyData*; звільняється EUFreeSignInfo.
/// Настанова ІІТ (EUSignMSWCPPAppendixA, EU_SIGN_INFO): BOOL bFilled, рівно 17 PSTR
/// у цьому порядку, BOOL bTimeAvail, BOOL bTimeStamp, SYSTEMTIME Time.
typedef struct {
    BOOL       bFilled;
    char*      pszIssuer;         char* pszIssuerCN;       char* pszSerial;
    char*      pszSubject;        char* pszSubjCN;         char* pszSubjOrg;
    char*      pszSubjOrgUnit;    char* pszSubjTitle;      char* pszSubjState;
    char*      pszSubjLocality;   char* pszSubjFullName;   char* pszSubjAddress;
    char*      pszSubjPhone;      char* pszSubjEMail;      char* pszSubjDNS;
    char*      pszSubjEDRPOUCode; char* pszSubjDRFOCode;
    BOOL       bTimeAvail;
    BOOL       bTimeStamp;        // позначка часу з TSP-сервера
    SYSTEMTIME Time;
} EU_SIGN_INFO;

// Настанова ІІТ: EUSetSettingsRegPath(DWORD dwRootKey, PSTR pszRegPath).
// dwRootKey: 0 = DEFAULT, 1 = HKLM, 2 = HKCU, 3 = CURRENT.
// Ізоляція налаштувань у власну гілку реєстру КРИТИЧНА й робиться ДО EUInitialize:
// коли шлях налаштувань порожній, бібліотека на Windows бере налаштування з реєстру
// КОРИСТУВАЧА — і EUSetModeSettings(TRUE) мовчки перемкнув би інсталяцію ІІТ
// користувача в офлайн, тобто наш тест зіпсував би чужий софт побічним ефектом.
typedef DWORD (WINAPI *PFN_SetSettingsRegPath)(DWORD, char*);
typedef void  (WINAPI *PFN_SetUIMode)(BOOL);        // настанова ІІТ: VOID; FALSE = без діалогів
typedef DWORD (WINAPI *PFN_Initialize)(void);       // настанова ІІТ: DWORD, без аргументів
typedef DWORD (WINAPI *PFN_SetModeSettings)(BOOL);  // настанова ІІТ: DWORD; TRUE = офлайн
// Групи мережевих служб. Прототипи — настанова АТ «ІІТ» (звірено контролером),
// експорти на місці (dumpbin: EUSetLDAPSettings ord 223, EUSetOCSPSettings 229,
// EUSetProxySettings 22C, EUSetTSPSettings 234). Кількість аргументів у кожного
// РІЗНА (6 / 4 / 3 / 7) — при __stdcall помилка тут псує стек мовчки:
//   EUSetLDAPSettings (BOOL bUseLDAP,   PSTR pszAddress, PSTR pszPort,
//                      BOOL bAnonymous, PSTR pszUser,    PSTR pszPassword)          — 6
//   EUSetOCSPSettings (BOOL bUseOCSP,   BOOL bBeforeStore, PSTR pszAddress,
//                      PSTR pszPort)                                                — 4
//   EUSetTSPSettings  (BOOL bGetStamps, PSTR pszAddress, PSTR pszPort)              — 3
//   EUSetProxySettings(BOOL bUseProxy,  BOOL bAnonymous, PSTR pszAddress,
//                      PSTR pszPort, PSTR pszUser, PSTR pszPassword,
//                      BOOL bSavePassword)                                          — 7
typedef DWORD (WINAPI *PFN_SetLDAPSettings)(BOOL, char*, char*, BOOL, char*, char*);
typedef DWORD (WINAPI *PFN_SetOCSPSettings)(BOOL, BOOL, char*, char*);
typedef DWORD (WINAPI *PFN_SetTSPSettings)(BOOL, char*, char*);
typedef DWORD (WINAPI *PFN_SetProxySettings)(BOOL, BOOL, char*, char*, char*, char*, BOOL);
// Настанова ІІТ: EUSetFileStoreSettings(PSTR pszPath, BOOL bCheckCRLs,
//   BOOL bAutoRefresh, BOOL bOwnCRLsOnly, BOOL bFullAndDeltaCRLs,
//   BOOL bAutoDownloadCRLs, BOOL bSaveLoadedCerts, DWORD dwExpireTime).
// ВІСІМ аргументів. Восьмий (час зберігання стану перевіреного сертифіката, секунди)
// легко не помітити: із сімома викликана сторона зняла б зі стека 32 байти проти
// покладених 28. Ставимо dwExpireTime = 30 (аналог ocspResponseExpireTime у віджеті
// ЦЗО); при bCheckCRLs=FALSE та офлайні значення ні на що не впливає, але
// випадкове число тут неприйнятне.
typedef DWORD (WINAPI *PFN_SetFileStoreSettings)(char*, BOOL, BOOL, BOOL, BOOL, BOOL, BOOL, DWORD);
// Настанова ІІТ: EUSaveCertificates(PBYTE pbCertificates, DWORD dwCertificatesLength).
// Імпорт бандла сертифікатів (P7B) у файлове сховище — задача 7.
typedef DWORD (WINAPI *PFN_SaveCertificates)(BYTE*, DWORD);
// Настанова ІІТ: EUVerifyDataInternal(PSTR pszSignedData, PBYTE pbSignedData,
//   DWORD dwSignedDataLength, PBYTE* ppbData, PDWORD pdwDataLength, PEU_SIGN_INFO).
// Провідний PSTR — той самий підпис у base64; «якщо параметр == 0, перевіряються
// дані з масиву байт», тож nullptr + байти — ШТАТНИЙ задокументований шлях.
typedef DWORD (WINAPI *PFN_VerifyDataInternal)(char*, BYTE*, DWORD, BYTE**, DWORD*, EU_SIGN_INFO*);
// Настанова ІІТ: PSTR EUGetErrorLangDesc(DWORD dwError, DWORD dwLang) — ПОВЕРТАЄ
// рядок, а не заповнює буфер. dwLang = 0 (EU_DEFAULT_LANG; інших мовних констант
// настанова не наводить). EUFreeMemory на результат НЕ кликати: настанова цього не
// приписує, а зайве звільнення чужого статичного буфера гірше за витік кількох байтів.
// Потрібен, щоб «відхилив підпис» і «не зміг перевірити» не зливались у безликий exit 1.
typedef char* (WINAPI *PFN_GetErrorLangDesc)(DWORD, DWORD);
typedef void  (WINAPI *PFN_FreeMemory)(BYTE*);            // настанова ІІТ: VOID
typedef void  (WINAPI *PFN_FreeSignInfo)(EU_SIGN_INFO*);  // настанова ІІТ: VOID
typedef void  (WINAPI *PFN_Finalize)(void);               // настанова ІІТ: VOID, не DWORD

/// Прив'язана бібліотека ІІТ. Перелік експортів живе РІВНО ТУТ: задачі 6 і 7
/// беруть уже прив'язані вказівники й bindEu більше не правлять.
struct Eu {
    HMODULE      h = nullptr;
    std::wstring dir;                    // каталог, з якого завантажено DLL (для діагностики)
    PFN_SetSettingsRegPath   SetSettingsRegPath   = nullptr;
    PFN_SetUIMode            SetUIMode            = nullptr;
    PFN_Initialize           Initialize           = nullptr;
    PFN_SetModeSettings      SetModeSettings      = nullptr;
    PFN_SetLDAPSettings      SetLDAPSettings      = nullptr;
    PFN_SetOCSPSettings      SetOCSPSettings      = nullptr;
    PFN_SetTSPSettings       SetTSPSettings       = nullptr;
    PFN_SetProxySettings     SetProxySettings     = nullptr;
    PFN_SetFileStoreSettings SetFileStoreSettings = nullptr;
    PFN_SaveCertificates     SaveCertificates     = nullptr;
    PFN_VerifyDataInternal   VerifyDataInternal   = nullptr;
    PFN_GetErrorLangDesc     GetErrorLangDesc     = nullptr;
    PFN_FreeMemory           FreeMemory           = nullptr;
    PFN_FreeSignInfo         FreeSignInfo         = nullptr;
    PFN_Finalize             Finalize             = nullptr;
};

// ---------------------------------------------------------------------------
// Дрібний інструментарій
// ---------------------------------------------------------------------------

/// Широкий рядок -> UTF-8 (шляхи можуть містити кирилицю).
static std::string w2u8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

/// Рядки, які ПОВЕРТАЄ бібліотека ІІТ (pszSubjCN, EUGetErrorLangDesc), — це PSTR
/// без оголошеного кодування; на Windows-складанні вони приходять у системній
/// ANSI-сторінці (для української локалі — CP1251), а наш вивід за контрактом
/// UTF-8. Без перекодування кириличний опис помилки перетворився б на сміття
/// рівно там, де він найпотрібніший: у першому вимірі реальних кодів ІІТ.
/// Розрізняємо надійно й без здогадів: якщо байти вже валідний UTF-8 (сюди ж
/// потрапляє чистий ASCII) — лишаємо як є, інакше читаємо їх як ANSI.
static std::string toUtf8Loose(const char* s) {
    if (!s || !*s) return std::string();
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0) > 0)
        return std::string(s);                      // вже валідний UTF-8
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::string(s);              // не змогли розібрати — краще як є, ніж нічого
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w2u8(w);
}

/// UTF-8 -> ANSI для PSTR-аргументів ІІТ. Задача 7 віддає каталог сховища в UTF-8,
/// але PSTR у не-Unicode API означає САМЕ ANSI: на профілі з кириличним іменем
/// користувача (`C:\Users\Петро\...` — для української аудиторії звичайний випадок,
/// не екзотика) бібліотека дістала б спотворений шлях. Контракт IitStore.h не
/// чіпаємо — конвертуємо локально, тут.
/// WC_NO_BEST_FIT_CHARS + прапорець «був підстановочний символ» ЗАБОРОНЯЮТЬ тиху
/// заміну неконвертованого символу на '?': краще гучний ERROR із названою причиною,
/// ніж каталог із покаліченим іменем і незрозуміла помилка сховища через крок.
static bool u8ToAnsi(const std::string& u8, std::string& out) {
    out.clear();
    if (u8.empty()) return true;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.c_str(), (int)u8.size(), nullptr, 0);
    if (n <= 0) return false;
    std::wstring w((size_t)n, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.c_str(), (int)u8.size(), &w[0], n) != n)
        return false;
    BOOL usedDefault = FALSE;
    const int m = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), n,
                                      nullptr, 0, nullptr, &usedDefault);
    if (m <= 0) return false;
    out.resize((size_t)m);
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), n,
                            &out[0], m, nullptr, &usedDefault) != m) {
        out.clear();
        return false;
    }
    if (usedDefault) { out.clear(); return false; }   // символ не має представлення в ANSI
    return true;
}

/// Екранування рядка для JSON: шляхи Windows містять зворотні слеші, а вони
/// без екранування зробили б вивід невалідним JSON — і гейт (задача 9) мовчки
/// не розібрав би вердикт арбітра.
static std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

/// Єдиний рядок результату на stdout. status — завжди літерал, тож не екранується.
static void jsonOut(const char* status, const std::string& detail, long code) {
    std::printf("{\"status\":\"%s\",\"detail\":\"%s\",\"code\":%ld}\n",
                status, jsonEscape(detail).c_str(), code);
    std::fflush(stdout);
}

/// Вердикт арбітра — окремий, багатший рядок, ніж службовий jsonOut.
/// Поле desc ОБОВ'ЯЗКОВЕ. Числових кодів помилок нативної бібліотеки ІІТ ми ще не
/// знаємо: «Сертифікат не знайдено(51)» — число з ВЕБ-віджета ЦЗО, у якого власна
/// нумерація поверх asm.js-збірки, і його тотожність нативним кодам не доведена.
/// Тому не вгадуємо число, а робимо вивід самопояснювальним: із desc одразу видно,
/// це чесне «сертифікат не знайдено» (немає ланцюга довіри) чи щось на кшталт
/// «бібліотека не ініціалізована» — тобто НАША помилка, замаскована під відхилення.
static void emitVerdict(const char* status, DWORD code, const std::string& desc,
                        const std::string& subject, bool timeStamp, DWORD contentLen) {
    std::printf("{\"status\":\"%s\",\"code\":%lu,\"desc\":\"%s\",\"subject\":\"%s\","
                "\"timeStamp\":%s,\"contentLen\":%lu}\n",
                status, (unsigned long)code, jsonEscape(desc).c_str(),
                jsonEscape(subject).c_str(), timeStamp ? "true" : "false",
                (unsigned long)contentLen);
    std::fflush(stdout);
}

/// Тимчасове перемикання кодової сторінки КОНСОЛІ на UTF-8, щоб кириличні деталі
/// читалися при ручному запуску. Консоль спільна з батьківською оболонкою, тож
/// стару сторінку обов'язково повертаємо. На перенаправленому stdout не робить
/// нічого: туди й так ідуть UTF-8 байти — це і є контракт виводу.
struct ConsoleUtf8Guard {
    UINT prev = 0;
    ConsoleUtf8Guard() {
        DWORD mode = 0;
        if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) {
            const UINT cur = GetConsoleOutputCP();
            if (cur != CP_UTF8 && SetConsoleOutputCP(CP_UTF8)) prev = cur;
        }
    }
    ~ConsoleUtf8Guard() { if (prev) SetConsoleOutputCP(prev); }
};

/// Читання файла ЦІЛКОМ широким іменем. Саме CreateFileW, а не CreateFileA:
/// ANSI-шлях спотворив би кириличні імена (та сама причина, що й для
/// CommandLineToArgvW у native_host.cpp).
static bool readFileBytes(const std::wstring& path, std::vector<BYTE>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) != 0 && sz.QuadPart >= 0 && sz.QuadPart <= 64LL * 1024 * 1024;
    if (ok && sz.QuadPart > 0) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) != 0 && got == out.size();
        if (!ok) out.clear();
    }
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------------------
// Ім'я ізольованої гілки налаштувань. Форма РІВНО ОДНА (вузька): EUSetSettingsRegPath
// приймає PSTR, а власного запису в реєстр ми більше не робимо — групи мережевих
// служб виставляються документованим API. Доки форма одна, розбіжність між формами
// неможлива за побудовою; якщо колись знадобиться широка, виводь її з цього рядка
// препроцесором, а не набирай удруге руками.
// ---------------------------------------------------------------------------
static char kSettingsRegPathA[] = "Software\\SimplyAddinConnect\\IitVerify";   // не const: PSTR = char*

// ---------------------------------------------------------------------------
// Пошук і прив'язка бібліотеки ІІТ
// ---------------------------------------------------------------------------

/// Пошук каталогу ІІТ: спершу перекриття змінною середовища IIT_EU_DIR (нею ж
/// тест перевіряє гілку SKIP), потім стандартне місце встановлення.
/// Критерій добору — НЕ ім'я підкаталогу, а наявність у ньому «End User\EUSignCP.dll»:
/// перебираємо БУДЬ-ЯКУ підтеку «Institute of Informational Technologies» і беремо
/// першу, де така DLL реально лежить. Тому ані версія («Certificate Authority-1.3»),
/// ані сама схема іменування ЦСК у код не впаяні й переживуть оновлення ІІТ.
static bool findIitDir(std::wstring& out) {
    wchar_t env[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"IIT_EU_DIR", env, MAX_PATH) > 0) {
        out = env;
        return GetFileAttributesW((out + L"\\EUSignCP.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    // Процес 32-бітний, тож ProgramFiles(x86) вказує саме туди, де стоїть ІІТ.
    wchar_t pf[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH)) return false;
    const std::wstring base = std::wstring(pf) + L"\\Institute of Informational Technologies";
    WIN32_FIND_DATAW fd{};
    HANDLE hf = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring cand = base + L"\\" + n + L"\\End User";
        if (GetFileAttributesW((cand + L"\\EUSignCP.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
            out = cand;
            found = true;
            break;
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return found;
}

/// Завантаження EUSignCP.dll і прив'язка ВСІХ потрібних експортів (15 штук).
/// Перелік живе рівно тут і навмисно повний: розкидати його по місцях виклику
/// означало б ризикувати розбіжністю.
/// Будь-який відсутній експорт — це не «трохи менше можливостей», а неробочий
/// арбітр, тож повертаємо false з названим іменем -> SKIP, ніколи не PASS.
static bool bindEu(Eu& eu, std::string& err) {
    std::wstring dir;
    if (!findIitDir(dir)) {
        err = "EUSignCP.dll не знайдено (ІІТ не встановлено або IIT_EU_DIR вказує не туди)";
        return false;
    }
    eu.dir = dir;
    SetDllDirectoryW(dir.c_str());                       // залежності EUSignCP.dll лежать поруч із нею
    eu.h = LoadLibraryW((dir + L"\\EUSignCP.dll").c_str());
    if (!eu.h) {
        err = "LoadLibraryW не змогла завантажити EUSignCP.dll із " + w2u8(dir)
            + ", GetLastError=" + std::to_string((unsigned long)GetLastError());
        return false;
    }

    // FreeLibrary перед виходом: інакше частковий провал прив'язки лишав би
    // завантажений HMODULE — асиметрія з успішним шляхом, де модуль звільняє main.
    #define BIND(field, name)                                                    \
        eu.field = (decltype(eu.field))GetProcAddress(eu.h, name);               \
        if (!eu.field) {                                                         \
            err = std::string("Немає експорту ") + name;                         \
            FreeLibrary(eu.h); eu.h = nullptr;                                   \
            return false;                                                        \
        }
    BIND(SetSettingsRegPath,   "EUSetSettingsRegPath")   // ізоляція налаштувань (задача 6)
    BIND(SetUIMode,            "EUSetUIMode")
    BIND(Initialize,           "EUInitialize")
    BIND(SetModeSettings,      "EUSetModeSettings")
    BIND(SetLDAPSettings,      "EUSetLDAPSettings")     // чотири групи мережевих служб:
    BIND(SetOCSPSettings,      "EUSetOCSPSettings")     //   без них файлове сховище
    BIND(SetTSPSettings,       "EUSetTSPSettings")      //   не працює взагалі
    BIND(SetProxySettings,     "EUSetProxySettings")    //   (вимір — у main)
    BIND(SetFileStoreSettings, "EUSetFileStoreSettings")
    BIND(SaveCertificates,     "EUSaveCertificates")     // імпорт бандла довіри (задача 7)
    BIND(VerifyDataInternal,   "EUVerifyDataInternal")
    BIND(GetErrorLangDesc,     "EUGetErrorLangDesc")     // людський опис коду помилки (задача 6)
    BIND(FreeMemory,           "EUFreeMemory")
    BIND(FreeSignInfo,         "EUFreeSignInfo")
    BIND(Finalize,             "EUFinalize")
    #undef BIND
    return true;
}

// ---------------------------------------------------------------------------

int main() {
    ConsoleUtf8Guard cpGuard;

    // char** argv на Windows приходить у системному ANSI-кодуванні, тож кириличні
    // шляхи (а задача 9 подає сюди шляхи з Get-ChildItem) спотворювались би.
    // Беремо широкі аргументи напряму — як у native_host.cpp.
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) {
        jsonOut("ERROR", "CommandLineToArgvW не повернула аргументів", 0);
        return 2;
    }
    if (argc < 2) {
        jsonOut("ERROR", "usage: iit_verify_x86 <file.p7s>", 0);
        LocalFree(wargv);
        return 2;
    }
    const std::wstring sigPath = wargv[1];
    LocalFree(wargv);

    Eu eu;
    std::string err;
    if (!bindEu(eu, err)) {
        jsonOut("SKIP", err, 0);      // немає ІІТ -> явний SKIP із названою причиною, НІКОЛИ не PASS
        return 3;
    }

    // Вхідний файл читаємо ДО першого виклику ІІТ: недоступний вхід — це помилка
    // використання (exit 2), і виявляти її треба ПЕРШ НІЖ ми ініціалізували чужу
    // бібліотеку й потягли з мережі бандл довіри. Порядок викликів самої ІІТ це
    // не зачіпає — читання файлу до неї не належить.
    std::vector<BYTE> sig;
    if (!readFileBytes(sigPath, sig) || sig.empty()) {
        jsonOut("ERROR", "Не прочитано вхідний файл: " + w2u8(sigPath), 0);
        FreeLibrary(eu.h);
        return 2;
    }

    // ІЗОЛЯЦІЯ НАЛАШТУВАНЬ — ДО EUInitialize і до будь-якого іншого EUSet*.
    // Коли шлях налаштувань порожній, на Windows бібліотека бере їх із реєстру
    // КОРИСТУВАЧА, і EUSetModeSettings(TRUE) нижче мовчки перемкнув би інсталяцію
    // ІІТ користувача в офлайн — він дізнався б про це, коли його ІІТ перестав би
    // ходити в мережу. 2 = HKCU: гілка user-writable, адміністратора не потребує.
    // Провал тут не «неточність налаштувань», а втрата ізоляції, тож — стоп.
    if (const DWORD rc = eu.SetSettingsRegPath(2 /*HKCU*/, kSettingsRegPathA)) {
        jsonOut("ERROR", "EUSetSettingsRegPath не вдалася — без ізоляції налаштувань "
                         "продовжувати не можна", (long)rc);
        FreeLibrary(eu.h);
        return 2;
    }

    // GUI ЗАБОРОНЕНО, теж ДО Initialize: настанова — «бібліотеку буде завантажено
    // без графічного модуля». Без цього бібліотека на помилці може відкрити діалог
    // і підвісити автоматичний прогін НАЗАВЖДИ — класична пастка гейтів.
    eu.SetUIMode(FALSE);

    if (const DWORD rc = eu.Initialize()) {
        jsonOut("ERROR", "EUInitialize не вдалася", (long)rc);
        FreeLibrary(eu.h);
        return 2;
    }

    // Після успішного Initialize кожен вихід зобов'язаний пройти через Finalize.
    auto bail = [&](const char* status, const std::string& detail, long code, int exitCode) {
        jsonOut(status, detail, code);
        eu.Finalize();
        FreeLibrary(eu.h);
        return exitCode;
    };

    // Детермінізм: жодних звернень до серверів ЦСК. Тихого відкату немає за
    // конструкцією бібліотеки — операція, що потребує мережі, в офлайні падає.
    if (const DWORD rc = eu.SetModeSettings(TRUE))
        return bail("ERROR", "EUSetModeSettings(офлайн) не вдалася", (long)rc, 2);

    // ЧОТИРИ ГРУПИ МЕРЕЖЕВИХ СЛУЖБ — усі вимкнені. Це не косметика й не «про всяк
    // випадок»: у сховищі налаштувань вони мусять БУТИ, інакше файлове сховище не
    // працює взагалі. На порожній ізольованій гілці EUSaveCertificates падає з
    // кодом 49 («Виникла помилка при роботі з файловим сховищем сертифікатів та
    // СВС») — при будь-якому каталозі сховища й при всіх шести комбінаціях
    // прапорців EUSetFileStoreSettings, які я прогнав.
    //
    // Вимір «усі групи мінус одна» (еталон ЦЗО, кожен прогін із чистим сховищем):
    //   мінус CMP / FileStore / InternationalMode / Log / Mode -> працює
    //   мінус LDAP -> 49 | мінус OCSP -> 49 | мінус Proxy -> 49 | мінус TSP -> 49
    // Потрібні РІВНО ці чотири, і саме зі значеннями: самих порожніх ключів мало.
    //
    // Виставляємо їх документованим API, а не записом у реєстр: так ми не залежимо
    // від імен значень конкретної версії ІІТ і взагалі не пишемо в чужий конфіг
    // власними руками. Усі головні перемикачі FALSE — це і є офлайновий профіль,
    // оголошений явно, а не успадкований від інсталяції користувача.
    char empty[] = "";
    if (const DWORD rc = eu.SetLDAPSettings(FALSE, empty, empty, TRUE, empty, empty))
        return bail("ERROR", "EUSetLDAPSettings не вдалася: "
                             + toUtf8Loose(eu.GetErrorLangDesc(rc, 0)), (long)rc, 2);
    if (const DWORD rc = eu.SetOCSPSettings(FALSE, FALSE, empty, empty))
        return bail("ERROR", "EUSetOCSPSettings не вдалася: "
                             + toUtf8Loose(eu.GetErrorLangDesc(rc, 0)), (long)rc, 2);
    if (const DWORD rc = eu.SetTSPSettings(FALSE, empty, empty))
        return bail("ERROR", "EUSetTSPSettings не вдалася: "
                             + toUtf8Loose(eu.GetErrorLangDesc(rc, 0)), (long)rc, 2);
    if (const DWORD rc = eu.SetProxySettings(FALSE, TRUE, empty, empty, empty, empty, FALSE))
        return bail("ERROR", "EUSetProxySettings не вдалася: "
                             + toUtf8Loose(eu.GetErrorLangDesc(rc, 0)), (long)rc, 2);

    // Сховище довіри: власний каталог у %LOCALAPPDATA% + бандл ЦСК від ЦЗО.
    std::string  storeDir;
    std::wstring bundlePath;
    bool         needImport = false;
    if (!ensureIitStore(storeDir, bundlePath, needImport))
        return bail("SKIP", "Сховище довіри ІІТ недоступне: немає кешу й не вдалося "
                            "завантажити бандл ЦЗО", 0, 3);

    // ВІСІМ аргументів (див. прототип вище). bAutoDownloadCRLs і bSaveLoadedCerts
    // вимкнено НАВМИСНО: інакше сховище змінюється між прогонами й результат
    // перестає бути відтворюваним.
    std::string storeAnsi;
    if (!u8ToAnsi(storeDir, storeAnsi))
        return bail("ERROR", "Каталог сховища не представляється в ANSI, а PSTR у ІІТ "
                             "означає саме ANSI: " + storeDir, 0, 2);
    std::vector<char> storeBuf(storeAnsi.begin(), storeAnsi.end());
    storeBuf.push_back('\0');
    if (const DWORD rc = eu.SetFileStoreSettings(storeBuf.data(),
                                                 /*bCheckCRLs*/        FALSE,
                                                 /*bAutoRefresh*/      TRUE,
                                                 /*bOwnCRLsOnly*/      FALSE,
                                                 /*bFullAndDeltaCRLs*/ FALSE,
                                                 /*bAutoDownloadCRLs*/ FALSE,
                                                 /*bSaveLoadedCerts*/  FALSE,
                                                 /*dwExpireTime*/      30))
        return bail("ERROR", "EUSetFileStoreSettings не вдалася: "
                             + toUtf8Loose(eu.GetErrorLangDesc(rc, 0)), (long)rc, 2);

    // НАПОВНЕННЯ сховища. Покласти CACertificates.p7b у каталог — це ЩЕ НЕ
    // наповнити сховище: P7B є ВХІДНИМ форматом імпорту («Збереження списку
    // сертифікатів в форматі P7B до файлового сховища»), а саме сховище бібліотека
    // веде сама. Без цього виклику навіть валідний підпис отримав би «сертифікат
    // не знайдено», і причину довго списували б на відсутність ланцюга довіри.
    // EUSaveCertificatesEx тут НЕ годиться: він зберігає лише сертифікати,
    // «перевірені з використанням довіреного сховища», тобто на порожньому
    // сховищі імпортує НУЛЬ записів.
    if (needImport) {
        std::vector<BYTE> bundle;
        if (!readFileBytes(bundlePath, bundle) || bundle.empty())
            return bail("SKIP", "Бандл сертифікатів ЦЗО не прочитано: " + w2u8(bundlePath), 0, 3);
        if (const DWORD rc = eu.SaveCertificates(bundle.data(), (DWORD)bundle.size()))
            return bail("ERROR", "EUSaveCertificates не наповнила сховище довіри: "
                                 + toUtf8Loose(eu.GetErrorLangDesc(rc, 0)), (long)rc, 2);
        // Маркер робить імпорт одноразовим: EUSaveCertificates на бандлі ЦЗО
        // (~1,5 МБ) дорога, і повторювати її щопрогону марно. Невдача самого
        // маркера не є помилкою перевірки — вона лише сповільнить наступний запуск.
        markIitStoreImported(bundlePath);
    }

    // Перший PSTR — той самий підпис у base64; «якщо параметр == 0, перевіряються
    // дані з масиву байт», тож nullptr + байти є ШТАТНИМ задокументованим шляхом.
    EU_SIGN_INFO info{};
    BYTE*        content    = nullptr;
    DWORD        contentLen = 0;
    const DWORD  rc = eu.VerifyDataInternal(nullptr, sig.data(), (DWORD)sig.size(),
                                            &content, &contentLen, &info);

    const bool ok = (rc == 0);
    emitVerdict(ok ? "VALID" : "INVALID", rc,
                toUtf8Loose(eu.GetErrorLangDesc(rc, 0)),
                (info.bFilled && info.pszSubjCN) ? toUtf8Loose(info.pszSubjCN) : std::string(),
                info.bFilled && info.bTimeStamp != FALSE,
                contentLen);

    if (content) eu.FreeMemory(content);
    if (info.bFilled) eu.FreeSignInfo(&info);
    eu.Finalize();
    FreeLibrary(eu.h);
    // ВІДОМЕ ОБМЕЖЕННЯ: «відхилив підпис» і «не зміг перевірити» зливаються тут в
    // один exit 1. Правило їх розрізнення ухвалюється ПІСЛЯ першого виміру реальних
    // кодів ІІТ — саме для цього у виводі стоять code і desc.
    return ok ? 0 : 1;
}
