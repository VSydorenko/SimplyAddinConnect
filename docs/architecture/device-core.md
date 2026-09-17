# Фундамент драйверів обладнання (device-core + платформа)

Цей документ описує **спільний фундамент**, на якому будуються всі драйвери обладнання
проєкту: байтовий транспорт → кадрування → класифікація → сесія запит/відповідь
(`src/transport/`), плюс платформа-каркас драйвера (`src/platform/`). Він **драйвер-незалежний**:
жодної специфіки конкретного протоколу тут немає. Конкретний драйвер (перший — ECRPrivatJSON,
див. [ecrprivatjson.md](ecrprivatjson.md)) реалізує лише свою протокольну частину поверх цього
фундаменту.

> **Кому це читати.** Розробнику, що додає **новий драйвер обладнання**. Після цього документа
> дивись [ecrprivatjson.md](ecrprivatjson.md) як живий приклад повної реалізації.

Імена файлів, класів і полів звірені з кодом (`src/transport/*`, `src/platform/*`).

---

## 1. Навіщо фундамент і що він дає «безкоштовно»

Обладнання (платіжні термінали, ваги, принтери етикеток, сканери) спілкується байтами по
COM/TCP. У кожному протоколі повторюються одні й ті самі задачі: тримати фоновий
потік читання, різати потік на кадри, зіставляти відповідь із запитом, переживати обриви й
реконекти, не змішувати конкурентні запити. Фундамент розв'язує це **раз** — драйвер отримує
готовими:

- **байтовий канал** з фоновим читанням і реконектом (`ITransport` + реалізації);
- **кадрування** байтового потоку в повідомлення (`IFramer` / `NullTerminatedFramer`);
- **сесію запит/відповідь** з двома доріжками, таймаутами, desync-логікою і потокобезпечними
  user-колбеками (`DeviceSession`);
- **уніфікований результат** операції (`ResultEnvelope`) і **машину асинхронного завдання**
  (`JobEngine`).

Драйвер дописує лише **дві протокол-специфічні речі** (§6): **кодек** (повідомлення ⇄ байти)
і **класифікатор кадрів** (`IFrameClassifier`) — плюс тонку оркестрацію життєвого циклу й
операцій.

---

## 2. Карта шарів

```
              КОМПОНЕНТА 1С (фасад драйвера, src/components/*)
                        │  делегує
┌───────────────────────▼──────────────────────────────────────────┐
│ ПЛАТФОРМА-КАРКАС  src/platform/                                    │
│   ResultEnvelope   {ok, code, description, payload}               │
│   JobEngine        одне асинхронне завдання (worker-потік)        │
├───────────────────────────────────────────────────────────────────┤
│ ДРАЙВЕР (протокол-специфіка)  src/drivers/<name>/                 │
│   <Name>Codec        повідомлення ⇄ байти (без делімітера)        │
│   <Name>Classifier : IFrameClassifier   кореляція кадр→запит      │
│   <Name>Driver       життєвий цикл + операції                    │
├───────────────────────────────────────────────────────────────────┤
│ DEVICE-CORE (загальне)  src/transport/                           │
│   DeviceSession   reader→framer→classifier→{primary|service|…}   │
│        │            дві доріжки Request*, таймаути, desync, реконект│
│        ├─ IFramer / NullTerminatedFramer   потік байтів ⇄ кадри   │
│        ├─ IFrameClassifier                 (реалізує драйвер)     │
│        └─ ITransport                       COM / TCP / спулер     │
└───────────────────────────────────────────────────────────────────┘
```

Правило шарів: **верхній не знає деталей нижнього**. `DeviceSession` не знає ані про JSON, ані
про конкретний протокол — тільки байти й абстрактний класифікатор. Уся протокольна специфіка
ізольована в кодеку+класифікаторі драйвера.

---

## 3. Транспортний шар — `ITransport`

`src/transport/Transport.h`. Єдиний інтерфейс каналу; усі канали виглядають однаково:

