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
    void RequestCancel();
    bool CancelRequested() const;
    void SetState(JobState s);                         ///< драйвер: перехід (напр. Interrupting)
    void Join();                                       ///< дочекатися worker (Disconnect/dtor)

private:
    mutable std::mutex m_;
    std::thread worker_;
    JobState state_ = JobState::Idle;
    ResultEnvelope result_{};
    std::atomic<bool> cancel_{false};
};
