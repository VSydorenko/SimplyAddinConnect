#include "../core/pch.h"
#include "DeviceSession.h"
#include "../helpers/ServiceTools.h"

#include <algorithm>
#include <chrono>
#include <string>

// Мапінг classifier-локального RejectReason у публічний RequestStatus (§4.3, §7).
static RequestStatus MapReject(RejectReason reason) {
    return reason == RejectReason::Unsupported ? RequestStatus::Unsupported
                                               : RequestStatus::Busy;
}

/**
 * @file DeviceSession.cpp
 * @brief Реалізація device-сесії (§4.4, §5, §6, §7 дизайну).
 *
 * У ЦІЙ фазі (Task 4-5) реалізовано: каркас; Start (Open + підписка колбеків +
 * dispatcher-потік); Stop (базовий teardown §6 кроки 1-6); RequestPrimary/
 * RequestService (алгоритм §5 — send→wait→precedence) через спільний DoRequest;
 * OnBytes — усі гілки Classify: PrimaryResponse/ServiceResponse (дві доріжки,
 * service паралельно primary), RejectPrimary/RejectService (мапінг RejectReason→
 * RequestStatus негайно, без таймауту), RejectBoth (обидві доріжки + desync +
 * reconnectRequested_), Unsolicited (кадр у dispatch-чергу). Реконект, таймаут-desync,
 * epoch-guard проти stale, повний reentrancy-teardown — дороблюються в Task 6-7.
 */

DeviceSession::DeviceSession(std::unique_ptr<ITransport> transport,
                             std::unique_ptr<IFramer> framer,
                             std::unique_ptr<IFrameClassifier> classifier,
                             SessionConfig cfg)
    : transport_(std::move(transport)),
      framer_(std::move(framer)),
      classifier_(std::move(classifier)),
      cfg_(cfg) {
    // §4.4/§14: під'єднати ліміт буфера кадрувальника з конфіга сесії (#L).
    if (framer_) framer_->SetMaxBufferedBytes(cfg_.maxBufferedBytes);
}

DeviceSession::~DeviceSession() {
    Stop();  // ідемпотентний
    // Фінальний join dispatcher-а — на випадок, якщо Stop() викликано з dispatcher-потоку
    // (self-join там свідомо пропущено; §6 крок 5).
    if (dispatcher_.joinable() &&
        std::this_thread::get_id() != dispatcher_.get_id()) {
        dispatcher_.join();
    }
}

bool DeviceSession::Start() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (started_) {
            NEUTRAL_REPORT_WARN("DeviceSession", "Повторний Start проігноровано");
            return connected_;
        }
        started_ = true;
        stopping_ = false;
        desiredUp_ = true;
    }

    // Підписка транспортних колбеків ДО Open (щоб state(true) не загубився).
    transport_->SetDataReceivedCallback(
        [this](const std::vector<uint8_t>& bytes) { OnBytes(bytes); });
    transport_->SetConnectionStateCallback(
        [this](bool up) { OnTransportState(up); });
    transport_->SetErrorCallback(
        [this](const std::string& msg, int code) { OnTransportError(msg, code); });

    // Dispatcher-потік — ДО Open (Inline-транспорт доставляє state синхронно в Open).
    dispatcher_ = std::thread([this] { DispatchLoop(); });

    // Первинна спроба Open робиться тут (щоб уникнути гонки супервізора з Open, супервізор
    // піднімаємо ПІСЛЯ). Невдалий Open → просимо супервізора ретраїти через reconnectRequested_.
    bool opened = transport_->Open();
    if (opened) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::milliseconds(cfg_.connectDeadlineMs),
                     [this] { return connected_ || stopping_; });
    } else {
        NEUTRAL_REPORT_WARN("DeviceSession", "Транспорт не відкрився при Start — реконект супервізором");
        std::lock_guard<std::mutex> lk(m_);
        reconnectRequested_ = true;   // прокинути супервізора саме за reconnectRequested_
    }

    // Супервізор реконекту — ПІСЛЯ первинного Open. При успіху конекту його предикат
    // (reconnectRequested_ || !connected_) хибний і він мирно чекає.
    supervisor_ = std::thread([this] { ReconnectLoop(); });
    {
        std::lock_guard<std::mutex> lk(m_);
        supervisorCv_.notify_all();
    }

    std::lock_guard<std::mutex> lk(m_);
    return connected_;
}

