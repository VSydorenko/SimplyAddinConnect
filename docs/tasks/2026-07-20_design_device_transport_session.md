# Device-facing ядро: ITransport + IFramer + DeviceSession — дизайн (ред. 3)

*Дата: 2026-07-20. Статус: ред. 3 після 2 локальних Codex-аудитів (гілка `device-core`).*
*Ред. 3 усуває власні суперечності ред. 2 (єдиний порядок TCP `Close`; `RequestStatus`
у спільному заголовку; живі config-дефолти), уточнює desync/teardown/precedence, і
виносить дрібнозернисті реалізаційні деталі в §14 (інваріанти для TDD) та обмеження
класифікатора Привату в §15 (драйверна фаза) — замість доспецифікації кожної нитки в прозі.*
*База: `docs/tasks/2026-07-19_platform_architecture_design.md`, `docs/ECR_Privat_JSON_Protokol.md`,
Етап 0 (`2026-07-20_plan_etap0_core.md`).*

---

## 1. Мета й обсяг

Переюзний device-facing фундамент драйверів: «байтовий транспорт → кадрування →
класифікація → сесія запит/відповідь», повністю тестований без обладнання.

**У обсязі:** `ITransport` (контракт §4.1 + реальні фікси наявних транспортів §9.1);
`IFramer`+`NullTerminatedFramer`; `IFrameClassifier`; `DeviceSession` (reader, дві
доріжки `RequestPrimary`/`RequestService`, `RequestResult`, unsolicited через
dispatcher, реконект-супервізор, desync-логіка, wire-трейс); `wire_selftest`+
`LoopbackTransport`; прибирання коду + синхронізація CMake/доків.

**Поза обсягом (наступні фази):** рушій async-операцій (`JobEngine`), база
`DeviceDriverComponent`, `PrivatDriver` (+ конкретна Privat-логіка класифікатора, §15),
BPOS1/POSAPI. **Фабрика/парсер рядка підключення** — драйверна фаза; тут тести й caller
створюють транспорт прямими конструкторами.

## 2. Що лагодить

Хендшейк Привату колись не пішов = **баг №1** (розірваний прийом): очікувач висів на
CV, яку ніхто не сигналив. `DeviceSession` усуває структурно (єдиний власник байтового
потоку, прийом і очікування на спільних `mutex`/`cv`). Додатково: **самі транспорти
теж мають реальні баги** (витоки, можливий hang, WS-реконект, часткова відправка) —
§9.1.

## 3. Архітектура

```
  DeviceSession
   reader→framer→classifier→{primary|service|rejectPrimary|rejectService|unsolicited}
   RequestPrimary/RequestService → RequestResult ; dispatcher-потік user-колбеків ; супервізор
      │IFramer            │IFrameClassifier              │ITransport
  NullTerminatedFramer   (Privat: §15 — драйв.фаза)   TransportCOM/_TCP/_WSClient(+фікси §9.1)
   +framerMutex          тести: подвійники             + LoopbackTransport(тест)
```

**Правило потоків (наскрізне):** транспорт → внутрішні `OnBytes`/`OnTransportState`
(стан під `m_`); **усі user-колбеки (unsolicited/state/trace) — лише на dispatcher-потоці**,
ніколи під `m_`, ніколи на reader/caller/supervisor-потоці.

## 4. Компоненти й інтерфейси

### 4.0 Спільні типи (`src/transport/RequestTypes.h`)

Щоб класифікатор не залежав від сесії (аудит), статус-enum — окремий заголовок:

```cpp
enum class RequestStatus {
    Response,        // frame валідний
    Busy,            // deviceBusy
    Unsupported,     // methodNotImplemented
    Timeout,
    Disconnected,    // обрив під час in-flight
    SendFailed,
    Stopped,         // Stop()/деструктор
    Concurrent,      // друга операція тієї ж доріжки
    Desynchronized   // сесія в desync (див. §7), новий primary заборонено
};
struct RequestResult { RequestStatus status; std::vector<uint8_t> frame; };
```

