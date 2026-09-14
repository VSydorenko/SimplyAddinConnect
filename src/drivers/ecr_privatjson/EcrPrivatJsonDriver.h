#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <chrono>
#include <atomic>
#include <functional>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "../../platform/ResultEnvelope.h"
#include "../../platform/JobEngine.h"
#include "../../transport/RequestTypes.h"

class DeviceSession;
class ITransport;

/// Розібраний рядок підключення.
struct EcrConnParams {
    enum class Kind { Tcp, Com } kind = Kind::Tcp;
    std::string host;        ///< для Tcp
    int tcpPort = 2000;      ///< для Tcp
    std::string comPort;     ///< для Com, напр. "COM3"
    int baud = 115200;       ///< для Com
};

/// Доля останньої фінансової операції (спека §4.1). None - питання не стояло;
/// Pending - з'ясовуємо; Resolved - знімок готовий (успіх/невдача з'ясування читається
/// з facts.ok/facts.code, окремий стан для цього зайвий).
enum class OutcomeState { None, Pending, Resolved };

/// Намір каси: що саме ми відправляли на дріт, коли зв'язок обірвався.
struct OperationIntent {
    std::string method;      ///< "Purchase" | "Refund"
    std::string amount;      ///< рядком, як пішло на дріт (MoneyToString)
    std::string rrn;         ///< для Refund; інакше порожньо
    std::string requestId;   ///< ИдентификаторЗапроса з 1С, прозорий; "" якщо не задано
    std::chrono::system_clock::time_point startedAt{};
};

/// Стан зв'язку з терміналом (спека §4.9.1). НЕ дублює connected_ транспорту:
/// Ready означає «термінал відповів на Ping», а не «сокет відкрився».
enum class LinkState { Disconnected, Connecting, Ready };

struct LastOutcome {
    OutcomeState    state = OutcomeState::None;
    std::uint64_t   generation = 0;    ///< ++ на кожен перехід у Pending; ідентифікатор питання для 1С
    OperationIntent intent;
    std::string     reason;            ///< TIMEOUT|DISCONNECTED|SEND_FAILED|STOPPED|DESYNC
    bool            terminalIdle = false;
    ResultEnvelope  facts;             ///< результат RequestReceiptFacts (поля §5.30)
};

/// Пілотний драйвер ECRPrivatJSON: розбір підключення, фабрика транспорту, життєвий
/// цикл за еталонною схемою (спека §6). Операції — Частина 2.
class EcrPrivatJsonDriver {
public:
    // Конструктор/деструктор — поза класом (у .cpp): unique_ptr<DeviceSession> над
    // forward-оголошеним типом вимагає повного типу при інстанціюванні спецчленів;
    // out-of-line визначення тримає цю вимогу в TU драйвера (не протікає у фасад).
    EcrPrivatJsonDriver();
    ~EcrPrivatJsonDriver();

    /// Розбір рядка підключення → out. false, якщо не розібрано:
    ///   tcp://host:port — host як IPv4/hostname (IPv6-літерали не підтримуються);
    ///   COMn[:baud[,8,N,1]] — з рядка береться лише baud (дефолт 115200); формат кадру
    ///   завжди 8N1 (спека §3.1), поля після baud ігноруються.
    static bool ParseConnString(const std::string& s, EcrConnParams& out);

    /// Еталонна схема: Ping(+dc) → Identify(+dc) → постійний конект. true — на зв'язку.
    bool Connect(const std::string& connString);
    void Disconnect();
    bool IsConnected() const;

    /// «Термінал підтвердив готовність» - саме це бачить 1С у Подключен (спека §4.9.4).
    /// Внутрішній IsConnected() лишається «сокет відкритий».
    bool IsReady() const;

    /// Покоління з'єднання (спека §4.9.1). Публічний ЛИШЕ заради детермінованого тесту №23-біс
    /// (SetBeforeReadyHookForTest): тест чекає ЗМІНИ епохи замість непрямого проксі IsConnected().
    std::uint64_t LinkEpoch() const;

    std::string Vendor() const;
    std::string Model() const;

    // Синхронні операції (start+wait). Повертають ResultEnvelope (ok/code/description/payload).
    ResultEnvelope Execute(const std::string& method, const nlohmann::json& params, int timeoutMs);

    /// Увімкнути/вимкнути wire-трасування (діє з наступного Connect).
    void SetTrace(bool on) { traceEnabled_.store(on); }

