# Імітація фіскального сервера ДПС (`prro_fs_emulator`) — план імплементації

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Окремий тестовий exe `prro_fs_emulator`, що грає фіскальний сервер ДПС зі станом, форматом відповідей ДПС і керованими збоями (два штатні обриви) — для наскрізних тестів ПРРО споживача `SMP_SimplyConnect`.

**Architecture:** Чиста логіка (`FiscalServerState` — стан/нумерація/підсумки; `FaultPlan` — збої) живе окремо від HTTP і крипто й перевіряється гейтом без UAPKI (`prro_fs_state_selftest`, рівень L-f1). Крипто виноситься з `uapki_fiscal_emulator` у спільний `UapkiOracle` без зміни поведінки. `MiniHttpServer` отримує спільні заголовки (`Date`) і диспозиції «обрив» / «мовчати й обрив». `PrroFsService` склеює все в маршрути `/fs/doc`, `/fs/cmd`, `/control`; `prro_fs_emulator --self-test` проганяє сценарії споживача на рівні протоколу (рівень L-f2, з UAPKI).

**Tech Stack:** C++17 (MSVC, VS 2022), CMake ≥ 3.16, Winsock, pugixml (`extern/pugixml`), nlohmann/json (`extern/nlohmann_json`), UAPKI (статичне ядро `uapki_bundle`), PowerShell 5.1 (`run_tests.ps1`).

**Spec:** `docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md` (затверджено людиною 2026-09-24). Виконавець читає спеку й план разом; посилання `§N` без уточнення — на спеку.

## Global Constraints

- Тестові exe: **без** `src/core/pch.h`, `printf` дозволено; кожна ціль — `CXX_STANDARD 17`, `OUTPUT_NAME "<ім'я>${_TEST_ARCH_SUFFIX}"`, `target_compile_options(<ціль> PRIVATE /utf-8)` (без `/utf-8` кириличні літерали мовчки псуються).
- `WIN32_LEAN_AND_MEAN` і `NOMINMAX` — до **будь-якого** `#include`; `MiniHttpServer.h` / `PrroFsService.h` (тягнуть `winsock2.h`) — **до** `<windows.h>`.
- Порти: самотест L-f1 — `18100–18199`; самотест `prro_fs_emulator` — `18200–18299`; робочий дефолт `8099`, при зайнятому й не заданому явно — наступний вільний до `+10`. Слухати лише `127.0.0.1`.
- Базова адреса для споживача — `http://127.0.0.1:8099/fs`; `GET /ping` від кореня, тіло **рівно** `prro_fs_emulator alive`.
- Тіло `/fs/doc` і `/fs/cmd` поза `10…512000` байтів → `416`.
- Відмова (основний формат) → `400`, `text/plain; charset=utf-8`, тіло `Код помилки: <n> <СимвольнийКод>\r\n<опис>`; для коду 7 опис містить `Номер документа повинен дорівнювати N`, N — **останнє** число тексту.
- Перший фіскальний номер — `100000001`, лічильник один на сервер; `ShiftId` — окремий лічильник від `1`; `reset` скидає обидва.
- Гроші й відсотки — цілі копійки (`int64_t`); у JSON — число `копійки / 100.0` (nlohmann друкує найкоротше точне подання, напр. `150.0`, `150.05`; значення точне).
- Квитанції й `Data` тип 2 підписуються тестовим `test-diia.p12` профілем старого оракула (CAdES-BES, enveloping, `includeCert`, `includeTime`, `signAlgo 1.2.804.2.1.1.1.1.3.1.1`).
- `run_tests.ps1` зберігається в **UTF-8 з BOM** (`head -c 3 run_tests.ps1 | xxd -p` → `efbbbf`); перевіряти **лише** через `powershell.exe -File` (pwsh проблему кодування не показує).
- Збірка й гейт — **монопольні** на спільному `bin/Release`: перед `build_project.ps1` / `run_tests.ps1` — `ListAgents`, переконатись, що інша сесія не збирає; чужі процеси (емулятори) не вбивати.
- Повна збірка: `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests` (перегенеровує `version.h` — **комітити** разом із роботою задачі). Ітерації: `cmake --build build_x64 --config Release --target <ціль>` (exe лягає в `bin/Release`). Якщо `build_x64` немає: `cmake -S . -B build_x64 -A x64 -DBUILD_TESTS=ON -DBUILD_WITH_UAPKI=ON`. Ітерувати лише x64: x86 і x64 ділять `.lib` у `bin/Release` (пастка `LNK4272`); x86 — лише повною збіркою.
- Коміти: `git add <явні шляхи>` + `git commit --only -m "…" -- <ті самі шляхи>`; ніколи `-a`/`-A`; `-m` до `--`. Повідомлення закінчується трейлером атрибуції, який задає системне нагадування сесії-виконавця.
- `python scripts/check-doc-anchors.py` — перед кожним комітом, що зачіпає `docs/`, `AGENTS.md`, `.claude/skills/` **або переміщує рядки в `src/`/`tests/`** (Task 2 переміщує рядки `tests/uapki_fiscal_emulator.cpp`).
- Правило 1 `testing-rules.md`: кожен новий `CHECK` перевіряється на червоне; результат (що зламано → який `CHECK` почервонів) записується в тіло повідомлення коміту задачі.

## Review Focus

1. **Утримання одного з'єднання не блокує інші запити:** поки `/fs/doc` «мовчить» (`holdSeconds`), продукт шле `/fs/cmd` — він має відповісти одразу. → тест у Task 6 (`ScenarioDrops`, «під час утримання /fs/cmd відповідає»).
2. **JSON команди з BOM UTF-8 і пробілами перед `{`** (1С легко додає BOM при перетворенні рядка в байти) — має розпізнаватись як JSON, і в непідписаному тілі, і всередині CMS. → тест у Task 5 (`ScenarioFormats`, ServerState з BOM).
3. **`NumFiscal` / `NumLocal` числом або рядком** (в Описі — рядок у лапках для одних команд і число для інших) — обидва приймаються. → тести в Task 5 (TransactionsRegistrarState з числовим `NumFiscal`; CheckExt з `NumLocal` рядком).
4. **XML документа з BOM UTF-8 перед декларацією** (саме так лежать зразки ДПС у `prro_docs`) — розбирається, кодування визначається правильно. → тест у Task 3 (`TestParsing`, фікстура `check_sale_utf8.xml` з BOM).
5. **X-звіт на відкритій зміні без жодного чека** — `Totals` повної форми (об'єкти `Real`/`Ret` з нулями, порожні масиви `PayForm`/`Tax`, числа `ServiceInput`/`ServiceOutput`): друк споживача звертається до ключів прямо. → тест у Task 5 (`ScenarioShift`, одразу після відкриття).

---

## Структура файлів

| Файл | Дія | Відповідальність |
|---|---|---|
| `tests/support/MiniHttpServer.{h,cpp}` | змінити | + спільні заголовки, `Response::headers`, диспозиції `Abort`/`HoldThenAbort` |
| `tests/support/MiniHttpClient.{h,cpp}` | створити | сокет-клієнт для самотестів: розрізняє відповідь / розрив / таймаут |
| `tests/support/UapkiOracle.{h,cpp}` | створити | крипто (винесено з `uapki_fiscal_emulator.cpp` без змін логіки) |
| `tests/support/FiscalServerState.{h,cpp}` | створити | стан ПРРО, нумерація, збережені документи, підсумки; розбір XML; вставка `ORDERTAXNUM`; гроші; перекодування |
| `tests/support/FaultPlan.{h,cpp}` | створити | одноразові збої з фільтром цілі, постійні налаштування |
| `tests/support/PrroFsFixtures.h` | створити | завантаження шаблонів фікстур (`{N}`, `{REG}`) |
| `tests/support/PrroFsService.{h,cpp}` | створити | маршрути `/ping`, `/fs/doc`, `/fs/cmd`, `/control*`; JSON-форми; годинник сервера |
| `tests/prro_fs_emulator.cpp` | створити | CLI, bootstrap, пошук порту, життєвий цикл сервера |
| `tests/prro_fs_emulator_selftest.cpp` | створити | `--self-test`: сценарії споживача на рівні протоколу |
| `tests/prro_fs_state_selftest.cpp` | створити | рівень L-f1: транспорт + стан + підсумки + збої |
| `tests/data/prro_fs/*.xml` + `.gitattributes` | створити | синтетичні фікстури (windows-1251 і UTF-8 з BOM) |
| `tests/uapki_fiscal_emulator.cpp` | змінити | крипто → `UapkiOracle`, поведінка без змін |
| `tests/CMakeLists.txt` | змінити | цілі `prro_fs_state_selftest`, `prro_fs_emulator`; `UapkiOracle.cpp` у старий оракул |
| `run_tests.ps1` | змінити | рівні L-f1 і L-f2 |
| `docs/integration-1c/uapki.md` | змінити | новий §9.2 |
| `AGENTS.md` | змінити | цілі й рівні гейта |

**Відступ від таблиці §3 спеки (свідомий, у межах відповідальностей):** маршрутизацію й JSON-форми винесено з `prro_fs_emulator.cpp` у `tests/support/PrroFsService.{h,cpp}`, а самотест — у `tests/prro_fs_emulator_selftest.cpp`: інакше один файл тримав би CLI, маршрути й ~40 сценаріїв. **Додано до рівня L-f1** перевірку диспозицій `MiniHttpServer` (обрив/утримання/`Date` на транспортних відмовах): це найризиковіший механізм, і він не потребує UAPKI, тож заслуговує на дешевий гейт, а не лише на рівень L-f2.

---

### Task 1: `MiniHttpServer` — спільні заголовки й диспозиції; клієнт; рівень L-f1

**Files:**
- Modify: `tests/support/MiniHttpServer.h`
- Modify: `tests/support/MiniHttpServer.cpp`
- Create: `tests/support/MiniHttpClient.h`, `tests/support/MiniHttpClient.cpp`
- Create: `tests/prro_fs_state_selftest.cpp`
- Modify: `tests/CMakeLists.txt` (нова ціль перед гейтом `BUILD_WITH_UAPKI`)
- Modify: `run_tests.ps1` (рівень L-f1)

**Interfaces:**
- Consumes: —
- Produces:
  - `minihttp::HeaderList` = `std::vector<std::pair<std::string, std::string>>`
  - `enum class minihttp::Disposition { Send, Abort, HoldThenAbort }`
  - `minihttp::Response` + поля `HeaderList headers; Disposition disposition = Disposition::Send; int holdSeconds = 0;`
  - `using minihttp::CommonHeadersFn = std::function<HeaderList()>;` `void Server::SetCommonHeaders(CommonHeadersFn f);`
  - `minihttp::ClientResult { bool connected, responded, reset, timedOut; int code; std::map<std::string,std::string> headers /*нижній регістр*/; std::string body; long long elapsedMs; }`
  - `minihttp::ClientResult minihttp::RawRequest(int port, const std::string& raw, int timeoutMs);`
  - `minihttp::ClientResult minihttp::Request(int port, const std::string& method, const std::string& path, const std::string& body, const std::string& contentType, int timeoutMs);`
  - у `tests/prro_fs_state_selftest.cpp`: `static int g_failures`, макрос `CHECK(cond, msg)`, `main()` з викликами секцій (наступні задачі додають секції).

- [ ] **Step 1: Клієнт для тестів — `tests/support/MiniHttpClient.h`**

```cpp
#pragma once
// MiniHttpClient — мінімальний HTTP/1.1-клієнт для самотестів (лише loopback).
// Розрізняє три наслідки запиту, які для імітації ДПС і є предметом перевірки:
// відповідь отримано / з'єднання розірвано без відповіді / клієнтський таймаут.
// Winsock ініціалізує викликач (minihttp::InitNetwork()).
#include <map>
#include <string>

namespace minihttp {

struct ClientResult {
    bool        connected = false;   // TCP-з'єднання встановлено
    bool        responded = false;   // отримано status-line і заголовки
    bool        reset     = false;   // розрив без відповіді (RST або закриття до status-line)
    bool        timedOut  = false;   // клієнтський таймаут до будь-якої відповіді
    int         code      = 0;
    std::map<std::string, std::string> headers;   // імена — у нижньому регістрі
    std::string body;
    long long   elapsedMs = 0;
};

// Надіслати сирі байти запиту й прочитати відповідь до закриття з'єднання.
ClientResult RawRequest(int port, const std::string& raw, int timeoutMs);

// Звичайний запит: Content-Length за тілом, Connection: close.
ClientResult Request(int port, const std::string& method, const std::string& path,
                     const std::string& body, const std::string& contentType, int timeoutMs);

}  // namespace minihttp
```

- [ ] **Step 2: `tests/support/MiniHttpClient.cpp`**

```cpp
// MiniHttpClient — реалізація. Опис — у MiniHttpClient.h.
// УВАГА: pch.h тут НЕ підключається (правило tests/).
#include <winsock2.h>   // строго перед windows.h/ws2tcpip.h
#include <ws2tcpip.h>

#include "MiniHttpClient.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>

#pragma comment(lib, "ws2_32.lib")

namespace minihttp {
namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

ClientResult RawRequest(int port, const std::string& raw, int timeoutMs) {
    ClientResult r;
    const auto t0 = std::chrono::steady_clock::now();
    auto stamp = [&r, t0]() {
        r.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
    };

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { stamp(); return r; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
        closesocket(s);
        stamp();
        return r;
    }
    r.connected = true;
    const DWORD to = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));

    size_t sent = 0;
    while (sent < raw.size()) {
        const int chunk = static_cast<int>(std::min<size_t>(raw.size() - sent, 64u * 1024u));
        const int n = send(s, raw.data() + sent, chunk, 0);
        if (n == SOCKET_ERROR || n <= 0) break;
        sent += static_cast<size_t>(n);
    }

    std::string buf;
    char chunk[8192];
    for (;;) {
        const int n = recv(s, chunk, sizeof(chunk), 0);
        if (n > 0) { buf.append(chunk, static_cast<size_t>(n)); continue; }
        if (n == 0) break;                                   // штатне закриття (FIN)
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) r.timedOut = true;          // клієнтський таймаут
        else                     r.reset    = true;          // WSAECONNRESET тощо
        break;
    }
    closesocket(s);
    stamp();

    const size_t headEnd = buf.find("\r\n\r\n");
    if (buf.compare(0, 5, "HTTP/") != 0 || headEnd == std::string::npos) {
        // Відповіді не було. Закриття без жодного байта — теж розрив з погляду клієнта.
        if (!r.timedOut) r.reset = true;
        return r;
    }
    r.responded = true;
    const size_t sp = buf.find(' ');
    r.code = (sp != std::string::npos && sp < headEnd) ? std::atoi(buf.c_str() + sp + 1) : 0;

    size_t pos = buf.find("\r\n");                          // кінець status-line
    while (pos != std::string::npos && pos < headEnd) {
        const size_t eol  = buf.find("\r\n", pos + 2);
        const std::string line = buf.substr(pos + 2, eol - pos - 2);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string v = line.substr(colon + 1);
            while (!v.empty() && v.front() == ' ') v.erase(0, 1);
            r.headers[Lower(line.substr(0, colon))] = v;
        }
        pos = eol;
    }
    r.body = buf.substr(headEnd + 4);
    return r;
}

ClientResult Request(int port, const std::string& method, const std::string& path,
                     const std::string& body, const std::string& contentType, int timeoutMs) {
    std::string raw = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!contentType.empty()) raw += "Content-Type: " + contentType + "\r\n";
    raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    raw += "Connection: close\r\n\r\n";
    raw += body;
    return RawRequest(port, raw, timeoutMs);
}

}  // namespace minihttp
```

- [ ] **Step 3: Самотест L-f1 з транспортною секцією — `tests/prro_fs_state_selftest.cpp`**

```cpp
//
// @file tests/prro_fs_state_selftest.cpp
// @brief Рівень L-f1 гейта: чиста логіка імітації фіскального сервера ДПС (стан,
//        нумерація, підсумки, збої) і диспозиції MiniHttpServer. Без UAPKI, без 1С.
//        Спека: docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §10.1.
//        Критерій гейта — exit 0; кожна перевірка друкує [PASS]/[FAIL].
//
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "support/MiniHttpServer.h"   // winsock2.h — до windows.h
#include "support/MiniHttpClient.h"

#include <windows.h>

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("[FAIL] %s\n", msg); ++g_failures; } \
                              else { std::printf("[PASS] %s\n", msg); } } while(0)

// Сервер на першому вільному порту 18100..18199.
static std::unique_ptr<minihttp::Server> StartServer(minihttp::Handler h,
                                                     minihttp::CommonHeadersFn common, int& port) {
    for (int p = 18100; p < 18200; ++p) {
        std::unique_ptr<minihttp::Server> s(new minihttp::Server(p, "127.0.0.1"));
        s->SetHandler(h);
        if (common) s->SetCommonHeaders(common);
        if (s->Listen().first) { s->Start(); port = p; return s; }
    }
    port = 0;
    return nullptr;
}

static void TestTransport() {
    std::printf("== Транспорт MiniHttpServer: заголовки й диспозиції ==\n");
    static const char* kDate = "Thu, 24 Sep 2026 09:00:00 GMT";
    auto common  = []() { return minihttp::HeaderList{ { "Date", kDate } }; };
    auto handler = [](const minihttp::Request& rq) -> minihttp::Response {
        minihttp::Response r;
        if (rq.uri == "/send")   { r.body = "ok"; r.headers.push_back({ "X-Extra", "1" }); return r; }
        if (rq.uri == "/abort")  { r.disposition = minihttp::Disposition::Abort; return r; }
        if (rq.uri == "/hold")   { r.disposition = minihttp::Disposition::HoldThenAbort; r.holdSeconds = 2;  return r; }
        if (rq.uri == "/hold30") { r.disposition = minihttp::Disposition::HoldThenAbort; r.holdSeconds = 30; return r; }
        r.code = 404;
        return r;
    };
    int port = 0;
    std::unique_ptr<minihttp::Server> srv = StartServer(handler, common, port);
    CHECK(srv != nullptr, "сервер піднявся на вільному порту 18100..18199");
    if (!srv) return;

    minihttp::ClientResult a = minihttp::Request(port, "GET", "/send", "", "", 3000);
    CHECK(a.responded && a.code == 200 && a.body == "ok", "Send: відповідь 200 з тілом");
    CHECK(a.headers.count("date") == 1 && a.headers["date"] == kDate, "Send: спільний заголовок Date присутній");
    CHECK(a.headers.count("x-extra") == 1, "Send: власний заголовок відповіді (Response::headers) присутній");

    minihttp::ClientResult t = minihttp::RawRequest(port,
        "POST /x HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n", 3000);
    CHECK(t.responded && t.code == 411, "транспортна відмова 411 віддана самим сервером");
    CHECK(t.headers.count("date") == 1, "транспортна відмова теж має Date (спільні заголовки на кожній відповіді)");

    minihttp::ClientResult b = minihttp::Request(port, "GET", "/abort", "", "", 3000);
    CHECK(b.connected && !b.responded, "Abort: з'єднання було, відповіді немає");
    CHECK(b.reset && !b.timedOut, "Abort: клієнт бачить розрив (RST), а не таймаут");

    // Правило 3: утримання 2 с, клієнтський таймаут 1 с. Числа в звіті задачі — з виміру.
    minihttp::ClientResult c = minihttp::Request(port, "GET", "/hold", "", "", 1000);
    CHECK(!c.responded && c.timedOut, "HoldThenAbort: клієнт із таймаутом 1 с не отримав нічого");
    minihttp::ClientResult d = minihttp::Request(port, "GET", "/hold", "", "", 5000);
    CHECK(!d.responded && d.reset && d.elapsedMs >= 1800,
          "HoldThenAbort: після ~2 с тиші — розрив (elapsedMs >= 1800)");

    // Stop() має перервати утримання, а не чекати holdSeconds.
    std::thread cli([port]() { minihttp::Request(port, "GET", "/hold30", "", "", 40000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto t0 = std::chrono::steady_clock::now();
    srv->Stop();
    const long long stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
    cli.join();
    std::printf("  виміряно: Stop() під час утримання 30 с зайняв %lld мс\n", stopMs);
    CHECK(stopMs < 2000, "Stop() перериває утримання (< 2000 мс; без переривання — ~5000 мс стелі Stop)");
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    if (!minihttp::InitNetwork()) { std::printf("[FAIL] WSAStartup\n"); return 1; }

    TestTransport();

    minihttp::ShutdownNetwork();
    std::printf(g_failures ? "\n=== FAILED: %d ===\n" : "\n=== ALL PASS ===\n", g_failures);
    return g_failures ? 1 : 0;
}
```

- [ ] **Step 4: Ціль у `tests/CMakeLists.txt`** — вставити **перед** блоком коментаря `# Далі — цілі, що потребують крипто-ядра UAPKI` (тобто одразу після блоку `provider_contract_selftest`):

```cmake
# ---------------------------------------------------------------------------
# prro_fs_state_selftest — рівень L-f1: чиста логіка імітації фіскального сервера
# ДПС (стан, нумерація, підсумки, збої) + диспозиції MiniHttpServer. Без UAPKI:
# збирається завжди при BUILD_TESTS=ON і проходить у run_tests.ps1 -NoUapki.
# Спека: docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §10.1.
# ---------------------------------------------------------------------------
add_executable(prro_fs_state_selftest prro_fs_state_selftest.cpp
    support/MiniHttpServer.cpp
    support/MiniHttpClient.cpp
)
set_target_properties(prro_fs_state_selftest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "prro_fs_state_selftest${_TEST_ARCH_SUFFIX}"
)
target_include_directories(prro_fs_state_selftest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(prro_fs_state_selftest PRIVATE _WINDOWS UNICODE _UNICODE)
target_link_libraries(prro_fs_state_selftest PRIVATE ws2_32)
if(MSVC)
    target_compile_options(prro_fs_state_selftest PRIVATE /utf-8)
endif()
```

- [ ] **Step 5: Зібрати й переконатися, що тест ЧЕРВОНИЙ (API ще немає)**

Run: `cmake --build build_x64 --config Release --target prro_fs_state_selftest`
Expected: помилки компіляції `'HeaderList': is not a member of 'minihttp'`, `'Disposition'…`, `'SetCommonHeaders'…`.

- [ ] **Step 6: `tests/support/MiniHttpServer.h` — розширити API**

1. У блок `#include` додати `#include <vector>`.
2. Замінити визначення `struct Response { … };` цим:

```cpp
using HeaderList = std::vector<std::pair<std::string, std::string>>;

// Що зробити з з'єднанням після обробника.
enum class Disposition {
    Send,           // штатно: відповідь + shutdown(SD_SEND)
    Abort,          // закрити БЕЗ відповіді, RST (SO_LINGER {1,0}) — «обрив» для тестів
    HoldThenAbort   // мовчати holdSeconds, потім RST; Stop() перериває очікування
};

// Відповідь. Порожній reason -> сервер підставить стандартний текст для code.
struct Response {
    int         code = 200;
    std::string reason;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    HeaderList  headers;                          // додаткові заголовки (Location тощо)
    Disposition disposition = Disposition::Send;
    int         holdSeconds = 0;                  // лише для HoldThenAbort
};
```

3. Після `using Handler = std::function<Response(const Request&)>;` додати:

```cpp
// Заголовки, що додаються до КОЖНОЇ відповіді — і обробника, і власних відмов
// транспорту (400/411/413/431). Імітація ДПС ставить так заголовок Date.
using CommonHeadersFn = std::function<HeaderList()>;
```

4. У `class Server`, після `void SetHandler(Handler h);`, додати `void SetCommonHeaders(CommonHeadersFn f);`; у секцію `private:` після `void Serve(SOCKET client);` додати `HeaderList CommonHeaders() const;`, а після `Handler     handler_;` — поля:

```cpp
    CommonHeadersFn commonHeaders_;
    // Утримання з'єднання (HoldThenAbort): Stop() будить очікування через holdCv_.
    std::mutex              holdMx_;
    std::condition_variable holdCv_;
```

- [ ] **Step 7: `tests/support/MiniHttpServer.cpp` — реалізація**

1. Замінити функцію `SendRaw` (анонімний простір імен) на:

```cpp
void SendRaw(SOCKET s, int code, const std::string& reason,
             const std::string& ctype, const std::string& body, const HeaderList& extra) {
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
    head += "Content-Type: " + ctype + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto& h : extra) head += h.first + ": " + h.second + "\r\n";
    head += "Connection: close\r\n\r\n";
    if (!SendAll(s, head.data(), head.size())) return;
    if (!body.empty()) SendAll(s, body.data(), body.size());
}

// «Обрив»: SO_LINGER {1,0} — closesocket у потоці з'єднання надішле RST замість FIN.
void AbortConnection(SOCKET s) {
    linger l;
    l.l_onoff  = 1;
    l.l_linger = 0;
    setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
}
```

2. У `ReasonFor` додати гілки: `case 204: return "No Content";`, `case 302: return "Found";`, `case 409: return "Conflict";`, `case 416: return "Requested Range Not Satisfiable";`, `case 502: return "Bad Gateway";`, `case 503: return "Service Unavailable";`.

3. Після `Server::SetHandler` додати:

```cpp
void Server::SetCommonHeaders(CommonHeadersFn f) {
    commonHeaders_ = std::move(f);
}

HeaderList Server::CommonHeaders() const {
    return commonHeaders_ ? commonHeaders_() : HeaderList();
}
```

4. У `Server::Serve` кожен із чотирьох транспортних викликів `SendRaw(client, <код>, ReasonFor(<код>), "text/plain; charset=utf-8", <текст>);` (431, 400, 411, 413) доповнити шостим аргументом `CommonHeaders()`, наприклад:

```cpp
            SendRaw(client, 431, ReasonFor(431), "text/plain; charset=utf-8",
                    "headers too large", CommonHeaders());
```

5. Замінити розділ `// --- 5. Обробник ---` до кінця `Serve` (тобто від `Response resp;` до `shutdown(client, SD_SEND);` включно) на:

```cpp
    // --- 5. Обробник -----------------------------------------------------------
    Response resp;
    if (handler_) {
        resp = handler_(req);
    } else {
        resp.code = 500;
        resp.body = "no handler";
    }

    if (resp.disposition == Disposition::Abort) {
        AbortConnection(client);      // closesocket у потоці з'єднання дасть RST
        return;
    }
    if (resp.disposition == Disposition::HoldThenAbort) {
        std::unique_lock<std::mutex> lk(holdMx_);
        holdCv_.wait_for(lk, std::chrono::seconds(resp.holdSeconds),
                         [this] { return stopping_.load(); });
        lk.unlock();
        AbortConnection(client);
        return;
    }

    const std::string reason = resp.reason.empty() ? std::string(ReasonFor(resp.code)) : resp.reason;
    HeaderList all = CommonHeaders();
    all.insert(all.end(), resp.headers.begin(), resp.headers.end());
    SendRaw(client, resp.code, reason, resp.contentType, resp.body, all);

    // Дати клієнту дочитати відповідь до закриття сокета.
    shutdown(client, SD_SEND);
```

6. У `Server::Stop()` одразу після `if (stopping_.exchange(true)) return;` додати:

```cpp
    // Розбудити утримувані з'єднання (HoldThenAbort): предикат бачить stopping_.
    { std::lock_guard<std::mutex> lk(holdMx_); }
    holdCv_.notify_all();
```

- [ ] **Step 8: Зібрати й прогнати — ЗЕЛЕНИЙ**

Run: `cmake --build build_x64 --config Release --target prro_fs_state_selftest && ./bin/Release/prro_fs_state_selftest_x64.exe; echo exit=$?`
Expected: 11 рядків `[PASS]`, `=== ALL PASS ===`, `exit=0`; рядок `виміряно: Stop() … мс` — записати число.

- [ ] **Step 9: Негативна верифікація (правило 1) — кожну правку відкотити після перевірки**
  - `Serve`: у гілці `Disposition::Abort` замінити тіло на відправку звичайної відповіді (прибрати `AbortConnection(client); return;`) → мають почервоніти обидва `Abort: …`.
  - `Stop()`: прибрати `holdCv_.notify_all();` → має почервоніти `Stop() перериває утримання` (виміряти й записати ~5000 мс — це друге значення порогу за правилом 3).
  - транспортний `SendRaw(…411…)`: передати `HeaderList()` замість `CommonHeaders()` → має почервоніти `транспортна відмова теж має Date`.
  Записати три пари «зламано → CHECK» для повідомлення коміту.

- [ ] **Step 10: Регресія старого оракула** — він користується `MiniHttpServer` за замовчуванням.

Run: `cmake --build build_x64 --config Release --target uapki_fiscal_emulator`
Expected: збірка без помилок і попереджень про `SendRaw`.

- [ ] **Step 11: Рівень L-f1 у `run_tests.ps1`**

1. Після рядка `$LabelNativeHostExe = Join-Path $BinRelease ("label_native_host" + $ArchSuffix + ".exe")` додати:

```powershell
$PrroFsStateExe = Join-Path $BinRelease ("prro_fs_state_selftest" + $ArchSuffix + ".exe")
```

2. Перед блоком

```powershell
# =====================================================================
# ЕТАП 2 (L1): uapki_selftest на кожному сценарії (окремий процес)
# =====================================================================
```

вставити:

```powershell
# =====================================================================
# ЕТАП L-f1: prro_fs_state_selftest — чиста логіка імітації фіскального сервера ДПС
# (стан, нумерація, підсумки, збої) і диспозиції MiniHttpServer. Без UAPKI: збирається
# завжди при BUILD_TESTS=ON і проходить у -NoUapki. Критерій — exit 0.
# Спека: docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §10.1.
# =====================================================================
Section 'ЕТАП L-f1: prro_fs_state_selftest імітації ДПС'

if (-not (Test-Path $PrroFsStateExe)) {
    Add-Result 'L-f1' 'prro_fs_state_selftest' 'FAIL' `
        "немає prro_fs_state_selftest.exe: $PrroFsStateExe — зберіть з -WithTests (збирається завжди при BUILD_TESTS=ON, без UAPKI)"
}
else {
    $outF = Join-Path ([System.IO.Path]::GetTempPath()) ("prro_fs_state_" + [guid]::NewGuid().ToString('N').Substring(0,6) + '.out')
    $p = Start-Process -FilePath $PrroFsStateExe `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
    $txt = ''
    if (Test-Path $outF) { $txt = Get-Content -Raw $outF }
    if ($p.ExitCode -eq 0) {
        $nPassLines = ([regex]::Matches($txt, '\[PASS\]')).Count
        Add-Result 'L-f1' 'prro_fs_state_selftest' 'PASS' "усі CHECK пройшли (PASS: $nPassLines)"
    }
    else {
        $fails = ($txt -split "`n" | Where-Object { $_ -match '\[FAIL\]' }) -join ' | '
        Add-Result 'L-f1' 'prro_fs_state_selftest' 'FAIL' "exit=$($p.ExitCode) $fails"
    }
    Remove-Item $outF, "$outF.err" -ErrorAction SilentlyContinue
}

