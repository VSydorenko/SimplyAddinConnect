#include "../core/pch.h"
#include "DeviceSession.h"
#include "../helpers/ServiceTools.h"

#include <chrono>

/**
 * @file DeviceSession.cpp
 * @brief Реалізація device-сесії (§4.4, §5, §6, §7 дизайну).
 *
 * У ЦІЙ фазі (Task 4) реалізовано: каркас; Start (Open + підписка колбеків +
 * dispatcher-потік); Stop (базовий teardown §6 кроки 1-6); RequestPrimary/
 * RequestService (алгоритм §5 — send→wait→precedence) через спільний DoRequest;
 * OnBytes → PrimaryResponse. Reject-гілки, Unsolicited, service-класифікація, реконект,
 * таймаут-desync, epoch-guard проти stale, повний reentrancy-teardown —
 * дороблюються в Task 5-7.
 */

DeviceSession::DeviceSession(std::unique_ptr<ITransport> transport,
                             std::unique_ptr<IFramer> framer,
                             std::unique_ptr<IFrameClassifier> classifier,
                             SessionConfig cfg)
    : transport_(std::move(transport)),
      framer_(std::move(framer)),
      classifier_(std::move(classifier)),
      cfg_(cfg) {
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

    if (!transport_->Open()) {
        // Реконект-супервізор доробляється в Task 6; поки просто повідомляємо невдачу.
        NEUTRAL_REPORT_WARN("DeviceSession", "Транспорт не відкрився при Start");
        return false;
    }

    // Дочекатися state(true) як критерію успіху конекту (§4.1, connectDeadlineMs).
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, std::chrono::milliseconds(cfg_.connectDeadlineMs),
                 [this] { return connected_ || stopping_; });
    return connected_;
}

void DeviceSession::Stop() {
    // §6 крок 1: під m_ — stopping_, усі pending → Stopped, розбудити очікувачів.
    {
        std::lock_guard<std::mutex> lk(m_);
        if (stopping_) return;  // ідемпотентність
        stopping_ = true;
        if (pendingPrimary_.active && !pendingPrimary_.done) {
            pendingPrimary_.result = { RequestStatus::Stopped, {} };
            pendingPrimary_.done = true;
        }
        if (pendingService_.active && !pendingService_.done) {
            pendingService_.result = { RequestStatus::Stopped, {} };
            pendingService_.done = true;
        }
        cv_.notify_all();
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
        // Таймаут; desync-логіка primary — Task 6.
        result = { RequestStatus::Timeout, {} };
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

        switch (cls.cls) {
        case FrameClass::PrimaryResponse:
            if (pendingPrimary_.active && !pendingPrimary_.done) {
                pendingPrimary_.result = { RequestStatus::Response, frame };
                pendingPrimary_.done = true;
                cv_.notify_all();
            }
            break;
        case FrameClass::ServiceResponse:
        case FrameClass::RejectPrimary:
        case FrameClass::RejectService:
        case FrameClass::RejectBoth:
        case FrameClass::Unsolicited:
        default:
            // Доробляється в Task 5 (service-доріжка, Reject*, unsolicited).
            break;
        }
    }
}

void DeviceSession::OnTransportState(bool up) {
    {
        std::lock_guard<std::mutex> lk(m_);
        connected_ = up;
        if (!up) {
            ++epoch_;                    // нова генерація (guard проти stale — повне в Task 6)
            reconnectRequested_ = true;  // будити супервізор (Task 6)
        }
        cv_.notify_all();
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

void DeviceSession::ReconnectLoop() {
    // Повна логіка супервізора реконекту — Task 6.
}

void DeviceSession::DispatchLoop() {
    std::unique_lock<std::mutex> lk(dispatchMutex_);
    for (;;) {
        dispatchCv_.wait(lk, [this] { return dispatchStop_ || !dispatchQueue_.empty(); });
        while (!dispatchQueue_.empty()) {
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
        if (dispatchStop_) return;
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
