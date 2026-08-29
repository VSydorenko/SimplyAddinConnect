# Ядро AddInNative та наскрізні сервіси ServiceTools

Цей документ описує базовий шар SimplyAddinConnect — ядро `AddInNative` і
наскрізні сервіси `ServiceTools`. Це фундамент, спільний для **будь-якої**
підсистеми (UAPKI, майбутні драйвери обладнання на device-core тощо): кожна компонента успадковує `AddInNative`,
а весь прикладний код логує та конвертує рядки через `ServiceTools`.

Усі шляхи наведено відносно кореня репозиторію. Наведені імена
файлів, функцій і полів звірені з кодом.

---

## 1. Точка входу DLL і реєстр компонент

### 1.1. Експортовані функції

Модуль експортує рівно три C-функції (`src/core/AddInNative.def:1-4`):

| Функція | Призначення |
|---|---|
| `GetClassNames` | перелік зареєстрованих імен компонент |
| `GetClassObject` | створення екземпляра компоненти на ім'я |
| `DestroyObject` | знищення екземпляра |

`GetClassNames` і `GetClassObject` оголошені `friend`-функціями класу
`AddInNative` (`src/core/AddInNative.h:146-148`), а `CreateObject` — приватний
статичний метод класу. (У SDK присутня ще декларація `SetPlatformCapabilities`,
але в `.def` вона не експортується.)

Реалізації (`src/core/AddInNative.cpp`):

- `GetClassNames()` (`47-51`) повертає `AddInNative::getComponentNames()` —
  список імен, розділених `|`, закешований у `static const std::u16string names`.
- `GetClassObject(wsName, pInterface)` (`53-58`): якщо `*pInterface` уже не
  nullptr — повертає `0` (відмова створювати поверх наявного); інакше створює
  об'єкт через `AddInNative::CreateObject(cls_name)` і повертає
  `long(*pInterface = ...)` — тобто ознакою успіху є ненульова адреса,
  приведена до `long`.
- `DestroyObject(pInterface)` (`60-66`): якщо `*pInterface == nullptr` → `-1`;
  інакше `delete *pInterface; *pInterface = nullptr; return 0;`.

### 1.2. Реєстр компонент і статична реєстрація

Одна DLL може містити багато компонент. Реєстр — статична мапа
«ім'я 1С → фабрика»:

```cpp
static std::map<std::u16string, CompFunction> components;              // AddInNative.h:150
static std::u16string AddComponent(name, creator);                     // AddInNative.h:114
static AddInNative* CreateObject(const std::u16string& name);          // приватний
```

- `AddComponent(name, creator)` (`AddInNative.cpp:401-405`) вставляє пару
  `components.insert({name, creator})` і **повертає те саме `name`** — щоб
  результат можна було використати як елемент ініціалізатора статичного вектора.
- `CreateObject(name)` (`AddInNative.cpp:407-413`) шукає фабрику в `components`,
  викликає її (`it->second()`), проставляє `object->name = name` і повертає
  вказівник; якщо ім'я не зареєстроване — `nullptr`.

**Реєстрація відбувається статично, до `main`**, через побічний ефект
ініціалізації статичного члена класу-нащадка. Компонента з одним ім'ям
реєструється макросом `REGISTER_COMPONENT` (§6.2), який визначає `names` і додає
анти-стрип reference одним рядком; компонента з кількома іменами — ручним
`names = { AddComponent(...), ... }` + окремий анти-стрип:

- `src/components/AddinUAPKIConnect.cpp:7` — `REGISTER_COMPONENT(u"AddinUAPKIConnect", …)`.
- `src/TestComponent.cpp` — ручний `TestComponent::names` (`u"AddInNative"`,
  `u"SimplyAddinConnect"`, `u"SimplyConnect"`) + анти-стрип `_forceTestComponentNames`.

### 1.3. Прийом `_force` (захист від відкидання лінкером)