```

- [ ] **Step 12: Перевірити BOM і парсинг скрипта в Windows PowerShell 5.1**

Run: `head -c 3 run_tests.ps1 | xxd -p` → Expected: `efbbbf`.
Run (лише якщо інші сесії не ганяють гейт — `ListAgents`): `powershell.exe -ExecutionPolicy Bypass -File run_tests.ps1 x64 -NoUapki`
Expected: рядок `[PASS   ] L-f1 prro_fs_state_selftest — усі CHECK пройшли (PASS: 11)`; жодного `Missing closing '}'`.

- [ ] **Step 13: Commit**

```bash
git add tests/support/MiniHttpServer.h tests/support/MiniHttpServer.cpp tests/support/MiniHttpClient.h tests/support/MiniHttpClient.cpp tests/prro_fs_state_selftest.cpp tests/CMakeLists.txt run_tests.ps1
git commit --only -m "feat(tests): MiniHttpServer — спільні заголовки й диспозиції обриву; рівень L-f1" -m "<три пари негативної верифікації з Step 9 + виміри Stop(): з перериванням / без>" -- tests/support/MiniHttpServer.h tests/support/MiniHttpServer.cpp tests/support/MiniHttpClient.h tests/support/MiniHttpClient.cpp tests/prro_fs_state_selftest.cpp tests/CMakeLists.txt run_tests.ps1
```

---

### Task 2: `UapkiOracle` — винесення крипто з `uapki_fiscal_emulator` без зміни поведінки

**Files:**
- Create: `tests/support/UapkiOracle.h`, `tests/support/UapkiOracle.cpp`
- Modify: `tests/uapki_fiscal_emulator.cpp` (видалення винесених блоків, рядки на `main` `93761f5`: 60-65, 83-86, 92-111, 117-164, 227-238, 246-259, 262-337, 352-409, 412-519)
- Modify: `tests/CMakeLists.txt` (джерело в ціль `uapki_fiscal_emulator`)

**Interfaces:**
- Consumes: —
- Produces (namespace `oracle`):
  - `struct OracleConfig { std::wstring providersDir; std::wstring dataDir; std::wstring keyPath; std::string pass; };`
  - `struct VerifyOutcome { bool accepted; std::string status, statusSig, statusMd, signerCertId, contentB64; bool validSig, validDig, certEmbedded; long errorCode; std::string errorText; };`
  - `bool Init(const OracleConfig&)`, `VerifyOutcome Verify(const std::string& der)`, `std::string Sign(const std::string& raw)`, `void Shutdown()`
  - `std::wstring u8to16(const std::string&)`, `std::string w2u8(const std::wstring&)`, `std::string b64encode(const std::string&)`, `bool b64decode(const std::string&, std::string&)`

- [ ] **Step 1: Базова лінія — вивід самотесту ДО винесення**

Run:
```bash
cmake --build build_x64 --config Release --target uapki_fiscal_emulator
./bin/Release/uapki_fiscal_emulator_x64.exe --self-test > "$TEMP/ufe_before.txt" 2>&1; echo exit=$?
```
Expected: `exit=0`; файл містить рядки `ok:` і фінальний `==== self-test: fails=0 ====`.

- [ ] **Step 2: `tests/support/UapkiOracle.h`**

```cpp
#pragma once
// UapkiOracle — крипто-частина тестових HTTP-оракулів (uapki_fiscal_emulator,
// prro_fs_emulator): статично злінковане ядро UAPKI через process()/json_free().
// Винесено з tests/uapki_fiscal_emulator.cpp БЕЗ зміни логіки (спека
// docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §9).
// UAPKI — процесний singleton (одне сховище, один обраний ключ), тому модуль —
// набір вільних функцій над внутрішнім станом, а не клас з екземплярами.
// Усі функції потокобезпечні: process() серіалізується внутрішнім м'ютексом.
#include <string>

namespace oracle {

struct OracleConfig {
    std::wstring providersDir;   // каталог із cm-pkcs12_*.dll
    std::wstring dataDir;        // certs/ + crls/ (копіюються у %TEMP%\uapki_oracle_<pid>)
    std::wstring keyPath;        // PKCS#12-контейнер тест-ключа
    std::string  pass;
};

struct VerifyOutcome {
    bool        accepted = false;   // критерій прийняття — РІВНО 4 умови (див. Verify)
    std::string status, statusSig, statusMd, signerCertId, contentB64;
    bool        validSig = false, validDig = false;
    // Діагностичний крос-чек: сертифікат підписувача реально ВКЛАДЕНО в CMS
    // (certIds непорожній І містить signerCertId). У критерій прийняття НЕ входить.
    bool        certEmbedded = false;
    // Причина відмови від крипто-ядра (errorCode != 0).
    long        errorCode = 0;
    std::string errorText;
};

// Bootstrap: writable-кеш у %TEMP% -> INIT -> OPEN -> KEYS -> SELECT_KEY.
bool          Init(const OracleConfig& cfg);
// STRUCT-перевірка CMS; accepted = errorCode==0 && status=="TOTAL-VALID" && validSignatures && validDigests.
VerifyOutcome Verify(const std::string& derBytes);
// CAdES-BES enveloping тест-ключем. Сирі DER-байти або порожній рядок.
std::string   Sign(const std::string& rawBytes);
// Прибрати temp-каталог. Ідемпотентний; безпечний з потоку ctrl-хендлера.
void          Shutdown();

std::wstring u8to16(const std::string& s);
std::string  w2u8(const std::wstring& w);
std::string  b64encode(const std::string& in);
bool         b64decode(const std::string& b64, std::string& out);

}  // namespace oracle
```

- [ ] **Step 3: `tests/support/UapkiOracle.cpp`** — перенести **дослівно** тіла функцій зі старого файлу; змінюються лише імена/сигнатури, позначені коментарем `// БУЛО:`.

```cpp
// UapkiOracle — реалізація. Опис — у UapkiOracle.h.
// Код перенесено ДОСЛІВНО з tests/uapki_fiscal_emulator.cpp (main 93761f5);
// змінено лише імена й сигнатури (позначено «БУЛО:»).
// УВАГА: pch.h НЕ підключається (правило tests/).
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include "UapkiOracle.h"

#include <cstdio>
#include <exception>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include <windows.h>

using nlohmann::json;

// Символи крипто-ядра (C-лінкування), статично злінковані через uapki_bundle.
extern "C" char* process(const char* request);
extern "C" void  json_free(char* buf);

namespace oracle {
namespace {

// Провайдер НКІ за compile-time архітектурою: cm-pkcs12_x64.dll / _x86.dll.
#ifdef _WIN64
const char* ARCH_PROVIDER = "cm-pkcs12_x64";
#else
const char* ARCH_PROVIDER = "cm-pkcs12_x86";
#endif

// ← перенести ДОСЛІВНО рядки 227-238 старого файлу (коментарі + g_uapkiMtx, g_workDir,
//   g_workDirMtx), прибравши слово static (анонімний простір імен уже дає внутрішнє зв'язування).
std::mutex   g_uapkiMtx;
std::wstring g_workDir;
std::mutex   g_workDirMtx;

// Абсолютний шлях для JSON: forward-slashes (дослівно зі старого файлу, рядки 106-111).
std::string fwd(const std::wstring& w) {
    std::string s = w2u8(w);
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

// ← перенести ДОСЛІВНО: callUapki (рядки 246-259), copyDirFiles (265-277),
//   removeDirRec (279-295), prepareWorkDir (313-337) — без слова static.

}  // namespace

// ← u8to16 і w2u8: тіла ДОСЛІВНО з рядків 92-105 (без static).
// ← b64encode і b64decode: тіла ДОСЛІВНО з рядків 117-164 (без static).

// БУЛО: static void removeWorkDir()  (рядки 304-311, тіло без змін)
void Shutdown() {
    std::lock_guard<std::mutex> crypto(g_uapkiMtx);
    std::lock_guard<std::mutex> lk(g_workDirMtx);
    if (g_workDir.empty()) return;
    const std::wstring dir = g_workDir;
    g_workDir.clear();
    removeDirRec(dir);
}

// БУЛО: static bool cryptoBootstrap(const Config& cfg)  (рядки 355-409).
// Тіло ДОСЛІВНО; параметр тепер OracleConfig з тими самими іменами полів
// (providersDir, dataDir, keyPath, pass), тож рядки тіла не змінюються.
bool Init(const OracleConfig& cfg) {
    // ← тіло cryptoBootstrap дослівно
}

// БУЛО: static VerifyOutcome verifyCms(const std::string& derBytes)  (рядки 432-486, тіло без змін)
VerifyOutcome Verify(const std::string& derBytes) {
    // ← тіло verifyCms дослівно
}

// БУЛО: static std::string signData(const std::string& rawBytes)  (рядки 489-519, тіло без змін)
std::string Sign(const std::string& rawBytes) {
    // ← тіло signData дослівно
}

}  // namespace oracle
```

Позначки `← … дослівно` — це інструкції **переносу** конкретних рядків, а не заглушки: результат має збігтися з видаленим кодом символ у символ, крім рядків «БУЛО:». Перевірка — Step 7.

- [ ] **Step 4: `tests/uapki_fiscal_emulator.cpp` — прибрати винесене й підключити модуль**

1. Після `#include "support/MiniHttpServer.h"` додати `#include "support/UapkiOracle.h"`.
2. Видалити: блок `ARCH_PROVIDER` (60-65); `extern "C"` (83-86); функції `u8to16`, `w2u8`, `fwd` разом із заголовком секції (89-111); `b64encode`, `b64decode` разом із заголовком секції (114-164); `g_uapkiMtx`/`g_workDir`/`g_workDirMtx` з коментарями (227-238); `callUapki` (246-259); секцію temp-каталогу `copyDirFiles`…`prepareWorkDir` (262-337); секцію `cryptoBootstrap` (352-409); `struct VerifyOutcome`, `verifyCms`, `signData` разом із заголовком секції (412-519). `ctrlHandler`, `g_stop*`, `xmlEscape`, `buildTicketXml`, HTTP-хелпери, `judgeByIit`, `runSelfTest`, `main` — лишаються.
3. На місці видаленої секції `VerifyOutcome/verifyCms/signData` вставити тонкі перехідники (зберігають імена в місцях виклику й у тексті самотесту):

```cpp
// ============================================================================
// Крипто — у спільному модулі tests/support/UapkiOracle (спека prro_fs_emulator §9).
// Перехідники зберігають імена, під якими крипто кличеться в цьому файлі.
// ============================================================================
using oracle::VerifyOutcome;
using oracle::u8to16;
using oracle::w2u8;
using oracle::b64decode;
static VerifyOutcome verifyCms(const std::string& der) { return oracle::Verify(der); }
static std::string   signData(const std::string& raw)  { return oracle::Sign(raw); }
static void          removeWorkDir()                   { oracle::Shutdown(); }
```

   **Де саме:** на місці видаленого блоку `u8to16/w2u8/fwd` (одразу після видаленого `extern "C"`), а не на місці 412-519: `ctrlHandler` (рядок 342) кличе `removeWorkDir`, тож перехідники мають бути оголошені вище за нього.
4. У `main` замінити `if (!cryptoBootstrap(cfg)) {` на:

```cpp
    if (!oracle::Init(oracle::OracleConfig{ cfg.providersDir, cfg.dataDir, cfg.keyPath, cfg.pass })) {
```

- [ ] **Step 5: `tests/CMakeLists.txt`** — у `add_executable(uapki_fiscal_emulator …)` додати джерело:

```cmake
add_executable(uapki_fiscal_emulator uapki_fiscal_emulator.cpp
    support/MiniHttpServer.cpp            # власний HTTP/1.1-сервер (замість ix::HttpServer)
    support/UapkiOracle.cpp               # крипто, спільне з prro_fs_emulator
)
```

- [ ] **Step 6: Зібрати**

Run: `cmake --build build_x64 --config Release --target uapki_fiscal_emulator`
Expected: без помилок і нових попереджень.

- [ ] **Step 7: Доказ незмінності — порівняння виводу й коду**

Run:
```bash
./bin/Release/uapki_fiscal_emulator_x64.exe --self-test > "$TEMP/ufe_after.txt" 2>&1; echo exit=$?
diff "$TEMP/ufe_before.txt" "$TEMP/ufe_after.txt" && echo IDENTICAL
git diff -U0 tests/uapki_fiscal_emulator.cpp | grep '^-' | grep -v '^---' > "$TEMP/moved.txt"
```
Expected: `exit=0` і `IDENTICAL`. Потім переглянути `moved.txt` поруч із `UapkiOracle.cpp`: кожен видалений рядок логіки присутній у модулі (відрізняються лише `static`, рядки «БУЛО:» та перейменування функцій).

- [ ] **Step 8: Ручний HTTP-прогін старого оракула (без 1С)**

Run (PowerShell; процес — власний, зупиняється за Id):
```powershell
$p = Start-Process -FilePath bin\Release\uapki_fiscal_emulator_x64.exe -ArgumentList '8199' -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3
curl.exe -s -o "$env:TEMP\ref.der" "http://127.0.0.1:8199/reference?type=check"
curl.exe -s -o "$env:TEMP\t.der" -w "%{http_code}`n" --data-binary "@$env:TEMP\ref.der" http://127.0.0.1:8199/doc
Stop-Process -Id $p.Id
```
Expected: друкується `200`.

- [ ] **Step 9: Гард якорів** (Task переміщує рядки `tests/`)

Run: `python scripts/check-doc-anchors.py --quiet; echo exit=$?`
Expected: `exit=0`. Якщо червоніють якорі на `tests/uapki_fiscal_emulator.cpp:<рядок>` — оновити номери в тих документах (зміст рядка має лишитися тим самим).

- [ ] **Step 10: Commit** — нових `CHECK` немає; доказ — `IDENTICAL` зі Step 7 і `200` зі Step 8 (записати в тіло коміту).

```bash
git add tests/support/UapkiOracle.h tests/support/UapkiOracle.cpp tests/uapki_fiscal_emulator.cpp tests/CMakeLists.txt
git commit --only -m "refactor(tests): крипто оракула винесено в UapkiOracle без зміни поведінки" -m "Вивід uapki_fiscal_emulator --self-test до і після тотожний; POST /doc на /reference -> 200." -- tests/support/UapkiOracle.h tests/support/UapkiOracle.cpp tests/uapki_fiscal_emulator.cpp tests/CMakeLists.txt
```
(Якщо Step 9 змусив правити документи — додати їх у той самий коміт.)

---

### Task 3: `FiscalServerState` — стан, нумерація, підсумки, розбір XML

**Files:**
- Create: `tests/support/FiscalServerState.h`, `tests/support/FiscalServerState.cpp`
- Create: `tests/support/PrroFsFixtures.h`
- Create: `tests/data/prro_fs/.gitattributes` і дев'ять фікстур (Step 1)
- Modify: `tests/prro_fs_state_selftest.cpp` (секції `TestMoney`, `TestParsing`, `TestInsertOrderTaxNum`, `TestStateFlow`, `TestTotals`)
- Modify: `tests/CMakeLists.txt` (ціль `prro_fs_state_selftest`)

**Interfaces:**
- Consumes: `CHECK`, `g_failures`, `main()` з `tests/prro_fs_state_selftest.cpp` (Task 1).
- Produces (namespace `prrofs`) — повний заголовок у Step 3: `ErrorCode` (`kOk=0, kTransactionsRegistrarAbsent=1, kShiftAlreadyOpened=4, kShiftNotOpened=5, kLastDocumentMustBeZRep=6, kCheckLocalNumberInvalid=7, kZRepAlreadyRegistered=8, kDocumentValidationError=9, kInvalidQueryParameter=11`), `const char* ErrorCodeName(int)`, `enum class DocClass { Check, ZRep }`, `PayFormTotal`, `TaxTotal`, `OrderTypeTotals`, `ShiftTotals`, `ParsedDoc`, `bool ParseDocument(const std::string&, ParsedDoc&, std::string&)`, `bool InsertOrderTaxNum(const std::string&, const std::string&, std::string&)`, `bool ParseMoney(const std::string&, int64_t&)`, `std::string FormatMoney(int64_t)`, `std::string Cp1251ToUtf8(const std::string&)`, `std::string Utf8ToCp1251(const std::string&)`, `RegistrarSeed`, `ShiftRecord`, `RegistrarState`, `StoredDoc`, `SubmitResult`, `class FiscalServerState { void Reset(const std::vector<RegistrarSeed>&); SubmitResult Submit(const ParsedDoc&, const std::string& originalXml, const std::string& nowIso, long long nowEpoch); const RegistrarState* Find(const std::string&) const; const StoredDoc* FindDoc(const std::string& registrar, long long localNum) const; const StoredDoc* FindDocByFiscal(const std::string& registrar, const std::string& fiscalNum) const; std::vector<const RegistrarState*> Registrars() const; const std::vector<StoredDoc>& Docs() const; }`.
- Produces (`tests/support/PrroFsFixtures.h`): `std::string prrofs::LoadFixture(const std::string& dir, const std::string& name, long long orderNum, const std::string& registrar)`.

- [ ] **Step 1: Фікстури**

Створити `tests/data/prro_fs/.gitattributes` (байти фікстур не нормалізуються — частина з них у windows-1251, і тести порівнюють «побайтовий оригінал»):

```
* -text
```

Створити файли **спершу в UTF-8** (вміст нижче; `{N}` і `{REG}` — плейсхолдери, їх підставляє `LoadFixture`).

`tests/data/prro_fs/open_shift_1251.xml`:
```xml
<?xml version="1.0" encoding="windows-1251"?>
<CHECK xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="check01.xsd">
  <CHECKHEAD>
    <DOCTYPE>100</DOCTYPE>
    <UID>10000000-0000-0000-0000-000000000001</UID>
    <TIN>99900000</TIN>
    <IPN>999000000009</IPN>
    <ORGNM>ТОВ "Тест"</ORGNM>
    <POINTNM>Магазин "Тест"</POINTNM>
    <POINTADDR>м. Київ, вул. Тестова, 1</POINTADDR>
    <ORDERDATE>24092026</ORDERDATE>
    <ORDERTIME>080000</ORDERTIME>
    <ORDERNUM>{N}</ORDERNUM>
    <CASHDESKNUM>1</CASHDESKNUM>
    <CASHREGISTERNUM>{REG}</CASHREGISTERNUM>
    <CASHIER>Тестовий Касир</CASHIER>
    <VER>1</VER>
  </CHECKHEAD>
</CHECK>
```

`tests/data/prro_fs/close_shift_1251.xml` — той самий вміст, що `open_shift_1251.xml`, з трьома відмінностями: `<DOCTYPE>101</DOCTYPE>`, `<UID>10000000-0000-0000-0000-000000000002</UID>`, `<ORDERTIME>200500</ORDERTIME>`.

`tests/data/prro_fs/check_sale_1251.xml`:
```xml
<?xml version="1.0" encoding="windows-1251"?>
<CHECK xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="check01.xsd">
  <CHECKHEAD>
    <DOCTYPE>0</DOCTYPE>
    <DOCSUBTYPE>0</DOCSUBTYPE>
    <UID>20000000-0000-0000-0000-000000000001</UID>
    <TIN>99900000</TIN>
    <IPN>999000000009</IPN>
    <ORGNM>ТОВ "Тест"</ORGNM>
    <POINTNM>Магазин "Тест"</POINTNM>
    <POINTADDR>м. Київ, вул. Тестова, 1</POINTADDR>
    <ORDERDATE>24092026</ORDERDATE>
    <ORDERTIME>100000</ORDERTIME>
    <ORDERNUM>{N}</ORDERNUM>
    <CASHDESKNUM>1</CASHDESKNUM>
    <CASHREGISTERNUM>{REG}</CASHREGISTERNUM>
    <CASHIER>Тестовий Касир</CASHIER>
    <VER>1</VER>
  </CHECKHEAD>
  <CHECKTOTAL>
    <SUM>150.00</SUM>
  </CHECKTOTAL>
  <CHECKPAY>
    <ROW ROWNUM="1"><PAYFORMCD>0</PAYFORMCD><PAYFORMNM>ГОТІВКА</PAYFORMNM><SUM>100.00</SUM></ROW>
    <ROW ROWNUM="2"><PAYFORMCD>1</PAYFORMCD><PAYFORMNM>КАРТКА</PAYFORMNM><SUM>50.00</SUM></ROW>
  </CHECKPAY>
  <CHECKTAX>
    <ROW ROWNUM="1"><TYPE>0</TYPE><NAME>ПДВ</NAME><LETTER>А</LETTER><PRC>20.00</PRC><TURNOVER>150.00</TURNOVER><SOURCESUM>150.00</SOURCESUM><SUM>25.00</SUM></ROW>
  </CHECKTAX>
  <CHECKBODY>
    <ROW ROWNUM="1"><CODE>1</CODE><NAME>Товар</NAME><UNITCD>2009</UNITCD><UNITNM>шт</UNITNM><AMOUNT>1.000</AMOUNT><PRICE>150.00</PRICE><LETTERS>А</LETTERS><COST>150.00</COST></ROW>
  </CHECKBODY>
