# Шарування БПО-фасадів — план імплементації

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Розшарувати БПО-фасади на три рівні (контракт → семантика еквайрингу → ревізія →
протокол) і виправити `LabelPrinter`, який через штатну підсистему БПО не підхоплюється взагалі.

**Architecture:** Нова спільна база `BpoFacadeBase` тримає системну половину контракту БПО (не
знає типу обладнання). Над нею — `AcquiringFacadeBase`, що працює виключно через інтерфейс
`IAcquiringDriver` (не знає протоколу); під ним — адаптер `EcrPrivatJsonAcquiring` над наявним
`EcrPrivatJsonDriver`. Ревізійні шари `AcquiringBpo3004`/`AcquiringBpo4000` містять рівно
розкладку параметрів платіжних методів; конкретні фасади `EcrPrivatBpo3004`/`EcrPrivatBpo4000` —
рівно `MakeDriver()` + `REGISTER_COMPONENT`. `AddinLabelPrinter` стає другим нащадком
`BpoFacadeBase` і переходить на короткі імена контракту.

**Tech Stack:** C++17, MSVC (Visual Studio 2022), CMake (OBJECT-бібліотеки → одна SHARED DLL),
SDK 1С (`IComponentBase`/`tVariant`), nlohmann/json, spdlog, pugixml (vendored), GDI+/winspool.
Тести — самописні консольні exe з макросом `CHECK`, без фреймворку.

**Spec:** [`docs/superpowers/specs/2026-09-01-bpo-facade-layering-design.md`](../specs/2026-09-01-bpo-facade-layering-design.md)

**Уточнення до спеки, отримані від автора спеки (сесія `SimplyAddinConnect_companion`)** —
частина з них уже внесена в спеку, частина фіксується тільки тут:

1. «Реєстр згортається до одного» стосується **ТІЛЬКИ фасаду**. `LabelPrinterDriver` лишається
   мульти-пристроєвим, його публічний API і тести драйвера **не чіпаються**.
2. `ИДУстройства` LabelPrinter — **реальний id від драйвера**, без шару трансляції.
3. `ИнициализацияПринтера`/`ПечатьЭтикеток` **звіряють** `ИДУстройства` (як `CheckDeviceId`).
4. Гачок підключення — `bool OpenDevice(std::string& deviceIdOut)`: нащадок сам вирішує,
   синтезувати id (ECR) чи прокинути справжній (LabelPrinter).
5. Логування у `Подключить` лишається в базі; LabelPrinter просто не має параметрів
   `LogPath`/`LogLevel` → лог не вмикається, `LogIsEnabled="false"`.
6. `TerminalID` і `ShortSlip` — поля `AcquiringCapabilities` (властивість термінала, не фасаду).
7. `IAcquiringDriver` живе в `src/drivers/`, **не** в `src/platform/`. `AddinEcrBpoBase` зникає
   повністю, аліасом не лишається.
8. Паспорт драйвера збирає **база**; нащадок віддає лише змінну частину через `BuildDriverInfo()`.
9. Базі потрібні `protected` `Param()` **і** `ParamBool()` — розбір «true/false/1/0» в одному місці.
10. `ТестУстройства` для принтера **фактично відкриває транспорт і одразу закриває** (новий
    `LabelPrinterDriver::Probe`); ініт-пакет НЕ шлемо — тест має бути спостереженням, не дією.
11. Числова таксономія помилок — **єдина таблиця в базі**, не віртуальний метод. Нумерація ECR
    (0..11) зберігається недоторканою, коди принтера дописуються з 12 (див. Global Constraints).

---

## Global Constraints

Ці вимоги діють у **кожному** завданні плану; окремо в кроках не повторюються.

- **Мова коду й комітів — українська.** Коментарі, повідомлення про помилки, тексти
  `REPORT_*` — українською; дотримуйся мови файлу, який редагуєш.
