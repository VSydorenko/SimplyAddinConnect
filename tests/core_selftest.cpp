// core_selftest — L1-харнес ядра AddInNative без 1С і без UAPKI.
// Лінкує OBJECT-бібліотеки ядра напряму; емулює платформу моками.
// УВАГА: pch.h тут НЕ підключається (правило tests/).
#include "../src/core/AddInNative.h"
#include "../src/helpers/ServiceTools.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

static int g_failed = 0;
#define CHECK(cond, name) do { \
    if (cond) { std::printf("[PASS] %s\n", name); } \
    else { std::printf("[FAIL] %s\n", name); ++g_failed; } \
} while (0)

// ---- Мок платформи 1С ----
struct MockConnect : public IAddInDefBase {
    std::vector<std::u16string> errors;      // тексти AddError
    std::vector<std::u16string> events;      // "source|message|data" з ExternalEvent
    bool ADDIN_API AddError(unsigned short, const WCHAR_T*,
                            const WCHAR_T* descr, long) override {
        errors.push_back(reinterpret_cast<const char16_t*>(descr));
        return true;
    }
    bool ADDIN_API Read(WCHAR_T*, tVariant*, long*, WCHAR_T**) override { return false; }
    bool ADDIN_API Write(WCHAR_T*, tVariant*) override { return false; }
    bool ADDIN_API RegisterProfileAs(WCHAR_T*) override { return true; }
    bool ADDIN_API SetEventBufferDepth(long) override { return true; }
    long ADDIN_API GetEventBufferDepth() override { return 100; }
    bool ADDIN_API ExternalEvent(WCHAR_T* src, WCHAR_T* msg, WCHAR_T* data) override {
        std::u16string s = reinterpret_cast<char16_t*>(src);
        s += u"|"; s += reinterpret_cast<char16_t*>(msg);
        s += u"|"; s += reinterpret_cast<char16_t*>(data);
        events.push_back(s);
        return true;
    }
    void ADDIN_API CleanEventBuffer() override {}
    bool ADDIN_API SetStatusLine(WCHAR_T*) override { return true; }
    void ADDIN_API ResetStatusLine() override {}
};

struct MockMemory : public IMemoryManager {
    bool ADDIN_API AllocMemory(void** p, unsigned long n) override {
        *p = std::malloc(n); return *p != nullptr;
    }
    void ADDIN_API FreeMemory(void** p) override {
        std::free(*p); *p = nullptr;
    }
};

// ---- Смоук: реєстрація компоненти, життєвий цикл, властивість Version ----
static void TestSmokeLifecycle() {
    // Пробна компонента реєструється прямо в тесті через публічний AddComponent
    struct CoreProbe : public AddInNative {};
    AddInNative::AddComponent(u"CoreProbe", []() -> AddInNative* { return new CoreProbe; });

    AddInNative* comp = AddInNative::CreateObject(u"CoreProbe");
    CHECK(comp != nullptr, "CreateObject(CoreProbe)");
    if (!comp) return;

    MockConnect connect; MockMemory memory;
    CHECK(comp->Init(&connect), "Init");
    CHECK(comp->setMemManager(&memory), "setMemManager");

    long propNum = comp->FindProp((WCHAR_T*)u"Version");
    CHECK(propNum >= 0, "FindProp(Version)");

    tVariant val{}; tVarInit(&val);
    CHECK(comp->GetPropVal(propNum, &val), "GetPropVal(Version)");
    CHECK(val.vt == VTYPE_PWSTR && val.wstrLen > 0, "Version is non-empty string");

    comp->Done();
    delete comp;
}

// ---- Реєстр компонент + макрос REGISTER_COMPONENT ----
// MacroProbe реєструється макросом на файловому рівні (компайл-гейт макросу).
struct MacroProbe : public AddInNative { static std::vector<std::u16string> names; };
REGISTER_COMPONENT(u"MacroProbe", MacroProbe)

static void TestComponentRegistry() {
    std::u16string names = AddInNative::getComponentNames();
    // core_selftest лінкує лише base+helpers, тому в реєстрі — проби (CoreProbe/RetProbe),
    // а не ECR/Test. Перевіряємо наявність зареєстрованої проби.
    CHECK(names.find(u"CoreProbe") != std::u16string::npos,
          "registry contains CoreProbe");
}