void DeviceSession::Stop() {
    // §6 крок 1: під m_ — stopping_, усі pending → Stopped, розбудити очікувачів.
    {
        std::lock_guard<std::mutex> lk(m_);
        if (stopping_) return;  // ідемпотентність
        stopping_ = true;
        desiredUp_ = false;
        FinishPendingLocked(RequestStatus::Stopped);
        cv_.notify_all();
        supervisorCv_.notify_all();   // розбудити супервізора на завершення
    }

    // §6 крок 2: розбудити+join супервізор (повний реконект — Task 6).
    if (supervisor_.joinable() &&
        std::this_thread::get_id() != supervisor_.get_id()) {
        supervisor_.join();
    }

    // §6 крок 3: закрити транспорт (без m_; контракт §4.1 — колбеків далі нема).
    if (transport_) transport_->Close();

    // §6 крок 4: від'єднати транспортні колбеки.
    if (transport_) {
        transport_->SetDataReceivedCallback(nullptr);
        transport_->SetConnectionStateCallback(nullptr);
        transport_->SetErrorCallback(nullptr);
    }

    // §6 крок 5: зупинити dispatcher. Self-join заборонено (Stop із dispatcher-потоку) —
    // тоді фінальний join робить ~DeviceSession з іншого потоку.
    {
        std::lock_guard<std::mutex> lk(dispatchMutex_);
        dispatchStop_ = true;
    }
    dispatchCv_.notify_all();
    if (dispatcher_.joinable() &&
        std::this_thread::get_id() != dispatcher_.get_id()) {
        dispatcher_.join();
    }

    // §6 крок 6: скинути кадрувальник.
    {
        std::lock_guard<std::mutex> fl(framerMutex_);
        if (framer_) framer_->Reset();
    }
}

bool DeviceSession::IsConnected() const {
    std::lock_guard<std::mutex> lk(m_);
    return connected_;
}

RequestResult DeviceSession::RequestPrimary(const std::vector<uint8_t>& payload,
                                            int timeoutMs, FrameOptions opts) {
    return DoRequest(pendingPrimary_, /*isPrimary=*/true, payload, timeoutMs, opts);
}

RequestResult DeviceSession::RequestService(const std::vector<uint8_t>& payload,
                                            int timeoutMs, FrameOptions opts) {
    return DoRequest(pendingService_, /*isPrimary=*/false, payload, timeoutMs, opts);
}

