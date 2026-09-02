# Архітектура драйвера LabelPrinter (принтер етикеток, ZPL)

Другий драйвер обладнання проєкту. На відміну від [ECRPrivatJSON](ecrprivatjson.md), друк
етикеток **односпрямований і синхронний**, тож фундамент [device-core](device-core.md)
(`DeviceSession`/framer/класифікатор) тут **не задіяний** — драйвер сам володіє `ITransport`
на кожен пристрій. Зі спільної платформи перевикористано лише `ResultEnvelope` (уніфікований
результат) і транспортний інтерфейс `ITransport` (+ наявний `TransportTCP`). У 1С виставляється
компонента **`LabelPrinter`** — фасад **БПО** («Библиотека подключаемого оборудования», тип
обладнання `LabelPrinter`) поверх спільної бази `BpoFacadeBase`: параметри підключення приходять
**поодинці** через `УстановитьПараметр`, пакет друку — **XML**.

Цей документ описує **внутрішню будову** драйвера — для розвитку й додавання можливостей. Хто
пише **прикладний 1С-код** проти готового драйвера — читає окрему теку
[docs/integration-1c/](../integration-1c/README.md).

**Цей документ — джерело правди** про будову драйвера; імена й поведінка звірені з кодом
(`src/drivers/label_printer/*`, `src/components/AddinLabelPrinter.*`,
`src/transport/Transport_SpoolerRaw.*`, `src/platform/*`). Прикладний контракт із боку 1С —
стандартна «Библиотека подключаемого оборудования» (БПО), тип «Принтер этикеток»
(розд. 3.7 документації ІТС).

---

## 1. Шари драйвера

```
1С:Підприємство  (підсистема БПО «Подключаемое оборудование»)
   │ IComponentBase / tVariant · короткі імена контракту (BOOL + OUT + ПолучитьОшибку)
┌──▼──────────────────────────────────────────────────────────────────┐
│ КОНТРАКТ  BpoFacadeBase          (src/components/, bpo-contract.md §2)│
│   системні методи, мапа УстановитьПараметр, ИДУстройства, CodeToInt    │
├──────────────────────────────────────────────────────────────────────┤
│ ФАСАД  AddinLabelPrinter : BpoFacadeBase  (компонента 1С «LabelPrinter»)│
│   гачки типу обладнання + ИнициализацияПринтера/ПечатьЭтикеток;        │
│   XML ⇄ LabelModel (LabelXml/pugixml); ResultEnvelope → BOOL + lastError│
├──────────────────────────────────────────────────────────────────────┤
│ ДРАЙВЕР  LabelPrinterDriver                                           │
│   map<DeviceID, DeviceContext>: транспорт + кеш Formatting + per-device m │
│   batch state machine (first/regular/last) · MapResult · GdiplusRuntime │
│   ├─ LabelZplGenerator (── «кодек»)   LabelModel → ZPL (^XA…^XZ)        │
│   │    ├─ LabelRaster     текст/картинка/рамка → 1-bit (GDI+)          │
│   │    ├─ GfEncoder       1-bit bitmap → ^GFA (з тайлінгом по рядках)  │
│   │    └─ BarcodeZpl      нативні штрихкоди (^BE/^BC/^BQ/…), ^FH-escape │
├──────────────────────────────────────────────────────────────────────┤
│ ПЛАТФОРМА  ResultEnvelope                              (src/platform/) │
├──────────────────────────────────────────────────────────────────────┤
│ ТРАНСПОРТ  ITransport → TransportSpoolerRaw (Winspool) | TransportTCP  │
└──────────────────────────────────────────────────────────────────────┘
      ▲ e2e-тест: LabelEmulator (TCP-захоплювач ZPL, localhost)
```

Уся ZPL-специфіка ізольована у чотирьох класах генератора
(`LabelZplGenerator`/`LabelRaster`/`GfEncoder`/`BarcodeZpl`); транспорт свопабельний за
`ITransport`; `LabelModel` — стабільний внутрішній контракт, від якого фасад незалежний.
`DeviceSession`/`JobEngine`/wire-компоненти у v1 не потрібні (друк — односпрямований потік).

---

## 2. Прикладний контекст (стисло)

Принтер етикеток — **інтерпретатор командної мови** (ZPL/EPL/TSPL). Верстку етикетки користувач
робить у самій 1С; компонента — **міст даних 1С → принтер**: приймає стандартний БПО-пакет
(шаблон + дані у XML) і перетворює його на потік ZPL, доставляючи на пристрій **в обхід GDI-друку**
вендорського драйвера. Це прибирає потребу в платних per-printer драйверах.

