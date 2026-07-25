# UAPKI-тестування з 1С через HTTP-оракул — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Дати змогу тестувати ЕЦП-стек UAPKI з реальної 1С — консоль-оракул `uapki_fiscal_emulator`
(HTTP-сервер, що незалежно перевіряє підпис і повертає підписану квитанцію) + кнопки на 11 методів
UAPKI і HTTP-раунд-тріп у тестовій обробці.

**Architecture:** Консоль лінкує крипто-ядро UAPKI напряму (`process()`/`json_free()`), приймає
підписаний компонентою CMS по HTTP, VERIFY-ить за холістичним критерієм `status=="TOTAL-VALID"` і
відповідає підписаною квитанцією. Обробка 1С отримала персистентний об'єкт компоненти, кнопки
lifecycle підпису і кнопки HTTP-обміну з консоллю. Крипто по обидва боки — той самий рушій, різні
шляхи інтеграції (cross-path validation).

**Tech Stack:** C++17, `ixwebsocket` (`ix::HttpServer`), `nlohmann/json`, UAPKI (`uapki_bundle`,
`parson`), CMake; 1С:Підприємство (BSL, керована форма, `HTTPСоединение`).

**Spec:** `docs/superpowers/specs/2026-07-22-uapki-1c-testing-design.md` (ред. 2).

## Global Constraints

- **Жодних змін** у сабмодулі `extern/uapki` і в компоненті `AddinUAPKIConnect`. Уся робота — в гілці `docs-uapki`.
- Консоль будується **лише** при `BUILD_WITH_UAPKI=ON AND BUILD_TESTS=ON`; ціль **ручна** (не в `run_tests.ps1`).
- Тест-ключ: `tests/data/test-diia.p12`, пароль `testpassword`, `KEY_ID` = `5BC6C06EE1E00C1700E92AA7A9AD75F82D3CB7A9B66E3A98023209B24513315C`.
- Підпис: CAdES-BES enveloping, `signAlgo` = `1.2.804.2.1.1.1.1.3.1.1` (ДСТУ 4145), `detachedData:false`, `includeCert:true`, `includeContentTS:false`, `options.ignoreCertStatus:true`.
- Провайдер: `allowedProviders.lib` = `cm-pkcs12_x64` (x86: `cm-pkcs12_x86`); після INIT вимагати `result.countCmProviders == 1`.
- **Критерій прийняття VERIFY (оракул і 1С):** `errorCode==0 && signatureInfos[0].status=="TOTAL-VALID" && validSignatures && validDigests`. `statusSignature=="VALID"` **недостатньо**.
- Вкладений вміст: `result.content.bytes` (Base64) з опцією `options.returnContent:true`.
- **certCache/crlCache — лише writable-копія** тест-сертів у temp (CerStore пише в каталог), НЕ read-only оригінали.
- Консоль **НЕ друкує** OPEN-параметри/пароль у трейсі.
- Мова коду/комітів — укр/рос (за файлом). Логіка обробки — укр/рос як у наявних регіонах.

---

# ЧАСТИНА A — Консоль `uapki_fiscal_emulator`

**Файл:** `tests/uapki_fiscal_emulator.cpp` (новий, один файл; `pch.h` НЕ підключати).
Верифікація частини A — вбудований режим `--self-test` (без 1С) + ручний `curl`.

## Task A1: Скелет консолі + CMake-ціль (будується, слухає HTTP)

**Files:**
- Create: `tests/uapki_fiscal_emulator.cpp`
- Modify: `tests/CMakeLists.txt` (додати ціль після UAPKI-гейта, ~після рядка 260, у секції `BUILD_WITH_UAPKI`)

**Interfaces:**
- Produces: exe `uapki_fiscal_emulator_x64.exe`/`_x86.exe` у `bin/Release`; HTTP-сервер на `port` (деф. 8080), відповідає `200 "uapki_fiscal_emulator alive"` на `GET /ping`.

- [x] **Step 1: Створити скелет `tests/uapki_fiscal_emulator.cpp`**

```cpp
// uapki_fiscal_emulator — HTTP-оракул ЕЦП для тестування UAPKI з 1С (Роль 2).
// Standalone-exe; pch.h НЕ підключаємо (правило tests/). printf дозволено.
// ixwebsocket тягне winsock2 — його заголовки ПЕРШИМИ, до windows.h.
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>            // SetConsoleOutputCP, GetTempPathW, CopyFileW
#include <nlohmann/json.hpp>
#include <cstdio>
#include <mutex>
#include <string>

using nlohmann::json;

#ifdef _WIN64
static const char* ARCH_PROVIDER = "cm-pkcs12_x64";
#else
static const char* ARCH_PROVIDER = "cm-pkcs12_x86";
#endif

// Крипто-ядро UAPKI (C-лінкування, статично через uapki_bundle).
extern "C" char* process(const char* request);
extern "C" void  json_free(char* buf);

static ix::HttpResponsePtr httpResp(int code, const std::string& ctype, const std::string& body) {
    ix::WebSocketHttpHeaders h; h["Content-Type"] = ctype;
    return std::make_shared<ix::HttpResponse>(code, std::string(), ix::HttpErrorCode::Ok, h, body);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    int port = (argc > 1 && argv[1][0] != '-') ? std::atoi(argv[1]) : 8080;
    ix::initNetSystem();

    ix::HttpServer server(port, "127.0.0.1");
    server.setOnConnectionCallback(
        [](ix::HttpRequestPtr req, std::shared_ptr<ix::ConnectionState>) -> ix::HttpResponsePtr {
            std::printf("← %s %s (%zu B)\n", req->method.c_str(), req->uri.c_str(), req->body.size());
            std::fflush(stdout);
            if (req->method == "GET" && req->uri.rfind("/ping", 0) == 0)
                return httpResp(200, "text/plain", "uapki_fiscal_emulator alive");
            return httpResp(404, "text/plain", "unknown endpoint");
        });

    auto res = server.listen();
    if (!res.first) { std::printf("Не вдалося зайняти порт %d: %s\n", port, res.second.c_str()); return 1; }
    server.start();
    std::printf("uapki_fiscal_emulator слухає 127.0.0.1:%d — Ctrl-C для виходу\n", port);
    server.wait();
    ix::uninitNetSystem();
    return 0;
}
```

- [x] **Step 2: Додати CMake-ціль у `tests/CMakeLists.txt`**

Вставити ПІСЛЯ блоку `uapki_selftest` (після його `if(MSVC) target_compile_options(...) endif()`, ~рядок 261), у секції `BUILD_WITH_UAPKI`:

```cmake
# ---------------------------------------------------------------------------
# uapki_fiscal_emulator — HTTP-оракул ЕЦП (ручний інструмент для тесту з 1С).
# Лінкує крипто-ядро НАПРЯМУ (process/json_free) + HTTP-сервер ixwebsocket.
# ---------------------------------------------------------------------------
add_executable(uapki_fiscal_emulator uapki_fiscal_emulator.cpp)
set_target_properties(uapki_fiscal_emulator PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "uapki_fiscal_emulator${_TEST_ARCH_SUFFIX}")
target_link_libraries(uapki_fiscal_emulator PRIVATE uapki_bundle parson ixwebsocket ws2_32)
target_include_directories(uapki_fiscal_emulator PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/json
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/macros
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapki/include
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/uapkic/include)
target_compile_definitions(uapki_fiscal_emulator PRIVATE _WINDOWS UNICODE _UNICODE
    HOST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
    HOST_BIN_DIR="${CMAKE_SOURCE_DIR}/bin/Release")
# Провайдер cm-pkcs12 потрібен у рантаймі — гарантуємо його збірку.
add_dependencies(uapki_fiscal_emulator cm-pkcs12-provider)
if(MSVC)
    target_compile_options(uapki_fiscal_emulator PRIVATE /utf-8)
endif()
```