- **PCH:** `#include "../core/pch.h"` — **першим рядком кожного `.cpp`**. У `.h` — ніколи.
- **Логування — лише макросами** з `ServiceTools.h`: `REPORT_*` у нестатичних методах компоненти,
  `NEUTRAL_REPORT_*` у статичних (перший аргумент — ім'я компоненти рядком). Прямий `spdlog`
  заборонено. printf-стиль у макросах заборонено — повідомлення складай конкатенацією
  (`"Текст: " + var`), з великої літери, без крапки в кінці.
- **Помилки:** після `REPORT_ERROR` — `return false`; `throw` для бізнес-помилок не кидати; код,
  що може кинути, обгортати `try-catch` з `REPORT_ERROR` у `catch`.
- **⚠️ `/utf-8` для КОЖНОЇ нової CMake-цілі** — рядок `target_compile_options(<ціль> PRIVATE /utf-8)`
  у `CMake/compiler_settings.cmake`. Перелік там **поіменний**. Без цього MSVC мовчки псує
  кириличні `u"..."`-літерали: **помилки збірки немає**, англійські імена методів у DLL є,
  російські — ні, а 1С каже «Метод объекта не обнаружен».
- **⚠️ 1С не перевіряє імена методів нативної компоненти на етапі компіляції.** Ні збірка `.epf`,
  ні `/CheckModules -ExtendedModulesCheck` зламаного виклику не спіймають. Кожне перейменування
  методу треба вручну відстежити по всіх викликах у BSL (Задача 6).
- **Повернення значення в 1С — через `Ret(...)`.** Не обгортати `Ret()` хендлери, які самі
  ставлять `this->result`.
- **Параметри — декларативно через `ParamSpec`** (перевантаження `AddFunction`/`AddProcedure`
  з `const std::vector<ParamSpec>&`).
- **⚠️ Вхідні значення читати ЛИШЕ через `VariantToString`/`VariantToDouble`.** Пряме
  `static_cast<std::string>(VH)` кидає на всьому, крім `VTYPE_PWSTR`, а 1С передає параметри
  форми налаштувань їхніми оголошеними типами: `Port`/`Baud`/`DotsPerMm` приходять **числами**,
  `VoidAsRefund` — **булевим**.
- **`version.h` перегенеровується при кожній збірці** — очікуваний diff, руками не чіпати.
- **⚠️ Гейт після кожного завдання — СПЕРШУ ЗБІРКА, потім прогон, обидві архітектури:**
  ```powershell
  powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
  powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
  powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
  ```
  **`run_tests.ps1` НЕ перезбирає проєкт, якщо тестові exe вже лежать у `bin/Release`**
  (`run_tests.ps1:241-251` — збірка лише за їх відсутності). Прогін без попереднього
  `build_project.ps1` крутить СТАРУ DLL і до твоїх правок нечутливий: гейт зелений,
  а перевірено нічого. Виявлено в Задачі 1 — не наступай на це знову.
  Критерій — ненульових exit-кодів немає; у підсумковій таблиці `L0.5`, `L0.6`, `L0.7`,
  `L2-ecr`, `L-p1`, `L-p3` — `PASS`. `L0.2`, `L1`, `L2/L3` — `SKIP` (це нормально без UAPKI).
- **⚠️ `/utf-8` гейтом НЕ перевіряється.** `ecr_native_host` і `label_native_host` шукають
  методи за англійськими іменами, тож зіпсовані кириличні літерали пройдуть зеленими.
  Додавши нову CMake-ціль, звір наявність кириличних імен у зібраній DLL прямим пошуком
  UTF-16LE-рядків (як зроблено в Задачі 1), а не покладайся на гейт.
- **⚠️ `tests/data/czo/` (16 файлів) — НЕ сміття й НЕ артефакт харнесу.** Це офіційні
  тестові приклади ЕЦП з ЦЗО, покладені паралельною сесією як вхідні дані для майбутнього
  тесту UAPKI. До цього плану стосунку не мають: **не комітити, не видаляти, не чіпати.**
- **⚠️ Критерій успіху Задач 1-3: `ecr_native_host` лишається зеленим БЕЗ ЖОДНОЇ правки в
  `tests/ecr_native_host.cpp`.** Знадобилось правити тест — зламано зовнішню поведінку, відкочуй.
- **Єдина числова таксономія помилок** (`BpoFacadeBase::CodeToInt`), глобально унікальна на всю
  компоненту. Числові коди (напр. код відповіді термінала) проходять як є; невідомий нечисловий
  код → `-1`:

  | Код | Число | | Код | Число |
  |---|---|---|---|---|
  | `OK` | 0 | | `DESYNC` | 9 |
  | `NOT_CONNECTED` | 1 | | `BAD_RESPONSE` | 10 |
  | `DEVICE_BUSY` | 2 | | `EXCEPTION` | 11 |
  | `UNSUPPORTED` | 3 | | `BAD_INPUT` | 12 |
  | `TIMEOUT` | 4 | | `TRANSPORT_ERROR` | 13 |
  | `DISCONNECTED` | 5 | | `UNSUPPORTED_BARCODE` | 14 |
  | `SEND_FAILED` | 6 | | `BARCODE_TOO_WIDE` | 15 |
  | `STOPPED` | 7 | | `RENDER_ERROR` | 16 |
  | `CONCURRENT` | 8 | | *вільні* | 17+ |

  Числа 0..11 — це наявна нумерація ECR, вона **не рухається** (`ecr_native_host` жорстко чекає
  `== 3` для `UNSUPPORTED`). Коди принтера, які раніше були 2..7, переїжджають на 12..16;
  споживачів у них немає (звірено автором спеки по конфігурації ru, розширенню `cfe` і нашій
  тестовій обробці — жодного порівняння з числом). `EXCEPTION` зводиться до 11 для обох.

---

## File Structure

**Створюються:**

| Файл | Відповідальність |
|---|---|
| `src/platform/MoneyFormat.h` | Локаленезалежна конверсія `double` → `"100.50"` через цілі копійки. Header-only. Спільна для фасаду (`PayloadStr`) і драйвера (формат протоколу). |
| `src/components/BpoFacadeBase.{h,cpp}` | Системна половина контракту БПО: 11 системних методів, мапа параметрів, `deviceId_`, `lastError` + `CodeToInt`, толерантні конвертації. Типу обладнання не знає. |
| `src/drivers/IAcquiringDriver.h` | Межа «фасад ↔ протокол еквайрингу»: `AcquiringCapabilities`, `AcquiringUnsupported()`, інтерфейс з дефолтами-відмовами. |
| `src/drivers/ecr_privatjson/EcrPrivatJsonAcquiring.{h,cpp}` | Адаптер: протокол ПриватБанк JSON у контракті `IAcquiringDriver`. Тут же — рядок підключення, `SettingsXml()`, `Capabilities()`. |
| `src/components/AcquiringFacadeBase.{h,cpp}` | Семантика еквайрингу поверх `IAcquiringDriver&`: `ПараметрыТерминала`, `ПечатьКвитанцийНаТерминале`, `ИтогиДняПоКартам`, `АварийнаяОтменаОперации`, асинхронна група, `VoidAsRefund`. Протоколу не знає. |
| `src/components/AcquiringBpo3004.{h,cpp}` | Рівно розкладка сімки + `InterfaceRevision() == 3004`. Абстрактний. |
| `src/components/AcquiringBpo4000.{h,cpp}` | Рівно розкладка дев'ятки/десяток + `InterfaceRevision() == 4000`. Абстрактний. |
| `src/components/EcrPrivatBpo3004.{h,cpp}` | `MakeDriver()` + `REGISTER_COMPONENT(u"ECRPrivatBPO3004", …)`. |
| `src/components/EcrPrivatBpo4000.{h,cpp}` | `MakeDriver()` + `REGISTER_COMPONENT(u"ECRPrivatBPO4000", …)`. |

**Видаляються:** `src/components/AddinEcrBpoBase.{h,cpp}`, `src/components/AddinEcrBpo3004.{h,cpp}`,
`src/components/AddinEcrBpo4000.{h,cpp}` (їхній вміст розходиться по файлах вище).

**Змінюються:**

| Файл | Що саме |
|---|---|
| `src/components/AddinLabelPrinter.{h,cpp}` | Успадковується від `BpoFacadeBase`; короткі імена; реалізує гачки; один пристрій. |
| `src/drivers/label_printer/LabelPrinterDriver.{h,cpp}` | **Додається** `ResultEnvelope Probe(deviceId)`. Решта — без змін. |
| `src/drivers/label_printer/LabelXml.{h,cpp}` | **Додається** `ProfileFromParameters(map, …)`; `ParseConnectionParameters` делегує в неї. |
| `CMake/components.cmake` | Нові OBJECT-цілі `bpo_facade_component`, `acquiring_facade_component`; склад `ecr_bpo_facade_component` замінюється; `label_facade_component` отримує залежність. |
| `CMake/compiler_settings.cmake` | `/utf-8` для двох нових цілей. |
| `tests/CMakeLists.txt` | `label_printer_selftest` лінкує `bpo_facade_component`. |
| `tests/label_native_host.cpp` | Повністю на новий контракт. |
| `tests/label_printer_selftest.cpp` | `TestFacadeSmoke` під нові імена; нові тести `Probe`/`ProfileFromParameters`. |
| `ExtDataProcessors/SimplyAddinConnect_test/SimplyAddinConnect/Forms/Форма/Ext/{Form.xml,Form/Module.bsl}` | Область `ПринтерЭтикеток` під новий контракт + кнопка «Тест устройства». |
| `docs/architecture/{bpo-contract,label_printer,README}.md`, `docs/integration-1c/label_printer.md` | Шарування фасадів, зняття дефект-нотісу, нові коди помилок. |

**НЕ змінюються (навмисно):** `src/components/AddinECRPrivatJSON.*` (прямий API, не БПО),
увесь ZPL-стек (`LabelZplGenerator`/`LabelRaster`/`GfEncoder`/`BarcodeZpl`),
`src/drivers/ecr_privatjson/EcrPrivatJsonDriver.*`, `tests/ecr_native_host.cpp`,
`tests/ecr_privatjson_selftest.cpp`, `ExtDataProcessors/.build/**` (артефакт збірки).

---

## Схема залежностей CMake після рефакторингу

```
base_component ─┬─ bpo_facade_component ──┬── acquiring_facade_component ── ecr_bpo_facade_component
                │        (BpoFacadeBase)  │   (AcquiringFacadeBase +          (EcrPrivatBpo3004/4000)
                │                         │    AcquiringBpo3004/4000)
                │                         └── label_facade_component (AddinLabelPrinter)
                └─ platform_component, helpers_component, driver_*_component …
```

`bpo_facade_component` тримає **тільки** `BpoFacadeBase` — саме тому окремою ціллю:
`label_printer_selftest` лінкує його `$<TARGET_OBJECTS>` і не має тягнути еквайринговий код
(інакше знадобились би `driver_ecr_privatjson_component` + `wire_component`).

---

### Task 1: Спільна база `BpoFacadeBase`

Виносимо контрактну половину з `AddinEcrBpoBase` у новий базовий клас. Еквайринг лишається
працездатним, `AddinEcrBpoBase` стає нащадком. `AddinLabelPrinter` у цьому завданні **не чіпаємо**.

**Files:**
- Create: `src/platform/MoneyFormat.h`
- Create: `src/components/BpoFacadeBase.h`, `src/components/BpoFacadeBase.cpp`
- Modify: `src/components/AddinEcrBpoBase.h`, `src/components/AddinEcrBpoBase.cpp`
- Modify: `src/components/AddinEcrBpo3004.cpp:26-30`, `src/components/AddinEcrBpo4000.cpp:25-29`
- Modify: `CMake/components.cmake`, `CMake/compiler_settings.cmake`
- Test: `tests/ecr_native_host.cpp` (**не редагувати** — це регресійний гейт)

**Interfaces:**
- Consumes: `AddInNative` (`VH`, `ParamSpec`, `Ret`, `AddFunction`, `AddProcedure`,
  `AddInNative::version()`), `ResultEnvelope`, `ServiceTools`.
- Produces (Задачі 2, 5 спираються на це дослівно):
  - `class BpoFacadeBase : public AddInNative`
  - `struct BpoFacadeBase::DriverInfo { std::string name, description, equipmentType; bool logEnabled; }`
  - гачки: `int InterfaceRevision() const`, `DriverInfo BuildDriverInfo() const`,
    `std::string BuildSettingsXml() const`, `std::string BuildActionsXml() const`,
    `bool AcceptEquipmentType(const std::string&) const`,
    `bool OpenDevice(std::string& deviceIdOut)`, `void CloseDevice()`,
    `bool ProbeDevice(std::string& resultOut, bool& demoOut)`, `bool RunAction(const std::string&)`
  - сервіси: `void RegisterSystemMethods()`, `const std::string& DeviceId() const`,
    `bool CheckDeviceId(const std::string&)`,
    `std::string Param(const char* name, const std::string& fallback = "") const`,
    `bool ParamBool(const char* name, bool fallback) const`,
    `const std::map<std::string,std::string>& Params() const`,
    `void SetError(int, const std::string&)`, `void ClearError()`,
    `bool MapEnvToBool(const ResultEnvelope&)`, `static int CodeToInt(const std::string&)`,
    `static std::string PayloadStr(const ResultEnvelope&, const char*)`,
    `static std::string VariantToString(VH)`, `static double VariantToDouble(VH)`,
    `static std::string XmlEscape(const std::string&)`, `static std::string ToUpperAscii(std::string)`
  - `inline std::string MoneyToString(double)` у `src/platform/MoneyFormat.h`

---

- [ ] **Крок 1: Зафіксувати «до» — гейт має бути зеленим ДО правок**

```powershell
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
```
Очікування: `L2-ecr ecr_native_host` — `PASS`. Якщо вже червоний — зупинись, це не наш регрес.

- [ ] **Крок 2: Створити `src/platform/MoneyFormat.h`**

```cpp
#pragma once
// Локаленезалежне форматування грошової суми.
//
// Через ЦІЛІ КОПІЙКИ, а не printf: `%.2f` у ru/uk-локалі дає КОМУ замість крапки,
// а протокол термінала й payload драйвера чекають саме крапку. Помилки при цьому
// немає — термінал просто відхиляє суму, тож баг знайшовся б аж на живому обладнанні.
#include <cmath>
#include <string>

inline std::string MoneyToString(double amount) {
    const bool negative = amount < 0;
    const long long cents = std::llround(std::fabs(amount) * 100.0);
    std::string s = (negative ? "-" : "") + std::to_string(cents / 100) + ".";
    const long long frac = cents % 100;
    if (frac < 10) s += '0';
    s += std::to_string(frac);
    return s;
}
```

- [ ] **Крок 3: Створити `src/components/BpoFacadeBase.h`**

```cpp
#pragma once
#include "../core/AddInNative.h"
#include "../platform/ResultEnvelope.h"
#include <map>
#include <string>

/// Спільна КОНТРАКТНА половина БПО-фасадів — усе, що однакове для будь-якого типу
/// обладнання. Типу обладнання, протоколу й драйвера цей клас не знає.
///
/// Контракт «Подключаемое оборудование» — docs/architecture/bpo-contract.md §2.
/// Послідовність підключення жорстка: УстановитьПараметр("EquipmentType", …) →
/// УстановитьПараметр(…) × N → Подключить(ИДУстройства[OUT]). Тому параметри
/// накопичуються НА ОБ'ЄКТІ, і один об'єкт обслуговує рівно ОДИН пристрій:
/// параметри другого затерли б перший.
///
/// Нащадок реалізує гачки нижче й кличе RegisterSystemMethods() у своєму конструкторі.
class BpoFacadeBase : public AddInNative {
public:
    virtual ~BpoFacadeBase();

protected:
    BpoFacadeBase();

    /// ЗМІННА частина паспорта драйвера. Решту XML (версії, IntegrationComponent,
    /// MainDriverInstalled, IsEmulator, LocalizationSupported, LogPath) складає база —
    /// це контрактні поля, однакові для всіх фасадів (bpo-contract.md §4.2).
    struct DriverInfo {
        std::string name;            ///< DriverDescription@Name
        std::string description;     ///< @Description
        std::string equipmentType;   ///< @EquipmentType — рядок ІТС (POSTerminal / LabelPrinter)
        bool logEnabled = false;     ///< @LogIsEnabled (ІТС вимагає true для ККТ і POSTerminal)
    };

    // ======================= гачки нащадка =======================

    /// Число ревізії інтерфейсу = ВИБІР РОЗКЛАДКИ ПАРАМЕТРІВ, а не «наскільки ми сучасні».
    virtual int InterfaceRevision() const = 0;
    virtual DriverInfo BuildDriverInfo() const = 0;
    /// XML форми налаштувань: корінь Settings → Page → Group → Parameter@TypeValue.
    virtual std::string BuildSettingsXml() const = 0;
    /// XML додаткових дій. Дефолт — порожній набір.
    virtual std::string BuildActionsXml() const;
    /// ⚠️ 1С передає ІМ'Я ЗНАЧЕННЯ ПЕРЕЛІКУ (ЭквайринговыйТерминал / ПринтерЭтикеток),
    /// а не англійський рядок ІТС. Порівнювати толерантно (§4.1 контракту).
    virtual bool AcceptEquipmentType(const std::string& value) const = 0;
    /// Фактичне підключення з накопичених параметрів. Нащадок сам вирішує, який
    /// ИДУстройства віддати: синтезований чи справжній id від драйвера.
    /// При невдачі САМ ставить lastError через SetError і повертає false.
    virtual bool OpenDevice(std::string& deviceIdOut) = 0;
    virtual void CloseDevice() = 0;
    /// Тіло ТестУстройства: перевірка ПАРАМЕТРІВ + досяжності обладнання.
    /// При невдачі сам ставить lastError; resultOut — текст для адміністратора.
    virtual bool ProbeDevice(std::string& resultOut, bool& demoOut) = 0;
    /// Виконання додаткової дії з BuildActionsXml. Дефолт — «невідома дія».
    virtual bool RunAction(const std::string& name);

    /// Реєструє системну половину контракту. Нащадок кличе її у своєму конструкторі
    /// ПЕРЕД реєстрацією власних методів.
    void RegisterSystemMethods();

    // ======================= сервіси для нащадків =======================

    /// Виданий при Подключить. Порожній = не підключено.
    const std::string& DeviceId() const { return deviceId_; }
    /// Перевіряє, що ИДУстройства збігається з виданим. При розбіжності ставить
    /// lastError і повертає false.
    bool CheckDeviceId(const std::string& deviceId);

    /// Накопичене через УстановитьПараметр.
    std::string Param(const char* name, const std::string& fallback = "") const;
    /// Булевий параметр форми налаштувань. Приймає true/false/1/0/да/нет/так/ні
    /// незалежно від регістру — 1С передає Boolean-параметр БУЛЕВИМ, а не рядком,
    /// і VariantToString зводить його до "true"/"false".
    bool ParamBool(const char* name, bool fallback) const;
    const std::map<std::string, std::string>& Params() const { return params_; }

    void SetError(int code, const std::string& description);
    void ClearError();
    /// Хвіст методу: ставить lastError із конверта і повертає значення для 1С.
    bool MapEnvToBool(const ResultEnvelope& env);
    /// ЄДИНА числова таксономія на всю компоненту (див. план, Global Constraints).
    /// Не робити віртуальною: кожен новий драйвер завів би свою нумерацію.
    static int CodeToInt(const std::string& code);
    /// Рядкове поле з payload конверта; відсутнє поле → порожній рядок.
    static std::string PayloadStr(const ResultEnvelope& env, const char* field);

    /// ⚠️ Толерантне читання вхідного параметра в рядок. Пряме
    /// static_cast<std::string>(VH) КИДАЄ на всьому, крім VTYPE_PWSTR, а 1С передає
    /// параметри форми налаштувань їхніми ОГОЛОШЕНИМИ типами: Port/Baud/DotsPerMm —
    /// числами, VoidAsRefund — булевим.
    static std::string VariantToString(VH value);
    /// Те саме для числа: порожній/незаповнений параметр → 0.0 замість винятку.
    static double VariantToDouble(VH value);

    static std::string XmlEscape(const std::string& s);
    static std::string ToUpperAscii(std::string s);

private:
    std::map<std::string, std::string> params_;   ///< накопичене через УстановитьПараметр
    std::string deviceId_;                        ///< видане при Подключить
    int lastErrorCode_ = 0;
    std::string lastErrorDesc_;
};
```

- [ ] **Крок 4: Створити `src/components/BpoFacadeBase.cpp`**

Тіла `XmlEscape`, `ToUpperAscii`, `SetError`, `ClearError`, `MapEnvToBool`, `PayloadStr`,
`VariantToString`, `VariantToDouble`, `Param`, `CheckDeviceId` переносяться з
`AddinEcrBpoBase.cpp` **дослівно** (рядки 12-31, 44-52, 77-81, 92-102, 115-144, 151-154, 168-178),
з трьома змінами:

* `XmlEscape`/`ToUpperAscii` були в анонімному просторі імен → стають `static`-членами класу;
* `AmountToString` **видаляється**, її виклики в `PayloadStr` і `VariantToString` замінюються на
  `MoneyToString` з `../platform/MoneyFormat.h`;
* `CodeToInt` дістає шість нових рядків (єдина таксономія).

Нове/змінене — повністю:

```cpp
#include "../core/pch.h"
#include "BpoFacadeBase.h"
#include "../helpers/ServiceTools.h"
#include "../platform/MoneyFormat.h"

BpoFacadeBase::BpoFacadeBase() = default;

BpoFacadeBase::~BpoFacadeBase() {
    // Пристрій закриває НАЩАДОК у своєму деструкторі: CloseDevice() віртуальний,
    // а на момент роботи базового деструктора нащадка вже немає.
    ServiceTools::DisableComponentLogging(this);
}

// Числова таксономія для ПолучитьОшибку (LONG). Протокольний код термінала —
// числовий, проходить як є; рядкові коди драйверів мапляться в стабільні числа.
// ⚠️ Таблиця ЄДИНА на всю компоненту: числа 0..11 — наявна нумерація еквайрингу
// (ecr_native_host жорстко чекає 3 = UNSUPPORTED), 12..16 — коди принтера етикеток.
// Нові коди дописувати з 17, наявні НЕ РУХАТИ.
int BpoFacadeBase::CodeToInt(const std::string& code) {
    if (code.empty()) return -1;
    bool numeric = true;
    for (char c : code) if (c < '0' || c > '9') { numeric = false; break; }
    if (numeric) { try { return std::stoi(code); } catch (...) { return -1; } }
    if (code == "OK")                  return 0;
    if (code == "NOT_CONNECTED")       return 1;
    if (code == "DEVICE_BUSY")         return 2;
    if (code == "UNSUPPORTED")         return 3;
    if (code == "TIMEOUT")             return 4;
    if (code == "DISCONNECTED")        return 5;
    if (code == "SEND_FAILED")         return 6;
    if (code == "STOPPED")             return 7;
    if (code == "CONCURRENT")          return 8;
    if (code == "DESYNC")              return 9;
    if (code == "BAD_RESPONSE")        return 10;
    if (code == "EXCEPTION")           return 11;
    if (code == "BAD_INPUT")           return 12;
    if (code == "TRANSPORT_ERROR")     return 13;
    if (code == "UNSUPPORTED_BARCODE") return 14;
    if (code == "BARCODE_TOO_WIDE")    return 15;
    if (code == "RENDER_ERROR")        return 16;
    return -1;   // невідомий нечисловий код
}

bool BpoFacadeBase::ParamBool(const char* name, bool fallback) const {
    const std::string raw = Param(name);
    if (raw.empty()) return fallback;
    const std::string v = ToUpperAscii(raw);
    if (v == "FALSE" || v == "0" || v == "НЕТ" || v == "НІ") return false;
    if (v == "TRUE"  || v == "1" || v == "ДА"  || v == "ТАК") return true;
    return fallback;
}

std::string BpoFacadeBase::BuildActionsXml() const {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Actions/>";
}

bool BpoFacadeBase::RunAction(const std::string& name) {
    SetError(2, "Невідома додаткова дія: " + name);
    return false;
}
```

Далі — `RegisterSystemMethods()`. Одинадцять методів; тіла `ПолучитьРевизиюИнтерфейса`,
`ПолучитьНомерВерсии`, `ПолучитьОшибку` переносяться дослівно (`AddinEcrBpoBase.cpp:232-236`,
`444-449`), решта — нижче:

```cpp
void BpoFacadeBase::RegisterSystemMethods() {

    AddFunction(u"GetInterfaceRevision", u"ПолучитьРевизиюИнтерфейса",
        Ret([this]() -> int { return InterfaceRevision(); }));

    AddFunction(u"GetVersionNumber", u"ПолучитьНомерВерсии",
        Ret([]() -> std::string { return AddInNative::version(); }));

    // Паспорт драйвера. EquipmentType тут ІНФОРМАЦІЙНИЙ (конфігурація його з переліком
    // не звіряє) — на відміну від EquipmentType, що ПРИХОДИТЬ в УстановитьПараметр.
    // Булеві конфігурація читає як ВРег(...) = "TRUE", тож "1" не спрацював би.
    AddFunction(u"GetDescription", u"ПолучитьОписание",
        Ret([this](VH out) -> bool {
            try {
                const DriverInfo info = BuildDriverInfo();
                const std::string ver = AddInNative::version();
                out = std::string(
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                    "<DriverDescription "
                    "Name=\"" + XmlEscape(info.name) + "\" "
                    "Description=\"" + XmlEscape(info.description) + "\" "
                    "EquipmentType=\"" + XmlEscape(info.equipmentType) + "\" "
                    "IntegrationComponent=\"false\" "
                    "MainDriverInstalled=\"true\" "
                    "DriverVersion=\"" + ver + "\" "
                    "IntegrationComponentVersion=\"" + ver + "\" "
                    "IsEmulator=\"false\" "
                    "LocalizationSupported=\"false\" "
                    "LogIsEnabled=\"" + (info.logEnabled ? "true" : "false") + "\" "
                    "LogPath=\"" + XmlEscape(Param("LogPath")) + "\"/>");
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка ПолучитьОписание: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DriverDescription", u"ОписаниеДрайвера", false, {} } });

    // Опис форми налаштувань. Формат СУВОРИЙ: корінь Settings, тип у TypeValue —
    // інакше форма БПО мовчки лишається без жодного поля (§3.1 контракту).
    AddFunction(u"GetParameters", u"ПолучитьПараметры",
        Ret([this](VH out) -> bool {
            try {
                out = BuildSettingsXml();
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DriverParameters", u"ПараметрыДрайвера", false, {} } });

    // Параметри приходять ПООДИНЦІ до Подключить, і першим — EquipmentType.
    AddFunction(u"SetParameter", u"УстановитьПараметр",
        Ret([this](VH name, VH value) -> bool {
            try {
                const std::string n = VariantToString(name);
                const std::string v = VariantToString(value);
                if (n == "EquipmentType" && !AcceptEquipmentType(v)) {
                    SetError(2, "Непідтримуваний тип обладнання: " + v);
                    return false;
                }
                params_[n] = v;
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"Name", u"ИмяПараметра", true, {} },
                                ParamSpec{ u"Value", u"ЗначениеПараметра", false, DefaultHelper(u"") } });

    // Підключення: параметрів не приймає (вони вже накопичені), ИДУстройства — OUT.
    AddFunction(u"Connect", u"Подключить",
        Ret([this](VH deviceIdOut) -> bool {
            try {
                // Журнал вмикаємо ДО OpenDevice — щоб діагностика самого підключення
                // потрапила у файл. Рівень і шлях — з параметрів підключення.
                const std::string logPath = Param("LogPath");
                if (!logPath.empty())
                    ServiceTools::EnableComponentLogging(this, Param("LogLevel", "Info"), logPath);

                std::string id;
                if (!OpenDevice(id)) return false;   // OpenDevice сам поставив lastError
                deviceId_ = id;
                deviceIdOut = deviceId_;
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка Подключить: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", false, {} } });

    AddFunction(u"Disconnect", u"Отключить",
        Ret([this](VH deviceId) -> bool {
            try {
                const std::string id = VariantToString(deviceId);
                if (!deviceId_.empty() && id != deviceId_) {
                    SetError(1, "Невідомий ідентифікатор пристрою: " + id);
                    return false;
                }
                CloseDevice();
                deviceId_.clear();
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", false, {} } });

    // Перевірка параметрів підключення з форми налаштувань. Підключеним бути НЕ
    // зобов'язана: форма кличе ТестУстройства одразу після УстановитьПараметр.
    AddFunction(u"EquipmentTest", u"ТестУстройства",
        Ret([this](VH resultOut, VH demoOut) -> bool {
            std::string text;
            bool demo = false;
            bool ok = false;
            try {
                ok = ProbeDevice(text, demo);
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                text = std::string("Помилка тесту: ") + e.what();
                ok = false;
            }
            demoOut = demo;
            resultOut = text;
            return ok;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"TestResult", u"РезультатТеста", false, {} },
                                ParamSpec{ u"DemoMode", u"АктивированДемоРежим", false, {} } });

    AddFunction(u"GetLastError", u"ПолучитьОшибку",
        Ret([this](VH descriptionOut) -> int {
            descriptionOut = lastErrorDesc_;
            return lastErrorCode_;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"ErrorDescription", u"ОписаниеОшибки", false, {} } });

    // Додаткові дії — пункти меню «Функції» форми налаштувань обладнання
    // (форма АДМІНІСТРАТОРА, не робоче місце касира).
    AddFunction(u"GetAdditionalActions", u"ПолучитьДополнительныеДействия",
        Ret([this](VH out) -> bool {
            try {
                out = BuildActionsXml();
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"Actions", u"ДополнительныеДействия", false, {} } });

    AddFunction(u"DoAdditionalAction", u"ВыполнитьДополнительноеДействие",
        Ret([this](VH actionName) -> bool {
            try {
                return RunAction(VariantToString(actionName));
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"ActionName", u"ИмяДействия", true, {} } });

    REPORT_INFO("Реєстрація системних методів БПО-фасаду завершена");
}
```

- [ ] **Крок 5: Перевести `AddinEcrBpoBase` на нову базу**

У `AddinEcrBpoBase.h`: змінити `class AddinEcrBpoBase : public AddInNative` на
`: public BpoFacadeBase`, `#include "../core/AddInNative.h"` → `#include "BpoFacadeBase.h"`.

**Прибрати з класу** (усе це тепер у базі): оголошення `InterfaceRevision()` (воно чисте
віртуальне в `BpoFacadeBase`, а перевизначають його `AddinEcrBpo3004`/`AddinEcrBpo4000` —
дублювати в проміжному класі не треба), `RegisterSystemMethods()`, `CheckDeviceId`,
`MapEnvToBool`, `PayloadStr`, `VariantToString`, `VariantToDouble`, `AmountToString`,
`SetError`, `ClearError`, `CodeToInt`, `Param`, `VoidAsRefundEnabled` (тіло спрощується, див.
нижче, але сам метод лишається), а також поля `params_`, `deviceId_`, `lastErrorCode_`,
`lastErrorDesc_`.

**Лишаються в класі:** `driver_`, `RunPurchase`/`RunRefund`/`RunVoid`/`RunEmergencyVoid`/
`RunDayTotals`, `Unsupported`, `BuildConnectionString`, `RegisterAsyncExtensions`,
`VoidAsRefundEnabled`.

**Додати** реалізації гачків і перейменувати точку реєстрації:

```cpp
protected:
    // ---- гачки BpoFacadeBase ----
    DriverInfo BuildDriverInfo() const override;
    std::string BuildSettingsXml() const override;
    std::string BuildActionsXml() const override;
    bool AcceptEquipmentType(const std::string& value) const override;
    bool OpenDevice(std::string& deviceIdOut) override;
    void CloseDevice() override;
    bool ProbeDevice(std::string& resultOut, bool& demoOut) override;
    bool RunAction(const std::string& name) override;

    /// Реєструє еквайрингову половину: методи можливостей, ИтогиДняПоКартам,
    /// АварийнаяОтменаОперации, асинхронне розширення. Похідний клас кличе її
    /// ПІСЛЯ RegisterSystemMethods() і ПЕРЕД своїми платіжними методами.
    void RegisterAcquiringMethods();
```

У `AddinEcrBpoBase.cpp`:

* `RegisterSystemMethods()` (стара версія) **видаляється цілком**; те, що в ній лишалось
  еквайринговим — `ПараметрыТерминала` (485-510), `ПечатьКвитанцийНаТерминале` (514-515),
  `ИтогиДняПоКартам` (519-528), `АварийнаяОтменаОперации` (530-535) і виклик
  `RegisterAsyncExtensions()` — переїжджає в `RegisterAcquiringMethods()` **дослівно**.
* `RegisterAsyncExtensions()` — дві заміни: `AmountToString(...)` → `MoneyToString(...)`
  (з `../platform/MoneyFormat.h`) і `deviceId_.empty()` → `DeviceId().empty()` у гардах
  `НачатьОплату`/`НачатьВозврат` (поле стало приватним у базі).
* нові гачки:

```cpp
BpoFacadeBase::DriverInfo AddinEcrBpoBase::BuildDriverInfo() const {
    // Лог для POSTerminal вмикається за замовчуванням — вимога ІТС для цього типу.
    return DriverInfo{ "Драйвер еквайрингового термінала (SimplyAddinConnect)",
                       "Платіжний термінал ПриватБанк, JSON-протокол, TCP/COM",
                       "POSTerminal",
                       /*logEnabled*/ true };
}

std::string AddinEcrBpoBase::BuildSettingsXml() const {
    return /* дослівно літерал із AddinEcrBpoBase.cpp:275-315 */;
}

std::string AddinEcrBpoBase::BuildActionsXml() const {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<Actions>"
           "<Action Name=\"XReport\" Caption=\"X-звіт (без вилучення)\"/>"
           "</Actions>";
}

bool AddinEcrBpoBase::AcceptEquipmentType(const std::string& value) const {
    // ⚠️ 1С передає ІМ'Я ЗНАЧЕННЯ ПЕРЕЛІКУ (ЭквайринговыйТерминал), а не англійський
    // рядок ІТС. Приймаємо обидва написання — страховка від наступної редакції (§4.1).
    return value == "ЭквайринговыйТерминал" || ToUpperAscii(value) == "POSTERMINAL";
}

bool AddinEcrBpoBase::OpenDevice(std::string& deviceIdOut) {
    const std::string conn = BuildConnectionString();
    if (conn.empty()) {
        SetError(2, "Не задано параметри підключення до термінала");
        return false;
    }
    if (!driver_.Connect(conn)) {
        SetError(1, "Не вдалося підключитися до термінала: " + conn);
        return false;
    }
    deviceIdOut = "ECR-1";   // драйвер поняття id не має — синтезуємо стабільний
    return true;
}

void AddinEcrBpoBase::CloseDevice() { driver_.Disconnect(); }

bool AddinEcrBpoBase::ProbeDevice(std::string& resultOut, bool& demoOut) {
    demoOut = false;                       // демо-режиму драйвер не має
    const std::string conn = BuildConnectionString();
    if (conn.empty()) {
        SetError(2, "Не задано параметри підключення до термінала");
        resultOut = "Не задано параметри підключення";
        return false;
    }
    if (!driver_.Connect(conn)) {
        SetError(1, "Термінал не відповідає: " + conn);
        resultOut = "Термінал не відповідає (" + conn + ")";
        return false;
    }
    const ResultEnvelope env = driver_.Execute("GetTerminalInfo",
                                               nlohmann::json::object(), kProbeTimeoutMs);
    const std::string vendor = driver_.Vendor();
    const std::string model = driver_.Model();
    driver_.Disconnect();

    if (!env.ok) {
        SetError(CodeToInt(env.code), env.description);
        resultOut = "Підключення є, але термінал відповів помилкою: " + env.code;
        return false;
    }
    resultOut = "Термінал на зв'язку: " + vendor + " " + model + " (" + conn + ")";
    ClearError();
    return true;
}

bool AddinEcrBpoBase::RunAction(const std::string& name) {
    if (name != "XReport") return BpoFacadeBase::RunAction(name);
    return MapEnvToBool(driver_.Audit("0"));
}
```

* `VoidAsRefundEnabled()` спрощується до `return ParamBool("VoidAsRefund", true);`
* деструктор лишається: `driver_.Disconnect();` (рядок `ServiceTools::DisableComponentLogging`
  прибрати — це тепер робить база).

- [ ] **Крок 6: Оновити конструктори похідних фасадів**

`AddinEcrBpo3004.cpp:26-30` і `AddinEcrBpo4000.cpp:25-29` — вставити виклик між двома наявними:

```cpp
    RegisterSystemMethods();
    RegisterAcquiringMethods();   // ← нове
    RegisterPaymentMethods();
```

- [ ] **Крок 7: CMake — нова ціль `bpo_facade_component`**

У `CMake/components.cmake` **перед** блоком `ecr_bpo_facade_component` додати:

```cmake
## @var bpo_facade_component
## @brief Спільна КОНТРАКТНА половина БПО-фасадів (BpoFacadeBase) — системні методи,
##        мапа параметрів, єдина числова таксономія помилок. Типу обладнання не знає.
## @note Окремою ціллю СВІДОМО: label_printer_selftest лінкує саме її $<TARGET_OBJECTS>
##       і не має тягнути еквайринговий код (інакше знадобились би driver_ecr_privatjson_
##       component + wire_component).
add_library(bpo_facade_component OBJECT
    src/components/BpoFacadeBase.h
    src/components/BpoFacadeBase.cpp
)
set_target_properties(bpo_facade_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(bpo_facade_component PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${SPDLOG_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_compile_definitions(bpo_facade_component PRIVATE _WINDOWS UNICODE _UNICODE)
add_dependencies(bpo_facade_component base_component spdlog nlohmann_json helpers_component
    platform_component)
```

У `HEADER_FILES` додати `src/components/BpoFacadeBase.h` і `src/platform/MoneyFormat.h`,
у `SOURCE_FILES` — `src/components/BpoFacadeBase.cpp`.
У `platform_component` до списку джерел додати `src/platform/MoneyFormat.h`.
У `ecr_bpo_facade_component` дописати `bpo_facade_component` в `add_dependencies`.
У `add_library(${TARGET} SHARED ...)` додати рядок `$<TARGET_OBJECTS:bpo_facade_component>`
**перед** `$<TARGET_OBJECTS:ecr_bpo_facade_component>`.

- [ ] **Крок 8: ⚠️ `/utf-8` для нової цілі**

`CMake/compiler_settings.cmake`, поруч із наявними рядками (біля коментаря-попередження):

```cmake
    target_compile_options(bpo_facade_component PRIVATE /utf-8)
```

Пропустиш — збірка буде зелена, а 1С скаже «Метод объекта не обнаружен» на всіх російських
іменах системних методів.

- [ ] **Крок 9: Прогнати гейт на обох архітектурах**

```powershell
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```
Очікування: `L0.5`, `L0.6`, `L0.7`, `L2-ecr`, `L-p1`, `L-p3` — `PASS`; exit 0.
`tests/ecr_native_host.cpp` **не редаговано** — перевір `git status`.

- [ ] **Крок 10: Коміт**

```bash
git add src/platform/MoneyFormat.h src/components/BpoFacadeBase.h src/components/BpoFacadeBase.cpp \
        src/components/AddinEcrBpoBase.h src/components/AddinEcrBpoBase.cpp \
        src/components/AddinEcrBpo3004.cpp src/components/AddinEcrBpo4000.cpp \
        CMake/components.cmake CMake/compiler_settings.cmake
git commit -m "refactor(bpo): виділено BpoFacadeBase — контрактна половина БПО без типу обладнання"
```

---

### Task 2: `IAcquiringDriver`, адаптер ПриватБанку і `AcquiringFacadeBase`

Семантика еквайрингу перестає знати протокол. Прапорці можливостей переїжджають із фасаду в
драйвер, `ПараметрыТерминала` починає рендерити їх, а не константи.

**Files:**
- Create: `src/drivers/IAcquiringDriver.h`
- Create: `src/drivers/ecr_privatjson/EcrPrivatJsonAcquiring.h`, `…/EcrPrivatJsonAcquiring.cpp`
- Create: `src/components/AcquiringFacadeBase.h`, `src/components/AcquiringFacadeBase.cpp`
- Delete: `src/components/AddinEcrBpoBase.h`, `src/components/AddinEcrBpoBase.cpp`
- Modify: `src/components/AddinEcrBpo3004.{h,cpp}`, `src/components/AddinEcrBpo4000.{h,cpp}`
  (тільки базовий клас в `#include`/оголошенні + одна перевірка можливостей у 4000)
- Modify: `CMake/components.cmake`, `CMake/compiler_settings.cmake`, `tests/CMakeLists.txt`
- Test: `tests/ecr_privatjson_selftest.cpp` (**новий** тест гілки відкату скасування)
- Test: `tests/ecr_native_host.cpp` (**не редагувати** — регресійний гейт)

**Interfaces:**
- Consumes: `BpoFacadeBase` (Задача 1), `MoneyToString`, `EcrPrivatJsonDriver`, `ResultEnvelope`.
- Produces (Задача 3 спирається дослівно):
  - `struct AcquiringCapabilities { std::string terminalId; bool printSlipOnTerminal, shortSlip,
    cashWithdrawal, electronicCertificates, partialCancellation, consumerPresentedQR,
    listCardTransactions; }` (усі bool = `false` за замовчуванням)
  - `inline ResultEnvelope AcquiringUnsupported(const std::string& method)`
  - `class IAcquiringDriver` (склад — крок 1 нижче)
  - `class AcquiringFacadeBase : public BpoFacadeBase` з `protected`:
    `virtual std::unique_ptr<IAcquiringDriver> MakeDriver() const = 0`,
    `IAcquiringDriver& Driver() const`, `AcquiringCapabilities Capabilities() const`,
    `void RegisterAcquiringMethods()`,
    `ResultEnvelope RunPurchase(double)`, `ResultEnvelope RunRefund(double, const std::string&)`,
    `ResultEnvelope RunVoid(double, const std::string&)`, `ResultEnvelope RunEmergencyVoid()`,
    `ResultEnvelope RunDayTotals()`, `ResultEnvelope Unsupported(const std::string&)`

---

- [ ] **Крок 1: Створити `src/drivers/IAcquiringDriver.h`**

```cpp
#pragma once
#include <map>
#include <string>
#include "../platform/ResultEnvelope.h"

/// Що термінал і протокол РЕАЛЬНО вміють. Це властивість ОБЛАДНАННЯ, а не фасаду:
/// саме тому прапорці живуть тут, а не захардкожені в ПараметрыТерминала.
/// Термінал, що вміє часткове скасування, вмикає його одним рядком у своєму драйвері.
///
/// Дефолти — усе вимкнено СВІДОМО: непідтримуване має бути станом за замовчуванням,
/// щоб забути ввімкнути було безпечно, а забути вимкнути — неможливо.
struct AcquiringCapabilities {
    std::string terminalId;               ///< TerminalParameters@TerminalID
    bool printSlipOnTerminal = false;     ///< термінал друкує квитанції сам
    bool shortSlip = false;
    bool cashWithdrawal = false;
    bool electronicCertificates = false;
    bool partialCancellation = false;
    bool consumerPresentedQR = false;
    bool listCardTransactions = false;
};

/// Чесна відмова у формі, якої вимагає ІТС §1.3: в описі помилки має бути прямо
/// сказано, що функція обладнанням не підтримується.
inline ResultEnvelope AcquiringUnsupported(const std::string& method) {
    return ResultEnvelope::Fail("UNSUPPORTED",
        "Операція \"" + method + "\" не підтримується обладнанням");
}

/// Межа, за якою ховається протокол еквайрингу. Фасад працює ВИКЛЮЧНО через неї.
///
/// ⚠️ Інтерфейс спроєктовано на ОДНОМУ відомому протоколі (ПриватБанк JSON), тож
/// ризик, що для BPOS1 він виявиться не зовсім тим, реальний і прийнятий свідомо.
/// Коли з'явиться друга реалізація — уточнюємо ЗА ФАКТОМ, а не вгадуємо наперед.
///
/// Операції мають ДЕФОЛТНІ реалізації з чесною відмовою: непідтримуване лишається
/// поведінкою за замовчуванням, а не обов'язком кожного драйвера.
class IAcquiringDriver {
public:
    virtual ~IAcquiringDriver() = default;

    // ---- з'єднання ----
    /// Параметри — сирою мапою з УстановитьПараметр: набір імен у кожного протоколу
    /// свій (він же й описує їх у SettingsXml), тож фасад у них не заглядає.
    virtual ResultEnvelope Open(const std::map<std::string, std::string>& params) = 0;
    virtual void Close() = 0;
    virtual bool IsConnected() const = 0;

    // ---- ідентичність і опис ----
    virtual std::string Vendor() const = 0;
    virtual std::string Model() const = 0;
    virtual std::string DriverName() const = 0;          ///< DriverDescription@Name
    virtual std::string DriverDescription() const = 0;   ///< @Description
    virtual std::string SettingsXml() const = 0;         ///< форма налаштувань цього протоколу
    virtual AcquiringCapabilities Capabilities() const = 0;
    /// Коротка перевірка зв'язку для ТестУстройства. Фінансових наслідків не має.
    virtual ResultEnvelope Probe() = 0;

    // ---- операції ----
    virtual ResultEnvelope Purchase(double amount) {
        (void)amount; return AcquiringUnsupported("Оплата");
    }
    virtual ResultEnvelope Refund(double amount, const std::string& rrn) {
        (void)amount; (void)rrn; return AcquiringUnsupported("Повернення");
    }
    /// Власна операція скасування протоколу. Немає — фасад піде шляхом VoidAsRefund.
    virtual ResultEnvelope Void(double amount, const std::string& rrn) {
        (void)amount; (void)rrn; return AcquiringUnsupported("Скасування");
    }
    virtual ResultEnvelope DayTotals()     { return AcquiringUnsupported("Підсумки дня по картах"); }
    virtual ResultEnvelope Audit()         { return AcquiringUnsupported("X-звіт"); }
    virtual ResultEnvelope EmergencyVoid() { return AcquiringUnsupported("Аварійне скасування"); }

    // ---- асинхронне розширення (не БПО; його бере розширення 1С із точки РМК) ----
    virtual bool StartPurchase(double amount) { (void)amount; return false; }
    virtual bool StartRefund(double amount, const std::string& rrn) {
        (void)amount; (void)rrn; return false;
    }
    /// 0 Idle, 1 Running, 2 Interrupting, 3 Done, 4 Error (значення JobState).
    virtual int  OperationState() const { return 0; }
    virtual bool TryGetOperationResult(ResultEnvelope& out) const { (void)out; return false; }
    /// Онлайн-статус термінала, -1..11; -1 = ще не було.
    virtual int  LastStatus() const { return -1; }
    virtual void CancelOperation() {}
};
```

- [ ] **Крок 2: Створити `src/drivers/ecr_privatjson/EcrPrivatJsonAcquiring.h`**

```cpp
#pragma once
#include "../IAcquiringDriver.h"
#include "EcrPrivatJsonDriver.h"

/// Адаптер: протокол ПриватБанк JSON у контракті IAcquiringDriver.
/// Уся Privat-специфіка (рядок підключення, форма налаштувань, можливості
/// термінала) — тут; фасад про неї не знає.
class EcrPrivatJsonAcquiring : public IAcquiringDriver {
public:
    ResultEnvelope Open(const std::map<std::string, std::string>& params) override;
    void Close() override;
    bool IsConnected() const override;

    std::string Vendor() const override;
    std::string Model() const override;
    std::string DriverName() const override;
    std::string DriverDescription() const override;
    std::string SettingsXml() const override;
    AcquiringCapabilities Capabilities() const override;
    ResultEnvelope Probe() override;

    ResultEnvelope Purchase(double amount) override;
    ResultEnvelope Refund(double amount, const std::string& rrn) override;
    ResultEnvelope DayTotals() override;
    ResultEnvelope Audit() override;
    // Void/EmergencyVoid НЕ перевизначені: власних операцій протокол не має,
    // працює дефолтна чесна відмова (скасування фасад робить поверненням за RRN).

    bool StartPurchase(double amount) override;
    bool StartRefund(double amount, const std::string& rrn) override;
    int  OperationState() const override;
    bool TryGetOperationResult(ResultEnvelope& out) const override;
    int  LastStatus() const override;
    void CancelOperation() override;

private:
    static std::string BuildConnectionString(const std::map<std::string, std::string>& params);
    EcrPrivatJsonDriver drv_;
};
```

- [ ] **Крок 3: Створити `src/drivers/ecr_privatjson/EcrPrivatJsonAcquiring.cpp`**

```cpp
#include "../../core/pch.h"
#include "EcrPrivatJsonAcquiring.h"
#include "../../platform/MoneyFormat.h"
#include "../../helpers/ServiceTools.h"

namespace {
constexpr const char* kTag = "EcrPrivatJsonAcquiring";
/// Таймаут короткого службового запиту (перевірка зв'язку при ТестУстройства).
constexpr int kProbeTimeoutMs = 5000;

std::string ToUpperAscii(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::string Get(const std::map<std::string, std::string>& p, const char* name,
                const std::string& fallback = "") {
    auto it = p.find(name);
    return (it == p.end() || it->second.empty()) ? fallback : it->second;
}
} // namespace

std::string EcrPrivatJsonAcquiring::BuildConnectionString(
        const std::map<std::string, std::string>& params) {
    const std::string kind = ToUpperAscii(Get(params, "TransportKind", "tcp"));
    if (kind == "COM") {
        const std::string port = Get(params, "ComPort");
        const std::string baud = Get(params, "Baud", "115200");
        return port.empty() ? std::string() : (port + ":" + baud);
    }
    const std::string host = Get(params, "Host");
    const std::string port = Get(params, "Port", "2000");
    return host.empty() ? std::string() : ("tcp://" + host + ":" + port);
}

ResultEnvelope EcrPrivatJsonAcquiring::Open(const std::map<std::string, std::string>& params) {
    const std::string conn = BuildConnectionString(params);
    if (conn.empty())
        return ResultEnvelope::Fail("BAD_INPUT", "Не задано параметри підключення до термінала");
    if (!drv_.Connect(conn))
        return ResultEnvelope::Fail("NOT_CONNECTED", "Не вдалося підключитися до термінала: " + conn);
    return ResultEnvelope::Ok({ { "connection", conn } });
}

void EcrPrivatJsonAcquiring::Close()            { drv_.Disconnect(); }
bool EcrPrivatJsonAcquiring::IsConnected() const { return drv_.IsConnected(); }
std::string EcrPrivatJsonAcquiring::Vendor() const { return drv_.Vendor(); }
std::string EcrPrivatJsonAcquiring::Model() const  { return drv_.Model(); }

std::string EcrPrivatJsonAcquiring::DriverName() const {
    return "Драйвер еквайрингового термінала (SimplyAddinConnect)";
}
std::string EcrPrivatJsonAcquiring::DriverDescription() const {
    return "Платіжний термінал ПриватБанк, JSON-протокол, TCP/COM";
}

std::string EcrPrivatJsonAcquiring::SettingsXml() const {
    // Літерал форми налаштувань переїжджає СЮДИ з тіла AddinEcrBpoBase::BuildSettingsXml()
    // (створеного в Задачі 1) дослівно: у BPOS1 набір параметрів підключення буде свій,
    // і фасад не має про це знати.
    return /* той самий літерал <Settings>…</Settings> */;
}

AcquiringCapabilities EcrPrivatJsonAcquiring::Capabilities() const {
    AcquiringCapabilities c;
    // Справжнього TID протокол не віддає — модель як найближче стабільне значення.
    c.terminalId = drv_.Model();
    c.printSlipOnTerminal = true;   // N950 друкує квитанції сам; 1С не проситиме друк на ЧПУ
    // Решта лишається false: часткове скасування, видача готівки, Consumer-Presented QR,
    // електронні сертифікати й список операцій протокол/термінал не вміють
    // (docs/architecture/ecrprivatjson.md §8). Конфігурація відсіє їх ДО виклику драйвера.
    return c;
}

ResultEnvelope EcrPrivatJsonAcquiring::Probe() {
    return drv_.Execute("GetTerminalInfo", nlohmann::json::object(), kProbeTimeoutMs);
}

ResultEnvelope EcrPrivatJsonAcquiring::Purchase(double amount) {
    try { return drv_.Purchase(MoneyToString(amount)); }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(kTag, std::string("Помилка Purchase: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope EcrPrivatJsonAcquiring::Refund(double amount, const std::string& rrn) {
    try { return drv_.Refund(MoneyToString(amount), rrn); }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(kTag, std::string("Помилка Refund: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope EcrPrivatJsonAcquiring::DayTotals() {
    // ИтогиДняПоКартам = підсумки на хост для звірки = Verify драйвера (спека §5.18).
    try { return drv_.Verify("0"); }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(kTag, std::string("Помилка DayTotals: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope EcrPrivatJsonAcquiring::Audit() {
    try { return drv_.Audit("0"); }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(kTag, std::string("Помилка Audit: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

bool EcrPrivatJsonAcquiring::StartPurchase(double amount) {
    return drv_.StartPurchase(MoneyToString(amount));
}
bool EcrPrivatJsonAcquiring::StartRefund(double amount, const std::string& rrn) {
    return drv_.StartRefund(MoneyToString(amount), rrn);
}
int  EcrPrivatJsonAcquiring::OperationState() const { return static_cast<int>(drv_.OperationState()); }
bool EcrPrivatJsonAcquiring::TryGetOperationResult(ResultEnvelope& out) const {
    return drv_.TryGetOperationResult(out);
}
int  EcrPrivatJsonAcquiring::LastStatus() const { return drv_.LastStatus(); }
void EcrPrivatJsonAcquiring::CancelOperation()  { drv_.CancelOperation(); }
```

⚠️ `OperationState()` і `TryGetOperationResult()` — `const`, а `EcrPrivatJsonDriver` оголошує
їх `const` (`EcrPrivatJsonDriver.h:79-80`). Якщо компілятор скаржиться — зроби `drv_` `mutable`,
**не** знімай `const` з інтерфейсу.

- [ ] **Крок 4: Створити `src/components/AcquiringFacadeBase.h`**

```cpp
#pragma once
#include "BpoFacadeBase.h"
#include "../drivers/IAcquiringDriver.h"
#include <memory>
#include <string>

/// Семантика еквайрингу поверх контракту БПО. Протоколу НЕ знає: працює виключно
/// через IAcquiringDriver&. Ревізії теж не знає — розкладку параметрів платіжних
/// методів додає ревізійний шар (AcquiringBpo3004 / AcquiringBpo4000).
///
/// Тут: методи можливостей (ПараметрыТерминала для ru, ПечатьКвитанцийНаТерминале
/// для ua), ИтогиДняПоКартам, АварийнаяОтменаОперации, асинхронне розширення
/// і логіка VoidAsRefund.
class AcquiringFacadeBase : public BpoFacadeBase {
public:
    ~AcquiringFacadeBase() override;

protected:
    AcquiringFacadeBase();

    /// Фабрика драйвера протоколу — ЄДИНЕ, що перевизначає конкретний фасад.
    virtual std::unique_ptr<IAcquiringDriver> MakeDriver() const = 0;

    /// Драйвер створюється ЛІНИВО: віртуальний виклик у конструкторі базового класу
    /// пішов би не в нащадка. Перший доступ — уже з методу, покликаного 1С.
    IAcquiringDriver& Driver() const;
    AcquiringCapabilities Capabilities() const { return Driver().Capabilities(); }

    /// Реєструє еквайрингову половину. Ревізійний шар кличе її ПІСЛЯ
    /// RegisterSystemMethods() і ПЕРЕД своїми платіжними методами.
    void RegisterAcquiringMethods();

    // ---- операції для платіжних методів ревізійного шару ----
    ResultEnvelope RunPurchase(double amount);
    ResultEnvelope RunRefund(double amount, const std::string& rrn);
    /// Скасування. Якщо протокол має ВЛАСНУ операцію void — вона й правильна.
    /// Немає — за параметром `VoidAsRefund` (дефолт увімкнено) скасування виконується
    /// ПОВЕРНЕННЯМ за RRN; вимкнений параметр → чесна відмова (§2.3.1 контракту).
    /// ⚠️ void і refund мають різну банківську семантику: void скасовує до звірки й
    /// сліду не лишає, refund створює зворотну транзакцію. Тому це видиме
    /// налаштування, а не зашите рішення.
    ResultEnvelope RunVoid(double amount, const std::string& rrn);
    ResultEnvelope RunEmergencyVoid();
    ResultEnvelope RunDayTotals();
    /// Відмова «операція не підтримується обладнанням» у формі, якої вимагає ІТС §1.3.
    ResultEnvelope Unsupported(const std::string& method);

    // ---- гачки BpoFacadeBase ----
    DriverInfo BuildDriverInfo() const override;
    std::string BuildSettingsXml() const override;
    std::string BuildActionsXml() const override;
    bool AcceptEquipmentType(const std::string& value) const override;
    bool OpenDevice(std::string& deviceIdOut) override;
    void CloseDevice() override;
    bool ProbeDevice(std::string& resultOut, bool& demoOut) override;
    bool RunAction(const std::string& name) override;

private:
    void RegisterAsyncExtensions();
    bool VoidAsRefundEnabled() const { return ParamBool("VoidAsRefund", true); }

    mutable std::unique_ptr<IAcquiringDriver> driver_;   ///< лінива ініціалізація, див. Driver()
};
```

- [ ] **Крок 5: Створити `src/components/AcquiringFacadeBase.cpp`**

`RegisterAsyncExtensions()` переноситься з `AddinEcrBpoBase.cpp:557-613` з двома змінами:
`driver_.X(...)` → `Driver().X(...)`, `AmountToString(static_cast<double>(amount))` →
`VariantToDouble(amount)` (форматування тепер робить адаптер).

Решта:

```cpp
#include "../core/pch.h"
#include "AcquiringFacadeBase.h"
#include "../helpers/ServiceTools.h"

AcquiringFacadeBase::AcquiringFacadeBase() = default;

AcquiringFacadeBase::~AcquiringFacadeBase() {
    if (driver_) driver_->Close();
}

IAcquiringDriver& AcquiringFacadeBase::Driver() const {
    if (!driver_) driver_ = MakeDriver();
    return *driver_;
}

ResultEnvelope AcquiringFacadeBase::Unsupported(const std::string& method) {
    return AcquiringUnsupported(method);
}

// ---------------------------- операції ----------------------------

ResultEnvelope AcquiringFacadeBase::RunPurchase(double amount) {
    try { return Driver().Purchase(amount); }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка ОплатитьПлатежнойКартой: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope AcquiringFacadeBase::RunRefund(double amount, const std::string& rrn) {
    try { return Driver().Refund(amount, rrn); }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка ВернутьПлатежПоПлатежнойКарте: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope AcquiringFacadeBase::RunVoid(double amount, const std::string& rrn) {
    // Є власна операція скасування — вона й правильна.
    ResultEnvelope env = Driver().Void(amount, rrn);
    // ⚠️⚠️ ВІДКАТ НА ПОВЕРНЕННЯ — ТІЛЬКИ при UNSUPPORTED. Це про гроші.
    // TIMEOUT/DISCONNECTED/DESYNC/SEND_FAILED означають «НЕВІДОМО, чи виконалось»:
    // термінал МІГ скасувати операцію, а ми просто не отримали відповіді. Відкат на
    // Refund після такого зрушив би гроші ДВІЧІ. UNSUPPORTED — єдина відповідь, що
    // гарантує: команда термінала не досягла й нічого не сталося.
    // Під тестом: TestVoidFallbackOnlyOnUnsupported у ecr_privatjson_selftest.
    if (env.code != "UNSUPPORTED") return env;

    if (!VoidAsRefundEnabled())
        return Unsupported("ОтменитьПлатежПоПлатежнойКарте");
    if (rrn.empty())
        return ResultEnvelope::Fail("BAD_INPUT",
            "Скасування виконується поверненням за RRN, але СсылочныйНомер порожній");
    REPORT_INFO("Скасування виконується поверненням за RRN " + rrn + " (VoidAsRefund)");
    return RunRefund(amount, rrn);
}

ResultEnvelope AcquiringFacadeBase::RunEmergencyVoid() {
    try { return Driver().EmergencyVoid(); }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка АварийнаяОтменаОперации: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

ResultEnvelope AcquiringFacadeBase::RunDayTotals() {
    try { return Driver().DayTotals(); }
    catch (const std::exception& e) {
        REPORT_ERROR(std::string("Помилка ИтогиДняПоКартам: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

// ---------------------------- гачки бази ----------------------------

BpoFacadeBase::DriverInfo AcquiringFacadeBase::BuildDriverInfo() const {
    // Name/Description бере драйвер: «…ПриватБанк» і «…BPOS1» — різні рядки.
    // Лог для POSTerminal вмикається за замовчуванням — вимога ІТС для цього типу.
    return DriverInfo{ Driver().DriverName(), Driver().DriverDescription(),
                       "POSTerminal", /*logEnabled*/ true };
}

std::string AcquiringFacadeBase::BuildSettingsXml() const { return Driver().SettingsXml(); }

std::string AcquiringFacadeBase::BuildActionsXml() const {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<Actions>"
           "<Action Name=\"XReport\" Caption=\"X-звіт (без вилучення)\"/>"
           "</Actions>";
}

bool AcquiringFacadeBase::AcceptEquipmentType(const std::string& value) const {
    // ⚠️ 1С передає ІМ'Я ЗНАЧЕННЯ ПЕРЕЛІКУ (ЭквайринговыйТерминал), а не англійський
    // рядок ІТС. Приймаємо обидва написання — страховка від наступної редакції (§4.1).
    return value == "ЭквайринговыйТерминал" || ToUpperAscii(value) == "POSTERMINAL";
}

bool AcquiringFacadeBase::OpenDevice(std::string& deviceIdOut) {
    const ResultEnvelope env = Driver().Open(Params());
    if (!env.ok) {
        SetError(CodeToInt(env.code), env.description);
        return false;
    }
    deviceIdOut = "ECR-1";   // поняття власного id протокол не має — синтезуємо стабільний
    return true;
}

void AcquiringFacadeBase::CloseDevice() {
    if (driver_ && driver_->IsConnected()) driver_->Close();
}

bool AcquiringFacadeBase::ProbeDevice(std::string& resultOut, bool& demoOut) {
    demoOut = false;                       // демо-режиму драйвер не має
    const ResultEnvelope opened = Driver().Open(Params());
    if (!opened.ok) {
        SetError(CodeToInt(opened.code), opened.description);
        resultOut = opened.description;
        return false;
    }
    const ResultEnvelope env = Driver().Probe();
    const std::string vendor = Driver().Vendor();
    const std::string model = Driver().Model();
    Driver().Close();

    if (!env.ok) {
        SetError(CodeToInt(env.code), env.description);
        resultOut = "Підключення є, але термінал відповів помилкою: " + env.code;
        return false;
    }
    resultOut = "Термінал на зв'язку: " + vendor + " " + model;
    ClearError();
    return true;
}

bool AcquiringFacadeBase::RunAction(const std::string& name) {
    if (name != "XReport") return BpoFacadeBase::RunAction(name);
    return MapEnvToBool(Driver().Audit());
}
```

І `RegisterAcquiringMethods()` — методи можливостей тепер рендерять `Capabilities()`:

```cpp
void AcquiringFacadeBase::RegisterAcquiringMethods() {

    // ---- Методи можливостей: різні в ru і ua, потрібні ОБИДВА ----

    // ru (ревізія >= 3004): XML із прапорцями. Прапорці невміючих операцій НЕ
    // виставляємо — конфігурація тоді відсіє їх ДО виклику драйвера (§2.3.1).
    AddFunction(u"TerminalParameters", u"ПараметрыТерминала",
        Ret([this](VH deviceId, VH out) -> bool {
            try {
                if (!CheckDeviceId(VariantToString(deviceId))) return false;
                const AcquiringCapabilities c = Capabilities();
                auto b = [](bool v) { return v ? "true" : "false"; };
                out = std::string(
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                    "<TerminalParameters "
                    "TerminalID=\"" + XmlEscape(c.terminalId) + "\" "
                    "PrintSlipOnTerminal=\"" + b(c.printSlipOnTerminal) + "\" "
                    "ShortSlip=\"" + b(c.shortSlip) + "\" "
                    "CashWithdrawal=\"" + b(c.cashWithdrawal) + "\" "
                    "ElectronicCertificates=\"" + b(c.electronicCertificates) + "\" "
                    "PartialCancellation=\"" + b(c.partialCancellation) + "\" "
                    "ConsumerPresentedQR=\"" + b(c.consumerPresentedQR) + "\" "
                    "ListCardTransactions=\"" + b(c.listCardTransactions) + "\"/>");
                ClearError();
                return true;
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", false, {} },
                                ParamSpec{ u"Parameters", u"ПараметрыТерминала", false, {} } });

    // ua: на місці ПараметрыТерминала — безпараметровий прапорець «термінал друкує
    // квитанції сам». Береться з тих самих можливостей, а не константою.
    AddFunction(u"PrintSlipOnTerminal", u"ПечатьКвитанцийНаТерминале",
        Ret([this]() -> bool { ClearError(); return Capabilities().printSlipOnTerminal; }));

    // ---- Еквайрингові методи БЕЗ гілок за ревізією (3004 і 4000 однакові) ----

    AddFunction(u"CardDayTotals", u"ИтогиДняПоКартам",
        Ret([this](VH deviceId, VH slip) -> bool {
            if (!CheckDeviceId(VariantToString(deviceId))) return false;
            const ResultEnvelope env = RunDayTotals();
            if (!MapEnvToBool(env)) return false;
            slip = PayloadStr(env, "receipt");
            return true;
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства",  false, {} },
                                ParamSpec{ u"SlipText", u"ТекстСлипЧека", false, {} } });

    AddFunction(u"EmergencyCancelOperation", u"АварийнаяОтменаОперации",
        Ret([this](VH deviceId) -> bool {
            if (!CheckDeviceId(VariantToString(deviceId))) return false;
            return MapEnvToBool(RunEmergencyVoid());
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", false, {} } });

    RegisterAsyncExtensions();

    REPORT_INFO("Реєстрація еквайрингових методів БПО-фасаду завершена");
}
```

- [ ] **Крок 6: ⚠️ Тест на гілку відкату скасування — це про гроші**

`RunVoid` відкочується на повернення **ТІЛЬКИ** при `UNSUPPORTED`. Будь-яка інша невдача
`Void()` — відмова, і крапка. `TIMEOUT`/`DISCONNECTED`/`DESYNC`/`SEND_FAILED`/`BAD_RESPONSE`
означають «**невідомо, чи виконалось**»: термінал МІГ скасувати операцію, а ми просто не
отримали відповіді. Відкат на `Refund` після такого зрушив би гроші **двічі** — спершу
скасування на терміналі, потім зворотна транзакція. `UNSUPPORTED` унікальний тим, що це
єдина відповідь, яка гарантує: команда термінала не досягла й нічого не сталося.

З результату методу цього не видно, тож тест перевіряє саме **факт невиклику** `Refund`.
Без нього регресія «додамо відкат на будь-яку помилку, буде надійніше» пройде зеленою.

У `tests/ecr_privatjson_selftest.cpp` додати перед `int main()`:

```cpp
#include "../src/components/AcquiringFacadeBase.h"   // до блоку include файлу
#include <map>
#include <memory>

namespace {

/// Лічильники викликів фейкового драйвера — саме їх перевіряє тест.
struct FakeAcquiringState {
    ResultEnvelope voidResult = AcquiringUnsupported("Скасування");
    int voidCalls = 0;
    int refundCalls = 0;
};

class FakeAcquiring : public IAcquiringDriver {
public:
    explicit FakeAcquiring(FakeAcquiringState* s) : s_(s) {}
    ResultEnvelope Open(const std::map<std::string, std::string>&) override { return ResultEnvelope::Ok(); }
    void Close() override {}
    bool IsConnected() const override { return true; }
    std::string Vendor() const override { return "FAKE"; }
    std::string Model() const override { return "FAKE-1"; }
    std::string DriverName() const override { return "Фейковий драйвер"; }
    std::string DriverDescription() const override { return "Лише для тесту"; }
    std::string SettingsXml() const override { return "<Settings/>"; }
    AcquiringCapabilities Capabilities() const override { return {}; }
    ResultEnvelope Probe() override { return ResultEnvelope::Ok(); }
    ResultEnvelope Void(double, const std::string&) override {
        ++s_->voidCalls;
        return s_->voidResult;
    }
    ResultEnvelope Refund(double, const std::string&) override {
        ++s_->refundCalls;
        return ResultEnvelope::Ok({ { "rrn", "R-1" } });
    }
private:
    FakeAcquiringState* s_;
};

/// Мінімальний конкретний фасад: методів у 1С не реєструє (RegisterSystemMethods не
/// кличеться), потрібен лише щоб дістати RunVoid із фейковим драйвером під ним.
class VoidProbeFacade : public AcquiringFacadeBase {
public:
    explicit VoidProbeFacade(FakeAcquiringState* s) : s_(s) {}
    using AcquiringFacadeBase::RunVoid;   // відкриваємо protected-метод для тесту
protected:
    int InterfaceRevision() const override { return 3004; }
    std::unique_ptr<IAcquiringDriver> MakeDriver() const override {
        return std::make_unique<FakeAcquiring>(s_);
    }
private:
    FakeAcquiringState* s_;
};

} // namespace

static void TestVoidFallbackOnlyOnUnsupported() {
    // 1) Власної операції void протокол не має -> штатний шлях: повернення за RRN.
    {
        FakeAcquiringState st;
        st.voidResult = AcquiringUnsupported("Скасування");
        VoidProbeFacade f(&st);
        ResultEnvelope r = f.RunVoid(100.50, "555000111");
        CHECK(r.ok, "Void=UNSUPPORTED + VoidAsRefund -> скасування виконано поверненням");
        CHECK(st.voidCalls == 1 && st.refundCalls == 1,
              "Void=UNSUPPORTED -> Void спитано, Refund викликано рівно раз");
    }
    // 2) TIMEOUT: термінал МІГ скасувати. Відкату бути НЕ МОЖЕ — інакше подвійний рух грошей.
    {
        FakeAcquiringState st;
        st.voidResult = ResultEnvelope::Fail("TIMEOUT", "Термінал не відповів");
        VoidProbeFacade f(&st);
        ResultEnvelope r = f.RunVoid(100.50, "555000111");
        CHECK(!r.ok && r.code == "TIMEOUT", "Void=TIMEOUT -> відмова з тим самим кодом");
        CHECK(st.refundCalls == 0, "Void=TIMEOUT -> Refund НЕ викликано (подвійний рух грошей)");
    }
    // 3) Решта «невідомо, чи виконалось» — так само без відкату.
    for (const char* code : { "DISCONNECTED", "DESYNC", "SEND_FAILED", "BAD_RESPONSE" }) {
        FakeAcquiringState st;
        st.voidResult = ResultEnvelope::Fail(code, "Збій зв'язку");
        VoidProbeFacade f(&st);
        ResultEnvelope r = f.RunVoid(100.50, "555000111");
        const std::string name =
            std::string("Void=") + code + " -> відмова без відкату на повернення";
        CHECK(!r.ok && r.code == code && st.refundCalls == 0, name.c_str());
    }
}
```

Зареєструвати в `main()` поруч із `TestFacadeSmoke();`:
```cpp
    TestVoidFallbackOnlyOnUnsupported();
```

- [ ] **Крок 7: Перевести `AddinEcrBpo3004`/`AddinEcrBpo4000` на нову базу, видалити `AddinEcrBpoBase`**

В обох `.h`: `#include "AddinEcrBpoBase.h"` → `#include "AcquiringFacadeBase.h"`,
`: public AddinEcrBpoBase` → `: public AcquiringFacadeBase`. `.cpp` не міняються, окрім
однієї правки в 4000 — перевірка часткового скасування починає питати можливості:

```cpp
            // Часткове скасування дозволено лише коли драйвер його задекларував.
            // Конфігурація мала б відсіяти виклик ДО драйвера за прапорцем
            // PartialCancellation, але якщо він усе-таки дійшов (інша гілка, інша
            // збірка) — відмовляємо явно, а не мовчки скасовуємо на іншу суму.
            const double original = VariantToDouble(originalAmount);
            if (original > 0.0 && !Capabilities().partialCancellation) {
                return MapEnvToBool(Unsupported("Часткове скасування"));
            }
```

Видалити `src/components/AddinEcrBpoBase.{h,cpp}` (`git rm`).

⚠️ `ОплатитьПлатежнойКартойCВыдачейНаличных` і `ПолучитьОперацииПоКартам` лишаються чесними
відмовами через `Unsupported(...)`: відповідних операцій в `IAcquiringDriver` немає навмисно —
їх додасть другий протокол, коли справді вмітиме (спека, «не вгадуємо наперед»).

- [ ] **Крок 8: CMake — ціль `acquiring_facade_component`, новий склад `ecr_bpo_facade_component`**

У `CMake/components.cmake`:

```cmake
## @var acquiring_facade_component
## @brief Семантика еквайрингу поверх IAcquiringDriver (AcquiringFacadeBase) + ревізійні
##        шари. ПРОТОКОЛУ НЕ ЗНАЄ — новий протокол реалізує IAcquiringDriver і додає два
##        тонкі класи, не чіпаючи цю ціль.
add_library(acquiring_facade_component OBJECT
    src/drivers/IAcquiringDriver.h
    src/components/AcquiringFacadeBase.h
    src/components/AcquiringFacadeBase.cpp
)
set_target_properties(acquiring_facade_component PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(acquiring_facade_component PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src
    ${SPDLOG_INCLUDE_DIR} ${NLOHMANN_JSON_INCLUDE_DIR})
target_compile_definitions(acquiring_facade_component PRIVATE _WINDOWS UNICODE _UNICODE)
add_dependencies(acquiring_facade_component base_component spdlog nlohmann_json
    helpers_component platform_component bpo_facade_component)
```

`ecr_bpo_facade_component`: у списку джерел `AddinEcrBpoBase.*` замінити на нічого
(файли видалено), лишити `AddinEcrBpo3004.*` і `AddinEcrBpo4000.*`; у `add_dependencies`
дописати `acquiring_facade_component`.
`driver_ecr_privatjson_component`: додати `EcrPrivatJsonAcquiring.h`/`.cpp` до джерел.

У `tests/CMakeLists.txt`, ціль `ecr_privatjson_selftest` — додати до списку джерел
(потрібні для `TestVoidFallbackOnlyOnUnsupported` із кроку 6):
```cmake
    $<TARGET_OBJECTS:bpo_facade_component>          # BpoFacadeBase
    $<TARGET_OBJECTS:acquiring_facade_component>    # AcquiringFacadeBase (RunVoid під тестом)
```

`HEADER_FILES`/`SOURCE_FILES`: замінити `AddinEcrBpoBase.*` на `AcquiringFacadeBase.*`,
додати `src/drivers/IAcquiringDriver.h`, `src/drivers/ecr_privatjson/EcrPrivatJsonAcquiring.*`.
`add_library(${TARGET} SHARED …)`: додати `$<TARGET_OBJECTS:acquiring_facade_component>`.

- [ ] **Крок 9: ⚠️ `/utf-8` для `acquiring_facade_component`**

```cmake
    target_compile_options(acquiring_facade_component PRIVATE /utf-8)
```

- [ ] **Крок 10: Гейт на обох архітектурах**

```powershell
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```
Особливо: `L3-bpo: скасування виконано поверненням за RRN (VoidAsRefund)`,
`L3-bpo: ПолучитьОшибку код UNSUPPORTED == 3`,
`L3-bpo4000: часткове скасування явно відхилено` — саме їх зачіпає нова логіка.
Плюс нові в `L0.7`: `Void=TIMEOUT -> Refund НЕ викликано (подвійний рух грошей)` і чотири
`Void=<код> -> відмова без відкату на повернення`.
`git status` не має показувати змін у `tests/ecr_native_host.cpp`.

- [ ] **Крок 11: Коміт**

```bash
git add -A src/drivers src/components CMake
git add tests/ecr_privatjson_selftest.cpp tests/CMakeLists.txt
git commit -m "refactor(bpo): IAcquiringDriver + адаптер ПриватБанку, семантика еквайрингу без протоколу"
```

---

### Task 3: Ревізійні шари й тонкі конкретні фасади

`AcquiringBpo3004`/`AcquiringBpo4000` містять рівно розкладку параметрів і число ревізії;
`ECRPrivatBPO3004`/`ECRPrivatBPO4000` — рівно `MakeDriver()` + реєстрацію компоненти.

**Files:**
- Create: `src/components/AcquiringBpo3004.{h,cpp}`, `src/components/AcquiringBpo4000.{h,cpp}`
- Create: `src/components/EcrPrivatBpo3004.{h,cpp}`, `src/components/EcrPrivatBpo4000.{h,cpp}`
- Delete: `src/components/AddinEcrBpo3004.{h,cpp}`, `src/components/AddinEcrBpo4000.{h,cpp}`
- Modify: `CMake/components.cmake`
- Test: `tests/ecr_native_host.cpp` (**не редагувати**)

**Interfaces:**
- Consumes: `AcquiringFacadeBase` (Задача 2), `EcrPrivatJsonAcquiring` (Задача 2),
  `REGISTER_COMPONENT`.
- Produces: класи компонент `ECRPrivatBPO3004` і `ECRPrivatBPO4000` у реєстрі DLL (імена
  для 1С **не змінюються** — `ecr_native_host` шукає їх через `GetClassObject`).

---

- [ ] **Крок 1: `AcquiringBpo3004.{h,cpp}` — перейменування `AddinEcrBpo3004` без реєстрації**

`git mv src/components/AddinEcrBpo3004.h src/components/AcquiringBpo3004.h` (те саме для `.cpp`),
далі в них:

* клас `AddinEcrBpo3004` → `AcquiringBpo3004`, база `AcquiringFacadeBase`;
* **прибрати** `static std::vector<std::u16string> names;` і рядок `REGISTER_COMPONENT(...)` —
  клас абстрактний (`MakeDriver()` лишається чистим), реєструє його нащадок;
* конструктор стає `protected`, тіло:
  ```cpp
  AcquiringBpo3004::AcquiringBpo3004() {
      RegisterSystemMethods();
      RegisterAcquiringMethods();
      RegisterPaymentMethods();
  }
  ```
  (`REPORT_INFO` про ініціалізацію переїжджає в конкретний фасад — там видно протокол);
* `RegisterPaymentMethods()`, анонімний `TryParseAmount` і `fillOut` — **дослівно без змін**;
* докоментар класу перефразувати: шар описує РОЗКЛАДКУ, протоколу не знає.

- [ ] **Крок 2: `AcquiringBpo4000.{h,cpp}` — те саме для ревізії 4000**

`git mv` обох файлів, ті самі чотири правки. `RegisterPaymentMethods` лишається дослівно
(включно з правкою `Capabilities().partialCancellation` із Задачі 2).

- [ ] **Крок 3: Створити `src/components/EcrPrivatBpo3004.h`**

```cpp
#pragma once
#include "AcquiringBpo3004.h"
#include <memory>
#include <string>
#include <vector>

/// БПО-фасад еквайрингу ПриватБанк, ревізія інтерфейсу 3004 (сімка параметрів).
/// Клас у 1С: "AddIn.<символьне ім'я>.ECRPrivatBPO3004" — адміністратор обирає його
/// записом довідника драйверів; автовизначення ревізії в БПО немає (§2.3 контракту).
class EcrPrivatBpo3004 : public AcquiringBpo3004 {
public:
    static std::vector<std::u16string> names;
    EcrPrivatBpo3004();

protected:
    std::unique_ptr<IAcquiringDriver> MakeDriver() const override;
};
```

- [ ] **Крок 4: Створити `src/components/EcrPrivatBpo3004.cpp`**

```cpp
#include "../core/pch.h"
#include "EcrPrivatBpo3004.h"
#include "../drivers/ecr_privatjson/EcrPrivatJsonAcquiring.h"
#include "../helpers/ServiceTools.h"

REGISTER_COMPONENT(u"ECRPrivatBPO3004", EcrPrivatBpo3004)

EcrPrivatBpo3004::EcrPrivatBpo3004() {
    REPORT_INFO("Ініціалізація БПО-фасаду еквайрингу ПриватБанк, ревізія 3004");
}

std::unique_ptr<IAcquiringDriver> EcrPrivatBpo3004::MakeDriver() const {
    return std::make_unique<EcrPrivatJsonAcquiring>();
}
```

- [ ] **Крок 5: Створити `EcrPrivatBpo4000.{h,cpp}` — дослівно те саме для 4000**

Ті самі два файли з заміною `3004` → `4000` скрізь: клас `EcrPrivatBpo4000`, база
`AcquiringBpo4000`, `REGISTER_COMPONENT(u"ECRPrivatBPO4000", EcrPrivatBpo4000)`, текст
`REPORT_INFO`. Докоментар класу — з `AddinEcrBpo4000.h` (дев'ятка/десятки, попередження
про вибір адміністратором).

- [ ] **Крок 6: CMake**

`ecr_bpo_facade_component` — новий склад:
```cmake
add_library(ecr_bpo_facade_component OBJECT
    src/components/EcrPrivatBpo3004.h
    src/components/EcrPrivatBpo3004.cpp
    src/components/EcrPrivatBpo4000.h
    src/components/EcrPrivatBpo4000.cpp
)
```
`acquiring_facade_component` — дописати `AcquiringBpo3004.{h,cpp}` і `AcquiringBpo4000.{h,cpp}`.
`HEADER_FILES`/`SOURCE_FILES` — замінити `AddinEcrBpo3004/4000.*` на чотири нові пари.

- [ ] **Крок 7: Гейт на обох архітектурах**

```powershell
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```
Ключові CHECK: `L3-bpo: компонента ECRPrivatBPO3004 створена через DLL`,
`L3-bpo4000: компонента ECRPrivatBPO4000 створена через DLL` — якщо вони червоні,
`REGISTER_COMPONENT` не спрацював (класи лишились абстрактними або зникла анти-стрип reference).

- [ ] **Крок 8: Коміт**

```bash
git add -A src/components CMake
git commit -m "refactor(bpo): ревізійні шари 3004/4000 окремо від конкретних фасадів ПриватБанку"
```

---

### Task 4: `LabelPrinterDriver::Probe` і `LabelXml::ProfileFromParameters`

Дві примітиви драйверного рівня, без яких фасад принтера не можна перевести на контракт:
профіль підключення з **мапи** параметрів (а не з XML) і форсоване відкриття транспорту для
`ТестУстройства`. Тут TDD у чистому вигляді — тест пишеться першим.

**Files:**
- Modify: `src/drivers/label_printer/LabelXml.h`, `src/drivers/label_printer/LabelXml.cpp:172-230`
- Modify: `src/drivers/label_printer/LabelPrinterDriver.h`, `…/LabelPrinterDriver.cpp`
- Test: `tests/label_printer_selftest.cpp`

**Interfaces:**
- Consumes: `labelprinter::DeviceProfile`, `ITransport`, `ResultEnvelope`.
- Produces (Задача 5 спирається дослівно):
  - `static bool LabelXml::ProfileFromParameters(const std::map<std::string,std::string>& params,
    DeviceProfile& out, std::string& err)`
  - `ResultEnvelope LabelPrinterDriver::Probe(const std::string& deviceId)`

---

- [ ] **Крок 1: Написати падаючі тести**

У `tests/label_printer_selftest.cpp` додати дві функції перед `int main()`:

```cpp
// Профіль підключення з МАПИ параметрів: контракт БПО подає їх по одному через
// УстановитьПараметр, а не XML-пакетом. Семантика має збігатися з XML-шляхом.
static void TestProfileFromParameters() {
    std::map<std::string, std::string> p{
        {"TransportKind", "tcp"}, {"Host", "192.168.0.50"}, {"Port", "9100"},
        {"DotsPerMm", "12"}, {"Darkness", "20"}, {"UnknownParam", "ignore-me"},
    };
    DeviceProfile dp; std::string err;
    CHECK(LabelXml::ProfileFromParameters(p, dp, err), "ProfileFromParameters ok");
    CHECK(dp.transport == DeviceProfile::Transport::Tcp, "map: TransportKind=tcp");
    CHECK(dp.host == "192.168.0.50" && dp.port == 9100, "map: host+port");
    CHECK(dp.dotsPerMm == 12 && dp.darkness == 20, "map: dotsPerMm+darkness, unknown ignored");

    // Порожня мапа -> дефолти профілю (spooler), не помилка: 1С може не заповнити нічого,
    // і відмова має прийти від транспорту з конкретикою, а не від розбору.
    std::map<std::string, std::string> empty;
    DeviceProfile dp2; std::string err2;
    CHECK(LabelXml::ProfileFromParameters(empty, dp2, err2), "empty map -> defaults, not error");
    CHECK(dp2.transport == DeviceProfile::Transport::Spooler && dp2.port == 9100,
          "empty map keeps DeviceProfile defaults");
}

// Probe форсує ліниву Open і повертає транспорт у попередній стан. Саме на цьому
// стоїть ТестУстройства: Connect сам по собі досяжності НЕ доводить.
static void TestDriverProbe() {
    LabelPrinterDriver drv;
    int opens = 0, closes = 0, sends = 0;
    drv.SetTransportFactoryForTest([&](const DeviceProfile&) -> std::unique_ptr<ITransport> {
        struct Fake : ITransport {
            int *o, *c, *s; bool open = false;
            bool Open() override { ++(*o); open = true; return true; }
            bool Close() override { ++(*c); open = false; return true; }
            bool IsOpen() const override { return open; }
            int Send(const std::vector<uint8_t>& d) override { ++(*s); return (int)d.size(); }
            void SetDataReceivedCallback(DataReceivedCallback) override {}
            void SetErrorCallback(ErrorCallback) override {}
            void SetConnectionStateCallback(ConnectionStateCallback) override {}
        };
        auto f = std::make_unique<Fake>(); f->o = &opens; f->c = &closes; f->s = &sends;
        return f;
    });
    DeviceProfile dp; dp.dotsPerMm = 8;
    std::string id = drv.Connect(dp);
    CHECK(opens == 0, "Connect does not open transport (lazy)");

    ResultEnvelope r = drv.Probe(id);
    CHECK(r.ok, "Probe on reachable transport -> ok");
    CHECK(opens == 1 && closes == 1, "Probe opens and closes exactly once");
    CHECK(sends == 0, "Probe sends NOTHING (no init packet, printer state untouched)");

    ResultEnvelope bad = drv.Probe("no-such-device");
    CHECK(!bad.ok && bad.code == "NOT_CONNECTED", "Probe on unknown DeviceID -> NOT_CONNECTED");
    drv.Disconnect(id);
}
```

Зареєструвати їх у `main()` після `TestXml();`:
```cpp
    TestProfileFromParameters();
    TestDriverProbe();
```
Додати `#include <map>` до заголовків файлу, якщо його там немає.

- [ ] **Крок 2: Переконатися, що тести НЕ компілюються**

```powershell
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
```
Очікування: помилка компіляції `label_printer_selftest` —
`'ProfileFromParameters': is not a member of 'labelprinter::LabelXml'` і
`'Probe': is not a member of 'labelprinter::LabelPrinterDriver'`. Це і є «червоний» стан.

- [ ] **Крок 3: `LabelXml::ProfileFromParameters` — реалізація**

У `LabelXml.h` додати `#include <map>` і оголошення:

```cpp
    /// Ті самі параметри, але вже розібрані підсистемою БПО в мапу: контракт кличе
    /// УстановитьПараметр по одному, а не подає XML-пакет. ParseConnectionParameters
    /// зводиться до XML→мапа + цей виклик, тож семантика параметрів — в ОДНОМУ місці.
    /// Невідомі параметри ігноруються (вимога БПО); порожня мапа -> дефолти профілю.
    static bool ProfileFromParameters(const std::map<std::string, std::string>& params,
                                      DeviceProfile& out, std::string& err);
```

У `LabelXml.cpp` тіло циклу `for (pugi::xml_node p : params.children("Parameter"))`
(рядки 189-217) **переїжджає** в нову функцію як цикл по мапі:

```cpp
bool LabelXml::ProfileFromParameters(const std::map<std::string, std::string>& params,
                                     DeviceProfile& out, std::string& err) {
    err.clear();
    for (const auto& kv : params) {
        const std::string& name = kv.first;
        const std::string& value = kv.second;
        if (name == "TransportKind") {
            std::string v;
            for (char c : value) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            out.transport = (v == "tcp") ? DeviceProfile::Transport::Tcp : DeviceProfile::Transport::Spooler;
        } else if (name == "PrinterName") {
            out.printerName = value;
        } else if (name == "Host") {
            out.host = value;
        } else if (name == "Port") {
            out.port = ParseInt(value.c_str(), out.port);
        } else if (name == "DotsPerMm") {
            out.dotsPerMm = ParseInt(value.c_str(), out.dotsPerMm);
        } else if (name == "Darkness") {
            out.darkness = ParseInt(value.c_str(), out.darkness);
        } else if (name == "Speed") {
            out.speed = ParseInt(value.c_str(), out.speed);
        } else if (name == "LabelWidthMm") {
            out.labelWidthMm = ParseDouble(value.c_str(), out.labelWidthMm);
        } else if (name == "LabelHeightMm") {
            out.labelHeightMm = ParseDouble(value.c_str(), out.labelHeightMm);
        } else if (name == "HomeXDots") {
            out.homeXDots = ParseInt(value.c_str(), out.homeXDots);
        } else if (name == "HomeYDots") {
            out.homeYDots = ParseInt(value.c_str(), out.homeYDots);
        }
        // невідомі параметри ігноруємо (вимога БПО)
    }
    return true;
}
```

А `ParseConnectionParameters` зводиться до XML→мапа + делегування (перевірки кореневого
вузла й помилки розбору лишаються дослівно, рядки 174-188 і хвіст функції):

```cpp
        std::map<std::string, std::string> flat;
        for (pugi::xml_node p : params.children("Parameter"))
            flat[p.attribute("Name").value()] = p.attribute("Value").value();
        return ProfileFromParameters(flat, out, err);
```

- [ ] **Крок 4: `LabelPrinterDriver::Probe` — реалізація**

У `LabelPrinterDriver.h` після `IsConnected`:

```cpp
    // Форсує ліниву Open і одразу закриває — для ТестУстройства БПО. Доводить
    // ДОСЯЖНІСТЬ (TCP: конект на host:port; spooler: OpenPrinter на черзі) і
    // НІЧОГО НЕ ДРУКУЄ: ініт-пакет змінив би стан принтера (темність/швидкість/
    // розмір), а адміністратор, який тисне «тест», на це не підписувався.
    // Уже відкритий транспорт лишається відкритим.
    ResultEnvelope Probe(const std::string& deviceId);
```

У `LabelPrinterDriver.cpp` після `SendBytes` (рядок 100):

```cpp
ResultEnvelope LabelPrinterDriver::Probe(const std::string& deviceId) {
    std::shared_ptr<DeviceContext> ctx = Lookup(deviceId);
    if (!ctx)
        return ResultEnvelope::Fail("NOT_CONNECTED", "Пристрій не підключено: " + deviceId);

    std::lock_guard<std::mutex> lk(ctx->m);
    if (!ctx->transport)
        return ResultEnvelope::Fail("TRANSPORT_ERROR", "Транспорт пристрою не ініціалізовано");
    if (ctx->transport->IsOpen()) {
        NEUTRAL_REPORT_INFO(kTag, "Канал до пристрою вже відкритий: " + deviceId);
        return ResultEnvelope::Ok();
    }
    if (!ctx->transport->Open())
        return ResultEnvelope::Fail("TRANSPORT_ERROR", "Не вдалося відкрити канал до принтера");
    ctx->transport->Close();   // повертаємо ліниво-закритий стан
    NEUTRAL_REPORT_INFO(kTag, "Канал до пристрою доступний: " + deviceId);
    return ResultEnvelope::Ok();
}
```

- [ ] **Крок 5: Прогнати гейт — тести мають позеленіти**

```powershell
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```
Очікування: усі нові CHECK — `[PASS]`; наявні `parse ConnectionParameters ok`,
`TransportKind=tcp`, `host+port parsed`, `dotsPerMm+darkness parsed, unknown ignored`
теж лишаються `[PASS]` (рефакторинг XML-шляху нічого не змінив).

- [ ] **Крок 6: Коміт**

```bash
git add src/drivers/label_printer/LabelXml.h src/drivers/label_printer/LabelXml.cpp \
        src/drivers/label_printer/LabelPrinterDriver.h src/drivers/label_printer/LabelPrinterDriver.cpp \
        tests/label_printer_selftest.cpp
git commit -m "feat(label): Probe для ТестУстройства + профіль підключення з мапи параметрів"
```

---

### Task 5: `AddinLabelPrinter` на контракт БПО

Головне завдання плану: фасад принтера переходить на короткі імена, `УстановитьПараметр` і
один пристрій. Тест пишеться першим і має впасти на `FindMethod` — саме так виглядав би
справжній дефект, якби його ловили тести.

**Files:**
- Modify: `tests/label_native_host.cpp` (переписати `main`)
- Modify: `tests/label_printer_selftest.cpp:474-483` (`TestFacadeSmoke`)
- Modify: `tests/CMakeLists.txt:122-129`
- Modify: `src/components/AddinLabelPrinter.h`, `src/components/AddinLabelPrinter.cpp`
- Modify: `CMake/components.cmake` (`label_facade_component` → залежність від бази)

**Interfaces:**
- Consumes: `BpoFacadeBase` (Задача 1), `LabelXml::ProfileFromParameters` і
  `LabelPrinterDriver::Probe` (Задача 4).
- Produces: компонента `LabelPrinter` з контрактом БПО — 11 системних методів
  (`ПолучитьРевизиюИнтерфейса`, `ПолучитьНомерВерсии`, `ПолучитьОписание`, `ПолучитьПараметры`,
  `УстановитьПараметр`, `Подключить`, `Отключить`, `ТестУстройства`, `ПолучитьОшибку`,
  `ПолучитьДополнительныеДействия`, `ВыполнитьДополнительноеДействие`) + два функціональні
  (`ИнициализацияПринтера`, `ПечатьЭтикеток`). Ревізія — `3004`.

---

- [ ] **Крок 1: Переписати `tests/label_native_host.cpp` під новий контракт**

Секції 1-3 (емулятор, завантаження DLL, `Init`+`setMemManager`) лишаються дослівно.
До блоку `#include` додати `<vector>` і `<cstring>` (потрібні для `callBool` і `memcpy`).
Замінити все від коментаря `// 4) ПодключитьОборудование…` до `pDestroyObject(&comp);` на:

```cpp
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

            // Дочекатися, поки емулятор прийме ZPL із сокета.
            std::string zpl;
            for (int i = 0; i < 50; ++i) {
                zpl = emu.LastZpl();
                if (zpl.find("^XA") != std::string::npos) break;
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
```

Оновити докоментар файлу (рядки 3-10): тепер харнес кличе **короткі** імена контракту БПО,
а не довгі за ІТС.

- [ ] **Крок 2: Оновити смоук у `label_printer_selftest.cpp`**

Замінити `TestFacadeSmoke` (рядки 474-483) на:

```cpp
static void TestFacadeSmoke() {
    AddInNative* comp = AddInNative::CreateObject(u"LabelPrinter");
    CHECK(comp != nullptr, "Facade: CreateObject(LabelPrinter) -> не null");
    if (!comp) return;
    MockMem mem; MockConn conn;
    comp->Init(&conn);
    comp->setMemManager(&mem);

    // Імена контракту БПО, які РЕАЛЬНО кличе конфігурація (bpo-contract.md §2).
    // GetNMethods тут не годиться: він був зеленим і на старих довгих іменах.
    const wchar_t* contract[] = {
        L"ПолучитьРевизиюИнтерфейса", L"ПолучитьНомерВерсии", L"ПолучитьОписание",
        L"ПолучитьПараметры", L"УстановитьПараметр", L"Подключить", L"Отключить",
        L"ТестУстройства", L"ПолучитьОшибку", L"ПолучитьДополнительныеДействия",
        L"ВыполнитьДополнительноеДействие", L"ИнициализацияПринтера", L"ПечатьЭтикеток"
    };
    bool all = true;
    for (const wchar_t* n : contract)
        if (comp->FindMethod(n) < 0) { all = false; std::printf("  немає: %ls\n", n); }
    CHECK(all, "Facade: усі 13 імен контракту БПО зареєстровано");

    const wchar_t* legacy[] = { L"ПодключитьОборудование", L"ПараметрыОборудования",
                                L"ТестированиеОборудования", L"ОтключитьОборудование",
                                L"УстановитьИнформациюПриложения" };
    bool none = true;
    for (const wchar_t* n : legacy)
        if (comp->FindMethod(n) >= 0) { none = false; std::printf("  лишилось: %ls\n", n); }
    CHECK(none, "Facade: старі довгі імена за ІТС прибрано");

    delete comp;
}
```

- [ ] **Крок 3: `tests/CMakeLists.txt` — селфтест лінкує базу фасадів**

У `target_sources(label_printer_selftest PRIVATE …)` (рядок 122) додати **перед**
`label_facade_component`:
```cmake
    $<TARGET_OBJECTS:bpo_facade_component>            # BpoFacadeBase — база фасаду принтера
```

- [ ] **Крок 4: Прогнати — обидва тести мають впасти**

```powershell
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
```
Очікування: `L-p1` і `L-p3` — `FAIL`. У виводі: `немає: ПолучитьПараметры`,
`немає: УстановитьПараметр`, `немає: Подключить`, `немає: ТестУстройства`, …,
`лишилось: ПодключитьОборудование`. Це і є той самий дефект, який тести не ловили.

- [ ] **Крок 5: Переписати `src/components/AddinLabelPrinter.h`**

```cpp
#pragma once
#include "BpoFacadeBase.h"
#include "../drivers/label_printer/LabelPrinterDriver.h"
#include <string>
#include <vector>

/// Компонента 1С «Принтер этикеток» — БПО-фасад над LabelPrinterDriver.
///
/// Реалізує контракт «Подключаемое оборудование» (docs/architecture/bpo-contract.md §2):
/// системна половина — у BpoFacadeBase, тут лише гачки під тип обладнання плюс два
/// функціональні методи. Ревізія — 3004 (§3.4 спеки): гілок за ревізією для друку
/// етикеток у конфігурації немає, а з 4000 1С починає кликати УстановитьЛокализацию,
/// якого ми не реалізуємо.
///
/// ⚠️ ОДИН об'єкт = ОДИН пристрій. Контракт накопичує параметри на об'єкті ДО
/// Подключить, тож параметри другого принтера затерли б перший. Сам
/// LabelPrinterDriver лишається мульти-пристроєвим — це обмеження ФАСАДУ.
///
/// Друк синхронний і односпрямований — DeviceSession/JobEngine не задіяні.
class AddinLabelPrinter : public BpoFacadeBase {
public:
    static std::vector<std::u16string> names;
    AddinLabelPrinter();
    ~AddinLabelPrinter() override;

protected:
    int InterfaceRevision() const override { return 3004; }
    DriverInfo BuildDriverInfo() const override;
    std::string BuildSettingsXml() const override;
    bool AcceptEquipmentType(const std::string& value) const override;
    bool OpenDevice(std::string& deviceIdOut) override;
    void CloseDevice() override;
    bool ProbeDevice(std::string& resultOut, bool& demoOut) override;
    // BuildActionsXml/RunAction — дефолтні: додаткових дій у принтера немає (§3.5 спеки).

private:
    void RegisterPrinterMethods();

    labelprinter::LabelPrinterDriver driver_;
};
```

- [ ] **Крок 6: Переписати `src/components/AddinLabelPrinter.cpp`**

`BuildTableParametersXml()` (рядки 66-104) переїжджає в `BuildSettingsXml()` **дослівно** —
формат уже правильний. `BuildDriverDescriptionXml()` і локальний `CodeToInt` **видаляються**
(їх робить база). Решта:

```cpp
#include "../core/pch.h"
#include "AddinLabelPrinter.h"
#include "../helpers/ServiceTools.h"
#include "../drivers/label_printer/LabelXml.h"
#include <string>

using namespace labelprinter;

REGISTER_COMPONENT(u"LabelPrinter", AddinLabelPrinter)

namespace {

/// Людський опис цілі підключення для РезультатТеста — адміністратор має бачити,
/// що саме перевірялось, а не просто «помилка».
std::string DescribeTarget(const DeviceProfile& p) {
    if (p.transport == DeviceProfile::Transport::Tcp)
        return "tcp://" + p.host + ":" + std::to_string(p.port);
    return "черга Windows \"" + p.printerName + "\"";
}

} // namespace

AddinLabelPrinter::AddinLabelPrinter() {
    REPORT_INFO("Ініціалізація компоненти LabelPrinter");
    RegisterSystemMethods();
    RegisterPrinterMethods();
}

AddinLabelPrinter::~AddinLabelPrinter() {
    REPORT_INFO("Завершення роботи компоненти LabelPrinter");
    if (!DeviceId().empty()) driver_.Disconnect(DeviceId());
}

// ============================ гачки BpoFacadeBase ============================

BpoFacadeBase::DriverInfo AddinLabelPrinter::BuildDriverInfo() const {
    // logEnabled=false: вимога ІТС про лог за замовчуванням стосується ККТ і
    // POSTerminal; принтер етикеток у той перелік не входить, і параметрів
    // LogPath/LogLevel у його формі налаштувань немає.
    return DriverInfo{ "Драйвер принтера этикеток (SimplyAddinConnect)",
                       "Друк етикеток на ZPL-принтер через spooler-RAW або TCP:9100",
                       "LabelPrinter",
                       /*logEnabled*/ false };
}

std::string AddinLabelPrinter::BuildSettingsXml() const {
    return /* дослівно літерал BuildTableParametersXml із AddinLabelPrinter.cpp:66-104 */;
}

bool AddinLabelPrinter::AcceptEquipmentType(const std::string& value) const {
    // ⚠️ 1С передає ІМ'Я ЗНАЧЕННЯ ПЕРЕЛІКУ Enums/ТипыПодключаемогоОборудования —
    // ПринтерЭтикеток, а не англійський рядок LabelPrinter з таблиці ІТС.
    // Приймаємо обидва: коштує нічого, страхує від наступної редакції.
    return value == "ПринтерЭтикеток" || ToUpperAscii(value) == "LABELPRINTER";
}

bool AddinLabelPrinter::OpenDevice(std::string& deviceIdOut) {
    DeviceProfile profile;
    std::string err;
    if (!LabelXml::ProfileFromParameters(Params(), profile, err)) {
        SetError(CodeToInt("BAD_INPUT"), err.empty() ? "Некоректні параметри підключення" : err);
        return false;
    }
    const std::string id = driver_.Connect(profile);
    if (id.empty()) {
        SetError(CodeToInt("TRANSPORT_ERROR"), "Не вдалося зареєструвати пристрій");
        return false;
    }
    deviceIdOut = id;   // прокидуємо СПРАВЖНІЙ id драйвера — лог фасаду й драйвера збігається
    return true;
}

void AddinLabelPrinter::CloseDevice() {
    if (!DeviceId().empty()) driver_.Disconnect(DeviceId());
}

bool AddinLabelPrinter::ProbeDevice(std::string& resultOut, bool& demoOut) {
    demoOut = false;                       // демо-режиму драйвер не має
    DeviceProfile profile;
    std::string err;
    if (!LabelXml::ProfileFromParameters(Params(), profile, err)) {
        const std::string text = err.empty() ? std::string("Некоректні параметри підключення") : err;
        SetError(CodeToInt("BAD_INPUT"), text);
        resultOut = text;
        return false;
    }
    // Тест іде на ОКРЕМОМУ пристрої драйвера, щоб не чіпати активне підключення:
    // адміністратор може натиснути «Тест устройства» при вже підключеному принтері.
    const std::string probeId = driver_.Connect(profile);
    if (probeId.empty()) {
        SetError(CodeToInt("TRANSPORT_ERROR"), "Не вдалося створити канал до принтера");
        resultOut = "Не вдалося створити канал до принтера";
        return false;
    }
    const ResultEnvelope env = driver_.Probe(probeId);
    driver_.Disconnect(probeId);

    const std::string target = DescribeTarget(profile);
    if (!env.ok) {
        SetError(CodeToInt(env.code), env.description);
        resultOut = target + " — недоступно: " + env.description;
        return false;
    }
    resultOut = target + " — доступно";
    ClearError();
    return true;
}

// ============================ функціональні методи ============================

void AddinLabelPrinter::RegisterPrinterMethods() {

    // Ініціалізація принтера: IN ИДУстройства -> BOOL.
    AddFunction(u"InitializePrinter", u"ИнициализацияПринтера",
        Ret([this](VH deviceId) -> bool {
            try {
                const std::string id = VariantToString(deviceId);
                if (!CheckDeviceId(id)) return false;
                return MapEnvToBool(driver_.InitializePrinter(id));
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка ИнициализацияПринтера: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"DeviceID", u"ИДУстройства", true, {} } });

    // Друк етикеток: IN ИДУстройства, IN ТаблицаЭтикеток(XML), IN СтатусПакета -> BOOL.
    // СтатусПакета — first/regular/last: first несе <Formatting> і скидає кеш формату,
    // last після друку його очищає.
    AddFunction(u"PrintLabels", u"ПечатьЭтикеток",
        Ret([this](VH deviceId, VH labelsTable, VH packageStatus) -> bool {
            try {
                const std::string id = VariantToString(deviceId);
                if (!CheckDeviceId(id)) return false;
                const std::string xml = VariantToString(labelsTable);
                LabelBatch batch;
                std::string err;
                if (!LabelXml::ParseLabelsTable(xml, batch, err)) {
                    const std::string text = err.empty() ? "Некоректний пакет ТаблицаЭтикеток" : err;
                    SetError(CodeToInt("BAD_INPUT"), text);
                    REPORT_ERROR("Помилка розбору ТаблицаЭтикеток: " + text);
                    return false;
                }
                return MapEnvToBool(driver_.PrintLabels(id, batch, VariantToString(packageStatus)));
            } catch (const std::exception& e) {
                SetError(-1, e.what());
                REPORT_ERROR(std::string("Помилка ПечатьЭтикеток: ") + e.what());
                return false;
            }
        }),
        std::vector<ParamSpec>{
            ParamSpec{ u"DeviceID", u"ИДУстройства", true, {} },
            ParamSpec{ u"LabelsTable", u"ДанныеДляВыгрузки", true, {} },
            ParamSpec{ u"PackageStatus", u"СтатусПакета", true, {} } });

    REPORT_INFO("Реєстрація методів LabelPrinter завершена");
}
```

⚠️ Другий параметр `ПечатьЭтикеток` названо `ДанныеДляВыгрузки` — саме так його називає
конфігурація (спека §2.1). Ім'я параметра на позиційний виклик не впливає, але має збігатися
з документом.

- [ ] **Крок 7: CMake — залежність фасаду принтера від бази**

`CMake/components.cmake`, `label_facade_component`: у `add_dependencies` дописати
`bpo_facade_component`.

- [ ] **Крок 8: Прогнати гейт — усе має позеленіти**

```powershell
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```
Очікування: `L-p1` і `L-p3` — `PASS`; `L2-ecr` лишається `PASS` (еквайринг не зачеплено).

- [ ] **Крок 9: Коміт**

```bash
git add src/components/AddinLabelPrinter.h src/components/AddinLabelPrinter.cpp \
        tests/label_native_host.cpp tests/label_printer_selftest.cpp tests/CMakeLists.txt \
        CMake/components.cmake
git commit -m "fix(label): LabelPrinter на реальний контракт БПО — короткі імена, УстановитьПараметр, один пристрій"
```

---

### Task 6: Тестова обробка 1С — область `ПринтерЭтикеток`

Єдиний прямий споживач старих імен. Статичний контроль BSL виклики компоненти **не перевіряє**
(ні збірка `.epf`, ні `/CheckModules`), тож пропустити цей крок означає лишити дві кнопки,
які мовчки падають під час виконання.

**Files:**
- Modify: `ExtDataProcessors/SimplyAddinConnect_test/SimplyAddinConnect/Forms/Форма/Ext/Form/Module.bsl:1244-1325`
- Modify: `ExtDataProcessors/SimplyAddinConnect_test/SimplyAddinConnect/Forms/Форма/Ext/Form.xml`
  (нова команда + кнопка)

**Interfaces:**
- Consumes: компонента `LabelPrinter` з контрактом БПО (Задача 5).
- Produces: форма, що покриває руками весь новий системний набір.

---

- [ ] **Крок 1: Переписати `ПодключитьПринтерLP`**

Замінити тіло процедури (рядки ~1244-1265) на:

```bsl
&НаКлиенте
Процедура ПодключитьПринтерLP(Команда)
	Если НЕ ОбъектLPГотов() Тогда Возврат; КонецЕсли;
	Попытка
		// Контракт БПО: параметри накопичуються ПООДИНЦІ, першим — тип обладнання,
		// і лише потім безпараметровий Подключить (docs/architecture/bpo-contract.md §2).
		ОбъектДрайвераLP.УстановитьПараметр("EquipmentType", "ПринтерЭтикеток");
		ОбъектДрайвераLP.УстановитьПараметр("TransportKind", "tcp");
		ОбъектДрайвераLP.УстановитьПараметр("Host", СокрЛП(АдресПринтераLP));
		ОбъектДрайвераLP.УстановитьПараметр("Port", Число(СокрЛП(ПортПринтераLP)));
		ОбъектДрайвераLP.УстановитьПараметр("DotsPerMm", Число(СокрЛП(DotsPerMmLP)));
		ОбъектДрайвераLP.УстановитьПараметр("Darkness", 10);
		ОбъектДрайвераLP.УстановитьПараметр("Speed", 4);

		ИДУстройстваLP = "";
		Успех = ОбъектДрайвераLP.Подключить(ИДУстройстваLP);
		ВывестиРезультатLP("Подключить (" + СокрЛП(АдресПринтераLP) + ":" + СокрЛП(ПортПринтераLP) + ") = "
			+ Строка(Успех) + "; ИДУстройства = " + ИДУстройстваLP + ?(Успех, "", " | " + ПрочитатьОшибкуLP()));
	Исключение
		ВывестиРезультатLP("Помилка Подключить: " + ОписаниеОшибки());
	КонецПопытки;
КонецПроцедуры
```

⚠️ `Port`/`DotsPerMm`/`Darkness`/`Speed` передаються **числами** — саме так їх передає форма
налаштувань БПО (вони оголошені `TypeValue="Number"`). Це навмисно: перевіряє толерантність
`VariantToString` на живому маршалінгу.

- [ ] **Крок 2: Переписати `ОтключитьПринтерLP`**

```bsl
&НаКлиенте
Процедура ОтключитьПринтерLP(Команда)
	Если НЕ ОбъектLPГотов() Тогда Возврат; КонецЕсли;
	Если НЕ ЗначениеЗаполнено(ИДУстройстваLP) Тогда ВывестиРезультатLP("Немає активного ИДУстройства"); Возврат; КонецЕсли;
	Попытка
		Успех = ОбъектДрайвераLP.Отключить(ИДУстройстваLP);
		ВывестиРезультатLP("Отключить = " + Строка(Успех));
		ИДУстройстваLP = "";
	Исключение
		ВывестиРезультатLP("Помилка Отключить: " + ОписаниеОшибки());
	КонецПопытки;
КонецПроцедуры
```

- [ ] **Крок 3: Додати процедуру `ТестУстройстваLP`**

Вставити після `ПодключитьПринтерLP`:

```bsl
&НаКлиенте
Процедура ТестУстройстваLP(Команда)
	Если НЕ ОбъектLPГотов() Тогда Возврат; КонецЕсли;
	Попытка
		// Форма налаштувань БПО кличе ТестУстройства ОДРАЗУ після УстановитьПараметр,
		// без Подключить — тому параметри ставимо тут же.
		ОбъектДрайвераLP.УстановитьПараметр("EquipmentType", "ПринтерЭтикеток");
		ОбъектДрайвераLP.УстановитьПараметр("TransportKind", "tcp");
		ОбъектДрайвераLP.УстановитьПараметр("Host", СокрЛП(АдресПринтераLP));
		ОбъектДрайвераLP.УстановитьПараметр("Port", Число(СокрЛП(ПортПринтераLP)));

		РезультатТеста = "";
		ДемоРежим = Ложь;
		Успех = ОбъектДрайвераLP.ТестУстройства(РезультатТеста, ДемоРежим);
		ВывестиРезультатLP("ТестУстройства = " + Строка(Успех) + " | " + РезультатТеста
			+ " | демо-режим: " + Строка(ДемоРежим) + ?(Успех, "", " | " + ПрочитатьОшибкуLP()));
	Исключение
		ВывестиРезультатLP("Помилка ТестУстройства: " + ОписаниеОшибки());
	КонецПопытки;
КонецПроцедуры
```

- [ ] **Крок 4: Оновити коментар-шапку області**

Рядки 1204-1208 — послідовність змінилась:

```bsl
// Тестовий харнес компоненти LabelPrinter (БПО-драйвер принтера етикеток).
// Послідовність: «Подключить компоненту» → «Створити обʼєкт LP» → «Тест устройства»
// (необовʼязково) → «Подключить принтер» → «Инициализировать» → «Напечатать этикетки».
// Контракт — короткі імена БПО (docs/architecture/bpo-contract.md §2); параметри
// підключення передаються ПООДИНЦІ через УстановитьПараметр ДО Подключить.
```

- [ ] **Крок 5: Додати команду й кнопку в `Form.xml`**

У блок `<UsualGroup name="ГруппаLPКнопки">`, **після** `<Button name="КнопкаПодключитьLP" …>`
(рядки 695-699):

```xml
						<Button name="КнопкаТестLP" id="414">
							<Type>CommandBarButton</Type>
							<CommandName>Form.Command.ТестУстройстваLP</CommandName>
							<ExtendedTooltip name="КнопкаТестLPTooltip" id="415"/>
						</Button>
```

У блок команд, після `<Command name="ОтключитьПринтерLP" id="36">…</Command>` (рядок 1573):

```xml
		<Command name="ТестУстройстваLP" id="416">
			<Title>
				<v8:item>
					<v8:lang>uk</v8:lang>
					<v8:content>Тест устройства LP</v8:content>
				</v8:item>
			</Title>
			<Action>ТестУстройстваLP</Action>
			<CurrentRowUse>DontUse</CurrentRowUse>
		</Command>
```

⚠️ `id` 414-416 вільні: максимальний наявний у файлі — 413. Звір перед вставкою:
```bash
grep -o 'id="[0-9]*"' "ExtDataProcessors/SimplyAddinConnect_test/SimplyAddinConnect/Forms/Форма/Ext/Form.xml" | grep -o '[0-9]*' | sort -n | tail -1
```
Точну структуру `<Button>` бери з сусіднього `КнопкаПодключитьLP` — вона в цьому файлі
канонічна.

- [ ] **Крок 6: Перевірити, що інших викликів старих імен не лишилось**

```bash
grep -rn "ПодключитьОборудование\|ПараметрыОборудования\|ТестированиеОборудования\|ОтключитьОборудование\|УстановитьИнформациюПриложения" \
  ExtDataProcessors/SimplyAddinConnect_test docs/ src/
```
Очікування: **нуль збігів** поза `docs/architecture/bpo-contract.md` (там вони згадуються як
доказ розбіжності з ІТС) — решту вичищено. Артефакт `ExtDataProcessors/.build/` не чіпаємо,
він перегенерується.

- [ ] **Крок 7: Синтаксична перевірка модуля (за наявності платформи 1С)**

```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
```
Останній крок скрипта перезбирає `bin/Release/SimplyAddinConnect.epf`. `SKIP`/`WARNING`
(немає платформи чи вихідників) — **не** провал збірки.

⚠️ Зелена збірка `.epf` **нічого не доводить** про виклики компоненти: 1С перевіряє синтаксис
BSL, але не існування методів нативної компоненти. Єдина справжня перевірка — натиснути
кнопки руками в 1С (Задача 7 спеки, поза цим планом).

- [ ] **Крок 8: Коміт**

```bash
git add "ExtDataProcessors/SimplyAddinConnect_test/SimplyAddinConnect/Forms/Форма/Ext/Form/Module.bsl" \
        "ExtDataProcessors/SimplyAddinConnect_test/SimplyAddinConnect/Forms/Форма/Ext/Form.xml"
git commit -m "test(1c): тестова обробка на новий контракт LabelPrinter + кнопка ТестУстройства"
```

---

### Task 7: Документація

Джерело правди про драйвер — рівно один документ на підсистему. Оновлюємо їх, а не
`AGENTS.md`/`CLAUDE.md` (вони лишаються драйвер-незалежними).

**Files:**
- Modify: `docs/architecture/bpo-contract.md` (§7, новий §2.6 про шарування)
- Modify: `docs/architecture/label_printer.md` (§1, §8, §10, §11)
- Modify: `docs/integration-1c/label_printer.md` (§1, §2, §3, §5, §6)
- Modify: `docs/architecture/README.md` (каталог підсистем, шари)
- Modify: `docs/architecture/ecrprivatjson.md` (згадка нового шару, якщо є посилання на `AddinEcrBpoBase`)

**Interfaces:**
- Consumes: фактичний стан коду після Задач 1-6.
- Produces: документація, узгоджена з кодом; знятий дефект-нотіс LabelPrinter.

---

- [ ] **Крок 1: `docs/architecture/bpo-contract.md` — шарування фасадів**

Додати новий підрозділ **§2.6 «Шарування фасадів»** після §2.3.1 з деревом класів зі спеки
(`BpoFacadeBase` → `AcquiringFacadeBase` → `AcquiringBpo3004/4000` → `EcrPrivatBpo*`,
а також `BpoFacadeBase` → `AddinLabelPrinter`), поясненням двох ортогональних осей
(ревізія vs протокол) і ціною нового протоколу (реалізувати `IAcquiringDriver` + два класи).

У **§7** («Стан драйверів») переписати рядок `LabelPrinter`:
```
| `LabelPrinter` | ✅ **відповідає контракту** (виправлено 2026-09-01): короткі системні імена,
`УстановитьПараметр`, один пристрій на об'єкт, ревізія 3004, `ТестУстройства` реально відкриває
транспорт. Покрито `label_native_host` (L-p3) — тест перевіряє САМЕ імена контракту й відсутність
старих довгих. Через штатну підсистему БПО на живій 1С ще не запускався. |
```
І прибрати з абзацу нижче згадку `AddinEcrBpoBase` — тепер це `BpoFacadeBase` +
`AcquiringFacadeBase`.

У **§4.2** дописати рядок про єдину числову таксономію `ПолучитьОшибку` з таблицею кодів
(скопіювати з Global Constraints цього плану) — зараз коди описані по драйверах окремо.

- [ ] **Крок 2: `docs/architecture/label_printer.md` — §8 повністю переписати**

Замінити таблиці системних методів на контракт із §2 `bpo-contract.md` (короткі імена),
`kInterfaceRevision = 4007` → `InterfaceRevision() == 3004`, додати:
* фасад успадковується від `BpoFacadeBase`; системна половина — там, тут лише гачки;
* один пристрій на об'єкт (обмеження ФАСАДУ; драйвер лишається мульти-пристроєвим);
* `ТестУстройства` = `LabelPrinterDriver::Probe` — фактичне відкриття/закриття транспорту,
  без ініт-пакета;
* нові числові коди помилок (`BAD_INPUT=12`, `TRANSPORT_ERROR=13`, `UNSUPPORTED_BARCODE=14`,
  `BARCODE_TOO_WIDE=15`, `RENDER_ERROR=16`, `EXCEPTION=11`, `NOT_CONNECTED=1`, `OK=0`) —
  замінити наявний перелік `1..7`.

У **§1** (шари) — рядок фасаду згадує `BpoFacadeBase`.
У **§7** дописати `Probe` до опису публічного API драйвера.
У **§10** оновити опис L-p3: тест кличе короткі імена контракту й перевіряє **відсутність**
старих довгих; додати згадку `ТестУстройства` й числових параметрів.
У **§11** додати `bpo_facade_component` до переліку цілей.

- [ ] **Крок 3: `docs/integration-1c/label_printer.md` — прикладний бік**

* **§1:** **прибрати блок «⚠️ ВІДОМИЙ ДЕФЕКТ»** цілком. Замінити послідовність підключення на
  контрактну: `ПолучитьРевизиюИнтерфейса` → `ПолучитьОписание` → `ПолучитьПараметры` →
  `УстановитьПараметр` × N → `Подключить` → `ИнициализацияПринтера` → `ПечатьЭтикеток`.
  Абзац про «реєстр `DeviceID → канал`» замінити: **один об'єкт = один принтер**; на два
  принтери — два об'єкти.
* **§2.2:** таблицю системних методів замінити на контрактну (13 рядків, як у смоуці Задачі 5);
  ревізія — `3004`.
* **§3:** «Параметри підключення» — з XML-пакета на `УстановитьПараметр(Имя, Значение)`;
  таблиця імен параметрів лишається, додати колонку про тип значення (Number передається
  **числом**). XML-форму `<Parameters>` лишити як історичну примітку, бо вона є в ІТС.
* **§5:** таблицю кодів помилок замінити на нову нумерацію (див. Крок 2).
* **§6.2/§6.5:** приклади коду переписати під `УстановитьПараметр` — **дослівно тим самим
  кодом**, що в Задачі 6 (тестова обробка й документація мають збігатися). Додати §6.6
  «Тест устройства».

- [ ] **Крок 4: `docs/architecture/README.md`**

У таблиці «Шари» рядок «Компоненти» доповнити: фасади БПО стоять на `BpoFacadeBase`
(контракт) → `AcquiringFacadeBase` (семантика еквайрингу) → ревізійний шар → конкретний
протокол. У каталозі підсистем рядок `04a` доповнити згадкою шарування.

- [ ] **Крок 5: Звірити документи з кодом**

```bash
grep -rn "AddinEcrBpoBase\|AddinEcrBpo3004\|AddinEcrBpo4000\|ПодключитьОборудование\|4007\|kInterfaceRevision" docs/
```
Допустимі збіги — **тільки** в `bpo-contract.md` §5 (розбіжність із документом ІТС) і §2
(таблиця-доказ із нулями збігів), де довгі імена згадуються як факт про ІТС. **Кожен інший
збіг виправ на місці** — зокрема в `docs/architecture/ecrprivatjson.md`, якщо він посилається
на `AddinEcrBpoBase` (тепер це `BpoFacadeBase` + `AcquiringFacadeBase` + `EcrPrivatJsonAcquiring`).

- [ ] **Крок 6: Фінальний гейт і коміт**

```powershell
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```

```bash
git add docs/
git commit -m "docs: шарування БПО-фасадів, LabelPrinter на контракті, єдина таксономія помилок"
```

---

## Після плану

Крок 7 порядку робіт спеки — **поза цим планом**, бо потребує іншої сесії й живого обладнання:

1. Зібрати свіжий ZIP: `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests`
   → `bin/Release/SimplyAddinConnectWin.zip`.
2. Передати компаньйону (`smp-simplyconnect-a0`) — він додає запис довідника драйверів із
   `ИдентификаторОбъекта = "LabelPrinter"` (макет спільний, `СМП_КомпонентаSimplyAddinConnect`,
   нового бандла не треба).
3. Живий прогін: форма налаштувань обладнання → «Тест устройства» → друк.

⚠️ Пам'ятай про кеш нативної компоненти 1С: версія входить в ім'я DLL усередині ZIP, тож
перевстановлення підхопить нову збірку; у `bin/Release` імена лишаються стабільними для тестів.
