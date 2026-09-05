# Доля фінансової операції ECR після обриву зв'язку

*Дата: 2026-09-05. Статус: дизайн затверджено, імплементації ще немає.*

Драйвер — `docs/architecture/ecrprivatjson.md`; транспортний фундамент — `docs/architecture/device-core.md`;
контракт БПО й реєстр намірів на боці розширення — `docs/architecture/bpo-contract.md` §4, §6.3;
прикладний бік — `docs/integration-1c/ecr-privatjson.md`. Протокол — `docs/ECR_Privat_JSON_Protokol.md`.

Дизайн зроблено після дослідження, у якому три незалежні підходи (мінімальна дельта, явна модель
станів, ризик-перший) зійшлися на одному ядрі, а дев'ять рев'ю з трьох лінз (гроші й гонки,
перевикористання, тестованість) виявили дефекти лише в механіці потоків. Тут — синтез.

---

## 1. Проблема: три дефекти наявного коду

Сценарій: касир натиснув «Оплата», драйвер відправив `Purchase`, і зв'язок із терміналом обірвався.
Термінал міг списати картку. Каса результату не отримала.

### 1.1. Справжній обрив не запускає з'ясування взагалі

`EcrPrivatJsonDriver::ExecuteInternal` (`src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp:318`)
запускає відновлення лише за умови `RequestStatus::Timeout && session_->IsDesynchronized()`.
Обрив TCP дає інший статус — `Disconnected` (`DeviceSession.cpp:204-213`: очікувач прокидається за
`!connected_`), і `desynchronized_` при цьому **не ставиться** (`OnTransportState`, `:376-399`, чіпає
`connected_`, `epoch_`, `reconnectRequested_` — і тільки). Статус мапиться в `DISCONNECTED`
(`MapResult`) і йде касі. Компонента не робить жодної спроби дізнатись, що з грошима.

### 1.2. Коли з'ясування спрацьовує — чужий чек видається за наш результат

`RecoverAfterDesync()` (`:331-354`) полить `getLastStatMsgCode` до `"0"`, робить `MarkSynchronized()`
і повертає результат `ExecuteInternal("GetReceiptInfo", {invoiceNumber:""})` **як результат операції**.
Порожній `invoiceNumber` за протоколом §5.30 (`:2111`) — «останній чек у пакеті», незалежно від того,
хто його ініціював (§5.20, `:1444`). Зіставлення із сумою/часом нашої операції немає (підтверджено
кодом). Якщо наш `Purchase` до термінала не дійшов, каса отримує `ok:true`, RRN і суму **попередньої**
операції.

Це вдвічі гірше, ніж здається: розширення 1С уже має реєстр `СМП_НезавершенныеОперацииЭквайринга`
(`bpo-contract.md` §6.3), який чекає від компоненти чесного «не знаю» (`DESYNC`=9) — а отримує «успіх».

### 1.3. Дрібніші дефекти того ж механізму

- `MarkSynchronized()` (`:351`) викликається **безумовно** після `kRecoverPollTries`=5 спроб — навіть якщо
  термінал так і не вийшов зі стану «зайнятий»; сесія позначається чистою достроково.
- Внутрішній `ExecuteInternal("GetReceiptInfo")` безумовно емітить `EmitEvent("state")` (`:298`) і
  `EmitEvent("result", …)` (`:326-328`) із чужим payload — тобто підміна тече й **каналом подій**, і
  подія `result` на одну операцію приходить двічі. Подія не несе поля `method`, 1С їх не розрізнить.
- `GetReceiptInfo` у відновленні іде з `kHandshakeTimeoutMs`=5000 мс — під авторизацією на хості може
  не вистачити.

---

## 2. Встановлені факти, на яких стоїть дизайн

### 2.1. Транспорт (`DeviceSession`)

- `DoRequest` чекає на `cv_` під `m_`; відповідь кладе **reader**-потік і будить `cv_` напряму
  (`:309-338`). Dispatcher у ланцюзі не бере участі → блокуючий `RequestService` із хука стану **не
  дедлокне**, але **заблокує dispatcher** на весь час виклику (`DispatchLoop`, `:503-525` — один потік,
  FIFO). Хук має лише сигналізувати, не ходити в мережу.
- `stateHandler_` кличеться виключно з `OnTransportState` через `Enqueue` (`:393-398`), один раз на
  кожну реальну зміну стану; `h(true)` після успішного реконекту **не синхронізований** із моментом, коли
  `RequestPrimary` повернув `Disconnected` викликачу — це інший потік, і порядок не гарантований.
- `desynchronized_` ставиться лише у двох місцях: справжній таймаут primary при живому з'єднанні
  (`:217-222`) і `RejectBoth` (`:341-359`). Знімає — лише `MarkSynchronized()` (`:548`). Service-доріжка
  в desync дозволена, primary — ні (`:175`).
- `SetConnectionStateHandler` працює **лише до `Start()`** (`:567-574`, після — no-op + WARN).
- Після give-up реконекту (`reconnectMaxTries`) сесія лишається `DISCONNECTED` без автономного
  відновлення (`TestReconnectMaxTries`).
- `TransportTCP::Send` закриває сокет лише при `WSAECONNRESET/ABORTED` (`Transport_TCP.cpp:370-387`);
  інші помилки `Send` дають `SendFailed` **без** `OnTransportState(false)` → реконекту не буде.

### 2.2. Драйвер (`EcrPrivatJsonDriver`)

- `MakeSession` (`:75-87`) ставить один і той самий порожній `SetConnectionStateHandler` на **три**
  сесії за один `Connect()`: короткоживучі `hs` (`:111`) та `id` (`:126`) і персистентну `session_`
  (`:142`). `Disconnect()` на `:107` обнуляє `session_` **до** створення `hs`/`id`.
