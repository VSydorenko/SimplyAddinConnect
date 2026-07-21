# Дизайн: драйвер принтера етикеток (LabelPrinter) для SimplyAddinConnect

> Статус: **специфікація, ревізія 2** (після Codex-аудиту, знахідки верифіковано вручну).
> Дата: 2026-07-20.
> Основа: дослідження `docs/research/label-printers-hardware-integration.md` +
> контракт 1С БПО «Принтер этикеток» (розд. 3.7) і загальні вимоги до драйверів БПО
> (системна частина). Ядро/платформа/транспорт/шаблон ECR звірені по факту — **не переписуємо**.
>
> Зміни проти ревізії 1: **БПО-first** (єдиний фасад — контракт БПО, XML; кастомний JSON-фасад
> прибрано); виправлено **мапінг штрихкодів ZPL** (реальні команди + `UNSUPPORTED` для нативно
> невідтворних); `^GF` — повний формат + тайлінг; **растровий фолбек штрихкодів прибрано**
> (GDI+ не кодує символіки); DPI — **dots/mm 8/12/24**; GDI+ **обмежено клієнтським контекстом**
> (серверна служба не стосується локального обладнання); `^FD`-escaping; batch state machine;
> concurrency; уточнено семантику результату («accepted», не «printed»).

---

## 1. Мета й контекст

Спроектувати **драйвер обладнання «Принтер етикеток»** поверх наявного ядра SimplyAddinConnect,
щоб друкувати етикетки **високої якості** прямо з 1С, **без** платних per-printer драйверів
(Гексагон) і без важких інструментів (BarTender). Верстку етикетки користувач робить **у самій
1С**; компонента — **міст даних 1С → принтер**: приймає стандартний БПО-пакет (шаблон + дані у XML)
і перетворює його на потік команд рідною мовою принтера, доставляючи на пристрій в обхід GDI-друку.

Принтер етикеток — **інтерпретатор командної мови** (ZPL/EPL/TSPL). Функцію платного драйвера
повністю відтворює власна нативна компонента, що **генерує потік команд** і **доставляє байти**.

---

## 2. Прийняті рішення (узгоджено)

| # | Рішення | Значення |
|---|---|---|
| 1 | **Інтерфейс 1С** | **БПО-first:** єдиний фасад — стандартний контракт «Библиотеки подключаемого оборудования», тип `LabelPrinter`. Вхід/вихід — **XML** (UTF-8). Кастомного JSON-фасаду немає. |
| 2 | **Архітектура компоненти** | **Односкладова функціональна** БПО-компонента (`IntegrationComponent=false`). Сертифікаційний **багатокомпонентний** split — окремо, лише за потреби стампу «1С:Сумісно» (§11). |
| 3 | **Стратегія рендеру** | **Гібрид:** штрихкоди — **нативними ZPL-командами**; текст/картинки/рамки — **растеризація GDI+** у 1-bit → `^GF`. Нативно невідтворні символіки (Code16K та ін.) → `UNSUPPORTED_BARCODE`. |
| 4 | **Мова команд v1** | **ZPL** (спільний знаменник; емулюється TSC/Godex/клонами). TSPL/EPL — пізніше. |
| 5 | **Транспорт v1** | **spooler-RAW** (Winspool, за іменем Windows-черги — покриває **USB**/локальні) + **TCP:9100** (мережеві). |
| 6 | **DPI / одиниці** | Роздільність задається як **dots/mm** (8/12/24 = «203/300/600»), бо номінальні DPI неточні. Параметр підключення. |
| 7 | **Контекст виконання** | **Клієнтський** (тонкий/товстий/web-клієнт), інтерактивна сесія — там, де локальний принтер. GDI+ там підтримується. Серверна служба й мобільні клієнти — поза обсягом (Windows-only компонента). |
| 8 | **Синхронність** | Друк **синхронний** (`PrintLabels`→BOOL). `DeviceSession`/`JobEngine` у v1 не потрібні. |
| 9 | **Тестування** | Контур як в ECR: selftest + мережевий емулятор + `native_host`, що кличе **саме БПО-методи** з XML. |

**Ядро не змінюємо.** Reuse як є: `AddInNative` (`Ret`/`ParamSpec`/`REGISTER_COMPONENT`/`VH`),
`ResultEnvelope`, `ITransport` + `TransportTCP`, шаблон драйвера/фасаду ECR.

