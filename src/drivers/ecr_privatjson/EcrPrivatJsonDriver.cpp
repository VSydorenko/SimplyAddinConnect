#include "../../core/pch.h"
#include "EcrPrivatJsonDriver.h"
#include "EcrJsonCodec.h"
#include "EcrPrivatJsonClassifier.h"
#include "../../transport/DeviceSession.h"
#include "../../transport/RequestTypes.h"
#include "../../transport/NullTerminatedFramer.h"
#include "../../transport/Transport_TCP.h"
#include "../../transport/Transport_COM.h"
#include "../../helpers/ServiceTools.h"
#include <thread>
#include <cstdio>
#include <ctime>
#include <utility>

namespace {
constexpr int kHandshakeTimeoutMs = 5000;   // Ping/Identify — з запасом (Verifone 3-5с)
constexpr int kPostPingPauseMs    = 1000;   // пауза 1с після Ping (спека §3.4)

// Дефолт pingTimeoutMs_ у заголовку продубльовано числом (константа живе в анонімному
// namespace цього .cpp і в .h не видима) - тримаємо їх у синхроні перевіркою компілятора.
static_assert(kHandshakeTimeoutMs == 5000, "pingTimeoutMs_ у .h ініціалізовано 5000 — оновити разом");

// Обов'язковий склад params оплати/повернення (спека §5.1.1/§5.2.1): реальні
// термінали (Newland N950, інцидент 2026-08-29) без discount/merchantId відбивають
// запит кодом 1000 "Введіть discount" — еталонна каса ПриватБанк ці поля шле завжди.
// Дефолти НЕ перетирають значення, передані викликачем через extra. subMerchant
// свідомо НЕ додаємо: спека дозволяє поле лише після реєстрації субмерчанта в банку.
void FillPaymentDefaults(nlohmann::json& p, bool withFacepay) {
    if (!p.contains("discount"))   p["discount"] = "";
    if (!p.contains("merchantId")) p["merchantId"] = "0";
    if (withFacepay && !p.contains("facepay")) p["facepay"] = "false";
}

// ISO 8601 UTC із мілісекундами. Дефолтний time_point (наміру не було) -> порожній рядок,
// щоб 1С не бачила фальшиве "1970-01-01".
std::string IsoUtc(std::chrono::system_clock::time_point tp) {
    if (tp == std::chrono::system_clock::time_point{}) return std::string{};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tmv{};
    gmtime_s(&tmv, &t);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms.count()));
    return std::string(buf);
}

const char* OutcomeStateName(OutcomeState s) {
    switch (s) {
        case OutcomeState::Pending:  return "pending";
        case OutcomeState::Resolved: return "resolved";
        default:                     return "none";
    }
}

const char* LinkStateName(LinkState s) {
    switch (s) {
        case LinkState::Ready:      return "ready";
        case LinkState::Connecting: return "connecting";
        default:                    return "disconnected";
    }
}
}

EcrPrivatJsonDriver::EcrPrivatJsonDriver() = default;
EcrPrivatJsonDriver::~EcrPrivatJsonDriver() { Disconnect(); }

bool EcrPrivatJsonDriver::ParseConnString(const std::string& s, EcrConnParams& out) {
    if (s.rfind("tcp://", 0) == 0) {
        auto rest = s.substr(6);
        auto colon = rest.rfind(':');   // IPv6-літерали не підтримуються (LAN-термінал — IPv4)
        if (colon == std::string::npos) return false;
        out.kind = EcrConnParams::Kind::Tcp;
        out.host = rest.substr(0, colon);
        const std::string portStr = rest.substr(colon + 1);
        try {
            std::size_t pos = 0;
            out.tcpPort = std::stoi(portStr, &pos);
            if (pos != portStr.size()) return false;   // хвостове сміття ("80abc") → відхилити
        } catch (...) { return false; }
        return !out.host.empty() && out.tcpPort > 0;
    }
    if (s.rfind("COM", 0) == 0) {
        auto colon = s.find(':');
        out.kind = EcrConnParams::Kind::Com;
        out.comPort = (colon == std::string::npos) ? s : s.substr(0, colon);
        out.baud = 115200;   // дефолт; формат кадру завжди 8N1 (спека §3.1), хвіст після baud ігнор
        if (colon != std::string::npos) {
            auto tail = s.substr(colon + 1);           // "115200,8,N,1"
            const std::string baudStr = tail.substr(0, tail.find(','));
            if (!baudStr.empty()) {                    // "COM3:" (порожній baud) → лишаємо дефолт
                try {
                    std::size_t pos = 0;
                    out.baud = std::stoi(baudStr, &pos);
                    if (pos != baudStr.size()) return false;   // сміття у baud → відхилити
                } catch (...) { return false; }
            }
        }
        return !out.comPort.empty();
    }
    return false;
}

std::unique_ptr<ITransport> EcrPrivatJsonDriver::MakeTransport(const EcrConnParams& p) const {
    if (transportFactory_) return transportFactory_(p);
    if (p.kind == EcrConnParams::Kind::Tcp)
        return std::make_unique<TransportTCP>(p.host, p.tcpPort);
    // COM: драйвер ЯВНО передає baud (дефолт TransportCOM = 9600), 8N1.
    return std::make_unique<TransportCOM>(p.comPort, p.baud, 8, 'N', 1.0f);
}

std::unique_ptr<DeviceSession> EcrPrivatJsonDriver::MakeSession(const EcrConnParams& p) {
    auto s = std::make_unique<DeviceSession>(MakeTransport(p),
                                             std::make_unique<NullTerminatedFramer>(),
                                             std::make_unique<EcrPrivatJsonClassifier>());
    // Колбеки — ЛИШЕ до Start() (DeviceSession: після Start — no-op+WARN).
    // deviceBusy без нашого запиту - лише пізній дубль на таймаутнутий запит (протокол §6.1);
    // інших самостійних нотифікацій протокол не документує. Логуємо, щоб wire-трасування
    // показувало, що прийшло без запиту.
    s->SetUnsolicitedHandler([](std::vector<uint8_t> frame) {
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSON",
            "Кадр без запиту: " + std::string(frame.begin(), frame.end()));
    });
    // Стан зв'язку слухає ЛИШЕ персистентна сесія - хук їй ставить Connect() (крок 3).
    // Короткоживучі hs/id його не мають: на момент їх створення session_ уже обнулено.
    s->SetConnectionStateHandler([](bool) {});
    if (traceEnabled_.load())
        s->SetWireTraceHandler([](bool tx, std::vector<uint8_t> b) {
            NEUTRAL_REPORT_TRACE("ECRPrivatJSON", std::string(tx ? "TX " : "RX ") + std::string(b.begin(), b.end()));
        });
    return s;
}

