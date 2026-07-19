---
name: 1c-native-component
description: How to build, package and deliver a 1C native (external) component on Windows — use when working with a "1C native component", "external component", "MANIFEST.XML", "IComponentBase", "ExtCompT", "tVariant", "AddInNative", or the dependent-DLL problem.
---

# Нативна (зовнішня) компонента 1С

Практичний посібник із технології зовнішніх компонент 1С:Підприємство. Спирається на офіційну статтю ITS «Технология создания внешних компонент» (формат ZIP/MANIFEST.XML, типи компонент, обмеження виконання) і на емпіричні факти, здобуті прямим тестуванням у реальній 1С. Загальні речі підходять для будь-якого проєкту 1С-компоненти; вкраплення, позначені як **приклад SimplyAddinConnect**, — специфіка цього репозиторію.

## Коли застосовувати

- Пишете або супроводжуєте нативну (`type="native"`) компоненту 1С на C++ під Windows.
- Правите `MANIFEST.XML`, структуру ZIP-постачання, іменування DLL за архітектурою.
- Реалізуєте експорти (`GetClassObject`/`DestroyObject`/`GetClassNames`), `IComponentBase`, роботу з `tVariant`, `IMemoryManager`.
- Стикаєтесь із тим, що компонента не завантажується через **залежні DLL**, або що 1С «не докладає» потрібні файли поруч із вашою DLL.

Офіційна стаття ITS покриває **статичну** частину (формат маніфесту/ZIP, типи, атрибути). Вона **не описує** рантайм-поведінку десктопної платформи (кеш-каталог розпаковки, фільтрацію за архітектурою, порядок пошуку DLL) — це емпіричний шар, позначений нижче як «доведений факт».

## MANIFEST.XML

Кожна компонента постачається в ZIP-архіві разом із файлом `MANIFEST.XML`, який описує, які бінарні файли для яких ОС/архітектур усередині архіву.

Кореневий елемент — `<bundle>` з namespace `http://v8.1c.ru/8.2/addin/bundle`. Усередині — по одному `<component>` на кожну підтримувану комбінацію ОС/архітектури.

Атрибути `<component>`:

| Атрибут | Призначення | Значення |
|---|---|---|
| `type` | тип компоненти | `native`, `com`, `plugin` |
| `os` | цільова ОС | `Windows`, `Linux`, `MacOS`, `WindowsRuntime`, `Android`, `iOS` |
| `arch` | архітектура | `i386`, `x86_64`, `ARM`, `ARM64`, `E2K`, `Universal` |
| `path` | шлях до файлу всередині ZIP | ім'я DLL/so |
| `object` | ім'я об'єкта | лише для `type="plugin"`, для `native` не потрібен |

Робочий приклад для Windows x86 + x64 (**приклад SimplyAddinConnect**, `manifest.xml`):

```xml
<?xml version="1.0" encoding="utf-8"?>
<bundle xmlns="http://v8.1c.ru/8.2/addin/bundle">
    <component type="native" os="Windows" arch="i386"   path="SimplyAddinConnectWin_x86.dll" />
    <component type="native" os="Windows" arch="x86_64" path="SimplyAddinConnectWin_x64.dll" />
</bundle>
```

