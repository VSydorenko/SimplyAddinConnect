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
`AddInNative` (`src/core/AddInNative.h:403-404`), а `CreateObject` — **публічний**
статичний метод класу: його прямо викликає L1-харнес `core_selftest`, що
створює пробні компоненти без платформи 1С (`src/core/AddInNative.h:257-259`).
(У SDK присутня ще декларація `SetPlatformCapabilities`, але в `.def` вона не
експортується.)

Реалізації (`src/core/AddInNative.cpp`):

- `GetClassNames()` (`42-48`) повертає `AddInNative::getComponentNames()` —
  список імен, розділених `|`, закешований у `static const std::u16string names`.
- `GetClassObject(wsName, pInterface)` (`50-60`): якщо `*pInterface` уже не
  nullptr — повертає `0` (відмова створювати поверх наявного); інакше створює
  об'єкт через `AddInNative::CreateObject(cls_name)` і повертає
  `long(*pInterface = ...)` — тобто ознакою успіху є ненульова адреса,
  приведена до `long`.
- `DestroyObject(pInterface)` (`62-68`): якщо `*pInterface == nullptr` → `-1`;
  інакше `delete *pInterface; *pInterface = nullptr; return 0;`.

### 1.2. Реєстр компонент і статична реєстрація

Одна DLL може містити багато компонент. Реєстр — мапа «ім'я 1С → фабрика», але
вона НЕ звичайний статичний член класу:

```cpp
static std::map<std::u16string, CompFunction>& components();          // AddInNative.h:415, приватний
static std::u16string AddComponent(name, creator);                    // AddInNative.h:256, публічний
static AddInNative* CreateObject(const std::u16string& name);         // AddInNative.h:259, публічний
```

`components()` (`AddInNative.cpp:70-73`) — **функціо-локальний статик
(Meyers singleton)**: мапа `registry` оголошена `static` УСЕРЕДИНІ тіла функції
й будується при першому виклику, а не як звичайний статичний член класу.
Причина: сама реєстрація компонент (`REGISTER_COMPONENT`, §6.2) іде через
статичну ініціалізацію — кожен `.cpp` компоненти під час старту DLL викликає
`AddComponent` з ініціалізатора власного статичного члена `names`. Порядок, у
якому виконуються статичні ініціалізатори РІЗНИХ одиниць трансляції,
стандартом не визначений («static initialization order fiasco»): якби
`components` була звичайною статичною мапою-членом, виклик `AddComponent` з
одного TU міг би спрацювати РАНІШЕ, ніж сама мапа встигла б сконструюватися.
Функціо-локальний статик такої проблеми не має — компілятор гарантує побудову
`registry` рівно один раз, при першому виклику `components()`, з якого б TU той
виклик не прийшов.

- `AddComponent(name, creator)` (`AddInNative.cpp:360-364`) вставляє пару
  `components().insert({name, creator})` і **повертає те саме `name`** — щоб
  результат можна було використати як елемент ініціалізатора статичного вектора.
- `CreateObject(name)` (`AddInNative.cpp:366-372`) шукає фабрику в
  `components()`, викликає її (`it->second()`), проставляє `object->name = name`
  і повертає вказівник; якщо ім'я не зареєстроване — `nullptr`.

**Кожен запис реєстру з'являється статично, до `main`**, через побічний ефект
ініціалізації статичного члена класу-нащадка (сама мапа-реєстр — лінива, див.
вище). Компонента з одним ім'ям реєструється макросом `REGISTER_COMPONENT`
(§6.2), який визначає `names` і додає анти-стрип reference одним рядком;
компонента з кількома іменами — ручним `names = { AddComponent(...), ... }` +
окремий анти-стрип:

- `src/components/AddinUAPKIConnect.cpp:8` — `REGISTER_COMPONENT(u"AddinUAPKIConnect", …)`.
- `src/TestComponent.cpp:5-9` — ручний `TestComponent::names` (`u"AddInNative"`,
  `u"SimplyAddinConnect"`, `u"SimplyConnect"`) + анти-стрип `_forceTestComponentNames`
  (`:11`).

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

Крім `AddinUAPKIConnect`, `ECRPrivatJSON` і трьох псевдонімів `TestComponent`,
той самий механізм реєструє й `LabelPrinter` та БПО-фасади `ECRPrivatBPO3004` /
`ECRPrivatBPO4000` — повний і актуальний перелік підтримує
[README.md](README.md) («Каталог підсистем»), тут його свідомо не дублюємо, щоб
знову не розійшовся з кодом. Драйвер **ECRPrivatJSON**
(`src/drivers/ecr_privatjson/`) — перший драйвер обладнання на фундаменті
device-core; фасад `AddinECRPrivatJSON` (`src/components/AddinECRPrivatJSON.*`)
через `REGISTER_COMPONENT` (`AddinECRPrivatJSON.cpp:6`) реєструє компоненту 1С
`ECRPrivatJSON`, що делегує драйверу (див. [ecrprivatjson.md](ecrprivatjson.md),
[device-core.md](device-core.md)).

---

## 2. Модель методів і властивостей

Компонента описує свій API реєструючими методами ядра
(`src/core/AddInNative.h:219-228`):

```cpp
void AddProperty (nameEn, nameRu, const PropFunction& getter, const PropFunction& setter = nullptr);
void AddProcedure(nameEn, nameRu, const MethFunction& handler, const MethDefaults& defs = {});
void AddFunction (nameEn, nameRu, const MethFunction& handler, const MethDefaults& defs = {});
// + перевантаження AddProcedure/AddFunction з декларативним const std::vector<ParamSpec>& (§6.4)
```

Обробники — лямбди, тому шаблонні описи API не потрібні. Точок реєстрації
публічно п'ять: одна для властивостей (`AddProperty`) і чотири методних (по два
перевантаження `AddProcedure` і `AddFunction` — з `MethDefaults` або з
`ParamSpec`); усі чотири методні зрештою викликають один приватний
`RegisterMethod` (§2.1), щоб індекс наповнювався в одному місці.

### 2.1. Зберігання

Дескриптори — прості структури (`src/core/AddInNative.h`):

```cpp
struct PropDesc {                        // :284-289, приватний вкладений тип
    std::u16string nameEn;
    std::u16string nameRu;
    PropFunction   getter;
    PropFunction   setter;                // порожній -> властивість лише для читання
};

struct MethDesc {                         // :292-299
    std::u16string nameEn;
    std::u16string nameRu;
    MethFunction   handler;
    MethDefaults   defaults;
    std::vector<ParamSpec> params;
    bool           hasRetVal = false;
};

std::vector<PropDesc> props_;             // :301
std::vector<MethDesc> meths_;             // :302
std::unordered_map<std::u16string, long> propIndex_;   // :307, ключ — NormalizeName(ім'я)
std::unordered_map<std::u16string, long> methIndex_;   // :308
```