void EcrPrivatJsonDriver::GateSend() {
    std::lock_guard<std::mutex> lk(sendGateMutex_);
    auto now = std::chrono::steady_clock::now();
    auto next = lastSend_ + std::chrono::milliseconds(kSendGapMs);
    if (now < next) std::this_thread::sleep_for(next - now);
    lastSend_ = std::chrono::steady_clock::now();
}

bool EcrPrivatJsonDriver::Connect(const std::string& connString) {
    // Нова спроба — скидаємо ідентичність попереднього пристрою, щоб при невдалому
    // Identify (best-effort) Vendor()/Model() не віддавали стару ідентичність.
    vendor_.clear();
    model_.clear();
    job_.ResetToIdle();   // A-defense: прибрати завислий стан завершеного попереднього завдання
    if (!ParseConnString(connString, params_)) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Невірний рядок підключення: " + connString);
        return false;
    }
    Disconnect();

    // 1) Хендшейк: коротка сесія, Ping із провідним 0x00.
    {
        auto hs = MakeSession(params_);
        if (!hs->Start()) { NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Хендшейк: не вдалося відкрити зв'язок"); return false; }
        auto ping = EcrJsonCodec::BuildRequest("PingDevice", 0, nullptr);
        GateSend();
        RequestResult r = hs->RequestPrimary(ping, kHandshakeTimeoutMs, FrameOptions{/*leadingDelimiter=*/true});
        // Busy приймаємо нарівні з Response - те саме рішення, що в EnsureReady (крок 0):
        // deviceBusy означає «термінал живий і чує нас, але зайнятий нашою ж нерозв'язаною
        // операцією», а не «зв'язку немає». Інакше ручний Отключить/Подключить під час
        // нерозв'язаної транзакції провалював би Connect цілком - тоді як автоматичний
        // реконект той самий стан обробляє правильно. Після коду 17 касир тисне
        // «перепідключити» саме в цьому стані (рев'ю гілки, фінальний раунд).
        if (r.status != RequestStatus::Response && r.status != RequestStatus::Busy) {
            NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Хендшейк PingDevice не вдався");
            return false;
        }
        hs->Stop();   // дисконект (еталонна схема)
        std::this_thread::sleep_for(std::chrono::milliseconds(kPostPingPauseMs));
    }

    // 2) Identify: коротка сесія.
    {
        auto id = MakeSession(params_);
        if (!id->Start()) { NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Identify: не вдалося відкрити зв'язок"); return false; }
        auto ident = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType", "identify"}});
        GateSend();
        RequestResult r = id->RequestService(ident, kHandshakeTimeoutMs);
        if (r.status == RequestStatus::Response) {
            auto pr = EcrJsonCodec::Parse(r.frame);
            if (pr.valid && pr.params.is_object()) {
                vendor_ = pr.params.value("vendor", std::string{});
                model_  = pr.params.value("model", std::string{});
            }
        }
        id->Stop();   // дисконект
    }

    // 3) Постійний режим: сесія лишається відкритою (реконект - супервізор DeviceSession).
    session_ = MakeSession(params_);
    session_->SetConnectionStateHandler([this](bool up) {
        // Хук іде на dispatcher-потоці сесії: сигналізуємо й повертаємось, у мережу не ходимо
        // (блокуючий виклик звідси заморозив би dispatcher - спека §2.1).
        if (up) { EnsureRecoveryRunning(); return; }   // TCP є -> Ping до готовності (крок 0)
        SetLinkState(LinkState::Connecting, "dropped");
    });
    // Ready ставимо ДО Start(): хендшейк-Ping пройшов секунду тому (крок 1), а хук up=true
    // після Start уже стоїть у черзі dispatcher-а. Якби Ready ставився після Start(), хук
    // побачив би Connecting і Task 4 запустив би зайвий Ping рівно тоді, коли 1С після
    // Подключить одразу кличе Оплату -> CONCURRENT на першій оплаті зміни (спека §4.9.1).
    //
    // ⚠️ З ЯВНОЮ ЕПОХОЮ. Дефолтна 0 тут не годиться: Connect() починається з Disconnect(),
    // після якого linkEpoch_ >= 1, і SetLinkState(Ready, "") мовчки відкинувся б - Ready не
    // став би на КОЖНОМУ повторному Connect(). Між LinkEpoch() і записом хук нової сесії
    // спрацювати не може: Start() ще не викликано.
    SetLinkState(LinkState::Ready, "", LinkEpoch());
    if (!session_->Start()) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Постійний режим: не вдалося відкрити зв'язок");
        // Не "closed": це аварія підключення, а не прохання каси відключитись (спека §4.9.3).
        SetLinkState(LinkState::Disconnected, "connect_failed");
        // Stop() ПЕРЕД reset() (рев'ю Task 4): при opened==false Start() лишає dispatcher і
        // супервізор живими, хук уже стоїть - без Stop() хук міг би виконати EnsureRecoveryRunning()
        // над знищеною сесією (use-after-free у вікні до першої спроби реконекту).
        session_->Stop();
        // recoveryJob_.Join() ТУТ ЖЕ (рев'ю Task 4, фікс-раунд 2) - для симетрії з Disconnect():
        // Stop() нічого не знає про recoveryJob_ і не приєднує його. Практично вікно перекрите
        // таймінгом супервізора (хук не встигає стартувати джоб до Stop), але це побічний ефект
        // конфігурації, а не гарантія - Join() тут прибирає залежність від таймінгу.
        recoveryJob_.Join();
        session_.reset();
        return false;
    }
    // Ручний реконект із 1С (Отключить/Подключить) при збереженому Pending: без Pending - no-op,
    // зайвого Ping не буде, бо linkState_ уже Ready.
    EnsureRecoveryRunning();
    return true;
}