- **Мова команд v1 — ZPL** (спільний знаменник; емулюється TSC/Godex/клонами). TSPL/EPL — пізніше.
- **Одиниці — dots/mm** (`8`/`12`/`24` ≈ «203/300/600 dpi»), бо номінальні DPI неточні; коефіцієнт
  задається параметром підключення `DotsPerMm`. Переклад — `LabelUnits.h`:
  `mmToDots(mm) = round(mm·dotsPerMm)`, `ptToDots(pt) = round(pt·dotsPerMm·25.4/72)`.
- **Транспорт v1 — spooler-RAW** (Winspool за іменем Windows-черги — покриває USB/локальні) +
  **TCP:9100** (мережеві).
- **Контекст — клієнтський** (інтерактивна сесія, де фізично стоїть принтер): там підтримується
  GDI+. Серверна служба й мобільні клієнти — поза обсягом (Windows-only компонента).
- Семантика результату — **«прийнято»**, не «надруковано» (статус-полінгу немає, це v2).

---

## 3. Модель даних — `LabelModel`

`src/drivers/label_printer/LabelModel.h` — типізоване дзеркало БПО-контракту `LabelsTable`
(розд. 3.7 ІТС) з **коректною value-семантикою**. Замість одного generic-поля — окремі типи полів:
`TextField`, `BarcodeField`, `ImageField`, `UserDataField`; геометрія — `FieldGeom`
(`left/top/width/height` у мм + `orientation` 0/90/180/270).

- `LabelFormatting` — розкладка етикетки (`width`/`height` мм + вектори полів). **Кеш формату**
  драйвера.
- `LabelRecord{fieldName, value}` — `value` — `std::optional<std::string>`: **відсутній атрибут ≠
  порожній рядок** (ключове для семантики дефолтів).
- `LabelInstance{quantity, records}` — один екземпляр етикетки; `quantity` → `^PQ` (копії).
- `LabelBatch{formatting?, labels}` — пакет; `formatting` присутнє лише при `PackageStatus="first"`.
- `DeviceProfile` — параметри пристрою (`transport`, `printerName`|`host`/`port`, `dotsPerMm`,
  `darkness`, `speed`, `labelWidthMm`/`labelHeightMm`, `homeXDots`/`homeYDots`).

**Value-семантика полів** (за контрактом ІТС 3.7, реалізовано в `LabelXml`/`LabelRaster`/`LabelZplGenerator`):

| Поле | `Static=true` | `Static=false` (динамічне) |
|---|---|---|
| Text / UserData | `Formatting.Value` — спільне значення | `Record.value` за іменем; якщо запису немає — `Formatting.Value` як **дефолт** |
| Barcode | `Formatting.ValueBase64` — **Base64** (декодується) | `Record.value` — **простий текст** (не Base64) |
| Image | `Formatting.Value` — **Base64** | `Record.value` — **Base64** |

`PackageStatus` (`first`/`regular`/`last`) — **лише аргумент методу**, у моделі його немає.

---

## 4. Генерація ZPL — гібридна стратегія

`LabelZplGenerator::BuildLabel(fmt, instance, profile) → GenResult` формує один `^XA … ^XZ` на
екземпляр. **Гібрид:** штрихкоди — **нативними ZPL-командами** (чіткі, скануються); текст,
картинки, рамки — **растеризуються GDI+** у 1-bit і вставляються одним `^GF`. Порядок збірки:

1. `^PW`/`^LL` з **геометрії формату** (`Formatting.Width/Height` — авторитет розкладки пакета),
   а не з device-профілю.
2. Растровий шар: `LabelRaster::Render → Bitmap1` → `GfEncoder::EncodeGfa` → один/кілька `^GFA`.
3. Нативні штрихкоди: для кожного `BarcodeField` — `BarcodeZpl::Emit`.
4. `^PQ<quantity>` (копії) → `^XZ`.

`GenResult{ok, zpl, errCode, errDesc}`: при `ok=false` — `errCode` один з
`UNSUPPORTED_BARCODE`/`RENDER_ERROR`/`BAD_INPUT`/`BARCODE_TOO_WIDE`/`EXCEPTION`. `valueOf` (значення
поля за іменем із записів екземпляра) — спільне джерело для растру й штрихкодів; відсутній запис →
`nullopt`, і растр/генератор підставляє статичне/дефолтне значення поля.

### 4.1. Растр — `LabelRaster` (GDI+)

`LabelRaster::Render(fmt, valueOf, dotsPerMm, ok&) → Bitmap1`
(`Bitmap1{widthDots, heightDots, rows}`, 1bpp MSB-first, `1=чорний`):

