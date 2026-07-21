# ECRPrivatJSON — Частина 2: операції, JobEngine, 1С-фасад, механізм тестування (план впровадження)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Довести пілотний драйвер ECRPrivatJSON до повнофункціональної DLL: асинхронні операції (Оплата/Возврат/…) з полінгом статусу, перериванням і відновленням після обриву, 1С-фасад `AddinECRPrivatJSON` (реєстрація методів, події), і механізм тестування без обладнання — standalone-емулятор термінала (для тесту з реальної 1С) + L3-харнес через DLL.

**Architecture:** `JobEngine` (`src/platform/`) — загальна машина станів одного асинхронного завдання (worker-потік). `EcrPrivatJsonDriver` (доповнення) — виконання операцій поверх `DeviceSession`: worker блокується в `RequestPrimary`, окремий poller-потік полить `getLastStatMsgCode` на service-доріжці, шле `interrupt` за прапорцем скасування, відновлює desync через service+`GetReceiptInfo`. Фасад `AddinECRPrivatJSON` (`src/components/`) реєструє методи прямо (як `AddinUAPKIConnect`), делегує драйверу, шле події через `PostExternalEvent`. Тест: `ecr_native_host` (L3, вантажить DLL) + `ecr_terminal_emulator` (standalone EXE для 1С).

**Tech Stack:** C++17, MSVC (VS2022), Winsock2 (`ws2_32`), `nlohmann_json`, SDK 1С (`IComponentBase`/`IAddInDefBase`/`IMemoryManager`), CMake ≥3.16.

**Спека:** `docs/tasks/2026-07-21_design_ecr_privatjson_driver.md` (v2). Частина 1 (wire-спина: `EcrJsonCodec`/`EcrPrivatJsonClassifier`/`EcrPrivatJsonDriver.Connect`/`TerminalEmulator`) — **виконана** (`139bc39`/`b584d59`/`ea9a8ff`). Ця Частина 2 покриває §13 кроки 5-7.

## Рішення для Частини 2 (уточнення спеки з користувачем)

1. **Інтерактивне `correctTransaction` (пауза на рішення каси) НЕ реалізуємо.** На робочому місці касира немає процесу коригування суми в польоті. За спекою при `getLastStatMsgCode==11` без корекції «продаж продовжується в рамках початкових даних» — тож ми просто **не шлемо** `correctTransaction`; poller фіксує код 11 як статус, операція завершується штатно фінальною primary-відповіддю. Стани `AwaitingCashDecision`/`AwaitingPartialDecision` у JobEngine **не потрібні**.
2. **Partial approval (`responseCode 0010`) НЕ інтерактивний** — просто повертаємо результат у 1С (operator/1С вирішує далі поза драйвером).
3. **`interrupt` реалізуємо** (реальна дія «Скасувати» касира) — дешево, бо poller уже володіє service-доріжкою: 1С ставить прапорець, poller шле `interrupt`.
4. **Генерований `OperationRegistry` НЕ робимо** (YAGNI) — методи реєструємо прямо у фасаді (як `AddinUAPKIConnect`). Загальний реєстр — коли зʼявиться 2-й драйвер.
5. **Poller — ЄДИНИЙ власник service-доріжки** (уникає `RequestStatus::Concurrent`): і полінг `getLastStatMsgCode`, і `interrupt` йдуть тільки з poller-потоку; worker тримає лише primary-доріжку.

## Global Constraints