Жодних вкладених `std::vector<std::u16string> names` більше немає: кожен
дескриптор тримає `nameEn`/`nameRu` прямими полями, а пошук за іменем іде через
окремий індекс (§2.2), а не через сам вектор дескрипторів.

- `AddProperty` (`AddInNative.cpp:374-384`) пуш-бекає `PropDesc{nameEn, nameRu,
  getter, setter}` у `props_` і одразу реєструє обидва нормалізовані імена в
  `propIndex_` через `try_emplace` (нижче).
- `AddProcedure`/`AddFunction` (обидва перевантаження) не пушать у `meths_`
  напряму: усі чотири (`AddInNative.cpp:386-389`, `391-394`, `418-422`,
  `424-428`) делегують у приватний `RegisterMethod`
  (`AddInNative.cpp:397-407`), який пуш-бекає `MethDesc{nameEn, nameRu,
  handler, defs, params, hasRetVal}` і наповнює `methIndex_`. Спільна точка
  потрібна саме тому, що точок виклику чотири — індекс мав би наповнюватися
  однаково з кожної.
- Дублікат імені (та сама англійська чи російська назва зареєстрована двічі) —
  `try_emplace` НЕ перезаписує вже наявний ключ, тож перемагає **перша**
  реєстрація; друга мовчки не потрапляє в індекс (лишається в самому векторі,
  але знайти її за іменем не можна).

Різниця «процедура vs функція» — це, як і раніше, лише прапорець `hasRetVal`,
який повертає `HasRetVal()` (`AddInNative.cpp:290-294`).

### 2.2. Два імені (en/ru) і пошук через індекс

`PropDesc`/`MethDesc` тримають `nameEn`/`nameRu` прямими полями (§2.1), без
вкладеного вектора. `GetPropName` (`AddInNative.cpp:186-193`) і `GetMethodName`
(`AddInNative.cpp:237-244`) читають ці поля напряму за `lPropAlias`/
`lMethodAlias`, де `0` — міжнародний псевдонім, `1` — російський (за
коментарем SDK `include/ComponentBase.h:79-80,134-135`); порожнє `nameRu` при
`alias == 1` підміняється на `nameEn`.

Пошук за іменем — **одне звернення до `unordered_map`**, а не двоетапний скан.
`FindProp` (`AddInNative.cpp:174-180`) і `FindMethod` (`AddInNative.cpp:229-235`)
нормалізують вхідний рядок через `NormalizeName` (`AddInNative.cpp:154-167`,
приватний статичний метод) і шукають ключ у `propIndex_`/`methIndex_`
(`std::unordered_map<std::u16string, long>`, §2.1) — `O(1)` замість лінійного
обходу вектора.

`NormalizeName` — **таблична**, не через локаль: `a-z` зсувом ASCII, кирилиця
`а-я`/`ё` і `ѐ-џ` (зокрема **і, ї, є**) — зсувом за діапазонами UTF-16, без
жодного звернення до `std::locale`/`std::toupper`. Свідомий вибір: компонента
мусить розпізнавати ті самі імена методів **незалежно від локалі машини**, на
якій запущено 1С, а не лише тоді, коли ОС має встановлений `ru_RU.UTF-8`
(порівняй зі знятим боот-фіксом локалі — §6.6: `NormalizeName` більше взагалі
не звертається до `std::locale`). **`ґ`/`Ґ` (U+0490/U+0491) свідомо НЕ
згортаються** — вони лежать поза діапазоном `0450-045F`, і жодне зареєстроване
ім'я в `src/components` чи `src/drivers` цих літер не містить, тож покриття не
потрібне (коментар прямо в коді, `AddInNative.cpp:161-162`).

Публічні `upper(std::u16string&)`/`upper(std::wstring&)`
(`AddInNative.cpp:498-511`) лишаються — на них спирається код поза ядром — але
тепер це тонкі обгортки над `NormalizeName`, а не власна реалізація через
локаль.

Дублікат імені після нормалізації (наприклад, `FindProp("MyMethod")` і
`FindProp("mymethod")` дають той самий ключ) резолвиться на **перше**
зареєстроване — наслідок `try_emplace` у §2.1. Це трохи інакше, ніж у старому
лінійному пошуку: той спершу пробував **точний** (чутливий до регістру) збіг і
лише потім згортав регістр, тож два імені, що різнилися РІВНО регістром, могли
на точний запит віддати саме той запис, а не обов'язково перший зареєстрований
(коментар у коді, `AddInNative.cpp:378-381`). Новий індекс проміжного етапу
точного збігу не має — перемагає порядок реєстрації, а не точність запиту.

### 2.3. Арність методів: `MethFunction0..16`, породжені, а не виписані

Обробник методу — `std::variant` лямбд від 0 до `kMaxArity` (=16) аргументів,
але сімнадцять псевдонімів `MethFunctionN` і сам варіант більше НЕ виписані
руками — вони породжуються з `std::index_sequence` (`AddInNative.h:173-199`):

```cpp
template <size_t N> using MethFunctionN = /* std::function<void(VH×N)>, з HandlerOf */;
using MethFunction = /* std::variant<MethFunctionN<0>, ..., MethFunctionN<kMaxArity>>, з HandlerVariantOf */;
static constexpr size_t kMaxArity = 16;
```

Побудова з `index_sequence` виключає описку в одному з N рядків за
конструкцією — раніше довелося б виписувати 17 псевдонімів руками. Чотири
`static_assert` (`AddInNative.h:204-217`) перевіряють не рукописний список
(його вже немає), а те, що побудова дає обіцяне: розмір варіанта —
`kMaxArity + 1`, альтернатива `0` — хендлер без параметрів, альтернатива `1` —
рівно один `VH`, альтернатива `9` (потрібна БПО-контракту,
`ОплатитьПлатежнойКартой`) — рівно дев'ять, остання альтернатива — рівно
`kMaxArity`.