- [x] **Step 3: Зібрати**

Run: `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests`
Expected: успіх; зʼявляється `bin/Release/uapki_fiscal_emulator_x64.exe`.

- [x] **Step 4: Димовий запуск**

Run (в окремому вікні): `bin\Release\uapki_fiscal_emulator_x64.exe 8080`
Потім: `curl -s http://127.0.0.1:8080/ping`
Expected: `uapki_fiscal_emulator alive`; у консолі — рядок `← GET /ping`.

- [x] **Step 5: Commit**

```bash
git add tests/uapki_fiscal_emulator.cpp tests/CMakeLists.txt
git commit -m "test(uapki): скелет HTTP-оракула uapki_fiscal_emulator + CMake-ціль"
```

## Task A2: Крипто-bootstrap (INIT провайдера, writable certCache, OPEN/SELECT_KEY) під mutex

**Files:**
- Modify: `tests/uapki_fiscal_emulator.cpp`

**Interfaces:**
- Produces:
  - `json callUapki(const json& request)` — виклик `process()` під `g_uapkiMtx`, повертає розпарсену відповідь.
  - `bool cryptoBootstrap(const Config&)` — INIT (провайдер + writable cache, `countCmProviders==1`) → OPEN → KEYS → SELECT_KEY. `false` при будь-якій помилці.
  - `std::string b64encode(const std::string&)`, `bool b64decode(const std::string&, std::string&)`.
  - `Config` з полями: `int port; std::wstring keyPath, providersDir, dataDir, samplesDir; std::string pass; bool canned;`.

- [x] **Step 1: Додати конфіг, mutex, base64 і хелпери шляхів** (над `main`)

```cpp
static std::mutex g_uapkiMtx;
static std::wstring g_workDir;   // writable-копія сертів; прибирається при виході

struct Config {
    int port = 8080;
    std::wstring keyPath, providersDir, dataDir, samplesDir;
    std::string pass = "testpassword";
    bool canned = false;
};

// Виклик крипто-ядра ПІД ЗАМКОМ (UAPKI має глобальний стан — cm-providers.cpp).
static json callUapki(const json& request) {
    std::lock_guard<std::mutex> lk(g_uapkiMtx);
    std::string req = request.dump();
    char* res = process(req.c_str());
    if (!res) return json{{"errorCode", -1}, {"error", "process() returned null"}};
    json out;
    try { out = json::parse(res); }
    catch (...) { out = json{{"errorCode", -2}, {"error", "invalid JSON from process()"}}; }
    json_free(res);
    return out;
}

static std::string b64encode(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8) | (unsigned char)in[i+2];
        out += T[(v>>18)&63]; out += T[(v>>12)&63]; out += T[(v>>6)&63]; out += T[v&63];
    }
    if (i < in.size()) {
        bool two = (i + 1 < in.size());
        unsigned v = (unsigned char)in[i] << 16; if (two) v |= (unsigned char)in[i+1] << 8;
        out += T[(v>>18)&63]; out += T[(v>>12)&63]; out += two ? T[(v>>6)&63] : '='; out += '=';
    }
    return out;
}
static bool b64decode(const std::string& b64, std::string& out) {
    auto val = [](char c)->int {
        if (c>='A'&&c<='Z') return c-'A'; if (c>='a'&&c<='z') return c-'a'+26;
        if (c>='0'&&c<='9') return c-'0'+52; if (c=='+') return 62; if (c=='/') return 63; return -1; };
    out.clear(); int acc=0, nbits=0;
    for (char c : b64) {
        if (c=='='||c=='\r'||c=='\n'||c==' '||c=='\t') continue;
        int v = val(c); if (v < 0) return false;
        acc=(acc<<6)|v; nbits+=6;
        if (nbits>=8){ nbits-=8; out.push_back((char)((acc>>nbits)&0xFF)); }
    }
    return true;
}

// wstring → utf8 з forward-slash (щоб не екранувати у JSON).
static std::string fwd(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    for (char& c : s) if (c=='\\') c='/';
    return s;
}
```

- [x] **Step 2: Додати підготовку writable-каталогу й bootstrap**

```cpp
// Копіює <data>\certs, <data>\crls у temp-каталог (CerStore ПИШЕ туди).
static bool copyDir(const std::wstring& src, const std::wstring& dst) {
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true; // порожньо/нема — не фатал
    do {
        std::wstring n = fd.cFileName; if (n==L"."||n==L"..") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            CopyFileW((src+L"\\"+n).c_str(), (dst+L"\\"+n).c_str(), FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h); return true;
}
static bool prepareWorkDir(const std::wstring& dataDir) {
    wchar_t tp[MAX_PATH]; GetTempPathW(MAX_PATH, tp);
    g_workDir = std::wstring(tp) + L"uapki_oracle_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_workDir.c_str(), nullptr);
    copyDir(dataDir + L"\\certs", g_workDir + L"\\certs");
    copyDir(dataDir + L"\\crls",  g_workDir + L"\\crls");
    return true;
}

static bool cryptoBootstrap(const Config& cfg) {
    prepareWorkDir(cfg.dataDir);
    json p;
    p["offline"] = true;
    p["cmProviders"]["dir"] = fwd(cfg.providersDir) + "/";
    p["cmProviders"]["allowedProviders"] = json::array({ json{{"lib", ARCH_PROVIDER}} });
    p["certCache"]["path"] = fwd(g_workDir + L"\\certs") + "/";
    p["crlCache"]["path"]  = fwd(g_workDir + L"\\crls") + "/";
    json r = callUapki({{"method","INIT"},{"parameters",p}});
    if (r.value("errorCode", -1) != 0) { std::printf("INIT помилка: %s\n", r.value("error","").c_str()); return false; }
    int cnt = r["result"].value("countCmProviders", 0);
    if (cnt != 1) { std::printf("Провайдер НКІ не завантажився (countCmProviders=%d)\n", cnt); return false; }

    json op; op["provider"]="PKCS12"; op["storage"]=fwd(cfg.keyPath); op["password"]=cfg.pass; op["mode"]="RO";
    if (callUapki({{"method","OPEN"},{"parameters",op}}).value("errorCode",-1) != 0)
        { std::printf("OPEN тест-ключа не вдався\n"); return false; }   // пароль НЕ друкуємо
    json k = callUapki({{"method","KEYS"}});
    if (k.value("errorCode",-1)!=0 || !k["result"].contains("keys") || k["result"]["keys"].empty())
        { std::printf("KEYS порожній\n"); return false; }
    std::string id = k["result"]["keys"][0].value("id","");
    if (callUapki({{"method","SELECT_KEY"},{"parameters",{{"id",id}}}}).value("errorCode",-1) != 0)
        { std::printf("SELECT_KEY не вдався\n"); return false; }
    std::printf("Крипто-ядро готове: провайдер OK, ключ %s вибрано\n", id.substr(0,16).c_str());
    return true;
}
```

- [x] **Step 3: Розібрати CLI й викликати bootstrap у `main`** (перед `server.listen()`)

```cpp
    Config cfg;
    cfg.port = port;
    cfg.dataDir = L"" HOST_DATA_DIR;          // wide через L-конкатенацію макроса
    cfg.providersDir = L"" HOST_BIN_DIR;
    cfg.keyPath = cfg.dataDir + L"\\test-diia.p12";
    // (Прості CLI-прапорці --key/--pass/--providers/--data/--samples/--canned — за наявності argv; опустимо парсер тут, дефолти робочі.)
    if (!cryptoBootstrap(cfg)) { std::printf("Bootstrap не вдався — вихід\n"); return 2; }
```

