# Device-facing ядро: ITransport + IFramer + DeviceSession — дизайн (ред. 2)

*Дата: 2026-07-20. Статус: ред. 2 після локального Codex-аудиту (гілка `device-core`).*
*Спирається на: `docs/tasks/2026-07-19_platform_architecture_design.md`,
`docs/ECR_Privat_JSON_Protokol.md`, виконаний Етап 0 (`2026-07-20_plan_etap0_core.md`).*
*Ред. 2 враховує підтверджені по коду/спеці знахідки аудиту: класифікація кадрів
замість `bool`-кореляції, дві доріжки primary/service, результат-структура зі
статусом, desync-на-тайм-аут, реальні фікси транспортів (не «зберегти як є»),
дисципліна потоків (dispatcher user-колбеків, теардаун, framer-mutex), точні
CMake/тест/доки-чеклісти.*

---

## 1. Мета й обсяг

Побудувати переюзний device-facing фундамент драйверів: «байтовий транспорт →
кадрування → класифікація → сесія запит/відповідь», повністю тестований без
обладнання (loopback + байтові вектори).

**У обсязі:**
- `ITransport` — байтовий контракт із **чіткою моделлю потоків/помилок** + реальні
  фікси наявних `TransportCOM`/`TransportTCP`/`TransportWSClient` (§9.1).
- `IFramer` + `NullTerminatedFramer` (per-request `FrameOptions`).
- `IFrameClassifier` — класифікація вхідного кадру (не `bool`-кореляція).
- `DeviceSession` — постійна дуплексна сесія: reader, буфер, framer, класифікація,
  **дві доріжки** `RequestPrimary`/`RequestService`, результат-структура,
  unsolicited-канал через **dispatcher**, реконект-супервізор, desync-логіка,
  wire-трейс.
- `wire_selftest` + `LoopbackTransport` (з окремим потоком доставки).
- Прибирання мертвого коду + синхронізація CMake/доків (§9.2).

**Поза обсягом (наступні фази):**
- Рушій async-операцій (`JobEngine`), база `DeviceDriverComponent`, `PrivatDriver`,
  BPOS1/POSAPI.
- **Фабрика/парсер рядка підключення** (`"tcp://…"`/`"COM3:…"`/`"ws://…"` → `ITransport`) —
  свідомо в драйверну фазу; у цій фазі тести й майбутній caller створюють транспорт
  **прямими конструкторами** (`TransportCOM/TCP/WSClient`) і передають `unique_ptr` у
  сесію. Формат рядка тут лише зафіксовано документально.

## 2. Що лагодить

Хендшейк Привату колись не пішов = **баг №1** (розірваний прийом): очікувач висів на
condition variable, яку ніхто не сигналив (`ProcessReceivedData` не підключений до
транспорту). `DeviceSession` усуває це структурно: прийом і очікування в одному
об'єкті на спільних `mutex`/`cv`, єдиний власник байтового потоку.

Додатковий висновок аудиту: **самі транспорти теж треба лагодити** (витоки хендлів,
можливий hang у `Close`, WS-реконект, часткова відправка) — «зберегти як є» було
хибним припущенням (§9.1).

## 3. Архітектура

```
              ┌──────────────────────────────────────────────┐
   (пізніше)  │ DeviceDriverComponent : AddInNative           │  ← НЕ в цій задачі
              └───────────────────┬──────────────────────────┘
                  RequestPrimary/RequestService/unsolicited
  ┌───────────────────────────────▼───────────────────────────┐
  │ DeviceSession                                              │
  │  reader→framer→classifier→{primary|service|reject|unsol}   │
  │  RequestPrimary / RequestService → RequestResult           │
  │  dispatcher-потік для user-колбеків; реконект-супервізор    │
  └──┬────────────┬───────────────┬──────────────┬─────────────┘
     │ IFramer    │ IFrameClassifier             │ ITransport
  ┌──▼───────┐ ┌──▼──────────────┐  ┌────────────▼──────────────┐
  │NullTerm  │ │(Privat: method+ │  │ TransportCOM/_TCP/_WSClient│
  │Framer    │ │ msgType) — драйв│  │ (з фіксами §9.1)           │
  │+mutex    │ │ тести: подвійники│  │ + LoopbackTransport (тест)│
  └──────────┘ └─────────────────┘  └───────────────────────────┘
```