Диспетчеризація в `CallMethod` (`AddInNative.cpp:296-302`) — теж породжена:
`TryEachArity` (`AddInNative.h:381-386`) згортає `TryCallArity<N>`
(`AddInNative.h:355-375`) по `||` для всіх `N` від `0` до `kMaxArity`.
`TryCallArity<N>` дістає з `variant` альтернативу через `std::get_if`; якщо це
не та арність — `false` (диспетчер іде далі); якщо арність та, але
`lSizeArray < N` — **`AddError`** з ім'ям методу (укр./рос. за `alias`, з тим
самим fallback на порожнє `nameRu`, що й у `GetMethodName`) і `false`. Раніше
на брак параметрів летів голий `std::bad_function_call()`, який зовнішній
`catch(...)` мовчки гасив у `false` — без жодного повідомлення 1С-розробнику;
явна перевірка лишає той самий `false`, але з діагностикою
(`AddInNative.h:360-371`).

`GetNParams` (`AddInNative.cpp:246-254`) повертає `handler.index()`:
альтернативи впорядковані **за арністю**, тож індекс variant-а і Є кількістю
параметрів — той самий інваріант, закріплений `static_assert`-ами вище. Перед
цим — перевірка `handler.valueless_by_exception()` (variant без активної
альтернативи після винятку під час emplace) → `0`.

> **Чому 16, а не 7 (розширено 2026-08-31).** Контракт «Подключаемое оборудование» має методи
> на 9-10 параметрів (`ОплатитьПлатежнойКартой` — 9, `ОтменитьПлатежПоПлатежнойКарте` — 10).
> До розширення такий метод **не реєструвався взагалі**: гілки в `CallMethod` не було, а
> `GetNParams` тихо віддавав `0`, і 1С вважала метод безпараметровим. Помилки при цьому не було
> ніде — ні при збірці, ні під час виконання.
>
> **Одне ім'я методу = одна арність.** `GetNParams` віддає для методу рівно одне число, тож
> зареєструвати той самий метод із різною кількістю параметрів **неможливо**. Саме тому
> БПО-фасади робляться окремими класами на кожне сімейство сигнатур — див.
> [bpo-contract.md](bpo-contract.md) §2.2.

### 2.4. Значення за замовчанням: `MethDefaults` / `DefaultHelper`

`using MethDefaults = std::map<long, DefaultHelper>` (`AddInNative.h:158`) —
відображення «номер параметра → значення за замовчанням».

`DefaultHelper` (`AddInNative.h:27-74`) оголошений ще **перед** `AddInNative`
(бо `MethDefaults`/`ParamSpec` потрібні класу як типи параметрів методів
реєстрації) — обгортка над приватним `Slot` — `std::variant<Unset,
std::u16string, int64_t, double, bool>` (тег «немає дефолту» — окремий
порожній тип `Unset`, не `std::monostate`, щоб альтернатива читалась за
іменем у діагностиці компілятора) — з конструкторами від `u16string`,
`int64_t`, `double`, `bool` і `const char16_t*` (де `nullptr` дає `Unset`).
Порядок альтернатив `Slot` закріплено чотирма `static_assert`-ами
(`AddInNative.h:62-71`) — на ньому тримається `switch (slot_.index())` у
`Apply` нижче.

Розбір дефолту більше НЕ живе всередині самого `GetParamDefValue`: він
винесений у `DefaultHelper::Apply(tVariant*, AddInNative*)`
(`AddInNative.cpp:261-271`) — `Apply` конструює тимчасовий `VariantHelper` над
переданим `tVariant*` і пише в нього через `operator=` за `slot_.index()`;
альтернатива `0` (`Unset`) не пише нічого — комірка вже приведена викликачем
до `VTYPE_EMPTY`. Доступ до `protected VariantHelper` дає `friend class
DefaultHelper;` (`AddInNative.h:410`) — потрібен саме тому, що `DefaultHelper`
не може назвати вкладений тип `AddInNative::VariantHelper` у власному
оголошенні: у його точці `AddInNative` ще лише forward-declared.

`GetParamDefValue` (`AddInNative.cpp:273-288`) — тепер тонкий читач цієї
структури: спершу **безумовно** очищає передану комірку до `VTYPE_EMPTY`
(`VA(pvarParamDefValue).clear()`), і лише ПОТІМ перевіряє межі `lMethodNum`;
якщо метод знайдено — шукає дефолт для `lParamNum` у `MethDesc::defaults` і за
наявності викликає `DefaultHelper::Apply`.

> **Єдиний інвертований guard у файлі.** Кожен інший метод, що приймає
> `lMethodNum`/`lPropNum` з платформи, на невалідному індексі повертає `false`
> або `nullptr` (§6.7). `GetParamDefValue` на невалідному `lMethodNum`
> (`AddInNative.cpp:281`) повертає `true`. Причина — порядок дій: очищення
> комірки виконується **до**, а не після перевірки меж, бо викликач передає
> комірку саме під «дефолту немає», і вона мусить лишитися валідним
> `VTYPE_EMPTY`, навіть коли метод чи параметр не знайдено — а не чужим сміттям
> з попереднього виклику. З погляду платформи `true` тут означає «значення за
> замовчуванням успішно описано (як відсутнє)», а не «метод існує»; ці два
> факти в цьому методі навмисно розведені.

---

## 3. VariantHelper (VH) — міст типів

`VariantHelper` (скорочено `VH`, `using VH = VariantHelper;` на
`AddInNative.h:157`) — protected вкладений клас-обгортка над `tVariant`
(варіантний тип платформи 1С). Оголошення: `src/core/AddInNative.h:84-155`.

Ядро адаптера — два явні шаблонні методи, `Get<T>()`/`Set<T>()`
(`AddInNative.h:107-108`, лише декларація в тілі класу); увесь звичний набір
операторів (`operator=`, `operator T() const`) — тонкі однорядкові обгортки
над ними, означені в `.cpp` (`AddInNative.cpp:766-779`), а НЕ inline у тілі
класу. Причина: виклик `Get<T>()`/`Set<T>()` з функції, визначеної ВСЕРЕДИНІ
тіла класу, компілюється в complete-class context одразу після закриття
`VariantHelper` (клас `AddInNative` закривається на `AddInNative.h:470`) —
тобто ще ДО того, як компілятор побачить explicit-спеціалізації `Get<T>()`/
`Set<T>()`, оголошені за межами класу, у просторі імен
(`AddInNative.h:479-491`) — вони й фізично не можуть стояти раніше:
explicit-спеціалізація вкладеного шаблону методу мусить бути в просторі
імен, а `AddInNative` на той момент ще не закрився. Наслідок за inline-
визначення в тілі класу — MSVC C2908 «явная специализация; уже создан
экземпляр»: компілятор мовчки інстанціює primary-шаблон РАНІШЕ оголошення
спеціалізації. Тому в тілі класу лишаються лише декларації
(`AddInNative.h:116-139`), а самі спеціалізації `Get<T>`/`Set<T>` і тіла
операторів-обгорток над ними — у `.cpp`, нижче за спеціалізації.