### 4.1 `ITransport` (еволюція `src/transport/Transport.h`) — контракт

```cpp
class ITransport {
public:
    using DataReceivedCallback    = std::function<void(const std::vector<uint8_t>&)>;
    using ErrorCallback           = std::function<void(const std::string&, int)>;
    using ConnectionStateCallback = std::function<void(bool)>;
    virtual ~ITransport() = default;

    virtual bool Open() = 0;   // синхронна спроба; для WS true=start ініційовано,
                               // фактичний конект — лише через ConnectionState(true)
    virtual bool Close() = 0;  // див. КОНТРАКТ нижче
    virtual bool IsOpen() const = 0;
    virtual int  Send(const std::vector<uint8_t>&) = 0;  // ALL-OR-ERROR: ==size | <0
    virtual void SetDataReceivedCallback(DataReceivedCallback) = 0;
    virtual void SetErrorCallback(ErrorCallback) = 0;
    virtual void SetConnectionStateCallback(ConnectionStateCallback) = 0;
};
```

**Контракт `Close()` (єдиний, несуперечливий):**
1. Ідемпотентний; ЗАВЖДИ звільняє ресурси за їх валідністю (handle/socket/thread), НЕ
   за прапорцем `m_isOpen`.
2. **Синхронізований із `Send`** (бере той самий `m_sendMutex`/lifecycle-guard) — щоб не
   закрити дескриптор посеред активної відправки.
3. Порядок для сокетів (розблокувати `recv` у reader ДО join): atomically
   `exchange(m_socket, INVALID_SOCKET)` → `shutdown(SD_BOTH)` копії → `closesocket` копії →
   **потім** `join` reader-потоку. (Аналогічно COM: скинути handle → перервати
   read-цикл → join.)
4. Після повернення `Close()` жоден колбек (data/error/state) більше НЕ почнеться.
5. `state(false)` генерується **рівно один раз** на розрив (чи то з reader при
   remote-close, чи то з `Close`, але не двічі).

**Модель потоків колбеків (частина контракту):** `DataReceived` — reader-потік;
`ConnectionState` — РІЗНІ потоки (caller при `Open`/локальному `Close`; reader при
remote-disconnect; ix-worker для WS); `Error` — reader/caller. Колбеки не з-під
внутрішнього локу транспорту.

### 4.2 `IFramer` + `NullTerminatedFramer`

```cpp
struct FrameOptions { bool leadingDelimiter = false; };   // per-request (лише Wrap)
class IFramer {
public:
    virtual ~IFramer() = default;
    virtual void Feed(const std::vector<uint8_t>& chunk,
                      std::vector<std::vector<uint8_t>>& out) = 0;
    virtual std::vector<uint8_t> Wrap(const std::vector<uint8_t>& payload,
                                      FrameOptions opts = {}) = 0;
    virtual void Reset() = 0;
};
```

`NullTerminatedFramer`: `Feed` stateful, ріже по `0x00`, термінатор відкидається,
порожні кадри (провідний/подвійний `0x00`) ігноруються (тож вхідний провідний `0x00`
не породжує фальшивого unsolicited). `Wrap`: `payload+0x00`; за `leadingDelimiter` —
`0x00+payload+0x00` (лише PingDevice-хендшейк). `Reset` — при реконекті.
**Синхронізація:** власний `framerMutex_` (Feed/Wrap/Reset із різних потоків).
**Переповнення:** ліміт `maxBufferedBytes`; при перевищенні — `Reset()` буфера +
повідомлення про framing-error (постумова: наступний валідний кадр обробляється
нормально, буфер не «отруєний»).

### 4.3 `IFrameClassifier` — класифікація вхідного кадру

