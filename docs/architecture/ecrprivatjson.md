# Архітектура драйвера ECRPrivatJSON (платіжний термінал ПриватБанк)

Пілотний драйвер обладнання поверх [фундаменту device-core](device-core.md). Реалізує
JSON-протокол платіжних терміналів ПриватБанку (COM/TCP) і виставляє в 1С компоненту
**`ECRPrivatJSON`**. Цей документ описує **внутрішню будову** драйвера — для подальшого
розвитку й додавання операцій. Хто пише **прикладний 1С-код** проти готового драйвера — читає
[docs/integration-1c/ecr-privatjson.md](../integration-1c/ecr-privatjson.md).

Специфікація протоколу — [docs/ECR_Privat_JSON_Protokol.md](../ECR_Privat_JSON_Protokol.md).
Імена звірені з кодом (`src/drivers/ecr_privatjson/*`, `src/components/AddinECRPrivatJSON.*`,
`src/platform/*`).

> **Історична примітка.** До 2026-07 у проєкті був інший драйвер `AddinECRPrivatJSON`
> (`src/protocols/` + `src/helpers/ECRPrivatJSON/`), **видалений як непрацездатний**
> (два непоєднані буфери прийому). Його заміняє **цей** драйвер поверх device-core. Опис
> старої реалізації лишився в git-історії; тут не повторюється.

---

## 1. Шари драйвера

```
1С:Підприємство
   │ tVariant / IComponentBase / ВнешнееСобытие
┌──▼──────────────────────────────────────────────────────────────┐
│ ФАСАД  AddinECRPrivatJSON  (компонента 1С «ECRPrivatJSON»)        │
│   тонка: реєструє методи EN/RU, делегує драйверу, серіалізує      │
│   ResultEnvelope → JSON-рядок; опційні події через PostExternalEvent│
├──────────────────────────────────────────────────────────────────┤
│ ДРАЙВЕР  EcrPrivatJsonDriver                                      │
│   Connect (еталонна схема) · операції sync/async · worker+poller  │
│   · send-арбітр 0.1с · desync-відновлення · MapResult             │
│   ├─ EcrJsonCodec              JSON ⇄ байти (без делімітера)       │
│   └─ EcrPrivatJsonClassifier : IFrameClassifier  кореляція кадрів  │
├──────────────────────────────────────────────────────────────────┤
│ ПЛАТФОРМА  ResultEnvelope · JobEngine        (src/platform/)      │
├──────────────────────────────────────────────────────────────────┤
│ DEVICE-CORE  DeviceSession · NullTerminatedFramer · TransportTCP/COM │
└──────────────────────────────────────────────────────────────────┘
      ▲ e2e-тест: TerminalEmulator (TCP, localhost)
```

Уся Privat-специфіка ізольована у двох класах — `EcrJsonCodec` і `EcrPrivatJsonClassifier`;
`DeviceSession` і транспорти лишаються загальними (їх перевикористає наступний драйвер).

---

## 2. Прикладний протокол (стисло)

Повна спека — окремо; для розуміння драйвера достатньо:

- **Кадр** = UTF-8 JSON + термінатор `0x00` (C-рядок). Хендшейк-`PingDevice` має ще й **провідний
  `0x00`** на початку. Кодуванням делімітера опікується `NullTerminatedFramer`, не драйвер.
- **Структура повідомлення:** `{method, step, params{}, error(bool), errorDescription}`. Кореляція
  запит/відповідь — **за полем `method`** (окремого id немає; відповідь ехом дублює `method`).
- **Дві категорії методів:**
  - **несервісні** (власне `method`: `PingDevice`, `CheckConnection`, `GetTerminalInfo`,
    `Purchase`, `Refund`, `GetReceiptInfo`, …) — «займають» термінал, ідуть **primary**-доріжкою,
    їх не можна слати під час активної операції;
  - **`ServiceMessage`** (з `params.msgType`: `identify`, `getLastStatMsgCode`, `interrupt`,
    `correctTransaction`, …) — легкі, дозволені **паралельно** з операцією, ідуть **service**-доріжкою.
- **`deviceBusy`** — відповідь-відмова термінала на конкурентний несервісний запит.
- **`getLastStatMsgCode`** — опитування стану термінала під час операції (коди 0–11, §6.2).
- **Скасування:** сервісний `interrupt` → термінал шле `interruptTransmitted` (ack) + фінальну
  primary-відповідь операції з `responseCode 1001`.