RequestResult DeviceSession::DoRequest(Pending& pending, bool isPrimary,
                                       const std::vector<uint8_t>& payload,
                                       int timeoutMs, FrameOptions opts) {
    const int timeout = timeoutMs < 0
        ? (isPrimary ? cfg_.primaryTimeoutMs : cfg_.serviceTimeoutMs)
        : timeoutMs;

    std::uint64_t myEpoch = 0;

    // §5.1: під m_ — перевірки та резервація pending. Без мережі, якщо відмова.
    {
        std::unique_lock<std::mutex> lk(m_);
        if (stopping_)                     return { RequestStatus::Stopped, {} };
        if (!connected_)                   return { RequestStatus::Disconnected, {} };
        if (isPrimary && desynchronized_)  return { RequestStatus::Desynchronized, {} };
        if (pending.active)                return { RequestStatus::Concurrent, {} };

        pending.active  = true;
        pending.done    = false;
        pending.payload = payload;
        pending.epoch   = epoch_;
        pending.result  = {};
        myEpoch = epoch_;
    }

    // §5.2: поза m_ — Wrap (під framerMutex_) → wire-trace(sendAttempt) → Send.
    std::vector<uint8_t> frame;
    {
        std::lock_guard<std::mutex> fl(framerMutex_);
        frame = framer_->Wrap(payload, opts);
    }
    EnqueueWireTrace(/*sendAttempt=*/true, frame);
    const int sent = transport_->Send(frame);

    // §5.3-5.4: під m_ — precedence + очікування результату.
    std::unique_lock<std::mutex> lk(m_);
    // SendFailed виставляємо ЛИШЕ якщо pending досі належить цьому запиту й не завершений
    // іншим джерелом (напр. state(false), викликаний Send'ом синхронно).
    if (sent < 0 && pending.active && pending.epoch == myEpoch && !pending.done) {
        pending.result = { RequestStatus::SendFailed, {} };
        pending.done = true;
    }

    cv_.wait_for(lk, std::chrono::milliseconds(timeout),
                 [&] { return pending.done || stopping_ || !connected_; });

    RequestResult result;
    if (pending.done) {
        result = pending.result;
    } else if (stopping_) {
        result = { RequestStatus::Stopped, {} };
    } else if (!connected_) {
        result = { RequestStatus::Disconnected, {} };
    } else {
        // Справжній таймаут (з'єднання живе, не зупинено, відповіді нема).
        result = { RequestStatus::Timeout, {} };
        if (isPrimary) {
            // §7: нема ID запиту — пізня відповідь могла б зматчитись на наступний primary,
            // тож переходимо в desync (знімає лише MarkSynchronized) і просимо реконект.
            desynchronized_ = true;
            reconnectRequested_ = true;
            supervisorCv_.notify_all();
        } else {
            // §7/§14/#H2: service-таймаут без desync, але карантинимо дискримінатор цього
            // service — пізній дубль/reject проковтнути, щоб він не завершив новий service.
            // Карантин ОБМЕЖЕНИЙ і scoped: діє лише в межах вікна (~ один serviceTimeout)
            // і лише в поточній генерації; після вікна/зміни epoch кадр — легітимний.
            ++serviceQuarantine_;
            serviceQuarantineEpoch_ = epoch_;
            serviceQuarantineDeadline_ = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(cfg_.serviceTimeoutMs > 0 ? cfg_.serviceTimeoutMs : 0);
        }
    }

    // Звільнити доріжку (запит завершено).
    pending.active = false;
    pending.done = false;
    pending.payload.clear();
    pending.result = {};
    return result;
}