</CHECK>
```

`tests/data/prro_fs/check_return_1251.xml` — як `check_sale_1251.xml`, але: `<DOCSUBTYPE>1</DOCSUBTYPE>`, `<UID>20000000-0000-0000-0000-000000000002</UID>`, `CHECKTOTAL/SUM` = `40.00`, `CHECKPAY` — один рядок `<ROW ROWNUM="1"><PAYFORMCD>0</PAYFORMCD><PAYFORMNM>ГОТІВКА</PAYFORMNM><SUM>40.00</SUM></ROW>`, `CHECKTAX` — `<ROW ROWNUM="1"><TYPE>0</TYPE><NAME>ПДВ</NAME><LETTER>А</LETTER><PRC>20.00</PRC><TURNOVER>40.00</TURNOVER><SOURCESUM>40.00</SOURCESUM><SUM>6.67</SUM></ROW>`, у `CHECKBODY` `PRICE`/`COST` = `40.00`.

`tests/data/prro_fs/check_deposit_1251.xml`:
```xml
<?xml version="1.0" encoding="windows-1251"?>
<CHECK xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="check01.xsd">
  <CHECKHEAD>
    <DOCTYPE>0</DOCTYPE>
    <DOCSUBTYPE>2</DOCSUBTYPE>
    <UID>20000000-0000-0000-0000-000000000003</UID>
    <TIN>99900000</TIN>
    <IPN>999000000009</IPN>
    <ORGNM>ТОВ "Тест"</ORGNM>
    <POINTNM>Магазин "Тест"</POINTNM>
    <POINTADDR>м. Київ, вул. Тестова, 1</POINTADDR>
    <ORDERDATE>24092026</ORDERDATE>
    <ORDERTIME>090000</ORDERTIME>
    <ORDERNUM>{N}</ORDERNUM>
    <CASHDESKNUM>1</CASHDESKNUM>
    <CASHREGISTERNUM>{REG}</CASHREGISTERNUM>
    <CASHIER>Тестовий Касир</CASHIER>
    <VER>1</VER>
  </CHECKHEAD>
  <CHECKTOTAL>
    <SUM>500.00</SUM>
  </CHECKTOTAL>
</CHECK>
```

`tests/data/prro_fs/check_issue_1251.xml` — як `check_deposit_1251.xml`, але `<DOCSUBTYPE>4</DOCSUBTYPE>`, `<UID>20000000-0000-0000-0000-000000000004</UID>`, `CHECKTOTAL/SUM` = `200.00`.

`tests/data/prro_fs/check_storno_1251.xml` — як `check_sale_1251.xml`, але `<DOCSUBTYPE>5</DOCSUBTYPE>`, `<UID>20000000-0000-0000-0000-000000000005</UID>`, `CHECKPAY` — один рядок `<ROW ROWNUM="1"><PAYFORMCD>0</PAYFORMCD><PAYFORMNM>ГОТІВКА</PAYFORMNM><SUM>150.00</SUM></ROW>`.

`tests/data/prro_fs/check_sale_utf8.xml` (декларація UTF-8; файл зберігається **з BOM**):
```xml
<?xml version="1.0" encoding="utf-8"?>
<CHECK xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="check01.xsd">
  <CHECKHEAD>
    <DOCTYPE>0</DOCTYPE>
    <DOCSUBTYPE>0</DOCSUBTYPE>
    <UID>20000000-0000-0000-0000-000000000006</UID>
    <TIN>99900000</TIN>
    <IPN>999000000009</IPN>
    <ORGNM>ТОВ "Тест"</ORGNM>
    <POINTNM>Магазин "Тест"</POINTNM>
    <POINTADDR>м. Київ, вул. Тестова, 1</POINTADDR>
    <ORDERDATE>24092026</ORDERDATE>
    <ORDERTIME>110000</ORDERTIME>
    <ORDERNUM>{N}</ORDERNUM>
    <CASHDESKNUM>1</CASHDESKNUM>
    <CASHREGISTERNUM>{REG}</CASHREGISTERNUM>
    <CASHIER>Тестовий Касир</CASHIER>
    <VER>1</VER>
  </CHECKHEAD>
  <CHECKTOTAL>
    <SUM>100.00</SUM>
  </CHECKTOTAL>
  <CHECKPAY>
    <ROW ROWNUM="1"><PAYFORMCD>0</PAYFORMCD><PAYFORMNM>ГОТІВКА</PAYFORMNM><SUM>100.00</SUM></ROW>
  </CHECKPAY>
  <CHECKTAX>
    <ROW ROWNUM="1"><TYPE>0</TYPE><NAME>ПДВ</NAME><LETTER>А</LETTER><PRC>20.00</PRC><SIGN>true</SIGN><TURNOVER>100.00</TURNOVER><TURNOVERDISCOUNT>90.00</TURNOVERDISCOUNT><SOURCESUM>100.00</SOURCESUM><SUM>16.67</SUM></ROW>
  </CHECKTAX>
</CHECK>
```

`tests/data/prro_fs/zrep_1251.xml`:
```xml
<?xml version="1.0" encoding="windows-1251"?>
<ZREP xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="zrep01.xsd">
  <ZREPHEAD>
    <UID>30000000-0000-0000-0000-000000000001</UID>
    <TIN>99900000</TIN>
    <IPN>999000000009</IPN>
    <ORGNM>ТОВ "Тест"</ORGNM>
    <POINTNM>Магазин "Тест"</POINTNM>
    <POINTADDR>м. Київ, вул. Тестова, 1</POINTADDR>
    <ORDERDATE>24092026</ORDERDATE>
    <ORDERTIME>200000</ORDERTIME>
    <ORDERNUM>{N}</ORDERNUM>
    <CASHDESKNUM>1</CASHDESKNUM>
    <CASHREGISTERNUM>{REG}</CASHREGISTERNUM>
    <CASHIER>Тестовий Касир</CASHIER>
    <VER>1</VER>
  </ZREPHEAD>
  <ZREPREALIZ>
    <SUM>150.00</SUM>
    <ORDERSCNT>1</ORDERSCNT>
  </ZREPREALIZ>
</ZREP>
```

Перекодувати `*_1251.xml` у windows-1251, а `check_sale_utf8.xml` — у UTF-8 з BOM (PowerShell):
```powershell
$w1251 = [Text.Encoding]::GetEncoding(1251)
Get-ChildItem tests\data\prro_fs\*_1251.xml | ForEach-Object {
    $t = [IO.File]::ReadAllText($_.FullName, [Text.Encoding]::UTF8)
    [IO.File]::WriteAllText($_.FullName, $t, $w1251)
}
$u = 'tests\data\prro_fs\check_sale_utf8.xml'
$t = [IO.File]::ReadAllText($u, [Text.Encoding]::UTF8)
[IO.File]::WriteAllText($u, $t, (New-Object Text.UTF8Encoding $true))
```
Перевірка: `python -c "b=open('tests/data/prro_fs/check_sale_1251.xml','rb').read(); print(b'\xc3\xce\xd2\xb2\xc2\xca\xc0' in b)"` → `True` (слово «ГОТІВКА» у windows-1251); `head -c 3 tests/data/prro_fs/check_sale_utf8.xml | xxd -p` → `efbbbf`.

- [ ] **Step 2: `tests/support/PrroFsFixtures.h`**

```cpp
#pragma once
// Завантаження шаблонів фікстур tests/data/prro_fs/<name>: {N} -> ORDERNUM,
// {REG} -> CASHREGISTERNUM. Плейсхолдери ASCII, тож заміна на байтах не чіпає
// кодування файлу (windows-1251 або UTF-8). Порожній рядок — файл не прочитано.
#include <fstream>
#include <sstream>
#include <string>

namespace prrofs {

inline std::string LoadFixture(const std::string& dir, const std::string& name,
                               long long orderNum, const std::string& registrar) {
    std::ifstream f(dir + "/" + name, std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    auto replaceAll = [&s](const std::string& from, const std::string& to) {
        size_t p = 0;
        while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
    };
    replaceAll("{N}", std::to_string(orderNum));
    replaceAll("{REG}", registrar);
    return s;
}

}  // namespace prrofs
```

- [ ] **Step 3: `tests/support/FiscalServerState.h`**

```cpp
#pragma once
// FiscalServerState — стан імітації фіскального сервера ДПС: ПРРО, зміни, локальна й
// фіскальна нумерація, збережені документи, підсумки змін; розбір XML документа.
// Чиста логіка: без HTTP і без крипто (спека
// docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §4).
// НЕ потокобезпечний: викликач (PrroFsService) тримає власний м'ютекс.
#include <cstdint>
#include <string>
#include <vector>

namespace prrofs {

// Коди помилок ДПС ([Опис] ~755-875) — лише ті, що породжує імітація.
enum ErrorCode : int {
    kOk                          = 0,
    kTransactionsRegistrarAbsent = 1,
    kShiftAlreadyOpened          = 4,
    kShiftNotOpened              = 5,
    kLastDocumentMustBeZRep      = 6,
    kCheckLocalNumberInvalid     = 7,
    kZRepAlreadyRegistered       = 8,
    kDocumentValidationError     = 9,
    kInvalidQueryParameter       = 11,
};
const char* ErrorCodeName(int code);   // "CheckLocalNumberInvalid"…; невідомий — "Unknown"

enum class DocClass { Check, ZRep };

// Суми й відсотки — цілими копійками / сотими (150.00 -> 15000, 20.00 % -> 2000).
struct PayFormTotal {
    int         code = 0;
    std::string name;          // UTF-8
    int64_t     sum = 0;
};

struct TaxTotal {
    int         type = 0;
    std::string name;          // UTF-8
    std::string letter;        // UTF-8
    int64_t     prc = 0;
    bool        sign = false;
    int64_t     turnover = 0, turnoverDiscount = 0, sourceSum = 0, sum = 0;
};

struct OrderTypeTotals {
    int64_t sum = 0, rndSum = 0, noRndSum = 0;
    int     ordersCount = 0;
    std::vector<PayFormTotal> payForms;
    std::vector<TaxTotal>     taxes;
};

struct ShiftTotals {
    OrderTypeTotals real, ret;
    int64_t serviceInput = 0, serviceOutput = 0;
};

// Розібраний документ: заголовок + суми, потрібні для підсумків (§4.7).
struct ParsedDoc {
    DocClass    klass = DocClass::Check;
    int         docType = 0;        // DOCTYPE (CHECK)
    int         docSubType = 0;     // DOCSUBTYPE; відсутній = 0
    long long   orderNum = 0;       // ORDERNUM
    std::string cashRegisterNum;    // CASHREGISTERNUM
    std::string uid;                // UID
    std::string cashier;            // CASHIER, UTF-8
    bool        testing = false;    // TESTING
    int64_t     totalSum = 0, rndSum = 0, noRndSum = 0;   // CHECKTOTAL
    std::vector<PayFormTotal> pays;                       // CHECKPAY/ROW
    std::vector<TaxTotal>     taxes;                      // CHECKTAX/ROW
};

// Розбір XML документа ПРРО; кодування — з XML-декларації (windows-1251 або UTF-8,
// BOM UTF-8 допустимий). false + err — не розібрано.
bool ParseDocument(const std::string& xmlBytes, ParsedDoc& out, std::string& err);

// Вставити ORDERTAXNUM останнім елементом CHECKHEAD (наявний — замінити на місці).
// Робота на байтах (вставка ASCII), тож оголошене кодування документа не змінюється.
bool InsertOrderTaxNum(const std::string& xmlBytes, const std::string& taxNum, std::string& out);

// "150.00" -> 15000; "20" -> 2000; "-1.5" -> -150. Більше двох знаків після крапки,
// порожньо чи не число -> false.
bool        ParseMoney(const std::string& text, int64_t& out);
std::string FormatMoney(int64_t v);   // 15000 -> "150.00", -5 -> "-0.05"

std::string Cp1251ToUtf8(const std::string& s);
std::string Utf8ToCp1251(const std::string& s);

struct RegistrarSeed {
    std::string numFiscal;
    long long   nextLocalNum = 1;
};

struct ShiftRecord {
    long long   shiftId = 0;
    std::string opened, closed;             // ISO 8601; closed порожній — зміна не закрита
    long long   openedEpoch = 0;            // для фільтра Shifts From/To
    std::string openName, closeName;
    std::string openShiftFiscalNum, closeShiftFiscalNum, zRepFiscalNum;
    bool        testing = false;
};

struct RegistrarState {
    std::string numFiscal;
    int         numLocal = 0;               // порядковий номер ПРРО (1, 2, …) для Objects
    bool        shiftOpen = false;
    long long   shiftId = 0;                // поточна/остання зміна; 0 — змін не було
    std::string openShiftFiscalNum;
    bool        zRepPresent = false;
    bool        testing = false;
    std::string name;                       // CASHIER документа відкриття, UTF-8
    long long   firstLocalNum = 0;
    long long   nextLocalNum = 1;
    std::string lastFiscalNum;              // у відкритій зміні; порожньо = null
    ShiftTotals totals;
    std::vector<ShiftRecord> shifts;        // історія; поточна/остання — back()
};

struct StoredDoc {
    std::string registrar;
    long long   localNum = 0;
    std::string fiscalNum;
    DocClass    klass = DocClass::Check;
    int         docType = 0, docSubType = 0;
    long long   shiftId = 0;
    std::string registeredAt;               // ISO 8601
    std::string originalXml;                // побайтово, як прийшло всередині CMS
};

struct SubmitResult {
    int         errorCode = kOk;
    std::string errorText;                  // UTF-8; для коду 7 закінчується числом N
    std::string fiscalNum;                  // виданий, якщо errorCode == kOk
};

class FiscalServerState {
public:
    static constexpr long long kFirstFiscalNum = 100000001;

    void Reset(const std::vector<RegistrarSeed>& seeds);
    // Рішення по документу (§4.4) і, якщо прийнято, реєстрація (§4.5).
    SubmitResult Submit(const ParsedDoc& doc, const std::string& originalXml,
                        const std::string& nowIso, long long nowEpoch);

    const RegistrarState* Find(const std::string& numFiscal) const;
    const StoredDoc*      FindDoc(const std::string& registrar, long long localNum) const;
    const StoredDoc*      FindDocByFiscal(const std::string& registrar, const std::string& fiscalNum) const;
    std::vector<const RegistrarState*> Registrars() const;     // у порядку reset
    const std::vector<StoredDoc>&      Docs() const { return docs_; }

private:
    RegistrarState* FindMut(const std::string& numFiscal);
    static void AddToTotals(ShiftTotals& t, const ParsedDoc& d);