Verbatim зі спеки/AGENTS.md (кожна задача неявно включає):
- Windows-only, C++17, MSVC, `/utf-8`. **PCH** першим рядком у `src/*.cpp` (`src/platform/*`→`"../core/pch.h"`, `src/drivers/ecr_privatjson/*`→`"../../core/pch.h"`, `src/components/*`→`"../core/pch.h"`); у `.h` і в `tests/*` — ніколи.
- **Логування** макросами `ServiceTools.h`: у компоненті (`AddinECRPrivatJSON`) — `REPORT_*`; у не-компонентних (драйвер/JobEngine) — `NEUTRAL_REPORT_*` з 1-м аргументом `"ECRPrivatJSON"`. Прямий `spdlog` заборонено. Повідомлення — конкатенацією, з великої літери, без крапки, без printf.
- **Помилки:** `REPORT_ERROR`/`NEUTRAL_REPORT_ERROR` → `return false`; винятки не перетинають межу 1С (`try/catch` з репортом у `catch`).
- **Повернення в 1С:** value-хендлери — через `Ret(...)` або `this->result = ...`; функція операції повертає `ok` (bool), деталі — `ПолучитьРезультатJSON()`.
- **Параметри 1С:** декларативно через `ParamSpec` (валідація обов'язкових у ядрі).
- **Конвертації рядків:** `ServiceTools::SafeMB2WCHAR`/`SafeWCHAR2MB` (не прямі `AddInNative::*`); у тестових host-ах — `MultiByteToWideChar`/`WideCharToMultiByte`.
- **CMake:** OBJECT-ліби + `$<TARGET_OBJECTS>`; `/utf-8` для нових ліб у `CMake/compiler_settings.cmake`; тест-цілі ECR — ДО гейта `if(NOT BUILD_WITH_UAPKI) return()`.
- **Мова коду** — українська/російська (за файлом).

**Цикл збірки/тесту:**
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests   # повний конфіг+DLL+тести
cmake --build build_x64 --config Release --target ecr_privatjson_selftest   # швидкий unit-цикл
./bin/Release/ecr_privatjson_selftest_x64.exe
cmake --build build_x64 --config Release --target ecr_native_host           # L3 (потребує DLL)
./bin/Release/ecr_native_host_x64.exe
```

---

### Task 1: `JobEngine` — загальна машина асинхронного завдання

**Files:**
- Create: `src/platform/JobEngine.h`, `src/platform/JobEngine.cpp`
- Modify: `CMake/components.cmake` (файли до `platform_component`)
- Test: `tests/ecr_privatjson_selftest.cpp`

**Interfaces:**
- Produces:
  - `enum class JobState { Idle, Running, Interrupting, Done, Error };`
  - `class JobEngine { public: ~JobEngine(); bool Start(std::function<ResultEnvelope()> op); JobState State() const; bool TryGetResult(ResultEnvelope& out) const; void RequestCancel(); bool CancelRequested() const; void SetState(JobState s); void Join(); };`
  - Семантика: `Start` піднімає worker-потік, що виконує `op()`; при поверненні зберігає результат і ставить `Done` (або `Error`, якщо `op` кинув). `Start` під час `Running` → `false`. `RequestCancel` виставляє прапорець (спостерігає драйвер). Потокобезпечно.

- [ ] **Step 1: Написати падаючий тест (`ecr_privatjson_selftest.cpp`)**

Додати `#include "../src/platform/JobEngine.h"`, `#include <atomic>`, `#include <chrono>`, `#include <thread>` і:
```cpp
static void TestJobEngine() {
    JobEngine eng;
    CHECK(eng.State() == JobState::Idle, "JobEngine: стартовий стан Idle");

    std::atomic<bool> release{false};
    bool started = eng.Start([&]() -> ResultEnvelope {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ResultEnvelope::Ok({{"done", true}});
    });
    CHECK(started && eng.State() == JobState::Running, "JobEngine: Start → Running");
    CHECK(eng.Start([]{ return ResultEnvelope::Ok(); }) == false, "JobEngine: повторний Start під час Running → false");

    eng.RequestCancel();
    CHECK(eng.CancelRequested(), "JobEngine: RequestCancel виставляє прапорець");

    release.store(true);
    eng.Join();
    CHECK(eng.State() == JobState::Done, "JobEngine: після завершення op → Done");
    ResultEnvelope out;
    CHECK(eng.TryGetResult(out) && out.ok && out.payload["done"] == true, "JobEngine: результат op збережено");

    // op кидає → Error
    JobEngine eng2;
    eng2.Start([]() -> ResultEnvelope { throw std::runtime_error("boom"); });
    eng2.Join();
    CHECK(eng2.State() == JobState::Error, "JobEngine: виняток у op → Error");
}
```
Додати `TestJobEngine();` у `main()`.

- [ ] **Step 2: Запустити — має впасти**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest`
Expected: FAIL — `Cannot open include file: '.../JobEngine.h'`.

- [ ] **Step 3: Створити `src/platform/JobEngine.h`**

```cpp
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
```

- [ ] **Step 4: Створити `src/platform/JobEngine.cpp`**

```cpp
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
```

- [ ] **Step 5: Додати файли до `platform_component` + `/utf-8`**

`CMake/components.cmake` — у `add_library(platform_component OBJECT ...)` додати `src/platform/JobEngine.h` і `src/platform/JobEngine.cpp`.
`CMake/compiler_settings.cmake` — переконатися, що `/utf-8` для `platform_component` уже є (Частина 1) — без змін, якщо є.

- [ ] **Step 6: Зібрати й запустити — має пройти**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest && ./bin/Release/ecr_privatjson_selftest_x64.exe`
Expected: усі `[PASS]` для `TestJobEngine`, exit 0.

- [ ] **Step 7: Commit**

```bash
git add src/platform/JobEngine.h src/platform/JobEngine.cpp tests/ecr_privatjson_selftest.cpp CMake/components.cmake
git commit -m "feat(ecr): JobEngine — машина асинхронного завдання"
```

---

### Task 2: Драйвер — виконання операції + мапінг результату (Purchase/Refund sync)

**Files:**
- Modify: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h`, `.cpp`
- Test: `tests/ecr_privatjson_selftest.cpp`

**Interfaces:**
- Consumes: `DeviceSession::RequestPrimary`, `EcrJsonCodec`, `ResultEnvelope`.
- Produces (public на `EcrPrivatJsonDriver`):
  - `ResultEnvelope Execute(const std::string& method, const nlohmann::json& params, int timeoutMs);` — синхронно: `RequestPrimary` (блокує) → мапінг у `ResultEnvelope`.
  - `ResultEnvelope Purchase(const std::string& amount, const nlohmann::json& extra = {});`
  - `ResultEnvelope Refund(const std::string& amount, const std::string& rrn, const nlohmann::json& extra = {});`
  - `ResultEnvelope CheckConnection();`
  - `ResultEnvelope GetReceiptInfo(const std::string& invoiceNumber);`
  - приватний `static ResultEnvelope MapResult(const RequestResult& r);`

- [ ] **Step 1: Написати падаючі тести**

```cpp
static void TestDriverPurchaseHappy() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","step":0,"params":{"responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    emu.OnRequest("Purchase", [](const nlohmann::json&){
        return R"({"method":"Purchase","step":0,"params":{"responseCode":"0000","invoiceNumber":"42","rrn":"123"},"error":false,"errorDescription":""})";
    });
    emu.OnRequest("Refund", [](const nlohmann::json&){
        return R"({"method":"Refund","step":0,"params":{"responseCode":"1002"},"error":true,"errorDescription":"EMV Decline"})";
    });
    CHECK(emu.Start(), "DriverPurchase: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "DriverPurchase: Connect");

    auto pr = drv.Purchase("100.00");
    CHECK(pr.ok && pr.code == "0000" && pr.payload["invoiceNumber"] == "42",
          "Purchase happy → ok, code 0000, invoiceNumber");

    auto rf = drv.Refund("50.00", "123");
    CHECK(!rf.ok && rf.code == "1002" && rf.description == "EMV Decline",
          "Refund declined → !ok, code 1002, description");

    drv.Disconnect();
    emu.Stop();
}
```
Додати `TestDriverPurchaseHappy();` у `main()`.

- [ ] **Step 2: Запустити — має впасти (немає `Purchase`)**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest`
Expected: FAIL компіляції — `Purchase`/`Refund` не члени.

- [ ] **Step 3: Додати оголошення у `EcrPrivatJsonDriver.h`**

У `public:` (після `Model()`):
```cpp
    // Синхронні операції (start+wait). Повертають ResultEnvelope (ok/code/description/payload).
    ResultEnvelope Execute(const std::string& method, const nlohmann::json& params, int timeoutMs);
    ResultEnvelope Purchase(const std::string& amount, const nlohmann::json& extra = {});
    ResultEnvelope Refund(const std::string& amount, const std::string& rrn, const nlohmann::json& extra = {});
    ResultEnvelope CheckConnection();
    ResultEnvelope GetReceiptInfo(const std::string& invoiceNumber);
```
У `private:`:
```cpp
    static ResultEnvelope MapResult(const RequestResult& r);
    static constexpr int kOperationTimeoutMs = 120000;   // до 120с на фінансову операцію
```
Додати інклуд у `.h`: `#include <nlohmann/json.hpp>` і `#include "../../platform/ResultEnvelope.h"`; forward-типи лишити.

- [ ] **Step 4: Реалізувати у `EcrPrivatJsonDriver.cpp`**

Додати інклуди `#include "../../transport/RequestTypes.h"` (для `RequestStatus`) і реалізацію:
```cpp
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

ResultEnvelope EcrPrivatJsonDriver::Execute(const std::string& method,
                                            const nlohmann::json& params, int timeoutMs) {
    if (!IsConnected()) return ResultEnvelope::Fail("NOT_CONNECTED", "Термінал не підключено");
    auto req = EcrJsonCodec::BuildRequest(method, 0, params.is_null() ? nlohmann::json(nullptr) : params);
    GateSend();
    RequestResult r = session_->RequestPrimary(req, timeoutMs);
    return MapResult(r);
}

ResultEnvelope EcrPrivatJsonDriver::Purchase(const std::string& amount, const nlohmann::json& extra) {
    nlohmann::json p = extra.is_object() ? extra : nlohmann::json::object();
    p["amount"] = amount;
    return Execute("Purchase", p, kOperationTimeoutMs);
}
ResultEnvelope EcrPrivatJsonDriver::Refund(const std::string& amount, const std::string& rrn, const nlohmann::json& extra) {
    nlohmann::json p = extra.is_object() ? extra : nlohmann::json::object();
    p["amount"] = amount; p["rrn"] = rrn;
    return Execute("Refund", p, kOperationTimeoutMs);
}
ResultEnvelope EcrPrivatJsonDriver::CheckConnection() { return Execute("CheckConnection", nlohmann::json::object(), kHandshakeTimeoutMs); }
ResultEnvelope EcrPrivatJsonDriver::GetReceiptInfo(const std::string& invoiceNumber) {
    return Execute("GetReceiptInfo", nlohmann::json{{"invoiceNumber", invoiceNumber}}, kHandshakeTimeoutMs);
}
```
(`kHandshakeTimeoutMs` уже в анонімному namespace — зробити його доступним: перенести обидві константи у приватні `static constexpr` класу або лишити в namespace і посилатися; тут використано namespace-константу.)

- [ ] **Step 5: Зібрати й запустити — має пройти**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest && ./bin/Release/ecr_privatjson_selftest_x64.exe`
Expected: `[PASS]` для Purchase happy (ok/0000/invoiceNumber) і Refund declined (!ok/1002/EMV Decline).

- [ ] **Step 6: Commit**

```bash
git add src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp tests/ecr_privatjson_selftest.cpp
git commit -m "feat(ecr): синхронні операції Purchase/Refund/CheckConnection/GetReceiptInfo + MapResult"
```

---

### Task 3: Драйвер — poller статусу (getLastStatMsgCode) під час операції

**Files:** Modify `EcrPrivatJsonDriver.{h,cpp}`; Test у `ecr_privatjson_selftest.cpp`.

**Interfaces:**
- Produces: `int LastStatus() const;` (останній `getLastStatMsgCode`, -1 якщо нема). Внутрішньо: poller-потік, що під час активної операції полить статус на service-доріжці й оновлює `lastStatus_`. Метод `Execute` тепер піднімає poller на час primary-запиту.

- [ ] **Step 1: Написати падаючий тест**

Емулятор віддає послідовність статусів через лічильник викликів, а Purchase — фінальну відповідь:
```cpp
static void TestDriverStatusPoll() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"3"},"error":false})";
        return "";
    });
    // Purchase відповідає не одразу — даємо poller-у шанс опитати статус (емулятор спить перед відповіддю).
    emu.OnRequest("Purchase", [](const nlohmann::json&)->std::string{
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"7"},"error":false})";
    });
    CHECK(emu.Start(), "StatusPoll: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "StatusPoll: Connect");
    auto pr = drv.Purchase("10.00");
    CHECK(pr.ok && pr.code == "0000", "StatusPoll: Purchase завершився ok");
    CHECK(drv.LastStatus() == 3, "StatusPoll: poller зафіксував LastStatMsgCode=3 під час операції");
    drv.Disconnect();
    emu.Stop();
}
```
Додати `TestDriverStatusPoll();` у `main()`.

- [ ] **Step 2: Запустити — має впасти (`LastStatus` немає)**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest`
Expected: FAIL — `LastStatus` не член.

