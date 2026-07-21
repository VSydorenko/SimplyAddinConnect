# Дизайн: драйвер принтера етикеток (LabelPrinter) для SimplyAddinConnect

> Статус: **специфікація** (вихід брейнштормінгу; до реалізації).
> Дата: 2026-07-20.
> Основа: дослідження `docs/research/label-printers-hardware-integration.md` +
> контракт 1С БПО «Принтер этикеток» (розд. 3.7) і загальні вимоги до драйверів БПО
> (системна частина), звірені з учасником; поточний код ядра/платформи/транспорту/шаблону
> ECR звірено по факту (не переписуємо ядро).

---

## 1. Мета й контекст

Спроектувати **новий драйвер обладнання — принтер етикеток** поверх наявного ядра
SimplyAddinConnect, щоб друкувати етикетки **високої якості** прямо з 1С, **без** платних
per-printer драйверів (Гексагон) і без важких інструментів (BarTender). Верстку етикетки
користувач робить **у самій 1С**; наша компонента — **міст даних 1С → принтер**: приймає
структурований опис (шаблон + дані) і перетворює його на потік команд рідною мовою принтера,
доставляючи його на пристрій в обхід GDI-друку.

Головний факт із дослідження: принтер етикеток — **інтерпретатор командної мови** (ZPL/EPL/
TSPL). Тож функцію платного драйвера повністю відтворює власна нативна компонента, яка
**генерує потік команд** і **доставляє байти**.

---

## 2. Прийняті рішення (узгоджено)

| # | Рішення | Значення |
|---|---|---|
| 1 | **Модель постачання** | Спільний **facade-агностичний драйвер** + **2 фасади**: кастомний (зараз) і БПО-сумісний (пізніше). Модулі БПО зараз не потрібні. |
| 2 | **Стратегія рендеру** | **Гібрид (C):** штрихкоди — **нативними командами принтера** (найкраще сканування + не пишемо кодери символік); текст/картинки/рамки — **растеризація на хості** (GDI+) у 1-bit і надсилання як `^GF` (точна кирилиця + WYSIWYG). |
| 3 | **Мова команд v1** | **ZPL** як спільний знаменник (емулюється TSC/Godex/дешевими клонами). TSPL/EPL — окремими генераторами пізніше. |
| 4 | **Транспорт v1** | **spooler-RAW** (Winspool, за іменем Windows-черги — покриває **USB** і локальні) + **TCP:9100** (мережеві). «USB» = spooler-RAW за іменем, окремого USB-коду немає. |
| 5 | **DPI** | Параметр підключення пристрою; **дефолт 203**; підтримати 203/300/600 (мм→доти за DPI). |
| 6 | **Синхронність** | Друк — **синхронний** (як у контракті БПО `PrintLabels`→BOOL). `DeviceSession`/`JobEngine` у v1 **не потрібні** (вони для запит/відповідь і довгих інтерактивних операцій). |
| 7 | **Тестування** | 4 рівні (див. §10), **мережевий емулятор-EXE** (тупий захоплювач ZPL) + `native_host` через DLL; візуальна перевірка — Labelary. |

**Ядро не змінюємо.** Використовуємо як є: `AddInNative` (Ret/ParamSpec/REGISTER_COMPONENT/VH),
`ResultEnvelope`, `JobEngine` (за потреби в майбутньому), `ITransport` + `TransportTCP`, шаблон
драйвера/фасаду ECR.

---

## 3. Обсяг