void DeviceSession::OnBytes(const std::vector<uint8_t>& chunk) {
    // §8: incoming-trace чанку — у чергу ДО unsolicited того ж чанку (FIFO).
    EnqueueWireTrace(/*sendAttempt=*/false, chunk);

    std::vector<std::vector<uint8_t>> frames;
    {
        std::lock_guard<std::mutex> fl(framerMutex_);
        try {
            framer_->Feed(chunk, frames);
        } catch (...) {
            NEUTRAL_REPORT_WARN("DeviceSession", "Виняток кадрувальника у Feed, чанк відкинуто");
            return;
        }
    }

    for (auto& frame : frames) {
        std::lock_guard<std::mutex> lk(m_);
        PendingView pv{
            pendingPrimary_.active ? &pendingPrimary_.payload : nullptr,
            pendingService_.active ? &pendingService_.payload : nullptr
        };
        Classification cls;
        try {
            cls = classifier_->Classify(pv, frame);
        } catch (...) {
            NEUTRAL_REPORT_WARN("DeviceSession", "Виняток класифікатора, кадр відкинуто");
            continue;
        }

        // Guard проти stale-кадрів (§14): доріжку завершує лише відповідь ТІЄЇ Ж генерації.
        // Кадр старого epoch (пізня відповідь до реконекту) не завершує новий запит.
        const bool primaryLive =
            pendingPrimary_.active && !pendingPrimary_.done && pendingPrimary_.epoch == epoch_;
        const bool serviceLive =
            pendingService_.active && !pendingService_.done && pendingService_.epoch == epoch_;

        // #H2: карантин дискримінатора service — пізній дубль/reject таймаутнутого service
        // проковтнути (щоб не завершив/десинхронізував новий), але ЛИШЕ поки карантин
        // ПЛАУЗИБЕЛЬНО належить таймаутнутому запиту: у межах вікна і тієї ж генерації.
        // Після вікна/зміни epoch — карантин знімаємо, кадр трактуємо як легітимний.
        auto quarantineSwallowsService = [&]() -> bool {
            if (serviceQuarantine_ <= 0) return false;
            const bool plausible =
                serviceQuarantineEpoch_ == epoch_ &&
                std::chrono::steady_clock::now() < serviceQuarantineDeadline_;
            if (plausible) { --serviceQuarantine_; return true; }
            serviceQuarantine_ = 0;   // вікно минуло / інша генерація → карантин зняти
            return false;
        };

        switch (cls.cls) {
        case FrameClass::PrimaryResponse:
            if (primaryLive) {
                pendingPrimary_.result = { RequestStatus::Response, frame };
                pendingPrimary_.done = true;
                cv_.notify_all();
            }
            break;
        case FrameClass::ServiceResponse:
            // Карантин (§7/§14/#H2): пізній дубль timed-out service — проковтнути (bounded).
            if (quarantineSwallowsService()) break;
            // Доріжка service серіалізована серед service, але паралельна primary.
            if (serviceLive) {
                pendingService_.result = { RequestStatus::Response, frame };
                pendingService_.done = true;
                serviceQuarantine_ = 0;   // #H2: новий service завершено — карантин зняти
                cv_.notify_all();
            }
            break;
        case FrameClass::RejectPrimary:
            // Явний reject primary → завершити НЕГАЙНО (без таймауту) з мапінгом reason.
            if (primaryLive) {
                pendingPrimary_.result = { MapReject(cls.reason), {} };
                pendingPrimary_.done = true;
                cv_.notify_all();
            }
            break;
        case FrameClass::RejectService:
            // #H2: пізній reject таймаутнутого service не має завершити новий.
            if (quarantineSwallowsService()) break;
            if (serviceLive) {
                pendingService_.result = { MapReject(cls.reason), {} };
                pendingService_.done = true;
                serviceQuarantine_ = 0;   // #H2: новий service завершено — карантин зняти
                cv_.notify_all();
            }
            break;
        case FrameClass::RejectBoth:
            // #H2: пізній reject таймаутнутого service не має ДЕСИНХРОНІЗУВАТИ/завершити
            // новий — якщо карантин плаузибельно ковтає цей кадр, трактуємо як старий (skip).
            if (quarantineSwallowsService()) break;
            // Неоднозначний reject при обох pending: немає причинного ID, який запит
            // відхилено → завершити ОБИДВІ доріжки + перевести сесію в desync (§7).
            if (primaryLive) {
                pendingPrimary_.result = { MapReject(cls.reason), {} };
                pendingPrimary_.done = true;
            }
            if (serviceLive) {
                pendingService_.result = { MapReject(cls.reason), {} };
                pendingService_.done = true;
            }
            desynchronized_ = true;
            reconnectRequested_ = true;
            cv_.notify_all();
            supervisorCv_.notify_all();   // будити супервізор реконекту
            break;
        case FrameClass::Unsolicited:
        default:
            // Кадр без прив'язки до доріжки → user-хендлер на dispatcher-потоці (не тут).
            {
                std::vector<uint8_t> f = frame;
                Enqueue([this, f = std::move(f)]() mutable {
                    std::function<void(std::vector<uint8_t>)> h;
                    { std::lock_guard<std::mutex> lk(m_); h = unsolicitedHandler_; }
                    if (h) h(std::move(f));
                });
            }
            break;
        }
    }
}

