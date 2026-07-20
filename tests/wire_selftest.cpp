// wire_selftest — харнес device-facing ядра (framer/classifier/session) без 1С і без UAPKI.
// Лінкує wire_component+helpers+base напряму; свій хенд-ролед раннер (як core_selftest).
// УВАГА: pch.h тут НЕ підключається (правило tests/).
#include "../src/transport/NullTerminatedFramer.h"
#include "../src/transport/IFrameClassifier.h"
#include "../src/transport/Transport.h"
#include "../src/transport/Transport_TCP.h"   // реальний транспорт для TCP-смоук (Task 8)
#include "../src/transport/Transport_COM.h"   // реальний транспорт для COM-фіксів (Task 9)
#include "../src/transport/Transport_WSClient.h"  // реальний транспорт для WS-фіксів (Task 10)
#include "extern/ixwebsocket/ixwebsocket/IXWebSocketServer.h"  // локальний ws-echo (лише тест)
#include "extern/ixwebsocket/ixwebsocket/IXNetSystem.h"
#include "../src/transport/DeviceSession.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <memory>
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

    // #L: SetMaxBufferedBytes — ліміт можна оновити після конструктора (саме так DeviceSession
    // під'єднує SessionConfig::maxBufferedBytes); overflow-recovery діє за оновленим лімітом.
    { NullTerminatedFramer fs; std::vector<std::vector<uint8_t>> o3;
      fs.SetMaxBufferedBytes(4);
      fs.Feed(B("12345"), o3);                 // >4 без термінатора за оновленим лімітом
      CHECK(o3.empty(), "SetMaxBufferedBytes: overflow honored after setter");
      fs.Feed([]{ auto v=B("Z"); v.push_back(0); return v; }(), o3);
      CHECK(o3.size()==1 && o3[0]==B("Z"), "SetMaxBufferedBytes: recovers after setter-driven overflow"); }
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
        { std::lock_guard<std::mutex> lk(openMtx_); ++openCount_; }
        openCv_.notify_all();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (failAllOpens_) return false;                             // постійний фейл (§H3)
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
        bool fail = false, failState = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!open_) return -1;                 // закрито → помилка (all-or-error)
            if (failNextSend_) {                   // скриптований провал відправки (§5.3)
                failNextSend_ = false;
                fail = true;
                failState = failNextSendState_;
                failNextSendState_ = false;
            }
        }
        if (fail) {
            // Модель TCP-Send, що детектує обрив: провал (<0) + опційно СИНХРОННИЙ state(false)
            // (Transport_TCP.cpp:371). Без захоплення в sent_ (all-or-error: не «успіх»).
            if (failState) ForceRemoteClose();     // state(false) рівно раз (sync у Inline)
            return -1;
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
    // Усі наступні Open провалюються (постійний фейл для maxTries, §H3) — без state(true).
    void ScriptFailAllOpens() { std::lock_guard<std::mutex> lk(mtx_); failAllOpens_ = true; }
    // Скільки разів викликано Open() (успіх чи фейл) — для перевірки бюджету спроб реконекту.
    size_t OpenCount() { std::lock_guard<std::mutex> lk(openMtx_); return openCount_; }
    // Бар'єр: дочекатися щонайменше n викликів Open() (без sleep).
    bool WaitForOpens(size_t n, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lk(openMtx_);
        return openCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                [&] { return openCount_ >= n; });
    }
    // Наступний Send провалиться (<0). withSyncState=true → синхронно доставити state(false)
    // ДО повернення Send (модель TCP-Send, що детектує обрив; §5.3 precedence).
    void ScriptFailNextSend(bool withSyncState = false) {
        std::lock_guard<std::mutex> lk(mtx_);
        failNextSend_ = true;
        failNextSendState_ = withSyncState;
    }

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
    bool failAllOpens_ = false;
    bool failNextSend_ = false;
    bool failNextSendState_ = false;
    std::atomic<LoopbackStateMode> mode_{ LoopbackStateMode::Worker };

    // Лічильник викликів Open() (для бюджету спроб реконекту, §H3).
    std::mutex openMtx_;
    std::condition_variable openCv_;
    size_t openCount_ = 0;

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

// --- DeviceSession над LoopbackTransport (§4.4/§5/§6 дизайну) ---------------

// Кадр із термінатором 0x00 (те, що доставляє транспорт у OnBytes → framer зніме 0x00).
static std::vector<uint8_t> NT(const std::string& s) {
    auto v = B(s); v.push_back(0); return v;
}

// Watchdog: тест зі стелею часу. Якщо всередині DeviceSession дедлок/lost-wakeup —
// процес не має зависати назавжди: фіксуємо FAIL і аварійно виходимо (як core_selftest).
static void RunGuarded(const char* name, std::function<void()> fn, int seconds = 10) {
    auto fut = std::async(std::launch::async, std::move(fn));
    if (fut.wait_for(std::chrono::seconds(seconds)) != std::future_status::ready) {
        std::printf("[FAIL] %s (timeout %ds — можливий дедлок)\n", name, seconds);
        std::fflush(stdout);
        std::_Exit(3);
    }
    fut.get();
}

// Транспорт, що СИНХРОННО ехо-ить відправлений кадр назад у data-колбек ще ДО
// повернення Send — модель «відповідь надійшла до входу очікувача в wait».
// Дає детермінований repro відсутності lost-wakeup (predicate у cv_.wait_for).
class EchoOnSendTransport : public ITransport {
public:
    bool Open() override {
        open_ = true;
        if (stateCb_) stateCb_(true);   // Inline-подібна доставка state(true)
        return true;
    }
    bool Close() override { open_ = false; return true; }
    bool IsOpen() const override { return open_; }
    int Send(const std::vector<uint8_t>& data) override {
        if (open_ && dataCb_) dataCb_(data);   // ехо ДО повернення Send (reader-роль)
        return static_cast<int>(data.size());
    }
    void SetDataReceivedCallback(DataReceivedCallback cb) override { dataCb_ = std::move(cb); }
    void SetErrorCallback(ErrorCallback cb) override { errCb_ = std::move(cb); }
    void SetConnectionStateCallback(ConnectionStateCallback cb) override { stateCb_ = std::move(cb); }
private:
    bool open_ = false;
    DataReceivedCallback dataCb_;
    ErrorCallback errCb_;
    ConnectionStateCallback stateCb_;
};

// happy-path: Start → RequestPrimary у окремому потоці → WaitForSend →
// InjectRecv(ехо) → {Response, "REQ"}.
static void TestSessionPrimaryHappy() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>());
    CHECK(session.Start(), "PrimaryHappy: Start connects");

    RequestResult result{};
    std::thread req([&] { result = session.RequestPrimary(B("REQ")); });

    CHECK(tp->WaitForSend(), "PrimaryHappy: request reached transport");
    tp->InjectRecv(NT("REQ"));   // EchoClassifier → PrimaryResponse

    req.join();
    CHECK(result.status == RequestStatus::Response && result.frame == B("REQ"),
          "PrimaryHappy: result {Response, REQ}");
    session.Stop();
}

// response-before-wait: транспорт ехо-ить синхронно в Send — відповідь виставляє
// pending.done ДО того, як очікувач входить у wait. Predicate ловить, lost-wakeup нема.
static void TestResponseBeforeWait() {
    DeviceSession session(std::make_unique<EchoOnSendTransport>(),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>());
    CHECK(session.Start(), "ResponseBeforeWait: Start connects");

    RequestResult result = session.RequestPrimary(B("REQ"));
    CHECK(result.status == RequestStatus::Response && result.frame == B("REQ"),
          "ResponseBeforeWait: result {Response, REQ} (no lost-wakeup)");
    session.Stop();
}

// stop-during-pending: Start → RequestPrimary без відповіді → Stop() → {Stopped}.
static void TestStopDuringPending() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>());
    CHECK(session.Start(), "StopDuringPending: Start connects");

    RequestResult result{};
    std::thread req([&] { result = session.RequestPrimary(B("REQ")); });
    CHECK(tp->WaitForSend(), "StopDuringPending: request pending");

    session.Stop();   // pending → Stopped
    req.join();
    CHECK(result.status == RequestStatus::Stopped, "StopDuringPending: result {Stopped}");
}