---

## 3. Обсяг

**У обсязі v1:**
- Односкладова БПО-компонента `LabelPrinter` (системні + функціональні методи, XML I/O, `GetLastError`).
- Генератор `ZPL` за гібридною стратегією (нативні штрихкоди + растр у `^GF`).
- Нормалізована модель `LabelModel` (типізоване дзеркало `LabelsTable`, з коректною value-семантикою).
- Транспорти: `TransportSpoolerRaw` (новий) + `TransportTCP` (наявний).
- Мульти-`DeviceID`, **явний state machine** пакетності `first/regular/last`, кеш `Formatting`.
- XML-парсер (`pugixml`, MIT) для `LabelsTable`/`ConnectionParameters`.
- Тест-контур (selftest + мережевий емулятор + `native_host` через БПО-методи) + `run_tests.ps1`.

**Поза обсягом v1:**
- **Сертифікаційний багатокомпонентний** драйвер (інтеграційний компонент + основна поставка + IPC) — §11.
- Генератори **TSPL/EPL**; статус-полінг (`<ESC>!?`) → `DeviceSession` (v2).
- **Сирий USB** (WinUSB) без Windows-черги; нативні символіки, яких немає в ZPL (Code16K тощо) — `UNSUPPORTED`.
- Не-Windows / мобільні клієнти / серверний рендер.

---

## 4. Архітектура: шари й файли

```
[1С БПО «Подключаемое оборудование»]
        │ ConnectEquipment / PrintLabels(LabelsTable XML) / … (BOOL + OUT + GetLastError)
        ▼
      AddinLabelPrinter (БПО-фасад)                    src/components/AddinLabelPrinter.{h,cpp}   [НОВЕ]
        │ XML → LabelModel (pugixml); ResultEnvelope → BOOL + lastError
        ▼
      LabelPrinterDriver (драйвер)                     src/drivers/label_printer/LabelPrinterDriver.{h,cpp}  [НОВЕ]
        │ map<DeviceID, DeviceContext{ITransport, кеш Formatting, batch-стан, mutex}>
        ├─ LabelZplGenerator (── "кодек")              src/drivers/label_printer/LabelZplGenerator.{h,cpp}   [НОВЕ]
        │    │ LabelModel → ZPL (нативні штрихкоди + вставки ^GF)
        │    └─ LabelRaster (растр-хелпер)             src/drivers/label_printer/LabelRaster.{h,cpp}         [НОВЕ]
        │         текст/картинка/рамка → 1-bit (GDI+) → ^GF (з тайлінгом)
        ▼
      ITransport                                        src/transport/Transport.h                 [НАЯВНЕ]
        ├─ TransportSpoolerRaw (Winspool за іменем черги) src/transport/Transport_SpoolerRaw.{h,cpp} [НОВЕ]
        └─ TransportTCP (:9100)                            src/transport/Transport_TCP.{h,cpp}        [НАЯВНЕ]
        ▼
      [принтер: приймає потік ZPL]
```

**Ізоляція (як у ECR):** драйвер оркеструє; ZPL-специфіка — у генераторі+растрі; транспорт
свопабельний за `ITransport`; `LabelModel` — стабільний внутрішній контракт, від якого фасад
незалежний. **`DeviceSession`/`wire_component` у v1 не задіяні** (друк — односпрямований).

---

## 5. Модель даних: `LabelModel`

Типізоване дзеркало `LabelsTable` (розд. 3.7) з **коректною value-семантикою** контракту (одиниці —
мм). Замість одного generic-поля — окремі типи полів:

```
struct FieldGeom { double left, top, width, height;  int orientation = 0; };   // мм, 0/90/180/270

struct TextField {
    std::string fieldName; FieldGeom geom;
    std::string fontName; int fontSize = 0; std::string fontStyle;   // Bold/Italic/Underline/StrikeOut
    std::string align = "Left", vAlign = "Top"; bool multiline = false;
    std::string border; int borderWidth = 1; std::string borderStyle = "Solid";
    bool isStatic = false;
    std::optional<std::string> defaultOrStaticValue;   // Formatting.Value: спільне (Static) або дефолт (non-static)
};
struct BarcodeField {
    std::string fieldName; FieldGeom geom;
    std::string type;                       // EAN13/Code128/QRCode/...
    bool printHRI = true; int hriFontSize = 0; bool checkSymbol = true;
    bool isStatic = false;
    std::optional<std::string> staticValueBase64;   // Formatting.ValueBase64 (лише static)
};
struct ImageField {
    std::string fieldName; FieldGeom geom; std::string border; int borderWidth = 1; std::string borderStyle = "Solid";
    bool isStatic = false;
    std::optional<std::string> staticValueBase64;   // Formatting.Value (Base64, лише static)
};
struct UserDataField { std::string fieldName; bool isStatic = false; std::optional<std::string> defaultOrStaticValue; };

struct LabelFormatting {
    double width = 0, height = 0;           // мм
    std::vector<TextField> texts; std::vector<BarcodeField> barcodes;
    std::vector<ImageField> images; std::vector<UserDataField> userData;
};
struct LabelRecord { std::string fieldName; std::optional<std::string> value; };  // optional: відсутній ≠ порожній
struct LabelInstance { int quantity = 1; std::vector<LabelRecord> records; };     // Image record.value — Base64
struct LabelBatch { std::optional<LabelFormatting> formatting; std::vector<LabelInstance> labels; };
```

**Value-семантика (звірено з 3.7):**
- `Text`/`UserData`: `Formatting.Value` — спільне значення при `Static=true`, **інакше — значення за
  замовчуванням**, якщо в `Record` для поля значення не задано (тому `Record.value` — `optional`:
  відсутній атрибут ≠ порожній рядок).
- `Barcode` (static): `ValueBase64` (Base64). `Barcode` (dynamic): `Record.value` — **простий текст**
  (не Base64; приклад контракту «4008110271538»).
- `Image` (static): `Value` Base64; (dynamic): `Record.value` Base64.

**`PackageStatus`** (`first/regular/last`) — **лише аргумент методу**, у моделі його немає (усунено
дублювання). `Formatting` присутнє тільки при `first`.

---

## 6. Генерація ZPL (гібрид)

`LabelZplGenerator::Build(formatting, instance, deviceProfile) → std::vector<uint8_t>` формує один
`^XA … ^XZ` на екземпляр (`^PQ<quantity>` для копій).

**Одиниці:** `dots = round(mm * dotsPerMm)`, де `dotsPerMm ∈ {8,12,24}` (не через номінальний DPI —
уникаємо похибки 203 vs 203.2). Кегль→пікселі растру тим самим коефіцієнтом: `px = round(pt/25.4*dotsPerMm*... )`
(точна формула — у плані; узгоджена з растром, §7).

### 6.1. Штрихкоди — нативні команди (`^FO x,y` + `^BY` + баркод-команда + `^FD data ^FS`)
Виправлений мапінг (звірено з довідником Zebra):

| Тип БПО | ZPL | Примітки |
|---|---|---|
| EAN13 | `^BE` | приймає **12 цифр**, 13-ту (контрольну) рахує принтер → **нормалізувати вхід** (контракт може дати 13) |
| EAN8 | `^B8` | 7 цифр + контрольна |
| EAN13Addon2 / Addon5 | **відкладено до v2** | у v1 → `UNSUPPORTED_BARCODE` («відкладено до v2»); реалізація (`^BE` + `^BS`) — у v2 |
| UPC/додатки (extensions) | **відкладено до v2** | у v1 → `UNSUPPORTED_BARCODE` («відкладено до v2»); реалізація (`^BS`, UPC/EAN Extensions) — у v2 |
| Code128 | `^BC` | режим/орієнтація `^BCo,h,f,g,e,m` (перший параметр — орієнтація `N/R/I/B`) |
| EAN128 (GS1-128) | `^BC` + **FNC1** | FNC1 у даних (`>8`) + коректне кодування AI (окремий GS1-препроцесор) |
| Code39 | `^B3` | |
| Code93 | `^BA` | |
| ITF14 | **`^B2`** | Interleaved 2of5 (НЕ `^BC`); 14 цифр + guard/bearer |
| QRCode | `^BQ` | |
| DataMatrix | `^BX` | |
| PDF417 | `^B7` | |
| GS1DataBarExpandedStacked | **`^BR`** з типом Expanded Stacked + ширина сегмента | голий `^BR` недостатній |
| **Code16K** | **немає нативної команди в ZPL** | → `UNSUPPORTED_BARCODE` (з описом у `GetLastError`) |