При статичному лінкуванні OBJECT-бібліотек у фінальну DLL лінкер може відкинути
транслейшн-юніт, на символи якого немає прямих посилань — разом зі статичним
ініціалізатором, що виконує реєстрацію. Щоб цьому запобігти, у `.cpp` додається
анонімний non-const reference-об'єкт рівня TU. Для компонент з одним ім'ям це
робить `REGISTER_COMPONENT` (§6.2) автоматично (`_force_##CLASS##_names`); для
`TestComponent` (кілька імен) reference додано вручну:

```cpp
// src/TestComponent.cpp — після ручного списку names
namespace { [[maybe_unused]] auto& _forceTestComponentNames = TestComponent::names; }
```

Наразі зареєстровані імена: `AddinUAPKIConnect`, `ECRPrivatJSON`,
`AddInNative` / `SimplyAddinConnect` / `SimplyConnect` (останні три — через
`TestComponent`). Драйвер **ECRPrivatJSON** (`src/drivers/ecr_privatjson/`) — перший драйвер
обладнання на фундаменті device-core; фасад `AddinECRPrivatJSON`
(`src/components/AddinECRPrivatJSON.*`) через `REGISTER_COMPONENT` реєструє компоненту
1С `ECRPrivatJSON`, що делегує драйверу (див. [ecrprivatjson.md](ecrprivatjson.md),
[device-core.md](device-core.md)).

---

## 2. Модель методів і властивостей

Компонента описує свій API трьома реєструючими методами ядра
(`src/core/AddInNative.h:110-112`):

```cpp
void AddProperty (nameEn, nameRu, const PropFunction& getter, const PropFunction& setter = nullptr);
void AddProcedure(nameEn, nameRu, const MethFunction& handler, const MethDefaults& defs = {});
void AddFunction (nameEn, nameRu, const MethFunction& handler, const MethDefaults& defs = {});
```

Обробники — лямбди, тому шаблонні описи API не потрібні.

### 2.1. Зберігання

Реалізація (`src/core/AddInNative.cpp:415-428`):

- `AddProperty` пуш-бекає `{ {nameEn, nameRu}, getter, setter }` у `properties`.
- `AddProcedure` пуш-бекає `{ {nameEn, nameRu}, handler, defs, hasRetVal=false }`
  у `methods`.
- `AddFunction` — те саме з `hasRetVal=true`.

Тобто різниця «процедура vs функція» — це лише прапорець `hasRetVal`, який
пізніше повертає `HasRetVal()` (`AddInNative.cpp:291-301`).

### 2.2. Два імені (en/ru) і регістронезалежний пошук

`struct Prop` і `struct Meth` мають поле `std::vector<std::u16string> names`
(`AddInNative.h:127,133`), куди завжди кладуться рівно два елементи
`{nameEn, nameRu}`. `GetPropName` / `GetMethodName`
(`AddInNative.cpp:140-152`, `226-238`) індексують `names` за
`lPropAlias` / `lMethodAlias`, де `0` — міжнародний псевдонім, `1` — російський
(за коментарем SDK `include/ComponentBase.h:79-80,134-135`).

Пошук імені двоетапний. `FindProp` (`AddInNative.cpp:123-138`) і `FindMethod`
(`AddInNative.cpp:209-224`) спершу шукають **точний** збіг (`compare`) — швидкий
шлях; і лише якщо не знайдено — приводять шуканий рядок і всі імена до верхнього
регістру через `AddInNative::upper()` і шукають знову. `upper()`
(`AddInNative.cpp:473-484`) використовує `std::toupper` з локаллю `ru_RU.UTF-8`.

### 2.3. Арність методів: `MethFunction0..7`

Обробник методу — `std::variant` лямбд від 0 до 7 аргументів
(`AddInNative.h:90-108`):

```cpp
using MethFunction = std::variant<MethFunction0, ..., MethFunction7>;
// MethFunctionN = std::function<void(VH, VH, ... N раз)>
```

Кількість параметрів визначається під час виконання через
`std::get_if<MethFunctionN>` у `GetNParams` (`AddInNative.cpp:240-253`) і так само
диспетчеризується у `CallMethod` (`AddInNative.cpp:303-345`). Якщо `lSizeArray`
менше за очікувану арність, `CallMethod` кидає `std::bad_function_call()`
(`AddInNative.cpp:310,315,320,...`).