    std::vector<RegistrarState> regs_;
    std::vector<StoredDoc>      docs_;
    long long nextFiscal_  = kFirstFiscalNum;
    long long nextShiftId_ = 1;
};

}  // namespace prrofs
```

- [ ] **Step 4: Секції самотесту (червоний)** — у `tests/prro_fs_state_selftest.cpp`:

1. Після `#include "support/MiniHttpClient.h"` додати `#include "support/FiscalServerState.h"` і `#include "support/PrroFsFixtures.h"`; після `#include <thread>` додати `#include <cstring>`.
2. Після макросу `CHECK` додати:

```cpp
#ifndef PRRO_FS_DATA_DIR
#  define PRRO_FS_DATA_DIR ""
#endif

static const char* kReg1       = "4000000001";
static const char* kReg2       = "4000000002";
static const char* kRegUnknown = "4999999999";

static std::string Fx(const char* name, long long n, const char* reg = kReg1) {
    return prrofs::LoadFixture(PRRO_FS_DATA_DIR, name, n, reg);
}

// Останнє число в тексті (для «…повинен дорівнювати N»); -1 — чисел немає.
static long long LastNumber(const std::string& s) {
    const size_t e = s.find_last_of("0123456789");
    if (e == std::string::npos) return -1;
    size_t b = e;
    while (b > 0 && s[b - 1] >= '0' && s[b - 1] <= '9') --b;
    return std::stoll(s.substr(b, e - b + 1));
}

static prrofs::SubmitResult SubmitFx(prrofs::FiscalServerState& st, const char* name,
                                     long long n, const char* reg = kReg1) {
    const std::string xml = Fx(name, n, reg);
    prrofs::ParsedDoc d;
    std::string err;
    if (xml.empty() || !prrofs::ParseDocument(xml, d, err)) {
        std::printf("[FAIL] фікстура %s не розібралась: %s\n", name, err.c_str());
        ++g_failures;
        prrofs::SubmitResult bad;
        bad.errorCode = -1;
        return bad;
    }
    return st.Submit(d, xml, "2026-09-24T12:00:00+03:00", 1790240400LL);
}

static void TestMoney() {
    std::printf("== Гроші й перекодування ==\n");
    int64_t v = 0;
    CHECK(prrofs::ParseMoney("150.00", v) && v == 15000, "ParseMoney 150.00 -> 15000");
    CHECK(prrofs::ParseMoney("0.05", v) && v == 5, "ParseMoney 0.05 -> 5");
    CHECK(prrofs::ParseMoney("20", v) && v == 2000, "ParseMoney 20 -> 2000");
    CHECK(prrofs::ParseMoney("-1.5", v) && v == -150, "ParseMoney -1.5 -> -150");
    CHECK(!prrofs::ParseMoney("1.234", v) && !prrofs::ParseMoney("abc", v) && !prrofs::ParseMoney("", v),
          "ParseMoney відхиляє три знаки, нечисло й порожнечу");
    CHECK(prrofs::FormatMoney(15000) == "150.00" && prrofs::FormatMoney(-5) == "-0.05"
          && prrofs::FormatMoney(0) == "0.00", "FormatMoney");
    CHECK(prrofs::Utf8ToCp1251("ГОТІВКА") == "\xC3\xCE\xD2\xB2\xC2\xCA\xC0"
          && prrofs::Cp1251ToUtf8("\xC3\xCE\xD2\xB2\xC2\xCA\xC0") == "ГОТІВКА",
          "перекодування UTF-8 <-> windows-1251 (включно з українською І)");
}

static void TestParsing() {
    std::printf("== Розбір документів ==\n");
    prrofs::ParsedDoc d;
    std::string err;
    CHECK(prrofs::ParseDocument(Fx("check_sale_1251.xml", 7), d, err), "check_sale_1251 розібрано");
    CHECK(d.klass == prrofs::DocClass::Check && d.docType == 0 && d.docSubType == 0 && d.orderNum == 7
          && d.cashRegisterNum == kReg1, "заголовок: CHECK, DOCTYPE 0, DOCSUBTYPE 0, ORDERNUM, CASHREGISTERNUM");
    CHECK(d.cashier == "Тестовий Касир", "CASHIER перекодовано з windows-1251 у UTF-8");
    CHECK(d.totalSum == 15000 && d.pays.size() == 2 && !d.pays.empty() && d.pays[0].name == "ГОТІВКА",
          "CHECKTOTAL і CHECKPAY розібрано, PAYFORMNM у UTF-8");
    CHECK(d.taxes.size() == 1 && !d.taxes[0].sign && d.taxes[0].turnoverDiscount == d.taxes[0].turnover,
          "відсутні SIGN=false і TURNOVERDISCOUNT=TURNOVER (припущення §4.7)");

    prrofs::ParsedDoc u;
    CHECK(prrofs::ParseDocument(Fx("check_sale_utf8.xml", 3), u, err), "UTF-8 з BOM перед декларацією розібрано (Review Focus 4)");
    CHECK(u.taxes.size() == 1 && u.taxes[0].sign && u.taxes[0].turnoverDiscount == 9000,
          "наявні SIGN і TURNOVERDISCOUNT узято з документа");

    prrofs::ParsedDoc z;
    CHECK(prrofs::ParseDocument(Fx("zrep_1251.xml", 4), z, err) && z.klass == prrofs::DocClass::ZRep
          && z.orderNum == 4, "ZREP розібрано: клас ZRep, ORDERNUM з ZREPHEAD");

    prrofs::ParsedDoc bad;
    CHECK(!prrofs::ParseDocument("<FOO/>", bad, err), "невідомий корінь — відмова");
    CHECK(!prrofs::ParseDocument("<?xml version=\"1.0\" encoding=\"koi8-u\"?><CHECK/>", bad, err),
          "непідтримуване кодування — відмова");
    CHECK(!prrofs::ParseDocument("<CHECK><CHECKHEAD><ORDERNUM>x</ORDERNUM><CASHREGISTERNUM>1</CASHREGISTERNUM></CHECKHEAD></CHECK>", bad, err),
          "нечисловий ORDERNUM — відмова");
}

static void TestInsertOrderTaxNum() {
    std::printf("== Вставка ORDERTAXNUM ==\n");
    const std::string x = Fx("check_sale_1251.xml", 2);
    const std::string elem = "<ORDERTAXNUM>100000002</ORDERTAXNUM>";
    std::string y;
    CHECK(prrofs::InsertOrderTaxNum(x, "100000002", y), "вставка в чек виконана");
    const size_t xClose = x.find("</CHECKHEAD>");
    const size_t yClose = y.find("</CHECKHEAD>");
    CHECK(yClose != std::string::npos && yClose >= elem.size()
          && y.compare(yClose - elem.size(), elem.size(), elem) == 0,
          "ORDERTAXNUM — останній елемент CHECKHEAD");
    CHECK(xClose != std::string::npos && y.size() == x.size() + elem.size()
          && y.compare(0, xClose, x, 0, xClose) == 0
          && y.compare(yClose, std::string::npos, x, xClose, std::string::npos) == 0,
          "решта байтів не змінена (кодування windows-1251 збережено)");
    std::string z;
    CHECK(prrofs::InsertOrderTaxNum(y, "100000099", z), "повторна вставка виконана");
    size_t count = 0;
    for (size_t p = z.find("<ORDERTAXNUM>"); p != std::string::npos; p = z.find("<ORDERTAXNUM>", p + 1)) ++count;
    CHECK(count == 1 && z.find("<ORDERTAXNUM>100000099</ORDERTAXNUM>") != std::string::npos,
          "наявний ORDERTAXNUM замінено, а не продубльовано");
    CHECK(!prrofs::InsertOrderTaxNum(Fx("zrep_1251.xml", 1), "1", z), "ZREP без CHECKHEAD — відмова");
}

static void TestStateFlow() {
    std::printf("== FiscalServerState: зміна й нумерація ==\n");
    prrofs::FiscalServerState st;
    st.Reset({ { kReg1, 1 }, { kReg2, 10 } });
    const prrofs::RegistrarState* r = st.Find(kReg1);
    CHECK(r && r->nextLocalNum == 1 && !r->shiftOpen && st.Find(kReg2) && st.Find(kReg2)->numLocal == 2,
          "reset: два ПРРО, NextLocalNum із сіду, зміна закрита, порядковий номер");

    prrofs::SubmitResult x = SubmitFx(st, "check_sale_1251.xml", 1);
    CHECK(x.errorCode == prrofs::kShiftNotOpened, "чек при закритій зміні -> 5 ShiftNotOpened");
    x = SubmitFx(st, "open_shift_1251.xml", 2);
    CHECK(x.errorCode == prrofs::kCheckLocalNumberInvalid && LastNumber(x.errorText) == 1,
          "неправильний номер -> 7, останнє число тексту N = 1");
    CHECK(x.errorText.find("Номер документа повинен дорівнювати") != std::string::npos,
          "текст коду 7 містить фразу споживача");
    CHECK(st.Find(kReg1)->nextLocalNum == 1 && st.Docs().empty(),
          "відхилені документи номер не займають і не зберігаються");
    CHECK(SubmitFx(st, "open_shift_1251.xml", 1, kRegUnknown).errorCode == prrofs::kTransactionsRegistrarAbsent,
          "невідомий ПРРО -> 1 TransactionsRegistrarAbsent");

    x = SubmitFx(st, "open_shift_1251.xml", 1);
    r = st.Find(kReg1);
    CHECK(x.errorCode == prrofs::kOk && x.fiscalNum == "100000001", "відкриття прийнято, перший фіскальний номер 100000001");
    CHECK(r->shiftOpen && r->shiftId == 1 && r->firstLocalNum == 1 && r->nextLocalNum == 2
          && r->openShiftFiscalNum == "100000001" && r->lastFiscalNum == "100000001",
          "стан після відкриття: зміна 1, FirstLocalNum 1, NextLocalNum 2, LastFiscalNum");
    CHECK(r->name == "Тестовий Касир", "Name = CASHIER документа відкриття (UTF-8)");
    CHECK(SubmitFx(st, "open_shift_1251.xml", 2).errorCode == prrofs::kShiftAlreadyOpened,
          "повторне відкриття -> 4 ShiftAlreadyOpened");
    CHECK(SubmitFx(st, "check_sale_1251.xml", 2).errorCode == prrofs::kOk, "чек №2 прийнято");
    CHECK(SubmitFx(st, "close_shift_1251.xml", 3).errorCode == prrofs::kLastDocumentMustBeZRep,
          "закриття без Z-звіту -> 6 LastDocumentMustBeZRep");
    x = SubmitFx(st, "zrep_1251.xml", 3);
    r = st.Find(kReg1);
    CHECK(x.errorCode == prrofs::kOk && r->zRepPresent && !r->shifts.empty()
          && r->shifts.back().zRepFiscalNum == x.fiscalNum, "Z-звіт прийнято, ZRepFiscalNum у зміні");
    CHECK(SubmitFx(st, "zrep_1251.xml", 4).errorCode == prrofs::kZRepAlreadyRegistered,
          "другий Z-звіт -> 8 ZRepAlreadyRegistered");
    x = SubmitFx(st, "close_shift_1251.xml", 4);
    r = st.Find(kReg1);
    CHECK(x.errorCode == prrofs::kOk && !r->shiftOpen && r->lastFiscalNum.empty()
          && !r->shifts.back().closed.empty() && r->shifts.back().closeShiftFiscalNum == x.fiscalNum,
          "закриття: зміна закрита, LastFiscalNum порожній (null), Closed заповнено");

    x = SubmitFx(st, "open_shift_1251.xml", 10, kReg2);
    CHECK(x.errorCode == prrofs::kOk && x.fiscalNum == "100000005",
          "фіскальна нумерація наскрізна між ПРРО (п'ятий прийнятий -> 100000005)");

    const prrofs::StoredDoc* d = st.FindDoc(kReg1, 2);
    CHECK(d && d->originalXml == Fx("check_sale_1251.xml", 2) && d->fiscalNum == "100000002" && d->shiftId == 1,
          "документ №2 збережено побайтово, з фіскальним номером і зміною");
    CHECK(st.FindDocByFiscal(kReg1, "100000002") == d && st.FindDoc(kReg1, 99) == nullptr,
          "пошук за фіскальним номером; неіснуючий -> nullptr");
}

static void TestTotals() {
    std::printf("== FiscalServerState: підсумки зміни ==\n");
    prrofs::FiscalServerState st;
    st.Reset({ { kReg1, 1 } });
    SubmitFx(st, "open_shift_1251.xml", 1);
    const prrofs::ShiftTotals& t0 = st.Find(kReg1)->totals;
    CHECK(t0.real.ordersCount == 0 && t0.real.payForms.empty() && t0.ret.ordersCount == 0 && t0.serviceInput == 0,
          "підсумки відкритої зміни без чеків — нулі");
    const bool allOk =
        SubmitFx(st, "check_sale_1251.xml", 2).errorCode == prrofs::kOk &&
        SubmitFx(st, "check_sale_utf8.xml", 3).errorCode == prrofs::kOk &&
        SubmitFx(st, "check_return_1251.xml", 4).errorCode == prrofs::kOk &&
        SubmitFx(st, "check_deposit_1251.xml", 5).errorCode == prrofs::kOk &&
        SubmitFx(st, "check_issue_1251.xml", 6).errorCode == prrofs::kOk &&
        SubmitFx(st, "check_storno_1251.xml", 7).errorCode == prrofs::kOk;
    CHECK(allOk, "шість чеків різних підтипів прийнято");
    const prrofs::ShiftTotals& t = st.Find(kReg1)->totals;
    CHECK(t.real.ordersCount == 2 && t.real.sum == 25000, "Real: 2 чеки на 250.00 (сторно не враховано)");
    CHECK(t.ret.ordersCount == 1 && t.ret.sum == 4000, "Ret: 1 повернення на 40.00");
    CHECK(t.serviceInput == 50000 && t.serviceOutput == 20000, "ServiceInput 500.00, ServiceOutput 200.00");
    CHECK(t.real.payForms.size() == 2 && t.real.payForms[0].code == 0 && t.real.payForms[0].sum == 20000
          && t.real.payForms[1].code == 1 && t.real.payForms[1].sum == 5000, "PayForm агреговано за кодом");
    CHECK(!t.real.payForms.empty() && t.real.payForms[0].name == "ГОТІВКА", "PayFormName у UTF-8");
    const prrofs::TaxTotal* noSign = nullptr;
    const prrofs::TaxTotal* withSign = nullptr;
    for (const prrofs::TaxTotal& x : t.real.taxes) (x.sign ? withSign : noSign) = &x;
    CHECK(t.real.taxes.size() == 2, "Tax: різний SIGN -> два окремі рядки");
    CHECK(noSign && noSign->turnover == 15000 && noSign->turnoverDiscount == 15000 && noSign->sum == 2500
          && noSign->prc == 2000 && noSign->letter == "А" && noSign->name == "ПДВ",
          "рядок без SIGN: суми, відсоток, літера й назва; TurnoverDiscount = Turnover");
    CHECK(withSign && withSign->turnoverDiscount == 9000 && withSign->sourceSum == 10000,
          "рядок із SIGN: TurnoverDiscount з документа");
}
```

3. У `main()` після `TestTransport();` додати:

```cpp
    TestMoney();
    TestParsing();
    TestInsertOrderTaxNum();
    TestStateFlow();
    TestTotals();
```

- [ ] **Step 5: CMake — ціль `prro_fs_state_selftest`** — замінити її `add_executable`, `target_include_directories` і `target_compile_definitions` на:

```cmake
add_executable(prro_fs_state_selftest prro_fs_state_selftest.cpp
    support/MiniHttpServer.cpp
    support/MiniHttpClient.cpp
    support/FiscalServerState.cpp
    ${CMAKE_SOURCE_DIR}/extern/pugixml/src/pugixml.cpp
)
```
```cmake
target_include_directories(prro_fs_state_selftest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/extern/pugixml/src
)
target_compile_definitions(prro_fs_state_selftest PRIVATE _WINDOWS UNICODE _UNICODE
    PRRO_FS_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/prro_fs"
)
```

Run: `cmake --build build_x64 --config Release --target prro_fs_state_selftest`
Expected: FAIL — лінкер: `unresolved external symbol … prrofs::ParseDocument …` (реалізації ще немає).

- [ ] **Step 6: `tests/support/FiscalServerState.cpp`**

```cpp
// FiscalServerState — реалізація. Опис — у заголовку.
// УВАГА: pch.h НЕ підключається (правило tests/).
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include "FiscalServerState.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pugixml.hpp"

#include <windows.h>

namespace prrofs {
namespace {

std::string Recode(const std::string& in, UINT fromCp, UINT toCp) {
    if (in.empty()) return in;
    const int wn = MultiByteToWideChar(fromCp, 0, in.data(), static_cast<int>(in.size()), nullptr, 0);
    if (wn <= 0) return std::string();
    std::wstring w(static_cast<size_t>(wn), L'\0');
    MultiByteToWideChar(fromCp, 0, in.data(), static_cast<int>(in.size()), &w[0], wn);
    const int n = WideCharToMultiByte(toCp, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(toCp, 0, w.data(), wn, &out[0], n, nullptr, nullptr);
    return out;
}

// Кодування з XML-декларації (нижній регістр). Немає декларації/атрибута — "utf-8".
std::string DeclaredEncoding(const std::string& x) {
    size_t start = 0;
    if (x.size() >= 3 && static_cast<unsigned char>(x[0]) == 0xEF
        && static_cast<unsigned char>(x[1]) == 0xBB && static_cast<unsigned char>(x[2]) == 0xBF) start = 3;
    if (x.compare(start, 5, "<?xml") != 0) return "utf-8";
    const size_t end = x.find("?>", start);
    const size_t enc = x.find("encoding", start);
    if (end == std::string::npos || enc == std::string::npos || enc > end) return "utf-8";
    const size_t q = x.find_first_of("\"'", enc);
    if (q == std::string::npos || q > end) return "utf-8";
    const size_t q2 = x.find(x[q], q + 1);
    if (q2 == std::string::npos || q2 > end) return "utf-8";
    std::string v = x.substr(q + 1, q2 - q - 1);
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

std::string ChildText(const pugi::xml_node& n, const char* name) {
    return n.child(name).text().as_string();
}

// Необов'язкове грошове поле: відсутнє = 0; присутнє нечислове -> false.
bool OptMoney(const pugi::xml_node& n, const char* name, int64_t& dst) {
    const std::string t = ChildText(n, name);
    if (t.empty()) { dst = 0; return true; }
    return ParseMoney(t, dst);
}

bool IsTrue(const std::string& s) { return s == "true" || s == "1"; }

void AddOrder(OrderTypeTotals& o, const ParsedDoc& d) {
    o.sum      += d.totalSum;
    o.rndSum   += d.rndSum;
    o.noRndSum += d.noRndSum;
    o.ordersCount += 1;
    for (const PayFormTotal& p : d.pays) {
        auto it = std::find_if(o.payForms.begin(), o.payForms.end(),
                               [&p](const PayFormTotal& x) { return x.code == p.code; });
        if (it == o.payForms.end()) o.payForms.push_back(p);
        else                        it->sum += p.sum;
    }
    for (const TaxTotal& t : d.taxes) {
        auto it = std::find_if(o.taxes.begin(), o.taxes.end(), [&t](const TaxTotal& x) {
            return x.type == t.type && x.letter == t.letter && x.prc == t.prc && x.sign == t.sign;
        });
        if (it == o.taxes.end()) { o.taxes.push_back(t); continue; }
        it->turnover         += t.turnover;
        it->turnoverDiscount += t.turnoverDiscount;
        it->sourceSum        += t.sourceSum;
        it->sum              += t.sum;
    }
}

}  // namespace

const char* ErrorCodeName(int code) {
    switch (code) {
        case kOk:                          return "Ok";
        case kTransactionsRegistrarAbsent: return "TransactionsRegistrarAbsent";
        case kShiftAlreadyOpened:          return "ShiftAlreadyOpened";
        case kShiftNotOpened:              return "ShiftNotOpened";
        case kLastDocumentMustBeZRep:      return "LastDocumentMustBeZRep";
        case kCheckLocalNumberInvalid:     return "CheckLocalNumberInvalid";
        case kZRepAlreadyRegistered:       return "ZRepAlreadyRegistered";
        case kDocumentValidationError:     return "DocumentValidationError";
        case kInvalidQueryParameter:       return "InvalidQueryParameter";
        default:                           return "Unknown";
    }
}

std::string Cp1251ToUtf8(const std::string& s) { return Recode(s, 1251, CP_UTF8); }
std::string Utf8ToCp1251(const std::string& s) { return Recode(s, CP_UTF8, 1251); }

bool ParseMoney(const std::string& text, int64_t& out) {
    if (text.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (text[0] == '-') { neg = true; i = 1; }
    if (i >= text.size()) return false;
    int64_t whole = 0;
    size_t digits = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++digits) whole = whole * 10 + (text[i] - '0');
    if (digits == 0) return false;
    int64_t frac = 0;
    size_t fracDigits = 0;
    if (i < text.size() && text[i] == '.') {
        for (++i; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++fracDigits) frac = frac * 10 + (text[i] - '0');
        if (fracDigits == 0 || fracDigits > 2) return false;
        if (fracDigits == 1) frac *= 10;
    }
    if (i != text.size()) return false;
    out = whole * 100 + frac;
    if (neg) out = -out;
    return true;
}

std::string FormatMoney(int64_t v) {
    const bool neg = v < 0;
    const int64_t a = neg ? -v : v;
    char b[32];
    std::snprintf(b, sizeof(b), "%s%lld.%02lld", neg ? "-" : "",
                  static_cast<long long>(a / 100), static_cast<long long>(a % 100));
    return b;
}

bool ParseDocument(const std::string& xmlBytes, ParsedDoc& out, std::string& err) {
    out = ParsedDoc();
    const std::string enc = DeclaredEncoding(xmlBytes);
    std::string utf8;
    if (enc == "windows-1251" || enc == "cp1251")  utf8 = Cp1251ToUtf8(xmlBytes);
    else if (enc == "utf-8" || enc == "utf8")      utf8 = xmlBytes;
    else { err = "непідтримуване кодування: " + enc; return false; }
    // BOM UTF-8 (так лежать зразки ДПС) знімаємо самі — не покладаючись на поведінку парсера.
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF
        && static_cast<unsigned char>(utf8[1]) == 0xBB && static_cast<unsigned char>(utf8[2]) == 0xBF) utf8.erase(0, 3);

    pugi::xml_document doc;
    const pugi::xml_parse_result pr =
        doc.load_buffer(utf8.data(), utf8.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!pr) { err = std::string("XML не розбирається: ") + pr.description(); return false; }

    const pugi::xml_node root = doc.document_element();
    const std::string rootName = root.name();
    pugi::xml_node head;
    if (rootName == "CHECK")     { out.klass = DocClass::Check; head = root.child("CHECKHEAD"); }
    else if (rootName == "ZREP") { out.klass = DocClass::ZRep;  head = root.child("ZREPHEAD"); }
    else { err = "невідомий корінь документа: " + rootName; return false; }
    if (!head) { err = "немає заголовка документа"; return false; }

    const std::string orderNum = ChildText(head, "ORDERNUM");
    out.cashRegisterNum = ChildText(head, "CASHREGISTERNUM");
    if (orderNum.empty() || out.cashRegisterNum.empty()) { err = "немає ORDERNUM або CASHREGISTERNUM"; return false; }
    char* e = nullptr;
    out.orderNum = std::strtoll(orderNum.c_str(), &e, 10);
    if (*e != '\0' || out.orderNum <= 0) { err = "ORDERNUM не є додатним числом: " + orderNum; return false; }
    out.uid     = ChildText(head, "UID");
    out.cashier = ChildText(head, "CASHIER");
    out.testing = IsTrue(ChildText(head, "TESTING"));
    if (out.klass == DocClass::ZRep) return true;

    out.docType    = head.child("DOCTYPE").text().as_int(0);
    out.docSubType = head.child("DOCSUBTYPE").text().as_int(0);
    const pugi::xml_node total = root.child("CHECKTOTAL");
    if (!OptMoney(total, "SUM", out.totalSum) || !OptMoney(total, "RNDSUM", out.rndSum)
        || !OptMoney(total, "NORNDSUM", out.noRndSum)) { err = "CHECKTOTAL: нечислове поле"; return false; }
    for (pugi::xml_node row : root.child("CHECKPAY").children("ROW")) {
        PayFormTotal p;
        p.code = row.child("PAYFORMCD").text().as_int(0);
        p.name = ChildText(row, "PAYFORMNM");
        if (!OptMoney(row, "SUM", p.sum)) { err = "CHECKPAY/ROW/SUM: не число"; return false; }
        out.pays.push_back(p);
    }
    for (pugi::xml_node row : root.child("CHECKTAX").children("ROW")) {
        TaxTotal t;
        t.type   = row.child("TYPE").text().as_int(0);
        t.name   = ChildText(row, "NAME");
        t.letter = ChildText(row, "LETTER");
        t.sign   = IsTrue(ChildText(row, "SIGN"));            // відсутній = false (§4.7)
        if (!OptMoney(row, "PRC", t.prc) || !OptMoney(row, "TURNOVER", t.turnover)
            || !OptMoney(row, "SOURCESUM", t.sourceSum) || !OptMoney(row, "SUM", t.sum)) {
            err = "CHECKTAX/ROW: нечислове поле";
            return false;
        }
        if (ChildText(row, "TURNOVERDISCOUNT").empty()) t.turnoverDiscount = t.turnover;   // §4.7
        else if (!OptMoney(row, "TURNOVERDISCOUNT", t.turnoverDiscount)) { err = "TURNOVERDISCOUNT: не число"; return false; }
        out.taxes.push_back(t);
    }
    return true;
}

bool InsertOrderTaxNum(const std::string& x, const std::string& taxNum, std::string& out) {
    static const char* kOpen  = "<ORDERTAXNUM>";
    static const char* kClose = "</ORDERTAXNUM>";
    const size_t headOpen  = x.find("<CHECKHEAD>");
    const size_t headClose = x.find("</CHECKHEAD>");
    if (headOpen == std::string::npos || headClose == std::string::npos || headClose < headOpen) return false;
    const std::string elem = std::string(kOpen) + taxNum + kClose;
    const size_t oldOpen = x.find(kOpen, headOpen);
    if (oldOpen != std::string::npos && oldOpen < headClose) {
        const size_t oldClose = x.find(kClose, oldOpen);
        if (oldClose == std::string::npos || oldClose > headClose) return false;
        out = x.substr(0, oldOpen) + elem + x.substr(oldClose + std::strlen(kClose));
        return true;
    }
    out = x.substr(0, headClose) + elem + x.substr(headClose);
    return true;
}

void FiscalServerState::Reset(const std::vector<RegistrarSeed>& seeds) {
    regs_.clear();
    docs_.clear();
    nextFiscal_  = kFirstFiscalNum;
    nextShiftId_ = 1;
    int i = 1;
    for (const RegistrarSeed& s : seeds) {
        RegistrarState r;
        r.numFiscal    = s.numFiscal;
        r.numLocal     = i++;
        r.nextLocalNum = s.nextLocalNum;
        regs_.push_back(r);
    }
}

RegistrarState* FiscalServerState::FindMut(const std::string& numFiscal) {
    for (RegistrarState& r : regs_) if (r.numFiscal == numFiscal) return &r;
    return nullptr;
}

const RegistrarState* FiscalServerState::Find(const std::string& numFiscal) const {
    for (const RegistrarState& r : regs_) if (r.numFiscal == numFiscal) return &r;
    return nullptr;
}

const StoredDoc* FiscalServerState::FindDoc(const std::string& registrar, long long localNum) const {
    for (const StoredDoc& d : docs_) if (d.registrar == registrar && d.localNum == localNum) return &d;
    return nullptr;
}

const StoredDoc* FiscalServerState::FindDocByFiscal(const std::string& registrar,
                                                    const std::string& fiscalNum) const {
    for (const StoredDoc& d : docs_) if (d.registrar == registrar && d.fiscalNum == fiscalNum) return &d;
    return nullptr;
}

std::vector<const RegistrarState*> FiscalServerState::Registrars() const {
    std::vector<const RegistrarState*> out;
    for (const RegistrarState& r : regs_) out.push_back(&r);
    return out;
}

void FiscalServerState::AddToTotals(ShiftTotals& t, const ParsedDoc& d) {
    switch (d.docSubType) {
        case 0:          AddOrder(t.real, d);              break;
        case 1:          AddOrder(t.ret, d);               break;
        case 2: case 3:  t.serviceInput  += d.totalSum;    break;
        case 4:          t.serviceOutput += d.totalSum;    break;
        default:                                           break;   // 5 сторно — поза обсягом (§12)
    }
}

SubmitResult FiscalServerState::Submit(const ParsedDoc& d, const std::string& originalXml,
                                       const std::string& nowIso, long long nowEpoch) {
    SubmitResult r;
    RegistrarState* reg = FindMut(d.cashRegisterNum);
    if (!reg) {
        r.errorCode = kTransactionsRegistrarAbsent;
        r.errorText = "ПРРО з фіскальним номером " + d.cashRegisterNum + " не зареєстрований";
        return r;
    }
    // Порядок перевірок — §4.4; номер раніше за стан — [припущення] спеки.
    if (d.orderNum != reg->nextLocalNum) {
        r.errorCode = kCheckLocalNumberInvalid;
        r.errorText = "Номер документа повинен дорівнювати " + std::to_string(reg->nextLocalNum);
        return r;
    }
    const bool isCheck = (d.klass == DocClass::Check);
    const bool isOpen  = isCheck && d.docType == 100;
    const bool isClose = isCheck && d.docType == 101;
    if (isOpen && reg->shiftOpen)   { r.errorCode = kShiftAlreadyOpened;     r.errorText = "Зміну вже відкрито";                       return r; }
    if (!isOpen && !reg->shiftOpen) { r.errorCode = kShiftNotOpened;         r.errorText = "Зміну не відкрито";                        return r; }
    if (isClose && !reg->zRepPresent) { r.errorCode = kLastDocumentMustBeZRep; r.errorText = "Останнім документом зміни має бути Z-звіт"; return r; }
    if (d.klass == DocClass::ZRep && reg->zRepPresent) { r.errorCode = kZRepAlreadyRegistered; r.errorText = "Z-звіт уже зареєстровано"; return r; }

    r.fiscalNum = std::to_string(nextFiscal_++);
    if (isOpen) {
        reg->shiftOpen          = true;
        reg->shiftId            = nextShiftId_++;
        reg->openShiftFiscalNum = r.fiscalNum;
        reg->zRepPresent        = false;
        reg->testing            = d.testing;
        reg->name               = d.cashier.empty() ? std::string("Тестовий касир") : d.cashier;
        reg->firstLocalNum      = d.orderNum;
        reg->totals             = ShiftTotals();
        ShiftRecord s;
        s.shiftId            = reg->shiftId;
        s.opened             = nowIso;
        s.openedEpoch        = nowEpoch;
        s.openName           = reg->name;
        s.openShiftFiscalNum = r.fiscalNum;
        s.testing            = d.testing;
        reg->shifts.push_back(s);
    } else if (d.klass == DocClass::ZRep) {
        reg->zRepPresent = true;
        reg->shifts.back().zRepFiscalNum = r.fiscalNum;
    } else if (isClose) {
        reg->shiftOpen = false;
        ShiftRecord& s = reg->shifts.back();
        s.closed              = nowIso;
        s.closeName           = d.cashier.empty() ? reg->name : d.cashier;
        s.closeShiftFiscalNum = r.fiscalNum;
    } else if (isCheck && d.docType == 0) {
        AddToTotals(reg->totals, d);
    }
    reg->nextLocalNum += 1;
    reg->lastFiscalNum = reg->shiftOpen ? r.fiscalNum : std::string();

    StoredDoc sd;
    sd.registrar    = reg->numFiscal;
    sd.localNum     = d.orderNum;
    sd.fiscalNum    = r.fiscalNum;
    sd.klass        = d.klass;
    sd.docType      = d.docType;
    sd.docSubType   = d.docSubType;
    sd.shiftId      = reg->shiftId;
    sd.registeredAt = nowIso;
    sd.originalXml  = originalXml;
    docs_.push_back(std::move(sd));
    return r;
}

}  // namespace prrofs
```

- [ ] **Step 7: Зібрати й прогнати — ЗЕЛЕНИЙ**

Run: `cmake --build build_x64 --config Release --target prro_fs_state_selftest && ./bin/Release/prro_fs_state_selftest_x64.exe; echo exit=$?`
Expected: `=== ALL PASS ===`, `exit=0`.

- [ ] **Step 8: Негативна верифікація (правило 1)** — кожну правку відкотити:
  - `Submit`: прибрати перевірку `d.orderNum != reg->nextLocalNum` → відкриття з №2 приймається, і червоніє `неправильний номер -> 7, останнє число тексту N = 1`.
  - `Submit`: перенести `reg->nextLocalNum += 1;` на початок функції (відхилений займає номер) → червоніє `відхилені документи номер не займають…`.
  - `ParseDocument`: замінити `t.turnoverDiscount = t.turnover` на `= 0` → червоніють `відсутні SIGN=false і TURNOVERDISCOUNT=TURNOVER` і рядок підсумків «TurnoverDiscount = Turnover».
  - `ParseDocument`: для `windows-1251` не перекодовувати (`utf8 = xmlBytes`) → червоніє `CASHIER перекодовано…`.
  - `AddToTotals`: додати `case 5:` до `AddOrder(t.real, d)` → червоніє `Real: 2 чеки на 250.00 (сторно не враховано)`.

- [ ] **Step 9: Commit**

```bash
git add tests/support/FiscalServerState.h tests/support/FiscalServerState.cpp tests/support/PrroFsFixtures.h tests/data/prro_fs tests/prro_fs_state_selftest.cpp tests/CMakeLists.txt
git commit --only -m "feat(tests): FiscalServerState — стан, нумерація, підсумки імітації ДПС" -m "<пари негативної верифікації зі Step 8>" -- tests/support/FiscalServerState.h tests/support/FiscalServerState.cpp tests/support/PrroFsFixtures.h tests/data/prro_fs tests/prro_fs_state_selftest.cpp tests/CMakeLists.txt
```

---

### Task 4: `FaultPlan` — одноразові збої й постійні налаштування

**Files:**
- Create: `tests/support/FaultPlan.h`, `tests/support/FaultPlan.cpp`
- Modify: `tests/prro_fs_state_selftest.cpp` (секція `TestFaultPlan`)
- Modify: `tests/CMakeLists.txt` (джерело в `prro_fs_state_selftest`)

**Interfaces:**
- Consumes: `CHECK`, `main()` самотесту.
- Produces (namespace `prrofs`): `enum class FaultMode { Delay, Status, DropBeforeRegister, DropAfterRegister }`, `const char* FaultModeName(FaultMode)` (`"delay"`, `"status"`, `"dropBeforeRegister"`, `"dropAfterRegister"`), `bool ParseFaultMode(const std::string&, FaultMode&)`, `struct Fault { FaultMode mode; int seconds; int code; std::string body; std::string location; int holdSeconds; }`, `enum class RejectFormat { Text, Ticket }`, `class FaultPlan { enum class ArmResult { Ok, Conflict, BadTarget }; ArmResult Arm(const std::string& target, const Fault&); bool Take(const std::string& kind, const std::function<std::string()>& commandName, Fault& out); void Clear(); std::vector<std::pair<std::string, Fault>> Armed() const; int dateSkewSeconds = 0; RejectFormat rejectFormat = RejectFormat::Text; }`.

- [ ] **Step 1: `tests/support/FaultPlan.h`**

```cpp
#pragma once
// FaultPlan — керовані збої імітації ДПС (спека §8.2-8.3): одноразовий збій на
// наступний запит своєї цілі + постійні налаштування до наступного reset.
// Чиста логіка; НЕ потокобезпечний — викликач тримає м'ютекс.
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace prrofs {

enum class FaultMode { Delay, Status, DropBeforeRegister, DropAfterRegister };
const char* FaultModeName(FaultMode m);                 // "delay" | "status" | "dropBeforeRegister" | "dropAfterRegister"
bool        ParseFaultMode(const std::string& s, FaultMode& out);

struct Fault {
    FaultMode   mode = FaultMode::Delay;
    int         seconds = 0;       // delay
    int         code = 0;          // status
    std::string body;              // status (необов'язково)
    std::string location;          // status 302 (необов'язково)
    int         holdSeconds = 0;   // drop*: 0 — розрив одразу; >0 — мовчати, потім розрив
};

enum class RejectFormat { Text, Ticket };

class FaultPlan {
public:
    enum class ArmResult { Ok, Conflict, BadTarget };

    // target: "doc" | "cmd" | "cmd:<Команда>". Для тієї самої цілі вже взведено -> Conflict (409).
    ArmResult Arm(const std::string& target, const Fault& f);
    // Витратити збій для запиту kind ("doc" | "cmd"). commandName викликається ЛИШЕ коли
    // взведено хоч один "cmd:<Команда>" — інакше тіло команди до збою не розбирається.
    // "cmd:<Команда>" має перевагу над "cmd"; витрачається рівно один збій.
    bool Take(const std::string& kind, const std::function<std::string()>& commandName, Fault& out);
    void Clear();                                          // reset: збої + постійні налаштування
    std::vector<std::pair<std::string, Fault>> Armed() const { return armed_; }

    int          dateSkewSeconds = 0;
    RejectFormat rejectFormat    = RejectFormat::Text;

private:
    bool TakeTarget(const std::string& target, Fault& out);
    std::vector<std::pair<std::string, Fault>> armed_;
};

}  // namespace prrofs
```

- [ ] **Step 2: Секція самотесту (червоний)** — у `tests/prro_fs_state_selftest.cpp` додати `#include "support/FaultPlan.h"` після `#include "support/FiscalServerState.h"`, функцію перед `main()` і виклик `TestFaultPlan();` у `main()` після `TestTotals();`:

```cpp
static void TestFaultPlan() {
    std::printf("== FaultPlan ==\n");
    using prrofs::Fault;
    using prrofs::FaultMode;
    using prrofs::FaultPlan;
    int calls = 0;
    auto name = [&calls](const char* n) {
        return std::function<std::string()>([&calls, n]() { ++calls; return std::string(n); });
    };

    FaultPlan fp;
    Fault drop;
    drop.mode = FaultMode::DropAfterRegister;
    CHECK(fp.Arm("doc", drop) == FaultPlan::ArmResult::Ok, "Arm doc -> Ok");
    CHECK(fp.Arm("doc", drop) == FaultPlan::ArmResult::Conflict, "повторне взведення тієї самої цілі -> Conflict (черги немає)");
    CHECK(fp.Arm("foo", drop) == FaultPlan::ArmResult::BadTarget && fp.Arm("cmd:", drop) == FaultPlan::ArmResult::BadTarget,
          "некоректна ціль -> BadTarget");
    Fault got;
    CHECK(!fp.Take("cmd", name("ServerState"), got), "збій doc не спрацьовує на cmd");
    CHECK(fp.Take("doc", nullptr, got) && got.mode == FaultMode::DropAfterRegister, "збій doc спрацював на doc");
    CHECK(!fp.Take("doc", nullptr, got), "вдруге не спрацьовує (витрачено)");

    Fault s503;
    s503.mode = FaultMode::Status;
    s503.code = 503;
    CHECK(fp.Arm("cmd", s503) == FaultPlan::ArmResult::Ok, "Arm cmd -> Ok");
    calls = 0;
    CHECK(fp.Take("cmd", name("ServerState"), got) && got.code == 503, "загальний cmd спрацював");
    CHECK(calls == 0, "лише загальний cmd: ім'я команди не розбиралось");

    Fault s500 = s503;
    s500.code = 500;
    fp.Arm("cmd", s500);
    fp.Arm("cmd:CheckExt", s503);
    calls = 0;
    CHECK(fp.Take("cmd", name("CheckExt"), got) && got.code == 503, "cmd:CheckExt має перевагу над cmd");
    CHECK(calls == 1 && fp.Armed().size() == 1 && fp.Armed()[0].first == "cmd",
          "витрачено рівно один збій — загальний cmd лишився");
    fp.Arm("cmd:CheckExt", s503);
    CHECK(fp.Take("cmd", name("ServerState"), got) && got.code == 500, "інша команда бере загальний cmd");
    CHECK(fp.Armed().size() == 1 && fp.Armed()[0].first == "cmd:CheckExt", "cmd:CheckExt лишився для CheckExt");

    fp.dateSkewSeconds = 40;
    fp.rejectFormat = prrofs::RejectFormat::Ticket;
    fp.Clear();
    CHECK(fp.Armed().empty() && fp.dateSkewSeconds == 0 && fp.rejectFormat == prrofs::RejectFormat::Text,
          "Clear (reset) скидає збої й постійні налаштування");
    FaultMode m = FaultMode::Delay;
    CHECK(prrofs::ParseFaultMode("dropBeforeRegister", m) && m == FaultMode::DropBeforeRegister
          && !prrofs::ParseFaultMode("drop", m), "ParseFaultMode: відома назва — так, невідома — ні");
}
```

Також додати `#include <functional>` після `#include <cstring>`. У CMake у `add_executable(prro_fs_state_selftest …)` додати рядок `support/FaultPlan.cpp`.

Run: `cmake --build build_x64 --config Release --target prro_fs_state_selftest`
Expected: FAIL — лінкер: `unresolved external symbol … FaultPlan::Arm …`.

- [ ] **Step 3: `tests/support/FaultPlan.cpp`**

```cpp
// FaultPlan — реалізація. Опис — у заголовку.
// УВАГА: pch.h НЕ підключається (правило tests/).
#include "FaultPlan.h"

namespace prrofs {
namespace {

bool ValidTarget(const std::string& t) {
    return t == "doc" || t == "cmd" || (t.size() > 4 && t.compare(0, 4, "cmd:") == 0);
}

}  // namespace

const char* FaultModeName(FaultMode m) {
    switch (m) {
        case FaultMode::Delay:              return "delay";
        case FaultMode::Status:             return "status";
        case FaultMode::DropBeforeRegister: return "dropBeforeRegister";
        case FaultMode::DropAfterRegister:  return "dropAfterRegister";
    }
    return "?";
}

bool ParseFaultMode(const std::string& s, FaultMode& out) {
    for (FaultMode m : { FaultMode::Delay, FaultMode::Status,
                         FaultMode::DropBeforeRegister, FaultMode::DropAfterRegister }) {
        if (s == FaultModeName(m)) { out = m; return true; }
    }
    return false;
}

FaultPlan::ArmResult FaultPlan::Arm(const std::string& target, const Fault& f) {
    if (!ValidTarget(target)) return ArmResult::BadTarget;
    for (const auto& a : armed_) if (a.first == target) return ArmResult::Conflict;
    armed_.push_back({ target, f });
    return ArmResult::Ok;
}

bool FaultPlan::TakeTarget(const std::string& target, Fault& out) {
    for (auto it = armed_.begin(); it != armed_.end(); ++it) {
        if (it->first == target) { out = it->second; armed_.erase(it); return true; }
    }
    return false;
}

bool FaultPlan::Take(const std::string& kind, const std::function<std::string()>& commandName, Fault& out) {
    if (kind == "doc") return TakeTarget("doc", out);
    if (kind != "cmd") return false;
    bool anySpecific = false;
    for (const auto& a : armed_) if (a.first.compare(0, 4, "cmd:") == 0) { anySpecific = true; break; }
    if (anySpecific && commandName) {
        const std::string name = commandName();
        if (!name.empty() && TakeTarget("cmd:" + name, out)) return true;
    }
    return TakeTarget("cmd", out);
}

void FaultPlan::Clear() {
    armed_.clear();
    dateSkewSeconds = 0;
    rejectFormat    = RejectFormat::Text;
}

}  // namespace prrofs
```

- [ ] **Step 4: Зібрати й прогнати — ЗЕЛЕНИЙ**

Run: `cmake --build build_x64 --config Release --target prro_fs_state_selftest && ./bin/Release/prro_fs_state_selftest_x64.exe; echo exit=$?`
Expected: `=== ALL PASS ===`, `exit=0`.

- [ ] **Step 5: Негативна верифікація** — відкотити кожну після перевірки:
  - `TakeTarget`: прибрати `armed_.erase(it);` → червоніє `вдруге не спрацьовує (витрачено)`.
  - `Take`: викликати `commandName()` безумовно (прибрати `anySpecific &&`) → червоніє `лише загальний cmd: ім'я команди не розбиралось`.
  - `Arm`: прибрати цикл перевірки дубля → червоніє `повторне взведення … -> Conflict`.

- [ ] **Step 6: Commit**

```bash
git add tests/support/FaultPlan.h tests/support/FaultPlan.cpp tests/prro_fs_state_selftest.cpp tests/CMakeLists.txt
git commit --only -m "feat(tests): FaultPlan — одноразові збої з фільтром цілі для імітації ДПС" -m "<пари негативної верифікації зі Step 5>" -- tests/support/FaultPlan.h tests/support/FaultPlan.cpp tests/prro_fs_state_selftest.cpp tests/CMakeLists.txt
```

---

### Task 5: `PrroFsService` + `prro_fs_emulator` — маршрути ДПС, самотест протоколу, рівень L-f2

**Files:**
- Create: `tests/support/PrroFsService.h`, `tests/support/PrroFsService.cpp`
- Create: `tests/prro_fs_emulator.cpp`, `tests/prro_fs_emulator_selftest.cpp`
- Modify: `tests/CMakeLists.txt` (ціль `prro_fs_emulator` після блоку `uapki_fiscal_emulator`)
- Modify: `run_tests.ps1` (рівень L-f2)

**Interfaces:**
- Consumes: `oracle::*` (Task 2), `prrofs::FiscalServerState`, `ParseDocument`, `InsertOrderTaxNum`, `FormatMoney`, `Utf8ToCp1251`, `ErrorCodeName` (Task 3), `prrofs::FaultPlan`, `FaultMode`, `Fault`, `RejectFormat`, `FaultModeName` (Task 4), `minihttp::*`, `ClientResult`, `Request` (Task 1), `prrofs::LoadFixture` (Task 3).
- Produces: `prrofs::ServerNow(int) -> std::time_t`, `HttpDate(std::time_t)`, `IsoLocal(std::time_t)`, `OrderDate(std::time_t)`, `OrderTime(std::time_t)`, `bool ParseDateTime(const std::string&, long long&)`, `std::string ErrorBody(int, const std::string&)`; `class prrofs::PrroFsService { explicit PrroFsService(int port); void SetPort(int); void SetTrace(bool); minihttp::Response Handle(const minihttp::Request&); minihttp::HeaderList CommonHeaders(); }`; `int RunPrroFsSelfTest();` (у `prro_fs_emulator_selftest.cpp`), статичні хелпери самотесту `Fx`, `PostDoc`, `PostCmd`, `Control`, `State`, `Body`, `Data`, `Reset`, `TicketOk`, `LastNumber`, `RegState`, `HasDoc` — ними користується Task 6.

- [ ] **Step 1: `tests/support/PrroFsService.h`** (повний заголовок; методи збоїв `CommandNameOf`, `FaultStatus`, `FaultDrop` реалізує Task 6)

```cpp
#pragma once
// PrroFsService — обробник HTTP-запитів імітації фіскального сервера ДПС
// (спека docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §5, §6, §8).
// Крипто — oracle::* (UapkiOracle), стан — FiscalServerState, збої — FaultPlan.
// Потокобезпечний: MiniHttpServer кличе Handle з потоку кожного з'єднання.
// УВАГА: тягне MiniHttpServer.h (winsock2.h) — включати ДО windows.h.
#include <ctime>
#include <mutex>
#include <string>

#include "FaultPlan.h"
#include "FiscalServerState.h"
#include "MiniHttpServer.h"

namespace prrofs {

// Годинник сервера = системний час + зсув (§5, §8.3).
std::time_t ServerNow(int skewSeconds);
std::string HttpDate(std::time_t t);    // RFC 1123, GMT: "Thu, 24 Sep 2026 09:00:00 GMT"
std::string IsoLocal(std::time_t t);    // ISO 8601 з місцевим зсувом: "2026-09-24T12:00:00+03:00"
std::string OrderDate(std::time_t t);   // ddmmyyyy, місцевий час
std::string OrderTime(std::time_t t);   // hhmmss, місцевий час
// ISO 8601 ("Z", "±hh:mm" або без зсуву = місцевий) чи "/Date(ms)/" -> epoch. false — не розібрано.
bool        ParseDateTime(const std::string& s, long long& epoch);
// "Код помилки: <n> <Назва>\r\n<опис>" ([Опис] ~899-901).
std::string ErrorBody(int code, const std::string& description);

class PrroFsService {
public:
    explicit PrroFsService(int port) : port_(port) {}
    void SetPort(int port) { port_ = port; }
    void SetTrace(bool on) { trace_ = on; }

    minihttp::Response   Handle(const minihttp::Request& req);
    minihttp::HeaderList CommonHeaders();             // Date за годинником сервера

private:
    minihttp::Response HandleDoc(const std::string& body);
    minihttp::Response HandleCmd(const std::string& body);
    minihttp::Response HandleControl(const std::string& body);
    minihttp::Response StateJson();
    minihttp::Response Reject(int code, const std::string& text, const ParsedDoc* doc);
    std::string        BuildTicket(const ParsedDoc* doc, const std::string& taxNum, std::time_t now,
                                   int errorCode, const std::string& errorTextUtf8);
    // Збої (Task 6):
    std::string        CommandNameOf(const std::string& body);
    minihttp::Response FaultStatus(const Fault& f);
    static minihttp::Response FaultDrop(const Fault& f);

    std::mutex        mx_;          // state_ + faults_
    FiscalServerState state_;
    FaultPlan         faults_;
    int               port_  = 0;
    bool              trace_ = true;
};

}  // namespace prrofs
```

- [ ] **Step 2: Самотест протоколу (червоний) — `tests/prro_fs_emulator_selftest.cpp`**

```cpp
//
// @file tests/prro_fs_emulator_selftest.cpp
// @brief prro_fs_emulator --self-test: сценарії споживача на рівні протоколу, без 1С
//        (спека docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §10.2).
//        Сервер піднімається in-process на вільному порту 18200..18299, клієнт —
//        MiniHttpClient. Крипто вже ініціалізовано в main (oracle::Init).
//
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "support/MiniHttpServer.h"   // winsock2.h — до windows.h
#include "support/MiniHttpClient.h"
#include "support/PrroFsFixtures.h"
#include "support/PrroFsService.h"
#include "support/UapkiOracle.h"

#include <windows.h>

using nlohmann::json;

#ifndef PRRO_FS_DATA_DIR
#  define PRRO_FS_DATA_DIR ""
#endif

namespace {

int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("[FAIL] %s\n", msg); ++g_failures; } \
                              else { std::printf("[PASS] %s\n", msg); } } while(0)

const char* kReg = "4000000001";

std::string Fx(const char* name, long long n, const char* reg = kReg) {
    return prrofs::LoadFixture(PRRO_FS_DATA_DIR, name, n, reg);
}

minihttp::ClientResult PostDoc(int port, const char* fixture, long long n, int timeoutMs = 5000) {
    return minihttp::Request(port, "POST", "/fs/doc", oracle::Sign(Fx(fixture, n)),
                             "application/octet-stream", timeoutMs);
}

minihttp::ClientResult PostCmd(int port, const json& q, bool sign, int timeoutMs = 5000) {
    const std::string text = q.dump();
    if (!sign) return minihttp::Request(port, "POST", "/fs/cmd", text, "application/json", timeoutMs);
    return minihttp::Request(port, "POST", "/fs/cmd", oracle::Sign(text), "application/octet-stream", timeoutMs);
}

minihttp::ClientResult Control(int port, const json& q) {
    return minihttp::Request(port, "POST", "/control", q.dump(), "application/json", 5000);
}

json Body(const minihttp::ClientResult& r) {
    json j = json::parse(r.body, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

json State(int port) {
    return Body(minihttp::Request(port, "GET", "/control/state", "", "", 5000));
}

std::string Data(const json& j) {
    return (j.contains("Data") && j["Data"].is_string()) ? j["Data"].get<std::string>() : std::string();
}

bool Reset(int port, long long next = 1) {
    const minihttp::ClientResult r = Control(port, { { "action", "reset" },
        { "registrars", json::array({ { { "numFiscal", kReg }, { "nextLocalNum", next } } }) } });
    return r.responded && r.code == 200;
}

// Квитанція: CMS -> XML. true, якщо підпис прийнято, ERRORCODE 0 і є ORDERTAXNUM.
bool TicketOk(const minihttp::ClientResult& r, std::string& taxNum) {
    taxNum.clear();
    if (!r.responded || r.code != 200) return false;
    const oracle::VerifyOutcome v = oracle::Verify(r.body);
    std::string xml;
    if (!v.accepted || !oracle::b64decode(v.contentB64, xml)) return false;
    if (xml.find("<ERRORCODE>0</ERRORCODE>") == std::string::npos) return false;
    const size_t a = xml.find("<ORDERTAXNUM>");
    const size_t b = xml.find("</ORDERTAXNUM>");
    if (a == std::string::npos || b == std::string::npos) return false;
    taxNum = xml.substr(a + 13, b - a - 13);
    return !taxNum.empty();
}

long long LastNumber(const std::string& s) {
    const size_t e = s.find_last_of("0123456789");
    if (e == std::string::npos) return -1;
    size_t b = e;
    while (b > 0 && s[b - 1] >= '0' && s[b - 1] <= '9') --b;
    return std::stoll(s.substr(b, e - b + 1));
}

json RegState(int port) {
    json st = State(port);
    if (st.contains("registrars") && st["registrars"].is_array())
        for (const json& r : st["registrars"]) if (r.value("numFiscal", std::string()) == kReg) return r;
    return json::object();
}

bool HasDoc(int port, long long localNum) {
    json st = State(port);
    if (st.contains("documents") && st["documents"].is_array())
        for (const json& d : st["documents"])
            if (d.value("registrar", std::string()) == kReg && d.value("localNum", 0LL) == localNum) return true;
    return false;
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

void ScenarioPing(int port) {
    std::printf("== /ping і невідомий шлях ==\n");
    const minihttp::ClientResult p = minihttp::Request(port, "GET", "/ping", "", "", 3000);
    CHECK(p.responded && p.code == 200 && p.body == "prro_fs_emulator alive",
          "/ping: тіло ідентифікує сервіс (не старий оракул)");
    CHECK(p.headers.count("date") == 1, "/ping: заголовок Date");
    const minihttp::ClientResult nf = minihttp::Request(port, "GET", "/doc", "", "", 3000);
    CHECK(nf.responded && nf.code == 404 && nf.headers.count("date") == 1, "невідомий шлях -> 404 з Date");
}

// Сценарій споживача 1: зміна повністю + запити, якими продукт з'ясовує стан.
void ScenarioShift(int port) {
    std::printf("== Сценарій 1: зміна ==\n");
    CHECK(Reset(port), "reset: ПРРО зареєстровано");
    std::string t1, t2, t3, tz, tc;
    CHECK(TicketOk(PostDoc(port, "open_shift_1251.xml", 1), t1), "відкриття зміни прийнято, квитанція з ORDERTAXNUM");
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "LastShiftTotals" }, { "NumFiscal", kReg }, { "UID", "u1" } }, true);
        json j = Body(r);
        CHECK(r.code == 200 && j["ShiftState"] == 1 && j["Totals"].is_object(), "LastShiftTotals: зміна відкрита, Totals є");
        json& t = j["Totals"];
        CHECK(t["Real"].is_object() && t["Real"]["OrdersCount"] == 0 && t["Real"]["PayForm"].is_array()
              && t["Real"]["PayForm"].empty() && t["Real"]["Tax"].is_array(),
              "Totals без чеків: Real — об'єкт з нулями й порожніми масивами (Review Focus 5)");
        CHECK(t["Ret"].is_object() && t["ServiceInput"].is_number() && t["ServiceOutput"].is_number(),
              "Totals без чеків: Ret, ServiceInput, ServiceOutput присутні");
    }
    CHECK(TicketOk(PostDoc(port, "check_sale_1251.xml", 2), t2), "чек продажу прийнято");
    CHECK(TicketOk(PostDoc(port, "check_return_1251.xml", 3), t3), "повернення прийнято");
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "TransactionsRegistrarState" }, { "NumFiscal", kReg }, { "UID", "u2" } }, true);
        json j = Body(r);
        CHECK(r.code == 200 && j["ShiftState"] == 1 && j["NextLocalNum"] == 4,
              "TransactionsRegistrarState: зміна відкрита, NextLocalNum = 4");
        CHECK(j["Name"] == "Тестовий Касир", "TransactionsRegistrarState: Name з CASHIER документа відкриття");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "LastShiftTotals" }, { "NumFiscal", kReg }, { "UID", "u3" } }, true);
        json j = Body(r);
        json& real = j["Totals"]["Real"];
        CHECK(r.code == 200 && real["OrdersCount"] == 1 && real["Sum"] == 150.0, "LastShiftTotals: Real = один чек на 150.00");
        CHECK(real["PayForm"].size() == 2 && real["Tax"].size() == 1 && real["Tax"][0].contains("TurnoverDiscount"),
              "LastShiftTotals: PayForm/Tax агреговано, TurnoverDiscount є");
        CHECK(j["Totals"]["Ret"]["OrdersCount"] == 1 && j["Totals"]["Ret"]["Sum"] == 40.0,
              "LastShiftTotals: Ret = повернення на 40.00");
    }
    CHECK(TicketOk(PostDoc(port, "zrep_1251.xml", 4), tz), "Z-звіт прийнято");
    CHECK(TicketOk(PostDoc(port, "close_shift_1251.xml", 5), tc), "закриття зміни прийнято");
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "Shifts" }, { "NumFiscal", kReg }, { "UID", "u4" } }, true);
        json j = Body(r);
        CHECK(r.code == 200 && j["Shifts"].size() == 1 && j["Shifts"][0]["ZRepFiscalNum"] == tz,
              "Shifts: ZRepFiscalNum = фіскальний номер Z-звіту");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "ZRepExt" }, { "RegistrarNumFiscal", kReg },
                                                         { "NumFiscal", tz }, { "Type", 1 }, { "UID", "u5" } }, true);
        json j = Body(r);
        std::string data;
        CHECK(r.code == 200 && j["ResultCode"] == 0 && oracle::b64decode(Data(j), data) && data == Fx("zrep_1251.xml", 4),
              "ZRepExt Type 1: побайтовий оригінал Z-звіту");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                         { "NumLocal", 2 }, { "Type", 2 }, { "UID", "u6" } }, false);
        json j = Body(r);
        std::string der, xml;
        const bool got = r.code == 200 && j["ResultCode"] == 0 && oracle::b64decode(Data(j), der) && !der.empty();
        const oracle::VerifyOutcome v = oracle::Verify(der);
        CHECK(got && v.accepted && oracle::b64decode(v.contentB64, xml), "CheckExt Type 2: Data — CMS, що проходить перевірку");
        CHECK(xml.find("<ORDERTAXNUM>" + t2 + "</ORDERTAXNUM></CHECKHEAD>") != std::string::npos,
              "CheckExt Type 2: у XML сервера вставлено ORDERTAXNUM чека");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                         { "NumLocal", "2" }, { "Type", 3 }, { "UID", "u7" } }, false);
        std::string text;
        CHECK(r.code == 200 && oracle::b64decode(Data(Body(r)), text) && text.find(t2) != std::string::npos
              && text.find("Чек") != std::string::npos,
              "CheckExt Type 3: UTF-8 візуалізація з фіскальним номером; NumLocal рядком приймається (Review Focus 3)");
    }
    {
        const minihttp::ClientResult r = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                         { "NumLocal", 99 }, { "Type", 2 }, { "UID", "u8" } }, false);
        CHECK(r.code == 200 && Body(r)["ResultCode"] == 5, "CheckExt неіснуючого номера: ResultCode 5 DocumentAbsent");
    }
}

void ScenarioFormats(int port) {
    std::printf("== Формати відповідей і відмов ==\n");
    CHECK(Reset(port), "reset для перевірок формату");
    {
        const minihttp::ClientResult r = PostDoc(port, "open_shift_1251.xml", 7);
        CHECK(r.responded && r.code == 400 && StartsWith(r.body, "Код помилки: 7 CheckLocalNumberInvalid"),
              "неправильний локальний номер: 400 + «Код помилки: 7 CheckLocalNumberInvalid»");
        CHECK(r.body.find("Номер документа повинен дорівнювати") != std::string::npos && LastNumber(r.body) == 1,
              "текст містить фразу, N = 1 — останнє число");
        CHECK(r.headers.count("date") == 1, "відмова теж має Date");
    }
    {
        const minihttp::ClientResult a = minihttp::Request(port, "POST", "/fs/doc", std::string(9, 'x'), "application/octet-stream", 5000);
        const minihttp::ClientResult b = minihttp::Request(port, "POST", "/fs/doc", std::string(512001, 'x'), "application/octet-stream", 10000);
        const minihttp::ClientResult c = minihttp::Request(port, "POST", "/fs/cmd", std::string(9, ' '), "application/json", 5000);
        CHECK(a.code == 416 && b.code == 416 && c.code == 416, "розмір поза 10…512000 -> 416 (doc 9 і 512001 байт, cmd 9 байт)");
    }
    {
        std::string der = oracle::Sign(Fx("open_shift_1251.xml", 1));
        const size_t pos = der.find("<DOCTYPE>100</DOCTYPE>");
        if (pos != std::string::npos) der[pos + 9] = static_cast<char>(der[pos + 9] ^ 0x01);   // '1' -> '0' у підписаному вмісті
        const minihttp::ClientResult r = minihttp::Request(port, "POST", "/fs/doc", der, "application/octet-stream", 5000);
        CHECK(pos != std::string::npos && r.code == 400 && StartsWith(r.body, "Код помилки: 9 DocumentValidationError"),
              "зіпсований CMS -> 400, код 9");
        CHECK(RegState(port)["nextLocalNum"] == 1, "відхилений документ номер не зайняв (NextLocalNum = 1)");
    }
    {
        const std::string der = oracle::Sign(Fx("open_shift_1251.xml", 1, "4999999999"));
        const minihttp::ClientResult r = minihttp::Request(port, "POST", "/fs/doc", der, "application/octet-stream", 5000);
        CHECK(r.code == 400 && StartsWith(r.body, "Код помилки: 1 TransactionsRegistrarAbsent"), "документ невідомого ПРРО -> 400, код 1");
    }
    {
        const minihttp::ClientResult a = PostCmd(port, { { "Command", "Objects" }, { "UID", "o1" } }, false);
        CHECK(a.code == 400 && StartsWith(a.body, "Код помилки: 9"), "Objects без підпису -> 400, код 9");
        const minihttp::ClientResult b = PostCmd(port, { { "Command", "Objects" }, { "UID", "o2" } }, true);
        json j = Body(b);
        CHECK(b.code == 200 && j["TaxObjects"][0]["TransactionsRegistrars"][0]["NumFiscal"] == 4000000001LL,
              "Objects: ПРРО з reset, NumFiscal числом");
        const minihttp::ClientResult c = minihttp::Request(port, "POST", "/fs/cmd",
            "\xEF\xBB\xBF  {\"Command\":\"ServerState\",\"UID\":\"s1\"}", "application/json", 5000);
        json jc = Body(c);
        CHECK(c.code == 200 && jc["UID"] == "s1" && jc.contains("Timestamp"),
              "ServerState без підпису, з BOM і пробілами -> 200 (Review Focus 2)");
        const minihttp::ClientResult d = PostCmd(port, { { "Command", "NoSuchCommand" }, { "UID", "x" } }, true);
        CHECK(d.code == 400 && StartsWith(d.body, "Код помилки: 11"), "невідома команда -> 400, код 11");
        const minihttp::ClientResult e = PostCmd(port, { { "Command", "LastShiftTotals" }, { "NumFiscal", "4999999999" }, { "UID", "x" } }, true);
        CHECK(e.code == 204 && e.body.empty(), "LastShiftTotals невідомого ПРРО -> 204 без тіла");
        const minihttp::ClientResult f = PostCmd(port, { { "Command", "TransactionsRegistrarState" }, { "NumFiscal", 4000000001LL }, { "UID", "x" } }, true);
        CHECK(f.code == 200 && Body(f)["NextLocalNum"] == 1, "TransactionsRegistrarState: NumFiscal числом приймається (Review Focus 3)");
    }
    // Сценарій 5 споживача: лічильник каси розійшовся із сервером — розбіжність створюється
    // чесно, через reset з іншим nextLocalNum (§8.4), а не підробленою відповіддю.
    CHECK(Reset(port, 5), "reset із nextLocalNum = 5 (розбіжність лічильника, сценарій 5)");
    {
        const minihttp::ClientResult r = PostDoc(port, "open_shift_1251.xml", 1);
        CHECK(r.code == 400 && LastNumber(r.body) == 5, "каса шле №1, сервер чекає №5 -> «повинен дорівнювати 5»");
        std::string t;
        CHECK(TicketOk(PostDoc(port, "open_shift_1251.xml", 5), t), "після самовідновлення лічильника №5 проходить");
    }
}

}  // namespace

int RunPrroFsSelfTest() {
    std::printf("\n==== prro_fs_emulator self-test ====\n");
    if (!minihttp::InitNetwork()) { std::printf("[FAIL] WSAStartup\n"); return 1; }

    std::unique_ptr<prrofs::PrroFsService> svc;
    std::unique_ptr<minihttp::Server>      srv;
    int port = 0;
    for (int p = 18200; p < 18300 && !srv; ++p) {
        std::unique_ptr<prrofs::PrroFsService> s(new prrofs::PrroFsService(p));
        std::unique_ptr<minihttp::Server>      h(new minihttp::Server(p, "127.0.0.1"));
        prrofs::PrroFsService* raw = s.get();
        raw->SetTrace(false);
        h->SetHandler([raw](const minihttp::Request& r) { return raw->Handle(r); });
        h->SetCommonHeaders([raw]() { return raw->CommonHeaders(); });
        if (h->Listen().first) { h->Start(); svc = std::move(s); srv = std::move(h); port = p; }
    }
    CHECK(srv != nullptr, "сервер self-test піднявся на вільному порту 18200..18299");
    if (srv) {
        ScenarioPing(port);
        ScenarioShift(port);
        ScenarioFormats(port);
        srv->Stop();
    }
    minihttp::ShutdownNetwork();
    std::printf(g_failures ? "\n=== FAILED: %d ===\n" : "\n=== ALL PASS ===\n", g_failures);
    return g_failures ? 1 : 0;
}
```

- [ ] **Step 3: `tests/prro_fs_emulator.cpp`**

```cpp
//
// @file tests/prro_fs_emulator.cpp
// @brief Імітація фіскального сервера ДПС для наскрізних тестів ПРРО
//        (спека docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md).
//        Ендпоінти: GET /ping; POST /fs/doc; POST /fs/cmd; POST /control; GET /control/state.
//        Базова адреса для споживача — http://127.0.0.1:<port>/fs.
// CLI: prro_fs_emulator [port] [--key <p12>] [--pass <pwd>] [--providers <dir>] [--data <dir>] [--self-test]
// Коди повернення: 0 — штатно; 1 — не піднявся порт / провал self-test; 2 — CLI/bootstrap.
//
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "support/MiniHttpServer.h"   // winsock2.h — до windows.h
#include "support/PrroFsService.h"
#include "support/UapkiOracle.h"

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW

#pragma comment(lib, "shell32.lib")

#ifndef HOST_DATA_DIR
#  define HOST_DATA_DIR ""
#endif

int RunPrroFsSelfTest();   // tests/prro_fs_emulator_selftest.cpp

namespace {

struct Config {
    int          port = 8099;
    std::wstring keyPath, providersDir, dataDir;
    std::string  pass = "testpassword";
    bool         selfTest = false;
};

std::mutex              g_stopMx;
std::condition_variable g_stopCv;
bool                    g_stopRequested = false;   // guarded by g_stopMx

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
        { std::lock_guard<std::mutex> lk(g_stopMx); g_stopRequested = true; }
        g_stopCv.notify_all();
        oracle::Shutdown();
    }
    return FALSE;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring p(buf, n);
    const size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : p.substr(0, pos);
}

void Usage() {
    std::printf(
        "prro_fs_emulator [port] [--key <p12>] [--pass <pwd>] [--providers <dir>] [--data <dir>] [--self-test]\n"
        "  port        порт (деф. 8099; зайнятий і не заданий явно — наступний вільний до +10)\n"
        "  --key       PKCS#12 ключа «сервера» (деф. <data>\\test-diia.p12)\n"
        "  --pass      пароль контейнера (деф. testpassword)\n"
        "  --providers каталог із cm-pkcs12_*.dll (деф. каталог цього exe)\n"
        "  --data      каталог тест-даних certs/ + crls/ (деф. compile-time tests/data)\n"
        "  --self-test сценарії протоколу in-process і вихід\n"
        "Керування: POST /control {\"action\":\"reset\"|\"fault\"|\"set\",…}, GET /control/state.\n");
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) { std::printf("FAIL: CommandLineToArgvW\n"); return 2; }

    Config cfg;
    bool bad = false, portSet = false;
    for (int i = 1; i < argc && !bad; ++i) {
        const std::wstring a = wargv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 < argc) return std::wstring(wargv[++i]);
            std::printf("Аргумент %s потребує значення\n", oracle::w2u8(a).c_str());
            bad = true;
            return std::wstring();
        };
        if      (a == L"--key")       cfg.keyPath      = next();
        else if (a == L"--pass")      cfg.pass         = oracle::w2u8(next());
        else if (a == L"--providers") cfg.providersDir = next();
        else if (a == L"--data")      cfg.dataDir      = next();
        else if (a == L"--self-test") cfg.selfTest     = true;
        else if (!a.empty() && a[0] != L'-' && !portSet) { cfg.port = _wtoi(a.c_str()); portSet = true; }
        else { std::printf("Невідомий аргумент: %s\n", oracle::w2u8(a).c_str()); bad = true; }
    }
    LocalFree(wargv);
    if (bad) { Usage(); return 2; }
    if (cfg.port <= 0 || cfg.port > 65535) { std::printf("Некоректний порт: %d\n", cfg.port); Usage(); return 2; }

    if (cfg.dataDir.empty())      cfg.dataDir      = oracle::u8to16(HOST_DATA_DIR);
    if (cfg.keyPath.empty())      cfg.keyPath      = cfg.dataDir + L"\\test-diia.p12";
    if (cfg.providersDir.empty()) cfg.providersDir = ExeDir();

    std::printf("prro_fs_emulator\n  port=%d\n  data=%s\n  key=%s\n  providers=%s\n", cfg.port,
                oracle::w2u8(cfg.dataDir).c_str(), oracle::w2u8(cfg.keyPath).c_str(),
                oracle::w2u8(cfg.providersDir).c_str());
    std::fflush(stdout);

    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    if (!oracle::Init(oracle::OracleConfig{ cfg.providersDir, cfg.dataDir, cfg.keyPath, cfg.pass })) {
        std::printf("Bootstrap не вдався — вихід\n");
        oracle::Shutdown();
        return 2;
    }

    if (cfg.selfTest) {
        const int rc = RunPrroFsSelfTest();
        oracle::Shutdown();
        return rc;
    }

    if (!minihttp::InitNetwork()) { std::printf("Не вдалося ініціалізувати Winsock\n"); oracle::Shutdown(); return 1; }

    prrofs::PrroFsService service(cfg.port);
    std::unique_ptr<minihttp::Server> server;
    int chosenPort = 0;
    std::string listenErr;
    const int attempts = portSet ? 1 : 11;
    for (int i = 0; i < attempts; ++i) {
        const int tryPort = cfg.port + i;
        if (tryPort > 65535) break;
        std::unique_ptr<minihttp::Server> s(new minihttp::Server(tryPort, "127.0.0.1"));
        s->SetHandler([&service](const minihttp::Request& r) { return service.Handle(r); });
        s->SetCommonHeaders([&service]() { return service.CommonHeaders(); });
        const std::pair<bool, std::string> res = s->Listen();
        if (res.first) { server = std::move(s); chosenPort = tryPort; break; }
        listenErr = res.second;
        if (i == 0) std::printf("Порт %d зайнятий (%s)%s\n", tryPort, listenErr.c_str(),
                                attempts > 1 ? " — шукаю вільний" : "");
    }
    if (!server) {
        std::printf("Не вдалося зайняти порт %d: %s\n", cfg.port, listenErr.c_str());
        minihttp::ShutdownNetwork();
        oracle::Shutdown();
        return 1;
    }
    service.SetPort(chosenPort);
    server->Start();
    std::printf("\nprro_fs_emulator слухає 127.0.0.1:%d — базова адреса http://127.0.0.1:%d/fs — Ctrl-C для виходу\n",
                chosenPort, chosenPort);
    if (chosenPort != cfg.port)
        std::printf("УВАГА: порт %d був зайнятий. У параметрі АдресСервераДПС вкажіть http://127.0.0.1:%d/fs\n",
                    cfg.port, chosenPort);
    std::fflush(stdout);

    {
        std::unique_lock<std::mutex> lk(g_stopMx);
        g_stopCv.wait(lk, [] { return g_stopRequested; });
    }
    server->Stop();
    minihttp::ShutdownNetwork();
    oracle::Shutdown();
    return 0;
}
```

- [ ] **Step 4: CMake — ціль `prro_fs_emulator`** — після блоку `uapki_fiscal_emulator` (після його `target_compile_options … /utf-8` + `endif()`):

```cmake
# ---------------------------------------------------------------------------
# prro_fs_emulator — імітація фіскального сервера ДПС зі станом і керованими
# збоями для наскрізних тестів ПРРО споживача (SMP_SimplyConnect). Ручний
# інструмент + рівень гейта L-f2 через вбудований --self-test.
# Спека: docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md.
# ---------------------------------------------------------------------------
add_executable(prro_fs_emulator prro_fs_emulator.cpp prro_fs_emulator_selftest.cpp
    support/PrroFsService.cpp
    support/FiscalServerState.cpp
    support/FaultPlan.cpp
    support/UapkiOracle.cpp
    support/MiniHttpServer.cpp
    support/MiniHttpClient.cpp
    ${CMAKE_SOURCE_DIR}/extern/pugixml/src/pugixml.cpp
)
set_target_properties(prro_fs_emulator PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "prro_fs_emulator${_TEST_ARCH_SUFFIX}"
)
target_link_libraries(prro_fs_emulator PRIVATE uapki_bundle parson ws2_32)
target_include_directories(prro_fs_emulator PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/extern/pugixml/src
)
target_compile_definitions(prro_fs_emulator PRIVATE _WINDOWS UNICODE _UNICODE
    HOST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
    PRRO_FS_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/prro_fs"
)
add_dependencies(prro_fs_emulator cm-pkcs12-provider)
if(MSVC)
    target_compile_options(prro_fs_emulator PRIVATE /utf-8)
endif()
```

Run: `cmake --build build_x64 --config Release --target prro_fs_emulator`
Expected: FAIL — лінкер: `unresolved external symbol … PrroFsService::Handle …` (реалізації ще немає).

- [ ] **Step 5: `tests/support/PrroFsService.cpp`** (версія Task 5: без збоїв; `Handle` і `HandleControl` Task 6 замінить)

```cpp
// PrroFsService — реалізація. Опис — у заголовку.
// УВАГА: pch.h НЕ підключається (правило tests/).
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include "PrroFsService.h"      // тягне winsock2.h — до windows.h

#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "UapkiOracle.h"

#include <windows.h>

using nlohmann::json;

namespace prrofs {
namespace {

const size_t kMinBody = 10;        // [споживач] вимір на бойовому ДПС (§5)
const size_t kMaxBody = 512000;
const char*  kSubjectKeyId = "0000000000000000000000000000000000000000000000000000000000000000";

std::string StripJsonPrefix(const std::string& s) {
    size_t i = 0;
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF
        && static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) i = 3;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return s.substr(i);
}

std::string StrField(const json& q, const char* k) {
    if (!q.contains(k) || q[k].is_null()) return std::string();
    if (q[k].is_string()) return q[k].get<std::string>();
    if (q[k].is_number_integer()) return std::to_string(q[k].get<long long>());
    return std::string();
}

bool IntField(const json& q, const char* k, long long& out) {
    if (!q.contains(k) || q[k].is_null()) return false;
    if (q[k].is_number_integer()) { out = q[k].get<long long>(); return true; }
    if (q[k].is_string()) {
        const std::string s = q[k].get<std::string>();
        char* e = nullptr;
        out = std::strtoll(s.c_str(), &e, 10);
        return !s.empty() && *e == '\0';
    }
    return false;
}

json Money(int64_t v) { return json(static_cast<double>(v) / 100.0); }
json NullIfEmpty(const std::string& s) { return s.empty() ? json(nullptr) : json(s); }

std::string XmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

minihttp::Response Text(int code, const std::string& text) {
    minihttp::Response r;
    r.code = code;
    r.contentType = "text/plain; charset=utf-8";
    r.body = text;
    return r;
}

minihttp::Response Json(int code, const json& j) {
    minihttp::Response r;
    r.code = code;
    r.contentType = "application/json; charset=utf-8";
    r.body = j.dump();
    return r;
}

minihttp::Response Binary(const std::string& der) {
    minihttp::Response r;
    r.contentType = "application/octet-stream";
    r.body = der;
    return r;
}

minihttp::Response NoContent() {
    minihttp::Response r;
    r.code = 204;
    return r;
}

minihttp::Response ErrorResponse(int code, const std::string& text) {
    return Text(400, ErrorBody(code, text));
}

json TaxObjectJson(const std::vector<const RegistrarState*>& regs) {
    json trs = json::array();
    for (const RegistrarState* r : regs) {
        trs.push_back({ { "NumFiscal", std::stoll(r->numFiscal) }, { "NumLocal", r->numLocal },
                        { "Name", "Тестовий ПРРО " + std::to_string(r->numLocal) }, { "Closed", false } });
    }
    return { { "Entity", 1 }, { "TaxObjGuid", "00000000-0000-0000-0000-000000000001" }, { "TaxObjId", 1 },
             { "SingleTax", false }, { "Name", "Тестова господарська одиниця" },
             { "Address", "м. Київ, вул. Тестова, 1" }, { "Tin", "99900000" }, { "Ipn", "999000000009" },
             { "OrgName", "ТОВ \"ТЕСТ\"" }, { "ChiefCashier", true }, { "TransactionsRegistrars", trs } };
}

json OrderJson(const OrderTypeTotals& o) {
    json pf = json::array();
    for (const PayFormTotal& p : o.payForms)
        pf.push_back({ { "PayFormCode", p.code }, { "PayFormName", p.name }, { "Sum", Money(p.sum) } });
    json tx = json::array();
    for (const TaxTotal& t : o.taxes)
        tx.push_back({ { "Type", t.type }, { "Name", t.name }, { "Letter", t.letter }, { "Prc", Money(t.prc) },
                       { "Sign", t.sign }, { "Turnover", Money(t.turnover) },
                       { "TurnoverDiscount", Money(t.turnoverDiscount) }, { "SourceSum", Money(t.sourceSum) },
                       { "Sum", Money(t.sum) } });
    return { { "Sum", Money(o.sum) }, { "PwnSumIssued", 0 }, { "PwnSumReceived", 0 },
             { "RndSum", Money(o.rndSum) }, { "NoRndSum", Money(o.noRndSum) },
             { "TotalCurrencySum", 0 }, { "TotalCurrencyCommission", 0 },
             { "OrdersCount", o.ordersCount }, { "PwnOrdersCountIssued", 0 }, { "PwnOrdersCountReceived", 0 },
             { "TotalCurrencyCost", 0 }, { "PayForm", pf }, { "Tax", tx } };
}

// Totals — ЗАВЖДИ повної форми (§4.7): друк споживача звертається до ключів прямо.
json TotalsJson(const ShiftTotals& t) {
    return { { "Real", OrderJson(t.real) }, { "Ret", OrderJson(t.ret) }, { "Cash", nullptr },
             { "Currency", nullptr }, { "ServiceInput", Money(t.serviceInput) },
             { "ServiceOutput", Money(t.serviceOutput) } };
}

json ShiftJson(const ShiftRecord& s) {
    const bool closed = !s.closed.empty();
    return { { "ShiftId", s.shiftId }, { "OpenShiftFiscalNum", s.openShiftFiscalNum },
             { "CloseShiftFiscalNum", NullIfEmpty(s.closeShiftFiscalNum) }, { "Testing", s.testing },
             { "Opened", s.opened }, { "OpenName", s.openName }, { "OpenSubjectKeyId", kSubjectKeyId },
             { "Closed", NullIfEmpty(s.closed) }, { "CloseName", NullIfEmpty(s.closeName) },
             { "CloseSubjectKeyId", closed ? json(kSubjectKeyId) : json(nullptr) },
             { "ZRepFiscalNum", NullIfEmpty(s.zRepFiscalNum) } };
}

// Візуалізація документа (Type 3): UTF-8 ([Опис] ~633-689).
std::string Visualization(const StoredDoc& d) {
    ParsedDoc p;
    std::string err;
    const bool ok = ParseDocument(d.originalXml, p, err);
    std::string t = (d.klass == DocClass::ZRep) ? "Z-звіт\n" : "Чек\n";
    t += "Локальний номер: " + std::to_string(d.localNum) + "\n";
    t += "Фіскальний номер: " + d.fiscalNum + "\n";
    if (ok && d.klass == DocClass::Check) t += "Сума: " + FormatMoney(p.totalSum) + "\n";
    return t;
}

}  // namespace

std::time_t ServerNow(int skewSeconds) {
    return std::time(nullptr) + skewSeconds;
}

std::string HttpDate(std::time_t t) {
    static const char* kDays[]   = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    std::tm g{};
    gmtime_s(&g, &t);
    char b[64];
    std::snprintf(b, sizeof(b), "%s, %02d %s %04d %02d:%02d:%02d GMT", kDays[g.tm_wday], g.tm_mday,
                  kMonths[g.tm_mon], g.tm_year + 1900, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string IsoLocal(std::time_t t) {
    std::tm l{};
    localtime_s(&l, &t);
    std::tm copy = l;
    const long long offset = static_cast<long long>(_mkgmtime(&copy)) - static_cast<long long>(t);
    const char sign = offset < 0 ? '-' : '+';
    const long long a = offset < 0 ? -offset : offset;
    char b[48];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d%c%02lld:%02lld", l.tm_year + 1900,
                  l.tm_mon + 1, l.tm_mday, l.tm_hour, l.tm_min, l.tm_sec, sign, a / 3600, (a % 3600) / 60);
    return b;
}

std::string OrderDate(std::time_t t) {
    std::tm l{};
    localtime_s(&l, &t);
    char b[16];
    std::snprintf(b, sizeof(b), "%02d%02d%04d", l.tm_mday, l.tm_mon + 1, l.tm_year + 1900);
    return b;
}

std::string OrderTime(std::time_t t) {
    std::tm l{};
    localtime_s(&l, &t);
    char b[16];
    std::snprintf(b, sizeof(b), "%02d%02d%02d", l.tm_hour, l.tm_min, l.tm_sec);
    return b;
}

bool ParseDateTime(const std::string& s, long long& epoch) {
    if (s.rfind("/Date(", 0) == 0) {
        char* e = nullptr;
        const long long ms = std::strtoll(s.c_str() + 6, &e, 10);
        if (e == s.c_str() + 6) return false;
        epoch = ms / 1000;
        return true;
    }
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return false;
    std::tm tmv{};
    tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1; tmv.tm_mday = D;
    tmv.tm_hour = h;        tmv.tm_min = m;     tmv.tm_sec = sec;
    size_t p = 19;
    if (p < s.size() && s[p] == '.') { ++p; while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p; }
    if (p >= s.size()) {                       // без зсуву — місцевий час
        tmv.tm_isdst = -1;
        const std::time_t lt = std::mktime(&tmv);
        if (lt == static_cast<std::time_t>(-1)) return false;
        epoch = static_cast<long long>(lt);
        return true;
    }
    const long long asUtc = static_cast<long long>(_mkgmtime(&tmv));
    if (s[p] == 'Z') { epoch = asUtc; return true; }
    if (s[p] == '+' || s[p] == '-') {
        int oh = 0, om = 0;
        if (std::sscanf(s.c_str() + p + 1, "%2d:%2d", &oh, &om) != 2) return false;
        const long long off = oh * 3600LL + om * 60LL;
        epoch = (s[p] == '+') ? asUtc - off : asUtc + off;
        return true;
    }
    return false;
}

std::string ErrorBody(int code, const std::string& description) {
    return "Код помилки: " + std::to_string(code) + " " + ErrorCodeName(code) + "\r\n" + description;
}

minihttp::HeaderList PrroFsService::CommonHeaders() {
    int skew = 0;
    { std::lock_guard<std::mutex> lk(mx_); skew = faults_.dateSkewSeconds; }
    return { { "Date", HttpDate(ServerNow(skew)) } };
}

minihttp::Response PrroFsService::Handle(const minihttp::Request& req) {
    if (trace_) { std::printf("← %s %s (%zu B)\n", req.method.c_str(), req.uri.c_str(), req.body.size()); std::fflush(stdout); }
    std::string path = req.uri;
    const size_t qm = path.find('?');
    if (qm != std::string::npos) path.resize(qm);

    if (req.method == "GET" && path == "/ping") return Text(200, "prro_fs_emulator alive");
    if (req.method == "POST" && path == "/control") { std::lock_guard<std::mutex> lk(mx_); return HandleControl(req.body); }
    if (req.method == "GET" && path == "/control/state") { std::lock_guard<std::mutex> lk(mx_); return StateJson(); }

    const bool isDoc = (req.method == "POST" && path == "/fs/doc");
    const bool isCmd = (req.method == "POST" && path == "/fs/cmd");
    if (!isDoc && !isCmd) return Text(404, "unknown endpoint");

    std::lock_guard<std::mutex> lk(mx_);
    return isDoc ? HandleDoc(req.body) : HandleCmd(req.body);
}

std::string PrroFsService::BuildTicket(const ParsedDoc* doc, const std::string& taxNum, std::time_t now,
                                       int errorCode, const std::string& errorTextUtf8) {
    std::string x = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\r\n<TICKET>";
    x += "<UID>" + XmlEscape(doc ? doc->uid : std::string()) + "</UID>";
    x += "<ORDERDATE>" + OrderDate(now) + "</ORDERDATE>";
    x += "<ORDERTIME>" + OrderTime(now) + "</ORDERTIME>";
    if (doc) x += "<ORDERNUM>" + std::to_string(doc->orderNum) + "</ORDERNUM>";
    if (!taxNum.empty()) x += "<ORDERTAXNUM>" + taxNum + "</ORDERTAXNUM>";
    x += "<OFFLINESESSIONID>0</OFFLINESESSIONID><OFFLINESEED>0</OFFLINESEED>";
    x += "<ERRORCODE>" + std::to_string(errorCode) + "</ERRORCODE>";
    x += "<ERRORTEXT>" + XmlEscape(errorTextUtf8) + "</ERRORTEXT>";
    x += "<VER>1</VER></TICKET>";
    return Utf8ToCp1251(x);
}

minihttp::Response PrroFsService::Reject(int code, const std::string& text, const ParsedDoc* doc) {
    if (trace_) std::printf("  відмова: %d %s — %s\n", code, ErrorCodeName(code), text.c_str());
    if (faults_.rejectFormat == RejectFormat::Ticket) {
        const std::string der = oracle::Sign(BuildTicket(doc, std::string(),
                                                         ServerNow(faults_.dateSkewSeconds), code, text));
        if (der.empty()) return Text(500, "cannot sign ticket");
        return Binary(der);
    }
    return Text(400, ErrorBody(code, text));
}

minihttp::Response PrroFsService::HandleDoc(const std::string& body) {
    if (body.size() < kMinBody || body.size() > kMaxBody)
        return Text(416, "Недопустимий розмір повідомлення: " + std::to_string(body.size()) + " байт (допустимо 10…512000)");
    const oracle::VerifyOutcome v = oracle::Verify(body);
    std::string xml;
    if (!v.accepted || !oracle::b64decode(v.contentB64, xml))
        return Reject(kDocumentValidationError, "Підпис документа не пройшов перевірку"
                      + (v.errorText.empty() ? std::string() : " (" + v.errorText + ")"), nullptr);
    ParsedDoc d;
    std::string err;
    if (!ParseDocument(xml, d, err)) return Reject(kDocumentValidationError, "Документ не розбирається: " + err, nullptr);

    const std::time_t now = ServerNow(faults_.dateSkewSeconds);
    const SubmitResult sr = state_.Submit(d, xml, IsoLocal(now), static_cast<long long>(now));
    if (sr.errorCode != kOk) return Reject(sr.errorCode, sr.errorText, &d);

    const std::string der = oracle::Sign(BuildTicket(&d, sr.fiscalNum, now, kOk, std::string()));
    if (der.empty()) return Text(500, "cannot sign ticket");
    if (trace_) std::printf("  /fs/doc: прийнято ORDERNUM=%lld ORDERTAXNUM=%s\n", d.orderNum, sr.fiscalNum.c_str());
    return Binary(der);
}

minihttp::Response PrroFsService::HandleCmd(const std::string& body) {
    if (body.size() < kMinBody || body.size() > kMaxBody)
        return Text(416, "Недопустимий розмір повідомлення: " + std::to_string(body.size()) + " байт (допустимо 10…512000)");
    try {
        std::string text = StripJsonPrefix(body);
        bool isSigned = false;
        if (text.empty() || text[0] != '{') {
            const oracle::VerifyOutcome v = oracle::Verify(body);
            std::string content;
            if (!v.accepted || !oracle::b64decode(v.contentB64, content))
                return ErrorResponse(kDocumentValidationError, "Підпис команди не пройшов перевірку");
            text = StripJsonPrefix(content);
            isSigned = true;
        }
        const json q = json::parse(text, nullptr, false);
        if (q.is_discarded() || !q.is_object()) return ErrorResponse(kInvalidQueryParameter, "Запит не є JSON-об'єктом");

        const std::string cmd = StrField(q, "Command");
        const std::string uid = StrField(q, "UID");
        static const std::set<std::string> kSigned = { "Objects", "TransactionsRegistrarState", "ZRepExt",
                                                       "Shifts", "LastShiftTotals" };
        if (kSigned.count(cmd) && !isSigned)
            return ErrorResponse(kDocumentValidationError, "Запит " + cmd + " має бути засвідчений КЕП");
        const std::time_t now = ServerNow(faults_.dateSkewSeconds);
        const std::string ts = IsoLocal(now);

        if (cmd == "ServerState") return Json(200, { { "UID", uid }, { "Timestamp", ts } });

        if (cmd == "Objects") {
            const std::vector<const RegistrarState*> regs = state_.Registrars();
            if (regs.empty()) return NoContent();
            return Json(200, { { "UID", uid }, { "Timestamp", ts }, { "TaxObjects", json::array({ TaxObjectJson(regs) }) } });
        }

        if (cmd == "TransactionsRegistrarState") {
            const RegistrarState* r = state_.Find(StrField(q, "NumFiscal"));
            if (!r) return NoContent();
            json j = {
                { "UID", uid }, { "Timestamp", ts },
                { "ShiftState", r->shiftOpen ? 1 : 0 },
                { "ShiftId", r->shiftId },
                { "OpenShiftFiscalNum", r->shiftOpen ? json(r->openShiftFiscalNum) : json(nullptr) },
                { "ZRepPresent", r->zRepPresent },
                { "Testing", r->testing },
                { "Name", r->shiftId ? json(r->name) : json(nullptr) },
                { "SubjectKeyId", r->shiftOpen ? json(kSubjectKeyId) : json(nullptr) },
                { "FirstLocalNum", r->shiftOpen ? r->firstLocalNum : 0LL },
                { "NextLocalNum", r->nextLocalNum },
                { "LastFiscalNum", NullIfEmpty(r->lastFiscalNum) },
                { "OfflineSupported", false }, { "ChiefCashier", true },
                { "OfflineSessionId", nullptr }, { "OfflineSeed", nullptr }, { "OfflineNextLocalNum", nullptr },
                { "OfflineSessionDuration", nullptr }, { "OfflineSessionsMonthlyDuration", nullptr },
                { "OfflineSessionRolledBack", nullptr }, { "OfflineSessionRollbackCmdUID", nullptr },
                { "Closed", false }
            };
            if (q.contains("IncludeTaxObject") && q["IncludeTaxObject"].is_boolean() && q["IncludeTaxObject"].get<bool>())
                j["TaxObject"] = TaxObjectJson(state_.Registrars());
            return Json(200, j);
        }

        if (cmd == "Shifts") {
            const RegistrarState* r = state_.Find(StrField(q, "NumFiscal"));
            if (!r) return NoContent();
            long long shiftId = 0;
            const bool byId = IntField(q, "ShiftId", shiftId);
            long long from = LLONG_MIN, to = LLONG_MAX;
            if (!byId) {
                const std::string f = StrField(q, "From"), t = StrField(q, "To");
                if (!f.empty() && !ParseDateTime(f, from)) return ErrorResponse(kInvalidQueryParameter, "From: " + f);
                if (!t.empty() && !ParseDateTime(t, to))   return ErrorResponse(kInvalidQueryParameter, "To: " + t);
            }
            json arr = json::array();
            for (const ShiftRecord& s : r->shifts) {
                if (byId ? (s.shiftId != shiftId) : (s.openedEpoch < from || s.openedEpoch > to)) continue;
                arr.push_back(ShiftJson(s));
            }
            if (arr.empty()) return NoContent();
            return Json(200, { { "UID", uid }, { "Shifts", arr } });
        }

        if (cmd == "LastShiftTotals") {
            const RegistrarState* r = state_.Find(StrField(q, "NumFiscal"));
            if (!r || r->shifts.empty()) return NoContent();
            json j = ShiftJson(r->shifts.back());
            j.erase("ShiftId");
            j.erase("OpenShiftFiscalNum");
            j.erase("CloseShiftFiscalNum");
            j["UID"]         = uid;
            j["ShiftState"]  = r->shiftOpen ? 1 : 0;
            j["ZRepPresent"] = r->zRepPresent;
            j["Totals"]      = r->shiftOpen ? TotalsJson(r->totals) : json(nullptr);   // [Опис] 1487
            return Json(200, j);
        }

        if (cmd == "CheckExt" || cmd == "ZRepExt") {
            const bool wantZ = (cmd == "ZRepExt");
            long long type = -1;
            if (!IntField(q, "Type", type) || type < 0 || type > 3)
                return ErrorResponse(kInvalidQueryParameter, "Непідтримуваний Type (0..3)");
            json j = { { "UID", uid }, { "Data", nullptr }, { "ShiftId", nullptr }, { "ResultCode", 0 }, { "ResultText", "OK" } };
            if (!wantZ) j["CabinetUrl"] = "";
            const std::string reg = StrField(q, "RegistrarNumFiscal");
            if (!state_.Find(reg)) { j["ResultCode"] = 4; j["ResultText"] = "ПРРО не зареєстрований"; return Json(200, j); }
            const StoredDoc* d = nullptr;
            const std::string numFiscal = StrField(q, "NumFiscal");
            long long numLocal = 0;
            if (!numFiscal.empty())                 d = state_.FindDocByFiscal(reg, numFiscal);
            else if (IntField(q, "NumLocal", numLocal)) d = state_.FindDoc(reg, numLocal);
            if (d && ((d->klass == DocClass::ZRep) != wantZ)) d = nullptr;   // чек не шукаємо як Z-звіт і навпаки
            if (!d) { j["ResultCode"] = 5; j["ResultText"] = "Документ не зареєстрований на ПРРО"; return Json(200, j); }
            j["ShiftId"] = d->shiftId;
            std::string data;
            if (type == 1) data = d->originalXml;
            if (type == 2) {
                std::string serverXml = d->originalXml;
                if (d->klass == DocClass::Check && !InsertOrderTaxNum(d->originalXml, d->fiscalNum, serverXml))
                    return Text(500, "cannot build server XML");
                data = oracle::Sign(serverXml);
                if (data.empty()) return Text(500, "cannot sign document");
            }
            if (type == 3) data = Visualization(*d);
            j["Data"] = oracle::b64encode(data);
            return Json(200, j);
        }

        return ErrorResponse(kInvalidQueryParameter, "Невідома команда: " + cmd);
    } catch (const std::exception& e) {
        return ErrorResponse(kInvalidQueryParameter, std::string("Некоректний запит: ") + e.what());
    }
}

minihttp::Response PrroFsService::HandleControl(const std::string& body) {
    try {
        const json q = json::parse(StripJsonPrefix(body), nullptr, false);
        if (q.is_discarded() || !q.is_object()) return Json(400, { { "ok", false }, { "error", "тіло не є JSON-об'єктом" } });
        const std::string action = StrField(q, "action");
        if (action == "reset") {
            if (!q.contains("registrars") || !q["registrars"].is_array())
                return Json(400, { { "ok", false }, { "error", "registrars — обов'язковий масив" } });
            std::vector<RegistrarSeed> seeds;
            for (const json& e : q["registrars"]) {
                if (!e.is_object()) return Json(400, { { "ok", false }, { "error", "елемент registrars — об'єкт" } });
                RegistrarSeed s;
                s.numFiscal = StrField(e, "numFiscal");
                if (s.numFiscal.empty() || s.numFiscal.size() > 18
                    || s.numFiscal.find_first_not_of("0123456789") != std::string::npos)
                    return Json(400, { { "ok", false }, { "error", "numFiscal — лише цифри, до 18" } });
                long long n = 1;
                if (e.contains("nextLocalNum") && (!IntField(e, "nextLocalNum", n) || n < 1))
                    return Json(400, { { "ok", false }, { "error", "nextLocalNum — ціле >= 1" } });
                s.nextLocalNum = n;
                seeds.push_back(s);
            }
            state_.Reset(seeds);
            faults_.Clear();
            return Json(200, { { "ok", true } });
        }
        return Json(400, { { "ok", false }, { "error", "невідома дія: " + action } });
    } catch (const std::exception& e) {
        return Json(400, { { "ok", false }, { "error", e.what() } });
    }
}

minihttp::Response PrroFsService::StateJson() {
    json regs = json::array();
    for (const RegistrarState* r : state_.Registrars())
        regs.push_back({ { "numFiscal", r->numFiscal }, { "shiftState", r->shiftOpen ? 1 : 0 },
                         { "shiftId", r->shiftId }, { "nextLocalNum", r->nextLocalNum },
                         { "firstLocalNum", r->firstLocalNum }, { "lastFiscalNum", NullIfEmpty(r->lastFiscalNum) },
                         { "zRepPresent", r->zRepPresent }, { "openShiftFiscalNum", NullIfEmpty(r->openShiftFiscalNum) },
                         { "shiftsCount", r->shifts.size() } });
    json docs = json::array();
    for (const StoredDoc& d : state_.Docs())
        docs.push_back({ { "registrar", d.registrar }, { "localNum", d.localNum }, { "fiscalNum", d.fiscalNum },
                         { "class", d.klass == DocClass::Check ? "CHECK" : "ZREP" }, { "docType", d.docType },
                         { "docSubType", d.docSubType }, { "shiftId", d.shiftId } });
    json faults = json::array();
    for (const auto& a : faults_.Armed())
        faults.push_back({ { "target", a.first }, { "mode", FaultModeName(a.second.mode) },
                           { "seconds", a.second.seconds }, { "code", a.second.code },
                           { "holdSeconds", a.second.holdSeconds } });
    return Json(200, { { "registrars", regs }, { "documents", docs }, { "faults", faults },
                       { "dateSkewSeconds", faults_.dateSkewSeconds },
                       { "rejectFormat", faults_.rejectFormat == RejectFormat::Ticket ? "ticket" : "text" } });
}

}  // namespace prrofs
```

- [ ] **Step 6: Зібрати й прогнати — ЗЕЛЕНИЙ**

Run: `cmake --build build_x64 --config Release --target prro_fs_emulator && ./bin/Release/prro_fs_emulator_x64.exe --self-test; echo exit=$?`
Expected: `=== ALL PASS ===`, `exit=0`.

- [ ] **Step 7: Негативна верифікація** — відкотити кожну:
  - `CommonHeaders`: повернути `{}` → червоніють `/ping: заголовок Date`, `невідомий шлях -> 404 з Date`, `відмова теж має Date`.
  - `HandleCmd`: прибрати перевірку `kSigned.count(cmd) && !isSigned` → червоніє `Objects без підпису -> 400, код 9`.
  - `LastShiftTotals`: віддавати `Totals: null` при відкритій зміні без чеків (умова `r->shiftOpen && r->totals.real.ordersCount > 0`) → червоніє `Totals без чеків: Real — об'єкт…`.
  - `StripJsonPrefix`: повертати `s` без змін → червоніє `ServerState без підпису, з BOM і пробілами -> 200`.
  - `CheckExt` тип 2: підписувати `d->originalXml` без `InsertOrderTaxNum` → червоніє `у XML сервера вставлено ORDERTAXNUM`.

- [ ] **Step 8: Рівень L-f2 у `run_tests.ps1`**

1. Після рядка `$PrroFsStateExe = …` (Task 1) додати:

```powershell
$PrroFsEmulatorExe = Join-Path $BinRelease ("prro_fs_emulator" + $ArchSuffix + ".exe")
```

2. Перед блоком

```powershell
# =====================================================================
# ЕТАП 3 (L2/L3): native_host кейси 1..4 (+5 за наявності ПРРО-еталонів)
# =====================================================================
```

вставити:

```powershell
# =====================================================================
# ЕТАП L-f2: prro_fs_emulator --self-test — протокол імітації фіскального сервера ДПС:
# сценарії споживача (зміна, обриви до/після реєстрації, збої, формати відповідей) без 1С.
# Потребує UAPKI (підпис квитанцій і перевірка CMS): без -WithUAPKI — SKIP, не FAIL.
# Спека: docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md §10.2.
# =====================================================================
Section 'ЕТАП L-f2: prro_fs_emulator --self-test (протокол ДПС)'

if ($NoUapki) {
    Add-Result 'L-f2' 'prro_fs_emulator' 'SKIP' 'режим -NoUapki: крипто-стек UAPKI не збирається'
}
elseif (-not (Test-Path $PrroFsEmulatorExe)) {
    Add-Result 'L-f2' 'prro_fs_emulator' 'FAIL' `
        "немає prro_fs_emulator.exe: $PrroFsEmulatorExe — зберіть build_project.ps1 -WithUAPKI -WithTests"
}
else {
    $outF = Join-Path ([System.IO.Path]::GetTempPath()) ("prro_fs_emu_" + [guid]::NewGuid().ToString('N').Substring(0,6) + '.out')
    $p = Start-Process -FilePath $PrroFsEmulatorExe -ArgumentList @('--self-test') `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
    $txt = ''
    if (Test-Path $outF) { $txt = Get-Content -Raw $outF }
    if ($p.ExitCode -eq 0) {
        $nPassLines = ([regex]::Matches($txt, '\[PASS\]')).Count
        Add-Result 'L-f2' 'prro_fs_emulator' 'PASS' "усі CHECK пройшли (PASS: $nPassLines)"
    }
    else {
        $fails = ($txt -split "`n" | Where-Object { $_ -match '\[FAIL\]' }) -join ' | '
        Add-Result 'L-f2' 'prro_fs_emulator' 'FAIL' "exit=$($p.ExitCode) $fails"
    }
    Remove-Item $outF, "$outF.err" -ErrorAction SilentlyContinue
}