```cpp
bool Open();  bool Close();  bool IsOpen() const;
int  Send(const std::vector<uint8_t>& data);            // <0 — помилка відправки
void SetDataReceivedCallback(DataReceivedCallback);     // асинхронний прийом (фоновий потік)
void SetErrorCallback(ErrorCallback);
void SetConnectionStateCallback(ConnectionStateCallback);
```

Типи колбеків:
`DataReceivedCallback = std::function<void(const std::vector<uint8_t>&)>`,
`ErrorCallback = std::function<void(const std::string&, int)>`,
`ConnectionStateCallback = std::function<void(bool)>`.

**Прийом — подієвий:** транспорт із власного фонового reader-потоку віддає прочитані байти в
`DataReceivedCallback`. Драйвер сам байти не читає — це робить `DeviceSession`.

Реалізації (`src/transport/`):

| Клас | Файл | Канал | Ключове |
|---|---|---|---|
| `TransportCOM` | `Transport_COM.*` | Послідовний порт | конструктор `(portName, baud=9600, dataBits=8, parity='N', stopBits=1.0f)`; **дефолт baud 9600** — драйвер зазвичай передає свій |
| `TransportTCP` | `Transport_TCP.*` | TCP | клієнт `(host, port)` або сервер `(port, maxConnections=5)`; `SetTimeout(ms)` |
| `TransportSpoolerRaw` | `Transport_SpoolerRaw.*` | RAW-черга спулера Windows | односпрямований друк (`LabelPrinter`); device-core не задіяний |

> **WebSocket у DLL більше немає.** WS-**сервер** прибрали ще на етапі device-core: нативна
> компонента має власний фоновий reader у `DeviceSession`, тож EXE-міст/WS-сервер (як у
> вендорських `genericDriverJson*`) не потрібні. Слідом за ним пішов і WS-**клієнт**
> (`TransportWSClient` + сабмодуль `ixwebsocket`): жоден драйвер його не створював, а
> єдиним споживачем бібліотеки лишався ручний `uapki_fiscal_emulator`, який тепер має
> власний HTTP-сервер (`tests/support/MiniHttpServer.*`). Якщо WebSocket колись
> знадобиться — це буде новий `ITransport` із власним рішенням щодо бібліотеки; мультидоступ
> до одного пристрою — окремий CLI-процес поза головною DLL.

---

## 4. Кадрування — `IFramer` / `NullTerminatedFramer`

`src/transport/IFramer.h`, `NullTerminatedFramer.h`. Кадрувальник перетворює **потік байтів**
(який приходить довільними шматками) у **список повних кадрів** і навпаки — загортає payload у
дротовий формат перед відправкою.

- `Feed(bytes) → frames[]` — накопичує вхідні байти й повертає всі повні кадри, що зібралися
  (split/coalesce: кадр може прийти кількома `Feed`, або кілька кадрів — одним).
- `Wrap(payload, FrameOptions) → wireBytes` — загортає payload у кадр для відправлення.
- `Reset()` — скидає внутрішній буфер (при реконекті).

**`NullTerminatedFramer`** — єдина наявна реалізація: кадр = payload + термінатор `0x00`
(C-рядок). `FrameOptions{leadingDelimiter}` додатково ставить `0x00` **перед** payload (потрібно
деяким протоколам для першого кадру-хендшейку). Порожня дейтаграма (лише `0x00`) не ламає розбір.

Кадрувальник **не знає змісту** повідомлень — лише де межа кадру. Якщо новий протокол
використовує інше обрамлення (довжина-префікс, STX/ETX, CRC-кадр) — реалізуй новий `IFramer`;
`DeviceSession` від цього не залежить.

---

## 5. Сесія запит/відповідь — `DeviceSession`

`src/transport/DeviceSession.h` — серце фундаменту. **Єдиний власник байтового потоку.**

### 5.1. Єдиний шлях приймання

```
transport DataReceivedCallback → OnBytes → framer.Feed → кадри[]
   → classifier.Classify(PendingView{primary,service}, frame) → Classification{cls, reason}
      PrimaryResponse   → завершує pendingPrimary_ (result=frame)
      ServiceResponse   → завершує pendingService_
      RejectPrimary     → primary завершується статусом Busy|Unsupported
      RejectService     → service завершується статусом Busy|Unsupported
      RejectBoth        → неоднозначний reject при обох pending → сесія в desync
      Unsolicited       → unsolicitedHandler_ (на dispatcher-потоці)
```