// service-доріжка виконується ПАРАЛЕЛЬНО з primary: обидва запити in-flight,
// кожен отримує свою відповідь (ScriptedClassifier: PrimaryResponse, потім ServiceResponse
// — у порядку інжекту кадрів). Перевіряє, що доріжки незалежні.
static void TestServiceParallelToPrimary() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<ScriptedClassifier>(std::deque<Classification>{
                              { FrameClass::PrimaryResponse, RejectReason::Busy },
                              { FrameClass::ServiceResponse, RejectReason::Busy }
                          }));
    CHECK(session.Start(), "ServiceParallel: Start connects");

    RequestResult pr{}, sr{};
    std::thread pth([&] { pr = session.RequestPrimary(B("PRI")); });
    CHECK(tp->WaitForSend(), "ServiceParallel: primary sent");
    std::thread sth([&] { sr = session.RequestService(B("SRV")); });
    CHECK(tp->WaitForSend(), "ServiceParallel: service sent (parallel to primary)");

    tp->InjectRecv(NT("PRI"));   // ScriptedClassifier[0] → PrimaryResponse
    tp->InjectRecv(NT("SRV"));   // ScriptedClassifier[1] → ServiceResponse

    pth.join();
    sth.join();
    CHECK(pr.status == RequestStatus::Response && pr.frame == B("PRI"),
          "ServiceParallel: primary got its own response");
    CHECK(sr.status == RequestStatus::Response && sr.frame == B("SRV"),
          "ServiceParallel: service got its own response");
    session.Stop();
}

// deviceBusy-кадр на primary → {Busy} НЕГАЙНО (без очікування таймауту).
static void TestRejectPrimaryBusy() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<ScriptedClassifier>(std::deque<Classification>{
                              { FrameClass::RejectPrimary, RejectReason::Busy }
                          }));
    CHECK(session.Start(), "RejectPrimaryBusy: Start connects");

    RequestResult r{};
    std::thread th([&] { r = session.RequestPrimary(B("REQ")); });
    CHECK(tp->WaitForSend(), "RejectPrimaryBusy: primary sent");
    tp->InjectRecv(NT("busy"));   // → RejectPrimary/Busy
    th.join();
    CHECK(r.status == RequestStatus::Busy, "RejectPrimaryBusy: result {Busy} immediately");
    session.Stop();
}

// RejectService з reason=Unsupported → service завершується {Unsupported}.
static void TestRejectService() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<ScriptedClassifier>(std::deque<Classification>{
                              { FrameClass::RejectService, RejectReason::Unsupported }
                          }));
    CHECK(session.Start(), "RejectService: Start connects");

    RequestResult r{};
    std::thread th([&] { r = session.RequestService(B("SRV")); });
    CHECK(tp->WaitForSend(), "RejectService: service sent");
    tp->InjectRecv(NT("notimpl"));   // → RejectService/Unsupported
    th.join();
    CHECK(r.status == RequestStatus::Unsupported, "RejectService: result {Unsupported}");
    session.Stop();
}

// RejectBoth (неоднозначний reject при обох pending) → обидві доріжки завершуються
// + сесія в desync (IsDesynchronized()).
static void TestRejectBoth() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<ScriptedClassifier>(std::deque<Classification>{
                              { FrameClass::RejectBoth, RejectReason::Unsupported }
                          }));
    CHECK(session.Start(), "RejectBoth: Start connects");

    RequestResult pr{}, sr{};
    std::thread pth([&] { pr = session.RequestPrimary(B("PRI")); });
    CHECK(tp->WaitForSend(), "RejectBoth: primary sent");
    std::thread sth([&] { sr = session.RequestService(B("SRV")); });
    CHECK(tp->WaitForSend(), "RejectBoth: service sent");

    tp->InjectRecv(NT("ambiguous"));   // → RejectBoth → обидві + desync

    pth.join();
    sth.join();
    CHECK(pr.status == RequestStatus::Unsupported, "RejectBoth: primary completed");
    CHECK(sr.status == RequestStatus::Unsupported, "RejectBoth: service completed");
    CHECK(session.IsDesynchronized(), "RejectBoth: session desynchronized");
    session.Stop();
}

// Unsolicited-кадр → unsolicited-хендлер на dispatcher-потоці (НЕ на потоці інжектора).
static void TestUnsolicitedToHandler() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    auto session = std::make_unique<DeviceSession>(
        std::move(transport),
        std::make_unique<NullTerminatedFramer>(),
        std::make_unique<ScriptedClassifier>(std::deque<Classification>{
            { FrameClass::Unsolicited, RejectReason::Busy }
        }));

    std::mutex umtx;
    std::condition_variable ucv;
    std::vector<std::vector<uint8_t>> unsol;
    std::thread::id handlerThread;
    session->SetUnsolicitedHandler([&](std::vector<uint8_t> f) {
        std::lock_guard<std::mutex> lk(umtx);
        handlerThread = std::this_thread::get_id();
        unsol.push_back(std::move(f));
        ucv.notify_all();
    });
    CHECK(session->Start(), "Unsolicited: Start connects");

    const std::thread::id injectorThread = std::this_thread::get_id();
    tp->InjectRecv(NT("EVENT"));   // → Unsolicited → у dispatch-чергу

    {
        std::unique_lock<std::mutex> lk(umtx);
        CHECK(ucv.wait_for(lk, std::chrono::seconds(2), [&] { return !unsol.empty(); }),
              "Unsolicited: handler invoked");
        CHECK(unsol.size() == 1 && unsol[0] == B("EVENT"),
              "Unsolicited: frame delivered to handler");
        CHECK(handlerThread != injectorThread,
              "Unsolicited: handler runs on dispatcher thread, not injector/reader");
    }
    session->Stop();
}

// Другий primary під час активного першого → {Concurrent} (без відправки в мережу).
static void TestSecondPrimaryConcurrent() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>());
    CHECK(session.Start(), "SecondPrimaryConcurrent: Start connects");

    RequestResult first{};
    std::thread th([&] { first = session.RequestPrimary(B("REQ")); });
    CHECK(tp->WaitForSend(), "SecondPrimaryConcurrent: first primary pending");

    RequestResult second = session.RequestPrimary(B("REQ2"));
    CHECK(second.status == RequestStatus::Concurrent,
          "SecondPrimaryConcurrent: second primary {Concurrent}");
    CHECK(tp->SentCount() == 1, "SecondPrimaryConcurrent: concurrent request not sent to wire");

    session.Stop();   // звільнити перший
    th.join();
    CHECK(first.status == RequestStatus::Stopped,
          "SecondPrimaryConcurrent: first released by Stop");
}

// Лічильник подій стану з'єднання — для детермінованого очікування (ре)конекту
// без sleep (підписується через SetConnectionStateHandler ДО Start).
struct StateWaiter {
    std::mutex m;
    std::condition_variable cv;
    int ups = 0;
    int downs = 0;
    void On(bool up) {
        std::lock_guard<std::mutex> lk(m);
        if (up) ++ups; else ++downs;
        cv.notify_all();
    }
    bool WaitUps(int n, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return ups >= n; });
    }
};

// Таймаут primary: мовчання пристрою → {Timeout} + сесія в desync (§7).
static void TestPrimaryTimeout() {
    SessionConfig cfg;
    cfg.autoReconnect      = true;
    cfg.primaryTimeoutMs   = 120;
    cfg.reconnectDelayMs   = 10;
    cfg.reconnectMaxDelayMs = 40;
    cfg.connectDeadlineMs  = 2000;
    auto transport = std::make_unique<LoopbackTransport>();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>(), cfg);
    CHECK(session.Start(), "PrimaryTimeout: Start connects");

    RequestResult r = session.RequestPrimary(B("REQ"));   // жодної відповіді → таймаут
    CHECK(r.status == RequestStatus::Timeout, "PrimaryTimeout: result {Timeout}");
    CHECK(session.IsDesynchronized(), "PrimaryTimeout: session desynchronized after timeout");
    session.Stop();
}

// desync блокує primary, але НЕ service; MarkSynchronized() знімає (§7).
static void TestDesyncBlocksPrimaryNotService() {
    SessionConfig cfg;
    cfg.autoReconnect = false;   // без churn реконекту — спостерігаємо чистий desync
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<ScriptedClassifier>(std::deque<Classification>{
                              { FrameClass::ServiceResponse, RejectReason::Busy },
                              { FrameClass::PrimaryResponse, RejectReason::Busy }
                          }), cfg);
    CHECK(session.Start(), "DesyncBlocks: Start connects");

    // Ввести desync через таймаут primary (короткий timeout, без відповіді).
    RequestResult t = session.RequestPrimary(B("X"), 100);
    CHECK(t.status == RequestStatus::Timeout, "DesyncBlocks: inducer primary timed out");
    CHECK(session.IsDesynchronized(), "DesyncBlocks: desynchronized");
    tp->WaitForSend();   // спожити send індьюсера X

    // У desync новий primary → {Desynchronized} НЕГАЙНО, без відправки в мережу.
    size_t sentBefore = tp->SentCount();
    RequestResult p = session.RequestPrimary(B("Y"), 100);
    CHECK(p.status == RequestStatus::Desynchronized, "DesyncBlocks: primary rejected {Desynchronized}");
    CHECK(tp->SentCount() == sentBefore, "DesyncBlocks: desynced primary not sent to wire");

    // Service дозволено навіть у desync (для відновлення).
    RequestResult sr{};
    std::thread sth([&] { sr = session.RequestService(B("SRV")); });
    CHECK(tp->WaitForSend(), "DesyncBlocks: service sent despite desync");
    tp->InjectRecv(NT("srvresp"));   // ScriptedClassifier[0] → ServiceResponse
    sth.join();
    CHECK(sr.status == RequestStatus::Response, "DesyncBlocks: service completes in desync");

    // MarkSynchronized() знімає desync — primary знову дозволено.
    session.MarkSynchronized();
    CHECK(!session.IsDesynchronized(), "DesyncBlocks: MarkSynchronized clears desync");
    RequestResult pr{};
    std::thread pth([&] { pr = session.RequestPrimary(B("Z")); });
    CHECK(tp->WaitForSend(), "DesyncBlocks: primary sent after MarkSynchronized");
    tp->InjectRecv(NT("priresp"));   // ScriptedClassifier[1] → PrimaryResponse
    pth.join();
    CHECK(pr.status == RequestStatus::Response, "DesyncBlocks: primary works after resync");
    session.Stop();
}