> Примітка: `L"" HOST_DATA_DIR` конкатенує вузький макро-літерал у wide тільки якщо `HOST_DATA_DIR` — теж L-літерал. Оскільки макрос вузький, замінити на конвертацію: `cfg.dataDir = toW(HOST_DATA_DIR);` де `toW` — `MultiByteToWideChar(CP_UTF8,...)`. Додати хелпер `toW` поряд з `fwd`.

- [x] **Step 4: Зібрати й запустити — bootstrap проходить**

Run: `build_project.ps1 -WithUAPKI -WithTests`, потім `bin\Release\uapki_fiscal_emulator_x64.exe`
Expected: `Крипто-ядро готове: провайдер OK, ключ 5BC6C06EE1E00C17… вибрано`; далі слухає порт.

- [x] **Step 5: Commit**

```bash
git add tests/uapki_fiscal_emulator.cpp
git commit -m "test(uapki): крипто-bootstrap оракула (провайдер+writable cache+OPEN/SELECT_KEY) під mutex"
```

## Task A3: VERIFY-логіка + `/verify` + `--self-test` (позитив і НЕГАТИВ)

**Files:**
- Modify: `tests/uapki_fiscal_emulator.cpp`

**Interfaces:**
- Consumes: `callUapki`, `b64encode/b64decode`, `cryptoBootstrap`.
- Produces:
  - `struct VerifyOutcome { bool accepted; std::string status, statusSig, statusMd, signerCertId, contentB64; bool validSig=false, validDig=false; };`
  - `VerifyOutcome verifyCms(const std::string& derBytes)` — приймає СИРІ DER-байти, VERIFY за критерієм TOTAL-VALID.
  - `std::string signData(const std::string& rawBytes)` — SIGN CAdES-BES, повертає СИРІ DER-байти підпису (або порожньо).
  - `int runSelfTest(const Config&)` — 0 = усі перевірки пройшли.

- [x] **Step 1: Написати негативний self-test першим (він має провалитись, поки нема реалізації)**

Додати нижче `cryptoBootstrap`:

```cpp
static int runSelfTest(const Config& cfg) {
    int fails = 0;
    auto check = [&](bool ok, const char* msg){ std::printf(ok?"  ok: %s\n":"  FAIL: %s\n", msg); if(!ok) fails++; };

    // Позитив: підписати "hello" і перевірити — має бути accepted.
    std::string sig = signData("hello world");
    check(!sig.empty(), "signData повернув підпис");
    VerifyOutcome ok = verifyCms(sig);
    check(ok.accepted && ok.status=="TOTAL-VALID", "валідний CMS accepted (TOTAL-VALID)");
    std::string content; b64decode(ok.contentB64, content);
    check(content == "hello world", "витягнутий content.bytes == вхідні дані");

    // НЕГАТИВ 1: зіпсувати байт у DER → VERIFY НЕ має accepted.
    std::string bad = sig; if (bad.size() > 40) bad[bad.size()/2] ^= 0x01;
    VerifyOutcome nok = verifyCms(bad);
    check(!nok.accepted, "зіпсований CMS ВІДХИЛЕНО (не accepted)");

    // НЕГАТИВ 2: сміття замість CMS → не accepted, без падіння.
    VerifyOutcome junk = verifyCms(std::string("\x01\x02\x03not-a-cms", 12));
    check(!junk.accepted, "malformed CMS ВІДХИЛЕНО");

    std::printf("\n==== self-test: fails=%d ====\n", fails);
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Запустити self-test — має ПРОВАЛИТИСЬ (функцій ще нема)** — НЕ виконувався (див. «Фактичний перебіг»)

Тимчасово в `main` після bootstrap: `if (argc>1 && std::string(argv[argc-1])=="--self-test") return runSelfTest(cfg);`
Run: `uapki_fiscal_emulator_x64.exe --self-test`
Expected: помилка компіляції (`signData`/`verifyCms`/`VerifyOutcome` не визначені) — це очікуваний «червоний».

- [x] **Step 3: Реалізувати `verifyCms` і `signData`** (над `runSelfTest`)

```cpp
struct VerifyOutcome {
    bool accepted = false;
    std::string status, statusSig, statusMd, signerCertId, contentB64;
    bool validSig = false, validDig = false;
};

static VerifyOutcome verifyCms(const std::string& derBytes) {
    VerifyOutcome o;
    json p;
    p["signature"]["bytes"] = b64encode(derBytes);
    p["options"]["validationType"] = "STRUCT";
    p["options"]["returnContent"]  = true;
    json r = callUapki({{"method","VERIFY"},{"parameters",p}});
    if (r.value("errorCode", -1) != 0) return o;
    json& res = r["result"];
    if (res.contains("content") && res["content"].is_object() && res["content"].contains("bytes"))
        o.contentB64 = res["content"]["bytes"].get<std::string>();
    if (!res.contains("signatureInfos") || !res["signatureInfos"].is_array() || res["signatureInfos"].empty())
        return o;
    json& si = res["signatureInfos"][0];
    o.status       = si.value("status", "");
    o.statusSig    = si.value("statusSignature", "");
    o.statusMd     = si.value("statusMessageDigest", "");
    o.validSig     = si.value("validSignatures", false);
    o.validDig     = si.value("validDigests", false);
    o.signerCertId = si.value("signerCertId", "");
    // Критерій прийняття — ХОЛІСТИЧНИЙ (див. uapki_selftest.cpp:254-301, uapki.md §4.5).
    o.accepted = (o.status == "TOTAL-VALID") && o.validSig && o.validDig;
    return o;
}

static std::string signData(const std::string& rawBytes) {
    json sp;
    sp["signatureFormat"]="CAdES-BES"; sp["signAlgo"]="1.2.804.2.1.1.1.1.3.1.1";
    sp["detachedData"]=false; sp["includeCert"]=true; sp["includeTime"]=true; sp["includeContentTS"]=false;
    json p;
    p["signParams"] = sp;
    p["dataTbs"]    = json::array({ json{{"id","doc-0"},{"bytes", b64encode(rawBytes)}} });
    p["options"]["ignoreCertStatus"] = true;
    json r = callUapki({{"method","SIGN"},{"parameters",p}});
    if (r.value("errorCode",-1)!=0) return "";
    std::string b64 = r["result"]["signatures"][0].value("bytes","");
    std::string der; if (!b64decode(b64, der)) return "";
    return der;
}
```

- [x] **Step 4: Запустити self-test — має ПРОЙТИ**

Run: `uapki_fiscal_emulator_x64.exe --self-test`
Expected: усі `ok:`, `==== self-test: fails=0 ====`, exit 0. Особливо `зіпсований CMS ВІДХИЛЕНО` — доказ, що критерій ловить пошкодження вмісту.

- [x] **Step 5: Додати ендпоінт `POST /verify`** (у callback, перед `return httpResp(404,...)`)

```cpp
            if (req->method == "POST" && req->uri.rfind("/verify", 0) == 0) {
                VerifyOutcome o = verifyCms(req->body);
                json out{{"accepted",o.accepted},{"status",o.status},{"statusSignature",o.statusSig},
                         {"statusMessageDigest",o.statusMd},{"validSignatures",o.validSig},
                         {"validDigests",o.validDig},{"signerCertId",o.signerCertId},{"contentBase64",o.contentB64}};
                std::printf("  VERIFY: %s (status=%s)\n", o.accepted?"ACCEPTED":"REJECTED", o.status.c_str());
                return httpResp(o.accepted?200:422, "application/json", out.dump());
            }
