//
// @file tests/native_host.cpp
// @brief L2-e2e + L3-крос-валідаційний харнес компоненти ЕЦП AddinUAPKIConnect.
//        Емулює платформу 1С: вантажить головну DLL через LoadLibraryW,
//        отримує IComponentBase через експорт GetClassObject, надає власні
//        IAddInDefBase та IMemoryManager і викликає метод "CallUapki" точно
//        так, як це робить платформа (tVariant VTYPE_PWSTR у параметрах і в
//        результаті). Крипто-ядро НЕ лінкується — усе працює через головну DLL.
//
// Це окремий консольний exe; PCH головного проєкту НЕ підключається, дозволено printf.
//

#ifndef _WIN32

#include <cstdio>
int main() {
    printf("native_host: Windows only\n");
    return 0;
}

#else // _WIN32

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW (windows.h не тягне його при WIN32_LEAN_AND_MEAN)
#include <psapi.h>      // EnumProcessModules — перепис модулів для кейса 12
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <algorithm>

#include <nlohmann/json.hpp>

#include "support/LocalKeys.h"

#pragma comment(lib, "shell32.lib")  // CommandLineToArgvW
#pragma comment(lib, "psapi.lib")    // EnumProcessModules

// SDK 1С (include/). types.h визначає WCHAR_T=wchar_t та ADDIN_API=__stdcall
// лише коли задано _WINDOWS — його задає CMake для цієї цілі (див. CMakeLists).
#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"

using nlohmann::json;

// --- Архітектурний суфікс імен DLL --------------------------------------
#ifdef _WIN64
#  define ARCH_W L"_x64"
#else
#  define ARCH_W L"_x86"
#endif

// --- Дефолтні шляхи, впаяні CMake (target_compile_definitions) ----------
#ifndef HOST_BIN_DIR
#  define HOST_BIN_DIR ""
#endif
#ifndef HOST_DATA_DIR
#  define HOST_DATA_DIR ""
#endif

// Ідентифікатор ключа у test-diia.p12 (див. sign-pkcs12-*.json)
static const char* KEY_ID =
    "5BC6C06EE1E00C1700E92AA7A9AD75F82D3CB7A9B66E3A98023209B24513315C";
// "The quick brown fox jumps over the lazy dog" у base64
static const char* DATA_TBS_B64 =
    "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wcyBvdmVyIHRoZSBsYXp5IGRvZw==";

// Другий ключ того самого контейнера — ШИФРУВАЛЬНИЙ (див. tests/scenarios/06_encrypt_decrypt.json).
// Український КЕП-контейнер завжди двоключовий; test-diia.p12 тут не виняток, а типовий зразок.
static const char* KEY_ID_ENCRYPT =
    "6B1B77C0D1A1B60473A98DD6D4FE5302742AEDE101DAA21F2C83A67CCDEDB782";

// Префікси імен сертифікатів у tests/data/certs/. CerStore іменує файли
// "<AKI>-<SKI>-<thumbprint>.cer", тож префікс однозначно адресує сертифікат за
// ідентифікатором його ключа, а хвіст (thumbprint) у тесті фіксувати не треба.
// Звірено openssl: SKI підписного == KEY_ID, SKI шифрувального == KEY_ID_ENCRYPT;
// keyUsage підписного — Digital Signature + Non Repudiation, шифрувального — Key Agreement;
// СУБ'ЄКТ В ОБОХ ОДНАКОВИЙ, тож розрізнити їх можна ВИКЛЮЧНО за keyUsage.
static const wchar_t* CERT_PREFIX_SIGN    = L"BED50831-5BC6C06E-";
static const wchar_t* CERT_PREFIX_ENCRYPT = L"BED50831-6B1B77C0-";

// ========================================================================
// Конвертації рядків UTF-8 <-> UTF-16
// ========================================================================
static std::wstring u8to16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string u16to8(const wchar_t* p, size_t len) {
    if (!p || len == 0) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, p, (int)len, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, p, (int)len, &s[0], n, nullptr, nullptr);
    return s;
}
static std::string w2u8(const std::wstring& w) { return u16to8(w.c_str(), w.size()); }
static std::string wz2u8(const WCHAR_T* p) {
    if (!p) return std::string();
    return u16to8(reinterpret_cast<const wchar_t*>(p), wcslen(reinterpret_cast<const wchar_t*>(p)));
}
// Абсолютний шлях у JSON: forward-slashes, щоб уникнути екранування
static std::string fwd(const std::wstring& w) {
    std::string s = w2u8(w);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Екранування рядка для вставки у вручну зібраний JSON (printf-складання, не nlohmann::json).
// Той самий підхід, що й jsonEscape у tests/iit_verify.cpp: без нього шлях із зворотними
// слешами (звичайний Windows-шлях) робить рядок невалідним JSON — "\g" не є коректною
// escape-послідовністю, і жоден строгий парсер (напр. ConvertFrom-Json) його не прочитає.
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

// ========================================================================
// Емуляція платформи 1С: менеджер пам'яті та об'єкт-з'єднання
// ========================================================================
// Головна DLL повертає рядок-результат, виділяючи буфер саме ЦИМ менеджером
// (AddInNative::AllocMemory -> m_iMemory->AllocMemory). malloc/free тут —
// щоб потім коректно звільнити ret.pwstrVal через free().
class HostMemoryManager : public IMemoryManager {
public:
    bool ADDIN_API AllocMemory(void** pMemory, unsigned long ulCountByte) override {
        *pMemory = malloc(ulCountByte);
        return *pMemory != nullptr;
    }
    void ADDIN_API FreeMemory(void** pMemory) override {
        if (pMemory && *pMemory) { free(*pMemory); *pMemory = nullptr; }
    }
};

// Мінімальна реалізація IAddInDefBase: AddError друкує джерело+опис, решта —
// нейтральні заглушки (true/no-op/0), достатні для життєвого циклу компоненти.
class HostConnect : public IAddInDefBase {
public:
    bool ADDIN_API AddError(unsigned short wcode, const WCHAR_T* source,
                            const WCHAR_T* descr, long scode) override {
        printf("  [AddError] wcode=%u scode=%ld src=%s descr=%s\n",
               (unsigned)wcode, scode, wz2u8(source).c_str(), wz2u8(descr).c_str());
        return true;
    }
    bool ADDIN_API Read(WCHAR_T* /*wszPropName*/, tVariant* /*pVal*/,
                        long* /*pErrCode*/, WCHAR_T** /*errDescriptor*/) override { return false; }
    bool ADDIN_API Write(WCHAR_T* /*wszPropName*/, tVariant* /*pVar*/) override { return true; }
    bool ADDIN_API RegisterProfileAs(WCHAR_T* /*wszProfileName*/) override { return true; }
    bool ADDIN_API SetEventBufferDepth(long /*lDepth*/) override { return true; }
    long ADDIN_API GetEventBufferDepth() override { return 0; }
    bool ADDIN_API ExternalEvent(WCHAR_T* /*wszSource*/, WCHAR_T* /*wszMessage*/,
                                 WCHAR_T* /*wszData*/) override { return true; }
    void ADDIN_API CleanEventBuffer() override {}
    bool ADDIN_API SetStatusLine(WCHAR_T* /*wszStatusLine*/) override { return true; }
    void ADDIN_API ResetStatusLine() override {}
};

// ========================================================================
// Обгортка над завантаженою компонентою
// ========================================================================
struct Component {
    HMODULE          h       = nullptr;
    IComponentBase*  comp    = nullptr;
    DestroyObjectPtr destroy = nullptr;
    long             callIdx = -1;
    HostMemoryManager mem;
    HostConnect       conn;

    // Вантажить головну DLL, створює об'єкт "AddinUAPKIConnect" і проганяє
    // послідовність ініціалізації точно як платформа 1С.
    bool load(const std::wstring& dllPath) {
        h = LoadLibraryW(dllPath.c_str());
        if (!h) {
            printf("FAIL: LoadLibraryW('%s') err=%lu\n", w2u8(dllPath).c_str(), GetLastError());
            return false;
        }
        auto pGetClassObject = (GetClassObjectPtr)GetProcAddress(h, "GetClassObject");
        destroy              = (DestroyObjectPtr)GetProcAddress(h, "DestroyObject");
        auto pGetClassNames  = (GetClassNamesPtr)GetProcAddress(h, "GetClassNames");
        if (!pGetClassObject || !destroy || !pGetClassNames) {
            printf("FAIL: GetProcAddress (GetClassObject/DestroyObject/GetClassNames) err=%lu\n", GetLastError());
            return false;
        }
        printf("  ClassNames: %s\n", wz2u8(pGetClassNames()).c_str());

        comp = nullptr; // GetClassObject вимагає *pIntf == nullptr
        pGetClassObject(L"AddinUAPKIConnect", &comp);
        if (!comp) { printf("FAIL: GetClassObject('AddinUAPKIConnect') повернув nullptr\n"); return false; }

        if (!comp->Init((void*)&conn))            { printf("FAIL: Init повернув false\n"); return false; }
        if (!comp->setMemManager((void*)&mem))    { printf("FAIL: setMemManager повернув false\n"); return false; }
        long info = comp->GetInfo();
        printf("  GetInfo=%ld\n", info);

        callIdx = comp->FindMethod(L"CallUapki");
        if (callIdx < 0) { printf("FAIL: FindMethod('CallUapki') = %ld\n", callIdx); return false; }
        return true;
    }

    // Виклик CallUapki(method, jsonParams) -> рядок JSON-відповіді.
    std::string call(const std::string& method, const std::string& jsonParams) {
        std::wstring wm = u8to16(method);
        std::wstring wp = u8to16(jsonParams);

        tVariant params[2];
        tVarInit(&params[0]);
        params[0].vt       = VTYPE_PWSTR;
        params[0].pwstrVal = (WCHAR_T*)wm.c_str();
        params[0].wstrLen  = (uint32_t)wm.size();
        tVarInit(&params[1]);
        params[1].vt       = VTYPE_PWSTR;
        params[1].pwstrVal = (WCHAR_T*)wp.c_str();
        params[1].wstrLen  = (uint32_t)wp.size();

        tVariant ret;
        tVarInit(&ret);
        bool ok = comp->CallAsFunc(callIdx, &ret, params, 2);

        std::string out;
        if (ret.vt == VTYPE_PWSTR && ret.pwstrVal) {
            out = u16to8(reinterpret_cast<const wchar_t*>(ret.pwstrVal), ret.wstrLen);
            free(ret.pwstrVal); // виділено нашим HostMemoryManager через malloc
        } else if (!ok) {
            printf("  [call] CallAsFunc('%s') повернув false без рядка-результату\n", method.c_str());
        }
        return out;
    }

    // Виклик EnableLogging(logLevel, logFilePath) — базовий метод, успадкований
    // усіма компонентами (AddInNative.cpp:90), функція з двома рядковими
    // параметрами й bool-результатом. Від'ємний FindMethod — тихого успіху
    // бути не може, повертаємо false.
    bool enableLogging(const std::wstring& logLevel, const std::wstring& logFilePath) {
        long idx = comp->FindMethod(L"EnableLogging");
        if (idx < 0) { printf("FAIL: FindMethod('EnableLogging') = %ld\n", idx); return false; }

        tVariant params[2];
        tVarInit(&params[0]);
        params[0].vt       = VTYPE_PWSTR;
        params[0].pwstrVal = (WCHAR_T*)logLevel.c_str();
        params[0].wstrLen  = (uint32_t)logLevel.size();
        tVarInit(&params[1]);
        params[1].vt       = VTYPE_PWSTR;
        params[1].pwstrVal = (WCHAR_T*)logFilePath.c_str();
        params[1].wstrLen  = (uint32_t)logFilePath.size();

        tVariant ret;
        tVarInit(&ret);
        bool ok = comp->CallAsFunc(idx, &ret, params, 2);
        if (!ok) { printf("FAIL: CallAsFunc('EnableLogging') повернув false\n"); return false; }
        if (ret.vt != VTYPE_BOOL) { printf("FAIL: EnableLogging повернув неочікуваний тип vt=%d\n", (int)ret.vt); return false; }
        return ret.bVal;
    }

    void unload() {
        if (comp && destroy) destroy(&comp);
        comp = nullptr;
        if (h) { FreeLibrary(h); h = nullptr; }
    }

    // RAII: гарантує знищення об'єкта й вивантаження DLL навіть на ранніх
    // return false у кейсах (unload() ідемпотентний, тож явні виклики нижче
    // лишаються безпечними).
    ~Component() { unload(); }
};

// ========================================================================
// JSON-хелпери
// ========================================================================
// Парсить відповідь; повертає errorCode (або від'ємний код розбору) та json.
static long errCode(const std::string& resp, json& j) {
    try { j = json::parse(resp); }
    catch (...) { printf("  [json] не вдалося розпарсити відповідь: %s\n", resp.c_str()); return -999; }
    if (!j.contains("errorCode")) return -998;
    try { return j["errorCode"].get<long>(); }
    catch (...) { return -997; }
}

// ========================================================================
// Файлові/каталогові WinAPI-хелпери
// ========================================================================
static bool pathExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static void rmrf(const std::wstring& dir) {
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pattern.c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) { RemoveDirectoryW(dir.c_str()); return; }
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmrf(full);
        else { SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(full.c_str()); }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    RemoveDirectoryW(dir.c_str());
}
static bool findFileRec(const std::wstring& dir, const std::wstring& name, std::wstring& found) {
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pattern.c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring full = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (findFileRec(full, name, found)) { ok = true; break; }
        } else if (_wcsicmp(n.c_str(), name.c_str()) == 0) {
            found = full; ok = true; break;
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return ok;
}
static std::wstring localAppDataApp() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(buf) + L"\\SimplyAddinConnect";
}
static std::wstring makeTempDir(const wchar_t* tag) {
    wchar_t tp[MAX_PATH];
    GetTempPathW(MAX_PATH, tp);
    std::wstring d = std::wstring(tp) + L"sac_" + tag + L"_" + std::to_wstring(GetCurrentProcessId());
    rmrf(d);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
static bool readFileBytes(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(hf, &sz)) { CloseHandle(hf); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    bool ok = out.empty() ? true :
              (ReadFile(hf, out.data(), (DWORD)out.size(), &rd, nullptr) && rd == out.size());
    CloseHandle(hf);
    return ok;
}
static bool readFileText(const std::wstring& path, std::string& out) {
    std::vector<unsigned char> raw;
    if (!readFileBytes(path, raw)) return false;
    out.assign(raw.begin(), raw.end());
    return true;
}
static std::string b64encode(const std::vector<unsigned char>& in) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];  out += T[v & 63];
    }
    if (i < in.size()) {
        bool two = (i + 1 < in.size());
        unsigned v = in[i] << 16; if (two) v |= in[i + 1] << 8;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += two ? T[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// Зворотне до b64encode: base64 -> байти. Пробіли й переноси ігноруються,
// сторонній символ -> false (щоб зіпсована відповідь не пішла у файл мовчки).
static bool b64decode(const std::string& b64, std::vector<unsigned char>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    out.reserve((b64.size() / 4) * 3);
    int acc = 0, nbits = 0;
    for (char c : b64) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        const int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out.push_back((unsigned char)((acc >> nbits) & 0xFF));
        }
    }
    return true;
}
static bool writeFileBytes(const std::wstring& path, const std::vector<unsigned char>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(h);
    return ok != 0 && written == data.size();
}