    /// Опційний обробник подій термінала (state/status/result) → у 1С через ExternalEvent.
    /// Викликається з poller/worker-потоку; nullptr → події не емітяться (за замовчуванням).
    using EventHandler = std::function<void(const std::string& event, const std::string& dataJson)>;
    void SetEventHandler(EventHandler h);

    ResultEnvelope Purchase(const std::string& amount, const nlohmann::json& extra = {});
    ResultEnvelope Refund(const std::string& amount, const std::string& rrn, const nlohmann::json& extra = {});
    ResultEnvelope CheckConnection();
    ResultEnvelope GetReceiptInfo(const std::string& invoiceNumber);
    /// X-звіт (спека §5.17 "Audit. Він же X-balance") — підсумки без вилучення.
    ResultEnvelope Audit(const std::string& merchantId = "0");
    /// Звірка / Загальний звіт (спека §5.18 "Verify") — звірка підсумків із хостом.
    ResultEnvelope Verify(const std::string& merchantId = "0");

    /// Останній прочитаний getLastStatMsgCode (-1, якщо ще не було).
    int LastStatus() const;

    /// Запит на скасування активної операції: poller надішле interrupt на service-доріжці.
    void RequestInterrupt();

    // Асинхронний API поверх JobEngine (неблокуючий; результат — TryGetOperationResult).
    bool StartOperation(const std::string& method, const nlohmann::json& params, int timeoutMs);
    bool StartPurchase(const std::string& amount, const nlohmann::json& extra = {});
    bool StartRefund(const std::string& amount, const std::string& rrn, const nlohmann::json& extra = {});
    JobState OperationState() const;
    bool TryGetOperationResult(ResultEnvelope& out) const;
    void CancelOperation();   ///< RequestInterrupt() + JobEngine → Interrupting

    // Send-арбітр (спека §7): мін. інтервал 0.1с між ФАКТИЧНИМИ відправленнями.
    // Використовуватиметься в Частині 2; тут — інфраструктура.
    void GateSend();   // блокує до дозволеного моменту, оновлює мітку

    /// Знімок долі останньої операції для 1С. Мережею не ходить, працює в будь-якому стані.
    /// НЕ const: Task 4 додасть сюди самозцілення (мутує recoveryJob_).
    ResultEnvelope InquireLastOutcome();

    /// ИдентификаторЗапроса каси, що пройде у знімок прозорим рядком. Забирається
    /// одноразово на вході наступної фінансової операції.
    void SetRequestId(std::string id);

    /// Тестові шви (у продакшені не викликаються).
    using TransportFactory = std::function<std::unique_ptr<ITransport>(const EcrConnParams&)>;
    /// Підмінити фабрику транспорту (стаб Send<0 без закриття сокета). Ставити ДО Connect.
    void SetTransportFactoryForTest(TransportFactory f);
    /// Скоротити очікування з'ясування: 120с ліміту спокою в тесті нестерпні.
    /// pingTimeoutMs < 0 - не чіпати (дефолт kHandshakeTimeoutMs); коротке значення потрібне
    /// тесту №25, щоб джоб гарантовано ДІЙШОВ до backoff-сну, а не стояв у чеканні відповіді.
    void SetOutcomeTimingForTest(int idleWaitMs, int syncWaitMs, int pingTimeoutMs = -1);
    /// Зафіксувати намір БЕЗ старту джоба - модель вікна «Pending без виконавця» (спека §4.4).
    void MarkPendingForTest(const std::string& method, const std::string& amount, const std::string& reason);
    /// Детермінований тест №23-біс (рев'ю Task 4, I1): хук викликається в EnsureReady МІЖ
    /// отриманням Response/Busy на Ping і SetLinkState(Ready, "", epoch) - у вікні, де тест сам
    /// рве з'єднання, епоха встигає піти вперед, і Ready записується зі СВІДОМО застарілим
    /// знімком epoch. Викликається СИНХРОННО на потоці джоба; nullptr (дефолт) - без хука.
    void SetBeforeReadyHookForTest(std::function<void()> hook);

    /// Тестовий шов: зупинити ЛИШЕ сесію (усі pending -> Stopped), не чіпаючи драйвер.
    /// Моделює Stopped-тригер без session_.reset() під активним запитом.
    void StopSessionForTest();

private:
    std::unique_ptr<ITransport> MakeTransport(const EcrConnParams& p) const;
    /// Зібрати нову DeviceSession з колбеками (ставляться ДО Start()).
    std::unique_ptr<DeviceSession> MakeSession(const EcrConnParams& p);

