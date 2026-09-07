# Життєвий цикл ECR-з'єднання — план імплементації

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Компонента перестає видавати чужий чек за результат перерваної оплати, чесно віддає касі код `UNKNOWN_OUTCOME`=17 із фактами, сама з'ясовує долю операції у фоні після реконекту, і знає різницю між «сокет відкритий» і «термінал підтвердив готовність» (код `RECONNECTING`=18, TCP keepalive).

**Архітектура:** Уся нова логіка — у `EcrPrivatJsonDriver` (бізнес-семантика еквайрингу); `DeviceSession` НЕ змінюється. Стан «доля останньої операції» (`LastOutcome` під `outcomeMutex_`) і стан зв'язку (`LinkState` під `linkMutex_` з епохою) обслуговує один фоновий `JobEngine recoveryJob_`: крок 0 — Ping до готовності, крок 1 — полінг статусу до спокою й `GetReceiptInfo` без підміни результату операції. Платформа отримує дві мінімальні правки: серіалізацію `JobEngine::Start` і TCP keepalive у `TransportTCP`.

**Tech Stack:** C++17, MSVC (VS2022), CMake; `nlohmann/json`; власні харнеси `tests/ecr_privatjson_selftest.cpp` (драйвер напряму + `TerminalEmulator`), `tests/wire_selftest.cpp` (транспорт), `tests/ecr_native_host.cpp` (компонента через головну DLL).

**Spec:** `docs/superpowers/specs/2026-09-05-ecr-connection-lifecycle-design.md`

## Global Constraints

Проєктні правила, що діють у **кожному** завданні (джерела: `AGENTS.md`, `CLAUDE.md`, спека §3):

- **Мова коду й комітів — українська** (дотримуйся мови файлу, який редагуєш). Повідомлення складай конкатенацією, з великої літери, без крапки в кінці.
- **PCH:** `#include "../../core/pch.h"` першим рядком у кожному `.cpp` під `src/`; у `.h` — ніколи. Файли в `tests/` pch **не** підключають.
- **Логування — лише макросами** з `ServiceTools.h`: `NEUTRAL_REPORT_*` у статичних/const-контекстах (1-й аргумент — ім'я компоненти рядком), `REPORT_*` у нестатичних методах компоненти. Прямий `spdlog` заборонено. printf-стиль у макросах заборонено.
- **Помилки:** після `REPORT_ERROR` — `return false`; бізнес-помилки не кидати `throw`; код, що може кинути, обгортати `try-catch`.
- **`DeviceSession` не змінюється** (спека §3, принцип архітектора): ні `desync` при обриві, ні нові хуки, ні зміни `MarkSynchronized()`.
- **Коди 0..16 не рухаються.** Нові — `UNKNOWN_OUTCOME`=17, `RECONNECTING`=18, і тільки символьні (числова гілка `CodeToInt` віддала б цифру за код термінала).
- **Компонента не вгадує про гроші:** жодного зіставлення чека з наміром, жодного автоповтору/сторно, жодного запису фактів чужого чека в OUT-параметри БПО.
- **Під `outcomeMutex_`/`linkMutex_` — жодного мережевого виклику й жодного `EmitEvent`** (той самий стиль, що `eventMutex_`).
- **`min`/`max` не використовувати** (windows.h визначає їх макросами — тихо ламає збірку): писати тернарний вибір.
- **Гейт кожного завдання — зелений на x64 і x86.** Швидкий цикл:
  ```
  cmake -S . -B build_x64 -A x64 -DBUILD_TESTS=ON
  cmake --build build_x64 --config Release --target ecr_privatjson_selftest
  bin\Release\ecr_privatjson_selftest_x64.exe
  ```
  Повний гейт: `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests`, потім
  `powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64` і те саме `x86`.
  **`run_tests.ps1` не перезбирає, якщо exe вже є** — після правки коду збирай сам.
- **Негативна верифікація обов'язкова** (спека §6.4): кожен новий CHECK хоч раз побачений червоним на старій/дефектній поведінці. Тест, що не падає на дефекті, — не тест.

---

## Структура файлів

**Нових файлів немає — усе зміни наявних.** Межі відповідальності:

| Файл | Відповідальність після змін |
|---|---|
| `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h` | типи `OutcomeState`/`OperationIntent`/`LastOutcome`/`LinkState`, поля стану під двома м'ютексами, публічні `InquireLastOutcome`/`SetRequestId`/`IsReady`, приватні хелпери відновлення |
| `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp` | вся нова логіка: тригер, гейт, фоновий джоб (Ping+з'ясування), стан зв'язку, життєвий цикл |
| `src/platform/JobEngine.h/.cpp` | `startMutex_` — `Start()` безпечний для двох викликачів |
| `src/transport/Transport_TCP.h/.cpp` | TCP keepalive завжди + тестовий шов `GetSocketForTest()` |
| `src/components/BpoFacadeBase.cpp` | таксономія кодів: 17, 18 |
| `src/drivers/IAcquiringDriver.h` | `InquireLastOutcome()` (дефолт «не вмію»), `SetRequestId()` (дефолт no-op) |
| `src/drivers/ecr_privatjson/EcrPrivatJsonAcquiring.h/.cpp` | прохід обох нових методів у `drv_` |
| `src/components/AcquiringFacadeBase.cpp` | реєстрація `ИсходПоследнейОперацииJSON` і `УстановитьИдентификаторЗапроса` у БПО-фасаді |
| `src/components/AddinECRPrivatJSON.cpp` | ті самі два методи прямого API; `Подключен` → `IsReady()` |
| `tests/support/TerminalEmulator.h/.cpp` | `DropConnection()` — обрив з боку сценарію |
| `tests/ecr_privatjson_selftest.cpp` | сценарії §6.2 №1-17, 19-25 |
| `tests/wire_selftest.cpp` | сценарій №18 (keepalive) |
| `tests/ecr_native_host.cpp` | e2e §6.3 через головну DLL |
| `docs/architecture/ecrprivatjson.md`, `docs/architecture/device-core.md`, `docs/architecture/bpo-contract.md`, `docs/integration-1c/ecr-privatjson.md` | документація за §5 і §9 спеки |

**Наскрізні сигнатури** (визначаються в Task 2-4, споживаються далі — повний перелік, щоб виконавець будь-якого завдання бачив імена сусідів):

```cpp
// EcrPrivatJsonDriver.h, файловий рівень (поруч з EcrConnParams)
enum class OutcomeState { None, Pending, Resolved };
enum class LinkState    { Disconnected, Connecting, Ready };

struct OperationIntent {
    std::string method;      // "Purchase" | "Refund"
    std::string amount;      // як пішло на дріт
    std::string rrn;         // для Refund; інакше порожньо
    std::string requestId;   // ИдентификаторЗапроса з 1С, прозорий
    std::chrono::system_clock::time_point startedAt;
};

struct LastOutcome {
    OutcomeState    state = OutcomeState::None;
    std::uint64_t   generation = 0;
    OperationIntent intent;
    std::string     reason;            // TIMEOUT|DISCONNECTED|SEND_FAILED|STOPPED|DESYNC
    bool            terminalIdle = false;
    ResultEnvelope  facts;
};

// EcrPrivatJsonDriver — публічне
ResultEnvelope InquireLastOutcome();          // НЕ const: самозцілення мутує recoveryJob_
void           SetRequestId(std::string id);
bool           IsReady() const;               // linkState_ == Ready

// EcrPrivatJsonDriver — приватне
static bool       IsFinancial(const std::string& method);
std::uint64_t     MarkPending(const OperationIntent& intent, const std::string& reason);
nlohmann::json    OutcomeSnapshotJson();      // об'єкт payload.outcome
ResultEnvelope    BuildUnknownOutcome();      // конверт коду 17 зі знімком
void              WaitOutcomeBounded(std::uint64_t generation);
void              SleepInterruptible(int ms);   // сон, перериваний closing_
void              EnsureRecoveryRunning();
ResultEnvelope    RecoveryJob(std::uint64_t generation);
ResultEnvelope    CaptureOutcome(std::uint64_t generation);
ResultEnvelope    RequestReceiptFacts(const std::string& invoiceNumber);
RequestStatus     PollStatusOnce(int& code);
bool              EnsureReady();
void              SetLinkState(LinkState target, const char* reason, std::uint64_t epoch = 0);
std::uint64_t     LinkEpoch() const;

// IAcquiringDriver — дефолти
virtual ResultEnvelope InquireLastOutcome() { return AcquiringUnsupported("Доля останньої операції"); }
virtual void           SetRequestId(const std::string&) {}
```

---

### Task 1: Чужий чек більше не видається за результат операції

**Окремий PR, самодостатній** (спека §8). Наявні 84 CHECK `ecr_privatjson_selftest` і 77 CHECK
`ecr_native_host` не зачіпаються: тестів на `RecoverAfterDesync`/desync у них немає (спека §2.4).

⚠️ **Гонка, яку це завдання свідомо лишає.** Після `Timeout` primary `DoRequest` ставить
`desynchronized_`+`reconnectRequested_` (`DeviceSession.cpp:217-222`), а супервізор прокидається й
**негайно, без backoff**, робить `FinishPendingLocked(Disconnected); connected_ = false` і далі
`Close→Open` (`:436-447`). Тобто «зв'язок живий після Timeout» — ілюзія тривалістю в планувальник ОС:
синхронний `RecoverAfterDesync` або встигає на дріт, або отримує `Disconnected`. Це недетермінізм
**наявного коду**, не тесту. Для окремого PR прийнятно — дефект підміни закрито; детерміноване
з'ясування приходить із джобом у Task 4, тому тест нижче перевіряє **відсутність підміни**, а не
наявність чека.

**Files:**
- Modify: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h` (оголошення `RequestReceiptFacts`)
- Modify: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp:318-354` (гілка відновлення, `RecoverAfterDesync`)
- Modify: `src/components/BpoFacadeBase.cpp:31-54` (`CodeToInt`)
- Test: `tests/ecr_privatjson_selftest.cpp` (новий `TestForeignReceiptNotCredited`)

**Interfaces:**
- Consumes: наявні `EcrJsonCodec::BuildRequest`, `DeviceSession::RequestPrimary`, `MapResult`, `GateSend`.
- Produces: `ResultEnvelope RequestReceiptFacts(const std::string& invoiceNumber)` — приватний вузький
  виклик `GetReceiptInfo` **без** `EmitEvent`, без дотику до `lastStatus_`/`interruptSent_`/`job_`;
  таймаут `kOperationTimeoutMs`. Форма `payload.outcome` — рівно п'ять ключів:
  `{state, reason, facts, factsOk, factsCode}` (Task 2 їх лише доповнює).

- [ ] **Step 1: Написати тест, що падає**

У `tests/ecr_privatjson_selftest.cpp` перед `TestFacadeSmoke` додати:

```cpp
// Дефект 1.2 спеки: після desync драйвер віддавав результат ЧУЖОГО чека як результат
// операції. Емулятор: Purchase мовчить (-> Timeout+desync), GetReceiptInfo віддає завідомо
// іншу суму/RRN. Каса має отримати код 17, а не «успішну» чужу транзакцію.
static void TestForeignReceiptNotCredited() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    // Purchase не відповідає ніколи -> primary Timeout при живому сокеті -> desync.
    emu.OnRequest("Purchase", [](const nlohmann::json&)->std::string{ return ""; });
    // Чужий чек: інша сума, інший RRN - саме він раніше видавався за наш результат.
    emu.OnRequest("GetReceiptInfo", [](const nlohmann::json&){
        return R"({"method":"GetReceiptInfo","params":{"responseCode":"0000","invoiceNumber":"11","rrn":"999000111","amount":"777.77"},"error":false})";
    });
    CHECK(emu.Start(), "ForeignReceipt: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "ForeignReceipt: Connect");

    // Рахуємо події: рівно ОДНА подія result на операцію, і в ній немає чужого payload.
    std::atomic<int> resultEvents{ 0 };
    std::string lastResultData;
    std::mutex evMutex;
    drv.SetEventHandler([&](const std::string& ev, const std::string& data) {
        if (ev != "result") return;
        resultEvents.fetch_add(1);
        std::lock_guard<std::mutex> lk(evMutex);
        lastResultData = data;
    });

    ResultEnvelope env = drv.Execute("Purchase",
        nlohmann::json{{"amount","100.51"},{"discount",""},{"merchantId","0"},{"facepay","false"}}, 2000);

    CHECK(!env.ok && env.code == "UNKNOWN_OUTCOME", "ForeignReceipt: операція -> ok=false, code=UNKNOWN_OUTCOME");
    CHECK(env.payload.contains("outcome") && env.payload["outcome"].is_object(),
          "ForeignReceipt: payload.outcome присутній");
    const auto& oc = env.payload["outcome"];
    // reason за таблицею 4.2: статус Timeout має пріоритет над IsDesynchronized(), тож "TIMEOUT".
    // Значення навмисно збігається з тим, яке дасть повна таблиця в Task 2 — тест не переписується.
    const std::string state = oc.value("state", std::string{});
    CHECK((state == "resolved" || state == "pending") && oc.value("reason", std::string{}) == "TIMEOUT",
          "ForeignReceipt: outcome.state заповнено, outcome.reason=TIMEOUT");
    // Гонка з супервізором (див. шапку завдання): факти - АБО чужий чек (запит виграв гонку),
    // АБО невдача запиту. Обидва виходи легітимні; тест перевіряє не їх, а відсутність підміни.
    const bool gotForeign = oc["facts"].is_object() && oc["facts"].value("rrn", std::string{}) == "999000111";
    const bool gotFailure = oc.value("factsOk", true) == false;
    CHECK(gotForeign || gotFailure,
          "ForeignReceipt: outcome.facts описує САМЕ запит чека (чужий чек або чесна невдача)");
    if (gotForeign)
        CHECK(oc.value("factsOk", false) == true && oc.value("factsCode", std::string{}) == "0000",
              "ForeignReceipt: чужий чек прийшов - factsOk/factsCode це відображають");
    // Головне: жодне поле чужого чека НЕ на топ-рівні payload - там раніше стояли rrn/amount.
    CHECK(!env.payload.contains("rrn") && !env.payload.contains("amount") && !env.payload.contains("invoiceNumber"),
          "ForeignReceipt: поля чужого чека НЕ на топ-рівні payload операції");
    CHECK(resultEvents.load() == 1, "ForeignReceipt: рівно одна подія result на операцію");
    {
        std::lock_guard<std::mutex> lk(evMutex);
        auto j = nlohmann::json::parse(lastResultData, nullptr, false);
        CHECK(!j.is_discarded() && j.value("code", std::string{}) == "UNKNOWN_OUTCOME",
              "ForeignReceipt: подія result несе код 17, не чужий чек");
    }
    drv.Disconnect();
    emu.Stop();
}
```

І в `main()` — рядок `TestForeignReceiptNotCredited();` після `TestDriverCancelNoWedge();`.

- [ ] **Step 2: Запустити тест — переконатися, що падає**

```
cmake -S . -B build_x64 -A x64 -DBUILD_TESTS=ON
cmake --build build_x64 --config Release --target ecr_privatjson_selftest
bin\Release\ecr_privatjson_selftest_x64.exe
```

Очікування: FAIL на `ForeignReceipt: операція -> ok=false, code=UNKNOWN_OUTCOME` (сьогодні код `0000`
і `ok=true` — чужий чек), FAIL на топ-рівневих полях і на `resultEvents == 1` (сьогодні їх дві).

- [ ] **Step 3: Додати `RequestReceiptFacts` — вузький виклик без подій**

У `EcrPrivatJsonDriver.h`, у приватну секцію поруч із `RecoverAfterDesync`:

```cpp
    /// Вузький запит чека для з'ясування долі операції: БЕЗ EmitEvent, без дотику до
    /// lastStatus_/interruptSent_/job_. Публічний GetReceiptInfo (ПолучитьЧек) лишається як є.
    ResultEnvelope RequestReceiptFacts(const std::string& invoiceNumber);
```

У `EcrPrivatJsonDriver.cpp` — після визначення `RecoverAfterDesync`:

```cpp
ResultEnvelope EcrPrivatJsonDriver::RequestReceiptFacts(const std::string& invoiceNumber) {
    if (!session_) return ResultEnvelope::Fail("NOT_CONNECTED", "Термінал не підключено");
    // Таймаут операційний, не хендшейковий: під авторизацією на хості 5с не вистачає (спека §1.3).
    auto req = EcrJsonCodec::BuildRequest("GetReceiptInfo", 0,
                                          nlohmann::json{{"invoiceNumber", invoiceNumber}});
    GateSend();
    RequestResult r = session_->RequestPrimary(req, kOperationTimeoutMs);
    return MapResult(r);
}
```

- [ ] **Step 4: Перевести гілку відновлення на код 17**

У `EcrPrivatJsonDriver.cpp`, у безіменний `namespace` вгорі файлу:

```cpp
// Знімок долі операції для 1С. П'ять ключів (спека §6.2 №1); Task 2 їх доповнює,
// не переписує. facts — сирі поля §5.30 як є, порожній об'єкт при невдалому запиті.
nlohmann::json OutcomePayload(const ResultEnvelope& facts, const char* reason) {
    return nlohmann::json{{"outcome", nlohmann::json{
        {"state",     "resolved"},
        {"reason",    reason},
        {"facts",     facts.payload},
        {"factsOk",   facts.ok},
        {"factsCode", facts.code}}}};
}
```

`RecoverAfterDesync` — останній рядок замінити (чек тепер іде через вузький виклик):

```cpp
    if (session_) session_->MarkSynchronized();
    // best-effort: деталі останнього чека. НЕ через ExecuteInternal - той емітить події
    // й підмінив би результат операції чужим чеком (дефект 1.2 спеки).
    return RequestReceiptFacts(std::string{});
```

`ExecuteInternal` — гілка відновлення:

```cpp
    ResultEnvelope env;
    if (r.status == RequestStatus::Timeout && session_ && session_->IsDesynchronized() && !inRecovery_.load()) {
        inRecovery_.store(true);
        // Результат з'ясування - це ФАКТИ ПРО, можливо, ЧУЖИЙ чек, а не результат нашої
        // операції: GetReceiptInfo("") віддає останній чек у пакеті, ким би він не був
        // започаткований (спека §1.2). Каса отримує чесне «не знаю» + факти окремим полем.
        const ResultEnvelope facts = RecoverAfterDesync();
        inRecovery_.store(false);
        env = ResultEnvelope::Fail("UNKNOWN_OUTCOME",
            "Зв'язок із терміналом обірвався під час операції; доля невідома");
        env.payload = OutcomePayload(facts, "TIMEOUT");   // таблиця 4.2: статус Timeout > desync
    } else {
        env = MapResult(r);
    }
```

Рекурсія `RecoverAfterDesync -> ExecuteInternal` зникла, але `inRecovery_` лишається до Task 4
(там він прибирається разом із переїздом на `recoveryJob_`).

- [ ] **Step 5: Додати код 17 у таксономію**

`src/components/BpoFacadeBase.cpp`, у `CodeToInt` перед `return -1;`:

```cpp
    if (code == "UNKNOWN_OUTCOME")     return 17;
```

- [ ] **Step 6: Запустити тести — мають пройти**

```
cmake --build build_x64 --config Release --target ecr_privatjson_selftest
bin\Release\ecr_privatjson_selftest_x64.exe
```

Очікування: PASS усіх нових CHECK; наявні 84 CHECK лишаються PASS (`OK` в кінці, exit 0).

- [ ] **Step 7: Повний гейт x64 і x86**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```

Очікування: підсумкова таблиця без FAIL/BLOCKED, exit 0 в обох прогонах.

- [ ] **Step 8: Коміт**

```bash
git add src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp src/components/BpoFacadeBase.cpp tests/ecr_privatjson_selftest.cpp
git commit -m "fix(ecr): чужий чек більше не видається за результат операції (код 17)"
```

---

### Task 2: Стан «доля останньої операції», тригер і наскрізний `requestId`

Фонового джоба ще немає: при живому з'єднанні з'ясування лишається **синхронним** (як у Task 1),
але результат тепер лягає в `lastOutcome_`, а касі йде знімок. Task 4 замінить синхронний блок на
`EnsureRecoveryRunning()` + `WaitOutcomeBounded()`.

**Files:**
- Modify: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h` (типи, поля, нові методи)
- Modify: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp` (`IsoUtc`, `PollStatusOnce`, `MarkPending`,
  `OutcomeSnapshotJson`, `BuildUnknownOutcome`, `SetRequestId`, `InquireLastOutcome`, тригер 4.2)
- Test: `tests/ecr_privatjson_selftest.cpp` (`TestNonFinancialNotTracked` №10, `TestInquireNone` №11)

**Interfaces:**
- Consumes: `RequestReceiptFacts` (Task 1), `MapResult`, `GateSend`, `EcrJsonCodec`.
- Produces: типи `OutcomeState`/`OperationIntent`/`LastOutcome`; `MarkPending(intent, reason) -> generation`;
  `OutcomeSnapshotJson()`; `BuildUnknownOutcome()`; `SetRequestId(std::string)`; `InquireLastOutcome()`;
  `RequestStatus PollStatusOnce(int& code)` — `code` валідний **лише** при `Response`, викликач сам
  вирішує, що робити з `Timeout` (продовжити) і `Disconnected`/`Stopped` (вийти).

- [ ] **Step 1: Написати тести, що падають**

У `tests/ecr_privatjson_selftest.cpp`:

```cpp
// №10: нефінансовий метод при збої НЕ створює наміру - каса нічого не з'ясовує.
static void TestNonFinancialNotTracked() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    emu.OnRequest("Audit", [](const nlohmann::json&)->std::string{ return ""; });   // мовчить -> Timeout
    CHECK(emu.Start(), "NonFinancial: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "NonFinancial: Connect");

    ResultEnvelope env = drv.Execute("Audit", nlohmann::json{{"merchantId","0"}}, 1500);
    CHECK(!env.ok && env.code == "TIMEOUT", "NonFinancial: Audit при мовчанні -> TIMEOUT, не 17");

    ResultEnvelope oc = drv.InquireLastOutcome();
    CHECK(oc.ok && oc.code == "OK" && oc.payload["outcome"].value("state", std::string{}) == "none",
          "NonFinancial: намір не створено (state=none)");
    drv.Disconnect();
    emu.Stop();
}

// №11: ИсходПоследнейОперацииJSON без жодної операції - не помилка, а «нема про що питати».
static void TestInquireNone() {
    EcrPrivatJsonDriver drv;                       // навіть без Connect
    ResultEnvelope env = drv.InquireLastOutcome();
    CHECK(env.ok && env.code == "OK", "InquireNone: ok=true, code=OK");
    CHECK(env.payload.contains("outcome") &&
          env.payload["outcome"].value("state", std::string{}) == "none",
          "InquireNone: outcome.state=none");
    CHECK(env.payload["outcome"].value("channelConnected", true) == false,
          "InquireNone: channelConnected=false без сесії");
}
```

У `main()` — `TestNonFinancialNotTracked();` і `TestInquireNone();` після `TestForeignReceiptNotCredited();`.

- [ ] **Step 2: Запустити — переконатися, що не компілюється / падає**

```
cmake --build build_x64 --config Release --target ecr_privatjson_selftest
```

Очікування: помилка компіляції `InquireLastOutcome: is not a member of EcrPrivatJsonDriver` — метод
ще не існує. Це і є «червоне» для цього кроку.

- [ ] **Step 3: Оголосити типи й поля стану**

`EcrPrivatJsonDriver.h` — після `struct EcrConnParams`, перед класом:

```cpp
/// Доля останньої фінансової операції (спека §4.1). None - питання не стояло;
/// Pending - з'ясовуємо; Resolved - знімок готовий (успіх/невдача з'ясування читається
/// з facts.ok/facts.code, окремий стан для цього зайвий).
enum class OutcomeState { None, Pending, Resolved };

/// Намір каси: що саме ми відправляли на дріт, коли зв'язок обірвався.
struct OperationIntent {
    std::string method;      ///< "Purchase" | "Refund"
    std::string amount;      ///< рядком, як пішло на дріт (MoneyToString)
    std::string rrn;         ///< для Refund; інакше порожньо
    std::string requestId;   ///< ИдентификаторЗапроса з 1С, прозорий; "" якщо не задано
    std::chrono::system_clock::time_point startedAt{};
};

struct LastOutcome {
    OutcomeState    state = OutcomeState::None;
    std::uint64_t   generation = 0;    ///< ++ на кожен перехід у Pending; ідентифікатор питання для 1С
    OperationIntent intent;
    std::string     reason;            ///< TIMEOUT|DISCONNECTED|SEND_FAILED|STOPPED|DESYNC
    bool            terminalIdle = false;
    ResultEnvelope  facts;             ///< результат RequestReceiptFacts (поля §5.30)
};
```

У публічну секцію класу (поруч з асинхронним API):

```cpp
    /// Знімок долі останньої операції для 1С. Мережею не ходить, працює в будь-якому стані.
    /// НЕ const: Task 4 додасть сюди самозцілення (мутує recoveryJob_).
    ResultEnvelope InquireLastOutcome();

    /// ИдентификаторЗапроса каси, що пройде у знімок прозорим рядком. Забирається
    /// одноразово на вході наступної фінансової операції.
    void SetRequestId(std::string id);
```

У приватну секцію:

```cpp
    static bool IsFinancial(const std::string& method);   ///< kFinancialMethods = {Purchase, Refund}

    /// Зафіксувати намір: state=Pending, generation++, reason. Повертає нове покоління.
    std::uint64_t MarkPending(const OperationIntent& intent, const std::string& reason);
    /// Об'єкт payload.outcome (спека §4.7). Бере outcomeMutex_; мережею не ходить.
    nlohmann::json OutcomeSnapshotJson();
    /// Конверт коду 17 зі знімком усередині.
    ResultEnvelope BuildUnknownOutcome();

    /// Один цикл getLastStatMsgCode на service-доріжці. Спільний для PollerLoop і
    /// з'ясування долі. code валідний ЛИШЕ при Response; інакше не чіпається.
    RequestStatus PollStatusOnce(int& code);

    mutable std::mutex outcomeMutex_;   ///< lastOutcome_ + pendingRequestId_
    LastOutcome        lastOutcome_;
    std::string        pendingRequestId_;
```

- [ ] **Step 4: Реалізувати знімок, намір і `PollStatusOnce`**

`EcrPrivatJsonDriver.cpp` — у безіменний `namespace` (замість `OutcomePayload` з Task 1, він
прибирається):

```cpp
// ISO 8601 UTC із мілісекундами. Дефолтний time_point (наміру не було) -> порожній рядок,
// щоб 1С не бачила фальшиве "1970-01-01".
std::string IsoUtc(std::chrono::system_clock::time_point tp) {
    if (tp == std::chrono::system_clock::time_point{}) return std::string{};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tmv{};
    gmtime_s(&tmv, &t);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms.count()));
    return std::string(buf);
}

