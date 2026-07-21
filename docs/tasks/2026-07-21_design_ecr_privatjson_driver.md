# Пілотний драйвер ECRPrivatJSON — дизайн-специфікація

*Дата: 2026-07-21. Версія: **v2** (після Codex-аудиту: 5 critical + 14 major верифіковано по коду/спеці й враховано). Статус: затверджено напрям (4 розвилки вирішено з користувачем), специфікація на рев'ю.*

> **Прогрес:** §13 кроки 1-7 — **✅ реалізовано повністю** (гілка `design-ecr-privatjson`).
> Частина 1 (кроки 1-4: платформа-каркас + wire-спина до першого transport-e2e) — план/звіт
> [`2026-07-21_plan_ecr_privatjson_p1_foundation.md`](2026-07-21_plan_ecr_privatjson_p1_foundation.md).
> Частина 2 (кроки 5-7: операції Purchase/Refund/… + `JobEngine`/poller/interrupt/async + 1С-фасад
> `AddinECRPrivatJSON` (компонента `ECRPrivatJSON`) + інтеграція L0.7/L2-ecr у `run_tests`) —
> план/звіт [`2026-07-21_plan_ecr_privatjson_p2_operations_and_1c.md`](2026-07-21_plan_ecr_privatjson_p2_operations_and_1c.md).

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
| **Схема конекту (A7)** | Еталонна схема буквально: connect→Ping→dc→connect→Identify→dc→connect→keepalive | Спека: ця схема «гарантує роботу з терміналами будь-яких вендорів»; відхилення «не гарантуються» |

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
- **Дві категорії методів (визначає доріжку й поведінку `deviceBusy`):** (а) **справжні
  `ServiceMessage`** — `method:"ServiceMessage"` + `msgType` (identify, getLastStatMsgCode,
  getDiscountName, interrupt, correctTransaction, getMerchantList, getMaskList, debug); ЛИШЕ вони
  асинхронні й дозволені під час іншої операції; (б) **несервісні методи** — власне `method`
  (PingDevice, CheckConnection, GetTerminalInfo, Purchase, Refund, GetReceiptInfo, …); їх НЕ можна
  слати під час активної операції.
- **3 логічні потоки в 1 конекті:** (1) поточна операція; (2) асинхронні `ServiceMessage` —
  будь-коли, навіть під час іншого методу; (3) `deviceBusy` — **відповідь-відмова термінала на
  конкуруючий НЕсервісний запит**, коли він зайнятий. Це НЕ «unsolicited» у строгому сенсі: корелює
  із запитом, що його спричинив; окремої відповіді від каси не потребує; генерується лише терміналом.
- **Пауза на рішення каси (correctTransaction):** базовий Purchase/Cashback → полінг
  `getLastStatMsgCode` (0.5-1с); **коли `getLastStatMsgCode==11`** («correct transaction») →
  `getDiscountName` (discountName/pan/posEntryMode/hash) → каса вирішує → сервісний
  `correctTransaction{amount,discount}` (картку/PIN повторно не запитують) → термінал шле
  `correctionTransmitted` (ack корекції) у межах Adjust Timeout; **операція завершується ФІНАЛЬНОЮ
  primary-відповіддю Purchase/Cashback, а не `correctionTransmitted`** — після корекції продаж
  продовжується. Інакше продаж триває з початковою сумою.
- **Interrupt:** сервісний `msgType:interrupt` → термінал шле `interruptTransmitted` (ack) І
  фінальну відповідь активної транзакції з `responseCode 1001`.
- **Статус-коди `getLastStatMsgCode`:** `0` спокій (завершено/не почато), `1` card read,
  `2` chip card used, `3` authorization, `4` waiting cashier, `5` printing, `6` pin,
  `7` card removed, `8` EMV multi aid's, `9` waiting card, `10` in progress, `11` correct
  transaction. `getLastResult`: `0` успіх / `2` in progress.
- **Помилки:** `responseCode≥1000` → скорочена відповідь (1000 general, 1001 canceled, 1002 EMV
  decline, 1003 log full, 1004 no host, 1005 no paper, 1006 crypto keys, 1007 no reader, 1008
  already complete). **Partial approval (`responseCode 0010`, `error:false`)** — часткове схвалення
  суми; каса обирає ГІЛКУ: покупка в межах схваленої суми (+ доплата готівкою); коригування
  (зменшення) суми; доплата іншою карткою (новий `Purchase`); або скасування
  (`Withdrawal`/`WithdrawalPartly`). Потребує окремого стану JobEngine (§7).