static void TestRegisterComponentMacro() {
    CHECK(AddInNative::CreateObject(u"MacroProbe") != nullptr, "REGISTER_COMPONENT works");
}

// Експорти оголошені в ComponentBase.h (extern "C"); для прямого виклику з тесту:
extern "C" long GetClassObject(const WCHAR_T* wsName, IComponentBase** pInterface);
extern "C" long DestroyObject(IComponentBase** pInterface);

// ---- Boot-фікси: контракт 1/0 у GetClassObject і лінива локаль upper() ----
static void TestBootFixes() {
    // 1) GetClassObject не залежить від молодших біт адреси: контракт — 1/0
    IComponentBase* iface = nullptr;
    long rc = GetClassObject((const WCHAR_T*)u"CoreProbe", &iface);
    CHECK(rc == 1 && iface != nullptr, "GetClassObject returns 1 on success");
    long rc2 = GetClassObject((const WCHAR_T*)u"CoreProbe", &iface); // *pInterface != null
    CHECK(rc2 == 0, "GetClassObject refuses non-null pInterface");
    CHECK(DestroyObject(&iface) == 0 && iface == nullptr, "DestroyObject");

    IComponentBase* none = nullptr;
    CHECK(GetClassObject((const WCHAR_T*)u"NoSuchComponent", &none) == 0,
          "GetClassObject returns 0 for unknown name");

    // 2) upper() не падає навіть якщо ru_RU.UTF-8 недоступна (fallback), ASCII працює
    std::u16string s = u"abcXYZ123";
    CHECK(AddInNative::upper(s) == u"ABCXYZ123", "upper() ASCII");
}

// ---- Конвенція повернення значень: Ret() ----
static void TestRetConvention() {
    struct RetProbe : public AddInNative {
        RetProbe() {
            AddFunction(u"EchoBool",   u"ЭхоБул",    Ret([](VH v) { return (bool)v; }));
            AddFunction(u"EchoString", u"ЭхоСтрока", Ret([]() { return std::string("hello"); }));
            AddFunction(u"EchoInt",    u"ЭхоЧисло",  Ret([]() { return 42; }));
        }
    };
    AddInNative::AddComponent(u"RetProbe", []() -> AddInNative* { return new RetProbe; });
    AddInNative* comp = AddInNative::CreateObject(u"RetProbe");
    MockConnect connect; MockMemory memory;
    comp->Init(&connect); comp->setMemManager(&memory);

    // EchoString: без аргументів, повертає "hello"
    long m = comp->FindMethod((WCHAR_T*)u"EchoString");
    CHECK(m >= 0, "FindMethod(EchoString)");
    tVariant ret{}; std::memset(&ret, 0, sizeof(ret)); ret.vt = VTYPE_EMPTY;
    CHECK(comp->CallAsFunc(m, &ret, nullptr, 0), "CallAsFunc(EchoString)");
    CHECK(ret.vt == VTYPE_PWSTR && ret.wstrLen == 5, "EchoString returned 'hello'");

    // EchoBool(true)
    long mb = comp->FindMethod((WCHAR_T*)u"EchoBool");
    tVariant arg{}; std::memset(&arg, 0, sizeof(arg));
    arg.vt = VTYPE_BOOL; arg.bVal = true;
    tVariant rb{}; std::memset(&rb, 0, sizeof(rb)); rb.vt = VTYPE_EMPTY;
    CHECK(comp->CallAsFunc(mb, &rb, &arg, 1), "CallAsFunc(EchoBool)");
    CHECK(rb.vt == VTYPE_BOOL && rb.bVal == true, "EchoBool returned true");

    // EchoInt → int64
    long mi = comp->FindMethod((WCHAR_T*)u"EchoInt");
    tVariant ri{}; std::memset(&ri, 0, sizeof(ri)); ri.vt = VTYPE_EMPTY;
    CHECK(comp->CallAsFunc(mi, &ri, nullptr, 0), "CallAsFunc(EchoInt)");
    CHECK((ri.vt == VTYPE_I8 || ri.vt == VTYPE_I4) , "EchoInt returned integer");

    comp->Done(); delete comp;
}

