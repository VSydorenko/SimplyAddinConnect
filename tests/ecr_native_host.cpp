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
    // Коротка перевірка зв'язку, якою ProbeDevice обслуговує ТестУстройства.
    emu.OnRequest("GetTerminalInfo", [](const json&) {
        return std::string(R"({"method":"GetTerminalInfo","params":{"responseCode":"0000"},"error":false})");
    });
    // Повний набір полів, як віддає реальний термінал: БПО-фасад розкладає їх
    // по OUT-параметрах, і саме це перевіряє крок 7.
    emu.OnRequest("Purchase", [](const json&) {
        return std::string(R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"77",)"
                           R"("rrn":"555000111","approvalCode":"A12345","cardPAN":"444455**1234",)"
                           R"("amount":"100.50","receiptText":"СЛІП\nрядок 2"},"error":false})");
    });
    // Потрібен для перевірки скасування-через-повернення (VoidAsRefund).
    emu.OnRequest("Refund", [](const json&) {
        return std::string(R"({"method":"Refund","params":{"responseCode":"0000","invoiceNumber":"78",)"
                           R"("rrn":"555000222","approvalCode":"B67890","cardPAN":"444455**1234",)"
                           R"("amount":"100.50","receiptText":"ПОВЕРНЕННЯ"},"error":false})");
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

    // 6) Відключити прямий клас — доступ до термінала монопольний, і БПО-фасад
    //    нижче має підключитися сам.
    comp->FindMethod(L"Disconnect") >= 0
        ? (void)comp->CallAsProc(comp->FindMethod(L"Disconnect"), nullptr, 0)
        : (void)0;

    // ================= 7) БПО-фасад ревізії 3004 через ту саму DLL =================
    // Перевіряє найризикованіше: диспетчеризацію семи параметрів, IN/OUT-запис у
    // слоти платформи, числову СуммаОперации (VTYPE_R8) і чесну відмову там, де
    // драйвер операції не має. Контракт — docs/architecture/bpo-contract.md §2.4.
    {
        IComponentBase* bpo = nullptr;
        pGetClassObject(L"ECRPrivatBPO3004", &bpo);
        CHECK(bpo != nullptr, "L3-bpo: компонента ECRPrivatBPO3004 створена через DLL");

        if (bpo) {
            HostConnect bconn;
            HostMemoryManager bmem;
            CHECK(bpo->Init((void*)&bconn) && bpo->setMemManager((void*)&bmem),
                  "L3-bpo: Init + setMemManager");

            // Ревізія — рівно 3004, інакше 1С візьме іншу розкладку параметрів.
            long idxRev = bpo->FindMethod(L"GetInterfaceRevision");
            CHECK(idxRev >= 0, "L3-bpo: ПолучитьРевизиюИнтерфейса знайдено");
            if (idxRev >= 0) {
                tVariant ret; tVarInit(&ret);
                bpo->CallAsFunc(idxRev, &ret, nullptr, 0);
                long rev = (ret.vt == VTYPE_I4) ? ret.lVal : (long)ret.dblVal;
                CHECK(rev == 3004, "L3-bpo: ревізія інтерфейсу == 3004");
            }

            // EquipmentType приходить ІМЕНЕМ ЗНАЧЕННЯ ПЕРЕЛІКУ, не англійським рядком.
            long idxSetParam = bpo->FindMethod(L"SetParameter");
            CHECK(idxSetParam >= 0, "L3-bpo: УстановитьПараметр знайдено");
            auto setParam = [&](const char* name, const std::string& value) -> bool {
                bool rb = false; std::string rs; bool gotStr = false;
                callFunc(bpo, idxSetParam, { name, value }, rb, rs, gotStr);
                return rb;
            };
            if (idxSetParam >= 0) {
                CHECK(setParam("EquipmentType", "ЭквайринговыйТерминал"),
                      "L3-bpo: УстановитьПараметр(EquipmentType) прийнято");
                CHECK(!setParam("EquipmentType", "BarcodeScanner"),
                      "L3-bpo: чужий тип обладнання відхилено");
                setParam("EquipmentType", "ЭквайринговыйТерминал");
                setParam("TransportKind", "tcp");
                setParam("Host", "127.0.0.1");

                // ⚠️ Port оголошено в формі налаштувань як Number, тож 1С передає
                // його ЧИСЛОМ, а не рядком. Пряме приведення VH до рядка на цьому
                // кидає — саме тому подаємо число, а не std::to_string(port).
                std::wstring wName = u8to16("Port");
                tVariant pp[2];
                for (auto& v : pp) tVarInit(&v);
                pp[0].vt = VTYPE_PWSTR; pp[0].pwstrVal = (WCHAR_T*)wName.c_str();
                pp[0].wstrLen = (uint32_t)wName.size();
                pp[1].vt = VTYPE_R8;    pp[1].dblVal = (double)emu.Port();
                tVariant pret; tVarInit(&pret);
                bpo->CallAsFunc(idxSetParam, &pret, pp, 2);
                CHECK(pret.vt == VTYPE_BOOL && pret.bVal,
                      "L3-bpo: УстановитьПараметр приймає ЧИСЛОВЕ значення (Port)");
            }

            // Подключить: параметрів не приймає, ИДУстройства — OUT.
            std::string deviceId;
            long idxBpoConnect = bpo->FindMethod(L"Connect");
            CHECK(idxBpoConnect >= 0, "L3-bpo: Подключить знайдено");
            if (idxBpoConnect >= 0) {
                tVariant p; tVarInit(&p);
                tVariant ret; tVarInit(&ret);
                bpo->CallAsFunc(idxBpoConnect, &ret, &p, 1);
                bool rb = (ret.vt == VTYPE_BOOL) && ret.bVal;
                if (p.vt == VTYPE_PWSTR && p.pwstrVal) {
                    deviceId = u16to8(reinterpret_cast<const wchar_t*>(p.pwstrVal), p.wstrLen);
                    free(p.pwstrVal);
                }
                CHECK(rb, "L3-bpo: Подключить → true");
                CHECK(!deviceId.empty(), "L3-bpo: ИДУстройства повернуто в OUT");
            }

            // ⚠️ Вхідний рядок IN/OUT-параметра ОБОВ'ЯЗКОВО виділяти через malloc:
            // компонента перед перезаписом кличе FreeMemory на ньому (шлях
            // VariantHelper::clear()). У реальній 1С пам'ять виділяє платформа своїм
            // менеджером, тож це коректно; у харнесі вказівник на c_str() дав би
            // STATUS_HEAP_CORRUPTION. Той самий malloc, що й у HostMemoryManager.
            auto setInStr = [](tVariant& v, const std::wstring& s) {
                tVarInit(&v);
                const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
                v.vt = VTYPE_PWSTR;
                v.pwstrVal = (WCHAR_T*)malloc(bytes);
                memcpy(v.pwstrVal, s.c_str(), bytes);
                v.wstrLen = (uint32_t)s.size();
            };

            // ⚠️ РЕГРЕСІЯ ПРО ГРОШІ: ТестУстройства НЕ СМІЄ рвати активне з'єднання.
            // ProbeDevice раніше безумовно робив Open → Probe → Close, тож на вже
            // підключеному об'єкті лишав драйвер роз'єднаним, а ИДУстройства на
            // фасаді — виданим. Каса вважала термінал підключеним (тест же щойно
            // сказав «на зв'язку»), і перша ж оплата падала з NOT_CONNECTED, доки
            // обладнання не перепідключать вручну. Відтворюємо шлях адміністратора:
            // «Тест устройства» на підключеному об'єкті, одразу після нього — оплата.
            long idxTest = bpo->FindMethod(L"EquipmentTest");
            long idxPayCheck = bpo->FindMethod(L"PayByPaymentCard");
            CHECK(idxTest >= 0, "L3-bpo: ТестУстройства знайдено");
            if (idxTest >= 0 && idxPayCheck >= 0 && !deviceId.empty()) {
                tVariant t[2];
                for (auto& v : t) tVarInit(&v);
                tVariant tret; tVarInit(&tret);
                bpo->CallAsFunc(idxTest, &tret, t, 2);
                const std::string testText = (t[0].vt == VTYPE_PWSTR && t[0].pwstrVal)
                    ? u16to8(reinterpret_cast<const wchar_t*>(t[0].pwstrVal), t[0].wstrLen)
                    : std::string{};
                std::printf("  РезультатТеста: %s\n", testText.c_str());
                CHECK(tret.vt == VTYPE_BOOL && tret.bVal,
                      "L3-bpo: ТестУстройства на підключеному об'єкті -> true");
                for (auto& v : t)
                    if (v.vt == VTYPE_PWSTR && v.pwstrVal) free(v.pwstrVal);

                // Ось воно: якби тест закрив канал, оплата віддала б NOT_CONNECTED.
                tVariant p[7];
                for (auto& v : p) tVarInit(&v);
                setInStr(p[0], u8to16(deviceId));
                p[2].vt = VTYPE_R8; p[2].dblVal = 1.00;
                tVariant pret; tVarInit(&pret);
                bpo->CallAsFunc(idxPayCheck, &pret, p, 7);
                CHECK(pret.vt == VTYPE_BOOL && pret.bVal,
                      "L3-bpo: ТестУстройства не рве активне з'єднання (оплата після тесту проходить)");
                for (auto& v : p)
                    if (v.vt == VTYPE_PWSTR && v.pwstrVal) free(v.pwstrVal);
            }

            // ⚠️ ДРУГА ПОЛОВИНА ТІЄЇ САМОЇ ВИМОГИ: перевикористання живого каналу
            // дозволене, тільки поки параметри ведуть НА ТУ САМУ ЦІЛЬ. Сценарій
            // адміністратора: термінал підключений на робочій адресі, у формі
            // налаштувань міняють адресу (тут — порт) і тиснуть «Тест устройства».
            // Перевірка старого каналу відрапортувала б «Термінал на зв'язку» про
            // адресу, якої ніхто не чіпав. Сусідній фасад AddinLabelPrinter цей
            // сценарій ловить — еквайринговий зобов'язаний ловити так само.
            if (idxTest >= 0 && idxSetParam >= 0 && !deviceId.empty()) {
                // Port оголошено Number — шлемо ЧИСЛОМ, як платформа (див. вище).
                auto setPortNumber = [&](double port) {
                    std::wstring wn = u8to16("Port");
                    tVariant pp[2];
                    for (auto& v : pp) tVarInit(&v);
                    pp[0].vt = VTYPE_PWSTR; pp[0].pwstrVal = (WCHAR_T*)wn.c_str();
                    pp[0].wstrLen = (uint32_t)wn.size();
                    pp[1].vt = VTYPE_R8;    pp[1].dblVal = port;
                    tVariant r; tVarInit(&r);
                    bpo->CallAsFunc(idxSetParam, &r, pp, 2);
                };
                // Порт 1: на ньому свідомо ніхто не слухає, з'єднання відхиляється
                // одразу — нова ціль недосяжна, і чесна відповідь тесту тільки одна.
                setPortNumber(1.0);

                tVariant t[2];
                for (auto& v : t) tVarInit(&v);
                tVariant tret; tVarInit(&tret);
                bpo->CallAsFunc(idxTest, &tret, t, 2);
                const std::string movedText = (t[0].vt == VTYPE_PWSTR && t[0].pwstrVal)
                    ? u16to8(reinterpret_cast<const wchar_t*>(t[0].pwstrVal), t[0].wstrLen)
                    : std::string{};
                std::printf("  РезультатТеста (ціль змінено): %s\n", movedText.c_str());
                CHECK(!(tret.vt == VTYPE_BOOL && tret.bVal),
                      "L3-bpo: ТестУстройства перевіряє НОВУ ціль, а не старий канал");
                for (auto& v : t)
                    if (v.vt == VTYPE_PWSTR && v.pwstrVal) free(v.pwstrVal);

                // Повертаємо робочу ціль. Живий канал перевірка чужої адреси чіпати
                // не мала права — це доводять перевірки нижче, які йдуть по ньому ж.
                setPortNumber((double)emu.Port());
            }

            // ПараметрыТерминала: за цими прапорцями конфігурація вирішує, які операції
            // показати касиру. Після переїзду прапорців у Capabilities() драйвера цей
            // рядок не мав змінитись — для ПриватБанку друк сліпа на терміналі увімкнено,
            // решта шість можливостей вимкнені.
            long idxTermParams = bpo->FindMethod(L"TerminalParameters");
            CHECK(idxTermParams >= 0, "L3-bpo: ПараметрыТерминала знайдено");
            if (idxTermParams >= 0 && !deviceId.empty()) {
                tVariant p[2];
                for (auto& v : p) tVarInit(&v);
                setInStr(p[0], u8to16(deviceId));
                tVariant ret; tVarInit(&ret);
                bpo->CallAsFunc(idxTermParams, &ret, p, 2);
                const std::string xml = (p[1].vt == VTYPE_PWSTR && p[1].pwstrVal)
                    ? u16to8(reinterpret_cast<const wchar_t*>(p[1].pwstrVal), p[1].wstrLen)
                    : std::string{};
                std::printf("  ПараметрыТерминала: %s\n", xml.c_str());
                CHECK(ret.vt == VTYPE_BOOL && ret.bVal, "L3-bpo: ПараметрыТерминала -> true");
                CHECK(xml.find("PrintSlipOnTerminal=\"true\"") != std::string::npos,
                      "L3-bpo: PrintSlipOnTerminal=true (термінал друкує квитанції сам)");
                CHECK(xml.find("PartialCancellation=\"false\"") != std::string::npos,
                      "L3-bpo: PartialCancellation=false");
                CHECK(xml.find("CashWithdrawal=\"false\"") != std::string::npos,
                      "L3-bpo: CashWithdrawal=false");
                CHECK(xml.find("ConsumerPresentedQR=\"false\"") != std::string::npos,
                      "L3-bpo: ConsumerPresentedQR=false");
                CHECK(xml.find("ElectronicCertificates=\"false\"") != std::string::npos,
                      "L3-bpo: ElectronicCertificates=false");
                CHECK(xml.find("ListCardTransactions=\"false\"") != std::string::npos,
                      "L3-bpo: ListCardTransactions=false");
                CHECK(xml.find("ShortSlip=\"false\"") != std::string::npos,
                      "L3-bpo: ShortSlip=false");
                for (auto& v : p)
                    if (v.vt == VTYPE_PWSTR && v.pwstrVal) free(v.pwstrVal);
            }

            // ОплатитьПлатежнойКартой: сімка, позиція 2 — ЧИСЛО, решта після
            // ИДУстройства — IN/OUT. Перевіряємо саме запис назад у слоти.
            long idxPay = bpo->FindMethod(L"PayByPaymentCard");
            CHECK(idxPay >= 0, "L3-bpo: ОплатитьПлатежнойКартой знайдено");
            if (idxPay >= 0 && !deviceId.empty()) {
                tVariant p[7];
                for (auto& v : p) tVarInit(&v);
                setInStr(p[0], u8to16(deviceId));
                p[2].vt = VTYPE_R8;    p[2].dblVal = 100.50;
                // 1,3..6 лишаються VTYPE_EMPTY — саме так 1С шле незаповнені IN/OUT.

                tVariant ret; tVarInit(&ret);
                bool ok = bpo->CallAsFunc(idxPay, &ret, p, 7);
                bool rb = (ret.vt == VTYPE_BOOL) && ret.bVal;
                CHECK(ok && rb, "L3-bpo: ОплатитьПлатежнойКартой → true");

                auto outStr = [](const tVariant& v) -> std::string {
                    return (v.vt == VTYPE_PWSTR && v.pwstrVal)
                        ? u16to8(reinterpret_cast<const wchar_t*>(v.pwstrVal), v.wstrLen)
                        : std::string{};
                };
                const std::string card = outStr(p[1]);
                const std::string receipt = outStr(p[3]);
                const std::string rrn = outStr(p[4]);
                const std::string auth = outStr(p[5]);
                const std::string slip = outStr(p[6]);
                std::printf("  bpo OUT: card=%s amount=%.2f receipt=%s rrn=%s auth=%s slipLen=%zu\n",
                            card.c_str(), p[2].dblVal, receipt.c_str(), rrn.c_str(),
                            auth.c_str(), slip.size());

                CHECK(card == "444455**1234", "L3-bpo: НомерКарты записано в OUT");
                CHECK(receipt == "77",        "L3-bpo: НомерЧека записано в OUT");
                CHECK(rrn == "555000111",     "L3-bpo: СсылочныйНомер записано в OUT");
                CHECK(auth == "A12345",       "L3-bpo: КодАвторизации записано в OUT");
                CHECK(!slip.empty() && slip.find('\n') != std::string::npos,
                      "L3-bpo: ТекстСлипЧека багаторядковий, записано в OUT");
                CHECK(p[2].vt == VTYPE_R8 && p[2].dblVal > 100.49 && p[2].dblVal < 100.51,
                      "L3-bpo: СуммаОперации лишилась числом і не втратила дріб");

                for (auto& v : p)
                    if (v.vt == VTYPE_PWSTR && v.pwstrVal) free(v.pwstrVal);
            }

            // Скасування (сторно). Власної операції void термінал не має, тож за
            // дефолтним VoidAsRefund воно має піти ПОВЕРНЕННЯМ за RRN і вдатися;
            // зі знятим параметром — чесно відмовити.
            long idxVoid = bpo->FindMethod(L"CancelPaymentByPaymentCard");
            CHECK(idxVoid >= 0, "L3-bpo: ОтменитьПлатежПоПлатежнойКарте зареєстровано");

            auto callVoid = [&](const std::string& rrnIn, bool& retBool,
                                std::string& outSlip) {
                tVariant p[7];
                for (auto& v : p) tVarInit(&v);
                setInStr(p[0], u8to16(deviceId));
                p[2].vt = VTYPE_R8; p[2].dblVal = 100.50;
                setInStr(p[4], u8to16(rrnIn));

                tVariant ret; tVarInit(&ret);
                bpo->CallAsFunc(idxVoid, &ret, p, 7);
                retBool = (ret.vt == VTYPE_BOOL) && ret.bVal;
                outSlip = (p[6].vt == VTYPE_PWSTR && p[6].pwstrVal)
                    ? u16to8(reinterpret_cast<const wchar_t*>(p[6].pwstrVal), p[6].wstrLen)
                    : std::string{};
                for (auto& v : p)
                    if (v.vt == VTYPE_PWSTR && v.pwstrVal) free(v.pwstrVal);
            };

            if (idxVoid >= 0 && !deviceId.empty()) {
                bool rb = false; std::string slip;
                callVoid("555000111", rb, slip);
                CHECK(rb, "L3-bpo: скасування виконано поверненням за RRN (VoidAsRefund)");
                CHECK(slip.find("ПОВЕРНЕННЯ") != std::string::npos,
                      "L3-bpo: сліп скасування — від операції повернення");

                // Порожній RRN: повертати нема за чим — має бути відмова, не тиша.
                bool rbEmpty = false; std::string slipEmpty;
                callVoid("", rbEmpty, slipEmpty);
                CHECK(!rbEmpty, "L3-bpo: скасування без RRN відхилено");

                // Вимикаємо VoidAsRefund → має спрацювати чесна відмова (ІТС §1.3).
                if (idxSetParam >= 0) {
                    bool sb = false; std::string ss; bool sg = false;
                    callFunc(bpo, idxSetParam, { "VoidAsRefund", "false" }, sb, ss, sg);
                }
                bool rbOff = false; std::string slipOff;
                callVoid("555000111", rbOff, slipOff);
                CHECK(!rbOff, "L3-bpo: зі знятим VoidAsRefund скасування відмовило");

                long idxErr = bpo->FindMethod(L"GetLastError");
                if (idxErr >= 0) {
                    tVariant ep; tVarInit(&ep);
                    tVariant eret; tVarInit(&eret);
                    bpo->CallAsFunc(idxErr, &eret, &ep, 1);
                    long code = (eret.vt == VTYPE_I4) ? eret.lVal : (long)eret.dblVal;
                    std::string desc;
                    if (ep.vt == VTYPE_PWSTR && ep.pwstrVal) {
                        desc = u16to8(reinterpret_cast<const wchar_t*>(ep.pwstrVal), ep.wstrLen);
                        free(ep.pwstrVal);
                    }
                    std::printf("  bpo GetLastError: code=%ld desc=%s\n", code, desc.c_str());
                    CHECK(code == 3, "L3-bpo: ПолучитьОшибку код UNSUPPORTED == 3");
                    CHECK(desc.find("не підтримується") != std::string::npos,
                          "L3-bpo: опис помилки каже, що операція не підтримується");
                }
            }

            // Асинхронна трійця ПОВЕРХ БПО: штатний обробник її не кличе, її бере
            // розширення з точки розширення РМК, щоб показати живий статус.
            {
                const wchar_t* asyncNames[] = { L"StartPurchase", L"StartRefund",
                                                L"OperationState", L"OperationResult",
                                                L"LastStatus", L"CancelOperation" };
                bool allFound = true;
                for (const wchar_t* n : asyncNames)
                    if (bpo->FindMethod(n) < 0) { allFound = false; std::printf("  немає: %ls\n", n); }
                CHECK(allFound, "L3-bpo: асинхронна трійця зареєстрована на фасаді");

                long idxState = bpo->FindMethod(L"OperationState");
                if (idxState >= 0) {
                    tVariant ret; tVarInit(&ret);
                    bpo->CallAsFunc(idxState, &ret, nullptr, 0);
                    long st = (ret.vt == VTYPE_I4) ? ret.lVal : (long)ret.dblVal;
                    CHECK(st == 0, "L3-bpo: СостояниеОперации == 0 (Idle) без активної операції");
                }
            }

            long idxBpoDisc = bpo->FindMethod(L"Disconnect");
            if (idxBpoDisc >= 0 && !deviceId.empty()) {
                bool rb = false; std::string rs; bool gotStr = false;
                callFunc(bpo, idxBpoDisc, { deviceId }, rb, rs, gotStr);
                CHECK(rb, "L3-bpo: Отключить → true");
            }
            pDestroyObject(&bpo);
        }
    }

    // ================= 8) БПО-фасад ревізії 4000 =================
    // Інше сімейство сигнатур: попереду з'являються НомерМерчанта і РеквизитыКартыQR,
    // тож хвіст зсунутий. Перевіряємо саме РОЗКЛАДКУ — помилка на позицію тут була б
    // тихою: жодного винятку, просто не ті дані в чеку. Контракт — bpo-contract.md §2.5.
    {
        IComponentBase* bpo = nullptr;
        pGetClassObject(L"ECRPrivatBPO4000", &bpo);
        CHECK(bpo != nullptr, "L3-bpo4000: компонента ECRPrivatBPO4000 створена через DLL");

        if (bpo) {
            HostConnect bconn;
            HostMemoryManager bmem;
            bpo->Init((void*)&bconn);
            bpo->setMemManager((void*)&bmem);

            long idxRev = bpo->FindMethod(L"GetInterfaceRevision");
            if (idxRev >= 0) {
                tVariant ret; tVarInit(&ret);
                bpo->CallAsFunc(idxRev, &ret, nullptr, 0);
                long rev = (ret.vt == VTYPE_I4) ? ret.lVal : (long)ret.dblVal;
                CHECK(rev == 4000, "L3-bpo4000: ревізія інтерфейсу == 4000");
            }

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

            long idxSetParam = bpo->FindMethod(L"SetParameter");
            std::string deviceId;
            if (idxSetParam >= 0) {
                bool rb = false; std::string rs; bool gs = false;
                callFunc(bpo, idxSetParam, { "EquipmentType", "ЭквайринговыйТерминал" }, rb, rs, gs);
                callFunc(bpo, idxSetParam, { "TransportKind", "tcp" }, rb, rs, gs);
                callFunc(bpo, idxSetParam, { "Host", "127.0.0.1" }, rb, rs, gs);
                callFunc(bpo, idxSetParam, { "Port", std::to_string(emu.Port()) }, rb, rs, gs);
            }
            long idxConn = bpo->FindMethod(L"Connect");
            if (idxConn >= 0) {
                tVariant p; tVarInit(&p);
                tVariant ret; tVarInit(&ret);
                bpo->CallAsFunc(idxConn, &ret, &p, 1);
                if (p.vt == VTYPE_PWSTR && p.pwstrVal) {
                    deviceId = u16to8(reinterpret_cast<const wchar_t*>(p.pwstrVal), p.wstrLen);
                    free(p.pwstrVal);
                }
                CHECK(ret.vt == VTYPE_BOOL && ret.bVal, "L3-bpo4000: Подключить → true");
            }

            // Дев'ятка оплати: хвіст із позиції 4, а не 1 як на 3004.
            long idxPay = bpo->FindMethod(L"PayByPaymentCard");
            CHECK(idxPay >= 0, "L3-bpo4000: ОплатитьПлатежнойКартой знайдено");
            if (idxPay >= 0 && !deviceId.empty()) {
                tVariant p[9];
                for (auto& v : p) tVarInit(&v);
                setInStr(p[0], u8to16(deviceId));
                p[3].vt = VTYPE_R8; p[3].dblVal = 100.50;

                tVariant ret; tVarInit(&ret);
                bpo->CallAsFunc(idxPay, &ret, p, 9);
                CHECK(ret.vt == VTYPE_BOOL && ret.bVal, "L3-bpo4000: ОплатитьПлатежнойКартой → true");
                CHECK(outStr(p[4]) == "444455**1234", "L3-bpo4000: НомерКарты на позиції 4");
                CHECK(outStr(p[5]) == "77",           "L3-bpo4000: НомерЧека на позиції 5");
                CHECK(outStr(p[6]) == "555000111",    "L3-bpo4000: СсылочныйНомер на позиції 6");
                CHECK(outStr(p[7]) == "A12345",       "L3-bpo4000: КодАвторизации на позиції 7");
                CHECK(!outStr(p[8]).empty(),          "L3-bpo4000: ТекстСлипЧека на позиції 8");
                CHECK(p[3].vt == VTYPE_R8 && p[3].dblVal > 100.49 && p[3].dblVal < 100.51,
                      "L3-bpo4000: СуммаОперации на позиції 3, дріб уцілів");
                for (auto& v : p)
                    if (v.vt == VTYPE_PWSTR && v.pwstrVal) free(v.pwstrVal);
            }

            // Скасування — десятка. ГОЛОВНЕ: позиція 4 (СуммаОригинальнойОперации)
            // конфігурацією назад НЕ читається, тож фасад не має в неї писати.
            long idxVoid = bpo->FindMethod(L"CancelPaymentByPaymentCard");
            CHECK(idxVoid >= 0, "L3-bpo4000: ОтменитьПлатежПоПлатежнойКарте знайдено");
            if (idxVoid >= 0 && !deviceId.empty()) {
                tVariant p[10];
                for (auto& v : p) tVarInit(&v);
                setInStr(p[0], u8to16(deviceId));
                p[3].vt = VTYPE_R8; p[3].dblVal = 100.50;
                p[4].vt = VTYPE_R8; p[4].dblVal = 0.0;      // повне скасування
                setInStr(p[7], u8to16("555000111"));        // СсылочныйНомер

                tVariant ret; tVarInit(&ret);
                bpo->CallAsFunc(idxVoid, &ret, p, 10);
                CHECK(ret.vt == VTYPE_BOOL && ret.bVal,
                      "L3-bpo4000: скасування виконано (поверненням за RRN)");
                CHECK(p[4].vt == VTYPE_R8 && p[4].dblVal == 0.0,
                      "L3-bpo4000: СуммаОригинальнойОперации НЕ перезаписана (IN-only)");
                CHECK(outStr(p[5]) == "444455**1234", "L3-bpo4000: НомерКарты на позиції 5");
                CHECK(outStr(p[6]) == "78",           "L3-bpo4000: НомерЧека на позиції 6");
                CHECK(outStr(p[8]) == "B67890",       "L3-bpo4000: КодАвторизации на позиції 8");
                CHECK(!outStr(p[9]).empty(),          "L3-bpo4000: ТекстСлипЧека на позиції 9");
                for (auto& v : p)
                    if (v.vt == VTYPE_PWSTR && v.pwstrVal) free(v.pwstrVal);
            }

            // Часткове скасування не вміємо: ненульова СуммаОригинальнойОперации
            // має дати явну відмову, а не скасування на іншу суму.
            if (idxVoid >= 0 && !deviceId.empty()) {
                tVariant p[10];
                for (auto& v : p) tVarInit(&v);
                setInStr(p[0], u8to16(deviceId));
                p[3].vt = VTYPE_R8; p[3].dblVal = 50.0;
                p[4].vt = VTYPE_R8; p[4].dblVal = 100.50;   // часткове
                setInStr(p[7], u8to16("555000111"));

                tVariant ret; tVarInit(&ret);
                bpo->CallAsFunc(idxVoid, &ret, p, 10);
                CHECK(ret.vt == VTYPE_BOOL && !ret.bVal,
                      "L3-bpo4000: часткове скасування явно відхилено");
                for (auto& v : p)
                    if (v.vt == VTYPE_PWSTR && v.pwstrVal) free(v.pwstrVal);
            }

            // Методи, яких термінал не вміє, зареєстровані й чесно відмовляють.
            long idxCash = bpo->FindMethod(L"PayByPaymentCardWithCashWithdrawal");
            CHECK(idxCash >= 0, "L3-bpo4000: ОплатитьПлатежнойКартойCВыдачейНаличных зареєстровано");
            if (idxCash >= 0 && !deviceId.empty()) {
                tVariant p[10];
                for (auto& v : p) tVarInit(&v);
                setInStr(p[0], u8to16(deviceId));
                p[3].vt = VTYPE_R8; p[3].dblVal = 100.0;
                p[4].vt = VTYPE_R8; p[4].dblVal = 50.0;
                tVariant ret; tVarInit(&ret);
                bpo->CallAsFunc(idxCash, &ret, p, 10);
                CHECK(ret.vt == VTYPE_BOOL && !ret.bVal,
                      "L3-bpo4000: видача готівки чесно відмовила");
                for (auto& v : p)
                    if (v.vt == VTYPE_PWSTR && v.pwstrVal) free(v.pwstrVal);
            }

            long idxList = bpo->FindMethod(L"GetCardTransactions");
            CHECK(idxList >= 0, "L3-bpo4000: ПолучитьОперацииПоКартам зареєстровано");

            long idxDisc = bpo->FindMethod(L"Disconnect");
            if (idxDisc >= 0 && !deviceId.empty()) {
                bool rb = false; std::string rs; bool gs = false;
                callFunc(bpo, idxDisc, { deviceId }, rb, rs, gs);
                CHECK(rb, "L3-bpo4000: Отключить → true");
            }
            pDestroyObject(&bpo);
        }
    }

    // 9) Прибирання: знищити об'єкт компоненти й вивантажити DLL.
    pDestroyObject(&comp);
    FreeLibrary(h);
    emu.Stop();

    std::printf(g_failed ? "\nFAILED: %d\n" : "\nOK\n", g_failed);
    return g_failed ? 1 : 0;
}

#endif // _WIN32