```

- [x] **Step 6: Commit**

```bash
git add tests/uapki_fiscal_emulator.cpp
git commit -m "test(uapki): VERIFY-критерій TOTAL-VALID + /verify + self-test (позитив+негатив)"
```

## Task A4: Ендпоінт `POST /doc` — VERIFY + підписана квитанція

**Files:**
- Modify: `tests/uapki_fiscal_emulator.cpp`

**Interfaces:**
- Consumes: `verifyCms`, `signData`.
- Produces: `std::string buildTicketXml(int errorCode, const std::string& errorText)` — `ticket01`-подібний XML (windows-1251-сумісний ASCII-каркас); `POST /doc` повертає підписану квитанцію (DER) або `422`.

- [x] **Step 1: Додати генератор квитанції** (над callback)

```cpp
static std::string buildTicketXml(int errorCode, const std::string& errorText) {
    // ticket01-подібний каркас (ASCII; поля-заглушки достатні для round-trip).
    std::string x = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\r\n<TICKET>";
    x += "<UID>00000000-0000-0000-0000-000000000001</UID>";
    x += "<ORDERDATE>22072026</ORDERDATE><ORDERTIME>120000</ORDERTIME>";
    x += "<ERRORCODE>" + std::to_string(errorCode) + "</ERRORCODE>";
    x += "<ERRORTEXT>" + errorText + "</ERRORTEXT><VER>1</VER></TICKET>";
    return x;
}
```

- [x] **Step 2: Додати `POST /doc`** (у callback, перед `/verify`)

```cpp
            if (req->method == "POST" && req->uri.rfind("/doc", 0) == 0) {
                if (req->body.empty()) return httpResp(400, "text/plain", "empty body");
                VerifyOutcome o = verifyCms(req->body);
                std::string content; b64decode(o.contentB64, content);
                std::printf("  /doc: %s status=%s signer=%s\n  вміст: %.200s\n",
                            o.accepted?"ACCEPTED":"REJECTED", o.status.c_str(),
                            o.signerCertId.substr(0,16).c_str(), content.c_str());
                std::fflush(stdout);
                int code = o.accepted ? 0 : 9;                     // 0=Ok, 9=DocumentValidationError (ЄВПЕЗ)
                std::string text = o.accepted ? "" : ("Підпис не прийнято: " + o.status);
                std::string ticketDer = signData(buildTicketXml(code, text));
                if (ticketDer.empty()) return httpResp(500, "text/plain", "cannot sign ticket");
                return httpResp(o.accepted?200:422, "application/octet-stream", ticketDer);
            }
```

- [x] **Step 3: Розширити self-test — round-trip квитанції** (у `runSelfTest`, перед підсумком)

```cpp
    // Round-trip: підписати квитанцію і перевірити її назад.
    std::string tkt = signData(buildTicketXml(0, ""));
    VerifyOutcome tv = verifyCms(tkt);
    std::string tc; b64decode(tv.contentB64, tc);
    check(tv.accepted, "квитанція VERIFY-иться назад як TOTAL-VALID");
    check(tc.find("<TICKET>") != std::string::npos, "content квитанції містить <TICKET>");
```

- [x] **Step 4: Self-test проходить + ручний `/doc`**

Run: `uapki_fiscal_emulator_x64.exe --self-test` → `fails=0`.

ПОЗИТИВ (підпис нашим тест-ключем): підняти сервер **без** `--canned` і повернути йому ж свіжий CMS —
```
curl -s "http://127.0.0.1:8080/reference?type=check" -o ref.der
curl -s -X POST --data-binary @ref.der http://127.0.0.1:8080/doc -o ticket.der
```
Expected: HTTP `200`; у консолі `/doc: ACCEPTED status=TOTAL-VALID certEmbedded=так signer=…`;
`ticket.der` — бінарний CMS (~1-3 КБ).
(`/reference` тут обовʼязково БЕЗ `--canned`, інакше він віддасть еталон ДПС і позитив провалиться.)

НЕГАТИВ (еталон ДПС — очікувано НЕ приймається):
```
curl -s -o tk.der -w "%{http_code}\n" -X POST --data-binary \
  @"R:\github\prro_docs\Єдине вікно подання електронної звітності\Приклади\Приклади з КЕП\чек.xml.signed" \
  http://127.0.0.1:8080/doc
```
Expected: `422`; у консолі `/doc: REJECTED status=(немає)` і рядок
`причина: VERIFY errorCode=4161` (`CERT_NOT_FOUND`). Це НЕ дефект оракула: еталони ДПС підписані
з позначкою часу (CAdES-T), а UAPKI бракує сертифіката **TSP-сервера АЦСК** (у детальній
відповіді — `"expectedCerts":[{"entity":"TSP","keyId":"66ECEB8E…"}]`); сертифікат підписувача при
цьому знайдено (`statusSignature:"VALID"`, `validDigests:true`), але `status:"INDETERMINATE"` і
`validSignatures:false`. Той самий `4161` дає `native_host` (кейс 5) через головну DLL без
тестового `certCache`. Кейс лишається корисним: доводить, що оракул не приймає документ, який не
проходить холістичний критерій.

- [x] **Step 5: Commit**

```bash
git add tests/uapki_fiscal_emulator.cpp
git commit -m "test(uapki): /doc — VERIFY чека + підписана квитанція ticket01"
```

## Task A5: Ендпоінт `GET /reference` + фіналізація (прибирання, CLI)

**Files:**
- Modify: `tests/uapki_fiscal_emulator.cpp`

**Interfaces:**
- Consumes: `signData`, `--samples`.
- Produces: `GET /reference?type=check|zrep|ticket` → DER CMS (свіжо-підписаний або канонічний `.signed` при `--canned`).

- [x] **Step 1: Додати читання файлу й `/reference`**

```cpp
static bool readFileBin(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr); out.resize(sz); DWORD rd=0;
    bool ok = out.empty() ? true : (ReadFile(h, &out[0], sz, &rd, nullptr) && rd==sz);
    CloseHandle(h); return ok;
}
```
У callback (перед `/doc`):
```cpp
            if (req->method == "GET" && req->uri.rfind("/reference", 0) == 0) {
                std::string type = "check";
                auto q = req->uri.find("type=");
                if (q != std::string::npos) type = req->uri.substr(q+5);
                std::string der;
                if (cfg.canned) {
                    const wchar_t* fn = (type=="zrep") ? L"\\Z-звіт.xml.signed"
                                       : (type=="ticket") ? L"\\чек.xml.signed" : L"\\чек.xml.signed";
                    if (cfg.samplesDir.empty() || !readFileBin(cfg.samplesDir + fn, der))
                        return httpResp(404, "text/plain", "sample not found");
                } else {
                    std::string doc = (type=="zrep") ? "<ZREP>reference</ZREP>"
                                     : (type=="ticket") ? buildTicketXml(0,"") : "<CHECK>reference</CHECK>";
                    der = signData(doc);
                    if (der.empty()) return httpResp(500, "text/plain", "cannot sign reference");
                }
                std::printf("  /reference type=%s → %zu B\n", type.c_str(), der.size());
                return httpResp(200, "application/octet-stream", der);
            }
```
> `cfg` треба захопити в лямбді: змінити `setOnConnectionCallback([](…))` на `setOnConnectionCallback([&cfg](…))`.

- [x] **Step 2: Додати прибирання workDir при виході** (наприкінці `main`, після `server.wait()` недосяжно через Ctrl-C — реєструємо на SIGINT або лишаємо temp; достатньо задокументувати)

```cpp
    // g_workDir лишається в %TEMP% після Ctrl-C — прийнятно (temp прибирає ОС).