void DeviceSession::OnTransportState(bool up) {
    {
        std::lock_guard<std::mutex> lk(m_);
        connected_ = up;
        if (!up) {
            ++epoch_;                 // нова генерація — guard проти stale-кадрів (§14)
            serviceQuarantine_ = 0;   // карантин діє «до реконекту» — скидаємо
            // Обрив трактуємо як запит на реконект, ОКРІМ навмисного Close супервізора
            // (інакше після нашого ж Close залишився б хибний reconnectRequested_ → цикл).
            if (!expectedClose_) {
                reconnectRequested_ = true;
                reconnectGaveUp_ = false;   // #H3: новий намір реконекту — скинути give-up латч
            }
        }
        cv_.notify_all();
        supervisorCv_.notify_all();
    }
    // Прокинути стан драйверу — лише на dispatcher-потоці.
    Enqueue([this, up] {
        std::function<void(bool)> h;
        { std::lock_guard<std::mutex> lk(m_); h = stateHandler_; }
        if (h) h(up);
    });
}

void DeviceSession::OnTransportError(const std::string& message, int code) {
    NEUTRAL_REPORT_WARN("DeviceSession",
        "Помилка транспорту: " + message + " (код " + std::to_string(code) + ")");
}

void DeviceSession::FinishPendingLocked(RequestStatus status) {
    if (pendingPrimary_.active && !pendingPrimary_.done) {
        pendingPrimary_.result = { status, {} };
        pendingPrimary_.done = true;
    }
    if (pendingService_.active && !pendingService_.done) {
        pendingService_.result = { status, {} };
        pendingService_.done = true;
    }
}

void DeviceSession::ReconnectLoop() {
    // Супервізор реконекту. Предикат пробудження — desiredUp_ && autoReconnect &&
    // (reconnectRequested_ || !connected_): НЕ лише !connected_, бо після таймауту primary
    // connected_ лишається true (§5). Успіх реконекту — ЛИШЕ state(true) у межах
    // connectDeadlineMs (§4.1). Backoff cfg.reconnectDelayMs..reconnectMaxDelayMs.
    int backoff = cfg_.reconnectDelayMs;
    int tries = 0;

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m_);
            supervisorCv_.wait(lk, [this] {
                return stopping_ ||
                       (desiredUp_ && cfg_.autoReconnect &&
                        (reconnectRequested_ ||
                         (!connected_ && !reconnectGaveUp_)));   // #H3: give-up латч гасить цикл
            });
            if (stopping_) return;

            // #H3: свіжий намір реконекту (reconnectRequested_ від обриву/зовн.запиту) →
            // новий бюджет спроб; продовження внутрішнього ретраю tries НЕ обнуляє.
            if (reconnectRequested_) {
                tries = 0;
                reconnectGaveUp_ = false;
            }
            // Споживаємо запит; завершуємо ще-активні pending як Disconnected (обрив/реконект).
            reconnectRequested_ = false;
            FinishPendingLocked(RequestStatus::Disconnected);
            connected_ = false;
            expectedClose_ = true;    // наступний Close — навмисний (не хибний reconnectRequested_)
            cv_.notify_all();
        }

        // Кадрувальник — у чисте: старі байти не переносимо в нову генерацію.
        {
            std::lock_guard<std::mutex> fl(framerMutex_);
            if (framer_) framer_->Reset();
        }

        // Backoff (скасовний через stopping_).
        {
            std::unique_lock<std::mutex> lk(m_);
            if (supervisorCv_.wait_for(lk, std::chrono::milliseconds(backoff),
                                       [this] { return stopping_; })) {
                return;
            }
        }

        // Спроба реконекту: Close → Open (без m_ — контракт §4.1, ніколи під локом).
        transport_->Close();
        const bool opened = transport_->Open();

        bool ok = false;
        {
            std::unique_lock<std::mutex> lk(m_);
            if (opened) {
                supervisorCv_.wait_for(lk, std::chrono::milliseconds(cfg_.connectDeadlineMs),
                                       [this] { return connected_ || stopping_; });
                ok = connected_ && !stopping_;
            }
            expectedClose_ = false;
            if (stopping_) return;
        }

        if (ok) {
            backoff = cfg_.reconnectDelayMs;   // скинути backoff на майбутнє
            tries = 0;
            // desynchronized_ НЕ знімаємо автоматично — лише MarkSynchronized() (§5).
        } else {
            // Невдача: збільшити backoff. Наступну спробу драйвить сам предикат супервізора
            // (!connected_ && !reconnectGaveUp_) — reconnectRequested_ тут НЕ ставимо, інакше
            // «свіжий-намір → tries=0» обнуляв би бюджет спроб щоітерації, і maxTries ніколи
            // б не досягався (#H3).
            backoff = (std::min)(backoff * 2, cfg_.reconnectMaxDelayMs);
            ++tries;
            if (cfg_.reconnectMaxTries > 0 && tries >= cfg_.reconnectMaxTries) {
                std::lock_guard<std::mutex> lk(m_);
                reconnectGaveUp_ = true;   // латч: цикл Close+Open спиняється (предикат гасне)
                NEUTRAL_REPORT_WARN("DeviceSession", "Реконект вичерпав спроби: " +
                                    std::to_string(tries));
            }
            // else: латч не ставимо — предикат сам ретраїть після backoff.
        }
    }
}