### 3.1. Контекст

Поля (`AddInNative.h:86-91`):

```cpp
tVariant*        pvar   = nullptr;   // сам варіант 1С
AddInNative*     addin  = nullptr;   // ядро (для доступу до пам'яті та AddError)
const PropDesc*  prop   = nullptr;   // прив'язана властивість (лише контекст для тексту помилки)
const MethDesc*  meth   = nullptr;   // прив'язаний метод
long             number = -1;        // номер параметра
```

`prop`/`meth` — `const`-покажчики й нічим не володіють: коментар у коді прямо
це підкреслює («Контекст ЛИШЕ для тексту помилки: чиє це значення. Не володіє
нічим», `AddInNative.h:88`). Контекст (`prop`/`meth`/`number`) потрібен, щоб
формувати змістовні повідомлення про помилку типу (§3.5).

### 3.2. Конвертації з `tVariant` у C++ (`operator T() const` → `Get<T>()`)

Кожен оператор — однорядкова обгортка над `Get<T>()` (`AddInNative.cpp:773-779`);
уся логіка — у явних спеціалізаціях `Get<T>()`:

| Оператор | Обгортка | Спеціалізація `Get<T>` | Поведінка |
|---|---|---|---|
| `std::string` | `:773` | `644-648` | через `Get<u16string>()`, потім `WCHAR2MB` |
| `std::wstring` | `:774` | `650-655` | через `Get<u16string>()`, потім `WCHAR2WC` |
| `std::u16string` | `:775` | `632-641` | вимагає `VTYPE_PWSTR`, інакше `TypeError(VTYPE_PWSTR)`; `pwstrVal == nullptr` (не мало б траплятись за коректного `VTYPE_PWSTR`) → порожній рядок, а не розіменування нуля |
| `int64_t` | `:776` | `657-662` | `ReadNumeric<int64_t>(VTYPE_I4)` — дробову частину відкидає приведення |
| `int` | `:779` | — | `static_cast<int>(Get<int64_t>())`, окремої спеціалізації немає |
| `double` | `:777` | `664-668` | `ReadNumeric<double>(VTYPE_R4)` |
| `bool` | `:778` | `670-682` | НЕ через `ReadNumeric` (див. нижче); `VTYPE_BOOL` → `TV_BOOL`, цілі різновиди → `lVal != 0`, інакше `TypeError(VTYPE_BOOL)` |

(Спеціалізації й обгортки — у `src/core/AddInNative.cpp`.)

`int64_t`/`double` (і `int`, що будується поверх `int64_t`) ідуть через один
спільний приватний `ReadNumeric<N>(TYPEVAR expectedForError)`
(`AddInNative.cpp:521-538`) замість трьох окремих `switch`:
`I2`/`I4`/`UI1`/`ERROR` читаються з `lVal`, `R4` — з **`fltVal`**, `R8` — з
`dblVal`; будь-який інший тип кидає `TypeError(expectedForError)`.

> ⚠️ **Свідома зміна поведінки: `VTYPE_R4` тепер читається з `fltVal`, а не з
> `dblVal`.** Успадкований код на `VTYPE_R4` читав `dblVal` — тобто
> інтерпретував байти 4-байтового `float` як 8-байтовий `double`; практично
> значення на кшталт `2.5f` поверталося як ≈5.3e-315. Це була вада, а не риса:
> `TV_R4` — це `fltVal` (`include/types.h:179`), `TV_R8` — `dblVal` (`:180`), і
> це РІЗНІ члени об'єднання `tVariant`. Виправлено як частину переписування
> (рішення власника 2026-09-08), закріплено регрес-тестом
> `TestFloatR4Conversion` (`tests/core_selftest.cpp`) — на старій реалізації він
> падає 3/3 (перевірено відкатом фіксу й перезбіркою). Сторона запису не
> зачеплена: компонента `VTYPE_R4` сама не породжує, лише читає, коли його
> присилає платформа.

`Get<bool>()` НЕ ділить набір типів із числовим читанням навмисно: дійсні числа
до булевого не приймаються, бо «0.0 це Ложь?» — питання без однозначної
відповіді в термінах 1С (коментар прямо в коді, `AddInNative.cpp:673-674`).

### 3.3. Конвертації з C++ у `tVariant` (`operator=` → `Set<T>()`)

Так само — оператори (`AddInNative.cpp:766-771`) тонко обгортають `Set<T>()`:

| Аргумент | Обгортка | Спеціалізація `Set<T>` | Поведінка |
|---|---|---|---|
| `const std::string&` | `:766` | `703-707` | делегує в `Set<u16string>(MB2WCHAR(value))` |
| `const std::wstring&` | `:767` | `709-715` | безумовний reinterpret у `u16string` — проєкт Windows-only, `sizeof(wchar_t)==2` гарантовано, гілки на `WC2MB` більше немає (сам гелпер видалено як мертвий код) |
| `const std::u16string&` | `:768` | `689-701` | `VTYPE_PWSTR`; `clear()`, виділяє пам'ять через `addin->AllocMemory`, `memcpy`, підрізає кінцеві нулі (`wstrLen`) |
| `int64_t` | `:769` | `717-733` | `clear()`, `VTYPE_I4`, якщо влазить у `INT32_MIN..INT32_MAX`, інакше `VTYPE_R8` |
| `double` | `:770` | `735-743` | `clear()`, `VTYPE_R8` |
| `bool` | `:771` | `745-753` | `clear()`, `VTYPE_BOOL` |

Усі спеціалізації `Set<T>`, крім `Set<u16string>` (яка сама викликає `clear()`
першим рядком), теж починають з `clear()` — але **лише якщо `pvar != nullptr`**:
відв'язаний `result` (`CallAsProc` обнуляє `pvar`, §3.6) робить присвоєння тихим
no-op, а не винятком. Це протилежно до `Get<T>()`, де `nullptr`-`pvar` кидає
`bad_variant_access` через `Bound()` (§3.4) — читання відв'язаного `result`
уважається помилкою програміста, запис у нього під час виклику-як-процедури —
ні (`TestRetViaCallAsProc`).

Завдяки цьому в лямбдах пишемо звичайний C++ (`std::string s = param;
param = result;`), а перетворення на/з `tVariant` та виділення пам'яті під рядки
ядро робить саме.

