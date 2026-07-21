# ECRPrivatJSON — Частина 1: платформа-каркас + транспортний e2e (план впровадження)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Довести wire-спину пілотного драйвера ECRPrivatJSON до першого зеленого e2e — `Connect` за еталонною схемою (PingDevice+dc→Identify+dc→постійний конект) проти власного протокол-обізнаного TCP-емулятора термінала.

**Architecture:** Нові класи поверх наявного `DeviceSession` (`src/transport/`): `EcrJsonCodec` (JSON↔байти), `EcrPrivatJsonClassifier : IFrameClassifier` (кореляція за `method`/`msgType`, `deviceBusy`/`*Transmitted`/`methodNotImplemented`), `EcrPrivatJsonDriver` (розбір рядка підключення, фабрика транспорту, життєвий цикл), `ResultEnvelope` (уніфікований результат). Тести — окремий exe `ecr_privatjson_selftest` з `TerminalEmulator` (Winsock-сервер на localhost, скриптовані відповіді за `method`). Уся Privat-специфіка — в кодеку+класифікаторі; `DeviceSession`/транспорти лишаються загальними.

**Tech Stack:** C++17, MSVC (VS2022), Winsock2 (`ws2_32`), `nlohmann_json` (header-only, наявний), CMake ≥3.16. Тести — хенд-ролед CHECK-харнес (як `wire_selftest`), гейт по exit-коду.

**Спека:** `docs/tasks/2026-07-21_design_ecr_privatjson_driver.md` (v2). Ця Частина 1 покриває §13 кроки 1-4 (до першого e2e). Операції/пауза/1С-фасад — Частина 2. Порядок трохи уточнено проти §13: спину-до-Connect будуємо раніше за `JobEngine`/`OperationRegistry`, бо саме вона доводить wire-шлях; платформа операцій вправляється в Частині 2.

## Global Constraints

Кожна задача неявно включає ці правила (значення — verbatim зі спеки/AGENTS.md):

- **Платформа:** тільки Windows, C++17, MSVC. Компіляція з `/utf-8` (кириличні коментарі/літерали).
- **PCH:** `src/*.cpp` **першим рядком** підключають `#include "<відносний>/core/pch.h"` (для `src/platform/*` → `"../core/pch.h"`; для `src/drivers/ecr_privatjson/*` → `"../../core/pch.h"`). У `.h` — **ніколи**. У `tests/*` — pch **НЕ** підключати (правило теки `tests/`).
- **Транспорт:** термінал = сервер, ми = клієнт. TCP порт за замовч. `2000`; COM `115200 8N1` (драйвер **явно** передає baud `115200` — дефолт `TransportCOM` = 9600).
- **Кадр:** UTF-8 JSON + один термінатор `0x00`. Термінатор додає/знімає `NullTerminatedFramer` (кодек делімітером НЕ оперує). Хендшейк PingDevice — з провідним `0x00` через `FrameOptions{leadingDelimiter=true}`.
- **Кореляція** — за полем `method` (окремого id немає).
- **Логування** — лише макросами з `ServiceTools.h`; у не-компонентних класах (драйвер/кодек/класифікатор) — `NEUTRAL_REPORT_*` з 1-м аргументом-ім'ям компоненти (рядок `"ECRPrivatJSON"`). Прямий `spdlog` заборонено. Повідомлення — конкатенацією, з великої літери, без крапки в кінці, без printf.
- **Помилки:** після `NEUTRAL_REPORT_ERROR` — зазвичай `return false`; винятки не перетинають межу 1С — код, що може кинути (JSON-парс), обгортати `try/catch` з `NEUTRAL_REPORT_ERROR` у `catch`.
- **Мова коду/коментарів** — українська/російська (за файлом).
- **CMake:** склад головної DLL — ЛИШЕ через OBJECT-ліби + `$<TARGET_OBJECTS>` (`HEADER_FILES`/`SOURCE_FILES` вестигіальні). Тестові exe — окремі цілі, НЕ в DLL, оголошені ДО гейта `if(NOT BUILD_WITH_UAPKI) return()` у `tests/CMakeLists.txt`.
- **Тести** лягають у `bin/Release/<name>_x64.exe` / `_x86.exe`.

**Інкрементальний цикл збірки/тесту** (після першого повного конфіга):
```powershell
# один раз конфігуруємо + збираємо тести:
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
# далі швидкий цикл — тільки наша ціль:
cmake --build build_x64 --config Release --target ecr_privatjson_selftest
./bin/Release/ecr_privatjson_selftest_x64.exe    # exit 0 = всі CHECK пройшли
```

---

### Task 1: `ResultEnvelope` + OBJECT-ліба `platform_component` + бутстрап селф-тесту

**Files:**
- Create: `src/platform/ResultEnvelope.h`
- Create: `src/platform/ResultEnvelope.cpp`
- Create: `tests/ecr_privatjson_selftest.cpp`
- Modify: `CMake/components.cmake` (нова OBJECT-ліба `platform_component`)
- Modify: `tests/CMakeLists.txt` (нова ціль `ecr_privatjson_selftest`, ДО UAPKI-гейта)

**Interfaces:**
- Produces: `struct ResultEnvelope { bool ok; std::string code; std::string description; nlohmann::json payload; };` та `nlohmann::json ResultEnvelope::ToJson() const;` (`{"ok":...,"code":...,"description":...,"payload":...}`).

- [X] **Step 1: Створити `src/platform/ResultEnvelope.h`**

```cpp
#pragma once
// ResultEnvelope — уніфікований конверт результату операції драйвера (спека §3.1/§7).
#include <string>
#include <nlohmann/json.hpp>

struct ResultEnvelope {
    bool ok = false;
    std::string code;          ///< машинний код ("OK", "TIMEOUT", "DEVICE_BUSY", "1004", ...)
    std::string description;   ///< людиночитний опис
    nlohmann::json payload = nlohmann::json::object();  ///< корисне навантаження операції

    /// Серіалізація для 1С (ПолучитьРезультатJSON): {ok, code, description, payload}.
    nlohmann::json ToJson() const;

    static ResultEnvelope Ok(nlohmann::json payload = nlohmann::json::object()) {
        return ResultEnvelope{ true, "OK", "", std::move(payload) };
    }
    static ResultEnvelope Fail(std::string code, std::string description) {
        return ResultEnvelope{ false, std::move(code), std::move(description), nlohmann::json::object() };
    }
};
```

- [X] **Step 2: Створити `src/platform/ResultEnvelope.cpp`**

```cpp
#include "../core/pch.h"
#include "ResultEnvelope.h"

nlohmann::json ResultEnvelope::ToJson() const {
    nlohmann::json j;
    j["ok"] = ok;
    j["code"] = code;
    j["description"] = description;
    j["payload"] = payload;
    return j;
}
```

- [X] **Step 3: Створити `tests/ecr_privatjson_selftest.cpp` з CHECK-харнесом і першим тестом**