```cpp
enum class FrameClass {
    PrimaryResponse, ServiceResponse,
    RejectPrimary, RejectService,   // напр. deviceBusy/methodNotImplemented
    RejectBoth,                     // неоднозначний reject при обох pending → + desync
    Unsolicited
};
enum class RejectReason { Busy, Unsupported };     // classifier-local, не RequestStatus
struct PendingView { const std::vector<uint8_t>* primary; const std::vector<uint8_t>* service; };
struct Classification { FrameClass cls; RejectReason reason; };

class IFrameClassifier {
public:
    virtual ~IFrameClassifier() = default;
    virtual Classification Classify(const PendingView& pending,
                                    const std::vector<uint8_t>& frame) = 0;
};
```

Сесія мапить `RejectReason`→`RequestStatus` (Busy/Unsupported). `RejectBoth` завершує
обидві доріжки й переводить сесію в desync (немає причинного ID, щоб знати, який запит
відхилено). Конкретна Privat-логіка (deviceBusy/methodNotImplemented/identify за
response-полями та `msgType`) — **драйверна фаза, обмеження §15**. Тести цієї фази —
подвійники (`EchoClassifier` + програмовані на кожен `FrameClass`).

### 4.4 `DeviceSession`

```cpp
struct SessionConfig {
    int  primaryTimeoutMs = 30000;   // ВИКОРИСТОВУЄТЬСЯ, коли Request timeout не заданий
    int  serviceTimeoutMs = 5000;
    bool autoReconnect    = true;
    int  reconnectDelayMs = 1000, reconnectMaxDelayMs = 15000, reconnectMaxTries = 0;
    int  connectDeadlineMs = 10000;  // очікування state(true) як успіху конекту
    size_t maxBufferedBytes = 1<<20;
};

class DeviceSession {
public:
    DeviceSession(std::unique_ptr<ITransport>, std::unique_ptr<IFramer>,
                  std::unique_ptr<IFrameClassifier>, SessionConfig cfg = {});
    ~DeviceSession();

    bool Start();
    void Stop();                    // безпечний з БУДЬ-ЯКОГО потоку, у т.ч. dispatcher (§6)
    bool IsConnected() const;

    // timeoutMs<0 → cfg.primaryTimeoutMs/serviceTimeoutMs (дефолти тепер живі).
    RequestResult RequestPrimary(const std::vector<uint8_t>& payload,
                                 int timeoutMs = -1, FrameOptions = {});
    RequestResult RequestService(const std::vector<uint8_t>& payload,
                                 int timeoutMs = -1, FrameOptions = {});

    // Драйвер знімає desync ПІСЛЯ протокольного відновлення результату транзакції.
    void MarkSynchronized();
    bool IsDesynchronized() const;

    // Setters — ЛИШЕ до Start() (уникнення гонки з dispatcher); після Start — no-op+WARN.
    void SetUnsolicitedHandler(std::function<void(std::vector<uint8_t>)>);
    void SetConnectionStateHandler(std::function<void(bool)>);
    void SetWireTraceHandler(std::function<void(bool /*sendAttempt*/, std::vector<uint8_t>)>);
private:
    void OnBytes(const std::vector<uint8_t>&);
    void OnTransportState(bool);
    void OnTransportError(const std::string&, int);
    void ReconnectLoop();
    void DispatchLoop();
    // m_, cv_; framerMutex_; dispatchQueue_+dispatchCv_
    // pendingPrimary/pendingService; connected_, stopping_, desynchronized_, reconnectRequested_
};
```

Доріжки: **primary** серіалізований (один in-flight); **service** серіалізований серед
service, але виконується **паралельно** з primary (спека: ServiceMessage async,
`ECR_Privat_JSON_Protokol.md:303`). Пріоритет `interrupt` над polling-статусом — не тут,
а в JobEngine (§15).

## 5. Потоки даних

**Відправка (`RequestPrimary`/`RequestService`):**
1. Під `m_`: перевірити `stopping_`(→`Stopped`), `connected_`(→`Disconnected`),
   для primary — `desynchronized_`(→`Desynchronized`), відсутність іншого запиту доріжки
   (→`Concurrent`); зарезервувати pending (payload + `epoch` + місце під результат). Без
   мережі, якщо відмова.
