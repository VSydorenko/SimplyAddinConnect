//
// @file tests/ecr_native_host.cpp
// @brief L3-харнес компоненти ECRPrivatJSON поверх ГОЛОВНОЇ DLL.
//        Емулює платформу 1С: вантажить головну DLL через LoadLibraryW,
//        отримує IComponentBase через експорт GetClassObject, надає власні
//        IAddInDefBase та IMemoryManager і викликає методи "Подключить"/"Оплата"
//        точно так, як платформа (маршалінг tVariant VTYPE_PWSTR у параметрах і
//        в результаті). Термінал емулюється in-process TerminalEmulator-ом.
//        Драйвер/крипто-ядро НЕ лінкуються — усе працює через головну DLL.
//
// Це окремий консольний exe; PCH головного проєкту НЕ підключається, дозволено printf.
//

#ifndef _WIN32

#include <cstdio>
int main() {
    printf("ecr_native_host: Windows only\n");
    return 0;
}

#else // _WIN32

#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include <nlohmann/json.hpp>

// SDK 1С (include/). types.h визначає WCHAR_T=wchar_t та ADDIN_API=__stdcall
// лише коли задано _WINDOWS — його задає CMake для цієї цілі (див. CMakeLists).
// GetClassObjectPtr/DestroyObjectPtr оголошені у ComponentBase.h — НЕ переоголошуємо.
#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"

#include "support/TerminalEmulator.h"

using nlohmann::json;

// --- Архітектурний суфікс імен DLL (як у native_host.cpp) ---------------
#ifdef _WIN64
#  define ARCH_W L"_x64"
#else
#  define ARCH_W L"_x86"
#endif

// --- Дефолтний каталог DLL, впаяний CMake (target_compile_definitions) --
#ifndef HOST_BIN_DIR
#  define HOST_BIN_DIR ""
#endif

// ========================================================================
// Конвертації рядків UTF-8 <-> UTF-16 (дослівно за native_host.cpp)
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

// ========================================================================
// Емуляція платформи 1С: менеджер пам'яті та об'єкт-з'єднання
// (дослівно за native_host.cpp — malloc/free, щоб коректно звільнити
//  ret.pwstrVal, який головна DLL виділяє нашим менеджером)
// ========================================================================
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
// Тест-інфраструктура
// ========================================================================
static int g_failed = 0;
#define CHECK(c, n) do { \
    if (c) { std::printf("[PASS] %s\n", (n)); } \
    else   { std::printf("[FAIL] %s\n", (n)); ++g_failed; } \
} while (0)

// Виклик методу-функції: усі параметри — рядки (VTYPE_PWSTR); результат
// повертається у ret (VTYPE_BOOL або VTYPE_PWSTR). retStr/retBool — вихідні.
static bool callFunc(IComponentBase* comp, long idx,
                     const std::vector<std::string>& args,
                     bool& retBool, std::string& retStr, bool& gotStr) {
    std::vector<std::wstring> wargs;
    wargs.reserve(args.size());
    for (const auto& a : args) wargs.push_back(u8to16(a));

    std::vector<tVariant> params(args.empty() ? 1 : args.size());
    for (size_t i = 0; i < wargs.size(); ++i) {
        tVarInit(&params[i]);
        params[i].vt       = VTYPE_PWSTR;
        params[i].pwstrVal = (WCHAR_T*)wargs[i].c_str();
        params[i].wstrLen  = (uint32_t)wargs[i].size();
    }

    tVariant ret;
    tVarInit(&ret);
    bool ok = comp->CallAsFunc(idx, &ret, params.data(), (long)wargs.size());

    retBool = false;
    retStr.clear();
    gotStr = false;
    if (ret.vt == VTYPE_PWSTR && ret.pwstrVal) {
        retStr = u16to8(reinterpret_cast<const wchar_t*>(ret.pwstrVal), ret.wstrLen);
        gotStr = true;
        free(ret.pwstrVal); // виділено нашим HostMemoryManager через malloc
    } else if (ret.vt == VTYPE_BOOL) {
        retBool = ret.bVal;
    }
    return ok;
}