- Offscreen `Bitmap(W,H, 32bppARGB)` у нативній роздільності → `Graphics`, білий фон,
  `TextRenderingHintSingleBitPerPixelGridFit` (без антиаліасу — чистий 1-біт).
- **Текст:** шрифт у `UnitPixel` (кегль через `ptToDots`, мінімум 1px), fallback-родина `Arial`,
  вхід — UTF-16 через `ServiceTools::SafeMB2WCHAR`; `DrawString` у прямокутник поля. Рамка — `HasBorder`.
- **Картинки:** Base64 → `Image::FromStream` поверх `IStream`/`HGLOBAL`
  (`GMEM_MOVEABLE`+`fDeleteOnRelease`), `DrawImage` у прямокутник.
- **32bpp → 1bpp:** `LockBits` (read), поріг за яскравістю (`(R+G+B)/3 < 128 → чорний`), MSB-first.
- **Порогова контрактна деталь `ok`:** `ok=true` навіть при **легітимно-порожньому** растрі (немає
  text/image/border); `ok=false` **лише при реальному збої GDI+** (некоректний розмір, Bitmap/Graphics
  не створено, `LockBits` fail, виняток) — тоді генератор піднімає `RENDER_ERROR`, а не тихо губить растр.
- **Спільний хелпер `DecodeBase64`** (нестрогий: пропускає пробіли/переноси, стоп на `=`) —
  використовує і растр, і генератор (static-штрихкоди).

> **v1-межа:** орієнтація **растрових** полів не реалізована. Якщо у `TextField`/`ImageField`
> `orientation != 0` — `Render` повертає `ok=false` → генератор дає `RENDER_ERROR` (**явна помилка,
> не тихе ігнорування**). Поворот **штрихкодів** працює (див. 4.3).

### 4.2. `GfEncoder` — 1-bit bitmap → `^GFA` з тайлінгом

`GfEncoder::EncodeGfa(bm, xDots, yDots, maxRowsPerTile) → string`. Повний формат
`^FO x,y ^GFA,<b>,<c>,<d>,<hex> ^FS`, де `b`=байтів передати, `c`=повний розмір, `d`=`bytesPerRow =
ceil(widthDots/8)`. **Тайлінг по рядках:** якщо `heightDots > maxRowsPerTile`, бітмап ріжеться на
кілька `^GF` з власними `^FO` (наступний зі зсувом `yDots+row`), щоб не впертися в ліміти поля
`^GF`/памʼяті принтера на великих етикетках (`24 dots/mm`). Ліміт тайла — `kMaxTileBytes = 60000`,
`maxRowsPerTile = max(1, 60000/bytesPerRow)`.

### 4.3. `BarcodeZpl` — нативні штрихкоди

`BarcodeZpl::Emit(field, value, dotsPerMm) → BarcodeEmit{ok, zpl, errCode, errDesc}`. Формує
`^FO x,y ^BY<moduleW> <barcode-cmd> <^FD…> ^FS`. Мапінг типів БПО → ZPL:

| Тип БПО | ZPL | Примітка |
|---|---|---|
| EAN13 | `^BE` | нормалізація: 13 цифр → перші 12 (контрольну рахує принтер) |
| EAN8 | `^B8` | 8 цифр → перші 7 |
| Code128 | `^BC` | орієнтація — перший параметр (`N/R/I/B`) |
| EAN128 (GS1-128) | `^BC` + **FNC1** | префікс `>;>8` вставляється **поза** `^FH`-escaping (лишається керуючим) |
| Code39 | `^B3` | |
| Code93 | `^BA` | |
| ITF14 | `^B2` | Interleaved 2of5 (не `^BC`) |
| QRCode | `^BQ` | |
| DataMatrix | `^BX` | |
| PDF417 | `^B7` | |
| GS1DataBarExpandedStacked | `^BR` | тип Expanded Stacked + ширина сегмента |
| інший (напр. Code16K) | — | → `UNSUPPORTED_BARCODE` |

- **Орієнтація** (`geom.orientation`) → `orientChar`: `0→N`, `90→R`, `180→I`, `270→B` (штрихкоди
  повертаються нативно, на відміну від растру).
- **Ширина модуля `^BY`** з `geom.width`: `moduleW = avail/estM` (де `estM` — оцінка модулів символіки
  з quiet-zone, `estBarcodeModules`); клампиться `1..10 dots`; якщо `<1` (не влазить) → `BARCODE_TOO_WIDE`.
- **`^FD`-escaping — `EscapeFd`:** якщо дані містять `^`/`~`/`>`/control-байти — переходить на `^FH_`
  hex-escaping (символ `_` як escape-префікс). Оскільки **текст растеризується**, `^FD` вживається лише
  для даних штрихкодів, тож ризик кирилиці в `^FD` знято.