```

- [x] **Step 3: Self-test лишається зеленим + ручний `/reference`**

Run: `--self-test` → `fails=0`. Ручний: `curl -s http://127.0.0.1:8080/reference?type=check -o ref.der` → бінарний CMS.

- [x] **Step 4: Commit**

```bash
git add tests/uapki_fiscal_emulator.cpp
git commit -m "test(uapki): /reference (свіжий/канонічний .signed) + фіналізація консолі"
```

---

# ЧАСТИНА B — Обробка 1С (`ExtDataProcessors/SimplyAddinConnect_test`)

**Файли:** `.../NativeAddIn_Н/Forms/Форма/Ext/Form/Module.bsl`, `.../Form.xml`.
Верифікація — **ручне завантаження у 1С:Підприємстві** (автотесту нема). Кожен крок — точкова правка.

## Task B1: Module.bsl — виправити баг + новий регіон UAPKI

**Files:**
- Modify: `.../Forms/Форма/Ext/Form/Module.bsl` (регіон `# Область UAPKI`, рядки 345-359; шапка Перем, рядки 1-4; дефолти в `ПриСозданииНаСервере`)

**Interfaces:**
- Produces: персистентний `ОбъектДрайвераUAPKI`; процедури-обробники команд (імена = `<Action>` у Form.xml, Task B2): `СоздатьОбъектUAPKI`, `КомандаVERSIONUAPKI` (виправлена), `КомандаINITUAPKI`, `КомандаDEINITUAPKI`, `КомандаPROVIDERSUAPKI`, `КомандаOPENUAPKI`, `КомандаCLOSEUAPKI`, `КомандаKEYSUAPKI`, `КомандаSELECTKEYUAPKI`, `КомандаSIGNUAPKI`, `КомандаVERIFYUAPKI`, `КомандаDIGESTUAPKI`, `ПодписатьИОтправитьUAPKI`, `ЗапроситьЭталонUAPKI`.

- [x] **Step 1: Оголосити персистентний об'єкт** — у шапці модуля (після рядка 4)

```bsl
&НаКлиенте Перем ОбъектДрайвераUAPKI;
```

- [x] **Step 2: Додати дефолти реквізитів** — у `ПриСозданииНаСервере` (перед `КонецПроцедуры`, рядок 67)

```bsl
	// UAPKI — тест-ключ і дані за замовчуванням (правте під себе).
	ШляхДоКлючаUAPKI = "R:\github\SimplyAddinConnect\tests\data\test-diia.p12";
	ПарольКлючаUAPKI = "testpassword";
	ДаніДляПідпису = "<CHECK><SUM>1.00</SUM></CHECK>";
	ПідписBase64 = "";
	ІдКлючаUAPKI = "";
	РезультатUAPKI = "";
	ХостКонсолі = "127.0.0.1";
	ПортКонсолі = "8080";
```

- [x] **Step 3: Замінити ВЕСЬ регіон `# Область UAPKI`** (рядки 345-359) на новий

```bsl
# Область UAPKI

&НаКлиенте
Функция ОбъектUAPKIГотов()
	Если ОбъектДрайвераUAPKI = Неопределено Тогда
		Сообщить("Спочатку «Подключить компоненту», потім «Створити обʼєкт UAPKI»"); Возврат Ложь;
	КонецЕсли;
	Возврат Истина;
КонецФункции

// Спільна обгортка: викликає метод, пише сирий JSON у РезультатUAPKI, повертає розібрану структуру.
&НаКлиенте
Функция ВыполнитьUAPKI(Метод, Параметры = "")
	Если НЕ ОбъектUAPKIГотов() Тогда Возврат Неопределено; КонецЕсли;
	Попытка
		Ответ = ОбъектДрайвераUAPKI.ВызватьUAPKI(Метод, Параметры);
		РезультатUAPKI = Ответ;
		Сообщить(Метод + " → " + Лев(Ответ, 500));
		Возврат РаскодироватьJSON(Ответ);
	Исключение
		РезультатUAPKI = "Помилка " + Метод + ": " + ОписаниеОшибки();
		Сообщить(РезультатUAPKI); Возврат Неопределено;
	КонецПопытки;
КонецФункции

&НаКлиенте
Процедура СоздатьОбъектUAPKI(Команда)
	Попытка
		ОбъектДрайвераUAPKI = Новый("AddIn." + ПолучитьИмяКомпоненты() + ".AddinUAPKIConnect");
		ОбъектДрайвераUAPKI.ИспользоватьЛогирование("Trace", "C:\log\SimplyAddinConnect.log");
		Сообщить("Обʼєкт AddinUAPKIConnect створено");
	Исключение
		Сообщить("Помилка створення обʼєкта (чи підключена компонента?): " + ОписаниеОшибки());
	КонецПопытки;
КонецПроцедуры

// --- Будівники параметрів (через КодироватьJSON — коректний escaping) ---
&НаКлиенте
Функция ПараметриINIT()
	П = Новый Структура; П.Вставить("offline", Истина); Возврат КодироватьJSON(П);
КонецФункции

&НаКлиенте
Функция ПараметриOPEN()
	П = Новый Структура;
	П.Вставить("provider", "PKCS12"); П.Вставить("storage", ШляхДоКлючаUAPKI);
	П.Вставить("password", ПарольКлючаUAPKI); П.Вставить("mode", "RO");
	Возврат КодироватьJSON(П);
КонецФункции

&НаКлиенте
Функция ПараметриSIGN()
	B64 = Base64Строка(ПолучитьДвоичныеДанныеИзСтроки(ДаніДляПідпису));
	signParams = Новый Структура;
	signParams.Вставить("signatureFormat", "CAdES-BES");
	signParams.Вставить("signAlgo", "1.2.804.2.1.1.1.1.3.1.1");
	signParams.Вставить("detachedData", Ложь); signParams.Вставить("includeCert", Истина);
	signParams.Вставить("includeTime", Истина); signParams.Вставить("includeContentTS", Ложь);
	doc = Новый Структура; doc.Вставить("id", "doc-0"); doc.Вставить("bytes", B64);
	dataTbs = Новый Массив; dataTbs.Добавить(doc);
	options = Новый Структура; options.Вставить("ignoreCertStatus", Истина);
	П = Новый Структура;
	П.Вставить("signParams", signParams); П.Вставить("dataTbs", dataTbs); П.Вставить("options", options);
	Возврат КодироватьJSON(П);
КонецФункции

&НаКлиенте
Функция ПараметриVERIFY()
	signature = Новый Структура; signature.Вставить("bytes", ПідписBase64);
	options = Новый Структура; options.Вставить("validationType", "STRUCT"); options.Вставить("returnContent", Истина);
	П = Новый Структура; П.Вставить("signature", signature); П.Вставить("options", options);
	Возврат КодироватьJSON(П);
КонецФункции

// --- Кнопки методів ---
&НаКлиенте Процедура КомандаVERSIONUAPKI(Команда) ВыполнитьUAPKI("VERSION", ""); КонецПроцедуры
&НаКлиенте Процедура КомандаINITUAPKI(Команда) ВыполнитьUAPKI("INIT", ПараметриINIT()); КонецПроцедуры
&НаКлиенте Процедура КомандаDEINITUAPKI(Команда) ВыполнитьUAPKI("DEINIT", ""); КонецПроцедуры
&НаКлиенте Процедура КомандаPROVIDERSUAPKI(Команда) ВыполнитьUAPKI("PROVIDERS", ""); КонецПроцедуры
&НаКлиенте Процедура КомандаOPENUAPKI(Команда) ВыполнитьUAPKI("OPEN", ПараметриOPEN()); КонецПроцедуры
&НаКлиенте Процедура КомандаCLOSEUAPKI(Команда) ВыполнитьUAPKI("CLOSE", ""); КонецПроцедуры

&НаКлиенте
Процедура КомандаKEYSUAPKI(Команда)
	Стр = ВыполнитьUAPKI("KEYS", "");
	Если Стр <> Неопределено И Стр.errorCode = 0 И Стр.result.keys.Количество() > 0 Тогда
		ІдКлючаUAPKI = Стр.result.keys[0].id;
		Сообщить("Обрано ключ: " + ІдКлючаUAPKI);
	КонецЕсли;
КонецПроцедуры

&НаКлиенте
Процедура КомандаSELECTKEYUAPKI(Команда)
	П = Новый Структура; П.Вставить("id", ІдКлючаUAPKI);
	ВыполнитьUAPKI("SELECT_KEY", КодироватьJSON(П));
КонецПроцедуры

&НаКлиенте
Процедура КомандаSIGNUAPKI(Команда)
	Стр = ВыполнитьUAPKI("SIGN", ПараметриSIGN());
	Если Стр <> Неопределено И Стр.errorCode = 0 Тогда
		ПідписBase64 = Стр.result.signatures[0].bytes;
		Сообщить("Підпис отримано (" + Строка(СтрДлина(ПідписBase64)) + " симв. Base64)");
	КонецЕсли;
КонецПроцедуры

&НаКлиенте
Процедура КомандаVERIFYUAPKI(Команда)
	Стр = ВыполнитьUAPKI("VERIFY", ПараметриVERIFY());
	Если Стр <> Неопределено И Стр.errorCode = 0 Тогда
		С = Стр.result.signatureInfos[0].status;
		Сообщить("Вердикт: " + С + ?(С = "TOTAL-VALID", " (валідний)", " (НЕ валідний)"));
	КонецЕсли;
КонецПроцедуры

&НаКлиенте
Процедура КомандаDIGESTUAPKI(Команда)
	B64 = Base64Строка(ПолучитьДвоичныеДанныеИзСтроки(ДаніДляПідпису));
	П = Новый Структура; П.Вставить("bytes", B64); П.Вставить("hashAlgo", "1.2.804.2.1.1.1.1.2.1");
	ВыполнитьUAPKI("DIGEST", КодироватьJSON(П));
КонецПроцедуры

#КонецОбласти
```