// ---- Ret() через CallAsProc: функція, викликана як процедура (результат
// відкидається). CallAsProc відʼєднує result.pvar; до фіксу operator= кидав тут
// bad_variant_access, і CallAsProc повертав false попри виконану дію. ----
static bool g_procSideEffect = false;
static void TestRetViaCallAsProc() {
    struct ProcProbe : public AddInNative {
        ProcProbe() {
            AddFunction(u"DoIt", u"Сделать",
                Ret([]() { g_procSideEffect = true; return true; }));
        }
    };
    AddInNative::AddComponent(u"ProcProbe", []() -> AddInNative* { return new ProcProbe; });
    AddInNative* comp = AddInNative::CreateObject(u"ProcProbe");
    MockConnect connect; MockMemory memory;
    comp->Init(&connect); comp->setMemManager(&memory);
    long m = comp->FindMethod((WCHAR_T*)u"DoIt");
    CHECK(m >= 0, "FindMethod(DoIt)");
    g_procSideEffect = false;
    // Виклик як ПРОЦЕДУРА: 1С відкидає результат -> платформа кличе CallAsProc.
    CHECK(comp->CallAsProc(m, nullptr, 0), "Ret() via CallAsProc returns true (no spurious throw)");
    CHECK(g_procSideEffect, "Ret() handler side effect ran under CallAsProc");
    CHECK(connect.errors.empty(), "no spurious AddError from CallAsProc");
    comp->Done(); delete comp;
}

