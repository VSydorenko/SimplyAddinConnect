#include "../core/pch.h"
#include "JobEngine.h"

JobEngine::~JobEngine() { Join(); }

bool JobEngine::Start(std::function<ResultEnvelope()> op) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (state_ == JobState::Running || state_ == JobState::Interrupting) return false;
    }
    if (worker_.joinable()) worker_.join();   // прибрати попередній завершений потік
    cancel_.store(false);
    { std::lock_guard<std::mutex> lk(m_); state_ = JobState::Running; result_ = ResultEnvelope{}; }

    worker_ = std::thread([this, op = std::move(op)]() {
        ResultEnvelope r;
        JobState s = JobState::Done;
        try { r = op(); }
        catch (const std::exception& e) { r = ResultEnvelope::Fail("EXCEPTION", e.what()); s = JobState::Error; }
        catch (...)                     { r = ResultEnvelope::Fail("EXCEPTION", "Unknown"); s = JobState::Error; }
        std::lock_guard<std::mutex> lk(m_);
        result_ = std::move(r);
        state_ = s;
    });
    return true;
}

JobState JobEngine::State() const { std::lock_guard<std::mutex> lk(m_); return state_; }

bool JobEngine::TryGetResult(ResultEnvelope& out) const {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ != JobState::Done && state_ != JobState::Error) return false;
    out = result_;
    return true;
}

void JobEngine::RequestCancel() { cancel_.store(true); }
bool JobEngine::CancelRequested() const { return cancel_.load(); }

void JobEngine::SetState(JobState s) { std::lock_guard<std::mutex> lk(m_); state_ = s; }

void JobEngine::Join() { if (worker_.joinable()) worker_.join(); }