```

Run: `head -c 3 run_tests.ps1 | xxd -p` → `efbbbf`.

- [ ] **Step 9: Commit**

```bash
git add tests/support/PrroFsService.h tests/support/PrroFsService.cpp tests/prro_fs_emulator.cpp tests/prro_fs_emulator_selftest.cpp tests/CMakeLists.txt run_tests.ps1
git commit --only -m "feat(tests): prro_fs_emulator — маршрути ДПС /fs/doc і /fs/cmd зі станом; рівень L-f2" -m "<пари негативної верифікації зі Step 7>" -- tests/support/PrroFsService.h tests/support/PrroFsService.cpp tests/prro_fs_emulator.cpp tests/prro_fs_emulator_selftest.cpp tests/CMakeLists.txt run_tests.ps1
```

---

### Task 6: Керування збоями — `fault`, `set`, обриви, утримання, зсув годинника

**Files:**
- Modify: `tests/support/PrroFsService.cpp` (`Handle`, `HandleControl` — повна заміна; нові `CommandNameOf`, `FaultStatus`, `FaultDrop`)
- Modify: `tests/prro_fs_emulator_selftest.cpp` (сценарії `ScenarioDrops`, `ScenarioStatus`, `ScenarioSkewAndReject`)

**Interfaces:**
- Consumes: усе з Task 5; `FaultPlan::Arm/Take/Clear`, `ParseFaultMode`, `FaultModeName` (Task 4); `Disposition` (Task 1).
- Produces: протокол керування `POST /control` `{"action":"fault","target":…,"mode":…,"seconds"|"code"|"body"|"location"|"holdSeconds":…}` → `200`/`400`/`409`; `{"action":"set","dateSkewSeconds":n,"rejectFormat":"text"|"ticket"}` → `200`/`400`.

**Покриття критерію §15.5 («усі дев'ять сценаріїв споживача на рівні протоколу»):** сценарії 1, 5 (Task 5), 2, 3, 7, 8 (цей Task) мають власну поведінку сервера й відтворюються самотестом. Сценарії 4 (порядок зняття наміру відносно оповіщення БПО), 6 (скасування до відправки не спалює номер) і 9 (попередження про нештатну адресу) — **поведінка продукту без власної поведінки сервера**: на рівні протоколу їм досить штатних маршрутів сценарію 1, тож окремих сценаріїв самотесту для них немає — вони перевіряються в `СМП_ТестыСквозныеПРРО` споживача.

- [ ] **Step 1: Сценарії збоїв (червоний)** — у `tests/prro_fs_emulator_selftest.cpp` перед `}  // namespace` додати:

```cpp
// Парсинг RFC 1123 (для перевірки зсуву Date). -1 — не розібрано.
long long ParseHttpDate(const std::string& s) {
    static const char* kM = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = { 0 };
    int d = 0, y = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%*3s, %2d %3s %4d %2d:%2d:%2d GMT", &d, mon, &y, &h, &mi, &se) != 6) return -1;
    const char* p = std::strstr(kM, mon);
    if (!p) return -1;
    std::tm t{};
    t.tm_year = y - 1900; t.tm_mon = static_cast<int>(p - kM) / 3; t.tm_mday = d;
    t.tm_hour = h;        t.tm_min = mi;                            t.tm_sec = se;
    return static_cast<long long>(_mkgmtime(&t));
}

// Сценарії споживача 2 і 3: обриви до й після реєстрації (+ «мовчати довше за таймаут»).
void ScenarioDrops(int port) {
    std::printf("== Сценарії 2-3: обриви ==\n");
    CHECK(Reset(port), "reset для обривів");
    std::string t;
    CHECK(TicketOk(PostDoc(port, "open_shift_1251.xml", 1), t), "зміну відкрито");

    // --- Обрив ПІСЛЯ реєстрації ---
    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "dropAfterRegister" } }).code == 200,
          "збій dropAfterRegister взведено");
    const minihttp::ClientResult a = PostDoc(port, "check_sale_1251.xml", 2);
    CHECK(a.connected && !a.responded && a.reset, "чек №2: з'єднання розірвано без відповіді (НетОтвета)");
    // Правило 2: стан доводиться парою предикатів — номер зайнято І документ збережено.
    CHECK(RegState(port)["nextLocalNum"] == 3 && HasDoc(port, 2),
          "стан: чек №2 ЗАРЕЄСТРОВАНО (NextLocalNum = 3, документ №2 є)");
    const minihttp::ClientResult again = PostDoc(port, "check_sale_1251.xml", 2);
    CHECK(again.code == 400 && LastNumber(again.body) == 3, "повтор №2 -> «повинен дорівнювати 3»");
    const minihttp::ClientResult ce = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                      { "NumLocal", 2 }, { "Type", 2 }, { "UID", "d1" } }, false);
    CHECK(ce.code == 200 && Body(ce)["ResultCode"] == 0 && !Data(Body(ce)).empty(),
          "CheckExt №2 після обриву: знайдено (ResultCode 0, Data є)");
    CHECK(TicketOk(PostDoc(port, "check_sale_1251.xml", 3), t), "наступний чек іде з №3");

    // --- Обрив ДО реєстрації ---
    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "dropBeforeRegister" } }).code == 200,
          "збій dropBeforeRegister взведено");
    const minihttp::ClientResult b = PostDoc(port, "check_sale_1251.xml", 4);
    CHECK(b.connected && !b.responded && b.reset, "чек №4: з'єднання розірвано без відповіді");
    CHECK(RegState(port)["nextLocalNum"] == 4 && !HasDoc(port, 4),
          "стан: чек №4 НЕ зареєстровано (NextLocalNum = 4, документа №4 немає)");
    const minihttp::ClientResult ce2 = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                       { "NumLocal", 4 }, { "Type", 2 }, { "UID", "d2" } }, false);
    CHECK(ce2.code == 200 && Body(ce2)["ResultCode"] == 5, "CheckExt №4: DocumentAbsent");
    CHECK(TicketOk(PostDoc(port, "check_sale_1251.xml", 4), t), "повторна відправка №4 з тим самим номером проходить");

    // --- «Мовчати довше за таймаут» (правило 3: клієнт 1 с, утримання 2 с) ---
    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "dropAfterRegister" },
                          { "holdSeconds", 2 } }).code == 200, "утримання після реєстрації взведено");
    const minihttp::ClientResult c = PostDoc(port, "check_sale_1251.xml", 5, 1000);
    CHECK(!c.responded && c.timedOut, "утримання: клієнт із таймаутом 1 с не отримав нічого");
    // Review Focus 1: утримання не тримає замок стану — інші запити обслуговуються.
    const minihttp::ClientResult ss = PostCmd(port, { { "Command", "ServerState" }, { "UID", "h" } }, false, 1000);
    CHECK(ss.code == 200, "під час утримання /fs/cmd відповідає (утримання не блокує інші запити)");
    CHECK(RegState(port)["nextLocalNum"] == 6 && HasDoc(port, 5), "утримання після реєстрації: №5 зареєстровано");

    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "dropBeforeRegister" },
                          { "holdSeconds", 2 } }).code == 200, "утримання до реєстрації взведено");
    const minihttp::ClientResult e = PostDoc(port, "check_sale_1251.xml", 6, 1000);
    CHECK(!e.responded && e.timedOut, "утримання до реєстрації: клієнт не отримав нічого");
    CHECK(RegState(port)["nextLocalNum"] == 6 && !HasDoc(port, 6), "утримання до реєстрації: №6 НЕ зареєстровано");
}

// Сценарій 8 споживача + механіка збоїв: 204/5xx/302, delay, 409, фільтр цілі.
void ScenarioStatus(int port) {
    std::printf("== Сценарій 8: збої-статуси й фільтр цілі ==\n");
    CHECK(Reset(port), "reset для збоїв-статусів");
    for (int code : { 204, 503 }) {
        CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "status" }, { "code", code } }).code == 200,
              "збій status взведено");
        const minihttp::ClientResult r = PostDoc(port, "open_shift_1251.xml", 1);
        CHECK(r.responded && r.code == code, code == 204 ? "status 204 віддано" : "status 503 віддано");
        CHECK(RegState(port)["nextLocalNum"] == 1, "status-збій: документ не оброблено (NextLocalNum = 1)");
    }
    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "status" }, { "code", 302 } }).code == 200,
          "збій 302 взведено");
    const minihttp::ClientResult r302 = PostDoc(port, "open_shift_1251.xml", 1);
    CHECK(r302.code == 302 && r302.headers.count("location") == 1
          && r302.headers["location"] == "http://127.0.0.1:" + std::to_string(port) + "/moved",
          "302 з Location за замовчуванням");

    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "delay" }, { "seconds", 1 } }).code == 200,
          "збій delay 1 с взведено");
    std::string t;
    const minihttp::ClientResult d = PostDoc(port, "open_shift_1251.xml", 1, 5000);
    std::printf("  виміряно: відповідь із delay 1 с — %lld мс\n", d.elapsedMs);
    CHECK(d.elapsedMs >= 900 && TicketOk(d, t), "delay: відповідь після паузи, документ прийнято");

    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "delay" }, { "seconds", 1 } }).code == 200,
          "delay знову взведено");
    CHECK(Control(port, { { "action", "fault" }, { "target", "doc" }, { "mode", "status" }, { "code", 500 } }).code == 409,
          "друге взведення тієї самої цілі -> 409");
    CHECK(Control(port, { { "action", "fault" }, { "target", "nope" }, { "mode", "delay" }, { "seconds", 1 } }).code == 400,
          "некоректна ціль -> 400");
    CHECK(Reset(port) && State(port)["faults"].is_array() && State(port)["faults"].empty(), "reset очищає взведені збої");

    CHECK(Control(port, { { "action", "fault" }, { "target", "cmd:CheckExt" }, { "mode", "status" }, { "code", 503 } }).code == 200,
          "збій cmd:CheckExt взведено");
    const minihttp::ClientResult s1 = PostCmd(port, { { "Command", "ServerState" }, { "UID", "f1" } }, false);
    CHECK(s1.code == 200, "інша команда збій cmd:CheckExt не витрачає");
    const minihttp::ClientResult s2 = PostCmd(port, { { "Command", "CheckExt" }, { "RegistrarNumFiscal", kReg },
                                                      { "NumLocal", 1 }, { "Type", 0 }, { "UID", "f2" } }, false);
    CHECK(s2.code == 503 && State(port)["faults"].is_array() && State(port)["faults"].empty(),
          "CheckExt отримав 503, збій витрачено");
}

// Сценарій 7 споживача (зсув Date) + альтернативний формат відмови.
void ScenarioSkewAndReject(int port) {
    std::printf("== Сценарій 7: зсув годинника; формат відмови ==\n");
    CHECK(Reset(port), "reset для зсуву годинника");
    CHECK(Control(port, { { "action", "set" }, { "dateSkewSeconds", 40 } }).code == 200, "dateSkewSeconds = 40");
    const minihttp::ClientResult p = minihttp::Request(port, "GET", "/ping", "", "", 3000);
    const long long skewDate = ParseHttpDate(p.headers.count("date") ? p.headers["date"] : std::string())
                             - static_cast<long long>(std::time(nullptr));
    std::printf("  виміряно: зсув Date = %lld с\n", skewDate);
    CHECK(skewDate >= 35 && skewDate <= 45, "Date зсунуто на ~40 с");
    long long ts = 0;
    const minihttp::ClientResult s = PostCmd(port, { { "Command", "ServerState" }, { "UID", "k" } }, false);
    json js = Body(s);
    const bool tsOk = js.contains("Timestamp") && js["Timestamp"].is_string()
                      && prrofs::ParseDateTime(js["Timestamp"].get<std::string>(), ts);
    const long long skewTs = ts - static_cast<long long>(std::time(nullptr));
    CHECK(tsOk && skewTs >= 35 && skewTs <= 45, "Timestamp ServerState теж зсунуто (годинник сервера один)");
    CHECK(Control(port, { { "action", "set" }, { "dateSkewSeconds", 0 } }).code == 200, "зсув знято");

    CHECK(Control(port, { { "action", "set" }, { "rejectFormat", "ticket" } }).code == 200, "rejectFormat = ticket");
    const minihttp::ClientResult r = PostDoc(port, "open_shift_1251.xml", 5);
    const oracle::VerifyOutcome v = oracle::Verify(r.body);
    std::string xml;
    CHECK(r.code == 200 && v.accepted && oracle::b64decode(v.contentB64, xml)
          && xml.find("<ERRORCODE>7</ERRORCODE>") != std::string::npos
          && xml.find("<ORDERTAXNUM>") == std::string::npos,
          "rejectFormat ticket: 200 + підписана квитанція з ERRORCODE 7 без ORDERTAXNUM");
    CHECK(Control(port, { { "action", "set" }, { "rejectFormat", "bogus" } }).code == 400, "невідомий rejectFormat -> 400");
    CHECK(Reset(port) && State(port)["rejectFormat"] == "text" && State(port)["dateSkewSeconds"] == 0,
          "reset повертає постійні налаштування до типових");
}
```