Топологія — **монопольний постійний доступ**: один активний конект, термінал слухає (сервер),
каса конектиться (клієнт), TCP-порт за замовч. `2000`, COM — `115200 8N1`.

---

## 3. Кодек — `EcrJsonCodec`

`src/drivers/ecr_privatjson/EcrJsonCodec.{h,cpp}`. Чистий, без стану; делімітером `0x00` **не
оперує** (це `NullTerminatedFramer`):

```cpp
static std::vector<uint8_t> BuildRequest(method, step, params);   // {method,step[,params]} → UTF-8 байти; params==nullptr → без поля
static ParsedResponse       Parse(frameNoDelimiter);              // повний розбір; невалідний JSON → {valid=false}, БЕЗ винятку
static bool                 PeekMethod(frame, method, msgType);   // легкий парс лише method (+ msgType для ServiceMessage)
```

`ParsedResponse` = `{method, step, params, error, errorDescription, msgType, valid}`.

---

## 4. Класифікатор — `EcrPrivatJsonClassifier`

`src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.{h,cpp}`, реалізує `IFrameClassifier`. Уся
логіка розрізнення потоків (спека §5). `Classify(pending, frame)`:

1. Розпарсити `method` (і `params.msgType`, якщо `method=="ServiceMessage"`).
2. `method` == `method` активного **primary** (не-`ServiceMessage`) → `{PrimaryResponse}`.
3. `method=="ServiceMessage"`:
   - `msgType` корелює з активним **service**-запитом (у т.ч. `interrupt`→`interruptTransmitted`,
     `correctTransaction`→`correctionTransmitted` — це **відповіді** на service-запити, НЕ
     unsolicited; а також `identify`/`getLastStatMsgCode`/`getDiscountName`) → `{ServiceResponse}`;
   - `msgType=="deviceBusy"`: є активний primary → `{RejectPrimary, Busy}`; немає → `{Unsolicited}`;
   - `msgType=="methodNotImplemented"`: кадр не називає відхилений метод, тож при обох pending →
     `{RejectBoth}` (→ desync); при одній доріжці → `{Reject*, Unsupported}`;
   - інший `msgType` без відповідного pending → `{Unsolicited}`.
4. `method` == `method` активного **service** → `{ServiceResponse}`.
5. Інакше → `{Unsolicited}`.

Двохдоріжкова модель прямо підтримує **полінг статусу під час операції**: `Purchase` займає
primary, а `getLastStatMsgCode` летить service-доріжкою й **не** провокує `deviceBusy`.

---

## 5. Життєвий цикл — `Connect`

`EcrPrivatJsonDriver::Connect(connString)` реалізує **еталонну схему буквально** (спека:
«гарантує роботу з терміналами будь-яких вендорів»), перевикористовуючи `DeviceSession` короткими
сесіями для хендшейку/Identify і однією постійною для основного режиму:

1. **Розбір рядка** (`ParseConnString`): `tcp://host:port` (IPv6-літерали не підтримуються) або
   `COMn[:baud[,8,N,1]]` (з рядка береться лише baud, дефолт `115200`; формат кадру завжди 8N1,
   хвіст після baud ігнорується). Скидання `vendor_`/`model_`, `job_.ResetToIdle()`.
2. **Хендшейк** (коротка сесія): `MakeSession` → колбеки (лише до `Start()`) → `Start()` →
   `RequestPrimary(PingDevice, FrameOptions{leadingDelimiter=true})` → перевірка → **`Stop()`
   (дисконект)** → пауза 1с.
3. **Identify** (коротка сесія): нова сесія → `RequestService(ServiceMessage/identify)` → зберегти
   `vendor`/`model` з `params` (best-effort) → **`Stop()` (дисконект)**.
4. **Постійний режим:** нова `session_`, лишається відкритою; реконект — супервізор `DeviceSession`
   (`autoReconnect`, backoff).

`MakeTransport`: TCP → `TransportTCP(host, port)`; COM → `TransportCOM(port, baud, 8, 'N', 1.0f)` —
драйвер **явно** передає baud (дефолт `TransportCOM` = 9600). `MakeSession` збирає
`DeviceSession(transport, NullTerminatedFramer, EcrPrivatJsonClassifier)`, ставить колбеки й
(якщо увімкнено) `SetWireTraceHandler` для wire-трасування.