### 2.4. Значення за замовчанням: `MethDefaults` / `DefaultHelper`

`using MethDefaults = std::map<long, DefaultHelper>` (`AddInNative.h:88`) —
відображення «номер параметра → значення за замовчанням».

`DefaultHelper` (`AddInNative.h:20-41`) — обгортка над
`std::variant<EmptyValue, std::u16string, int64_t, double, bool>` з конструкторами
від `u16string`, `int64_t`, `double`, `bool` і `const char16_t*` (де `nullptr`
дає `EmptyValue`). `GetParamDefValue` (`AddInNative.cpp:255-289`) читає значення з
`defs` за номером параметра і пише його у `tVariant` через `VH::operator=`.

---

## 3. VariantHelper (VH) — міст типів

`VariantHelper` (скорочено `VH`, `using VH = VariantHelper;` на `AddInNative.h:87`)
— protected вкладений клас-обгортка над `tVariant` (варіантний тип платформи 1С).
Оголошення: `src/core/AddInNative.h:51-85`.

### 3.1. Контекст

Поля (`AddInNative.h:53-57`):

```cpp
tVariant*    pvar;    // сам варіант 1С
AddInNative* addin;   // ядро (для доступу до пам'яті та AddError)
Prop*        prop;    // прив'язана властивість (для повідомлень про помилки)
Meth*        meth;    // прив'язаний метод
long         number;  // номер параметра
```

Контекст (`prop`/`meth`/`number`) потрібен, щоб формувати змістовні
повідомлення про помилку типу.

### 3.2. Конвертації з `tVariant` у C++ (`operator T() const`)

| Оператор | Місце | Поведінка |
|---|---|---|
| `std::string` | `634-638` | через ланцюжок до `u16string`, потім `WCHAR2MB` |
| `std::wstring` | `640-644` | |
| `std::u16string` | `646-651` | вимагає `VTYPE_PWSTR`, інакше `error(VTYPE_PWSTR)` |
| `int64_t` | `653-668` | приймає `I2/I4/UI1/ERROR`→`lVal` або `R4/R8`→`dblVal`, інакше `throw error(VTYPE_I4)` |
| `int` | `670-685` | аналогічно `int64_t` |
| `double` | `687-702` | інакше `error(VTYPE_R4)` |
| `bool` | `704-718` | приймає `VTYPE_BOOL` або цілі типи, інакше `error(VTYPE_BOOL)` |

(Місця вказано за `src/core/AddInNative.cpp`.)

### 3.3. Конвертації з C++ у `tVariant` (`operator=`)

| Аргумент | Місце | Поведінка |
|---|---|---|
| `const std::string&` | `507-510` | делегує в `u16string` через `MB2WCHAR` |
| `const std::wstring&` | `512-520` | якщо `sizeof(wchar_t)==2` — прямий reinterpret у `u16string`, інакше через `WC2MB` |
| `int64_t` | `534-546` | `VTYPE_I4`, якщо влазить у `INT32_MIN..INT32_MAX`, інакше `VTYPE_R8` |
| `double` | `548-554` | `VTYPE_R8` |
| `bool` | `556-562` | `VTYPE_BOOL` |
| `const std::u16string&` | `564-575` | `VTYPE_PWSTR`; виділяє пам'ять через `addin->AllocMemory`, `memcpy`, підрізає кінцеві нулі (`wstrLen`) |

Завдяки цьому в лямбдах пишемо звичайний C++ (`std::string s = param;
param = result;`), а перетворення на/з `tVariant` та виділення пам'яті під рядки
ядро робить саме.

### 3.4. Очищення, бінарні дані

Кожен сеттер починається з `clear()` (`AddInNative.cpp:522-532`): для
`VTYPE_BLOB`/`VTYPE_PWSTR` звільняє попередню пам'ять через
`addin->FreeMemory(&TV_WSTR(pvar))`, потім `tVarInit(pvar)`.