У `RunPrroFsSelfTest()` після `ScenarioFormats(port);` додати:

```cpp
        ScenarioDrops(port);
        ScenarioStatus(port);
        ScenarioSkewAndReject(port);
```

Run: `cmake --build build_x64 --config Release --target prro_fs_emulator && ./bin/Release/prro_fs_emulator_x64.exe --self-test; echo exit=$?`
Expected: FAIL — `[FAIL] збій dropAfterRegister взведено` (дія `fault` ще повертає 400) і подальші.

- [ ] **Step 2: `tests/support/PrroFsService.cpp` — `Handle` (повна заміна)**

```cpp
minihttp::Response PrroFsService::Handle(const minihttp::Request& req) {
    if (trace_) { std::printf("← %s %s (%zu B)\n", req.method.c_str(), req.uri.c_str(), req.body.size()); std::fflush(stdout); }
    std::string path = req.uri;
    const size_t qm = path.find('?');
    if (qm != std::string::npos) path.resize(qm);

    if (req.method == "GET" && path == "/ping") return Text(200, "prro_fs_emulator alive");
    if (req.method == "POST" && path == "/control") { std::lock_guard<std::mutex> lk(mx_); return HandleControl(req.body); }
    if (req.method == "GET" && path == "/control/state") { std::lock_guard<std::mutex> lk(mx_); return StateJson(); }

    const bool isDoc = (req.method == "POST" && path == "/fs/doc");
    const bool isCmd = (req.method == "POST" && path == "/fs/cmd");
    if (!isDoc && !isCmd) return Text(404, "unknown endpoint");

    // Збій береться під замком; пауза й утримання — БЕЗ замка (Review Focus 1).
    Fault f;
    bool faulted = false;
    {
        std::lock_guard<std::mutex> lk(mx_);
        faulted = faults_.Take(isDoc ? "doc" : "cmd", [this, &req]() { return CommandNameOf(req.body); }, f);
    }
    if (faulted) {
        if (trace_) std::printf("  збій: %s\n", FaultModeName(f.mode));
        if (f.mode == FaultMode::Delay)              std::this_thread::sleep_for(std::chrono::seconds(f.seconds));
        if (f.mode == FaultMode::Status)             return FaultStatus(f);
        if (f.mode == FaultMode::DropBeforeRegister) return FaultDrop(f);
    }

    minihttp::Response r;
    {
        std::lock_guard<std::mutex> lk(mx_);
        r = isDoc ? HandleDoc(req.body) : HandleCmd(req.body);
    }
    if (faulted && f.mode == FaultMode::DropAfterRegister) return FaultDrop(f);   // стан змінено, відповідь не йде
    return r;
}
```