- **Автопідтвердження** завжди (окремий `confirm` не слати). Відновлення після обриву: якщо каса
  не отримала відповідь, а термінал завершив (`getLastStatMsgCode=0`) — тягнути результат через
  `GetReceiptInfo(invoiceNumber)`.
- **Нове в 1.0.3.5** (лише прикладний рівень): поле `adv` (JSON-рядок `{er,natr,rid}` для
  нац./Е-чека), `invoiceNumber` трактувати як String/long64. Діф 1.0.3.2-1.0.3.5 — на рівні полів
  окремих операцій, транспорту не зачіпає.

### 3.5 Каталог операцій

Категоризація за доріжкою (визначає `RequestPrimary` vs `RequestService` — див. §5, §7):

- **Несервісні методи → primary-доріжка** (власне `method`; блокують термінал):
  - фінансові: `Purchase{amount,discount,merchantId,facepay,subMerchant}`,
    `Refund{amount,discount,merchantId,rrn,subMerchant}`, `Withdrawal(скасування){invoiceNumber}`,
    `WithdrawalPartly{amount,invoiceNumber}`, `Cashback{amount,amountCash,merchantId,subMerchant}`,
    `Preauthorization{amount,merchantId}`, `SaleCompletion{amount,addamount,approvalCode,rrn}`;
  - звітні/інфо: `CheckConnection`, `PingDevice`, `GetTerminalInfo`, `GetBalance`,
    `Audit{merchantId,getTotals}`, `Verify`/`VerifyCopy`, `PrintReceiptNum`, `PrintBatchJournal`,
    `GetReceiptInfo`, `ReadCardDiscount`, `GetPhoneNumber`, `GetOTPpassword`;
  - іменні сервіси розстрочки/PbP (`ServiceRefund`, `ServicePbP`, `ServiceRefPbP`, …; merchantId/
    service ID хардкодом, Табл.1.1), `ServiceGeneric{amount,param,srvNum,merchantId}`, бонусні
    картки (`ReadBonusCard`/`GetPinBonusCard`/`PinChangeBonusCard`; `prompt.line`, 3DES pinblock).
- **`ServiceMessage` (msgType) → service-доріжка** (асинхронні, можуть іти під час primary):
  `identify`, `getLastStatMsgCode`/`getLastStatMsgDescription`, `getDiscountName`,
  `correctTransaction`, `interrupt`, `getMerchantList`, `getMaskList`, `debug`; unsolicited від
  термінала: `deviceBusy`, `interruptTransmitted`, `correctionTransmitted`, `methodNotImplemented`.

## 4. Цільова архітектура

```
1С:Підприємство
   │ tVariant / IComponentBase / ExternalEvent
┌──▼──────────────────────────────────────────────────────────────┐
│ ЯДРО 1С (Є)   AddInNative · PostExternalEvent · Ret() · ParamSpec │
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
SetDataReceivedCallback → DeviceSession::OnBytes → framer_(NullTerminatedFramer).Feed → кадри[]
   → classifier_->Classify(PendingView{primary,service}, frame) → Classification{cls, reason}
      FrameClass::PrimaryResponse                           → pendingPrimary_.result
      FrameClass::ServiceResponse                           → pendingService_.result
      FrameClass::RejectPrimary (reason Busy|Unsupported)   → RequestStatus Busy|Unsupported (primary)
      FrameClass::RejectService (reason ...)                → …(service)
      FrameClass::RejectBoth                                → desync (§7 wire-дизайну)
      FrameClass::Unsolicited                               → unsolicitedHandler_ (dispatcher-потік)
```

Драйвер лише формує `RequestPrimary(payload, timeoutMs, FrameOptions)` /
`RequestService(...)` і читає `RequestResult{status, frame}`. Власного приймання/буфера **не має**.
`Classify` повертає `Classification{FrameClass cls; RejectReason reason;}` (реальні типи з
`IFrameClassifier.h`), а не суміщений enum.