void DeviceSession::DispatchLoop() {
    std::unique_lock<std::mutex> lk(dispatchMutex_);
    for (;;) {
        dispatchCv_.wait(lk, [this] { return dispatchStop_ || !dispatchQueue_.empty(); });
        // #H4: внутрішній drain перевіряє dispatchStop_ — реентрантний Stop() з першого
        // хендлера чанку не має пропускати наступні колбеки (§14 «нуль колбеків», §6 крок 5).
        while (!dispatchStop_ && !dispatchQueue_.empty()) {
            auto fn = std::move(dispatchQueue_.front());
            dispatchQueue_.pop_front();
            lk.unlock();
            try {
                fn();
            } catch (...) {
                NEUTRAL_REPORT_WARN("DeviceSession", "Виняток user-колбека на dispatcher-потоці");
            }
            lk.lock();
        }
        if (dispatchStop_) {
            dispatchQueue_.clear();   // #H4: недоставлені події скинути — після Stop колбеків нема
            return;
        }
    }
}

void DeviceSession::Enqueue(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(dispatchMutex_);
        if (dispatchStop_) return;  // після Stop нових подій не приймаємо
        dispatchQueue_.push_back(std::move(fn));
    }
    dispatchCv_.notify_one();
}

void DeviceSession::EnqueueWireTrace(bool sendAttempt, std::vector<uint8_t> bytes) {
    std::function<void(bool, std::vector<uint8_t>)> h;
    {
        std::lock_guard<std::mutex> lk(m_);
        h = wireTraceHandler_;
    }
    if (!h) return;
    Enqueue([h, sendAttempt, bytes = std::move(bytes)]() mutable {
        h(sendAttempt, std::move(bytes));
    });
}

void DeviceSession::MarkSynchronized() {
    std::lock_guard<std::mutex> lk(m_);
    desynchronized_ = false;
}

bool DeviceSession::IsDesynchronized() const {
    std::lock_guard<std::mutex> lk(m_);
    return desynchronized_;
}

void DeviceSession::SetUnsolicitedHandler(std::function<void(std::vector<uint8_t>)> h) {
    std::lock_guard<std::mutex> lk(m_);
    if (started_) {
        NEUTRAL_REPORT_WARN("DeviceSession", "SetUnsolicitedHandler після Start проігноровано");
        return;
    }
    unsolicitedHandler_ = std::move(h);
}

void DeviceSession::SetConnectionStateHandler(std::function<void(bool)> h) {
    std::lock_guard<std::mutex> lk(m_);
    if (started_) {
        NEUTRAL_REPORT_WARN("DeviceSession", "SetConnectionStateHandler після Start проігноровано");
        return;
    }
    stateHandler_ = std::move(h);
}

void DeviceSession::SetWireTraceHandler(std::function<void(bool, std::vector<uint8_t>)> h) {
    std::lock_guard<std::mutex> lk(m_);
    if (started_) {
        NEUTRAL_REPORT_WARN("DeviceSession", "SetWireTraceHandler після Start проігноровано");
        return;
    }
    wireTraceHandler_ = std::move(h);
}