- **Static-штрихкоди:** `staticValueBase64` **декодується з Base64** (за контрактом ІТС
  `Formatting.ValueBase64` — це Base64) у корисне значення; некоректний Base64 → `BAD_INPUT`.

> **v1-межа:** `EAN13Addon2`/`Addon5` та UPC-extensions **відкладені** — тип, що містить `Addon`
> або `Extension`, дає `UNSUPPORTED_BARCODE` («відкладено до v2»).

### 4.4. `BuildInit` — ініціалізація принтера

`LabelZplGenerator::BuildInit(profile)` — окремий `^XA … ^XZ` з device-профілю: темність `^MD`,
швидкість `^PR`, розмір `^PW`/`^LL` (з `labelWidthMm`/`labelHeightMm`), home `^LH`. Геометрію
конкретного формату (`^PW/^LL`) **перевизначає** `BuildLabel` — щоб уникнути конфлікту
«device-профіль vs формат пакета».

---

## 5. GDI+ lifecycle — `GdiplusRuntime`

`LabelRaster.{h,cpp}` містить RAII-обгортку `GdiplusRuntime` — **процес-широку ref-count**
ініціалізацію GDI+ (`GdiplusStartup`/`GdiplusShutdown` рівно один раз під `g_gdiMutex`, лічильник
`g_gdiRefs`). Ключове рішення:

- **Член драйвера, перший у класі** (`LabelPrinterDriver::gdiplus_`): конструюється до реєстру
  пристроїв, руйнується після нього — тож увесь час життя драйвера (= час життя компоненти) GDI+
  ініціалізовано, і растровий шлях `BuildLabel → LabelRaster::Render` завжди має робочий GDI+.
  Без цього члена растр (текст/картинки/рамки) тихо випадав би, лишаючи самі штрихкоди.
- **Поза `DllMain`:** `GdiplusStartup`/`GdiplusShutdown` заборонено з loader-lock; компонента
  конструюється 1С поза `DllMain`, тож інваріант дотримано. Non-copyable.

---

## 6. Транспорти

`ITransport` (7 методів) — спільний інтерфейс. Драйвер обирає реалізацію за `DeviceProfile::transport`.

**`TransportSpoolerRaw` [НОВЕ]** — `src/transport/Transport_SpoolerRaw.{h,cpp}`, RAW-друк у
Windows-чергу за **іменем** (Winspool). Семантика `ITransport`:

- `Open()` → `OpenPrinter`-хендл; `IsOpen()` — стан хендла; `Close()` — `ClosePrinter`.
- `Send()` — **повна job-послідовність** `StartDocPrinter{pDatatype="RAW"}` → `StartPagePrinter` →
  `WritePrinter` → `EndPagePrinter` → `EndDocPrinter`, повертає к-сть записаних байтів (перевірка
  `pcWritten == requested` — all-or-error).
- Канал **write-only:** колбеки `SetData/Error/ConnectionState` зберігаються, але **no-op**.
- Тест-шов `SetSendFunctionForTest(fn)` — перехоплює етап запису замість реального Winspool-циклу.
- Лінк `winspool.lib`. Деплой-нота: найнадійніша універсальна черга — **Generic / Text Only**
  (RAW обходить GDI-рендер вендорського драйвера).

**`TransportTCP` [НАЯВНЕ]** — сокет на `ip:9100`, перевикористаний як є.

Драйвер відкриває транспорт **ліниво** — при першому `Send` (`SendBytes` робить `Open()`, якщо
`!IsOpen()`). Усі транспортні відмови зводяться до єдиного коду `TRANSPORT_ERROR`, конкретика —
у `description`.

---

## 7. Драйвер — `LabelPrinterDriver`

`src/drivers/label_printer/LabelPrinterDriver.{h,cpp}`. Оркеструє мульти-пристрій, batch і
concurrency; **синхронно, виняток-безпечно** (try/catch → `Fail`). Публічний API:
`Connect(profile) → DeviceID`, `InitializePrinter`, `PrintLabels`, `Probe`, `Disconnect`,
`IsConnected` + тест-шов `SetTransportFactoryForTest`.

**`Probe(deviceId)`** — форсує ліниву `Open()` і одразу закриває: доводить **досяжність**
(TCP — конект на `host:port`; spooler — `OpenPrinter` на черзі) і **нічого не друкує**. Ініт-пакет
змінив би стан принтера (темність/швидкість/розмір), а адміністратор, який тисне «Тест
устройства», на це не підписувався. Уже відкритий транспорт лишається відкритим і повертає `Ok`
без чіпання каналу. Невідомий `DeviceID` → `NOT_CONNECTED`; канал не відкрився → `TRANSPORT_ERROR`.