**`EcrPrivatJsonClassifier::Classify`** (уся логіка розрізнення потоків):
1. Розпарсити `method` (і `params.msgType`, якщо `method=="ServiceMessage"`).
2. `method` == `method` у `pending.primary` (не-`ServiceMessage`) → `{PrimaryResponse}`.
3. `method=="ServiceMessage"`:
   - `msgType` корелює з активним service-запитом — **у т.ч. `interrupt`→`interruptTransmitted`,
     `correctTransaction`→`correctionTransmitted`** (це ВІДПОВІДІ на service-запити, НЕ unsolicited),
     а також identify / getLastStatMsgCode / getDiscountName → `{ServiceResponse}`;
   - `msgType=="deviceBusy"`: якщо активний primary → `{RejectPrimary, Busy}` (політика повтору —
     у драйвері); якщо primary немає → `{Unsolicited}`;
   - `msgType=="methodNotImplemented"`: кадр НЕ називає відхилений метод, тож при обох активних
     pending → `{RejectBoth}`; при одній доріжці → `{RejectService|RejectPrimary, Unsupported}`;
   - інший `msgType` без відповідного pending → `{Unsolicited}`.
4. `method` == `method` у `pending.service` → `{ServiceResponse}`.
5. Інакше → `{Unsolicited}`.

Двохдоріжкова модель `DeviceSession` прямо підтримує **полінг статусу під час операції**: `Purchase`
займає primary-доріжку, а `getLastStatMsgCode` (справжній `ServiceMessage`) летить по service-доріжці
паралельно й НЕ провокує `deviceBusy` — на відміну від несервісних `PingDevice`/`CheckConnection`.

## 6. Життєвий цикл з'єднання

Драйвер реалізує **еталонну схему буквально** (рішення A7), перевикористовуючи `DeviceSession`
короткими сесіями для хендшейку/Identify і однією постійною для основного режиму. Транспорт:
`TransportTCP(host, 2000)` або `TransportCOM(port, 115200, 8, 'N', 1.0f)` — драйвер **явно** передає
baud `115200` (дефолт `TransportCOM` — 9600). Рядок: `"tcp://host:2000"` | `"COM3:115200,8,N,1"`.

`EcrPrivatJsonDriver::Connect(connString)`:
1. **Хендшейк** (коротка сесія): зібрати `DeviceSession`, **спершу поставити user-колбеки**
   (`SetUnsolicitedHandler`/`SetConnectionStateHandler`/`SetWireTraceHandler` — сеттери дозволені
   ЛИШЕ до `Start()`; після — no-op+WARN), потім `Start()`. `RequestPrimary(pingPayload, timeoutMs,
   FrameOptions{/*leadingDelimiter=*/true})` → перевірити відповідь → пауза 1с (конфіг; Verifone
   3-5с) → **`Stop()` (дисконект)**.
2. **Identify** (коротка сесія): новий `DeviceSession`+колбеки+`Start()` → `RequestService(identify)`
   → зберегти `vendor`/`model` (можливе розгалуження за вендором) → **`Stop()` (дисконект)**.
3. **Основний режим** (постійна сесія): новий `DeviceSession`+колбеки+`Start()`; з'єднання лишається
   відкритим, дескриптор глобальний, не відключаємось; реконект делеговано супервізору
   `DeviceSession` (`autoReconnect`, backoff).
4. **Keepalive — ЛИШЕ в idle:** коли операції немає, драйверний таймер шле `PingDevice`/
   `CheckConnection` (несервісні, primary-доріжка) з періодом-конфігом (~20с). **Під час активної
   операції keepalive НЕ шлемо** — стан термінала бачимо з полінгу `getLastStatMsgCode` (service).
5. **Відновлення після desync:** primary-timeout САМ виставляє `desynchronized_`
   (`DeviceSession.cpp`); реконект його НЕ знімає — лише `MarkSynchronized()`; а primary під час
   desync заборонено кодом. Тому драйвер з'ясовує наслідок операції по **service-доріжці**
   (`getLastStatMsgCode` — не блокується desync; `0` = термінал завершив/у спокої), викликає
   `MarkSynchronized()`, і лише тоді відновлює primary (напр. `GetReceiptInfo(invoiceNumber)`).

## 7. Модель операцій (платформа)

- **`OperationRegistry`** — кожна операція описується один раз: імена en/ru, параметри, таймаут,
  прапорець «підтримує паузу на рішення каси», обробник → авто-реєстрація 1С-методу (синхронна +
  асинхронна форма) і валідація параметрів. **Примітка:** наявний `ParamSpec` (`AddInNative.h`) має
  лише `nameEn/nameRu/required/byDefault` — **без поля типу**; для типізованої валідації `ParamSpec`
  розширюється полем типу в межах цієї роботи (або тип перевіряється в обробнику).