- [x] **Step 4: Синтаксична перевірка** — відкрити `Module.bsl`, переконатись, що регіон `#КонецОбласти` збалансований і немає дублю `Область UAPKI`.

Run: `grep -c "Область UAPKI" ".../Module.bsl"` → Expected: `2` (відкриття `# Область UAPKI` + `#КонецОбласти` рахуються окремо; фактично один блок).

- [x] **Step 5: Commit**

```bash
git add "ExtDataProcessors/SimplyAddinConnect_test/NativeAddIn_Н/Forms/Форма/Ext/Form/Module.bsl"
git commit -m "test(uapki): обробка — виправлено КомандаВерсияUAPKI + регіон lifecycle підпису"
```

## Task B2: Module.bsl — HTTP-раунд-тріп (Поверхня B)

**Files:**
- Modify: `.../Module.bsl` (додати регіон `#Область UAPKI_HTTP` після регіону UAPKI)

**Interfaces:**
- Consumes: `ВыполнитьUAPKI`, `ПараметриSIGN`, `ПараметриVERIFY`, `РаскодироватьJSON`.
- Produces: `ПодписатьИОтправитьUAPKI`, `ЗапроситьЭталонUAPKI`.

- [x] **Step 1: Додати регіон HTTP-обміну**

```bsl
#Область UAPKI_HTTP

&НаКлиенте
Функция ОбменССервером(Метод, Ресурс, ТелоДвоичное = Неопределено)
	Соединение = Новый HTTPСоединение(ХостКонсолі, Число(ПортКонсолі));
	Запрос = Новый HTTPЗапрос(Ресурс);
	Если ТелоДвоичное <> Неопределено Тогда
		Запрос.Заголовки.Вставить("Content-Type", "application/octet-stream");
		Запрос.УстановитьТелоИзДвоичныхДанных(ТелоДвоичное);
	КонецЕсли;
	Если Метод = "POST" Тогда
		Возврат Соединение.ОтправитьДляОбработки(Запрос);
	Иначе
		Возврат Соединение.Получить(Запрос);
	КонецЕсли;
КонецФункции

// Сценарій 1+3: підписати чек компонентою → POST /doc → розгорнути підписану квитанцію.
&НаКлиенте
Процедура ПодписатьИОтправитьUAPKI(Команда)
	Стр = ВыполнитьUAPKI("SIGN", ПараметриSIGN());
	Если Стр = Неопределено ИЛИ Стр.errorCode <> 0 Тогда Возврат; КонецЕсли;
	ПідписBase64 = Стр.result.signatures[0].bytes;
	Попытка
		Ответ = ОбменССервером("POST", "/doc", Base64Значение(ПідписBase64));
	Исключение
		Сообщить("HTTP помилка: " + ОписаниеОшибки()); Возврат;
	КонецПопытки;
	Сообщить("Консоль відповіла: HTTP " + Ответ.КодСостояния);
	Если Ответ.КодСостояния <> 200 Тогда Возврат; КонецЕсли;
	// Квитанція (бінарний CMS) → авто-VERIFY компонентою + видобуток вмісту.
	ПідписBase64 = Base64Строка(Ответ.ПолучитьТелоКакДвоичныеДанные());
	СтрV = ВыполнитьUAPKI("VERIFY", ПараметриVERIFY());
	Если СтрV <> Неопределено И СтрV.errorCode = 0 Тогда
		Вміст = СтрV.result.content.bytes;
		XML = ПолучитьСтрокуИзДвоичныхДанных(Base64Значение(Вміст));
		Сообщить("Квитанція ДПС (розгорнута): " + XML);
	КонецЕсли;
КонецПроцедуры

// Сценарій 2: запитати еталон із консолі → перевірити компонентою.
&НаКлиенте
Процедура ЗапроситьЭталонUAPKI(Команда)
	Попытка
		Ответ = ОбменССервером("GET", "/reference?type=check");
	Исключение
		Сообщить("HTTP помилка: " + ОписаниеОшибки()); Возврат;
	КонецПопытки;
	Если Ответ.КодСостояния <> 200 Тогда Сообщить("Консоль: HTTP " + Ответ.КодСостояния); Возврат; КонецЕсли;
	ПідписBase64 = Base64Строка(Ответ.ПолучитьТелоКакДвоичныеДанные());
	СтрV = ВыполнитьUAPKI("VERIFY", ПараметриVERIFY());
	Если СтрV <> Неопределено И СтрV.errorCode = 0 Тогда
		С = СтрV.result.signatureInfos[0].status;
		Вміст = ПолучитьСтрокуИзДвоичныхДанных(Base64Значение(СтрV.result.content.bytes));
		Сообщить("Еталон: " + С + " | вміст: " + Вміст);
	КонецЕсли;
КонецПроцедуры

#КонецОбласти
```

- [x] **Step 2: Commit**

```bash
git add "ExtDataProcessors/SimplyAddinConnect_test/NativeAddIn_Н/Forms/Форма/Ext/Form/Module.bsl"
git commit -m "test(uapki): обробка — HTTP-раунд-тріп проти консолі (POST /doc, GET /reference)"
```

## Task B3: Form.xml — реквізити, команди, елементи

**Files:**
- Modify: `.../Forms/Форма/Ext/Form.xml`