```cpp
// ecr_privatjson_selftest — харнес пілотного драйвера ECRPrivatJSON (кодек/класифікатор/
// емулятор/e2e) без 1С і без UAPKI. Свій хенд-ролед раннер (як wire_selftest). pch тут НЕ підключаємо.
#include "../src/platform/ResultEnvelope.h"
#include <cstdio>
#include <string>

static int g_failed = 0;
#define CHECK(c,n) do{ if(c){std::printf("[PASS] %s\n",n);} else {std::printf("[FAIL] %s\n",n);++g_failed;} }while(0)

static void TestResultEnvelope() {
    auto ok = ResultEnvelope::Ok({{"invoiceNumber", "42"}});
    auto j = ok.ToJson();
    CHECK(j["ok"] == true && j["code"] == "OK" && j["payload"]["invoiceNumber"] == "42",
          "ResultEnvelope::Ok → {ok:true, code:OK, payload}");

    auto fail = ResultEnvelope::Fail("TIMEOUT", "Немає відповіді термінала");
    auto jf = fail.ToJson();
    CHECK(jf["ok"] == false && jf["code"] == "TIMEOUT" && jf["description"] == "Немає відповіді термінала",
          "ResultEnvelope::Fail → {ok:false, code, description}");
}

int main() {
    TestResultEnvelope();
    std::printf(g_failed ? "\nFAILED: %d\n" : "\nOK\n", g_failed);
    return g_failed ? 1 : 0;
}
```

- [X] **Step 4: Додати OBJECT-лібу `platform_component` у `CMake/components.cmake`**

Після блоку `add_library(wire_component OBJECT ...)` (≈ рядок 153) додати:
```cmake
add_library(platform_component OBJECT
    src/platform/ResultEnvelope.h
    src/platform/ResultEnvelope.cpp
)
target_include_directories(platform_component PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${SPDLOG_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
add_dependencies(platform_component base_component spdlog nlohmann_json)
```
І додати `$<TARGET_OBJECTS:platform_component>` у `add_library(${TARGET} SHARED ...)` (≈ рядок 248, поряд із `$<TARGET_OBJECTS:wire_component>`).

- [X] **Step 5: Додати ціль `ecr_privatjson_selftest` у `tests/CMakeLists.txt` ДО UAPKI-гейта**

Після блоку `wire_selftest` і **перед** `if(NOT BUILD_WITH_UAPKI)` (≈ рядок 83) додати:
```cmake
# ecr_privatjson_selftest — харнес пілотного драйвера ECRPrivatJSON (без UAPKI).
add_executable(ecr_privatjson_selftest ecr_privatjson_selftest.cpp
    $<TARGET_OBJECTS:platform_component>
    $<TARGET_OBJECTS:helpers_component>
    $<TARGET_OBJECTS:base_component>
)
set_target_properties(ecr_privatjson_selftest PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ecr_privatjson_selftest${_TEST_ARCH_SUFFIX}"
)
target_include_directories(ecr_privatjson_selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    ${SPDLOG_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_compile_definitions(ecr_privatjson_selftest PRIVATE _WINDOWS UNICODE _UNICODE)
target_link_libraries(ecr_privatjson_selftest PRIVATE spdlog::spdlog ws2_32)
if(MSVC)
    target_compile_options(ecr_privatjson_selftest PRIVATE /utf-8)
endif()
```

- [X] **Step 6: Зібрати й запустити — має пройти**

Run:
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
./bin/Release/ecr_privatjson_selftest_x64.exe
```
Expected: `[PASS] ResultEnvelope::Ok ...`, `[PASS] ResultEnvelope::Fail ...`, `OK`, exit 0.

- [X] **Step 7: Commit**

```bash
git add src/platform/ResultEnvelope.h src/platform/ResultEnvelope.cpp tests/ecr_privatjson_selftest.cpp CMake/components.cmake tests/CMakeLists.txt
git commit -m "feat(ecr): ResultEnvelope + platform_component + селф-тест-харнес"
```

---

### Task 2: `EcrJsonCodec` — Build/Parse кадрів JSON

**Files:**
- Create: `src/drivers/ecr_privatjson/EcrJsonCodec.h`
- Create: `src/drivers/ecr_privatjson/EcrJsonCodec.cpp`
- Modify: `CMake/components.cmake` (нова OBJECT-ліба `driver_ecr_privatjson_component`)
- Modify: `tests/CMakeLists.txt` (додати `$<TARGET_OBJECTS:driver_ecr_privatjson_component>` до `ecr_privatjson_selftest`)
- Test: `tests/ecr_privatjson_selftest.cpp`

**Interfaces:**
- Consumes: nlohmann_json.
- Produces:
  - `struct ParsedResponse { std::string method; int step = 0; nlohmann::json params; bool error = false; std::string errorDescription; std::string msgType; /* params.msgType, якщо method=="ServiceMessage" */ bool valid = false; };`
  - `std::vector<uint8_t> EcrJsonCodec::BuildRequest(const std::string& method, int step, const nlohmann::json& params);` — JSON-байти БЕЗ делімітера.
  - `ParsedResponse EcrJsonCodec::Parse(const std::vector<uint8_t>& frameNoDelimiter);` — при невалідному JSON повертає `{valid=false}`.
  - `bool EcrJsonCodec::PeekMethod(const std::vector<uint8_t>& frameNoDelimiter, std::string& method, std::string& msgType);` — легкий парс лише `method` (+`params.msgType`, якщо ServiceMessage) для класифікатора.

- [X] **Step 1: Написати падаючі тести (додати у `ecr_privatjson_selftest.cpp`)**

Додати `#include "../src/drivers/ecr_privatjson/EcrJsonCodec.h"` до інклудів і функцію:
```cpp
static std::vector<uint8_t> Bytes(const std::string& s){ return {s.begin(), s.end()}; }

static void TestEcrJsonCodec() {
    // BuildRequest: PingDevice без params → точні байти (nlohmann сортує ключі: method<step)
    auto ping = EcrJsonCodec::BuildRequest("PingDevice", 0, nullptr);
    CHECK(ping == Bytes(R"({"method":"PingDevice","step":0})"),
          "BuildRequest PingDevice → exact bytes, без делімітера");

    // Parse: повна відповідь Purchase
    auto pr = EcrJsonCodec::Parse(Bytes(
        R"({"method":"Purchase","step":0,"params":{"invoiceNumber":"42"},"error":false,"errorDescription":""})"));
    CHECK(pr.valid && pr.method == "Purchase" && pr.error == false && pr.params["invoiceNumber"] == "42",
          "Parse Purchase → поля method/error/params");

    // Parse: ServiceMessage deviceBusy → msgType заповнено
    auto db = EcrJsonCodec::Parse(Bytes(
        R"({"method":"ServiceMessage","step":0,"params":{"msgType":"deviceBusy"},"error":false,"errorDescription":""})"));
    CHECK(db.valid && db.method == "ServiceMessage" && db.msgType == "deviceBusy",
          "Parse ServiceMessage → msgType=deviceBusy");

    // PeekMethod: легкий парс
    std::string m, mt;
    CHECK(EcrJsonCodec::PeekMethod(Bytes(R"({"method":"ServiceMessage","params":{"msgType":"identify"}})"), m, mt)
          && m == "ServiceMessage" && mt == "identify", "PeekMethod → method+msgType");

    // Parse невалідного JSON → valid=false (без винятку)
    CHECK(EcrJsonCodec::Parse(Bytes("{not json")).valid == false, "Parse невалідного → valid=false, без кидка");
}
```
Додати виклик `TestEcrJsonCodec();` у `main()` перед виводом підсумку.