Правило потоків (наскрізне): **транспорт → `OnBytes`/`OnTransportState` (внутрішній
стан під `m_`); user-колбеки (unsolicited / connection-state / wire-trace) — ЛИШЕ на
окремому dispatcher-потоці, ніколи не під `m_` і не на reader/caller/supervisor-потоці.**

## 4. Компоненти й інтерфейси

### 4.1 `ITransport` (еволюція `src/transport/Transport.h`) — уточнений контракт

```cpp
class ITransport {
public:
    using DataReceivedCallback    = std::function<void(const std::vector<uint8_t>&)>;
    using ErrorCallback           = std::function<void(const std::string&, int)>;
    using ConnectionStateCallback = std::function<void(bool /*connected*/)>;

    virtual ~ITransport() = default;

    // Open: синхронна спроба. true = ресурс відкрито (для WS — start ініційовано;
    // фактичний конект підтверджується ConnectionState(true), див. контракт нижче).
    virtual bool Open() = 0;

    // Close: ІДЕМПОТЕНТНА і ЗАВЖДИ звільняє ресурси (handle/socket/thread) незалежно
    // від внутрішніх прапорців. КОНТРАКТ: після повернення Close() жоден колбек
    // (data/error/state) більше НЕ почнеться. Порядок для сокетів: atomic→closed →
    // shutdown(SD_BOTH) → join reader → closesocket (щоб розблокувати recv у reader).
    virtual bool Close() = 0;

    virtual bool IsOpen() const = 0;

    // Send: ALL-OR-ERROR. Повертає кількість надісланих байтів == data.size() при
    // успіху, або <0 при помилці. Реалізація ЦИКЛІЧНО дописує залишок; часткова
    // відправка НЕ вважається успіхом.
    virtual int Send(const std::vector<uint8_t>& data) = 0;

    // Єдиний власник колбеків — DeviceSession (для СВОГО екземпляра транспорту).
    // МОДЕЛЬ ПОТОКІВ (частина контракту):
    //  • DataReceived — завжди на фоновому reader-потоці транспорту.
    //  • ConnectionState — може приходити з РІЗНИХ потоків: caller-потік при
    //    Open()/локальному Close(); reader-потік при remote-disconnect; ix-worker
    //    для WS. Гарантія: state(false) генерується РІВНО ОДИН раз на розрив.
    //  • Error — reader/caller-потік.
    //  Колбеки НЕ викликаються з-під внутрішнього локу транспорту.
    virtual void SetDataReceivedCallback(DataReceivedCallback) = 0;
    virtual void SetErrorCallback(ErrorCallback) = 0;
    virtual void SetConnectionStateCallback(ConnectionStateCallback) = 0;
};
```

Формат рядка підключення (лише документально; парсер — драйверна фаза):
`"COM3:115200,8,N,1"` | `"tcp://host:2000"` | `"ws://host:3000/path"`.

### 4.2 `IFramer` + `NullTerminatedFramer`

```cpp
struct FrameOptions { bool leadingDelimiter = false; };  // per-request, НЕ конструктор

class IFramer {
public:
    virtual ~IFramer() = default;
    virtual void Feed(const std::vector<uint8_t>& chunk,
                      std::vector<std::vector<uint8_t>>& out) = 0;   // stateful буфер
    virtual std::vector<uint8_t> Wrap(const std::vector<uint8_t>& payload,
                                      FrameOptions opts = {}) = 0;
    virtual void Reset() = 0;
};
```

