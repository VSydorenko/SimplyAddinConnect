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
    s->SetUnsolicitedHandler([](std::vector<uint8_t>) { /* deviceBusy/нотифікації — Частина 2 */ });
    s->SetConnectionStateHandler([](bool) { /* стан зв'язку — Частина 2 (події в 1С) */ });
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
        if (r.status != RequestStatus::Response) {
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

    // 3) Постійний режим: сесія лишається відкритою (реконект — супервізор DeviceSession).
    session_ = MakeSession(params_);
    if (!session_->Start()) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Постійний режим: не вдалося відкрити зв'язок");
        session_.reset();
        return false;
    }
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
    job_.Join();   // дочекатись worker, щоб не рвати сесію під активним запитом
    if (session_) {
        session_->Stop();
        session_.reset();
        NEUTRAL_REPORT_INFO("ECRPrivatJSON", "Disconnect: сесію закрито");
    } else {
        // Повторний Отключить без активної сесії — легальний no-op; пишемо в лог,
        // щоб «нічого не сталося на терміналі» не виглядало як збій (2026-08-29).
        NEUTRAL_REPORT_INFO("ECRPrivatJSON", "Disconnect: сесії немає (вже відключено) — no-op");
    }
}

bool EcrPrivatJsonDriver::IsConnected() const { return session_ && session_->IsConnected(); }
std::string EcrPrivatJsonDriver::Vendor() const { return vendor_; }
std::string EcrPrivatJsonDriver::Model() const { return model_; }

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

    if (!IsConnected()) return ResultEnvelope::Fail("NOT_CONNECTED", "Термінал не підключено");
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
        const std::uint64_t gen = MarkPending(intent, reason);

        // ТИМЧАСОВО (до Task 4): при живому з'єднанні з'ясовуємо синхронно. Task 4
        // замінює цей блок на EnsureRecoveryRunning() + WaitOutcomeBounded(gen).
        if (IsConnected() && !inRecovery_.load()) {
            inRecovery_.store(true);
            const ResultEnvelope facts = RecoverAfterDesync();
            inRecovery_.store(false);
            std::lock_guard<std::mutex> lk(outcomeMutex_);
            if (lastOutcome_.generation == gen) {
                lastOutcome_.state        = OutcomeState::Resolved;
                lastOutcome_.terminalIdle = (lastStatus_.load() == 0);
                lastOutcome_.facts        = facts;
            }
        }
        env = BuildUnknownOutcome();
    } else {
        env = MapResult(r);
    }
    // Завершення операції → у 1С (фінальний результат: approved/declined/помилка).
    EmitEvent("result", { {"ok", env.ok}, {"code", env.code}, {"description", env.description},
                          {"state", env.ok ? "Done" : "Error"}, {"payload", env.payload} });
    return env;
}

ResultEnvelope EcrPrivatJsonDriver::RecoverAfterDesync() {
    NEUTRAL_REPORT_WARN("ECRPrivatJSON", "Відновлення після десинхронізації: полінг статусу термінала");
    // Полимо getLastStatMsgCode ПОКИ код != "0" (bounded): "0" = термінал у спокої (спека §6.5).
    // НЕ рвемо на будь-якій Response — лише коли статус реально спокійний.
    for (int i = 0; i < kRecoverPollTries && session_; ++i) {
        int code = -1;
        if (PollStatusOnce(code) == RequestStatus::Response && code >= 0) {
            lastStatus_.store(code);                 // без події: це фон, не хід операції
            if (code == 0) break;                    // термінал у спокої (спека §6.5)
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
    if (session_) session_->MarkSynchronized();
    // best-effort: деталі останнього чека. НЕ через ExecuteInternal - той емітить події
    // й підмінив би результат операції чужим чеком (дефект 1.2 спеки).
    return RequestReceiptFacts(std::string{});
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

nlohmann::json EcrPrivatJsonDriver::OutcomeSnapshotJson() {
    const bool channel = IsConnected();   // ПОЗА локом: чіпає сесію, не lastOutcome_
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
        {"channelConnected", channel}};
}

ResultEnvelope EcrPrivatJsonDriver::BuildUnknownOutcome() {
    ResultEnvelope env = ResultEnvelope::Fail("UNKNOWN_OUTCOME",
        "Зв'язок із терміналом обірвався під час операції; доля невідома");
    env.payload = nlohmann::json{{"outcome", OutcomeSnapshotJson()}};
    return env;
}

ResultEnvelope EcrPrivatJsonDriver::InquireLastOutcome() {
    // Task 4 додасть тут EnsureRecoveryRunning() - самозцілення (спека §4.4).
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
