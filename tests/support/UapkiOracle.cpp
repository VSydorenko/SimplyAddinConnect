// UapkiOracle — реалізація. Опис — у UapkiOracle.h.
// Код перенесено ДОСЛІВНО з tests/uapki_fiscal_emulator.cpp (main 93761f5);
// змінено лише імена й сигнатури (позначено «БУЛО:»).
// УВАГА: pch.h НЕ підключається (правило tests/).
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include "UapkiOracle.h"

#include <cstdio>
#include <exception>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include <windows.h>

using nlohmann::json;

// Символи крипто-ядра (C-лінкування), статично злінковані через uapki_bundle.
extern "C" char* process(const char* request);
extern "C" void  json_free(char* buf);

namespace oracle {
namespace {

// Провайдер НКІ за compile-time архітектурою: cm-pkcs12_x64.dll / _x86.dll.
#ifdef _WIN64
const char* ARCH_PROVIDER = "cm-pkcs12_x64";
#else
const char* ARCH_PROVIDER = "cm-pkcs12_x86";
#endif

// MiniHttpServer обробляє КОЖНЕ з'єднання окремим потоком (як і ix::HttpServer до
// нього), а UAPKI має глобальний
// крипто-стан (єдине активне сховище й глобально вибраний ключ). Тому ВСІ виклики
// process() серіалізуються цим замком — жодного прямого виклику process() повз callUapki().
std::mutex  g_uapkiMtx;
// Writable-копія certs/crls у %TEMP%: CerStore ПИШЕ/перейменовує файли в certCache.path,
// тож указувати на read-only оригінали tests/data не можна.
std::wstring g_workDir;
// g_workDir читають/мутують ДВА потоки: головний (штатний вихід) і потік ctrl-хендлера,
// який ОС інжектує в процес. Серіалізуємо доступ окремим замком.
std::mutex   g_workDirMtx;

// Абсолютний шлях для JSON: forward-slashes (дослівно зі старого файлу, рядки 106-111).
std::string fwd(const std::wstring& w) {
    std::string s = w2u8(w);
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

json callUapki(const json& request) {
    std::lock_guard<std::mutex> lk(g_uapkiMtx);
    const std::string req = request.dump();
    char* res = process(req.c_str());
    if (!res) return json{{"errorCode", -1}, {"error", "process() повернув null"}};
    json out;
    try {
        out = json::parse(res);
    } catch (...) {
        out = json{{"errorCode", -2}, {"error", "невалідний JSON від process()"}};
    }
    json_free(res);
    return out;
}

void copyDirFiles(const std::wstring& src, const std::wstring& dst) {
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;   // немає/порожньо — не фатально
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            CopyFileW((src + L"\\" + n).c_str(), (dst + L"\\" + n).c_str(), FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void removeDirRec(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring full = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeDirRec(full);
            else                                                DeleteFileW(full.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

bool prepareWorkDir(const std::wstring& dataDir) {
    wchar_t tp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tp);
    if (n == 0 || n >= MAX_PATH) {
        std::printf("Не вдалося отримати %%TEMP%% (GetTempPathW)\n");
        return false;
    }
    // Ctrl-хендлер уже зареєстровано, тож публікуємо/скидаємо g_workDir під тим самим
    // замком, що й Shutdown() (одного g_workDirMtx досить — g_uapkiMtx тут не беремо,
    // тож циклу в порядку захоплення немає).
    const std::wstring dir = std::wstring(tp) + L"uapki_oracle_" + std::to_wstring(GetCurrentProcessId());
    {
        std::lock_guard<std::mutex> lk(g_workDirMtx);
        g_workDir = dir;
    }
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        std::printf("Не вдалося створити робочий каталог: %s\n", w2u8(dir).c_str());
        std::lock_guard<std::mutex> lk(g_workDirMtx);
        g_workDir.clear();
        return false;
    }
    copyDirFiles(dataDir + L"\\certs", dir + L"\\certs");
    copyDirFiles(dataDir + L"\\crls",  dir + L"\\crls");
    return true;
}

}  // namespace

std::wstring u8to16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string w2u8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string b64encode(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned)(unsigned char)in[i] << 16)
                   | ((unsigned)(unsigned char)in[i + 1] << 8)
                   |  (unsigned)(unsigned char)in[i + 2];
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];  out += T[v & 63];
    }
    if (i < in.size()) {
        const bool two = (i + 1 < in.size());
        unsigned v = (unsigned)(unsigned char)in[i] << 16;
        if (two) v |= (unsigned)(unsigned char)in[i + 1] << 8;
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += two ? T[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool b64decode(const std::string& b64, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    int acc = 0, nbits = 0;
    for (char c : b64) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        const int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out.push_back((char)((acc >> nbits) & 0xFF));
        }
    }
    return true;
}

// БУЛО: static void removeWorkDir()  (рядки 304-311, тіло без змін)
// Прибирання temp-копії сертів. Викликається при штатному завершенні й з ctrl-хендлера,
// а ctrl-хендлер виконується в ОКРЕМОМУ потоці, тож:
//   1) спершу чекаємо завершення in-flight process() (g_uapkiMtx) — інакше знесемо
//      certCache під CerStore, який саме туди пише (див. коментар до g_workDir);
//   2) серіалізуємо сам g_workDir (g_workDirMtx) — два Ctrl-C поспіль або Ctrl-C у момент,
//      коли головний потік уже прибирає, інакше давали б одночасні читання/мутації рядка.
// Ідемпотентність: g_workDir.clear() робиться ПІД замком, тож повторний виклик (з хендлера
// і з main) одразу виходить по empty(). На CTRL_CLOSE_EVENT ОС дає ~5 с — якщо process()
// не встигне, процес уб'ють і temp-каталог лишиться (не гірше за kill із Task Manager).
void Shutdown() {
    std::lock_guard<std::mutex> crypto(g_uapkiMtx);
    std::lock_guard<std::mutex> lk(g_workDirMtx);
    if (g_workDir.empty()) return;
    const std::wstring dir = g_workDir;
    g_workDir.clear();
    removeDirRec(dir);
}

// БУЛО: static bool cryptoBootstrap(const Config& cfg)  (рядки 355-409).
// Тіло ДОСЛІВНО; параметр тепер OracleConfig з тими самими іменами полів
// (providersDir, dataDir, keyPath, pass), тож рядки тіла не змінюються.
bool Init(const OracleConfig& cfg) {
    if (!prepareWorkDir(cfg.dataDir)) return false;

    json p;
    p["offline"] = true;                                   // тест-ланцюг прострочений: без OCSP/CRL/TSP
    p["cmProviders"]["dir"] = fwd(cfg.providersDir) + "/"; // dir — обов'язково із завершальним роздільником
    p["cmProviders"]["allowedProviders"] = json::array({ json{{"lib", ARCH_PROVIDER}} });
    std::wstring workDir;
    { std::lock_guard<std::mutex> lk(g_workDirMtx); workDir = g_workDir; }
    p["certCache"]["path"] = fwd(workDir + L"\\certs") + "/";
    p["crlCache"]["path"]  = fwd(workDir + L"\\crls") + "/";

    json r = callUapki(json{{"method", "INIT"}, {"parameters", p}});
    if (r.value("errorCode", -1) != 0) {
        std::printf("INIT помилка: %s\n", r.value("error", std::string()).c_str());
        return false;
    }
    // INIT віддає errorCode:0 навіть коли провайдер не завантажився — справжній
    // індикатор готовності саме countCmProviders.
    const int cnt = r.contains("result") ? r["result"].value("countCmProviders", 0) : 0;
    if (cnt != 1) {
        std::printf("Провайдер НКІ не завантажився (countCmProviders=%d, очікували 1); каталог: %s\n",
                    cnt, w2u8(cfg.providersDir).c_str());
        return false;
    }

    // OPEN: параметри (і пароль) у трейс НЕ виводимо (спека §3.5).
    json op;
    op["provider"] = "PKCS12";
    op["storage"]  = fwd(cfg.keyPath);
    op["password"] = cfg.pass;
    op["mode"]     = "RO";
    if (callUapki(json{{"method", "OPEN"}, {"parameters", op}}).value("errorCode", -1) != 0) {
        std::printf("OPEN тест-ключа не вдався: %s\n", w2u8(cfg.keyPath).c_str());
        return false;
    }

    json k = callUapki(json{{"method", "KEYS"}});
    if (k.value("errorCode", -1) != 0 || !k.contains("result")
        || !k["result"].contains("keys") || !k["result"]["keys"].is_array()
        || k["result"]["keys"].empty()) {
        std::printf("KEYS порожній — у контейнері немає доступних ключів\n");
        return false;
    }
    const std::string id = k["result"]["keys"][0].value("id", std::string());
    if (callUapki(json{{"method", "SELECT_KEY"}, {"parameters", json{{"id", id}}}})
            .value("errorCode", -1) != 0) {
        std::printf("SELECT_KEY не вдався\n");
        return false;
    }

    std::printf("Крипто-ядро готове: провайдер OK, ключ %s… вибрано\n", id.substr(0, 16).c_str());
    std::fflush(stdout);
    return true;
}

// БУЛО: static VerifyOutcome verifyCms(const std::string& derBytes)  (рядки 432-486, тіло без змін)
VerifyOutcome Verify(const std::string& derBytes) {
    VerifyOutcome o;
    // try/catch навколо розбору: сюди приходить довільне тіло HTTP-запиту, а
    // nlohmann кидає при несподіваних типах полів — падати сервером не можна.
    try {
        json p;
        p["signature"]["bytes"]        = b64encode(derBytes);
        p["options"]["validationType"] = "STRUCT";   // офлайн: чисто структурна перевірка
        p["options"]["returnContent"]  = true;       // потрібен вкладений вміст (enveloping)

        json r = callUapki(json{{"method", "VERIFY"}, {"parameters", p}});
        o.errorCode = r.value("errorCode", -1L);
        if (r.contains("error") && r["error"].is_string())
            o.errorText = r["error"].get<std::string>();
        if (o.errorCode != 0 || !r.contains("result") || !r["result"].is_object())
            return o;   // не розібралось як CMS -> accepted лишається false

        const json& res = r["result"];
        if (res.contains("content") && res["content"].is_object()
            && res["content"].contains("bytes") && res["content"]["bytes"].is_string())
            o.contentB64 = res["content"]["bytes"].get<std::string>();

        if (!res.contains("signatureInfos") || !res["signatureInfos"].is_array()
            || res["signatureInfos"].empty())
            return o;

        const json& si = res["signatureInfos"][0];
        o.status       = si.value("status", std::string());
        o.statusSig    = si.value("statusSignature", std::string());
        o.statusMd     = si.value("statusMessageDigest", std::string());
        o.validSig     = si.value("validSignatures", false);
        o.validDig     = si.value("validDigests", false);
        o.signerCertId = si.value("signerCertId", std::string());

        if (!o.signerCertId.empty() && res.contains("certIds") && res["certIds"].is_array()) {
            for (const auto& e : res["certIds"]) {
                if (e.is_string() && e.get<std::string>() == o.signerCertId) {
                    o.certEmbedded = true;
                    break;
                }
            }
        }

        // КРИТЕРІЙ ПРИЙНЯТТЯ (docs/integration-1c/uapki.md §4.5): холістичний status +
        // validSignatures + validDigests. statusSignature=="VALID" НЕДОСТАТНЬО — він
        // лишається "VALID" навіть при пошкодженому вмісті (підпис над signedAttributes
        // при цьому коректний), а ловить пошкодження саме status/validDigests.
        o.accepted = (o.status == "TOTAL-VALID") && o.validSig && o.validDig;
    } catch (const std::exception& e) {
        std::printf("  VERIFY: виняток при розборі відповіді: %s\n", e.what());
        std::fflush(stdout);
        o.accepted = false;
    }
    return o;
}

// БУЛО: static std::string signData(const std::string& rawBytes)  (рядки 489-519, тіло без змін)
std::string Sign(const std::string& rawBytes) {
    try {
        json sp;
        sp["signatureFormat"]  = "CAdES-BES";
        sp["signAlgo"]         = "1.2.804.2.1.1.1.1.3.1.1";   // ДСТУ 4145
        sp["detachedData"]     = false;                        // enveloping: вміст усередині CMS
        sp["includeCert"]      = true;
        sp["includeTime"]      = true;
        sp["includeContentTS"] = false;                        // офлайн: TSP недоступний

        json p;
        p["signParams"] = sp;
        p["dataTbs"]    = json::array({ json{{"id", "doc-0"}, {"bytes", b64encode(rawBytes)}} });
        p["options"]["ignoreCertStatus"] = true;               // тест-ланцюг прострочений

        json r = callUapki(json{{"method", "SIGN"}, {"parameters", p}});
        if (r.value("errorCode", -1) != 0 || !r.contains("result")
            || !r["result"].contains("signatures") || !r["result"]["signatures"].is_array()
            || r["result"]["signatures"].empty())
            return std::string();

        const std::string b64 = r["result"]["signatures"][0].value("bytes", std::string());
        std::string der;
        if (b64.empty() || !b64decode(b64, der)) return std::string();
        return der;
    } catch (const std::exception& e) {
        std::printf("  SIGN: виняток при розборі відповіді: %s\n", e.what());
        std::fflush(stdout);
        return std::string();
    }
}

}  // namespace oracle