`NullTerminatedFramer`:
- `Feed`: накопичує в буфер (stateful між викликами), ріже по `0x00`, термінатор
  відкидається, **порожні кадри (подвійний/провідний `0x00`) ігноруються** — так
  вхідний провідний `0x00` (спека дозволяє лише перед handshake, `ECR_Privat_JSON_Protokol.md:87`)
  не породжує фальшивого unsolicited.
- `Wrap`: `payload + 0x00`; за `opts.leadingDelimiter` — `0x00 + payload + 0x00`
  (лише для `PingDevice`-хендшейку; звичайні дейтаграми — тільки кінцевий `0x00`,
  `ECR_Privat_JSON_Protokol.md:145`). **Провідний `0x00` — per-request, не постійний.**
- `Reset`: очищає буфер (виклик при реконекті).
- Синхронізація: див. §6 — Feed/Wrap/Reset із різних потоків → **власний `framerMutex_`**;
  внутрішній ліміт `maxBufferedBytes` (захист від нескінченного накопичення без `0x00`).

### 4.3 `IFrameClassifier` (замість `IFrameCorrelator`) — класифікація, не `bool`

Кореляція за одним полем `method` некоректна (deviceBusy/identify — див. нижче), тож
класифікатор бачить, що зараз in-flight, і повертає **клас** кадру:

```cpp
enum class FrameClass {
    PrimaryResponse,  // відповідь на pendingPrimary → завершити primary статусом Response
    ServiceResponse,  // відповідь на pendingService → завершити service статусом Response
    RejectPrimary,    // напр. deviceBusy/methodNotImplemented → завершити primary rejectStatus
    Unsolicited       // ініціативне повідомлення терміналу (статуси, prompt тощо)
};
struct PendingView {                       // що зараз чекає (nullptr = нема)
    const std::vector<uint8_t>* primary;
    const std::vector<uint8_t>* service;
};
struct Classification { FrameClass cls; RequestStatus rejectStatus; };  // rejectStatus лише для RejectPrimary

class IFrameClassifier {
public:
    virtual ~IFrameClassifier() = default;
    virtual Classification Classify(const PendingView& pending,
                                    const std::vector<uint8_t>& frame) = 0;
};
```

**Чому так (докази зі спеки):**
- `deviceBusy` — відповідь на несервісний запит, але `method="ServiceMessage"`
  (`:2187`). За `method` не зматчиться з `Purchase` → без `RejectPrimary` primary
  даремно чекав би до 120с. → клас `RejectPrimary`, `rejectStatus=Busy`.
- `methodNotImplemented` (`:2244`) — теж `RejectPrimary`, `rejectStatus=Unsupported`.
- `identify`: і запит, і відповідь мають `method="ServiceMessage"` (`:2529`) —
  кореляція лише за `method` хибно зматчила б чужий `ServiceMessage`; класифікатор
  Привату розрізняє за `params.msgType` (identify→identify) і за наявним `pendingService`.
- Службові мапінги запит→відповідь: `interrupt→interruptTransmitted` (`:2213`),
  `correctTransaction→correctionTransmitted` (`:2633`), `debug→debugOn/Off` (`:2372`) —
  класифікатор Привату враховує `msgType`, невідомий `ServiceMessage` НЕ завершує
  service-pending, а йде в `Unsolicited`.

Privat-класифікатор — драйверна фаза. Тести цієї фази: подвійники
(`EchoClassifier` — рівність байтів = `PrimaryResponse`; програмовані для inject
`RejectPrimary`/`ServiceResponse`/`Unsolicited`).

### 4.4 `DeviceSession`