bool EcrPrivatJsonDriver::StartOperation(const std::string& method, const nlohmann::json& params, int timeoutMs) {
    if (!IsConnected()) return false;
    // Скидаємо прапорці скасування ТУТ (у викликача), а НЕ у worker: ExecuteInternal їх не
    // чіпає, тож cancel одразу після Start (fix E) не губиться під ре-ресетом у worker-потоці.
    interruptRequested_.store(false);
    interruptSent_.store(false);
    return job_.Start([this, method, params, timeoutMs]() { return ExecuteInternal(method, params, timeoutMs); });
}

bool EcrPrivatJsonDriver::StartPurchase(const std::string& amount, const nlohmann::json& extra) {
    nlohmann::json p = extra.is_object() ? extra : nlohmann::json::object(); p["amount"] = amount;
    FillPaymentDefaults(p, /*withFacepay=*/true);
    return StartOperation("Purchase", p, kOperationTimeoutMs);
}

bool EcrPrivatJsonDriver::StartRefund(const std::string& amount, const std::string& rrn, const nlohmann::json& extra) {
    nlohmann::json p = extra.is_object() ? extra : nlohmann::json::object(); p["amount"] = amount; p["rrn"] = rrn;
    FillPaymentDefaults(p, /*withFacepay=*/false);
    return StartOperation("Refund", p, kOperationTimeoutMs);
}

JobState EcrPrivatJsonDriver::OperationState() const { return job_.State(); }
bool EcrPrivatJsonDriver::TryGetOperationResult(ResultEnvelope& out) const { return job_.TryGetResult(out); }
void EcrPrivatJsonDriver::CancelOperation() { RequestInterrupt(); job_.RequestCancel(); }

void EcrPrivatJsonDriver::Disconnect() {
    closing_.store(true);      // хук і CaptureOutcome бачать і виходять
    job_.Join();               // фінансову операцію НЕ рвемо - як і раніше
    if (session_) session_->Stop();   // усі pending -> Stopped негайно; dispatcher join-нуто,
                                      // тож нових хуків (і нових стартів джоба) більше не буде
    recoveryJob_.Join();       // швидкий: його запити вже повернули Stopped
    if (session_) {
        session_.reset();
        NEUTRAL_REPORT_INFO("ECRPrivatJSON", "Disconnect: сесію закрито");
    } else {
        // Повторний Отключить без активної сесії — легальний no-op; пишемо в лог,
        // щоб «нічого не сталося на терміналі» не виглядало як збій (2026-08-29).
        NEUTRAL_REPORT_INFO("ECRPrivatJSON", "Disconnect: сесії немає (вже відключено) — no-op");
    }
    SetLinkState(LinkState::Disconnected, "closed");   // ручний розрив - не аварія
    closing_.store(false);
}

bool EcrPrivatJsonDriver::IsConnected() const { return session_ && session_->IsConnected(); }
std::string EcrPrivatJsonDriver::Vendor() const { return vendor_; }
std::string EcrPrivatJsonDriver::Model() const { return model_; }

bool EcrPrivatJsonDriver::IsReady() const {
    std::lock_guard<std::mutex> lk(linkMutex_);
    return linkState_ == LinkState::Ready;
}

std::uint64_t EcrPrivatJsonDriver::LinkEpoch() const {
    std::lock_guard<std::mutex> lk(linkMutex_);
    return linkEpoch_;
}

void EcrPrivatJsonDriver::SetLinkState(LinkState target, const char* reason, std::uint64_t epoch) {
    // Спека §4.9.3: порядок подій "connection" = порядок переходів, остання отримана подія
    // відповідає поточному стану. Без цього лока джоб (ставить Ready, витісняється) і хук
    // (ставить Connecting, емітить одразу) можуть емітити "ready" ПІСЛЯ "connecting", хоча
    // фактичний стан УЖЕ Connecting - 1С побачила б ready останнім при живому Connecting.
    // Свідомий виняток із «жодного EmitEvent під locked-станом»: linkEmitMutex_ береться
    // ЗОВНІ linkMutex_ на ВЕСЬ виклик, EmitEvent лишається ПОЗА linkMutex_ (як і раніше), але
    // тепер ще й ПІД linkEmitMutex_. Порядок узяття - ЗАВЖДИ linkEmitMutex_ -> linkMutex_ і
    // linkEmitMutex_ -> eventMutex_ (усередині EmitEvent), НІКОЛИ навпаки: жоден обробник
    // події не кличе SetLinkState, дедлоку це не додає. НЕ прибирай цей лок - саме він і є
    // фіксом інверсії журналу подій (рев'ю Task 4, фікс-раунд 2).
    std::lock_guard<std::mutex> emitLk(linkEmitMutex_);
    {
        std::lock_guard<std::mutex> lk(linkMutex_);
        // Ping приніс Ready з епохи, яка вже мертва (сокет упав одразу після відповіді) - ігноруємо:
        // інакше Ready лишився б на мертвому сокеті, і наступний up=true його не полагодив би.
        if (target == LinkState::Ready && epoch != linkEpoch_) return;
        // linkEpoch_ - ПОКОЛІННЯ З'ЄДНАННЯ (спека §4.9.1, дефект першої редакції, рев'ю Task 4):
        // росте на КОЖНОМУ записі не-Ready, ДО перевірки «той самий стан». Перша редакція
        // інкрементувала лише на виході з Ready - і в гонці «Ping відповіли, сокет упав» стан уже
        // Connecting (джоб у EnsureReady), хук виходив без інкременту, Ready проходив guard.
        if (target != LinkState::Ready) ++linkEpoch_;
        if (linkState_ == target) return;                    // без події: стан не змінився
        linkState_ = target;
    }
    EmitEvent("connection", { {"state", LinkStateName(target)}, {"reason", reason ? reason : ""} });
}