- `JobEngine job_` — один worker; `Start()` повертає `false`, якщо `Running`; `Start` сам приєднує
  попередній завершений потік (`JobEngine.cpp:11`); `Join()` є. Це готовий безпечний «один фоновий
  потік» — **для одного викликача**: перевірка стану іде під `m_`, а `join()` попереднього потоку й
  присвоєння `worker_` — поза ним (`JobEngine.cpp:6-16`). Два одночасні `Start()` при «не Running» —
  подвійний `join` того самого потоку і присвоєння joinable-потоку (`std::terminate`). Сьогодні
  безпечно, бо `job_.Start` кличе лише потік 1С.
- Стан `JobEngine` стає `Done` **після повернення** лямбди (`JobEngine.cpp:22-23`) — тобто пізніше, ніж
  лямбда записала свій результат кудись назовні. `Start()` у цьому вікні поверне `false`.
- `inRecovery_` — `load()`+`store()`, не CAS (`:318-321`). Сьогодні безпечно (один потік), із другим
  викликачем — TOCTOU.
- Блок «спитай статус» (`GateSend → RequestService(getLastStatMsgCode) → Parse → stoi`) дублюється в
  `PollerLoop` (`:264-279`) і `RecoverAfterDesync` (`:336-347`).
- Контекст операції (`method`, `params["amount"]`) проходить через `ExecuteInternal(method, params)` —
  нових параметрів у сигнатурах не потрібно.
- `EmitEvent` (`:225-231`): копія хендлера під `eventMutex_`, виклик поза локом — готовий канал для
  нової події.

### 2.3. Протокол

- Запит `Purchase` (§5.1.1, `:474-487`) містить лише `amount`, `discount`, `merchantId`, `facepay`,
  `subMerchant`. **Жодного ідентифікатора транзакції від каси**; `invoiceNumber` і `rrn` генерує термінал.
- `GetReceiptInfo` (§5.30): при `invoiceNumber=""` — останній чек у пакеті; для **неуспішної** операції
  повертає всі поля з порожніми значеннями, `responseCode` дублює код транзакції-джерела (`:447-449`,
  `:2166`). Це звичайний метод, не `ServiceMessage`, — на зайнятому терміналі отримає `deviceBusy` (§6.1).
- `getLastResult` (§6.7, `:2412-2416`) має рівно два значення: `0` — виконано, `2` — в процесі. Для мети
  «дочекатися, поки термінал не в процесі» він еквівалентний уже наявному `getLastStatMsgCode` (`0` =
  «status not available» = спокій). У критичному шляху **не потрібен**.
- `responseCode 1008 — "Transaction is already complete"` (`:429`) — задокументований сигнал дублювання
  при повторі `Purchase`. Не гарантія ідемпотентності, але корисний касі.
- Пакет = операційний день (§5.3, `:681`). Протокол **мовчить** про поведінку термінала при обриві каси
  посеред операції (`:171`, `:415` — лише про клієнта), про монотонність `invoiceNumber` і про те, чий
  годинник у `date/time`.

### 2.4. Фасади й 1С

- «Додаткові дії» БПО (`ВыполнитьДополнительноеДействие`) — форма **адміністратора**
  (`BpoFacadeBase.cpp:314-316`); касир у момент обриву до них не дістанеться.
- Під час синхронного виклику БПО клієнтський потік 1С **мертвий** (`bpo-contract.md` §4) — «спитати
  пізніше в межах виклику» неможливо.
- Розширення вже ходить у компоненту **прямим API** через `ОбъектДрайвера` — асинхронне розширення з
  шести методів: `НачатьОплату`/`НачатьВозврат`/`СостояниеОперации`/`РезультатОперацииJSON`/
  `СтатусТерминала`/`ПрерватьОперацию` (`bpo-contract.md:655-661`).
- `Disconnect()` драйвера кличеться лише з потоку 1С: деструктори компоненти й драйвера, `Отключить`
  (`AddinECRPrivatJSON.cpp:20,35`), `Close()` адаптера, початок `Connect()` (`:107`). Одночасності
  синхронної операції й `Disconnect()` архітектура не передбачає — але й не забороняє формально.
- Локалізовані імена методів компоненти — **російські без винятку** (`НачатьОплату`,
  `РезультатОперацииJSON`, `ОплатитьПлатежнойКартой`, …); методи, що повертають рядок JSON, мають суфікс
  `JSON` у локалізованому імені (`AcquiringFacadeBase.cpp:324`).
- Публічного методу `PingDevice` **немає** — це внутрішній хендшейк `Connect()` (`:113`). Тиха перевірка
  стану в прямому API — `IsConnected`/`Подключен` (`AddinECRPrivatJSON.cpp:39-40`), читає стан
  транспорту, термінала не турбує.
- `BpoFacadeBase::CodeToInt` (`BpoFacadeBase.cpp:31-54`): числовий рядок іде через `stoi` як код
  термінала; символьні — таблицею 0..16. Новий код має бути **символьним**.
- Ревізійні шари `AcquiringBpo3004/4000` заповнюють OUT-параметри **лише** при успіху
  (`if (!MapEnvToBool(env)) return false;` перед `fillOut`).
- Тестів на `RecoverAfterDesync`/`GetReceiptInfo`/desync у `ecr_privatjson_selftest` (84 CHECK) і
  `ecr_native_host` (77) **немає** — зміна цієї гілки нічого не ламає.

---

## 3. Прийняті рішення

Рішення користувача (не переглядаються):

1. **Компонента з'ясовує факти, каса вирішує.** Компонента інкапсулює протокол (дочекатися спокою →
   забрати чек) і віддає **факти**: є чек на суму X о 12:34:07, RRN такий-то, `responseCode` такий-то.
   Рішення «це наша транзакція, зараховуємо» приймає 1С, де є реєстр намірів. Компонента **не вгадує
   про гроші й не повторює операцію сама**.
2. **Знімок долі робиться одразу після реконекту** (фоново), не лише на запит каси.
3. **Операція в польоті повертає новий код `UNKNOWN_OUTCOME`=17** — продовження єдиної таксономії,
   коди 0..16 не рухаються.