> ⚠️ **Конвертації НЕ толерантні — читати чужий тип у рядок КИДАЄ.**
> `operator std::u16string()` (а отже й `std::string`) вимагає рівно `VTYPE_PWSTR`. Тому
> `const std::string v = param;` на параметрі, який 1С прислала **числом** або **булевим**,
> кине виняток. Це не гіпотетика: у формі налаштувань БПО параметр, оголошений як `Number`
> (напр. `Port`), приходить саме числом — фасад мовчки не встановлював параметр, а причина
> виглядала як «не задано параметри підключення».
>
> Числові конвертації теж не покривають усього: `operator int64_t()` / `operator double()`
> приймають `I2/I4/UI1/ERROR/R4/R8` і кидають на `I8`, `UI2`, `UI4`, `UI8`, `INT`, `UINT`.
>
> **Правило:** будь-яке значення, тип якого визначає ПЛАТФОРМА (параметри БПО, аргументи від
> прикладного коду), читай через толерантний хелпер із перевіркою `type()`, а не прямим
> приведенням. Зразок — `VariantToString`/`VariantToDouble` у `BpoFacadeBase` (спільні для всіх
> БПО-фасадів).
>
> Зміряно на живій 1С 8.3.27: **число з форми налаштувань приходить як `VTYPE_R8`** — тобто
> для сум `operator double()` достатньо, а дробова частина не втрачається.

### 3.4. Очищення, бінарні дані

`clear()` (`AddInNative.cpp:557-566`): для `VTYPE_BLOB`/`VTYPE_PWSTR` звільняє
попередню пам'ять через `addin->FreeMemory(&TV_WSTR(pvar))`, потім
`tVarInit(pvar)`. Перевірку прив'язки робить `Bound()`
(`AddInNative.cpp:515-519`, приватний) — єдина точка, де `pvar == nullptr`
кидає `std::bad_variant_access`; усі читання (`Get<T>`, `type()`, `size()`,
`data()`) і сам `clear()` ходять саме через неї, щоб цей guard існував в
одному екземплярі, а не повторювався в кожній спеціалізації.

`AllocMemory(unsigned long size)` (`AddInNative.cpp:755-761`) готує варіант під
бінарні дані: `clear()`, потім `addin->AllocMemory((void**)&pvar->pstrVal, size)`,
виставляє `VTYPE_BLOB` і `pvar->strLen = size`. Методи `size()` / `data()`
(`AddInNative.cpp:545-549`, `551-555`) доступні лише для `VTYPE_BLOB`, інакше
кидають `TypeError(VTYPE_BLOB)`.

### 3.4б. IN/OUT-параметри: перезапис прийнятого значення

Параметр методу — це слот `tVariant`, у який компонента може **писати**, і платформа читає
його назад. Так побудований увесь контракт БПО: напр. `ОплатитьПлатежнойКартой` приймає
`НомерКарты`/`НомерЧека`/`СсылочныйНомер`/`КодАвторизации` заповненими й повертає зміненими.

Механіка: `param = value;` → `clear()` → якщо в слоті вже лежав `VTYPE_PWSTR`, його пам'ять
звільняється через `addin->FreeMemory` → далі `AllocMemory` під нове значення. Тобто
**компонента звільняє рядок, який виділила платформа** — і це коректно, бо менеджер пам'яті
один і той самий (`setMemManager`).

Перевірено на живій 1С 8.3.27: читання вхідного значення й перезапис того самого слоту
працюють для рядків і чисел, багаторядковий текст проходить межу без втрат, тип числа
зберігається.

⚠️ **Наслідок для тестових харнесів.** Якщо харнес подає у IN/OUT-параметр вказівник на
`c_str()` власного рядка, `FreeMemory` (у харнесі — `free`) отримає не-malloc'ований вказівник
→ `STATUS_HEAP_CORRUPTION`. Вхідні рядки в харнесі **обов'язково** виділяти тим самим
менеджером, що віддано в `setMemManager`. Зразок — `tests/ecr_native_host.cpp`.

### 3.5. Помилки типу

`error(TYPEVAR)` перейменовано на `VariantHelper::TypeError(TYPEVAR expected)
const` (`AddInNative.cpp:606-627`) — тексти повідомлень лишились
**побайтово тими самими** (їх бачить 1С-розробник у власному коді, міняти сенсу
не було), змінилась лише структура: замість переліку `if`/конкатенацій —
таблиця `TypeName(TYPEVAR, bool alias)` (`AddInNative.cpp:584-604`, статична
вільна функція, `map<TYPEVAR, pair<en, ru>>`), яку `TypeError` двічі викликає
— для очікуваного й фактичного типу.

`TypeError` формує локалізоване повідомлення (укр./рос. або англ. — залежно
від `addin->alias`) на кшталт «Error getting value of property ... / when
calling method ... parameter ... expected ... actual value ...», **реєструє
його через `addin->AddError(...)`** (усередині самого `TypeError`, до
`return`) і повертає об'єкт-виняток `std::bad_typeid`. `throw` на виклику
служить лише для переривання потоку — `AddError` уже відбувся. `TypeError`
тепер кличуть більше місць, ніж раніше (бо `ReadNumeric`, `size()`, `data()` і
частина `Get<T>` теж кидають через нього): `ReadNumeric<N>` (`:536`),
`size()`/`data()` (`:547,553`), `Get<u16string>` (`:635`), `Get<bool>` (`:681`).
Ловлять цей виняток (як і будь-який інший) не в самих цих методах, а на межі
виклику з платформи — через спільний `Guarded` (§3.7).

### 3.6. Повернення результату функції: `this->result` і спільний `Dispatch`

`VariantHelper result;` — публічний член (`AddInNative.h:260`), ініціалізований
у конструкторі `result(nullptr, this)` (`AddInNative.cpp:75`).

Рибіндинг `result` іде **лише** через `operator<<` (`AddInNative.h:97`), не
через `operator=`: копіювальне присвоєння `VariantHelper& operator=(const
VariantHelper&) = delete` (`AddInNative.h:101`) — навмисний guard. Без нього
рядок `this->result = f(a...)` усередині `WrapRet` (§6.1) міг би мовчки
скомпілюватись як рибіндинг самого адаптера `result` (переприв'язка на чужий
`pvar`/`addin`/`prop`/`meth`), замість запису значення в комірку, на яку
`result` уже вказує, — і 1С отримала б не той результат. `= delete`
перетворює таку помилку на помилку компіляції.