2. **Unlock `m_`** → `Wrap` (під `framerMutex_`) → wire-trace(sendAttempt) → `transport_->Send()`.
3. Під `m_`: **precedence** — якщо `Send<0` І pending ЩЕ належить цьому запиту й не
   завершений іншим шляхом (напр. `state(false)`, викликаний Send'ом синхронно —
   `Transport_TCP.cpp:371`), тоді `SendFailed`; інакше повернути вже збережений результат
   (`Disconnected`).
4. `cv_.wait_for(lock, timeout, pred = результат готовий || stopping_ || !connected_)`.
5. Повернути `RequestResult`.

**Прийом (`OnBytes`, reader-потік):** wire-trace(in) у dispatch-чергу (FIFO ДО unsolicited
того ж чанку) → під `framerMutex_` `Feed` (винятки ловляться тут) → для кожного кадру
під `m_` `Classify(pendingView, frame)`: `PrimaryResponse`/`ServiceResponse` → у pending
+ `notify`; `RejectPrimary`/`RejectService` → завершити відповідну доріжку
(`Busy`/`Unsupported`); `RejectBoth` → обидві + `desynchronized_=true`+`reconnectRequested_`;
`Unsolicited` → кадр у dispatch-чергу. Жодного user-колбека під `m_`.

**Реконект/desync:** будь-що, що ставить `reconnectRequested_=true` (обрив `state(false)`,
таймаут primary, `RejectBoth`), будить супервізор. Предикат супервізора —
`desiredUp && (reconnectRequested_ || !connected_)` (НЕ лише `!connected_` — після
таймауту `connected_` лишається true). Супервізор: завершити pending `Disconnected`,
`framer_->Reset()`, backoff, `Close()`+`Open()`, успіх — лише `state(true)` у межах
`connectDeadlineMs`; **`desynchronized_` НЕ знімається автоматично** — лише `MarkSynchronized()`
драйвером після протокольного відновлення.

## 6. Модель потоків і teardown

Ролі: потік-виклик (`Request`, чекає на `cv_`); reader-потік (OnBytes, без user-коду);
супервізор реконекту; **dispatcher-потік** (усі user-колбеки з черги — усуває реентрантний
`Request` і `Stop` з user-колбека). Локи: `m_` (pending/стан, коротко, ніколи під
Send/Close/колбеків); `framerMutex_`; `dispatchCv_`.

**`Stop()` (безпечний з будь-якого потоку, у т.ч. dispatcher):**
1. Під `m_`: `stopping_=true`; усі pending → `Stopped`; `cv_.notify_all`.
2. Розбудити+join супервізор (без `m_`).
3. `transport_->Close()` (без `m_`; контракт §4.1 — колбеків далі нема).
4. Від'єднати транспортні колбеки (`Set*Callback(nullptr)`).
5. Зупинити dispatcher: якщо `Stop()` викликано **НЕ** з dispatcher-потоку — join; якщо
   **З** dispatcher-потоку (user-колбек викликав `Stop`) — **не self-join**: позначити
   dispatcher на завершення після повернення поточного колбека, а фінальний join зробити
   в `~DeviceSession` (з іншого потоку). Прапорець `stopping_` робить крок ідемпотентним.
6. `framer_->Reset()`.
`~DeviceSession` викликає `Stop()` (ідемпотентний) і гарантує фінальний join dispatcher.

## 7. Обробка помилок / результат

- `Send<0` → `SendFailed` (з precedence §5.3).
- Таймаут **primary** → `Timeout` + `desynchronized_` + `reconnectRequested_` (бо нема ID
  запиту: пізня відповідь могла б зматчитись на наступний primary). Знімається лише
  `MarkSynchronized()`. Новий primary у desync → `Desynchronized`; **service дозволено**
  (для відновлення). Таймаут **service** — `Timeout` без desync, але з карантином
  дискримінатора цього service до наступного кадру/реконекту (щоб пізній дубль не
  завершив новий service; §14).
- Обрив → усі pending `Disconnected` + реконект.
- `RejectPrimary`/`RejectService` → `Busy`/`Unsupported` негайно. `RejectBoth` → обидві +
  desync.