Правила: per-symbology нормалізація довжини/контрольної цифри; `^BY` — ціла ширина модуля 1–10 dots,
розрахунок із **перевіркою quiet-zone** і що код фізично влазить у прямокутник (інакше — відмова з
описом); `PrintHRI`/`hriFontSize` збільшують висоту; `checkSymbol` трактувати **per-symbology**
(у Code128 Mod103 завжди є; EAN — власні правила).

### 6.2. Текст / Image / Border — растр (§7) одним `^GF` на етикетку
Збираємо в **1-bit бітмап на всю етикетку** (ділянки штрихкодів лишаються порожніми) і вставляємо
`^GF`. Формат повний: `^GFA,b,c,d,<hex>` — `b` = байтів передати, `c` = повний розмір, `d` =
`bytesPerRow = ceil(widthDots/8)`. **Тайлінг:** якщо етикетка перевищує ліміти поля `^GF`/памʼяті
(великі розміри при 24 dots/mm), бітмап ріжеться на кілька `^GF` із власними `^FO` (правила
розбиття/padding/порядку бітів — у плані; додати golden-тести розміру).

### 6.3. `^FD`-escaping (безпека/коректність)
Оскільки **текст растеризуємо**, `^FD` вживається **лише для даних штрихкодів** — тож ризик кирилиці
в `^FD` здебільшого знято. Але дані штрихкоду можуть містити `^`, `~`, `>` (спецсемантика Code128/GS1)
або control-байти → **централізований field-encoder через `^FH`/hex-escaping**; заборона сирих
control-prefix байтів; окремий GS1-парсер для FNC1. Валідація даних за дозволеним алфавітом символіки.

### 6.4. `InitializePrinter`
Окремий `^XA … ^XZ`: темність (`^MD`/`~SD`, діапазон per-model), швидкість (`^PR`), розмір/довжина
(`^PW`/`^LL`), режим (`^MM`), home (`^LH`). Значення — з `ConnectionParameters`. **Геометрію
(`^PW/^LL`) генеруємо разом із кожним форматом** (де `Formatting.Width/Height` — авторитет для формату),
щоб уникнути конфлікту «device-профіль vs формат пакета».

---

## 7. Растеризація: `LabelRaster` (клієнтський контекст)

- Рендер через **GDI+** в offscreen-бітмап у нативній роздільності (`dotsPerMm`). **Виконується в
  клієнтській інтерактивній сесії** (локальне обладнання — §2.7), де GDI+ підтримується; серверна
  служба до нас не стосується.
- **Порядок:** рендер grayscale з керованим `TextRenderingHint` → **порогування/дизеринг у 1-bit**
  (не «сірий» шум термоголовці). Фіксуємо: pixel-format, поріг, дизеринг для картинок, `UnitPixel`
  для шрифтів (щоб уникнути подвійного масштабування), правила округлення.
- **Шрифти:** політика fallback — якщо `FontName` не встановлений, GDI+ робить substitution;
  документуємо, що WYSIWYG — «best-effort», а гарантія — лише для встановлених шрифтів. Вхід тексту —
  UTF-16 з `VH` (`AddInNative.cpp:749`). Golden-тести — на **bundled test-font** (детермінізм).
- **Картинки:** декод Base64 → перелік підтримуваних форматів (PNG/BMP/JPEG), правила
  scale/crop/rotate, збереження aspect ratio, ліміти розміру, коректне життя stream для `Image::FromStream`.
- **Життєвий цикл GDI+:** **process-wide ref-counted** `GdiplusStartup/Shutdown` (НЕ з `DllMain`), усі
  GDI+-обʼєкти знищуються до `GdiplusShutdown`; конкурентні рендери серіалізуються (§9).

---

## 8. Транспорти

**`TransportSpoolerRaw : ITransport` [НОВЕ]** — Winspool, RAW за **іменем Windows-черги**.
Семантика методів `ITransport` (звірено з `Transport.h`): `Open()` відкриває `OpenPrinter`-хендл;
`Send()` виконує повну послідовність job (`StartDocPrinter{pDatatype="RAW", pOutputFile=NULL}` →
`StartPagePrinter` → `WritePrinter` → `EndPagePrinter` → `EndDocPrinter`) і повертає к-сть записаних
байтів; `Close()` закриває хендл; `IsOpen()` — стан хендла; колбеки `SetData/Error/ConnectionState`
для write-only — **no-op** (документовано). Обовʼязково: перевірка `pcWritten == requested`
(all-or-error), збереження `GetLastError` **по кожному кроку**, гарантований teardown залежно від
досягнутого стану, ліміт розміру job. Блокувальність (`WritePrinter`/`StartDoc`) — задокументувати
(реального timeout Winspool API не має). Тест-шов `SetSendFunctionForTest` (перехоплення запису).
Лінк `winspool.lib`.