**У обсязі v1:**
- Генератор `ZPL` за гібридною стратегією (нативні штрихкоди + растр тексту/картинок/рамок).
- Нормалізована модель `LabelModel` (дзеркало `Formatting`/`Labels` з контракту 3.7).
- Вхід у драйвер — через **JSON** (кастомний фасад; nlohmann уже в проєкті).
- Транспорти: `TransportSpoolerRaw` (новий) + `TransportTCP` (наявний).
- Мульти-`DeviceID`, пакетність `first/regular/last`, кеш `Formatting`.
- Метод `InitializePrinter` (темність/швидкість/розмір етикетки) + `PrintLabels`.
- Кастомна компонента 1С `AddinLabelPrint` (двомовні методи EN/RU).
- Тест-контур (selftest + мережевий емулятор + native_host) + інтеграція в `run_tests.ps1`.

**Поза обсягом v1 (майбутнє, з окремим обґрунтуванням):**
- **БПО-сумісний фасад** (системні методи, XML `LabelsTable`→`LabelModel`, `BOOL`+`GetLastError`,
  `DriverDescription`/`EquipmentParameters`/…, **багатокомпонентна** архітектура — див. §9).
- Генератори **TSPL/EPL**.
- **Статус-полінг** (`<ESC>!?`) по COM/USB — тоді знадобиться `DeviceSession`.
- **Сирий USB** (WinUSB/libusb) без Windows-черги.
- Оптимізація **нативними шрифтами** принтера (для моделей із гарантованою кирилицею).
- Локалізація (`SetLocale`/`SetLocalization`), не-Windows платформи.

---

## 4. Архітектура: шари й файли

```
[1С] → AddinLabelPrint (кастомний фасад)                 src/components/AddinLabelPrint.{h,cpp}   [НОВЕ]
        │ JSON → LabelModel; ResultEnvelope → this->result
        ▼
      LabelPrinterDriver (драйвер)                       src/drivers/label_printer/LabelPrinterDriver.{h,cpp}  [НОВЕ]
        │ map<DeviceID, DeviceContext{ITransport, кеш Formatting}>
        │ пакетність first/regular/last; MapResult → ResultEnvelope
        ├─ LabelZplGenerator (── "кодек")                src/drivers/label_printer/LabelZplGenerator.{h,cpp}   [НОВЕ]
        │    │ LabelModel → ZPL-байти (нативні штрихкоди + вставки ^GF)
        │    └─ LabelRaster (растр-хелпер)               src/drivers/label_printer/LabelRaster.{h,cpp}         [НОВЕ]
        │         текст/картинка/рамка → 1-bit (GDI+) → ^GFA
        ▼
      ITransport                                          src/transport/Transport.h                 [НАЯВНЕ]
        ├─ TransportSpoolerRaw (Winspool, за іменем черги) src/transport/Transport_SpoolerRaw.{h,cpp} [НОВЕ]
        └─ TransportTCP (:9100)                            src/transport/Transport_TCP.{h,cpp}        [НАЯВНЕ]
        ▼
      [принтер: приймає потік ZPL]
```

**Ключова інверсія (як у ECR):** драйвер оркеструє загальну схему; уся ZPL-специфіка — у
`LabelZplGenerator`+`LabelRaster`; транспорт — свопабельний за `ITransport`; `LabelModel` —
стабільний внутрішній контракт, від якого фасади незалежні.

**`DeviceSession` у v1 не використовується** — друк «вистрелив і забув», відповіді немає.
Драйвер шле згенеровані байти прямо через `ITransport` (`Open`→`Send`→за потреби `Close`).
`DeviceSession`/`wire_component` знадобляться лише для статус-полінгу (v2).

---

## 5. Модель даних: `LabelModel`

C++-дзеркало контракту `LabelsTable` (розд. 3.7). Одиниці — **міліметри**; координати/розміри
конвертуються в доти за DPI пристрою на етапі генерації.