- Винятки framer/classifier — у `OnBytes`, лог WARN, кадр відкинуто, сесія живе.
- Винятки user-колбека — на dispatcher, лог WARN. Немає `throw` через межу API.

## 8. Wire-трейс

`SetWireTraceHandler(sendAttempt, bytes)` — байти на межі `ITransport` (для WS це
payload, не сирі WebSocket-кадри). `sendAttempt=true` — **спроба** відправки (ставиться
в чергу ДО `Send`, тож присутня навіть при `SendFailed`); `false` — прийнятий чанк.
Порядок у dispatch-черзі FIFO: incoming-trace чанку — перед unsolicited того ж чанку.
Логер драйвера форматує в hex за `trace.log` (`ECR_Privat_JSON_Protokol.md:2335`).

## 9. Наявні транспорти й прибирання

### 9.1 Реальні фікси (підтверджені по коду)

- **`TransportCOM`**: `CreateFileA`→`CreateFileW` (`:41`); `Close` cleanup за валідністю
  `m_portHandle`, не прапорця (`:103`); reader при неусувній помилці → `state(false)`+будити
  супервізор (`:~439`); **all-or-error `Send`**: `WriteFile` при partial write зараз лише
  WARN і повертає `bytesWritten<size` як «успіх» (`:360-372`) — цикл дозапису або трактувати
  partial як `<0`; прибрати мертвий `#include <winsock2.h>`.
- **`TransportTCP`**: `Close` cleanup завжди (`:283`); **єдиний порядок** (§4.1) — swap
  socket→INVALID, `shutdown`, `closesocket`, потім join (`:293-305`,`:490`); **`Close`
  синхронізувати з `m_sendMutex`** (Send його тримає `:353`, Close — ні); неблокуючий
  connect+`select`+`SO_ERROR`+повернення в blocking; **обмежити DNS**: `getaddrinfo`
  (`:102`) блокує до select — вимагати numeric IP або cancellable `GetAddrInfoExW` із
  deadline; `Send` all-or-error цикл; видалити серверний режим (`TransportTCP(int,int)`,
  `StartServer`, `AcceptThreadFunction`, `m_serverSocket`, `m_isServer`).
- **`TransportWSClient`**: додати `disableAutomaticReconnection()` у конструкторі (зараз
  НЕ викликається; ix default `true`); `Open` успіх — лише за `state(true)` (`:70`);
  окремий `m_started` і `stop()` навіть коли `m_isOpen/m_isConnecting==false` (`:254-255,
  282-283`); перестворювати `ix::WebSocket` не треба.
- Спільне: усі три довести до **контракту `Close()` §4.1** (ідемпотентність, Send-sync,
  правильний порядок, «після Close колбеків нема», єдиний `state(false)`).

### 9.2 Видалення + синхронізація

- Видалити `Transport_WSServer.{h,cpp}` + серверний режим TCP.
- Видалити старий ECR-драйвер: `src/components/AddinECRPrivatJSON.{h,cpp}`,
  `src/protocols/ECRPrivatJSON/*`, `src/helpers/ECRPrivatJSON/*` (+ мертві `CreateTransport`/
  `SetTransport`, дві буферні підсистеми, `stod→string`). Grep підтвердив (аудит): поза
  власними файлами WSServer/TCP-сервер не інстанціюються; єдиний зовнішній `TransportTCP` —
  клієнт у ECR (`ECRPrivatJSON_Connection.cpp:203`), що видаляється; `native_host` створює
  лише `AddinUAPKIConnect` (`tests/native_host.cpp:165`); `manifest.xml`/`.def` не чіпати.
- **CMake `components.cmake`**: прибрати `HEADER_FILES` 29,31,35,37-38; `SOURCE_FILES`
  55,57-63,67,69-74; `Transport_WSServer` 146-147; цілі ECR 150-228 + залежності/лінк
  297-310 + `$<TARGET_OBJECTS>` 340-342. Додати `wire_component`. При перейменуванні
  `transport_component→wire_component` — оновити 136-148,291,313-329,337 і
  `CMake/compiler_settings.cmake:29`.