### 7.1. Мульти-пристрій і concurrency

- `map<DeviceID, shared_ptr<DeviceContext>>`; `DeviceContext` = `unique_ptr<ITransport>` +
  `optional<LabelFormatting> cachedFormatting` + `DeviceProfile` + **per-device `mutex m`**.
- `Connect(profile)` створює транспорт **поза** `registryMutex_` (щоб паралельні `Connect` не
  серіалізувалися на I/O-конструкторах) і повертає детермінований `DeviceID` (лічильник `nextId_`
  рядком). Тест-фабрика `SetTransportFactoryForTest` (ставиться до `Connect`) підміняє `MakeTransport`.
- **`registryMutex_` тримається лише навколо `devices_`/`nextId_`** — ніколи під час
  GDI+/Winspool/TCP. `Lookup` повертає **shared-копію** контексту, взяту під замком: вона тримає
  контекст живим на весь час in-flight операції, навіть якщо паралельний `Disconnect` того ж
  `DeviceID` зробить `erase` — **UAF неможливий**.
- **Пакетну послідовність одного пристрою** серіалізує `ctx->m` (генерація+`Send`); `Disconnect`
  теж бере `ctx->m` перед `Close`, тож не рве активний `Send`.
- Конструктор/деструктор `LabelPrinterDriver` винесені в `.cpp` (повний тип `ITransport` для
  знищення `unique_ptr` у `DeviceContext`); dtor закриває всі транспорти, що лишилися.

### 7.2. Batch state machine (кеш `Formatting`)

`PrintLabels(deviceId, batch, packageStatus)` керує кешем формату per-DeviceID:

| `packageStatus` | Поведінка |
|---|---|
| `first` | вимагає `batch.formatting` (інакше `BAD_INPUT`); reset старого + set нового кешу; друкує |
| `regular` | вимагає активного кешу (інакше `BAD_INPUT`, навіть якщо пакет несе власне форматування); друкує з кешу |
| `last` | як `regular`, але після друку **очищає кеш** (`cachedFormatting.reset()`) |

`Formatting` встановлює **лише `first`**. `Disconnect`/dtor знімають реєстрацію (кеш зникає з
контекстом).

### 7.3. `MapResult` → `ResultEnvelope`

Цикл по `batch.labels`: `BuildLabel` → `SendBytes`. **Семантика чесна:** успіх = *прийнято*
(TCP — байти в сокет; spooler — job у черзі), **не фізичний друк**.

- Успіх усього пакета → `Ok({acceptedInstances, acceptedCopies})` (`acceptedCopies` — сума
  `quantity`).
- Перша ж помилка перериває пакет і повертає `Fail(code, desc)` з **частковим прогресом** у payload:
  `failedIndex`, `acceptedInstances`, `acceptedCopies` (скільки прийнято до збою).

**Таксономія кодів драйвера:** `NOT_CONNECTED` (невідомий DeviceID), `BAD_INPUT` (батч/Base64/стан
batch), `TRANSPORT_ERROR` (будь-яка відмова транспорту, зокрема short-write),
`UNSUPPORTED_BARCODE` (символіка/addon/extension), `RENDER_ERROR` (збій GDI+ або растрова
орієнтація), `EXCEPTION` (перехоплений виняток). Генератор може додати `BARCODE_TOO_WIDE`.

---

## 8. БПО-фасад — `AddinLabelPrinter : BpoFacadeBase`

`src/components/AddinLabelPrinter.{h,cpp}`. Тонка компонента: `REGISTER_COMPONENT(u"LabelPrinter",
AddinLabelPrinter)`, тримає `LabelPrinterDriver driver_`. **Системна половина контракту живе не
тут, а в базі** `BpoFacadeBase` (`bpo-contract.md` §2, §2.6): системні методи, накопичення
параметрів, `ИДУстройства`, `lastError`, `CodeToInt`. Фасад реалізує лише **гачки** типу
обладнання плюс два функціональні методи. `InterfaceRevision() == 3004`.

**Чому 3004, а не вище** (виправлено 2026-09-01, було `4007`): гілок за ревізією для друку
етикеток у конфігурації немає, тобто число нічого не перемикає, — а з `4000` 1С починає кликати
`УстановитьЛокализацию`, якого ми не реалізуємо.

**Системні методи** реєструє `BpoFacadeBase::RegisterSystemMethods()` — короткі імена, які 1С
реально кличе (повна таблиця з параметрами — `bpo-contract.md` §2):