**`TransportTCP` [НАЯВНЕ]** — сокет на `ip:9100`, без обгортки.

**Параметри підключення** приходять як БПО `ConnectionParameters` (XML, §10): `TransportKind`
(`spooler`/`tcp`), `PrinterName` (spooler) або `Host`/`Port` (tcp), `DotsPerMm`, `Darkness`, `Speed`,
`LabelWidthMm`/`LabelHeightMm`, home. Невідомі параметри **ігноруються** (вимога БПО).

**Деплой-нота:** RAW обходить GDI-рендер вендорського драйвера; найнадійніший універсальний варіант —
черга на **Generic / Text Only** (задокументуємо в `docs/integration-1c/`).

---

## 9. Драйвер `LabelPrinterDriver`

- **Мульти-пристрій:** `map<DeviceID, DeviceContext>`; `DeviceContext` = `unique_ptr<ITransport>` +
  кеш `LabelFormatting` + **batch-стан** + параметри + **per-device mutex**. Реєстр — під власним
  mutex; **під час GDI+/TCP/Winspool глобального локу не тримати** (лише per-device), пакетну
  послідовність одного пристрою серіалізувати.
- **State machine пакетності (per-DeviceID):** явні правила — `first` (зберегти `Formatting`, скинути
  попередній стан), `regular`/`last` без активного `first` → `Fail`, `last` → друк + **очистити кеш**;
  duplicate `first` → рестарт серії; новий `first` після незавершеної серії → рестарт; однопакетний
  сценарій (`first`+`last` водночас можлива семантика); помилка після часткового друку → повернути
  скільки прийнято + індекс збою; очищення кешу при `Disconnect`.
- **`InitializePrinter(deviceId)`** — ініт-`^XA…^XZ` (§6.4).
- **Результат:** `MapResult → ResultEnvelope`. **Семантика чесна:** успіх = *прийнято* (TCP: байти в
  сокет; spooler: job у черзі) — **не фізичний друк**. Payload: `acceptedInstances`, `acceptedCopies`
  (сума `Quantity`), `failedIndex?`, `transportJobId?`. Термін «printed» не вживаємо (потребує
  статус-полінгу, v2). Коди: `NOT_CONNECTED`/`BAD_INPUT`/`TRANSPORT_ERROR`/`UNSUPPORTED_BARCODE`/
  `RENDER_ERROR`/`EXCEPTION`.
- **Синхронно**, виняток-безпечно (try/catch → `Fail`).
- **Трасування** (`SetTrace`): **opt-in**, з **redaction/cap** (ZPL містить ціни/PII/GS1; `^GF` —
  логувати hash/розмір, не весь блоб).

---

## 10. БПО-фасад `AddinLabelPrinter` (компонента 1С, тип `LabelPrinter`)

`class AddinLabelPrinter : public AddInNative`, `REGISTER_COMPONENT`, `RegisterMethods`; тримає
`LabelPrinterDriver driver_` + `lastErrorCode/lastErrorDescription`. Вхід/вихід — **XML** (парсинг
`pugixml`). Кожен метод: `ResultEnvelope` драйвера → `BOOL` + при помилці зберегти
код+опис для `GetLastError`.

**Системні методи** (звірено із загальними вимогами):

| EN / RU | Параметри | Повертає |
|---|---|---|
| `GetInterfaceRevision` / `ПолучитьРевизиюИнтерфейса` | — | **LONG** (версія вимог) |
| `GetDescription` / `ПолучитьОписание` | `DriverDescription`[OUT XML] | BOOL |
| `GetLastError` / `ПолучитьОшибку` | `ErrorDescription`[OUT] | **LONG** (код) |
| `EquipmentParameters` / `ПараметрыОборудования` | `EquipmentType`[IN], `TableParameters`[OUT XML] | BOOL |
| `ConnectEquipment` / `ПодключитьОборудование` | `DeviceID`[OUT], `EquipmentType`[IN], `ConnectionParameters`[IN XML] | BOOL |
| `DisconnectEquipment` / `ОтключитьОборудование` | `DeviceID`[IN] | BOOL |
| `EquipmentTest` / `ТестированиеОборудования` | `EquipmentType`[IN], `ConnectionParameters`[IN], `Description`[OUT], `DemoModeIsActivated`[OUT] | BOOL |
| `SetApplicationInformation` / `УстановитьИнформациюПриложения` | `ApplicationSettings`[IN XML] | BOOL |
| (за потреби) `GetAdditionalActions`/`DoAdditionalAction`, локалізація | … | BOOL |

