// wire_selftest — харнес device-facing ядра (framer/classifier/session) без 1С і без UAPKI.
// Лінкує wire_component+helpers+base напряму; свій хенд-ролед раннер (як core_selftest).
// УВАГА: pch.h тут НЕ підключається (правило tests/).
#include "../src/transport/NullTerminatedFramer.h"
#include "../src/transport/IFrameClassifier.h"
#include "../src/transport/Transport.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int g_failed = 0;
#define CHECK(c,n) do{ if(c){std::printf("[PASS] %s\n",n);} else {std::printf("[FAIL] %s\n",n);++g_failed;} }while(0)

static std::vector<uint8_t> B(const std::string& s){ return {s.begin(), s.end()}; }

static void TestNullTerminatedFramer() {
    NullTerminatedFramer f;
    std::vector<std::vector<uint8_t>> out;

    // 1 кадр
    f.Feed([]{ auto v=B("{\"m\":1}"); v.push_back(0); return v; }(), out);
    CHECK(out.size()==1 && out[0]==B("{\"m\":1}"), "single frame");

    // split посередині кадру між Feed
    out.clear(); f.Reset();
    f.Feed(B("{\"a\""), out);            CHECK(out.empty(), "partial: no frame yet");
    f.Feed([]{ auto v=B(":2}"); v.push_back(0); return v; }(), out);
    CHECK(out.size()==1 && out[0]==B("{\"a\":2}"), "split frame reassembled");

    // два кадри в одному чанку + провідний/подвійний 0x00 ігнорується
    out.clear(); f.Reset();
    std::vector<uint8_t> c; c.push_back(0);            // провідний 0x00 → порожній кадр, ігнор
    for(char ch:std::string("A")) c.push_back(ch); c.push_back(0);
    for(char ch:std::string("B")) c.push_back(ch); c.push_back(0);
    f.Feed(c, out);
    CHECK(out.size()==2 && out[0]==B("A") && out[1]==B("B"), "two frames, leading 0x00 ignored");

    // Wrap
    CHECK(f.Wrap(B("X")) == ([]{ auto v=B("X"); v.push_back(0); return v; }()), "Wrap adds trailing 0x00");
    auto hs = f.Wrap(B("X"), FrameOptions{true});
    CHECK(hs.size()==3 && hs[0]==0 && hs[2]==0, "Wrap leadingDelimiter for handshake");

    // переповнення: буфер без 0x00 понад ліміт → framing-error, буфер очищено, наступний кадр ОК
    { NullTerminatedFramer fo(4); std::vector<std::vector<uint8_t>> o2;
      fo.Feed(B("12345"), o2);                 // >4 без термінатора
      CHECK(o2.empty(), "overflow: no frame emitted");
      fo.Feed([]{ auto v=B("Z"); v.push_back(0); return v; }(), o2);
      CHECK(o2.size()==1 && o2[0]==B("Z"), "overflow: recovers on next valid frame"); }
}
// --- тестові подвійники IFrameClassifier (§4.3 дизайну) ---------------------

// EchoClassifier — PrimaryResponse, якщо вхідний кадр байт-у-байт збігається
// з pending.primary; інакше Unsolicited (ігнорує все, що не матчиться).
class EchoClassifier : public IFrameClassifier {
public:
    Classification Classify(const PendingView& pending,
                             const std::vector<uint8_t>& frame) override {
        if (pending.primary && frame == *pending.primary) {
            return { FrameClass::PrimaryResponse, RejectReason::Busy };
        }
        return { FrameClass::Unsolicited, RejectReason::Busy };
    }
};

// ScriptedClassifier — повертає наперед задану чергу Classification, по одному
// результату на виклик Classify (незалежно від вмісту кадру/pending).
class ScriptedClassifier : public IFrameClassifier {
public:
    explicit ScriptedClassifier(std::deque<Classification> script) : script_(std::move(script)) {}
    Classification Classify(const PendingView&, const std::vector<uint8_t>&) override {
        Classification c = script_.front();
        script_.pop_front();
        return c;
    }
private:
    std::deque<Classification> script_;
};