4. **Одна спека на весь життєвий цикл; виправлення дефекту 1.2 — перше завдання плану, мерджиться
   окремо.**
5. **При живому з'єднанні `ExecuteInternal` чекає знімок bounded (~8 с) і віддає 17 із фактами**;
   **`Resolved` не блокує нову оплату** (блокує лише `Pending`).

Принцип архітектора, з якого випливає межа шарів:

> **«Доля фінансової операції невідома» — семантика еквайрингу. `desync` у device-core — семантика
> кадрової синхронізації (§7).** Після обриву TCP фреймер чистий (`epoch_++`), кадрам ніщо не загрожує.
> Тому `DeviceSession` **не змінюється**; гейт «не пускай нову оплату, поки доля невідома» — бізнес-правило
> драйвера. Альтернатива (ставити `desync` при обриві) має два шляхи в «назавжди»: `SendFailed` без
> закриття транспорту і give-up реконекту — і в обох випадках `desync` ніхто не зніме.

---

## 4. Дизайн

### 4.1. Стан «доля останньої операції»

Одна структура під одним мʼютексом у `EcrPrivatJsonDriver`:

```cpp
enum class OutcomeState { None, Pending, Resolved };

struct OperationIntent {
    std::string method;                                  // "Purchase" | "Refund"
    std::string amount;                                  // як відправлено на дріт
    std::string rrn;                                     // для Refund; інакше порожньо
    std::chrono::system_clock::time_point startedAt;     // мітка для 1С (ISO 8601 UTC)
};

struct LastOutcome {
    OutcomeState  state = OutcomeState::None;
    std::uint64_t generation = 0;      // бампається на кожен перехід у Pending
    OperationIntent intent;
    std::string   reason;              // "TIMEOUT" | "DISCONNECTED" | "SEND_FAILED" | "STOPPED" | "DESYNC"
    bool          terminalIdle = false;// чи термінал вийшов зі стану «зайнятий» до ліміту
    ResultEnvelope facts;              // результат RequestReceiptFacts (payload = поля §5.30)
};

mutable std::mutex outcomeMutex_;
LastOutcome        lastOutcome_;
JobEngine          outcomeJob_;        // другий екземпляр, не job_
std::atomic<bool>  closing_{ false };
```

Три стани, не п'ять: успіх/невдача з'ясування читається з `facts.ok`/`facts.code`, окремий
`Unresolvable` зайвий.

`generation` бампається на кожен перехід у `Pending` і має два призначення. Головне — **ідентифікатор
питання для 1С**: каса, що вже обробила знімок `generation=3`, бачить, що наступний виклик
`InquireLastOutcome` повернув той самий знімок, а не новий, і не зараховує його вдруге. Допоміжне —
страховка запису в `CaptureOutcome`: джоб порівнює своє покоління з поточним перед записом. Перетин двох
джобів різних поколінь у штатному потоці неможливий (гейт 4.6 не пускає нову фінансову операцію під час
`Pending`, `Disconnect()` приєднує джоб), тож guard — дешевий пояс безпеки, не основний механізм.

Під `outcomeMutex_` — жодного мережевого виклику й жодного `EmitEvent` (той самий стиль, що
`eventMutex_`).

### 4.2. Тригер: коли доля вважається невідомою

Після `RequestPrimary` **фінансового** методу (`kFinancialMethods = {"Purchase", "Refund"}`) у
`ExecuteInternal`:

1. **Що:** статус ∈ {`Timeout`, `Disconnected`, `SendFailed`, `Stopped`} **або**
   `session_->IsDesynchronized()`. Друга частина ловить `RejectBoth` (`DeviceSession.cpp:355` ставить
   desync, а статус віддає з `MapReject` — `Busy`/`Unsupported`), який переліком статусів пропускається.
   `Stopped` включено як страховку: синхронний `Execute()` не проходить через `job_`, і якщо колись
   `Disconnect()` прийде з іншого потоку під час операції, `Stop()` заверши́ть запит статусом `Stopped`
   (`FinishPendingLocked`, `DeviceSession.cpp:107`) — а термінал міг устигнути. Сьогодні така
   одночасність неможлива (2.4), але страховка коштує один рядок. `Busy`, `Unsupported` (без desync),
   `Concurrent` — відомі однозначно, не тригер.

   Значення `reason` — за таблицею, порядок перевірки саме такий:

   | Умова | `reason` |
   |---|---|
   | `r.status == Timeout` | `"TIMEOUT"` |
   | `r.status == Disconnected` | `"DISCONNECTED"` |
   | `r.status == SendFailed` | `"SEND_FAILED"` |
   | `r.status == Stopped` | `"STOPPED"` |
   | інакше, якщо `session_->IsDesynchronized()` | `"DESYNC"` |
2. **Як з'ясовувати:** **зв'язок є → зараз; зв'язку нема → після реконекту.** Перевірка —
   `session_->IsConnected()`. Це одне правило закриває й `SendFailed` без закриття транспорту (2.1):
   там з'єднання лишається живим, реконекту не буде, і хук ніколи не спрацював би.

`SendFailed` трактується так само консервативно, як `Disconnected`: `ITransport` не гарантує, що при
`Send<0` нічого не пішло на дріт.

Нефінансові методи (`Audit`, `Verify`, `CheckConnection`, `GetReceiptInfo`, `GetTerminalInfo`) при обриві
лишаються як сьогодні — `TIMEOUT`/`DISCONNECTED`/`SEND_FAILED`.

### 4.3. З'ясування — один механізм для всіх тригерів

**`outcomeJob_` — другий `JobEngine`**, не сирий `std::thread` і не `job_`:
- сирий потік як член класу при повторному обриві дає `std::terminate` (попередній не приєднано) —
  `JobEngine::Start` це вже вирішує (`JobEngine.cpp:11`);