**Правило id:** глобальний max наявний = **331**. Нові — від **332**, послідовно, БЕЗ повторів
(item/attribute/command в одному просторі). Точні присвоєння — у таблицях нижче.

- [x] **Step 1: Виправити назву команди КомандаВерсияUAPKI** (щоб відповідала дії VERSION)

У блоці `<Command name="КомандаВерсияUAPKI" id="3">` (рядки 1074-1092) `<Action>КомандаВерсияUAPKI</Action>` лишається (процедуру виправлено в Task B1 на VERSION). Змінити `<v8:content>Версия UAPKI</v8:content>` (обидва, ru+uk) на `<v8:content>VERSION</v8:content>` — тепер напис = дія.

- [x] **Step 2: Додати 8 реквізитів** — у секцію `<Attributes>` (перед `</Attributes>`, рядок 846)

Кожен — за шаблоном (String, Variable). Приклад для першого:

```xml
		<Attribute name="ШляхДоКлючаUAPKI" id="332">
			<Title><v8:item><v8:lang>uk</v8:lang><v8:content>Шлях до ключа .p12</v8:content></v8:item></Title>
			<Type><v8:Type>xs:string</v8:Type><v8:StringQualifiers><v8:Length>0</v8:Length><v8:AllowedLength>Variable</v8:AllowedLength></v8:StringQualifiers></Type>
		</Attribute>
```

Решта (той самий шаблон, свій `name`/`id`/`Title`):

| name | id | Title |
|---|---|---|
| ШляхДоКлючаUAPKI | 332 | Шлях до ключа .p12 |
| ПарольКлючаUAPKI | 333 | Пароль ключа |
| ДаніДляПідпису | 334 | Дані для підпису (XML чека) |
| ПідписBase64 | 335 | Підпис (Base64) |
| ІдКлючаUAPKI | 336 | Id ключа |
| РезультатUAPKI | 337 | Результат UAPKI |
| ХостКонсолі | 338 | Хост консолі |
| ПортКонсолі | 339 | Порт консолі |

- [x] **Step 3: Додати 13 команд** — у секцію `<Commands>` (перед `</Commands>`, рядок 1258)

Шаблон (приклад):

```xml
		<Command name="СоздатьОбъектUAPKI" id="340">
			<Title><v8:item><v8:lang>uk</v8:lang><v8:content>Створити обʼєкт UAPKI</v8:content></v8:item></Title>
			<Action>СоздатьОбъектUAPKI</Action>
			<CurrentRowUse>DontUse</CurrentRowUse>
		</Command>
```

Решта (той самий шаблон):

| name (= Action) | id | Title |
|---|---|---|
| СоздатьОбъектUAPKI | 340 | Створити обʼєкт UAPKI |
| КомандаINITUAPKI | 341 | INIT |
| КомандаDEINITUAPKI | 342 | DEINIT |
| КомандаPROVIDERSUAPKI | 343 | PROVIDERS |
| КомандаOPENUAPKI | 344 | OPEN |
| КомандаCLOSEUAPKI | 345 | CLOSE |
| КомандаKEYSUAPKI | 346 | KEYS |
| КомандаSELECTKEYUAPKI | 347 | SELECT_KEY |
| КомандаSIGNUAPKI | 348 | SIGN |
| КомандаVERIFYUAPKI | 349 | VERIFY |
| КомандаDIGESTUAPKI | 350 | DIGEST |
| ПодписатьИОтправитьUAPKI | 351 | Підписати й відправити на валідацію |
| ЗапроситьЭталонUAPKI | 352 | Запитати еталон і перевірити |

- [x] **Step 4: Додати елементи у групу `ГруппаUAPKI`** (ChildItems, рядки 226-232)

Перед наявним `<Button name="КомандаВерсияUAPKI" …>` (рядок 227) вставити **8 InputField** (шаблон нижче), а ПІСЛЯ нього — **13 Button** (шаблон нижче). Кожен InputField споживає 3 id (field+ContextMenu+ExtendedTooltip), кожен Button — 2 id (button+ExtendedTooltip). Item-id — від **353** послідовно.

Шаблон InputField (приклад для першого поля, id 353/354/355):

```xml
<InputField name="ПолеШляхДоКлючаUAPKI" id="353">
	<DataPath>ШляхДоКлючаUAPKI</DataPath>
	<Title><v8:item><v8:lang>uk</v8:lang><v8:content>Шлях до ключа .p12</v8:content></v8:item></Title>
	<ContextMenu name="ПолеШляхДоКлючаUAPKIContextMenu" id="354"/>
	<ExtendedTooltip name="ПолеШляхДоКлючаUAPKIExtendedTooltip" id="355"/>
</InputField>
```

InputFields (DataPath = реквізит; для `ДаніДляПідпису`,`ПідписBase64`,`РезультатUAPKI` додати `<MultiLine>true</MultiLine>` перед `<ContextMenu>`):

| DataPath | field id | ctxMenu id | tooltip id | MultiLine |
|---|---|---|---|---|
| ШляхДоКлючаUAPKI | 353 | 354 | 355 | — |
| ПарольКлючаUAPKI | 356 | 357 | 358 | — |
| ІдКлючаUAPKI | 359 | 360 | 361 | — |
| ДаніДляПідпису | 362 | 363 | 364 | true |
| ПідписBase64 | 365 | 366 | 367 | true |
| ХостКонсолі | 368 | 369 | 370 | — |
| ПортКонсолі | 371 | 372 | 373 | — |
| РезультатUAPKI | 374 | 375 | 376 | true |

Шаблон Button (приклад, id 377/378):

```xml
<Button name="КнопкаСоздатьОбъектUAPKI" id="377">
	<Type>UsualButton</Type>
	<CommandName>Form.Command.СоздатьОбъектUAPKI</CommandName>
	<ExtendedTooltip name="КнопкаСоздатьОбъектUAPKITooltip" id="378"/>
</Button>
```

Buttons (CommandName = `Form.Command.<name>`; існуючий VERSION-button НЕ дублювати):

| Command name | button id | tooltip id |
|---|---|---|
| СоздатьОбъектUAPKI | 377 | 378 |
| КомандаINITUAPKI | 379 | 380 |
| КомандаDEINITUAPKI | 381 | 382 |
| КомандаPROVIDERSUAPKI | 383 | 384 |
| КомандаOPENUAPKI | 385 | 386 |
| КомандаKEYSUAPKI | 387 | 388 |
| КомандаSELECTKEYUAPKI | 389 | 390 |
| КомандаSIGNUAPKI | 391 | 392 |
| КомандаVERIFYUAPKI | 393 | 394 |
| КомандаCLOSEUAPKI | 395 | 396 |
| КомандаDIGESTUAPKI | 397 | 398 |
| ПодписатьИОтправитьUAPKI | 399 | 400 |
| ЗапроситьЭталонUAPKI | 401 | 402 |

- [x] **Step 5: Перевірити коректність XML і завантажити в 1С**

Run: `powershell -Command "[xml](Get-Content -Raw -Path '.../Form.xml') | Out-Null; 'XML OK'"` → Expected: `XML OK` (без винятку — well-formed).
Потім: завантажити обробку в 1С:Підприємстві (Конфігуратор → відкрити зовнішню обробку з джерела / зібрати `.epf`), відкрити форму — усі кнопки/поля групи UAPKI видимі, форма відкривається без помилок.

- [x] **Step 6: Commit**

```bash
git add "ExtDataProcessors/SimplyAddinConnect_test/NativeAddIn_Н/Forms/Форма/Ext/Form.xml"
git commit -m "test(uapki): обробка Form.xml — реквізити/команди/кнопки UAPKI + фікс назви VERSION"
```

---

# ЧАСТИНА C — Документація