// Stale-frame після таймауту+реконекту: пізня відповідь старого запиту не завершує
// новий primary (epoch/generation guard, §14).
static void TestStaleFrameAfterReconnect() {
    SessionConfig cfg;
    cfg.autoReconnect      = true;
    cfg.primaryTimeoutMs   = 120;
    cfg.reconnectDelayMs   = 10;
    cfg.reconnectMaxDelayMs = 40;
    cfg.connectDeadlineMs  = 2000;
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<ScriptedClassifier>(std::deque<Classification>{
                              { FrameClass::PrimaryResponse, RejectReason::Busy },  // stale — відкинути
                              { FrameClass::PrimaryResponse, RejectReason::Busy }   // відповідь новому
                          }), cfg);
    StateWaiter sw;
    session.SetConnectionStateHandler([&](bool up) { sw.On(up); });
    CHECK(session.Start(), "StaleFrame: Start connects");
    CHECK(sw.WaitUps(1), "StaleFrame: initial connect state(true)");

    // primary тайм-аутить → desync + reconnectRequested → супервізор реконектить.
    RequestResult t = session.RequestPrimary(B("OLD"));
    CHECK(t.status == RequestStatus::Timeout, "StaleFrame: old primary timed out");
    tp->WaitForSend();   // спожити send старого primary
    CHECK(sw.WaitUps(2), "StaleFrame: supervisor reconnected (2nd state(true))");

    // Пізня відповідь СТАРОГО запиту прилітає вже після реконекту. Активного primary
    // нема (desync блокує новий) — має бути безпечно відкинута, НЕ збережена «на потім».
    tp->InjectRecv(NT("STALE"));   // ScriptedClassifier[0] → PrimaryResponse (dropped)

    // Драйвер відновив синхронізацію → новий primary дозволено.
    session.MarkSynchronized();
    RequestResult nr{};
    std::thread th([&] { nr = session.RequestPrimary(B("NEW")); });
    CHECK(tp->WaitForSend(), "StaleFrame: new primary sent");
    tp->InjectRecv(NT("NEWRESP"));   // ScriptedClassifier[1] → PrimaryResponse → completes NEW
    th.join();
    CHECK(nr.status == RequestStatus::Response && nr.frame == B("NEWRESP"),
          "StaleFrame: new primary completed by its own response, stale not applied");
    session.Stop();
}

// Таймаут service: {Timeout} без desync, з карантином — пізній дубль не завершує
// новий service (§7, §14).
static void TestServiceTimeoutQuarantine() {
    SessionConfig cfg;
    cfg.autoReconnect = false;
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<ScriptedClassifier>(std::deque<Classification>{
                              { FrameClass::ServiceResponse, RejectReason::Busy },  // пізній дубль
                              { FrameClass::ServiceResponse, RejectReason::Busy }   // відповідь новому
                          }), cfg);
    CHECK(session.Start(), "ServiceQuarantine: Start connects");

    RequestResult t = session.RequestService(B("A"), 100);   // мовчання → таймаут
    CHECK(t.status == RequestStatus::Timeout, "ServiceQuarantine: service timed out");
    CHECK(!session.IsDesynchronized(), "ServiceQuarantine: service timeout does NOT desync");
    tp->WaitForSend();   // спожити send першого service

    RequestResult sr{};
    std::thread sth([&] { sr = session.RequestService(B("B")); });
    CHECK(tp->WaitForSend(), "ServiceQuarantine: new service sent");

    // Пізній дубль відповіді старого (timed-out) service — карантин має проковтнути.
    tp->InjectRecv(NT("late-dup"));   // ScriptedClassifier[0] → ServiceResponse (swallowed)
    // Справжня відповідь новому service.
    tp->InjectRecv(NT("real"));       // ScriptedClassifier[1] → ServiceResponse → completes B
    sth.join();
    CHECK(sr.status == RequestStatus::Response && sr.frame == B("real"),
          "ServiceQuarantine: new service completed by its own response, not the late dup");
    session.Stop();
}

// #H2: карантин ОБМЕЖЕНИЙ — service таймаутить, пізнього дубля НЕ приходить; після вікна
// карантину новий service має завершитись СВОЄЮ відповіддю (а не бути проковтнутим назавжди).
static void TestServiceQuarantineExpiresWithoutDuplicate() {
    SessionConfig cfg;
    cfg.autoReconnect   = false;   // без реконекту — карантин НЕ скидається обривом
    cfg.serviceTimeoutMs = 30;     // мале вікно карантину, щоб детерміновано його перетнути
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<ScriptedClassifier>(std::deque<Classification>{
                              { FrameClass::ServiceResponse, RejectReason::Busy }  // відповідь новому
                          }), cfg);
    CHECK(session.Start(), "QuarantineExpires: Start connects");

    RequestResult t = session.RequestService(B("A"));   // default=30ms → мовчання → таймаут
    CHECK(t.status == RequestStatus::Timeout, "QuarantineExpires: service A timed out");
    CHECK(tp->WaitForSend(), "QuarantineExpires: A reached wire");

    // Пізнього дубля НЕ інжектимо. Перечекати вікно карантину (serviceTimeoutMs=30мс) з запасом —
    // це ЄДИНИЙ спосіб детерміновано перевірити «після вікна кадр легітимний» (вікно — за годинником).
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    RequestResult sr{};
    std::thread sth([&] { sr = session.RequestService(B("B"), 2000); });
    CHECK(tp->WaitForSend(), "QuarantineExpires: new service B sent");
    tp->InjectRecv(NT("own"));   // ВЛАСНА відповідь B — вікно минуло, НЕ карантинити
    sth.join();
    CHECK(sr.status == RequestStatus::Response && sr.frame == B("own"),
          "QuarantineExpires: B completed by its own response (quarantine window expired, not swallowed)");
    session.Stop();
}

// Обрив під час in-flight запиту → pending {Disconnected} + автоматичний реконект.
static void TestDisconnectDuringPending() {
    SessionConfig cfg;
    cfg.autoReconnect      = true;
    cfg.reconnectDelayMs   = 10;
    cfg.reconnectMaxDelayMs = 40;
    cfg.connectDeadlineMs  = 2000;
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>(), cfg);
    StateWaiter sw;
    session.SetConnectionStateHandler([&](bool up) { sw.On(up); });
    CHECK(session.Start(), "DisconnectDuringPending: Start connects");
    CHECK(sw.WaitUps(1), "DisconnectDuringPending: initial connect");

    RequestResult r{};
    std::thread th([&] { r = session.RequestPrimary(B("REQ")); });
    CHECK(tp->WaitForSend(), "DisconnectDuringPending: request in-flight");

    tp->ForceRemoteClose();   // обрив під час очікування відповіді
    th.join();
    CHECK(r.status == RequestStatus::Disconnected,
          "DisconnectDuringPending: pending completes {Disconnected}");
    CHECK(sw.WaitUps(2), "DisconnectDuringPending: supervisor reconnected after drop");
    session.Stop();
}

// Провал ПЕРШОГО Open: супервізор має прокинутись за reconnectRequested_
// (не лише !connected_) і перепідключитись.
static void TestInitialOpenFailure() {
    SessionConfig cfg;
    cfg.autoReconnect      = true;
    cfg.reconnectDelayMs   = 10;
    cfg.reconnectMaxDelayMs = 40;
    cfg.connectDeadlineMs  = 2000;
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    tp->ScriptFailNextOpen();   // перший Open провалиться (без state(true))
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>(), cfg);
    StateWaiter sw;
    session.SetConnectionStateHandler([&](bool up) { sw.On(up); });
    session.Start();   // Open fail → супервізор має ретраїти
    CHECK(sw.WaitUps(1), "InitialOpenFailure: supervisor reconnects after failed initial Open");
    CHECK(session.IsConnected(), "InitialOpenFailure: session connected after retry");
    session.Stop();
}

