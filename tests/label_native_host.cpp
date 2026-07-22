//
// @file tests/label_native_host.cpp
// @brief L-p3-харнес компоненти LabelPrinter поверх ГОЛОВНОЇ DLL.
//        Емулює платформу 1С: вантажить головну DLL через LoadLibraryW,
//        отримує IComponentBase через експорт GetClassObject, надає власні
//        IAddInDefBase та IMemoryManager і викликає БПО-методи
//        "ПодключитьОборудование"/"ПечатьЭтикеток" точно так, як платформа
//        (маршалінг tVariant VTYPE_PWSTR у параметрах; OUT DeviceID — через
//        paParams[0]). ZPL-принтер емулюється in-process LabelEmulator-ом.
//        Драйвер/фасад НЕ лінкуються — усе працює через головну DLL.
//
// Це окремий консольний exe; PCH головного проєкту НЕ підключається, дозволено printf.
//

#ifndef _WIN32

#include <cstdio>
int main() {
    printf("label_native_host: Windows only\n");
    return 0;
}

#else // _WIN32

#include "support/LabelEmulator.h"   // тягне winsock2.h ПЕРШИМ (до windows.h)
#include <windows.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <thread>
#include <chrono>

// SDK 1С (include/). types.h визначає WCHAR_T=wchar_t та ADDIN_API=__stdcall
// лише коли задано _WINDOWS — його задає CMake для цієї цілі (див. CMakeLists).
#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"

// --- Архітектурний суфікс імен DLL (як у ecr_native_host.cpp) ------------
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
// Конвертації рядків UTF-8 <-> UTF-16 (дослівно за ecr_native_host.cpp)
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
// (malloc/free, щоб коректно звільнити pwstrVal, який головна DLL виділяє
//  нашим менеджером)
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

