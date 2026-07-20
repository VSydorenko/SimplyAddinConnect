// core_selftest — L1-харнес ядра AddInNative без 1С і без UAPKI.
// Лінкує OBJECT-бібліотеки ядра напряму; емулює платформу моками.
// УВАГА: pch.h тут НЕ підключається (правило tests/).
#include "../src/core/AddInNative.h"
#include "../src/helpers/ServiceTools.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

// ---- Дедлок ShutdownLogging: до фіксу зависав назавжди, після — миттєво ----
static void TestShutdownLoggingNoDeadlock() {
    // Ініціалізуємо логер у %TEMP% і одразу гасимо: до фіксу тут висло б назавжди
    const char* tempDir = std::getenv("TEMP");
    std::string path = std::string(tempDir ? tempDir : ".") + "\\core_selftest_dl.log";
    ServiceTools::InitLogging("DeadlockProbe", ServiceTools::LogLevel::Info, path);
    ServiceTools::ShutdownLogging("DeadlockProbe");
    CHECK(true, "ShutdownLogging does not deadlock");
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

int main() {
    // Небуферизований stdout: щоб при аварійному завершенні (AV) не втратити
    // останні рядки й точно локалізувати місце падіння.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== core_selftest ===\n");
    TestSmokeLifecycle();
    TestBootFixes();
    TestRetConvention();
    TestComponentRegistry();
    TestRegisterComponentMacro();
    TestShutdownLoggingNoDeadlock();
    TestBaseEnableLogging();
    std::printf("=== %s (failed: %d) ===\n", g_failed ? "FAIL" : "OK", g_failed);
    return g_failed ? 1 : 0;
}