```
struct LabelField {
    enum class Kind { Text, Barcode, Image, UserData };
    Kind kind;
    std::string fieldName;                 // унікальне ім'я поля
    double left = 0, top = 0;              // мм
    double width = 0, height = 0;          // мм
    int orientation = 0;                    // 0/90/180/270
    // Text:
    std::string fontName; int fontSize = 0; std::string fontStyle;   // Bold/Italic/Underline/StrikeOut
    std::string align, vAlign; bool multiline = false;
    std::string border; int borderWidth = 1; std::string borderStyle;
    // Barcode:
    std::string barcodeType;               // EAN13/Code128/QRCode/DataMatrix/...
    bool printHRI = true; bool checkSymbol = true;
    // спільне:
    bool isStatic = false;                 // однакове значення для всіх етикеток
    std::string staticValue;               // при isStatic (для Image/Barcode — Base64)
};

struct LabelFormatting {
    double width = 0, height = 0;          // мм (геометрія етикетки)
    std::vector<LabelField> fields;
};

struct LabelRecord { std::string fieldName, value; };   // value: для Image — Base64
struct LabelInstance { int quantity = 1; std::vector<LabelRecord> records; };

struct LabelBatch {
    std::optional<LabelFormatting> formatting;   // присутнє лише в пакеті "first"
    std::vector<LabelInstance> labels;
    std::string packageStatus;                   // "first" | "regular" | "last"
};
```

**JSON-вхід кастомного фасаду** — 1:1 до цих структур (ті самі імена полів, що в 3.7, лише JSON
замість XML). Майбутній БПО-фасад додасть адаптер **XML `LabelsTable` → `LabelModel`** (тоді ж
підключимо легкий XML-парсер — pugixml/tinyxml2), не змінюючи драйвер.

---

## 6. Генерація ZPL (гібрид)

`LabelZplGenerator::Build(formatting, instance, dpi) → std::vector<uint8_t>` формує один
`^XA … ^XZ` на екземпляр етикетки (з `^PQ<quantity>` для копій).

**Конвертація одиниць:** `dots = round(mm * dpi / 25.4)` (203dpi: 1мм ≈ 8 доти). Розмір шрифту
в кеглях → пікселі растру: `px = round(pt * dpi / 72)`.

**Розкладка поля за типом:**
- **Barcode → нативна команда принтера** (`^FO x,y` + `^BY` + баркод-команда + `^FD data ^FS`):

  | Тип (3.7) | ZPL |
  |---|---|
  | EAN13 / EAN8 | `^BE` / `^B8` |
  | EAN13Addon2/Addon5 | `^BE` з add-on |
  | Code128 / EAN128(GS1-128) | `^BC` (GS1 через `^FD>;`/`^BC…,,,,,A`) |
  | Code39 / Code93 | `^B3` / `^BA` |
  | ITF14 | `^BC`/`^B2` (Interleaved 2of5, з guard) |
  | QRCode | `^BQ` |
  | DataMatrix | `^BX` |
  | PDF417 | `^B7` |
  | Code16k / GS1DataBarExpandedStacked | `^BF` / `^BR` (перевірити підтримку; інакше **фолбек у растр**) |

  `PrintHRI` → прапорець підпису; `orientation` → параметр напряму (`N/R/I/B`);
  `checkSymbol` → контрольний символ, де застосовно. **Рідкісні/непідтримувані типи
  падають у растровий фолбек** (рендеримо штрихкод як зображення), щоб не втратити етикетку —
  з `WARN` у лог.
- **Text / Image / Border → растр**: збираємо в **один 1-bit бітмап на всю етикетку** (текст,
  картинки, рамки; ділянки штрихкодів лишаються порожніми) і вставляємо одним
  `^FO0,0^GFA,…` (див. §7). Так текст/рамки виходять точно як у редакторі 1С, з гарантованою
  кирилицею, а штрихкоди — нативні поверх.

**Чому один бітмап на етикетку, а не по полю:** простіше вирівнювання (усе в координатах
етикетки), точний WYSIWYG рамок/переносів; ціна — трохи більший payload (прийнятно).
Оптимізацію «per-field ^GF» лишаємо на потім за потреби.