`Disconnect()`: `job_.Join()` (дочекатись worker, щоб не рвати сесію під активним запитом) →
`session_->Stop()`.

---

## 6. Модель операцій

### 6.1. Потокова модель (важливо)

`RequestPrimary` **блокує** свій потік до відповіді/таймауту. Тому активна операція тримає **два
потоки**:

- **worker** (синхронний виклик 1С або worker `JobEngine`) — тримає `RequestPrimary(операція)`;
- **poller** — окремий потік, піднятий на час операції: раз на `kPollIntervalMs` (500мс)
  шле `getLastStatMsgCode` через `RequestService`, оновлює `lastStatus_`, і — якщо надійшов запит
  на скасування — раз надсилає `interrupt` (service). Poller — **єдиний власник service-доріжки**.

Poller піднімається/зупиняється всередині `ExecuteInternal` через **RAII-guard**: `PollerJoin`
у деструкторі виставляє `stop` і `join`-ить потік — навіть при винятку з `RequestPrimary`
(service-доріжка звільняється до desync-відновлення, `std::terminate` не станеться).

**Send-арбітр (`GateSend`, 0.1с):** спека забороняє слати дві команди одночасно. `DeviceSession`
серіалізує лише в межах доріжки, тож драйвер має власний send-gate з мінімальним інтервалом
`kSendGapMs` (100мс) поверх **обох** доріжок; кожне фактичне відправлення проходить через нього.

### 6.2. `getLastStatMsgCode` → `LastStatus`

Poller зберігає останній код у `lastStatus_` (атомік, `-1` якщо ще не було). Мапа кодів
(`StatusText`): `0` Готово/очікування, `1` Картку зчитано, `2` Чіп-картку, `3` Авторизація,
`4` Очікування касира, `5` Друк чека, `6` Введіть PIN, `7` Картку вилучено, `8` Оберіть застосунок,
`9` Вставте/піднесіть картку, `10` Виконується, `11` Коригування транзакції.

### 6.3. Синхронні операції

`Execute(method, params, timeoutMs)` → `ExecuteInternal`. Реалізовані:
`Purchase(amount)`, `Refund(amount, rrn)`, `CheckConnection()`, `GetReceiptInfo(invoiceNumber)`,
`Audit(merchantId="0")` (X-звіт, §5.17), `Verify(merchantId="0")` (Звірка/Загальний звіт, §5.18),
плюс `Execute("GetTerminalInfo", …)`.

**Обов'язкові поля params оплати/повернення** (`FillPaymentDefaults`, спека §5.1.1/§5.2.1):
`Purchase`/`StartPurchase` і `Refund`/`StartRefund` перед відправленням дозаповнюють
`discount:""`, `merchantId:"0"`, а Purchase — ще `facepay:"false"`. Реальний термінал
(Newland N950, інцидент 2026-08-29) без цих полів відбиває запит кодом `1000`
«Введіть discount» — еталонна каса ПриватБанк шле їх завжди. Дефолти не перетирають
значення, передані через `extra`; `subMerchant` свідомо НЕ додається (спека дозволяє
поле лише після реєстрації субмерчанта в банку). Регрес-тест — строгий емулятор у
`TestDriverStrictTerminalParams` (`ecr_privatjson_selftest`). Кожна: скид прапорців скасування (у **викликача**, не в
worker — інакше cancel одразу після Start губиться) → `job_.ResetToIdle()` → `ExecuteInternal`:
перевірка `IsConnected` → підняти poller (RAII) → `GateSend` → `RequestPrimary` → мапінг у
`ResultEnvelope`.

### 6.4. Асинхронний API (поверх `JobEngine`)

`StartOperation(method, params, timeoutMs)` (та обгортки `StartPurchase`/`StartRefund`) —
неблокуючий старт: `job_.Start([...]{ return ExecuteInternal(...); })`. 1С опитує:

- `OperationState()` → `JobState` (`Idle=0, Running=1, Interrupting=2, Done=3, Error=4`);
- `TryGetOperationResult(out)` → `true` при `Done/Error`;
- `CancelOperation()` = `RequestInterrupt()` (виставити прапорець для poller) + `job_.RequestCancel()`
  (гардовано `Running→Interrupting`).