// #H3: reconnectMaxTries РЕАЛЬНО обмежує — при постійному фейлі Open супервізор робить
// РІВНО maxTries спроб реконекту, тоді ідлить (не крутить Close+Open вічно).
static void TestReconnectMaxTries() {
    SessionConfig cfg;
    cfg.autoReconnect      = true;
    cfg.reconnectDelayMs   = 5;
    cfg.reconnectMaxDelayMs = 20;
    cfg.reconnectMaxTries  = 3;      // бюджет спроб реконекту
    cfg.connectDeadlineMs  = 200;
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>(), cfg);
    StateWaiter sw;
    session.SetConnectionStateHandler([&](bool up) { sw.On(up); });
    CHECK(session.Start(), "ReconnectMaxTries: Start connects");
    CHECK(sw.WaitUps(1), "ReconnectMaxTries: initial connect");
    CHECK(tp->OpenCount() == 1, "ReconnectMaxTries: exactly one Open at Start");

    tp->ScriptFailAllOpens();   // кожен наступний Open реконекту провалюється (без state(true))
    tp->ForceRemoteClose();     // обрив → супервізор ретраїть, усі спроби фейлять

    // Рівно maxTries спроб реконекту (кожна = 1 Open) поверх початкового Open.
    CHECK(tp->WaitForOpens(1 + 3), "ReconnectMaxTries: supervisor made maxTries reconnect attempts");

    // Перечекати понад backoff (<=20мс), щоб зловити «вічний цикл», якби латч не спрацював.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(tp->OpenCount() == 1 + 3,
          "ReconnectMaxTries: supervisor idles after maxTries (no infinite Close+Open)");
    CHECK(!session.IsConnected(), "ReconnectMaxTries: remains disconnected after giving up");
    session.Stop();
}

// === §14 інваріанти: потокобезпека, precedence, reentrancy, quiescence, wire-trace ===

// Реентрантний Request з unsolicited-хендлера: хендлер (на dispatcher-потоці) кличе
// RequestService. НЕ має вішати, бо dispatcher ≠ reader — відповідь класифікується на
// reader-потоці (InjectRecv), доки dispatcher чекає на cv_. Watchdog ловить дедлок.
static void TestReentrantRequestFromUnsolicited() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    auto session = std::make_unique<DeviceSession>(
        std::move(transport),
        std::make_unique<NullTerminatedFramer>(),
        std::make_unique<ScriptedClassifier>(std::deque<Classification>{
            { FrameClass::Unsolicited, RejectReason::Busy },      // тригер реентрантного service
            { FrameClass::ServiceResponse, RejectReason::Busy }   // відповідь реентрантному service
        }));
    DeviceSession* sp = session.get();

    std::mutex rmtx;
    std::condition_variable rcv;
    RequestResult reentrant{};
    bool done = false;
    session->SetUnsolicitedHandler([&, sp](std::vector<uint8_t>) {
        // Реентрантний виклик із dispatcher-потоку — не має самозаблокуватись.
        RequestResult r = sp->RequestService(B("REENTRANT"));
        std::lock_guard<std::mutex> lk(rmtx);
        reentrant = r; done = true; rcv.notify_all();
    });
    CHECK(session->Start(), "ReentrantRequest: Start connects");

    tp->InjectRecv(NT("EVENT"));    // → Unsolicited → dispatcher кличе RequestService
    CHECK(tp->WaitForSend(), "ReentrantRequest: reentrant service reached wire");
    tp->InjectRecv(NT("SRVRESP"));  // → ServiceResponse → завершує реентрантний service

    {
        std::unique_lock<std::mutex> lk(rmtx);
        CHECK(rcv.wait_for(lk, std::chrono::seconds(3), [&] { return done; }),
              "ReentrantRequest: reentrant service completed (no self-deadlock)");
        CHECK(reentrant.status == RequestStatus::Response,
              "ReentrantRequest: reentrant service got its response");
    }
    session->Stop();
}

// Stop() з unsolicited-хендлера (dispatcher-потік): має завершитись БЕЗ self-join
// (§6 крок 5 — детект потоку; фінальний join у ~DeviceSession). Watchdog ловить дедлок.
static void TestStopFromUnsolicited() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    auto session = std::make_unique<DeviceSession>(
        std::move(transport),
        std::make_unique<NullTerminatedFramer>(),
        std::make_unique<ScriptedClassifier>(std::deque<Classification>{
            { FrameClass::Unsolicited, RejectReason::Busy }
        }));
    DeviceSession* sp = session.get();

    std::mutex dmtx;
    std::condition_variable dcv;
    bool stopped = false;
    session->SetUnsolicitedHandler([&, sp](std::vector<uint8_t>) {
        sp->Stop();   // Stop із dispatcher-потоку — без self-join
        std::lock_guard<std::mutex> lk(dmtx); stopped = true; dcv.notify_all();
    });
    CHECK(session->Start(), "StopFromUnsolicited: Start connects");

    tp->InjectRecv(NT("EVENT"));   // → Unsolicited → dispatcher кличе Stop()
    {
        std::unique_lock<std::mutex> lk(dmtx);
        CHECK(dcv.wait_for(lk, std::chrono::seconds(3), [&] { return stopped; }),
              "StopFromUnsolicited: Stop() from dispatcher returned (no self-join)");
    }
    // Фінальний join dispatcher-а — у ~DeviceSession з цього (не-dispatcher) потоку.
    session.reset();
    CHECK(true, "StopFromUnsolicited: destroyed cleanly (final join in dtor)");
}

// #H4: чанк із 2 unsolicited-кадрами; перший хендлер кличе Stop() з dispatcher-потоку —
// ДРУГИЙ хендлер того ж drain-циклу НЕ має спрацювати (§14 «нуль колбеків», §6 крок 5).
static void TestStopFromUnsolicitedMidChunk() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    auto session = std::make_unique<DeviceSession>(
        std::move(transport),
        std::make_unique<NullTerminatedFramer>(),
        std::make_unique<ScriptedClassifier>(std::deque<Classification>{
            { FrameClass::Unsolicited, RejectReason::Busy },   // кадр 1 → Stop()
            { FrameClass::Unsolicited, RejectReason::Busy }    // кадр 2 → НЕ доставляти
        }));
    DeviceSession* sp = session.get();

    std::mutex dmtx;
    std::condition_variable dcv;
    std::atomic<int> handlerCalls{ 0 };
    std::atomic<bool> injected{ false };
    bool firstDone = false;
    session->SetUnsolicitedHandler([&, sp](std::vector<uint8_t>) {
        const int n = handlerCalls.fetch_add(1) + 1;
        if (n == 1) {
            // Дочекатися, поки інжектор доставить ВЕСЬ чанк (обидва кадри вже в dispatch-черзі),
            // аби Stop() гарантовано спрацював, коли 2-й хендлер уже стоїть у черзі drain-циклу.
            while (!injected.load()) std::this_thread::yield();
            sp->Stop();   // реентрантний Stop() з першого кадру чанку
            std::lock_guard<std::mutex> lk(dmtx); firstDone = true; dcv.notify_all();
        }
    });
    CHECK(session->Start(), "StopMidChunk: Start connects");

    // Два кадри в ОДНОМУ чанку → framer віддає 2 → OnBytes ставить у чергу 2 unsolicited.
    std::vector<uint8_t> chunk = NT("E1");
    { auto e2 = NT("E2"); chunk.insert(chunk.end(), e2.begin(), e2.end()); }
    tp->InjectRecv(chunk);        // OnBytes СИНХРОННО на цьому потоці ставить обидва в чергу
    injected.store(true);         // тепер перший хендлер може кликати Stop()

    {
        std::unique_lock<std::mutex> lk(dmtx);
        CHECK(dcv.wait_for(lk, std::chrono::seconds(3), [&] { return firstDone; }),
              "StopMidChunk: first handler ran and called Stop()");
    }
    session.reset();   // ~DeviceSession — фінальний join dispatcher
    CHECK(handlerCalls.load() == 1,
          "StopMidChunk: second unsolicited NOT delivered after reentrant Stop()");
}

// Precedence (§5.3): Send провалюється із СИНХРОННИМ state(false). pending має
// завершитись РІВНО одним джерелом. Без реконект-churn (autoReconnect=false) джерело —
// precedence-блок RequestPrimary → SendFailed; disconnect зареєстровано (IsConnected=false),
// але pending НЕ переписано вдруге.
static void TestSendFailedVsDisconnected() {
    SessionConfig cfg;
    cfg.autoReconnect = false;   // без супервізора-конкурента — детермінований precedence
    cfg.primaryTimeoutMs = 2000;
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    tp->SetStateMode(LoopbackStateMode::Inline);   // state(false) синхронно з Send
    tp->ScriptFailNextSend(/*withSyncState=*/true);
    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>(), cfg);
    CHECK(session.Start(), "SendFailedVsDisconnected: Start connects");

    RequestResult r = session.RequestPrimary(B("REQ"));
    // Обидва статуси — валідне завершення РІВНО одним джерелом; тут детерміновано SendFailed.
    CHECK(r.status == RequestStatus::SendFailed || r.status == RequestStatus::Disconnected,
          "SendFailedVsDisconnected: pending completed by exactly one source");
    CHECK(r.status == RequestStatus::SendFailed,
          "SendFailedVsDisconnected: SendFailed wins (precedence, no double-complete)");
    CHECK(!session.IsConnected(),
          "SendFailedVsDisconnected: sync state(false) registered the disconnect");
    session.Stop();
}