```cpp
enum class RequestStatus {
    Response, Busy, Unsupported, Timeout, Disconnected, SendFailed, Stopped, Concurrent
};
struct RequestResult {
    RequestStatus status;
    std::vector<uint8_t> frame;   // валідний лише при status==Response
};

struct SessionConfig {
    int  primaryTimeoutMsDefault = 30000;   // операції задають свій (напр. 120000)
    int  serviceTimeoutMsDefault = 5000;
    bool autoReconnect           = true;
    int  reconnectDelayMs        = 1000;    // експоненційний backoff
    int  reconnectMaxDelayMs     = 15000;
    int  reconnectMaxTries       = 0;       // 0 = нескінченно (постійний конект)
    int  connectDeadlineMs       = 10000;   // очікування state(true) як успіху конекту
};

class DeviceSession {
public:
    DeviceSession(std::unique_ptr<ITransport>, std::unique_ptr<IFramer>,
                  std::unique_ptr<IFrameClassifier>, SessionConfig cfg = {});
    ~DeviceSession();                        // Stop() з гарантованим теардауном (§6)

    bool Start();
    void Stop();
    bool IsConnected() const;

    // Основна операція (Purchase/Refund/…): серіалізована — один primary in-flight.
    // Другий primary під час активного → {Concurrent}. Не з'єднано → {Disconnected}.
    // Тайм-аут → {Timeout} + сесія переходить у Desynchronized і форсує реконект
    // (див. §7): бо в Приваті нема унікального ID запиту, «протухла» відповідь може
    // зматчитися на наступний primary.
    RequestResult RequestPrimary(const std::vector<uint8_t>& payload, int timeoutMs,
                                 FrameOptions frameOpts = {});

    // Службовий запит (getLastStatMsgCode/interrupt/correctTransaction/…): серіалізований
    // серед service, але може виконуватися ПАРАЛЕЛЬНО з активним primary
    // (спека: ServiceMessage async, `ECR_Privat_JSON_Protokol.md:303`).
    RequestResult RequestService(const std::vector<uint8_t>& payload, int timeoutMs,
                                 FrameOptions frameOpts = {});

    // User-колбеки — усі викликаються на DISPATCHER-потоці (§6), короткі/неблокуючі.
    void SetUnsolicitedHandler(std::function<void(std::vector<uint8_t>)>);
    void SetConnectionStateHandler(std::function<void(bool)>);
    // Wire-трейс: СИРІ байти (для outgoing — обгорнутий кадр із термінатором; для
    // incoming — чанк як прийшов від транспорту). Формат сумісний із trace.log спеки.
    void SetWireTraceHandler(std::function<void(bool outgoing, std::vector<uint8_t>)>);

private:
    void OnBytes(const std::vector<uint8_t>&);        // reader-потік
    void OnTransportState(bool connected);            // різні потоки (див. ITransport)
    void OnTransportError(const std::string&, int);
    void ReconnectLoop();                             // супервізор-потік
    void DispatchLoop();                              // dispatcher-потік (user-колбеки)
    // transport_, framer_(+framerMutex_), classifier_, cfg_
    // m_ (pending primary/service, стан); cv_; dispatchQueue_+dispatchCv_
    // stopping_, desynchronized_, connected_
};
```

## 5. Потоки даних

**Відправка (`RequestPrimary`/`RequestService`, потік-виклик):**
1. Під `m_`: перевірити `stopping_`/`connected_`/`desynchronized_` і відсутність
   іншого запиту тієї ж доріжки; зарезервувати pending (payload + місце під результат).
   Якщо не можна — повернути `{Concurrent}`/`{Disconnected}`/`{Stopped}` **без** мережі.
2. **Unlock `m_`** → `framer_->Wrap(payload, opts)` (під `framerMutex_`) → wire-trace(out,
   через dispatcher) → `transport_->Send()`. Помилка `Send<0` → зняти pending, `{SendFailed}`.
3. `cv_.wait_for(lock, timeout, predicate=результат готовий || розрив || stop)`.
4. Повернути збережений `RequestResult` (`Response`/`Busy`/`Unsupported`/`Timeout`/
   `Disconnected`/`Stopped`).