- [ ] **Step 3: Оголосити у `.h`**

`public:` → `int LastStatus() const;`
`private:`:
```cpp
    void PollerLoop(std::atomic<bool>& stop);   ///< полить getLastStatMsgCode; шле interrupt за прапорцем
    std::atomic<int> lastStatus_{ -1 };
    static constexpr int kPollIntervalMs = 500;    ///< 0.5с полінг статусу
    static constexpr int kServiceTimeoutMs = 3000;
```
Додати інклуди `.h`: `#include <atomic>`.

- [ ] **Step 4: Реалізувати у `.cpp`**

Додати `#include <chrono>` (є), реалізацію poller і обгортання `Execute` у poller:
```cpp
int EcrPrivatJsonDriver::LastStatus() const { return lastStatus_.load(); }

void EcrPrivatJsonDriver::PollerLoop(std::atomic<bool>& stop) {
    while (!stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        if (stop.load() || !session_) break;
        auto stat = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType", "getLastStatMsgCode"}});
        GateSend();
        RequestResult s = session_->RequestService(stat, kServiceTimeoutMs);
        if (s.status == RequestStatus::Response) {
            ParsedResponse pr = EcrJsonCodec::Parse(s.frame);
            if (pr.valid && pr.params.is_object()) {
                std::string code = pr.params.value("LastStatMsgCode", std::string{});
                if (!code.empty()) { try { lastStatus_.store(std::stoi(code)); } catch (...) {} }
            }
        }
    }
}
```
І в `Execute` обгорнути primary-запит poller-ом (замінити тіло після `GateSend()`):
```cpp
ResultEnvelope EcrPrivatJsonDriver::Execute(const std::string& method,
                                            const nlohmann::json& params, int timeoutMs) {
    if (!IsConnected()) return ResultEnvelope::Fail("NOT_CONNECTED", "Термінал не підключено");
    lastStatus_.store(-1);
    auto req = EcrJsonCodec::BuildRequest(method, 0, params.is_null() ? nlohmann::json(nullptr) : params);

    std::atomic<bool> pollerStop{ false };
    std::thread poller([this, &pollerStop] { PollerLoop(pollerStop); });

    GateSend();
    RequestResult r = session_->RequestPrimary(req, timeoutMs);

    pollerStop.store(true);
    poller.join();
    return MapResult(r);
}
```