Драйвер **не має власного буфера прийому** — це структурно унеможливлює клас багів «два
непоєднані буфери». Драйвер лише формує запит і читає результат.

### 5.2. Дві доріжки — primary і service

```cpp
RequestResult RequestPrimary(payload, timeoutMs=-1, FrameOptions={});
RequestResult RequestService(payload, timeoutMs=-1, FrameOptions={});
```

- **primary** — основна операція пристрою (та, що «займає» його: оплата, друк, зважування).
- **service** — легкі сервісні запити, що дозволені **паралельно** з активною primary-операцією
  (опитування статусу, скасування).

Кожна доріжка тримає **один** активний запит (`Pending`). Друга операція тієї ж доріжки одразу
завершується статусом `Concurrent`. Дві доріжки дозволяють, наприклад, **опитувати статус під
час довгої операції**, не чіпаючи саму операцію.

`RequestPrimary`/`RequestService` **блокують** потік викликача до відповіді/таймауту
(`cv_.wait_for`). Тому драйвер, якому потрібен паралельний полінг, тримає **два потоки**: worker
(active primary) і poller (service). Див. приклад у [ecrprivatjson.md](ecrprivatjson.md).

### 5.3. `RequestResult` / `RequestStatus`

`src/transport/RequestTypes.h`:

```cpp
struct RequestResult { RequestStatus status; std::vector<uint8_t> frame; };  // frame порожній, якщо status != Response
```

| `RequestStatus` | Коли |
|---|---|
| `Response` | валідна відповідь (у `frame`) |
| `Busy` | пристрій зайнятий (протокольний reject конкурентного запиту) |
| `Unsupported` | метод не підтримується пристроєм |
| `Timeout` | вичерпано час очікування |
| `Disconnected` | обрив під час in-flight |
| `SendFailed` | помилка відправки (`Send()<0`) |
| `Stopped` | `Stop()`/деструктор перервав запит |
| `Concurrent` | друга операція тієї ж доріжки |
| `Desynchronized` | сесія в desync — новий primary заборонено (§5.5) |

Драйвер мапить `RequestStatus` у людиночитний `ResultEnvelope` (§7).

### 5.4. Життєвий цикл і потокова модель

```cpp
bool Start();   // підписати колбеки транспорту, підняти dispatcher, відкрити транспорт
void Stop();    // безпечна зупинка з будь-якого потоку; ідемпотентна
bool IsConnected() const;
```

Потоки всередині сесії: **reader** (транспорту), **dispatcher** (user-колбеки), **supervisor**
(реконект). Наскрізне правило (`DeviceSession.h` §6): **усі user-колбеки — ЛИШЕ на
dispatcher-потоці**, ніколи під внутрішнім м'ютексом `m_`, ніколи на reader/caller-потоці. М'ютекс
тримається коротко, ніколи під `Send`/`Close`/колбеком.

**Колбеки ставляться лише до `Start()`** (після — no-op + WARN, щоб уникнути гонки з dispatcher):

```cpp
void SetUnsolicitedHandler(std::function<void(std::vector<uint8_t>)>);   // ініціативні кадри
void SetConnectionStateHandler(std::function<void(bool)>);               // зміна стану зв'язку
void SetWireTraceHandler(std::function<void(bool sendAttempt, std::vector<uint8_t>)>);  // wire-трасування
```

`SessionConfig` (дефолти в `DeviceSession.h`): `primaryTimeoutMs=30000`, `serviceTimeoutMs=5000`,
`autoReconnect=true`, `reconnectDelayMs=1000`…`reconnectMaxDelayMs=15000` (backoff),
`connectDeadlineMs=10000`, `maxBufferedBytes=1MiB`. Реконект — у супервізора; драйверу не треба
його писати.

### 5.5. Desync — коли новий primary заборонено