```
ПолучитьРевизиюИнтерфейса · ПолучитьНомерВерсии · ПолучитьОписание · ПолучитьПараметры
УстановитьПараметр · Подключить · Отключить · ТестУстройства · ПолучитьОшибку
ПолучитьДополнительныеДействия · ВыполнитьДополнительноеДействие
```

⚠️ **Старі довгі імена за документом ІТС** (`ПодключитьОборудование`, `ПараметрыОборудования`,
`ТестированиеОборудования`, `ОтключитьОборудование`, `УстановитьИнформациюПриложения`)
**прибрано**: 1С їх не кличе **в жодній із двох конфігурацій** (доказ — `bpo-contract.md` §2), тож
драйвер через штатну підсистему не підхоплювався взагалі. `УстановитьИнформациюПриложения` не
замінено нічим — нуль викликів в обох продуктах.

**Функціональні методи** (`RegisterPrinterMethods`):

| EN / RU | Параметри (EN / RU) | Повертає |
|---|---|---|
| `InitializePrinter` / `ИнициализацияПринтера` | `DeviceID`/`ИДУстройства`[IN] | BOOL |
| `PrintLabels` / `ПечатьЭтикеток` | `DeviceID`/`ИДУстройства`[IN], `LabelsTable`/`ДанныеДляВыгрузки`[IN XML], `PackageStatus`/`СтатусПакета`[IN] | BOOL |

**Гачки бази, які реалізує фасад:**

| Гачок | Що робить у принтера |
|---|---|
| `InterfaceRevision` | `3004` |
| `BuildDriverInfo` | `EquipmentType="LabelPrinter"`, `logEnabled=false` (вимога ІТС про лог за замовчуванням стосується ККТ і POSTerminal; параметрів `LogPath`/`LogLevel` у формі принтера немає) |
| `BuildSettingsXml` | форма налаштувань: транспорт/черга/IP/порт/DPI/темність/швидкість/розмір |
| `AcceptEquipmentType` | приймає `ПринтерЭтикеток` (ім'я значення переліку — саме це шле 1С) **і** `LabelPrinter` |
| `OpenDevice` | `LabelXml::ProfileFromParameters(Params())` → валідація → `driver_.Connect` |
| `CloseDevice` | `driver_.Disconnect(DeviceId())` |
| `ProbeDevice` | тіло `ТестУстройства` — див. нижче |
| `BuildActionsXml`/`RunAction` | **дефолтні** — додаткових дій у принтера немає |

**⚠️ ОДИН об'єкт = ОДИН пристрій.** Контракт накопичує параметри **на об'єкті** до `Подключить`,
тож параметри другого принтера затерли б перший. Це обмеження **ФАСАДУ**; сам
`LabelPrinterDriver` лишається мульти-пристроєвим (§7.1) — на два принтери потрібні два об'єкти
компоненти. `OpenDevice` віддає нагору **справжній** `DeviceID` драйвера (не синтезований), щоб
лог фасаду й драйвера збігався; повторний `Подключить` без `Отключить` (штатний сценарій
перепідключення після помилки друку) знімає старий пристрій **лише** після успішної реєстрації
нового.

**`ТестУстройства` реально перевіряє зв'язок.** `ProbeDevice` розбирає накопичені параметри,
вимагає обов'язкові для обраного транспорту (`Host` для `tcp`, `PrinterName` для `spooler` —
інакше `BAD_INPUT` з іменем поля, а не «принтер недоступний»), і кличе `LabelPrinterDriver::Probe`
— фактичне відкриття/закриття каналу **без друку** (§7). Підключеним фасад бути не зобов'язаний:
форма налаштувань кличе `ТестУстройства` одразу після `УстановитьПараметр`. Якщо пристрій **уже**
підключений, проба йде **на ньому самому** — окрему сесію на `:9100` типовий ZPL-принтер не
приймає, і адміністратор побачив би `TRANSPORT_ERROR` на справному пристрої; тимчасовий пристрій
проби знімається, чужий — ні. `РезультатТеста` несе людський опис цілі (`tcp://host:port` або
`черга Windows "…"`) і вердикт; `АктивированДемоРежим` — завжди `Ложь` (демо-режиму драйвер не має).

**Формат `ПолучитьПараметры` суворий** (виправлено 2026-08-31): корінь **`Settings`** →
`Page@Caption` → `Group@Caption` → `Parameter@Name/@Caption/@TypeValue/@DefaultValue/@Description`
(+ вкладений `ChoiceList/Item@Value`). Форма налаштувань БПО входить у розбір лише за коренем
`Settings` і читає тип з `TypeValue`; попередній варіант (корінь `Parameters`, атрибут `Type`)
давав **мовчки порожню форму**. Прикладний бік — `docs/integration-1c/label_printer.md` §3.