// Quiescence (§14): після Stop() — нуль НОВИХ колбеків; Stop не вішає (watchdog);
// запит після Stop → {Stopped}; знищення без колбеків.
static void TestCallbackQuiescenceAfterClose() {
    auto transport = std::make_unique<LoopbackTransport>();
    LoopbackTransport* tp = transport.get();
    auto session = std::make_unique<DeviceSession>(
        std::move(transport),
        std::make_unique<NullTerminatedFramer>(),
        std::make_unique<ScriptedClassifier>(std::deque<Classification>{
            { FrameClass::Unsolicited, RejectReason::Busy }
        }));

    std::atomic<int> unsolCount{0};
    std::atomic<int> stateCount{0};
    session->SetUnsolicitedHandler([&](std::vector<uint8_t>) { unsolCount.fetch_add(1); });
    session->SetConnectionStateHandler([&](bool) { stateCount.fetch_add(1); });
    CHECK(session->Start(), "Quiescence: Start connects");

    tp->InjectRecv(NT("EVENT"));   // подія в чергу — dispatcher має бути активним

    session->Stop();   // teardown; після повернення — dispatcher joined, транспорт закрито+відписано

    const int unsolAfter = unsolCount.load();
    const int stateAfter = stateCount.load();

    // Спроби спровокувати колбеки після Stop — усе no-op (транспорт закрито, dispatcher joined).
    tp->InjectRecv(NT("LATE"));
    tp->ForceRemoteClose();
    RequestResult r = session->RequestPrimary(B("AFTER"), 100);
    CHECK(r.status == RequestStatus::Stopped, "Quiescence: request after Stop → {Stopped}");

    CHECK(unsolCount.load() == unsolAfter, "Quiescence: no unsolicited callback after Stop");
    CHECK(stateCount.load() == stateAfter, "Quiescence: no state callback after Stop");

    session.reset();   // ~DeviceSession — фінальний join, без колбеків
    CHECK(unsolCount.load() == unsolAfter && stateCount.load() == stateAfter,
          "Quiescence: no callback during destruction");
}

// Wire-trace порядок (§8): (A) sendAttempt ставиться в чергу ДО Send, тож присутній навіть
// коли Send провалюється; (B) incoming-trace чанку — ПЕРЕД unsolicited того ж чанку (FIFO).
static void TestWireTraceOrder() {
    // --- (A) sendAttempt присутній навіть при провалі Send ---
    {
        SessionConfig cfg; cfg.autoReconnect = false; cfg.primaryTimeoutMs = 2000;
        auto transport = std::make_unique<LoopbackTransport>();
        LoopbackTransport* tp = transport.get();
        tp->ScriptFailNextSend(/*withSyncState=*/false);   // Send → -1, без зміни стану
        DeviceSession session(std::move(transport),
                              std::make_unique<NullTerminatedFramer>(),
                              std::make_unique<EchoClassifier>(), cfg);
        std::mutex tmtx; std::condition_variable tcv;
        std::vector<std::pair<bool, std::vector<uint8_t>>> trace;
        session.SetWireTraceHandler([&](bool sa, std::vector<uint8_t> b) {
            std::lock_guard<std::mutex> lk(tmtx); trace.push_back({ sa, std::move(b) }); tcv.notify_all();
        });
        CHECK(session.Start(), "WireTrace(A): Start connects");

        RequestResult r = session.RequestPrimary(B("REQ"));
        CHECK(r.status == RequestStatus::SendFailed, "WireTrace(A): Send failed as scripted");
        {
            std::unique_lock<std::mutex> lk(tmtx);
            CHECK(tcv.wait_for(lk, std::chrono::seconds(2), [&] { return !trace.empty(); }),
                  "WireTrace(A): sendAttempt dispatched despite failed Send");
            CHECK(trace[0].first == true && trace[0].second == NT("REQ"),
                  "WireTrace(A): sendAttempt trace precedes/despite Send failure");
        }
        session.Stop();
    }

    // --- (B) incoming-trace ПЕРЕД unsolicited того ж чанку ---
    {
        auto transport = std::make_unique<LoopbackTransport>();
        LoopbackTransport* tp = transport.get();
        auto session = std::make_unique<DeviceSession>(
            std::move(transport),
            std::make_unique<NullTerminatedFramer>(),
            std::make_unique<ScriptedClassifier>(std::deque<Classification>{
                { FrameClass::Unsolicited, RejectReason::Busy }
            }));
        std::mutex omtx; std::condition_variable ocv;
        std::vector<int> order;   // 0 = incoming-trace, 1 = unsolicited
        session->SetWireTraceHandler([&](bool sa, std::vector<uint8_t>) {
            if (sa) return;       // цікавить лише incoming-trace
            std::lock_guard<std::mutex> lk(omtx); order.push_back(0); ocv.notify_all();
        });
        session->SetUnsolicitedHandler([&](std::vector<uint8_t>) {
            std::lock_guard<std::mutex> lk(omtx); order.push_back(1); ocv.notify_all();
        });
        CHECK(session->Start(), "WireTrace(B): Start connects");

        tp->InjectRecv(NT("EVENT"));   // чанк → incoming-trace, потім unsolicited (той же чанк)
        {
            std::unique_lock<std::mutex> lk(omtx);
            auto findFirst = [&](int v) { for (size_t i = 0; i < order.size(); ++i) if (order[i] == v) return (int)i; return -1; };
            CHECK(ocv.wait_for(lk, std::chrono::seconds(2),
                               [&] { return findFirst(0) >= 0 && findFirst(1) >= 0; }),
                  "WireTrace(B): both incoming-trace and unsolicited dispatched");
            int ti = findFirst(0), ui = findFirst(1);
            CHECK(ti >= 0 && ui >= 0 && ti < ui,
                  "WireTrace(B): incoming-trace precedes unsolicited of same chunk");
        }
        session->Stop();
    }
}

// === Task 8: смоук реального TransportTCP через DeviceSession (localhost-echo) ======

// RawTcpEchoServer — in-process ехо-сервер на СИРОМУ Winsock (НЕ серверний режим
// TransportTCP, який видалено). bind(127.0.0.1:0)+getsockname → ефемерний порт;
// accept-потік читає байти й ехо-ить кожен кадр із 0x00-термінатором назад.
// echoEnabled=false → «мовчазний» сервер (приймає, читає, але не відповідає) —
// для TestTcpCloseNoHang. Сокети атомарні: Stop() закриває їх і розблоковує
// accept/recv ДО join (детермінований teardown без sleep).
class RawTcpEchoServer {
public:
    ~RawTcpEchoServer() { Stop(); }

    bool Start(bool echoEnabled) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
        started_ = true;

        SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (l == INVALID_SOCKET) return false;
        listen_.store(l);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;   // ефемерний порт
        if (bind(l, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) return false;

        int len = sizeof(addr);
        if (getsockname(l, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) return false;
        port_ = ntohs(addr.sin_port);

        if (listen(l, 1) == SOCKET_ERROR) return false;

        echo_ = echoEnabled;
        running_.store(true);
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    int Port() const { return port_; }

    void Stop() {
        running_.store(false);
        SOCKET l = listen_.exchange(INVALID_SOCKET);
        if (l != INVALID_SOCKET) { shutdown(l, SD_BOTH); closesocket(l); }
        SOCKET c = client_.exchange(INVALID_SOCKET);
        if (c != INVALID_SOCKET) { shutdown(c, SD_BOTH); closesocket(c); }
        if (thread_.joinable()) thread_.join();
        if (started_) { WSACleanup(); started_ = false; }
    }

private:
    void Run() {
        SOCKET c = accept(listen_.load(), nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        client_.store(c);
        if (!running_.load()) { closesocket(c); client_.store(INVALID_SOCKET); return; }

        std::vector<uint8_t> buf;
        char tmp[4096];
        while (running_.load()) {
            int n = recv(c, tmp, static_cast<int>(sizeof(tmp)), 0);
            if (n <= 0) break;
            if (!echo_) continue;   // мовчазний сервер: читає, але не відповідає
            for (int i = 0; i < n; ++i) {
                if (tmp[i] == 0) {
                    std::vector<uint8_t> frame = buf;
                    frame.push_back(0);   // ехо кадру з термінатором
                    int off = 0, total = static_cast<int>(frame.size());
                    while (off < total) {
                        int s = ::send(c, reinterpret_cast<const char*>(frame.data()) + off,
                                       total - off, 0);
                        if (s <= 0) break;
                        off += s;
                    }
                    buf.clear();
                } else {
                    buf.push_back(static_cast<uint8_t>(tmp[i]));
                }
            }
        }
    }

    std::atomic<SOCKET> listen_{ INVALID_SOCKET };
    std::atomic<SOCKET> client_{ INVALID_SOCKET };
    std::thread thread_;
    std::atomic<bool> running_{ false };
    bool echo_ = true;
    bool started_ = false;
    int port_ = 0;
};

// Ехо-roundtrip: DeviceSession над реальним TransportTCP робить RequestPrimary,
// сирий ехо-сервер вертає кадр → {Response, payload}.
static void TestTcpEchoRoundtrip() {
    RawTcpEchoServer server;
    CHECK(server.Start(/*echoEnabled=*/true), "TcpEcho: echo server started");

    DeviceSession session(std::make_unique<TransportTCP>("127.0.0.1", server.Port()),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>());
    CHECK(session.Start(), "TcpEcho: session connected to echo server");

    RequestResult r = session.RequestPrimary(B("PING"));
    CHECK(r.status == RequestStatus::Response && r.frame == B("PING"),
          "TcpEcho: RequestPrimary round-trips {Response, PING}");
    session.Stop();
    server.Stop();
}

// Close без hang: сервер мовчить → reader висить у recv; Stop()/Close() мусить
// закрити сокет (shutdown+closesocket ДО join) і повернутися. Watchdog ловить дедлок.
static void TestTcpCloseNoHang() {
    RawTcpEchoServer server;
    CHECK(server.Start(/*echoEnabled=*/false), "TcpCloseNoHang: silent server started");

    DeviceSession session(std::make_unique<TransportTCP>("127.0.0.1", server.Port()),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>());
    CHECK(session.Start(), "TcpCloseNoHang: session connected");

    session.Stop();   // reader у recv → Close має розблокувати й не зависнути
    CHECK(true, "TcpCloseNoHang: Stop() returned without hang");
    server.Stop();
}

// Partial-send: injectable write-seam пише лише половину за виклик — Send мусить
// дописати залишок у циклі (all-or-error). Повний payload доходить (roundtrip),
// а лічильник підтверджує, що шов реально розбив відправку.
static void TestTcpPartialSend() {
    RawTcpEchoServer server;
    CHECK(server.Start(/*echoEnabled=*/true), "TcpPartialSend: echo server started");

    auto transport = std::make_unique<TransportTCP>("127.0.0.1", server.Port());
    TransportTCP* tp = transport.get();

    std::atomic<int> writeCalls{ 0 };
    tp->SetSendFunctionForTest([&writeCalls](SOCKET s, const char* buf, int len) -> int {
        writeCalls.fetch_add(1);
        int chunk = (len > 1) ? (len / 2) : len;   // половина → примусовий partial
        return ::send(s, buf, chunk, 0);
    });

    DeviceSession session(std::move(transport),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EchoClassifier>());
    CHECK(session.Start(), "TcpPartialSend: session connected");

    const std::string big(5000, 'Z');   // великий payload → кілька half-ітерацій
    RequestResult r = session.RequestPrimary(B(big));
    CHECK(r.status == RequestStatus::Response && r.frame == B(big),
          "TcpPartialSend: full payload round-trips despite partial writes (all-or-error дописує)");
    CHECK(writeCalls.load() > 1, "TcpPartialSend: seam split the send into multiple writes");
    session.Stop();
    server.Stop();
}

// === Task 9: фікси реального TransportCOM =========================================
// COM round-trip проти реального обладнання неможливий у CI (немає порту/com0com),
// тому логіку Send (all-or-error) і Close-cleanup перевіряємо через тестові шви:
//  • m_writeFn (SetWriteFunctionForTest) моделює partial/failed WriteFile;
//  • AttachHandleForTest дає валідний хендл-приймач (NUL) без реального COM-порту.

// All-or-error Send: partial-write шов дописує залишок у циклі; помилка/0-байт → -1.
static void TestComSendAllOrError() {
    // NUL-приймач: валідний writable-хендл, що відкидає записи (шов handle ігнорує).
    HANDLE nul = CreateFileW(L"\\\\.\\NUL", GENERIC_WRITE, 0, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(nul != INVALID_HANDLE_VALUE, "ComSendAllOrError: NUL sink opened");

    TransportCOM com("COM-TEST");
    com.AttachHandleForTest(nul);   // хендл закриє Close()/деструктор

    // Шов #1 — примусовий partial: пише половину залишку за виклик.
    std::atomic<int> writeCalls{ 0 };
    com.SetWriteFunctionForTest(
        [&writeCalls](HANDLE, const void*, DWORD n, DWORD* written) -> BOOL {
            writeCalls.fetch_add(1);
            DWORD chunk = (n > 1) ? (n / 2) : n;   // половина → примусовий partial
            *written = chunk;
            return TRUE;
        });

    std::vector<uint8_t> big(5000, 'Z');
    int r = com.Send(big);
    CHECK(r == static_cast<int>(big.size()),
          "ComSendAllOrError: all-or-error дописує повний payload попри partial");
    CHECK(writeCalls.load() > 1, "ComSendAllOrError: шов розбив запис на кілька викликів");

    // Шов #2 — WriteFile повертає FALSE → Send == -1 (не «успіх»).
    com.SetWriteFunctionForTest([](HANDLE, const void*, DWORD, DWORD*) -> BOOL {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    });
    CHECK(com.Send(big) == -1, "ComSendAllOrError: помилка WriteFile → -1");

    // Шов #3 — 0 записаних байт при непустому залишку (обрив) → -1.
    com.SetWriteFunctionForTest([](HANDLE, const void*, DWORD, DWORD* written) -> BOOL {
        *written = 0;
        return TRUE;
    });
    CHECK(com.Send(big) == -1, "ComSendAllOrError: запис 0 байт → -1");
    // com деструктор → Close() → CloseHandle(nul)
}

// Close-cleanup: Open з невдалим ConfigurePort (NUL — не comm-девайс) не має
// лишати відкритий хендл. Перевіряємо: Open==false, !IsOpen, хендл скинуто в INVALID.
static void TestComCloseCleansHandle() {
    // portName="NUL" → Open будує \\.\NUL: CreateFileW успішний, але GetCommState
    // на NUL провалиться → ConfigurePort=false → Open викликає Close і повертає false.
    TransportCOM com("NUL");
    bool opened = com.Open();
    CHECK(!opened, "ComCloseCleansHandle: Open провалюється на не-comm девайсі");
    CHECK(!com.IsOpen(), "ComCloseCleansHandle: не open після невдалого ConfigurePort");
    CHECK(com.GetHandleForTest() == INVALID_HANDLE_VALUE,
          "ComCloseCleansHandle: хендл звільнено (без витоку) після невдалого Open");
    // Ідемпотентність Close (§4.1 п.1): повторний Close на вже прибраному — no-op.
    CHECK(com.Close(), "ComCloseCleansHandle: повторний Close ідемпотентний");
}

// Реальний COM round-trip проти com0com — ручний крок (у CI пропущено, НЕ FAIL).
// Ручна перевірка: створити віртуальну пару com0com (напр. COM5<->COM6), запустити
// ехо-заглушку на одному кінці, і DeviceSession над TransportCOM("COM5") має
// пройти RequestPrimary. Автоматизувати в CI без обладнання/драйвера неможливо.
static void TestComRoundtripSkip() {
    std::printf("[SKIP] ComRoundtrip: потрібна пара com0com + ехо-заглушка (ручний смоук)\n");
}

// === Task 10: фікси реального TransportWSClient ===================================

// --- (1) Детерміновані юніти на прапорцях (без мережі, завжди виконуються) --------

// Конструктор ОБОВ'ЯЗКОВО вимикає авто-реконект ix (ним керує супервізор DeviceSession
// через Close+Open; інакше ix-реконект ламав би exactly-once state, §9.1/§4.1).
static void TestWsDisableAutoReconnect() {
    TransportWSClient ws("ws://127.0.0.1:1");   // без Open — лише перевірка конструктора
    CHECK(!ws.IsAutomaticReconnectionEnabledForTest(),
          "WsDisableAutoReconnect: конструктор викликав disableAutomaticReconnection()");
}

// m_started (а НЕ m_isOpen) керує stop(): після remote-close з'єднання «впало»
// (m_isOpen==false), але ix-воркер ще живий — Close ЗОБОВ'ЯЗАНИЙ його зупинити, інакше
// наступний Open (reopen) не перепідключить. Детерміновано через stop-шов + лічильник.
static void TestWsStartedControlsStop() {
    TransportWSClient ws("ws://127.0.0.1:1");
    std::atomic<int> stops{ 0 };
    ws.SetStopHookForTest([&stops] { stops.fetch_add(1); });

    // Модель стану «start() викликано, з'єднання впало remote-close»: started=true, !open.
    ws.ForceStartedForTest(true);
    CHECK(!ws.IsOpen(), "WsStartedControlsStop: не open (модель після remote-close)");

    CHECK(ws.Close(), "WsStartedControlsStop: Close повертає true");
    CHECK(stops.load() == 1,
          "WsStartedControlsStop: Close викликав stop() попри !m_isOpen (m_started керує)");

    // §4.1 п.1: повторний Close ідемпотентний — воркер уже зупинено, stop() не повторюється.
    CHECK(ws.Close(), "WsStartedControlsStop: повторний Close ідемпотентний");
    CHECK(stops.load() == 1, "WsStartedControlsStop: другий Close не викликає stop() повторно");
}

// --- (2) Локальний ws-echo (ix::WebSocketServer) для reopen/exactly-once ----------
// Порт ефемерний: ix-сервер не оновлює _port при port=0, тож пробуємо вільний порт
// сирим Winsock (bind(0)+getsockname), звільняємо і віддаємо ix-серверу. Якщо сервер
// не піднявся (зайнятий/недоступний у CI) — тест SKIP, а не FAIL (ручний смоук лишається).

static int ProbeFreePort() {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 0;
    int port = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
            int len = sizeof(a);
            if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) {
                port = ntohs(a.sin_port);
            }
        }
        closesocket(s);
    }
    WSACleanup();
    return port;
}

// Локальний ws-echo: ехо-ить кожне Message назад тому ж клієнту. nullptr при невдачі.
static std::unique_ptr<ix::WebSocketServer> StartWsEcho(int port) {
    auto server = std::make_unique<ix::WebSocketServer>(port, "127.0.0.1");
    server->setOnClientMessageCallback(
        [](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& webSocket,
           const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Message) {
                webSocket.send(msg->str, msg->binary);   // ехо кадру назад
            }
        });
    if (!server->listenAndStart()) return nullptr;
    return server;
}

