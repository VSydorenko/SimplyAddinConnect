# Етап 0 «Ядро-міст 1С» — план імплементації

> **СТАТУС: ✅ ВИКОНАНО** (2026-07-20, гілка `etap0-core-mist`). Усі 9 задач реалізовані
> (TDD, per-task коміти) + 4 виправлення за адверсарним код-рев'ю: (1) `Ret()`-функція як
> процедура (`CallAsProc` обнуляє `result.pvar`) більше не кидає `bad_variant_access`
> (`operator=` тихо відкидає присвоєння у від'єднаний result); (2) `ShutdownLogging` робить
> `spdlog::drop` (повторний `EnableLogging` того ж типу компоненти в одній сесії 1С не падає);
> (3) watchdog у дедлок-тесті (регресія = FAIL, не хенг); (4) стрес-гонка EventBridge.
> Гейт зелений: `build_project.ps1 -WithUAPKI -WithTests` (x86+x64, ZIP), `run_tests.ps1`
> x64 (18 PASS) та x86 (17 PASS, 1 SKIP — 64-біт python не вантажить 32-біт провайдер);
> `core_selftest` — 47 CHECK. Правки ECR-фасаду свідомо відкладені на Етап 2.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Зміцнити ядро-міст 1С за специфікацією `docs/tasks/2026-07-19_platform_architecture_design.md` §3.0: тестовий харнес ядра без UAPKI, boot-фікси, конвенція повернення значень, `REGISTER_COMPONENT`, спільний `EnableLogging`, декларативні параметри з валідацією, потокобезпечний міст подій `ExternalEvent`, fallback-sink логера.

**Architecture:** Усі зміни — в `src/core/AddInNative.{h,cpp}`, `src/helpers/ServiceTools_Log.cpp`, компонентах-фасадах і CMake тестів. Новий тестовий exe `core_selftest` лінкує OBJECT-бібліотеки ядра напряму (без 1С, без UAPKI) і ганяє перевірки через мок `IAddInDefBase`/`IMemoryManager`.

**Tech Stack:** C++17, MSVC (VS2022), CMake ≥3.16, spdlog (поки лишається — заміна в Етапі 3), власний хенд-ролед тест-раннер (як `uapki_selftest`, без gtest).

## Global Constraints (з AGENTS.md і спеки — діють у КОЖНІЙ задачі)

- Кожен `.cpp` у `src/` починається з `#include "../core/pch.h"` (або `"pch.h"`/`"core/pch.h"` за фактичною відносною глибиною). **Тестові `.cpp` у `tests/` pch НЕ підключають.**
- Логування — лише макроси `REPORT_*` (нестатичні методи компонент) / `NEUTRAL_REPORT_*` (статичні/вільні, 1-й аргумент — ім'я компоненти). Прямий `spdlog` заборонено (виняток — сам `ServiceTools_Log.cpp`).
- Повідомлення: конкатенація рядків (`"Текст: " + var`), з великої літери, без крапки в кінці, без printf-стилю.
- Після `REPORT_ERROR` — зазвичай `return false`; винятки не перетинають межу 1С.
- Мова коду/комітів — українська/російська (дотримуйся мови файлу, який редагуєш).
- MSVC: вихідники UTF-8 → нові цілі компілюються з `/utf-8`.
- `version.h` перегенерується `build_project.ps1` — не редагувати руками.
- Збірка має лишатись зеленою для обох архітектур і з `-WithUAPKI`.
- **Обсяг Етапу 0 НЕ включає** правки фасаду `AddinECRPrivatJSON` (методи, повернення, приймання) — весь драйвер переписується в Етапі 2; чіпати його зараз — марна робота (там детермінований баг з'єднання, методи все одно непрацездатні).

**Робочий цикл збірки/тестів у задачах:**

```powershell
# одноразова конфігурація (з кореня репо)
cmake -S . -B build_x64 -A x64 -DBUILD_TESTS=ON
# збірка тест-цілі
cmake --build build_x64 --config Release --target core_selftest
# запуск (EXECUTABLE_OUTPUT_PATH кладе exe у bin/Release)
bin\Release\core_selftest_x64.exe
```

---

### Task 1: Тестовий харнес ядра `core_selftest` (без UAPKI)

**Files:**
- Modify: `tests/CMakeLists.txt` (зняти жорсткий гейт `BUILD_WITH_UAPKI` для нової цілі; рядки 8-14)
- Create: `tests/core_selftest.cpp`
- Modify: `run_tests.ps1` — НЕ чіпати в цій задачі (інтеграція — Task 9)

**Interfaces:**
- Consumes: OBJECT-цілі `base_component`, `helpers_component` (`CMake/components.cmake:93,111`), `spdlog::spdlog`.
- Produces: виконуваний `core_selftest_x64.exe` (exit 0 = усі перевірки пройшли); мок-хост `MockConnect`/`MockMemory` і макрос `CHECK(...)` — їх використовують УСІ наступні задачі, додаючи тест-функції в цей самий файл.

- [X] **Step 1: Написати падаючий тест (каркас + смоук)**

Створити `tests/core_selftest.cpp`:

```cpp
// core_selftest — L1-харнес ядра AddInNative без 1С і без UAPKI.
// Лінкує OBJECT-бібліотеки ядра напряму; емулює платформу моками.
// УВАГА: pch.h тут НЕ підключається (правило tests/).
#include "../src/core/AddInNative.h"
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

int main() {
    std::printf("=== core_selftest ===\n");
    TestSmokeLifecycle();
    std::printf("=== %s (failed: %d) ===\n", g_failed ? "FAIL" : "OK", g_failed);
    return g_failed ? 1 : 0;
}
```

Примітка: якщо `tVarInit` недоступний з `types.h` без додаткових include — замінити на `std::memset(&val, 0, sizeof(val)); val.vt = VTYPE_EMPTY;`.

- [X] **Step 2: Переконатися, що збірка падає (цілі ще немає)**

Run: `cmake --build build_x64 --config Release --target core_selftest`
Expected: FAIL — `core_selftest` невідома ціль (у `tests/CMakeLists.txt` її ще немає, а сам каталог відсікається гейтом).

- [X] **Step 3: Додати ціль у tests/CMakeLists.txt**

Замінити гейт (рядки 8-14) і додати ціль ПЕРЕД UAPKI-блоком:

```cmake
# Каталог тестів доступний на Windows завжди; окремі цілі мають власні умови.
if(NOT WIN32)
    message(STATUS "[tests] пропущено (потрібен WIN32)")
    return()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_TEST_ARCH_SUFFIX "_x64")
else()
    set(_TEST_ARCH_SUFFIX "_x86")
endif()

# ---------------------------------------------------------------------------
# core_selftest — L1-харнес ядра AddInNative (без 1С, без UAPKI).
# Лінкує OBJECT-бібліотеки ядра напряму; платформа 1С емулюється моками.
# ---------------------------------------------------------------------------
add_executable(core_selftest core_selftest.cpp
    $<TARGET_OBJECTS:base_component>
    $<TARGET_OBJECTS:helpers_component>
)
set_target_properties(core_selftest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "core_selftest${_TEST_ARCH_SUFFIX}"
)
target_include_directories(core_selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${SPDLOG_INCLUDE_DIR}
)
# _WINDOWS — критично: types.h саме за ним вмикає WCHAR_T=wchar_t і
# ADDIN_API=__stdcall; об'єкти ядра зібрані з _WINDOWS — ABI мусить збігатися.
target_compile_definitions(core_selftest PRIVATE _WINDOWS UNICODE _UNICODE)
target_link_libraries(core_selftest PRIVATE spdlog::spdlog)
if(MSVC)
    target_compile_options(core_selftest PRIVATE /utf-8)
endif()
```

Далі — наявний UAPKI-блок обгорнути умовою (щоб решта файлу лишилась як є):

```cmake
if(NOT BUILD_WITH_UAPKI)
    message(STATUS "[tests] uapki_selftest/native_host пропущено (потрібен BUILD_WITH_UAPKI)")
    return()
endif()
# ... далі незмінні uapki_selftest та native_host ...
```

Якщо `helpers_component` не збирається без глобальних include-шляхів у цьому контексті — конфігурація сама покаже; шляхи вже задаються глобально (компоненти в `components.cmake` збираються з `include`/`src`/`${SPDLOG_INCLUDE_DIR}`).

- [X] **Step 4: Зібрати і запустити — тест зелений**

Run:
```powershell
cmake -S . -B build_x64 -A x64 -DBUILD_TESTS=ON
cmake --build build_x64 --config Release --target core_selftest
bin\Release\core_selftest_x64.exe
```
Expected: усі `[PASS]`, exit code 0. Якщо лінкер скаржиться на відсутні символи ServiceTools (їх тягне `AddError`-шлях) — перевірити, що `$<TARGET_OBJECTS:helpers_component>` присутній у add_executable.

- [X] **Step 5: Commit**

```powershell
git add tests/CMakeLists.txt tests/core_selftest.cpp
git commit -m "Тести: core_selftest — L1-харнес ядра AddInNative без UAPKI (мок IAddInDefBase/IMemoryManager)"
```

---

### Task 2: Boot-фікси — локаль і GetClassObject

**Files:**
- Modify: `src/core/AddInNative.cpp:473-485` (глобальна локаль), `src/core/AddInNative.cpp:53-58` (`GetClassObject`)
- Test: `tests/core_selftest.cpp` (додати тест-функції)

**Interfaces:**
- Consumes: харнес Task 1.
- Produces: `GetClassObject` повертає `1` при успіху (не адресу); внутрішня `RuLocale()` — статична функція, зовнішнього API не змінює.

- [X] **Step 1: Написати падаючі тести**

У `tests/core_selftest.cpp` додати (і викликати з `main` після смоуку):

```cpp
// Експорти оголошені в ComponentBase.h/сборці; для прямого виклику з тесту:
extern "C" long GetClassObject(const WCHAR_T* wsName, IComponentBase** pInterface);
extern "C" long DestroyObject(IComponentBase** pInterface);

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
```

- [X] **Step 2: Запустити — переконатися, що падає**

Run: збірка + запуск (цикл із шапки).
Expected: `[FAIL] GetClassObject returns 1 on success` (зараз повертається `long(вказівник)` — на x64 усічений; значення не дорівнює 1).

- [X] **Step 3: Мінімальна реалізація**

`src/core/AddInNative.cpp:53-58` — замінити тіло:

```cpp
long GetClassObject(const WCHAR_T* wsName, IComponentBase** pInterface)
{
	if (*pInterface) return 0;
	auto cls_name = std::u16string(reinterpret_cast<const char16_t*>(wsName));
	*pInterface = AddInNative::CreateObject(cls_name);
	// Контракт 1С: ненульове значення = успіх. Повертаємо 1 замість адреси,
	// бо приведення 64-бітного вказівника до long усікає його (UB на x64).
	return *pInterface ? 1 : 0;
}
```

Прибрати `#pragma warning(disable: 4311 4302)` угорі файлу (рядки ~8-10), якщо після зміни попередження зникли.

`src/core/AddInNative.cpp:473` — замінити глобальну локаль на ліниву з fallback:

```cpp
// Локаль для регістронезалежного пошуку імен. НЕ глобальний об'єкт:
// std::locale("ru_RU.UTF-8") може кинути виняток, а на етапі статичної
// ініціалізації DLL це означає відмову завантаження компоненти в 1С.
static const std::locale& RuLocale()
{
	static const std::locale loc = []() -> std::locale {
		try { return std::locale("ru_RU.UTF-8"); }
		catch (...) {
			try { return std::locale("Russian_Russia.1251"); }
			catch (...) { return std::locale::classic(); }
		}
	}();
	return loc;
}
```

І в обох `AddInNative::upper` (рядки 475-485) замінити `locale_ru` на `RuLocale()`.

- [X] **Step 4: Запустити — зелено**

Run: цикл збірки/запуску.
Expected: усі `[PASS]`, exit 0.

- [X] **Step 5: Commit**

```powershell
git add src/core/AddInNative.cpp tests/core_selftest.cpp
git commit -m "Ядро: boot-фікси — лінива локаль з fallback (замість глобальної) і контракт 1/0 у GetClassObject (без усічення вказівника)"
```

---

### Task 3: Конвенція повернення — обгортка `Ret()`

**Files:**
- Modify: `src/core/AddInNative.h` (секція після `using MethFunction = std::variant<...>;`, рядок ~108)
- Test: `tests/core_selftest.cpp`

**Interfaces:**
- Consumes: `VariantHelper::operator=` для `std::string/std::u16string/std::wstring/int64_t/double/bool` (`AddInNative.h:68-73`), член `VariantHelper result` (`AddInNative.h:115`).
- Produces: `protected` шаблонний метод `MethFunction Ret(F f)` — приймає лямбду, що ПОВЕРТАЄ значення (`bool`, рядок, число), і загортає її у void-хендлер, який присвоює `this->result`. Використовується всіма наступними реєстраціями функцій (Task 5) і майбутніми драйверами.

- [X] **Step 1: Написати падаючий тест**

```cpp
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
```

- [X] **Step 2: Запустити — компіляція падає**

Expected: FAIL компіляції — `Ret` не оголошено.

- [X] **Step 3: Мінімальна реалізація**

У `src/core/AddInNative.h`, у `protected`-секції класу (поряд із `AddFunction`), додати:

```cpp
	// Обгортає value-повертаючу лямбду у void-хендлер, який присвоює this->result.
	// Потрібно, бо MethFunction — це std::function<void(...)>: значення, повернуте
	// лямбдою напряму, мовчки відкидається і НЕ потрапляє в 1С.
	template <typename F>
	MethFunction Ret(F f) { return WrapRet(std::function(std::move(f))); }

private:
	template <typename R, typename... A>
	MethFunction WrapRet(std::function<R(A...)> f)
	{
		static_assert(!std::is_void_v<R>,
			"Ret(): лямбда мусить повертати значення; для void використовуйте AddProcedure");
		return MethFunction(std::function<void(A...)>(
			[this, f = std::move(f)](A... a) {
				if constexpr (std::is_same_v<R, bool>)
					this->result = f(a...);
				else if constexpr (std::is_integral_v<R>)
					this->result = static_cast<int64_t>(f(a...));
				else if constexpr (std::is_floating_point_v<R>)
					this->result = static_cast<double>(f(a...));
				else
					this->result = f(a...);
			}));
	}
protected:
```

Потрібні include: `<type_traits>` (перевірити наявність у заголовку; додати за відсутності).

- [X] **Step 4: Запустити — зелено**

Expected: усі `[PASS]`.

- [X] **Step 4б: Полагодити зламані сайти TestComponent через Ret()**

> **ПРАВИЛО застосування `Ret()` (за аудитом):** обгортати ЛИШЕ хендлери, чиє
> `return`-значення і є результатом для 1С. **НЕ обгортати** хендлери, які самі
> присвоюють `this->result`, а повертають лише службовий bool — `Ret()` перезаписав
> би корисний результат булевим статусом. Приклади «не чіпати»:
> `GetAvailablePorts` (`TestComponent.cpp:92` — ставить `result = portsStr`, `return true` службовий),
> `CallUapki` (`AddinUAPKIConnect.cpp:80` — ставить `result = jsonResponse`).

Реально зламані сьогодні сайти (повертають bool у void-`std::function` — значення
губиться): `TestComponent.cpp:96-101` (`CheckPortExists`) і `:103-108`
(`IsPortAvailable`). Обгорнути:

```cpp
	AddFunction(
		u"CheckPortExists", u"ПроверитьСуществованиеПорта",
		Ret([&](VH portName) {
			std::u16string port = portName;
			return this->CheckPortExists(port);
		}));

	AddFunction(
		u"IsPortAvailable", u"ДоступенПорт",
		Ret([&](VH portName) {
			std::u16string port = portName;
			return this->IsPortAvailable(port);
		}));
```

Перевірка: збірка повної DLL зелена; core_selftest зелений.

- [X] **Step 5: Commit**

```powershell
git add src/core/AddInNative.h src/TestComponent.cpp tests/core_selftest.cpp
git commit -m "Ядро: Ret() — конвенція повернення значень у 1С; полагоджено CheckPortExists/IsPortAvailable у TestComponent"
```

---

### Task 4: `REGISTER_COMPONENT` + анти-стрип для TestComponent

**Files:**
- Modify: `src/core/AddInNative.h` (макрос у кінці файлу, поза класом)
- Modify: `src/components/AddinUAPKIConnect.cpp:6-10`, `src/TestComponent.cpp:5-9`
- Test: `tests/core_selftest.cpp`

**Interfaces:**
- Produces: макрос `REGISTER_COMPONENT(NAME_U16, CLASS)` — реєструє клас під одним ім'ям і створює анти-стрип reference. Компоненти з кількома іменами (TestComponent) реєструються як зараз + додається анти-стрип reference вручну.

- [X] **Step 1: Написати падаючий тест**

```cpp
static void TestComponentRegistry() {
    std::u16string names = AddInNative::getComponentNames();
    CHECK(names.find(u"AddinECRPrivatJSON") != std::u16string::npos,
          "registry contains AddinECRPrivatJSON");
    CHECK(names.find(u"AddInNative") != std::u16string::npos,
          "registry contains TestComponent aliases");
}
```

Примітка: `core_selftest` НЕ лінкує компоненти ECR/Test (лише base+helpers), тому цей тест у межах селфтесту перевіряє лише реєстр проб (`CoreProbe`, `RetProbe`). Перший CHECK замінити на `names.find(u"CoreProbe") != npos`. Перевірка повного реєстру DLL — через наявний `native_host` в UAPKI-збірці (Task 9). Тест тут — компайл-гейт макросу:

```cpp
struct MacroProbe : public AddInNative {};
REGISTER_COMPONENT(u"MacroProbe", MacroProbe)   // на файловому рівні tests/core_selftest.cpp

static void TestRegisterComponentMacro() {
    CHECK(AddInNative::CreateObject(u"MacroProbe") != nullptr, "REGISTER_COMPONENT works");
}
```

Для цього `MacroProbe` потрібен статичний член: макрос сам його оголошує (див. Step 3) — у тестовій структурі додати `static std::vector<std::u16string> names;`.

- [X] **Step 2: Запустити — компіляція падає**

Expected: FAIL — `REGISTER_COMPONENT` не визначено.

- [X] **Step 3: Мінімальна реалізація**

У кінці `src/core/AddInNative.h` (після класу):

```cpp
// Реєстрація компоненти в реєстрі DLL + захист від відкидання лінкером.
// Клас мусить оголосити: static std::vector<std::u16string> names;
// Використання (у .cpp компоненти, на файловому рівні):
//   REGISTER_COMPONENT(u"МояКомпонента", МійКлас)
#define REGISTER_COMPONENT(NAME_U16, CLASS) \
	std::vector<std::u16string> CLASS::names = { \
		AddInNative::AddComponent(NAME_U16, []() -> AddInNative* { return new CLASS; }) \
	}; \
	namespace { [[maybe_unused]] auto& _force_##CLASS##_names = CLASS::names; }
```

Застосувати в `src/components/AddinUAPKIConnect.cpp:6-10` — замінити ручний блок на:

```cpp
REGISTER_COMPONENT(u"AddinUAPKIConnect", AddinUAPKIConnect)
```

У `src/TestComponent.cpp:5-9` (3 імені — макрос не підходить) лишити список, додати ПІСЛЯ нього відсутній анти-стрип:

```cpp
namespace { [[maybe_unused]] auto& _forceTestComponentNames = TestComponent::names; }
```

`src/components/AddinECRPrivatJSON.cpp:10-13` — за тим самим зразком, що UAPKI:

```cpp
REGISTER_COMPONENT(u"AddinECRPrivatJSON", AddinECRPrivatJSON)
```

- [X] **Step 4: Запустити — зелено; повна DLL збирається**

Run: цикл тестів + `cmake --build build_x64 --config Release --target SimplyAddinConnectWin` (ім'я цілі — з кореневого CMakeLists, перевірити фактичне).
Expected: `[PASS] REGISTER_COMPONENT works`; DLL лінкується.

- [X] **Step 5: Commit**

```powershell
git add src/core/AddInNative.h src/components/AddinUAPKIConnect.cpp src/components/AddinECRPrivatJSON.cpp src/TestComponent.cpp tests/core_selftest.cpp
git commit -m "Ядро: REGISTER_COMPONENT — макрос реєстрації з анти-стрип захистом; анти-стрип для TestComponent"
```

---

### Task 5: Спільний `EnableLogging` у базі + прибирання дублів

**Files:**
- Modify: `src/core/AddInNative.cpp:82-84` (конструктор), `src/core/AddInNative.h` (декларація хелпера)
- Modify: `src/components/AddinUAPKIConnect.cpp` (видалити реєстрацію :30-58 і метод :161-185; декларацію з .h)
- Modify: `src/TestComponent.cpp` (видалити реєстрацію :44-75, дубль Version :15-17; метод EnableLogging з .cpp/.h)
- Test: `tests/core_selftest.cpp`

**Interfaces:**
- Consumes: `ServiceTools::EnableComponentLogging(AddInNative*, const std::string&, const std::string&)` (`src/helpers/ServiceTools.h:127`), `Ret()` з Task 3, `DefaultHelper`.
- Produces: КОЖНА компонента автоматично має 1С-функцію `EnableLogging`/`ИспользоватьЛогирование(уровень="info", путь="")`. Дублікати в компонентах видалені. `DisableComponentLogging` лишається у деструкторах похідних (RTTI-імена в базовому деструкторі некоректні — не переносити).

- [X] **Step 0: Виправити латентний дедлок у ShutdownLogging (передумова)**

Підтверджений аудитом баг наявного коду: `ShutdownLogging` бере `loggersMutex`
(`src/helpers/ServiceTools_Log.cpp:163`) і під ним викликає `Info(componentName, ...)`
(`:168`), а `Info → GetLogger` бере ТОЙ САМИЙ нерекурсивний м'ютекс (`:82`) —
дедлок. Латентний, бо спрацьовує лише коли для компоненти існує логер (у 1С:
увімкнули логування → закрили 1С → деструктор → `DisableComponentLogging` →
зависання процесу). Виправлення — логувати БЕЗ повторного захоплення м'ютекса:

```cpp
void ShutdownLogging(const std::string& componentName) {
    std::lock_guard<std::mutex> lock(loggersMutex);

    auto it = loggers.find(componentName);
    if (it != loggers.end()) {
        try {
            // Логуємо напряму через об'єкт логера (НЕ через Info(): той знову
            // бере loggersMutex усередині GetLogger — це був дедлок)
            it->second->info("Завершение работы логгера для компонента " + componentName);
            it->second->flush();
            loggers.erase(it);

            auto settingsIt = componentLogSettings.find(componentName);
            if (settingsIt != componentLogSettings.end()) {
                componentLogSettings.erase(settingsIt);
            }
        }
        catch (...) {
            // Игнорируем исключения при закрытии логгера
        }
    }
}
```

Тест у `core_selftest.cpp` (додати ДО TestBaseEnableLogging):

```cpp
static void TestShutdownLoggingNoDeadlock() {
    // Ініціалізуємо логер у %TEMP% і одразу гасимо: до фіксу тут висне назавжди
    std::string path = std::string(std::getenv("TEMP")) + "\\core_selftest_dl.log";
    ServiceTools::InitLogging("DeadlockProbe", ServiceTools::LogLevel::Info, path);
    ServiceTools::ShutdownLogging("DeadlockProbe");
    CHECK(true, "ShutdownLogging does not deadlock");
}
```

(Точні імена/неймспейси `InitLogging`/`ShutdownLogging`/`LogLevel` звірити з
`ServiceTools.h` — якщо вони не в публічному заголовку, викликати через
`EnableComponentLogging`/`DisableComponentLogging` з пробною компонентою.)
Запуск до фіксу — тест висне (обірвати вручну), після фіксу — зелений. Коміт
разом зі Step 5 задачі.

- [X] **Step 1: Написати падаючий тест**

```cpp
static void TestBaseEnableLogging() {
    AddInNative* comp = AddInNative::CreateObject(u"CoreProbe");
    MockConnect connect; MockMemory memory;
    comp->Init(&connect); comp->setMemManager(&memory);
    // Метод має бути в БАЗІ — CoreProbe нічого не реєструє сам
    long m = comp->FindMethod((WCHAR_T*)u"EnableLogging");
    CHECK(m >= 0, "base registers EnableLogging");
    long mru = comp->FindMethod((WCHAR_T*)u"ИспользоватьЛогирование");
    CHECK(mru >= 0, "base registers ru alias");
    // Виклик з рівнем off не потребує файлу і має повернути true
    tVariant args[2]; std::memset(args, 0, sizeof(args));
    // рядок "off":
    std::u16string off = u"off";
    args[0].vt = VTYPE_PWSTR;
    args[0].pwstrVal = (WCHAR_T*)off.c_str();
    args[0].wstrLen = 3;
    args[1].vt = VTYPE_PWSTR; args[1].pwstrVal = (WCHAR_T*)u""; args[1].wstrLen = 0;
    tVariant ret{}; std::memset(&ret, 0, sizeof(ret)); ret.vt = VTYPE_EMPTY;
    CHECK(comp->CallAsFunc(m, &ret, args, 2), "CallAsFunc(EnableLogging off)");
    CHECK(ret.vt == VTYPE_BOOL, "EnableLogging returns bool");
    comp->Done(); delete comp;
}
```

(Якщо `EnableComponentLogging` з рівнем `"off"` повертає false за поточною реалізацією — перевірити її семантику в `ServiceTools_Log.cpp` і в тесті використати `"info"` + шлях у `%TEMP%`: `std::string(getenv("TEMP")) + "\\core_selftest.log"`, переданий як u16-рядок. Головне, що перевіряється: метод існує в базі й повертає bool.)

- [X] **Step 2: Запустити — падає**

Expected: `[FAIL] base registers EnableLogging` (метод є лише в похідних).

- [X] **Step 3: Мінімальна реалізація**

`src/core/AddInNative.cpp` — включити хелперний заголовок (після `#include "AddInNative.h"`):

```cpp
#include "../helpers/ServiceTools.h"
```

Конструктор (рядки 82-84) доповнити:

```cpp
AddInNative::AddInNative(void) : result(nullptr, this) {
	AddProperty(u"Version", u"Версия", [&](VH var) { var = this->version(); });
	// Спільний для всіх компонент вмикач логування (делегат у ServiceTools).
	AddFunction(u"EnableLogging", u"ИспользоватьЛогирование",
		Ret([this](VH logLevel, VH logFilePath) {
			try {
				std::string level = logLevel;
				std::string path = logFilePath;
				return ServiceTools::EnableComponentLogging(this, level, path);
			}
			catch (...) { return false; }
		}),
		// Явний тип: після появи перевантаження з vector<ParamSpec> (Task 6)
		// braced-list без типу може стати неоднозначним
		MethDefaults{ {0, DefaultHelper(u"info")}, {1, DefaultHelper(u"")} });
}
```

Якщо include створює циклічну залежність заголовків (ServiceTools.h включає AddInNative.h) — цикл існує лише на рівні .cpp і безпечний; за потреби в base_component додати include-шлях `src` (перевірити, чи компілюється — глобальні шляхи вже містять `src`).

Видалити:
- `AddinUAPKIConnect.cpp:29-58` (реєстрація EnableLogging) і `:161-185` (метод), декларацію `EnableLogging` з `AddinUAPKIConnect.h`;
- `TestComponent.cpp:15-17` (дубль Version), `:43-75` (реєстрація EnableLogging), метод `EnableLogging` з TestComponent.{h,cpp};
- у `AddinECRPrivatJSON.cpp` — аналогічну реєстрацію (`:81-95` за грепом `EnableLogging`) і метод/декларацію.

- [X] **Step 4: Запустити — зелено; DLL збирається**

Expected: `[PASS]` усі; повна ціль DLL лінкується (перевірити, що видалені методи ніде більше не викликаються: `grep -n "->EnableLogging(" src/`).

- [X] **Step 5: Commit**

```powershell
git add src/core/AddInNative.cpp src/core/AddInNative.h src/components/AddinUAPKIConnect.* src/components/AddinECRPrivatJSON.* src/TestComponent.* tests/core_selftest.cpp
git commit -m "Ядро: EnableLogging/ИспользоватьЛогирование у базовому AddInNative; прибрано 3 дублі та дубль властивості Version"
```

---

### Task 6: Декларативні параметри (`ParamSpec`) з валідацією і зрозумілими помилками

**Files:**
- Modify: `src/core/AddInNative.h` (struct `ParamSpec`, поле в `Meth`, нові перевантаження `AddFunction`/`AddProcedure`), `src/core/AddInNative.cpp` (`CallAsProc`/`CallAsFunc`: валідація перед `CallMethod`)
- Test: `tests/core_selftest.cpp`

**Interfaces:**
- Consumes: `Meth` (`AddInNative.h:132-137`), `MethDefaults`, `AddError` (`AddInNative.cpp:577-581`), `alias` (з `SetLocale`).
- Produces:
```cpp
struct ParamSpec {
    std::u16string nameEn;
    std::u16string nameRu;
    bool required = false;
    std::optional<DefaultHelper> byDefault{};  // якщо задано — потрапляє в MethDefaults
};
// нові перевантаження (старі лишаються незмінними):
void AddProcedure(nameEn, nameRu, handler, const std::vector<ParamSpec>& params);
void AddFunction (nameEn, nameRu, handler, const std::vector<ParamSpec>& params);
```
Правило валідації (виконується в `CallAsProc`/`CallAsFunc` ДО виклику хендлера): для кожного `required`-параметра без дефолту — якщо аргумент не передано (`i >= lSizeArray`) або він `VTYPE_EMPTY` → `AddError` з текстом «Параметр '<ім'я>' методу '<метод>' обов'язковий, отримано порожнє значення» (ім'я — ru/en за `alias`) і `return false`. Хендлер не викликається.

- [X] **Step 1: Написати падаючий тест**

```cpp
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
```

- [X] **Step 2: Запустити — компіляція падає**

Expected: FAIL — `ParamSpec` не оголошено.

- [X] **Step 3: Мінімальна реалізація**

`AddInNative.h`:
- після `using MethDefaults = ...` додати `struct ParamSpec` (як в Interfaces; потрібен `#include <optional>`);
- у `struct Meth` додати поле `std::vector<ParamSpec> params;` (за замовчуванням порожнє — старі реєстрації не валідуються, поведінка без змін);
- оголосити перевантаження:

```cpp
	void AddProcedure(const std::u16string& nameEn, const std::u16string& nameRu,
	                  const MethFunction& handler, const std::vector<ParamSpec>& params);
	void AddFunction(const std::u16string& nameEn, const std::u16string& nameRu,
	                 const MethFunction& handler, const std::vector<ParamSpec>& params);
```

`AddInNative.cpp` — реалізація перевантажень (поряд із наявними, :420-428):

```cpp
static AddInNative::MethDefaults DefaultsFromSpecs(const std::vector<AddInNative::ParamSpec>& params)
{
	AddInNative::MethDefaults defs;
	for (long i = 0; i < (long)params.size(); ++i)
		if (params[i].byDefault) defs.emplace(i, *params[i].byDefault);
	return defs;
}

void AddInNative::AddProcedure(const std::u16string& nameEn, const std::u16string& nameRu,
                               const MethFunction& handler, const std::vector<ParamSpec>& params)
{
	methods.push_back({ { nameEn, nameRu }, handler, DefaultsFromSpecs(params), false, params });
}

void AddInNative::AddFunction(const std::u16string& nameEn, const std::u16string& nameRu,
                              const MethFunction& handler, const std::vector<ParamSpec>& params)
{
	methods.push_back({ { nameEn, nameRu }, handler, DefaultsFromSpecs(params), true, params });
}
```

(Порядок полів у Meth-ініціалізації звірити з фактичним оголошенням struct; поле `params` — останнє. Старі виклики `AddProcedure/AddFunction` з `MethDefaults` мають продовжити компілюватись — за конфлікту перевантажень із `{}`-литералами додати явний тип у місцях виклику.)

Валідація — приватний метод + виклик у `CallAsProc` (:347-362) і `CallAsFunc` (:364-382) ПЕРЕД `CallMethod`:

```cpp
bool AddInNative::ValidateParams(Meth& m, tVariant* paParams, const long lSizeArray)
{
	for (size_t i = 0; i < m.params.size(); ++i) {
		const ParamSpec& spec = m.params[i];
		if (!spec.required || spec.byDefault) continue;
		const bool missing = (long)i >= lSizeArray
			|| paParams == nullptr
			|| paParams[i].vt == VTYPE_EMPTY;
		if (missing) {
			const std::u16string& pname = alias ? spec.nameRu : spec.nameEn;
			const std::u16string& mname = alias ? m.names[1] : m.names[0];
			AddError(u"Параметр '" + pname + u"' методу '" + mname +
			         u"' обов'язковий, отримано порожнє значення");
			return false;
		}
	}
	return true;
}
```

У `CallAsProc`: після отримання `it` — `if (!ValidateParams(*it, paParams, lSizeArray)) return false;`. У `CallAsFunc` — так само (до `CallMethod`, після `result << VA(pvarRetValue)`).

Додатково (за аудитом): у місцях виклику зі старим 4-м аргументом-braced-list
(`TestComponent.cpp:36`, база з Task 5) типізувати явно `MethDefaults{...}` /
`std::vector<ParamSpec>{...}` — щоб перевантаження не стали неоднозначними
(елемент `{0, DefaultHelper(...)}` теоретично матчиться і на `ParamSpec`,
бо літерал `0` конвертується в `const char16_t*` для `std::u16string`).

- [X] **Step 3б: Захист індексів методів/властивостей (hardening за аудитом)**

`std::next(begin, N)` при `N < 0` або `N > size()` — UB ще ДО перевірки
`it == end()` (патерн у `AddInNative.cpp:242,259,349,366` та в property-шляхах).
Додати на початок КОЖНОГО методу, що приймає `lMethodNum`/`lPropNum` з-зовні
(`GetNParams`, `GetParamDefValue`, `HasRetVal`, `CallAsProc`, `CallAsFunc`,
`GetMethodName`, `GetPropVal`, `SetPropVal`, `IsPropReadable`, `IsPropWritable`,
`GetPropName`) охорону за зразком:

```cpp
	if (lMethodNum < 0 || static_cast<size_t>(lMethodNum) >= methods.size()) return false;
```

(для функцій, що повертають вказівник — `return nullptr;`, для `long` — `return -1;`).
Тест у `core_selftest.cpp`:

```cpp
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
```

- [X] **Step 4: Запустити — зелено**

Expected: усі `[PASS]`, старі тести теж зелені (регресія перевантажень).

- [X] **Step 5: Commit**

```powershell
git add src/core/AddInNative.h src/core/AddInNative.cpp tests/core_selftest.cpp
git commit -m "Ядро: декларативні параметри ParamSpec — required/default із валідацією і точним AddError до виклику хендлера"
```

---

### Task 7: EventBridge — потокобезпечний `ExternalEvent`

**Files:**
- Modify: `src/core/AddInNative.h` (метод + м'ютекс), `src/core/AddInNative.cpp` (`Init`/`Done` + реалізація)
- Test: `tests/core_selftest.cpp`

**Interfaces:**
- Consumes: `IAddInDefBase::ExternalEvent(WCHAR_T*, WCHAR_T*, WCHAR_T*)` (`include/AddInDefBase.h:96-98`), `m_iConnect`, ім'я компоненти `name` (виставляється в `CreateObject`, `AddInNative.cpp:411`).
- Produces:
```cpp
// Публічний, викликається з БУДЬ-ЯКОГО потоку (фонові reader-потоки транспортів).
// source = ім'я компоненти в 1С (this->name). false — якщо зв'язку з 1С немає.
bool PostExternalEvent(const std::u16string& message, const std::u16string& data);
```
Гарантія: після повернення з `Done()` жоден `PostExternalEvent` не торкнеться `m_iConnect` (м'ютекс + обнулення). Це фундамент подій для Етапів 1-2 (JobEngine, SimplyChannel).

- [X] **Step 1: Написати падаючий тест**

```cpp
#include <thread>
static void TestEventBridge() {
    AddInNative* comp = AddInNative::CreateObject(u"CoreProbe");
    MockConnect connect; MockMemory memory;
    comp->Init(&connect); comp->setMemManager(&memory);

    // Постинг з фонового потоку доставляє подію в IAddInDefBase
    std::thread t([&] { comp->PostExternalEvent(u"OnData", u"payload123"); });
    t.join();
    CHECK(connect.events.size() == 1, "event delivered from background thread");
    CHECK(connect.events[0].find(u"OnData") != std::u16string::npos &&
          connect.events[0].find(u"payload123") != std::u16string::npos,
          "event carries message and data");

    // Після Done() постинг безпечний і повертає false
    comp->Done();
    CHECK(!comp->PostExternalEvent(u"OnData", u"late"), "post after Done returns false");
    CHECK(connect.events.size() == 1, "no delivery after Done");
    delete comp;
}
```

- [X] **Step 2: Запустити — компіляція падає**

Expected: FAIL — `PostExternalEvent` не оголошено.

- [X] **Step 3: Мінімальна реалізація**

`AddInNative.h`: у public-секцію — декларація методу; у private — `std::mutex connectMutex_;` (include `<mutex>`).

`AddInNative.cpp`:

```cpp
bool AddInNative::Init(void* pConnection)
{
	std::lock_guard<std::mutex> lock(connectMutex_);
	m_iConnect = static_cast<IAddInDefBase*>(pConnection);
	if (m_iConnect) m_iConnect->SetEventBufferDepth(100);
	return m_iConnect != nullptr;
}

void AddInNative::Done()
{
	// Зв'язок з 1С далі недійсний: відсікаємо фонові PostExternalEvent
	std::lock_guard<std::mutex> lock(connectMutex_);
	m_iConnect = nullptr;
}

bool AddInNative::PostExternalEvent(const std::u16string& message, const std::u16string& data)
{
	// ExternalEvent приймає WCHAR_T* без const — віддаємо mutable-буфери
	// локальних копій (u16string::data() не-const з C++17); платформа копіює
	// їх синхронно всередині виклику
	std::u16string src = name, msg = message, dat = data;
	std::lock_guard<std::mutex> lock(connectMutex_);
	if (!m_iConnect) return false;
	return m_iConnect->ExternalEvent(
		reinterpret_cast<WCHAR_T*>(src.data()),
		reinterpret_cast<WCHAR_T*>(msg.data()),
		reinterpret_cast<WCHAR_T*>(dat.data()));
}
```

Додатково (за аудитом): `AddError` (`AddInNative.cpp:577-581`) читає `m_iConnect`
без синхронізації — з фоновими потоками це data race з `Done()`. Узяти той самий
м'ютекс:

```cpp
void AddInNative::AddError(const std::u16string& descr)
{
	std::u16string source = u"AddIn." + name;
	std::u16string text = descr;
	std::lock_guard<std::mutex> lock(connectMutex_);
	if (!m_iConnect) return;
	m_iConnect->AddError(ADDIN_E_IMPORTANT,
		reinterpret_cast<WCHAR_T*>(source.data()),
		reinterpret_cast<WCHAR_T*>(text.data()), 0);
}
```

(Точну поточну сигнатуру/тіло `AddError` звірити на місці — зберегти наявну
семантику, додавши лише lock + null-guard. У тесті EventBridge доповнити:
`comp->Done();` потім виклик методу, що всередині робить `AddError`, — не падає.)

- [X] **Step 4: Запустити — зелено**

Expected: усі `[PASS]`. Прогнати кілька разів поспіль (потоковий тест):
`for ($i=0; $i -lt 20; $i++) { bin\Release\core_selftest_x64.exe | Select-String FAIL }` — порожньо.

- [X] **Step 5: Commit**

```powershell
git add src/core/AddInNative.h src/core/AddInNative.cpp tests/core_selftest.cpp
git commit -m "Ядро: PostExternalEvent — потокобезпечний міст подій у 1С (ExternalEvent) з відсіканням після Done"
```

---

### Task 8: Fallback-sink логера (ранні логи не губляться)

**Files:**
- Modify: `src/helpers/ServiceTools_Log.cpp:81-91` (`GetLogger`)
- Test: `tests/core_selftest.cpp` (смоук — відсутність креша)

**Interfaces:**
- Consumes: spdlog `msvc_sink` (`spdlog/sinks/msvc_sink.h` — OutputDebugString).
- Produces: до виклику `EnableLogging` усі `REPORT_*`/`NEUTRAL_REPORT_*` пишуть у DebugView (OutputDebugString) на рівні warn+, а не в «порожній» `spdlog::default_logger`.

- [X] **Step 1: Написати тест (смоук)**

```cpp
#include "../src/helpers/ServiceTools.h"
static void TestFallbackLogging() {
    // До EnableLogging репорти не мають ані падати, ані вимагати файлу
    NEUTRAL_REPORT_WARN("CoreSelftest", "Перевірка fallback-логера до EnableLogging");
    CHECK(true, "fallback logging does not crash");
}
```

- [X] **Step 2: Запустити — має бути зелено вже зараз (базлайн)**

Expected: PASS (default_logger теж не падає). Це базлайн-тест: захищає від регресії ПІСЛЯ зміни. Головна перевірка Step 4 — ручна.

- [X] **Step 3: Реалізація**

`ServiceTools_Log.cpp` — додати include і fallback:

```cpp
#include <spdlog/sinks/msvc_sink.h>

// Fallback-логер до першого EnableLogging: OutputDebugString (видно в DebugView/
// відладчику), рівень warn — щоб діагностика старту DLL не губилась мовчки.
static std::shared_ptr<spdlog::logger> GetFallbackLogger() {
    static std::shared_ptr<spdlog::logger> fallback = [] {
        auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("SimplyAddinConnect", sink);
        logger->set_level(spdlog::level::warn);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
        return logger;
    }();
    return fallback;
}
```

У `GetLogger` (рядок 90) замінити `return spdlog::default_logger();` на `return GetFallbackLogger();`.

- [X] **Step 4: Перевірити**

Run: збірка + `bin\Release\core_selftest_x64.exe` (зелено). Ручна верифікація: запуск під відладчиком VS або DebugView — рядок `[SimplyAddinConnect] [warning] Перевірка fallback-логера...` присутній.

- [X] **Step 5: Commit**

```powershell
git add src/helpers/ServiceTools_Log.cpp tests/core_selftest.cpp
git commit -m "Логування: fallback-sink OutputDebugString до EnableLogging — рання діагностика не губиться"
```

---

### Task 9: Інтеграція в `run_tests.ps1`, повна збірка, синхронізація доків

**Files:**
- Modify: `run_tests.ps1` (додати рівень L0.5: запуск `core_selftest_<arch>.exe` перед L1)
- Modify: `AGENTS.md` (розділи «Тести», «Як додати компоненту»), `docs/architecture/core.md`, `CLAUDE.md` за потреби

**Interfaces:**
- Consumes: усі попередні задачі.
- Produces: зелений повний прогін; документація відповідає коду.

- [X] **Step 1: Додати core_selftest у run_tests.ps1**

Конкретні точки (за аудитом): поряд з `$SelfTestExe` (`run_tests.ps1:39`) додати
`$CoreSelftestExe = Join-Path $BinRelease "core_selftest$ArchSuffix.exe"`;
перевірку `$haveExes` (`:208` і `:218`) НЕ розширювати — вона стосується
UAPKI-екзешників; для core — окрема перевірка наявності з власним
`Add-Result 'build' 'core-exe' ...`. Запуск core_selftest — ПЕРЕД блоком L1
(`:229+`): exit 0 → PASS, інакше FAIL, у підсумкову таблицю тим самим
`Add-Result`-патерном. Ключова вимога: core-крок НЕ повинен вимагати провайдера
UAPKI (`$ProviderDll`) чи UAPKI-екзешників — він має проходити і в збірці без
`-WithUAPKI`.

- [X] **Step 2: Повна збірка з UAPKI і тестами**

Run: `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests`
Expected: обидві архітектури зібрані, ZIP створено, тестові exe у `bin/Release`.

- [X] **Step 3: Повний прогін тестів**

Run: `powershell -File run_tests.ps1 x64`, потім `powershell -File run_tests.ps1 x86`
Expected: core_selftest PASS + всі попередні UAPKI-рівні (L0/L1/L2/L3) без регресій — підсумкова таблиця без FAIL.

- [X] **Step 4: Синхронізувати документацію**

- `AGENTS.md` → «Як додати компоненту»: замінити ручний блок `names`+`_force` на `REGISTER_COMPONENT`, згадати `Ret()` (і правило «не обгортати хендлери, що самі ставлять this->result»), `ParamSpec`, успадкований `EnableLogging`; «Тести» → додати `core_selftest`. Перевірено аудитом: `build_project.ps1:78` ВЖЕ передає `-DBUILD_TESTS=ON` незалежно від `-WithUAPKI` — скрипт міняти не треба; виправити лише формулювання в AGENTS.md («-WithTests працює лише разом з -WithUAPKI» → «core_selftest збирається завжди при -WithTests; uapki_selftest/native_host — лише разом з -WithUAPKI»).
- `docs/architecture/core.md`: нові механізми (Ret, ParamSpec+валідація, PostExternalEvent, EnableLogging у базі, boot-фікси) — окремим розділом «Платформенні механізми ядра (Етап 0)».

- [X] **Step 5: Commit**

```powershell
git add run_tests.ps1 AGENTS.md docs/architecture/core.md
git commit -m "Етап 0: core_selftest у run_tests.ps1, повний зелений прогін, синхронізація доків ядра"
```

---

## Self-review плану (виконано)

- **Покриття спеки §3.0:** (1) конвенція повернення — Task 3; (2) REGISTER_COMPONENT — Task 4; (3) EnableLogging у базі — Task 5; (4) декларативні параметри — Task 6; (5) EventBridge — Task 7; (6) boot-фікси — Task 2 (локаль, GetClassObject) + Task 8 (sink). JSON-форма методів і OperationRegistry — Етап 1 (за спекою §3.1). Правки ECR-фасаду — свідомо Етап 2.
- **Типи узгоджені:** `Ret()` повертає `MethFunction`; `ParamSpec` використовує `DefaultHelper`; `PostExternalEvent(u16string,u16string)` — однаково в Task 7 тесті й реалізації.
- **Ризики виконавцю:** точні номери рядків можуть «поплисти» — орієнтуватися на цитовані фрагменти коду; сигнатуру `AddError`/`FindProp`/`GetPropVal` звірити з `AddInNative.h` перед написанням моків (тест Task 1 компілюється лише за точної відповідності інтерфейсам `include/*.h`).