// ---- Широкі арності (8..16) + IN/OUT-параметр ----
// Контракт БПО «Подключаемое оборудование» має методи на 9-10 параметрів
// (ОплатитьПлатежнойКартой — 9, ОтменитьПлатежПоПлатежнойКарте — 10). До розширення
// MethFunction такий метод не реєструвався ВЗАГАЛІ: CallMethod не мав гілки, а
// GetNParams віддавав 0 — 1С вважала метод безпараметровим. Перевіряємо і кількість,
// і ПОРЯДОК аргументів (щоб розгортання index_sequence не переплутало індекси).
static std::string g_wide9;
static std::string g_wide16;
static void TestWideArity() {
    struct WideProbe : public AddInNative {
        WideProbe() {
            AddFunction(u"Wide9", u"Широкий9",
                Ret([](VH a, VH b, VH c, VH d, VH e, VH f, VH g, VH h, VH i) {
                    g_wide9 = (std::string)a + (std::string)b + (std::string)c
                            + (std::string)d + (std::string)e + (std::string)f
                            + (std::string)g + (std::string)h + (std::string)i;
                    return true;
                }));
            // Верхня межа списку альтернатив: остання арність мусить дожити до виклику.
            AddFunction(u"Wide16", u"Широкий16",
                Ret([](VH a, VH, VH, VH, VH, VH, VH, VH,
                       VH, VH, VH, VH, VH, VH, VH, VH p16) {
                    g_wide16 = (std::string)a + (std::string)p16;
                    return true;
                }));
            // IN/OUT: параметр приходить ЗАПОВНЕНИМ, хендлер читає і перезаписує.
            // Саме цей шлях вмикає clear() -> FreeMemory на чужому рядку; у продуктових
            // фасадах (LabelPrinter) OUT-параметри завжди приходять порожніми, тож до
            // БПО-еквайрингу гілка не виконувалась жодного разу.
            AddProcedure(u"InOut", u"ВходВыход",
                MethFunction(std::function<void(VH)>([](VH v) {
                    std::string in = v;
                    v = std::string("<") + in + ">";
                })));
        }
    };
    AddInNative::AddComponent(u"WideProbe", []() -> AddInNative* { return new WideProbe; });
    AddInNative* comp = AddInNative::CreateObject(u"WideProbe");
    MockConnect connect; MockMemory memory;
    comp->Init(&connect); comp->setMemManager(&memory);

    // --- 9 параметрів ---
    long m9 = comp->FindMethod((WCHAR_T*)u"Wide9");
    CHECK(m9 >= 0, "FindMethod(Wide9)");
    CHECK(comp->GetNParams(m9) == 9, "GetNParams(Wide9) == 9");

    tVariant args9[9]{};
    std::u16string src9[9];
    for (int k = 0; k < 9; ++k) {
        std::memset(&args9[k], 0, sizeof(tVariant));
        src9[k] = std::u16string(1, static_cast<char16_t>(u'1' + k));
        args9[k].vt = VTYPE_PWSTR;
        args9[k].pwstrVal = reinterpret_cast<WCHAR_T*>(const_cast<char16_t*>(src9[k].c_str()));
        args9[k].wstrLen = 1;
    }
    g_wide9.clear();
    tVariant r9{}; std::memset(&r9, 0, sizeof(r9)); r9.vt = VTYPE_EMPTY;
    CHECK(comp->CallAsFunc(m9, &r9, args9, 9), "CallAsFunc(Wide9) dispatches");
    CHECK(g_wide9 == "123456789", "Wide9 got all 9 params IN ORDER");

    // --- 16 параметрів (верхня межа) ---
    long m16 = comp->FindMethod((WCHAR_T*)u"Wide16");
    CHECK(comp->GetNParams(m16) == 16, "GetNParams(Wide16) == 16");
    tVariant args16[16]{};
    std::u16string first = u"A", last = u"Z";
    for (int k = 0; k < 16; ++k) {
        std::memset(&args16[k], 0, sizeof(tVariant));
        args16[k].vt = VTYPE_PWSTR;
        const std::u16string& s = (k == 0) ? first : last;
        args16[k].pwstrVal = reinterpret_cast<WCHAR_T*>(const_cast<char16_t*>(s.c_str()));
        args16[k].wstrLen = 1;
    }
    g_wide16.clear();
    tVariant r16{}; std::memset(&r16, 0, sizeof(r16)); r16.vt = VTYPE_EMPTY;
    CHECK(comp->CallAsFunc(m16, &r16, args16, 16), "CallAsFunc(Wide16) dispatches");
    CHECK(g_wide16 == "AZ", "Wide16 got first and last param");

    // --- Регресія: старі арності не поїхали ---
    CHECK(comp->GetNParams(comp->FindMethod((WCHAR_T*)u"InOut")) == 1, "GetNParams(InOut) == 1");

    // --- IN/OUT: читання вхідного значення + перезапис ---
    // Вхідний рядок виділяємо ЧЕРЕЗ той самий менеджер пам'яті, що й платформа,
    // бо clear() віддасть його у FreeMemory — інакше перевірятимемо не той шлях.
    tVariant io{}; std::memset(&io, 0, sizeof(io));
    const std::u16string seed = u"IN";
    void* mem = nullptr;
    memory.AllocMemory(&mem, static_cast<unsigned long>((seed.size() + 1) * sizeof(char16_t)));
    std::memcpy(mem, seed.c_str(), (seed.size() + 1) * sizeof(char16_t));
    io.vt = VTYPE_PWSTR;
    io.pwstrVal = static_cast<WCHAR_T*>(mem);
    io.wstrLen = static_cast<uint32_t>(seed.size());

    long mio = comp->FindMethod((WCHAR_T*)u"InOut");
    CHECK(comp->CallAsProc(mio, &io, 1), "CallAsProc(InOut)");
    CHECK(io.vt == VTYPE_PWSTR && io.wstrLen == 4, "InOut rewrote param in place");
    CHECK(std::u16string(reinterpret_cast<char16_t*>(io.pwstrVal), io.wstrLen) == u"<IN>",
          "InOut read incoming value and wrote back");
    memory.FreeMemory(reinterpret_cast<void**>(&io.pwstrVal));

    comp->Done(); delete comp;
}

// ---- Дедлок ShutdownLogging: до фіксу зависав назавжди, після — миттєво ----
static void TestShutdownLoggingNoDeadlock() {
    const char* tempDir = std::getenv("TEMP");
    std::string path = std::string(tempDir ? tempDir : ".") + "\\core_selftest_dl.log";
    // Watchdog: якщо ShutdownLogging самозаблокується (регресія дедлоку), процес
    // не має зависати назавжди — фіксуємо FAIL і аварійно виходимо через 5 с.
    // (без цього регресія дедлоку повісила б увесь run_tests без жодного [FAIL]).
    auto fut = std::async(std::launch::async, [&] {
        ServiceTools::InitLogging("DeadlockProbe", ServiceTools::LogLevel::Info, path);
        ServiceTools::ShutdownLogging("DeadlockProbe");
    });
    if (fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        fut.get();
        CHECK(true, "ShutdownLogging does not deadlock");
    } else {
        CHECK(false, "ShutdownLogging DEADLOCK (timeout 5s)");
        std::fflush(stdout);
        std::_Exit(2);
    }
}