**Функціональні методи** (розд. 3.7):

| EN / RU | Параметри | Повертає |
|---|---|---|
| `InitializePrinter` / `ИнициализацияПринтера` | `DeviceID`[IN] | BOOL |
| `PrintLabels` / `ПечатьЭтикеток` | `DeviceID`[IN], `LabelsTable`[IN XML], `PackageStatus`[IN] | BOOL |

`GetDescription` віддає `DriverDescription` (`EquipmentType="LabelPrinter"`, `IntegrationComponent=false`,
версії, `LogIsEnabled`/`LogPath`); `EquipmentParameters` — `TableParameters` (форма налаштувань:
transport/порт/DPI/темність/швидкість/розмір). Локалізацію початково оголошуємо мінімальною
(`LocalizationSupported` за станом; RU/EN). `Version` — **property** (успадкована з `AddInNative`,
`AddProperty`), не метод.

---

## 11. Сертифікація «1С:Сумісно» (майбутнє, опційно)

Для офіційного стампа тип `LabelPrinter` вимагає **багатокомпонентної** архітектури: окрема
**інтеграційна DLL з одним обʼєктом** (лише знаходить/ініціалізує/делегує) + **основна поставка**
драйвера з визначеним IPC/ABI. Це важча схема, потрібна **тільки для сертифікації** — функціонально
односкладова компонента (§2.2) працює в конфігураціях без неї. Рішення про сертифікацію й розбивку —
окремий етап, за бізнес-потреби.

---

## 12. Тест-план

Контур як в ECR; збірка завжди при `-WithTests`, без UAPKI. ZPL однаковий незалежно від транспорту.

**L-p1 — `label_printer_selftest.exe` (без 1С, без заліза) — головний гейт:**
- `LabelModel → ZPL`: **структура команд** проти еталону (позиції `^FO` у dots, коректні
  баркод-команди з даними, `^PQ`, ініт); **`^GF`-блоб не звіряємо байт-в-байт** (GDI недетермінований);
- `^GF`-енкодер — окремо, синтетичним 1-bit бітмапом (детерміновано), включно з **тайлінгом**;
- XML→`LabelModel` (усі поля/типи/value-семантика); **batch state machine** (усі переходи);
  мульти-`DeviceID`; `MapResult`; `UNSUPPORTED_BARCODE`; спец-символи в даних штрихкоду (`^FH`);
  8/12/24 dots/mm.

**L-p2 — transport-e2e через `TransportTCP` → `label_printer_emulator`:** структура ZPL, отримана мережевим емулятором.

**L-p3 — `label_native_host.exe` (через головну DLL):** `LoadLibraryW` → компонента `LabelPrinter` →
кличе **БПО-методи** (`ПодключитьОборудование` + `ПечатьЭтикеток` з `LabelsTable` XML) проти
in-process емулятора → перевірка BOOL/`GetLastError` + отриманого ZPL. Тестує реальний XML-контракт.

**L-p4 — standalone `label_printer_emulator.exe`:** ручний тест із РЕАЛЬНОЇ 1С (реєстрація як
«Подключаемое оборудование», друк) → емулятор зберігає ZPL; візуалка — **Labelary**.

**spooler-RAW:** ін'єкція повної таблиці Winspool-функцій (не лише запис) → тести кожної точки відмови
(short write, помилки StartPage/EndDoc, teardown); + ручний смоук на реальному USB-принтері.

---

## 13. Збірка (CMake)

- **`transport_component`**: `Transport_SpoolerRaw.{h,cpp}`.
- **`driver_label_printer_component` [НОВЕ]**: `LabelModel.h`, `LabelZplGenerator.{h,cpp}`,
  `LabelRaster.{h,cpp}`, `LabelPrinterDriver.{h,cpp}`; deps: `base_component spdlog nlohmann_json
  transport_component platform_component pugixml`.