`AllocMemory(unsigned long size)` (`AddInNative.cpp:720-726`) готує варіант під
бінарні дані: `clear()`, потім `addin->AllocMemory((void**)&pvar->pstrVal, size)`,
виставляє `VTYPE_BLOB` і `pvar->strLen = size`. Методи `size()` / `data()`
(`493-505`) доступні лише для `VTYPE_BLOB`, інакше кидають `error(VTYPE_BLOB)`.

### 3.5. Помилки типу

`VariantHelper::error(TYPEVAR vt) const` (`AddInNative.cpp:611-632`) формує
локалізоване повідомлення (укр./рос. або англ. — залежно від прапорця
`addin->alias`) на кшталт «Error getting value of property ... / when calling
method ... parameter ... expected ... actual ...», **реєструє його через
`addin->AddError(...)`** і повертає об'єкт-виняток `std::bad_typeid`. Тобто
`AddError` уже викликано всередині `error()`, а сам throw служить лише для
переривання потоку виконання; тому в місцях виклику
(`AddInNative.cpp:163-169,180-187,282-289`) його ловлять як `catch(...)`.

### 3.6. Повернення результату функції: `this->result`

`VariantHelper result;` — публічний член (`AddInNative.h:115`), ініціалізований у
конструкторі `result(nullptr, this)` (`AddInNative.cpp:82`).

- У `CallAsProc` (`347-362`) перед викликом метода виконується
  `result << VA(nullptr)` — скидання прив'язки.