- `job_` несе контракт `СостояниеОперации` для 1С — касовий полінг бачив би «операцію», якої касир не
  починав;
- `Start()` повертає `false`, якщо `Running` — і це вже захист від подвійного запуску; окремий
  CAS-прапорець не потрібен, `inRecovery_` прибирається.

**Зміна платформи — одна: `JobEngine::Start` серіалізується.** Сьогодні він безпечний для одного
викликача (2.2); дизайн уводить двох — `ExecuteInternal` і dispatcher-хук. Виправлення — `std::mutex
startMutex_` навколо всього `Start()` (три рядки, `src/platform/JobEngine.h/.cpp`). Зовнішня серіалізація
через `outcomeMutex_` відкинута: «машина одного завдання» з двома викликачами — легітимний сценарій, і
кожен майбутній споживач не має пам'ятати про пастку. Покривається тестом 6.2 №12.

Тіло джоба — `CaptureOutcome(generation)`, переписаний `RecoverAfterDesync`:

```
1. PollStatusOnce() у циклі з kPollIntervalMs (500 мс), доки код != "0",
   але не довше kOutcomeIdleWaitMs (= kOperationTimeoutMs, 120 с — стільки термінал
   може вести операцію: касир вводить пін). У фоні чекати можна довго.
   Кожну ітерацію: якщо closing_ або generation != lastOutcome_.generation → вихід без запису.
2. terminalIdle = (код == "0").
3. MarkSynchronized() — ЛИШЕ якщо terminalIdle (сьогодні — безумовно, дефект 1.3).
   Якщо не idle — сесія лишається в desync (якщо була), facts.code = "TERMINAL_BUSY",
   і 1С бачить це у знімку. Компонента не оголошує канал чистим, коли термінал зайнятий.
4. facts = RequestReceiptFacts("")  — вузький виклик, див. нижче; таймаут kOperationTimeoutMs,
   не kHandshakeTimeoutMs.
5. Під outcomeMutex_: якщо generation збігається — state = Resolved, записати terminalIdle, facts.
6. EmitEvent("outcome", <знімок, як у 4.7>) — окрема подія, не "result".
7. return ResultEnvelope::Ok()  — лише сигнал завершення для JobEngine; значення НЕ читається,
   факти беруться з lastOutcome_ під outcomeMutex_. Ранній вихід (closing_ / інше покоління /
   Stopped від сесії) → return Fail("ABORTED", …), без запису в lastOutcome_.
```

**`RequestReceiptFacts(invoiceNumber)`** — приватний вузький виклик: `GateSend()` →
`EcrJsonCodec::BuildRequest("GetReceiptInfo", …)` → `session_->RequestPrimary(req, kOperationTimeoutMs)`
→ `MapResult`. **Без** `EmitEvent("state"/"result")`, без дотику до `lastStatus_`/`interruptSent_`/
`job_`. Публічний `GetReceiptInfo` (прямий API `ПолучитьЧек`) лишається як є.

**`PollStatusOnce(int& code)`** — приватний хелпер, спільний для `PollerLoop` і `CaptureOutcome`:
прибирає дублювання `:264-279` / `:336-347`.

### 4.4. Старт джоба: одна функція, три точки входу

Джоб ніколи не стартує «рівно один раз з одного місця» — це створило б стан `Pending`, з якого немає
виходу. Приклад: `CaptureOutcome(1)` записав `Resolved` (крок 5), але лямбда ще не повернула, і
`JobEngine` досі `Running` (2.2); нова операція провалюється → `MarkPending` (`generation=2`) → `Start()`
повертає `false` → для покоління 2 ніхто ніколи не працює → `Pending` назавжди, гейт 4.6 блокує все.
Тому — **самозцілення**:

```cpp
// Єдина точка старту. Кличуть: ExecuteInternal після MarkPending, dispatcher-хук, гейт 4.6,
// InquireLastOutcome. Ідемпотентна: якщо джоб іде — нічого не робить.
void EnsureCaptureRunning() {
    if (closing_.load() || !IsConnected()) return;          // без зв'язку з'ясовувати нічим
    std::uint64_t gen;
    { std::lock_guard<std::mutex> lk(outcomeMutex_);
      if (lastOutcome_.state != OutcomeState::Pending) return;
      gen = lastOutcome_.generation; }
    outcomeJob_.Start([this, gen]{ return CaptureOutcome(gen); });   // false = уже йде, це нормально
}
```

Будь-яка точка входу, що бачить `Pending` при живому з'єднанні й непрацюючому джобі, перезапускає
з'ясування. Вікно з прикладу закривається наступним же викликом гейта або `InquireLastOutcome`.

**(а) З `ExecuteInternal`, зв'язок живий** (таймаут/desync/`SendFailed`-без-закриття):
```
MarkPending(intent, reason)                  // під outcomeMutex_, generation++
EnsureCaptureRunning()
чекати TryGetResult bounded kOutcomeSyncWaitMs (8000 мс), крок kPollIntervalMs
return BuildUnknownOutcome()                 // 17 з тим, що встигло з'ясуватися
```
Чому чекати, якщо секція про тригери каже «повертати негайно»: там ішлося про очікування
**реконекту** (необмежене). Тут — очікування **відповіді терміналу при живому з'єднанні**, секунди.
Для синхронного БПО-шляху, де потік 1С мертвий, це єдиний спосіб віддати факти в тому ж виклику.
8 с — параметр, менший за очікування касира в РМК; не догма.