static void TestClassifierDoubles() {
    EchoClassifier echo;
    auto primary = B("resp");
    PendingView pv{ &primary, nullptr };

    auto r1 = echo.Classify(pv, B("resp"));
    CHECK(r1.cls == FrameClass::PrimaryResponse, "EchoClassifier: matches equal bytes");

    auto r2 = echo.Classify(pv, B("other"));
    CHECK(r2.cls == FrameClass::Unsolicited, "EchoClassifier: ignores non-matching bytes");

    ScriptedClassifier scripted(std::deque<Classification>{
        { FrameClass::ServiceResponse, RejectReason::Busy },
        { FrameClass::RejectBoth, RejectReason::Unsupported }
    });
    auto s1 = scripted.Classify(pv, B("x"));
    CHECK(s1.cls == FrameClass::ServiceResponse, "ScriptedClassifier: first scripted result");
    auto s2 = scripted.Classify(pv, B("y"));
    CHECK(s2.cls == FrameClass::RejectBoth && s2.reason == RejectReason::Unsupported,
          "ScriptedClassifier: second scripted result");
}

// --- LoopbackTransport: детермінований подвійник ITransport (§4.1) ----------
// Тестовий транспорт БЕЗ обладнання. Send захоплюється в sent_ (з сигналом
// sendCv_ для бар'єра WaitForSend, щоб тест чекав ФАКТУ Send без sleep);
// InjectRecv доставляє байти в DataReceived-колбек; стан ConnectionState
// доставляється або з worker-потоку (async, режим Worker — модель реального
// transport reader/ix-worker), або синхронно з Open/Close (режим Inline).
// Контракт §4.1: state(true)/state(false) рівно раз на сесію; після Close
// колбеків нема (гейт open_). Детермінізм — черги/mutex/CV, без std::sleep.
enum class LoopbackStateMode { Worker, Inline };

class LoopbackTransport : public ITransport {
public:
    LoopbackTransport() {
        worker_ = std::thread([this] { WorkerLoop(); });
    }
    ~LoopbackTransport() override {
        Close();
        {
            std::lock_guard<std::mutex> lk(workerMtx_);
            workerStop_ = true;
        }
        workerCv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // ---- ITransport ----
    bool Open() override {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (failNextOpen_) { failNextOpen_ = false; return false; }  // без state(true)
            if (open_) return true;                                       // ідемпотентність
            open_ = true;
            falseEmitted_ = false;
        }
        DeliverState(true);
        return true;
    }