**`InitializePrinter`** формує окремий `^XA … ^XZ` з налаштуваннями пристрою: темність
(`~SD`/`^MD`), швидкість (`^PR`), розмір/довжина етикетки (`^PW`/`^LL`), режим (`^MM`), home
(`^LH`). Значення — з параметрів підключення (§8). Оскільки текст растеризуємо, кодова
сторінка (`^CI`) для тексту не потрібна — ще й менше проблем із кирилицею.

---

## 7. Растеризація: `LabelRaster`

- Рендер через **GDI+** (системна `gdiplus.dll`, без зовнішніх залежностей) в offscreen-бітмап
  **у нативному DPI принтера**. Текст (`Graphics::DrawString`, будь-який TTF, кирилиця),
  картинки (декод Base64 → `Image`), рамки (`DrawRectangle`), орієнтація/вирівнювання/перенос —
  усе засобами GDI+.
- **Порогування в 1-bit** (чорно-біле, без згладжування) — щоб термоголовка не отримувала
  «сірий» шум; за потреби — дизеринг для напівтонів картинок.
- Кодування в **`^GFA`** (ASCII-hex, Format A — для сумісності/діагностики) з опційним
  **ACS/RLE-стисненням** для зменшення трафіку. `bytesPerRow = ceil(widthDots/8)`.
- **Ініціалізація GDI+** — один `GdiplusStartup`/`GdiplusShutdown` на час життя драйвера
  (RAII), не на кожен виклик.
- **Ризик (перевірити):** рендер offscreen-бітмапа GDI+ має працювати і на 1С-сервері без
  десктопа (для `PrintLabels` це зазвичай клієнт/РМК, але зафіксуємо перевірку).

---

## 8. Транспорти

**`TransportSpoolerRaw : ITransport` [НОВЕ]** — Winspool, друк RAW за **іменем Windows-черги**:
`OpenPrinter(name)` → `StartDocPrinter(DOC_INFO_1{pDatatype="RAW", pOutputFile=NULL})` →
`StartPagePrinter` → `WritePrinter(bytes)` → `EndPagePrinter` → `EndDocPrinter` → `ClosePrinter`,
з teardown уже відкритих ресурсів при помилці. `Send` синхронний; reader-потоку немає
(друк — односпрямований). Має тест-шов `SetSendFunctionForTest` (як інші транспорти) для
перехоплення байтів у тестах. Лінкує `winspool.lib`.

**`TransportTCP` [НАЯВНЕ]** — сокет на `ip:9100`, без обгортки протоколу.

**Рядок/параметри підключення (JSON, кастомний фасад):**
```
{ "transport": "spooler" | "tcp",
  "printerName": "<Windows queue>",        // для spooler
  "host": "192.168.0.50", "port": 9100,    // для tcp
  "dpi": 203, "darkness": 10, "speed": 4,
  "labelWidthMm": 60, "labelHeightMm": 40, "homeXDots": 0, "homeYDots": 0 }
```
Невідомі параметри **ігноруються** (вимога БПО п.1.5 — сумісність зі старими налаштуваннями).

**Деплой-нота:** RAW обходить GDI-рендер вендорського драйвера; працює й з ним, але
найнадійніший універсальний варіант — поставити чергу на **Generic / Text Only**. Задокументуємо
в `docs/integration-1c/`.

---

## 9. Драйвер `LabelPrinterDriver`

- **Мульти-пристрій:** `std::map<std::string /*DeviceID*/, DeviceContext>`; `DeviceContext`
  тримає `unique_ptr<ITransport>` + **кеш `LabelFormatting`** (з пакета "first") + параметри
  (DPI тощо). `Connect(paramsJson)` створює транспорт, генерує/повертає `DeviceID`.
- **Пакетність:** `PrintLabels(deviceId, batch)` — якщо `packageStatus=="first"`, зберігає
  `formatting` у контекст; для кожної `LabelInstance` бере кешований `formatting`, генерує ZPL,
  шле через транспорт; на "last" — фіналізація за потреби. Порожній `formatting` без попереднього
  "first" → `Fail("BAD_INPUT", …)`.
