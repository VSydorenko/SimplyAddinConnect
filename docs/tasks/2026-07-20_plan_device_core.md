# Device-facing ядро (ITransport + IFramer + DeviceSession) — план імплементації

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development або
> superpowers:executing-plans — виконувати задача за задачею. Кроки — чекбокси `- [ ]`.

**Goal:** Реалізувати device-facing фундамент драйверів за дизайном
`docs/tasks/2026-07-20_design_device_transport_session.md` (ред. 3): байтовий транспорт →
кадрування → класифікація → сесія запит/відповідь, повністю тестований без обладнання;
+ реальні фікси наявних транспортів; + прибирання мертвого ECR/WSServer.

**Architecture:** Нова ціль `wire_component` (OBJECT-бібліотека) у `src/transport/`:
`RequestTypes.h`, `IFramer`+`NullTerminatedFramer`, `IFrameClassifier`, `DeviceSession`,
+ приведені під контракт транспорти COM/TCP/WSClient. Тестова консоль `wire_selftest`
(свій раннер, як `core_selftest` з Етапу 0) лінкує `wire_component`+`helpers`+`base` і
ганяє framer-вектори та `DeviceSession` над `LoopbackTransport` (детермінований подвійник
ITransport). Async-рушій операцій, `DeviceDriverComponent`, `PrivatDriver` — НЕ тут.

**Tech Stack:** C++17, MSVC (VS2022), CMake ≥3.16, spdlog (лишається), ixwebsocket
(client-only), Winsock2, Win32 Comm API. Свій хенд-ролед тест-раннер (без gtest).

**Спосіб роботи з дизайном:** дизайн ред. 3 — джерело правди. §14 (інваріанти) —
ОБОВ'ЯЗКОВІ тести. §15 (класифікатор Привату) — НЕ реалізується тут (драйверна фаза);
у цій фазі класифікатор — лише інтерфейс + тестові подвійники. Контракт для виконавця:
**повні інтерфейси й тести в цьому плані — це специфікація; тіла складних методів
(`DeviceSession`) виконавець пише так, щоб пройшли наведені тести й §14.**

## Global Constraints (з AGENTS.md + дизайну — діють у КОЖНІЙ задачі)

- Кожен `.cpp` у `src/` починається з `#include "../core/pch.h"` (за фактичною глибиною).
  **Тестові `.cpp` у `tests/` pch НЕ підключають.**