    bool Close() override {
        bool emit = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (open_ && !falseEmitted_) { emit = true; falseEmitted_ = true; }
            open_ = false;
        }
        // Синхронно завершити доставку state(false) ДО повернення Close, щоб після
        // Close жоден колбек не стартував (контракт §4.1, п.4-5).
        if (emit) DeliverState(false);
        DrainWorker();
        return true;
    }

    bool IsOpen() const override {
        std::lock_guard<std::mutex> lk(mtx_);
        return open_;
    }

    int Send(const std::vector<uint8_t>& data) override {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!open_) return -1;                 // закрито → помилка (all-or-error)
        }
        {
            std::lock_guard<std::mutex> lk(sentMtx_);
            sent_.push_back(data);
            ++sendCount_;
        }
        sendCv_.notify_all();
        return static_cast<int>(data.size());
    }

    void SetDataReceivedCallback(DataReceivedCallback cb) override {
        std::lock_guard<std::mutex> lk(mtx_); dataCb_ = std::move(cb);
    }
    void SetErrorCallback(ErrorCallback cb) override {
        std::lock_guard<std::mutex> lk(mtx_); errCb_ = std::move(cb);
    }
    void SetConnectionStateCallback(ConnectionStateCallback cb) override {
        std::lock_guard<std::mutex> lk(mtx_); stateCb_ = std::move(cb);
    }

    // ---- Тестове API ----
    // Режим доставки state; ставиться ДО Open (гонки з worker нема).
    void SetStateMode(LoopbackStateMode m) { mode_.store(m); }
    // Наступний Open провалиться (для тестів реконекту) — без state(true).
    void ScriptFailNextOpen() { std::lock_guard<std::mutex> lk(mtx_); failNextOpen_ = true; }

    // Доставити байти в DataReceived-колбек (модель reader-потоку). Синхронно на
    // потоці викликача; після Close/до Open — no-op (гейт open_).
    void InjectRecv(const std::vector<uint8_t>& bytes) {
        DataReceivedCallback cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!open_) return;
            cb = dataCb_;
        }
        if (cb) cb(bytes);
    }

    // Емуляція обриву з боку пристрою: state(false) рівно раз, з'єднання вниз.
    void ForceRemoteClose() {
        bool emit = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (open_ && !falseEmitted_) { emit = true; falseEmitted_ = true; }
            open_ = false;
        }
        if (emit) DeliverState(false);   // async у Worker (модель remote-close), sync у Inline
    }

    // Бар'єр очікування ФАКТУ Send (без sleep): true, якщо новий Send надійшов.
    bool WaitForSend(int timeoutMs = 2000) {
        std::unique_lock<std::mutex> lk(sentMtx_);
        bool ok = sendCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                   [this] { return sendCount_ > waited_; });
        if (ok) ++waited_;
        return ok;
    }
    std::vector<std::vector<uint8_t>> Sent() {
        std::lock_guard<std::mutex> lk(sentMtx_); return sent_;
    }
    size_t SentCount() {
        std::lock_guard<std::mutex> lk(sentMtx_); return sendCount_;
    }

private:
    void DeliverState(bool up) {
        if (mode_.load() == LoopbackStateMode::Inline) {
            ConnectionStateCallback cb;
            { std::lock_guard<std::mutex> lk(mtx_); cb = stateCb_; }
            if (cb) cb(up);          // синхронно на потоці Open/Close
        } else {
            PostWorker([this, up] {
                ConnectionStateCallback cb;
                { std::lock_guard<std::mutex> lk(mtx_); cb = stateCb_; }
                if (cb) cb(up);      // на worker-потоці (колбек не з-під внутрішнього локу)
            });
        }
    }
    void PostWorker(std::function<void()> fn) {
        { std::lock_guard<std::mutex> lk(workerMtx_); workerQ_.push_back(std::move(fn)); }
        workerCv_.notify_all();
    }
    void WorkerLoop() {
        std::unique_lock<std::mutex> lk(workerMtx_);
        for (;;) {
            workerCv_.wait(lk, [this] { return workerStop_ || !workerQ_.empty(); });
            if (workerStop_ && workerQ_.empty()) return;
            auto fn = std::move(workerQ_.front());
            workerQ_.pop_front();
            workerBusy_ = true;
            lk.unlock();
            fn();
            lk.lock();
            workerBusy_ = false;
            workerDrainedCv_.notify_all();
        }
    }
    // Дочекатися, поки worker спорожнить чергу й не в колбеку (бар'єр quiescence).
    // Викликати НЕ з worker-потоку.
    void DrainWorker() {
        std::unique_lock<std::mutex> lk(workerMtx_);
        workerDrainedCv_.wait(lk, [this] { return workerQ_.empty() && !workerBusy_; });
    }

    mutable std::mutex mtx_;
    DataReceivedCallback dataCb_;
    ErrorCallback errCb_;
    ConnectionStateCallback stateCb_;
    bool open_ = false;
    bool falseEmitted_ = false;
    bool failNextOpen_ = false;
    std::atomic<LoopbackStateMode> mode_{ LoopbackStateMode::Worker };

    // Захоплення Send
    std::mutex sentMtx_;
    std::condition_variable sendCv_;
    std::vector<std::vector<uint8_t>> sent_;
    size_t sendCount_ = 0;
    size_t waited_ = 0;

    // Worker-потік доставки state
    std::thread worker_;
    std::mutex workerMtx_;
    std::condition_variable workerCv_;
    std::condition_variable workerDrainedCv_;
    std::deque<std::function<void()>> workerQ_;
    bool workerStop_ = false;
    bool workerBusy_ = false;
};