- **`InitializePrinter(deviceId)`** — шле ініт-`^XA…^XZ` (§6).
- **Результат:** `MapResult` → `ResultEnvelope`: `Ok({printed: N})` або `Fail(code, desc)`
  з кодами `NOT_CONNECTED` / `BAD_INPUT` / `TRANSPORT_ERROR` / `UNSUPPORTED_BARCODE` (лише якщо
  й растровий фолбек неможливий) / `RENDER_ERROR` / `EXCEPTION`.
- **Синхронно** (без `JobEngine`). Виняток-безпека: кожен публічний метод у try/catch →
  `Fail("EXCEPTION", …)` (винятки не перетинають межу 1С).
- **Трасування:** `SetTrace(on)` — лог згенерованого ZPL (як wire-trace ECR).
- **Pass-through (escape hatch):** `PrintRaw(deviceId, zplBytes)` — надіслати готовий потік
  напряму (для експертних кейсів і діагностики).

---

## 10. Кастомний фасад `AddinLabelPrint` (компонента 1С `LabelPrint`)

`class AddinLabelPrint : public AddInNative` (як `AddinECRPrivatJSON`): `REGISTER_COMPONENT`,
`RegisterMethods`, тримає `LabelPrinterDriver driver_`, JSON-вхід, результат — JSON
`ResultEnvelope` у `this->result` (хелпер `runSync`, як у ECR).

| EN / RU | Тип | Вхід | Результат |
|---|---|---|---|
| `Connect` / `Подключить` | Function | `paramsJson` | JSON (містить `deviceId`) |
| `Disconnect` / `Отключить` | Procedure | `deviceId?` | — |
| `IsConnected` / `Подключен` | Function | `deviceId?` | bool |
| `InitializePrinter` / `ИнициализироватьПринтер` | Function | `deviceId?` | JSON |
| `PrintLabels` / `ПечататьЭтикетки` | Function | `labelsJson`, `packageStatus` | JSON |
| `PrintRaw` / `ПечататьСырой` | Function | `zpl`, `deviceId?` | JSON |
| `LastError` / `ПоследняяОшибка` | Function | — | string |
| `EnableTrace` / `ВключитьТрассировку` | Function | `enable=true` | bool |
| `Version` / `Версия` | (успадковано) | — | string |

`deviceId` необов'язковий: для простого сценарію фасад тримає **один активний пристрій**;
драйвер усередині все одно мульти-`DeviceID` (щоб БПО-фасад ліг без переробки).

---

## 11. Майбутній БПО-фасад (не v1 — фіксуємо контракт)

Друга компонента на **той самий драйвер**, коли знадобиться drop-in заміна Гексагона в типових
конфігураціях. Що доведеться додати (і чому це НЕ чіпає драйвер):
- **Системні методи БПО:** `GetInterfaceRevision`(→`4007`), `GetDescription`(`DriverDescription`
  XML, `EquipmentType="LabelPrinter"`), `EquipmentParameters`(`TableParameters` XML — форма
  налаштувань), `ConnectEquipment`(→`DeviceID`)/`DisconnectEquipment`, `EquipmentTest`,
  `GetLastError`, (опц.) `EquipmentAutoSetup`, `SetApplicationInformation`, локалізація.
- **Функціональні:** `InitializePrinter`, `PrintLabels(DeviceID, LabelsTable XML, PackageStatus)`.
- **Адаптація результатів:** БПО-методи повертають `BOOL` + OUT-параметр + `GetLastError`;
  фасад транслює `ResultEnvelope → (BOOL, OUT, код/опис)`.