**(б) З `ExecuteInternal`, зв'язку нема** (`Disconnected`, `Stopped`, `SendFailed`-із-закриттям):
```
MarkPending(intent, reason)
EnsureCaptureRunning()                       // при !IsConnected() — no-op; лишаємо для симетрії
return BuildUnknownOutcome()                 // 17 негайно, facts порожні, state=pending
```
Після реконекту джоб стартує з хука стану:
```cpp
// Connect(), крок 3, ПІСЛЯ MakeSession і ДО Start():
session_->SetConnectionStateHandler([this](bool up) { if (up) EnsureCaptureRunning(); });
```
Хук робить рівно `EnsureCaptureRunning` і повертається — dispatcher не блокується (2.1). Ставиться
**лише на персистентну `session_`** окремим викликом у кроці 3 `Connect()` (`:142`, до `Start()`);
`MakeSession` для `hs`/`id` лишає заглушку — інакше хук спрацював би на `session_ == nullptr` (`:107`
обнуляє її до створення `hs`).

Порядок «хук раніше за `MarkPending`» (між `Disconnected` у `DoRequest` і `OnTransportState(true)` немає
happens-before) безпечний: хук не побачить `Pending` і нічого не зробить, а `MarkPending` після нього
викличе `EnsureCaptureRunning` сам — і при живому вже з'єднанні джоб стартує звідти.

**(в) З `Connect()`** після успішного старту `session_`: `EnsureCaptureRunning()` — покриває ручний
реконект із 1С (`Отключить`/`Подключить`) при збереженому `Pending` (4.5).

### 4.5. Життєвий цикл

`Disconnect()`:
```
closing_ = true             // хук і CaptureOutcome бачать і виходять
job_.Join()                 // фінансову операцію НЕ рвемо — як сьогодні (:177)
session_->Stop()            // крок 1 Stop: усі pending-запити → Stopped негайно (:107);
                            // крок 5: dispatcher join-нуто — хуків більше не буде
outcomeJob_.Join()          // швидкий: його RequestPrimary/Service уже повернули Stopped
session_.reset()
closing_ = false
```
Порядок важливий: `outcomeJob_.Join()` **до** `Stop()` зависав би до `kOperationTimeoutMs` (120 с) —
джоб може стояти в `RequestPrimary`. `Stop()` завершує його запити миттєво. Один `Join` після `Stop()`
достатній: dispatcher приєднано (`DeviceSession.cpp:127-137`), нових хуків не буде; джоб, що вже йшов,
виходить із `Fail("ABORTED")` на першому ж `Stopped`. `shared_ptr` на сесію не потрібен.

`Disconnect()` **не викликати з хука** — `Stop()` із dispatcher-потоку не робить self-join (`:133-135`),
і гарантія «після `Stop()` хуків немає» не тримається. Сьогодні всі виклики — з потоку 1С (2.4).

`Connect()` **не скидає** `lastOutcome_`: факти про попередній обрив належать операції, не сесії, і
каса ще могла їх не забрати. Скидає лише наступний перехід у `Pending`.

### 4.6. Гейт: нова фінансова операція під час `Pending`

На вході `ExecuteInternal` для методу з `kFinancialMethods` — **перед** наявною перевіркою
`IsConnected()` (`:295`): якщо `lastOutcome_.state == Pending` → `EnsureCaptureRunning()` (самозцілення
4.4) і `return BuildUnknownOutcome()` з описом «З'ясовую долю попередньої операції; повторіть після
`ИсходПоследнейОперацииJSON`». Порядок саме такий, щоб касир при незавершеному намірі завжди бачив 17 з
поясненням, а не `NOT_CONNECTED`. На дріт нічого не йде. Це закриває:
- вікно «касир побачив помилку й тиснe ще раз, поки фоновий `GetReceiptInfo` ще іде» — найімовірніший
  шлях до подвійного списання;
- конкуренцію за єдину primary-доріжку між фоновим `RequestReceiptFacts` і новим `Purchase`
  (`CONCURRENT`), яку виявило рев'ю.

`Pending` короткий: при живому з'єднанні — секунди; без з'єднання нова операція й так неможлива
(`NOT_CONNECTED`). Після `Resolved` гейт **не діє**: факти віддані, далі рішення каси (рішення 1).
Блокування «до явного підтвердження» відкинуто: конфігурація без підтримки нового методу заблокувала б
термінал назавжди.

Гейт стосується й асинхронного шляху (`StartPurchase`/`StartRefund` → той самий `ExecuteInternal`).

**Нефінансові методи під час `Pending` не гейтяться** — свідомий компроміс. `Audit`/`Verify`/
`GetReceiptInfo`/`CheckConnection`, викликані, поки фоновий `RequestReceiptFacts` тримає primary-доріжку,
отримають `CONCURRENT` (`DeviceSession.cpp:176`) — чесний і зрозумілий код, вікно — секунди. Блокувати їх
означало б ховати діагностику саме тоді, коли адміністратор її хоче. Це задокументовано для 1С у §9.

### 4.7. Контракт із 1С

**Код 17.** `BpoFacadeBase::CodeToInt`: `if (code == "UNKNOWN_OUTCOME") return 17;` перед `return -1;`.
Рядок символьний — числова гілка `stoi` цифру прийняла б за код термінала.