Якщо primary-запит завершився `Timeout` під час in-flight-транзакції, сесія виставляє
`desynchronized_`: пристрій міг завершити операцію, поки касу «не почули», тож слати новий
primary **небезпечно** (можна дублювати фінансову дію). У цьому стані:

```cpp
bool IsDesynchronized() const;   // сесія в desync
void MarkSynchronized();         // драйвер знімає desync ПІСЛЯ протокольного відновлення
```

`Desynchronized` знімається **лише** `MarkSynchronized()` — реконект його не знімає. **Service-доріжка
в desync НЕ блокується**, тож драйвер з'ясовує наслідок операції по service-запитах (опитати
статус пристрою), викликає `MarkSynchronized()` і лише тоді відновлює primary. Конкретна
процедура — протокол-специфічна (приклад — `CaptureOutcome` у ECR-драйвері,
[ecrprivatjson.md](ecrprivatjson.md) §6.6).

#### Межа шарів: що відновлення НЕ має права робити

> Протокол-специфічна процедура відновлення **не має підміняти результат операції**. `desync` у
> device-core — семантика **кадрової синхронізації**; «доля фінансової операції невідома» —
> семантика **драйвера**. Драйвер, що після відновлення повертає касі чужий чек, порушує саме цю
> межу (історія: `ECRPrivatJSON` віддавав «останній чек у пакеті» як результат нашої операції;
> виправлено 2026-09 — `docs/architecture/ecrprivatjson.md` §6.6-6.7).
>
> `desync` знімає **драйвер** (`MarkSynchronized()`), коли **за його критерієм** канал
> синхронізований; критерій — його, не сесії. Наслідок для будь-якого драйвера на device-core:
> primary-запит відновлення (хендшейк, Ping) при `desync` **до дроту не вийде**
> (`DeviceSession.cpp:175`) — драйвер має зняти прапорець **перед** ним сам, і робити це перед
> **кожною** спробою, бо `Timeout` власного Ping ставить `desync` знову. Захист грошей при цьому
> тримає **власний гейт драйвера**, а не `desync` (`ECRPrivatJSON`: `ecrprivatjson.md` §6.7 —
> нова фінансова операція не йде на дріт, поки доля попередньої невідома).

---

## 6. Що реалізує НОВИЙ драйвер

Фундамент дає все, крім **двох протокол-специфічних частин** + тонкої оркестрації:

### 6.1. Кодек (обов'язково) — повідомлення ⇄ байти

Чиста функція без стану: будує байтовий payload запиту (без делімітера — обрамлення робить
`IFramer`) і розбирає байтовий кадр відповіді у структуру. Приклад контракту
(`EcrJsonCodec` — JSON-протокол):

```cpp
static std::vector<uint8_t> BuildRequest(...);          // → payload без делімітера
static ParsedResponse       Parse(const std::vector<uint8_t>& frameNoDelimiter);  // невалідний вхід → {valid=false}, БЕЗ винятку
```

**Правило:** кодек **не кидає винятків** на невалідний вхід — повертає ознаку невалідності.

### 6.2. Класифікатор (обов'язково) — `IFrameClassifier`

`src/transport/IFrameClassifier.h`. Визначає, до якого активного запиту належить вхідний кадр:

```cpp
struct PendingView { const std::vector<uint8_t>* primary; const std::vector<uint8_t>* service; };
struct Classification { FrameClass cls; RejectReason reason; };   // FrameClass: PrimaryResponse|ServiceResponse|RejectPrimary|RejectService|RejectBoth|Unsolicited

virtual Classification Classify(const PendingView& pending, const std::vector<uint8_t>& frame) = 0;
```

Класифікатор дивиться на активні pending-запити (їхні payload для кореляції) і на вхідний кадр,
і вирішує: це відповідь на primary? на service? відмова (busy/unsupported)? неоднозначність
(→ desync)? ініціативний кадр? **Уся протокольна специфіка кореляції — тут** (приклад логіки —
[ecrprivatjson.md §4](ecrprivatjson.md)).

### 6.3. Драйвер + фасад 1С (оркестрація)