- У `CallAsFunc` (`364-382`) перед викликом — `result << VA(pvarRetValue)`:
  `this->result` прив'язується до вихідного `tVariant*`, лямбда пише результат
  через `this->result = ...`; після виклику знову `result << VA(nullptr)`
  (від'єднання). При виключенні `catch(...)` теж скидає `result << VA(nullptr)`
  (`378-380`).

---

## 4. Пам'ять і життєвий цикл

### 4.1. `IMemoryManager` — пам'ять для рядків, що йдуть у 1С

`include/IMemoryManager.h:15-32` — інтерфейс з двома чисто віртуальними методами:
`AllocMemory(void** pMemory, unsigned long ulCountByte)` і
`FreeMemory(void** pMemory)`.

- Приватне поле ядра `IMemoryManager* m_iMemory = nullptr;`
  (`src/core/AddInNative.h:193`).
- Приватні обгортки `AllocMemory` / `FreeMemory` (`AddInNative.cpp:430-438`)
  делегують у `m_iMemory->...`, якщо той не null (інакше `AllocMemory` повертає
  `false`, `FreeMemory` — no-op).
- `setMemManager(void* memory)` (`AddInNative.cpp:98-101`):
  `return m_iMemory = static_cast<IMemoryManager*>(memory);` — присвоєння в умові
  `return`, результат `true`, якщо `memory != nullptr`.

Напряму керувати цією пам'яттю прикладному коду не треба — це робить `VH` (див. §3.4).

### 4.2. `IAddInDefBase` — зворотний зв'язок з платформою

`include/AddInDefBase.h:35-115` — інтерфейс 1С: `AddError`, `Read`/`Write`
властивостей, `RegisterProfileAs`, `SetEventBufferDepth`/`GetEventBufferDepth`,
`ExternalEvent`, `CleanEventBuffer`, `SetStatusLine`/`ResetStatusLine`.

- Приватне поле `IAddInDefBase* m_iConnect = nullptr;` (`src/core/AddInNative.h:194`).
- `Init(void* pConnection)` (`AddInNative.cpp:91-96`): зберігає `m_iConnect`, і
  якщо він не null — виставляє `m_iConnect->SetEventBufferDepth(100)`; повертає
  `m_iConnect != nullptr`.
- `AddError(descr, scode=0)` (`AddInNative.cpp:577-581`): формує джерело
  `u"AddIn." + name`, викликає
  `m_iConnect->AddError(ADDIN_E_IMPORTANT, source, descr, scode)`
  (`ADDIN_E_IMPORTANT = 1003`, `include/types.h:51`); повертає `false`, якщо
  `m_iConnect == nullptr`.

### 4.3. Життєвий цикл SDK

Ядро реалізує `IComponentBase` (`class AddInNative : public IComponentBase`,
`src/core/AddInNative.h:45`), який успадковує `IInitDoneBase`,
`ILanguageExtenderBase`, `LocaleBase` (`include/ComponentBase.h:213-220`).

Порядок викликів платформи (SDK 1С):

```
Init → setMemManager → GetInfo → робота → Done
```

- `GetInfo()` (`AddInNative.cpp:103-106`) завжди повертає `2000` (версія 2 SDK 1С).
- `Done()` (`AddInNative.cpp:108-110`) — порожня; ресурси звільняються через
  RAII/деструктор, а не тут явно.
- `SetLocale(locale)` (`AddInNative.cpp:384-388`, частина `LocaleBase`):
  конвертує локаль через `WCHAR2MB` і виставляє
  `this->alias = (loc.substr(0,3) == "rus")` — прапорець мови повідомлень про
  помилки (укр./рос. vs англ., див. §3.5).

Ресурси компоненти зазвичай звільняються через RAII; зокрема, компоненти в
деструкторі вимикають логування (`ServiceTools::DisableComponentLogging`).

---

## 5. ServiceTools — наскрізні сервіси

`src/helpers/ServiceTools.*` (розбитий на `_Conversion`, `_Errors`, `_Log`,
`_Validation`) — єдина точка логування та конвертацій рядків.

### 5.1. Логування: `REPORT_*` і `NEUTRAL_REPORT_*`

Логування побудовано поверх `spdlog`; **прямий доступ до `spdlog` у компонентах
заборонено** (див. [AGENTS.md](../../AGENTS.md) «Конвенції коду»).

Макроси для методів компоненти (використовують `this` і `__func__`,
`src/helpers/ServiceTools.h:373-377`):

```cpp
#define REPORT_TRACE(msg) ServiceTools::ReportComponentEvent(this, "trace", __func__, msg)
#define REPORT_DEBUG(msg) ServiceTools::ReportComponentEvent(this, "debug", __func__, msg)
#define REPORT_INFO(msg)  ServiceTools::ReportComponentEvent(this, "info",  __func__, msg)
#define REPORT_WARN(msg)  ServiceTools::ReportComponentEvent(this, "warn",  __func__, msg)
#define REPORT_ERROR(msg) ServiceTools::ReportComponentEvent(this, "error", __func__, msg)
```

Макроси `NEUTRAL_REPORT_*` — для статичних/const-контекстів без `this`
(`ServiceTools.h:381-385`); вони делегують у `NeutralReportImpl("<level>",
__func__, ...)`. Є два перевантаження `NeutralReportImpl` — з тегом і без
(`ServiceTools.h:367-368`); якщо тег не вказано, використовується `"General"`.

**Neutral-канал прив'язано до файлового лога.** Логер `"General"` окремо ніде не
реєструється, тож раніше ВСІ `NEUTRAL_REPORT_*` (транспорти, драйвери, статичні
методи) йшли лише у fallback і не потрапляли у файл, увімкнений через
`ИспользоватьЛогирование`, — діагностика `Connect`/`TransportTCP` була невидимою
з 1С (інцидент 2026-08-29). Тепер `InitLogging` після створення/оновлення
файлового логера компоненти викликає `BindGeneralToLocked(logger)`
(`ServiceTools_Log.cpp`): `"General"` отримує ті самі sink'и й рівень (семантика
«останній `EnableLogging` виграє»; логер створюється вручну, повз глобальний
реєстр spdlog). `ShutdownLogging` переприв'язує `"General"` до будь-якого живого
компонентного логера, а якщо їх не лишилось — прибирає (інакше він тримав би
файл відкритим). Регрес-тест — `TestNeutralReportReachesFile` (`core_selftest`).