// Reopen після Close реально перепідключає (m_started→stop() у Close дозволяє повторний
// start()). Exactly-once state: рівно один up і один down на цикл; повторний Close — no-op.
static void TestWsReopen() {
    ix::initNetSystem();
    const int port = ProbeFreePort();
    if (port == 0) {
        std::printf("[SKIP] WsReopen: не вдалося отримати вільний порт (ручний смоук)\n");
        ix::uninitNetSystem();
        return;
    }
    auto server = StartWsEcho(port);
    if (!server) {
        std::printf("[SKIP] WsReopen: локальний ws-сервер не піднявся на порту %d (ручний смоук)\n", port);
        ix::uninitNetSystem();
        return;
    }

    const std::string url = "ws://127.0.0.1:" + std::to_string(port);
    TransportWSClient ws(url);
    ws.SetTimeout(3);   // короткий дедлайн конекту, щоб SKIP-гілка не вішала watchdog

    std::atomic<int> ups{ 0 }, downs{ 0 };
    ws.SetConnectionStateCallback([&](bool up) { (up ? ups : downs).fetch_add(1); });

    // Цикл 1: Open (успіх лише за state(true)) → рівно один up.
    bool o1 = ws.Open();
    if (!o1) {
        std::printf("[SKIP] WsReopen: конект до локального ws-сервера не встановився (ручний смоук)\n");
        server->stop();
        ix::uninitNetSystem();
        return;
    }
    CHECK(ws.IsOpen(), "WsReopen: перший Open встановив з'єднання (state-based)");
    CHECK(ups.load() == 1, "WsReopen: рівно один state(true) на перший цикл");

    // Close → рівно один down (exactly-once, §4.1 п.5).
    CHECK(ws.Close(), "WsReopen: перший Close повертає true");
    CHECK(downs.load() == 1, "WsReopen: рівно один state(false) на розрив");
    CHECK(ws.Close(), "WsReopen: повторний Close ідемпотентний");
    CHECK(downs.load() == 1, "WsReopen: state(false) не дублюється при повторному Close");

    // Цикл 2: Open ПІСЛЯ Close реально перепідключає (reopen через новий start()).
    bool o2 = ws.Open();
    CHECK(o2 && ws.IsOpen(), "WsReopen: reopen після Close реально перепідключив");
    CHECK(ups.load() == 2, "WsReopen: другий цикл дав другий state(true)");
    ws.Close();
    CHECK(downs.load() == 2, "WsReopen: другий Close дав другий state(false)");

    server->stop();
    ix::uninitNetSystem();
}

// === Task 8b: регресії код-рев'ю TransportTCP (#T5/#T7/#T9/#T-sym) ================

// #T5: колбек помилки Send викликається ПОЗА m_sendMutex. Реентрантний Close із
// error-колбека НЕ повинен self-deadlock (до фіксу колбек ішов з-під m_sendMutex,
// а Close бере той самий мьютекс). Транспорт не відкрито → Send бʼє closed-гілку.
// Watchdog RunGuarded ловить дедлок як FAIL.
static void TestTcpErrorCallbackOutsideLock() {
    TransportTCP transport("127.0.0.1", 1);   // НЕ відкриваємо
    std::atomic<int> errs{ 0 };
    transport.SetErrorCallback([&](const std::string&, int) {
        errs.fetch_add(1);
        transport.Close();   // реентрантний Close із колбека — не має заблокуватись
    });

    int r = transport.Send(B("X"));   // closed-гілка → error-колбек (вже поза локом)
    CHECK(r == -1, "TcpErrorCbLock: Send на закритому з'єднанні → -1");
    CHECK(errs.load() == 1, "TcpErrorCbLock: error-колбек викликано рівно раз");
    CHECK(true, "TcpErrorCbLock: реентрантний Close із колбека не призвів до дедлоку");
}

// Ехо-сервер із стабільним портом, що приймає ПОСЛІДОВНІ з'єднання й уміє
// «скинути» поточного клієнта (емуляція remote-close), продовжуючи слухати той
// самий порт — потрібно для re-Open проти незмінного host:port.
class ReopenTcpEchoServer {
public:
    ~ReopenTcpEchoServer() { Stop(); }