**Прийом (reader-потік `OnBytes`):**
1. wire-trace(in) через dispatcher.
2. Під `framerMutex_`: `framer_->Feed(chunk, frames)` (винятки framer ловляться тут).
3. Для кожного `frame`: під `m_` викликати `classifier_->Classify(pendingView, frame)`:
   - `PrimaryResponse` → записати у primary-pending, `notify` очікувача primary.
   - `ServiceResponse` → у service-pending, `notify` очікувача service.
   - `RejectPrimary` → завершити primary-pending статусом `rejectStatus`, `notify`.
   - `Unsolicited` → покласти кадр у `dispatchQueue_` (user-хендлер — на dispatcher).
   *(Класифікація — швидка, без user-коду; жодного user-колбека під `m_`.)*

**Реконект (`OnTransportState(false)` → супервізор):** якщо `autoReconnect` і не
`stopping_` → завершити pending-и статусом `Disconnected`, `framer_->Reset()`,
розбудити `ReconnectLoop`. Супервізор працює за предикатом `desiredUp && !connected_`
(включно з initial-Open-failure), backoff, `transport_->Close()`+`Open()`; успіхом
вважає лише `state(true)` у межах `connectDeadlineMs` (не сам факт `Open()==true`).
Після успішного реконекту знімає `desynchronized_`.

## 6. Модель потоків (явно)

Чотири ролі потоків:
- **Потік-виклик** — `RequestPrimary/Service`, блокується на `cv_`.
- **reader-потік транспорту** — `OnBytes` (Feed+класифікація+notify); **user-код не виконує**.
- **супервізор реконекту** — окремий; join у `Stop()`.
- **dispatcher-потік** — єдиний, виконує ВСІ user-колбеки (unsolicited/state/trace) з
  черги. Це усуває (а) реентрантний `Request` з reader-потоку (той не зміг би читати
  свою ж відповідь) і (б) `Stop()` з user-колбека, що join-ив би сам себе.

Локи:
- `m_` — pending-и primary/service, `connected_`/`stopping_`/`desynchronized_`. Тримається
  коротко; **ніколи** під час `Send`/`Open`/`Close` і user-колбеків.
- `framerMutex_` — Feed/Wrap/Reset (різні потоки).
- `dispatchCv_`/`dispatchQueue_` — черга dispatcher.

**Теардаун `Stop()` (гарантований порядок, проти callback-у в знищену сесію):**
1. Під `m_`: `stopping_=true`; завершити всі pending статусом `Stopped`; `cv_.notify_all`.
2. Розбудити й **join** супервізор (без `m_`).
3. `transport_->Close()` (без `m_`) — контракт §4.1 гарантує: після повернення колбеків
   не буде.
4. Від'єднати три транспортні колбеки (`Set*Callback(nullptr)`).
5. Зупинити й **join** dispatcher-потік (після Close — нових елементів не додасться).
6. `framer_->Reset()`.
`~DeviceSession` викликає `Stop()` (ідемпотентний).

## 7. Обробка помилок і результат

- `Send<0` → `{SendFailed}`, pending знято.
- Таймаут **primary** → `{Timeout}` + `desynchronized_=true` + форс-реконект (нова
  синхронізація). Результат транзакції НЕВІДОМИЙ — відновлення (запит статусу) робить
  драйвер у наступній фазі. Таймаут **service** — м'якший: `{Timeout}` без desync.
- Обрив (`state(false)`) → усі pending `{Disconnected}` + реконект за політикою.
- `RejectPrimary` (deviceBusy/methodNotImplemented) → primary завершується
  `{Busy}`/`{Unsupported}` НЕГАЙНО (не чекає таймауту).
- Виняток framer/classifier — ловиться в `OnBytes`, лог WARN, кадр відкидається,
  сесія живе; не пропускається через reader-потік транспорту.