**Живий flush.** Файлові логери створюються з `flush_on(spdlog::level::info)`
(і `"General"` теж): рядки info+ (Connect/Disconnect/помилки) лягають на диск
одразу, а trace/debug (wire-дамп) буферизуються до наступного info+ або
`ShutdownLogging` — спільний sink скидає їх разом. Без цього лог, скопійований
при живому процесі 1С, обривався посеред рядка й «губив» останні події
(інцидент 2026-08-29 №2). Регрес-тест — `TestInfoFlushedWithoutShutdown`.

`ReportComponentEvent(component, level, methodName, message)` (декларація
`ServiceTools.h:365`): якщо `level == "error"`, викликає `AddComponentError`;
інакше перевіряє `IsComponentLoggingEnabled` і за потреби `AddComponentLog`.

`AddComponentError(component, description, code)`
(`src/helpers/ServiceTools_Errors.cpp:26-41`): визначає ім'я компонента через
`GetComponentName` (RTTI; для `nullptr` — `"General"`), логує рядок
`"[<name>] Ошибка компонента: <descr>, код: <code>"` через `Error(...)`, і якщо
`component != nullptr` — викликає `component->AddError(description, code)`. Тобто
саме через ядро `REPORT_ERROR` зрештою реєструє помилку в 1С, а не лише пише в
лог-файл; для `nullptr` (нейтральний контекст) повертає `true`.

> **Ключове правило:** `REPORT_ERROR` одночасно пише в лог **і** реєструє помилку
> для показу в 1С (через `AddError`).

### 5.2. Конвертації рядків: `Safe*`

`src/helpers/ServiceTools_Conversion.cpp`:

- `SafeMB2WCHAR(const char* str)` (`26-32`): `nullptr` → `u16string()`, інакше
  делегує в `AddInNative::MB2WCHAR(str)`.
- `SafeWCHAR2MB(const std::u16string& str)` (`43-51`): порожній рядок →
  `std::string()`, інакше будує `basic_string_view<WCHAR_T>` над `str.data()` і
  делегує в `AddInNative::WCHAR2MB(view)`.
- `U16StringToWString` (`62-64`): прямий `reinterpret_cast` (передбачає однакове
  представлення `char16_t` і `wchar_t` на Windows, без конвертера кодових сторінок).

> **Правило:** у прикладному коді використовувати `SafeMB2WCHAR`/`SafeWCHAR2MB`,
> а не напряму `AddInNative::MB2WCHAR`/`WCHAR2MB` (див. [AGENTS.md](../../AGENTS.md) «Конвенції коду»).

### 5.3. Інше

`ServiceTools` також надає керування логуванням з боку 1С
(`EnableComponentLogging` / `DisableComponentLogging` — рівень + файл)
і валідацію (`CalculateCRC32` у
`ServiceTools_Validation.cpp`). Детальна реалізація `_Log.cpp` і `_Validation.cpp`
у межах цього документа не розкривалася.

---

## 6. Платформенні механізми ядра (Етап 0)

Етап 0 зміцнив ядро-міст платформенними механізмами, спільними для будь-якої
компоненти. Усі — в `src/core/AddInNative.{h,cpp}` і `src/helpers/ServiceTools_Log.cpp`;
покриті харнесом `tests/core_selftest.cpp` (див. §6.9).

### 6.1. Конвенція повернення значень: `Ret()`

`MethFunction` — це `std::variant<std::function<void(...)>>` (§2.3): значення,
повернуте «голою» лямбдою-хендлером, мовчки відкидається й **не потрапляє в 1С**.
`Ret(F f)` (`AddInNative.h:140`, protected шаблон) загортає value-повертаючу
лямбду у void-хендлер, який присвоює `this->result`. Диспетч за типом результату
в `WrapRet` (`AddInNative.h:144`): `bool` → як є, цілі → `int64_t`, з рухомою
комою → `double`, решта (рядки) — напряму через `VH::operator=`. `static_assert`
забороняє `void`-лямбди (для них — `AddProcedure`).

```cpp
AddFunction(u"IsPortAvailable", u"ДоступенПорт",
    Ret([&](VH portName) { std::u16string p = portName; return this->IsPortAvailable(p); }));
```