// Знаходить файл сертифіката за ПРЕФІКСОМ імені (CERT_PREFIX_*). Перший збіг —
// єдиний: у tests/data/certs/ на кожен SKI припадає рівно один файл.
static bool findCertByPrefix(const std::wstring& dir, const std::wstring& prefix,
                             std::wstring& found) {
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"\\" + prefix + L"*.cer").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return false;
    found = dir + L"\\" + fd.cFileName;
    FindClose(hf);
    return true;
}

// Читає DER-сертифікат і віддає base64 — рівно той формат, у якому сертифікат
// приходить із бази 1С у ADD_CERT.
static bool readCertB64(const std::wstring& path, std::string& b64) {
    std::vector<unsigned char> raw;
    if (!readFileBytes(path, raw) || raw.empty()) return false;
    b64 = b64encode(raw);
    return true;
}

// Порівняння hex-ідентифікаторів без урахування регістру: UAPKI віддає `id` у
// різних відповідях через різні шляхи, і покладатись на однаковий регістр не можна.
static std::string upperAscii(std::string s) {
    for (char& ch : s) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    return s;
}

// Друк відповіді SELECT_KEY без ВМІСТУ сертифіката: сам сертифікат — особистий
// документ розробника, а вивід гейта потрапляє у звіти. Усе, заради чого спека §9
// п.4 вимагає сирий друк (перелік полів і наявність certId), лишається видимим.
static std::string elideCert(const std::string& resp) {
    json j;
    try { j = json::parse(resp); } catch (...) { return resp; }
    if (j.contains("result") && j["result"].is_object() && j["result"].contains("certificate")) {
        const std::string cert = j["result"].value("certificate", std::string());
        j["result"]["certificate"] = "<" + std::to_string(cert.size()) + " символів base64>";
    }
    return j.dump();
}

// Макрос перевірки: провал -> друк і повернення false з кейса.
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); return false; } \
    printf("  ok: %s\n", (msg)); \
} while (0)

// ========================================================================
// Спільні будівники запитів
// ========================================================================
static std::string buildInit(bool offline) {
    json p; p["offline"] = offline;
    return p.dump();
}
static std::string buildOpen(const std::wstring& p12Path) {
    json p;
    p["provider"] = "PKCS12";
    p["storage"]  = fwd(p12Path);
    p["password"] = "testpassword";
    p["mode"]     = "RO";
    return p.dump();
}
// Той самий OPEN, але в ПЛОСКОМУ форматі "ключ=значення" — це друга гілка розбору в
// ExecuteUapkiCommand (ParseParamsString), і саме її docs/integration-1c/uapki.md радить
// «для простих методів без вкладень». Пароль тут іде в компоненту сирим рядком, тож
// перевірка «пароля немає в лозі» мусить покривати обидва формати, а не лише JSON.
static std::string buildOpenFlat(const std::wstring& p12Path) {
    return "provider=PKCS12,storage=" + fwd(p12Path) + ",password=testpassword,mode=RO";
}
static std::string buildSign() {
    json sp;
    sp["signatureFormat"]  = "CAdES-BES";
    sp["signAlgo"]         = "1.2.804.2.1.1.1.1.3.1.1";
    sp["detachedData"]     = false;
    sp["includeCert"]      = true;
    sp["includeTime"]      = true;
    sp["includeContentTS"] = false;
    json d; d["id"] = "doc-0"; d["bytes"] = DATA_TBS_B64;
    json p;
    p["signParams"]        = sp;
    p["dataTbs"]           = json::array({ d });
    p["options"]["ignoreCertStatus"] = true;
    return p.dump();
}

// ========================================================================
// КЕЙС 1 — ресурсне розгортання провайдера (головний доказ)
// ========================================================================
static bool case1_resourceDeploy(const std::wstring& mainDllSrc) {
    printf("== Case 1: ресурсне розгортання провайдера ==\n");
    std::wstring appDir = localAppDataApp();
    CHECK(!appDir.empty(), "LOCALAPPDATA визначено");

    // Чистимо стан
    rmrf(appDir);
    // Найчастіша причина невдачі — каталог тримає ЗАПУЩЕНА 1С із раніше підключеною компонентою:
    // вона завантажила cm-pkcs12_*.dll із providers/<версія>/, і Windows не дає видалити файл.
    // Без цієї підказки кейс падав глухим FAIL, і причину доводилось шукати щоразу наново.
    if (pathExists(appDir)) {
        printf("  FAIL: не вдалося прибрати %s\n"
               "        Найімовірніше каталог тримає запущена 1С (провайдер cm-pkcs12 завантажений\n"
               "        у процес 1cv8 після підключення компоненти). Закрийте 1С і повторіть прогін.\n",
               w2u8(appDir).c_str());
        return false;
    }
    printf("  ok: %%LOCALAPPDATA%%\\SimplyAddinConnect прибрано\n");

    // Тимч. каталог ТІЛЬКИ з головною DLL (без провайдера поруч)
    std::wstring tmp = makeTempDir(L"case1");
    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    std::wstring dllDst  = tmp + L"\\" + dllName;
    CHECK(CopyFileW(mainDllSrc.c_str(), dllDst.c_str(), FALSE) != 0, "скопійовано головну DLL у чистий каталог");
    std::wstring provName = std::wstring(L"cm-pkcs12") + ARCH_W + L".dll";
    CHECK(!pathExists(tmp + L"\\" + provName), "провайдера поруч НЕМАЄ");

    Component c;
    if (!c.load(dllDst)) return false;

    std::string resp = c.call("INIT", ""); // порожні параметри -> авто-конфіг провайдера
    printf("  INIT resp: %s\n", resp.c_str());
    json j; long ec = errCode(resp, j);
    CHECK(ec == 0, "INIT errorCode == 0");
    CHECK(j["result"].contains("countCmProviders"), "result.countCmProviders присутній");
    long cnt = j["result"]["countCmProviders"].get<long>();
    printf("  countCmProviders=%ld\n", cnt);
    CHECK(cnt == 1, "countCmProviders == 1");

    // Головний доказ: провайдер розгорнуто під %LOCALAPPDATA%/.../providers/<ver>/
    std::wstring providers = appDir + L"\\providers";
    CHECK(pathExists(providers), "каталог providers створено розгортанням");
    std::wstring found;
    CHECK(findFileRec(providers, provName, found), "файл провайдера знайдено під providers");
    printf("  провайдер розгорнуто: %s\n", w2u8(found).c_str());

    c.call("DEINIT", "");
    c.unload();
    rmrf(tmp);
    return true;
}

// ========================================================================
// КЕЙС 2 — провайдер поруч із DLL (розгортання НЕ відбувається)
// ========================================================================
static bool case2_providerBeside(const std::wstring& mainDllSrc, const std::wstring& binDir) {
    printf("== Case 2: провайдер поруч із DLL ==\n");
    std::wstring appDir = localAppDataApp();
    CHECK(!appDir.empty(), "LOCALAPPDATA визначено");
    rmrf(appDir);
    // Та сама пастка, що й у кейсі 1: кейс доводить ВІДСУТНІСТЬ розгортання, тож починати
    // мусить з чистого каталогу — інакше залишок від запущеної 1С дасть хибний FAIL наприкінці.
    if (pathExists(appDir)) {
        printf("  FAIL: не вдалося прибрати %s\n"
               "        Найімовірніше каталог тримає запущена 1С (провайдер cm-pkcs12 завантажений\n"
               "        у процес 1cv8 після підключення компоненти). Закрийте 1С і повторіть прогін.\n",
               w2u8(appDir).c_str());
        return false;
    }

    std::wstring tmp = makeTempDir(L"case2");
    std::wstring dllName  = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    std::wstring provName = std::wstring(L"cm-pkcs12") + ARCH_W + L".dll";
    CHECK(CopyFileW(mainDllSrc.c_str(), (tmp + L"\\" + dllName).c_str(), FALSE) != 0, "скопійовано головну DLL");
    CHECK(CopyFileW((binDir + L"\\" + provName).c_str(), (tmp + L"\\" + provName).c_str(), FALSE) != 0,
          "скопійовано провайдера ПОРУЧ із DLL");

    Component c;
    if (!c.load(tmp + L"\\" + dllName)) return false;

    std::string resp = c.call("INIT", "");
    printf("  INIT resp: %s\n", resp.c_str());
    json j; long ec = errCode(resp, j);
    CHECK(ec == 0, "INIT errorCode == 0");
    CHECK(j["result"].contains("countCmProviders")
          && j["result"]["countCmProviders"].get<long>() == 1, "countCmProviders == 1");

    // Розгортання НЕ мало відбутись -> каталог providers відсутній
    CHECK(!pathExists(appDir + L"\\providers"), "розгортання НЕ відбулось (providers відсутній)");

    c.call("DEINIT", "");
    c.unload();
    rmrf(tmp);
    return true;
}

// ========================================================================
// КЕЙС 3 — явний cmProviders.dir
// ========================================================================
static bool case3_explicitDir(const std::wstring& binDir) {
    printf("== Case 3: явний cmProviders.dir ==\n");
    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json p;
    // dir із завершальним роздільником: ядро конкатенує dir+ім'я БЕЗ вставки '/'
    p["cmProviders"]["dir"] = fwd(binDir + L"\\");
    p["cmProviders"]["allowedProviders"] = json::array({ json{{"lib", "cm-pkcs12"}} });
    std::string resp = c.call("INIT", p.dump());
    printf("  INIT resp: %s\n", resp.c_str());
    json j; long ec = errCode(resp, j);
    CHECK(ec == 0, "INIT errorCode == 0");
    CHECK(j["result"]["countCmProviders"].get<long>() == 1, "countCmProviders == 1 (хелпер дописав арх-суфікс, поважає dir)");

    c.call("DEINIT", "");
    c.unload();
    return true;
}