- Виняток user-колбека (на dispatcher) — ловиться, лог WARN, dispatcher живе.
- Немає `throw` через межу API.

## 8. Wire-трейс

`SetWireTraceHandler(outgoing, bytes)` — **сирі байти**: outgoing = обгорнутий кадр
(із термінатором), incoming = чанк як прийшов від транспорту (може не збігатися з
межами кадрів — це прийнятно для сирого трейсу; логер драйвера форматує в hex за
`trace.log`, `ECR_Privat_JSON_Protokol.md:2335`). Викликається на dispatcher-потоці.

## 9. Наявні транспорти й прибирання коду

### 9.1 Реальні фікси транспортів (аудит: «зберегти як є» — хибно)

- **`TransportCOM`** (`Transport_COM.cpp`): (а) `CreateFileA` → `CreateFileW`
  (`:41`); (б) `Close()` при `!m_isOpen` виходить, не закривши handle — cleanup має
  залежати від валідності `m_portHandle`, а не прапорця (`:103`), інакше помилка
  `ConfigurePort`/`SetTimeouts` в `Open` (`:66-77`) → витік handle; (в) reader при
  неусувній read-помилці лише логує — має генерувати `state(false)` і будити супервізор
  (`:~439`); (г) прибрати мертвий `#include <winsock2.h>`.
- **`TransportTCP`** (`Transport_TCP.cpp`): (а) `Close()` при `!m_isOpen` → рання відмова
  без `closesocket` (`:283`) — cleanup завжди; (б) можливий hang: `StopReadThread` join
  (`:478`) поки reader у блокуючому `recv` (`:490`) — порядок `shutdown(SD_BOTH)`+
  `closesocket` **перед** join; (в) неблокуючий connect+`select` із таймаутом, потім
  перевірити `SO_ERROR` і повернути сокет у blocking для reader; (г) видалити серверний
  режим (конструктор `TransportTCP(int,int)`, `StartServer`, `AcceptThreadFunction`,
  `m_serverSocket`, `m_isServer`, accept-потік); (д) `Send` — all-or-error цикл.
- **`TransportWSClient`** (`Transport_WSClient.cpp`): (а) додати виклик
  `disableAutomaticReconnection()` у конструкторі (зараз НЕ викликається — grep порожній;
  ix default `true`); (б) `Open()` повертає `true` одразу після `start()` (`:70`) — сесія
  вважає конект успішним лише за `state(true)` (§5); (в) реконект: вести окремий
  `m_started` і завжди `stop()` для запущеного об'єкта, навіть коли `m_isOpen/m_isConnecting`
  вже `false` (`:254-255,282-283`) — інакше повторний `Open` не спрацює; перестворювати
  `ix::WebSocket` не треба (ix скидає stop-стан після join).
- Спільне: усі три довести до **контракту `Close()`** з §4.1 (ідемпотентність,
  «після Close колбеків нема», єдиний `state(false)`).

### 9.2 Видалення + синхронізація

- **Видалити** `Transport_WSServer.{h,cpp}` (мертвий) і серверний режим TCP (§9.1г).
- **Видалити старий непрацездатний драйвер ECRPrivatJSON**: `src/components/AddinECRPrivatJSON.{h,cpp}`,
  `src/protocols/ECRPrivatJSON/*`, `src/helpers/ECRPrivatJSON/*` (+ мертві
  `CreateTransport`/`SetTransport`, дві буферні підсистеми, `stod→string`). Grep-перевірка
  (аудит підтвердив): поза власними файлами `TransportWSServer`/TCP-сервер не
  інстанціюються; єдиний зовнішній `TransportTCP` — клієнт у ECR-коді, що видаляється
  (`ECRPrivatJSON_Connection.cpp:203`); `native_host` створює лише `AddinUAPKIConnect`
  (`tests/native_host.cpp:165`); `manifest.xml`/`.def` правок не потребують.