- [X] **Step 2: Запустити — має впасти (немає `EcrJsonCodec.h`)**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest`
Expected: FAIL збірки — `Cannot open include file: '.../EcrJsonCodec.h'`.

- [X] **Step 3: Створити `src/drivers/ecr_privatjson/EcrJsonCodec.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

/// Розібрана відповідь термінала (кадр без делімітера).
struct ParsedResponse {
    std::string method;
    int step = 0;
    nlohmann::json params = nlohmann::json::object();
    bool error = false;
    std::string errorDescription;
    std::string msgType;   ///< params.msgType, якщо method=="ServiceMessage"; інакше порожній
    bool valid = false;    ///< false → JSON не розібрано
};

/// Кодек прикладного рівня ECR ПриватБанк: JSON <-> байти. Делімітером 0x00 НЕ оперує
/// (це робить NullTerminatedFramer); працює з payload без термінатора.
class EcrJsonCodec {
public:
    /// {method, step[, params]} → UTF-8 JSON-байти без делімітера. params==nullptr → без поля params.
    static std::vector<uint8_t> BuildRequest(const std::string& method, int step,
                                             const nlohmann::json& params);
    /// Повний розбір кадру. Невалідний JSON → {valid=false} (без винятку).
    static ParsedResponse Parse(const std::vector<uint8_t>& frameNoDelimiter);
    /// Легкий парс лише method (+ params.msgType для ServiceMessage). false → не розібрано.
    static bool PeekMethod(const std::vector<uint8_t>& frameNoDelimiter,
                           std::string& method, std::string& msgType);
};
```

- [X] **Step 4: Створити `src/drivers/ecr_privatjson/EcrJsonCodec.cpp`**

```cpp
#include "../../core/pch.h"
#include "EcrJsonCodec.h"

std::vector<uint8_t> EcrJsonCodec::BuildRequest(const std::string& method, int step,
                                                const nlohmann::json& params) {
    nlohmann::json j;
    j["method"] = method;
    j["step"] = step;
    if (!params.is_null()) j["params"] = params;
    std::string s = j.dump();   // UTF-8, ключі впорядковані (method < params < step)
    return std::vector<uint8_t>(s.begin(), s.end());
}