**Payload операції з кодом 17** (той самий об'єкт — у `InquireLastOutcome` і в події `outcome`):
```json
{ "ok": false, "code": "UNKNOWN_OUTCOME",
  "description": "Зв'язок із терміналом обірвався під час операції; доля невідома",
  "payload": { "outcome": {
      "state": "pending" | "resolved",
      "generation": 3,
      "reason": "DISCONNECTED",
      "intent": { "method": "Purchase", "amount": "100.51", "rrn": "",
                  "startedAt": "2026-09-05T12:34:07.123Z" },
      "terminalIdle": true,
      "facts": { "amount": "100.51", "rrn": "…", "invoiceNumber": "…", "date": "…", "time": "…",
                 "responseCode": "0000", "txnType": "1", "approvalCode": "…", "pan": "…",
                 "receipt": "…" },
      "factsOk": true, "factsCode": "0000",
      "channelConnected": true } } }
```
`facts` — сирі поля §5.30 як є; `null`, поки `state == "pending"`. `intent` і `facts` поруч — каса
зіставляє суму/час/тип зі своїм реєстром і вирішує. Компонента ніде не пише «збіглося».

`intent.amount` — рядок **у тому вигляді, в якому пішов на дріт**: `MoneyToString` (`MoneyFormat.h`) —
крапка як десятковий роздільник, рівно два знаки, без групових роздільників (`"100.51"`). Термінал у
`facts.amount` віддає той самий формат (§5.1.2 приклад `"0.60"`), тож 1С може порівнювати текстово.
`intent.startedAt` — годинник машини каси (`system_clock`), ISO 8601 UTC; `facts.date`/`facts.time` — годинник
термінала, формат термінала (§7 п.3).

**OUT-параметри БПО при 17 лишаються порожніми.** `СсылочныйНомер`/`КодАвторизации`/`ТекстСлипЧека` — про
нашу операцію; клас­ти туди поля чужого чека — та сама підміна іншим каналом. Ревізійні шари
`AcquiringBpo3004/4000` **не змінюються**: їхній `if (!MapEnvToBool(env)) return false;` уже дає потрібну
поведінку. Каса на БПО-шляху отримує `false` + `GetLastError()`=17 і бере факти окремим викликом.

**Новий метод прямого API** — поруч з асинхронною трійцею, без параметрів, без мережі, будь-коли:

| Рівень | Сигнатура |
|---|---|
| `EcrPrivatJsonDriver` | `ResultEnvelope InquireLastOutcome() const;` — знімок під `outcomeMutex_` + `channelConnected = IsConnected()` |
| `IAcquiringDriver` | `virtual ResultEnvelope InquireLastOutcome() const { return AcquiringUnsupported("Доля останньої операції"); }` — дефолт «не вмію» за патерном `Void`/`DayTotals`, **не** чисто віртуальний |
| `EcrPrivatJsonAcquiring` | `override` — прохід у `drv_` |
| `AcquiringFacadeBase::RegisterAsyncExtensions` | `AddFunction(u"InquireLastOutcome", u"ИсходПоследнейОперацииJSON", Ret(... → std::string JSON))` |
| `AddinECRPrivatJSON` | той самий метод у прямому API компоненти `ECRPrivatJSON` |

Ім'я — за конвенцією компоненти (2.4): локалізоване російське, суфікс `JSON` для рядкового результату,
як у `РезультатОперацииJSON`.

При `state == None` метод повертає `ok:true, code:"OK", payload.outcome.state:"none"`.

**Подія `outcome`** через `EmitEvent` — коли знімок готовий; слухач «одна подія `result` на операцію»
не плутається: `result` для операції приходить рівно один раз, із кодом 17.

**Асинхронний шлях** (`TryGetOperationResult`/`РезультатОперацииJSON`) віддає той самий
`ResultEnvelope` з кодом 17 — без окремої логіки.

**`getLastResult`** (§6.7) у критичному шляху не використовується. Опційно — одне додаткове поле
`facts.lastResult` у майбутньому; не в цій спеці.

### 4.8. Що свідомо не робимо

- Не змінюємо `DeviceSession` — ні `desync` при обриві, ні нові хуки, ні `MarkDesynchronized()`.
- Не зіставляємо чек з наміром у компоненті — жодного `if (amount == intent.amount) ok = true`.
- Не повторюємо й не сторнуємо фінансову операцію автоматично.
- Не додаємо «додаткову дію» БПО для з'ясування (адміністраторська форма).
- Не додаємо прапорець у `AcquiringCapabilities` — дефолт «не вмію» достатній (YAGNI).
- Не заводимо узагальнений RAII scoped-thread у `platform/` — `JobEngine` уже є.
- Не робимо окремий recovery-модуль над драйверами — другого протоколу ще немає.
- Не заповнюємо OUT-параметри БПО фактами чужого чека.
- Не тримаємо стан між сеансами — це обов'язок реєстру в 1С.

---

## 5. Зміни по файлах

| Файл | Тип | Що |
|---|---|---|
| `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h/.cpp` | зміна | `LastOutcome`, `outcomeMutex_`, `outcomeJob_`, `closing_`; `kFinancialMethods`, `kOutcomeSyncWaitMs`, `kOutcomeIdleWaitMs`; `MarkPending`, `EnsureCaptureRunning`, `CaptureOutcome` (з `RecoverAfterDesync`), `RequestReceiptFacts`, `PollStatusOnce`, `BuildUnknownOutcome`, `InquireLastOutcome`; тригер у `ExecuteInternal` за 4.2/4.4; гейт 4.6; хук у `Connect()` крок 3 + `EnsureCaptureRunning` після старту; `Disconnect()` за 4.5; `inRecovery_` прибрано |
| `src/platform/JobEngine.h/.cpp` | зміна | `startMutex_` — серіалізація `Start()` для двох викликачів (4.3) |
| `src/components/BpoFacadeBase.cpp` | зміна | `CodeToInt`: `UNKNOWN_OUTCOME` → 17 |
| `src/drivers/IAcquiringDriver.h` | зміна | `InquireLastOutcome()` з дефолтом |
| `src/drivers/ecr_privatjson/EcrPrivatJsonAcquiring.h/.cpp` | зміна | `override` — прохід |
| `src/components/AcquiringFacadeBase.cpp` | зміна | реєстрація `ИсходПоследнейОперацииJSON` в `RegisterAsyncExtensions` |
| `src/components/AddinECRPrivatJSON.cpp` | зміна | той самий метод у прямому API |
| `tests/support/TerminalEmulator.h/.cpp` | зміна | `DropConnection()` — розірвати активне з'єднання з боку сценарію |
| `tests/ecr_privatjson_selftest.cpp` | зміна | тести §6 |
| `tests/ecr_native_host.cpp` | зміна | e2e через DLL: 17 на БПО-шляху, OUT порожні, `ИсходПоследнейОперацииJSON` |
| `docs/architecture/ecrprivatjson.md` | зміна | §6.6 переписати; новий розділ «Доля операції після обриву»; модель потоків (`outcomeJob_`) |
| `docs/architecture/device-core.md` | зміна | §5.5: протокол-специфічна процедура відновлення **не має підміняти результат операції** |
| `docs/architecture/bpo-contract.md` | зміна | таблиця кодів: 17; §6.3: компонента дає 17 із фактами; **звірити з компаньйоном** (§8), чи реєстр реагує на 17 як на «не знаю» |
| `docs/integration-1c/ecr-privatjson.md` | зміна | код 17, `ИсходПоследнейОперацииJSON`, подія `outcome`, `1008`, контракт розширення (§9); **`CheckConnection` позначити інтерактивним** (§5.5 протоколу — вибір мерчанта на терміналі); тиха перевірка стану — `IsConnected`/`Подключен` (публічного `PingDevice` немає, 2.4) |

`EcrJsonCodec`/`EcrPrivatJsonClassifier` — без змін (кодек будує довільний запит, класифікатор роутить
`ServiceMessage` за `msgType` generic). `AcquiringBpo3004/4000` — без змін.

---

## 6. Тестування

### 6.1. Емулятор

`TerminalEmulator::DropConnection()` — закрити `client_` з боку сценарію (сьогодні доступу до сокета з
`Responder` немає). Емулятор уже приймає наступний `accept` після обриву — реконект моделюється без
змін. Лічильники «на N-й виклик відповісти інакше» — локальний `std::atomic` у лямбді, як
`interruptSeen` у `TestDriverInterrupt`.

### 6.2. Сценарії (`ecr_privatjson_selftest`, драйвер напряму)

| # | Сценарій | CHECK |
|---|---|---|
| 1 | **Чужий чек не зараховано** (Task 1). `Purchase` таймаутить; `GetReceiptInfo` віддає завідомо іншу суму/RRN | код 17, `ok=false`; факти під `payload.outcome.facts`, не на топ-рівні; рівно **одна** подія `result` на операцію, жодної події з чужим payload під `result` |
| 2 | `Disconnected` → 17 негайно. `Purchase` → `DropConnection()` | 17 повернуто до реконекту; `state=pending`, `facts=null` |
| 3 | Знімок після реконекту. Продовження 2: емулятор приймає нове з'єднання, статус `10`,`10`,`0`, потім чек | `state=resolved`, `terminalIdle=true`, факти є; подія `outcome` рівно одна |
| 4 | **Гейт у `Pending`.** Під час 3, до `resolved`, викликати `Purchase` ще раз | 17 з описом про попередню; емулятор **не отримав** другого `Purchase` (лічильник = 1) |
| 5 | Після `Resolved` гейт знято | `Purchase` проходить на дріт |
| 6 | `SendFailed` при живому з'єднанні → з'ясування одразу (стаб транспорту з `Send<0` без закриття) | 17 із фактами в тому ж виклику; `channelConnected=true` |
| 7 | Термінал не виходить зі стану «зайнятий» до ліміту | `terminalIdle=false`, `factsCode="TERMINAL_BUSY"`; `MarkSynchronized` **не** викликано (якщо був desync — лишається) |
| 8 | `Disconnect()` посеред `Pending` з активним джобом | без `terminate`/UAF; після повторного `Connect()` `lastOutcome_` збережено |
| 9 | **Повторний обрив під час з'ясування.** `Purchase` → `DropConnection()` (gen=1) → реконект → хук стартує `CaptureOutcome(1)`; емулятор тримає статус `10` → `DropConnection()` ще раз під час полінгу → реконект | перший джоб вийшов з `ABORTED` без запису; після другого реконекту хук бачить `Pending`, джоб не `Running` → `EnsureCaptureRunning` перезапускає; знімок один, `generation` лишається 1 |
| 10 | Нефінансовий метод (`Audit`) при обриві | як сьогодні — `DISCONNECTED`, `lastOutcome_` не чіпається |
| 11 | `InquireLastOutcome` при `None` | `ok:true`, `state:"none"` |
| 12 | **`JobEngine::Start` з двох потоків одночасно** (юніт, без термінала): 100 ітерацій, два потоки кличуть `Start` на завершеному джобі | рівно один `true`, жодного `terminate`; без `startMutex_` тест має падати/аварійно завершуватись (негативна верифікація) |
| 13 | **Самозцілення.** Змоделювати вікно 4.4: `Pending` є, джоб не `Running`, зв'язок є (напр. `MarkPending` без `Start`) → викликати `InquireLastOutcome` | джоб стартує; за ним `state=resolved` |
| 14 | `Stopped` як тригер: `Disconnect()` з іншого потоку під час синхронного `Purchase` | 17, `reason="STOPPED"`, `Pending`; після `Connect()` (4.4в) — знімок |

### 6.3. Через DLL (`ecr_native_host`)

`ОплатитьПлатежнойКартой` на БПО-фасаді при обриві → `false`, `GetLastError()`=17, усі OUT порожні;
`ИсходПоследнейОперацииJSON` віддає JSON зі `state`/`intent`/`facts`; компонента `ECRPrivatJSON` — те
саме прямим API.

### 6.4. Негативна верифікація

Кожен новий CHECK перевіряється на червоне: тимчасово повернути стару поведінку (напр. `env = facts`
замість `BuildUnknownOutcome()`, або зняти гейт 4.6) і побачити падіння. Тест, що не падає на дефектній
поведінці, не тест.

---

## 7. Що лишається неперевіреним — на реальному терміналі

Протокол мовчить, і емулятор це лише моделює за припущенням:

1. Чи термінал **доводить** оплату до кінця, коли каса зникла посеред операції, чи скасовує її.
2. Чи `GetReceiptInfo("")` віддає **відхилену** останню операцію з її `responseCode`, чи лише успішні.
3. Чий годинник у `date`/`time` (термінала чи хоста) і наскільки він розходиться з касою.
4. Чи повертає термінал `1008` при повторі `Purchase` після виконаної операції.
5. Чи монотонний `invoiceNumber` у межах пакета.

Ці пункти — сценарії для Newland N950 після імплементації; до перевірки в документації вони позначені
як припущення.

---

## 8. Порядок робіт

**Task 1 — окремий PR, самодостатній:** дефект 1.2 і дірка з подіями.
- у гілці `Timeout && IsDesynchronized()` результат `RecoverAfterDesync` більше не стає `env`: `env =
  Fail("UNKNOWN_OUTCOME", …)`, факти — у `payload.outcome.facts`;
- `RecoverAfterDesync` бере чек через новий `RequestReceiptFacts` (без подій), не через `ExecuteInternal`;
- `CodeToInt`: 17;
- тест 6.2 №1.
Наявні 84+77 CHECK не зачіпаються (2.4).

**Task 2:** `LastOutcome` + `MarkPending` + `BuildUnknownOutcome` + `InquireLastOutcome` у драйвері;
тригер 4.2 (чотири статуси + `IsDesynchronized`, таблиця `reason`); `PollStatusOnce`; тести №10, №11.

**Task 3:** `JobEngine::startMutex_` (тест №12); `outcomeJob_` + `CaptureOutcome` (з
`RecoverAfterDesync`) + `EnsureCaptureRunning` + bounded очікування 4.4(а) + `MarkSynchronized` лише при
спокої; `inRecovery_` прибрано; тести №6, №7, №13.

**Task 4:** хук на `session_` у `Connect()` + `EnsureCaptureRunning` після старту (4.4в) + `Disconnect()`
за 4.5 + `generation`; `TerminalEmulator::DropConnection()`; тести №2, №3, №8, №9, №14.

**Task 5:** гейт 4.6 (перед `IsConnected()`, із самозціленням); тести №4, №5.

**Task 6:** `IAcquiringDriver`/`EcrPrivatJsonAcquiring`/`AcquiringFacadeBase`/`AddinECRPrivatJSON` —
`ИсходПоследнейОперацииJSON`; подія `outcome`; `ecr_native_host` 6.3.

**Task 7:** документація за §5 + §9 + правка `CheckConnection` в `integration-1c`.

**Паралельно Task 6-7 — звірка з компаньйоном (сесія розширення `SMP_SimplyConnect`):** чи реєстр
`СМП_НезавершенныеОперацииЭквайринга` реагує на **будь-який** неуспішний код як на «не знаю», чи
специфічно чекає `DESYNC`=9; чи розширення готове викликати `ИсходПоследнейОперацииJSON` і чекати
`state == "resolved"` (§9). Це контракт БПО — **звіряти, не вгадувати** (`AGENTS.md`). За потреби — задача
на боці розширення.

Кожне завдання — зелений гейт на x64 і x86 (`build_project.ps1 -WithTests` → `run_tests.ps1`; **`run_tests`
не перезбирає**).

---

## 9. Контракт розширення 1С

Цей розділ — для сесії розширення `SMP_SimplyConnect`. Компонента віддає факти; нижче — що з ними робити.
Без цього розширення вигадає власний протокол реагування і може повторити саме ту помилку, заради якої
писалась спека.

**Отримавши код 17 (`UNKNOWN_OUTCOME`) від фінансової операції:**

1. **Не повторювати цю оплату.** Ні автоматично, ні за натисканням касира, доки доля не з'ясована.
   Запис у `СМП_НезавершенныеОперацииЭквайринга` лишається відкритим.
2. **Забрати знімок.** Синхронний БПО-шлях подій не має (потік 1С мертвий під час виклику): одразу після
   `false`/`GetLastError()=17` викликати `ОбъектДрайвера.ИсходПоследнейОперацииJSON()`. Прямий API — те
   саме, або підписатися на подію `outcome` (`ВключитьСобытия`).
3. **Якщо `state == "pending"`** — компонента ще з'ясовує (термінал зайнятий або зв'язок відновлюється).
   Поллити `ИсходПоследнейОперацииJSON` із кроком ~1 с; `channelConnected:false` означає, що з'ясування
   почнеться після реконекту. Показати касиру стан, не помилку.