    EcrConnParams params_{};
    std::unique_ptr<DeviceSession> session_;   ///< постійна сесія (після Connect)
    std::string vendor_, model_;
    JobEngine job_;                            ///< одне активне асинхронне завдання

    static ResultEnvelope MapResult(const RequestResult& r);
    static constexpr int kOperationTimeoutMs = 120000;   ///< до 120с на фінансову операцію

    /// Poller-цикл: під час активної primary-операції полить getLastStatMsgCode на
    /// service-доріжці й оновлює lastStatus_. Єдиний власник service-доріжки.
    void PollerLoop(std::atomic<bool>& stop);
    std::atomic<int> lastStatus_{ -1 };
    static constexpr int kPollIntervalMs = 500;    ///< 0.5с полінг статусу
    static constexpr int kServiceTimeoutMs = 3000;

    // Скасування: 1С ставить interruptRequested_, poller шле interrupt один раз (interruptSent_).
    std::atomic<bool> interruptRequested_{ false };
    std::atomic<bool> interruptSent_{ false };

    /// Внутрішній виконавець (без ре-ресету interruptRequested_): sync-шлях і worker
    /// асинхронного завдання кличуть саме його. Публічний Execute робить reset ПЕРЕД цим.
    ResultEnvelope ExecuteInternal(const std::string& method, const nlohmann::json& params, int timeoutMs);

    /// Вузький запит чека для з'ясування долі операції: БЕЗ EmitEvent, без дотику до
    /// lastStatus_/interruptSent_/job_. Публічний GetReceiptInfo (ПолучитьЧек) лишається як є.
    ResultEnvelope RequestReceiptFacts(const std::string& invoiceNumber);

    /// Єдина точка старту фонового відновлення (спека §4.4). Ідемпотентна: якщо джоб іде -
    /// нічого не робить. Кличуть: ExecuteInternal після MarkPending, dispatcher-хук (up=true),
    /// гейт §4.6, InquireLastOutcome, Connect() після старту сесії.
    void EnsureRecoveryRunning();
    /// Тіло фонового джоба: крок 0 - готовність (Ping), крок 1 - з'ясування долі.
    ResultEnvelope RecoveryJob(std::uint64_t generation);
    /// Крок 0: цикл Ping із backoff, доки термінал не відповість (Response або deviceBusy).
    bool EnsureReady();
    /// Крок 1: полінг статусу до спокою -> MarkSynchronized -> RequestReceiptFacts -> знімок.
    ResultEnvelope CaptureOutcome(std::uint64_t generation);
    /// Bounded очікування знімка в синхронному виклику (спека §4.4а).
    void WaitOutcomeBounded(std::uint64_t generation);
    /// Сон, перериваний Disconnect-ом: кроками по 100 мс із перевіркою closing_. Голий
    /// sleep_for(backoff) до 15 с ззовні не перервати, і Отключить під silence монополії
    /// висів би на recoveryJob_.Join() до кінця інтервалу, а §4.5 обіцяє швидкий Join.
    void SleepInterruptible(int ms);

    JobEngine          recoveryJob_;      ///< ДРУГИЙ движок: job_ несе контракт СостояниеОперации
    std::atomic<bool>  closing_{ false }; ///< Disconnect у процесі: джоб і хук виходять
    TransportFactory   transportFactory_; ///< тестовий шов; nullptr -> справжні транспорти
    /// Тестовий шов №23-біс: кличеться в EnsureReady МІЖ Response на Ping і SetLinkState(Ready).
    /// mutable не потрібен - EnsureReady не const; захисту мютексом не потребує (ставиться ДО
    /// Connect(), як і transportFactory_, читається лише на одному потоці джоба).
    std::function<void()> beforeReadyHookForTest_;
    std::atomic<int>   outcomeIdleWaitMs_{ kOperationTimeoutMs };  ///< скільки чекати спокою
    std::atomic<int>   outcomeSyncWaitMs_{ kOutcomeSyncWaitMs };   ///< скільки чекати в синхронному виклику
    /// Таймаут Ping у EnsureReady. Дефолт = kHandshakeTimeoutMs (.cpp); поле, а не константа,
    /// лише заради тесту №25 - інакше джоб до backoff-сну не доходить (стоїть у чеканні Ping).
    std::atomic<int>   pingTimeoutMs_{ 5000 };

