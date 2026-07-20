#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "RequestTypes.h"
#include "IFramer.h"
#include "IFrameClassifier.h"
#include "Transport.h"

/**
 * @file DeviceSession.h
 * @brief Сесія запит/відповідь над байтовим транспортом (§4.4 дизайну).
 *
 * Єдиний власник байтового потоку: reader → framer → classifier →
 * {primary|service|reject|unsolicited}; дві доріжки RequestPrimary/RequestService;
 * user-колбеки лише на dispatcher-потоці; супервізор реконекту; desync-логіка.
 *
 * Потокове правило (§6, наскрізне): усі user-колбеки — ЛИШЕ на dispatcher-потоці,
 * ніколи під m_, ніколи на reader/caller/supervisor-потоці. m_ тримати коротко,
 * ніколи під Send/Close/user-колбеком.
 */

/// Конфіг сесії з живими дефолтами (§4.4). timeoutMs<0 у запиті → відповідний дефолт.
struct SessionConfig {
    int  primaryTimeoutMs   = 30000;   ///< дефолт таймауту primary, коли Request timeout не заданий
    int  serviceTimeoutMs   = 5000;    ///< дефолт таймауту service
    bool autoReconnect      = true;
    int  reconnectDelayMs   = 1000;
    int  reconnectMaxDelayMs = 15000;
    int  reconnectMaxTries  = 0;       ///< 0 = без обмеження
    int  connectDeadlineMs  = 10000;   ///< очікування state(true) як успіху конекту
    std::size_t maxBufferedBytes = 1u << 20;
};

class DeviceSession {
public:
    DeviceSession(std::unique_ptr<ITransport> transport,
                  std::unique_ptr<IFramer> framer,
                  std::unique_ptr<IFrameClassifier> classifier,
                  SessionConfig cfg = {});
    ~DeviceSession();

    DeviceSession(const DeviceSession&) = delete;
    DeviceSession& operator=(const DeviceSession&) = delete;

    /// Підписати транспортні колбеки, підняти dispatcher, відкрити транспорт.
    /// true — з'єднання встановлено (state(true) у межах connectDeadlineMs).
    bool Start();

    /// Безпечна зупинка з БУДЬ-ЯКОГО потоку, у т.ч. dispatcher (§6). Ідемпотентна.
    void Stop();

    bool IsConnected() const;

    /// timeoutMs<0 → cfg.primaryTimeoutMs / cfg.serviceTimeoutMs.
    RequestResult RequestPrimary(const std::vector<uint8_t>& payload,
                                 int timeoutMs = -1, FrameOptions opts = {});
    RequestResult RequestService(const std::vector<uint8_t>& payload,
                                 int timeoutMs = -1, FrameOptions opts = {});

    /// Драйвер знімає desync ПІСЛЯ протокольного відновлення транзакції.
    void MarkSynchronized();
    bool IsDesynchronized() const;

    // Setters — ЛИШЕ до Start() (уникнення гонки з dispatcher); після Start — no-op+WARN.
    void SetUnsolicitedHandler(std::function<void(std::vector<uint8_t>)>);
    void SetConnectionStateHandler(std::function<void(bool)>);
    void SetWireTraceHandler(std::function<void(bool /*sendAttempt*/, std::vector<uint8_t>)>);

private:
    // Внутрішні колбеки транспорту (не user-код).
    void OnBytes(const std::vector<uint8_t>&);
    void OnTransportState(bool);
    void OnTransportError(const std::string&, int);
    // Супервізор реконекту (повна логіка — Task 6).
    void ReconnectLoop();
    // Dispatcher-потік user-подій.
    void DispatchLoop();

    // Спільна реалізація обох доріжок запиту (§5).
    struct Pending;
    RequestResult DoRequest(Pending& pending, bool isPrimary,
                            const std::vector<uint8_t>& payload,
                            int timeoutMs, FrameOptions opts);

    // Поставити user-подію в чергу dispatcher-потоку.
    void Enqueue(std::function<void()> fn);
    // Wire-trace (sendAttempt/incoming) у чергу dispatcher-а (FIFO).
    void EnqueueWireTrace(bool sendAttempt, std::vector<uint8_t> bytes);

    // Стан однієї доріжки запиту.
    struct Pending {
        bool active = false;                 ///< запит зарезервував доріжку
        bool done = false;                   ///< результат уже виставлено (одним джерелом)
        std::vector<uint8_t> payload;        ///< payload для матчингу класифікатором
        std::uint64_t epoch = 0;             ///< генерація на момент резервації (stale-guard)
        RequestResult result{};              ///< зібраний результат
    };

    std::unique_ptr<ITransport> transport_;
    std::unique_ptr<IFramer> framer_;
    std::unique_ptr<IFrameClassifier> classifier_;
    SessionConfig cfg_;

    mutable std::mutex m_;                    ///< pending/стан; коротко, ніколи під Send/Close/колбек
    std::condition_variable cv_;              ///< сигнал завершення запиту / зміни стану
    std::mutex framerMutex_;                  ///< серіалізація Feed/Wrap/Reset між потоками

    Pending pendingPrimary_;
    Pending pendingService_;

    bool connected_ = false;
    bool stopping_ = false;
    bool desynchronized_ = false;
    bool reconnectRequested_ = false;
    std::uint64_t epoch_ = 0;

    // Dispatcher-потік і його черга.
    std::thread dispatcher_;
    std::mutex dispatchMutex_;
    std::condition_variable dispatchCv_;
    std::deque<std::function<void()>> dispatchQueue_;
    bool dispatchStop_ = false;

    // Супервізор реконекту (Task 6).
    std::thread supervisor_;

    // User-колбеки (встановлюються до Start).
    std::function<void(std::vector<uint8_t>)> unsolicitedHandler_;
    std::function<void(bool)> stateHandler_;
    std::function<void(bool, std::vector<uint8_t>)> wireTraceHandler_;

    bool started_ = false;
};
