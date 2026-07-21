# Пілотний драйвер ECRPrivatJSON — дизайн-специфікація

*Дата: 2026-07-21. Статус: затверджено напрям (3 розвилки вирішено з користувачем), специфікація на рев'ю.*

*Підготовлено за результатами глибокого дослідження: повне прочитання новішої специфікації
`ECR протокол ПриватБанк (JSON based) 1.0.3.5_integrator (ukr)_14012026` (3261 рядок) зі звіркою
зі старою `docs/ECR_Privat_JSON_Protokol.md` (v1.0.3.1); розбір офіційного архіву інтегратора
(емулятори, драйвери, референс-код `example.7z` — C#/Go), статті-практики infostart
(`infostart.ru/1c/tools/1408984`); 4 розрізи наявної кодової бази `src/transport/`.
Усі 14 вирішальних тверджень пройшли адверсарну верифікацію (CONFIRMED) + особисту звірку
з першоджерелами. Продовжує `docs/tasks/2026-07-19_platform_architecture_design.md` (§3, §6,
Етапи 1-2) і `docs/tasks/2026-07-20_design_device_transport_session.md` (wire-стек).*

---

## 1. Мета та обсяг

Реалізувати **пілотний драйвер ECRPrivatJSON** — перший конкретний драйвер обладнання поверх
наявного wire-фундаменту (`DeviceSession`) — **вертикальним зрізом**: спільні платформені
механізми (`OperationRegistry`, `JobEngine`, `ResultEnvelope`), драйвер, 1С-фасад і тестовий
контур розробляються разом, а платформа формується під реальні потреби пілота й з першого дня
валідується end-to-end проти власного емулятора термінала.

**У обсязі:**
- платформа: `OperationRegistry`, `JobEngine`, `ResultEnvelope` (лишок Етапу 1, §3.1 архдоку);
- драйвер: `EcrJsonCodec`, `EcrPrivatJsonClassifier`, `EcrPrivatJsonDriver`;
- 1С-компонента `AddinECRPrivatJSON` (фасад над драйвером);
- тестовий контур: протокол-обізнаний `TerminalEmulator` (TCP, localhost) + сценарії з
  байтових прикладів спеки, інтеграція в `run_tests.ps1` (рівні L0-L3).

**Транспорти пілота — TCP і COM.** Спека 1.0.3.5 як база (транспортний/канальний рівень
ідентичний 1.0.3.1, прикладний — суперсет; фундамент версійно-нейтральний).

**Поза обсягом** (§13): WS-клієнт-транспорт, брокер-сервіс мультидоступу, перевірка на реальному
залізі, заміна spdlog→власний Logger.

## 2. Затверджені рішення

| Розвилка | Рішення | Обґрунтування |
|---|---|---|
| **Порядок** | Вертикальний зріз: платформа + драйвер разом, e2e проти емулятора з дня 1 | Платформа доведена реальним споживачем, не проєктується у вакуумі; урок §2.2 архдоку — «жодних тестів проти вигаданих форматів»; розділення ядро↔драйвери збережене |
| **WS у пілоті** | Ні — TCP:2000 + COM 115200; WS-клієнт пізніше окремим кроком | Пілот легший, тестова поверхня менша; native-топологія достатня (див. §4) |
| **Тестування** | Емулятор-only — власний протокол-обізнаний TCP-емулятор; реальне залізо — окрема задача | Готового банківського емулятора термінала немає; архітектура дозволяє пізню верифікацію без переробки |

## 3. Ключові факти протоколу (верифіковано)

Джерела — секції спеки 1.0.3.5 (посилання на розділи в дужках).

### 3.1 Фізичний і канальний рівень

- **Канали:** USB → віртуальний COM (usbserial bridge) + вендорський драйвер моделі, апаратний
  COM, або Ethernet/WiFi. COM/USB — **`115200 8N1`** (фіксована «формула»). Мережа — **TCP,
  порт за замовчуванням `2000`** (налаштовується + DHCP-тумблер). *Термінал слухає (сервер),
  каса конектиться (клієнт).* (розд. «Опис фізичного рівня»)
- **Кадр = UTF-8 JSON + один термінатор `0x00`** (як C-рядок). Протокол єдиний для всіх каналів:
  на serial це канальний рівень, у TCP/IP — прикладний, без змін змісту. Кирилиця — завжди
  UTF-8. При розборі `0x00` зняти, решту декодувати. (розд. «Опис канального протоколу»)

### 3.2 Топологія — монопольний постійний доступ

- Термінал **«не є повноцінним мережевим сервером»** — немає керування доступом і запобігання
  колізіям; для мультидоступу інтегратор будує власний транслятор (розд. «Важливо!»).
- **Один активний конект** — паралельні звернення з різних кас неможливі; для наступного клієнта
  «повністю звільнити термінал і повторити хендшейк» (розд. «Архітектура роботи»).
- **Еталонна схема** (з емулятора каси ПриватБанк): конект → хендшейк (PingDevice) → дисконект →
  Identify → дисконект → **основний режим: тримати з'єднання ПОСТІЙНО (keepalive), не
  відключатись, реконект при обриві**. Аналогічна для USB/COM. Підтверджено референс-кодом
  `example.7z` (Go-лістенер: serial 115200 8N1 → буфер до `0x00` → `go reconnect()`).

### 3.3 Роль WebSocket

WebSocket у специфікації — **не транспорт термінала**, а приклад **власного middleware-шару
інтегратора поверх постійного сокета** (Go-обробник «з передаванням даних у websocket» для
мультиклієнтського доступу). Вендорські `genericDriverJson*.exe` — саме такий Go-міст: служба
Windows, тримає TCP:2000/COM до термінала, піднімає WS-сервер :3000 для 1С, окремий екземпляр на
касу. Існує тому, що *скрипт 1С не тримає фоновий сокет* (infostart). **Наша нативна компонента
цим не обмежена — фоновий reader-потік уже є в `DeviceSession`, тож EXE-міст/WS-сервер не
потрібні.**

### 3.4 Прикладний рівень

- Структура: `{method, step, params{}, error(bool), errorDescription}`. **Кореляція
  запит/відповідь — за полем `method`** (окремого id немає; відповідь ехом дублює `method`+`step`).
  `step=0` у однопрохідних, зарезервовано для багатопрохідних. (розд. «4. Прикладний рівень»)
- **Хендшейк:** `{"method":"PingDevice","step":0}` — **особливість: провідний `0x00` НА ПОЧАТКУ**
  (`00 7b...7d 00`); усі інші кадри — `0x00` лише в кінці. Після відповіді таймаут 1с (Verifone
  3-5с). Далі `Identify` (`ServiceMessage`/`msgType:identify`) → `vendor`/`model`.
- **3 логічні потоки в 1 конекті:** (1) поточна операція; (2) асинхронні `ServiceMessage` —
  будь-коли, навіть під час іншого методу; (3) `ServiceMessage Busy` (deviceBusy) у відповідь на
  конкуруючий несервісний запит. `deviceBusy` — unsolicited, відповіді не потребує.
- **Пауза на рішення каси (`correctTransaction`):** базовий Purchase/Cashback → полінг
  `getLastStatMsgCode` (0.5-1с); **код `11`** → `getDiscountName` (discountName/pan/posEntryMode/
  hash) → каса вирішує → `correctTransaction{amount,discount}` (картку/PIN повторно не запитують)
  → `correctionTransmitted` у межах Adjust Timeout; інакше продаж триває з початковою сумою.
- **Interrupt:** `msgType:interrupt` → `interruptTransmitted` + відповідь активної транзакції з
  `responseCode 1001`.
- **Статус-коди `getLastStatMsgCode`:** `0` спокій (завершено/не почато) … `1` card read,
  `3` authorization, `4` waiting cashier, `5` printing, `6` pin, `9` waiting card, `10` in
  progress, `11` correct transaction. `getLastResult`: `0` успіх / `2` in progress.
- **Помилки:** `responseCode≥1000` → скорочена відповідь (1000 general, 1001 canceled, 1002 EMV
  decline, 1003 log full, 1004 no host, 1005 no paper, 1006 crypto keys, 1007 no reader, 1008
  already complete); `0010` = Partial approval (`error:false`, каса вирішує доплату).
- **Автопідтвердження** завжди (окремий `confirm` не слати). Відновлення після обриву: якщо каса
  не отримала відповідь, а термінал завершив (`getLastStatMsgCode=0`) — тягнути результат через
  `GetReceiptInfo(invoiceNumber)`.
- **Нове в 1.0.3.5** (лише прикладний рівень): поле `adv` (JSON-рядок `{er,natr,rid}` для
  нац./Е-чека), `invoiceNumber` трактувати як String/long64. Діф 1.0.3.2-1.0.3.5 — на рівні полів
  окремих операцій, транспорту не зачіпає.

### 3.5 Каталог операцій

- **Фінансові (primary):** `Purchase{amount,discount,merchantId,facepay,subMerchant}`,
  `Refund{amount,discount,merchantId,rrn}`, `Withdrawal(скасування){invoiceNumber}`,
  `WithdrawalPartly{amount,invoiceNumber}`, `Cashback{amount,amountCash,merchantId}`,
  `Preauthorization{amount,merchantId}`, `SaleCompletion{amount,addamount,approvalCode,rrn}`.
- **Сервісні/звітні:** `CheckConnection`, `PingDevice`, `GetTerminalInfo`, `GetBalance`,
  `Audit{merchantId,getTotals}`, `Verify`/`VerifyCopy`, `PrintReceiptNum`, `PrintBatchJournal`,
  `GetReceiptInfo`, `ReadCardDiscount`, `GetPhoneNumber`, `GetOTPpassword`.
- **Іменні сервіси розстрочки/PbP** (merchantId/service ID хардкодом, Табл.1.1),
  `ServiceGeneric{amount,param,srvNum,merchantId}`, **бонусні картки** (`prompt.line`, 3DES
  pinblock).

## 4. Цільова архітектура

```
1С:Підприємство
   │ tVariant / IComponentBase / ExternalEvent
┌──▼──────────────────────────────────────────────────────────────┐
│ ЯДРО 1С (Є)   AddInNative · EventBridge · Ret() · ParamSpec       │
│               · REGISTER_COMPONENT                                │
├──────────────────────────────────────────────────────────────────┤
│ ПЛАТФОРМА (НОВЕ)   OperationRegistry · JobEngine · ResultEnvelope │
├──────────────────────────────────────────────────────────────────┤
│ ДРАЙВЕР (НОВЕ)     EcrPrivatJsonDriver                            │
│                    ├─ EcrJsonCodec (Build/Parse JSON)            │
│                    └─ EcrPrivatJsonClassifier : IFrameClassifier │
├──────────────────────────────────────────────────────────────────┤
│ WIRE-СТЕК (Є)      DeviceSession · NullTerminatedFramer          │
│                    · TransportTCP · TransportCOM                 │
└──────────────────────────────────────────────────────────────────┘
              ▲ e2e-тест: TerminalEmulator (TCP, localhost) — НОВЕ
```

Принцип: **уся Privat-специфіка ізольована в драйвері** (`EcrJsonCodec` +
`EcrPrivatJsonClassifier`); `DeviceSession` та транспорти лишаються загальними й
перевикористовними наступними драйверами.

## 5. Шлях приймання і кореляція (структурно вбиває баг «два буфери»)

Єдиний шлях, без власного буфера драйвера:

```
Transport.OnData → DeviceSession::OnBytes → framer_(NullTerminatedFramer).Feed → кадри[]
   → classifier_(EcrPrivatJsonClassifier).Classify(PendingView{primary,service}, frame)
      → PrimaryResponse   → pendingPrimary_.result
      → ServiceResponse   → pendingService_.result
      → RejectPrimary(Busy/Unsupported)  → RequestStatus::Busy/Unsupported на primary
      → RejectService(...)               → …на service
      → RejectBoth                       → desync (§7 wire-дизайну)
      → Unsolicited       → unsolicitedHandler_ (dispatcher-потік)
```

Драйвер лише формує `RequestPrimary(payload, timeoutMs, FrameOptions)` /
`RequestService(...)` і читає `RequestResult{status, frame}`. Власного приймання/буфера **не має**.

**`EcrPrivatJsonClassifier::Classify`** (уся логіка розрізнення потоків):
1. Розпарсити `method` вхідного кадру.
2. Якщо `method` збігається з `method` у `pending.primary` → `PrimaryResponse`.
3. Якщо `method=="ServiceMessage"`:
   - `params.msgType=="deviceBusy"` → `RejectPrimary{Busy}` (термінал зайнятий, primary
     відхилено; політика повтору — у драйвері);
   - `msgType` відповідає активному service-запиту (identify / getLastStatMsgCode /
     getDiscountName / …) → `ServiceResponse`;
   - `msgType` ∈ {`interruptTransmitted`, `correctionTransmitted`} → `Unsolicited`
     (сигнал стану для JobEngine);
   - `msgType=="methodNotImplemented"` → `RejectService/RejectPrimary{Unsupported}` за контекстом.
4. Якщо `method` збігається з `pending.service` → `ServiceResponse`.
5. Інакше (немає відповідного pending) → `Unsolicited`.

Двохдоріжкова модель `DeviceSession` прямо підтримує **полінг статусу під час операції**:
`Purchase` займає primary-доріжку, а `getLastStatMsgCode` летить по service-доріжці паралельно.

## 6. Життєвий цикл з'єднання

`EcrPrivatJsonDriver::Connect(connString)`:
1. Зібрати `DeviceSession(transport, NullTerminatedFramer, EcrPrivatJsonClassifier, SessionConfig)`;
   `transport` — `TransportTCP(host, 2000)` або `TransportCOM(port, 115200, 8, 'N', 1)` за
   рядком підключення (`"tcp://host:2000"` | `"COM3:115200,8,N,1"`).
2. `Start()` (підписка колбеків + reader + dispatcher + Open у межах `connectDeadlineMs`).
3. **Хендшейк:** `RequestPrimary(PingDevice, opts{leadingDelimiter=true})` → перевірити відповідь
   → пауза 1с (конфіг; Verifone 3-5с).
4. **Identify:** `RequestService(ServiceMessage/identify)` → зберегти `vendor`/`model` (можливе
   розгалуження поведінки за вендором, напр. таймаути Verifone).
5. **Основний режим:** з'єднання лишається відкритим; **keepalive** — драйверний періодичний
   `RequestService(CheckConnection|PingDevice)` (період — конфіг, дефолт напр. 20с);
   реконект делеговано супервізору `DeviceSession` (`autoReconnect`, backoff).
6. Після протокольного відновлення транзакції драйвер знімає desync через `MarkSynchronized()`.

## 7. Модель операцій (платформа)

- **`OperationRegistry`** — кожна операція описується один раз: імена en/ru, `ParamSpec[]`
  (тип, обов'язковість, дефолт), таймаут, прапорець «підтримує паузу на рішення каси», обробник.
  З опису — авто-реєстрація типізованого 1С-методу (синхронна + асинхронна форма) і валідація
  параметрів у ядрі (порожній обов'язковий → `AddError` з ім'ям параметра, без виклику обробника).
- **`JobEngine`** — виконавець операції на одному worker-потоці екземпляра драйвера. Стани:
  `Idle → Running → [AwaitingCashDecision] → Done | Error`.
  - синхронно: `Оплата(...)` = start + wait;
  - асинхронно: `НачатьОплату(...)` → `ПолучитьСостояние()`/`ПолучитьСтатусОперации()`
    (полінг з `ОбработчикОжидания`) → `ПолучитьРезультат()`; опційно подія через `EventBridge`;
  - пауза: у `AwaitingCashDecision` — `ПодтвердитьОперацию()`/`СкорректироватьСумму()`/
    `ОтклонитьОперацию()`.
  Keepalive і полінг `getLastStatMsgCode` — **на рівні драйвера**, не JobEngine (JobEngine —
  загальна машина станів, не знає протоколу).
- **`ResultEnvelope`** — `{ok:bool, code:string, description:string, payload:json}`. У 1С: функція
  повертає `ok`, деталі — `ПолучитьРезультатJSON()`/типізовані геттери.

**Флоу Purchase з паузою:** JobEngine `Running` → драйвер полить `getLastStatMsgCode` (service,
0.5-1с) → код `11` → JobEngine `AwaitingCashDecision`, драйвер тягне `getDiscountName` → 1С
викликає `СкорректироватьСумму`/`Подтвердить` → драйвер шле `correctTransaction` → чекає
`correctionTransmitted` (Adjust Timeout) → `Done`. `interrupt` → `ПрерватьОперацию` (responseCode
1001). `deviceBusy` → помилка/повтор за політикою.

## 8. Операції зрізу vs повний каталог

**Вертикальний зріз (зелений найперше):** `Connect`/`Disconnect`, `PingDevice`,
`Identify`/`GetTerminalInfo`, `Purchase`, `Refund`, `CheckConnection`, `GetReceiptInfo`,
плюс наскрізний механізм паузи (`getLastStatMsgCode`→`11`→`correctTransaction`), `interrupt`,
`deviceBusy`. Цей набір вправляє **всі** платформені механізми (реєстр, JobEngine, пауза,
класифікатор, обидві доріжки).

**Інкрементально після зрізу** (уже без нових механізмів, лише описи операцій + парсинг полів):
`Withdrawal[Partly]`, `Cashback`, `Preauthorization`, `SaleCompletion`, `GetBalance`,
`Audit`, `Verify`/`VerifyCopy`, друк, сервіси розстрочки/`ServiceGeneric`, бонусні картки,
`GetPhoneNumber`/`GetOTPpassword`, поле `adv`.

## 9. Обробка помилок

- `error(bool)` + `errorDescription`; `responseCode≥1000` → скорочена відповідь (мапа §3.4);
  `0010` → Partial approval (`error:false`).
- Уніфікація в `ResultEnvelope`; у 1С — `REPORT_ERROR` (лог + `AddError`) → `return false`;
  винятки межу 1С не перетинають (`try-catch` з `REPORT_ERROR` у `catch`).
- Статуси транспорту/сесії (`RequestStatus`: Timeout/Disconnected/SendFailed/Busy/Desynchronized)
  мапляться на коди `ResultEnvelope` з людиночитним описом.

## 10. Конвенції 1С-API

Успадкувати `AddInNative`; реєстрація через `REGISTER_COMPONENT(u"ECRPrivatJSON",
AddinECRPrivatJSON)`. Методи — з `OperationRegistry` (пари en/ru, синхронна+асинхронна форма).
Результат — через `Ret()` для value-хендлерів або явний `this->result` + геттери
`ResultEnvelope`. Події зі зміни стану операції — з worker-потоку **лише** через `EventBridge`
(`PostExternalEvent`), результати методів і `AddError` — лише на потоці виклику 1С.
`EnableLogging`/`ИспользоватьЛогирование` — успадковані, реєструвати не треба.

## 11. Тестова стратегія (емулятор-only)

- **L0.6 (юніт, розширити наявний `wire_selftest`)** — `EcrPrivatJsonClassifier` і `EcrJsonCodec`
  на детермінованих кадрах: method-кореляція, `deviceBusy`, `interruptTransmitted`,
  `correctionTransmitted`, `methodNotImplemented`, провідний `0x00` хендшейку, split/coalesce
  кадрів у одному читанні.
- **L1/L2 (інтеграція, новий `TerminalEmulator`)** — TCP-сервер на localhost, що приймає
  `JSON+0x00`, зіставляє за `method` і віддає **скриптовані** відповіді за байтовими прикладами
  спеки: `PingDevice`(з провідним 0x00)/`Identify`(→PAX s800)/`Purchase` з полінгом
  `getLastStatMsgCode`/`deviceBusy`/`correctTransaction=11`/помилки 1000-1008/`interrupt`→1001.
  Наявні `RawTcpEchoServer`/`ReopenTcpEchoServer` (сирий echo) замінюємо цим протокол-обізнаним
  емулятором. Джерело фікстур — байтові приклади спеки + референс `example.7z`.
- **L3 (компонента)** — драйвер через 1С-фасад проти емулятора (за зразком `native_host` UAPKI).
- Інтеграція в `run_tests.ps1` рівнями L0-L3; гейт дивиться exit-код (задокументовані `[SKIP]` —
  напр. COM-roundtrip без com0com — не FAIL).

Тестова політика (§2.2 архдоку): жодних тестів проти вигаданих форматів — вектори лише з
байтових прикладів специфікації або реальних дампів.

## 12. Нові файли й CMake

```
src/platform/OperationRegistry.{h,cpp}
src/platform/JobEngine.{h,cpp}
src/platform/ResultEnvelope.{h,cpp}
src/drivers/ecr_privatjson/EcrJsonCodec.{h,cpp}
src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.{h,cpp}
src/drivers/ecr_privatjson/EcrPrivatJsonDriver.{h,cpp}
src/components/AddinECRPrivatJSON.{h,cpp}
tests/support/TerminalEmulator.{h,cpp}     # протокол-обізнаний TCP-емулятор
tests/ecr_privatjson_selftest.cpp          # L1/L2/L3 драйвера (або в межах wire_selftest)
```

`CMake/components.cmake` — нові OBJECT-бібліотеки `platform_component`,
`driver_ecr_privatjson_component`, файли фасаду до `HEADER_FILES`/`SOURCE_FILES`,
`$<TARGET_OBJECTS:...>` до фінальної SHARED-цілі; `add_dependencies` (мінімум
`base_component wire_component transport_component spdlog`). `tests/CMakeLists.txt` — емулятор і
селф-тест лінкуються НЕ в головну DLL (за зразком наявних селф-тестів).

## 13. Етапи виконання (вертикальний зріз; кожен крок — збірка зелена)

1. **Платформа-каркас:** `ResultEnvelope`, кістяк `OperationRegistry` і `JobEngine` (стани,
   worker, sync+async форми) — юніт-тести машини станів на моках.
2. **Кодек+класифікатор:** `EcrJsonCodec` (Build/Parse), `EcrPrivatJsonClassifier` — L0.6-вектори.
3. **Емулятор термінала:** `TerminalEmulator` зі сценаріями Ping/Identify/GetTerminalInfo.
4. **Драйвер — транспортний e2e:** `EcrPrivatJsonDriver.Connect` (handshake→Identify→keepalive)
   проти емулятора по TCP — перший зелений e2e.
5. **Драйвер — Purchase з паузою:** Purchase/Refund через JobEngine, полінг `getLastStatMsgCode`,
   `correctTransaction=11`, `interrupt`, `deviceBusy`, `GetReceiptInfo`.
6. **1С-фасад:** `AddinECRPrivatJSON` (пари en/ru, sync+async, EventBridge), L3 проти емулятора.
7. **Інтеграція в `run_tests.ps1`** (L0-L3) + інкрементальне доповнення каталогу операцій (§8).

## 14. Поза обсягом

- **WS-клієнт-транспорт** — пізніше окремим кроком (`TransportWSClient` лишається в кодовій базі);
  потрібен, щоб під'єднатись **клієнтом** до чужого транслятора, не для сервера в DLL.
- **Брокер-сервіс мультидоступу** — лише якщо ділити один термінал між кількома касами; тоді
  **окремий CLI-процес поза головною DLL** (не in-DLL WSServer, який видалено), може перевикористати
  wire-стек. За замовчуванням не робимо (пілот монопольний).
- **Реальне залізо** — фінальна e2e-верифікація на фізичному терміналі; окрема майбутня задача.
- **spdlog→власний Logger** (Етап 3 «дієта») — не блокує пілот.
- Драйвери BPOS1/POSAPI — після здобуття wire-специфікацій (§3.4 архдоку).

## 15. Ризики та відкриті питання

| Питання | Дефолт / підхід |
|---|---|
| Період keepalive не заданий спекою | Драйверний періодичний `CheckConnection`/`PingDevice`, конфіг, дефолт ~20с |
| Глобальний таймаут операції (~120с зі старої задачі) у 1.0.3.5 явно не знайдено | Пер-операційні таймаути конфігуровані; дефолти консервативні; звірити з ENG-PDF/терміналом |
| Adjust Timeout для `correctionTransmitted` числом не заданий | Конфіг з безпечним дефолтом; уточнити на залізі |
| Максимальний розмір кадру/JSON не зазначено | `SessionConfig::maxBufferedBytes` — безпечний ліміт емпірично |
| `TransportCOM` дефолт baud = 9600 | Драйвер явно передає `115200` у конструктор |
| Split/coalesce на TCP, провідний `0x00` хендшейку | Закласти тест-вектори в емулятор; провідний `0x00` = порожня дейтаграма, не спричиняє desync (перевірено в `NullTerminatedFramer`) |
| COM-автотести потребують пари com0com | Наразі ручний `[SKIP]`-смоук; автоматизація в CI — відкрито |
| Цільові моделі/прошивки терміналів (підтримка `adv`/`getTotals`/`terminalId`) | Будуємо під 1.0.3.5; сумісність зі старшими прошивками — на рівні опційного парсингу полів |
| Реальний термінал для фінальної e2e | Емулятор-only поки; архітектура дозволяє пізню верифікацію без переробки |

## Додаток A. Байтові тест-вектори зі спеки (фікстури емулятора)

- **Хендшейк:** запит `00 7b 22 6d 65 74 68 6f 64 22 3a 22 50 69 6e 67 44 65 76 69 63 65 22 2c 22
  73 74 65 70 22 3a 30 7d 00` (`{"method":"PingDevice","step":0}` з провідним і кінцевим `0x00`);
  відповідь `{"method":"PingDevice","step":0,"params":{"code":"00","responseCode":"0000"},
  "error":false,"errorDescription":""}` + `0x00`.
- **Identify:** запит `{"method":"ServiceMessage","step":0,"params":{"msgType":"identify"}}` →
  відповідь з `result`/`vendor`(PAX)/`model`(s800).
- **GetTerminalInfo:** `{"method":"GetTerminalInfo","step":0}` → повний `[]byte` + NULL-terminated
  варіант (базовий приклад кодування UTF-8→байти).
- **deviceBusy** (unsolicited): `{"method":"ServiceMessage","step":0,"params":{"msgType":
  "deviceBusy"},"error":false,"errorDescription":""}`.
- **interrupt→скасування:** `{"error":true,"errorDescription":"Oперація скасов.","method":
  "Purchase","params":{"responseCode":"1001"},"step":0}`.
- Джерело: `ECR протокол ПриватБанк (JSON based) 1.0.3.5` + референс `example.7z` (C#/Go).