- Синхронізувати доки: `docs/architecture/README.md:20`, `core.md:85`,
  `build-and-packaging.md:27`, `AGENTS.md:10`.
- DLL після: `TestComponent`+wire; `AddinUAPKIConnect` лише при `BUILD_WITH_UAPKI=ON`.

## 10. Тести

Ціль **`wire_selftest`** (свій раннер, без gtest). CMake — **скопіювати properties з
core_selftest** (`tests/CMakeLists.txt:28-47`): `OUTPUT_NAME "wire_selftest${_TEST_ARCH_SUFFIX}"`,
`CXX_STANDARD 17`, include dirs, `_WINDOWS UNICODE _UNICODE`, `/utf-8`; лінк
`$<TARGET_OBJECTS:wire_component/helpers_component/base_component>` + `spdlog::spdlog`+
`ixwebsocket`+`ws2_32`. Оголосити **ДО** UAPKI-`return()` (`:54`).

1. **Framer — байтові вектори** (§10 ред.2) + переповнення з перевіркою відновлення.
2. **`DeviceSession` над `LoopbackTransport`** — подвійник із **двома режимами доставки
   state**: worker-потік (async) І синхронний з `Open/Close` (контракт §4.1 дозволяє
   обидва). Детермінізм — черги+CV+watchdog, без `sleep`. Кейси: Response;
   response-before-wait; deviceBusy→Busy негайно; methodNotImplemented→Unsupported;
   RejectBoth→обидві+desync; unsolicited→dispatcher; primary+service паралельно; другий
   primary→Concurrent; таймаут primary→Timeout+desync (+ stale-frame після нового
   `state(true)` НЕ матчиться); service-timeout+карантин дубля; обрив→Disconnected+реконект;
   initial-Open-failure будить супервізор; `Stop` під час pending→Stopped;
   callback-quiescence після Close; **реентрантні `Request`/`Stop` з unsolicited** (dispatcher,
   watchdog); SendFailed-vs-Disconnected precedence; WS `Close/Open` repro.
3. **Реальні транспорти — смоук:** `TransportTCP` авто (localhost-echo, `bind(0)`+
   `getsockname`, readiness-бар'єр); **injectable write-seam** для partial-send (echo не
   змусить partial); `TransportCOM` — `com0com` (CI SKIP); `TransportWSClient` + локальний
   ws-echo для Close/Open/exact-once-state (за можливості авто, інакше ручний).
4. **`run_tests.ps1`**: рівень **L0.6** + **режим без UAPKI** — окремий `$WireSelftestExe`,
   mode-specific `$haveExes`/build/gates, у якому provider/L1/L2/L3 → SKIP і НЕ форсують
   `BUILD_WITH_UAPKI` (зараз `:225` форсує, `:135` = FAIL до build).

Принцип: жодних тестів проти вигаданого wire-формату.

## 11. Структура файлів

```
src/transport/  RequestTypes.h(нове) · Transport.h(§4.1) · Transport_COM/_TCP/_WSClient.{h,cpp}(фікси §9.1)
                IFramer.h · NullTerminatedFramer.{h,cpp} · IFrameClassifier.h · DeviceSession.{h,cpp}  (нове)
                (видалити) Transport_WSServer.{h,cpp}
tests/ wire_selftest.cpp(+LoopbackTransport 2 режими)   CMake/ components.cmake · compiler_settings.cmake:29
run_tests.ps1(L0.6+режим без UAPKI)   docs/architecture/*, AGENTS.md(синхр.)
```

## 12. Критерії приймання

- `wire_selftest` зелений (framer + DeviceSession-over-loopback усі кейси §10.2 +
  TCP-echo + partial-send через write-seam); COM/WS — керований SKIP або локальний ws-echo.