Генерувати маніфест зручно скриптом (у цьому репо — `manifest.ps1`, який читає ім'я проєкту з `CMakeLists.txt` і пише рівно два `<component>` через `XmlTextWriter`). Linux-гілка в цьому проєкті закоментована — постачаються лише дві Windows-компоненти.

## Native API

### Експортовані функції

Модуль DLL експортує рівно **три** C-функції (через `.def`-файл або `__declspec(dllexport)`):

```
GetClassObject
DestroyObject
GetClassNames
```

(**Приклад SimplyAddinConnect** — `src/core/AddInNative.def`.) SDK також оголошує `SetPlatformCapabilities`, але цей проєкт її не експортує.

Семантика (**приклад**, `src/core/AddInNative.cpp`):

- `GetClassNames()` — повертає список імен класів, розділених `|` (закешований `static const std::u16string`).
- `GetClassObject(wsName, pInterface)` — якщо `*pInterface` уже не `nullptr`, повертає `0` (відмова); інакше створює об'єкт і повертає його адресу, приведену до `long` (ненульове = успіх).
- `DestroyObject(pInterface)` — якщо `*pInterface == nullptr`, повертає `-1`; інакше `delete`, обнуляє вказівник, повертає `0`.

### IComponentBase та життєвий цикл

Сам об'єкт компоненти реалізує інтерфейс `IComponentBase`, що складається з `IInitDoneBase`, `ILanguageExtenderBase` та `LocaleBase` (`include/ComponentBase.h`).

Порядок викликів платформи (SDK):

```
Init(void* disp) → setMemManager(void* mem) → GetInfo() → <робота: методи/властивості> → Done()
```

- `Init` отримує вказівник на `IAddInDefBase` (зворотний зв'язок із платформою: `AddError`, `Read`/`Write`, `ExternalEvent`, `SetStatusLine` тощо). **Приклад SimplyAddinConnect** при ініціалізації виставляє `SetEventBufferDepth(100)`.
- `setMemManager` отримує вказівник на `IMemoryManager`.
- `GetInfo()` повертає `2000` — версію 2 SDK (за коментарем SDK).
- `Done()` — фіналізація (у цьому проєкті порожня; ресурси через RAII).

### tVariant

`tVariant` (`include/types.h`) — універсальний контейнер обміну значеннями з платформою: union усіх примітивів + рядкові/бінарні структури, поле `vt` (тип, `TYPEVAR`) і `cbElements`. Ключові типи `vt` (enum `ENUMVAR`):

- цілі: `VTYPE_I2/I4/I8`, `VTYPE_UI1..UI8`, `VTYPE_INT`;
- дійсні: `VTYPE_R4` (float), `VTYPE_R8` (double);
- `VTYPE_BOOL`, `VTYPE_ERROR`, `VTYPE_EMPTY`;
- рядок: `VTYPE_PWSTR` (поля `pwstrVal` + `wstrLen` — кількість символів);
- бінарні дані: `VTYPE_BLOB` (у str-структурі `pstrVal` + `strLen` — кількість байтів).

Рядки в `VTYPE_PWSTR` — це `WCHAR_T*` (див. нижче). Пам'ять під рядки/BLOB, що повертаються платформі, **обов'язково** виділяється через менеджер пам'яті 1С, а не звичайним `new`/`malloc`.

Практична обгортка над `tVariant` (**приклад SimplyAddinConnect**, `VariantHelper`/`VH` у `AddInNative`) інкапсулює конвертації C++↔`tVariant` (`std::u16string`, `int64_t`, `double`, `bool`, BLOB), тримає контекст (яка властивість/метод/номер параметра) для змістовних повідомлень про помилки типів, і перед кожним записом викликає `clear()`, що звільняє попередню пам'ять `VTYPE_BLOB`/`VTYPE_PWSTR`.

### IMemoryManager

Інтерфейс керування пам'яттю (`include/IMemoryManager.h`):

```cpp
virtual bool ADDIN_API AllocMemory(void** pMemory, unsigned long ulCountByte) = 0;
virtual void ADDIN_API FreeMemory(void** pMemory) = 0;
```

Правило: усе, що компонента повертає платформі в рядкових/BLOB-полях `tVariant`, має бути виділене через `AllocMemory` менеджера, отриманого в `setMemManager`. Платформа сама звільнить цю пам'ять. Пам'ять під власні внутрішні потреби компоненти цим менеджером виділяти не обов'язково.

### WCHAR_T та ADDIN_API

Обидва макроси — з `include/types.h`:

- `WCHAR_T` — на Windows (`_WINDOWS`) розкривається в `wchar_t` (2 байти), інакше в `uint16_t`. Тому на Windows `char16_t`/`std::u16string` і `WCHAR_T` мають однакове представлення, і між ними роблять `reinterpret_cast` без перекодування.
- `ADDIN_API` — угода виклику: на Windows `__stdcall`, інакше порожньо. Усі віртуальні методи SDK-інтерфейсів оголошені з `ADDIN_API`.

Увага: на Windows-збірці без `UNICODE` макрос `RT_RCDATA` та інші `RT_*` розкриваються в ANSI-форму. Для `FindResourceW` тоді слід брати `MAKEINTRESOURCEW(10)` замість `RT_RCDATA` (див. розділ про залежні DLL).

## Реєстрація методів і властивостей

1С звертається до компоненти за іменами методів/властивостей, які повертають `GetPropName`/`GetMethodName`/`FindProp`/`FindMethod`. Зручний патерн — реєструвати їх декларативно у конструкторі компоненти (**приклад SimplyAddinConnect**, `AddInNative`):

```cpp
void AddProperty (nameEn, nameRu, getter, setter = nullptr);
void AddProcedure(nameEn, nameRu, handler, defs = {});   // без повернення значення
void AddFunction (nameEn, nameRu, handler, defs = {});   // з поверненням значення
```

Особливості цієї реалізації:

- **Два імені** (en/ru) на кожен метод/властивість — платформа індексує їх за псевдонімом (`0` = міжнародний, `1` = російський).
- Різниця «процедура/функція» — лише прапорець `hasRetVal`.
- **Регістронезалежний пошук**: спершу точний збіг, потім порівняння у верхньому регістрі.
- Арність методів визначається під час виконання (варіант `MethFunction0..7`); за нестачі аргументів кидається `std::bad_function_call`.
- Значення параметрів за замовчанням задаються мапою `номер параметра → значення` (`MethDefaults`).

### Статична реєстрація класів (граблі лінкера)

Компоненти часто реєструють свої імена **до `main`**, через побічний ефект ініціалізації статичного члена (**приклад SimplyAddinConnect**):

```cpp
std::vector<std::u16string> AddinUAPKIConnect::names = {
    AddComponent(u"AddinUAPKIConnect", /* фабрика */)
};
```

Проблема: коли OBJECT-бібліотеки лінкуються у фінальну DLL і на символи TU немає прямих посилань, лінкер може **викинути** об'єктний файл разом зі статичним ініціалізатором — і реєстрація не спрацює. Обхід — анонімний non-const reference, який створює «використання» символу:

```cpp
namespace { auto& _forceAddinUAPKIConnectNames = AddinUAPKIConnect::names; }
```

## Пакування й доставка

### ZIP + MANIFEST.XML

Компонента постачається одним ZIP, у корені якого лежать `MANIFEST.XML` і всі бінарні файли, перелічені в маніфесті. **Приклад SimplyAddinConnect** (`build_project.ps1`): збираються Release-конфігурації для Win32 і x64, потім `bin/Release/*.dll` + `manifest.xml` пакуються `Compress-Archive` у `SimplyAddinConnectWin.zip`. Очікуваний склад архіву:

- без опції UAPKI — **3 файли**: 2 головні DLL (`_x86`/`_x64`) + `manifest.xml`;
- з UAPKI — **5 файлів**: додатково 2 провайдери `cm-pkcs12_x86.dll`/`cm-pkcs12_x64.dll` (провайдери в самому маніфесті **не** описані).

### ExtCompT — доведені емпіричні факти

Це поведінка **десктопної** платформи 1С, перевірена прямим тестуванням (не описана в ITS):

1. **1С розпаковує з ZIP лише файли, описані в `manifest.xml`, і рівно ОДИН файл** — той, що відповідає поточній ОС+архітектурі хост-процесу. Решта манiфестованих файлів ігнорується.
2. **Додаткові/фіктивні записи в маніфесті ігноруються.** Трюк із зайвим `<component arch="ARM" .../>`, щоб «протягнути» додатковий файл, не працює — 1С усе одно бере лише один файл під фактичну ОС/арх.
3. **Розпаковка відбувається в `%APPDATA%\1C\1cv8\ExtCompT\`.** Цей каталог належить самій платформі; компонента туди нічого не повинна дописувати.

Наслідок для архітектури: **не можна** розраховувати, що потрібні вашій DLL додаткові файли (залежні DLL, провайдери, дані) опиняться поруч із нею після розпаковки. Модель ZIP+manifest доставляє **рівно один** бінарник на платформу.

### Офіційна альтернатива — макети «Двоичные данные»

ITS прямо документує інший шлях доставки довільних бінарних файлів: у конфігурацію 1С їх кладуть у **макети** з типом «Внешняя компонента» або «**Двоичные данные**», які потім читає/розпаковує код самої конфігурації. Це єдиний офіційно задокументований механізм для додаткових файлів на десктопі. Мінус — він вимагає змін у конфігурації 1С (поза контролем розробника компоненти), тому самодостатнє вбудовування ресурсів у DLL часто зручніше (див. нижче).

## Проблема залежних DLL і як її розв'язати

Оскільки поруч із розпакованою DLL немає жодних інших файлів (див. ExtCompT-факти), будь-яка **залежна DLL** створює дві біди:

1. **Неявні (implicit) імпорти.** Статично прив'язані до вашої DLL імпорти Windows-завантажувач шукає відносно каталогу **процесу-хоста** (`1cv8.exe`), а не каталогу вашої підвантаженої компоненти. Тому DLL, покладена поруч із вашою, не знайдеться. *(Це стандартна поведінка DLL search order; за аналогією підтверджено пояснювальним фактом нижче, дослівної фрази «implicit imports» у джерелах немає — див. uncertain.)*
2. **Визначення власного каталогу.** Наївний `GetModuleFileName(NULL, ...)` у хості 1С поверне шлях до `1cv8.exe`, а не до вашої DLL.

**Розв'язання — визначати каталог власної DLL через якір усередині модуля** (**приклад SimplyAddinConnect**, `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp`):

```cpp
static void ModuleAnchor() {}   // якір: лише адреса всередині НАШОГО модуля

HMODULE hMod = nullptr;
GetModuleHandleExW(
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCWSTR>(&ModuleAnchor),   // адреса, а не NULL!
    &hMod);
GetModuleFileNameW(hMod, pathBuf.data(), pathBuf.size());
```

Ключове — прапорець `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS` з адресою функції-якоря (`&ModuleAnchor`), а **не** `NULL`. Так отримується дескриптор саме своєї DLL, і `GetModuleFileNameW` дає її реальний шлях.

**Розв'язання проблеми доставки залежностей — вбудований ресурс.** Замість покладатися на файли поруч, залежність (наприклад, провайдер) вшивається у DLL як `RCDATA`-ресурс і за потреби розгортається власним кодом у власний каталог (**приклад SimplyAddinConnect**: `%LOCALAPPDATA%\SimplyAddinConnect\providers\<VERSION_FULL>\`, окремий від `ExtCompT`):

```cpp
// 10 == RT_RCDATA; без UNICODE макрос RT_RCDATA дав би ANSI-форму — беремо MAKEINTRESOURCEW(10)
HRSRC hRes = FindResourceW(hMod, UAPKI_PROVIDER_RESOURCE_NAME, MAKEINTRESOURCEW(10));
```

Так компонента стає самодостатньою: 1С розпаковує один бінарник, а той сам розгортає все, що йому потрібно.

## Типові граблі

- **`GetModuleFileName(NULL, ...)`** у хості 1С повертає шлях до `1cv8.exe`, а не до вашої DLL. Використовуйте `GetModuleHandleExW(FROM_ADDRESS, &anchor, ...)`.
- **Розрахунок на «файли поруч» після ZIP-розпаковки.** 1С кладе в `ExtCompT` рівно один бінарник — залежностей поруч не буде.
- **Спроба протягнути зайвий файл через фіктивний `arch`** (`ARM` тощо) — ігнорується.
- **Пам'ять `tVariant` через `new`/`malloc`** замість `IMemoryManager::AllocMemory` — платформа не звільнить її коректно; використовуйте менеджер пам'яті 1С.
- **`RT_RCDATA` у не-UNICODE збірці** розкривається в ANSI-форму й не підходить до `FindResourceW`; беріть `MAKEINTRESOURCEW(10)`.
- **Лінкер викидає статичний ініціалізатор реєстрації** класу — додайте анонімний `_force`-reference у TU.
- **Провайдери/додаткові DLL у `manifest.xml`** — навіть якщо вписати, 1С розпакує лише один файл під поточну арх; додаткові бінарники доставляйте вбудованим ресурсом або макетами «Двоичные данные».

## Докладніше

- Ядро компоненти (експорти, `IComponentBase`, `VariantHelper`, реєстрація методів): `docs/architecture/core.md`.
- Збірка й пакування (OBJECT→SHARED, іменування DLL, ZIP): `docs/architecture/build-and-packaging.md`.