⚠️ **Значення параметрів приходять їхніми ОГОЛОШЕНИМИ типами**, а не рядками: `Port`/`DotsPerMm`/
`Darkness` оголошені як `Number` — 1С шле `VTYPE_R8`. Тому база читає їх толерантним
`VariantToString`, а не прямим `static_cast<std::string>(VH)`, який кинув би на всьому, крім
`VTYPE_PWSTR`.

Решта конвенцій:

- **`try/catch` у кожному методі** (виняток C++ межу 1С не перетинає): при винятку — `REPORT_ERROR`
  + `lastError`, не сирий текст. LONG-код для `ПолучитьОшибку` дає `BpoFacadeBase::CodeToInt` —
  **єдина числова таксономія на всю компоненту**: `OK=0`, `NOT_CONNECTED=1`, `EXCEPTION=11`,
  `BAD_INPUT=12`, `TRANSPORT_ERROR=13`, `UNSUPPORTED_BARCODE=14`, `BARCODE_TOO_WIDE=15`,
  `RENDER_ERROR=16`; суто числовий код пристрою проходить як є; невідомий нечисловий код і збої
  рівня фасаду → `-1`. Повна таблиця — `bpo-contract.md` §4.3. **Це НЕ старі коди `1..7`** —
  власної нумерації в принтера більше немає.
- Валідація обов'язкових полів профілю живе **у фасаді**, а не в
  `LabelXml::ProfileFromParameters`: її контракт «порожня мапа → дефолти, а не помилка»
  закріплений тестом і ламати його не можна.
- `EnableLogging`/`ИспользоватьЛогирование` — успадкований (реєструвати не треба); у деструкторі —
  `driver_.Disconnect(DeviceId())`, `ServiceTools::DisableComponentLogging(this)` робить база.
  `Version` — property, не метод.

**XML-адаптер `LabelXml`** (pugixml): толерантний, з дефолтами; **невідомі параметри/атрибути
ігноруються** (вимога БПО); `OptAttr` реалізує «відсутній атрибут ≠ порожній рядок»
(`std::optional`). Корінь `LabelsTable` — `<Data>` (`Formatting` присутнє лише при `first`; далі
`Labels/Label/Record`). Параметри підключення розбирає `ProfileFromParameters(мапа)` — саме її
кличе фасад; `ParseConnectionParameters(XML)` лишилась як XML→мапа + той самий виклик (семантика
параметрів — в **одному** місці) і використовується тестами й історичним XML-пакетом ІТС.

---

## 9. Обсяг v1: реалізовано vs межі

**Реалізовано (вертикальний зріз):** підключення/відключення мульти-пристрою, `InitializePrinter`,
`PrintLabels` з batch state machine (`first`/`regular`/`last` + кеш `Formatting`), гібридний рендер
(нативні штрихкоди + растр GDI+ у `^GF` з тайлінгом), обидва транспорти (spooler-RAW + TCP:9100),
XML-контракт БПО, чесний `MapResult` («прийнято»), таксономія помилок, повний тест-контур.

**Свідомі v1-межі (as-built):**

- **Растрова орієнтація не реалізована** — `orientation != 0` на текст/картинку → `RENDER_ERROR`
  (поворот **штрихкодів** працює через ZPL `N/R/I/B`).
- **Addon/UPC-extension штрихкоди відкладені** → `UNSUPPORTED_BARCODE` («відкладено до v2»);
  символіки без нативної ZPL-команди (Code16K тощо) → `UNSUPPORTED_BARCODE` (растрового фолбеку
  немає — GDI+ не кодує символіки).
- **Немає статус-полінгу** (`<ESC>!?` → `DeviceSession`) — тому результат «прийнято», не «надруковано».
- Генератори **TSPL/EPL**, сирий USB (WinUSB), не-Windows/мобільні клієнти/серверний рендер — поза
  обсягом v1.
- **Сертифікаційний багатокомпонентний** split (інтеграційна DLL + IPC для стампа «1С:Сумісно») —
  окремий етап; функціонально односкладова компонента працює без нього.

---

## 10. Тестовий контур

Контур як в ECR: без обладнання, проти емулятора-захоплювача ZPL (`tests/support/LabelEmulator`,
Winsock). Збірка **завжди при `-WithTests`, без UAPKI**. Джерело правди складу — [AGENTS.md](../../AGENTS.md).