- Режим без UAPKI: `run_tests.ps1` збирає+ганяє `core_selftest`+`wire_selftest` без
  `-WithUAPKI` (UAPKI-рівні SKIP).
- `build_project.ps1 -WithUAPKI -WithTests` зелений x86/x64; повний `run_tests.ps1` без
  регресій UAPKI (L0-L3)+L0.6. ECR/WSServer/TCP-сервер видалені; доки синхронізовані;
  UAPKI незачеплений. Транспортні фікси §9.1 покриті (Close-quiescence, all-or-error,
  WS reopen, exact-once state) — де автотест недосяжний (реальний WS/COM), явно
  задокументований ручний крок, а не мовчазна прогалина.

## 13. Ризики

| Ризик | Пом'якшення |
|---|---|
| Гонки/дедлоки (4 ролі потоків) | Ролі §6; user-колбеки лише dispatcher; `m_` не під Send/Close; стрес/reentrant/quiescence-тести; watchdog |
| Фікси транспортів ширші | Кожен локальний; loopback+injectable seam; робочого коду нема — рефактор вільний |
| desync ускладнює драйвер | Сесія лише сигналить+тримає desync; відновлення (MarkSynchronized) — драйвер проти емулятора |
| Класифікатор — тонкі кейси Привату | Інтерфейс достатньо виразний (Reject*/ambiguity §4.3); Privat-логіка+edge-cases — §15, драйверна фаза |
| Нескінченне доспецифікування дизайну | §14/§15 фіксують інваріанти/обмеження; решта точності — у TDD (тести форсують поведінку), не в прозі |

## 14. Інваріанти для TDD (не доспецифікуються в прозі — форсуються тестами)

Ці властивості реалізація мусить забезпечити; точний механізм — у коді під тест:
- **Немає callback-у в знищену/зупинену сесію**: після `Close()`/`Stop()` — нуль колбеків
  (тест quiescence з watchdog).
- **Немає self-deadlock**: `Request`/`Stop` з unsolicited-хендлера не вішають (dispatcher).
- **Exactly-once `state(false)`** на розрив; `state(true)` — критерій успіху реконекту.
- **Precedence результату**: pending завершується РІВНО одним джерелом (Send-fail vs
  disconnect vs response vs stop) — без подвійного запису.
- **Stale-frame** після таймауту/реконекту не завершує новий запит (epoch/generation guard).
- **framer-overflow**: буфер не «отруюється»; наступний валідний кадр обробляється.
- **Send all-or-error** (COM і TCP) — під injectable write-seam.

## 15. Обмеження класифікатора Привату (драйверна фаза — записано, не реалізується тут)

Коли писатиметься `PrivatClassifier` (проти емулятора каси):
- Розрізняти **запит vs відповідь** не лише за `method`/`msgType`, а за **response-only
  полями** (identify-відповідь має `result/vendor/model`, `ECR_Privat_JSON_Protokol.md:2529`;
  статуси мають свої response-поля) — інакше чужий `ServiceMessage` хибно зматчиться.
- `deviceBusy`(`:2187`)→`RejectPrimary/Busy`; `methodNotImplemented`(`:2244`)→`Reject*/Unsupported`
  (може стосуватися primary АБО service; при обох pending без причинного ID → `RejectBoth`).
- Службові мапінги запит→відповідь: `interrupt→interruptTransmitted`(`:2213`),
  `correctTransaction→correctionTransmitted`(`:2633`), `debug→debugOn/Off`(`:2372`);
  невідомий `ServiceMessage` НЕ завершує service-pending → `Unsolicited`.
- **Нормалізація літералів**: у спеці `msgType` містять провідні/кінцеві пробіли
  (`" interruptTransmitted"` `:2221`, `" methodNotImplemented"` `:2251`) — ASCII-trim перед
  порівнянням; вектори — з буквальних прикладів.
- **Пріоритет `interrupt`** над polling-статусом (spec вимагає interrupt під час Purchase,
  `:2201`) — політика JobEngine: зупинити polling, звільнити service-доріжку, повторити
  interrupt (не проблема сесії).