const char* OutcomeStateName(OutcomeState s) {
    switch (s) {
        case OutcomeState::Pending:  return "pending";
        case OutcomeState::Resolved: return "resolved";
        default:                     return "none";
    }
}
```

Далі — реалізації:

```cpp
bool EcrPrivatJsonDriver::IsFinancial(const std::string& method) {
    return method == "Purchase" || method == "Refund";
}

void EcrPrivatJsonDriver::SetRequestId(std::string id) {
    std::lock_guard<std::mutex> lk(outcomeMutex_);
    pendingRequestId_ = std::move(id);
}

std::uint64_t EcrPrivatJsonDriver::MarkPending(const OperationIntent& intent, const std::string& reason) {
    std::lock_guard<std::mutex> lk(outcomeMutex_);
    const std::uint64_t gen = lastOutcome_.generation + 1;   // ідентифікатор питання для 1С
    lastOutcome_ = LastOutcome{};
    lastOutcome_.state      = OutcomeState::Pending;
    lastOutcome_.generation = gen;
    lastOutcome_.intent     = intent;
    lastOutcome_.reason     = reason;
    return gen;
}

nlohmann::json EcrPrivatJsonDriver::OutcomeSnapshotJson() {
    const bool channel = IsConnected();   // ПОЗА локом: чіпає сесію, не lastOutcome_
    std::lock_guard<std::mutex> lk(outcomeMutex_);
    return nlohmann::json{
        {"state",      OutcomeStateName(lastOutcome_.state)},
        {"generation", lastOutcome_.generation},
        {"reason",     lastOutcome_.reason},
        {"intent", nlohmann::json{
            {"method",    lastOutcome_.intent.method},
            {"amount",    lastOutcome_.intent.amount},
            {"rrn",       lastOutcome_.intent.rrn},
            {"requestId", lastOutcome_.intent.requestId},
            {"startedAt", IsoUtc(lastOutcome_.intent.startedAt)}}},
        {"terminalIdle", lastOutcome_.terminalIdle},
        // facts - сирі поля §5.30 як є; null, поки доля не з'ясована.
        {"facts", lastOutcome_.state == OutcomeState::Resolved
                      ? lastOutcome_.facts.payload : nlohmann::json(nullptr)},
        {"factsOk",   lastOutcome_.facts.ok},
        {"factsCode", lastOutcome_.facts.code},
        {"channelConnected", channel}};
}

ResultEnvelope EcrPrivatJsonDriver::BuildUnknownOutcome() {
    ResultEnvelope env = ResultEnvelope::Fail("UNKNOWN_OUTCOME",
        "Зв'язок із терміналом обірвався під час операції; доля невідома");
    env.payload = nlohmann::json{{"outcome", OutcomeSnapshotJson()}};
    return env;
}

ResultEnvelope EcrPrivatJsonDriver::InquireLastOutcome() {
    // Task 4 додасть тут EnsureRecoveryRunning() - самозцілення (спека §4.4).
    bool none;
    { std::lock_guard<std::mutex> lk(outcomeMutex_); none = lastOutcome_.state == OutcomeState::None; }
    if (!none) return BuildUnknownOutcome();
    ResultEnvelope env = ResultEnvelope::Ok();
    env.payload = nlohmann::json{{"outcome", OutcomeSnapshotJson()}};
    return env;
}

RequestStatus EcrPrivatJsonDriver::PollStatusOnce(int& code) {
    if (!session_) return RequestStatus::Disconnected;
    auto stat = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType", "getLastStatMsgCode"}});
    GateSend();
    RequestResult s = session_->RequestService(stat, kServiceTimeoutMs);
    if (s.status != RequestStatus::Response) return s.status;
    ParsedResponse pr = EcrJsonCodec::Parse(s.frame);
    if (pr.valid && pr.params.is_object()) {
        const std::string c = pr.params.value("LastStatMsgCode", std::string{});
        if (!c.empty()) { try { code = std::stoi(c); } catch (...) {} }
    }
    return s.status;
}
```

- [ ] **Step 5: Перевести `PollerLoop` і `RecoverAfterDesync` на `PollStatusOnce`**

`PollerLoop` — блок запиту статусу замінити (подію емітить викликач, а не хелпер):

```cpp
        int code = -1;
        if (PollStatusOnce(code) == RequestStatus::Response && code >= 0) {
            if (code != lastStatus_.exchange(code))   // статус змінився -> подія в 1С
                EmitEvent("status", { {"code", code}, {"text", StatusText(code)}, {"state", "Running"} });
        }