`CallAsProc` і `CallAsFunc` НЕ побудовані одне на одному — обидва делегують у
спільний приватний `Dispatch(n, paParams, lSizeArray)`
(`AddInNative.cpp:309-315`), який робить межі індексу методу, `ValidateParams`
(§6.4) і сам виклик хендлера через `Guarded` (§3.7); прив'язкою `result`
керує **лише** сторона виклику:

- `CallAsProc` (`AddInNative.cpp:317-324`): `result << VA(nullptr)` (скидання
  прив'язки — функцію викликано як процедуру, писати нікуди) → `Dispatch(...)`.
- `CallAsFunc` (`AddInNative.cpp:326-336`): `result << VA(pvarRetValue)`
  (прив'язка до вихідної комірки) → `Dispatch(...)` → **безумовно**
  `result << VA(nullptr)` (від'єднання) — і за успіху, і за відмови, бо
  `Dispatch` уже проковтнув усі винятки через `Guarded` і завжди повертає
  `bool`, а не кидає.

Композиція «`CallAsFunc` = bind + `CallAsProc` + unbind» була б коротшою, але
НЕ використовується навмисно: `CallAsProc` відв'язує `result` на самому вході
(перший рядок вище) — якби `CallAsFunc` викликав `CallAsProc` після власного
`bind`, цей перший рядок стер би прив'язку до `pvarRetValue` РАНІШЕ, ніж
хендлер устиг би через `Ret()` щось у неї записати.

### 3.7. Спільна обгортка винятків: `Guarded`

`std::u16string → AddError + false`, будь-що інше → тихий `false` — один і той
самий ланцюг раніше повторювався дослівно в кількох місцях. Тепер це один
приватний member-шаблон (`AddInNative.h:334-340`):

```cpp
template <typename Body>
bool Guarded(Body&& body)
{
    try { return body(); }
    catch (const std::u16string& msg) { AddError(msg); return false; }
    catch (...) { return false; }
}
```

Шаблон — саме member (не вільна функція), щоб `AddError` у catch-гілці
резолвився на `this` без явної передачі. Виклики: `GetPropVal`
(`AddInNative.cpp:207-215`), `SetPropVal` (`:217-222`), `GetParamDefValue`
(`:273-288`, §2.4), `Dispatch` (`:309-315`, §3.6) — кожен передає лямбду з
власним тілом, а сам ланцюг `try/catch` існує в коді рівно один раз.

---

## 4. Пам'ять і життєвий цикл

### 4.1. `IMemoryManager` — пам'ять для рядків, що йдуть у 1С

`include/IMemoryManager.h:15-32` — інтерфейс з двома чисто віртуальними методами:
`AllocMemory(void** pMemory, unsigned long ulCountByte)` і
`FreeMemory(void** pMemory)`.

- Приватне поле ядра `IMemoryManager* m_iMemory = nullptr;`
  (`src/core/AddInNative.h:466`).
- Приватні обгортки `AllocMemory` / `FreeMemory` (`AddInNative.cpp:449-452`,
  `454-457`) делегують у `m_iMemory->...`, якщо той не null (інакше
  `AllocMemory` повертає `false`, `FreeMemory` — no-op).
- `setMemManager(void* memory)` (`AddInNative.cpp:108-112`):
  `return m_iMemory = static_cast<IMemoryManager*>(memory);` — присвоєння в умові
  `return`, результат `true`, якщо `memory != nullptr`.

Напряму керувати цією пам'яттю прикладному коду не треба — це робить `VH` (див. §3.3-3.4).

Той самий `m_iMemory` живить і ДРУГИЙ примітив алокації — для рядків, що
повертаються через `WCHAR_T**`/`const WCHAR_T*` OUT-параметри SDK
(`GetPropName`, `GetMethodName`, `RegisterExtensionAs`), а не через `tVariant`:

- `AllocString(const std::u16string& src) const` (приватний, оголошення
  `AddInNative.h:401`, реалізація `AddInNative.cpp:784-793`) — примітив
  алокації-й-копіювання: `nullptr`, якщо менеджера ще немає або алокація
  провалилась.
- `W(const char16_t* str) const` (публічний, оголошення `AddInNative.h:276`,
  реалізація `AddInNative.cpp:795-800`) — тонка обгортка над `AllocString`, яка
  на тій самій невдачі **кидає** `std::bad_alloc` замість повернення `nullptr`
  (контракт інший: `W()` використовують місця на кшталт
  `RegisterExtensionAs`/`REGISTER_COMPONENT`, де порожній результат
  неприпустимий, а `GetPropName`/`GetMethodName` користуються `AllocString`
  напряму й самі повертають `nullptr` платформі).

### 4.2. `IAddInDefBase` — зворотний зв'язок з платформою

`include/AddInDefBase.h:35-115` — інтерфейс 1С: `AddError`, `Read`/`Write`
властивостей, `RegisterProfileAs`, `SetEventBufferDepth`/`GetEventBufferDepth`,
`ExternalEvent`, `CleanEventBuffer`, `SetStatusLine`/`ResetStatusLine`.

- Приватне поле `IAddInDefBase* m_iConnect = nullptr;` (`src/core/AddInNative.h:467`).
- `Init(void* pConnection)` (`AddInNative.cpp:97-106`): бере `connectMutex_`
  (`:101`, той самий, що й `Done`/`AddError`/`PostExternalEvent` — §6.5),
  зберігає `m_iConnect`, і якщо він не null — виставляє
  `m_iConnect->SetEventBufferDepth(100)`; повертає `m_iConnect != nullptr`.
- `AddError(descr, scode=0)` (`AddInNative.cpp:568-577`): формує джерело
  `u"AddIn." + name`, під тим самим `connectMutex_` (`:574`) викликає
  `m_iConnect->AddError(ADDIN_E_IMPORTANT, source, descr, scode)`
  (`ADDIN_E_IMPORTANT = 1003`, `include/types.h:51`); повертає `false`, якщо
  `m_iConnect == nullptr`.

### 4.3. Життєвий цикл SDK

Ядро реалізує `IComponentBase` (`class AddInNative : public IComponentBase`,
`src/core/AddInNative.h:78`), який успадковує `IInitDoneBase`,
`ILanguageExtenderBase`, `LocaleBase` (`include/ComponentBase.h:213-220`).

Порядок викликів платформи (SDK 1С):

```
Init → setMemManager → GetInfo → робота → Done
```

- `GetInfo()` (`AddInNative.cpp:114-118`) завжди повертає `2000` (версія 2 SDK 1С).
- `Done()` (`AddInNative.cpp:120-127`) — **не порожня**: під `connectMutex_`
  обнуляє `m_iConnect`, щоб фонові потоки транспортів (`PostExternalEvent`,
  §6.5) після завершення роботи компоненти більше не намагались достукатись до
  вже недійсного зв'язку з 1С. Решта ресурсів компоненти звільняється через
  RAII/деструктор, а не тут явно.
- `SetLocale(locale)` (`AddInNative.cpp:338-344`, частина `LocaleBase`):
  конвертує локаль через `WCHAR2MB` і виставляє `this->alias` в результат
  `loc.compare(0, 3, "rus") == 0` — прапорець мови повідомлень про помилки
  (укр./рос. vs англ., див. §3.5).

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

### 5.2. Конвертації рядків: `Safe*` над `AddInNative::WCHAR2MB`/`MB2WCHAR`

Сам механізм конвертації живе в ядрі, не в `ServiceTools`:
`AddInNative::WCHAR2MB` (`AddInNative.cpp:468-478`), `MB2WCHAR` (`:488-496`) і
`WCHAR2WC` (`:482-484`, поелементна копія `char16_t`→`wchar_t`, БЕЗ
перекодування — обидва 2-байтні на Windows). `WCHAR2MB`/`MB2WCHAR` побудовані
над WinAPI `WideCharToMultiByte`/`MultiByteToWideChar` з `CP_UTF8`, а не над
`std::wstring_convert` (у проєкті його більше немає взагалі); довжину джерела
скрізь передають ЯВНО (`src.size()`, не `-1`) — саме це зберігає вбудований
`\0` усередині рядка, бо з `-1` конвертація зупинилась би на першому нулі.

> ⚠️ **Свідома зміна поведінки: невалідний вхід дає U+FFFD, а не виняток.**
> `std::wstring_convert` на невалідному UTF-8/UTF-16 кидав `std::range_error`.
> WinAPI-конвертації викликаються з прапорцями `0` (без
> `MB_ERR_INVALID_CHARS`/`WC_ERR_INVALID_CHARS`) — на битій послідовності вони
> підставляють символ заміни U+FFFD і продовжують, а не кидають і не
> повертають порожній рядок. Підстава: `ServiceTools::SafeMB2WCHAR` (нижче) НЕ
> ловить винятків, попри назву — «Safe» тут про керування пам'яттю, не про
> винятки, — тож раніше виняток проходив крізь неї в тому числі в сам шлях
> обробки помилки (`ReportComponentEvent` → `AddComponentError`, §5.1), тобто
> аварія траплялась усередині аварії. Виняток, що вилітає в 1С, — гірша
> відмова, ніж символ заміни; порожній рядок (те, що дали б
> `MB_ERR_INVALID_CHARS`) — тиха втрата даних, це теж гірше. Межу ризику
> окреслено: через ці конвертації ходять описи помилок, лог, текст етикетки,
> шляхи до каталогу провайдера UAPKI, події компонент — **крипто-дані сюди не
> потрапляють** (підписи й контейнери йдуть через base64/BLOB), тож найгірший
> реалістичний наслідок — символ заміни в тексті логу чи на етикетці, а не
> зіпсований підпис.

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

`MethFunction` — це `std::variant` хендлерів (§2.3): значення, повернуте
«голою» лямбдою-хендлером, мовчки відкидається й **не потрапляє в 1С**.
`Ret(F f)` (`AddInNative.h:233-234`, protected шаблон) загортає
value-повертаючу лямбду у void-хендлер, який присвоює `this->result`. Диспетч
за типом результату в приватному `WrapRet` (`AddInNative.h:237-253`): `bool` →
як є, цілі → `int64_t`, з рухомою комою → `double`, решта (рядки) — напряму
через `VH::operator=`. `static_assert` (`:240-241`) забороняє `void`-лямбди
(для них — `AddProcedure`).

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

Макрос (`AddInNative.h:497-501`) на файловому рівні `.cpp` компоненти визначає
`CLASS::names` через `AddComponent` **і** створює анти-стрип reference (проти
відкидання TU лінкером — §1.3) одним рядком:

```cpp
REGISTER_COMPONENT(u"AddinUAPKIConnect", AddinUAPKIConnect)   // AddinUAPKIConnect.cpp:8
```

Клас мусить оголосити `static std::vector<std::u16string> names;`. Для компонент
з кількома іменами (`TestComponent` — 3 псевдоніми) макрос не годиться: лишається
ручний `names = { AddComponent(...), ... }` + окремий анти-стрип
`namespace { [[maybe_unused]] auto& _force… = TestComponent::names; }`.

### 6.3. Успадкований `EnableLogging` у базі

Конструктор базового `AddInNative` (`AddInNative.cpp:75-90`) реєструє, окрім
властивості `Version`, спільну для всіх компонент функцію
`EnableLogging`/`ИспользоватьЛогирование(рівень="info", шлях="")` — делегат у
`ServiceTools::EnableComponentLogging(this, level, path)`, обгорнутий `Ret()` і
`try/catch`. `MethDefaults{...}` тут пишеться з явним іменем типу (не
нетипізованим `{...}`) навмисно: після появи перевантаження
`AddFunction`/`AddProcedure` з `const std::vector<ParamSpec>&` (§6.4)
нетипізований `{...}` став неоднозначним для компілятора. Дублікати з
`AddinUAPKIConnect`, `TestComponent` прибрані. `DisableComponentLogging`
лишається в деструкторах похідних (RTTI-ім'я в базовому деструкторі вже
некоректне).

### 6.4. Декларативні параметри: `ParamSpec` + валідація

`struct ParamSpec` (`AddInNative.h:164-169`): `nameEn`, `nameRu`, `required`,
`std::optional<DefaultHelper> byDefault`. Перевантаження
`AddProcedure`/`AddFunction` з `const std::vector<ParamSpec>&`
(`AddInNative.cpp:418-422`, `424-428`) зберігають специфікації в полі
`MethDesc::params` і похідні дефолти (`DefaultsFromSpecs`,
`AddInNative.cpp:409-416`) — у `MethDefaults`; обидва, як і решта двох
перевантажень (§2.1), делегують у спільний `RegisterMethod`.

`ValidateParams` (`AddInNative.cpp:430-447`) виконується **всередині
спільного `Dispatch`** (`AddInNative.cpp:313`, §3.6) — тобто ДО хендлера і для
`CallAsProc`, і для `CallAsFunc` однаково (обидва йдуть через `Dispatch`, а не
дублюють виклик кожен у себе): для кожного `required`-параметра без дефолту,
якщо аргумент відсутній (`i >= lSizeArray`) або `VTYPE_EMPTY`, ядро робить
`AddError` з іменем параметра (ru/en за `alias`) і методу та повертає `false`
— хендлер не викликається. Старі реєстрації (без `params`) не валідуються —
поведінка без змін.

### 6.5. Потокобезпечний міст подій: `PostExternalEvent`

```cpp
bool PostExternalEvent(const std::u16string& message, const std::u16string& data);  // AddInNative.h:435
```

Публічний, викликається з **будь-якого** потоку (фонові reader-потоки
транспортів). `source` події — ім'я компоненти в 1С (`this->name`). Реалізація
(`AddInNative.cpp:129-141`) робить локальні mutable-копії рядків (бо
`ExternalEvent` приймає `WCHAR_T*` без const, `:134`) і під `connectMutex_`
перевіряє `m_iConnect`.

Гарантія відсікання: `Init` (лок на `:101`) і `Done` (лок на `:125`) беруть той
самий `connectMutex_`; `Done` обнуляє `m_iConnect`, тож після повернення з
`Done()` жоден фоновий `PostExternalEvent` уже не торкнеться зв'язку з 1С
(повертає `false`). `AddError` (`AddInNative.cpp:574`) теж узятий під
`connectMutex_` + null-guard — усунуто data race з `Done()` при фонових
потоках. Це фундамент подій для Етапів 1-2 (JobEngine, SimplyChannel).

> ⚠️ **ЗМІРЯНО: під час блокуючого виклику методу клієнтський потік 1С МЕРТВИЙ.**
> Зонд на живій 1С 8.3.27 (2026-08-31): метод компоненти блокував потік на 5 секунд, а окремий
> потік компоненти щопівсекунди слав `PostExternalEvent`. Результат:
>
> - `ПодключитьОбработчикОжидания` з інтервалом 1 с **не спрацював жодного разу** за 5-секундне
>   вікно;
> - усі 10 подій прийшли **одним пакетом через 23 мс ПІСЛЯ повернення** з методу, хоча
>   компонента слала їх за розкладом — платформа тримала їх у буфері подій.
>
> **`Асинх`-варіант справи не міняє.** Платформа сама генерує `<Метод>Асинх` для методів
> компоненти, але виклик **не виходить із клієнтського потоку** — проміс резолвиться після
> синхронного виконання. `Ждать` тут не віддає керування циклу обробки повідомлень.
>
> **Практичний висновок:** штовхати прогрес довгої операції з компоненти **марно** — ні
> подіями, ні полінгом. Живий прогрес можливий лише тоді, коли операцію веде прикладний код:
> неблокуючий старт (`JobEngine`) + полінг стану з боку 1С між викликами. Саме тому в
> `ECRPrivatJSON` існує асинхронна пара `НачатьОплату` + `СостояниеОперации`, а не лише
> синхронна `Оплата`.

### 6.6. Boot-фікси старту DLL

- **Локаль прибрано з боку старту DLL повністю.** Історичний фікс «лінива
  локаль з fallback» (`RuLocale()`, окрема статична функція з ланцюжком
  `ru_RU.UTF-8` → `Russian_Russia.1251` → `std::locale::classic()`) розв'язував
  проблему, якої в поточному коді вже немає: `upper()`/`NormalizeName` (§2.2)
  більше НЕ звертаються до `std::locale` взагалі — заміна на табличну
  нормалізацію прибрала ризик винятку зі старту DLL заодно з самою функцією.
  `RuLocale` у поточному `AddInNative.cpp` відсутня.
- **Контракт `GetClassObject` 1/0.** `GetClassObject` (`AddInNative.cpp:50-60`)
  повертає `1` при успіху / `0` при відмові (`return *pInterface ? 1 : 0;`,
  `:59`), а не адресу, приведену до `long`: приведення 64-бітного вказівника до
  `long` усікало його (UB на x64). При `*pInterface != nullptr` — одразу `0`
  (відмова створювати поверх наявного).
- **Fallback-sink логера.** До першого `EnableLogging` `GetLogger`
  (`ServiceTools_Log.cpp`) повертає не «порожній» `spdlog::default_logger`, а
  `GetFallbackLogger()` — `msvc_sink_mt(false)` (OutputDebugString), рівень
  `warn`: рання діагностика старту DLL не губиться. Аргумент `false` вимикає
  перевірку `IsDebuggerPresent`: DebugView читає буфер DBWIN, але відладчиком не
  є, тож із дефолтним `check_debugger_present=true` він не бачив ЖОДНОГО рядка
  (інцидент 2026-08-29 — діагностику без Visual Studio зібрати було неможливо).

### 6.7. Захист індексів методів/властивостей

Реєстри — прямі вектори `props_`/`meths_` (§2.1), тож кожен метод, що приймає
`lMethodNum`/`lPropNum` з платформи, перед індексацією явно звіряє його з
розміром вектора — інакше `operator[]` на індексі поза межами був би UB:

```cpp
if (lMethodNum < 0 || static_cast<size_t>(lMethodNum) >= meths_.size()) return false;
```

(для вказівникових — `return nullptr;`, для `long` — відповідне безпечне
значення). Приклади: `IsPropReadable`/`IsPropWritable`
(`AddInNative.cpp:195-199`, `201-205`), `GetMethodName` (`:239`), `GetNParams`
(`:248`), `HasRetVal` (`:292-293`), `Dispatch` (`:311`, спільний для
`CallAsProc`/`CallAsFunc` — §3.6). `GetParamDefValue` теж має цю перевірку
(`:281`), але з ІНВЕРТОВАНИМ результатом на відмову — див. §2.4.

Окремий, вужчий guard — на параметр **аліасу** (`lPropAlias`/`lMethodAlias`) у
`GetPropName` (`AddInNative.cpp:186-193`) і `GetMethodName` (`:237-244`): `0` →
англійське ім'я, `1` → національне (за порожнього `nameRu` — англійське),
**будь-що інше → `nullptr`**. Платформа за межі `0..1` не ходить, але старе
ядро на аліасі `>= 2` робило `std::next` по 2-елементному вектору імен —
виходило за межі ще ДО перевірки `it == end()`. У плоскій моделі полів
`nameEn`/`nameRu` такого способу вийти за межі вже немає фізично (це прості
поля, не контейнер), але явна перевірка `== 0` / `== 1` / інше лишається —
вона і Є весь контракт аліасу, а не рудимент.

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