    static constexpr int kOutcomeSyncWaitMs   = 8000;   ///< менше за терпіння касира в РМК
    /// Ритм Ping-повторів навмисно дублює дефолти SessionConfig (DeviceSession.h:35-38):
    /// сесія конфіг назовні не віддає, а стукати ми маємо в такт із супервізором.
    static constexpr int kReconnectDelayMs    = 1000;
    static constexpr int kReconnectMaxDelayMs = 15000;

    static bool IsFinancial(const std::string& method);   ///< WHITELIST {Purchase, Refund}: гейт §4.6 + requestId + тригер Pending; новий фінансовий метод — сюди, інакше тихо випаде (див. .cpp)

    /// Зафіксувати намір: state=Pending, generation++, reason. Повертає нове покоління.
    std::uint64_t MarkPending(const OperationIntent& intent, const std::string& reason);
    /// Об'єкт payload.outcome (спека §4.7). Бере outcomeMutex_; мережею не ходить.
    nlohmann::json OutcomeSnapshotJson();
    /// Конверт коду 17 зі знімком усередині.
    ResultEnvelope BuildUnknownOutcome();

    /// Один цикл getLastStatMsgCode на service-доріжці. Спільний для PollerLoop і
    /// з'ясування долі. code валідний ЛИШЕ при Response; інакше не чіпається.
    RequestStatus PollStatusOnce(int& code);

    mutable std::mutex outcomeMutex_;   ///< lastOutcome_ + pendingRequestId_
    LastOutcome        lastOutcome_;
    std::string        pendingRequestId_;

    /// Єдина точка зміни стану зв'язку. Пишуть три потоки: dispatcher-хук, фоновий джоб
    /// і потік 1С. reason - для події; epoch береться до участі ЛИШЕ для Ready: Ping зі
    /// вже мертвої епохи не має піднімати прапорець на новому сокеті.
    void SetLinkState(LinkState target, const char* reason, std::uint64_t epoch = 0);

    mutable std::mutex linkMutex_;
    LinkState          linkState_ = LinkState::Disconnected;   ///< під linkMutex_
    /// ПОКОЛІННЯ З'ЄДНАННЯ: ++ на КОЖНОМУ записі не-Ready, ДО перевірки «той самий стан».
    /// Не «вихід із Ready» — так було в першій редакції, і в гонці «Ping відповіли, сокет упав»
    /// хук виходив без інкременту, а Ready лягав на мертвий сокет (спека §4.9.1).
    std::uint64_t      linkEpoch_ = 0;
    /// Серіалізує ЕМІСІЮ подій "connection" (спека §4.9.3): порядок подій = порядок переходів,
    /// остання отримана подія відповідає поточному стану. Береться ЗОВНІ linkMutex_, на весь
    /// SetLinkState - інакше джоб (пише Ready, витісняється) і хук (пише Connecting, емітить
    /// одразу) можуть емітити "ready" ПІСЛЯ "connecting", хоча стан УЖЕ Connecting - 1С бачила б
    /// ready останнім при фактичному Connecting. Порядок узяття - ЗАВЖДИ linkEmitMutex_ →
    /// linkMutex_ і linkEmitMutex_ → eventMutex_ (усередині EmitEvent), НІКОЛИ навпаки: жоден
    /// обробник події не кличе SetLinkState, тож дедлоку це не додає. Свідомий, задокументований
    /// ВИНЯТОК із правила «під linkMutex_ жодного EmitEvent» - сам EmitEvent лишається ПОЗА
    /// linkMutex_ (коротким лишається лише внутрішній лок), просто тепер ще й ПІД linkEmitMutex_.
    /// Наступний читач: не «полагодь» це назад, прибравши linkEmitMutex_ - саме він і є фіксом.
    std::mutex linkEmitMutex_;
    /// Wire-трасування: якщо true — MakeSession чіпляє SetWireTraceHandler (діє з наступного Connect).
    std::atomic<bool> traceEnabled_{ false };

    /// Події термінала → 1С (опційно). EmitEvent будує JSON і кличе eventHandler_ (якщо є).
    void EmitEvent(const std::string& event, const nlohmann::json& data);
    static std::string StatusText(int code);   ///< getLastStatMsgCode → людський текст (укр)
    EventHandler eventHandler_;
    std::mutex eventMutex_;                     ///< захист eventHandler_ (set із 1С vs виклик з poller)

    mutable std::mutex sendGateMutex_;
    std::chrono::steady_clock::time_point lastSend_{};
    static constexpr int kSendGapMs = 100;     ///< 0.1с між командами (спека)
};