ParsedResponse EcrJsonCodec::Parse(const std::vector<uint8_t>& frame) {
    ParsedResponse r;
    auto j = nlohmann::json::parse(frame.begin(), frame.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return r;   // valid лишається false
    r.method = j.value("method", std::string{});
    r.step = j.value("step", 0);
    r.params = j.value("params", nlohmann::json::object());
    r.error = j.value("error", false);
    r.errorDescription = j.value("errorDescription", std::string{});
    if (r.method == "ServiceMessage" && r.params.is_object())
        r.msgType = r.params.value("msgType", std::string{});
    r.valid = true;
    return r;
}

bool EcrJsonCodec::PeekMethod(const std::vector<uint8_t>& frame,
                              std::string& method, std::string& msgType) {
    auto j = nlohmann::json::parse(frame.begin(), frame.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    method = j.value("method", std::string{});
    msgType.clear();
    if (method == "ServiceMessage") {
        auto it = j.find("params");
        if (it != j.end() && it->is_object()) msgType = it->value("msgType", std::string{});
    }
    return !method.empty();
}
```

- [X] **Step 5: Додати OBJECT-лібу драйвера у `CMake/components.cmake`**

Після `platform_component` додати:
```cmake
add_library(driver_ecr_privatjson_component OBJECT
    src/drivers/ecr_privatjson/EcrJsonCodec.h
    src/drivers/ecr_privatjson/EcrJsonCodec.cpp
)
target_include_directories(driver_ecr_privatjson_component PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src
    ${SPDLOG_INCLUDE_DIR} ${NLOHMANN_JSON_INCLUDE_DIR}
)
add_dependencies(driver_ecr_privatjson_component base_component spdlog nlohmann_json)
```
І `$<TARGET_OBJECTS:driver_ecr_privatjson_component>` — у SHARED-ціль (поряд із `platform_component`).

- [X] **Step 6: Додати обʼєкти драйвера в тестову ціль (`tests/CMakeLists.txt`)**

У `add_executable(ecr_privatjson_selftest ...)` додати рядок:
```cmake
    $<TARGET_OBJECTS:driver_ecr_privatjson_component>
```

- [X] **Step 7: Зібрати й запустити — має пройти**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest && ./bin/Release/ecr_privatjson_selftest_x64.exe`
Expected: усі `[PASS]` для `TestEcrJsonCodec`, exit 0.

- [X] **Step 8: Commit**

```bash
git add src/drivers/ecr_privatjson/EcrJsonCodec.h src/drivers/ecr_privatjson/EcrJsonCodec.cpp tests/ecr_privatjson_selftest.cpp CMake/components.cmake tests/CMakeLists.txt
git commit -m "feat(ecr): EcrJsonCodec (Build/Parse/PeekMethod) + OBJECT-ліба драйвера"
```

---

### Task 3: `EcrPrivatJsonClassifier` — кореляція кадрів

**Files:**
- Create: `src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.h`
- Create: `src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.cpp`
- Modify: `CMake/components.cmake` (додати файли до `driver_ecr_privatjson_component`)
- Test: `tests/ecr_privatjson_selftest.cpp`

**Interfaces:**
- Consumes: `IFrameClassifier` (`src/transport/IFrameClassifier.h`), `EcrJsonCodec`.
- Produces: `class EcrPrivatJsonClassifier : public IFrameClassifier { Classification Classify(const PendingView&, const std::vector<uint8_t>&) override; };` — логіка §5 спеки.

- [X] **Step 1: Написати падаючі тести (додати у `ecr_privatjson_selftest.cpp`)**

Додати `#include "../src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.h"` і `#include "../src/transport/IFrameClassifier.h"`, тоді:
```cpp
static void TestEcrClassifier() {
    EcrPrivatJsonClassifier clf;
    auto purchaseReq = EcrJsonCodec::BuildRequest("Purchase", 0, {{"amount","1.00"}});
    auto identifyReq = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType","identify"}});
    auto correctReq  = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType","correctTransaction"},{"amount","0.50"}});
    auto interruptReq= EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType","interrupt"}});
    auto statReq     = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType","getLastStatMsgCode"}});

    auto F = [](const char* s){ return std::vector<uint8_t>(std::string(s).begin(), std::string(s).end()); };

    // 1) primary-відповідь: method збігається з primary
    { PendingView pv{ &purchaseReq, nullptr };
      auto c = clf.Classify(pv, F(R"({"method":"Purchase","step":0,"params":{},"error":false})"));
      CHECK(c.cls == FrameClass::PrimaryResponse, "Classify: Purchase-відповідь → PrimaryResponse"); }

    // 2) service-відповідь: identify під час identify
    { PendingView pv{ nullptr, &identifyReq };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"identify","vendor":"PAX"}})"));
      CHECK(c.cls == FrameClass::ServiceResponse, "Classify: identify-відповідь → ServiceResponse"); }

    // 3) correctionTransmitted — ВІДПОВІДЬ на correctTransaction (service), НЕ Unsolicited
    { PendingView pv{ &purchaseReq, &correctReq };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"correctionTransmitted"}})"));
      CHECK(c.cls == FrameClass::ServiceResponse, "Classify: correctionTransmitted → ServiceResponse"); }

    // 4) interruptTransmitted — відповідь на interrupt (service)
    { PendingView pv{ &purchaseReq, &interruptReq };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"interruptTransmitted"}})"));
      CHECK(c.cls == FrameClass::ServiceResponse, "Classify: interruptTransmitted → ServiceResponse"); }

    // 5) deviceBusy за наявного primary → RejectPrimary{Busy}
    { PendingView pv{ &purchaseReq, nullptr };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"deviceBusy"}})"));
      CHECK(c.cls == FrameClass::RejectPrimary && c.reason == RejectReason::Busy,
            "Classify: deviceBusy+primary → RejectPrimary/Busy"); }

    // 6) methodNotImplemented при ОБОХ pending → RejectBoth (кадр не називає метод)
    { PendingView pv{ &purchaseReq, &statReq };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"methodNotImplemented"}})"));
      CHECK(c.cls == FrameClass::RejectBoth && c.reason == RejectReason::Unsupported,
            "Classify: methodNotImplemented+обидва → RejectBoth/Unsupported"); }

    // 7) немає відповідного pending → Unsolicited
    { PendingView pv{ nullptr, nullptr };
      auto c = clf.Classify(pv, F(R"({"method":"ServiceMessage","params":{"msgType":"deviceBusy"}})"));
      CHECK(c.cls == FrameClass::Unsolicited, "Classify: deviceBusy без primary → Unsolicited"); }
}
```
Додати виклик `TestEcrClassifier();` у `main()`.

- [X] **Step 2: Запустити — має впасти (немає класифікатора)**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest`
Expected: FAIL — `Cannot open include file: '.../EcrPrivatJsonClassifier.h'`.

- [X] **Step 3: Створити `src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.h`**

```cpp
#pragma once
#include "../../transport/IFrameClassifier.h"

/// Класифікатор кадрів ECR ПриватБанк (спека §5): кореляція за method/msgType,
/// deviceBusy→RejectPrimary, interrupt/correctTransaction відповіді→ServiceResponse,
/// methodNotImplemented→RejectBoth/single. Уся Privat-специфіка ізольована тут.
class EcrPrivatJsonClassifier : public IFrameClassifier {
public:
    Classification Classify(const PendingView& pending,
                            const std::vector<uint8_t>& frame) override;
};
```

- [X] **Step 4: Створити `src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.cpp`**

```cpp
#include "../../core/pch.h"
#include "EcrPrivatJsonClassifier.h"
#include "EcrJsonCodec.h"

namespace {
// Очікуваний msgType ВІДПОВІДІ на service-запит із заданим msgType-ЗАПИТУ.
// Більшість — тотожні; interrupt/correctTransaction мають окремий ack.
std::string ExpectedResponseMsgType(const std::string& requestMsgType) {
    if (requestMsgType == "interrupt")         return "interruptTransmitted";
    if (requestMsgType == "correctTransaction") return "correctionTransmitted";
    return requestMsgType;
}
} // namespace

Classification EcrPrivatJsonClassifier::Classify(const PendingView& pending,
                                                 const std::vector<uint8_t>& frame) {
    std::string method, msgType;
    if (!EcrJsonCodec::PeekMethod(frame, method, msgType))
        return { FrameClass::Unsolicited, RejectReason::Busy };   // не розібрано → нейтрально

    std::string pm, pmt, sm, smt;
    const bool hasP = pending.primary && EcrJsonCodec::PeekMethod(*pending.primary, pm, pmt);
    const bool hasS = pending.service && EcrJsonCodec::PeekMethod(*pending.service, sm, smt);

    if (method != "ServiceMessage") {
        // Несервісна відповідь корелює лише з primary за method.
        if (hasP && method == pm) return { FrameClass::PrimaryResponse, RejectReason::Busy };
        return { FrameClass::Unsolicited, RejectReason::Busy };
    }

    // method == "ServiceMessage": розводимо за msgType.
    if (msgType == "deviceBusy")
        return hasP ? Classification{ FrameClass::RejectPrimary, RejectReason::Busy }
                    : Classification{ FrameClass::Unsolicited, RejectReason::Busy };

    if (msgType == "methodNotImplemented") {
        // Кадр не називає відхилений метод.
        if (hasP && hasS) return { FrameClass::RejectBoth, RejectReason::Unsupported };
        if (hasS)         return { FrameClass::RejectService, RejectReason::Unsupported };
        if (hasP)         return { FrameClass::RejectPrimary, RejectReason::Unsupported };
        return { FrameClass::Unsolicited, RejectReason::Unsupported };
    }

    // Відповідь на активний service-запит (identify/getLastStatMsgCode/getDiscountName/
    // interrupt→interruptTransmitted/correctTransaction→correctionTransmitted).
    if (hasS && !smt.empty() && msgType == ExpectedResponseMsgType(smt))
        return { FrameClass::ServiceResponse, RejectReason::Busy };

    return { FrameClass::Unsolicited, RejectReason::Busy };
}
```

- [X] **Step 5: Додати файли класифікатора до `driver_ecr_privatjson_component` (`CMake/components.cmake`)**

У `add_library(driver_ecr_privatjson_component OBJECT ...)` додати:
```cmake
    src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.h
    src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.cpp
```
`add_dependencies` доповнити: `add_dependencies(driver_ecr_privatjson_component wire_component)` (класифікатор інклудить `IFrameClassifier.h` з `src/transport/`).

- [X] **Step 6: Зібрати й запустити — має пройти**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest && ./bin/Release/ecr_privatjson_selftest_x64.exe`
Expected: усі 7 `[PASS]` для `TestEcrClassifier`, exit 0.

- [X] **Step 7: Commit**

```bash
git add src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.h src/drivers/ecr_privatjson/EcrPrivatJsonClassifier.cpp tests/ecr_privatjson_selftest.cpp CMake/components.cmake
git commit -m "feat(ecr): EcrPrivatJsonClassifier — кореляція method/msgType (§5)"
```

---

### Task 4: `TerminalEmulator` — протокол-обізнаний TCP-емулятор термінала

**Files:**
- Create: `tests/support/TerminalEmulator.h`
- Create: `tests/support/TerminalEmulator.cpp`
- Modify: `tests/CMakeLists.txt` (додати `support/TerminalEmulator.cpp` до `ecr_privatjson_selftest`)
- Test: `tests/ecr_privatjson_selftest.cpp`

**Interfaces:**
- Produces: `class TerminalEmulator` з `bool Start(); int Port() const; void Stop(); void OnRequest(std::string method, std::function<std::string(const nlohmann::json& request)> responder);` — на кадр із заданим `method` викликає responder і шле його JSON-рядок назад (кадр із `0x00`). Емулятор коректно ковтає порожній провідний кадр (подвійний `0x00` хендшейку). Модель сокетів — за `RawTcpEchoServer` з `wire_selftest.cpp`.

- [X] **Step 1: Написати падаючий e2e-тест (додати у `ecr_privatjson_selftest.cpp`)**

Додати інклуди `#include "support/TerminalEmulator.h"`, `#include "../src/transport/DeviceSession.h"`, `#include "../src/transport/Transport_TCP.h"`, `#include "../src/transport/NullTerminatedFramer.h"`, і тест:
```cpp
static void TestEmulatorPing() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&) {
        return R"({"method":"PingDevice","step":0,"params":{"code":"00","responseCode":"0000"},"error":false,"errorDescription":""})";
    });
    CHECK(emu.Start(), "Emulator: стартував на ефемерному порту");

    DeviceSession session(std::make_unique<TransportTCP>("127.0.0.1", emu.Port()),
                          std::make_unique<NullTerminatedFramer>(),
                          std::make_unique<EcrPrivatJsonClassifier>());
    CHECK(session.Start(), "Emulator: DeviceSession підключився");

    auto req = EcrJsonCodec::BuildRequest("PingDevice", 0, nullptr);
    // хендшейк-кадр має провідний 0x00
    RequestResult r = session.RequestPrimary(req, 3000, FrameOptions{/*leadingDelimiter=*/true});
    CHECK(r.status == RequestStatus::Response, "Emulator: PingDevice → Response");
    auto pr = EcrJsonCodec::Parse(r.frame);
    CHECK(pr.valid && pr.method == "PingDevice" && pr.params["responseCode"] == "0000",
          "Emulator: відповідь PingDevice розібрано, responseCode=0000");

    session.Stop();
    emu.Stop();
}
```
Додати виклик `TestEmulatorPing();` у `main()`.

- [X] **Step 2: Запустити — має впасти (немає емулятора)**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest`
Expected: FAIL — `Cannot open include file: 'support/TerminalEmulator.h'`.

- [X] **Step 3: Створити `tests/support/TerminalEmulator.h`**

```cpp
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <winsock2.h>

// Протокол-обізнаний емулятор термінала ПриватБанк (лише тести).
// Приймає одне TCP-з'єднання на localhost, ріже вхід по 0x00, парсить method,
// викликає зареєстрований responder і шле його відповідь кадром із 0x00.
// Модель сокетів — за RawTcpEchoServer (wire_selftest.cpp).
class TerminalEmulator {
public:
    using Responder = std::function<std::string(const nlohmann::json& request)>;

    ~TerminalEmulator() { Stop(); }
    bool Start();
    int Port() const { return port_; }
    void Stop();

    /// Зареєструвати відповідь на кадр із заданим method (responder повертає JSON-рядок).
    void OnRequest(std::string method, Responder responder) { handlers_[std::move(method)] = std::move(responder); }

private:
    void Run();
    void HandleFrame(SOCKET c, const std::vector<uint8_t>& frame);

    std::map<std::string, Responder> handlers_;
    std::atomic<SOCKET> listen_{ INVALID_SOCKET };
    std::atomic<SOCKET> client_{ INVALID_SOCKET };
    std::thread thread_;
    std::atomic<bool> running_{ false };
    bool started_ = false;
    int port_ = 0;
};
```

- [X] **Step 4: Створити `tests/support/TerminalEmulator.cpp`**

```cpp
#include "TerminalEmulator.h"
#include <ws2tcpip.h>

bool TerminalEmulator::Start() {
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

    running_.store(true);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void TerminalEmulator::Stop() {
    running_.store(false);
    SOCKET l = listen_.exchange(INVALID_SOCKET);
    if (l != INVALID_SOCKET) { shutdown(l, SD_BOTH); closesocket(l); }
    SOCKET c = client_.exchange(INVALID_SOCKET);
    if (c != INVALID_SOCKET) { shutdown(c, SD_BOTH); closesocket(c); }
    if (thread_.joinable()) thread_.join();
    if (started_) { WSACleanup(); started_ = false; }
}

void TerminalEmulator::Run() {
    SOCKET c = accept(listen_.load(), nullptr, nullptr);
    if (c == INVALID_SOCKET) return;
    client_.store(c);
    if (!running_.load()) { closesocket(c); client_.store(INVALID_SOCKET); return; }

    std::vector<uint8_t> buf;
    char tmp[4096];
    while (running_.load()) {
        int n = recv(c, tmp, static_cast<int>(sizeof(tmp)), 0);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            if (tmp[i] == 0) {
                if (!buf.empty()) HandleFrame(c, buf);   // порожній (провідний/подвійний 0x00) — ігнор
                buf.clear();
            } else {
                buf.push_back(static_cast<uint8_t>(tmp[i]));
            }
        }
    }
}

void TerminalEmulator::HandleFrame(SOCKET c, const std::vector<uint8_t>& frame) {
    auto j = nlohmann::json::parse(frame.begin(), frame.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    std::string method = j.value("method", std::string{});
    auto it = handlers_.find(method);
    if (it == handlers_.end()) return;   // нема сценарію — мовчимо

    std::string resp = it->second(j);
    std::vector<uint8_t> out(resp.begin(), resp.end());
    out.push_back(0);   // термінатор кадру
    int off = 0, total = static_cast<int>(out.size());
    while (off < total) {
        int s = ::send(c, reinterpret_cast<const char*>(out.data()) + off, total - off, 0);
        if (s <= 0) break;
        off += s;
    }
}
```

- [X] **Step 5: Додати емулятор до тестової цілі (`tests/CMakeLists.txt`)**

У `add_executable(ecr_privatjson_selftest ...)` додати початковим файлом:
```cmake
    support/TerminalEmulator.cpp
```
І додати обʼєкти реального транспорту (тест конструює `TransportTCP`+`DeviceSession`):
```cmake
    $<TARGET_OBJECTS:transport_component>
    $<TARGET_OBJECTS:wire_component>
```
Лінкування вже містить `ws2_32`; додати `ixwebsocket` (тягне `transport_component`) і корінь у include (для `extern/ixwebsocket`):
```cmake
target_link_libraries(ecr_privatjson_selftest PRIVATE spdlog::spdlog ixwebsocket ws2_32)
target_include_directories(ecr_privatjson_selftest PRIVATE ${IXWEBSOCKET_INCLUDE_DIR})
```

- [X] **Step 6: Зібрати й запустити — має пройти**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest && ./bin/Release/ecr_privatjson_selftest_x64.exe`
Expected: `[PASS]` для `TestEmulatorPing` (Response + розбір відповіді), exit 0.

- [X] **Step 7: Commit**

```bash
git add tests/support/TerminalEmulator.h tests/support/TerminalEmulator.cpp tests/ecr_privatjson_selftest.cpp tests/CMakeLists.txt
git commit -m "feat(ecr): TerminalEmulator — протокол-обізнаний TCP-емулятор (тести)"
```

---

### Task 5: `EcrPrivatJsonDriver::Connect` — еталонна схема (перший e2e)

**Files:**
- Create: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h`
- Create: `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp`
- Modify: `CMake/components.cmake` (додати файли драйвера до `driver_ecr_privatjson_component`; `add_dependencies` на `transport_component`)
- Test: `tests/ecr_privatjson_selftest.cpp`

**Interfaces:**
- Consumes: `DeviceSession`, `TransportTCP`, `TransportCOM`, `NullTerminatedFramer`, `EcrPrivatJsonClassifier`, `EcrJsonCodec`.
- Produces:
  - `struct EcrConnParams { enum class Kind { Tcp, Com } kind; std::string host; int tcpPort = 2000; std::string comPort; int baud = 115200; };`
  - `static bool EcrPrivatJsonDriver::ParseConnString(const std::string& s, EcrConnParams& out);`
  - `class EcrPrivatJsonDriver { public: bool Connect(const std::string& connString); void Disconnect(); bool IsConnected() const; std::string Vendor() const; std::string Model() const; };`
  - Внутрішня фабрика: `std::unique_ptr<ITransport> MakeTransport(const EcrConnParams&);`

- [X] **Step 1: Написати падаючі тести (додати у `ecr_privatjson_selftest.cpp`)**

Додати `#include "../src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h"`, тоді:
```cpp
static void TestConnStringParse() {
    EcrConnParams p;
    CHECK(EcrPrivatJsonDriver::ParseConnString("tcp://192.168.0.10:2000", p)
          && p.kind == EcrConnParams::Kind::Tcp && p.host == "192.168.0.10" && p.tcpPort == 2000,
          "ParseConnString: tcp://host:port");
    EcrConnParams c;
    CHECK(EcrPrivatJsonDriver::ParseConnString("COM3:115200,8,N,1", c)
          && c.kind == EcrConnParams::Kind::Com && c.comPort == "COM3" && c.baud == 115200,
          "ParseConnString: COM3:115200,8,N,1");
    EcrConnParams bad;
    CHECK(EcrPrivatJsonDriver::ParseConnString("garbage", bad) == false, "ParseConnString: сміття → false");
}

static void TestConnectReferenceScheme() {
    TerminalEmulator emu;
    emu.OnRequest("PingDevice", [](const nlohmann::json&) {
        return R"({"method":"PingDevice","step":0,"params":{"code":"00","responseCode":"0000"},"error":false,"errorDescription":""})";
    });
    emu.OnRequest("ServiceMessage", [](const nlohmann::json& req) -> std::string {
        // Identify: відповідь із vendor/model
        if (req.contains("params") && req["params"].value("msgType","") == "identify")
            return R"({"method":"ServiceMessage","step":0,"params":{"msgType":"identify","result":"OK","vendor":"PAX","model":"s800"},"error":false,"errorDescription":""})";
        return "";
    });
    CHECK(emu.Start(), "Connect: емулятор стартував");

    EcrPrivatJsonDriver drv;
    CHECK(drv.Connect(std::string("tcp://127.0.0.1:") + std::to_string(emu.Port())),
          "Connect: еталонна схема (Ping+dc→Identify+dc→постійний конект) → true");
    CHECK(drv.IsConnected(), "Connect: постійна сесія на зв'язку");
    CHECK(drv.Vendor() == "PAX" && drv.Model() == "s800", "Connect: збережено vendor/model з Identify");

    drv.Disconnect();
    CHECK(!drv.IsConnected(), "Disconnect: сесію закрито");
    emu.Stop();
}
```
Додати виклики `TestConnStringParse();` і `TestConnectReferenceScheme();` у `main()`.

> **Примітка про емулятор:** `TerminalEmulator::Run` обробляє одне з'єднання й завершується при закритті сокета. Еталонна схема робить кілька конектів поспіль (Ping, Identify, постійний), тож на Кроці 3 драйвера емулятор має приймати **послідовні** з'єднання. Це вже покрито в реалізації нижче (див. Step 3 драйвера — цикл `accept` в емуляторі оновлюється).

- [X] **Step 2: Оновити `TerminalEmulator` на послідовні з'єднання**

У `tests/support/TerminalEmulator.cpp` замінити тіло `Run()` так, щоб після завершення одного клієнта приймати наступного, доки `running_`:
```cpp
void TerminalEmulator::Run() {
    while (running_.load()) {
        SOCKET c = accept(listen_.load(), nullptr, nullptr);
        if (c == INVALID_SOCKET) return;              // listen-сокет закрито у Stop()
        client_.store(c);
        if (!running_.load()) { closesocket(c); client_.store(INVALID_SOCKET); return; }

        std::vector<uint8_t> buf; char tmp[4096];
        while (running_.load()) {
            int n = recv(c, tmp, static_cast<int>(sizeof(tmp)), 0);
            if (n <= 0) break;                        // клієнт закрив (dc еталонної схеми)
            for (int i = 0; i < n; ++i) {
                if (tmp[i] == 0) { if (!buf.empty()) HandleFrame(c, buf); buf.clear(); }
                else buf.push_back(static_cast<uint8_t>(tmp[i]));
            }
        }
        SOCKET old = client_.exchange(INVALID_SOCKET);
        if (old != INVALID_SOCKET) closesocket(old);   // закрити цього клієнта, чекати наступного
    }
}
```

- [X] **Step 3: Запустити — має впасти (немає драйвера)**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest`
Expected: FAIL — `Cannot open include file: '.../EcrPrivatJsonDriver.h'`.

- [X] **Step 4: Створити `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h`**

```cpp
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <chrono>

class DeviceSession;
class ITransport;

/// Розібраний рядок підключення.
struct EcrConnParams {
    enum class Kind { Tcp, Com } kind = Kind::Tcp;
    std::string host;        ///< для Tcp
    int tcpPort = 2000;      ///< для Tcp
    std::string comPort;     ///< для Com, напр. "COM3"
    int baud = 115200;       ///< для Com
};

/// Пілотний драйвер ECRPrivatJSON: розбір підключення, фабрика транспорту, життєвий
/// цикл за еталонною схемою (спека §6). Операції — Частина 2.
class EcrPrivatJsonDriver {
public:
    EcrPrivatJsonDriver() = default;
    ~EcrPrivatJsonDriver();

    /// tcp://host:port | COMn:115200,8,N,1 → out. false, якщо не розібрано.
    static bool ParseConnString(const std::string& s, EcrConnParams& out);

    /// Еталонна схема: Ping(+dc) → Identify(+dc) → постійний конект. true — на зв'язку.
    bool Connect(const std::string& connString);
    void Disconnect();
    bool IsConnected() const;

    std::string Vendor() const;
    std::string Model() const;

    // Send-арбітр (спека §7): мін. інтервал 0.1с між ФАКТИЧНИМИ відправленнями.
    // Використовуватиметься в Частині 2; тут — інфраструктура.
    void GateSend();   // блокує до дозволеного моменту, оновлює мітку

private:
    std::unique_ptr<ITransport> MakeTransport(const EcrConnParams& p) const;
    /// Зібрати нову DeviceSession з колбеками (ставляться ДО Start()).
    std::unique_ptr<DeviceSession> MakeSession(const EcrConnParams& p);

    EcrConnParams params_{};
    std::unique_ptr<DeviceSession> session_;   ///< постійна сесія (після Connect)
    std::string vendor_, model_;

    mutable std::mutex sendGateMutex_;
    std::chrono::steady_clock::time_point lastSend_{};
    static constexpr int kSendGapMs = 100;     ///< 0.1с між командами (спека)
};
```

- [X] **Step 5: Створити `src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp`**

```cpp
#include "../../core/pch.h"
#include "EcrPrivatJsonDriver.h"
#include "EcrJsonCodec.h"
#include "EcrPrivatJsonClassifier.h"
#include "../../transport/DeviceSession.h"
#include "../../transport/NullTerminatedFramer.h"
#include "../../transport/Transport_TCP.h"
#include "../../transport/Transport_COM.h"
#include "../../helpers/ServiceTools.h"
#include <thread>

namespace {
constexpr int kHandshakeTimeoutMs = 5000;   // Ping/Identify — з запасом (Verifone 3-5с)
constexpr int kPostPingPauseMs    = 1000;   // пауза 1с після Ping (спека §3.4)
}

EcrPrivatJsonDriver::~EcrPrivatJsonDriver() { Disconnect(); }

bool EcrPrivatJsonDriver::ParseConnString(const std::string& s, EcrConnParams& out) {
    if (s.rfind("tcp://", 0) == 0) {
        auto rest = s.substr(6);
        auto colon = rest.rfind(':');
        if (colon == std::string::npos) return false;
        out.kind = EcrConnParams::Kind::Tcp;
        out.host = rest.substr(0, colon);
        try { out.tcpPort = std::stoi(rest.substr(colon + 1)); } catch (...) { return false; }
        return !out.host.empty() && out.tcpPort > 0;
    }
    if (s.rfind("COM", 0) == 0) {
        auto colon = s.find(':');
        out.kind = EcrConnParams::Kind::Com;
        out.comPort = (colon == std::string::npos) ? s : s.substr(0, colon);
        out.baud = 115200;
        if (colon != std::string::npos) {
            auto tail = s.substr(colon + 1);           // "115200,8,N,1"
            auto comma = tail.find(',');
            try { out.baud = std::stoi(tail.substr(0, comma)); } catch (...) { return false; }
        }
        return true;
    }
    return false;
}

std::unique_ptr<ITransport> EcrPrivatJsonDriver::MakeTransport(const EcrConnParams& p) const {
    if (p.kind == EcrConnParams::Kind::Tcp)
        return std::make_unique<TransportTCP>(p.host, p.tcpPort);
    // COM: драйвер ЯВНО передає baud (дефолт TransportCOM = 9600), 8N1.
    return std::make_unique<TransportCOM>(p.comPort, p.baud, 8, 'N', 1.0f);
}

std::unique_ptr<DeviceSession> EcrPrivatJsonDriver::MakeSession(const EcrConnParams& p) {
    auto s = std::make_unique<DeviceSession>(MakeTransport(p),
                                             std::make_unique<NullTerminatedFramer>(),
                                             std::make_unique<EcrPrivatJsonClassifier>());
    // Колбеки — ЛИШЕ до Start() (DeviceSession: після Start — no-op+WARN).
    s->SetUnsolicitedHandler([](std::vector<uint8_t>) { /* deviceBusy/нотифікації — Частина 2 */ });
    s->SetConnectionStateHandler([](bool) { /* стан зв'язку — Частина 2 (події в 1С) */ });
    return s;
}

void EcrPrivatJsonDriver::GateSend() {
    std::lock_guard<std::mutex> lk(sendGateMutex_);
    auto now = std::chrono::steady_clock::now();
    auto next = lastSend_ + std::chrono::milliseconds(kSendGapMs);
    if (now < next) std::this_thread::sleep_for(next - now);
    lastSend_ = std::chrono::steady_clock::now();
}

bool EcrPrivatJsonDriver::Connect(const std::string& connString) {
    if (!ParseConnString(connString, params_)) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Невірний рядок підключення: " + connString);
        return false;
    }
    Disconnect();

    // 1) Хендшейк: коротка сесія, Ping із провідним 0x00.
    {
        auto hs = MakeSession(params_);
        if (!hs->Start()) { NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Хендшейк: не вдалося відкрити зв'язок"); return false; }
        auto ping = EcrJsonCodec::BuildRequest("PingDevice", 0, nullptr);
        GateSend();
        RequestResult r = hs->RequestPrimary(ping, kHandshakeTimeoutMs, FrameOptions{/*leadingDelimiter=*/true});
        if (r.status != RequestStatus::Response) {
            NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Хендшейк PingDevice не вдався");
            return false;
        }
        hs->Stop();   // дисконект (еталонна схема)
        std::this_thread::sleep_for(std::chrono::milliseconds(kPostPingPauseMs));
    }

    // 2) Identify: коротка сесія.
    {
        auto id = MakeSession(params_);
        if (!id->Start()) { NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Identify: не вдалося відкрити зв'язок"); return false; }
        auto ident = EcrJsonCodec::BuildRequest("ServiceMessage", 0, {{"msgType", "identify"}});
        GateSend();
        RequestResult r = id->RequestService(ident, kHandshakeTimeoutMs);
        if (r.status == RequestStatus::Response) {
            auto pr = EcrJsonCodec::Parse(r.frame);
            if (pr.valid && pr.params.is_object()) {
                vendor_ = pr.params.value("vendor", std::string{});
                model_  = pr.params.value("model", std::string{});
            }
        }
        id->Stop();   // дисконект
    }

    // 3) Постійний режим: сесія лишається відкритою (реконект — супервізор DeviceSession).
    session_ = MakeSession(params_);
    if (!session_->Start()) {
        NEUTRAL_REPORT_ERROR("ECRPrivatJSON", "Постійний режим: не вдалося відкрити зв'язок");
        session_.reset();
        return false;
    }
    return true;
}

void EcrPrivatJsonDriver::Disconnect() {
    if (session_) { session_->Stop(); session_.reset(); }
}

bool EcrPrivatJsonDriver::IsConnected() const { return session_ && session_->IsConnected(); }
std::string EcrPrivatJsonDriver::Vendor() const { return vendor_; }
std::string EcrPrivatJsonDriver::Model() const { return model_; }
```

- [X] **Step 6: Додати файли драйвера до CMake й залежності на транспорт**

У `CMake/components.cmake`, `add_library(driver_ecr_privatjson_component OBJECT ...)` додати:
```cmake
    src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h
    src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp
```
Доповнити залежності (драйвер інклудить транспорти й ServiceTools):
```cmake
add_dependencies(driver_ecr_privatjson_component transport_component helpers_component)
```

- [X] **Step 7: Зібрати й запустити — має пройти**

Run: `cmake --build build_x64 --config Release --target ecr_privatjson_selftest && ./bin/Release/ecr_privatjson_selftest_x64.exe`
Expected: `[PASS]` для `TestConnStringParse` і `TestConnectReferenceScheme` (Connect→true, IsConnected, vendor=PAX/model=s800, Disconnect), exit 0.

- [X] **Step 8: Повний гейт — переконатися, що нічого не зламано**

Run: `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests; powershell -File run_tests.ps1 -NoUapki x64`
Expected: L0.5 core + L0.6 wire + новий `ecr_privatjson_selftest` — усі зелені; підсумкова таблиця без FAIL; exit 0.

- [X] **Step 9: Commit**

```bash
git add src/drivers/ecr_privatjson/EcrPrivatJsonDriver.h src/drivers/ecr_privatjson/EcrPrivatJsonDriver.cpp tests/support/TerminalEmulator.cpp tests/ecr_privatjson_selftest.cpp CMake/components.cmake
git commit -m "feat(ecr): EcrPrivatJsonDriver.Connect — еталонна схема + перший e2e проти емулятора"
```

---

## Підсумок Частини 1

Після Task 5: **перший зелений transport-e2e** — драйвер конектиться до емулятора за еталонною схемою (Ping+dc→Identify+dc→постійний конект), зберігає vendor/model, тримає постійну сесію. Wire-спина (кодек+класифікатор+транспорт+сесія) доведена наскрізь. Готово до **Частини 2**: `JobEngine`/`OperationRegistry`, poller-потік, операції Purchase/Refund, пауза `getLastStatMsgCode==11`→`correctTransaction`, `interrupt`, `deviceBusy`, desync-відновлення, 1С-фасад `AddinECRPrivatJSON`, інтеграція рівнів L0-L3 у `run_tests.ps1`.

## Self-review (проти спеки, свіжим оком)

- **Покриття §3-§6, кроки §13.1-4:** канали/кадр (Task 2 кодек), кореляція §5 (Task 3 класифікатор — усі гілки: primary/service/deviceBusy/*Transmitted/methodNotImplemented/unsolicited), провідний 0x00 (Task 4 e2e з `FrameOptions{leadingDelimiter=true}`), еталонна схема A7 (Task 5), send-арбітр інфраструктура (Task 5 `GateSend`), колбеки до Start() (Task 5 `MakeSession`), явний baud 115200 (Task 5 `MakeTransport`). Емулятор-only (Task 4). CMake — OBJECT-ліби+`$<TARGET_OBJECTS>`, тест-ціль до UAPKI-гейта (Task 1/5).
- **Свідомо в Частині 2 (не прогалина):** `JobEngine`/`OperationRegistry`/poller-потік/операції/пауза/desync-відновлення/1С-фасад/`run_tests.ps1`-рівні. Send-арбітр тут — лише інфраструктура (`GateSend`), фактичне застосування під час полінгу — Частина 2.
- **Плейсхолдери:** відсутні — кожен крок має повний код або точну команду з очікуваним результатом.
- **Узгодженість типів:** `ParsedResponse`/`EcrConnParams`/`Classification{cls,reason}`/`RequestResult{status,frame}`/`FrameOptions{leadingDelimiter}` — імена й поля збігаються з наявними заголовками (`IFrameClassifier.h`, `RequestTypes.h`, `IFramer.h`, `DeviceSession.h`) і між задачами.

---

## Статус виконання (2026-07-21) — ✅ ВИКОНАНО

Усі Task 1-5 реалізовано; збірка обох архітектур (x86+x64) зелена, ZIP зібрано; повний гейт
`run_tests.ps1 -NoUapki` зелений на обох архітектурах: **L0.5 core + L0.6 wire + L0.7 ecr
(28 CHECK PASS)**, FAIL=0. Перший зелений transport-e2e досягнуто.

> **Продовження:** Частину 2 (операції/`JobEngine`/poller/interrupt/async + 1С-фасад
> `AddinECRPrivatJSON` (компонента `ECRPrivatJSON`) + рівень **L2-ecr** `ecr_native_host` у
> `run_tests`) **виконано** — план/звіт
> [`2026-07-21_plan_ecr_privatjson_p2_operations_and_1c.md`](2026-07-21_plan_ecr_privatjson_p2_operations_and_1c.md).

**Відхилення/уточнення проти буквального плану (свідомі):**
- **CMake-прогалина плану закрита.** Кроки Task 1/2 давали для нових OBJECT-ліб лише
  `target_include_directories`+`add_dependencies`. Додатково (як наявний `wire_component`)
  прописано `set_target_properties`(PIC+CXX17), `target_compile_definitions(_WINDOWS UNICODE
  _UNICODE)` і — головне — **`/utf-8` у `CMake/compiler_settings.cmake`** для
  `platform_component` та `driver_ecr_privatjson_component` (план цей файл не згадував; без
  `/utf-8` кириличні коментарі/літерали зламали б MSVC).
- **Баг у тест-коді плану виправлено.** Лямбда `F` у `TestEcrClassifier` викликала
  `std::string(s)` двічі → `begin()`/`end()` від РІЗНИХ тимчасових об'єктів → несумісні
  ітератори (UB) → відкладена heap-corruption / `0xC0000409`. Замінено на один іменований
  `std::string t(s)` (як у `Bytes()`).
- **Код-рев'ю (3 лінзи) — усі зауваження закрито:**
  - *critical:* `nlohmann::json::value<T>()` кидає `type_error(302)` при типовій невідповідності
    поля попри `allow_exceptions=false` — `Parse()`/`PeekMethod()` обгорнуто в `try/catch`
    (виняток не перетинає межу 1С; тихо `valid=false` за контрактом). Додано регресійні CHECK.
  - `BuildRequest` `dump()` → `error_handler::replace` (не кидає на невалідному UTF-8).
  - `vendor_`/`model_` скидаються на початку `Connect` (стала ідентичність через реконект).
  - `TerminalEmulator` TOCTOU подвійного `closesocket` усунено.
  - `ParseConnString` — строга валідація (хвостове сміття порту/baud → відхилення; порожній
    baud `COM3:` → дефолт 115200); IPv6/COM-8N1-обмеження задокументовано в коментарях.
- **`run_tests.ps1`:** додано рівень **L0.7** для `ecr_privatjson_selftest` (Task 5 Step 8
  очікував ecr у виводі; повна інтеграція L0-L3 лишається Частиною 2).
- **Доки синхронізовано:** `AGENTS.md` (розділи Проєкт/Тести/Структура), `docs/architecture/README.md`
  (блок device-core, каталог #02, шари, стан-гілка), `core.md` (реєстрація компонент).

**Готово до Частини 2:** `JobEngine`/`OperationRegistry`, poller-потік, операції Purchase/Refund,
пауза/interrupt/deviceBusy/desync-відновлення, 1С-фасад `AddinECRPrivatJSON`.