> **Правило застосування `Ret()`:** обгортати ЛИШЕ хендлери, чиє `return`-значення
> і є результатом для 1С. **НЕ обгортати** хендлери, які самі присвоюють
> `this->result`, а повертають лише службовий `bool` (напр. `CallUapki` у
> `AddinUAPKIConnect` ставить `result = jsonResponse`, `return true` службовий) —
> `Ret()` перезаписав би корисний результат булевим статусом.

### 6.2. Реєстрація компоненти: `REGISTER_COMPONENT`

Макрос (`AddInNative.h:267`) на файловому рівні `.cpp` компоненти визначає
`CLASS::names` через `AddComponent` **і** створює анти-стрип reference (проти
відкидання TU лінкером — §1.3) одним рядком:

```cpp
REGISTER_COMPONENT(u"AddinUAPKIConnect", AddinUAPKIConnect)   // AddinUAPKIConnect.cpp:7
```

Клас мусить оголосити `static std::vector<std::u16string> names;`. Для компонент
з кількома іменами (`TestComponent` — 3 псевдоніми) макрос не годиться: лишається
ручний `names = { AddComponent(...), ... }` + окремий анти-стрип
`namespace { [[maybe_unused]] auto& _force… = TestComponent::names; }`.

### 6.3. Успадкований `EnableLogging` у базі

Конструктор базового `AddInNative` (`AddInNative.cpp:87-99`) реєструє, окрім
властивості `Version`, спільну для всіх компонент функцію
`EnableLogging`/`ИспользоватьЛогирование(рівень="info", шлях="")` — делегат у
`ServiceTools::EnableComponentLogging(this, level, path)`, обгорнутий `Ret()` і
`try/catch`. Дублікати з `AddinUAPKIConnect`, `TestComponent` прибрані. `DisableComponentLogging` лишається в деструкторах
похідних (RTTI-ім'я в базовому деструкторі вже некоректне).

### 6.4. Декларативні параметри: `ParamSpec` + валідація

`struct ParamSpec` (`AddInNative.h:97`): `nameEn`, `nameRu`, `required`,
`std::optional<DefaultHelper> byDefault`. Нові перевантаження
`AddFunction`/`AddProcedure` з `const std::vector<ParamSpec>&`
(`AddInNative.cpp:487-498`) зберігають специфікації в полі `Meth::params` і
похідні дефолти (`DefaultsFromSpecs`, `:480`) — у `MethDefaults`.

`ValidateParams` (`AddInNative.cpp:500`) виконується в `CallAsProc` (`:397`) і
`CallAsFunc` (`:416`) **ДО** хендлера: для кожного `required`-параметра без
дефолту, якщо аргумент відсутній (`i >= lSizeArray`) або `VTYPE_EMPTY`, ядро
робить `AddError` з іменем параметра (ru/en за `alias`) і методу та повертає
`false` — хендлер не викликається. Старі реєстрації (без `params`) не
валідуються — поведінка без змін.

### 6.5. Потокобезпечний міст подій: `PostExternalEvent`

```cpp
bool PostExternalEvent(const std::u16string& message, const std::u16string& data);  // AddInNative.h:226
```

Публічний, викликається з **будь-якого** потоку (фонові reader-потоки
транспортів). `source` події — ім'я компоненти в 1С (`this->name`). Реалізація
(`AddInNative.cpp:134`) робить локальні mutable-копії рядків (бо `ExternalEvent`
приймає `WCHAR_T*` без const) і під `connectMutex_` перевіряє `m_iConnect`.

Гарантія відсікання: `Init` (`:109`) і `Done` (`:127`) беруть той самий
`connectMutex_`; `Done` обнуляє `m_iConnect`, тож після повернення з `Done()`
жоден фоновий `PostExternalEvent` уже не торкнеться зв'язку з 1С (повертає
`false`). `AddError` (`:679`) теж узятий під `connectMutex_` + null-guard —
усунуто data race з `Done()` при фонових потоках. Це фундамент подій для
Етапів 1-2 (JobEngine, SimplyChannel).

### 6.6. Boot-фікси старту DLL