- [ ] **Step 3: `CommandNameOf`, `FaultStatus`, `FaultDrop`** — додати після `Handle`:

```cpp
std::string PrroFsService::CommandNameOf(const std::string& body) {
    std::string text = StripJsonPrefix(body);
    if (text.empty() || text[0] != '{') {
        const oracle::VerifyOutcome v = oracle::Verify(body);
        std::string content;
        if (!v.accepted || !oracle::b64decode(v.contentB64, content)) return std::string();
        text = StripJsonPrefix(content);
    }
    const json q = json::parse(text, nullptr, false);
    return q.is_object() ? StrField(q, "Command") : std::string();
}

minihttp::Response PrroFsService::FaultStatus(const Fault& f) {
    minihttp::Response r;
    r.code = f.code;
    r.body = (f.code == 204) ? std::string() : f.body;
    if (f.code == 302)
        r.headers.push_back({ "Location", f.location.empty()
                                          ? "http://127.0.0.1:" + std::to_string(port_) + "/moved"
                                          : f.location });
    return r;
}

minihttp::Response PrroFsService::FaultDrop(const Fault& f) {
    minihttp::Response r;
    r.disposition = (f.holdSeconds > 0) ? minihttp::Disposition::HoldThenAbort : minihttp::Disposition::Abort;
    r.holdSeconds = f.holdSeconds;
    return r;
}
```

- [ ] **Step 4: `HandleControl` (повна заміна)**

```cpp
minihttp::Response PrroFsService::HandleControl(const std::string& body) {
    try {
        const json q = json::parse(StripJsonPrefix(body), nullptr, false);
        if (q.is_discarded() || !q.is_object()) return Json(400, { { "ok", false }, { "error", "тіло не є JSON-об'єктом" } });
        const std::string action = StrField(q, "action");

        if (action == "reset") {
            if (!q.contains("registrars") || !q["registrars"].is_array())
                return Json(400, { { "ok", false }, { "error", "registrars — обов'язковий масив" } });
            std::vector<RegistrarSeed> seeds;
            for (const json& e : q["registrars"]) {
                if (!e.is_object()) return Json(400, { { "ok", false }, { "error", "елемент registrars — об'єкт" } });
                RegistrarSeed s;
                s.numFiscal = StrField(e, "numFiscal");
                if (s.numFiscal.empty() || s.numFiscal.size() > 18
                    || s.numFiscal.find_first_not_of("0123456789") != std::string::npos)
                    return Json(400, { { "ok", false }, { "error", "numFiscal — лише цифри, до 18" } });
                long long n = 1;
                if (e.contains("nextLocalNum") && (!IntField(e, "nextLocalNum", n) || n < 1))
                    return Json(400, { { "ok", false }, { "error", "nextLocalNum — ціле >= 1" } });
                s.nextLocalNum = n;
                seeds.push_back(s);
            }
            state_.Reset(seeds);
            faults_.Clear();
            return Json(200, { { "ok", true } });
        }

        if (action == "fault") {
            Fault f;
            if (!ParseFaultMode(StrField(q, "mode"), f.mode))
                return Json(400, { { "ok", false }, { "error", "mode: delay | status | dropBeforeRegister | dropAfterRegister" } });
            long long v = 0;
            if (f.mode == FaultMode::Delay) {
                if (!IntField(q, "seconds", v) || v < 0 || v > 600)
                    return Json(400, { { "ok", false }, { "error", "seconds — 0..600" } });
                f.seconds = static_cast<int>(v);
            }
            if (f.mode == FaultMode::Status) {
                if (!IntField(q, "code", v) || v < 100 || v > 599)
                    return Json(400, { { "ok", false }, { "error", "code — 100..599" } });
                f.code     = static_cast<int>(v);
                f.body     = StrField(q, "body");
                f.location = StrField(q, "location");
            }
            if ((f.mode == FaultMode::DropBeforeRegister || f.mode == FaultMode::DropAfterRegister)
                && q.contains("holdSeconds")) {
                if (!IntField(q, "holdSeconds", v) || v < 0 || v > 600)
                    return Json(400, { { "ok", false }, { "error", "holdSeconds — 0..600" } });
                f.holdSeconds = static_cast<int>(v);
            }
            switch (faults_.Arm(StrField(q, "target"), f)) {
                case FaultPlan::ArmResult::Ok:       return Json(200, { { "ok", true } });
                case FaultPlan::ArmResult::Conflict: return Json(409, { { "ok", false }, { "error", "для цієї цілі вже взведено збій" } });
                default:                             return Json(400, { { "ok", false }, { "error", "target: doc | cmd | cmd:<Команда>" } });
            }
        }

        if (action == "set") {
            bool any = false;
            long long v = 0;
            if (q.contains("dateSkewSeconds")) {
                if (!IntField(q, "dateSkewSeconds", v)) return Json(400, { { "ok", false }, { "error", "dateSkewSeconds — ціле" } });
                faults_.dateSkewSeconds = static_cast<int>(v);
                any = true;
            }
            if (q.contains("rejectFormat")) {
                const std::string rf = StrField(q, "rejectFormat");
                if (rf == "text")        faults_.rejectFormat = RejectFormat::Text;
                else if (rf == "ticket") faults_.rejectFormat = RejectFormat::Ticket;
                else return Json(400, { { "ok", false }, { "error", "rejectFormat: text | ticket" } });
                any = true;
            }
            if (!any) return Json(400, { { "ok", false }, { "error", "set: dateSkewSeconds і/або rejectFormat" } });
            return Json(200, { { "ok", true } });
        }

        return Json(400, { { "ok", false }, { "error", "невідома дія: " + action } });
    } catch (const std::exception& e) {
        return Json(400, { { "ok", false }, { "error", e.what() } });
    }
}
```

- [ ] **Step 5: Зібрати й прогнати — ЗЕЛЕНИЙ**

Run: `cmake --build build_x64 --config Release --target prro_fs_emulator && ./bin/Release/prro_fs_emulator_x64.exe --self-test; echo exit=$?`
Expected: `=== ALL PASS ===`, `exit=0`; записати виміряні `delay` мс і `зсув Date` с.

- [ ] **Step 6: Негативна верифікація** — відкотити кожну:
  - `Handle`: для `DropAfterRegister` повертати `FaultDrop(f)` **до** `HandleDoc` (як DropBefore) → червоніє `стан: чек №2 ЗАРЕЄСТРОВАНО…` (саме це правило 2 і ловить: ззовні обидва обриви однакові).
  - `Handle`: у гілці `DropAfterRegister` з `holdSeconds > 0` тимчасово взяти `std::lock_guard` на `mx_` і `std::this_thread::sleep_for(std::chrono::seconds(f.holdSeconds))` перед `return FaultDrop(f);` (утримання під замком стану) → червоніє `під час утримання /fs/cmd відповідає` (Review Focus 1).
  - `CommonHeaders`: ігнорувати `dateSkewSeconds` (передавати `0`) → червоніє `Date зсунуто на ~40 с`.
  - `FaultPlan::Take` (Task 4) лишити як є, а в `Handle` передавати `nullptr` замість лямбди `CommandNameOf` → червоніє `CheckExt отримав 503, збій витрачено`.

- [ ] **Step 7: Commit**

```bash
git add tests/support/PrroFsService.cpp tests/prro_fs_emulator_selftest.cpp
git commit --only -m "feat(tests): prro_fs_emulator — керовані збої, два штатні обриви, зсув годинника" -m "<пари негативної верифікації зі Step 6 + виміри delay і зсуву Date>" -- tests/support/PrroFsService.cpp tests/prro_fs_emulator_selftest.cpp
```

---

### Task 7: Документація, повний гейт x64/x86, публікація

**Files:**
- Modify: `docs/integration-1c/uapki.md` (новий §9.2 перед `## 10. Типові помилки й рішення`)
- Modify: `AGENTS.md`
- Modify: `version.h` (перегенерує `build_project.ps1`)

**Interfaces:**
- Consumes: усе попереднє.
- Produces: документований інструмент; зелений гейт x64 і x86.

- [ ] **Step 1: §9.2 у `docs/integration-1c/uapki.md`** — вставити перед рядком `---`, що передує `## 10. Типові помилки й рішення` (тобто одразу після абзацу «Регіон `UAPKI_HTTP` — раунд-тріп…»):

```markdown
### 9.2. `prro_fs_emulator` — імітація фіскального сервера ДПС зі станом

Окремий від `uapki_fiscal_emulator` інструмент для **наскрізних тестів ПРРО** (розширення
`SMP_SimplyConnect_SMBru` і його тестове розширення): грає сервер ДПС **зі станом** — зміни,
локальна й фіскальна нумерація, збережені документи, підсумки для Z-звіту — у форматі відповідей
ДПС і з **керованими збоями**. Дизайн — `docs/superpowers/specs/2026-09-23-prro-fs-emulator-design.md`.
Позначки джерел: **[Опис]** — «Опис АРІ фіскального сервера контролюючого органу (ЄВПЕЗ)»
(репозиторій `github.com/VSydorenko/prro_docs`); **[споживач]** — рішення або вимір команди
`SMP_SimplyConnect`, у цьому репо неперевірюване; **[припущення]** — Опис не задає, обрано свідомо.

**Запуск:** `bin/Release/prro_fs_emulator_x64.exe [port] [--key <p12>] [--pass <pwd>] [--providers <dir>] [--data <dir>] [--self-test]`.
Порт `8099` (зайнятий і не заданий явно — наступний вільний до `+10`), лише `127.0.0.1`, лише `http`.
Базова адреса для параметра обладнання — `http://127.0.0.1:8099/fs`. Детектор «оракул запущено» —
`GET /ping` **від кореня** з тілом рівно `prro_fs_emulator alive` (на тому ж порту може відповідати й
`uapki_fiscal_emulator` — перевіряйте тіло). `--self-test` — сценарії протоколу in-process (рівень
гейта L-f2).

**Ендпоінти ДПС:**
- `POST /fs/doc` — тіло CMS (STRUCT-перевірка, критерій — §4.5), документ `CHECK`/`ZREP`; прийнято →
  `200` + CMS-квитанція (`windows-1251`, `UID`, `ORDERDATE`, `ORDERTIME`, `ORDERNUM`, `ORDERTAXNUM`,
  `ERRORCODE 0`), підписана тестовим `test-diia.p12`.
- `POST /fs/cmd` — `ServerState`, `Objects`, `TransactionsRegistrarState`, `Shifts`, `LastShiftTotals`,
  `CheckExt`, `ZRepExt` (типи `0..3`). Відповідь — звичайний JSON без підпису; у `CheckExt`/`ZRepExt`
  тип 2 поле `Data` — base64 CMS (XML сервера із вставленим `ORDERTAXNUM`), тип 1 — побайтовий
  оригінал, тип 3 — текст UTF-8. `Objects`, `TransactionsRegistrarState`, `Shifts`, `LastShiftTotals`,
  `ZRepExt` без підпису відхиляються (**[Опис]** вимагає підпис; код — **[припущення]**).
- Відмова → `400` + `Код помилки: <n> <СимвольнийКод>\r\n<опис>` **[Опис]**; для коду 7 опис містить
  «Номер документа повинен дорівнювати N», N — останнє число **[споживач]**; розмір тіла поза
  `10…512000` → `416` **[споживач]**; `Date` за Гринвічем на кожній відповіді **[споживач]**; `204` —
  де Опис каже «у разі відсутності даних».

**Керування** (від кореня, поза `/fs`): `POST /control` з `{"action":"reset","registrars":[{"numFiscal":"…","nextLocalNum":1}]}`
(ПРРО реєструються лише так; невідомий → код `1`), `{"action":"fault","target":"doc"|"cmd"|"cmd:<Команда>","mode":"delay"|"status"|"dropBeforeRegister"|"dropAfterRegister",…}`
(одноразовий збій на наступний запит своєї цілі; повторне взведення тієї самої цілі → `409`),
`{"action":"set","dateSkewSeconds":N,"rejectFormat":"text"|"ticket"}` (до наступного `reset`);
`GET /control/state` — стан ПРРО, документи, взведені збої. **Обриви:** `dropAfterRegister` —
документ повністю зареєстровано (номер зайнято, фіскальний номер видано, `CheckExt` його знайде),
з'єднання розірвано RST без відповіді; `dropBeforeRegister` — стан не змінено, розрив;
`holdSeconds` — «мовчати» заданий час, потім розрив. Ззовні обидва обриви однакові — розрізняє
їх лише `GET /control/state`.

> **Межі імітації — не калібрувати на ній обробку помилок.** STRUCT не перевіряє ланцюга, строку
> дії сертифіката, OCSP/CRL: `TOTAL-VALID` = «підпис і вміст не підмінено», а не «ДПС прийме»;
> чинність сертифіката міряє арбітр ІІТ (`docs/architecture/uapki.md` §8.4). Не збігаються з ДПС
> або не виміряні: коди для непідписаної команди й зіпсованого підпису, `404` замість `403` на
> невідомий шлях, порядок перевірок `/fs/doc` (номер раніше за стан зміни — **[припущення]**),
> формат відмови (Опис допускає і текст, і квитанцію — звідси `rejectFormat`), стала частина тексту
> коду 7. Квитанції й `Data` тип 2 підписані **простроченим** тестовим ключем — перевірка в 1С лише
> STRUCT з `ignoreCertStatus`. Поза обсягом: `/fs/pck` і офлайн, `?resultAsJson=true`, `415`,
> команди `Check`/`ZRep` без `Ext`, сторно й `DOCTYPE 1..3` у підсумках.
```

- [ ] **Step 2: `AGENTS.md`**

1. Абзац про склад `tests/`: після `контракт провайдера НКІ напряму через \`LoadLibraryW\`), ` вставити
   `` `tests/prro_fs_state_selftest.cpp` (харнес L-f1 — стан/нумерація/підсумки/збої імітації ДПС і диспозиції `MiniHttpServer`), `tests/prro_fs_emulator.cpp` + `tests/prro_fs_emulator_selftest.cpp` + `tests/support/{PrroFsService,FiscalServerState,FaultPlan,UapkiOracle,MiniHttpClient}.{h,cpp}` (імітація фіскального сервера ДПС зі станом, рівень L-f2; опис — `docs/integration-1c/uapki.md` §9.2), ``.
2. Таблиця цілей: після рядка `| \`uapki_fiscal_emulator.exe\` | …` додати:

```markdown
| `prro_fs_state_selftest.exe` | L-f1 | ні | чиста логіка імітації фіскального сервера ДПС (стан, нумерація, підсумки, збої) + диспозиції `MiniHttpServer` (обрив, утримання, `Date`) |
| `prro_fs_emulator.exe` | L-f2 (`--self-test`) + ручний | **так** | імітація фіскального сервера ДПС зі станом і керованими збоями для наскрізних тестів ПРРО; опис — `docs/integration-1c/uapki.md` §9.2 |
```

3. Речення «Цілі без UAPKI (`core`/`wire`/`ecr_*`, …» — після `так само як \`provider_contract_selftest\`` додати ` і \`prro_fs_state_selftest\``; перелік `` `uapki_selftest`/`uapki_fiscal_emulator`/`native_host` `` замінити на `` `uapki_selftest`/`uapki_fiscal_emulator`/`prro_fs_emulator`/`native_host` ``.
4. Ланцюжок рівнів: `L-p3 label_native_host компоненти LabelPrinter через DLL → L1 selftest по` → `L-p3 label_native_host компоненти LabelPrinter через DLL → L-f1 prro_fs_state_selftest імітації ДПС → L1 selftest по`; `→ L1.5 provider_contract_selftest → L2/L3 native_host` → `→ L1.5 provider_contract_selftest → L-f2 prro_fs_emulator --self-test → L2/L3 native_host`; `L0.5, L0.6, L0.7, L2-ecr, L-p1 і L-p3` → `L0.5, L0.6, L0.7, L2-ecr, L-p1, L-p3 і L-f1`.
5. Абзац `-NoUapki`: `+ L-p3 label_native_host)` → `+ L-p3 label_native_host + L-f1 prro_fs_state_selftest)`; `native_host і L4-iit → SKIP (не FAIL)` → `native_host, L-f2 і L4-iit → SKIP (не FAIL)`.
6. Блок структури `tests/`: рядок `#   uapki_fiscal_emulator (— ручний, HTTP-оракул ЕЦП для тесту UAPKI з 1С,` доповнити попереднім рядком `#   prro_fs_state_selftest (L-f1) + prro_fs_emulator (L-f2 + ручний, імітація ДПС) +`.

- [ ] **Step 3: Гард якорів**

Run: `python scripts/check-doc-anchors.py --quiet; echo exit=$?`
Expected: `exit=0`.

- [ ] **Step 4: Повний гейт x64 і x86** (монопольно — спершу `ListAgents`)

Run:
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
powershell -ExecutionPolicy Bypass -File run_tests.ps1 x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 x86
powershell -ExecutionPolicy Bypass -File run_tests.ps1 x64 -NoUapki
```
Expected: у всіх трьох прогонах — exit 0; `L-f1 prro_fs_state_selftest PASS`; `L-f2 prro_fs_emulator PASS` (x64, x86) і `SKIP` у `-NoUapki`; решта рівнів — як до початку роботи (PASS/SKIP без нових FAIL).

- [ ] **Step 5: Commit + push**

```bash
git add docs/integration-1c/uapki.md AGENTS.md version.h
git commit --only -m "docs: prro_fs_emulator — §9.2 інтеграції 1С, рівні L-f1/L-f2 у AGENTS.md" -- docs/integration-1c/uapki.md AGENTS.md version.h
git push -u origin prro-fs-emulator
```

- [ ] **Step 6: Повідомити споживача** — `SendMessage` сесії-архітектору `prro-test-contour-spec` (або тій, що веде їхній план): гілка й коміт, базова адреса `http://127.0.0.1:8099/fs`, тіло `/ping`, формат `POST /control` (`reset` з фіскальним номером тестової каси, `fault`, `set`), посилання на `docs/integration-1c/uapki.md` §9.2 і перелік розбіжностей з ДПС із блоку «Межі імітації».