- **CMake** (`components.cmake`): прибрати в `HEADER_FILES` рядки 29,31,35,37-38;
  `SOURCE_FILES` 55,57-63,67,69-74; `Transport_WSServer` з object-цілі 146-147; цілі ECR
  150-228, їх залежності/лінк 297-310, `$<TARGET_OBJECTS:…>` 340-342. Додати ціль
  `wire_component` (ITransport-реалізації + IFramer + IFrameClassifier + DeviceSession).
  Якщо перейменовуємо `transport_component→wire_component` — оновити згадки 136-148,291,
  313-329,337 і **`CMake/compiler_settings.cmake:29`** (інакше configure звернеться до
  неіснуючої цілі).
- **Синхронізувати доки** (пропущено в ред.1): `docs/architecture/README.md:20`,
  `core.md:85`, `build-and-packaging.md:27`, `AGENTS.md:10` — прибрати ECR як наявний.
- DLL після задачі: `TestComponent` + wire-об'єкти; `AddinUAPKIConnect` — **лише** при
  `BUILD_WITH_UAPKI=ON` (`components.cmake:232,345`).

## 10. Тести (без обладнання)

Нова ціль **`wire_selftest`** (свій раннер, без gtest). Лінкування (аудит):
`$<TARGET_OBJECTS:wire_component>` + `$<TARGET_OBJECTS:helpers_component>` +
`$<TARGET_OBJECTS:base_component>` (транспорти кличуть `ServiceTools`) + link
`spdlog::spdlog`, `ixwebsocket`, `ws2_32`; `_WINDOWS UNICODE _UNICODE`, `/utf-8`.
**Оголосити `wire_selftest` ДО `return()`** у `tests/CMakeLists.txt` (гейт після
core_selftest уже знято на `WIN32`; `:54` — UAPKI-return нижче).

1. **`NullTerminatedFramer` — байтові вектори** (без IO): один кадр; split посередині;
   кілька в чанку; порожній кадр; провідний `0x00` окремим чанком; провідний `0x00`+JSON
   одним чанком; звичайний кадр після handshake; `Wrap` з/без `leadingDelimiter`; `Reset`;
   переповнення `maxBufferedBytes`. Вектори — з hex-прикладів спеки (`:92,113,153,160`).
2. **`DeviceSession` над `LoopbackTransport`** (подвійник `ITransport`, що доставляє
   `DataReceived`/`ConnectionState` **з окремого worker-потоку** — щоб відтворити
   реальний асинхронний контракт; детермінізм — через черги+CV/бар'єри, БЕЗ `sleep_for`:
   тест запускає `Request`, чекає факту `Send`, inject-ить кадр, join). Кейси:
   - `RequestPrimary` → `PrimaryResponse` → `{Response}`;
   - **response-before-wait** (відповідь інжектнута до входу в `wait` — не втратити);
   - `deviceBusy` → `{Busy}` НЕГАЙНО; `methodNotImplemented` → `{Unsupported}`;
   - unsolicited → у хендлер (на dispatcher), не в `Request`;
   - **паралельно primary+service** (service проходить під час активного primary);
   - другий primary під час активного → `{Concurrent}`;
   - таймаут primary → `{Timeout}` + desync + реконект; **stale-відповідь після таймауту**
     не матчиться на наступний primary;
   - обрив під час in-flight → `{Disconnected}` + реконект; initial-Open-failure будить супервізор;
   - `Stop()` під час pending → `{Stopped}`; **callback-quiescence після `Close`**;
   - **реентрантний `Request`/`Stop` з unsolicited-хендлера** — не зависає (dispatcher);
   - stop супервізора під час backoff; wire-trace ловить in/out.
