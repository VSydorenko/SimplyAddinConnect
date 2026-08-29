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
        auto stat = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType", "getLastStatMsgCode"}});
        GateSend();
        RequestResult s = session_->RequestService(stat, kServiceTimeoutMs);
        if (s.status == RequestStatus::Response) {
            ParsedResponse pr = EcrJsonCodec::Parse(s.frame);
            if (pr.valid && pr.params.is_object()) {
                std::string code = pr.params.value("LastStatMsgCode", std::string{});
                if (!code.empty()) {
                    try {
                        int c = std::stoi(code);
                        if (c != lastStatus_.exchange(c))   // статус змінився → подія в 1С
                            EmitEvent("status", { {"code", c}, {"text", StatusText(c)}, {"state", "Running"} });
                    } catch (...) {}
                }
            }
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

    // Обрив у польоті транзакції: session у desync — best-effort відновлення (service вільний).
    ResultEnvelope env;
    if (r.status == RequestStatus::Timeout && session_ && session_->IsDesynchronized() && !inRecovery_.load()) {
        inRecovery_.store(true);
        env = RecoverAfterDesync();
        inRecovery_.store(false);
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
        auto stat = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType", "getLastStatMsgCode"}});
        GateSend();
        RequestResult s = session_->RequestService(stat, kServiceTimeoutMs);
        if (s.status == RequestStatus::Response) {
            ParsedResponse pr = EcrJsonCodec::Parse(s.frame);
            if (pr.valid && pr.params.is_object()) {
                std::string code = pr.params.value("LastStatMsgCode", std::string{});
                if (!code.empty()) {
                    try { lastStatus_.store(std::stoi(code)); } catch (...) {}
                    if (code == "0") break;   // термінал у спокої (спека §6.5)
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
    if (session_) session_->MarkSynchronized();
    // best-effort: деталі останнього чека. ExecuteInternal (НЕ публічний GetReceiptInfo):
    // inRecovery_ вже true → повторного recovery не станеться; interruptRequested_ не ресетиться.
    return ExecuteInternal("GetReceiptInfo", nlohmann::json{{"invoiceNumber", std::string{}}}, kHandshakeTimeoutMs);
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

ResultEnvelope EcrPrivatJsonDriver::CheckConnection() {
    return Execute("CheckConnection", nlohmann::json::object(), kHandshakeTimeoutMs);
}

ResultEnvelope EcrPrivatJsonDriver::GetReceiptInfo(const std::string& invoiceNumber) {
    return Execute("GetReceiptInfo", nlohmann::json{{"invoiceNumber", invoiceNumber}}, kHandshakeTimeoutMs);
}