- **`<Name>Driver`** — розбір рядка підключення → фабрика транспорту → життєвий цикл (`Connect`
  за схемою протоколу) → операції (`RequestPrimary`/`RequestService`, мапінг у `ResultEnvelope`)
  → за потреби worker+poller і async-API поверх `JobEngine`.
- **`AddIn<Name>`** (`src/components/`) — тонка компонента 1С: успадковує `AddInNative`,
  реєструє методи (`REGISTER_COMPONENT`), делегує драйверу. Як додати компоненту й конвенції
  методів — див. [AGENTS.md](../../AGENTS.md) «Як додати компоненту» і [core.md](core.md).

---

## 7. Платформа-каркас — `src/platform/`

### 7.1. `ResultEnvelope` — уніфікований результат

`src/platform/ResultEnvelope.h`. Один конверт результату будь-якої операції будь-якого драйвера:

```cpp
struct ResultEnvelope {
    bool ok = false;
    std::string code;          // машинний код ("OK", "TIMEOUT", протокольний код…)
    std::string description;   // людиночитний опис
    nlohmann::json payload;    // корисне навантаження операції
    nlohmann::json ToJson() const;                          // {ok, code, description, payload} — для 1С
    static ResultEnvelope Ok(payload = {});
    static ResultEnvelope Fail(code, description);
};
```

Фасад 1С серіалізує `ResultEnvelope` у JSON-рядок і віддає в 1С; 1С розбирає його `РаскодироватьJSON`.
Єдиний формат результату для всіх драйверів робить прикладний 1С-код одноманітним — див.
[docs/integration-1c/README.md](../integration-1c/README.md).

### 7.2. `JobEngine` — одне асинхронне завдання

`src/platform/JobEngine.h`. Машина станів для **неблокуючого** API (щоб 1С не «морозила» UI на
довгій операції):

```cpp
enum class JobState { Idle, Running, Interrupting, Done, Error };

bool Start(std::function<ResultEnvelope()> op);   // false, якщо вже Running
JobState State() const;
bool TryGetResult(ResultEnvelope& out) const;     // true при Done/Error
void RequestCancel();                              // гардовано: Running→Interrupting
void ResetToIdle();                                // скинути завершений стан у Idle
void Join();                                       // дочекатися worker (Disconnect/dtor)
```

`Start(op)` піднімає worker-потік, що виконує `op()` (звичайно — блокуючий `RequestPrimary`
драйвера) і кладе результат. 1С опитує `State()`; при `Done/Error` забирає `TryGetResult`.
`RequestCancel` лише **виставляє прапорець** — фактичне скасування (протокольний interrupt)
робить драйвер, спостерігаючи прапорець із poller-а. Це **poll-based** модель: подій у 1С за
замовчуванням немає (драйвер може опційно емітити їх через `PostExternalEvent` ядра).

**`Start()` серіалізовано `startMutex_`** (2026-09). «Машина одного завдання» з ДВОМА викликачами —
легітимний сценарій: драйвер може стартувати фонове завдання і з потоку 1С, і з dispatcher-хука
сесії. Перевірка стану йде під `m_`, а `join(worker_)` і присвоєння `worker_` — поза ним, тож без
окремого м'ютекса два одночасні `Start` дають подвійний `join` того самого потоку й присвоєння в
уже-joinable потік, тобто `std::terminate`. `startMutex_` накриває обидві частини. Наслідок для
драйверів: **кликати `Start()` з кількох потоків безпечно** — рівно один викликач отримає `true`.
`Start()` також дозволяє перезапуск із `Done`/`Error`/`Idle` (блокує лише `Running`/`Interrupting`),
тож окремий `ResetToIdle()` перед повторним стартом не потрібен.

---

## 8. Тестовий контур фундаменту

Драйвери тестуються **без обладнання**, проти протокол-обізнаних емуляторів на localhost. Рівні
гейта (`run_tests.ps1`, деталі — [AGENTS.md](../../AGENTS.md) «Тести»):