ResultEnvelope EcrPrivatJsonDriver::MapResult(const RequestResult& r) {
    switch (r.status) {
        case RequestStatus::Response: break;   // нижче
        case RequestStatus::Busy:          return ResultEnvelope::Fail("DEVICE_BUSY", "Термінал зайнятий");
        case RequestStatus::Unsupported:   return ResultEnvelope::Fail("UNSUPPORTED", "Метод не підтримується терміналом");
        case RequestStatus::Timeout:       return ResultEnvelope::Fail("TIMEOUT", "Немає відповіді термінала");
        case RequestStatus::Disconnected:  return ResultEnvelope::Fail("DISCONNECTED", "Обрив зв'язку з терміналом");
        case RequestStatus::SendFailed:    return ResultEnvelope::Fail("SEND_FAILED", "Помилка відправки");
        case RequestStatus::Stopped:       return ResultEnvelope::Fail("STOPPED", "Операцію перервано");
        case RequestStatus::Concurrent:    return ResultEnvelope::Fail("CONCURRENT", "Операція вже виконується");
        case RequestStatus::Desynchronized:return ResultEnvelope::Fail("DESYNC", "Потрібне відновлення зв'язку");
        default:                           return ResultEnvelope::Fail("UNKNOWN", "Невідомий статус");
    }
    ParsedResponse pr = EcrJsonCodec::Parse(r.frame);
    if (!pr.valid) return ResultEnvelope::Fail("BAD_RESPONSE", "Невалідна відповідь термінала");
    ResultEnvelope env;
    env.payload = pr.params;
    std::string rc = pr.params.is_object() ? pr.params.value("responseCode", std::string{}) : std::string{};
    env.code = rc.empty() ? (pr.error ? "ERROR" : "0000") : rc;
    env.description = pr.errorDescription;
    // ok: за прапорцем error (спека: 0010 Partial approval приходить із error:false).
    env.ok = !pr.error;
    return env;
}

int EcrPrivatJsonDriver::LastStatus() const { return lastStatus_.load(); }

void EcrPrivatJsonDriver::SetEventHandler(EventHandler h) {
    std::lock_guard<std::mutex> lk(eventMutex_);
    eventHandler_ = std::move(h);
}