int main() {
    // 1) In-process емулятор ZPL-принтера на ефемерному порту.
    LabelEmulator emu;
    CHECK(emu.Start(), "L-p3: емулятор стартував");

    // 2) Завантажити ГОЛОВНУ DLL і створити компоненту LabelPrinter.
    std::wstring dllPath = std::wstring(L"" HOST_BIN_DIR) + L"/SimplyAddinConnectWin" ARCH_W L".dll";
    std::printf("  dllPath=%s\n", w2u8(dllPath).c_str());
    HMODULE h = LoadLibraryW(dllPath.c_str());
    CHECK(h != nullptr, "L-p3: DLL завантажено");
    if (!h) {
        std::printf("  LoadLibraryW err=%lu\n", GetLastError());
        std::printf("\nFAILED\n");
        return 1;
    }

    auto pGetClassObject = (GetClassObjectPtr)GetProcAddress(h, "GetClassObject");
    auto pDestroyObject  = (DestroyObjectPtr)GetProcAddress(h, "DestroyObject");
    CHECK(pGetClassObject != nullptr && pDestroyObject != nullptr, "L-p3: експорти GetClassObject/DestroyObject знайдено");
    if (!pGetClassObject || !pDestroyObject) { std::printf("\nFAILED\n"); return 1; }

    IComponentBase* comp = nullptr; // GetClassObject вимагає *pIntf == nullptr
    pGetClassObject(L"LabelPrinter", &comp);
    CHECK(comp != nullptr, "L-p3: компонента LabelPrinter створена через DLL");
    if (!comp) { std::printf("\nFAILED\n"); return 1; }

    // 3) Ініціалізація життєвого циклу точно як платформа 1С.
    HostConnect conn;
    HostMemoryManager mem;
    bool initOk = comp->Init((void*)&conn) && comp->setMemManager((void*)&mem);
    CHECK(initOk, "L-p3: Init + setMemManager");

    // 4) ПодключитьОборудование: OUT DeviceID, IN EquipmentType, IN ConnectionParameters(XML) -> BOOL.
    //    DeviceID повертається через paParams[0] (VTYPE_PWSTR), НЕ через return.
    std::string deviceId;
    long idxConnect = comp->FindMethod(L"ПодключитьОборудование");
    CHECK(idxConnect >= 0, "L-p3: метод ПодключитьОборудование знайдено");
    if (idxConnect >= 0) {
        std::string connXml =
            "<?xml version=\"1.0\"?><Parameters>"
            "<Parameter Name=\"TransportKind\" Value=\"tcp\"/>"
            "<Parameter Name=\"Host\" Value=\"127.0.0.1\"/>"
            "<Parameter Name=\"Port\" Value=\"" + std::to_string(emu.Port()) + "\"/>"
            "</Parameters>";
        std::wstring wEquip = u8to16("LabelPrinter");
        std::wstring wXml   = u8to16(connXml);

        tVariant params[3];
        tVarInit(&params[0]);                       // OUT DeviceID — VTYPE_EMPTY (Undefined)
        tVarInit(&params[1]);
        params[1].vt = VTYPE_PWSTR; params[1].pwstrVal = (WCHAR_T*)wEquip.c_str(); params[1].wstrLen = (uint32_t)wEquip.size();
        tVarInit(&params[2]);
        params[2].vt = VTYPE_PWSTR; params[2].pwstrVal = (WCHAR_T*)wXml.c_str();   params[2].wstrLen = (uint32_t)wXml.size();

        tVariant ret; tVarInit(&ret);
        bool called = comp->CallAsFunc(idxConnect, &ret, params, 3);
        bool retTrue = (ret.vt == VTYPE_BOOL && ret.bVal);
        CHECK(called && retTrue, "L-p3: ПодключитьОборудование -> true");

        // OUT DeviceID з paParams[0].
        if (params[0].vt == VTYPE_PWSTR && params[0].pwstrVal) {
            deviceId = u16to8(reinterpret_cast<const wchar_t*>(params[0].pwstrVal), params[0].wstrLen);
            mem.FreeMemory((void**)&params[0].pwstrVal); // виділено головною DLL через наш HostMemoryManager
        }
        std::printf("  DeviceID=%s\n", deviceId.c_str());
        CHECK(!deviceId.empty(), "L-p3: DeviceID отримано через paParams[0]");
    }

    // 5) ПечатьЭтикеток: IN DeviceID, IN LabelsTable(XML з EAN13), IN PackageStatus="first" -> BOOL.
    if (!deviceId.empty()) {
        long idxPrint = comp->FindMethod(L"ПечатьЭтикеток");
        CHECK(idxPrint >= 0, "L-p3: метод ПечатьЭтикеток знайдено");
        if (idxPrint >= 0) {
            const char* labelsXml =
                "<?xml version=\"1.0\"?><Data>"
                "<Formatting Width=\"60\" Height=\"40\">"
                "<Text FieldName=\"Name\" Left=\"1\" Top=\"1\" Width=\"55\" Height=\"10\" FontName=\"Arial\" FontSize=\"8\"/>"
                "<Barcode FieldName=\"Bar\" Type=\"EAN13\" Left=\"1\" Top=\"22\" Height=\"10\" PrintHRI=\"true\" FontSize=\"8\"/>"
                "</Formatting>"
                "<Labels>"
                "<Label Quantity=\"2\">"
                "<Record FieldName=\"Name\" Value=\"Блокнот\"/>"
                "<Record FieldName=\"Bar\" Value=\"4008110271538\"/>"
                "</Label>"
                "</Labels></Data>";

            std::wstring wId     = u8to16(deviceId);
            std::wstring wLabels = u8to16(labelsXml);
            std::wstring wStatus = u8to16("first");

            tVariant params[3];
            tVarInit(&params[0]); params[0].vt = VTYPE_PWSTR; params[0].pwstrVal = (WCHAR_T*)wId.c_str();     params[0].wstrLen = (uint32_t)wId.size();
            tVarInit(&params[1]); params[1].vt = VTYPE_PWSTR; params[1].pwstrVal = (WCHAR_T*)wLabels.c_str(); params[1].wstrLen = (uint32_t)wLabels.size();
            tVarInit(&params[2]); params[2].vt = VTYPE_PWSTR; params[2].pwstrVal = (WCHAR_T*)wStatus.c_str(); params[2].wstrLen = (uint32_t)wStatus.size();

            tVariant ret; tVarInit(&ret);
            bool called = comp->CallAsFunc(idxPrint, &ret, params, 3);
            bool retTrue = (ret.vt == VTYPE_BOOL && ret.bVal);
            CHECK(called && retTrue, "L-p3: ПечатьЭтикеток -> true");

            // Дочекатися, поки емулятор прийме ZPL із сокета (Send синхронний, recv — асинхронний).
            std::string zpl;
            for (int i = 0; i < 50; ++i) {          // до ~5с
                zpl = emu.LastZpl();
                if (zpl.find("^XA") != std::string::npos) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            CHECK(zpl.find("^XA") != std::string::npos, "L-p3: емулятор отримав ZPL із ^XA");
        }
    }

    // 6) Прибирання: відключити пристрій, знищити компоненту, вивантажити DLL.
    long idxDisc = comp->FindMethod(L"ОтключитьОборудование");
    if (idxDisc >= 0 && !deviceId.empty()) {
        std::wstring wId = u8to16(deviceId);
        tVariant params[1];
        tVarInit(&params[0]); params[0].vt = VTYPE_PWSTR; params[0].pwstrVal = (WCHAR_T*)wId.c_str(); params[0].wstrLen = (uint32_t)wId.size();
        tVariant ret; tVarInit(&ret);
        comp->CallAsFunc(idxDisc, &ret, params, 1);
    }
    pDestroyObject(&comp);
    FreeLibrary(h);
    emu.Stop();

    std::printf(g_failed ? "\nFAILED: %d\n" : "\nOK\n", g_failed);
    return g_failed ? 1 : 0;
}

#endif // _WIN32