4. **Коли `state == "resolved"`** — звірити `intent` (наш намір: метод, сума, час каси) з `facts` (останній
   чек термінала: сума, час термінала, RRN, `responseCode`, `txnType`) і зі своїм записом реєстру. Рішення
   — за касою: збіг суми, типу (`txnType` 1=Purchase, 2=Refund) і часу в розумному вікні → зарахувати,
   записати RRN, закрити запис реєстру. `facts.responseCode != "0000"` → операція відхилена, закрити запис
   як невдалу. `factsOk:false` (`factsCode` `TERMINAL_BUSY`/`TIMEOUT`/…) → компонента чек не дістала —
   розбір оператором, запис лишається відкритим.
5. **`generation`** — ідентифікатор знімка. Той самий `generation`, що вже оброблено, обробляти вдруге
   не треба.
6. **Нова оплата за цим наміром** — лише після явного рішення каси за п. 4. Те, що компонента зняла
   свій гейт після `resolved`, — технічний факт, не дозвіл.
7. **Під час `pending` нефінансові виклики** (`ХОтчет`, `Сверка`, `ПолучитьЧек`, `ПроверитьСвязь`)
   можуть повернути `CONCURRENT` — фоновий запит тримає доріжку секунди. Повторити пізніше.
8. **Якщо каса все ж повторила `Purchase`** і термінал відповів `responseCode 1008 — "Transaction is
   already complete"` — це задокументований сигнал, що операція вже виконана; не гарантія, але привід
   зупинитись і звірити реєстр.

Що компонента **не** робить і на що розширенню не варто розраховувати: не зіставляє чек із наміром, не
повторює й не сторнує операцію, не тримає стан між сеансами (після перезапуску процесу `state == "none"`
— джерело правди про незавершені наміри лишається реєстр у 1С).