## Task C1: AGENTS.md + docs/integration-1c/uapki.md

**Files:**
- Modify: `AGENTS.md` (таблиця тестів), `docs/integration-1c/uapki.md` (§9)

- [x] **Step 1: Додати рядок у таблицю тестів `AGENTS.md`** (після рядка `native_host.exe`)

```markdown
| `uapki_fiscal_emulator.exe` | — (ручний) | **так** | HTTP-оракул ДПС: VERIFY підпису 1С (критерій TOTAL-VALID) + підписана квитанція + віддача еталонів; `--self-test` (позитив+негатив) |
```

- [x] **Step 2: Дописати в `docs/integration-1c/uapki.md` §9** (після опису `native_host.exe`)

```markdown
- **`uapki_fiscal_emulator.exe`** (ручний) — HTTP-оракул: грає «приймаючу сторону» (сервер ДПС).
  1С підписує чек компонентою й шле по HTTP (`POST /doc`), консоль незалежно `VERIFY`-ить (критерій
  `status=="TOTAL-VALID"`) і повертає підписану квитанцію, яку 1С розгортає компонентою. У тестовій
  обробці — кнопки «Підписати й відправити на валідацію» / «Запитати еталон і перевірити». Запуск:
  `uapki_fiscal_emulator_x64.exe [port] [--canned] [--self-test]`.
```

- [x] **Step 3: Оновити пам'ять** — `memory/uapki-testing-plan.md` дописати рядок про HTTP-оракул і Варіант A; оновити пойнтер у `MEMORY.md`.

- [x] **Step 4: Commit**

```bash
git add AGENTS.md docs/integration-1c/uapki.md
git commit -m "docs(uapki): HTTP-оракул uapki_fiscal_emulator у таблиці тестів і §9"
```

---

## Self-Review (виконано під час написання)

**Spec coverage:**
- §3.2 ендпоінти → Tasks A3(/verify), A4(/doc), A5(/reference). ✅
- §3.3 mutex → A2 (`callUapki` під `g_uapkiMtx`). ✅
- §3.4 провайдер+countCmProviders / writable certCache / TOTAL-VALID / content.bytes / certIds → A2, A3. ✅ (certIds-звірка — опційне посилення; критерій TOTAL-VALID реалізовано.)
- §3.5 пароль не друкується → A2 (OPEN без друку пароля). ✅
- §4.1 фікс + 11 кнопок + персистентний Перем + JSON через ЗаписьJSON → B1, B3. ✅
- §4.2 host/port окремо + бінарний HTTP → B2, B3. ✅
- §6 CMake + add_dependencies + AGENTS + uapki.md → A1, C1. ✅
- §8 негативний self-test → A3, A4. ✅

**Placeholder scan:** код у кроках повний; Form.xml — точні шаблони + таблиці id (не плейсхолдери).
Єдине свідоме спрощення — CLI-парсер прапорців у A2 Step 3 (дефолти робочі; повний парсер — за потреби).

**Type consistency:** `VerifyOutcome`, `callUapki`, `signData`, `verifyCms`, `buildTicketXml`,
`ВыполнитьUAPKI`, `ПараметриSIGN/OPEN/VERIFY/INIT`, `ОбменССервером` — імена узгоджені між Tasks.
Імена процедур-обробників у B1/B2 = `<Action>`/`CommandName` у B3.

**Ризик №1 — Form.xml:** ручне редагування; звіряти завантаженням у 1С (B3 Step 5). Якщо форма не
відкривається — перевірити унікальність усіх id ≥ 332 і збалансованість тегів.

---

## Фактичний перебіг виконання (2026-07-25)

**Статус: виконано, гейт зелений.** Нижче — чесні розбіжності між планом і тим, що сталося.

**Коміти згруповано.** План передбачав 9 окремих `Step: Commit`; фактично зроблено 4 коміти за
логічними одиницями (консоль+CMake / `Module.bsl` / `Form.xml` / доки+план), бо консоль писалась
цілісним файлом, і штучне дроблення дало б несправжню історію.

**A3 Step 2 (TDD-«червоний») НЕ виконувався** — єдиний невідмічений крок. Файл консолі писався
одним проходом, тож проміжного стану «self-test не компілюється» не існувало. Негативні перевірки
з A3/A4 при цьому реалізовані й реально проходять.

**Виконано понад план:**
- **Повний CLI** (§3.6 спеки) замість «свідомого спрощення» з A2 Step 3 — без нього `--canned`,
  `--samples` і `--self-test` були б мертвим кодом. Хибну пораду плану `L"" HOST_DATA_DIR`
  (не працює: макрос — вузький літерал) замінено на `u8to16()`, як у `native_host`.
- **Реальне прибирання temp** через `SetConsoleCtrlHandler` замість A5 Step 2 («достатньо
  задокументувати»), під `g_uapkiMtx`+`g_workDirMtx` — інакше Ctrl-C зносив би `certCache` під
  активною крипто-операцією.
- **Крос-чек `certIds`** (§3.4 спеки, план позначив як «опційне посилення») — реалізовано як
  окреме діагностичне поле `certEmbedded`, поза критерієм прийняття.
- **`errorCode`/`errorText`** у трейсі та JSON `/verify` — мовчазний `REJECTED` без причини
  знецінював оракула як інструмент діагностики.
- **Ліміт тіла 1 МіБ** (§3.2) з кодом `413` і `404` на невідомий `type`.
- **Власне очікування з предикатом** замість `server.wait()`: `ix::SocketServer::wait()` чекає на
  `condition_variable` БЕЗ предиката, тож спурйозне пробудження тихо завершило б оракула.

**Верифікація (фактична):**
- `build_project.ps1 -WithUAPKI -WithTests` — обидві архітектури, ZIP зібрано;
- `run_tests.ps1 x64` → `PASS=23 FAIL=0`, `x86` → `PASS=22 FAIL=0 SKIP=1`;
- `--self-test` x64 і x86 → `fails=0` (10 перевірок);
- ендпоінти перевірені `curl`: 200/400/404/413/422 за специфікацією, пароль у трейс не потрапляє;
- **обробка 1С зібрана в `.epf` реальним DESIGNER 8.3.27.1644** і
  `/CheckModules -ExtendedModulesCheck` → «Синтаксических ошибок не обнаружено!».

**Знахідки поза обсягом задачі (НЕ виправлялись, код не чіпався):**
1. **L3.1 крос-валідація ПРРО ніколи не виконувалась.** `run_tests.ps1` передає `native_host case 5`
   корінь `R:/github/prro_docs`, а `.signed` лежать трьома рівнями глибше; пошук у
   `case5_crossValidatePrro` нерекурсивний → завжди «SKIP … без *.signed» і **PASS**. Мовчазний
   SKIP читається в гейті як покриття.
2. **З правильним каталогом case 5 дає FAIL** — з тієї ж причини, що й `--canned` тут: еталони
   ДПС мають позначку часу (CAdES-T), і UAPKI бракує сертифіката **TSP-сервера АЦСК**
   (`"expectedCerts":[{"entity":"TSP",…}]`). Сертифікат ПІДПИСУВАЧА при цьому вкладено
   (`statusSignature:"VALID"`, `validDigests:true`), але `status:"INDETERMINATE"` →
   `errorCode=4161 CERT_NOT_FOUND`. Зразок `запит_стану_РРО.json.signed` (без TSP) проходить.
3. **`native_host case 1/2` падають від залишкового кешу** провайдера в
   `%LOCALAPPDATA%\SimplyAddinConnect` (`rmrf` харнесу не змогла прибрати зайнятий файл).
   Лікування: видалити каталог перед прогоном.