3. **Реальні транспорти — смоук:** `TransportTCP` авто проти in-process localhost-echo
   (`bind(0)`+`getsockname`, readiness-бар'єр, `shutdown` перед join); `TransportCOM` —
   ручний проти `com0com` (CI SKIP); `TransportWSClient` — ручний (повна валідація — у
   драйверній фазі проти емулятора). Репрод-тест WS `Close/Open`.
4. **`run_tests.ps1`**: додати `$WireSelftestExe` і рівень **L0.6** після L0.5, а також
   **режим без UAPKI** (параметр): у ньому build перевіряє `core_selftest`+`wire_selftest`,
   а provider/L1/L2/L3 → SKIP і **НЕ форсують** `BUILD_WITH_UAPKI=ON` (зараз `:225` форсує;
   `:135` = FAIL при відсутньому провайдері).

Принцип (урок обох попередніх спроб): жодних тестів проти вигаданого wire-формату;
вектори — з hex-прикладів спеки або реального round-trip.

## 11. Структура файлів

```
src/transport/
  Transport.h                # ITransport (уточнений контракт §4.1)
  Transport_COM.{h,cpp}      # ФІКСИ §9.1 (CreateFileW, Close, reader-state)
  Transport_TCP.{h,cpp}      # ФІКСИ §9.1 (Close-order, nonblock connect, all-or-error) + прибрати сервер
  Transport_WSClient.{h,cpp} # ФІКСИ §9.1 (disableAutoReconnect, m_started, state-based Open)
  IFramer.h                  # НОВЕ (+FrameOptions)
  NullTerminatedFramer.{h,cpp} # НОВЕ (+framerMutex, maxBufferedBytes)
  IFrameClassifier.h         # НОВЕ (заміна IFrameCorrelator)
  DeviceSession.{h,cpp}      # НОВЕ — ядро
  (видалити) Transport_WSServer.{h,cpp}
tests/  wire_selftest.cpp    # НОВЕ (+ LoopbackTransport з worker-потоком)
CMake/  components.cmake (wire_component; §9.2) · compiler_settings.cmake (:29)
run_tests.ps1                # L0.6 + режим без UAPKI
docs/architecture/*, AGENTS.md  # синхронізація (§9.2)
```

## 12. Критерії приймання

- `wire_selftest` зелений (усі кейси §10.1-2 + TCP-echo §10.3); COM/WS смоуки SKIP керовано.
- **Режим без UAPKI**: `run_tests.ps1` збирає+ганяє `core_selftest`+`wire_selftest` без
  `-WithUAPKI`, UAPKI-рівні SKIP.
- `build_project.ps1 -WithUAPKI -WithTests` зелений x86/x64; повний `run_tests.ps1`
  без регресій UAPKI (L0-L3) + L0.6.
- ECR-драйвер, WSServer, TCP-сервер видалені; доки синхронізовані; UAPKI незачеплений.
- Транспортні фікси §9.1 покриті смоук/loopback-тестами (Close-quiescence, all-or-error,
  WS reopen).

## 13. Ризики

| Ризик | Пом'якшення |
|---|---|
| Гонки/дедлоки (reader/caller/supervisor/dispatcher) | Чіткі ролі потоків §6; user-колбеки лише на dispatcher; `m_` не тримається під Send/Close/колбеків; стрес-тести (reentrant, quiescence) |
| Класифікатор/дві доріжки — складність не в тій фазі | Механізм (лейни, класифікація) generic і потрібен уже в сесії; Privat-логіка (`method`/`msgType`) — драйверна фаза; тести на подвійниках |
| Фікси транспортів ширші за очікуване | Кожен фікс — малий і локальний; покривається loopback/смоук; робочого коду немає — рефактор вільний |
| desync-на-тайм-аут ускладнює драйвер | Це вимога протоколу (нема ID запиту); сесія лише сигналить desync, відновлення — драйвер проти емулятора |
| Форма сесії не підійде рушію операцій | Шов вузький (`RequestPrimary/Service`+RequestResult+unsolicited); рушій — споживач із worker-потоку |