// ---- Повторний InitLogging після ShutdownLogging: spdlog::drop у реєстрі ----
// До фіксу 2-й init того ж імені кидав spdlog_ex "already exists" -> false,
// і файлове логування ламалось для другої й наступних інстанцій типу в сесії 1С.
static void TestReInitLoggingAfterShutdown() {
    const char* tempDir = std::getenv("TEMP");
    std::string path = std::string(tempDir ? tempDir : ".") + "\\core_selftest_reinit.log";
    CHECK(ServiceTools::InitLogging("ReInitProbe", ServiceTools::LogLevel::Info, path),
          "InitLogging(ReInitProbe) first");
    ServiceTools::ShutdownLogging("ReInitProbe");
    CHECK(ServiceTools::InitLogging("ReInitProbe", ServiceTools::LogLevel::Info, path),
          "InitLogging(ReInitProbe) again after shutdown (spdlog::drop)");
    ServiceTools::ShutdownLogging("ReInitProbe");
}

// ---- Neutral-репорти (транспорти/драйвери) мають потрапляти у файловий лог ----
// До фіксу NEUTRAL_REPORT_* писали в логер "General", який ніде не реєструвався:
// уся діагностика Connect/TransportTCP ішла у fallback (OutputDebugString) і НЕ
// досягала файлу, увімкненого через ИспользоватьЛогирование, — з 1С неможливо
// було зрозуміти, чому Подключить() повернув Ложь (інцидент 2026-08-29).
static void TestNeutralReportReachesFile() {
    const char* tempDir = std::getenv("TEMP");
    std::string path = std::string(tempDir ? tempDir : ".") + "\\core_selftest_neutral.log";
    std::remove(path.c_str());
    CHECK(ServiceTools::InitLogging("NeutralProbe", ServiceTools::LogLevel::Trace, path),
          "InitLogging(NeutralProbe)");
    NEUTRAL_REPORT_INFO("TransportTCP", "NEUTRAL-MARKER-7501");
    ServiceTools::ShutdownLogging("NeutralProbe");

    std::ifstream f(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    CHECK(content.find("NEUTRAL-MARKER-7501") != std::string::npos,
          "neutral report lands in component log file");

    // Після Shutdown спільний sink звільнено: повторний InitLogging того ж файлу
    // працює, а neutral-репорт без жодного активного логера не падає (fallback).
    NEUTRAL_REPORT_INFO("TransportTCP", "після shutdown — у fallback");
    CHECK(ServiceTools::InitLogging("NeutralProbe", ServiceTools::LogLevel::Info, path),
          "re-InitLogging after shutdown");
    ServiceTools::ShutdownLogging("NeutralProbe");
}

// ---- info+ скидається на диск ОДРАЗУ, без Shutdown (інцидент 2026-08-29 №2) ----
// Користувач копіював лог при живому процесі 1С: файл обривався посеред рядка, а
// записи Disconnect сиділи в буфері spdlog — «останній» лог брехав про стан.
// Вимога: рядок рівня info+ (Connect/Disconnect/помилки) видно у файлі одразу
// після повернення з логуючого виклику; trace/debug можуть лишатись у буфері.
static void TestInfoFlushedWithoutShutdown() {
    const char* tempDir = std::getenv("TEMP");
    std::string path = std::string(tempDir ? tempDir : ".") + "\\core_selftest_flush.log";
    std::remove(path.c_str());
    CHECK(ServiceTools::InitLogging("FlushProbe", ServiceTools::LogLevel::Trace, path),
          "InitLogging(FlushProbe)");
    NEUTRAL_REPORT_INFO("ECRPrivatJSON", "FLUSH-MARKER-7502");

    // Читаємо файл, поки логер ЖИВИЙ (без Shutdown/flush) — як користувач копіює лог.
    std::ifstream f(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    CHECK(content.find("FLUSH-MARKER-7502") != std::string::npos,
          "info-line hits disk immediately (no shutdown)");
    ServiceTools::ShutdownLogging("FlushProbe");
}

// ---- Спільний EnableLogging/ИспользоватьЛогирование у базі ----
static void TestBaseEnableLogging() {
    // CoreProbe зареєстрована в TestSmokeLifecycle і нічого сама не реєструє —
    // метод має бути в БАЗІ AddInNative.
    AddInNative* comp = AddInNative::CreateObject(u"CoreProbe");
    CHECK(comp != nullptr, "CreateObject(CoreProbe) for EnableLogging");
    if (!comp) return;
    MockConnect connect; MockMemory memory;
    comp->Init(&connect); comp->setMemManager(&memory);

    long m = comp->FindMethod((WCHAR_T*)u"EnableLogging");
    CHECK(m >= 0, "base registers EnableLogging");
    long mru = comp->FindMethod((WCHAR_T*)u"ИспользоватьЛогирование");
    CHECK(mru >= 0, "base registers ru alias");

    // Виклик з рівнем off не потребує файлу; перевіряємо структурний успіх і тип
    std::u16string off = u"off";
    std::u16string empty = u"";
    tVariant args[2]; std::memset(args, 0, sizeof(args));
    args[0].vt = VTYPE_PWSTR; args[0].pwstrVal = (WCHAR_T*)off.c_str();   args[0].wstrLen = 3;
    args[1].vt = VTYPE_PWSTR; args[1].pwstrVal = (WCHAR_T*)empty.c_str(); args[1].wstrLen = 0;
    tVariant ret{}; std::memset(&ret, 0, sizeof(ret)); ret.vt = VTYPE_EMPTY;
    CHECK(m >= 0 && comp->CallAsFunc(m, &ret, args, 2), "CallAsFunc(EnableLogging off)");
    CHECK(ret.vt == VTYPE_BOOL, "EnableLogging returns bool");

    comp->Done(); delete comp;
}

// ---- Декларативні параметри: ParamSpec + валідація required перед хендлером ----
static void TestParamValidation() {
    struct ParamProbe : public AddInNative {
        ParamProbe() {
            AddFunction(u"Pay", u"Оплатить",
                Ret([](VH amount, VH merchant) {
                    (void)merchant; return (double)amount > 0;
                }),
                std::vector<ParamSpec>{
                    { u"Amount", u"Сумма", /*required*/ true, std::nullopt },
                    { u"MerchantId", u"ИдМерчанта", false, DefaultHelper(u"0") },
                });
        }
    };
    AddInNative::AddComponent(u"ParamProbe", []() -> AddInNative* { return new ParamProbe; });
    AddInNative* comp = AddInNative::CreateObject(u"ParamProbe");
    MockConnect connect; MockMemory memory;
    comp->Init(&connect); comp->setMemManager(&memory);
    long m = comp->FindMethod((WCHAR_T*)u"Pay");
    CHECK(m >= 0, "FindMethod(Pay)");

    // 1) Порожній обов'язковий параметр → false + AddError з ім'ям параметра
    tVariant args[2]; std::memset(args, 0, sizeof(args));
    args[0].vt = VTYPE_EMPTY; args[1].vt = VTYPE_EMPTY;
    tVariant ret{}; std::memset(&ret, 0, sizeof(ret)); ret.vt = VTYPE_EMPTY;
    CHECK(!comp->CallAsFunc(m, &ret, args, 2), "empty required param rejected");
    bool msgOk = !connect.errors.empty() &&
        connect.errors.back().find(u"Amount") != std::u16string::npos;
    CHECK(msgOk, "AddError mentions param name");

    // 2) Валідний виклик проходить
    args[0].vt = VTYPE_R8; args[0].dblVal = 10.5;
    connect.errors.clear();
    CHECK(comp->CallAsFunc(m, &ret, args, 2), "valid call passes");
    CHECK(connect.errors.empty(), "no AddError on valid call");
    comp->Done(); delete comp;
}

// ---- Захист індексів методів/властивостей від негативних/завеликих значень ----
static void TestIndexHardening() {
    AddInNative* comp = AddInNative::CreateObject(u"CoreProbe");
    MockConnect connect; MockMemory memory;
    comp->Init(&connect); comp->setMemManager(&memory);
    tVariant ret{}; std::memset(&ret, 0, sizeof(ret)); ret.vt = VTYPE_EMPTY;
    CHECK(!comp->CallAsFunc(-1, &ret, nullptr, 0), "negative method index rejected");
    CHECK(!comp->CallAsFunc(9999, &ret, nullptr, 0), "out-of-range method index rejected");
    CHECK(!comp->GetPropVal(-1, &ret), "negative prop index rejected");
    comp->Done(); delete comp;
}

// ---- EventBridge: потокобезпечний PostExternalEvent з відсіканням після Done ----
static void TestEventBridge() {
    AddInNative* comp = AddInNative::CreateObject(u"CoreProbe");
    CHECK(comp != nullptr, "CreateObject(CoreProbe) for EventBridge");
    if (!comp) return;
    MockConnect connect; MockMemory memory;
    comp->Init(&connect); comp->setMemManager(&memory);

    // Постинг з фонового потоку доставляє подію в IAddInDefBase
    std::thread t([&] { comp->PostExternalEvent(u"OnData", u"payload123"); });
    t.join();
    CHECK(connect.events.size() == 1, "event delivered from background thread");
    CHECK(connect.events[0].find(u"OnData") != std::u16string::npos &&
          connect.events[0].find(u"payload123") != std::u16string::npos,
          "event carries message and data");

    // Після Done() постинг безпечний і повертає false, без доставки
    comp->Done();
    CHECK(!comp->PostExternalEvent(u"OnData", u"late"), "post after Done returns false");
    CHECK(connect.events.size() == 1, "no delivery after Done");
    // AddError після Done() теж безпечний (той самий м'ютекс + null-guard)
    CHECK(!comp->AddError(u"Тест після Done"), "AddError after Done returns false");
    delete comp;
}

// ---- Реальна гонка PostExternalEvent (фон) проти Done() (головний потік) ----
// Дає connectMutex_ змістовне покриття: без м'ютекса це data-race/UAF -> креш
// під стресом; assertion (пост після Done -> false, без креша) детермінований.
static void TestEventBridgeConcurrency() {
    bool ok = true;
    for (int i = 0; i < 40; ++i) {
        AddInNative* comp = AddInNative::CreateObject(u"CoreProbe");
        MockConnect connect; MockMemory memory;
        comp->Init(&connect); comp->setMemManager(&memory);
        std::atomic<bool> stop{ false };
        std::thread poster([&] {
            while (!stop.load(std::memory_order_relaxed))
                comp->PostExternalEvent(u"OnData", u"x");
        });
        std::this_thread::yield();
        comp->Done();                                   // гонка з poster
        stop.store(true, std::memory_order_relaxed);
        poster.join();
        if (comp->PostExternalEvent(u"OnData", u"late")) ok = false;  // після Done -> false
        delete comp;
    }
    CHECK(ok, "concurrent PostExternalEvent vs Done: no post after Done, no crash");
}

// ---- Fallback-sink логера: до EnableLogging репорти не падають і не вимагають файлу ----
static void TestFallbackLogging() {
    NEUTRAL_REPORT_WARN("CoreSelftest", "Перевірка fallback-логера до EnableLogging");
    CHECK(true, "fallback logging does not crash");
}

int main() {
    // Небуферизований stdout: щоб при аварійному завершенні (AV) не втратити
    // останні рядки й точно локалізувати місце падіння.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== core_selftest ===\n");
    TestSmokeLifecycle();
    TestBootFixes();
    TestRetConvention();
    TestRetViaCallAsProc();
    TestWideArity();
    TestComponentRegistry();
    TestRegisterComponentMacro();
    TestShutdownLoggingNoDeadlock();
    TestReInitLoggingAfterShutdown();
    TestNeutralReportReachesFile();
    TestInfoFlushedWithoutShutdown();
    TestBaseEnableLogging();
    TestParamValidation();
    TestIndexHardening();
    TestEventBridge();
    TestEventBridgeConcurrency();
    TestFallbackLogging();
    std::printf("=== %s (failed: %d) ===\n", g_failed ? "FAIL" : "OK", g_failed);
    return g_failed ? 1 : 0;
}