- **L-p1 `label_printer_selftest`** (головний гейт, без 1С/заліза): `LabelModel → ZPL` (структура
  команд проти еталону — `^FO` у dots, баркод-команди з даними, `^PQ`, ініт; `^GF`-блоб не звіряється
  байт-в-байт — GDI недетермінований), `GfEncoder` окремо синтетичним 1-bit бітмапом (+тайлінг),
  XML→`LabelModel` (усі поля/value-семантика), batch state machine, мульти-DeviceID, `MapResult`,
  `UNSUPPORTED_BARCODE`, `^FH`-escaping, `8/12/24 dots/mm`, e2e через `TransportTCP → LabelEmulator`
  (L-p2 інтегровано в selftest), смоук фасаду через `CreateObject`.
- **L-p3 `label_native_host`** — компонента `LabelPrinter` через **головну DLL** (`LoadLibraryW`+
  `GetClassObject`) проти in-process `LabelEmulator`. Переписаний 2026-09-01 під контракт: тест
  бере методи **за іменами, які кличе 1С** (`FindMethod`), а не за власними, — перевіряє
  **присутність** усіх 13 імен контракту (11 системних + `ИнициализацияПринтера`/`ПечатьЭтикеток`)
  і **відсутність** 5 старих довгих. Саме цього бракувало: попередня версія кликала власні ж імена
  й дефекту не бачила. Далі — ревізія `== 3004`, `УстановитьПараметр` із **числовим** значенням
  (`Port`/`DotsPerMm` приходять `VTYPE_R8`), відмова на чужому `EquipmentType`, `ТестУстройства`
  проти емулятора (BOOL + текст + `АктивированДемоРежим == Ложь`), `Подключить` з OUT-`ИДУстройства`,
  друк `ПечатьЭтикеток` і звірка отриманого ZPL, `ПолучитьОшибку` (зокрема `12` = `BAD_INPUT` на
  порожньому `Host`), `Отключить`.
- **`label_printer_emulator.exe`** (standalone, ручний) — TCP-емулятор ZPL-принтера для тесту з
  **реальної 1С без обладнання** (зберігає ZPL; візуалка — Labelary).

Гейт (`run_tests.ps1`): L-p1 → L-p3; обидва рівні проходять і без `-WithUAPKI`. Точний склад
перевірок і поточні лічильники — джерело правди [AGENTS.md](../../AGENTS.md); гейт дивиться лише
exit-код 0.

---

## 11. Збірка (CMake, коротко)

- `driver_label_printer_component` (OBJECT): `LabelModel`/`LabelUnits`/`GfEncoder`/`BarcodeZpl`/
  `LabelRaster`/`LabelZplGenerator`/`LabelPrinterDriver`/`LabelXml`; deps `base_component spdlog
  nlohmann_json`.
- `bpo_facade_component` (OBJECT): `BpoFacadeBase` — спільна контрактна половина БПО-фасадів.
  Виділена **окремою** ціллю свідомо: `label_printer_selftest` лінкує саме її й не має тягнути
  еквайринговий код (інакше знадобились би `driver_ecr_privatjson_component` + `wire_component`).
- `label_facade_component` (OBJECT): `AddinLabelPrinter`; deps `+helpers_component
  driver_label_printer_component platform_component bpo_facade_component`.
- `Transport_SpoolerRaw` — у транспортному компоненті.
- Усі OBJECT-цілі → `$<TARGET_OBJECTS:…>` фінальної DLL. **Системні бібліотеки лінкуються на
  ФІНАЛЬНИЙ таргет і тест-цілі** (`winspool gdiplus ole32`) — бо лінк на object-бібліотеці не
  пропагується. pugixml — vendored у `extern/`.

---

## 12. Пов'язані документи

- [bpo-contract.md](bpo-contract.md) — контракт «Подключаемое оборудование»: імена, які РЕАЛЬНО
  кличе 1С, шарування фасадів (§2.6), єдина числова таксономія помилок (§4.3). **Джерело правди
  щодо системної половини цього фасаду.**
- [device-core.md](device-core.md) — фундамент драйверів; тут **не** задіяний (друк
  односпрямований), але `ResultEnvelope`/`ITransport` перевикористано.
- [ecrprivatjson.md](ecrprivatjson.md) — перший драйвер обладнання (шаблон драйвера/фасаду/тестів).
- [core.md](core.md) — ядро `AddInNative` (`REGISTER_COMPONENT`, `Ret`/`ParamSpec`, `VH`).
- [docs/integration-1c/label_printer.md](../integration-1c/label_printer.md) — інструкція для
  1С-розробника: методи, XML-контракт `LabelsTable`, символіки, приклади, деплой, тестування.
- [docs/integration-1c/](../integration-1c/README.md) — прикладна інтеграція з 1С (по документу на драйвер).