- [ ] **Step 5: Зібрати й запустити — має пройти**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest && ./bin/Release/ecr_privatjson_selftest_x64.exe`
Expected: `[PASS]` StatusPoll (Purchase ok + LastStatus==3).

- [ ] **Step 6: Commit**

```bash
git add src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp tests/ecr_privatjson_selftest.cpp
git commit -m "feat(ecr): poller статусу getLastStatMsgCode на service-доріжці під час операції"
```

---

### Task 4: Драйвер — interrupt (скасування) + відновлення після desync

**Files:** Modify `EcrPrivatJsonDriver.{h,cpp}`; Test.

**Interfaces:**
- Produces: `void RequestInterrupt();` (виставляє прапорець; poller шле `interrupt` на service-доріжці). Poller при виставленому прапорці шле `interrupt` один раз. Плюс приватний `ResultEnvelope RecoverAfterDesync();` — при `Timeout`+desync: полить `getLastStatMsgCode` доки термінал не в спокої, `MarkSynchronized()`, тягне `GetReceiptInfo` (best-effort).

- [ ] **Step 1: Написати падаючі тести**

```cpp
static void TestDriverInterrupt() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    std::atomic<bool> interruptSeen{false};
    emu.OnRequest("ServiceMessage", [&](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"6"},"error":false})";
        if (mt == "interrupt") { interruptSeen.store(true); return R"({"method":"ServiceMessage","params":{"msgType":"interruptTransmitted"},"error":false})"; }
        return "";
    });
    // Purchase «висить», доки не прийде interrupt: емулятор чекає прапорець, тоді віддає 1001.
    emu.OnRequest("Purchase", [&](const nlohmann::json&)->std::string{
        for (int i = 0; i < 100 && !interruptSeen.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return R"({"method":"Purchase","params":{"responseCode":"1001"},"error":true,"errorDescription":"Oперація скасов."})";
    });
    CHECK(emu.Start(), "Interrupt: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "Interrupt: Connect");

    std::thread canceller([&]{ std::this_thread::sleep_for(std::chrono::milliseconds(300)); drv.RequestInterrupt(); });
    auto pr = drv.Purchase("10.00");
    canceller.join();
    CHECK(interruptSeen.load(), "Interrupt: термінал отримав interrupt");
    CHECK(!pr.ok && pr.code == "1001", "Interrupt: Purchase завершився responseCode 1001 (скасовано)");
    drv.Disconnect();
    emu.Stop();
}
```
Додати `TestDriverInterrupt();` у `main()`.

- [ ] **Step 2: Запустити — має впасти (`RequestInterrupt` немає)**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest`
Expected: FAIL — `RequestInterrupt` не член.

- [ ] **Step 3: Оголосити у `.h`**

`public:` → `void RequestInterrupt();`
`private:` → `std::atomic<bool> interruptRequested_{false};` та `std::atomic<bool> interruptSent_{false};`

- [ ] **Step 4: Реалізувати у `.cpp`**

```cpp
void EcrPrivatJsonDriver::RequestInterrupt() { interruptRequested_.store(true); }
```
У `PollerLoop`, на початку кожної ітерації (перед полінгом статусу), додати відправку interrupt раз:
```cpp
        if (interruptRequested_.load() && !interruptSent_.load()) {
            auto intr = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType", "interrupt"}});
            GateSend();
            session_->RequestService(intr, kServiceTimeoutMs);   // interruptTransmitted; фінал 1001 ловить worker
            interruptSent_.store(true);
        }
```
У `Execute` скинути прапорці перед стартом poller: `interruptRequested_.store(false); interruptSent_.store(false);`.

- [ ] **Step 5: Зібрати й запустити — має пройти**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest && ./bin/Release/ecr_privatjson_selftest_x64.exe`
Expected: `[PASS]` Interrupt (термінал отримав interrupt; Purchase→1001).

- [ ] **Step 6: Commit**

```bash
git add src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp tests/ecr_privatjson_selftest.cpp
git commit -m "feat(ecr): interrupt (скасування) через service-доріжку poller-а"
```

---

### Task 5: Драйвер — асинхронний API (StartOperation/State/Result/Cancel) поверх JobEngine

**Files:** Modify `EcrPrivatJsonDriver.{h,cpp}`; Test.

**Interfaces:**
- Produces:
  - `bool StartOperation(const std::string& method, const nlohmann::json& params, int timeoutMs);` — неблокуюче; піднімає JobEngine з `op = [=]{ return Execute(method, params, timeoutMs); }`. false, якщо вже виконується.
  - `bool StartPurchase(const std::string& amount, const nlohmann::json& extra = {});`
  - `JobState OperationState() const;`
  - `bool TryGetOperationResult(ResultEnvelope& out) const;`
  - `void CancelOperation();` — `RequestInterrupt()` + `JobEngine::SetState(Interrupting)`.
- Consumes: `JobEngine` (член `EcrPrivatJsonDriver`).

- [ ] **Step 1: Написати падаючий тест**

```cpp
static void TestDriverAsync() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    emu.OnRequest("Purchase", [](const nlohmann::json&)->std::string{
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"9"},"error":false})";
    });
    CHECK(emu.Start(), "Async: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "Async: Connect");
    CHECK(drv.StartPurchase("25.00"), "Async: StartPurchase → true (запущено)");
    CHECK(drv.StartPurchase("25.00") == false, "Async: повторний StartPurchase під час виконання → false");

    ResultEnvelope out;
    for (int i = 0; i < 200 && drv.OperationState() == JobState::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(drv.OperationState() == JobState::Done, "Async: операція завершилась (Done)");
    CHECK(drv.TryGetOperationResult(out) && out.ok && out.payload["invoiceNumber"] == "9",
          "Async: результат доступний (ok, invoiceNumber=9)");
    drv.Disconnect();
    emu.Stop();
}
```
Додати `TestDriverAsync();` у `main()`.

- [ ] **Step 2: Запустити — має впасти**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest`
Expected: FAIL — `StartPurchase`/`OperationState` не члени.

- [ ] **Step 3: Оголосити у `.h`**

Додати інклуд `#include "../../platform/JobEngine.h"`. У `public:`:
```cpp
    bool StartOperation(const std::string& method, const nlohmann::json& params, int timeoutMs);
    bool StartPurchase(const std::string& amount, const nlohmann::json& extra = {});
    bool StartRefund(const std::string& amount, const std::string& rrn, const nlohmann::json& extra = {});
    JobState OperationState() const;
    bool TryGetOperationResult(ResultEnvelope& out) const;
    void CancelOperation();
```
У `private:` → `JobEngine job_;`

- [ ] **Step 4: Реалізувати у `.cpp`**

```cpp
bool EcrPrivatJsonDriver::StartOperation(const std::string& method, const nlohmann::json& params, int timeoutMs) {
    if (!IsConnected()) return false;
    return job_.Start([this, method, params, timeoutMs]() { return Execute(method, params, timeoutMs); });
}
bool EcrPrivatJsonDriver::StartPurchase(const std::string& amount, const nlohmann::json& extra) {
    nlohmann::json p = extra.is_object() ? extra : nlohmann::json::object(); p["amount"] = amount;
    return StartOperation("Purchase", p, kOperationTimeoutMs);
}
bool EcrPrivatJsonDriver::StartRefund(const std::string& amount, const std::string& rrn, const nlohmann::json& extra) {
    nlohmann::json p = extra.is_object() ? extra : nlohmann::json::object(); p["amount"] = amount; p["rrn"] = rrn;
    return StartOperation("Refund", p, kOperationTimeoutMs);
}
JobState EcrPrivatJsonDriver::OperationState() const { return job_.State(); }
bool EcrPrivatJsonDriver::TryGetOperationResult(ResultEnvelope& out) const { return job_.TryGetResult(out); }
void EcrPrivatJsonDriver::CancelOperation() { RequestInterrupt(); job_.SetState(JobState::Interrupting); }
```
У `Disconnect()` додати `job_.Join();` ПЕРЕД зупинкою сесії (дочекатись worker, щоб не рвати сесію під активним запитом):
```cpp
void EcrPrivatJsonDriver::Disconnect() {
    job_.Join();
    if (session_) { session_->Stop(); session_.reset(); }
}
```

- [ ] **Step 5: Зібрати й запустити — має пройти**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest && ./bin/Release/ecr_privatjson_selftest_x64.exe`
Expected: `[PASS]` Async (Start→true, повторний→false, Done, результат).

- [ ] **Step 6: Commit**

```bash
git add src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp tests/ecr_privatjson_selftest.cpp
git commit -m "feat(ecr): асинхронний API драйвера поверх JobEngine (StartPurchase/State/Result/Cancel)"
```

---

### Task 6: 1С-фасад `AddinECRPrivatJSON`

**Files:**
- Create: `src/components/AddinECRPrivatJSON.h`, `.cpp`
- Modify: `CMake/components.cmake` (нова OBJECT-ліба `ecr_facade_component` + у SHARED DLL), `CMake/compiler_settings.cmake` (`/utf-8`)
- Test: `tests/ecr_privatjson_selftest.cpp` (смоук через `AddInNative::CreateObject`)

**Interfaces:**
- Consumes: `AddInNative`, `EcrPrivatJsonDriver`.
- Produces: компонента 1С `ECRPrivatJSON` з методами (en/ru): `Connect/Подключить`, `Disconnect/Отключить`, `IsConnected/Подключен`, `CheckConnection/ПроверитьСвязь`, `GetTerminalInfo/ВерсияПО`, `Purchase/Оплата`, `Refund/Возврат`, `GetReceiptInfo/ПолучитьЧек`, `StartPurchase/НачатьОплату`, `StartRefund/НачатьВозврат`, `OperationState/СостояниеОперации`, `OperationResult/РезультатОперацииJSON`, `CancelOperation/ПрерватьОперацию`, `LastStatus/СтатусТерминала`, `Vendor/Вендор`, `Model/Модель`, `EnableTrace/ВключитьТрассировку`.

- [ ] **Step 1: Написати падаючий смоук-тест (`ecr_privatjson_selftest.cpp`)**

Потрібен мінімальний мок платформи 1С. Додати перед `main()`:
```cpp
#include "../src/core/AddInNative.h"
// Мінімальний мок IMemoryManager/IAddInDefBase для інстанціювання компоненти в процесі
// (за зразком native_host/core_selftest). Достатньо для реєстрації методів і смоук-виклику.
#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"
class MockMem : public IMemoryManager {
public: bool ADDIN_API AllocMemory(void** p, unsigned long n) override { *p = malloc(n); return *p != nullptr; }
        void ADDIN_API FreeMemory(void** p) override { if (p && *p) { free(*p); *p = nullptr; } } };
class MockConn : public IAddInDefBase {
public: bool ADDIN_API AddError(unsigned short, const WCHAR_T*, const WCHAR_T*, long) override { return true; }
        bool ADDIN_API Read(WCHAR_T*, tVariant*, long*, WCHAR_T**) override { return false; }
        bool ADDIN_API Write(WCHAR_T*, tVariant*) override { return true; }
        bool ADDIN_API RegisterProfileAs(WCHAR_T*) override { return true; }
        bool ADDIN_API SetEventBufferDepth(long) override { return true; }
        long ADDIN_API GetEventBufferDepth() override { return 0; }
        bool ADDIN_API ExternalEvent(WCHAR_T*, WCHAR_T*, WCHAR_T*) override { return true; }
        void ADDIN_API CleanEventBuffer() override {}
        bool ADDIN_API SetStatusLine(WCHAR_T*) override { return true; }
        void ADDIN_API ResetStatusLine() override {} };

static void TestFacadeSmoke() {
    AddInNative* comp = AddInNative::CreateObject(u"ECRPrivatJSON");
    CHECK(comp != nullptr, "Facade: CreateObject(ECRPrivatJSON) → не null");
    if (!comp) return;
    MockMem mem; MockConn conn;
    comp->Init(&conn);            // передати об'єкт-з'єднання
    comp->setMemManager(&mem);    // менеджер пам'яті
    // Метод Connect має бути зареєстрований: перевіряємо через FindMethod (за іменем).
    long n = comp->GetNMethods();
    CHECK(n >= 10, "Facade: зареєстровано >=10 методів");
    delete comp;
}
```
> **Примітка:** точні імена `Init`/`setMemManager`/`GetNMethods`/`FindMethod` звірити з `IComponentBase`/`AddInNative.h` (SDK 1С); за потреби адаптувати виклики під фактичні сигнатури (це стандартні методи `IComponentBase`). Додати `TestFacadeSmoke();` у `main()`.

- [ ] **Step 2: Запустити — має впасти (немає компоненти `ECRPrivatJSON`)**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest`
Expected: FAIL лінкування/рантайм — компонента не зареєстрована / `AddinECRPrivatJSON.h` відсутній.

- [ ] **Step 3: Створити `src/components/AddinECRPrivatJSON.h`**

```cpp
#pragma once
#include "../core/AddInNative.h"
#include "../drivers/ecr_privatjson/EcrPrivatJsonDriver.h"
#include <vector>
#include <string>

/// Компонента 1С над пілотним драйвером ECRPrivatJSON. Реєструє методи прямо
/// (як AddinUAPKIConnect); делегує EcrPrivatJsonDriver; результати — через
/// this->result / РезультатОперацииJSON.
class AddinECRPrivatJSON : public AddInNative {
public:
    static std::vector<std::u16string> names;
    AddinECRPrivatJSON();
    virtual ~AddinECRPrivatJSON();

private:
    void RegisterMethods();
    EcrPrivatJsonDriver driver_;
    std::string lastResultJson_;   ///< останній ResultEnvelope у JSON (для РезультатОперацииJSON)
};
```

- [ ] **Step 4: Створити `src/components/AddinECRPrivatJSON.cpp`**

```cpp
#include "../core/pch.h"
#include "AddinECRPrivatJSON.h"
#include "../helpers/ServiceTools.h"

REGISTER_COMPONENT(u"ECRPrivatJSON", AddinECRPrivatJSON)

AddinECRPrivatJSON::AddinECRPrivatJSON() {
    REPORT_INFO("Ініціалізація компоненти ECRPrivatJSON");
    RegisterMethods();
}

AddinECRPrivatJSON::~AddinECRPrivatJSON() {
    driver_.Disconnect();
    ServiceTools::DisableComponentLogging(this);
}

void AddinECRPrivatJSON::RegisterMethods() {
    // --- Підключення --------------------------------------------------------
    AddFunction(u"Connect", u"Подключить",
        Ret([this](VH connString) -> bool {
            try { return driver_.Connect(static_cast<std::string>(connString)); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка Connect: ") + e.what()); return false; }
        }),
        std::vector<ParamSpec>{ ParamSpec{ u"connString", u"СтрокаПодключения", /*required*/true, {} } });

    AddProcedure(u"Disconnect", u"Отключить",
        MethFunction(std::function<void()>([this]() { driver_.Disconnect(); })));

    AddFunction(u"IsConnected", u"Подключен",
        Ret([this]() -> bool { return driver_.IsConnected(); }));

    // --- Синхронні операції (повертають ok; деталі — РезультатОперацииJSON) --
    auto runSync = [this](ResultEnvelope env) -> bool {
        lastResultJson_ = env.ToJson().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        this->result = lastResultJson_;   // одразу повертаємо JSON у 1С
        return env.ok;
    };

    AddFunction(u"CheckConnection", u"ПроверитьСвязь",
        [this, runSync](VH) { this->result = runSync(driver_.CheckConnection()); });   // note: див. нижче про Ret vs this->result

    AddFunction(u"Purchase", u"Оплата",
        [this, runSync](VH amount) {
            try { runSync(driver_.Purchase(static_cast<std::string>(amount))); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка Оплата: ") + e.what()); this->result = false; }
        },
        std::vector<ParamSpec>{ ParamSpec{ u"amount", u"Сумма", true, {} } });

    AddFunction(u"Refund", u"Возврат",
        [this, runSync](VH amount, VH rrn) {
            try { runSync(driver_.Refund(static_cast<std::string>(amount), static_cast<std::string>(rrn))); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка Возврат: ") + e.what()); this->result = false; }
        },
        std::vector<ParamSpec>{ ParamSpec{ u"amount", u"Сумма", true, {} }, ParamSpec{ u"rrn", u"RRN", true, {} } });

    AddFunction(u"GetReceiptInfo", u"ПолучитьЧек",
        [this, runSync](VH invoice) { runSync(driver_.GetReceiptInfo(static_cast<std::string>(invoice))); },
        std::vector<ParamSpec>{ ParamSpec{ u"invoiceNumber", u"НомерЧека", true, {} } });

    // --- Асинхронні операції ------------------------------------------------
    AddFunction(u"StartPurchase", u"НачатьОплату",
        Ret([this](VH amount) -> bool { return driver_.StartPurchase(static_cast<std::string>(amount)); }),
        std::vector<ParamSpec>{ ParamSpec{ u"amount", u"Сумма", true, {} } });

    AddFunction(u"OperationState", u"СостояниеОперации",
        Ret([this]() -> int { return static_cast<int>(driver_.OperationState()); }));

    AddFunction(u"OperationResult", u"РезультатОперацииJSON",
        Ret([this]() -> std::string {
            ResultEnvelope out;
            if (driver_.TryGetOperationResult(out))
                return out.ToJson().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            return lastResultJson_;
        }));

    AddProcedure(u"CancelOperation", u"ПрерватьОперацию",
        MethFunction(std::function<void()>([this]() { driver_.CancelOperation(); })));

    AddFunction(u"LastStatus", u"СтатусТерминала",
        Ret([this]() -> int { return driver_.LastStatus(); }));

    AddFunction(u"Vendor", u"Вендор", Ret([this]() -> std::string { return driver_.Vendor(); }));
    AddFunction(u"Model", u"Модель", Ret([this]() -> std::string { return driver_.Model(); }));

    REPORT_INFO("Реєстрація методів ECRPrivatJSON завершена");
}
```
> **Реалізаційна нотатка для виконавця:** `runSync` тут викликається двічі різними шляхами (у `CheckConnection` через `this->result = runSync(...)` помилково — `runSync` уже ставить `this->result`; прибрати зовнішнє присвоєння, лишити просто `runSync(driver_.CheckConnection());`). Уніфікуй: **не** обгортай `Ret()` хендлери, що самі ставлять `this->result` (правило AGENTS.md). Тобто синхронні операції — «голі» лямбди, що кличуть `runSync(...)` (яка ставить `this->result` рядком JSON). Тільки `Connect`/`IsConnected`/геттери — через `Ret()`.

- [ ] **Step 5: CMake — OBJECT-ліба фасаду + у SHARED DLL + `/utf-8`**

`CMake/components.cmake`:
```cmake
add_library(ecr_facade_component OBJECT
    src/components/AddinECRPrivatJSON.h
    src/components/AddinECRPrivatJSON.cpp
)
target_include_directories(ecr_facade_component PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src
    ${SPDLOG_INCLUDE_DIR} ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_compile_definitions(ecr_facade_component PRIVATE _WINDOWS UNICODE _UNICODE)
add_dependencies(ecr_facade_component base_component spdlog nlohmann_json helpers_component driver_ecr_privatjson_component platform_component)
```
Додати `$<TARGET_OBJECTS:ecr_facade_component>` у `add_library(${TARGET} SHARED ...)`.
`CMake/compiler_settings.cmake` — додати `target_compile_options(ecr_facade_component PRIVATE /utf-8)`.

- [ ] **Step 6: Тестова ціль лінкує фасад**

`tests/CMakeLists.txt` — у `ecr_privatjson_selftest` додати `$<TARGET_OBJECTS:ecr_facade_component>` (щоб `CreateObject(u"ECRPrivatJSON")` знаходив компоненту).

- [ ] **Step 7: Зібрати повністю (DLL реєструє компоненту) і запустити**

Run: `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests; ./bin/Release/ecr_privatjson_selftest_x64.exe`
Expected: `[PASS]` FacadeSmoke (CreateObject не null, >=10 методів). L0.1 (3 експорти DLL) без змін.

- [ ] **Step 8: Commit**

```bash
git add src/components/AddinECRPrivatJSON.h src/components/AddinECRPrivatJSON.cpp CMake/components.cmake CMake/compiler_settings.cmake tests/CMakeLists.txt tests/ecr_privatjson_selftest.cpp
git commit -m "feat(ecr): 1С-фасад AddinECRPrivatJSON (реєстрація методів, делегування драйверу)"
```

---

### Task 7: Standalone-емулятор термінала (EXE для тесту з 1С)

**Files:**
- Modify: `tests/support/TerminalEmulator.h`, `.cpp` (перевантаження `Start(int port)` з фіксованим портом)
- Create: `tests/ecr_terminal_emulator_main.cpp`
- Modify: `tests/CMakeLists.txt` (ціль `ecr_terminal_emulator`)

**Interfaces:**
- Produces: `bool TerminalEmulator::Start(int port);` (0=ефемерний — наявна поведінка). Standalone EXE `ecr_terminal_emulator[_x64].exe [port]` (default 2000) з повним набором сценаріїв зі спеки; слухає, доки не Ctrl-C.

- [ ] **Step 1: Розширити `TerminalEmulator` фіксованим портом**

У `TerminalEmulator.h`: `bool Start() { return Start(0); } bool Start(int port);`
У `.cpp` замінити `addr.sin_port = 0;` на `addr.sin_port = htons(static_cast<u_short>(port));` і сигнатуру `Start()`→`Start(int port)`; при `port==0` лишається ефемерний (htons(0)==0).

- [ ] **Step 2: Створити `tests/ecr_terminal_emulator_main.cpp`**

```cpp
// Standalone-емулятор термінала ПриватБанк для тестування З 1С без обладнання.
// Слухає TCP (default 2000), відповідає скриптованими JSON зі специфікації.
// pch НЕ підключаємо (правило tests/).
#include "support/TerminalEmulator.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>

int main(int argc, char** argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 2000;
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","step":0,"params":{"code":"00","responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType", std::string{}) : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"identify","result":"OK","vendor":"PAX","model":"s800"},"error":false,"errorDescription":""})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false,"errorDescription":""})";
        if (mt == "interrupt") return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"interruptTransmitted"},"error":false,"errorDescription":""})";
        return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"methodNotImplemented"},"error":true,"errorDescription":"Not implemented"})";
    });
    emu.OnRequest("CheckConnection", [](const nlohmann::json&){ return R"({"method":"CheckConnection","step":0,"params":{"responseCode":"0000"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("GetTerminalInfo", [](const nlohmann::json&){ return R"({"method":"GetTerminalInfo","step":0,"params":{"version":"emu-1.0"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("Purchase", [](const nlohmann::json& q){
        std::string amount = q.contains("params") ? q["params"].value("amount", std::string{}) : std::string{};
        return std::string(R"({"method":"Purchase","step":0,"params":{"responseCode":"0000","invoiceNumber":"1001","rrn":"555000111","amount":")") + amount + R"("},"error":false,"errorDescription":""})";
    });
    emu.OnRequest("Refund", [](const nlohmann::json&){ return R"({"method":"Refund","step":0,"params":{"responseCode":"0000","invoiceNumber":"1002"},"error":false,"errorDescription":""})"; });
    emu.OnRequest("GetReceiptInfo", [](const nlohmann::json&){ return R"({"method":"GetReceiptInfo","step":0,"params":{"responseCode":"0000","invoiceNumber":"1001","txnType":"1","trnStatus":"1"},"error":false,"errorDescription":""})"; });

    if (!emu.Start(port)) { std::printf("Не вдалося зайняти порт %d\n", port); return 1; }
    std::printf("ECR terminal emulator слухає 127.0.0.1:%d — Ctrl-C для виходу\n", emu.Port());
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
```

- [ ] **Step 3: CMake — ціль `ecr_terminal_emulator` (ДО UAPKI-гейта)**

`tests/CMakeLists.txt` (після `ecr_privatjson_selftest`, перед UAPKI-гейтом):
```cmake
add_executable(ecr_terminal_emulator ecr_terminal_emulator_main.cpp support/TerminalEmulator.cpp)
set_target_properties(ecr_terminal_emulator PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ecr_terminal_emulator${_TEST_ARCH_SUFFIX}")
target_include_directories(ecr_terminal_emulator PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR} ${NLOHMANN_JSON_INCLUDE_DIR})
target_compile_definitions(ecr_terminal_emulator PRIVATE _WINDOWS UNICODE _UNICODE)
target_link_libraries(ecr_terminal_emulator PRIVATE ws2_32)
if(MSVC)
    target_compile_options(ecr_terminal_emulator PRIVATE /utf-8)
endif()
```

- [ ] **Step 4: Зібрати й перевірити ручний запуск**

Run: `cmake --build build_x64 --config Release --target ecr_terminal_emulator; ./bin/Release/ecr_terminal_emulator_x64.exe 2000`
Expected: друкує `ECR terminal emulator слухає 127.0.0.1:2000 …` (Ctrl-C для виходу). (Юніт-тести не чіпаємо — це ручний інструмент для 1С.)

- [ ] **Step 5: Commit**

```bash
git add tests/support/TerminalEmulator.h tests/support/TerminalEmulator.cpp tests/ecr_terminal_emulator_main.cpp tests/CMakeLists.txt
git commit -m "feat(ecr): standalone-емулятор термінала (EXE) для тестування з 1С без обладнання"
```

---

### Task 8: L3-харнес `ecr_native_host` (через DLL, проти in-process емулятора)

**Files:**
- Create: `tests/ecr_native_host.cpp`
- Modify: `tests/CMakeLists.txt` (ціль `ecr_native_host`, ДО UAPKI-гейта)

**Interfaces:**
- Consumes: головна DLL (`SimplyAddinConnect_x64.dll`) через `LoadLibraryW`+`GetClassObject`; `IComponentBase`; `TerminalEmulator` (in-process).
- Produces: exe `ecr_native_host[_x64].exe` — вантажить DLL, створює компоненту `ECRPrivatJSON`, `Подключить(tcp://127.0.0.1:<emuPort>)`, `Оплата`, звіряє результат. Exit 0 = OK.

- [ ] **Step 1: Створити `tests/ecr_native_host.cpp`**

Побудувати за зразком `native_host.cpp` (той самий механізм `LoadLibraryW`/`GetClassObject`/`HostConnect`/`HostMemoryManager`/маршалінг `tVariant`), але викликати `ECRPrivatJSON`. Скелет (виконавець доповнює маршалінг рядків із `native_host.cpp` — ті самі хелпери `u8to16`/`wz2u8` і виклик `CallAsFunc`/`CallAsProc`):
```cpp
#ifndef _WIN32
#include <cstdio>
int main(){ printf("ecr_native_host: Windows only\n"); return 0; }
#else
#include <windows.h>
#include <string>
#include <cstdio>
#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"
#include "support/TerminalEmulator.h"

// ... (скопіювати з native_host.cpp: ARCH_W, HOST_BIN_DIR, u8to16/u16to8/wz2u8,
//      HostMemoryManager, HostConnect — без змін) ...

// GetClassObject/DestroyObject — прототипи експортів головної DLL (як у native_host.cpp).
typedef long (*GetClassObjectPtr)(const WCHAR_T*, IComponentBase**);

static int g_failed = 0;
#define CHECK(c,n) do{ if(c){std::printf("[PASS] %s\n",n);} else {std::printf("[FAIL] %s\n",n);++g_failed;} }while(0)

int main() {
    // 1) Емулятор термінала в цьому процесі.
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt=="identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt=="getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    emu.OnRequest("Purchase", [](const nlohmann::json&){ return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"77"},"error":false})"; });
    CHECK(emu.Start(), "L3: емулятор стартував");

    // 2) Завантажити DLL і створити компоненту ECRPrivatJSON (за зразком native_host.cpp).
    std::wstring dllPath = std::wstring(L"" HOST_BIN_DIR) + L"/SimplyAddinConnect" ARCH_W L".dll";
    HMODULE h = LoadLibraryW(dllPath.c_str());
    CHECK(h != nullptr, "L3: DLL завантажено");
    if (!h) { std::printf("FAILED\n"); return 1; }
    auto getObj = (GetClassObjectPtr)GetProcAddress(h, "GetClassObject");
    IComponentBase* comp = nullptr;
    getObj((const WCHAR_T*)u"ECRPrivatJSON", &comp);
    CHECK(comp != nullptr, "L3: компонента ECRPrivatJSON створена через DLL");

    // 3) Init + виклик Подключить(tcp://127.0.0.1:emuPort) і Оплата — через IComponentBase
    //    (маршалінг tVariant VTYPE_PWSTR — скопіювати з native_host.cpp: CallAsFunc).
    //    ... (виконавець доповнює за зразком native_host.cpp) ...
    //    Очікування: Подключить → true; Оплата("10.00") → JSON із responseCode 0000, invoiceNumber 77.

    std::printf(g_failed ? "\nFAILED: %d\n" : "\nOK\n", g_failed);
    return g_failed ? 1 : 0;
}
#endif
```
> **Виконавцю:** маршалінг `tVariant` (VTYPE_PWSTR у параметрах і в результаті), пошук методу за іменем (`FindMethod`), виклик `CallAsFunc`/`CallAsProc` — **дослівно за `tests/native_host.cpp`** (той самий SDK-контракт). Кроки нижче перевіряють результат.

- [ ] **Step 2: CMake — ціль `ecr_native_host` (ДО UAPKI-гейта)**

`tests/CMakeLists.txt`:
```cmake
add_executable(ecr_native_host ecr_native_host.cpp support/TerminalEmulator.cpp)
set_target_properties(ecr_native_host PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ecr_native_host${_TEST_ARCH_SUFFIX}")
target_include_directories(ecr_native_host PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR} ${NLOHMANN_JSON_INCLUDE_DIR})
target_compile_definitions(ecr_native_host PRIVATE _WINDOWS UNICODE _UNICODE
    HOST_BIN_DIR="${CMAKE_SOURCE_DIR}/bin/Release")
target_link_libraries(ecr_native_host PRIVATE ws2_32)
if(MSVC)
    target_compile_options(ecr_native_host PRIVATE /utf-8)
endif()
```

- [ ] **Step 3: Зібрати повністю й запустити L3**

Run: `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests; ./bin/Release/ecr_native_host_x64.exe`
Expected: `[PASS]` DLL завантажено / компонента створена / Подключить / Оплата → JSON 0000+invoiceNumber 77; exit 0.

- [ ] **Step 4: Commit**

```bash
git add tests/ecr_native_host.cpp tests/CMakeLists.txt
git commit -m "test(ecr): L3-харнес ecr_native_host — компонента через DLL проти емулятора"
```

---

### Task 9: Інтеграція в `run_tests.ps1` + синхронізація доків

**Files:**
- Modify: `run_tests.ps1` (рівень L2-ecr: `ecr_native_host`)
- Modify: `AGENTS.md`, `docs/architecture/ecrprivatjson.md` (статус: Частина 2 виконана)

**Interfaces:** оркестратор `run_tests.ps1` після L0.7 запускає `ecr_native_host` як окремий рівень (без UAPKI), додає рядок у підсумкову таблицю; ненульовий exit при провалі.

- [ ] **Step 1: Додати рівень у `run_tests.ps1`**

Знайти блок L0.7 (`ecr_privatjson_selftest`) і після нього додати запуск `ecr_native_host_x64.exe` за тим самим патерном (шукати exe в `bin/Release`, ловити exit-код, додати рядок у таблицю; проходить і в режимі `-NoUapki`, бо не залежить від UAPKI). Дотриматися наявного стилю рівнів.

- [ ] **Step 2: Зібрати й прогнати повний гейт (обидві архітектури, без UAPKI)**

Run:
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
powershell -File run_tests.ps1 -NoUapki x64
powershell -File run_tests.ps1 -NoUapki x86
```
Expected: L0.5/L0.6/L0.7/L2-ecr — усі PASS; підсумкова таблиця без FAIL; exit 0 на обох архітектурах.

- [ ] **Step 3: Оновити доки під фактичний стан**

`AGENTS.md` (розділ «Тести») і `docs/architecture/ecrprivatjson.md`: додати `ecr_native_host` (L2/L3 ECR) і `ecr_terminal_emulator` (ручний інструмент для 1С); позначити «Частина 2 виконана: операції/JobEngine/фасад/тест-контур». Відмітити в плані Частини 2 виконані кроки (`- [x]`).

- [ ] **Step 4: Commit**

```bash
git add run_tests.ps1 AGENTS.md docs/architecture/ecrprivatjson.md docs/tasks/2026-07-21_plan_ecr_privatjson_p2_operations_and_1c.md
git commit -m "test(ecr): інтеграція L2-ecr у run_tests + синхронізація канону (Частина 2 виконана)"
```

---

## Підсумок Частини 2

Після Task 9: DLL реєструє компоненту `ECRPrivatJSON` з повним набором методів (підключення, синхронні/асинхронні операції, статус, скасування, геттери); драйвер виконує операції з полінгом статусу, перериванням і мапінгом помилок; тест-контур покриває unit (L0.7), L3 через DLL (`ecr_native_host`) і дає **standalone-емулятор** для ручного тесту **з реальної 1С без обладнання**. Гейт зелений обидві архітектури.

**Далі — Етап 3 (з користувачем):** обробка/кнопки на боці 1С для тестування всіх викликів компоненти проти `ecr_terminal_emulator`.

## Self-review (проти спеки, свіжим оком)

- **Покриття §7/§13.5-7:** JobEngine (Task 1); операції+MapResult (Task 2); poller статусу (Task 3); interrupt+desync (Task 4); async API (Task 5); фасад із реєстрацією методів і подіями (Task 6); тест-контур — standalone-емулятор для 1С (Task 7), L3 через DLL (Task 8), run_tests (Task 9).
- **Свідомо ВИКЛЮЧЕНО (рішення користувача, задокументовано вгорі):** інтерактивне `correctTransaction`/`AwaitingCashDecision`, інтерактивний Partial approval, генерований `OperationRegistry`. Desync-відновлення — Task 4 (best-effort через service+GetReceiptInfo).
- **Плейсхолдери:** Task 6 і Task 8 містять реалізаційні нотатки, де точні SDK-сигнатури (`Init`/`GetNMethods`/`CallAsFunc`/маршалінг `tVariant`) беруться ДОСЛІВНО з наявних `native_host.cpp`/`AddInNative.h` — це не «вигадай», а «звірити з наявним контрактом» (SDK 1С однаковий для всіх компонент). Уся протокольна логіка (Tasks 1-5) — повний код.
- **Узгодженість типів:** `ResultEnvelope`/`JobState`/`RequestResult{status,frame}`/`ParsedResponse`/`ParamSpec`/`MethFunction`/`Ret` — імена й поля збігаються з наявними заголовками (`ResultEnvelope.h`, `JobEngine.h`, `RequestTypes.h`, `EcrJsonCodec.h`, `AddInNative.h`) і між задачами.
- **Ризик-нотатка для виконавця:** правило AGENTS.md «не обгортати `Ret()` хендлери, що самі ставлять `this->result`» — синхронні операції (Purchase/Refund/CheckConnection/GetReceiptInfo) ставлять `this->result` через `runSync`, тож реєструються «голими» лямбдами, НЕ через `Ret()` (див. нотатку в Task 6 Step 4).