- **XML-адаптер:** `LabelsTable`/`ConnectionParameters` (XML) → `LabelModel`/params.
- **Обмеження сертифікації:** для типу `LabelPrinter` 1С вимагає **БАГАТОкомпонентну**
  архітектуру (інтеграційний компонент + основна поставка). Це рішення й розбивку опрацюємо
  окремо перед сертифікацією; на функціональність кастомного фасаду не впливає.

---

## 12. Тест-план

Дзеркалимо контур ECR. Збірка — завжди при `-WithTests`, **без UAPKI**. Мова команд однакова
незалежно від транспорту, тож **мережевого емулятора достатньо** для наскрізного покриття.

**L-p1 — `label_printer_selftest.exe` (без 1С, без заліза) — головний гейт.**
- `LabelModel → ZPL`: перевірка **структури команд** (позиції `^FO` у дотах, `^BY`/`^BE`/`^BC`/
  `^BQ`/`^BX` з даними, `^PQ`, `^XA/^XZ`, ініт-команди) проти еталону. **`^GF`-байти НЕ звіряємо
  на повний збіг** (GDI-рендер тексту недетермінований між ОС/версіями шрифтів) — маскуємо блоб.
- **`^GF`-енкодер тестуємо окремо** синтетичним 1-bit бітмапом (не з GDI) → детерміновані байти.
- Парсинг JSON→`LabelModel`; пакетність first/regular/last; кеш `Formatting`; мульти-`DeviceID`;
  `MapResult`; растровий фолбек штрихкоду. Критерій — exit 0.

**L-p2 — transport-e2e через `TransportTCP` → `label_printer_emulator` (без 1С, без заліза).**
Драйвер друкує по TCP у мережевий емулятор; звіряємо, що емулятор отримав очікувану **структуру
ZPL** (не повний байт-збіг через недетермінізм растру).

**L-p3 — `label_native_host.exe` (через головну DLL, без 1С).**
`LoadLibraryW` → компонента `LabelPrint` через `IComponentBase` → `Подключить(tcp://127.0.0.1:
<port>)` + `ПечататьЭтикетки(...)` проти in-process емулятора → перевірка `ResultEnvelope` +
отриманого ZPL. Тестує **реальну DLL + фасад + маршалінг tVariant**.

**L-p4 — standalone `label_printer_emulator.exe` (ручний тест із РЕАЛЬНОЇ 1С без заліза).**
Емулятор на порту; з 1С під'єднати компоненту до `tcp://127.0.0.1:<port>`, надрукувати —
емулятор складає ZPL у файл. Валідовує весь шлях 1С → компонента → ZPL.

**Емулятор — «тупий захоплювач»:** слухає TCP, приймає сирий потік, зберігає ZPL у файл/буфер,
у тестах — перевірка структури. **Візуальна перевірка етикетки — через labelary.com** або
локальний `BinaryKits.Zpl.Viewer` (ZPL-рендер не винаходимо).

**spooler-RAW** покриваємо: (а) у selftest через `SetSendFunctionForTest` (перехоплення байтів);
(б) ручний смоук на реальному USB-принтері (єдине, що не бачить мережевий емулятор).

Інтеграція в `run_tests.ps1` — окремі етапи (як L0.7 / L2-ecr), ненульовий exit при провалі.

---

## 13. Збірка (CMake)

- **`transport_component`**: додати `Transport_SpoolerRaw.{h,cpp}`; лінк `winspool.lib`.
- **`driver_label_printer_component` [НОВЕ]**: `LabelModel.h`, `LabelZplGenerator.{h,cpp}`,
  `LabelRaster.{h,cpp}`, `LabelPrinterDriver.{h,cpp}`; лінк `gdiplus.lib`; deps:
  `base_component spdlog nlohmann_json transport_component platform_component`.
  (Без `wire_component` — `DeviceSession` у v1 не потрібна.)
- **`label_facade_component` [НОВЕ]**: `AddinLabelPrint.{h,cpp}`; deps: `base_component spdlog
  nlohmann_json helpers_component driver_label_printer_component platform_component`.