**Poll-based, без обов'язкових подій:** стан операції 1С **опитує**, а не отримує подіями.

### 6.5. Мапінг результату — `MapResult`

`RequestStatus` → `ResultEnvelope`:

| `RequestStatus` | `ResultEnvelope` |
|---|---|
| `Response` | розбір кадру: `payload=params`, `code=responseCode` (або `ERROR`/`0000`), `ok=!error` |
| `Busy` | `Fail("DEVICE_BUSY", "Термінал зайнятий")` |
| `Unsupported` | `Fail("UNSUPPORTED", "Метод не підтримується терміналом")` |
| `Timeout` | `Fail("TIMEOUT", "Немає відповіді термінала")` |
| `Disconnected` | `Fail("DISCONNECTED", "Обрив зв'язку з терміналом")` |
| `SendFailed` | `Fail("SEND_FAILED", "Помилка відправки")` |
| `Stopped` | `Fail("STOPPED", "Операцію перервано")` |
| `Concurrent` | `Fail("CONCURRENT", "Операція вже виконується")` |
| `Desynchronized` | `Fail("DESYNC", "Потрібне відновлення зв'язку")` |
| невалідний кадр | `Fail("BAD_RESPONSE", "Невалідна відповідь термінала")` |

`ok` визначає прапорець `error` термінала (спека: Partial approval `0010` приходить із `error:false`
— теж `ok:true`).

### 6.6. Відновлення після desync

Якщо `RequestPrimary` завершився `Timeout` **і** `session_->IsDesynchronized()` (і не
`inRecovery_`) — `RecoverAfterDesync`: полить `getLastStatMsgCode` **доки код != "0"** (bounded,
`kRecoverPollTries`; `"0"` = термінал у спокої, спека §6.5) → `MarkSynchronized()` →
best-effort `ExecuteInternal("GetReceiptInfo", …)` (не публічний `GetReceiptInfo`: `inRecovery_`
вже `true`, тож повторного відновлення не станеться). Захист від рекурсії — прапорець `inRecovery_`.

---

## 7. Фасад 1С — `AddinECRPrivatJSON`

`src/components/AddinECRPrivatJSON.{h,cpp}`. Тонка компонента: успадковує `AddInNative`,
`REGISTER_COMPONENT(u"ECRPrivatJSON", AddinECRPrivatJSON)`, тримає `EcrPrivatJsonDriver driver_`.
Реєструє 18 методів (пари EN/RU) у `RegisterMethods()` + успадкований `EnableLogging`; повний довідник методів, типів повернення
й прикладів — [docs/integration-1c/ecr-privatjson.md](../integration-1c/ecr-privatjson.md).

Конвенції фасаду:

- **Синхронні операції** (`ПроверитьСвязь`/`ВерсияПО`/`Оплата`/`Возврат`/`ПолучитьЧек`)
  реєструються «голими» void-лямбдами (не через `Ret`): локальний `runSync` серіалізує
  `ResultEnvelope` у `this->result` (JSON-рядок) і кешує в `lastResultJson_`. Тому в 1С ці
  функції **повертають JSON-рядок** результату. (Обгортати їх `Ret()` не можна — перезаписав би
  корисний результат; правило з [AGENTS.md](../../AGENTS.md).)
- **Решта** (`Подключить`/`Подключен`/`НачатьОплату`/`СостояниеОперации`/`РезультатОперацииJSON`/
  `СтатусТерминала`/`Вендор`/`Модель`/`ВключитьТрассировку`/`ВключитьСобытия`) — через `Ret()`
  (bool/int/string).
- **Межа 1С — `try/catch` в кожному методі:** виняток C++ межу 1С не перетинає; при винятку —
  `REPORT_ERROR` (лог + `AddError`) і валідний `ResultEnvelope::Fail`, не сирий текст.
- `EnableLogging`/`ИспользоватьЛогирование` — успадкований (реєструвати не треба); у деструкторі —
  `driver_.Disconnect()` + `ServiceTools::DisableComponentLogging(this)`.

### 7.1. Опційні події — `ВключитьСобытия`

За замовчуванням драйвер poll-based (подій немає). `ВключитьСобытия(Истина)` ставить драйверу
`EventHandler`, що кличе `PostExternalEvent(event, dataJson)` ядра (потокобезпечний, з
poller/worker-потоку). Драйвер емітить три події (`EmitEvent`) — у 1С вони приходять як
`ВнешнееСобытие(Источник="ECRPrivatJSON", Событие, Данные)`:

| `Событие` | Коли | `Данные` (JSON) |
|---|---|---|
| `state` | старт операції | `{state:"Running", method}` |
| `status` | зміна `getLastStatMsgCode` | `{code, text, state:"Running"}` |
| `result` | завершення операції | `{ok, code, description, state:"Done"\|"Error", payload}` |

Якщо у 1С немає обробника `ВнешнееСобытие` — `PostExternalEvent` тихо нічого не робить (не падає).

---

## 8. Каталог операцій: реалізовано vs заплановано

**Реалізовано (вертикальний зріз):** `Connect`/`Disconnect`/`IsConnected`, `CheckConnection`,
`GetTerminalInfo`, `Purchase`, `Refund`, `GetReceiptInfo` (sync + `Purchase`/`Refund` також async),
`interrupt` (скасування), полінг `getLastStatMsgCode`, desync-відновлення. Цей набір вправляє
**всі** механізми фундаменту (обидві доріжки, класифікатор, JobEngine, poller, send-арбітр).

**Заплановано інкрементально** (переважно описи операцій + парсинг полів, без нових механізмів):
`Withdrawal[Partly]`, `Cashback`, `Preauthorization`, `SaleCompletion`, `GetBalance`, `Audit`,
`Verify`/`VerifyCopy`, друк, сервіси розстрочки/`ServiceGeneric`, бонусні картки,
`GetPhoneNumber`/`GetOTPpassword`, поле `adv`.

**Свідомо НЕ реалізовано (as-built):** пауза на рішення каси (`getLastStatMsgCode==11` →
`getDiscountName`+`correctTransaction`) — код `11` наразі не перериває продаж (штатно, спека);
методи `ПодтвердитьОперацию`/`СкорректироватьСумму`/`ОтклонитьОперацию` відсутні. **Partial
approval** (`0010`) поки трактується як звичайний `ok:true` без окремої гілки вибору каси. Ці
розширення потребуватимуть додаткових станів `JobEngine` (`AwaitingCashDecision`/
`AwaitingPartialDecision`).

---

## 9. Тестовий контур

Без обладнання, проти емулятора термінала на localhost. Джерело правди складу — див.
[AGENTS.md](../../AGENTS.md) «Тести»; для 1С-розробника — покроково в
[docs/integration-1c/ecr-privatjson.md](../integration-1c/ecr-privatjson.md).

- **L0.7 `ecr_privatjson_selftest`** (без UAPKI) — кодек (`BuildRequest`/`Parse`/`PeekMethod`),
  класифікатор (усі гілки §4), transport-e2e поверх `DeviceSession`+`TransportTCP` проти
  `tests/support/TerminalEmulator` (Winsock, скриптовані відповіді за `method`): `Connect` за
  еталонною схемою, `JobEngine`, sync-операції + `MapResult`, poller, `interrupt`, async-API,
  смоук 1С-фасаду через `AddInNative::CreateObject`.
- **L2-ecr `ecr_native_host`** — компонента `ECRPrivatJSON` через **головну DLL** (`LoadLibraryW`+
  `GetClassObject`): `Подключить`+`Оплата` проти in-process `TerminalEmulator`, звірка `tVariant`.
- **`ecr_terminal_emulator.exe`** (standalone, `[port]`, default 2000) — протокол-обізнаний
  TCP-емулятор для ручного тесту з **реальної 1С без обладнання**: реалістична `Purchase` ~4с з
  прогресом статусу `9→1→6→3→10`, переривається `interrupt` (→ `1001`).

---

## 10. Пов'язані документи

- [device-core.md](device-core.md) — фундамент (транспорт/framer/класифікатор/`DeviceSession`/
  `ResultEnvelope`/`JobEngine`), який цей драйвер перевикористовує.
- [docs/integration-1c/ecr-privatjson.md](../integration-1c/ecr-privatjson.md) — інструкція для
  1С-розробника: методи, приклади коду, тестування.
- [docs/ECR_Privat_JSON_Protokol.md](../ECR_Privat_JSON_Protokol.md) — специфікація протоколу.
- [core.md](core.md) — ядро `AddInNative`.