int main() {
    // 1) Емулятор термінала в цьому процесі.
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const json&) {
        return std::string(R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})");
    });
    emu.OnRequest("ServiceMessage", [](const json& q) -> std::string {
        auto mt = q.contains("params") ? q["params"].value("msgType", std::string{}) : std::string{};
        if (mt == "identify")
            return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode")
            return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    emu.OnRequest("Purchase", [](const json&) {
        return std::string(R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"77"},"error":false})");
    });
    CHECK(emu.Start(), "L3: емулятор стартував");

    // 2) Завантажити ГОЛОВНУ DLL і створити компоненту ECRPrivatJSON.
    std::wstring dllPath = std::wstring(L"" HOST_BIN_DIR) + L"/SimplyAddinConnectWin" ARCH_W L".dll";
    std::printf("  dllPath=%s\n", w2u8(dllPath).c_str());
    HMODULE h = LoadLibraryW(dllPath.c_str());
    CHECK(h != nullptr, "L3: DLL завантажено");
    if (!h) {
        std::printf("  LoadLibraryW err=%lu\n", GetLastError());
        std::printf("\nFAILED\n");
        return 1;
    }

    auto pGetClassObject = (GetClassObjectPtr)GetProcAddress(h, "GetClassObject");
    auto pDestroyObject  = (DestroyObjectPtr)GetProcAddress(h, "DestroyObject");
    CHECK(pGetClassObject != nullptr && pDestroyObject != nullptr, "L3: експорти GetClassObject/DestroyObject знайдено");
    if (!pGetClassObject || !pDestroyObject) { std::printf("\nFAILED\n"); return 1; }

    IComponentBase* comp = nullptr; // GetClassObject вимагає *pIntf == nullptr
    pGetClassObject(L"ECRPrivatJSON", &comp);
    CHECK(comp != nullptr, "L3: компонента ECRPrivatJSON створена через DLL");
    if (!comp) { std::printf("\nFAILED\n"); return 1; }

    // 3) Ініціалізація життєвого циклу точно як платформа 1С.
    HostConnect conn;
    HostMemoryManager mem;
    bool initOk = comp->Init((void*)&conn) && comp->setMemManager((void*)&mem);
    CHECK(initOk, "L3: Init + setMemManager");

    // 4) Подключить(tcp://127.0.0.1:<emuPort>) → bool true.
    long idxConnect = comp->FindMethod(L"Connect"); // англ. і рос. аліаси обидва зареєстровані
    CHECK(idxConnect >= 0, "L3: метод Connect/Подключить знайдено");
    if (idxConnect >= 0) {
        std::string url = std::string("tcp://127.0.0.1:") + std::to_string(emu.Port());
        bool rb = false; std::string rs; bool gotStr = false;
        bool ok = callFunc(comp, idxConnect, { url }, rb, rs, gotStr);
        CHECK(ok && !gotStr && rb, "L3: Подключить → true");
    }

    // 5) Оплата("10.00") → JSON-рядок; звірити responseCode 0000 + invoiceNumber 77.
    long idxPurchase = comp->FindMethod(L"Purchase");
    CHECK(idxPurchase >= 0, "L3: метод Purchase/Оплата знайдено");
    if (idxPurchase >= 0) {
        bool rb = false; std::string rs; bool gotStr = false;
        bool ok = callFunc(comp, idxPurchase, { "10.00" }, rb, rs, gotStr);
        std::printf("  Оплата result: %s\n", rs.c_str());
        CHECK(ok && gotStr && !rs.empty(), "L3: Оплата повернула JSON-рядок");
        if (gotStr && !rs.empty()) {
            json j;
            bool parsed = false;
            try { j = json::parse(rs); parsed = true; }
            catch (...) { std::printf("  [json] не розпарсено: %s\n", rs.c_str()); }
            CHECK(parsed, "L3: результат Оплата — валідний JSON");
            if (parsed) {
                // ResultEnvelope::ToJson: code/payload на верхньому рівні.
                std::string code = j.value("code", std::string{});
                std::string invoice;
                if (j.contains("payload") && j["payload"].is_object())
                    invoice = j["payload"].value("invoiceNumber", std::string{});
                CHECK(code == "0000", "L3: Оплата responseCode == 0000");
                CHECK(invoice == "77", "L3: Оплата invoiceNumber == 77");
            }
        }
    }

    // 6) Прибирання: знищити об'єкт компоненти й вивантажити DLL.
    comp->FindMethod(L"Disconnect") >= 0
        ? (void)comp->CallAsProc(comp->FindMethod(L"Disconnect"), nullptr, 0)
        : (void)0;
    pDestroyObject(&comp);
    FreeLibrary(h);
    emu.Stop();

    std::printf(g_failed ? "\nFAILED: %d\n" : "\nOK\n", g_failed);
    return g_failed ? 1 : 0;
}

#endif // _WIN32