// ========================================================================
// КЕЙС 4 — повний ланцюг OPEN/KEYS/SELECT_KEY/SIGN/VERIFY (L2 + L3.2)
// ========================================================================
static bool case4_fullChain(const std::wstring& binDir, const std::wstring& dataDir) {
    printf("== Case 4: повний ланцюг ЕЦП (e2e) + L3.2 структура підпису ==\n");
    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    std::wstring p12     = dataDir + L"\\test-diia.p12";
    CHECK(pathExists(p12), "test-diia.p12 присутній");

    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    // INIT (offline + кеш сертифікатів/СВС: сертифікат підписувача лежить у
    // certs/, а НЕ в контейнері p12 — без certCache includeCert -> CERT_NOT_FOUND)
    { json ip;
      ip["offline"] = true;
      ip["certCache"]["path"] = fwd(dataDir + L"\\certs\\");
      ip["certCache"]["trustedCerts"] = json::array();
      ip["crlCache"]["path"] = fwd(dataDir + L"\\crls\\");
      std::string r = c.call("INIT", ip.dump());
      printf("  INIT: %s\n", r.c_str());
      CHECK(errCode(r, j) == 0, "INIT errorCode == 0");
    }
    std::string r;

    // OPEN
    r = c.call("OPEN", buildOpen(p12));
    printf("  OPEN: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "OPEN errorCode == 0");

    // KEYS
    r = c.call("KEYS", "");
    printf("  KEYS: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "KEYS errorCode == 0");

    // SELECT_KEY
    { json p; p["id"] = KEY_ID;
      r = c.call("SELECT_KEY", p.dump()); }
    printf("  SELECT_KEY: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "SELECT_KEY errorCode == 0");

    // SIGN
    r = c.call("SIGN", buildSign());
    printf("  SIGN: %s\n", r.substr(0, 300).c_str());
    CHECK(errCode(r, j) == 0, "SIGN errorCode == 0");
    CHECK(j["result"].contains("signatures") && j["result"]["signatures"].is_array()
          && !j["result"]["signatures"].empty(), "result.signatures непорожній");
    std::string sig = j["result"]["signatures"][0].value("bytes", std::string());
    CHECK(!sig.empty(), "підпис (signatures[0].bytes) не порожній");

    // VERIFY
    { json p; p["signature"]["bytes"] = sig;
      r = c.call("VERIFY", p.dump()); }
    printf("  VERIFY: %s\n", r.substr(0, 400).c_str());
    CHECK(errCode(r, j) == 0, "VERIFY errorCode == 0");

    // --- L3.2: структурна перевірка ОФЛАЙН-профілю ПРРО (CAdES-BES) ---
    // МЕЖА ПОКРИТТЯ, читай уважно:
    //   * норматив ДПС вимагає CAdES-E-T із signature-time-stamp для онлайн-документів
    //     («Опис АРІ фіскального сервера (ЄВПЕЗ)», розділ «Порядок засвідчення повідомлень»);
    //   * позначка часу НЕ обов'язкова лише для документів, створених в офлайні — саме цей
    //     профіль тут і перевіряється;
    //   * тому signatureTS відсутній ПРАВОМІРНО, а не «так має бути завжди»;
    //   * онлайн-шлях (похід у TSP) не покритий ЖОДНИМ тестом: код
    //     extern/uapki/library/uapki/src/doc-sign.cpp:864-877 не виконувався ніколи.
    // Доказом коректності самого підпису є вердикт стороннього двигуна (рівень L4-iit),
    // а не наш власний VERIFY нижче — той є РЕГРЕСІЙНОЮ перевіркою.
    auto& res = j["result"];
    CHECK(res.contains("signatureInfos") && res["signatureInfos"].is_array()
          && !res["signatureInfos"].empty(), "result.signatureInfos присутній");
    auto& si = res["signatureInfos"][0];
    std::string ss = si.value("statusSignature", std::string());
    printf("  statusSignature=%s signatureFormat=%s\n",
           ss.c_str(), si.value("signatureFormat", std::string()).c_str());
    CHECK(ss.rfind("VALID", 0) == 0, "statusSignature починається з VALID");
    CHECK(si.value("signatureFormat", std::string()) == "CAdES-BES", "signatureFormat == CAdES-BES");
    // includeCert:true -> сертифікат вкладено
    CHECK(res.contains("certIds") && res["certIds"].is_array() && !res["certIds"].empty(),
          "вкладений сертифікат присутній (certIds непорожній)");
    CHECK(si.contains("signerCertId"), "signerCertId присутній");
    // includeContentTS:false -> content-time-stamp відсутній
    CHECK(!si.contains("contentTS"), "content-time-stamp ВІДСУТНІЙ");
    CHECK(!si.contains("signatureTS"), "signature-time-stamp відсутній");
    CHECK(!si.contains("archiveTS"), "archive-time-stamp відсутній");
    // CAdES-BES -> у контейнері немає CRL/OCSP-посилань та значень
    CHECK(!si.contains("revocationRefs") && !si.contains("certValues")
          && !si.contains("certificateRefs"),
          "CRL/OCSP у контейнері відсутні (мінімальний CAdES-BES)");

    // CLOSE + DEINIT
    r = c.call("CLOSE", "");
    printf("  CLOSE: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "CLOSE errorCode == 0");
    c.call("DEINIT", "");
    c.unload();
    return true;
}

// ========================================================================
// КЕЙС 5 — L3.1 крос-валідація на еталонах ДФС (опційно)
// ========================================================================
// Рекурсивний збір *.signed. Еталони ДПС лежать НЕ в корені prro_docs, а трьома рівнями
// глибше («…/Єдине вікно…/Приклади/Приклади з КЕП»), тож нерекурсивний пошук у корені завжди
// давав порожній список -> «SKIP» -> exit 0 -> гейт малював PASS. Порожня перевірка читалась
// як покриття, тому пошук тепер рекурсивний, а «нічого не знайдено» — окремий статус (skipped).
static void collectSignedRec(const std::wstring& dir, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring full = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collectSignedRec(full, out);
        } else if (n.size() >= 7 && _wcsicmp(n.c_str() + (n.size() - 7), L".signed") == 0) {
            out.push_back(full);
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
}

// skipped=true -> еталонів немає (кейс не виконувався). Викликач мусить показати це як SKIP,
// а НЕ як PASS: інакше відсутність даних невідрізненна від успішної перевірки.
static bool case5_crossValidatePrro(const std::wstring& binDir, const std::wstring& prroDir,
                                    bool& skipped) {
    printf("== Case 5: L3.1 крос-валідація еталонів ДФС ==\n");
    skipped = false;
    if (prroDir.empty() || !pathExists(prroDir)) {
        printf("SKIP (prro_docs недоступний)\n");
        skipped = true;
        return true; // не провал, але й НЕ покриття
    }
    std::vector<std::wstring> files;
    collectSignedRec(prroDir, files);
    if (files.empty()) {
        printf("SKIP (prro_docs без *.signed файлів)\n");
        skipped = true;
        return true;
    }
    printf("  знайдено еталонів: %zu\n", files.size());

    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    std::string r = c.call("INIT", buildInit(true));
    CHECK(errCode(r, j) == 0, "INIT errorCode == 0");

    bool allOk = true;
    for (const auto& f : files) {
        printf("  -- %s\n", w2u8(f).c_str());
        std::vector<unsigned char> raw;
        if (!readFileBytes(f, raw) || raw.empty()) { printf("  FAIL: не прочитано файл\n"); allOk = false; continue; }
        json p;
        p["signature"]["bytes"]        = b64encode(raw);
        p["options"]["validationType"] = "STRUCT";   // офлайн: без OCSP/CRL/TSP
        r = c.call("VERIFY", p.dump());
        const long ec = errCode(r, j);

        // result заповнюється навіть при errorCode != 0: api-json.cpp створює jo_result ДО
        // виклику методу й при помилці не чистить — саме там лежить діагностика.
        json* si = nullptr;
        if (j.contains("result") && j["result"].is_object()) {
            auto& res = j["result"];
            if (res.contains("signatureInfos") && res["signatureInfos"].is_array()
                && !res["signatureInfos"].empty())
                si = &res["signatureInfos"][0];
        }
        const std::string st = si ? si->value("status", std::string()) : std::string();
        const std::string ss = si ? si->value("statusSignature", std::string()) : std::string();
        const bool validDig  = si ? si->value("validDigests", false) : false;
        // signerCertId — теж частина СТРУКТУРИ підпису (без нього нема кого перевіряти),
        // а не властивість довіри/строку дії; тому лишається в критерії FAIL поруч зі
        // statusSignature/validDigests, як і в кейсі 5 до цієї правки.
        const bool hasSigner = si && si->contains("signerCertId")
                            && !si->value("signerCertId", std::string()).empty();

        // РАНІШЕ: errorCode 4161 (CERT_NOT_FOUND) беззастережно вважався прийнятним, бо
        // офлайн бракує сертифіката TSP-сервера. Це судження жило всередині тесту й могло
        // маскувати справжній дефект (напр., прострочений сертифікат підписувача, якого
        // STRUCT не перевіряє). Тепер вердикт друкується машинно-читно без вироку; звірку з
        // незалежним двигуном (ІІТ) на цих самих файлах робить run_tests.ps1.
        // file екрановано jsonEscape (не лише fwd/forward-slash): рядок мусить лишатись
        // валідним JSON і на випадок лапок/керуючих символів у шляху, не тільки зворотних
        // слешів — Windows-шлях без екранування ламає будь-який строгий парсер (ConvertFrom-Json).
        printf("{\"engine\":\"uapki\",\"file\":\"%s\",\"status\":\"%s\","
               "\"statusSignature\":\"%s\",\"validDigests\":%s,\"hasSigner\":%s,\"errorCode\":%ld}\n",
               jsonEscape(w2u8(f)).c_str(), st.c_str(), ss.c_str(),
               validDig ? "true" : "false", hasSigner ? "true" : "false", ec);

        // FAIL лишається ЛИШЕ там, де зламана САМА структура підпису (немає signatureInfos,
        // statusSignature не починається з VALID, геші вмісту не збіглись або немає
        // сертифіката підписувача) — це вже не властивість вхідних даних (довіра/TSP/CRL),
        // а дефект нашого розбору.
        if (!si || ss.rfind("VALID", 0) != 0 || !validDig || !hasSigner) {
            printf("  FAIL: структурна частина невалідна\n");
            allOk = false;
        }
    }
    c.call("DEINIT", "");
    c.unload();
    return allOk;
}

// ========================================================================
// КЕЙС 6 — пароль контейнера НЕ потрапляє у файл лога
// ========================================================================
// Логи пишуться через ИспользоватьЛогирование. Перевіряємо не наявність
// маскування, а ВІДСУТНІСТЬ секрету: єдине, що справді має значення.
static bool case6_passwordNotLogged(const std::wstring& binDir, const std::wstring& dataDir) {
    printf("== Case 6: пароль не потрапляє в лог ==\n");
    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    std::wstring p12     = dataDir + L"\\test-diia.p12";
    CHECK(pathExists(p12), "test-diia.p12 присутній");

    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    // PID у імені: паралельні прогони x86/x64 інакше затирали б лог одне одному.
    std::wstring logPath = std::wstring(tmpDir) + L"sac_case6_" + std::to_wstring(GetCurrentProcessId()) + L".log";
    DeleteFileW(logPath.c_str());

    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    CHECK(c.enableLogging(L"Trace", logPath), "лог увімкнено");

    json j;
    std::string r = c.call("INIT", buildInit(true));
    CHECK(errCode(r, j) == 0, "INIT errorCode == 0");

    r = c.call("OPEN", buildOpen(p12));     // buildOpen кладе password "testpassword"
    printf("  OPEN (JSON): %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "OPEN (JSON) errorCode == 0");

    c.call("CLOSE", "");

    // Другий OPEN — плоским форматом. Окрема гілка розбору (ParseParamsString), і саме
    // вона колись клала сирий рядок з паролем у лог; без цього прогону регресія
    // повернулася б непоміченою.
    r = c.call("OPEN", buildOpenFlat(p12));
    printf("  OPEN (плоский): %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "OPEN (плоский формат) errorCode == 0");

    c.call("CLOSE", "");
    c.call("DEINIT", "");
    c.unload();                              // закрити лог перед читанням

    std::string logText;
    CHECK(readFileText(logPath, logText), "лог прочитано");
    CHECK(!logText.empty(), "лог не порожній");
    CHECK(logText.find("testpassword") == std::string::npos,
          "пароль ВІДСУТНІЙ у лозі (обидва OPEN: JSON і плоский формат)");
    CHECK(logText.find("\"password\":\"***\"") != std::string::npos ||
          logText.find("\"password\": \"***\"") != std::string::npos,
          "у лозі є замаскований password");
    DeleteFileW(logPath.c_str());
    return true;
}

// ========================================================================
// КЕЙС 7 — відкриття РЕАЛЬНИХ контейнерів КНЕДП
// ========================================================================
// Дві різні гілки детекту в cm-pkcs12: JKS (магія 0xFEEDFEED -> decodeJks ->
// jks_decrypt_key) і PKCS#12 (.ZS2 попри розширення є повноцінним PFX).
// Жодного реального контейнера від КНЕДП раніше не відкривали.
static bool case7_realContainers(const std::wstring& binDir, const std::wstring& dataDir,
                                 bool& skipped) {
    printf("== Case 7: реальні контейнери КНЕДП ==\n");
    skipped = false;

    std::vector<LocalKey> keys;
    std::string err;
    const std::wstring cfg = dataDir + L"\\local-keys.json";
    if (!LoadLocalKeys(cfg, keys, err)) {
        printf("FAIL: конфіг зіпсований: %s\n", err.c_str());
        return false;                       // конфіг є -> наміри заявлені -> FAIL
    }
    if (keys.empty()) {
        printf("SKIP (немає tests/data/local-keys.json — реальні ключі не налаштовані)\n");
        skipped = true;
        return true;
    }

    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    std::string r = c.call("INIT", buildInit(true));
    CHECK(errCode(r, j) == 0, "INIT errorCode == 0");

    bool allOk = true;
    for (const auto& k : keys) {
        printf("  -- ключ '%s'\n", k.id.c_str());

        json op;
        op["provider"] = "PKCS12";          // провайдер один: детект іде за ВМІСТОМ
        op["storage"]  = k.path;
        op["password"] = k.password;
        op["mode"]     = "RO";
        r = c.call("OPEN", op.dump());
        if (errCode(r, j) != 0) {
            printf("  FAIL: OPEN errorCode=%ld error=%s\n",
                   errCode(r, j), j.value("error", std::string()).c_str());
            allOk = false;
            continue;
        }
        printf("  OPEN ok\n");

        r = c.call("KEYS", "");
        if (errCode(r, j) != 0) { printf("  FAIL: KEYS\n"); allOk = false; c.call("CLOSE", ""); continue; }

        bool algoSeen = k.expectSignAlgo.empty();
        std::string firstId;
        if (j["result"].contains("keys") && j["result"]["keys"].is_array()) {
            for (const auto& key : j["result"]["keys"]) {
                if (firstId.empty()) firstId = key.value("id", std::string());
                if (key.contains("signAlgo") && key["signAlgo"].is_array()) {
                    for (const auto& a : key["signAlgo"])
                        if (a.get<std::string>() == k.expectSignAlgo) algoSeen = true;
                }
            }
        }
        if (!algoSeen) {
            printf("  FAIL: очікуваний signAlgo %s не знайдено серед можливостей ключа\n",
                   k.expectSignAlgo.c_str());
            allOk = false;
        } else {
            printf("  signAlgo %s підтверджено\n", k.expectSignAlgo.c_str());
        }

        if (!firstId.empty()) {
            json sp; sp["id"] = firstId;
            r = c.call("SELECT_KEY", sp.dump());
            if (errCode(r, j) != 0) { printf("  FAIL: SELECT_KEY\n"); allOk = false; }
        }
        c.call("CLOSE", "");
    }

    c.call("DEINIT", "");
    c.unload();
    CHECK(allOk, "усі реальні контейнери відкрито й алгоритми збіглися");
    return true;
}

// ========================================================================
// КЕЙС 8 — купинний підпис + структурний контракт ПРРО
// ========================================================================
// ДПС забороняє: content-time-stamp, CRL/OCSP, сертифікати видавця.
// ДПС вимагає: вкладений сертифікат підписувача, дані всередині (enveloping).
// OID дивимось у SignerInfo, а НЕ в сертифікаті: сертифікат старого зразка
// цілком може підписувати Купиною (живий ticket.p7s ДПС саме такий).
static bool case8_kupynaSign(const std::wstring& binDir, const std::wstring& dataDir,
                             const std::wstring& outSig, bool& skipped) {
    printf("== Case 8: купинний підпис + контракт ПРРО ==\n");
    skipped = false;

    std::vector<LocalKey> keys;
    std::string err;
    if (!LoadLocalKeys(dataDir + L"\\local-keys.json", keys, err)) {
        printf("FAIL: конфіг зіпсований: %s\n", err.c_str());
        return false;                       // конфіг є -> наміри заявлені -> FAIL
    }
    const LocalKey* k = FindLocalKey(keys, "jks-kupyna");
    if (!k) {
        printf("SKIP (немає ключа 'jks-kupyna' у local-keys.json)\n");
        skipped = true;
        return true;
    }

    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    std::string r = c.call("INIT", buildInit(true));
    CHECK(errCode(r, j) == 0, "INIT errorCode == 0");

    { json op;
      op["provider"] = "PKCS12";
      op["storage"]  = k->path;
      op["password"] = k->password;         // пароль НЕ друкуємо — це особистий КЕП
      op["mode"]     = "RO";
      r = c.call("OPEN", op.dump()); }
    CHECK(errCode(r, j) == 0, "OPEN errorCode == 0");

    // Діагностика CERT_NOT_FOUND: скільки сертифікатів контейнер віддав у cer-store.
    // Друкуємо ЛИШЕ кількості — вміст особистого КЕП у консоль не виносимо.
    { json lp; lp["storage"] = true;
      std::string rs = c.call("LIST_CERTS", lp.dump());
      json js; const long ecs = errCode(rs, js);
      size_t inStorage = (ecs == 0 && js["result"].contains("certIds")) ? js["result"]["certIds"].size() : 0;
      std::string rc = c.call("LIST_CERTS", "{}");
      json jc; const long ecc = errCode(rc, jc);
      size_t inCache = (ecc == 0 && jc["result"].contains("certIds")) ? jc["result"]["certIds"].size() : 0;
      printf("  LIST_CERTS: у контейнері=%zu (errorCode=%ld), у кеші=%zu (errorCode=%ld)\n",
             inStorage, ecs, inCache, ecc); }

    r = c.call("KEYS", "");
    CHECK(errCode(r, j) == 0, "KEYS errorCode == 0");
    CHECK(j["result"].contains("keys") && j["result"]["keys"].is_array()
          && !j["result"]["keys"].empty(), "result.keys непорожній");
    std::string keyId  = j["result"]["keys"][0].value("id", std::string());
    std::string keyId2 = j["result"]["keys"][0].value("keyId2", std::string());
    CHECK(!keyId.empty(), "id ключа отримано");
    printf("  keyId2 %s\n", keyId2.empty() ? "відсутній" : "присутній (купинний SKI)");

    // ПАСТКА КУПИНИ. У ДСТУ-ключа ДВА ідентифікатори: `id` — ГОСТ-34311-геш
    // відкритого ключа, `keyId2` — Купина-256 того самого ключа. Сертифікат
    // нового зразка несе в розширенні SubjectKeyIdentifier саме КУПИННИЙ геш,
    // а UAPKI шукає сертифікат підписувача рівно за тим id, яким обрано ключ
    // (cer-store порівнює з SKI сертифіката). Тож SELECT_KEY за звичним `id`
    // ключ обирає успішно, але сертифікат до нього НЕ знаходить — і SIGN з
    // includeCert:true падає з CERT_NOT_FOUND (4161), хоча сертифікат лежить
    // у контейнері й уже завантажений у кеш (див. LIST_CERTS вище).
    // Правильний ідентифікатор для купинного ключа — keyId2.
    auto selectKey = [&](const std::string& id) {
        json sp; sp["id"] = id;
        r = c.call("SELECT_KEY", sp.dump());
        return errCode(r, j) == 0 && j["result"].contains("certId");
    };
    bool selected = selectKey(keyId);
    if (!selected && !keyId2.empty()) {
        printf("  SELECT_KEY за `id` сертифіката не дав — пробуємо купинний keyId2\n");
        selected = selectKey(keyId2);
    }
    CHECK(errCode(r, j) == 0, "SELECT_KEY errorCode == 0");
    CHECK(selected, "SELECT_KEY повернув certId (сертифікат підписувача знайдено за SKI)");

    // Купинний підпис у профілі ПРРО (офлайн: без позначки часу).
    json sp2;
    sp2["signatureFormat"]  = "CAdES-BES";
    sp2["signAlgo"]         = "1.2.804.2.1.1.1.1.3.6.1.1";   // ДСТУ4145 + Купина-256
    sp2["detachedData"]     = false;                          // enveloping
    sp2["includeCert"]      = true;
    sp2["includeTime"]      = true;
    sp2["includeContentTS"] = false;
    json d; d["id"] = "doc-0"; d["bytes"] = DATA_TBS_B64;
    json p;
    p["signParams"] = sp2;
    p["dataTbs"]    = json::array({ d });
    p["options"]["ignoreCertStatus"] = true;

    r = c.call("SIGN", p.dump());
    printf("  SIGN: %s\n", r.substr(0, 300).c_str());
    // Перший купинний підпис у проєкті: невдача цінна не менше за вдачу, тож
    // фіксуємо відповідь ПОВНІСТЮ, а не обрізану до 300 символів.
    if (errCode(r, j) != 0) printf("  SIGN (повна відповідь): %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "SIGN errorCode == 0 (КУПИННИЙ ПІДПИС)");
    std::string sig = j["result"]["signatures"][0].value("bytes", std::string());
    CHECK(!sig.empty(), "підпис не порожній");

    // VERIFY нашим двигуном — РЕГРЕСІЙНА перевірка, не доказ коректності:
    // наш код підтверджує наш код. Доказ дає арбітр ІІТ (рівень L4-iit).
    { json vp; vp["signature"]["bytes"] = sig; r = c.call("VERIFY", vp.dump()); }
    printf("  VERIFY: %s\n", r.substr(0, 600).c_str());
    CHECK(errCode(r, j) == 0, "VERIFY errorCode == 0 (регресія)");
    auto& res = j["result"];
    CHECK(res.contains("signatureInfos") && !res["signatureInfos"].empty(), "signatureInfos є");
    auto& si = res["signatureInfos"][0];

    // Пастка ПРРО: алгоритм СЕРТИФІКАТА не визначає алгоритм ПІДПИСУ. Дивимось
    // саме в SignerInfo — UAPKI віддає його в signatureInfos[].
    // `status` — ХОЛІСТИЧНИЙ вердикт; друкуємо його для виміру, але CHECK на нього
    // НЕ ставимо: підпис створено offline з ignoreCertStatus, ланцюг не валідується,
    // тож не-TOTAL-VALID тут законний (кейс 5 на еталонах ДПС дає INDETERMINATE при
    // statusSignature=VALID). Спершу вимір — правило потім.
    printf("  SignerInfo: signAlgo=%s digestAlgo=%s statusSignature=%s statusMessageDigest=%s status=%s\n",
           si.value("signAlgo", std::string()).c_str(),
           si.value("digestAlgo", std::string()).c_str(),
           si.value("statusSignature", std::string()).c_str(),
           si.value("statusMessageDigest", std::string()).c_str(),
           si.value("status", std::string()).c_str());
    CHECK(si.value("statusSignature", std::string()).rfind("VALID", 0) == 0,
          "statusSignature починається з VALID (регресія)");
    // Самого statusSignature НЕДОСТАТНЬО: він лишається VALID навіть при пошкодженому
    // вмісті (docs/integration-1c/uapki.md:181, 386-388). Підміну вмісту ловить саме
    // statusMessageDigest.
    CHECK(si.value("statusMessageDigest", std::string()) == "VALID",
          "statusMessageDigest == VALID (саме це ловить підміну вмісту)");
    CHECK(si.value("signAlgo",   std::string()) == "1.2.804.2.1.1.1.1.3.6.1.1",
          "signAlgo у SignerInfo == ДСТУ4145 з Купиною-256");
    CHECK(si.value("digestAlgo", std::string()) == "1.2.804.2.1.1.1.1.2.2.1",
          "digestAlgo у SignerInfo == Купина-256");

    // Контракт ПРРО
    CHECK(res.contains("certIds") && !res["certIds"].empty(), "сертифікат підписувача вкладено");
    CHECK(!si.contains("contentTS"),      "content-time-stamp ВІДСУТНІЙ (ДПС забороняє)");
    CHECK(!si.contains("revocationRefs"), "revocationRefs відсутні (ДПС забороняє)");
    CHECK(!si.contains("certValues"),     "certValues відсутні (ДПС забороняє)");
    CHECK(!si.contains("certificateRefs"),"certificateRefs відсутні (ДПС забороняє)");

    // Записати підпис для арбітра
    if (!outSig.empty()) {
        std::vector<unsigned char> raw;
        CHECK(b64decode(sig, raw), "підпис декодовано з base64");
        CHECK(writeFileBytes(outSig, raw), "підпис збережено для арбітра");
        printf("  підпис записано: %s (%zu байт)\n", w2u8(outSig).c_str(), raw.size());
    }

    c.call("CLOSE", ""); c.call("DEINIT", ""); c.unload();
    return true;
}

// ========================================================================
// КЕЙС 9 — вердикт НАШОГО VERIFY по одному файлу (для матриці двох двигунів)
// ========================================================================
// Еталони ЦЗО лежать у репозиторії з 2026-09-01 і ЖОДНОГО разу не проганялися
// через нашу перевірку. Вердикт ІІТ для них відомий (code=51, "Сертифікат не
// знайдено") -> будуємо матрицю: кожен файл отримує ДВА незалежні вердикти.
// Мета — НЕ в тому, щоб обидва двигуни сказали "валідно" (еталони ЦЗО від
// тестового ЦСК, "невалідно" від обох — очікувано), а в тому, щоб вони не
// розходились несподівано.
//
// Друкує РІВНО ОДИН рядок JSON на початку рядка (без відступу): run_tests.ps1
// фільтрує вивід за регексом ^\{, щоб дістати вердикт із решти діагностики.
static bool case9_verifyOne(const std::wstring& binDir, const std::wstring& file) {
    if (file.empty()) { printf("FAIL: не задано файл (шостий аргумент, argv[6])\n"); return false; }
    std::vector<unsigned char> raw;
    if (!readFileBytes(file, raw) || raw.empty()) { printf("FAIL: файл не прочитано\n"); return false; }

    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    std::string r = c.call("INIT", buildInit(true));
    if (errCode(r, j) != 0) { printf("FAIL: INIT\n"); c.unload(); return false; }

    json p;
    p["signature"]["bytes"]        = b64encode(raw);
    p["options"]["validationType"] = "STRUCT";
    r = c.call("VERIFY", p.dump());
    const long ec = errCode(r, j);

    // result заповнюється навіть при errorCode != 0 (див. кейс 5) — саме там
    // лежить діагностика на кшталт CERT_NOT_FOUND.
    std::string status          = "NO-INFO";
    std::string statusSignature;
    bool        validDigests = false;
    if (j.contains("result") && j["result"].is_object()) {
        auto& res = j["result"];
        if (res.contains("signatureInfos") && res["signatureInfos"].is_array()
            && !res["signatureInfos"].empty()) {
            auto& si        = res["signatureInfos"][0];
            status          = si.value("status", std::string("NO-STATUS"));
            statusSignature = si.value("statusSignature", std::string());
            validDigests    = si.value("validDigests", false);
        }
    }
    printf("{\"engine\":\"uapki\",\"status\":\"%s\",\"statusSignature\":\"%s\",\"validDigests\":%s,\"errorCode\":%ld}\n",
           status.c_str(), statusSignature.c_str(), validDigests ? "true" : "false", ec);

    c.call("DEINIT", ""); c.unload();
    return true;
}

// ========================================================================
// КЕЙС 10 — вибір ключа за СЕРТИФІКАТОМ, універсальний (test-diia.p12)
// ========================================================================
// Український КЕП-контейнер завжди містить ДВІ ключові пари — підпис і шифрування.
// Кейс доводить три твердження, жодне з яких раніше не було покрите:
//   1. KEYS не дає ознаки, за якою можна обрати підписний ключ;
//   2. CERT_INFO розрізняє сертифікати за keyUsage — і розрізняє ЄДИНИЙ, бо решта
//      полів (зокрема суб'єкт) збігається;
//   3. SELECT_KEY(certId) обирає ключ САМЕ цього сертифіката й повертає certId.
// Зовнішніх залежностей немає: і контейнер, і обидва сертифікати лежать у git.
// Сертифікати подаються через ADD_CERT, а не через certCache.path — так робить 1С
// (вони приходять base64 з бази, не з каталогу), і так заразом покривається ADD_CERT,
// який досі не був покритий нічим.
static bool case10_selectByCertId(const std::wstring& binDir, const std::wstring& dataDir) {
    printf("== Case 10: вибір ключа за сертифікатом (SELECT_KEY за certId) ==\n");
    const std::wstring dllName  = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    const std::wstring p12      = dataDir + L"\\test-diia.p12";
    const std::wstring certsDir = dataDir + L"\\certs";
    CHECK(pathExists(p12), "test-diia.p12 присутній");

    std::wstring certSignPath, certEncPath;
    CHECK(findCertByPrefix(certsDir, CERT_PREFIX_SIGN, certSignPath),
          "файл ПІДПИСНОГО сертифіката знайдено");
    CHECK(findCertByPrefix(certsDir, CERT_PREFIX_ENCRYPT, certEncPath),
          "файл ШИФРУВАЛЬНОГО сертифіката знайдено");
    std::string certSignB64, certEncB64;
    CHECK(readCertB64(certSignPath, certSignB64), "підписний сертифікат прочитано в base64");
    CHECK(readCertB64(certEncPath,  certEncB64),  "шифрувальний сертифікат прочитано в base64");

    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    std::string r;

    // INIT БЕЗ certCache.path: постійного кеша немає, отже ADD_CERT кладе сертифікати
    // лише в пам'ять сесії — на диск нічого не пишеться, tests/data лишається read-only.
    r = c.call("INIT", buildInit(true));
    printf("  INIT: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "INIT errorCode == 0");

    // ADD_CERT: permanent НЕ задаємо -> default false (тимчасово).
    { json p; p["certificates"] = json::array({ certSignB64, certEncB64 });
      r = c.call("ADD_CERT", p.dump()); }
    printf("  ADD_CERT: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "ADD_CERT errorCode == 0");
    CHECK(j["result"].contains("added") && j["result"]["added"].is_array()
          && j["result"]["added"].size() == 2, "result.added містить рівно 2 записи");

    std::vector<std::string> certIds;
    for (const auto& a : j["result"]["added"]) {
        CHECK(a.value("errorCode", -1) == 0, "сертифікат додано (added[].errorCode == 0)");
        const std::string id = a.value("certId", std::string());
        CHECK(!id.empty(), "added[].certId не порожній");
        certIds.push_back(id);
    }

    // ТВЕРДЖЕННЯ 2. Класифікуємо за keyUsage, а НЕ за порядком у added[]: порядок —
    // деталь реалізації, а ознака призначення — контракт. keyUsage лежить у розширенні
    // 2.5.29.15; UAPKI кладе в decoded.value ЛИШЕ виставлені біти
    // (extension-helper-json.cpp:410-418), тож відсутність digitalSignature == false.
    auto certUsage = [&](const std::string& certId, bool& digitalSignature, json& subject) -> bool {
        json p; p["certId"] = certId;
        const std::string resp = c.call("CERT_INFO", p.dump());
        json ji;
        if (errCode(resp, ji) != 0) { printf("  CERT_INFO: %s\n", resp.c_str()); return false; }
        digitalSignature = false;
        subject = ji["result"].value("subject", json::object());
        if (ji["result"].contains("extensions") && ji["result"]["extensions"].is_array()) {
            for (const auto& e : ji["result"]["extensions"]) {
                if (e.value("extnId", std::string()) != "2.5.29.15") continue;
                if (!e.contains("decoded") || !e["decoded"].contains("value")) continue;
                digitalSignature = e["decoded"]["value"].value("digitalSignature", false);
            }
        }
        return true;
    };

    std::string certIdSign, certIdEnc;
    json subjSign, subjEnc;
    for (const auto& id : certIds) {
        bool ds = false; json subj;
        CHECK(certUsage(id, ds, subj), "CERT_INFO по certId відпрацював");
        printf("  CERT_INFO %s... digitalSignature=%s subject=%s\n",
               id.substr(0, 16).c_str(), ds ? "true" : "false", subj.dump().c_str());
        if (ds) { certIdSign = id; subjSign = subj; }
        else    { certIdEnc  = id; subjEnc  = subj; }
    }
    // Два РІЗНІ сертифікати + бінарна класифікація: «є підписний» і «є непідписний»
    // разом означають «рівно один кожного роду».
    CHECK(certIdSign != certIdEnc, "certId сертифікатів різні");
    CHECK(!certIdSign.empty(), "рівно один сертифікат має keyUsage.digitalSignature");
    CHECK(!certIdEnc.empty(),  "рівно один сертифікат НЕ має keyUsage.digitalSignature");
    // ...і keyUsage — ЄДИНА ознака: решта полів збігається. Якби механізм обирав за
    // іменем власника, обирати було б нема за чим — саме це тут і зафіксовано.
    CHECK(!subjSign.empty() && subjSign == subjEnc,
          "суб'єкт обох сертифікатів ОДНАКОВИЙ (розрізнення можливе лише за keyUsage)");

    r = c.call("OPEN", buildOpen(p12));
    printf("  OPEN: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "OPEN errorCode == 0");

    r = c.call("KEYS", "");
    printf("  KEYS: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "KEYS errorCode == 0");
    CHECK(j["result"].contains("keys") && j["result"]["keys"].is_array()
          && j["result"]["keys"].size() == 2, "контейнер двоключовий (keys.size() == 2)");
    // ТВЕРДЖЕННЯ 1. Це не діагностика, а CHECK: якщо в KEYS колись з'явиться ознака
    // розрізнення (keyUsage у cm-pkcs12, різні signAlgo), кейс почервоніє — і ми
    // дізнаємось, що міркування «сертифікат — єдине джерело правди» застаріло,
    // а не продовжимо спиратися на нього мовчки.
    const auto& k0 = j["result"]["keys"][0];
    const auto& k1 = j["result"]["keys"][1];
    // Сентинели value() тут не годяться: РІЗНІ дефолти ("a" vs "b") давали б зелене
    // навіть тоді, коли id немає в ЖОДНОГО ключа. Наявність поля перевіряємо явно,
    // і лише потім порівнюємо — const operator[] на відсутньому ключі дає UB
    // (json.hpp:22182-22193, JSON_ASSERT зникає під NDEBUG).
    CHECK(k0.contains("id") && k1.contains("id") && k0["id"] != k1["id"],
          "id ключів різні (є з чого обирати)");
    // contains() ОБОВ'ЯЗКОВО з ОБОХ боків: k0/k1 — це `const json&`, а константний
    // operator[] на відсутньому ключі — UB, не виняток (json.hpp:22182-22190; під
    // NDEBUG його JSON_ASSERT зникає). Перевірка лише по k0 лишала б k1["…"] голим.
    CHECK(k0.contains("mechanismId") && k1.contains("mechanismId")
          && k0["mechanismId"] == k1["mechanismId"],
          "mechanismId обох ключів ОДНАКОВИЙ");
    CHECK(k0.contains("signAlgo") && k1.contains("signAlgo")
          && k0["signAlgo"] == k1["signAlgo"],
          "signAlgo[] обох ключів ОДНАКОВИЙ");

    // ТВЕРДЖЕННЯ 3. CHECK не на саму НАЯВНІСТЬ certId, а на РІВНІСТЬ запитаному:
    // у test-diia keys[0] — підписний, тож кейс лишився б зеленим і тоді, коли
    // механізм ігнорує сертифікат і бере перший-ліпший ключ.
    { json p; p["certId"] = certIdSign;
      r = c.call("SELECT_KEY", p.dump()); }
    printf("  SELECT_KEY(certId підписного): %s\n", elideCert(r).c_str());
    CHECK(errCode(r, j) == 0, "SELECT_KEY(certId) errorCode == 0");
    CHECK(j["result"].contains("certId"), "SELECT_KEY(certId) повернув certId");
    CHECK(j["result"].value("certId", std::string()) == certIdSign,
          "повернутий certId == запитаному (зв'язка ключ<->сертифікат саме та)");
    CHECK(upperAscii(j["result"].value("id", std::string())) == upperAscii(KEY_ID),
          "обрано ПІДПИСНИЙ ключ (id == SKI підписного сертифіката)");

    // Наскрізна зв'язка ADD_CERT(permanent=false) -> SELECT_KEY(certId) -> SIGN(includeCert)
    // до цього прогону не була зміряна ніде.
    r = c.call("SIGN", buildSign());
    printf("  SIGN: %s\n", r.substr(0, 300).c_str());
    if (errCode(r, j) != 0) printf("  SIGN (повна відповідь): %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "SIGN errorCode == 0 (наскрізна зв'язка працює)");
    CHECK(j["result"].contains("signatures") && j["result"]["signatures"].is_array()
          && !j["result"]["signatures"].empty()
          && !j["result"]["signatures"][0].value("bytes", std::string()).empty(),
          "підпис не порожній");

    // --- НЕГАТИВНА ЧАСТИНА — обов'язкова ---
    // testing-rules.md, правило 1: без неї кейс лишався б зеленим навіть тоді, коли
    // механізм ігнорує сертифікат. Тут доводиться протилежне: обраний ключ справді
    // визначає, чим підписують, і підпис ключем ШИФРУВАННЯ не проходить.
    { json p; p["certId"] = certIdEnc;
      r = c.call("SELECT_KEY", p.dump()); }
    printf("  SELECT_KEY(certId ШИФРУВАЛЬНОГО): %s\n", elideCert(r).c_str());
    CHECK(errCode(r, j) == 0, "SELECT_KEY(certId шифрувального) errorCode == 0 (ключ існує)");
    // Доказ стану ПЕРЕД перевіркою (правило 2): без нього падіння SIGN нижче могло б
    // означати що завгодно, у т.ч. «ключ не вибрався взагалі».
    CHECK(upperAscii(j["result"].value("id", std::string())) == upperAscii(KEY_ID_ENCRYPT),
          "обрано саме ШИФРУВАЛЬНИЙ ключ");
    CHECK(j["result"].value("certId", std::string()) == certIdEnc,
          "до нього прив'язано ШИФРУВАЛЬНИЙ сертифікат");

    // УМОВА ВХОДУ в перевірку keyUsage НЕ безумовна: (формат != RAW) && (!sidUseKeyId
    // || includeCert) — sign.cpp:304-314. buildSign() дає CAdES-BES + includeCert:true,
    // тобто саме ту гілку, де страховка працює; для CMS з ідентифікацією за keyId і без
    // вкладеного сертифіката перевірки не буде взагалі, і підпис пройшов би тихо.
    // Тест фіксує ГІЛКУ, а не «властивість SIGN».
    // Компонента законно зареєструє помилку для 1С через REPORT_ERROR, тож у вивід
    // піде рядок [AddError] — ЦЕ ОЧІКУВАНО. Попереджаємо В САМОМУ ЛОЗІ, а не лише
    // коментарем: хибно прочитає це той, хто дивиться ВИВІД ГЕЙТА, а не вихідний код.
    printf("  ОЧІКУВАНО ДАЛІ: [AddError] і errorCode 4109 — це НЕГАТИВНА частина кейса\n");
    r = c.call("SIGN", buildSign());
    printf("  SIGN шифрувальним ключем: %s\n", r.c_str());
    const long ecBadUsage = errCode(r, j);
    // 4109 == 0x100D == RET_UAPKI_INVALID_KEY_USAGE (uapki-errors.h:58; Додаток А
    // extern/uapki/doc/UAPKI-PM-2.0.16.md). Рядок error — uapki-errors.c:170.
    CHECK(ecBadUsage == 4109,
          "SIGN шифрувальним ключем ВПАВ з 4109 (RET_UAPKI_INVALID_KEY_USAGE)");
    CHECK(j.value("error", std::string()) == "INVALID_KEY_USAGE",
          "error == INVALID_KEY_USAGE");

    c.call("CLOSE", "");
    c.call("DEINIT", "");
    c.unload();
    return true;
}

// ========================================================================
// КЕЙС 11 — jks-kupyna: SELECT_KEY(certId) знімає пастку 4161 за побудовою
// ========================================================================
// Кейс 10 твердження 4 довести не може: test-diia старої схеми, SKI там рахований
// ГОСТом і збігається з key.id — пастці немає де спрацювати. Тут контейнер нового
// зразка з КУПИННИМ SKI, тобто пастка жива (саме її кейс 8 обходить через keyId2).
// Доводиться, що шлях через certId робить обхід НЕПОТРІБНИМ.
//
// Сертифікати лежать УСЕРЕДИНІ контейнера, тому шлях двофазний: перший SELECT_KEY
// за id наповнює кеш сертифікатами контейнера НАВІТЬ тоді, коли сам повертає
// errorCode 0 без certId — addCerts іде на session-select-key.cpp:128-137, ДО пошуку
// сертифіката на :139-144, а ковтання CERT_NOT_FOUND — аж на :152-154.
//
// SKIP (exit 3) без tests/data/local-keys.json або без ключа 'jks-kupyna': купинного
// ключа в репозиторії немає й бути не може, тож поза цією машиною доказ не відтворюється.
// Це названо, а не замовчано.
static bool case11_jksSelectByCertId(const std::wstring& binDir, const std::wstring& dataDir,
                                     bool& skipped) {
    printf("== Case 11: jks-kupyna — двофазний шлях до SELECT_KEY(certId) ==\n");
    skipped = false;

    std::vector<LocalKey> keys;
    std::string err;
    if (!LoadLocalKeys(dataDir + L"\\local-keys.json", keys, err)) {
        printf("FAIL: конфіг зіпсований: %s\n", err.c_str());
        return false;                       // конфіг є -> наміри заявлені -> FAIL
    }
    const LocalKey* k = FindLocalKey(keys, "jks-kupyna");
    if (!k) {
        printf("SKIP (немає ключа 'jks-kupyna' у local-keys.json)\n");
        skipped = true;
        return true;
    }

    const std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    std::string r = c.call("INIT", buildInit(true));
    CHECK(errCode(r, j) == 0, "INIT errorCode == 0");

    { json op;
      op["provider"] = "PKCS12";            // провайдер один: детект іде за ВМІСТОМ
      op["storage"]  = k->path;
      op["password"] = k->password;         // пароль НЕ друкуємо — це особистий КЕП
      op["mode"]     = "RO";
      r = c.call("OPEN", op.dump()); }
    CHECK(errCode(r, j) == 0, "OPEN errorCode == 0");

    // Сирий KEYS друкуємо ПОВНІСТЮ: скільки ключів UAPKI бачить у цьому JKS — не міряно
    // ніде, а відповідь потрібна сусідній сесії (спека §7 крок 2, §9 критерій 4).
    r = c.call("KEYS", "");
    printf("  KEYS (сирий result): %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "KEYS errorCode == 0");
    // Число з ПРОГОНУ 2026-09-21 на pb_3168306135.jks, не з розрахунку: структурний
    // розбір контейнера показував один запис приватного ключа з ланцюгом із 4
    // сертифікатів, а скільки з цього стане елементами keys[] — питання до UAPKI,
    // не до ASN.1.
    // ПІДЛОГА, а не точне число — і це НЕ недбалість. НЕ ПІДТЯГУВАТИ до == 2.
    // Сама бібліотека трактує другий ключ як НЕОБОВ'ЯЗКОВИЙ саме на шляху JKS:
    // file-storage.cpp:346 (decodeJks) додає його ЛИШЕ при
    // `if (pkcs12_iit_read_kep_key(...) == RET_OK)`, тоді як на шляху .ZS2/PFX-ІІТ
    // (file-storage.cpp:285, decodeIit) той самий виклик стоїть під DO(...) і його
    // невдача валить розбір. Отже JKS з ОДНИМ ключем — передбачена гілка коду, а не
    // аномалія, і асерт на == 2 суперечив би бібліотеці. Фактичне число — у ВИМІРІ нижче.
    CHECK(j["result"].contains("keys") && j["result"]["keys"].is_array()
          && !j["result"]["keys"].empty(), "keys[] непорожній (фактичне число — у ВИМІРІ)");
    printf("  ВИМІР: keys.size() == %zu\n", j["result"]["keys"].size());
    const std::string keyId = j["result"]["keys"][0].value("id", std::string());
    CHECK(!keyId.empty(), "id першого ключа отримано");

    // --- ФАЗА 1: наповнити кеш ---
    // certId тут може НЕ прийти — це і є пастка. Вердикт ФІКСУЄМО ВИМІРОМ, а не CHECK:
    // обидва результати законні, і саме результат тут цікавий.
    { json p; p["id"] = keyId; r = c.call("SELECT_KEY", p.dump()); }
    printf("  SELECT_KEY фаза 1 (за id): %s\n", elideCert(r).c_str());
    CHECK(errCode(r, j) == 0, "SELECT_KEY фаза 1 errorCode == 0");
    const bool certIdInPhase1 = j["result"].contains("certId");
    printf("  ВИМІР: certId після фази 1 %s\n",
           certIdInPhase1 ? "ПРИЙШОВ — пастка 4161 на цьому ключі НЕ жива, зафіксувати й повідомити"
                          : "НЕ прийшов — пастка 4161 жива (очікувано)");

    // --- Доказ, що фаза 1 наповнила кеш попри відсутність certId ---
    { json lp; lp["showCertInfos"] = true; r = c.call("LIST_CERTS", lp.dump()); }
    CHECK(errCode(r, j) == 0, "LIST_CERTS errorCode == 0");
    CHECK(j["result"].contains("certInfos") && j["result"]["certInfos"].is_array()
          && !j["result"]["certInfos"].empty(),
          "кеш НЕ порожній після фази 1 (сертифікати контейнера вже там)");
    printf("  ВИМІР: certInfos.size() == %zu\n", j["result"]["certInfos"].size());

    // Підписний сертифікат = keyUsage.digitalSignature І НЕ сертифікат ЦСК.
    // ПІДСТАВА ФІЛЬТРА isCa — стандарт, а НЕ спостереження: RFC 5280 дозволяє
    // CA-сертифікату нести digitalSignature, але ЖОДЕН CA у цьому репозиторії його
    // не несе (зміряно openssl: обидва CA:TRUE у tests/data/certs/ мають рівно
    // "Certificate Sign, CRL Sign"). Тобто на наявних ланцюгах фільтр відсіює ті самі
    // сертифікати, що й сама умова digitalSignature, і розрізнювальним НЕ стає.
    // Лишаємо на випередження — на чужому ланцюгу він розрізнятиме; але не вдаємо,
    // що його перевірено. keyUsage іде ОКРЕМИМ полем certInfos[]
    // (list-certs.cpp:135-136), тож CERT_INFO по кожному certId не потрібен.
    // Суб'єкт у консоль НЕ виносимо — це особистий КЕП; для рішення досить keyUsage/isCa.
    std::vector<std::string> candidates;
    std::string skiCandidate;
    size_t caSeen = 0;   // скільки CA-сертифікатів фільтр реально відсіяв
    for (const auto& ci : j["result"]["certInfos"]) {
        const bool isCa = ci.value("isCa", false);
        if (isCa) ++caSeen;
        const json ku   = ci.value("keyUsage", json::object());
        const bool ds   = ku.value("digitalSignature", false);
        printf("  cert %s... isCa=%s keyAlgo=%s keyUsage=%s\n",
               ci.value("certId", std::string()).substr(0, 16).c_str(),
               isCa ? "true" : "false",
               ci.value("keyAlgo", std::string()).c_str(), ku.dump().c_str());
        if (ds && !isCa) {
            candidates.push_back(ci.value("certId", std::string()));
            skiCandidate = ci.value("subjectKeyIdentifier", std::string());
        }
    }
    printf("  ВИМІР: кандидатів на підпис (digitalSignature && !isCa) == %zu\n", candidates.size());
    printf("  ВИМІР: CA-сертифікатів у кеші == %zu\n", caSeen);
    // Число CA — ВИМІР, а не асерт, і це свідомо. Воно відрізняє «фільтр відсіяв CA»
    // від «фільтрувати не було чого», але передумову кейса НЕ стверджує: зникнуть CA з
    // ланцюга — кандидат лишиться той самий, і CHECK нижче встоїть. Отже червоне тут
    // означало б «у цьому JKS інший склад ланцюга», тобто ІНШИЙ КОНТЕЙНЕР, а не дефект
    // коду. Справжній вартовий наповнення кеша — CHECK(!certInfos.empty()) після фази 1.
    // Так само за фактом прогону 2026-09-21: у ланцюгу JKS лежать корінь, КНЕДП,
    // підписний і шифрувальний сертифікати, і рівно один із них НЕ-CA з
    // digitalSignature.
    // ТЕРМІН ПРИДАТНОСТІ: після перевипуску ключа в JKS може з'явитися ДРУГИЙ підписний
    // сертифікат (старий + новий), і цей CHECK почервоніє НЕ через регресію. Червоне тут
    // інформативне — передумова однозначного вибору справді відпаде, — але перш ніж
    // шукати дефект, перевір склад контейнера.
    CHECK(candidates.size() == 1, "рівно 1 кандидат(и) на підпис (зміряно)");

    // --- ФАЗА 2: правильна зв'язка ---
    // У гілці certId certId присутній ЗАВЖДИ: getCertByCertId уже успішно відпрацював на
    // session-select-key.cpp:81, а між ним і повторним пошуком на :141 лише addCerts
    // (:131) — додає, не видаляє. Отже CERT_NOT_FOUND на :152 тут недосяжний.
    { json p; p["certId"] = candidates[0]; r = c.call("SELECT_KEY", p.dump()); }
    printf("  SELECT_KEY фаза 2 (за certId): %s\n", elideCert(r).c_str());
    CHECK(errCode(r, j) == 0, "SELECT_KEY фаза 2 errorCode == 0");
    CHECK(j["result"].contains("certId"), "фаза 2 повернула certId (ТВЕРДЖЕННЯ 4)");
    CHECK(j["result"].value("certId", std::string()) == candidates[0],
          "повернутий certId == запитаному");

    // ТВЕРДЖЕННЯ 4 — ПРЯМИЙ вимір, а не висновок із того, що фаза 1 не дала certId.
    // Пастка за визначенням: SKI сертифіката дорівнює КУПИННОМУ ідентифікатору ключа
    // (keyId2), а не ГОСТ-івському (id) — тому пошук за `id` його й не знаходить.
    // На test-diia (кейс 10) те саме порівняння дало б протилежне: SKI == id.
    const std::string selId     = upperAscii(j["result"].value("id", std::string()));
    const std::string selKeyId2 = upperAscii(j["result"].value("keyId2", std::string()));
    const std::string ski       = upperAscii(skiCandidate);
    printf("  ВИМІР: SKI сертифіката = %s\n         id ключа       = %s\n         keyId2 ключа   = %s\n",
           ski.c_str(), selId.c_str(), selKeyId2.c_str());
    // Охорона ПЕРЕД порівнянням на рівність: два порожні рядки рівні між собою, і без
    // неї CHECK нижче зеленів би тавтологічно (Global Constraints, підправило 3).
    CHECK(!ski.empty() && !selKeyId2.empty() && !selId.empty(),
          "SKI сертифіката, id і keyId2 ключа отримано (усі три непорожні)");
    CHECK(ski == selKeyId2,
          "SKI сертифіката == keyId2 (купинний) — ось чому пошук за id не знаходить");
    CHECK(ski != selId,
          "SKI сертифіката НЕ дорівнює id (ГОСТ) — пастка за визначенням жива");

    // --- Купинний підпис БЕЗ обхідного keyId2 і БЕЗ 4161 ---
    json sp;
    sp["signatureFormat"]  = "CAdES-BES";
    sp["signAlgo"]         = "1.2.804.2.1.1.1.1.3.6.1.1";   // ДСТУ4145 + Купина-256
    sp["detachedData"]     = false;                          // enveloping
    sp["includeCert"]      = true;
    sp["includeTime"]      = true;
    sp["includeContentTS"] = false;
    json d; d["id"] = "doc-0"; d["bytes"] = DATA_TBS_B64;
    json p;
    p["signParams"] = sp;
    p["dataTbs"]    = json::array({ d });
    p["options"]["ignoreCertStatus"] = true;
    r = c.call("SIGN", p.dump());
    if (errCode(r, j) != 0) printf("  SIGN (повна відповідь): %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "SIGN errorCode == 0 — БЕЗ 4161 і БЕЗ обхідного keyId2");
    // Структуру перевіряємо ДО витягання: .value() на null кидає type_error, і кейс
    // упав би FATAL-винятком замість зрозумілого FAIL. Охоронний порядок той самий,
    // що в кейсі 10.
    CHECK(j["result"].contains("signatures") && j["result"]["signatures"].is_array()
          && !j["result"]["signatures"].empty(), "result.signatures непорожній");
    const std::string sig = j["result"]["signatures"][0].value("bytes", std::string());
    CHECK(!sig.empty(), "підпис не порожній");

    // VERIFY тут — РЕГРЕСІЙНА перевірка (наш код підтверджує наш код); доказ коректності
    // купинного підпису дає арбітр ІІТ на рівні L4-iit через кейс 8.
    // Пастка ПРРО: алгоритм СЕРТИФІКАТА не визначає алгоритм ПІДПИСУ — дивимось саме
    // в SignerInfo, який UAPKI віддає в signatureInfos[].
    { json vp; vp["signature"]["bytes"] = sig; r = c.call("VERIFY", vp.dump()); }
    CHECK(errCode(r, j) == 0, "VERIFY errorCode == 0 (регресія)");
    CHECK(j["result"].contains("signatureInfos") && j["result"]["signatureInfos"].is_array()
          && !j["result"]["signatureInfos"].empty(), "signatureInfos є");
    // УВАГА: si — ПОСИЛАННЯ в j. Будь-який наступний errCode(r, j) зробить його
    // висячим: повне присвоєння json знищує вузли разом зі сховищем. Нижче j більше
    // не перезаписується — не переставляй блоки; треба безпечніше, копіюй значення.
    const auto& si = j["result"]["signatureInfos"][0];
    printf("  SignerInfo: signAlgo=%s digestAlgo=%s statusSignature=%s statusMessageDigest=%s\n",
           si.value("signAlgo", std::string()).c_str(),
           si.value("digestAlgo", std::string()).c_str(),
           si.value("statusSignature", std::string()).c_str(),
           si.value("statusMessageDigest", std::string()).c_str());
    CHECK(si.value("signAlgo", std::string()) == "1.2.804.2.1.1.1.1.3.6.1.1",
          "signAlgo у SignerInfo == ДСТУ4145 з Купиною-256");
    CHECK(si.value("digestAlgo", std::string()) == "1.2.804.2.1.1.1.1.2.2.1",
          "digestAlgo у SignerInfo == Купина-256");
    // Самого statusSignature недостатньо: він лишається VALID навіть при пошкодженому
    // вмісті. Підміну вмісту ловить саме statusMessageDigest.
    CHECK(si.value("statusMessageDigest", std::string()) == "VALID",
          "statusMessageDigest == VALID");

    c.call("CLOSE", "");
    c.call("DEINIT", "");
    c.unload();
    return true;
}

// Повні шляхи всіх завантажених у процес модулів із заданим БАЗОВИМ іменем
// (без урахування регістру). Потрібен, щоб ДОВЕСТИ стан перед перевіркою:
// скільки саме модулів головної DLL і провайдера живе в процесі й звідки.
static std::vector<std::wstring> loadedModulePaths(const std::wstring& baseName) {
    std::vector<std::wstring> out;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return out;
    const DWORD n = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < n && i < 1024; ++i) {
        wchar_t path[MAX_PATH * 2];
        const DWORD len = GetModuleFileNameW(mods[i], path, (DWORD)(sizeof(path) / sizeof(path[0])));
        if (len == 0) continue;
        const std::wstring full(path, len);
        const size_t slash = full.find_last_of(L"\\/");
        const std::wstring base = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
        if (_wcsicmp(base.c_str(), baseName.c_str()) == 0) out.push_back(full);
    }
    return out;
}

// ========================================================================
// КЕЙС 12 — ДВА екземпляри головної DLL, ОДИН модуль провайдера.
// Модель сценарію 1С: компонента в процесі двічі (ExtCompT + тимчасова копія
// v8_*_c.), а провайдера обидва беруть з ОДНОГО каталогу розгортання
// %LOCALAPPDATA%\SimplyAddinConnect\providers\<VERSION_FULL>\ — бо версія та
// сама. Два РІЗНІ шляхи до головної DLL -> два модулі зі своїми статиками UAPKI;
// ОДИН шлях до провайдера -> один модуль, один глобал cm_pkcs12.
//
// Пастка, яку кейс мусить виключити: провайдер із ДВОХ різних шляхів дав би
// ДВА модулі провайдера з двома глобалами — дефект не відтворився б, і кейс
// зеленів би, нічого не довівши. Тому провайдер — з явного СПІЛЬНОГО
// cmProviders.dir, а перепис модулів доводить передумову ДО перевірки.
//
// До C1: другий INIT дає countCmProviders == 0.
// ========================================================================
static bool case12_twoInstances(const std::wstring& mainDllSrc, const std::wstring& binDir) {
    printf("== Case 12: два екземпляри головної DLL, один модуль провайдера ==\n");

    const std::wstring dllName  = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    const std::wstring provName = std::wstring(L"cm-pkcs12") + ARCH_W + L".dll";

    std::wstring dirA = makeTempDir(L"case12a");
    std::wstring dirB = makeTempDir(L"case12b");
    CHECK(!dirA.empty() && !dirB.empty() && dirA != dirB, "створено два різні тимчасові каталоги");

    const std::wstring dllA = dirA + L"\\" + dllName;
    const std::wstring dllB = dirB + L"\\" + dllName;
    CHECK(CopyFileW(mainDllSrc.c_str(), dllA.c_str(), FALSE) != 0, "скопійовано головну DLL -> A");
    CHECK(CopyFileW(mainDllSrc.c_str(), dllB.c_str(), FALSE) != 0, "скопійовано головну DLL -> B");
    // Провайдера поруч із копіями НЕ кладемо: ResolveProviderDir узяв би каталог
    // кожної копії, і модулів провайдера стало б два.

    // Обидва екземпляри — з ОДНОГО каталогу провайдера (формат як у кейсі 3:
    // завершальний роздільник обов'язковий, арх-суфікс дописує хелпер).
    json p;
    p["offline"] = true;
    p["cmProviders"]["dir"] = fwd(binDir + L"\\");
    p["cmProviders"]["allowedProviders"] = json::array({ json{{"lib", "cm-pkcs12"}} });
    const std::string initParams = p.dump();

    // --- Екземпляр A -----------------------------------------------------
    Component a;
    if (!a.load(dllA)) return false;
    std::string respA = a.call("INIT", initParams);
    printf("  A INIT: %s\n", respA.c_str());
    json jA;
    CHECK(errCode(respA, jA) == 0, "A: INIT errorCode == 0");
    CHECK(jA["result"]["countCmProviders"].get<long>() == 1, "A: countCmProviders == 1");

    // --- Екземпляр B — ОКРЕМИЙ модуль, свіжі статики UAPKI ---------------
    Component b;
    if (!b.load(dllB)) return false;
    std::string respB = b.call("INIT", initParams);
    printf("  B INIT: %s\n", respB.c_str());
    json jB;
    CHECK(errCode(respB, jB) == 0, "B: INIT errorCode == 0 (свіжі статики UAPKI)");

    // --- Доказ передумови: процес саме в тому стані, який кейс моделює ---
    const std::vector<std::wstring> mains = loadedModulePaths(dllName);
    const std::vector<std::wstring> provs = loadedModulePaths(provName);
    for (const auto& m : mains) printf("  [module] %s\n", w2u8(m).c_str());
    for (const auto& m : provs) printf("  [module] %s\n", w2u8(m).c_str());
    CHECK(mains.size() == 2, "у процесі ДВА модулі головної DLL");
    CHECK(mains.size() == 2 && _wcsicmp(mains[0].c_str(), mains[1].c_str()) != 0,
          "модулі головної DLL — з РІЗНИХ шляхів");
    CHECK(provs.size() == 1, "у процесі ОДИН модуль провайдера (інакше кейс нічого не доводить)");

    // --- Власне перевірка -------------------------------------------------
    CHECK(jB["result"]["countCmProviders"].get<long>() == 1,
          "B: countCmProviders == 1 <- ЦЕ Й Є ДЕФЕКТ до C1");

    // Лічильник, що піднявся, ще не означає робочого провайдера. Друга ознака:
    // OPEN у ДРУГОМУ екземплярі не впирається в 4102 UNKNOWN_PROVIDER.
    // Контейнера навмисно не відкриваємо — досить, щоб помилка була ІНША.
    json op;
    op["provider"] = "PKCS12";
    op["storage"]  = "Z:\\nonexistent-by-design.p12";
    op["password"] = "x";
    op["mode"]     = "RO";
    std::string respOpen = b.call("OPEN", op.dump());
    printf("  B OPEN(неіснуючий): %s\n", respOpen.c_str());
    json jO;
    CHECK(errCode(respOpen, jO) != 4102, "B: OPEN не дає 4102 UNKNOWN_PROVIDER (провайдер зареєстрований)");

    a.unload();
    b.unload();
    return true;
}

// ========================================================================
// КЕЙС 13 — безпека вивантаження головної DLL.
// Дві речі одним прогоном:
//   (а) чи не зависає/падає FreeLibrary головної DLL після INIT — з C2
//       деструктор статика UAPKI кличе FreeLibrary провайдера з-під
//       DLL_PROCESS_DETACH, тобто під loader lock;
//   (б) чи лишається cm-pkcs12_xNN.dll у процесі після вивантаження —
//       ДО C2 лишається (витік сирого вказівника), ПІСЛЯ C2 має зникнути.
// ========================================================================
static bool case13_unloadSafety(const std::wstring& mainDllSrc, const std::wstring& binDir) {
    printf("== Case 13: безпека вивантаження головної DLL ==\n");

    std::wstring dllName  = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    std::wstring provName = std::wstring(L"cm-pkcs12") + ARCH_W + L".dll";

    std::wstring tmp = makeTempDir(L"case13");
    CHECK(!tmp.empty(), "створено тимчасовий каталог");
    CHECK(CopyFileW(mainDllSrc.c_str(), (tmp + L"\\" + dllName).c_str(), FALSE) != 0,
          "скопійовано головну DLL");
    CHECK(CopyFileW((binDir + L"\\" + provName).c_str(), (tmp + L"\\" + provName).c_str(), FALSE) != 0,
          "скопійовано провайдера поруч");

    CHECK(GetModuleHandleW(provName.c_str()) == nullptr,
          "до старту провайдера в процесі НЕМАЄ");

    {
        Component c;
        if (!c.load(tmp + L"\\" + dllName)) return false;
        std::string resp = c.call("INIT", "");
        json j;
        CHECK(errCode(resp, j) == 0, "INIT errorCode == 0");
        CHECK(j["result"]["countCmProviders"].get<long>() == 1, "countCmProviders == 1");
        CHECK(GetModuleHandleW(provName.c_str()) != nullptr, "провайдер у процесі присутній");

        printf("  -> unload(): DestroyObject + FreeLibrary головної DLL\n");
        fflush(stdout);
        c.unload();     // якщо тут зависне — це і є відповідь виміру
        printf("  <- unload() повернувся\n");
        fflush(stdout);
    }

    CHECK(true, "FreeLibrary головної DLL повернувся без зависання й падіння");

    HMODULE stillThere = GetModuleHandleW(provName.c_str());
    printf("  після вивантаження GetModuleHandleW('%ls') = %p\n", provName.c_str(), (void*)stillThere);
    CHECK(stillThere == nullptr,
          "провайдера в процесі БІЛЬШЕ НЕМАЄ (витік закрито) <- ЦЕ ЧЕРВОНЕ до C2");
    return true;
}

// ========================================================================
// КЕЙС 14 — нуль провайдерів є ПОМИЛКОЮ для 1С (політика C4).
// INIT із єдиним свідомо неіснуючим провайдером: бібліотека віддасть
// errorCode 0 з countCmProviders 0, а обгортка мусить перетворити це на 502
// і зберегти відповідь бібліотеки цілою в uapkiResponse.
// ========================================================================
static bool case14_zeroProvidersIsError(const std::wstring& binDir) {
    printf("== Case 14: нуль провайдерів = помилка 502 ==\n");

    std::wstring dllPath = binDir + L"\\SimplyAddinConnectWin" + ARCH_W + L".dll";
    Component c;
    if (!c.load(dllPath)) return false;
    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring logPath = std::wstring(tmpDir) + L"sac_case14_" + std::to_wstring(GetCurrentProcessId()) + L".log";
    DeleteFileW(logPath.c_str());
    CHECK(c.enableLogging(L"Trace", logPath), "лог увімкнено");

    // Явний cmProviders вимикає автоінʼєкцію: компонента поважає непорожній dir як є.
    json p;
    p["offline"] = true;
    p["cmProviders"]["dir"] = fwd(binDir) + "/";
    p["cmProviders"]["allowedProviders"] = json::array({ json{{"lib", "cm-nonexistent"}} });

    std::string resp = c.call("INIT", p.dump());
    printf("  INIT resp: %s\n", resp.c_str());

    json j;
    long ec = errCode(resp, j);
    CHECK(ec == 502, "errorCode == 502 (обгортка не повторює брехню бібліотеки)");
    CHECK(j.contains("error") && j["error"].is_string()
          && j["error"].get<std::string>().rfind("NO_CM_PROVIDERS_LOADED", 0) == 0,
          "error починається з NO_CM_PROVIDERS_LOADED");
    CHECK(j.contains("uapkiResponse") && j["uapkiResponse"].is_object(),
          "відповідь бібліотеки збережена в uapkiResponse");
    CHECK(j["uapkiResponse"].contains("result")
          && j["uapkiResponse"]["result"].contains("countCmProviders")
          && j["uapkiResponse"]["result"]["countCmProviders"].get<long>() == 0,
          "uapkiResponse зберігає оригінальний countCmProviders == 0");

    c.unload();                              // закрити лог перед читанням
    std::string logText;
    CHECK(readFileText(logPath, logText), "лог прочитано");
    // Позитивний контроль ПЕРЕД перевіркою відсутності: доводить, що кодування логу й
    // літерала збігаються. Без нього «рядка немає» зеленіло б і тоді, коли пошук
    // просто не вміє знайти кирилицю (testing-rules, правило 2).
    // Контроль кодування — рядок ProvidersLoadedOrFail, що пишеться НЕЗАЛЕЖНО від
    // порядку логування вердикту (і до виправлення, і після).
    CHECK(logText.find("Провайдеры НКИ не загружены") != std::string::npos,
          "у лозі є рядок ProvidersLoadedOrFail (позитивний контроль кодування)");
    CHECK(logText.find("Команда UAPKI INIT завершилась с ошибкой") != std::string::npos,
          "у лозі ЗАПИСАНО вердикт помилки INIT — 1С отримала 502");
    CHECK(logText.find("Команда UAPKI INIT выполнена успешно") == std::string::npos,
          "у лозі НЕМАЄ вердикту успіху INIT");
    DeleteFileW(logPath.c_str());
    return true;
}

// ========================================================================
// КЕЙС 15 — повторний INIT у ТОМУ САМОМУ екземплярі (політика C5).
// UAPKI віддає 4106 ALREADY_INITIALIZED із порожнім результатом; обгортка
// мусить зміряти живий стан через PROVIDERS і віддати 1С успіх із реальним
// числом провайдерів і ознакою alreadyInitialized.
// ========================================================================
static bool case15_idempotentInit(const std::wstring& binDir) {
    printf("== Case 15: повторний INIT ідемпотентний для 1С ==\n");

    std::wstring dllPath = binDir + L"\\SimplyAddinConnectWin" + ARCH_W + L".dll";
    Component c;
    if (!c.load(dllPath)) return false;
    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring logPath = std::wstring(tmpDir) + L"sac_case15_" + std::to_wstring(GetCurrentProcessId()) + L".log";
    DeleteFileW(logPath.c_str());
    CHECK(c.enableLogging(L"Trace", logPath), "лог увімкнено");

    std::string r1 = c.call("INIT", buildInit(true));
    json j1;
    CHECK(errCode(r1, j1) == 0, "INIT #1 errorCode == 0");
    CHECK(j1["result"]["countCmProviders"].get<long>() == 1, "INIT #1 countCmProviders == 1");

    std::string r2 = c.call("INIT", buildInit(true));
    printf("  INIT #2 resp: %s\n", r2.c_str());
    json j2;
    CHECK(errCode(r2, j2) == 0, "INIT #2 errorCode == 0 <- ЧЕРВОНЕ до C5 (буде 4106)");
    CHECK(j2["result"].contains("alreadyInitialized")
          && j2["result"]["alreadyInitialized"].get<bool>(),
          "INIT #2 несе alreadyInitialized == true");
    CHECK(j2["result"]["countCmProviders"].get<long>() == 1,
          "INIT #2 countCmProviders == 1 (зміряно через PROVIDERS)");

    // Друга ознака: після такого INIT крипто-стек справді придатний.
    json op;
    op["provider"] = "PKCS12";
    op["storage"]  = "Z:\\nonexistent-by-design.p12";
    op["password"] = "x";
    op["mode"]     = "RO";
    json jo;
    long ecOpen = errCode(c.call("OPEN", op.dump()), jo);
    CHECK(ecOpen != 4102, "OPEN не дає 4102 UNKNOWN_PROVIDER");

    c.unload();                              // закрити лог перед читанням
    std::string logText;
    CHECK(readFileText(logPath, logText), "лог прочитано");
    // Позитивний контроль ПЕРЕД перевіркою відсутності: доводить, що кодування логу й
    // літерала збігаються. Без нього «рядка немає» зеленіло б і тоді, коли пошук
    // просто не вміє знайти кирилицю (testing-rules, правило 2).
    CHECK(logText.find("Команда UAPKI INIT выполнена успешно") != std::string::npos,
          "у лозі є вердикт успіху INIT (позитивний контроль кодування)");
    CHECK(logText.find("Команда UAPKI INIT завершилась с ошибкой") == std::string::npos,
          "у лозі НЕМАЄ вердикту помилки INIT — лог каже те саме, що отримала 1С");
    DeleteFileW(logPath.c_str());
    return true;
}

// ========================================================================
// КЕЙС 16 — завершення процесу з ЗАВАНТАЖЕНОЮ головною DLL.
// Кейс 13 міряє явний FreeLibrary. Але 1С може завершитись, не вивантаживши
// компоненту: тоді статики UAPKI руйнуються на DLL_PROCESS_DETACH під час
// завершення процесу. Порядок detach — зворотний до завантаження: провайдер
// (завантажений пізніше) отримує DETACH РАНІШЕ за нашу DLL. З C2 деструктор
// статика кличе provider_deinit і FreeLibrary вже після DETACH провайдера.
// Без C2 цей шлях не виконувався (витік).
// Вердикт — ЛИШЕ exit-код процесу: падіння на завершенні дасть код винятку
// (напр. 0xC0000005), а зависання — таймаут, а не 0.
// ========================================================================
static bool case16_exitWithLoadedDll(const std::wstring& binDir) {
    printf("== Case 16: завершення процесу з завантаженою головною DLL ==\n");
    const std::wstring dllPath = binDir + L"\\SimplyAddinConnectWin" + ARCH_W + L".dll";

    // Навмисно НЕ через RAII: Component на купі й не звільняється, щоб DLL
    // лишилась завантаженою до завершення процесу — як у 1С, що не вивантажила
    // компоненту.
    Component* c = new Component();
    if (!c->load(dllPath)) return false;
    std::string resp = c->call("INIT", buildInit(true));
    json j;
    CHECK(errCode(resp, j) == 0, "INIT errorCode == 0");
    CHECK(j["result"]["countCmProviders"].get<long>() == 1, "countCmProviders == 1");
    printf("  процес завершується з завантаженою DLL; вердикт — exit-код (очікується 0)\n");
    fflush(stdout);
    return true;   // c свідомо не звільняється
}

// ========================================================================
// КЕЙС 17 — повторний INIT з ІНШОЮ конфігурацією (спека §8.4).
// Та сама конфігурація -> alreadyInitialized (кейс 15). Інша -> 4106 з
// переліком ключів, що різняться: тиха підміна конфігурації — брехня про
// успіх (для ПРРО: офлайн замість онлайну з TSP).
// ========================================================================
static bool case17_initConfigMismatch(const std::wstring& binDir) {
    printf("== Case 17: повторний INIT з іншою конфігурацією ==\n");
    const std::wstring dllPath = binDir + L"\\SimplyAddinConnectWin" + ARCH_W + L".dll";
    Component c;
    if (!c.load(dllPath)) return false;

    json a; a["offline"] = true;
    json j;
    CHECK(errCode(c.call("INIT", a.dump()), j) == 0, "INIT(A) errorCode == 0");
    CHECK(!j["result"].contains("alreadyInitialized"), "INIT(A) — справжня ініціалізація");

    // skipSelfTest не є конфігурацією — та сама A.
    json a2 = a; a2["skipSelfTest"] = true;
    CHECK(errCode(c.call("INIT", a2.dump()), j) == 0, "INIT(A + skipSelfTest) errorCode == 0");
    CHECK(j["result"].value("alreadyInitialized", false), "INIT(A + skipSelfTest) -> alreadyInitialized");

    // Інша конфігурація: безпечний ключ, без мережі.
    json b = a; b["validationByCrl"] = true;
    std::string rb = c.call("INIT", b.dump());
    printf("  INIT(B) resp: %s\n", rb.c_str());
    CHECK(errCode(rb, j) == 4106, "INIT(B) errorCode == 4106 <- ЧЕРВОНЕ до Task 12 (буде 0)");
    CHECK(j["result"].value("alreadyInitialized", false), "INIT(B) несе alreadyInitialized");
    bool mentions = false;
    if (j["result"].contains("configMismatch") && j["result"]["configMismatch"].is_array())
        for (const auto& k : j["result"]["configMismatch"])
            if (k.is_string() && k.get<std::string>() == "validationByCrl") mentions = true;
    CHECK(mentions, "configMismatch називає validationByCrl");

    // Змінити конфігурацію — лише через DEINIT + INIT{skipSelfTest}.
    CHECK(errCode(c.call("DEINIT", ""), j) == 0, "DEINIT errorCode == 0");
    json b2 = b; b2["skipSelfTest"] = true;
    CHECK(errCode(c.call("INIT", b2.dump()), j) == 0, "INIT(B + skipSelfTest) після DEINIT == 0");
    CHECK(!j["result"].contains("alreadyInitialized"), "INIT(B) після DEINIT — справжня ініціалізація");
    CHECK(j["result"]["countCmProviders"].get<long>() == 1, "INIT(B) після DEINIT: countCmProviders == 1");

    c.unload();
    return true;
}

// ========================================================================
// main / CLI
// ========================================================================
static void usage() {
    printf(
        "native_host <case 1..17> [mainDll] [dataDir] [binDir] [prroDir] [outSig]\n"
        "  case     : номер сценарію (окремий процес на кейс — INIT раз на процес)\n"
        "  mainDll  : шлях до головної DLL (деф.: <binDir>/SimplyAddinConnectWin"
#ifdef _WIN64
        "_x64"
#else
        "_x86"
#endif
        ".dll)\n"
        "  dataDir  : каталог тест-даних test-diia.p12/certs/crls (деф. compile-time)\n"
        "  binDir   : каталог з провайдером cm-pkcs12_*.dll (деф. compile-time)\n"
        "  prroDir  : каталог еталонів ДФС для кейса 5 (або env PRRO_DOCS_DIR)\n"
        "  outSig   : файл, куди кейс 8 запише створений підпис (вхід для арбітра ІІТ);\n"
        "             для кейса 9 — той самий argv[6], але як ВХІД: файл .p7s для VERIFY\n");
}

int main() {
    // char** argv на Windows приходить у системному ANSI-кодуванні, тож кириличні
    // шляхи в аргументах (напр., репозиторій у теці з кирилицею) спотворювались би.
    // Беремо широкі аргументи напряму через CommandLineToArgvW.
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) { printf("FAIL: CommandLineToArgvW\n"); return 2; }
    auto argAt = [&](int i) -> const wchar_t* { return (i < argc) ? wargv[i] : L""; };

    if (argc < 2) { usage(); LocalFree(wargv); return 2; }
    int kase = _wtoi(wargv[1]);
    if (kase < 1 || kase > 17) { printf("Невідомий кейс: %s\n", w2u8(wargv[1]).c_str()); usage(); LocalFree(wargv); return 2; }

    std::wstring binDir  = argAt(4)[0] ? std::wstring(argAt(4)) : u8to16(HOST_BIN_DIR);
    std::wstring dataDir = argAt(3)[0] ? std::wstring(argAt(3)) : u8to16(HOST_DATA_DIR);
    std::wstring mainDll;
    if (argAt(2)[0]) mainDll = argAt(2);
    else mainDll = binDir + L"\\SimplyAddinConnectWin" + ARCH_W + L".dll";

    // prroDir: argv[5] або env PRRO_DOCS_DIR
    std::wstring prroDir;
    if (argAt(5)[0]) prroDir = argAt(5);
    else {
        wchar_t buf[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"PRRO_DOCS_DIR", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) prroDir = buf;
    }
    // outSig: argv[6] — куди кейс 8 кладе створений підпис (необов'язковий)
    std::wstring outSig = argAt(6);
    LocalFree(wargv);

    printf("native_host: case=%d\n  mainDll=%s\n  dataDir=%s\n  binDir=%s\n",
           kase, w2u8(mainDll).c_str(), w2u8(dataDir).c_str(), w2u8(binDir).c_str());

    if (binDir.empty() && kase != 1 && kase != 2) {
        // кейси 3/4/5/6/7/8 вантажать DLL із binDir
        if (mainDll.empty()) { printf("FAIL: не задано binDir/mainDll\n"); return 2; }
    }

    bool pass = false;
    bool skipped = false;   // кейси 5, 7, 8, 11: вхідних даних немає -> це НЕ покриття (exit 3)
    try {
        switch (kase) {
            case 1: pass = case1_resourceDeploy(mainDll);          break;
            case 2: pass = case2_providerBeside(mainDll, binDir);  break;
            case 3: pass = case3_explicitDir(binDir);              break;
            case 4: pass = case4_fullChain(binDir, dataDir);       break;
            case 5: pass = case5_crossValidatePrro(binDir, prroDir, skipped); break;
            case 6: pass = case6_passwordNotLogged(binDir, dataDir);  break;
            case 7: pass = case7_realContainers(binDir, dataDir, skipped); break;
            case 8: pass = case8_kupynaSign(binDir, dataDir, outSig, skipped); break;
            case 9: pass = case9_verifyOne(binDir, outSig);                    break;
            case 10: pass = case10_selectByCertId(binDir, dataDir);            break;
            case 11: pass = case11_jksSelectByCertId(binDir, dataDir, skipped); break;
            case 12: pass = case12_twoInstances(mainDll, binDir);              break;
            case 13: pass = case13_unloadSafety(mainDll, binDir);              break;
            case 14: pass = case14_zeroProvidersIsError(binDir);               break;
            case 15: pass = case15_idempotentInit(binDir);                     break;
            case 16: pass = case16_exitWithLoadedDll(binDir);                  break;
            case 17: pass = case17_initConfigMismatch(binDir);                 break;
        }
    } catch (const std::exception& e) {
        printf("FATAL: незловлений виняток: %s\n", e.what());
        return 2;
    } catch (...) {
        printf("FATAL: незловлений невідомий виняток\n");
        return 2;
    }

    // exit 3 = SKIPPED (кейс не виконувався через відсутність вхідних даних). Окремий код
    // потрібен, щоб оркестратор не малював PASS там, де нічого не перевірялось.
    if (skipped) {
        printf("\n=== Case %d: SKIPPED (немає вхідних даних) ===\n", kase);
        return 3;
    }
    printf("\n=== Case %d: %s ===\n", kase, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

#endif // _WIN32