- **Лінива локаль з fallback.** `RuLocale()` (`AddInNative.cpp:565`) — статична
  функція, а не глобальний об'єкт: `std::locale("ru_RU.UTF-8")` може кинути
  виняток, а на етапі статичної ініціалізації DLL це = відмова завантаження
  компоненти в 1С. Fallback-ланцюг: `ru_RU.UTF-8` → `Russian_Russia.1251` →
  `std::locale::classic()`. `upper()` (§2.2) використовує `RuLocale()`.
- **Контракт `GetClassObject` 1/0.** `GetClassObject` (`AddInNative.cpp:52-60`)
  повертає `1` при успіху / `0` при відмові (`return *pInterface ? 1 : 0;`, `:59`),
  а не адресу, приведену до `long`: приведення 64-бітного вказівника до `long`
  усікало його (UB на x64). При `*pInterface != nullptr` — одразу `0` (відмова
  створювати поверх наявного).
- **Fallback-sink логера.** До першого `EnableLogging` `GetLogger`
  (`ServiceTools_Log.cpp`) повертає не «порожній» `spdlog::default_logger`, а
  `GetFallbackLogger()` — `msvc_sink_mt(false)` (OutputDebugString), рівень
  `warn`: рання діагностика старту DLL не губиться. Аргумент `false` вимикає
  перевірку `IsDebuggerPresent`: DebugView читає буфер DBWIN, але відладчиком не
  є, тож із дефолтним `check_debugger_present=true` він не бачив ЖОДНОГО рядка
  (інцидент 2026-08-29 — діагностику без Visual Studio зібрати було неможливо).

### 6.7. Захист індексів методів/властивостей

`std::next(begin, N)` при `N < 0` або `N > size()` — UB ще ДО перевірки
`it == end()`. Тому кожен метод, що приймає `lMethodNum`/`lPropNum` з платформи
(`GetNParams`, `GetParamDefValue`, `HasRetVal`, `CallAsProc`, `CallAsFunc`,
`GetMethodName`, `GetPropVal`, `SetPropVal`, `IsPropReadable`, `IsPropWritable`,
`GetPropName`), починається з охорони діапазону, напр.:

```cpp
if (lMethodNum < 0 || static_cast<size_t>(lMethodNum) >= methods.size()) return false;
```

(для вказівникових — `return nullptr;`, для `long` — відповідне безпечне значення;
див. `AddInNative.cpp:178,193,212,269,284,337,394,413` тощо).

### 6.8. Фікс дедлоку `ShutdownLogging`

`ShutdownLogging` (`ServiceTools_Log.cpp:176`) бере `loggersMutex` і під ним
раніше викликав `Info(...)`, а `Info → GetLogger` брав ТОЙ САМИЙ нерекурсивний
м'ютекс — дедлок (латентний: спрацьовував лише коли для компоненти вже існував
логер: увімкнули логування в 1С → закрили 1С → деструктор →
`DisableComponentLogging` → зависання процесу). Виправлено логуванням напряму
через об'єкт логера (`it->second->info(...)`, `:184`), без повторного захоплення
м'ютекса.

### 6.9. Харнес `core_selftest` (L0.5)

`tests/core_selftest.cpp` — L1-харнес ядра без 1С і без UAPKI: лінкує
OBJECT-бібліотеки `base_component`+`helpers_component` напряму, платформу 1С
емулює моками `MockConnect : IAddInDefBase` і `MockMemory : IMemoryManager`
(pch тут НЕ підключається — правило `tests/`). Ціль збирається завжди при
`BUILD_TESTS=ON`, незалежно від `BUILD_WITH_UAPKI` (`tests/CMakeLists.txt`).
Тест-функції покривають усі механізми §6: смоук життєвого циклу, реєстр,
boot-фікси, `Ret()`, `ShutdownLogging` без дедлоку, базовий `EnableLogging`,
валідацію `ParamSpec`, захист індексів, `PostExternalEvent` (з фонового потоку +
відсікання після `Done`), fallback-логер. У `run_tests.ps1` — рівень **L0.5**
(між L0-dumpbin і L1-selftest), проходить і без `-WithUAPKI`.