- Обидві нові OBJECT-цілі → `$<TARGET_OBJECTS:...>` фінальної DLL; фасадні файли — у
  `HEADER_FILES`/`SOURCE_FILES`.
- **Тести** у `tests/CMakeLists.txt`: `label_printer_selftest`, `label_native_host`,
  `label_printer_emulator` (+ спільний `support/` за потреби), збірка завжди при `BUILD_TESTS=ON`.

---

## 14. Межі модулів (для ізоляції й тестованості)

| Модуль | Що робить | Вхід → вихід | Залежить від |
|---|---|---|---|
| `LabelModel` | нормалізована структура етикетки | — (POD) | — |
| `LabelRaster` | текст/картинка/рамка → 1-bit → `^GFA` | поля+DPI → байти `^GF` | GDI+ |
| `LabelZplGenerator` | розкладка → ZPL (нативні штрихкоди + `^GF`) | `LabelModel`+DPI → ZPL-байти | `LabelRaster` |
| `TransportSpoolerRaw` | доставка RAW у Windows-чергу | байти → пристрій | Winspool, `ITransport` |
| `LabelPrinterDriver` | оркестрація, мульти-DeviceID, пакетність | JSON-модель → `ResultEnvelope` | генератор + транспорт + platform |
| `AddinLabelPrint` | фасад 1С | tVariant ↔ JSON | драйвер + ядро |

Кожен модуль тестується окремо; генератор і енкодер — без заліза й без 1С.

---

## 15. Обробка помилок

- Бізнес-помилки — через `ResultEnvelope::Fail(code, description)`, ніколи `throw` за межу 1С.
- Кожен публічний метод драйвера й фасаду — у try/catch → `Fail("EXCEPTION", e.what())`.
- Логування — лише через макроси `ServiceTools` (`REPORT_*` у фасаді, `NEUTRAL_REPORT_*` у
  драйвері/фонових шляхах), повідомлення конкатенацією, з великої, без крапки (конвенції проєкту).
- Непідтримуваний штрихкод → **растровий фолбек + `WARN`**; лише якщо й він неможливий —
  `Fail("UNSUPPORTED_BARCODE", …)`.

---

## 16. Ризики й відкриті питання

- **GDI+ на 1С-сервері без десктопа** — перевірити offscreen-рендер (для друку етикеток шлях
  зазвичай клієнтський/РМК).
- **Точність нативних штрихкодів** vs растр — зняти реальні відбитки (203/300 dpi), перевірити
  grade сканером; за потреби переглянути дефолтні `^BY`/щільність.
- **Рідкісні типи** (`Code16k`, `GS1DataBarExpandedStacked`) — підтвердити нативну підтримку ZPL;
  інакше растровий фолбек.
- **Стиснення `^GF`** (ACS/Z64) — увімкнути після базового ASCII-hex, якщо трафік/пам'ять тиснуть.
- **Сирий USB без Windows-черги** — очікуємо, що такого сценарію немає (підтверджено); якщо
  зʼявиться — окрема v2-гілка з WinUSB.

---

## 17. Джерела

- Дослідження: `docs/research/label-printers-hardware-integration.md`.
- Контракт 1С БПО «Принтер этикеток» (розд. 3.7) — `tmp/3.7. Требования к разработке драйверов
  для принтеров этикеток.md` (перенести у `docs/` при потребі; ITS `metod8dev/content/4829`).
- Загальні вимоги до драйверів БПО — `tmp/Разработка драйвера для подключения оборудования
  локально к устройству пользователя.md` (ITS `metod8dev`).
- Шаблон драйвера/фасаду: `src/drivers/ecr_privatjson/`, `src/components/AddinECRPrivatJSON.*`,
  `src/platform/{ResultEnvelope,JobEngine}.h`, `src/transport/{Transport,Transport_TCP}.*`.