```

`RecoverAfterDesync` — цикл полінгу так само:

```cpp
    for (int i = 0; i < kRecoverPollTries && session_; ++i) {
        int code = -1;
        if (PollStatusOnce(code) == RequestStatus::Response && code >= 0) {
            lastStatus_.store(code);                 // без події: це фон, не хід операції
            if (code == 0) break;                    // термінал у спокої (спека §6.5)
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
```

- [ ] **Step 6: Замінити гілку Task 1 на повний тригер 4.2**

`ExecuteInternal` — на початку тіла, після наявної перевірки `IsConnected()`:

```cpp
    const bool financial = IsFinancial(method);
    // requestId забирається ОДНОРАЗОВО на вході фінансової операції, під тим самим
    // м'ютексом, що й SetRequestId (спека §4.7): інакше id прилип би до наступної операції.
    std::string requestId;
    if (financial) {
        std::lock_guard<std::mutex> lk(outcomeMutex_);
        requestId = std::exchange(pendingRequestId_, std::string{});
    }
    const auto startedAt = std::chrono::system_clock::now();
```

Гілку `if (r.status == RequestStatus::Timeout && …)` замінити повністю:

```cpp
    // Тригер «доля невідома» (спека §4.2): чотири статуси АБО desync. Порядок перевірки
    // саме такий - статус має пріоритет над desync. Busy/Unsupported/Concurrent без
    // desync відомі однозначно й тригером не є.
    const bool desync = session_ && session_->IsDesynchronized();
    const char* reason = nullptr;
    switch (r.status) {
        case RequestStatus::Timeout:      reason = "TIMEOUT";      break;
        case RequestStatus::Disconnected: reason = "DISCONNECTED"; break;
        case RequestStatus::SendFailed:   reason = "SEND_FAILED";  break;
        case RequestStatus::Stopped:      reason = "STOPPED";      break;
        default:                          reason = desync ? "DESYNC" : nullptr; break;
    }

    ResultEnvelope env;
    if (financial && reason) {
        OperationIntent intent;
        intent.method    = method;
        intent.amount    = params.is_object() ? params.value("amount", std::string{}) : std::string{};
        intent.rrn       = params.is_object() ? params.value("rrn", std::string{}) : std::string{};
        intent.requestId = requestId;
        intent.startedAt = startedAt;
        const std::uint64_t gen = MarkPending(intent, reason);

        // ТИМЧАСОВО (до Task 4): при живому з'єднанні з'ясовуємо синхронно. Task 4
        // замінює цей блок на EnsureRecoveryRunning() + WaitOutcomeBounded(gen).
        if (IsConnected() && !inRecovery_.load()) {
            inRecovery_.store(true);
            const ResultEnvelope facts = RecoverAfterDesync();
            inRecovery_.store(false);
            std::lock_guard<std::mutex> lk(outcomeMutex_);
            if (lastOutcome_.generation == gen) {
                lastOutcome_.state        = OutcomeState::Resolved;
                lastOutcome_.terminalIdle = (lastStatus_.load() == 0);
                lastOutcome_.facts        = facts;
            }
        }
        env = BuildUnknownOutcome();
    } else {
        env = MapResult(r);
    }
```

`SendFailed` трактується так само консервативно, як `Disconnected`: `ITransport` не гарантує, що при
`Send<0` на дріт нічого не пішло (спека §4.2).

- [ ] **Step 7: Запустити тести — мають пройти**

```
cmake --build build_x64 --config Release --target ecr_privatjson_selftest
bin\Release\ecr_privatjson_selftest_x64.exe
```

Очікування: PASS нових CHECK; `TestForeignReceiptNotCredited` (Task 1) лишається PASS — форма знімка
розширилась, але його п'ять ключів на місці й `reason` той самий (`TIMEOUT`).

- [ ] **Step 8: Негативна верифікація тригера**

Тимчасово звузити тригер назад до `r.status == RequestStatus::Timeout && desync` і запустити
`TestForeignReceiptNotCredited` — має лишитись зеленим (сценарій той самий), а `TestNonFinancialNotTracked`
навпаки: тимчасово додати `"Audit"` у `IsFinancial` і переконатися, що CHECK «намір не створено» падає.
Повернути обидві зміни.

- [ ] **Step 9: Повний гейт x64 і x86, коміт**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```

```bash
git add src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp tests/ecr_privatjson_selftest.cpp
git commit -m "feat(ecr): стан долі операції, тригер обриву й наскрізний requestId"
```

---

### Task 3: Стан зв'язку — три стани, епоха, код `RECONNECTING`=18

Фундамент частини Б. Після цього завдання зв'язок після обриву лишається `Connecting`, доки в Task 4
не з'явиться Ping — це очікувано й гейт не ламає (тестів на реконект іще немає).

**Files:**
- Modify: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h` (`LinkState`, `linkMutex_`, `linkState_`,
  `linkEpoch_`, `SetLinkState`, `LinkEpoch`, `IsReady`)
- Modify: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp` (`MakeSession` хук-заглушки, `Connect`
  крок 3, `Disconnect`, порядок перевірок у `ExecuteInternal`)
- Modify: `src/components/BpoFacadeBase.cpp` (`CodeToInt`: 18)
- Modify: `src/components/AddinECRPrivatJSON.cpp` (`Подключен` → `IsReady()`)
- Modify: `tests/support/TerminalEmulator.h/.cpp` (`DropConnection()`, `DropAfterNextResponse()`)
- Test: `tests/ecr_privatjson_selftest.cpp` (`TestLinkStateOnDisconnect` №20, `TestThreeStatesThreeCodes` №22,
  `TestReconnectByHandKeepsReady` №24)

**Interfaces:**
- Consumes: `EmitEvent` (наявний), `DeviceSession::SetConnectionStateHandler` (лише **до** `Start()`).
- Produces: `bool IsReady() const`; `std::uint64_t LinkEpoch() const`;
  `void SetLinkState(LinkState target, const char* reason, std::uint64_t epoch = 0)` — **єдина** точка
  зміни стану зв'язку, емітить подію `connection` поза локом;
  значення `reason`: `"dropped"` (обрив), `"closed"` (ручний `Отключить`), `"connect_failed"` (не вдався
  `Start()`), `"timeout"` (таймаут операції — супервізор зараз перевідкриє канал), `""` (для `ready`);
  `TerminalEmulator::DropConnection()` — розрив активного з'єднання з боку сценарію;
  `TerminalEmulator::DropAfterNextResponse()` — обрив одразу після наступної відповіді (для №23 у Task 4).

- [ ] **Step 1: Додати `DropConnection()` в емулятор**

`tests/support/TerminalEmulator.h`, у публічну секцію:

```cpp
    /// Розірвати активне з'єднання з боку «термінала» (моделювання обриву). Наступний
    /// accept емулятор робить сам - реконект драйвера обслуговується без додаткових дій.
    void DropConnection();
```

`tests/support/TerminalEmulator.cpp`:

```cpp
void TerminalEmulator::DropConnection() {
    SOCKET c = client_.exchange(INVALID_SOCKET);
    if (c == INVALID_SOCKET) return;
    shutdown(c, SD_BOTH);
    closesocket(c);
    // Read-loop у Run() вийде за помилкою recv; його client_.exchange віддасть уже
    // INVALID_SOCKET, тож подвійного closesocket того самого хендла не буде.
}
```

Там само — другий режим обриву, потрібний Task 4 (тест №23); додається тут, щоб уся робота з
емулятором лежала в одному коміті. `.h`, публічно:

```cpp
    /// Закрити з'єднання ОДРАЗУ після відправки наступної відповіді: «термінал відповів
    /// і зник». Без цього сценарій «Ping-відповідь + негайний обрив» недетермінований.
    void DropAfterNextResponse() { dropAfterResponse_.store(true); }
```
приватно: `std::atomic<bool> dropAfterResponse_{ false };`

`.cpp`, у кінець `HandleFrame` — після циклу `send`:

```cpp
    if (dropAfterResponse_.exchange(false)) {
        shutdown(c, SD_SEND);                      // FIN ПІСЛЯ вже відправлених байтів
        SOCKET old = client_.exchange(INVALID_SOCKET);
        if (old != INVALID_SOCKET) closesocket(old);
    }
```

- [ ] **Step 2: Написати тести, що падають**

```cpp
// №20: ручний Отключить під час Connecting -> подія disconnected(closed), не dropped.
static void TestLinkStateOnDisconnect() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&){ return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})"; });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        return "";
    });
    CHECK(emu.Start(), "LinkDisconnect: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::mutex evMutex;
    std::vector<std::pair<std::string, std::string>> conn;   // (state, reason)
    drv.SetEventHandler([&](const std::string& ev, const std::string& data) {
        if (ev != "connection") return;
        auto j = nlohmann::json::parse(data, nullptr, false);
        if (j.is_discarded()) return;
        std::lock_guard<std::mutex> lk(evMutex);
        conn.emplace_back(j.value("state", std::string{}), j.value("reason", std::string{}));
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "LinkDisconnect: Connect");
    CHECK(drv.IsReady(), "LinkDisconnect: після Connect зв'язок Ready");

    emu.DropConnection();
    for (int i = 0; i < 100 && drv.IsReady(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!drv.IsReady(), "LinkDisconnect: після обриву Ready знято");

    drv.Disconnect();
    std::lock_guard<std::mutex> lk(evMutex);
    CHECK(conn.size() >= 3, "LinkDisconnect: події connection надійшли");
    CHECK(conn.front().first == "ready", "LinkDisconnect: перша подія - ready (Connect)");
    bool sawDropped = false, sawClosed = false;
    for (const auto& e : conn) {
        if (e.first == "connecting"   && e.second == "dropped") sawDropped = true;
        if (e.first == "disconnected" && e.second == "closed")  sawClosed  = true;
    }
    CHECK(sawDropped, "LinkDisconnect: обрив -> connecting(dropped)");
    CHECK(sawClosed,  "LinkDisconnect: Отключить -> disconnected(closed), не dropped");
    emu.Stop();
}

// №22: три стани - три різні коди. Без правильного порядку перевірок (а) і (б) дали б 18.
static void TestThreeStatesThreeCodes() {
    TerminalEmulator emu;
    // Після реконекту термінал МОВЧИТЬ на Ping (модель монополії, §2.3): інакше в Task 4
    // фоновий джоб підняв би Ready за ~1 с після обриву, і крок (в) став би флакі -
    // оплата встигала б пройти замість RECONNECTING. Хендшейк кроку 1 Connect() при цьому
    // відповідає нормально: прапорець вмикається вже після успішного Connect.
    std::atomic<bool> silentPing{ false };
    emu.OnRequest("PingDevice", [&silentPing](const nlohmann::json&) -> std::string {
        if (silentPing.load()) return "";
        return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})";
    });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        return "";
    });
    std::atomic<int> purchaseSeen{ 0 };
    emu.OnRequest("Purchase", [&](const nlohmann::json&)->std::string{
        purchaseSeen.fetch_add(1);
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"1"},"error":false})";
    });
    CHECK(emu.Start(), "ThreeStates: емулятор стартував");

    EcrPrivatJsonDriver drv;
    // (а) без жодного Connect
    ResultEnvelope a = drv.Purchase("10.00");
    CHECK(!a.ok && a.code == "NOT_CONNECTED", "ThreeStates(а): Purchase без Connect -> NOT_CONNECTED");

    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "ThreeStates: Connect");
    // (в) під Connecting: рвемо з'єднання й одразу пробуємо платити
    silentPing.store(true);          // термінал ще тримає стару сесію - Ready не повернеться
    emu.DropConnection();
    for (int i = 0; i < 100 && drv.IsReady(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const int before = purchaseSeen.load();
    ResultEnvelope c = drv.Purchase("10.00");
    CHECK(!c.ok && c.code == "RECONNECTING", "ThreeStates(в): Purchase під Connecting -> RECONNECTING");
    CHECK(purchaseSeen.load() == before, "ThreeStates(в): на дріт нічого не пішло");

    // (б) одразу після Отключить
    drv.Disconnect();
    ResultEnvelope b = drv.Purchase("10.00");
    CHECK(!b.ok && b.code == "NOT_CONNECTED", "ThreeStates(б): Purchase після Отключить -> NOT_CONNECTED, не 18");
    emu.Stop();
}
```

```cpp
// №24: повторний Connect() мусить давати Ready. Дефолтна епоха 0 у SetLinkState зробила б
// це мовчазним no-op (linkEpoch_ уже >= 1 після першого Отключить) - Ready не став би,
// хук up=true запустив би зайвий Ping, і перша оплата зміни отримала б CONCURRENT.
static void TestReconnectByHandKeepsReady() {
    TerminalEmulator emu;
    std::atomic<int> pings{ 0 };
    std::atomic<int> purchases{ 0 };
    emu.OnRequest("PingDevice", [&pings](const nlohmann::json&) {
        pings.fetch_add(1);
        return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})";
    });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& q)->std::string{
        auto mt = q.contains("params") ? q["params"].value("msgType","") : std::string{};
        if (mt == "identify") return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode") return R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":"0"},"error":false})";
        return "";
    });
    emu.OnRequest("Purchase", [&purchases](const nlohmann::json&) {
        purchases.fetch_add(1);
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"3"},"error":false})";
    });
    CHECK(emu.Start(), "ReconnectByHand: емулятор стартував");

    EcrPrivatJsonDriver drv;
    const std::string conn = std::string("tcp://127.0.0.1:") + std::to_string(emu.Port());
    CHECK(drv.Connect(conn), "ReconnectByHand: перший Connect");
    drv.Disconnect();
    CHECK(drv.Connect(conn), "ReconnectByHand: другий Connect");
    CHECK(drv.IsReady(), "ReconnectByHand: Подключен=Истина одразу після другого Connect");

    const int pingsAfterConnect = pings.load();      // лише хендшейки кроку 1 (по одному на Connect)
    ResultEnvelope p = drv.Purchase("10.00");
    CHECK(p.ok && p.code == "0000", "ReconnectByHand: перша оплата після Connect проходить");
    CHECK(p.code != "CONCURRENT", "ReconnectByHand: не CONCURRENT");
    CHECK(pings.load() == pingsAfterConnect,
          "ReconnectByHand: на персистентній сесії нуль зайвих PingDevice");
    CHECK(purchases.load() == 1, "ReconnectByHand: оплата дійшла до термінала");
    drv.Disconnect();
    emu.Stop();
}
```

У `main()` — три виклики після `TestInquireNone();`.

- [ ] **Step 3: Запустити — переконатися, що падає**

```
cmake --build build_x64 --config Release --target ecr_privatjson_selftest
```

Очікування: помилка компіляції `IsReady: is not a member` — стану зв'язку ще немає.

- [ ] **Step 4: Оголосити стан зв'язку**

`EcrPrivatJsonDriver.h` — поруч із `OutcomeState`:

```cpp
/// Стан зв'язку з терміналом (спека §4.9.1). НЕ дублює connected_ транспорту:
/// Ready означає «термінал відповів на Ping», а не «сокет відкрився».
enum class LinkState { Disconnected, Connecting, Ready };
```

Публічно:

```cpp
    /// «Термінал підтвердив готовність» - саме це бачить 1С у Подключен (спека §4.9.4).
    /// Внутрішній IsConnected() лишається «сокет відкритий».
    bool IsReady() const;
```

Приватно:

```cpp
    /// Єдина точка зміни стану зв'язку. Пишуть три потоки: dispatcher-хук, фоновий джоб
    /// і потік 1С. reason - для події; epoch береться до участі ЛИШЕ для Ready: Ping зі
    /// вже мертвої епохи не має піднімати прапорець на новому сокеті.
    void SetLinkState(LinkState target, const char* reason, std::uint64_t epoch = 0);
    std::uint64_t LinkEpoch() const;

    mutable std::mutex linkMutex_;
    LinkState          linkState_ = LinkState::Disconnected;   ///< під linkMutex_
    std::uint64_t      linkEpoch_ = 0;                         ///< ++ на кожен вихід із Ready
```

- [ ] **Step 5: Реалізувати `SetLinkState` з епохою**

`EcrPrivatJsonDriver.cpp`, у безіменний `namespace`:

```cpp
const char* LinkStateName(LinkState s) {
    switch (s) {
        case LinkState::Ready:      return "ready";
        case LinkState::Connecting: return "connecting";
        default:                    return "disconnected";
    }
}
```

Реалізації:

```cpp
bool EcrPrivatJsonDriver::IsReady() const {
    std::lock_guard<std::mutex> lk(linkMutex_);
    return linkState_ == LinkState::Ready;
}

std::uint64_t EcrPrivatJsonDriver::LinkEpoch() const {
    std::lock_guard<std::mutex> lk(linkMutex_);
    return linkEpoch_;
}

void EcrPrivatJsonDriver::SetLinkState(LinkState target, const char* reason, std::uint64_t epoch) {
    {
        std::lock_guard<std::mutex> lk(linkMutex_);
        // Ping приніс Ready з епохи, яка вже мертва (сокет упав одразу після відповіді) - ігноруємо:
        // інакше Ready лишився б на мертвому сокеті, і наступний up=true його не полагодив би.
        if (target == LinkState::Ready && epoch != linkEpoch_) return;
        if (linkState_ == target) return;
        if (linkState_ == LinkState::Ready) ++linkEpoch_;   // вихід із Ready = нова епоха
        linkState_ = target;
    }
    EmitEvent("connection", { {"state", LinkStateName(target)}, {"reason", reason ? reason : ""} });
}
```

- [ ] **Step 6: Підключити стан до життєвого циклу**

`MakeSession` — заглушки хуків із поясненням замість «Частина 2»:

```cpp
    // deviceBusy без нашого запиту - лише пізній дубль на таймаутнутий запит (протокол §6.1);
    // інших самостійних нотифікацій протокол не документує. Логуємо, щоб wire-трасування
    // показувало, що прийшло без запиту.
    s->SetUnsolicitedHandler([](std::vector<uint8_t> frame) {
        NEUTRAL_REPORT_DEBUG("ECRPrivatJSON",
            "Кадр без запиту: " + std::string(frame.begin(), frame.end()));
    });
    // Стан зв'язку слухає ЛИШЕ персистентна сесія - хук їй ставить Connect() (крок 3).
    // Короткоживучі hs/id його не мають: на момент їх створення session_ уже обнулено.
    s->SetConnectionStateHandler([](bool) {});
```

`Connect()` — крок 3 (заміна блоку `session_ = MakeSession(params_); …`):

```cpp
    // 3) Постійний режим: сесія лишається відкритою (реконект - супервізор DeviceSession).
    session_ = MakeSession(params_);
    session_->SetConnectionStateHandler([this](bool up) {
        // Хук іде на dispatcher-потоці сесії: сигналізуємо й повертаємось, у мережу не ходимо
        // (блокуючий виклик звідси заморозив би dispatcher - спека §2.1).
        if (up) return;                                    // Task 4: EnsureRecoveryRunning()
        SetLinkState(LinkState::Connecting, "dropped");
    });
    // Ready ставимо ДО Start(): хендшейк-Ping пройшов секунду тому (крок 1), а хук up=true
    // після Start уже стоїть у черзі dispatcher-а. Якби Ready ставився після Start(), хук
    // побачив би Connecting і Task 4 запустив би зайвий Ping рівно тоді, коли 1С після
    // Подключить одразу кличе Оплату -> CONCURRENT на першій оплаті зміни (спека §4.9.1).
    //
    // ⚠️ З ЯВНОЮ ЕПОХОЮ. Дефолтна 0 тут не годиться: Connect() починається з Disconnect(),
    // після якого linkEpoch_ >= 1, і SetLinkState(Ready, "") мовчки відкинувся б - Ready не
    // став би на КОЖНОМУ повторному Connect(). Між LinkEpoch() і записом хук нової сесії
    // спрацювати не може: Start() ще не викликано.
    SetLinkState(LinkState::Ready, "", LinkEpoch());
    if (!session_->Start()) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Постійний режим: не вдалося відкрити зв'язок");
        // Не "closed": це аварія підключення, а не прохання каси відключитись (спека §4.9.3).
        SetLinkState(LinkState::Disconnected, "connect_failed");
        session_.reset();
        return false;
    }
    return true;
```

`Disconnect()` — після `session_.reset()` / гілки no-op додати:

```cpp
    SetLinkState(LinkState::Disconnected, "closed");   // ручний розрив - не аварія
```

- [ ] **Step 6-біс: Timeout/desync переводить зв'язок у `Connecting`**

У гілці тригера (Task 2, крок 6) — **перед** `MarkPending`:

```cpp
        // Після Timeout primary супервізор DeviceSession негайно робить connected_=false і
        // Close->Open (DeviceSession.cpp:436-447), навіть якщо TCP був живий. Отже «зв'язок є»
        // тут - гонка; чесний стан - Connecting, а знімок зробить джоб після реконекту (крок 0).
        if (r.status == RequestStatus::Timeout || desync)
            SetLinkState(LinkState::Connecting, "timeout");
```

- [ ] **Step 7: Порядок перевірок на вході `ExecuteInternal`**

Замінити наявний перший рядок `if (!IsConnected()) …`:

```cpp
    // Три різні стани - три різні коди, саме в цій послідовності (спека §4.9.4).
    // !IsReady() без IsConnected() перед ним перекрив би NOT_CONNECTED для всіх викликів
    // до Connect() і після Отключить - каса чекала б реконекту, якого нікому робити.
    if (!IsConnected()) return ResultEnvelope::Fail("NOT_CONNECTED", "Термінал не підключено");
    if (!IsReady())     return ResultEnvelope::Fail("RECONNECTING",
                                                    "Зв'язок з терміналом відновлюється, повторіть за мить");
```

- [ ] **Step 8: Код 18 і нова семантика `Подключен`**

`src/components/BpoFacadeBase.cpp`, у `CodeToInt` після рядка `UNKNOWN_OUTCOME`:

```cpp
    if (code == "RECONNECTING")        return 18;
```

`src/components/AddinECRPrivatJSON.cpp`:

```cpp
    // Подключен = «термінал підтвердив готовність», а не «сокет відкритий» (спека §4.9.4):
    // «сокет є, а термінал не чує» касі нічим не корисний.
    AddFunction(u"IsConnected", u"Подключен",
        Ret([this]() -> bool { return driver_.IsReady(); }));
```

`EcrPrivatJsonAcquiring::IsConnected()` **не змінюється** — там свідомо лишається сокет: інакше
`CloseDevice` фасаду БПО не викликався б під час `Connecting` і сесія висіла б (спека §4.9.4).

- [ ] **Step 9: Запустити тести — мають пройти**

```
cmake --build build_x64 --config Release --target ecr_privatjson_selftest
bin\Release\ecr_privatjson_selftest_x64.exe
```

Очікування: PASS №20 і №22; усі попередні тести лишаються зеленими (у них обриву немає, тож `Ready`
стоїть від `Connect()` і код 18 не з'являється).

- [ ] **Step 10: Негативна верифікація порядку перевірок і епохи**

1. Тимчасово поміняти місцями перевірки (`!IsReady()` перед `!IsConnected()`) і запустити №22: CHECK
   `ThreeStates(а)` і `ThreeStates(б)` мають упасти з кодом `RECONNECTING` замість `NOT_CONNECTED`.
2. Тимчасово прибрати епоху — `SetLinkState(LinkState::Ready, "")` у `Connect()` — і запустити №24:
   `Подключен` після другого `Connect()` стає `Ложь`. (Зайвий Ping і `CONCURRENT` проявляться лише
   після Task 4, коли хук `up=true` почне запускати джоб, — але сам відкинутий `Ready` видно вже тут.)

Повернути обидві зміни.

- [ ] **Step 11: Повний гейт x64 і x86, коміт**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```

```bash
git add src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp src/components/BpoFacadeBase.cpp src/components/AddinECRPrivatJSON.cpp tests/support/TerminalEmulator.h tests/support/TerminalEmulator.cpp tests/ecr_privatjson_selftest.cpp
git commit -m "feat(ecr): стан зв'язку з епохою, код RECONNECTING=18, подія connection"
```

---

### Task 4: Фоновий джоб — Ping до готовності і з'ясування долі

Найбільше завдання плану: тут з'являється `recoveryJob_`, і з ним — уся фонова механіка. Синхронний
блок Task 2 замінюється на «запустити джоб + зачекати bounded».

**Три тестові шви** (у спеці §5 названо перший; решта — рішення цього плану, бо без них сценарії №7
і №13 неможливо відтворити за розумний час; повідомити архітектора при виконанні):
`SetTransportFactoryForTest`, `SetOutcomeTimingForTest`, `MarkPendingForTest`.

**Files:**
- Modify: `src/platform/JobEngine.h/.cpp` (`startMutex_`)
- Modify: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h/.cpp` (джоб, `EnsureReady`, `CaptureOutcome`,
  `EnsureRecoveryRunning`, `WaitOutcomeBounded`, `Disconnect` за §4.5, шви)
- Consumes (не редагується): `tests/support/TerminalEmulator.h/.cpp` — `DropConnection()` і
  `DropAfterNextResponse()` додані в Task 3, крок 1; тут вони лише використовуються
- Test: `tests/ecr_privatjson_selftest.cpp` (№12, №6, №7, №13, №15, №16, №17, №23, №25)

**Interfaces:**
- Consumes: `MarkPending`/`BuildUnknownOutcome`/`OutcomeSnapshotJson`/`PollStatusOnce` (Task 2),
  `SetLinkState`/`IsReady`/`LinkEpoch` (Task 3), `RequestReceiptFacts` (Task 1).
- Produces:
  - `JobEngine::Start` серіалізовано `startMutex_` — безпечний для двох викликачів;
  - `void EnsureRecoveryRunning()` — ідемпотентна єдина точка старту фонового відновлення;
  - `ResultEnvelope RecoveryJob(std::uint64_t generation)` — крок 0 (`EnsureReady`) + крок 1 (`CaptureOutcome`);
  - `bool EnsureReady()` — цикл Ping із backoff до `Ready`;
  - `ResultEnvelope CaptureOutcome(std::uint64_t generation)` — полінг до спокою → `MarkSynchronized` →
    `RequestReceiptFacts` → запис знімка → подія `outcome`;
  - `void WaitOutcomeBounded(std::uint64_t generation)`;
  - `void SleepInterruptible(int ms)` — сон кроками по 100 мс із перевіркою `closing_`: у `EnsureReady`
    (backoff до 15 с) і в кроці полінгу `CaptureOutcome`, щоб `Disconnect()` не чекав повний інтервал;
  - шви: `SetTransportFactoryForTest(TransportFactory)`,
    `SetOutcomeTimingForTest(int idleWaitMs, int syncWaitMs, int pingTimeoutMs = -1)`,
    `MarkPendingForTest(method, amount, reason)`;
  - `TerminalEmulator::DropAfterNextResponse()`.

- [ ] **Step 1: Тест №12 — `JobEngine::Start` із двох потоків**

`tests/ecr_privatjson_selftest.cpp`, поруч із `TestJobEngine`:

```cpp
// №12: два викликачі Start (ExecuteInternal і dispatcher-хук) на завершеному джобі.
// Без startMutex_ обидва пройдуть перевірку стану, обидва зроблять join того самого
// потоку й присвоєння joinable-потоку -> std::terminate (спека §2.2).
static void TestJobEngineStartRace() {
    for (int iter = 0; iter < 100; ++iter) {
        JobEngine eng;
        eng.Start([]{ return ResultEnvelope::Ok(); });
        eng.Join();                      // джоб завершений; Join() уже приєднав worker_
        eng.ResetToIdle();
        // Гонка, яку ловить цей сценарій: обидва потоки проходять перевірку стану під m_,
        // обидва доходять до worker_ = std::thread(...) — присвоєння в уже-joinable потік
        // (другий переможець) дає std::terminate. Подвійний join покриває сценарій нижче.

        std::atomic<int> wins{ 0 };
        std::atomic<bool> go{ false };
        auto racer = [&]{
            while (!go.load()) std::this_thread::yield();
            if (eng.Start([]{ return ResultEnvelope::Ok(); })) wins.fetch_add(1);
        };
        std::thread t1(racer), t2(racer);
        go.store(true);
        t1.join(); t2.join();
        eng.Join();
        if (wins.load() != 1) {
            CHECK(false, "JobEngineStartRace: рівно один Start повернув true");
            return;
        }
    }
    CHECK(true, "JobEngineStartRace: 100 ітерацій, рівно один переможець, без terminate");

    // Другий сценарій - БЕЗ попереднього Join: worker_ завершеного джоба лишається joinable,
    // тож без startMutex_ обидва потоки роблять join(worker_) того самого потоку (теж terminate).
    for (int iter = 0; iter < 100; ++iter) {
        JobEngine eng;
        eng.Start([]{ return ResultEnvelope::Ok(); });
        while (eng.State() == JobState::Running) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        eng.ResetToIdle();               // Done -> Idle, worker_ НЕ приєднано

        std::atomic<int> wins{ 0 };
        std::atomic<bool> go{ false };
        auto racer = [&]{
            while (!go.load()) std::this_thread::yield();
            if (eng.Start([]{ return ResultEnvelope::Ok(); })) wins.fetch_add(1);
        };
        std::thread t1(racer), t2(racer);
        go.store(true);
        t1.join(); t2.join();
        eng.Join();
        if (wins.load() != 1) {
            CHECK(false, "JobEngineStartRace: без Join - рівно один Start повернув true");
            return;
        }
    }
    CHECK(true, "JobEngineStartRace: 100 ітерацій без попереднього Join, без подвійного join");
}
```

У `main()` — після `TestJobEngine();`.

- [ ] **Step 2: Запустити №12 — має падати або аварійно завершуватись**

```
cmake --build build_x64 --config Release --target ecr_privatjson_selftest
bin\Release\ecr_privatjson_selftest_x64.exe
```

Очікування: `wins != 1` або аварійне завершення процесу (`terminate` через подвійний join).
Обидва результати — «червоне»; якщо 100 ітерацій пройшли зелено, підняти лічильник до 1000, щоб
побачити гонку хоч раз перед фіксом.

- [ ] **Step 3: Серіалізувати `JobEngine::Start`**

`src/platform/JobEngine.h` — приватно:

```cpp
    /// «Машина одного завдання» з двома викликачами - легітимний сценарій (драйвер ECR
    /// стартує відновлення і з потоку 1С, і з dispatcher-хука сесії). Перевірка стану
    /// під m_, а join(worker_) і присвоєння - поза ним, тож без цього м'ютекса два
    /// одночасні Start дають подвійний join і присвоєння joinable-потоку.
    std::mutex startMutex_;
```

`src/platform/JobEngine.cpp` — перший рядок `Start`:

```cpp
bool JobEngine::Start(std::function<ResultEnvelope()> op) {
    std::lock_guard<std::mutex> startLk(startMutex_);
    {
        std::lock_guard<std::mutex> lk(m_);
        …
```

Запустити тест — має пройти (`wins == 1` у всіх ітераціях).

- [ ] **Step 4: Тестова інфраструктура — спільний емулятор і `DropAfterNextResponse`**

`DropAfterNextResponse()` в емуляторі вже є (Task 3, крок 1) — тут він лише використовується (№23).

`tests/ecr_privatjson_selftest.cpp` — спільний набір відповідей (замінює копіпасту в нових тестах):

```cpp
// Спільний стан емулятора для сценаріїв життєвого циклу: лічильники й керований статус.
struct EmuBase {
    std::atomic<int> pings{ 0 };
    std::atomic<int> purchases{ 0 };
    std::atomic<int> receipts{ 0 };
    std::atomic<int> statusCode{ 0 };   // що віддавати на getLastStatMsgCode
    std::atomic<bool> silentPing{ false };
};

static void BaseHandlers(TerminalEmulator& emu, EmuBase& st) {
    emu.OnRequest("PingDevice", [&st](const nlohmann::json&) -> std::string {
        st.pings.fetch_add(1);
        if (st.silentPing.load()) return "";      // монополія: термінал тримає стару сесію
        return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})";
    });
    emu.OnRequest("ServiceMessage", [&st](const nlohmann::json& q) -> std::string {
        const auto mt = q.contains("params") ? q["params"].value("msgType", std::string{}) : std::string{};
        if (mt == "identify")
            return R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX","model":"s800"},"error":false})";
        if (mt == "getLastStatMsgCode")
            return std::string(R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":")")
                 + std::to_string(st.statusCode.load()) + R"("},"error":false})";
        return "";
    });
    emu.OnRequest("GetReceiptInfo", [&st](const nlohmann::json&) {
        st.receipts.fetch_add(1);
        return R"({"method":"GetReceiptInfo","params":{"responseCode":"0000","invoiceNumber":"77","rrn":"555000111","amount":"100.51","txnType":"1"},"error":false})";
    });
}

// Дочекатися умови (мс) - щоб тести не спали фіксовано.
template <class F>
static bool WaitFor(F cond, int timeoutMs = 15000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return cond();
}

// Знімок долі як JSON-об'єкт outcome.
static nlohmann::json OutcomeOf(EcrPrivatJsonDriver& drv) {
    return drv.InquireLastOutcome().payload["outcome"];
}
```

- [ ] **Step 5: Тести стану зв'язку після реконекту (№15, №16, №17, №23)**

```cpp
// №15: після реконекту Ready дає лише протокольний Ping, а не відкритий сокет.
static void TestPingAfterReconnect() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    CHECK(emu.Start(), "PingReconnect: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::mutex evMutex; std::vector<std::string> states;
    drv.SetEventHandler([&](const std::string& ev, const std::string& data) {
        if (ev != "connection") return;
        auto j = nlohmann::json::parse(data, nullptr, false);
        if (j.is_discarded()) return;
        std::lock_guard<std::mutex> lk(evMutex);
        states.push_back(j.value("state", std::string{}));
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "PingReconnect: Connect");
    const int pingsAfterConnect = st.pings.load();

    emu.DropConnection();
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "PingReconnect: після обриву Подключен=Ложь");
    CHECK(WaitFor([&]{ return drv.IsReady(); }), "PingReconnect: після Ping зв'язок знову Ready");
    CHECK(st.pings.load() == pingsAfterConnect + 1,
          "PingReconnect: рівно один PingDevice на новому з'єднанні");

    drv.Disconnect();
    std::lock_guard<std::mutex> lk(evMutex);
    // ready(Connect) -> connecting(dropped) -> ready(Ping) -> disconnected(closed)
    CHECK(states.size() >= 4 && states[0] == "ready" && states[1] == "connecting" &&
          states[2] == "ready" && states.back() == "disconnected",
          "PingReconnect: послідовність подій connection правильна");
    emu.Stop();
}

// №16: silence монополії - Подключен=Ложь весь час, операції відбиваються 18, Ping повторюється.
static void TestSilenceAfterReconnect() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    emu.OnRequest("Purchase", [&st](const nlohmann::json&) {
        st.purchases.fetch_add(1);
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"1"},"error":false})";
    });
    CHECK(emu.Start(), "Silence: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "Silence: Connect");

    st.silentPing.store(true);       // термінал ще вважає стару сесію живою
    const int pingsBefore = st.pings.load();
    const int purchasesBefore = st.purchases.load();
    emu.DropConnection();
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "Silence: Ready знято");

    ResultEnvelope p = drv.Purchase("10.00");
    CHECK(!p.ok && p.code == "RECONNECTING", "Silence: операція під silence -> RECONNECTING");
    CHECK(st.purchases.load() == purchasesBefore, "Silence: Purchase на дріт не пішов");

    CHECK(WaitFor([&]{ return st.pings.load() >= pingsBefore + 2; }, 20000),
          "Silence: Ping повторюється з backoff (>=2 спроби)");
    st.silentPing.store(false);      // термінал відпустив стару сесію
    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "Silence: після першої відповіді -> Ready");
    drv.Disconnect();
    emu.Stop();
}

// №17: deviceBusy на Ping = «живий, зайнятий нашою операцією» -> теж Ready.
static void TestPingBusyIsReady() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    // Перший Ping після реконекту отримує deviceBusy (термінал веде операцію).
    emu.OnRequest("PingDevice", [&st](const nlohmann::json&) -> std::string {
        const int n = st.pings.fetch_add(1);
        if (n >= 1) return R"({"method":"ServiceMessage","params":{"msgType":"deviceBusy"},"error":false})";
        return R"({"method":"PingDevice","params":{"responseCode":"0000"},"error":false})";
    });
    CHECK(emu.Start(), "PingBusy: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "PingBusy: Connect");
    emu.DropConnection();
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "PingBusy: Ready знято після обриву");
    CHECK(WaitFor([&]{ return drv.IsReady(); }), "PingBusy: deviceBusy на Ping -> Ready (живий, зайнятий)");
    drv.Disconnect();
    emu.Stop();
}

// №23: Ready з мертвої епохи не записується - інакше прапорець завис би на мертвому сокеті.
static void TestLinkEpochRejectsStaleReady() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    CHECK(emu.Start(), "LinkEpoch: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::mutex evMutex; std::vector<std::string> states;
    drv.SetEventHandler([&](const std::string& ev, const std::string& data) {
        if (ev != "connection") return;
        auto j = nlohmann::json::parse(data, nullptr, false);
        if (j.is_discarded()) return;
        std::lock_guard<std::mutex> lk(evMutex);
        states.push_back(j.value("state", std::string{}));
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "LinkEpoch: Connect");

    emu.DropAfterNextResponse();   // відповість на Ping реконекту й одразу зникне
    emu.DropConnection();
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "LinkEpoch: Ready знято після першого обриву");
    // Другий обрив стався одразу після відповіді на Ping: Ready з тієї епохи має бути відкинутий,
    // а зв'язок відновитись лише наступним Ping - на живому сокеті.
    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "LinkEpoch: зрештою Ready на живому сокеті");

    drv.Disconnect();
    std::lock_guard<std::mutex> lk(evMutex);
    for (std::size_t i = 1; i < states.size(); ++i)
        if (states[i] == "ready" && states[i - 1] == "ready") {
            CHECK(false, "LinkEpoch: двох ready підряд не буває (Ready з мертвої епохи відкинуто)");
            return;
        }
    CHECK(true, "LinkEpoch: жодного ready поверх мертвої епохи");
}
```

- [ ] **Step 6: Тести з'ясування долі (№6, №7, №13)**

```cpp
// №6: SendFailed при ЖИВОМУ з'єднанні - з'ясування одразу, факти в тому ж виклику.
// Шов ламає рівно відправку Purchase (кодом, що НЕ закриває сокет), тож реконекту не буде
// й хук стану ніколи б не спрацював - саме цей шлях і перевіряємо.
static void TestSendFailedResolvesInline() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    emu.OnRequest("Purchase", [&st](const nlohmann::json&) {
        st.purchases.fetch_add(1);
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"1"},"error":false})";
    });
    CHECK(emu.Start(), "SendFailed: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::atomic<bool> breakPurchase{ false };
    drv.SetTransportFactoryForTest([&breakPurchase](const EcrConnParams& p) -> std::unique_ptr<ITransport> {
        auto t = std::make_unique<TransportTCP>(p.host, p.tcpPort);
        t->SetSendFunctionForTest([&breakPurchase](SOCKET s, const char* buf, int len) -> int {
            const std::string data(buf, static_cast<std::size_t>(len));
            if (breakPurchase.load() && data.find("\"Purchase\"") != std::string::npos) {
                // WSAENOBUFS, а НЕ CONNRESET/ABORTED: Send має провалитись БЕЗ закриття сокета.
                WSASetLastError(WSAENOBUFS);
                return -1;
            }
            return ::send(s, buf, len, 0);
        });
        return t;
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "SendFailed: Connect");

    breakPurchase.store(true);
    ResultEnvelope env = drv.Purchase("100.51");
    breakPurchase.store(false);

    CHECK(!env.ok && env.code == "UNKNOWN_OUTCOME", "SendFailed: операція -> код 17");
    const auto oc = env.payload["outcome"];
    CHECK(oc.value("reason", std::string{}) == "SEND_FAILED", "SendFailed: reason=SEND_FAILED");
    CHECK(oc.value("state", std::string{}) == "resolved", "SendFailed: знімок з'явився в тому ж виклику");
    CHECK(oc.value("channelConnected", false) == true, "SendFailed: channelConnected=true (сокет живий)");
    CHECK(oc["facts"].is_object() && oc["facts"].value("rrn", std::string{}) == "555000111",
          "SendFailed: факти чека у знімку");
    CHECK(st.purchases.load() == 0, "SendFailed: Purchase на термінал не дійшов");
    drv.Disconnect();
    emu.Stop();
}

// №7: термінал не звільнився до ліміту - чесний TERMINAL_BUSY, канал усе одно відкрито.
static void TestTerminalBusyUntilLimit() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    st.statusCode.store(10);                     // «виконується» - і так завжди
    emu.OnRequest("Purchase", [](const nlohmann::json&) -> std::string { return ""; });   // мовчить -> Timeout
    emu.OnRequest("Audit", [](const nlohmann::json&) {
        return R"({"method":"Audit","params":{"responseCode":"0000","receipt":"X"},"error":false})";
    });
    CHECK(emu.Start(), "TerminalBusy: емулятор стартував");

    EcrPrivatJsonDriver drv;
    drv.SetOutcomeTimingForTest(/*idleWaitMs=*/2000, /*syncWaitMs=*/8000);
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "TerminalBusy: Connect");

    ResultEnvelope env = drv.Execute("Purchase",
        nlohmann::json{{"amount","100.51"},{"discount",""},{"merchantId","0"},{"facepay","false"}}, 1500);
    CHECK(!env.ok && env.code == "UNKNOWN_OUTCOME", "TerminalBusy: операція -> код 17");
    CHECK(env.payload["outcome"].value("reason", std::string{}) == "TIMEOUT", "TerminalBusy: reason=TIMEOUT");

    // Знімок читаємо ОКРЕМО, а не з env: Timeout ставить desync, тож за повним критерієм §4.2
    // синхронного очікування немає - 17 повертається негайно, а з'ясування йде після реконекту
    // (крок 0 Ping -> крок 1 полінг). Ліміт спокою скорочено швом до 2 с.
    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }, 30000),
          "TerminalBusy: знімок зроблено після реконекту");
    const auto oc = OutcomeOf(drv);
    CHECK(oc.value("terminalIdle", true) == false, "TerminalBusy: terminalIdle=false");
    CHECK(oc.value("factsCode", std::string{}) == "TERMINAL_BUSY", "TerminalBusy: factsCode=TERMINAL_BUSY");
    CHECK(oc["facts"].is_object() && oc["facts"].empty(), "TerminalBusy: фактів чека немає");
    CHECK(st.receipts.load() == 0, "TerminalBusy: GetReceiptInfo не питали (термінал зайнятий)");

    // Канал відкрито попри те, що спокою не дочекались: інакше desync не зняв би ніхто.
    ResultEnvelope a = drv.Audit("0");
    CHECK(a.code != "DESYNC", "TerminalBusy: після ліміту сесія не лишилась у desync");
    drv.Disconnect();
    emu.Stop();
}

// №13: самозцілення - намір є, джоба немає, зв'язок живий; InquireLastOutcome запускає роботу.
static void TestSelfHealingFromInquire() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    CHECK(emu.Start(), "SelfHeal: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "SelfHeal: Connect");

    // Вікно спеки §4.4: MarkPending без старту джоба (шов) - Pending, з якого нікому вийти.
    drv.MarkPendingForTest("Purchase", "100.51", "TIMEOUT");
    CHECK(OutcomeOf(drv).value("state", std::string{}) != "none", "SelfHeal: намір зафіксовано");

    ResultEnvelope snap = drv.InquireLastOutcome();   // має САМ стартувати відновлення
    CHECK(!snap.ok && snap.code == "UNKNOWN_OUTCOME", "SelfHeal: InquireLastOutcome -> конверт 17");
    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }),
          "SelfHeal: джоб стартував сам і довів справу до resolved");
    CHECK(st.receipts.load() >= 1, "SelfHeal: чек справді запитано");
    drv.Disconnect();
    emu.Stop();
}
```

```cpp
// №25: Отключить під backoff-сном джоба. Без SleepInterruptible Disconnect висів би на
// recoveryJob_.Join() до кінця інтервалу (до 15 с) - саме в центральному сценарії монополії.
//
// ⚠️ Тест мусить зловити джоб САМЕ У СНІ. Якщо кликати Отключить одразу після першого Ping,
// джоб стоїть у DeviceSession::DoRequest (cv_.wait_for на відповідь), і Stop() завершує його
// миттєво незалежно від SleepInterruptible - тест зеленів би й без фіксу. Тому Ping-таймаут
// скорочено швом, і Отключить кличеться, коли ДРУГИЙ Ping уже таймаутнув.
static void TestDisconnectInterruptsBackoff() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    CHECK(emu.Start(), "DisconnectBackoff: емулятор стартував");

    constexpr int kPingMs = 200;
    EcrPrivatJsonDriver drv;
    drv.SetOutcomeTimingForTest(/*idleWaitMs=*/2000, /*syncWaitMs=*/2000, /*pingTimeoutMs=*/kPingMs);
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "DisconnectBackoff: Connect");

    st.silentPing.store(true);                       // термінал мовчить -> кожен Ping таймаутить
    const int pingsBefore = st.pings.load();
    emu.DropConnection();
    // pings >= 2: перший Ping таймаутнув (200 мс), джоб проспав backoff 1 с, відправив другий.
    CHECK(WaitFor([&]{ return st.pings.load() >= pingsBefore + 2; }, 20000),
          "DisconnectBackoff: джоб зробив другий Ping (перший цикл backoff пройдено)");
    // Даємо другому Ping таймаутнути - після цього джоб гарантовано в SleepInterruptible(2000).
    std::this_thread::sleep_for(std::chrono::milliseconds(kPingMs + 50));

    const auto t0 = std::chrono::steady_clock::now();
    drv.Disconnect();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    // Зі SleepInterruptible - до 100 мс на крок сну; з голим sleep_for - решта 2 с інтервалу.
    CHECK(elapsed < 1000, "DisconnectBackoff: Отключить повернувся < 1 с, не чекав backoff");
    emu.Stop();
}
```

У `main()` — усі вісім нових викликів після `TestThreeStatesThreeCodes();`.

- [ ] **Step 7: Запустити — переконатися, що падає**

```
cmake --build build_x64 --config Release --target ecr_privatjson_selftest
```

Очікування: помилки компіляції на `SetTransportFactoryForTest`, `SetOutcomeTimingForTest`,
`MarkPendingForTest`. Після додавання швів (наступний крок) — FAIL по суті: `PingReconnect` не бачить
другого `PingDevice` (хук `up=true` іще no-op), `SelfHeal` не виходить із `pending`.

- [ ] **Step 8: Оголосити джоб, шви й хелпери**

`EcrPrivatJsonDriver.h` — публічно (шви поруч із `SetTrace`, щоб було видно, що це тестова зона):

```cpp
    /// Тестові шви (у продакшені не викликаються).
    using TransportFactory = std::function<std::unique_ptr<ITransport>(const EcrConnParams&)>;
    /// Підмінити фабрику транспорту (стаб Send<0 без закриття сокета). Ставити ДО Connect.
    void SetTransportFactoryForTest(TransportFactory f);
    /// Скоротити очікування з'ясування: 120с ліміту спокою в тесті нестерпні.
    /// pingTimeoutMs < 0 - не чіпати (дефолт kHandshakeTimeoutMs); коротке значення потрібне
    /// тесту №25, щоб джоб гарантовано ДІЙШОВ до backoff-сну, а не стояв у чеканні відповіді.
    void SetOutcomeTimingForTest(int idleWaitMs, int syncWaitMs, int pingTimeoutMs = -1);
    /// Зафіксувати намір БЕЗ старту джоба - модель вікна «Pending без виконавця» (спека §4.4).
    void MarkPendingForTest(const std::string& method, const std::string& amount, const std::string& reason);
```

Приватно:

```cpp
    /// Єдина точка старту фонового відновлення (спека §4.4). Ідемпотентна: якщо джоб іде -
    /// нічого не робить. Кличуть: ExecuteInternal після MarkPending, dispatcher-хук (up=true),
    /// гейт §4.6, InquireLastOutcome, Connect() після старту сесії.
    void EnsureRecoveryRunning();
    /// Тіло фонового джоба: крок 0 - готовність (Ping), крок 1 - з'ясування долі.
    ResultEnvelope RecoveryJob(std::uint64_t generation);
    /// Крок 0: цикл Ping із backoff, доки термінал не відповість (Response або deviceBusy).
    bool EnsureReady();
    /// Крок 1: полінг статусу до спокою -> MarkSynchronized -> RequestReceiptFacts -> знімок.
    ResultEnvelope CaptureOutcome(std::uint64_t generation);
    /// Bounded очікування знімка в синхронному виклику (спека §4.4а).
    void WaitOutcomeBounded(std::uint64_t generation);
    /// Сон, перериваний Disconnect-ом: кроками по 100 мс із перевіркою closing_. Голий
    /// sleep_for(backoff) до 15 с ззовні не перервати, і Отключить під silence монополії
    /// висів би на recoveryJob_.Join() до кінця інтервалу, а §4.5 обіцяє швидкий Join.
    void SleepInterruptible(int ms);

    JobEngine          recoveryJob_;      ///< ДРУГИЙ движок: job_ несе контракт СостояниеОперации
    std::atomic<bool>  closing_{ false }; ///< Disconnect у процесі: джоб і хук виходять
    TransportFactory   transportFactory_; ///< тестовий шов; nullptr -> справжні транспорти
    std::atomic<int>   outcomeIdleWaitMs_{ kOperationTimeoutMs };  ///< скільки чекати спокою
    std::atomic<int>   outcomeSyncWaitMs_{ kOutcomeSyncWaitMs };   ///< скільки чекати в синхронному виклику
    /// Таймаут Ping у EnsureReady. Дефолт = kHandshakeTimeoutMs (.cpp); поле, а не константа,
    /// лише заради тесту №25 - інакше джоб до backoff-сну не доходить (стоїть у чеканні Ping).
    std::atomic<int>   pingTimeoutMs_{ 5000 };

    static constexpr int kOutcomeSyncWaitMs   = 8000;   ///< менше за терпіння касира в РМК
    /// Ритм Ping-повторів навмисно дублює дефолти SessionConfig (DeviceSession.h:35-38):
    /// сесія конфіг назовні не віддає, а стукати ми маємо в такт із супервізором.
    static constexpr int kReconnectDelayMs    = 1000;
    static constexpr int kReconnectMaxDelayMs = 15000;
```

Прибрати: `inRecovery_`, `RecoverAfterDesync()`, `kRecoverPollTries` (їх заміщає `CaptureOutcome`).

- [ ] **Step 9: Реалізувати шви й фабрику транспорту**

```cpp
// Дефолт pingTimeoutMs_ у заголовку продубльовано числом (константа живе в анонімному
// namespace цього .cpp і в .h не видима) - тримаємо їх у синхроні перевіркою компілятора.
static_assert(kHandshakeTimeoutMs == 5000, "pingTimeoutMs_ у .h ініціалізовано 5000 — оновити разом");

void EcrPrivatJsonDriver::SetTransportFactoryForTest(TransportFactory f) { transportFactory_ = std::move(f); }

void EcrPrivatJsonDriver::SetOutcomeTimingForTest(int idleWaitMs, int syncWaitMs, int pingTimeoutMs) {
    outcomeIdleWaitMs_.store(idleWaitMs);
    outcomeSyncWaitMs_.store(syncWaitMs);
    if (pingTimeoutMs > 0) pingTimeoutMs_.store(pingTimeoutMs);
}

void EcrPrivatJsonDriver::MarkPendingForTest(const std::string& method, const std::string& amount,
                                             const std::string& reason) {
    OperationIntent intent;
    intent.method    = method;
    intent.amount    = amount;
    intent.startedAt = std::chrono::system_clock::now();
    MarkPending(intent, reason);
}
```

`MakeTransport` — перший рядок:

```cpp
    if (transportFactory_) return transportFactory_(p);
```

- [ ] **Step 10: Реалізувати джоб**

```cpp
void EcrPrivatJsonDriver::SleepInterruptible(int ms) {
    constexpr int kStepMs = 100;
    for (int left = ms; left > 0 && !closing_.load(); left -= kStepMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(left < kStepMs ? left : kStepMs));
}

void EcrPrivatJsonDriver::EnsureRecoveryRunning() {
    if (closing_.load() || !IsConnected()) return;   // без сокета ні Ping, ні з'ясування
    const bool needReady = !IsReady();
    std::uint64_t gen = 0;
    bool needOutcome = false;
    {
        std::lock_guard<std::mutex> lk(outcomeMutex_);
        needOutcome = lastOutcome_.state == OutcomeState::Pending;
        gen = lastOutcome_.generation;
    }
    if (!needReady && !needOutcome) return;
    // false = джоб уже йде; це нормальний, найчастіший результат.
    recoveryJob_.Start([this, gen] { return RecoveryJob(gen); });
}

ResultEnvelope EcrPrivatJsonDriver::RecoveryJob(std::uint64_t generation) {
    // Крок 0: спершу термінал має підтвердити, що чує нас. Полінг статусу в тишу монополії
    // дав би хибний TERMINAL_BUSY (спека §4.4).
    if (!EnsureReady()) return ResultEnvelope::Fail("ABORTED", "Зв'язок не підтверджено");
    bool pending = false;
    {
        std::lock_guard<std::mutex> lk(outcomeMutex_);
        pending = lastOutcome_.state == OutcomeState::Pending && lastOutcome_.generation == generation;
    }
    if (!pending) return ResultEnvelope::Ok();       // готовність відновлено, питання про долю немає
    return CaptureOutcome(generation);
}

bool EcrPrivatJsonDriver::EnsureReady() {
    int backoff = kReconnectDelayMs;
    while (!closing_.load() && IsConnected() && !IsReady()) {
        const std::uint64_t epoch = LinkEpoch();     // епоха, в якій шлемо цей Ping
        auto ping = EcrJsonCodec::BuildRequest("PingDevice", 0, nullptr);
        GateSend();
        // Той самий Ping, що в Connect: провідний 0x00 «закриває» півкадр, що міг лишитись
        // у буфері термінала після обриву посеред передачі (спека §2.3).
        RequestResult r = session_->RequestPrimary(ping, pingTimeoutMs_.load(),
                                                   FrameOptions{ /*leadingDelimiter=*/true });
        if (r.status == RequestStatus::Response || r.status == RequestStatus::Busy) {
            // Busy = живий, зайнятий нашою операцією - теж «чує нас». Ready із мертвої епохи
            // SetLinkState відкине сам.
            SetLinkState(LinkState::Ready, "", epoch);
            return IsReady();
        }
        if (r.status == RequestStatus::Disconnected || r.status == RequestStatus::Stopped)
            return false;                            // сокет упав знову - перезапустить хук
        // Timeout тут - не аварія, а очікуваний стан «термінал ще тримає стару сесію»
        // (монополія, спека §2.3), тож повторюємо без обмеження кількості.
        SleepInterruptible(backoff);
        const int next = backoff * 2;
        backoff = (next > kReconnectMaxDelayMs) ? kReconnectMaxDelayMs : next;
    }
    return IsReady();
}

ResultEnvelope EcrPrivatJsonDriver::CaptureOutcome(std::uint64_t generation) {
    NEUTRAL_REPORT_WARN("ECRPrivatJSON", "З'ясування долі операції: полінг статусу термінала");
    bool idle = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(outcomeIdleWaitMs_.load());
    while (std::chrono::steady_clock::now() < deadline) {
        if (closing_.load()) return ResultEnvelope::Fail("ABORTED", "Відключення");
        {
            std::lock_guard<std::mutex> lk(outcomeMutex_);
            if (lastOutcome_.generation != generation)
                return ResultEnvelope::Fail("ABORTED", "Знімок належить іншому поколінню");
        }
        int code = -1;
        const RequestStatus st = PollStatusOnce(code);
        if (st == RequestStatus::Disconnected || st == RequestStatus::Stopped)
            // Продовжувати на непідтвердженому з'єднанні не можна: наступний up=true
            // приведе сюди знову - через крок 0 (Ping).
            return ResultEnvelope::Fail("ABORTED", "Зв'язок обірвався під час з'ясування");
        if (st == RequestStatus::Response && code >= 0) {
            lastStatus_.store(code);
            if (code == 0) { idle = true; break; }   // термінал у спокої (спека §6.5)
        }
        SleepInterruptible(kPollIntervalMs);
    }

    // Канал відкриваємо в ОБОХ випадках: після спокою - штатно; після вичерпання ліміту -
    // бо лишити desync назавжди нікому було б зняти (гейт після Resolved не діє), а нова
    // операція чесно отримає deviceBusy/Timeout від самого термінала. Різниця видима
    // у знімку: terminalIdle і factsCode.
    if (session_) session_->MarkSynchronized();

    ResultEnvelope facts = idle
        ? RequestReceiptFacts(std::string{})
        : ResultEnvelope::Fail("TERMINAL_BUSY", "Термінал не звільнився за відведений час");
    if (idle && (facts.code == "DISCONNECTED" || facts.code == "STOPPED"))
        return ResultEnvelope::Fail("ABORTED", "Зв'язок обірвався під час отримання чека");

    {
        std::lock_guard<std::mutex> lk(outcomeMutex_);
        if (lastOutcome_.generation != generation)
            return ResultEnvelope::Fail("ABORTED", "Знімок належить іншому поколінню");
        lastOutcome_.state        = OutcomeState::Resolved;
        lastOutcome_.terminalIdle = idle;
        lastOutcome_.facts        = facts;
    }
    // Подія несе ЛИШЕ об'єкт outcome (конверт є в result і в ИсходПоследнейОперацииJSON).
    EmitEvent("outcome", OutcomeSnapshotJson());
    // Значення НЕ читається: факти беруться з lastOutcome_ під outcomeMutex_.
    return ResultEnvelope::Ok();
}

void EcrPrivatJsonDriver::WaitOutcomeBounded(std::uint64_t generation) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(outcomeSyncWaitMs_.load());
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(outcomeMutex_);
            if (lastOutcome_.generation != generation) return;
            if (lastOutcome_.state == OutcomeState::Resolved) return;
        }
        const JobState js = recoveryJob_.State();
        if (js != JobState::Running && js != JobState::Interrupting) return;   // працювати нікому
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
}
```

- [ ] **Step 11: Підключити джоб до `ExecuteInternal`, `Connect`, хука і `Disconnect`**

`ExecuteInternal` — тимчасовий синхронний блок Task 2 **замінити цілком** (не доповнити): разом із
ним зникає послаблений критерій «зв'язок є» = `IsConnected()`, який Task 2 тримав лише тому, що
з'ясовувати після desync до появи джоба не було кому. Тут набирає чинності **повний критерій §4.2**.
Уся гілка тригера після заміни виглядає так (`SetLinkState(Connecting, "timeout")` — з Task 3,
крок 6-біс; він лишається на місці, перед `MarkPending`):

```cpp
        if (r.status == RequestStatus::Timeout || desync)
            SetLinkState(LinkState::Connecting, "timeout");        // Task 3, крок 6-біс
        const std::uint64_t gen = MarkPending(intent, reason);
        EnsureRecoveryRunning();
        // §4.2 п.2, ПОВНИЙ критерій: «зв'язок є» = сокет живий І сесія не в desync. Друга
        // умова - не формальність: після Timeout primary супервізор негайно рве й перевідкриває
        // сесію (DeviceSession.cpp:436-447), тож IsConnected() у цю мить - гонка. При desync
        // іде шлях (б): 17 негайно, знімок - після реконекту через хук, крок 0 (Ping), крок 1.
        // «Зараз» де-факто лишається для SendFailed без закриття транспорту: там з'єднання
        // справді живе, desync не ставиться, реконекту не буде, і хук ніколи б не спрацював.
        if (IsConnected() && !desync) WaitOutcomeBounded(gen);
        env = BuildUnknownOutcome();
```

Перевірка після заміни: у тілі `ExecuteInternal` не лишилось ані `inRecovery_`, ані виклику
`RecoverAfterDesync` — обидва прибрані (Step 8).

`Connect()` — хук стану (крок 3) і рядок після `Start()`:

```cpp
    session_->SetConnectionStateHandler([this](bool up) {
        if (up) { EnsureRecoveryRunning(); return; }   // TCP є -> Ping до готовності (крок 0)
        SetLinkState(LinkState::Connecting, "dropped");
    });
    …
    // Ручний реконект із 1С (Отключить/Подключить) при збереженому Pending: без Pending - no-op,
    // зайвого Ping не буде, бо linkState_ уже Ready.
    EnsureRecoveryRunning();
    return true;
```

`InquireLastOutcome` — перший рядок (самозцілення §4.4):

```cpp
    EnsureRecoveryRunning();   // вікно «Pending без виконавця» закривається тут
```

`Disconnect()` — повний порядок §4.5 (Task 5 його верифікує тестами):

```cpp
void EcrPrivatJsonDriver::Disconnect() {
    closing_.store(true);      // хук і CaptureOutcome бачать і виходять
    job_.Join();               // фінансову операцію НЕ рвемо - як і раніше
    if (session_) session_->Stop();   // усі pending -> Stopped негайно; dispatcher join-нуто,
                                      // тож нових хуків (і нових стартів джоба) більше не буде
    recoveryJob_.Join();       // швидкий: його запити вже повернули Stopped
    if (session_) {
        session_.reset();
        NEUTRAL_REPORT_INFO("ECRPrivatJSON", "Disconnect: сесію закрито");
    } else {
        NEUTRAL_REPORT_INFO("ECRPrivatJSON", "Disconnect: сесії немає (вже відключено) — no-op");
    }
    SetLinkState(LinkState::Disconnected, "closed");
    closing_.store(false);
}
```

Порядок важливий: `recoveryJob_.Join()` **до** `Stop()` зависав би до `kOperationTimeoutMs` — джоб може
стояти в `RequestPrimary`. `Disconnect()` **не викликати з хука**: `Stop()` із dispatcher-потоку не
робить self-join, і гарантія «після Stop хуків немає» не тримається.

- [ ] **Step 12: Запустити тести — мають пройти**

```
cmake --build build_x64 --config Release --target ecr_privatjson_selftest
bin\Release\ecr_privatjson_selftest_x64.exe
```

Очікування: PASS №12, №6, №7, №13, №15, №16, №17, №23 і всіх попередніх. №16 і №23 повільні
(backoff + реконект) — сумарний час харнесу зросте приблизно на 30-40 с.

- [ ] **Step 13: Негативна верифікація**

Три перевірки, кожну — окремо, з поверненням коду після неї:
1. Прибрати `EnsureRecoveryRunning()` з хука `up=true` → `PingReconnect` не дочекається `Ready`.
2. Прибрати перевірку `epoch != linkEpoch_` у `SetLinkState` → `LinkEpoch` рано чи пізно ловить
   два `ready` підряд (повторити прогін 5 разів, гонка не щоразу).
3. Повернути `MarkSynchronized()` під умову `if (idle)` → `TerminalBusy` падає на CHECK
   «після ліміту сесія не лишилась у desync».
4. Замінити `SleepInterruptible(backoff)` на `std::this_thread::sleep_for` → №25 падає:
   `elapsed` стає ≈2 с (залишок backoff-інтервалу) проти порога 1 с. Саме заради цієї перевірки
   тест чекає ДРУГОГО Ping і його таймауту — інакше джоб стояв би в очікуванні відповіді, і
   `Stop()` завершував би його миттєво навіть без `SleepInterruptible` (тест зеленів би завжди).

- [ ] **Step 14: Повний гейт x64 і x86, коміт**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```

```bash
git add src/platform/JobEngine.h src/platform/JobEngine.cpp src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp tests/ecr_privatjson_selftest.cpp
git commit -m "feat(ecr): фоновий джоб — Ping до готовності і з'ясування долі операції"
```

---

### Task 5: Життєвий цикл під обривами — верифікація

Код `Disconnect()` за §4.5 уже написаний у Task 4; це завдання доводить його тестами й закриває
дірки, якщо вони знайдуться. Плюс фіксує правило «`Connect()` не скидає `lastOutcome_`»: факти
належать операції, а не сесії, і каса могла їх ще не забрати.

**Files:**
- Modify: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h/.cpp` (шов `StopSessionForTest`)
- Test: `tests/ecr_privatjson_selftest.cpp` (№2+№3, №8, №9, №14) + `EmuBase.statusPolls`

**Interfaces:**
- Consumes: усе з Task 4.
- Produces: `void StopSessionForTest();` — зупиняє **сесію** (не драйвер), щоб змоделювати
  `Stopped`-тригер без `session_.reset()` під активним запитом. Прямий `Disconnect()` з іншого потоку
  під час синхронної операції архітектурою не передбачений (спека §2.4) і в тесті дав би
  use-after-free на `session_`, а не перевірку тригера.

- [ ] **Step 1: Доповнити хелпер емулятора лічильником полінгів**

`EmuBase` (Task 4) — додати поле й інкремент у `BaseHandlers`:

```cpp
    std::atomic<int> statusPolls{ 0 };   // скільки разів питали getLastStatMsgCode
```
```cpp
        if (mt == "getLastStatMsgCode") {
            st.statusPolls.fetch_add(1);
            return std::string(R"({"method":"ServiceMessage","params":{"msgType":"getLastStatMsgCode","LastStatMsgCode":")")
                 + std::to_string(st.statusCode.load()) + R"("},"error":false})";
        }
```

- [ ] **Step 2: Написати тести №2+№3, №8, №9, №14**

```cpp
// №2 + №3: обрив у польоті -> 17 негайно (pending, facts=null); після реконекту фоновий
// джоб дочікується спокою й робить знімок -> resolved, рівно одна подія outcome.
static void TestOutcomeAfterReconnect() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    st.statusCode.store(10);                       // термінал іще веде операцію
    emu.OnRequest("Purchase", [&](const nlohmann::json&) -> std::string {
        st.purchases.fetch_add(1);
        emu.DropConnection();                      // прийняв оплату й зник
        return "";
    });
    CHECK(emu.Start(), "OutcomeReconnect: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::atomic<int> outcomeEvents{ 0 };
    drv.SetEventHandler([&](const std::string& ev, const std::string&) {
        if (ev == "outcome") outcomeEvents.fetch_add(1);
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "OutcomeReconnect: Connect");

    ResultEnvelope env = drv.Purchase("100.51");
    CHECK(!env.ok && env.code == "UNKNOWN_OUTCOME", "OutcomeReconnect(№2): 17 повернуто до реконекту");
    {
        const auto oc = env.payload["outcome"];
        CHECK(oc.value("state", std::string{}) == "pending", "OutcomeReconnect(№2): state=pending");
        CHECK(oc["facts"].is_null(), "OutcomeReconnect(№2): facts=null, поки доля невідома");
        CHECK(oc.value("reason", std::string{}) == "DISCONNECTED", "OutcomeReconnect(№2): reason=DISCONNECTED");
        CHECK(oc.value("channelConnected", true) == false, "OutcomeReconnect(№2): channelConnected=false");
    }

    // Реконект -> Ping -> Ready -> полінг статусу. Відпускаємо термінал після кількох полінгів.
    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "OutcomeReconnect(№3): зв'язок відновлено Ping-ом");
    const int pollsAtReady = st.statusPolls.load();
    CHECK(WaitFor([&]{ return st.statusPolls.load() >= pollsAtReady + 2; }),
          "OutcomeReconnect(№3): джоб полить статус");
    st.statusCode.store(0);                        // термінал звільнився

    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }),
          "OutcomeReconnect(№3): знімок зроблено (resolved)");
    const auto oc = OutcomeOf(drv);
    CHECK(oc.value("terminalIdle", false) == true, "OutcomeReconnect(№3): terminalIdle=true");
    CHECK(oc["facts"].is_object() && oc["facts"].value("rrn", std::string{}) == "555000111",
          "OutcomeReconnect(№3): факти чека у знімку");
    CHECK(oc.value("generation", 0ull) == 1ull, "OutcomeReconnect(№3): generation=1 (перше питання)");
    CHECK(outcomeEvents.load() == 1, "OutcomeReconnect(№3): подія outcome рівно одна");
    drv.Disconnect();
    emu.Stop();
}

// №8: Отключить посеред Pending з активним джобом - без падінь і зависань; знімок переживає
// перепідключення (Connect НЕ скидає lastOutcome_: факти належать операції, не сесії).
static void TestDisconnectDuringPending() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    st.statusCode.store(10);                       // джоб застрягне в полінгу
    emu.OnRequest("Purchase", [&](const nlohmann::json&) -> std::string {
        st.purchases.fetch_add(1);
        emu.DropConnection();
        return "";
    });
    CHECK(emu.Start(), "DisconnectPending: емулятор стартував");

    EcrPrivatJsonDriver drv;
    const std::string conn = std::string("tcp://127.0.0.1:") + std::to_string(emu.Port());
    CHECK(drv.Connect(conn), "DisconnectPending: Connect");
    ResultEnvelope env = drv.Purchase("100.51");
    CHECK(env.code == "UNKNOWN_OUTCOME", "DisconnectPending: 17 отримано");
    CHECK(WaitFor([&]{ return st.statusPolls.load() > 0 && drv.IsReady(); }, 20000),
          "DisconnectPending: джоб працює (Ready + полінг)");

    const auto beforeGen = OutcomeOf(drv).value("generation", 0ull);
    drv.Disconnect();                              // посеред роботи джоба
    CHECK(!drv.IsReady(), "DisconnectPending: після Отключить зв'язку немає");

    CHECK(drv.Connect(conn), "DisconnectPending: повторний Connect");
    const auto oc = OutcomeOf(drv);
    CHECK(oc.value("state", std::string{}) != "none" && oc.value("generation", 0ull) == beforeGen,
          "DisconnectPending: lastOutcome_ переживає Отключить/Подключить");
    drv.Disconnect();
    emu.Stop();
}

// №9: повторний обрив УЖЕ під час з'ясування. Перший джоб виходить ABORTED саме через
// Disconnected від PollStatusOnce (покоління не мінялось, closing_ не ставився); після
// другого реконекту хук бачить Pending -> джоб знову з кроку 0. Знімок один.
static void TestSecondDropDuringCapture() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    st.statusCode.store(10);
    emu.OnRequest("Purchase", [&](const nlohmann::json&) -> std::string {
        st.purchases.fetch_add(1);
        emu.DropConnection();
        return "";
    });
    CHECK(emu.Start(), "SecondDrop: емулятор стартував");

    EcrPrivatJsonDriver drv;
    std::atomic<int> outcomeEvents{ 0 };
    drv.SetEventHandler([&](const std::string& ev, const std::string&) {
        if (ev == "outcome") outcomeEvents.fetch_add(1);
    });
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "SecondDrop: Connect");
    ResultEnvelope env = drv.Purchase("100.51");
    CHECK(env.code == "UNKNOWN_OUTCOME", "SecondDrop: 17 отримано (gen=1)");

    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "SecondDrop: перший реконект, Ready");
    const int polls = st.statusPolls.load();
    CHECK(WaitFor([&]{ return st.statusPolls.load() > polls; }), "SecondDrop: джоб полить статус");
    emu.DropConnection();                          // другий обрив ПОСЕРЕД з'ясування
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "SecondDrop: Ready знято вдруге");
    CHECK(OutcomeOf(drv).value("state", std::string{}) == "pending",
          "SecondDrop: намір лишився Pending (перший джоб вийшов без запису)");

    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "SecondDrop: другий реконект, Ready");
    st.statusCode.store(0);
    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }),
          "SecondDrop: знімок зроблено після другого реконекту");
    CHECK(OutcomeOf(drv).value("generation", 0ull) == 1ull, "SecondDrop: покоління те саме (1)");
    CHECK(outcomeEvents.load() == 1, "SecondDrop: подія outcome одна, не дві");
    drv.Disconnect();
    emu.Stop();
}

// №14: Stopped як тригер. Сесію зупиняє інший потік під час синхронної операції -
// FinishPendingLocked завершує запит статусом Stopped, а термінал МІГ устигнути.
static void TestStoppedIsTrigger() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    emu.OnRequest("Purchase", [&st](const nlohmann::json&) -> std::string {
        st.purchases.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::seconds(5));    // «касир вводить пін»
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"5"},"error":false})";
    });
    CHECK(emu.Start(), "Stopped: емулятор стартував");

    EcrPrivatJsonDriver drv;
    const std::string conn = std::string("tcp://127.0.0.1:") + std::to_string(emu.Port());
    CHECK(drv.Connect(conn), "Stopped: Connect");

    std::thread stopper([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        drv.StopSessionForTest();
    });
    ResultEnvelope env = drv.Purchase("100.51");
    stopper.join();

    CHECK(!env.ok && env.code == "UNKNOWN_OUTCOME", "Stopped: операція -> код 17");
    CHECK(env.payload["outcome"].value("reason", std::string{}) == "STOPPED", "Stopped: reason=STOPPED");
    CHECK(env.payload["outcome"].value("state", std::string{}) == "pending", "Stopped: state=pending");

    // Ручний реконект із 1С (спека §4.4в): Connect бачить Pending і запускає з'ясування сам.
    drv.Disconnect();
    CHECK(drv.Connect(conn), "Stopped: повторний Connect");
    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }),
          "Stopped: після Connect джоб зробив знімок (§4.4в)");
    drv.Disconnect();
    emu.Stop();
}
```

У `main()` — чотири виклики після тестів Task 4.

- [ ] **Step 3: Запустити — переконатися, що падає**

Очікування: помилка компіляції `StopSessionForTest: is not a member`; після додавання шва —
падіння по суті, якщо `Disconnect()` не тримає порядок §4.5 (зависання №8) або якщо `Connect()`
скидає знімок.

- [ ] **Step 4: Додати шов зупинки сесії**

`EcrPrivatJsonDriver.h` — поруч з іншими швами:

```cpp
    /// Тестовий шов: зупинити ЛИШЕ сесію (усі pending -> Stopped), не чіпаючи драйвер.
    /// Моделює Stopped-тригер без session_.reset() під активним запитом.
    void StopSessionForTest();
```

`.cpp`:

```cpp
void EcrPrivatJsonDriver::StopSessionForTest() { if (session_) session_->Stop(); }
```

- [ ] **Step 5: Запустити тести — мають пройти**

```
cmake --build build_x64 --config Release --target ecr_privatjson_selftest
bin\Release\ecr_privatjson_selftest_x64.exe
```

Якщо №8 зависає — перевірити порядок у `Disconnect()`: `recoveryJob_.Join()` мусить іти **після**
`session_->Stop()`, інакше джоб стоятиме в `RequestPrimary` до 120 с.

- [ ] **Step 6: Негативна верифікація**

1. Додати `lastOutcome_ = LastOutcome{};` на початок `Connect()` → №8 падає на «переживає Отключить».
2. Прибрати `Stopped` із таблиці `reason` → №14 падає (код стає `STOPPED`, не 17).
3. Прибрати ранній вихід на `Disconnected` у `CaptureOutcome` → №9 ловить другу подію `outcome`
   або знімок із чужого покоління.

- [ ] **Step 7: Повний гейт x64 і x86, коміт**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```

```bash
git add src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp tests/ecr_privatjson_selftest.cpp
git commit -m "test(ecr): життєвий цикл під обривами — знімок, повторний обрив, Stopped"
```

---

### Task 6: Гейт — нова оплата під час `Pending`

Найімовірніший шлях до подвійного списання: касир бачить помилку й тисне «Оплата» ще раз, поки
фоновий `GetReceiptInfo` ще йде. Гейт закриває і його, і конкуренцію за єдину primary-доріжку.

**Files:**
- Modify: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp` (`ExecuteInternal`, вхідні перевірки)
- Test: `tests/ecr_privatjson_selftest.cpp` (№4, №5, №21)

**Interfaces:**
- Consumes: `IsFinancial`, `BuildUnknownOutcome`, `EnsureRecoveryRunning` (Task 2, 4).
- Produces: поведінку «фінансовий метод під `Pending` не йде на дріт», описану для 1С у §9 спеки.

- [ ] **Step 1: Написати тести**

```cpp
// №4 + №5: під Pending друга оплата не йде на дріт; після Resolved гейт знято.
static void TestGateBlocksSecondPurchase() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    st.statusCode.store(10);                         // термінал зайнятий -> Pending триває
    std::atomic<bool> firstDone{ false };
    emu.OnRequest("Purchase", [&](const nlohmann::json&) -> std::string {
        const int n = st.purchases.fetch_add(1);
        if (n == 0) { emu.DropConnection(); return ""; }        // перша - обрив
        firstDone.store(true);
        return R"({"method":"Purchase","params":{"responseCode":"0000","invoiceNumber":"2"},"error":false})";
    });
    CHECK(emu.Start(), "Gate: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "Gate: Connect");
    CHECK(drv.Purchase("100.51").code == "UNKNOWN_OUTCOME", "Gate: перша оплата -> 17");
    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "Gate: зв'язок відновлено");

    // №4: доля ще не з'ясована (термінал тримає 10) - друга оплата відбивається гейтом.
    CHECK(OutcomeOf(drv).value("state", std::string{}) == "pending", "Gate: стан ще pending");
    const int purchasesBefore = st.purchases.load();
    ResultEnvelope second = drv.Purchase("100.51");
    CHECK(!second.ok && second.code == "UNKNOWN_OUTCOME", "Gate(№4): друга оплата -> 17, не на дріт");
    CHECK(second.description.find("попередньої") != std::string::npos,
          "Gate(№4): опис пояснює, що з'ясовується доля попередньої операції");
    CHECK(st.purchases.load() == purchasesBefore, "Gate(№4): емулятор другого Purchase не бачив");

    // №5: термінал звільнився -> знімок -> гейт знято.
    st.statusCode.store(0);
    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }),
          "Gate: знімок зроблено");
    ResultEnvelope third = drv.Purchase("100.51");
    CHECK(third.ok && third.code == "0000", "Gate(№5): після resolved оплата проходить");
    CHECK(firstDone.load() && st.purchases.load() == purchasesBefore + 1,
          "Gate(№5): саме ця оплата дійшла до термінала");
    drv.Disconnect();
    emu.Stop();
}

// №21: Pending і Connecting одночасно -> код 17 (гейт перший), а не 18.
static void TestGateBeforeReconnecting() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    st.statusCode.store(10);
    emu.OnRequest("Purchase", [&](const nlohmann::json&) -> std::string {
        st.purchases.fetch_add(1);
        emu.DropConnection();
        return "";
    });
    CHECK(emu.Start(), "GateOrder: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())), "GateOrder: Connect");
    st.silentPing.store(true);                       // після реконекту термінал мовчить -> Connecting
    CHECK(drv.Purchase("100.51").code == "UNKNOWN_OUTCOME", "GateOrder: перша оплата -> 17");
    CHECK(WaitFor([&]{ return !drv.IsReady(); }), "GateOrder: стан Connecting");
    CHECK(OutcomeOf(drv).value("state", std::string{}) == "pending", "GateOrder: намір Pending");

    ResultEnvelope p = drv.Purchase("100.51");
    CHECK(!p.ok && p.code == "UNKNOWN_OUTCOME",
          "GateOrder(№21): Pending+Connecting -> 17 (гейт §4.6 першим), не RECONNECTING");
    drv.Disconnect();
    emu.Stop();
}
```

- [ ] **Step 2: Запустити — переконатися, що падає**

Очікування: `Gate(№4)` падає — друга оплата сьогодні йде на дріт (`purchases` зростає);
`GateOrder(№21)` падає з кодом `RECONNECTING` замість 17.

- [ ] **Step 3: Поставити гейт першим на вході**

`ExecuteInternal` — початок тіла (перед усіма перевірками стану):

⚠️ **Нового `const bool financial` тут НЕ оголошувати.** Воно вже оголошене нижче в тілі
`ExecuteInternal` (Task 2, крок 6); друге оголошення в тому самому плоскому скоупі — помилка
компіляції MSVC C2374. Гейт користується `IsFinancial(method)` напряму — саме так, як у §4.9.4 спеки.

```cpp
    // Гейт §4.6: поки доля попередньої фінансової операції невідома, нову на дріт не пускаємо.
    // Саме тут закривається найімовірніший шлях до подвійного списання (касир тисне «Оплата»
    // ще раз, поки фоновий GetReceiptInfo іде) і конкуренція за єдину primary-доріжку.
    // ПЕРЕД перевірками стану зв'язку: при незавершеному намірі касир має бачити 17 із
    // поясненням, а не NOT_CONNECTED/RECONNECTING.
    if (IsFinancial(method)) {
        bool pending = false;
        { std::lock_guard<std::mutex> lk(outcomeMutex_); pending = lastOutcome_.state == OutcomeState::Pending; }
        if (pending) {
            EnsureRecoveryRunning();               // самозцілення §4.4
            ResultEnvelope env = BuildUnknownOutcome();
            env.description = "З'ясовую долю попередньої операції; "
                              "повторіть після ИсходПоследнейОперацииJSON";
            return env;
        }
    }
```

Гейт діє й на асинхронний шлях: `StartPurchase`/`StartRefund` ідуть через той самий `ExecuteInternal`.
Нефінансові методи під `Pending` свідомо **не** гейтяться: `Audit`/`Verify`/`ПолучитьЧек`/
`ПроверитьСвязь` можуть отримати `CONCURRENT` на секунди, поки фоновий запит тримає доріжку, — це
чесний код, а ховати діагностику саме тоді, коли адміністратор її хоче, гірше (спека §4.6).

- [ ] **Step 4: Запустити тести — мають пройти**

- [ ] **Step 5: Негативна верифікація**

Перенести гейт **після** перевірки `!IsReady()` → №21 падає (18 замість 17). Прибрати гейт зовсім →
№4 падає (емулятор бачить другу оплату). Повернути.

- [ ] **Step 6: Повний гейт x64 і x86, коміт**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```

```bash
git add src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp tests/ecr_privatjson_selftest.cpp
git commit -m "feat(ecr): гейт нової оплати, поки доля попередньої невідома"
```

---

### Task 7: API для 1С — `ИсходПоследнейОперацииJSON` і `УстановитьИдентификаторЗапроса`

Два методи проходять чотири шари: драйвер (готово) → `IAcquiringDriver` → адаптер → фасади (БПО й
прямий). Це **перше** використання прямого API розширенням, тож імена й форма важать (спека §2.4).

**Files:**
- Modify: `src/drivers/IAcquiringDriver.h`
- Modify: `src/drivers/ecr_privatjson/EcrPrivatJsonAcquiring.h/.cpp`
- Modify: `src/components/AcquiringFacadeBase.cpp` (`RegisterAsyncExtensions`)
- Modify: `src/components/AddinECRPrivatJSON.cpp`
- Test: `tests/ecr_privatjson_selftest.cpp` (№19), `tests/ecr_native_host.cpp` (§6.3)

**Interfaces:**
- Consumes: `EcrPrivatJsonDriver::InquireLastOutcome()`, `SetRequestId(std::string)`.
- Produces (імена, які бачить 1С — далі їх використовує розширення за §9 спеки):

| Рівень | Англ. | Локалізоване | Форма |
|---|---|---|---|
| прямий API `ECRPrivatJSON` | `InquireLastOutcome` | `ИсходПоследнейОперацииJSON` | функція без параметрів → рядок JSON |
| прямий API `ECRPrivatJSON` | `SetRequestId` | `УстановитьИдентификаторЗапроса` | процедура, 1 параметр |
| БПО-фасади (3004/4000) | ті самі два імені | ті самі | так само |

- [ ] **Step 1: Тест №19 — одноразовість `requestId`**

```cpp
// №19: id каси проходить у знімок прозоро й забирається ОДНОРАЗОВО - друга операція
// без сеттера не має успадкувати чужий ключ (інакше 1С зшила б не ту транзакцію).
static void TestRequestIdIsOneShot() {
    TerminalEmulator emu; EmuBase st; BaseHandlers(emu, st);
    emu.OnRequest("Purchase", [&](const nlohmann::json&) -> std::string {
        st.purchases.fetch_add(1);
        emu.DropConnection();
        return "";
    });
    CHECK(emu.Start(), "RequestId: емулятор стартував");

    EcrPrivatJsonDriver drv;
    const std::string conn = std::string("tcp://127.0.0.1:") + std::to_string(emu.Port());
    CHECK(drv.Connect(conn), "RequestId: Connect");

    drv.SetRequestId("abc-123");
    ResultEnvelope first = drv.Purchase("100.51");
    CHECK(first.code == "UNKNOWN_OUTCOME", "RequestId: перша оплата -> 17");
    CHECK(first.payload["outcome"]["intent"].value("requestId", std::string{}) == "abc-123",
          "RequestId: id каси у знімку операції");
    CHECK(OutcomeOf(drv)["intent"].value("requestId", std::string{}) == "abc-123",
          "RequestId: той самий id у ИсходПоследнейОперацииJSON");
    CHECK(OutcomeOf(drv)["intent"].value("amount", std::string{}) == "100.51",
          "RequestId: сума у знімку - рядком, як пішла на дріт");
    CHECK(!OutcomeOf(drv)["intent"].value("startedAt", std::string{}).empty(),
          "RequestId: startedAt заповнено (ISO 8601 UTC)");

    // Друга оплата БЕЗ сеттера: id має бути порожнім, а не успадкованим.
    CHECK(WaitFor([&]{ return drv.IsReady(); }, 20000), "RequestId: реконект");
    CHECK(WaitFor([&]{ return OutcomeOf(drv).value("state", std::string{}) == "resolved"; }),
          "RequestId: перший знімок готовий (гейт знято)");
    ResultEnvelope second = drv.Purchase("55.00");
    CHECK(second.code == "UNKNOWN_OUTCOME", "RequestId: друга оплата теж обірвалась");
    CHECK(second.payload["outcome"]["intent"].value("requestId", std::string{}).empty(),
          "RequestId: без сеттера id порожній (одноразовість)");
    drv.Disconnect();
    emu.Stop();
}
```

Плюс перевірка події `outcome` через прямий канал (подія несе **лише** об'єкт `outcome`, без конверта):

```cpp
    // у TestOutcomeAfterReconnect (Task 5) додати до обробника:
    drv.SetEventHandler([&](const std::string& ev, const std::string& data) {
        if (ev != "outcome") return;
        outcomeEvents.fetch_add(1);
        auto j = nlohmann::json::parse(data, nullptr, false);
        CHECK(!j.is_discarded() && j.contains("state") && !j.contains("code"),
              "Подія outcome несе сам знімок, без конверта ok/code");
    });
```

- [ ] **Step 2: Запустити — переконатися, що падає**

Очікування: помилка компіляції `SetRequestId: is not a member` немає (метод із Task 2), але
`requestId` у знімку порожній, якщо забір зроблено не на вході — тоді падає перший CHECK. Якщо все
зелене одразу, тимчасово перенести `std::exchange` у `MarkPending` і переконатися, що другий
`Purchase` успадковує `abc-123` — це і є та помилка, від якої тест страхує.

- [ ] **Step 3: Прокинути методи через `IAcquiringDriver`**

`src/drivers/IAcquiringDriver.h`, у секцію асинхронного розширення:

```cpp
    /// Доля останньої фінансової операції у вигляді конверта (спека ECR §4.7). Мережею не
    /// ходить. Дефолт - чесна відмова: драйвер, який цього не вміє, лишається чесним.
    virtual ResultEnvelope InquireLastOutcome() { return AcquiringUnsupported("Доля останньої операції"); }
    /// Correlation id каси (ИдентификаторЗапроса БПО), що має пройти у знімок прозорим
    /// рядком. Дефолт - no-op: протокол, який його не носить, нічого не втрачає.
    virtual void SetRequestId(const std::string& id) { (void)id; }
```

`EcrPrivatJsonAcquiring.h` — в оголошення:

```cpp
    ResultEnvelope InquireLastOutcome() override;
    void SetRequestId(const std::string& id) override;
```

`EcrPrivatJsonAcquiring.cpp`:

```cpp
ResultEnvelope EcrPrivatJsonAcquiring::InquireLastOutcome() {
    try { return drv_.InquireLastOutcome(); }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_ERROR(kTag, std::string("Помилка InquireLastOutcome: ") + e.what());
        return ResultEnvelope::Fail("EXCEPTION", e.what());
    }
}

void EcrPrivatJsonAcquiring::SetRequestId(const std::string& id) { drv_.SetRequestId(id); }
```

- [ ] **Step 4: Зареєструвати методи у БПО-фасаді**

`src/components/AcquiringFacadeBase.cpp`, у кінець `RegisterAsyncExtensions()`:

```cpp
    // Доля перерваної операції. Читається БЕЗ мережі й у будь-якому стані зв'язку - саме
    // тому доступна одразу після Ложь від платіжного методу, коли потік 1С уже живий.
    AddFunction(u"InquireLastOutcome", u"ИсходПоследнейОперацииJSON",
        Ret([this]() -> std::string {
            try {
                return Driver().InquireLastOutcome().ToJson()
                           .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            } catch (const std::exception& e) {
                REPORT_ERROR(std::string("Помилка ИсходПоследнейОперацииJSON: ") + e.what());
                return ResultEnvelope::Fail("EXCEPTION", e.what()).ToJson()
                           .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            }
        }));

    // Кличеться ПЕРЕД штатною платіжною командою, у тій самій точці, де 1С пише намір
    // у реєстр. Рядок прозорий: не парситься, не валідується, не обрізається.
    AddProcedure(u"SetRequestId", u"УстановитьИдентификаторЗапроса",
        MethFunction(std::function<void(VH)>([this](VH id) {
            try { Driver().SetRequestId(VariantToString(id)); }
            catch (const std::exception& e) {
                REPORT_ERROR(std::string("Помилка УстановитьИдентификаторЗапроса: ") + e.what());
            }
        })),
        std::vector<ParamSpec>{ ParamSpec{ u"Id", u"ИдентификаторЗапроса", /*required*/true, {} } });
```

- [ ] **Step 5: Ті самі методи в прямому API**

`src/components/AddinECRPrivatJSON.cpp`, поруч з асинхронною трійцею:

```cpp
    AddFunction(u"InquireLastOutcome", u"ИсходПоследнейОперацииJSON",
        Ret([this]() -> std::string {
            try {
                return driver_.InquireLastOutcome().ToJson()
                           .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            } catch (const std::exception& e) {
                REPORT_ERROR(std::string("Помилка ИсходПоследнейОперацииJSON: ") + e.what());
                return std::string("{}");
            }
        }));

    AddProcedure(u"SetRequestId", u"УстановитьИдентификаторЗапроса",
        MethFunction(std::function<void(VH)>([this](VH id) {
            try { driver_.SetRequestId(static_cast<std::string>(id)); }
            catch (const std::exception& e) { REPORT_ERROR(std::string("Помилка УстановитьИдентификаторЗапроса: ") + e.what()); }
        })),
        std::vector<ParamSpec>{ ParamSpec{ u"id", u"ИдентификаторЗапроса", /*required*/true, {} } });
```

- [ ] **Step 6: e2e через головну DLL (§6.3)**

`tests/ecr_native_host.cpp` — у блок БПО-фасаду 3004, після наявної перевірки `PayByPaymentCard`,
додати сценарій обриву. Емулятор у цьому харнесі спільний, тож окремий responder ставиться
безпосередньо перед кроком:

```cpp
    // ---- Обрив під час оплати: БПО віддає Ложь + 17, OUT-параметри лишаються порожні ----
    {
        emu.OnRequest("Purchase", [&emu](const json&) -> std::string {
            emu.DropConnection();          // термінал прийняв оплату й зник
            return "";
        });
        emu.OnRequest("GetReceiptInfo", [](const json&) {
            return std::string(R"({"method":"GetReceiptInfo","params":{"responseCode":"0000","invoiceNumber":"88","rrn":"555999000","amount":"100.50"},"error":false})");
        });

        long idxSetId = bpo->FindMethod(L"SetRequestId");
        CHECK(idxSetId >= 0, "L3-bpo: метод УстановитьИдентификаторЗапроса знайдено");
        if (idxSetId >= 0) {
            std::wstring wid = u8to16("req-42");
            tVariant p; tVarInit(&p);
            p.vt = VTYPE_PWSTR; p.pwstrVal = (WCHAR_T*)wid.c_str(); p.wstrLen = (uint32_t)wid.size();
            bpo->CallAsProc(idxSetId, &p, 1);
        }

        long idxPay = bpo->FindMethod(L"PayByPaymentCard");
        tVariant p[7];
        for (int i = 0; i < 7; ++i) tVarInit(&p[i]);
        std::wstring wIn = u8to16("in");
        p[0].vt = VTYPE_PWSTR; p[0].pwstrVal = (WCHAR_T*)wIn.c_str(); p[0].wstrLen = (uint32_t)wIn.size();
        p[1].vt = VTYPE_R8; p[1].dblVal = 100.50;
        for (int i = 2; i < 7; ++i) { p[i].vt = VTYPE_PWSTR; p[i].pwstrVal = nullptr; p[i].wstrLen = 0; }
        tVariant ret; tVarInit(&ret);
        bpo->CallAsFunc(idxPay, &ret, p, 7);
        CHECK(ret.vt == VTYPE_BOOL && ret.bVal == false, "L3-bpo: оплата при обриві -> Ложь");
        // OUT-параметри (СсылочныйНомер/КодАвторизации/ТекстСлипЧека) лишаються порожні:
        // класти туди поля чужого чека - та сама підміна іншим каналом (спека §4.7).
        bool outEmpty = true;
        for (int i = 3; i < 7; ++i)
            if (p[i].vt == VTYPE_PWSTR && p[i].wstrLen > 0) outEmpty = false;
        CHECK(outEmpty, "L3-bpo: OUT-параметри при 17 лишились порожні");

        long idxErr = bpo->FindMethod(L"GetLastError");
        tVariant ep, eret; tVarInit(&ep); tVarInit(&eret);
        ep.vt = VTYPE_PWSTR; ep.pwstrVal = nullptr; ep.wstrLen = 0;
        bpo->CallAsFunc(idxErr, &eret, &ep, 1);
        CHECK(eret.vt == VTYPE_I4 && eret.lVal == 17, "L3-bpo: ПолучитьОшибку = 17 (UNKNOWN_OUTCOME)");

        long idxOutcome = bpo->FindMethod(L"InquireLastOutcome");
        CHECK(idxOutcome >= 0, "L3-bpo: метод ИсходПоследнейОперацииJSON знайдено");
        if (idxOutcome >= 0) {
            bool rb = false; std::string rs; bool gotStr = false;
            callFunc(bpo, idxOutcome, {}, rb, rs, gotStr);
            CHECK(gotStr && !rs.empty(), "L3-bpo: ИсходПоследнейОперацииJSON повернув рядок");
            json j = json::parse(rs, nullptr, false);
            CHECK(!j.is_discarded() && j.value("code", std::string{}) == "UNKNOWN_OUTCOME",
                  "L3-bpo: конверт знімка - код 17");
            const auto oc = j["payload"]["outcome"];
            CHECK(oc.value("state", std::string{}) != "none", "L3-bpo: знімок має стан");
            CHECK(oc["intent"].value("requestId", std::string{}) == "req-42",
                  "L3-bpo: ИдентификаторЗапроса пройшов крізь усі шари");
        }
    }
```

Для прямого класу `ECRPrivatJSON` — окремий блок одразу після кроку 5 (`Оплата`), **до** кроку 6
(`Disconnect`), поки з'єднання ще живе:

```cpp
    // 5-біс) Прямий API: знімок долі доступний і тут, тими самими іменами.
    {
        long idxSetId = comp->FindMethod(L"SetRequestId");
        CHECK(idxSetId >= 0, "L3: метод УстановитьИдентификаторЗапроса знайдено в ECRPrivatJSON");
        long idxOutcome = comp->FindMethod(L"InquireLastOutcome");
        CHECK(idxOutcome >= 0, "L3: метод ИсходПоследнейОперацииJSON знайдено в ECRPrivatJSON");
        if (idxOutcome >= 0) {
            bool rb = false; std::string rs; bool gotStr = false;
            callFunc(comp, idxOutcome, {}, rb, rs, gotStr);
            CHECK(gotStr && !rs.empty(), "L3: ИсходПоследнейОперацииJSON повернув рядок");
            json j = json::parse(rs, nullptr, false);
            // Оплата на кроці 5 пройшла успішно, тож питання про долю не стояло взагалі.
            CHECK(!j.is_discarded() && j.value("ok", false) == true &&
                  j.value("code", std::string{}) == "OK",
                  "L3: після успішної оплати знімок - ok/OK, не помилка");
            CHECK(j["payload"]["outcome"].value("state", std::string{}) == "none",
                  "L3: state=none - з'ясовувати нема чого");
        }
    }
```

- [ ] **Step 7: Запустити гейт із L2-ecr**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```

Очікування: `L2-ecr ecr_native_host` PASS з новими CHECK; `ecr_privatjson_selftest` PASS.
**Якщо метод не знайдено з 1С** — перевірити `/utf-8` для цілі в `CMake/compiler_settings.cmake`:
без нього MSVC мовчки псує кириличні `u"…"`-літерали, англійські імена працюють, російські — ні.

- [ ] **Step 8: Негативна верифікація**

Прибрати `override` `InquireLastOutcome` в адаптері → e2e ловить `UNSUPPORTED` замість знімка
(дефолт інтерфейсу), тобто CHECK «конверт знімка — код 17» падає. Повернути.

- [ ] **Step 9: Коміт**

```bash
git add src/drivers/IAcquiringDriver.h src/drivers/ecr_privatjson/EcrPrivatJsonAcquiring.h src/drivers/ecr_privatjson/EcrPrivatJsonAcquiring.cpp src/components/AcquiringFacadeBase.cpp src/components/AddinECRPrivatJSON.cpp tests/ecr_privatjson_selftest.cpp tests/ecr_native_host.cpp
git commit -m "feat(ecr): ИсходПоследнейОперацииJSON і УстановитьИдентификаторЗапроса для 1С"
```

---

### Task 8: TCP keepalive — завжди

Тихий обрив (кабель, Wi-Fi, вимкнений термінал) не дає ні FIN, ні RST: `recv` у reader висить
нескінченно, і компонента ніколи не дізнається, що зв'язку немає. Keepalive виявляє мертву лінію за
~20 с без жодного протокольного трафіку.

**Files:**
- Modify: `src/transport/Transport_TCP.h` (константи, `GetSocketForTest`)
- Modify: `src/transport/Transport_TCP.cpp` (`#include <mstcpip.h>`, keepalive після `m_socket = sock`)
- Test: `tests/wire_selftest.cpp` (№18)

**Interfaces:**
- Produces: `SOCKET GetSocketForTest() const;` — тестовий шов за аналогією до `SetSendFunctionForTest`.
- Зміна діє на **всіх** споживачів `TransportTCP`, включно з `LabelPrinterDriver`: принтеру keepalive
  не шкодить, а прапорець заради нього не вартий розгалуження поведінки (спека §4.9.5).

- [ ] **Step 1: Тест №18**

`tests/wire_selftest.cpp`, поруч з іншими TCP-тестами:

```cpp
// Keepalive увімкнено завжди: без нього тихий обрив у простої не виявляється ніколи
// (recv висить, FIN/RST не буде). Перевіряємо сам факт увімкнення - виявлення мертвої
// лінії за ~20 с перевіряється лише на залізі (спека §7).
static void TestTcpKeepAliveEnabled() {
    RawTcpEchoServer server;
    CHECK(server.Start(/*echoEnabled=*/true), "TcpKeepAlive: echo server started");

    TransportTCP transport("127.0.0.1", server.Port());
    CHECK(transport.Open(), "TcpKeepAlive: Open");

    int val = 0;
    int len = sizeof(val);
    const int rc = getsockopt(transport.GetSocketForTest(), SOL_SOCKET, SO_KEEPALIVE,
                              reinterpret_cast<char*>(&val), &len);
    CHECK(rc == 0 && val != 0, "TcpKeepAlive: SO_KEEPALIVE увімкнено після Open");

    transport.Close();
    server.Stop();
}
```

У `main()` — `RunGuarded("TestTcpKeepAliveEnabled", TestTcpKeepAliveEnabled);` після
`TestTcpStateUpGate`.

- [ ] **Step 2: Запустити — переконатися, що падає**

```
cmake --build build_x64 --config Release --target wire_selftest
bin\Release\wire_selftest_x64.exe
```

Очікування: помилка компіляції `GetSocketForTest: is not a member`; після додавання шва — FAIL
`val == 0` (негативна верифікація вбудована в порядок кроків).

- [ ] **Step 3: Додати шов і константи**

`src/transport/Transport_TCP.h` — публічно, поруч із `SetSendFunctionForTest`:

```cpp
    /// Тестовий шов: сокет активного з'єднання (для getsockopt у харнесі).
    SOCKET GetSocketForTest() const { return m_socket.load(); }
```

Приватно, поруч із `CONNECT_TIMEOUT_MS`:

```cpp
    // Keepalive: перша проба через 10 с простою, далі кожну секунду. Кількість проб у
    // Windows фіксована (10), тож мертва лінія виявляється за ~20 с.
    static constexpr ULONG KEEPALIVE_IDLE_MS = 10000;
    static constexpr ULONG KEEPALIVE_INTERVAL_MS = 1000;
```

- [ ] **Step 4: Увімкнути keepalive у `Open`**

`src/transport/Transport_TCP.cpp` — до заголовків додати `#include <mstcpip.h>`; одразу після
`m_socket = sock;`:

```cpp
    // TCP keepalive - ЗАВЖДИ, без опції (спека §4.9.5). Протокол вимагає тримати з'єднання
    // відкритим (еталонна схема, крок 6 «keepalive»), а тихий обрив інакше не виявити:
    // ні FIN, ні RST не буде, reader висітиме в recv. Нуль протокольного трафіку - термінал
    // не турбуємо, працює й під час операції.
    {
        BOOL on = TRUE;
        if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE,
                       reinterpret_cast<const char*>(&on), sizeof(on)) == SOCKET_ERROR) {
            NEUTRAL_REPORT_WARN("TransportTCP",
                "Не вдалося увімкнути SO_KEEPALIVE, код " + std::to_string(WSAGetLastError()));
        }
        tcp_keepalive ka{ 1, KEEPALIVE_IDLE_MS, KEEPALIVE_INTERVAL_MS };
        DWORD returned = 0;
        if (WSAIoctl(sock, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0,
                     &returned, nullptr, nullptr) == SOCKET_ERROR) {
            NEUTRAL_REPORT_WARN("TransportTCP",
                "SIO_KEEPALIVE_VALS не застосовано, код " + std::to_string(WSAGetLastError()));
        }
    }
```

Невдача — `WARN` і продовжити: з'єднання без keepalive гірше за з'єднання з ним, але краще за
відсутнє.

- [ ] **Step 5: Запустити — має пройти; перевірити сусіда**

```
cmake --build build_x64 --config Release
bin\Release\wire_selftest_x64.exe
bin\Release\label_printer_selftest_x64.exe
bin\Release\label_native_host_x64.exe
```

`LabelPrinter`-гейт має лишитися зеленим на **обох** рівнях — транспорт спільний: `L-p1`
(`label_printer_selftest`) і `L-p3` (`label_native_host`, компонента через головну DLL; він
потребує зібраної `SimplyAddinConnectWin_x64.dll`, тож або збирай усе, як у команді вище, або
покладайся на повний гейт кроку 6).

- [ ] **Step 6: Повний гейт x64 і x86, коміт**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -NoUapki x86
```

```bash
git add src/transport/Transport_TCP.h src/transport/Transport_TCP.cpp tests/wire_selftest.cpp
git commit -m "feat(transport): TCP keepalive завжди — тихий обрив виявляється за ~20 с"
```

---

### Task 9: Документація — джерело правди

Код без цього завдання не «майже готовий»: документація в цьому репозиторії — джерело правди, і
розширення 1С робитиме свою частину саме за нею (спека §9).

**Files:**
- Modify: `docs/architecture/ecrprivatjson.md` (§5 життєвий цикл, §6.6 переписати, новий розділ
  «Доля операції після обриву», модель потоків)
- Modify: `docs/architecture/device-core.md` (§5.5)
- Modify: `docs/architecture/bpo-contract.md` (таблиця кодів, §6.3)
- Modify: `docs/integration-1c/ecr-privatjson.md` (§2, §3.3, §4, §7 + контракт §9)

**Interfaces:** документація описує рівно те, що реалізовано в Task 1-8; жодних «планується».

- [ ] **Step 1: `docs/architecture/ecrprivatjson.md`**

1. **§5 «Життєвий цикл — `Connect`»** — додати підрозділ «5.2. Стан зв'язку»: три стани
   (`Disconnected`/`Connecting`/`Ready`), таблиця переходів, епоха `linkEpoch_` і навіщо вона
   (Ping із мертвої епохи), Ping після реконекту замість повторення еталонної схеми, keepalive
   транспорту, подія `connection`. Явно: **`Ready` ≠ `connected_`**; `Connect()` ставить `Ready` до
   `Start()`.
2. **§6.6 «Відновлення після desync»** — переписати повністю: `RecoverAfterDesync`/`inRecovery_`/
   `kRecoverPollTries` більше немає; є `recoveryJob_` (другий `JobEngine`), `RecoveryJob` = крок 0
   `EnsureReady` + крок 1 `CaptureOutcome`, `EnsureRecoveryRunning` як єдина ідемпотентна точка
   старту з п'ятьма викликачами, `MarkSynchronized` при спокої **або** після вичерпання ліміту.
3. **Новий розділ «6.7. Доля операції після обриву»** — тригер (чотири статуси + desync, таблиця
   `reason`), `LastOutcome`/`generation`, гейт нової оплати, bounded очікування 8 с у синхронному
   виклику, форма `payload.outcome`, події `outcome`/`connection`, що компонента **не** робить
   (не зіставляє, не повторює, не сторнує, не тримає стан між сеансами).
4. **§6.1 «Потокова модель»** — додати `recoveryJob_` і правило «під `outcomeMutex_`/`linkMutex_` —
   ні мережі, ні подій»; згадати, що `JobEngine::Start` тепер серіалізовано.
5. **§9 «Тестовий контур»** — перелічити нові сценарії №1-25 і чотири тестові шви драйвера
   (`SetTransportFactoryForTest`, `SetOutcomeTimingForTest`, `MarkPendingForTest`, `StopSessionForTest`)
   плюс два режими обриву в емуляторі (`DropConnection`, `DropAfterNextResponse`).

- [ ] **Step 2: `docs/architecture/device-core.md` §5.5**

Додати абзац-межу:

> Протокол-специфічна процедура відновлення **не має підміняти результат операції**. `desync` у
> device-core — семантика кадрової синхронізації; «доля фінансової операції невідома» — семантика
> драйвера. Драйвер, що після відновлення повертає касі чужий чек, порушує саме цю межу
> (історія: `ECRPrivatJSON`, виправлено 2026-09; `docs/architecture/ecrprivatjson.md` §6.6-6.7).

- [ ] **Step 3: `docs/architecture/bpo-contract.md`**

1. Таблиця кодів помилок — два рядки після `16`:

| `17` | `UNKNOWN_OUTCOME` | доля фінансової операції невідома: зв'язок обірвався в польоті |
| `18` | `RECONNECTING` | зв'язок відновлюється, термінал ще не підтвердив готовність |

2. §6.3 (реєстр незавершених операцій) — вписати, що компонента тепер дає `17` + факти й **чекає**,
   що реєстр не знімає намір на цей код; посилання на §9 спеки.
3. Позначити для звірки з компаньйоном (сесія розширення): чи реагує реєстр на `17` як на «не знаю».

- [ ] **Step 4: `docs/integration-1c/ecr-privatjson.md`**

1. **§2.1** — нова семантика `Подключен`: «термінал підтвердив готовність», не «сокет відкритий»;
   публічного `PingDevice` немає, тиха перевірка стану — саме `Подключен`.
2. **§2.4** — два нові методи: `ИсходПоследнейОперацииJSON` (без параметрів, будь-коли, без мережі)
   і `УстановитьИдентификаторЗапроса` (кличеться **разом** із командою; сеттер без команди лишить
   id до наступного сеттера).
3. **§3.3 «Поле `code`»** — рядки `UNKNOWN_OUTCOME`=17 і `RECONNECTING`=18 з поясненням різниці:
   на `NOT_CONNECTED` треба `Подключить`, на `RECONNECTING` — **зачекати**.
4. **§3.4 «Поле `payload`»** — повна форма `outcome` з прикладом JSON (як у спеці §4.7).
5. **§4 «Події»** — `outcome` (несе сам знімок) і `connection` (`connecting`/`ready`/`disconnected`
   + `reason`), обидві лише в прямому API після `ВключитьСобытия`.
6. **§5 рецепти** — новий рецепт «Обрив під час оплати»: `Ложь` → `ПолучитьОшибку()`=17 →
   `ИсходПоследнейОперацииJSON` → полінг, поки `state=="pending"` → рішення каси за `facts`.
7. **§7 «Типові помилки»** — `CheckConnection`/`ПроверитьСвязь` позначити **інтерактивним**
   (§5.5 протоколу: вибір мерчанта на терміналі), тому для тихої перевірки — `Подключен`;
   `CONCURRENT` під час `pending` — нормальний тимчасовий стан, повторити пізніше;
   `responseCode 1008` при повторі — «транзакція вже виконана», привід звірити реєстр.
8. **Новий §8 «Контракт розширення»** — перенести §9 спеки повністю (11 пунктів): сеттер перед
   командою, читання коду через `ОбъектДрайвера.ПолучитьОшибку()`, «не знімати намір», «не
   повторювати», забір знімка, полінг `pending`, звірка за `requestId`, `generation` як
   ідентифікатор знімка, стан зв'язку, `CONCURRENT`, `1008`.

- [ ] **Step 5: Перевірка узгодженості**

```
grep -rn "RecoverAfterDesync\|inRecovery_\|kRecoverPollTries" docs/ src/
```

Очікування: **жодного** збігу (усе прибрано в Task 4). Далі очима: у `docs/` немає обіцянок
«планується» про те, що вже зроблено, і навпаки — немає опису неіснуючого `СостояниеСвязи`.

- [ ] **Step 6: Коміт**

```bash
git add docs/architecture/ecrprivatjson.md docs/architecture/device-core.md docs/architecture/bpo-contract.md docs/integration-1c/ecr-privatjson.md
git commit -m "docs(ecr): життєвий цикл зв'язку, доля операції після обриву, контракт для 1С"
```

---

### Task 10: Живий прогін на Newland N950

Емулятор моделює припущення; сім питань §7 спеки може закрити лише залізо. До цього прогону
відповідні місця документації лишаються позначеними як припущення.

**Files:**
- Modify: `docs/architecture/ecrprivatjson.md` §9 (розділ «Верифікація на реальному обладнанні» —
  дописати результати), `docs/superpowers/specs/2026-09-05-ecr-connection-lifecycle-design.md` §7
  (позначити перевірені пункти)

**Interfaces:** нічого нового; це верифікація зробленого.

- [ ] **Step 1: Зібрати постачання**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
```

`bin/Release/SimplyAddinConnectWin.zip` — у розширення 1С. Пам'ятати про кеш компоненти в 1С: імена
DLL усередині ZIP несуть версію, тож платформа бере свіжу (`docs/architecture/build-and-packaging.md`).

- [ ] **Step 2: Сценарії §7 спеки на живому терміналі**

Прогнати по черзі, фіксуючи факти (лог компоненти + `ИсходПоследнейОперацииJSON`):

1. Обрив каси посеред оплати: чи термінал **доводить** операцію до кінця, чи скасовує.
2. `GetReceiptInfo("")` після **відхиленої** операції: чи віддає її з `responseCode`, чи лише успішні.
3. Чий годинник у `facts.date`/`facts.time` і як він розходиться з `intent.startedAt`.
4. Повтор `Purchase` після виконаної операції: чи приходить `1008`.
5. Чи монотонний `invoiceNumber` у межах пакета.
6. **Найважливіше:** чи переживають факти операції **нову TCP-сесію** — каса зникла після `Purchase`,
   термінал операцію довів, каса реконектилась новою сесією: чи бачать `GetReceiptInfo("")` і
   `getLastStatMsgCode` ту операцію.
7. Скільки термінал тримає мертву сесію: висмикнути кабель каси на ~30 с у простої, вставити —
   через скільки він відповідає новому `PingDevice` (це час, який касир бачитиме `RECONNECTING`).

- [ ] **Step 3: Записати результати**

Дописати в `docs/architecture/ecrprivatjson.md` §9 підрозділ «Верифікація життєвого циклу
(дата, N950)» — по рядку на пункт: що перевірено, що виявилось. У спеці §7 позначити пункти
`перевірено`/`не підтвердилось`. Якщо п.6 показав, що стан скидається разом із сесією, — окремо
зафіксувати, що §9 п.4 контракту 1С у найгіршому сценарії не працює й зіставлення можливе лише за
номером чека.

- [ ] **Step 4: Коміт**

```bash
git add docs/architecture/ecrprivatjson.md docs/superpowers/specs/2026-09-05-ecr-connection-lifecycle-design.md
git commit -m "docs(ecr): результати живого прогону життєвого циклу на N950"
```

---

## Порядок і залежності

| Task | Залежить від | Мерджиться |
|---|---|---|
| 1. Чужий чек | — | **окремим PR** (спека §8) |
| 2. Стан долі, тригер, requestId | 1 | гілка `ecr-inflight-outcome` |
| 3. Стан зв'язку, код 18 | 2 (спільні місця в `ExecuteInternal`) | те саме |
| 4. Фоновий джоб | 2, 3 | те саме |
| 5. Життєвий цикл (тести) | 4 | те саме |
| 6. Гейт | 4 | те саме |
| 7. API для 1С | 2, 4 | те саме |
| 8. Keepalive | — (незалежне, але не раніше за 3: спільний гейт) | те саме |
| 9. Документація | 1-8 | те саме |
| 10. Живий прогін | 9 | те саме |

Після Task 10 — робота на боці розширення `SMP_SimplyConnect` за §9 спеки (зняття наміру з
урахуванням 17, сеттер `requestId`, індикатор стану). Це **окрема** сесія й окремий репозиторій;
компонента до того часу має зафіксований і перевірений контракт.
