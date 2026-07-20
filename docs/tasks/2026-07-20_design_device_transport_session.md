# Device-facing ядро: ITransport + IFramer + DeviceSession — дизайн

*Дата: 2026-07-20. Статус: дизайн на рев'ю. Гілка розробки — окрема (напр. `device-core`).*
*Спирається на: `docs/tasks/2026-07-19_platform_architecture_design.md` (загальна платформа),
`docs/ECR_Privat_JSON_Protokol.md` (модель з'єднання/сесії), виконаний Етап 0
(`docs/tasks/2026-07-20_plan_etap0_core.md` — ядро-міст 1С).*

---

## 1. Мета й обсяг

Побудувати **device-facing фундамент** — переюзний для всіх майбутніх драйверів
(ECRPrivatJSON, ECRCommXBPOS1, POSAPI) шар «байтовий транспорт → кадрування →
сесія запит/відповідь». Це та частина, форма якої певна вже зараз і яка
**повністю тестується без обладнання** (loopback), на відміну від async-рушія
операцій і самих драйверів, які проєктуються пізніше проти реальних емуляторів каси.

**У обсязі цієї задачі:**
- `ITransport` — чистий байтовий контракт + приведення наявних `TransportCOM`/
  `TransportTCP`/`TransportWSClient` до нього (робочі Win32-нутрощі зберігаємо).
- `IFramer` + `NullTerminatedFramer` — плагінне кадрування.
- `IFrameCorrelator` — плагінна кореляція «запит↔відповідь».
- `DeviceSession` — постійна дуплексна сесія: reader-потік, буфер, framer,
  диспетчеризація кадрів, синхронний `Request()` поверх async-каналу, канал
  unsolicited-повідомлень, реконект-супервізор, hook wire-трасування.
- Тестова ціль `wire_selftest` + внутрішньопам'ятний `LoopbackTransport`.
- Прибирання мертвого/непрацездатного коду (див. §9).

**Поза обсягом (наступні фази, валідація проти емуляторів каси):**
- Рушій async-операцій (`JobEngine`: стани, пауза на рішення каси, статуси касиру).
- База `DeviceDriverComponent` (стандартний async-фасад 1С).
- `PrivatDriver` (хендшейк, identify, еталонна схема, операції, маппінг).
- Драйвери BPOS1/POSAPI.

## 2. Навіщо саме зараз і що це лагодить

У проєкті **немає жодного робочого драйвера** — хендшейк ПриватБанку свого часу не
пішов, і проєкт заглух. Діагноз із дослідження: це майже напевно **баг №1** —
розірваний шлях приймання. Хендшейк надсилав запит і чекав відповідь на condition
variable, яку **ніхто не сигналив** (продюсер `ProcessReceivedData` не підключений
до транспорту). Відповідь термінала фізично приходила, але очікувач її не бачив →
вічний таймаут → «хендшейк не працює».

`DeviceSession` **структурно унеможливлює цей клас бага**: приймання (reader-потік)
і очікування (`Request`) живуть в одному об'єкті на спільних `mutex`/`condition_variable`,
з єдиним власником байтового потоку. Тобто це не спекулятивна абстракція, а прямий
ремонт стіни, об яку розбилися обидві попередні спроби.

## 3. Архітектура (в обсязі задачі)

```
              ┌──────────────────────────────────────────────┐
   (пізніше)  │ DeviceDriverComponent : AddInNative           │  ← НЕ в цій задачі
              │  рушій async-операцій, 1С-фасад               │
              └───────────────────┬──────────────────────────┘
                                  │ споживає Request()/unsolicited
  ┌───────────────────────────────▼───────────────────────────┐
  │ DeviceSession                                              │  ← ЦЯ ЗАДАЧА
  │  • єдиний власник ITransport (ставить його колбеки)        │
  │  • reader: OnBytes → framer.Feed → кадри → диспетчер       │
  │  • Request(payload,timeout): Wrap→Send→чекати корельовану  │
  │  • unsolicited-хендлер (deviceBusy, статуси, prompt)       │
  │  • реконект-супервізор; wire-трейс hook                    │
  └───────┬───────────────────┬───────────────────┬───────────┘
          │ IFramer           │ IFrameCorrelator   │ ITransport
  ┌───────▼──────┐   ┌────────▼─────────┐   ┌──────▼───────────────────┐
  │NullTerminated│   │(Privat: за       │   │ TransportCOM / _TCP /     │
  │Framer (0x00) │   │ полем "method")  │   │ _WSClient (Win32/Winsock/ │
  │              │   │ — інтерфейс зараз,│   │ ixwebsocket)              │
  │              │   │ реалізація з     │   │ + LoopbackTransport (тест)│
  │              │   │ драйвером        │   │                           │
  └──────────────┘   └──────────────────┘   └───────────────────────────┘
```

Розділення відповідальностей: **транспорт нічого не знає про формат** (сирі байти),
**framer нічого не знає про транспорт** (байти↔кадри), **корелятор нічого не знає
про мережу** (кадр↔кадр), **сесія зшиває їх** і дає драйверу простий синхронний
`Request` + канал незапитаних повідомлень.

## 4. Компоненти й інтерфейси

### 4.1 `ITransport` (еволюція наявного `src/transport/Transport.h`)

Контракт лишається майже як є (він добрий), з уточненою семантикою й чисткою:

```cpp
class ITransport {
public:
    using DataReceivedCallback     = std::function<void(const std::vector<uint8_t>&)>;
    using ErrorCallback            = std::function<void(const std::string&, int)>;
    using ConnectionStateCallback  = std::function<void(bool /*connected*/)>;

    virtual ~ITransport() = default;
    virtual bool Open() = 0;
    virtual bool Close() = 0;
    virtual bool IsOpen() const = 0;
    virtual int  Send(const std::vector<uint8_t>& data) = 0;   // <0 = помилка

    // ЄДИНИЙ власник — DeviceSession. Колбек викликається на фоновому reader-потоці
    // транспорту (COM/TCP: власний потік; WS: потік ixwebsocket). Документуємо це
    // в контракті (раніше було неявно й породжувало гонки).
    virtual void SetDataReceivedCallback(DataReceivedCallback) = 0;
    virtual void SetErrorCallback(ErrorCallback) = 0;
    virtual void SetConnectionStateCallback(ConnectionStateCallback) = 0;
};
```

- **`TransportCOM`** — зберігаємо як є (реальний `CreateFileW("\\\\.\\COMn")`+DCB+
  `COMMTIMEOUTS`+reader-потік `ReadThreadFunction`, конфіг порту, DTR/RTS). Покриває
  RS-232, USB-CDC (usbserial-міст — саме так підключається USB Привату, `115200 8N1`)
  і Bluetooth-SPP. Дрібна чистка: прибрати мертвий `#include <winsock2.h>` (COM його
  не потребує).
- **`TransportTCP`** — зберігаємо КЛІЄНТСЬКУ гілку (Winsock2, reader-потік). **Видаляємо
  серверний режим**: конструктор `TransportTCP(int port, int maxConnections)`,
  `StartServer`/`AcceptThreadFunction`/`m_serverSocket`/`m_isServer`/accept-потік — не
  потрібні (компонента — завжди клієнт до обладнання). Додаємо неблокуючий connect із
  таймаутом (через `select`) — щоб `Open()` не вис на мертвому хості.
- **`TransportWSClient`** — зберігаємо (ixwebsocket, уже має `SetPingInterval`/
  `SetExtraHeaders`/`SetTimeout`). Вбудований автореконект бібліотеки **вимикаємо**
  (`disableAutomaticReconnection`) — реконектом керує `DeviceSession`, щоб політика
  була єдиною для всіх транспортів.

Рядок підключення (парситься драйвером пізніше, але формат фіксуємо тут):
`"COM3:115200,8,N,1"` | `"tcp://host:2000"` | `"ws://host:3000/path"`.

### 4.2 `IFramer` + `NullTerminatedFramer` (`src/transport/IFramer.h`, `NullTerminatedFramer.{h,cpp}`)

```cpp
class IFramer {
public:
    virtual ~IFramer() = default;
    // Згодувати сирий чанк; додати у out усі повні кадри, що зібралися (0..N).
    virtual void Feed(const std::vector<uint8_t>& chunk,
                      std::vector<std::vector<uint8_t>>& out) = 0;
    // Обгорнути payload у кадр для відправки.
    virtual std::vector<uint8_t> Wrap(const std::vector<uint8_t>& payload) = 0;
    // Скинути внутрішній буфер (при реконекті/розсинхронізації).
    virtual void Reset() = 0;
};
```

`NullTerminatedFramer` (ECR ПриватБанк):
- `Feed`: накопичує в буфер, ріже по `0x00`, кожен шматок до термінатора → кадр
  (термінатор відкидається), решта лишається в буфері. Порожні кадри (подвійний `0x00`)
  ігноруються.
- `Wrap`: `payload + 0x00`. Прапорець `leadingZeroForHandshake` (конструктор) додає
  провідний `0x00` — потрібен лише для `PingDevice` хендшейку (`docs/ECR_Privat_JSON_Protokol.md:87-92`).
  За замовчуванням `false`.
- `Reset`: очищає буфер.

### 4.3 `IFrameCorrelator` (`src/transport/IFrameCorrelator.h`)

Плагінна відповідь на питання «чи цей вхідний кадр — відповідь на той запит»:

```cpp
class IFrameCorrelator {
public:
    virtual ~IFrameCorrelator() = default;
    // true, якщо incoming — відповідь на requestInFlight.
    virtual bool Matches(const std::vector<uint8_t>& requestInFlight,
                         const std::vector<uint8_t>& incoming) = 0;
};
```

Інтерфейс фіксуємо зараз; Privat-реалізація (парсить JSON обох, порівнює поле
`method`) — у фазі драйвера. Для тестів — `EchoCorrelator` (Matches, якщо байти
рівні) і завжди-`true`/завжди-`false` подвійники.

### 4.4 `DeviceSession` (`src/transport/DeviceSession.{h,cpp}`) — ядро задачі

```cpp
struct SessionConfig {
    int  requestTimeoutMs   = 30000;   // дефолт; операції задають свій
    bool autoReconnect      = true;
    int  reconnectDelayMs   = 1000;    // базова затримка; експоненційний backoff
    int  reconnectMaxDelayMs= 15000;
    int  reconnectMaxTries  = 0;       // 0 = нескінченно (постійний конект зі спеки)
};

class DeviceSession {
public:
    // Володіє транспортом; framer/correlator — стратегії драйвера.
    DeviceSession(std::unique_ptr<ITransport> transport,
                  std::unique_ptr<IFramer> framer,
                  std::unique_ptr<IFrameCorrelator> correlator,
                  SessionConfig cfg = {});
    ~DeviceSession();   // Stop() у деструкторі

    bool Start();       // Open транспорт + запустити диспетчеризацію
    void Stop();        // зупинити реконект, Close, від'єднати колбеки
    bool IsConnected() const;

    // Синхронний запит↔відповідь поверх async-каналу. Блокує потік-виклик,
    // доки корелятор не позначить вхідний кадр як відповідь, або таймаут.
    // Серіалізований: один запит in-flight. Другий Request під час активного
    // повертає nullopt ОДРАЗУ (сесія НЕ чергує; серіалізацію гарантує драйвер,
    // що працює одним worker-потоком). Не з'єднано → теж nullopt.
    std::optional<std::vector<uint8_t>>
        Request(const std::vector<uint8_t>& payload, int timeoutMs);

    // Канал незапитаних кадрів (ServiceMessage/deviceBusy/статуси/prompt).
    // Викликається на reader-потоці — хендлер має бути коротким/неблокуючим.
    void SetUnsolicitedHandler(std::function<void(std::vector<uint8_t>)> cb);

    // Події стану з'єднання (connected/disconnected) — для драйвера/логів.
    void SetConnectionStateHandler(std::function<void(bool)> cb);

    // Wire-трейс: hex-дамп кожного прийнятого/надісланого кадру (§8).
    void SetWireTraceHandler(std::function<void(bool /*outgoing*/,
                                                const std::vector<uint8_t>&)> cb);
private:
    void OnBytes(const std::vector<uint8_t>&);   // єдиний колбек транспорту
    void OnTransportState(bool connected);
    void ReconnectLoop();                        // окремий супервізор-потік
    // transport_, framer_, correlator_, cfg_
    // std::mutex m_; std::condition_variable cv_;
    // pending: payload запиту + місце під відповідь + прапорці
};
```

Ключова інваріанта (та, що лагодить handshake-баг): **єдиний власник байтового
потоку**. Транспорт віддає байти лише в `OnBytes`; `OnBytes` під `mutex` кладе кадр
або в pending-відповідь (`cv_.notify`), або в unsolicited-канал; `Request` чекає на
тій самій `cv_`. Немає другої буферної підсистеми — нема чого розсинхронізувати.

## 5. Потоки даних

**Відправка (потік-виклик драйвера):** `Request(payload, t)` → під `mutex` перевірити,
що немає іншого in-flight (серіалізація — Приват вимагає «один запит за раз»,
`docs/...:303-311`, підтверджено практикою infostart) → зберегти `payload` як pending
→ `framer_->Wrap(payload)` → wire-trace(out) → `transport_->Send()` →
`cv_.wait_for(t)` доки `responseReady` → повернути відповідь або `nullopt` (таймаут).

**Прийом (reader-потік транспорту):** `OnBytes(chunk)` → wire-trace(in, сирий) →
`framer_->Feed(chunk, frames)` → для кожного `frame`: під `mutex` якщо є pending і
`correlator_->Matches(pending, frame)` → зберегти як відповідь, `responseReady=true`,
`cv_.notify_one`; інакше → викликати unsolicited-хендлер (поза `mutex`, щоб не тримати
лок під час користувацького коду).

**Реконект:** `OnTransportState(false)` → якщо `autoReconnect` і не `Stop()` →
розбудити супервізор-потік `ReconnectLoop` → `framer_->Reset()`, backoff, `transport_->
Open()` до успіху/ліміту; pending-`Request` при обриві → `nullopt` (з кодом «обрив»).

## 6. Модель потоків (явно)

- **Потік-виклик** (1С/драйвер): виконує `Request`, блокується на `cv_`.
- **Reader-потік транспорту** (COM/TCP: власний; WS: ix): виконує `OnBytes`,
  диспетчеризацію, unsolicited-хендлер.
- **Супервізор реконекту**: окремий потік, спить, поки з'єднання живе.
- Синхронізація: один `mutex m_` захищає pending-стан і `responseReady`; `cv_` будить
  очікувача. Unsolicited-хендлер і wire-trace викликаються **поза** `m_`. Правило:
  жоден користувацький колбек не викликається під `m_` (проти дедлоків/реентрантності).

## 7. Обробка помилок

- `Send()<0` → `Request` повертає `nullopt` одразу, pending знімається.
- Таймаут → `nullopt`, pending знімається, з'єднання лишається (це не обрив).
- Обрив (`OnTransportState(false)`) під час in-flight → pending → `nullopt`; реконект
  за політикою.
- Виняток у user-колбеку (unsolicited/trace) — ловиться в сесії, логгером у WARN,
  сесія живе далі.
- Немає `throw` через межу; помилки — через `optional`/логи.

## 8. Wire-трейс

`SetWireTraceHandler` дає драйверу/логеру повний hex-дамп кожного кадру (in/out) —
це вимога самого протоколу (§6.6 «debug» у спеці, клієнтський обов'язок) і
підтверджена практикою потреба для діагностики. Формат hex-рядка узгоджуємо з
`trace.log` зі спеки (`docs/ECR_Privat_JSON_Protokol.md:2335-2349`). Сам логер —
на боці драйвера/ServiceTools; сесія лише віддає байти.

## 9. Прибирання коду

Оскільки робочого драйвера немає (хендшейк ніколи не працював), прибираємо
непрацездатне, щоб не тягнути дві моделі приймання:

- **Видалити** `src/transport/Transport_WSServer.{h,cpp}` — мертвий код (ніде не
  інстанціюється), і серверний WS у DLL всередині 1С архітектурно недоречний.
- **Видалити серверні гілки** `TransportTCP` (accept-потік, `StartServer`, серверний
  конструктор).
- **Видалити старий непрацездатний драйвер ECRPrivatJSON** — він і є те, що
  замінюємо; буде відбудований у фазі драйвера на новому фундаменті й валідований
  проти емуляторів: `src/components/AddinECRPrivatJSON.{h,cpp}`,
  `src/protocols/ECRPrivatJSON/*`, `src/helpers/ECRPrivatJSON/*`. Разом із ним —
  мертвий `CreateTransport`/`SetTransport`, дві роз'єднані буферні підсистеми (баг №1),
  тип-мисматч `stod→string`.
- **CMake**: прибрати з `components.cmake` object-цілі `ecr_privat_json_component`/
  `protocols_component`/`privat_json_helper_component` і `Transport_WSServer`; додати
  нову ціль `wire_component` (ITransport-реалізації + IFramer + DeviceSession).
- **Залишаємо незачепленим**: `base_component`, `helpers_component`, `TestComponent`,
  весь UAPKI-стек і його універсальну команду (окрема опція `BUILD_WITH_UAPKI`,
  окремі файли — рефакторинг їх не торкається).

DLL після задачі: `AddinUAPKIConnect` + `TestComponent` + об'єктний код wire-стеку
(ще не 1С-компонента — інфраструктура для майбутнього драйвера, вправляється тестами).

## 10. Стратегія тестування (без обладнання)

Нова консольна ціль **`wire_selftest`** (як `core_selftest`: свій хенд-ролед
раннер, без gtest; лінкує `wire_component`). Рівні:

1. **`NullTerminatedFramer` — байтові вектори (детерміновано, без IO):**
   - один кадр; кадр по частинах (chunk split посередині); кілька кадрів в одному
     чанку; порожній кадр (подвійний `0x00`); `Wrap` без/з провідним `0x00`;
     `Reset` очищає недобудований кадр. Вектори беремо з hex-прикладів спеки
     (PingDevice, Identify — `docs/ECR_Privat_JSON_Protokol.md:92,113,153,160`).
2. **`DeviceSession` над `LoopbackTransport` (детерміновано, керовано):**
   `LoopbackTransport : ITransport` — тестовий подвійник: `Send` кладе байти в чергу,
   тест «зі сторони термінала» скриптує відповіді, які транспорт віддає в колбек.
   Це фейк ТРАНСПОРТУ (байтова труба), НЕ фейк протоколу — механіка сесії справжня.
   Кейси: `Request` отримує корельовану відповідь; unsolicited-кадр іде в хендлер, не
   в `Request`; таймаут (термінал мовчить) → `nullopt`; серіалізація (другий `Request`
   під час in-flight → `nullopt` одразу, без чергування); обрив під час in-flight → `nullopt`
   + реконект (LoopbackTransport емулює `OnTransportState(false)`); wire-trace ловить in/out.
3. **Реальні транспорти — смоук:**
   - `TransportTCP`: **автоматично** проти in-process localhost-echo (bind ephemeral
     порт, приймає, віддає назад framed) — round-trip через `DeviceSession`.
   - `TransportCOM`: смоук проти віртуальної пари `com0com` — **ручний/опційний**
     (потрібен драйвер com0com; у CI SKIP із поясненням).
   - `TransportWSClient`: смоук проти локального ws-echo — **ручний/опційний**;
     повна валідація WS — у фазі драйвера проти емулятора.
4. **Інтеграція в `run_tests.ps1`**: рівень L0.6 (`wire_selftest`) після L0.5
   (`core_selftest`), перед UAPKI-рівнями; проходить без `-WithUAPKI`.

Принцип (урок обох попередніх спроб): **жодних тестів проти вигаданого wire-формату**;
вектори — лише з hex-прикладів спеки або реального round-trip.

## 11. Структура файлів

```
src/transport/
  Transport.h              # ITransport (еволюція: чистка, документація потоків)
  Transport_COM.{h,cpp}    # зберегти (дрібна чистка)
  Transport_TCP.{h,cpp}    # прибрати сервер; неблокуючий connect+select
  Transport_WSClient.{h,cpp}# зберегти; вимкнути внутр. автореконект
  IFramer.h                # НОВЕ
  NullTerminatedFramer.{h,cpp} # НОВЕ
  IFrameCorrelator.h       # НОВЕ
  DeviceSession.{h,cpp}    # НОВЕ — ядро задачі
  (видалити) Transport_WSServer.{h,cpp}
tests/
  wire_selftest.cpp        # НОВЕ (+ LoopbackTransport усередині)
CMake/
  components.cmake         # wire_component; прибрати ECR/WSServer цілі
  dependencies.cmake       # ixwebsocket client-only лишається (Етап 3 звузить до 26 файлів)
```

## 12. Критерії приймання

- `wire_selftest` зелений (framer-вектори + DeviceSession-over-loopback + TCP-echo);
  COM/WS смоуки SKIP-аються керовано.
- `build_project.ps1 -WithUAPKI -WithTests` зелений на x86 і x64; `run_tests.ps1`
  x64/x86 без регресій UAPKI (L0/L1/L2/L3) + новий L0.6.
- Старий ECR-драйвер і мертвий WSServer видалені; DLL містить UAPKI+TestComponent+wire.
- Жодних правок у UAPKI-стеку.

## 13. Ризики

| Ризик | Пом'якшення |
|---|---|
| Гонки в `DeviceSession` (reader vs caller vs reconnect) | Один `mutex`+`cv`; user-колбеки поза локом; стрес-тест над LoopbackTransport (як EventBridge в Етапі 0) |
| Видалення ECR ламає збірку (посилання) | ECR-компонента ізольована; після зняття її з `components.cmake` і `SOURCE_FILES` перевірити `grep` на залишкові посилання; UAPKI/Test не залежать від ECR |
| WS/COM важко автотестувати | Основну механіку валідує LoopbackTransport (детерміновано); реальні транспорти — смоук + повна валідація в фазі драйвера проти емулятора |
| Форма `Request`/unsolicited не підійде рушію операцій | Шов навмисно вузький (`Request`+хендлер); рушій — просто споживач із worker-потоку; за потреби доточується без зламу транспорту/framer'а |
```
