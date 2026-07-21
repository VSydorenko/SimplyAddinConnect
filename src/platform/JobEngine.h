#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include "ResultEnvelope.h"

enum class JobState { Idle, Running, Interrupting, Done, Error };

/// Загальна машина одного асинхронного завдання: worker-потік виконує op()->ResultEnvelope,
/// зберігає результат, ставить Done/Error. Прапорець скасування спостерігає драйвер (poller).
class JobEngine {
public:
    JobEngine() = default;
    ~JobEngine();
    JobEngine(const JobEngine&) = delete;
    JobEngine& operator=(const JobEngine&) = delete;

    bool Start(std::function<ResultEnvelope()> op);   ///< false, якщо вже Running
    JobState State() const;
    bool TryGetResult(ResultEnvelope& out) const;     ///< true при Done/Error
    void RequestCancel();                              ///< гардовано: cancel_=true; Running→Interrupting
    bool CancelRequested() const;
    void ResetToIdle();                                ///< скинути завершений (Done/Error) стан у Idle
    void Join();                                       ///< дочекатися worker (Disconnect/dtor)

private:
    mutable std::mutex m_;
    std::thread worker_;
    JobState state_ = JobState::Idle;
    ResultEnvelope result_{};
    std::atomic<bool> cancel_{false};
};