    bool Start() {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
        started_ = true;
        SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (l == INVALID_SOCKET) return false;
        listen_.store(l);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (bind(l, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) return false;
        int len = sizeof(a);
        if (getsockname(l, reinterpret_cast<sockaddr*>(&a), &len) == SOCKET_ERROR) return false;
        port_ = ntohs(a.sin_port);
        if (listen(l, 4) == SOCKET_ERROR) return false;
        running_.store(true);
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    int Port() const { return port_; }

    // Закрити поточне клієнтське з'єднання (remote-close), продовжуючи слухати.
    void DropClient() {
        SOCKET c = client_.exchange(INVALID_SOCKET);
        if (c != INVALID_SOCKET) { shutdown(c, SD_BOTH); closesocket(c); }
    }

    void Stop() {
        running_.store(false);
        SOCKET l = listen_.exchange(INVALID_SOCKET);
        if (l != INVALID_SOCKET) { shutdown(l, SD_BOTH); closesocket(l); }
        SOCKET c = client_.exchange(INVALID_SOCKET);
        if (c != INVALID_SOCKET) { shutdown(c, SD_BOTH); closesocket(c); }
        if (thread_.joinable()) thread_.join();
        if (started_) { WSACleanup(); started_ = false; }
    }

private:
    void Run() {
        while (running_.load()) {
            SOCKET c = accept(listen_.load(), nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            client_.store(c);
            std::vector<uint8_t> buf;
            char tmp[4096];
            while (running_.load()) {
                int n = recv(c, tmp, static_cast<int>(sizeof(tmp)), 0);
                if (n <= 0) break;
                for (int i = 0; i < n; ++i) {
                    if (tmp[i] == 0) {
                        std::vector<uint8_t> frame = buf;
                        frame.push_back(0);
                        int off = 0, total = static_cast<int>(frame.size());
                        while (off < total) {
                            int s = ::send(c, reinterpret_cast<const char*>(frame.data()) + off,
                                           total - off, 0);
                            if (s <= 0) break;
                            off += s;
                        }
                        buf.clear();
                    } else {
                        buf.push_back(static_cast<uint8_t>(tmp[i]));
                    }
                }
            }
            // Клієнт відпав (DropClient/remote): прибираємо й чекаємо наступного.
            SOCKET cur = client_.exchange(INVALID_SOCKET);
            if (cur != INVALID_SOCKET) closesocket(cur);
        }
    }

    std::atomic<SOCKET> listen_{ INVALID_SOCKET };
    std::atomic<SOCKET> client_{ INVALID_SOCKET };
    std::thread thread_;
    std::atomic<bool> running_{ false };
    bool started_ = false;
    int port_ = 0;
};

// #T7: re-Open БЕЗ Close після remote-close має знову підняти reader. До фіксу
// reader при remote-close не скидав m_readThreadRunning → StartReadThread бачив
// running==true й не стартував приймач → приймання після reopen тихо не працювало.
static void TestTcpReopenReaderReset() {
    ReopenTcpEchoServer server;
    CHECK(server.Start(), "TcpReopen: сервер піднявся");

    TransportTCP transport("127.0.0.1", server.Port());
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::vector<uint8_t>> got;
    int downs = 0;
    transport.SetDataReceivedCallback([&](const std::vector<uint8_t>& b) {
        std::lock_guard<std::mutex> lk(m); got.push_back(b); cv.notify_all();
    });
    transport.SetConnectionStateCallback([&](bool up) {
        std::lock_guard<std::mutex> lk(m); if (!up) ++downs; cv.notify_all();
    });

    NullTerminatedFramer fr;
    CHECK(transport.Open(), "TcpReopen: перший Open підключився");

    transport.Send(fr.Wrap(B("A")));
    {
        std::unique_lock<std::mutex> lk(m);
        CHECK(cv.wait_for(lk, std::chrono::seconds(3), [&] { return !got.empty(); }),
              "TcpReopen: перший roundtrip прийнято");
    }

    // remote-close: сервер закриває клієнта → наш reader виходить (state down).
    server.DropClient();
    {
        std::unique_lock<std::mutex> lk(m);
        CHECK(cv.wait_for(lk, std::chrono::seconds(3), [&] { return downs >= 1; }),
              "TcpReopen: reader побачив remote-close (state down)");
    }

    // re-Open БЕЗ Close: до фіксу приймання тут не піднялося б.
    { std::lock_guard<std::mutex> lk(m); got.clear(); }
    CHECK(transport.Open(), "TcpReopen: re-Open без Close успішний");

    transport.Send(fr.Wrap(B("B")));
    {
        std::unique_lock<std::mutex> lk(m);
        CHECK(cv.wait_for(lk, std::chrono::seconds(3), [&] { return !got.empty(); }),
              "TcpReopen: приймання працює після re-Open (reader перезапущено)");
    }

    transport.Close();
    server.Stop();
}

// #T9: чистий локальний Close НЕ породжує фантомний error-колбек. Reader висить у
// recv; Close закриває сокет і скидає m_isOpen (m_readThreadRunning — пізніше). Гейт
// у reader'і (додано !m_isOpen) глушить recv-помилку на щойно закритому сокеті.
static void TestTcpCleanCloseNoPhantomError() {
    RawTcpEchoServer server;
    CHECK(server.Start(/*echoEnabled=*/true), "TcpCleanClose: ехо-сервер піднявся");

    TransportTCP transport("127.0.0.1", server.Port());
    std::mutex m;
    std::condition_variable cv;
    std::atomic<int> errors{ 0 };
    std::vector<std::vector<uint8_t>> got;
    transport.SetErrorCallback([&](const std::string&, int) { errors.fetch_add(1); });
    transport.SetDataReceivedCallback([&](const std::vector<uint8_t>& b) {
        std::lock_guard<std::mutex> lk(m); got.push_back(b); cv.notify_all();
    });

    NullTerminatedFramer fr;
    CHECK(transport.Open(), "TcpCleanClose: Open підключився");

    // Roundtrip, щоб reader гарантовано був у recv на момент Close.
    transport.Send(fr.Wrap(B("A")));
    {
        std::unique_lock<std::mutex> lk(m);
        CHECK(cv.wait_for(lk, std::chrono::seconds(3), [&] { return !got.empty(); }),
              "TcpCleanClose: roundtrip прийнято (reader знову у recv)");
    }

    transport.Close();   // локальний Close на сокеті, де висить reader
    CHECK(errors.load() == 0,
          "TcpCleanClose: жодного фантомного error-колбека на чистому Close");
    server.Stop();
}

// #T-sym: невдалий Open (порт без слухача) НЕ повинен дати state(false) без
// попереднього state(true). До фіксу наступний Close/деструктор емітив фантомний
// state(false) (m_stateDownEmitted стартував false). Тепер гейтить m_upDelivered.
static void TestTcpStateUpGate() {
    int deadPort = ProbeFreePort();   // порт звільнено — ніхто не слухає
    if (deadPort == 0) {
        std::printf("[SKIP] TcpStateUpGate: не вдалося отримати вільний порт\n");
        return;
    }

    std::atomic<int> ups{ 0 }, downs{ 0 };
    {
        TransportTCP transport("127.0.0.1", deadPort);
        transport.SetConnectionStateCallback([&](bool up) { (up ? ups : downs).fetch_add(1); });
        CHECK(!transport.Open(), "TcpStateUpGate: Open на мертвий порт провалився");
        CHECK(ups.load() == 0, "TcpStateUpGate: жодного state(true) на невдалому Open");
        transport.Close();   // явний Close після невдалого Open
        CHECK(downs.load() == 0,
              "TcpStateUpGate: немає фантомного state(false) без попереднього state(true)");
    }   // деструктор → Close() ще раз
    CHECK(downs.load() == 0, "TcpStateUpGate: state(false) не зʼявився і після деструкції");
}

int main(){ std::printf("=== wire_selftest ===\n"); TestNullTerminatedFramer();
    TestClassifierDoubles();
    TestLoopbackTransport();
    RunGuarded("TestSessionPrimaryHappy", TestSessionPrimaryHappy);
    RunGuarded("TestResponseBeforeWait", TestResponseBeforeWait);
    RunGuarded("TestStopDuringPending", TestStopDuringPending);
    RunGuarded("TestServiceParallelToPrimary", TestServiceParallelToPrimary);
    RunGuarded("TestRejectPrimaryBusy", TestRejectPrimaryBusy);
    RunGuarded("TestRejectService", TestRejectService);
    RunGuarded("TestRejectBoth", TestRejectBoth);
    RunGuarded("TestUnsolicitedToHandler", TestUnsolicitedToHandler);
    RunGuarded("TestSecondPrimaryConcurrent", TestSecondPrimaryConcurrent);
    RunGuarded("TestPrimaryTimeout", TestPrimaryTimeout);
    RunGuarded("TestDesyncBlocksPrimaryNotService", TestDesyncBlocksPrimaryNotService);
    RunGuarded("TestStaleFrameAfterReconnect", TestStaleFrameAfterReconnect);
    RunGuarded("TestServiceTimeoutQuarantine", TestServiceTimeoutQuarantine);
    RunGuarded("TestServiceQuarantineExpiresWithoutDuplicate", TestServiceQuarantineExpiresWithoutDuplicate);
    RunGuarded("TestDisconnectDuringPending", TestDisconnectDuringPending);
    RunGuarded("TestInitialOpenFailure", TestInitialOpenFailure);
    RunGuarded("TestReconnectMaxTries", TestReconnectMaxTries);
    RunGuarded("TestReentrantRequestFromUnsolicited", TestReentrantRequestFromUnsolicited);
    RunGuarded("TestStopFromUnsolicited", TestStopFromUnsolicited);
    RunGuarded("TestStopFromUnsolicitedMidChunk", TestStopFromUnsolicitedMidChunk);
    RunGuarded("TestSendFailedVsDisconnected", TestSendFailedVsDisconnected);
    RunGuarded("TestCallbackQuiescenceAfterClose", TestCallbackQuiescenceAfterClose);
    RunGuarded("TestWireTraceOrder", TestWireTraceOrder);
    RunGuarded("TestTcpEchoRoundtrip", TestTcpEchoRoundtrip);
    RunGuarded("TestTcpCloseNoHang", TestTcpCloseNoHang);
    RunGuarded("TestTcpPartialSend", TestTcpPartialSend);
    RunGuarded("TestTcpErrorCallbackOutsideLock", TestTcpErrorCallbackOutsideLock);
    RunGuarded("TestTcpReopenReaderReset", TestTcpReopenReaderReset);
    RunGuarded("TestTcpCleanCloseNoPhantomError", TestTcpCleanCloseNoPhantomError);
    RunGuarded("TestTcpStateUpGate", TestTcpStateUpGate);
    RunGuarded("TestComSendAllOrError", TestComSendAllOrError);
    RunGuarded("TestComCloseCleansHandle", TestComCloseCleansHandle);
    TestComRoundtripSkip();
    RunGuarded("TestWsDisableAutoReconnect", TestWsDisableAutoReconnect);
    RunGuarded("TestWsStartedControlsStop", TestWsStartedControlsStop);
    RunGuarded("TestWsReopen", TestWsReopen, 30);
    std::printf("=== %s (failed:%d) ===\n", g_failed?"FAIL":"OK", g_failed); return g_failed?1:0; }