static void TestLoopbackTransport() {
    // --- Worker-режим (дефолт): state доставляється async з worker-потоку ---
    LoopbackTransport t;

    std::mutex rmtx;
    std::condition_variable rcv;
    std::vector<bool> states;
    std::vector<std::vector<uint8_t>> got;

    t.SetConnectionStateCallback([&](bool up) {
        std::lock_guard<std::mutex> lk(rmtx); states.push_back(up); rcv.notify_all();
    });
    t.SetDataReceivedCallback([&](const std::vector<uint8_t>& b) {
        std::lock_guard<std::mutex> lk(rmtx); got.push_back(b); rcv.notify_all();
    });

    auto waitStates = [&](size_t n) {
        std::unique_lock<std::mutex> lk(rmtx);
        return rcv.wait_for(lk, std::chrono::seconds(2), [&] { return states.size() >= n; });
    };
    auto waitData = [&](size_t n) {
        std::unique_lock<std::mutex> lk(rmtx);
        return rcv.wait_for(lk, std::chrono::seconds(2), [&] { return got.size() >= n; });
    };

    // Open → state(true) (async з worker-потоку)
    CHECK(t.Open(), "Loopback: Open returns true");
    CHECK(waitStates(1) && states.back() == true,
          "Loopback(Worker): Open delivers state(true)");

    // Send (з окремого потоку) → sent_ містить дані + WaitForSend розблоковується
    std::thread sender([&] { t.Send(B("PING")); });
    CHECK(t.WaitForSend(), "Loopback: WaitForSend unblocks on Send");
    sender.join();
    {
        auto s = t.Sent();
        CHECK(s.size() == 1 && s[0] == B("PING"), "Loopback: Send captured into sent_");
    }

    // InjectRecv → прилітає в data-колбек
    t.InjectRecv(B("PONG"));
    CHECK(waitData(1) && got.back() == B("PONG"),
          "Loopback: InjectRecv reaches data callback");

    // Close → state(false) рівно раз
    CHECK(t.Close(), "Loopback: Close returns true");
    CHECK(waitStates(2) && states.back() == false,
          "Loopback: Close delivers state(false)");

    // Close ідемпотентний — вдруге state НЕ емітиться
    CHECK(t.Close(), "Loopback: second Close idempotent");

    // Після Close колбеків нема: InjectRecv ігнорується; state(false) — рівно раз
    t.InjectRecv(B("LATE"));
    {
        std::lock_guard<std::mutex> lk(rmtx);
        CHECK(got.size() == 1, "Loopback: no data callback after Close");
        CHECK(states.size() == 2, "Loopback: state(false) delivered exactly once");
    }

    // --- Inline-режим: state доставляється СИНХРОННО з Open/Close ---
    LoopbackTransport ti;
    std::vector<bool> istates;
    ti.SetStateMode(LoopbackStateMode::Inline);
    ti.SetConnectionStateCallback([&](bool up) { istates.push_back(up); });
    ti.Open();
    CHECK(istates.size() == 1 && istates[0] == true,
          "Loopback(Inline): Open delivers state(true) synchronously");
    ti.Close();
    CHECK(istates.size() == 2 && istates[1] == false,
          "Loopback(Inline): Close delivers state(false) synchronously");
}

int main(){ std::printf("=== wire_selftest ===\n"); TestNullTerminatedFramer();
    TestClassifierDoubles();
    TestLoopbackTransport();
    std::printf("=== %s (failed:%d) ===\n", g_failed?"FAIL":"OK", g_failed); return g_failed?1:0; }