- **`label_facade_component` [НОВЕ]**: `AddinLabelPrinter.{h,cpp}`; deps: `+helpers_component
  driver_label_printer_component`.
- **`pugixml`** — додати як залежність (submodule/vendored, `extern/`).
- **Лінк системних бібліотек — на ФІНАЛЬНИЙ таргет і тест-цілі** (бо DLL збирається з
  `$<TARGET_OBJECTS>`, лінк на object-бібліотеці не пропагується — звірено `components.cmake:315`):
  `target_link_libraries(${TARGET} PRIVATE winspool gdiplus)` + ті самі для `label_printer_selftest`/
  `label_native_host`.
- Обидві нові OBJECT-цілі → `$<TARGET_OBJECTS:...>` фінальної DLL; фасадні файли — у
  `HEADER_FILES`/`SOURCE_FILES`.
- Тести: `label_printer_selftest`, `label_native_host`, `label_printer_emulator`.

---

## 14. Межі модулів

| Модуль | Що робить | Вхід → вихід | Залежить від |
|---|---|---|---|
| `LabelModel` | типізована структура етикетки | — (POD) | — |
| `LabelRaster` | текст/картинка/рамка → 1-bit → `^GF`(+тайли) | поля+профіль → байти | GDI+ |
| `LabelZplGenerator` | розкладка → ZPL (нативні штрихкоди + `^GF`, escaping) | `LabelModel`+профіль → ZPL | `LabelRaster` |
| `TransportSpoolerRaw` | RAW у Windows-чергу | байти → пристрій | Winspool, `ITransport` |
| `LabelPrinterDriver` | оркестрація, мульти-DeviceID, batch SM, concurrency | `LabelModel` → `ResultEnvelope` | генератор + транспорт + platform |
| `AddinLabelPrinter` | БПО-фасад | XML ↔ BOOL/OUT/GetLastError | драйвер + ядро + pugixml |

---

## 15. Обробка помилок

- Бізнес-помилки — `ResultEnvelope::Fail(code, description)`, ніколи `throw` за межу 1С; у фасаді →
  `BOOL false` + збережений `lastError` для `GetLastError` (LONG код + STRING опис).
- Кожен публічний метод — try/catch → `Fail("EXCEPTION", …)`.
- Логування — макроси `ServiceTools` (`REPORT_*`/`NEUTRAL_REPORT_*`), конкатенація, з великої, без
  крапки. Лог за замовчуванням увімкнено (БПО рекомендує; рівень — у параметрах).
- Непідтримувана символіка → `UNSUPPORTED_BARCODE` з описом (растрового фолбеку немає — GDI+ не кодує).

---

## 16. Ризики й відкриті питання

- **`^GF` при 24 dots/mm** — великі бітмапи; підтвердити ліміти поля `^GF` і правила тайлінгу на
  реальному пристрої; при 8/12 dots/mm — прийнятно.
- **Якість нативних штрихкодів vs quiet-zone/модуль** — зняти відбитки (перевірити сканером grade).
- **Рідкісні символіки** (`GS1DataBarExpandedStacked`, `EAN128`) — уточнити точні параметри `^BR`/FNC1.
- **Шрифти/кирилиця** — GDI+ substitution при відсутньому шрифті; політика fallback + bundled test-font.
- **GDI+** — підтверджено клієнтський контекст (§2.7, §7); серверний рендер поза обсягом.
- **Сертифікація** — багатокомпонентність (§11), лише за потреби.

---

## 17. Джерела

- Дослідження: `docs/research/label-printers-hardware-integration.md`.
- Контракт БПО «Принтер этикеток» (3.7) і загальні вимоги — ITS `metod8dev` (робочі копії — `tmp/`).
- Zebra ZPL — довідник команд (`^BE`/`^B2`/`^BF`/`^BR`/`^BS`/`^GF`/`^BC`), docs.zebra.com.
- Шаблон/фундамент: `src/drivers/ecr_privatjson/`, `src/components/AddinECRPrivatJSON.*`,
  `src/platform/{ResultEnvelope,JobEngine}.h`, `src/transport/{Transport,Transport_TCP}.*`,
  `src/core/AddInNative.*`, `CMake/components.cmake`.