- **`JobEngine`** — машина станів операції. Стани (розширено):
  `Idle → Running → [AwaitingCashDecision | AwaitingPartialDecision | Interrupting] → Done | Error`.
  Визначені також: таймаут рішення каси, обрив у стані очікування, повторний sync/async-виклик під
  час активного job (відхиляється як `Concurrent`).
  - синхронно: `Оплата(...)` = start + wait;
  - асинхронно: `НачатьОплату(...)` → `СостояниеОперации()` (полінг з `ОбработчикОжидания`) →
    `РезультатОперацииJSON()`; **as-built (Ч2): суто poll-based, без подій** — `PostExternalEvent`
    для стану операції не використовується (див. §10);
  - пауза: `ПодтвердитьОперацию()`/`СкорректироватьСумму()`/`ОтклонитьОперацию()` — **не реалізовано
    в Ч2** (див. `docs/tasks/2026-07-21_plan_ecr_privatjson_p2_operations_and_1c.md`, «Рішення для
    Частини 2», п.1-2).
- **Потокова модель (важливо):** `RequestPrimary` **блокує** свій потік до відповіді/таймауту
  (`DoRequest`→`cv_.wait_for`). Тому драйвер має **ДВА потоки**: (1) worker JobEngine, що тримає
  активний `RequestPrimary` операції; (2) окремий **poller/timer-потік**, який під час `Running`
  полить `getLastStatMsgCode` через `RequestService` (0.5-1с), а в idle шле keepalive.
- **Send-арбітр (0.1с):** спека забороняє слати дві команди одночасно (пауза 0.1с). Драйвер
  серіалізує ФАКТИЧНІ відправлення обох доріжок через власний send-gate з мінімальним інтервалом
  0.1с (бо `DeviceSession` серіалізує лише в межах доріжки, не між ними).
- **`ResultEnvelope`** — `{ok:bool, code:string, description:string, payload:json}`. У 1С: функція
  повертає `ok`, деталі — `ПолучитьРезультатJSON()`/геттери.

**Флоу Purchase з паузою (історичний дизайн, НЕ реалізовано в Ч2):** worker шле
`RequestPrimary(Purchase)` і блокується; poller полить `getLastStatMsgCode` (service) 0.5-1с. Код
`11` → `AwaitingCashDecision`, драйвер тягне `getDiscountName` → 1С викликає
`СкорректироватьСумму`/`Подтвердить` → драйвер шле сервісний `correctTransaction` → приходить
`correctionTransmitted` (ServiceResponse, ack) → **операція завершується, коли повертається
ФІНАЛЬНА primary-відповідь Purchase** (не ack корекції). `interrupt` (service) →
`interruptTransmitted` (ack) + primary-відповідь `responseCode 1001` → `Done`. `deviceBusy` на
primary → повтор/помилка за політикою.
**As-built (Ч2):** пауза на рішення каси не реалізована (код `11` без корекції — продаж
продовжується штатно, спека); 1С не отримує подій зі зміни стану — вона **опитує стан**
(`СостояниеОперации`/`РезультатОперацииJSON`/`СтатусТерминала`), `PostExternalEvent` для цього
не використовується (§10).

## 8. Операції зрізу vs повний каталог

**Вертикальний зріз (зелений найперше):** `Connect`/`Disconnect`, `PingDevice`,
`Identify`/`GetTerminalInfo`, `Purchase`, `Refund`, `CheckConnection`, `GetReceiptInfo`,
плюс наскрізний механізм паузи (`getLastStatMsgCode`→`11`→`correctTransaction`), `interrupt`,
`deviceBusy`. Цей набір вправляє **всі** платформені механізми (реєстр, JobEngine, пауза,
класифікатор, обидві доріжки).

**Інкрементально після зрізу** (переважно описи операцій + парсинг полів): `Withdrawal[Partly]`,
`Cashback`, `Preauthorization`, `SaleCompletion`, `GetBalance`, `Audit`, `Verify`/`VerifyCopy`,
друк, сервіси розстрочки/`ServiceGeneric`, бонусні картки, `GetPhoneNumber`/`GetOTPpassword`,
поле `adv`. **Виняток — Partial approval** (потребує стану `AwaitingPartialDecision` і запуску
`WithdrawalPartly`/нового `Purchase`/`Withdrawal`) і **interrupt** (очікування ack + фінальної
1001) — це НЕ «без нових механізмів»: відповідні стани закладаємо в JobEngine ще у зрізі, а гілки
наповнюємо при додаванні операцій.

## 9. Обробка помилок

