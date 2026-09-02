//
// @file tests/label_native_host.cpp
// @brief L-p3-харнес компоненти LabelPrinter поверх ГОЛОВНОЇ DLL.
//        Емулює платформу 1С: вантажить головну DLL через LoadLibraryW,
//        отримує IComponentBase через експорт GetClassObject, надає власні
//        IAddInDefBase та IMemoryManager і кличе КОРОТКІ імена контракту
//        «Подключаемое оборудование» — УстановитьПараметр / Подключить /
//        ТестУстройства / ИнициализацияПринтера / ПечатьЭтикеток, тобто рівно
//        ті, які кличе конфігурація, а НЕ довгі імена з таблиці ІТС
//        (маршалінг tVariant VTYPE_PWSTR і VTYPE_R8 у параметрах;
//        ИДУстройства — OUT через paParams[0]). Окремо перевіряє, що старі
//        довгі імена зникли: лишившись, вони маскували б помилку налаштування.
//        ZPL-принтер емулюється in-process LabelEmulator-ом.
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
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <clocale>
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
    // %ls у діагностиці нижче конвертує wchar_t за LC_CTYPE: у дефолтній
    // локалі "C" кирилиця не конвертується й printf МОВЧКИ обриває рядок —
    // імена ненайдених методів просто зникали б зі звіту. LC_NUMERIC не чіпаємо.
    std::setlocale(LC_CTYPE, ".UTF-8");

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

    // Хелпери маршалінгу (за зразком ecr_native_host.cpp).
    auto setInStr = [](tVariant& v, const std::wstring& s) {
        tVarInit(&v);
        const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
        v.vt = VTYPE_PWSTR;
        v.pwstrVal = (WCHAR_T*)malloc(bytes);
        memcpy(v.pwstrVal, s.c_str(), bytes);
        v.wstrLen = (uint32_t)s.size();
    };
    auto outStr = [](const tVariant& v) -> std::string {
        return (v.vt == VTYPE_PWSTR && v.pwstrVal)
            ? u16to8(reinterpret_cast<const wchar_t*>(v.pwstrVal), v.wstrLen)
            : std::string{};
    };
    // Виклик методу, усі аргументи якого — рядки; результат — BOOL.
    auto callBool = [&](long idx, const std::vector<std::string>& args) -> bool {
        std::vector<std::wstring> w;
        for (const auto& a : args) w.push_back(u8to16(a));
        std::vector<tVariant> p(args.empty() ? 1 : args.size());
        for (size_t i = 0; i < w.size(); ++i) setInStr(p[i], w[i]);
        tVariant ret; tVarInit(&ret);
        comp->CallAsFunc(idx, &ret, p.data(), (long)w.size());
        for (auto& v : p) if (v.vt == VTYPE_PWSTR && v.pwstrVal) free(v.pwstrVal);
        return ret.vt == VTYPE_BOOL && ret.bVal;
    };

    // 4) Ревізія — рівно 3004: за нею конфігурація обирає розкладку викликів.
    long idxRev = comp->FindMethod(L"ПолучитьРевизиюИнтерфейса");
    CHECK(idxRev >= 0, "L-p3: ПолучитьРевизиюИнтерфейса знайдено");
    if (idxRev >= 0) {
        tVariant ret; tVarInit(&ret);
        comp->CallAsFunc(idxRev, &ret, nullptr, 0);
        long rev = (ret.vt == VTYPE_I4) ? ret.lVal : (long)ret.dblVal;
        CHECK(rev == 3004, "L-p3: ревізія інтерфейсу == 3004");
    }

    // 5) Усі імена контракту БПО зареєстровані під ТИМИ іменами, які кличе 1С.
    //    Саме цього не було в старій версії: драйвер реєстрував довгі імена за ІТС.
    {
        const wchar_t* contract[] = {
            L"ПолучитьРевизиюИнтерфейса", L"ПолучитьНомерВерсии", L"ПолучитьОписание",
            L"ПолучитьПараметры", L"УстановитьПараметр", L"Подключить", L"Отключить",
            L"ТестУстройства", L"ПолучитьОшибку", L"ПолучитьДополнительныеДействия",
            L"ВыполнитьДополнительноеДействие", L"ИнициализацияПринтера", L"ПечатьЭтикеток"
        };
        bool all = true;
        for (const wchar_t* n : contract)
            if (comp->FindMethod(n) < 0) { all = false; std::printf("  немає: %ls\n", n); }
        CHECK(all, "L-p3: усі імена контракту БПО зареєстровано");

        // Старі довгі імена за документом ІТС мають ЗНИКНУТИ — вони не працювали
        // ніколи, а лишившись, маскували б помилку налаштування.
        const wchar_t* legacy[] = { L"ПодключитьОборудование", L"ПараметрыОборудования",
                                    L"ТестированиеОборудования", L"ОтключитьОборудование",
                                    L"УстановитьИнформациюПриложения" };
        bool none = true;
        for (const wchar_t* n : legacy)
            if (comp->FindMethod(n) >= 0) { none = false; std::printf("  лишилось: %ls\n", n); }
        CHECK(none, "L-p3: старі довгі імена прибрано");
    }

    // 6) Паспорт і форма налаштувань.
    long idxDescr = comp->FindMethod(L"ПолучитьОписание");
    CHECK(idxDescr >= 0, "L-p3: ПолучитьОписание знайдено");
    if (idxDescr >= 0) {
        tVariant p; tVarInit(&p);
        tVariant ret; tVarInit(&ret);
        comp->CallAsFunc(idxDescr, &ret, &p, 1);
        const std::string xml = outStr(p);
        if (p.vt == VTYPE_PWSTR && p.pwstrVal) free(p.pwstrVal);
        CHECK(xml.find("EquipmentType=\"LabelPrinter\"") != std::string::npos,
              "L-p3: паспорт оголошує EquipmentType=LabelPrinter");
    }
    long idxParams = comp->FindMethod(L"ПолучитьПараметры");
    CHECK(idxParams >= 0, "L-p3: ПолучитьПараметры знайдено");
    if (idxParams >= 0) {
        tVariant p; tVarInit(&p);
        tVariant ret; tVarInit(&ret);
        comp->CallAsFunc(idxParams, &ret, &p, 1);
        const std::string xml = outStr(p);
        if (p.vt == VTYPE_PWSTR && p.pwstrVal) free(p.pwstrVal);
        // ⚠️ Корінь МАЄ бути Settings: з коренем Parameters форма БПО мовчки
        // лишається без жодного поля.
        CHECK(xml.find("<Settings>") != std::string::npos,
              "L-p3: форма налаштувань має кореневий вузол Settings");
    }

    // 7) УстановитьПараметр: тип обладнання приходить ІМЕНЕМ ЗНАЧЕННЯ ПЕРЕЛІКУ.
    long idxSetParam = comp->FindMethod(L"УстановитьПараметр");
    CHECK(idxSetParam >= 0, "L-p3: УстановитьПараметр знайдено");
    if (idxSetParam >= 0) {
        CHECK(callBool(idxSetParam, { "EquipmentType", "ПринтерЭтикеток" }),
              "L-p3: УстановитьПараметр(EquipmentType) прийнято");
        CHECK(!callBool(idxSetParam, { "EquipmentType", "ЭквайринговыйТерминал" }),
              "L-p3: чужий тип обладнання відхилено");
        callBool(idxSetParam, { "EquipmentType", "ПринтерЭтикеток" });
        callBool(idxSetParam, { "TransportKind", "tcp" });
        callBool(idxSetParam, { "Host", "127.0.0.1" });

        // ⚠️ Port і DotsPerMm оголошені в формі як Number, тож 1С передає їх ЧИСЛОМ.
        // Пряме приведення VH до рядка на цьому кидає — саме на цьому горів
        // еквайринговий фасад.
        auto setNumber = [&](const char* name, double value) -> bool {
            std::wstring wName = u8to16(name);
            tVariant p[2];
            for (auto& v : p) tVarInit(&v);
            p[0].vt = VTYPE_PWSTR; p[0].pwstrVal = (WCHAR_T*)wName.c_str();
            p[0].wstrLen = (uint32_t)wName.size();
            p[1].vt = VTYPE_R8;    p[1].dblVal = value;
            tVariant ret; tVarInit(&ret);
            comp->CallAsFunc(idxSetParam, &ret, p, 2);
            return ret.vt == VTYPE_BOOL && ret.bVal;
        };
        CHECK(setNumber("Port", (double)emu.Port()),
              "L-p3: УстановитьПараметр приймає ЧИСЛОВЕ значення (Port)");
        CHECK(setNumber("DotsPerMm", 8.0),
              "L-p3: УстановитьПараметр приймає ЧИСЛОВЕ значення (DotsPerMm)");
    }

    // 8) ТестУстройства: реально відкриває канал до емулятора й закриває.
    long idxTest = comp->FindMethod(L"ТестУстройства");
    CHECK(idxTest >= 0, "L-p3: ТестУстройства знайдено");
    if (idxTest >= 0) {
        tVariant p[2];
        for (auto& v : p) tVarInit(&v);
        tVariant ret; tVarInit(&ret);
        comp->CallAsFunc(idxTest, &ret, p, 2);
        const std::string text = outStr(p[0]);
        std::printf("  ТестУстройства: %s\n", text.c_str());
        CHECK(ret.vt == VTYPE_BOOL && ret.bVal, "L-p3: ТестУстройства -> true (емулятор досяжний)");
        CHECK(p[1].vt == VTYPE_BOOL && !p[1].bVal, "L-p3: АктивированДемоРежим == Ложь");
        CHECK(!text.empty(), "L-p3: РезультатТеста несе текст для адміністратора");
        if (p[0].vt == VTYPE_PWSTR && p[0].pwstrVal) free(p[0].pwstrVal);
    }

    // 9) Подключить: параметрів не приймає, ИДУстройства — OUT.
    std::string deviceId;
    long idxConnect = comp->FindMethod(L"Подключить");
    CHECK(idxConnect >= 0, "L-p3: Подключить знайдено");
    if (idxConnect >= 0) {
        tVariant p; tVarInit(&p);
        tVariant ret; tVarInit(&ret);
        comp->CallAsFunc(idxConnect, &ret, &p, 1);
        if (p.vt == VTYPE_PWSTR && p.pwstrVal) {
            deviceId = u16to8(reinterpret_cast<const wchar_t*>(p.pwstrVal), p.wstrLen);
            free(p.pwstrVal);
        }
        CHECK(ret.vt == VTYPE_BOOL && ret.bVal, "L-p3: Подключить -> true");
        CHECK(!deviceId.empty(), "L-p3: ИДУстройства повернуто в OUT");
        std::printf("  DeviceID=%s\n", deviceId.c_str());
    }

    // 10) ИнициализацияПринтера + ПечатьЭтикеток проти емулятора.
    if (!deviceId.empty()) {
        long idxInit = comp->FindMethod(L"ИнициализацияПринтера");
        CHECK(idxInit >= 0 && callBool(idxInit, { deviceId }),
              "L-p3: ИнициализацияПринтера -> true");

        long idxPrint = comp->FindMethod(L"ПечатьЭтикеток");
        CHECK(idxPrint >= 0, "L-p3: ПечатьЭтикеток знайдено");
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
            CHECK(callBool(idxPrint, { deviceId, labelsXml, "first" }),
                  "L-p3: ПечатьЭтикеток -> true");

            // Дочекатися, поки емулятор прийме ZPL із сокета. Чекаємо саме на ^BE,
            // а не на ^XA/^XZ: Send синхронний, а recv — ні, і перед етикеткою в
            // потік уже пішов ЦІЛИЙ кадр ИнициализацияПринтера (^XA…^XZ). Тобто
            // і ^XA, і ^XZ у буфері з'являються ЗАДОВГО до штрихкоду — очікування
            // по них давало плаваючий FAIL на перевірці ^BE (спіймано на x86).
            std::string zpl;
            for (int i = 0; i < 50; ++i) {
                zpl = emu.LastZpl();
                if (zpl.find("^BE") != std::string::npos) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            CHECK(zpl.find("^XA") != std::string::npos, "L-p3: емулятор отримав ZPL із ^XA");
            CHECK(zpl.find("^BE") != std::string::npos, "L-p3: емулятор отримав нативний EAN13 ^BE");

            // 11) Навмисна помилка: чужий ИДУстройства має дати відмову з кодом і описом.
            CHECK(!callBool(idxPrint, { "no-such-device", labelsXml, "first" }),
                  "L-p3: ПечатьЭтикеток із чужим ИДУстройства відхилено");
            long idxErr = comp->FindMethod(L"ПолучитьОшибку");
            CHECK(idxErr >= 0, "L-p3: ПолучитьОшибку знайдено");
            if (idxErr >= 0) {
                tVariant ep; tVarInit(&ep);
                tVariant eret; tVarInit(&eret);
                comp->CallAsFunc(idxErr, &eret, &ep, 1);
                long code = (eret.vt == VTYPE_I4) ? eret.lVal : (long)eret.dblVal;
                const std::string desc = outStr(ep);
                if (ep.vt == VTYPE_PWSTR && ep.pwstrVal) free(ep.pwstrVal);
                std::printf("  ПолучитьОшибку: code=%ld desc=%s\n", code, desc.c_str());
                CHECK(code != 0 && !desc.empty(), "L-p3: ПолучитьОшибку дає код і опис");
            }
        }
    }

    // 12) Прибирання.
    long idxDisc = comp->FindMethod(L"Отключить");
    if (idxDisc >= 0 && !deviceId.empty())
        CHECK(callBool(idxDisc, { deviceId }), "L-p3: Отключить -> true");

    // 13) Порожній Host при tcp — це НЕЗАПОВНЕНИЙ ПАРАМЕТР, а не недоступний
    //     принтер. Адміністратор має побачити BAD_INPUT (12) з назвою параметра,
    //     інакше він шукатиме несправність у мережі замість форми налаштувань.
    if (idxSetParam >= 0 && idxConnect >= 0) {
        callBool(idxSetParam, { "Host", "" });
        tVariant p; tVarInit(&p);
        tVariant ret; tVarInit(&ret);
        comp->CallAsFunc(idxConnect, &ret, &p, 1);
        if (p.vt == VTYPE_PWSTR && p.pwstrVal) free(p.pwstrVal);
        CHECK(!(ret.vt == VTYPE_BOOL && ret.bVal),
              "L-p3: Подключить із порожнім Host відхилено");

        long idxErrBad = comp->FindMethod(L"ПолучитьОшибку");
        if (idxErrBad >= 0) {
            tVariant ep; tVarInit(&ep);
            tVariant eret; tVarInit(&eret);
            comp->CallAsFunc(idxErrBad, &eret, &ep, 1);
            long code = (eret.vt == VTYPE_I4) ? eret.lVal : (long)eret.dblVal;
            const std::string desc = outStr(ep);
            if (ep.vt == VTYPE_PWSTR && ep.pwstrVal) free(ep.pwstrVal);
            std::printf("  порожній Host: code=%ld desc=%s\n", code, desc.c_str());
            CHECK(code == 12, "L-p3: порожній Host дає код BAD_INPUT (12), а не транспортну помилку");
            CHECK(desc.find("Host") != std::string::npos,
                  "L-p3: опис помилки називає незаповнений параметр");
        }
    }

    pDestroyObject(&comp);
    FreeLibrary(h);
    emu.Stop();

    std::printf(g_failed ? "\nFAILED: %d\n" : "\nOK\n", g_failed);
    return g_failed ? 1 : 0;
}

#endif // _WIN32