- **L0.6 `wire_selftest`** — device-ядро без обладнання: `NullTerminatedFramer` (байтові вектори,
  split/coalesce), `IFrameClassifier`-подвійники, `DeviceSession` над детермінованим
  `LoopbackTransport` (happy/timeout/desync/reconnect/reentrancy/precedence/wire-trace) + смоук
  реального `TransportTCP` (localhost-echo).
- Драйвер додає власний рівень (напр. **L0.7** для ECR) поверх `DeviceSession`+`TransportTCP`
  проти свого емулятора термінала.

Тестова політика: **жодних форматів «з голови»** — вектори з байтових прикладів специфікації
протоколу; де спека дає лише текст — нормативне кодування цього тексту, не вигадка формату.

Як писати самі сценарії — [testing-rules.md](testing-rules.md): три правила проти хибно-зеленого.
Для драйверів на цьому фундаменті головне з них — **доказ стану перед перевіркою**, бо тут майже
кожна перевірка стосується поведінки в конкретному стані зв'язку.

---

## 8-біс. Проєктування підсистем із кількома потоками

Тут живуть потоки: dispatcher `DeviceSession`, супервізор реконекту, worker `JobEngine`, poller
драйвера, потік 1С. Специфікація такої підсистеми має особливість, яку варто знати **до** того, як
її писати.

**Специфікація про конкурентність не буває завершеною до першої живої траси.** Життєвий цикл
ECR-з'єднання (спека 2026-09-05) пройшов чотири раунди рев'ю тексту, рев'ю плану проти спеки й
звірку двома сесіями. Попри це імплементація знайшла **три дефекти рівня дизайну**, кожен —
заблокований механізм, а не описка: прапорець десинхронізації відбивав власний Ping відновлення
(замкнене коло); guard епохи не спрацьовував саме в тій гонці, заради якої написаний; події зміни
стану з двох потоків мінялися місцями, і слухач бачив порядок, зворотний до справжнього. Жоден не
був видимий у тексті.

Практичний висновок: **фікс-раунд специфікації плануйте як частину імплементації**, а не як аварію.
Архітектор лишається на зв'язку, поки код пишеться; кожна знахідка вертається в спеку з механізмом,
а не лише в код.

**Шість порід помилок, які пройшли крізь усі документні рев'ю.** Вони не про неуважність — усі
шість вижили після кількох читань:

| Порода | Як виглядає |
|---|---|
| Задум у доконаній формі | Документ описує намір як чинний факт («розширення зшиває реєстр із журналом») — і читач будує на ньому рішення |
| Добудова ланцюга | Видно фрагмент, решта ланцюга добудована уявою («параметр є у виклику обробника → дійде до драйвера») |
| Звужений пошук | Греп за одним патерном показує два випадки з семи, і виняток оголошують правилом |
| Факт записано, але не застосовано | Той самий документ містить факт і поруч дизайн, який його порушує (§2.1 знав, що primary в desync не проходить; §4.9.2 слав туди Ping) |
| Проза обіцяє те, чого код не робить | Текст описує механізм, реалізація описує інший, і тест перевіряє прозу |
| Локальне правило без глобальної гарантії | Правило виконано дослівно («жодних подій під локом»), а властивість, заради якої воно писалось (порядок подій), не забезпечена нічим |

Перші три ловить звірка з тим, хто має джерело. Останні три — **лише жива траса або рев'ю коду з
трасою в руках**: документне рев'ю читає прозу й код як два різні тексти, а ці дефекти живуть у
зазорі між ними.

---

## 9. Куди далі

- [ecrprivatjson.md](ecrprivatjson.md) — **повний приклад** драйвера на цьому фундаменті
  (кодек, класифікатор, життєвий цикл, worker+poller, async-API, стан зв'язку з Ping після
  реконекту, фонове з'ясування долі операції після обриву).
- [core.md](core.md) — ядро `AddInNative`: реєстрація компонент, `VariantHelper`, `Ret()`,
  `ParamSpec`, `PostExternalEvent`.
- [docs/integration-1c/](../integration-1c/README.md) — як 1С-розробник користується готовим драйвером.
- [AGENTS.md](../../AGENTS.md) — збірка, конвенції коду, «Як додати компоненту», «Тести».