- `error(bool)` + `errorDescription`; `responseCode≥1000` → скорочена відповідь (мапа §3.4);
  `0010` → Partial approval (`error:false`).
- Уніфікація в `ResultEnvelope`; у 1С — `REPORT_ERROR` (лог + `AddError`) → `return false`;
  винятки межу 1С не перетинають (`try-catch` з `REPORT_ERROR` у `catch`).
- Усі `RequestStatus` мапляться на `ResultEnvelope` з людиночитним описом: `Response` → успіх;
  `Busy` (deviceBusy) → зайнято/повтор; `Unsupported` (methodNotImplemented) → метод не
  підтримується терміналом; `Timeout` → таймаут; `Disconnected`/`SendFailed` → помилка зв'язку
  (реконект); `Stopped` → перервано зупинкою; `Concurrent` → операція вже виконується;
  `Desynchronized` → потрібне відновлення (§6, п.5).

## 10. Конвенції 1С-API

Успадкувати `AddInNative`; реєстрація через `REGISTER_COMPONENT(u"ECRPrivatJSON",
AddinECRPrivatJSON)`. Методи — з `OperationRegistry` (пари en/ru, синхронна+асинхронна форма).
Результат — через `Ret()` для value-хендлерів або явний `this->result` + геттери
`ResultEnvelope`.
**As-built (Ч2): фасад poll-based, без подій.** 1С сама опитує стан операції методами
`OperationState`/`СостояниеОперации` (int — `JobState`), `OperationResult`/`РезультатОперацииJSON`
(рядок JSON `ResultEnvelope`) і `LastStatus`/`СтатусТерминала`; `AddInNative::PostExternalEvent`
для стану операції **не використовується** (у `src/components/AddinECRPrivatJSON.cpp` викликів
немає — перевірено grep'ом). Результати методів і `AddError` — лише на потоці виклику 1С.
`EnableTrace`/`ВключитьТрассировку` вмикає wire-трасування драйвера
(`DeviceSession::SetWireTraceHandler`), діє з наступного `Connect`.
`EnableLogging`/`ИспользоватьЛогирование` — успадковані, реєструвати не треба.

## 11. Тестова стратегія (емулятор-only)

- **L0.6 (юніт, розширити `wire_selftest`):**
  - **рівень framer** (`NullTerminatedFramer`): провідний `0x00` хендшейку (порожня дейтаграма),
    split/coalesce кадрів у одному читанні — це механіка `Feed`, а НЕ класифікатора;
  - **рівень класифікатора/кодека** (`EcrPrivatJsonClassifier`/`EcrJsonCodec`): method-кореляція,
    `deviceBusy`→`RejectPrimary` за наявного primary, `interruptTransmitted`/`correctionTransmitted`
    →`ServiceResponse`, `methodNotImplemented`→`RejectBoth` при обох pending.
- **L1/L2 (інтеграція, новий `TerminalEmulator`)** — TCP-сервер на localhost, що приймає
  `JSON+0x00`, зіставляє за `method` і віддає скриптовані відповіді: `PingDevice`(провідний 0x00)/
  `Identify`(→PAX s800)/`Purchase` з полінгом `getLastStatMsgCode`/при `==11`
  `correctTransaction`+`correctionTransmitted`+фінальна відповідь/`deviceBusy`/помилки 1000-1008/
  `interrupt`→1001/Partial approval `0010`. **`TerminalEmulator` ДОДАЄМО, наявні
  `RawTcpEchoServer`/`ReopenTcpEchoServer` НЕ чіпаємо** — вони тримають generic-регресії wire-стека
  (raw echo/split/reconnect).
- **L3 (компонента)** — драйвер через 1С-фасад проти емулятора (за зразком `native_host`). Ціль ECR
  **не залежить від UAPKI**, тож у `tests/CMakeLists.txt` її оголошуємо **ДО** гейта
  `if(NOT BUILD_WITH_UAPKI) return()` (він зараз відсікає лише `native_host`).
- Інтеграція в `run_tests.ps1` рівнями L0-L3; гейт дивиться exit-код (`[SKIP]` — напр. COM-roundtrip
  без com0com — не FAIL).
- **Матриця має покрити межові випадки дизайну:** both-pending `methodNotImplemented`, `deviceBusy`
  на service-доріжці, реконект+повторний хендшейк, desync-відновлення після primary-timeout, колбеки
  до `Start()`, завершення по фінальній primary-відповіді після `correctionTransmitted`, send-арбітр 0.1с.

Тестова політика (§2.2 архдоку): жодних форматів «з голови». Вектори — з байтових прикладів спеки;
де спека дає лише JSON-текст (`deviceBusy`, `correctTransaction`, помилки 1000-1008, Partial
approval) — **нормативне UTF-8-кодування цього JSON + `0x00`** (не вигадка формату) або реальні дампи.

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

`CMake/components.cmake` — **склад DLL іде ЛИШЕ через OBJECT-ліби + `$<TARGET_OBJECTS>`** (змінні
`HEADER_FILES`/`SOURCE_FILES` вестигіальні, у SHARED-ціль не входять — перевірено на `:242`). Тому:
- `add_library(platform_component OBJECT ...)`, `add_library(driver_ecr_privatjson_component OBJECT ...)`,
  `add_library(ecr_privatjson_facade_component OBJECT src/components/AddinECRPrivatJSON.cpp)` — і
  кожну додати як `$<TARGET_OBJECTS:...>` у `add_library(${TARGET} SHARED ...)` (за зразком
  `uapki_connect_component`); **інакше фасад не потрапить у DLL**;
- `add_dependencies(...)` — лише порядок збірки; **лінкування окремо**: `target_link_libraries` для
  зовнішніх (`spdlog::spdlog`, `nlohmann_json`, за потреби `ws2_32`);
- `tests/CMakeLists.txt` — `TerminalEmulator`+ECR-селф-тест окремими exe (НЕ в DLL), з явним
  переліком `$<TARGET_OBJECTS:...>` і лінкуванням (за зразком `wire_selftest`), оголошені ДО
  UAPKI-гейта (§11).

## 13. Етапи виконання (вертикальний зріз; кожен крок — збірка зелена)

1. **Платформа-каркас:** `ResultEnvelope`, кістяк `OperationRegistry` і `JobEngine` (стани
   Idle→Running→[AwaitingCashDecision/AwaitingPartialDecision/Interrupting]→Done|Error; worker +
   poller-потік; send-арбітр 0.1с) — юніт-тести машини станів на моках.
2. **Кодек+класифікатор:** `EcrJsonCodec` (Build/Parse), `EcrPrivatJsonClassifier` (кореляція,
   deviceBusy, `*Transmitted`→service, `methodNotImplemented`→RejectBoth) — L0.6-вектори.
3. **Емулятор термінала:** `TerminalEmulator` (додано, не замінює echo-сервери) зі сценаріями
   Ping/Identify/GetTerminalInfo.
4. **Драйвер — транспортний e2e:** `Connect` за **еталонною схемою** (Ping[+dc]→Identify[+dc]→
   постійний конект; колбеки до `Start()`) проти емулятора по TCP — перший зелений e2e.
5. **Драйвер — Purchase з паузою:** Purchase/Refund; poller полить `getLastStatMsgCode`; при `==11`
   — `getDiscountName`+`correctTransaction`+`correctionTransmitted`+фінальна відповідь; `interrupt`;
   `deviceBusy`; desync-відновлення через service+`GetReceiptInfo`.
6. **1С-фасад:** `AddinECRPrivatJSON` (пари en/ru, sync+async, poll-based стан операції — без
   `PostExternalEvent`; `EnableTrace` для wire-трасування), L3 проти емулятора.
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
| Схема конекту (A7) | **Вирішено:** еталонна буквально (Ping+dc→Identify+dc→постійний конект) |
| Період keepalive не заданий спекою | Драйверний `PingDevice`/`CheckConnection` **лише в idle**, конфіг, дефолт ~20с |
| Send-арбітр 0.1с між командами | Драйверний send-gate (мін. інтервал 0.1с) поверх обох доріжок |
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
- **deviceBusy** (відмова термінала на несервісний запит; ack не потрібен):
  `{"method":"ServiceMessage","step":0,"params":{"msgType":"deviceBusy"},"error":false,"errorDescription":""}`.
- **interrupt→скасування:** `{"error":true,"errorDescription":"Oперація скасов.","method":
  "Purchase","params":{"responseCode":"1001"},"step":0}`.
- Спека дає hex-дампи лише для Ping/Identify/GetTerminalInfo; решта (`deviceBusy`,
  `correctTransaction`/`correctionTransmitted`, помилки 1000-1008, Partial approval) — JSON-текст,
  який фікстури кодують у UTF-8+`0x00` нормативно.
- Джерело: `ECR протокол ПриватБанк (JSON based) 1.0.3.5` + референс `example.7z` (C#/Go).