void EcrPrivatJsonDriver::EmitEvent(const std::string& event, const nlohmann::json& data) {
    EventHandler h;
    { std::lock_guard<std::mutex> lk(eventMutex_); h = eventHandler_; }   // копія під локом
    if (!h) return;   // події вимкнено (за замовчуванням) → нічого не робимо
    // Виклик ПОЗА локом (PostExternalEvent у фасаді бере свій м'ютекс). UTF-8 JSON без винятків.
    h(event, data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

std::string EcrPrivatJsonDriver::StatusText(int code) {
    switch (code) {
        case 0:  return "Готово / очікування";
        case 1:  return "Картку зчитано";
        case 2:  return "Використано чіп-картку";
        case 3:  return "Авторизація на хості...";
        case 4:  return "Очікування дій касира";
        case 5:  return "Друк чека";
        case 6:  return "Введіть PIN-код";
        case 7:  return "Картку вилучено";
        case 8:  return "Оберіть застосунок картки";
        case 9:  return "Вставте / піднесіть картку";
        case 10: return "Виконується...";
        case 11: return "Коригування транзакції";
        default: return "Статус " + std::to_string(code);
    }
}

void EcrPrivatJsonDriver::RequestInterrupt() { interruptRequested_.store(true); }

void EcrPrivatJsonDriver::PollerLoop(std::atomic<bool>& stop) {
    while (!stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        if (stop.load() || !session_) break;
        // Скасування: раз надсилаємо interrupt на service-доріжці; фінальну 1001 ловить worker.
        if (interruptRequested_.load() && !interruptSent_.load()) {
            auto intr = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType", "interrupt"}});
            GateSend();
            session_->RequestService(intr, kServiceTimeoutMs);
            interruptSent_.store(true);
        }
        int code = -1;
        if (PollStatusOnce(code) == RequestStatus::Response && code >= 0) {
            if (code != lastStatus_.exchange(code))   // статус змінився -> подія в 1С
                EmitEvent("status", { {"code", code}, {"text", StatusText(code)}, {"state", "Running"} });
        }
    }
}

ResultEnvelope EcrPrivatJsonDriver::Execute(const std::string& method,
                                            const nlohmann::json& params, int timeoutMs) {
    // Публічний sync-шлях: скидаємо прапорці скасування ТУТ (не всередині ExecuteInternal —
    // fix E: інакше worker ре-ресетив би cancel, виставлений одразу після Start).
    interruptRequested_.store(false);
    interruptSent_.store(false);
    job_.ResetToIdle();   // fix G: прибрати завислий Done/Error попереднього async-завдання
    return ExecuteInternal(method, params, timeoutMs);
}

ResultEnvelope EcrPrivatJsonDriver::ExecuteInternal(const std::string& method,
                                                    const nlohmann::json& params, int timeoutMs) {
    const bool financial = IsFinancial(method);
    // requestId забирається ОДНОРАЗОВО на самому вході фінансової операції - ДО всіх гейтів
    // (17 / NOT_CONNECTED / RECONNECTING), під тим самим м'ютексом, що й SetRequestId (спека §4.7).
    // Відбитий виклик до термінала не дійшов - його id помирає разом із ним; інакше він
    // прилип би до наступної фінансової операції без сеттера (тест №26).
    std::string requestId;
    if (financial) {
        std::lock_guard<std::mutex> lk(outcomeMutex_);
        requestId = std::exchange(pendingRequestId_, std::string{});
    }
    const auto startedAt = std::chrono::system_clock::now();

    // Гейт §4.6: поки доля попередньої фінансової операції невідома, нову на дріт не пускаємо.
    // Саме тут закривається найімовірніший шлях до подвійного списання (касир тисне «Оплата»
    // ще раз, поки фоновий GetReceiptInfo іде) і конкуренція за єдину primary-доріжку.
    // ПЕРЕД перевірками стану зв'язку: при незавершеному намірі касир має бачити 17 із
    // поясненням, а не NOT_CONNECTED/RECONNECTING. requestId цього виклику вже забрано вище
    // (Task 2 крок 6) - відбитий виклик його споживає.
    if (financial) {
        bool pending = false;
        { std::lock_guard<std::mutex> lk(outcomeMutex_); pending = lastOutcome_.state == OutcomeState::Pending; }
        if (pending) {
            EnsureRecoveryRunning();               // самозцілення §4.4
            ResultEnvelope env = BuildUnknownOutcome();
            // Текст - для КАСИРА: штатний код 1С показує саме description (спека §4.6).
            // Імен методів компоненти тут не буває. Обидва варіанти містять «попередньої» (тест №4).
            env.description = IsConnected()
                ? "Доля попередньої карткової операції ще з'ясовується; зачекайте кілька секунд і повторіть"
                : "Зв'язку з терміналом немає; доля попередньої карткової операції з'ясується після підключення";
            return env;
        }
    }

    // Три різні стани - три різні коди, саме в цій послідовності (спека §4.9.4).
    // !IsReady() без IsConnected() перед ним перекрив би NOT_CONNECTED для всіх викликів
    // до Connect() і після Отключить - каса чекала б реконекту, якого нікому робити.
    if (!IsConnected()) return ResultEnvelope::Fail("NOT_CONNECTED", "Термінал не підключено");
    if (!IsReady())     return ResultEnvelope::Fail("RECONNECTING",
                                                    "Зв'язок з терміналом відновлюється, повторіть за мить");
    lastStatus_.store(-1);
    interruptSent_.store(false);   // interruptRequested_ ТУТ НЕ чіпаємо (fix E)
    EmitEvent("state", { {"state", "Running"}, {"method", method} });   // старт операції → у 1С
    auto req = EcrJsonCodec::BuildRequest(method, 0, params.is_null() ? nlohmann::json(nullptr) : params);

    RequestResult r;
    {
        // RAII-джойнер (fix D): poller зупиняється й join-иться ДО desync-recovery і навіть
        // при винятку з RequestPrimary — service-доріжка вільна, std::terminate не станеться.
        std::atomic<bool> pollerStop{ false };
        std::thread poller([this, &pollerStop] { PollerLoop(pollerStop); });
        struct PollerJoin {
            std::atomic<bool>& stop; std::thread& th;
            ~PollerJoin() { stop.store(true); if (th.joinable()) th.join(); }
        } pj{ pollerStop, poller };

        GateSend();
        r = session_->RequestPrimary(req, timeoutMs);
    }   // poller зупинено+join тут

    // Тригер «доля невідома» (спека §4.2): чотири статуси АБО desync. Порядок перевірки
    // саме такий - статус має пріоритет над desync. Busy/Unsupported/Concurrent без
    // desync відомі однозначно й тригером не є.
    const bool desync = session_ && session_->IsDesynchronized();
    const char* reason = nullptr;
    switch (r.status) {
        case RequestStatus::Timeout:      reason = "TIMEOUT";      break;
        case RequestStatus::Disconnected: reason = "DISCONNECTED"; break;
        case RequestStatus::SendFailed:   reason = "SEND_FAILED";  break;
        case RequestStatus::Stopped:      reason = "STOPPED";      break;
        default:                          reason = desync ? "DESYNC" : nullptr; break;
    }

    ResultEnvelope env;
    if (financial && reason) {
        OperationIntent intent;
        intent.method    = method;
        intent.amount    = params.is_object() ? params.value("amount", std::string{}) : std::string{};
        intent.rrn       = params.is_object() ? params.value("rrn", std::string{}) : std::string{};
        intent.requestId = requestId;
        intent.startedAt = startedAt;
        // Після Timeout primary супервізор DeviceSession негайно робить connected_=false і
        // Close->Open (DeviceSession.cpp:436-447), навіть якщо TCP був живий. Отже «зв'язок є»
        // тут - гонка; чесний стан - Connecting, а знімок зробить джоб після реконекту (крок 0).
        if (r.status == RequestStatus::Timeout || desync)
            SetLinkState(LinkState::Connecting, "timeout");        // Task 3, крок 6-біс
        const std::uint64_t gen = MarkPending(intent, reason);
        EnsureRecoveryRunning();
        // §4.2 п.2, ПОВНИЙ критерій: «зв'язок є» = сокет живий І сесія не в desync. Друга
        // умова - не формальність: після Timeout primary супервізор негайно рве й перевідкриває
        // сесію (DeviceSession.cpp:436-447), тож IsConnected() у цю мить - гонка. При desync
        // іде шлях (б): 17 негайно, знімок - після реконекту через хук, крок 0 (Ping), крок 1.
        // «Зараз» де-факто лишається для SendFailed без закриття транспорту: там з'єднання
        // справді живе, desync не ставиться, реконекту не буде, і хук ніколи б не спрацював.
        if (IsConnected() && !desync) WaitOutcomeBounded(gen);
        env = BuildUnknownOutcome();
    } else {
        env = MapResult(r);
    }
    // Завершення операції → у 1С (фінальний результат: approved/declined/помилка).
    EmitEvent("result", { {"ok", env.ok}, {"code", env.code}, {"description", env.description},
                          {"state", env.ok ? "Done" : "Error"}, {"payload", env.payload} });
    return env;
}

void EcrPrivatJsonDriver::SleepInterruptible(int ms) {
    constexpr int kStepMs = 100;
    for (int left = ms; left > 0 && !closing_.load(); left -= kStepMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(left < kStepMs ? left : kStepMs));
}

void EcrPrivatJsonDriver::EnsureRecoveryRunning() {
    if (closing_.load() || !IsConnected()) return;   // без сокета ні Ping, ні з'ясування
    const bool needReady = !IsReady();
    bool needOutcome = false;
    {
        std::lock_guard<std::mutex> lk(outcomeMutex_);
        needOutcome = lastOutcome_.state == OutcomeState::Pending;
    }
    if (!needReady && !needOutcome) return;
    // Покоління джобу НЕ передаємо (рев'ю гілки, фінальний раунд): див. коментар у RecoveryJob.
    // false = джоб уже йде; це нормальний, найчастіший результат.
    recoveryJob_.Start([this] { return RecoveryJob(); });
}

ResultEnvelope EcrPrivatJsonDriver::RecoveryJob() {
    // Крок 0: спершу термінал має підтвердити, що чує нас. Полінг статусу в тишу монополії
    // дав би хибний TERMINAL_BUSY (спека §4.4).
    if (!EnsureReady()) return ResultEnvelope::Fail("ABORTED", "Зв'язок не підтверджено");
    // Покоління читаємо ТУТ, а не беремо захоплене на СТАРТІ джоба (рев'ю гілки, фінальний
    // раунд). Джоб міг стартувати з хука up=true ще ДО того, як потік операції зафіксував
    // намір (MarkPending): RAII-join поллера забирає до ~3.5 с, і супервізор встигає
    // реконектитись раніше. Порівняння зі "своїм" поколінням тоді давало "намір не мій",
    // джоб виходив Ok(), а свіжий Pending лишався без виконавця - рівно той клас, під який
    // спека §4.4 будувала самозцілення. З'ясовуємо намір, що існує НА МОМЕНТ ГОТОВНОСТІ.
    std::uint64_t current = 0;
    bool pending = false;
    {
        std::lock_guard<std::mutex> lk(outcomeMutex_);
        pending  = lastOutcome_.state == OutcomeState::Pending;
        current  = lastOutcome_.generation;
    }
    if (!pending) return ResultEnvelope::Ok();       // готовність відновлено, питання про долю немає
    // Захист від перезапису чужим поколінням лишається в CaptureOutcome: воно звіряє
    // generation під outcomeMutex_ і в циклі полінгу, і безпосередньо під локом запису.
    return CaptureOutcome(current);
}

bool EcrPrivatJsonDriver::EnsureReady() {
    int backoff = kReconnectDelayMs;
    while (!closing_.load() && IsConnected() && !IsReady()) {
        const std::uint64_t epoch = LinkEpoch();     // епоха, в якій шлемо цей Ping
        auto ping = EcrJsonCodec::BuildRequest("PingDevice", 0, nullptr);
        RequestResult r;
        // Спека §4.9.2 «desync і Ping»: до ДВОХ спроб на ітерацію - другу БЕЗ backoff.
        // Desynchronized недосяжний після MarkSynchronized нижче; лишається лише вузьке вікно
        // RejectBoth між зняттям і відправкою (DeviceSession.cpp:355). Обмежуємо повтор ОДНИМ
        // разом (не нескінченним continue, як у першій редакції коду): при потоці RejectBoth
        // нескінченний continue дав би цикл Ping-ів ~10 Гц (єдиний гальмівник - GateSend, 100 мс).
        for (int attempt = 0; attempt < 2; ++attempt) {
            GateSend();
            // ПЕРЕД КОЖНИМ Ping знімаємо desync (спека §4.9.2 «desync і Ping»): після таймауту
            // primary DoRequest відбиває будь-який primary до дроту (DeviceSession.cpp:175) - Ping
            // теж, а Timeout самого Ping ставить desync знову. Гроші тут не захищає desync, а гейт
            // §4.6: MarkPending уже стоїть, фінансові виклики отримують 17, нефінансові - 18.
            session_->MarkSynchronized();
            // Той самий Ping, що в Connect: провідний 0x00 «закриває» півкадр, що міг лишитись
            // у буфері термінала після обриву посеред передачі (спека §2.3).
            r = session_->RequestPrimary(ping, pingTimeoutMs_.load(),
                                         FrameOptions{ /*leadingDelimiter=*/true });
            if (r.status != RequestStatus::Desynchronized) break;
            NEUTRAL_REPORT_WARN("ECRPrivatJSON", "EnsureReady: Desynchronized після MarkSynchronized - повтор");
        }
        if (r.status == RequestStatus::Response || r.status == RequestStatus::Busy) {
            // Тестовий шов №23-біс (I1): МІЖ Response і SetLinkState тест сам рве з'єднання,
            // щоб детерміновано (без гонки потоків) відтворити «epoch пішла вперед, поки Ready
            // ще не записаний». У продакшені хук не встановлюється (nullptr) - без ефекту.
            if (beforeReadyHookForTest_) beforeReadyHookForTest_();
            // Busy = живий, зайнятий нашою операцією - теж «чує нас». Ready із мертвої епохи
            // SetLinkState відкине сам.
            SetLinkState(LinkState::Ready, "", epoch);
            return IsReady();
        }
        if (r.status == RequestStatus::Disconnected || r.status == RequestStatus::Stopped)
            return false;                            // сокет упав знову - перезапустить хук
        // Timeout тут (або Desynchronized ЩЕ РАЗ після повтору вище - вкрай малоймовірно) -
        // не аварія, а очікуваний стан «термінал ще тримає стару сесію» (монополія, спека
        // §2.3), тож повторюємо з backoff без обмеження кількості циклів.
        SleepInterruptible(backoff);
        const int next = backoff * 2;
        backoff = (next > kReconnectMaxDelayMs) ? kReconnectMaxDelayMs : next;
    }
    return IsReady();
}

ResultEnvelope EcrPrivatJsonDriver::CaptureOutcome(std::uint64_t generation) {
    NEUTRAL_REPORT_WARN("ECRPrivatJSON", "З'ясування долі операції: полінг статусу термінала");
    bool idle = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(outcomeIdleWaitMs_.load());
    while (std::chrono::steady_clock::now() < deadline) {
        if (closing_.load()) return ResultEnvelope::Fail("ABORTED", "Відключення");
        {
            std::lock_guard<std::mutex> lk(outcomeMutex_);
            if (lastOutcome_.generation != generation)
                return ResultEnvelope::Fail("ABORTED", "Знімок належить іншому поколінню");
        }
        int code = -1;
        const RequestStatus st = PollStatusOnce(code);
        if (st == RequestStatus::Disconnected || st == RequestStatus::Stopped)
            // Продовжувати на непідтвердженому з'єднанні не можна: наступний up=true
            // приведе сюди знову - через крок 0 (Ping).
            return ResultEnvelope::Fail("ABORTED", "Зв'язок обірвався під час з'ясування");
        if (st == RequestStatus::Response && code >= 0) {
            lastStatus_.store(code);
            if (code == 0) { idle = true; break; }   // термінал у спокої (спека §6.5)
        }
        SleepInterruptible(kPollIntervalMs);
    }

    // Канал відкриваємо в ОБОХ випадках: після спокою - штатно; після вичерпання ліміту -
    // бо лишити desync назавжди нікому було б зняти (гейт після Resolved не діє), а нова
    // операція чесно отримає deviceBusy/Timeout від самого термінала. Різниця видима
    // у знімку: terminalIdle і factsCode.
    if (session_) session_->MarkSynchronized();

    ResultEnvelope facts = idle
        ? RequestReceiptFacts(std::string{})
        : ResultEnvelope::Fail("TERMINAL_BUSY", "Термінал не звільнився за відведений час");
    if (idle && (facts.code == "DISCONNECTED" || facts.code == "STOPPED"))
        return ResultEnvelope::Fail("ABORTED", "Зв'язок обірвався під час отримання чека");

    OperationIntent snapIntent; std::string snapReason;     // копії для лоґу - читаються поза локом
    {
        std::lock_guard<std::mutex> lk(outcomeMutex_);
        if (lastOutcome_.generation != generation)
            return ResultEnvelope::Fail("ABORTED", "Знімок належить іншому поколінню");
        lastOutcome_.state        = OutcomeState::Resolved;
        lastOutcome_.terminalIdle = idle;
        lastOutcome_.facts        = facts;
        snapIntent = lastOutcome_.intent;
        snapReason = lastOutcome_.reason;
    }
    // Єдиний слід знімка, якщо каса його не забрала, а наступний Pending затер (спека §4.3 крок 6,
    // §9 п.7). БЕЗ pan і без тексту чека. Конкатенація, без printf-стилю (AGENTS.md). Обидва часи -
    // startedAt і capturedAt - на прохання боку 1С: спільний якір із їхнім реєстром при розборі.
    {
        const auto fp = [&](const char* key) {
            return facts.payload.is_object() ? facts.payload.value(key, std::string{}) : std::string{};
        };
        NEUTRAL_REPORT_WARN("ECRPrivatJSON",
            "Доля операції з'ясована: generation=" + std::to_string(generation)
            + " reason=" + snapReason + " requestId=" + snapIntent.requestId
            + " intent=" + snapIntent.method + "/" + snapIntent.amount
            + " startedAt=" + IsoUtc(snapIntent.startedAt)
            + " capturedAt=" + IsoUtc(std::chrono::system_clock::now())
            + " terminalIdle=" + std::string(idle ? "true" : "false")
            + " factsOk=" + std::string(facts.ok ? "true" : "false") + " factsCode=" + facts.code
            + " responseCode=" + fp("responseCode") + " rrn=" + fp("rrn")
            + " invoiceNumber=" + fp("invoiceNumber") + " amount=" + fp("amount")
            + " date=" + fp("date") + " time=" + fp("time"));
    }
    // Подія несе ЛИШЕ об'єкт outcome (конверт є в result і в ИсходПоследнейОперацииJSON).
    EmitEvent("outcome", OutcomeSnapshotJson());
    // Значення НЕ читається: факти беруться з lastOutcome_ під outcomeMutex_.
    return ResultEnvelope::Ok();
}

void EcrPrivatJsonDriver::WaitOutcomeBounded(std::uint64_t generation) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(outcomeSyncWaitMs_.load());
    while (std::chrono::steady_clock::now() < deadline) {
        if (closing_.load()) return;   // Disconnect у процесі - не тримати job_.Join() до кінця syncWait
        {
            std::lock_guard<std::mutex> lk(outcomeMutex_);
            if (lastOutcome_.generation != generation) return;
            if (lastOutcome_.state == OutcomeState::Resolved) return;
        }
        const JobState js = recoveryJob_.State();
        if (js != JobState::Running && js != JobState::Interrupting) return;   // працювати нікому
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
}

ResultEnvelope EcrPrivatJsonDriver::RequestReceiptFacts(const std::string& invoiceNumber) {
    if (!session_) return ResultEnvelope::Fail("NOT_CONNECTED", "Термінал не підключено");
    // Таймаут операційний, не хендшейковий: під авторизацією на хості 5с не вистачає (спека §1.3).
    auto req = EcrJsonCodec::BuildRequest("GetReceiptInfo", 0,
                                          nlohmann::json{{"invoiceNumber", invoiceNumber}});
    GateSend();
    RequestResult r = session_->RequestPrimary(req, kOperationTimeoutMs);
    return MapResult(r);
}

// ⚠️ WHITELIST фінансових методів - на нього спираються і гейт §4.6, і наскрізний requestId,
// і сам тригер Pending. Метод драйвера, що рухає гроші й не доданий сюди, ТИХО випаде з усіх
// трьох: жодної помилки збірки, а дефект - про гроші. Це НЕ гіпотетично: протокол ПриватБанку
// вже описує фінансові методи, яких драйвер поки не реалізує - Cashback (§5.16),
// Preauthorization (§5.26), SaleCompletion (§5.27), Withdrawal/WithdrawalPartly, ServiceRefund,
// ServicePbP/ServiceRefPbP (docs/ECR_Privat_JSON_Protokol.md). Реалізуєш будь-який - додай сюди
// й у тести тригера (Task 2). Скасування (Void) протокол не має: RunVoid фасаду відкочується
// на Refund (AcquiringFacadeBase.cpp:49-68), тому збіг переліку з командами БПО
// {Sales, Refund, Void} сьогодні тримається на цьому відкаті.
bool EcrPrivatJsonDriver::IsFinancial(const std::string& method) {
    return method == "Purchase" || method == "Refund";
}

void EcrPrivatJsonDriver::SetRequestId(std::string id) {
    std::lock_guard<std::mutex> lk(outcomeMutex_);
    pendingRequestId_ = std::move(id);
}

void EcrPrivatJsonDriver::SetTransportFactoryForTest(TransportFactory f) { transportFactory_ = std::move(f); }

void EcrPrivatJsonDriver::SetOutcomeTimingForTest(int idleWaitMs, int syncWaitMs, int pingTimeoutMs) {
    outcomeIdleWaitMs_.store(idleWaitMs);
    outcomeSyncWaitMs_.store(syncWaitMs);
    if (pingTimeoutMs > 0) pingTimeoutMs_.store(pingTimeoutMs);
}

void EcrPrivatJsonDriver::MarkPendingForTest(const std::string& method, const std::string& amount,
                                             const std::string& reason) {
    OperationIntent intent;
    intent.method    = method;
    intent.amount    = amount;
    intent.startedAt = std::chrono::system_clock::now();
    MarkPending(intent, reason);
}

void EcrPrivatJsonDriver::SetBeforeReadyHookForTest(std::function<void()> hook) {
    beforeReadyHookForTest_ = std::move(hook);
}

void EcrPrivatJsonDriver::StopSessionForTest() { if (session_) session_->Stop(); }

std::uint64_t EcrPrivatJsonDriver::MarkPending(const OperationIntent& intent, const std::string& reason) {
    std::lock_guard<std::mutex> lk(outcomeMutex_);
    const std::uint64_t gen = lastOutcome_.generation + 1;   // ідентифікатор питання для 1С
    lastOutcome_ = LastOutcome{};
    lastOutcome_.state      = OutcomeState::Pending;
    lastOutcome_.generation = gen;
    lastOutcome_.intent     = intent;
    lastOutcome_.reason     = reason;
    return gen;
}

const char* EcrPrivatJsonDriver::LinkStateNameNow() const {
    std::lock_guard<std::mutex> lk(linkMutex_);
    return LinkStateName(linkState_);   // вказівник на строковий літерал — час життя необмежений
}

nlohmann::json EcrPrivatJsonDriver::OutcomeSnapshotJson() {
    const bool channel = IsConnected();          // ПОЗА локом: чіпає сесію, не lastOutcome_
    const char* link   = LinkStateNameNow();     // ПОЗА локом: бере linkMutex_ (див. оголошення)
    std::lock_guard<std::mutex> lk(outcomeMutex_);
    return nlohmann::json{
        {"state",      OutcomeStateName(lastOutcome_.state)},
        {"generation", lastOutcome_.generation},
        {"reason",     lastOutcome_.reason},
        {"intent", nlohmann::json{
            {"method",    lastOutcome_.intent.method},
            {"amount",    lastOutcome_.intent.amount},
            {"rrn",       lastOutcome_.intent.rrn},
            {"requestId", lastOutcome_.intent.requestId},
            {"startedAt", IsoUtc(lastOutcome_.intent.startedAt)}}},
        {"terminalIdle", lastOutcome_.terminalIdle},
        // facts - сирі поля §5.30 як є; null, поки доля не з'ясована.
        {"facts", lastOutcome_.state == OutcomeState::Resolved
                      ? lastOutcome_.facts.payload : nlohmann::json(nullptr)},
        {"factsOk",   lastOutcome_.facts.ok},
        {"factsCode", lastOutcome_.facts.code},
        {"channelConnected", channel},
        // Стан зв'язку рядком (спека §4.7, §9 п.5). Розрізняє те, чого channelConnected не
        // розрізняє: "connecting" - сесія жива, супервізор перевідкриває канал, касі ЧЕКАТИ;
        // "disconnected" - сесії немає (хтось викликав Отключить - конфігурація після команди
        // або каса вручну), і без Подключить з'ясування не відновиться взагалі. Значення дає
        // та сама LinkStateName, що й подія "connection", тож два джерела не розійдуться.
        {"linkState", link}};
}

ResultEnvelope EcrPrivatJsonDriver::BuildUnknownOutcome() {
    ResultEnvelope env = ResultEnvelope::Fail("UNKNOWN_OUTCOME",
        "Зв'язок із терміналом обірвався під час операції; доля невідома");
    env.payload = nlohmann::json{{"outcome", OutcomeSnapshotJson()}};
    return env;
}

ResultEnvelope EcrPrivatJsonDriver::InquireLastOutcome() {
    EnsureRecoveryRunning();   // вікно «Pending без виконавця» закривається тут
    bool none;
    { std::lock_guard<std::mutex> lk(outcomeMutex_); none = lastOutcome_.state == OutcomeState::None; }
    if (!none) return BuildUnknownOutcome();
    ResultEnvelope env = ResultEnvelope::Ok();
    env.payload = nlohmann::json{{"outcome", OutcomeSnapshotJson()}};
    return env;
}

RequestStatus EcrPrivatJsonDriver::PollStatusOnce(int& code) {
    if (!session_) return RequestStatus::Disconnected;
    auto stat = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType", "getLastStatMsgCode"}});
    GateSend();
    RequestResult s = session_->RequestService(stat, kServiceTimeoutMs);
    if (s.status != RequestStatus::Response) return s.status;
    ParsedResponse pr = EcrJsonCodec::Parse(s.frame);
    if (pr.valid && pr.params.is_object()) {
        const std::string c = pr.params.value("LastStatMsgCode", std::string{});
        if (!c.empty()) { try { code = std::stoi(c); } catch (...) {} }
    }
    return s.status;
}

ResultEnvelope EcrPrivatJsonDriver::Purchase(const std::string& amount, const nlohmann::json& extra) {
    nlohmann::json p = extra.is_object() ? extra : nlohmann::json::object();
    p["amount"] = amount;
    FillPaymentDefaults(p, /*withFacepay=*/true);
    return Execute("Purchase", p, kOperationTimeoutMs);
}

ResultEnvelope EcrPrivatJsonDriver::Refund(const std::string& amount, const std::string& rrn, const nlohmann::json& extra) {
    nlohmann::json p = extra.is_object() ? extra : nlohmann::json::object();
    p["amount"] = amount; p["rrn"] = rrn;
    FillPaymentDefaults(p, /*withFacepay=*/false);
    return Execute("Refund", p, kOperationTimeoutMs);
}

ResultEnvelope EcrPrivatJsonDriver::Audit(const std::string& merchantId) {
    // §5.17: X-звіт (підсумки БЕЗ вилучення) — {merchantId}; відповідь {receipt}.
    return Execute("Audit", {{"merchantId", merchantId}}, kOperationTimeoutMs);
}

ResultEnvelope EcrPrivatJsonDriver::Verify(const std::string& merchantId) {
    // §5.18: Звірка (Загальний звіт) — підсумки на хост для звірки; йде до хоста,
    // тож таймаут операційний, як у Purchase.
    return Execute("Verify", {{"merchantId", merchantId}}, kOperationTimeoutMs);
}

ResultEnvelope EcrPrivatJsonDriver::CheckConnection() {
    return Execute("CheckConnection", nlohmann::json::object(), kHandshakeTimeoutMs);
}

ResultEnvelope EcrPrivatJsonDriver::GetReceiptInfo(const std::string& invoiceNumber) {
    return Execute("GetReceiptInfo", nlohmann::json{{"invoiceNumber", invoiceNumber}}, kHandshakeTimeoutMs);
}