- Логування — лише макроси `NEUTRAL_REPORT_*` (статичні/вільні; 1-й арг — ім'я компоненти).
  Прямий `spdlog` заборонено. Повідомлення — конкатенація, з великої, без крапки.
- Мова коду/комітів — українська/російська (дотримуйся мови файлу).
- MSVC: нові цілі з `/utf-8`; `_WINDOWS UNICODE _UNICODE` (інакше `WCHAR_T`/ABI розійдуться).
- Збірка лишається зеленою x86 і x64, з `-WithUAPKI` і без. UAPKI-стек НЕ чіпати.
- **Правки — точкові** (Edit наявних файлів), не переписування цілих файлів.
- Потокове правило (наскрізне, §6 дизайну): user-колбеки лише на dispatcher-потоці;
  `m_` ніколи не тримається під час `Send`/`Close`/user-колбеків.
- `version.h` генерує `build_project.ps1` — не редагувати руками.

**Цикл збірки/тесту (з кореня репо):**
```powershell
cmake -S . -B build_x64 -A x64 -DBUILD_TESTS=ON
cmake --build build_x64 --config Release --target wire_selftest
bin\Release\wire_selftest_x64.exe
```

---

### Task 1: Тест-харнес `wire_selftest` + `wire_component` + Framer

**Files:**
- Create: `src/transport/RequestTypes.h`, `src/transport/IFramer.h`,
  `src/transport/NullTerminatedFramer.h`, `src/transport/NullTerminatedFramer.cpp`
- Create: `tests/wire_selftest.cpp`
- Modify: `CMake/components.cmake` (нова ціль `wire_component`), `tests/CMakeLists.txt`

**Interfaces (Produces):**
```cpp
// RequestTypes.h
enum class RequestStatus { Response, Busy, Unsupported, Timeout, Disconnected,
                           SendFailed, Stopped, Concurrent, Desynchronized };
struct RequestResult { RequestStatus status; std::vector<uint8_t> frame; };

// IFramer.h
struct FrameOptions { bool leadingDelimiter = false; };
class IFramer {
public:
    virtual ~IFramer() = default;
    virtual void Feed(const std::vector<uint8_t>& chunk,
                      std::vector<std::vector<uint8_t>>& out) = 0;
    virtual std::vector<uint8_t> Wrap(const std::vector<uint8_t>& payload,
                                      FrameOptions opts = {}) = 0;
    virtual void Reset() = 0;
};

// NullTerminatedFramer.h — конструктор приймає maxBufferedBytes (дефолт 1<<20)
class NullTerminatedFramer : public IFramer { /* Feed/Wrap/Reset + std::mutex framerMutex_ */ };
```

- [ ] **Step 1: Написати падаючий тест (харнес + framer-вектори)**

`tests/wire_selftest.cpp` — раннер (скопіювати патерн `CHECK`/`main` з
`tests/core_selftest.cpp`) + функція `TestNullTerminatedFramer`:

```cpp
#include "../src/transport/NullTerminatedFramer.h"
#include <cstdio>
#include <string>
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
int main(){ std::printf("=== wire_selftest ===\n"); TestNullTerminatedFramer();
    std::printf("=== %s (failed:%d) ===\n", g_failed?"FAIL":"OK", g_failed); return g_failed?1:0; }
```

- [ ] **Step 2: Переконатися, що збірка падає**

Run: `cmake --build build_x64 --config Release --target wire_selftest`
Expected: FAIL — ціль/файли не існують.

- [ ] **Step 3: Реалізувати RequestTypes.h, IFramer.h, NullTerminatedFramer**

`NullTerminatedFramer.cpp`: `Feed` під `framerMutex_` накопичує в `buffer_`, у циклі шукає
`0x00`, витягує кадр (без термінатора), порожні кадри пропускає; якщо `buffer_.size()>max_`
без термінатора — `buffer_.clear()` (постумова §14: не «отруєний»). `Wrap` — `payload+0x00`,
за `opts.leadingDelimiter` — `0x00` спереду. `Reset` — `buffer_.clear()`.

- [ ] **Step 4: CMake — ціль wire_component + wire_selftest**

`CMake/components.cmake` — додати OBJECT-ціль (поки лише framer):
```cmake
add_library(wire_component OBJECT
    src/transport/RequestTypes.h
    src/transport/IFramer.h
    src/transport/NullTerminatedFramer.h
    src/transport/NullTerminatedFramer.cpp
)
set_target_properties(wire_component PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(wire_component PRIVATE include ${CMAKE_SOURCE_DIR} src ${SPDLOG_INCLUDE_DIR} ${NLOHMANN_JSON_INCLUDE_DIR})
target_compile_definitions(wire_component PRIVATE _WINDOWS UNICODE _UNICODE)
add_dependencies(wire_component base_component spdlog)
```
`tests/CMakeLists.txt` — ПЕРЕД UAPKI-`return()` (рядок ~54, після блоку `core_selftest`)
додати ціль `wire_selftest` за зразком `core_selftest` (Task 1 Етапу 0):
`OUTPUT_NAME "wire_selftest${_TEST_ARCH_SUFFIX}"`, `CXX_STANDARD 17`, include
`${CMAKE_SOURCE_DIR}/include`+`/src`+`${SPDLOG_INCLUDE_DIR}`,
`target_compile_definitions(... _WINDOWS UNICODE _UNICODE)`, `/utf-8`,
link `$<TARGET_OBJECTS:wire_component>` `$<TARGET_OBJECTS:helpers_component>`
`$<TARGET_OBJECTS:base_component>` + `spdlog::spdlog`.

- [ ] **Step 5: Зібрати, запустити — зелено; Commit**

```powershell
cmake -S . -B build_x64 -A x64 -DBUILD_TESTS=ON
cmake --build build_x64 --config Release --target wire_selftest
bin\Release\wire_selftest_x64.exe   # усі [PASS], exit 0
git add src/transport/RequestTypes.h src/transport/IFramer.h src/transport/NullTerminatedFramer.* tests/wire_selftest.cpp CMake/components.cmake tests/CMakeLists.txt
git commit -m "wire: харнес wire_selftest + wire_component + NullTerminatedFramer (байтові вектори, переповнення)"
```

---

### Task 2: `IFrameClassifier` + тестові подвійники

**Files:** Create `src/transport/IFrameClassifier.h`; Modify `tests/wire_selftest.cpp`,
`CMake/components.cmake` (додати заголовок до wire_component).

**Interfaces (Produces):**
```cpp
enum class FrameClass { PrimaryResponse, ServiceResponse, RejectPrimary, RejectService,
                        RejectBoth, Unsolicited };
enum class RejectReason { Busy, Unsupported };
struct PendingView { const std::vector<uint8_t>* primary; const std::vector<uint8_t>* service; };
struct Classification { FrameClass cls; RejectReason reason; };
class IFrameClassifier { public: virtual ~IFrameClassifier()=default;
    virtual Classification Classify(const PendingView&, const std::vector<uint8_t>&) = 0; };
```

- [ ] **Step 1: Падаючий тест — подвійники**

У `wire_selftest.cpp` додати тестові подвійники (у файлі, не в src): `EchoClassifier`
(`PrimaryResponse` якщо `incoming==*pending.primary`, інакше `Unsolicited`);
`ScriptedClassifier` (повертає задану наперед чергу `Classification`). Тест
`TestClassifierDoubles`: перевірити, що `EchoClassifier` матчить рівні байти й ігнорує інші.

- [ ] **Step 2-3:** Запустити (FAIL — нема заголовка) → створити `IFrameClassifier.h`,
      додати в `wire_component`.
- [ ] **Step 4-5:** Зелено; commit `"wire: IFrameClassifier + тестові подвійники (Echo/Scripted)"`.

---

### Task 3: `LoopbackTransport` (детермінований подвійник ITransport)

**Files:** Modify `tests/wire_selftest.cpp` (додати `LoopbackTransport` у тестовому TU).

**Interfaces (Produces, лише для тестів):**
```cpp
// LoopbackTransport : ITransport — тестовий подвійник (у wire_selftest.cpp).
//  • Send(data) → зберігає у sent_ (під lock) + сигналить sendCv_ (тест чекає факту Send).
//  • InjectRecv(bytes) → доставляє в DataReceived-колбек.
//  • SetStateMode(Worker|Inline) — доставка ConnectionState з worker-потоку АБО синхронно з Open/Close.
//  • ScriptFailNextOpen()/ForceRemoteClose() — для тестів реконекту.
//  • Open/Close — за контрактом §4.1 (exactly-once state; після Close колбеків нема).
```

- [ ] **Step 1: Падаючий тест — сам LoopbackTransport**

`TestLoopbackTransport`: `Open`→`state(true)`; `Send`→`sent_` містить дані + `WaitForSend()`
розблоковується; `InjectRecv`→прилітає в data-колбек; `Close`→`state(false)` рівно раз, після
Close колбеків нема; `SetStateMode(Inline)` доставляє state синхронно.

- [ ] **Step 2-3:** FAIL → реалізувати `LoopbackTransport` (черга + `std::mutex`/`cv`,
      worker-потік доставки для режиму Worker; для Inline — виклик у самому Open/Close).
      Доставка з worker-потоку, детермінізм — через `WaitForSend()` бар'єри, БЕЗ `sleep`.
- [ ] **Step 4-5:** Зелено; commit `"wire: LoopbackTransport — подвійник ITransport (2 режими доставки state, Send-capture, inject)"`.

---

### Task 4: `DeviceSession` — каркас, Start/Stop, RequestPrimary (happy + response-before-wait)

**Files:** Create `src/transport/DeviceSession.h`, `DeviceSession.cpp`; Modify
`CMake/components.cmake` (додати до wire_component + залежність від transport_component/
ixwebsocket за потреби на пізніх тасках), `tests/wire_selftest.cpp`.

**Interfaces (Produces):** повний клас `DeviceSession` за §4.4 дизайну (конструктор
`unique_ptr<ITransport>,unique_ptr<IFramer>,unique_ptr<IFrameClassifier>,SessionConfig`;
`Start/Stop/IsConnected`; `RequestPrimary/RequestService(payload,timeout=-1,FrameOptions)`;
`MarkSynchronized/IsDesynchronized`; `SetUnsolicitedHandler/SetConnectionStateHandler/
SetWireTraceHandler`). У цій задачі реалізувати мінімум: Start (Open+підписка колбеків+
запуск dispatcher-потоку), Stop (teardown §6), `RequestPrimary` (send→wait→classify
PrimaryResponse), решта — заглушки, що дороблюються в Task 5-7.

- [ ] **Step 1: Падаючі тести (над LoopbackTransport)**

`TestSessionPrimaryHappy`: `Start`; `RequestPrimary("REQ")` у окремому потоці; тест
`WaitForSend()`, `InjectRecv("REQ")` (EchoClassifier → PrimaryResponse); результат
`{Response, "REQ"}`. `TestResponseBeforeWait`: інжектнути відповідь ДО входу очікувача в
`wait` (перевірити відсутність lost-wakeup — predicate). `TestStopDuringPending`: `Start`,
`RequestPrimary` без відповіді, `Stop()` → результат `{Stopped}`.

- [ ] **Step 2: FAIL (нема класу).**
- [ ] **Step 3: Реалізація мінімуму `DeviceSession`.**

Алгоритм `RequestPrimary` (§5): під `m_` перевірити `stopping_/connected_/desynchronized_`
і `pendingPrimary` порожній (інакше `Concurrent`); зберегти pending (payload+`epoch`);
**unlock**; `Wrap`; wire-trace; `Send`; під `m_` виставити результат за precedence; далі
`cv_.wait_for(lock, timeout, pred=готово||stopping_||!connected_)`. `OnBytes` (reader): під
`framerMutex_` `Feed`; для кадру під `m_` `Classify`; `PrimaryResponse`→заповнити pending+
`notify`. `DispatchLoop`: черга user-подій. `Stop` — порядок §6 кроки 1-6 (у цій задачі
достатньо базового; повний reentrancy — Task 7).

- [ ] **Step 4: Зелено (прогнати кілька разів — потоковий).**
- [ ] **Step 5: Commit** `"wire: DeviceSession — каркас Start/Stop/RequestPrimary (happy, response-before-wait, stop-during-pending)"`.

---

### Task 5: `DeviceSession` — доріжка service, класи Reject*, unsolicited

**Files:** Modify `DeviceSession.{h,cpp}`, `tests/wire_selftest.cpp`.

**Interfaces (Produces):** `RequestService` робочий; класифікація всіх `FrameClass`.

- [ ] **Step 1: Падаючі тести** (ScriptedClassifier): `TestServiceParallelToPrimary`
      (primary in-flight, `RequestService` проходить паралельно, кожен отримує свою
      відповідь); `TestRejectPrimaryBusy` (deviceBusy-кадр→`{Busy}` НЕГАЙНО, без таймауту);
      `TestRejectService`; `TestRejectBoth` (обидві доріжки завершуються + `IsDesynchronized()`);
      `TestUnsolicitedToHandler` (кадр→unsolicited-хендлер на dispatcher, не в Request);
      `TestSecondPrimaryConcurrent` (другий primary під час активного → `{Concurrent}`).
- [ ] **Step 2-3:** FAIL → реалізувати `RequestService` (окремий `pendingService`,
      серіалізований серед service) і гілки `Classify` у `OnBytes`: `ServiceResponse`,
      `RejectPrimary/Service` (мапінг `RejectReason`→`RequestStatus`), `RejectBoth`
      (обидві+`desynchronized_`+`reconnectRequested_`), `Unsolicited` (кадр у dispatch-чергу).
- [ ] **Step 4-5:** Зелено; commit `"wire: DeviceSession — service-доріжка + класи Reject*/Unsolicited"`.

---

### Task 6: `DeviceSession` — таймаут, desync, реконект, обрив

**Files:** Modify `DeviceSession.{h,cpp}`, `tests/wire_selftest.cpp`.

- [ ] **Step 1: Падаючі тести:** `TestPrimaryTimeout` (мовчання→`{Timeout}` + `IsDesynchronized()`);
      `TestDesyncBlocksPrimaryNotService` (у desync `RequestPrimary`→`{Desynchronized}`,
      `RequestService` дозволено; `MarkSynchronized()` знімає); `TestStaleFrameAfterReconnect`
      (після таймауту+нового `state(true)` пізня відповідь старого epoch НЕ завершує новий
      primary); `TestServiceTimeoutQuarantine` (пізній дубль не завершує новий service);
      `TestDisconnectDuringPending` (`ForceRemoteClose`→pending `{Disconnected}`+реконект);
      `TestInitialOpenFailure` (`ScriptFailNextOpen`→супервізор прокидається за
      `reconnectRequested_`, не лише `!connected_`).
- [ ] **Step 2-3:** FAIL → реалізувати: `ReconnectLoop` (предикат
      `desiredUp && (reconnectRequested_||!connected_)`, backoff, Close+Open, успіх лише
      `state(true)` у межах `connectDeadlineMs`); `epoch`-guard проти stale-кадрів;
      таймаут primary→desync+`reconnectRequested_`; service-таймаут+карантин; `MarkSynchronized`.
- [ ] **Step 4-5:** Зелено (прогнати ×20 на флейки); commit
      `"wire: DeviceSession — timeout/desync/reconnect/disconnect + epoch-guard проти stale"`.

---

### Task 7: `DeviceSession` — потокобезпека, precedence, reentrancy, wire-trace

**Files:** Modify `DeviceSession.{h,cpp}`, `tests/wire_selftest.cpp`.

- [ ] **Step 1: Падаючі/стрес-тести (§14 інваріанти):**
      `TestReentrantRequestFromUnsolicited` (unsolicited-хендлер кличе `RequestService` —
      не вішає, бо dispatcher-потік ≠ reader; watchdog-таймер у тесті); `TestStopFromUnsolicited`
      (хендлер кличе `Stop()` — без self-join, watchdog); `TestSendFailedVsDisconnected`
      (LoopbackTransport робить `Send` fail із синхронним `state(false)` — pending
      завершується РІВНО одним джерелом, не двічі); `TestCallbackQuiescenceAfterClose`
      (після `Stop()` — нуль колбеків, watchdog); `TestWireTraceOrder` (sendAttempt перед
      Send навіть при fail; incoming-trace чанку — перед unsolicited того ж чанку).
- [ ] **Step 2-3:** FAIL → доробити: precedence в `RequestPrimary/Service` (виставляти
      `SendFailed` лише якщо pending ще належить запиту й не завершений); `Stop()` з
      dispatcher-потоку — без self-join (детект потоку, фінальний join у `~DeviceSession`);
      wire-trace у dispatch-чергу з FIFO-порядком; `framerMutex_` навколо Feed/Wrap/Reset;
      setters лише до `Start()`.
- [ ] **Step 4-5:** Зелено (×20); commit `"wire: DeviceSession — precedence/reentrancy/quiescence/wire-trace (§14 інваріанти)"`.

---

### Task 8: Фікси `TransportTCP` + смоук через DeviceSession (localhost-echo)

**Files:** Modify `src/transport/Transport_TCP.{h,cpp}`, `tests/wire_selftest.cpp`.

- [ ] **Step 1: Падаючі тести:** `TestTcpEchoRoundtrip` (in-process localhost-echo:
      `bind(port=0)`+`getsockname`, accept-потік, echo framed; `DeviceSession` над реальним
      `TransportTCP` робить `RequestPrimary` і отримує `{Response}`); `TestTcpCloseNoHang`
      (термінал-echo мовчить, `Stop()`/`Close()` не вішає — watchdog); `TestTcpPartialSend`
      (через injectable write-seam або малий SO_SNDBUF — `Send` дописує залишок / partial→`<0`).
- [ ] **Step 2-3:** FAIL/hang → фікси §9.1: видалити серверний режим; `Close` cleanup завжди
      + порядок swap→shutdown→closesocket→join + синхронізація з `m_sendMutex`; неблокуючий
      connect+select+`SO_ERROR`+повернення в blocking; DNS-deadline (numeric IP або
      `GetAddrInfoExW`); `Send` all-or-error цикл.
- [ ] **Step 4-5:** Зелено; commit `"transport: фікси TransportTCP (Close-порядок/Send-sync, nonblock connect, DNS-deadline, all-or-error) + TCP-echo смоук"`.

---

### Task 9: Фікси `TransportCOM`

**Files:** Modify `src/transport/Transport_COM.{h,cpp}`, `tests/wire_selftest.cpp`.

- [ ] **Step 1: Тест:** `TestComSendAllOrError` (injectable write-seam: partial write →
      дозапис або `<0`); `TestComCloseCleansHandle` (Open з невдалим ConfigurePort → handle
      не витікає — перевірити через мок/лічильник). COM round-trip проти `com0com` —
      **SKIP у CI** (як у дизайні), лише документований ручний крок.
- [ ] **Step 2-3:** FAIL → фікси §9.1: `CreateFileA`→`CreateFileW`; `Close` cleanup за
      валідністю `m_portHandle`; reader на неусувній помилці→`state(false)`+будити супервізор;
      `Send` all-or-error (цикл `WriteFile`); прибрати мертвий `#include <winsock2.h>`.
- [ ] **Step 4-5:** Зелено; commit `"transport: фікси TransportCOM (CreateFileW, Close-cleanup, all-or-error, reader-state)"`.

---

### Task 10: Фікси `TransportWSClient`

**Files:** Modify `src/transport/Transport_WSClient.{h,cpp}`, `tests/wire_selftest.cpp`.

- [ ] **Step 1: Тест:** якщо здійсненно — локальний ws-echo (ix-server лише в тест-збірці)
      для `TestWsReopen` (Close+Open після remote-close реально перепідключає) і
      `TestWsExactlyOnceState`; інакше — документований ручний смоук + юніт на прапорці
      `m_started`/`disableAutomaticReconnection` викликано.
- [ ] **Step 2-3:** FAIL → фікси §9.1: `disableAutomaticReconnection()` у конструкторі;
      `Open` успіх лише за `state(true)`; окремий `m_started`, `stop()` навіть коли
      `m_isOpen/m_isConnecting==false`; контракт `Close` §4.1.
- [ ] **Step 4-5:** Зелено/SKIP-документовано; commit `"transport: фікси TransportWSClient (disableAutoReconnect, m_started reopen, state-based Open)"`.

---

### Task 11: Видалення мертвого коду (WSServer + старий ECR) + CMake + доки

**Files:** Delete `src/transport/Transport_WSServer.{h,cpp}`,
`src/components/AddinECRPrivatJSON.{h,cpp}`, `src/protocols/ECRPrivatJSON/*`,
`src/helpers/ECRPrivatJSON/*`. Modify `CMake/components.cmake`, `CMake/compiler_settings.cmake`,
`docs/architecture/{README,core,build-and-packaging}.md`, `AGENTS.md`.

- [ ] **Step 1: Перевірка посилань** — `grep -rn "AddinECRPrivatJSON\|ECRPrivatJSON\|TransportWSServer\|CreateTransport\|SetTransport" src tests CMake` — переконатися, що поза
      файлами, які видаляємо, посилань немає (аудит підтвердив: `native_host` створює лише
      `AddinUAPKIConnect`; `.def`/`manifest.xml` не чіпати).
- [ ] **Step 2: Видалити файли + синхронно правки CMake** (`components.cmake`: HEADER_FILES
      29,31,35,37-38; SOURCE_FILES 55,57-63,67,69-74; WSServer 146-147; цілі ECR 150-228 +
      залежності/лінк 297-310 + `$<TARGET_OBJECTS>` 340-342; `wire_component` у фінальну DLL;
      `compiler_settings.cmake:29`) — **звірити фактичні рядки перед правкою** (можуть
      зсунутися). Синхронізувати доки (прибрати ECR як наявний).
- [ ] **Step 3: Повна збірка обох архітектур** — `build_project.ps1` (без UAPKI) і
      `build_project.ps1 -WithUAPKI -WithTests` — зелено; DLL містить `TestComponent`+wire
      (+UAPKI за прапором).
- [ ] **Step 4: Commit** `"wire: видалення мертвого WSServer і непрацездатного ECR-драйвера + CMake/доки синхронізовано"`.

---

### Task 12: `run_tests.ps1` L0.6 + режим без UAPKI + повний гейт

**Files:** Modify `run_tests.ps1`, `AGENTS.md` (розділ «Тести»).

- [ ] **Step 1:** Додати `$WireSelftestExe = Join-Path $BinRelease ("wire_selftest"+$ArchSuffix+".exe")`;
      рівень **L0.6** (запуск wire_selftest, exit 0=PASS) після L0.5. Додати **режим без
      UAPKI** (параметр, напр. `-NoUapki`): mode-specific `$haveExes` (лише core+wire),
      build без `-DBUILD_WITH_UAPKI=ON`, provider/L1/L2/L3 → SKIP (не FAIL, не форсувати
      UAPKI). Звірити фактичні рядки `:135`,`:218`,`:225`.
- [ ] **Step 2: Повний прогін** — `build_project.ps1 -WithUAPKI -WithTests`, потім
      `run_tests.ps1 x64` і `x86` (L0-L3 без регресій UAPKI + L0.6 wire), і окремо
      `run_tests.ps1 -NoUapki x64` (core+wire зелені, UAPKI SKIP).
- [ ] **Step 3: AGENTS.md** — додати `wire_selftest`/L0.6 і режим без UAPKI в «Тести».
- [ ] **Step 4: Commit** `"wire: run_tests L0.6 + режим без UAPKI; повний зелений гейт x86/x64"`.

---

## Self-review плану

- **Покриття дизайну:** §4.0-4.4→Task1-7; §5-7 (потоки/помилки/desync)→Task4-7; §8
  wire-trace→Task7; §9.1 фікси транспортів→Task8-10; §9.2 видалення/CMake/доки→Task11;
  §10 тести→кожна задача+Task12; §12 критерії→Task11-12; §14 інваріанти→Task6-7 тести;
  §15 (класифікатор Привату)→свідомо НЕ тут (лише інтерфейс+подвійники Task2).
- **Типи узгоджені:** `RequestStatus`/`RequestResult` (Task1), `FrameClass`/`RejectReason`/
  `Classification` (Task2) — вживаються в Task4-7 як оголошено.
- **Ризик виконавцю:** номери рядків у CMake/run_tests можуть зсунутися — звіряти
  фактичні перед правкою; складні тіла `DeviceSession` пишуться під наведені тести+§14,
  не «на око»; кожна потокова задача — прогін ×20 на флейки.
