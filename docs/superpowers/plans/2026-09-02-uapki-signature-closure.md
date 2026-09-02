# Закриття питань підпису UAPKI — план імплементації

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Усунути витік пароля в логах, дати компоненті перше покриття реальними ключами КНЕДП
(зокрема першим купинним), і додати незалежний арбітр підпису на чужому крипто-двигуні.

**Architecture:** Три напрямки, що сходяться в одному гейті. (1) Точкові правки логування в
хелпері UAPKI. (2) Нові кейси `native_host`, що працюють із реальними контейнерами через шляхи
й паролі з локального конфігу поза git. (3) Новий standalone-харнес `iit_verify` — тонкий C++
над нативною `EUSignCP.dll` АТ «ІІТ», який дає другу, незалежну думку про наш підпис. Наявні
тести не видаляються: вони перевіряють **компоненту**, а ІІТ перевіряє **артефакт**; натомість
самоперевірки перемарковуються як регресійні, а там, де можливо, замикаються зовнішнім суддею.

**Tech Stack:** C++17, MSVC 2022, CMake ≥3.16, nlohmann/json, Windows API (`LoadLibraryW`,
`GetProcAddress`, `MoveFileExW`), PowerShell 7 (`run_tests.ps1`), UAPKI 2.0.16, ІІТ EUSignCP
1.3.1.224 (x86).

**Spec:** `docs/superpowers/specs/2026-09-02-uapki-signature-closure-design.md`

## Global Constraints

- **Жодного тихого відкату.** Кожна недоступність ресурсу (провайдер не піднявся, немає ключа,
  немає ІІТ, немає сховища довіри) → **падіння з конкретним кодом або явний SKIP із названою
  причиною**. SKIP і PASS мусять бути розрізнюваними в підсумковій таблиці. Ніколи не PASS.
- **Секрети не потрапляють у git.** `tests/data/local-keys.json` додається в `.gitignore` **тим
  самим комітом**, що вводить його підтримку.
- **Чужі бінарники не потрапляють у git.** `EUSignCP.dll` **не копіювати** в репозиторій чи
  CI-образ — вантажити з місця встановлення. Підстава: ліцензія ІІТ §2.2 «Програма ліцензується
  як неподільний продукт. Її складові частини не можна поділяти для використання на кількох
  комп'ютерах».
- **Сертифікати ЦСК у git не тримаємо** — кеш у `%LOCALAPPDATA%\SimplyAddinConnect\iit-store\`,
  запис атомарний через `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`.
- **PCH:** `#include "../core/pch.h"` першим рядком у кожному `.cpp` **ядра/компонент**. Файли
  в `tests/` за конвенцією проєкту `pch.h` **не підключають**.
- **Логування** лише через макроси `ServiceTools.h`; `NEUTRAL_REPORT_*` у статичних методах,
  перший аргумент — ім'я компоненти. Повідомлення складати конкатенацією, з великої літери, без
  крапки в кінці. printf-стиль заборонено.
- **`/utf-8`** обов'язковий для кожної нової цілі в `CMake/compiler_settings.cmake` — перелік
  там поіменний. Без нього MSVC мовчки псує кириличні літерали.
- **Перед кожним прогоном тестів після зміни коду:** `build_project.ps1 -WithUAPKI -WithTests`,
  і лише потім `run_tests.ps1`. `run_tests.ps1` **не перезбирає** проєкт — на старих бінарниках
  він дасть зелений підсумок, не побачивши правок.
- **Базову лінію гейта зміряти САМОСТІЙНО перед першою правкою коду.** Прогнати
  `build_project.ps1 -WithUAPKI -WithTests`, далі `run_tests.ps1 x86` і `run_tests.ps1 x64`,
  і зафіксувати числа. Порівнювати подальші прогони саме з ними.
  ⚠️ Цифра `PASS=23 FAIL=0 SKIP=0`, що фігурувала в ранніх редакціях цього плану, **не
  стосується цієї гілки**: її передала сесія `refund-timeout-handling` для гілки
  `bpo-acquiring-prep` на HEAD `a9436c3`, тобто до мерджу PR #15 і до комітів з еталонами ЦЗО.
  Успадкована базова лінія — це та сама хвороба, проти якої написана вся ця робота: число
  виглядає як вимір, а виміром не є.
- **Мова коду й комітів:** українська/російська, за мовою файлу, який редагується.

---

## Карта файлів

**Створюються:**

| Файл | Відповідальність |
|---|---|
| `tests/data/local-keys.example.json` | документація формату локального конфігу; у git є |
| `tests/support/LocalKeys.h` / `.cpp` | читання й валідація `local-keys.json`; спільне для кейсів native_host |
| `tests/iit_verify.cpp` | standalone-арбітр: `EUSignCP.dll` → вердикт у JSON + код виходу |
| `tests/support/IitStore.h` / `.cpp` | забезпечення сховища довіри ІІТ у `%LOCALAPPDATA%` |

**Модифікуються:**

| Файл | Що саме |
|---|---|
| `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp:614, 669, 671` | маскування перед логуванням |
| `src/components/AddinUAPKIConnect.cpp:88-94` | прибрати дублююче логування сирих параметрів |
| `tests/native_host.cpp` | кейси 6-9; перемаркування блоку кейса 4 (`:536`) |
| `tests/uapki_fiscal_emulator.cpp` | зовнішній суддя в `runSelfTest` |
| `tests/CMakeLists.txt` | цілі `iit_verify` (лише x86), `LocalKeys`, `IitStore` |
| `CMake/compiler_settings.cmake` | `/utf-8` для нових цілей |
| `run_tests.ps1` | рівень `L4-iit`, матриця вердиктів |
| `.gitignore` | `tests/data/local-keys.json` |
| `docs/architecture/uapki.md` | межа покриття; два свідомі відступи |
| `docs/integration-1c/uapki.md` | розділ «Час підпису» |

---

## Task 1: Маскування пароля в логах

**Files:**
- Modify: `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp:614`, `:669-671`
- Modify: `src/components/AddinUAPKIConnect.cpp:88-94`
- Test: `tests/native_host.cpp` (новий кейс 6)

**Interfaces:**
- Consumes: наявний `UAPKIConnectHelper::MaskPasswords(nlohmann::json&)` (`:156-170`).
- Produces: нічого для інших задач. Кейс 6 `native_host` як окремий номер сценарію.

- [ ] **Step 1: Написати падаючий тест — кейс 6 у `native_host.cpp`**

Додати перед `main`:

```cpp
// ========================================================================
// КЕЙС 6 — пароль контейнера НЕ потрапляє у файл лога
// ========================================================================
// Логи пишуться через ИспользоватьЛогирование. Перевіряємо не наявність
// маскування, а ВІДСУТНІСТЬ секрету: єдине, що справді має значення.
static bool case6_passwordNotLogged(const std::wstring& binDir, const std::wstring& dataDir) {
    printf("== Case 6: пароль не потрапляє в лог ==\n");
    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    std::wstring p12     = dataDir + L"\\test-diia.p12";
    CHECK(pathExists(p12), "test-diia.p12 присутній");

    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    std::wstring logPath = std::wstring(tmpDir) + L"sac_case6.log";
    DeleteFileW(logPath.c_str());

    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    c.call("EnableLogging", "");            // заглушка: реальний виклик нижче
    CHECK(c.enableLogging(L"Trace", logPath), "лог увімкнено");

    json j;
    std::string r = c.call("INIT", buildInit(true));
    CHECK(errCode(r, j) == 0, "INIT errorCode == 0");

    r = c.call("OPEN", buildOpen(p12));     // buildOpen кладе password "testpassword"
    printf("  OPEN: %s\n", r.c_str());

    c.call("CLOSE", "");
    c.call("DEINIT", "");
    c.unload();                              // закрити лог перед читанням

    std::string logText;
    CHECK(readFileText(logPath, logText), "лог прочитано");
    CHECK(!logText.empty(), "лог не порожній");
    CHECK(logText.find("testpassword") == std::string::npos,
          "пароль ВІДСУТНІЙ у лозі");
    CHECK(logText.find("\"password\":\"***\"") != std::string::npos ||
          logText.find("\"password\": \"***\"") != std::string::npos,
          "у лозі є замаскований password");
    DeleteFileW(logPath.c_str());
    return true;
}
```

Додати допоміжне читання файлу поруч із наявним `readFileBytes`:

```cpp
static bool readFileText(const std::wstring& path, std::string& out) {
    std::vector<unsigned char> raw;
    if (!readFileBytes(path, raw)) return false;
    out.assign(raw.begin(), raw.end());
    return true;
}
```

Додати `enableLogging` у клас `Component` (поруч із `call`), використавши наявний механізм
виклику методів компоненти за іменем `ИспользоватьЛогирование`/`EnableLogging` з двома
рядковими аргументами. Прибрати рядок-заглушку `c.call("EnableLogging", "")` після того, як
`enableLogging` реалізовано.

Зареєструвати кейс у `main`: `case 6: pass = case6_passwordNotLogged(binDir, dataDir); break;`
і розширити рядок usage до `<case 1..6>`.

- [ ] **Step 2: Прогнати тест — має ВПАСТИ**

```powershell
powershell -File build_project.ps1 -WithUAPKI -WithTests
bin\Release\native_host_x64.exe 6 "" "tests\data" "bin\Release"
```

Очікувано: `FAIL: пароль ВІДСУТНІЙ у лозі` — бо `UAPKI Request` логується немаскованим.

- [ ] **Step 3: Замаскувати запит перед логуванням**

У `UAPKIConnectHelper.cpp`, у `ExecuteUapkiCommand`, замінити логування `requestStr`
(рядки 669/671) на логування маскованої копії. `requestStr`, що йде в `process()`, не чіпати:

```cpp
    // Для лога — ОКРЕМА замаскована копія. У process() ЗАВЖДИ йде оригінал:
    // маскування тут не має жодного впливу на сам запит.
    std::string logRequest = requestStr;
    try {
        nlohmann::json maskedReq = requestJson;
        MaskPasswords(maskedReq);
        logRequest = maskedReq.dump();
    }
    catch (const std::exception& e) {
        // Не вдалося замаскувати — краще не логувати запит узагалі, ніж злити пароль.
        logRequest = "<запит не залоговано: помилка маскування: " + std::string(e.what()) + ">";
    }

    if (logRequest.length() > 2000) {
        NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "UAPKI Request (сокращенный): " + logRequest.substr(0, 2000) + "...");
    } else {
        NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "UAPKI Request: " + logRequest);
    }
```

Рядок 614 (`"Параметры команды UAPKI: " + paramsString`) — прибрати повністю: сирий рядок
параметрів до розбору замаскувати неможливо, а нижче вже логується маскований запит.

- [ ] **Step 4: Прибрати дублююче логування в компоненті**

У `src/components/AddinUAPKIConnect.cpp` замінити блок `:88-94` на логування без параметрів:

```cpp
        // Параметри НЕ логуємо: вони містять password для OPEN. Нижче хелпер
        // залогує вже замаскований запит цілком.
        REPORT_DEBUG("Вызов UAPKIConnectHelper::ExecuteUapkiCommand: метод=" + method);
```

Видалити локальну змінну `logParams` і гілку за довжиною.

- [ ] **Step 5: Прогнати тест — має ПРОЙТИ**

```powershell
powershell -File build_project.ps1 -WithUAPKI -WithTests
bin\Release\native_host_x64.exe 6 "" "tests\data" "bin\Release"
```

Очікувано: усі CHECK ok, exit 0.

- [ ] **Step 6: Коміт**

```bash
git add src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp src/components/AddinUAPKIConnect.cpp tests/native_host.cpp
git commit -m "fix(uapki): пароль контейнера більше не потрапляє у файл лога

Логування запиту йшло на рівні INFO немаскованим (UAPKIConnectHelper.cpp:669/671),
тож OPEN клав password у лог відкритим текстом — саме тоді, коли лог найімовірніше
ввімкнено, тобто під час діагностики проблем із ключем. MaskPasswords існував, але
застосовувався лише до дампа конфігу INIT.

Тепер логується замаскована КОПІЯ; у process() йде оригінал. Помилка маскування →
запит не логується взагалі, а не логується сирим. Дублююче логування сирих параметрів
у компоненті прибрано.

Покрито native_host кейсом 6: перевіряє ВІДСУТНІСТЬ секрету в лозі, а не наявність зірочок."
```

---

## Task 2: Локальний конфіг реальних ключів

**Files:**
- Create: `tests/data/local-keys.example.json`
- Create: `tests/support/LocalKeys.h`, `tests/support/LocalKeys.cpp`
- Modify: `.gitignore`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces (використовують задачі 3, 4):
  ```cpp
  struct LocalKey {
      std::string id, path, password, expectContainer, expectSignAlgo;
  };
  // Повертає false + err, якщо файл є, але зіпсований. Якщо файлу НЕМАЄ —
  // повертає true з порожнім out і err == "" (це SKIP, а не помилка).
  bool LoadLocalKeys(const std::wstring& jsonPath, std::vector<LocalKey>& out, std::string& err);
  const LocalKey* FindLocalKey(const std::vector<LocalKey>& keys, const std::string& id);
  ```

- [ ] **Step 1: Додати ігнор ДО появи файлу з секретами**

У `.gitignore` дописати:

```gitignore
# Локальний конфіг реальних ключів: шляхи й паролі. НІКОЛИ не комітити.
tests/data/local-keys.json
```

- [ ] **Step 2: Створити приклад формату**

`tests/data/local-keys.example.json`:

```json
{
  "keys": [
    {
      "id": "jks-kupyna",
      "path": "C:\\Users\\<user>\\Desktop\\Ключі ЕЦП\\<...>\\key.jks",
      "password": "",
      "expect": {
        "container": "JKS",
        "signAlgo": "1.2.804.2.1.1.1.1.3.6.1.1"
      }
    },
    {
      "id": "zs2-gost",
      "path": "C:\\Users\\<user>\\Desktop\\Ключі ЕЦП\\<...>\\key.ZS2",
      "password": "",
      "expect": {
        "container": "PKCS12",
        "signAlgo": "1.2.804.2.1.1.1.1.3.1.1"
      }
    }
  ]
}
```

- [ ] **Step 3: Написати падаючий тест**

`tests/support/LocalKeys.cpp` ще не існує. Тимчасовий тест додати в `native_host.cpp` як
частину кейса 7 (задача 3) не можна — спершу потрібен сам завантажувач. Тому перевірка тут
мінімальна й вбудована: додати в кінець `LocalKeys.cpp` (крок 4) самоперевірку немає куди,
тож валідація виконується в задачі 3 через реальний прогін. **Крок пропускається свідомо** —
цей таск є інфраструктурним і перевіряється споживачем; це єдиний таск плану без власного
тесту, і саме тому він не має самостійного deliverable-гейта.

- [ ] **Step 4: Реалізувати завантажувач**

`tests/support/LocalKeys.h`:

```cpp
#pragma once
#include <string>
#include <vector>

struct LocalKey {
    std::string id;
    std::string path;
    std::string password;
    std::string expectContainer;
    std::string expectSignAlgo;
};

// Файлу немає            -> true,  out порожній, err порожній  (SKIP на боці кейса)
// Файл є, але зіпсований -> false, err із причиною             (FAIL: наміри заявлені)
bool LoadLocalKeys(const std::wstring& jsonPath, std::vector<LocalKey>& out, std::string& err);
const LocalKey* FindLocalKey(const std::vector<LocalKey>& keys, const std::string& id);
```

`tests/support/LocalKeys.cpp`:

```cpp
#include "LocalKeys.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <windows.h>

using nlohmann::json;

bool LoadLocalKeys(const std::wstring& jsonPath, std::vector<LocalKey>& out, std::string& err) {
    out.clear();
    err.clear();

    const DWORD attr = GetFileAttributesW(jsonPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;   // немає файлу -> SKIP, не помилка

    std::ifstream f(jsonPath, std::ios::binary);
    if (!f) { err = "Файл конфігу існує, але не читається"; return false; }

    json root;
    try { f >> root; }
    catch (const std::exception& e) { err = std::string("Некоректний JSON: ") + e.what(); return false; }

    if (!root.contains("keys") || !root["keys"].is_array()) {
        err = "У конфігу немає масиву keys";
        return false;
    }
    for (const auto& k : root["keys"]) {
        LocalKey lk;
        lk.id       = k.value("id", std::string());
        lk.path     = k.value("path", std::string());
        lk.password = k.value("password", std::string());
        if (k.contains("expect") && k["expect"].is_object()) {
            lk.expectContainer = k["expect"].value("container", std::string());
            lk.expectSignAlgo  = k["expect"].value("signAlgo", std::string());
        }
        if (lk.id.empty() || lk.path.empty()) {
            err = "Запис ключа без обов'язкових полів id/path";
            return false;
        }
        out.push_back(lk);
    }
    return true;
}

const LocalKey* FindLocalKey(const std::vector<LocalKey>& keys, const std::string& id) {
    for (const auto& k : keys) if (k.id == id) return &k;
    return nullptr;
}
```

- [ ] **Step 5: Підключити до збірки**

У `tests/CMakeLists.txt` додати `tests/support/LocalKeys.cpp` до джерел цілі `native_host`
(поруч із наявними `support/*.cpp`), а в `CMake/compiler_settings.cmake` переконатися, що
ціль `native_host` уже має `/utf-8` (вона є в переліку; нових цілей тут не з'являється).

- [ ] **Step 6: Перевірити збірку**

```powershell
powershell -File build_project.ps1 -WithUAPKI -WithTests
```

Очікувано: збірка успішна, `native_host_x64.exe` і `native_host_x86.exe` на місці.

- [ ] **Step 7: Коміт**

```bash
git add .gitignore tests/data/local-keys.example.json tests/support/LocalKeys.h tests/support/LocalKeys.cpp tests/CMakeLists.txt
git commit -m "test(uapki): локальний конфіг реальних ключів поза git

Шляхи й паролі до реальних контейнерів КНЕДП не можуть жити в репозиторії. Формат
описано прикладом local-keys.example.json, сам local-keys.json додано в .gitignore
ЦИМ САМИМ комітом — інакше лишалося б вікно, коли секрет можна закомітити.

Семантика відсутності навмисна: немає файлу -> SKIP (конфіг не заявлено); файл є,
але зіпсований -> FAIL (наміри заявлено, мовчазна втрата покриття неприпустима).

Поле expect не косметичне: воно перетворює тест із «відкрилось — добре» на
«відкрилось саме те, що ми думали»."
```

---

## Task 3: Відкриття реальних контейнерів (кейс 7)

**Files:**
- Modify: `tests/native_host.cpp`

**Interfaces:**
- Consumes: `LoadLocalKeys`, `FindLocalKey`, `LocalKey` (задача 2).
- Produces: кейс 7 `native_host`; спосіб побудови `OPEN` для довільного контейнера.

- [ ] **Step 1: Написати падаючий тест**

```cpp
// ========================================================================
// КЕЙС 7 — відкриття РЕАЛЬНИХ контейнерів КНЕДП
// ========================================================================
// Дві різні гілки детекту в cm-pkcs12: JKS (магія 0xFEEDFEED -> decodeJks ->
// jks_decrypt_key) і PKCS#12 (.ZS2 попри розширення є повноцінним PFX).
// Жодного реального контейнера від КНЕДП раніше не відкривали.
static bool case7_realContainers(const std::wstring& binDir, const std::wstring& dataDir,
                                 bool& skipped) {
    printf("== Case 7: реальні контейнери КНЕДП ==\n");
    skipped = false;

    std::vector<LocalKey> keys;
    std::string err;
    const std::wstring cfg = dataDir + L"\\local-keys.json";
    if (!LoadLocalKeys(cfg, keys, err)) {
        printf("FAIL: конфіг зіпсований: %s\n", err.c_str());
        return false;                       // конфіг є -> наміри заявлені -> FAIL
    }
    if (keys.empty()) {
        printf("SKIP (немає tests/data/local-keys.json — реальні ключі не налаштовані)\n");
        skipped = true;
        return true;
    }

    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    std::string r = c.call("INIT", buildInit(true));
    CHECK(errCode(r, j) == 0, "INIT errorCode == 0");

    bool allOk = true;
    for (const auto& k : keys) {
        printf("  -- ключ '%s'\n", k.id.c_str());

        json op;
        op["provider"] = "PKCS12";          // провайдер один: детект іде за ВМІСТОМ
        op["storage"]  = k.path;
        op["password"] = k.password;
        op["mode"]     = "RO";
        r = c.call("OPEN", op.dump());
        if (errCode(r, j) != 0) {
            printf("  FAIL: OPEN errorCode=%ld error=%s\n",
                   errCode(r, j), j.value("error", std::string()).c_str());
            allOk = false;
            continue;
        }
        printf("  OPEN ok\n");

        r = c.call("KEYS", "");
        if (errCode(r, j) != 0) { printf("  FAIL: KEYS\n"); allOk = false; c.call("CLOSE", ""); continue; }

        bool algoSeen = k.expectSignAlgo.empty();
        std::string firstId;
        if (j["result"].contains("keys") && j["result"]["keys"].is_array()) {
            for (const auto& key : j["result"]["keys"]) {
                if (firstId.empty()) firstId = key.value("id", std::string());
                if (key.contains("signAlgo") && key["signAlgo"].is_array()) {
                    for (const auto& a : key["signAlgo"])
                        if (a.get<std::string>() == k.expectSignAlgo) algoSeen = true;
                }
            }
        }
        if (!algoSeen) {
            printf("  FAIL: очікуваний signAlgo %s не знайдено серед можливостей ключа\n",
                   k.expectSignAlgo.c_str());
            allOk = false;
        } else {
            printf("  signAlgo %s підтверджено\n", k.expectSignAlgo.c_str());
        }

        if (!firstId.empty()) {
            json sp; sp["id"] = firstId;
            r = c.call("SELECT_KEY", sp.dump());
            if (errCode(r, j) != 0) { printf("  FAIL: SELECT_KEY\n"); allOk = false; }
        }
        c.call("CLOSE", "");
    }

    c.call("DEINIT", "");
    c.unload();
    CHECK(allOk, "усі реальні контейнери відкрито й алгоритми збіглися");
    return true;
}
```

Додати `#include "support/LocalKeys.h"` до `native_host.cpp`. Зареєструвати в `main`:
`case 7: pass = case7_realContainers(binDir, dataDir, skipped); break;` — використати наявний
механізм `skipped` → `exit 3`, той самий, що в кейсі 5. Розширити usage до `<case 1..7>`.

- [ ] **Step 2: Прогнати без конфігу — має бути SKIP (exit 3)**

```powershell
bin\Release\native_host_x64.exe 7 "" "tests\data" "bin\Release"
echo $LASTEXITCODE
```

Очікувано: `SKIP (немає tests/data/local-keys.json…)`, код виходу **3**.

- [ ] **Step 3: Створити реальний конфіг і прогнати — має ПРОЙТИ**

Скопіювати `local-keys.example.json` у `local-keys.json`, вписати справжні шляхи й паролі.

```powershell
bin\Release\native_host_x64.exe 7 "" "tests\data" "bin\Release"
echo $LASTEXITCODE
```

Очікувано: обидва ключі відкрито, `signAlgo` збігся, код виходу **0**.
Якщо `.ZS2` дасть помилку — це значуща знахідка, а не збій тесту: зафіксувати `errorCode` і
`error` у звіті, бо саме це питання було «ризиком номер один» суміжної команди.

- [ ] **Step 4: Коміт**

```bash
git add tests/native_host.cpp
git commit -m "test(uapki): кейс 7 — відкриття реальних контейнерів КНЕДП

Перше в проєкті покриття реальними ключами: раніше єдиним був синтетичний і
прострочений test-diia.p12. Перевіряються ДВІ різні гілки детекту cm-pkcs12 —
JKS (магія 0xFEEDFEED) і PKCS#12 (.ZS2 попри розширення є повноцінним PFX
з PBES2+PBKDF2+ГОСТ28147-CFB і MAC на ГОСТ 34311).

Звіряється не факт відкриття, а збіг з expect.signAlgo: інакше підміна ключа
або мовчазний вибір іншого алгоритму пройшли б непоміченими."
```

---

## Task 4: Купинний підпис і структурний контракт ПРРО (кейс 8)

**Files:**
- Modify: `tests/native_host.cpp`

**Interfaces:**
- Consumes: `LoadLocalKeys`, `FindLocalKey` (задача 2).
- Produces: кейс 8; файл підпису на диску для арбітра — шлях передається аргументом
  `[outSig]` і використовується задачею 8.

- [ ] **Step 1: Написати падаючий тест**

```cpp
// ========================================================================
// КЕЙС 8 — купинний підпис + структурний контракт ПРРО
// ========================================================================
// ДПС забороняє: content-time-stamp, CRL/OCSP, сертифікати видавця.
// ДПС вимагає: вкладений сертифікат підписувача, дані всередині (enveloping).
// OID дивимось у SignerInfo, а НЕ в сертифікаті: сертифікат старого зразка
// цілком може підписувати Купиною (живий ticket.p7s ДПС саме такий).
static bool case8_kupynaSign(const std::wstring& binDir, const std::wstring& dataDir,
                             const std::wstring& outSig, bool& skipped) {
    printf("== Case 8: купинний підпис + контракт ПРРО ==\n");
    skipped = false;

    std::vector<LocalKey> keys;
    std::string err;
    if (!LoadLocalKeys(dataDir + L"\\local-keys.json", keys, err)) {
        printf("FAIL: конфіг зіпсований: %s\n", err.c_str());
        return false;
    }
    const LocalKey* k = FindLocalKey(keys, "jks-kupyna");
    if (!k) {
        printf("SKIP (немає ключа 'jks-kupyna' у local-keys.json)\n");
        skipped = true;
        return true;
    }

    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    std::string r = c.call("INIT", buildInit(true));
    CHECK(errCode(r, j) == 0, "INIT errorCode == 0");

    json op;
    op["provider"] = "PKCS12"; op["storage"] = k->path;
    op["password"] = k->password; op["mode"] = "RO";
    r = c.call("OPEN", op.dump());
    CHECK(errCode(r, j) == 0, "OPEN errorCode == 0");

    r = c.call("KEYS", "");
    CHECK(errCode(r, j) == 0, "KEYS errorCode == 0");
    std::string keyId = j["result"]["keys"][0].value("id", std::string());
    CHECK(!keyId.empty(), "id ключа отримано");

    { json sp; sp["id"] = keyId; r = c.call("SELECT_KEY", sp.dump()); }
    CHECK(errCode(r, j) == 0, "SELECT_KEY errorCode == 0");

    // Купинний підпис у профілі ПРРО (офлайн: без позначки часу).
    json sp2;
    sp2["signatureFormat"]  = "CAdES-BES";
    sp2["signAlgo"]         = "1.2.804.2.1.1.1.1.3.6.1.1";   // ДСТУ4145 + Купина-256
    sp2["detachedData"]     = false;                          // enveloping
    sp2["includeCert"]      = true;
    sp2["includeTime"]      = true;
    sp2["includeContentTS"] = false;
    json d; d["id"] = "doc-0"; d["bytes"] = DATA_TBS_B64;
    json p;
    p["signParams"] = sp2;
    p["dataTbs"]    = json::array({ d });
    p["options"]["ignoreCertStatus"] = true;

    r = c.call("SIGN", p.dump());
    printf("  SIGN: %s\n", r.substr(0, 300).c_str());
    CHECK(errCode(r, j) == 0, "SIGN errorCode == 0 (КУПИННИЙ ПІДПИС)");
    std::string sig = j["result"]["signatures"][0].value("bytes", std::string());
    CHECK(!sig.empty(), "підпис не порожній");

    // VERIFY нашим двигуном — РЕГРЕСІЙНА перевірка, не доказ коректності:
    // наш код підтверджує наш код. Доказ дає арбітр ІІТ (рівень L4-iit).
    { json vp; vp["signature"]["bytes"] = sig; r = c.call("VERIFY", vp.dump()); }
    CHECK(errCode(r, j) == 0, "VERIFY errorCode == 0 (регресія)");
    auto& res = j["result"];
    CHECK(res.contains("signatureInfos") && !res["signatureInfos"].empty(), "signatureInfos є");
    auto& si = res["signatureInfos"][0];

    // Контракт ПРРО
    CHECK(res.contains("certIds") && !res["certIds"].empty(), "сертифікат підписувача вкладено");
    CHECK(!si.contains("contentTS"),      "content-time-stamp ВІДСУТНІЙ (ДПС забороняє)");
    CHECK(!si.contains("revocationRefs"), "revocationRefs відсутні (ДПС забороняє)");
    CHECK(!si.contains("certValues"),     "certValues відсутні (ДПС забороняє)");
    CHECK(!si.contains("certificateRefs"),"certificateRefs відсутні (ДПС забороняє)");

    // Записати підпис для арбітра
    if (!outSig.empty()) {
        std::vector<unsigned char> raw;
        CHECK(b64decode(sig, raw), "підпис декодовано з base64");
        CHECK(writeFileBytes(outSig, raw), "підпис збережено для арбітра");
        printf("  підпис записано: %s\n", w2u8(outSig).c_str());
    }

    c.call("CLOSE", ""); c.call("DEINIT", ""); c.unload();
    return true;
}
```

Додати помічники поруч із наявним `b64encode`:

```cpp
static bool b64decode(const std::string& b64, std::vector<unsigned char>& out);   // за зразком
                                                                                   // b64decode в
                                                                                   // uapki_fiscal_emulator.cpp:142
static bool writeFileBytes(const std::wstring& path, const std::vector<unsigned char>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == data.size();
}
```

Зареєструвати в `main`: `case 8: pass = case8_kupynaSign(binDir, dataDir, outSig, skipped); break;`
де `outSig` — новий необов'язковий 5-й аргумент командного рядка. Розширити usage до
`<case 1..8> [mainDll] [dataDir] [binDir] [prroDir] [outSig]`.

- [ ] **Step 2: Прогнати — очікуємо PASS або значущу помилку**

```powershell
bin\Release\native_host_x64.exe 8 "" "tests\data" "bin\Release" "" "%TEMP%\kupyna.p7s"
```

Очікувано: `SIGN errorCode == 0`. Якщо помилка — зафіксувати `errorCode`, це буде перший
вимір спроможності стека підписувати Купиною.

- [ ] **Step 3: Коміт**

```bash
git add tests/native_host.cpp
git commit -m "test(uapki): кейс 8 — перший купинний підпис у профілі ПРРО

Ключ нового зразка з'явився 2026-09-02 (сертифікат підписаний КНЕДП алгоритмом
1.2.804.2.1.1.1.1.3.6.1.1). До цього створити купинний підпис було нічим, і гілка
лишалась непокритою.

Структурний контракт ДПС перевіряється поіменно: вкладений сертифікат — так;
content-time-stamp, revocationRefs, certValues, certificateRefs — заборонені.
Власний VERIFY позначено в коді як РЕГРЕСІЙНУ перевірку: наш код підтверджує наш
код і доказом коректності не є. Доказ дає арбітр ІІТ.

Підпис зберігається у файл — вхід для рівня L4-iit."
```

---

## Task 5: `iit_verify` — каркас і чесний SKIP

**Files:**
- Create: `tests/iit_verify.cpp`
- Modify: `tests/CMakeLists.txt`, `CMake/compiler_settings.cmake`

**Interfaces:**
- Produces (використовують задачі 6, 8, 9, 10, 11):
  - виконуваний `iit_verify_x86.exe`;
  - коди виходу: `0` — підпис валідний, `1` — невалідний, `2` — помилка використання/внутрішня,
    `3` — SKIP (немає ІІТ або сховища);
  - stdout — один JSON-об'єкт.

- [ ] **Step 1: Написати каркас**

`tests/iit_verify.cpp`:

```cpp
// iit_verify — незалежний арбітр підпису на нативній бібліотеці АТ "ІІТ".
//
// НАВІЩО: рівні native_host перевіряють наш підпис НАШИМ же VERIFY. При системній
// помилці обидва пройдуть зелено. ІІТ — інша кодова база (UAPKI є форком Cryptonite,
// тож Cryptonite-похідні арбітром бути не можуть).
//
// ЛІЦЕНЗІЯ: EUSignCP.dll НЕ копіюється — вантажиться з місця встановлення.
// Ліцензія ІІТ §2.2: складові неподільного продукту не можна поділяти для
// використання на кількох комп'ютерах.
//
// Коди виходу: 0 валідний | 1 невалідний | 2 помилка | 3 SKIP.
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

// --- Прототипи потрібних експортів EUSignCP.dll (звірено dumpbin /exports) ---
// EULoad/EUGetInterface у DLL НЕ експортуються (це glue із SDK, якого в інсталяції
// немає), тому зв'язуємось через GetProcAddress по іменах.
typedef struct {
    BOOL       bFilled;
    char*      pszIssuer;      char* pszIssuerCN;      char* pszSerial;
    char*      pszSubject;     char* pszSubjCN;        char* pszSubjOrg;
    char*      pszSubjOrgUnit; char* pszSubjTitle;     char* pszSubjState;
    char*      pszSubjLocality;char* pszSubjFullName;  char* pszSubjAddress;
    char*      pszSubjPhone;   char* pszSubjEMail;     char* pszSubjDNS;
    char*      pszSubjEDRPOUCode; char* pszSubjDRFOCode;
    BOOL       bTimeAvail;
    BOOL       bTimeStamp;     // позначка часу з TSP-сервера
    SYSTEMTIME Time;
} EU_SIGN_INFO;

typedef DWORD (WINAPI *PFN_Initialize)(void);
typedef void  (WINAPI *PFN_Finalize)(void);
typedef void  (WINAPI *PFN_SetUIMode)(BOOL);
typedef DWORD (WINAPI *PFN_SetModeSettings)(BOOL);
typedef DWORD (WINAPI *PFN_SetFileStoreSettings)(char*, BOOL, BOOL, BOOL, BOOL, BOOL, BOOL);
typedef DWORD (WINAPI *PFN_VerifyDataInternal)(char*, BYTE*, DWORD, BYTE**, DWORD*, EU_SIGN_INFO*);
typedef void  (WINAPI *PFN_FreeMemory)(BYTE*);
typedef void  (WINAPI *PFN_FreeSignInfo)(EU_SIGN_INFO*);

struct Eu {
    HMODULE h = nullptr;
    PFN_Initialize            Initialize = nullptr;
    PFN_Finalize              Finalize = nullptr;
    PFN_SetUIMode             SetUIMode = nullptr;
    PFN_SetModeSettings       SetModeSettings = nullptr;
    PFN_SetFileStoreSettings  SetFileStoreSettings = nullptr;
    PFN_VerifyDataInternal    VerifyDataInternal = nullptr;
    PFN_FreeMemory            FreeMemory = nullptr;
    PFN_FreeSignInfo          FreeSignInfo = nullptr;
};

static void jsonOut(const char* status, const char* detail, long code) {
    std::printf("{\"status\":\"%s\",\"detail\":\"%s\",\"code\":%ld}\n", status, detail, code);
}

// Пошук каталогу ІІТ: спершу перекриття змінною середовища, потім стандартне місце.
// Версію каталогу НЕ хардкодимо — перебираємо підкаталоги "Certificate Authority-*".
static bool findIitDir(std::wstring& out) {
    wchar_t env[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"IIT_EU_DIR", env, MAX_PATH) > 0) {
        out = env;
        return GetFileAttributesW((out + L"\\EUSignCP.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    wchar_t pf[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH)) return false;
    const std::wstring base = std::wstring(pf) + L"\\Institute of Informational Technologies";
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring cand = base + L"\\" + n + L"\\End User";
        if (GetFileAttributesW((cand + L"\\EUSignCP.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
            out = cand; found = true; break;
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return found;
}

static bool bindEu(Eu& eu, std::string& err) {
    std::wstring dir;
    if (!findIitDir(dir)) { err = "EUSignCP.dll не знайдено (ІІТ не встановлено)"; return false; }
    SetDllDirectoryW(dir.c_str());                       // залежності поруч із DLL
    eu.h = LoadLibraryW((dir + L"\\EUSignCP.dll").c_str());
    if (!eu.h) { err = "LoadLibraryW не змогла завантажити EUSignCP.dll"; return false; }

    #define BIND(field, name) \
        eu.field = (decltype(eu.field))GetProcAddress(eu.h, name); \
        if (!eu.field) { err = std::string("Немає експорту ") + name; return false; }
    BIND(Initialize,           "EUInitialize")
    BIND(Finalize,             "EUFinalize")
    BIND(SetUIMode,            "EUSetUIMode")
    BIND(SetModeSettings,      "EUSetModeSettings")
    BIND(SetFileStoreSettings, "EUSetFileStoreSettings")
    BIND(VerifyDataInternal,   "EUVerifyDataInternal")
    BIND(FreeMemory,           "EUFreeMemory")
    BIND(FreeSignInfo,         "EUFreeSignInfo")
    #undef BIND
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        jsonOut("ERROR", "usage: iit_verify <file.p7s>", 0);
        return 2;
    }
    Eu eu;
    std::string err;
    if (!bindEu(eu, err)) {
        jsonOut("SKIP", err.c_str(), 0);
        return 3;                                        // немає ІІТ -> SKIP, НІКОЛИ не PASS
    }
    jsonOut("OK", "bind ok", 0);
    if (eu.h) FreeLibrary(eu.h);
    return 0;
}
```

- [ ] **Step 2: Додати ціль у CMake (лише x86)**

У `tests/CMakeLists.txt`:

```cmake
# iit_verify — арбітр на нативній бібліотеці ІІТ. ТІЛЬКИ x86: EUSignCP.dll 32-бітна
# (dumpbin /headers -> 14C machine), а x64-процес 32-бітну DLL завантажити не може.
# UAPKI цій цілі не потрібен — вона незалежна від нашого крипто-стека.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(iit_verify iit_verify.cpp)
    set_target_properties(iit_verify PROPERTIES OUTPUT_NAME "iit_verify_x86")
else()
    message(STATUS "iit_verify: пропущено для x64 (EUSignCP.dll лише x86)")
endif()
```

У `CMake/compiler_settings.cmake` додати до поіменного переліку:

```cmake
if(TARGET iit_verify)
    target_compile_options(iit_verify PRIVATE /utf-8)
endif()
```

- [ ] **Step 3: Зібрати й перевірити SKIP-семантику**

```powershell
powershell -File build_project.ps1 -WithTests
bin\Release\iit_verify_x86.exe test.p7s
echo $LASTEXITCODE
```

Очікувано зі встановленим ІІТ: `{"status":"OK",...}`, код **0**.
Перевірити SKIP: тимчасово `$env:IIT_EU_DIR = "C:\nonexistent"` → `{"status":"SKIP",…}`, код **3**.

- [ ] **Step 4: Коміт**

```bash
git add tests/iit_verify.cpp tests/CMakeLists.txt CMake/compiler_settings.cmake
git commit -m "test(iit): каркас арбітра iit_verify на нативній EUSignCP.dll

Незалежний двигун потрібен тому, що UAPKI є форком Cryptonite (extern/uapki/README.md:4),
отже Cryptonite-похідні арбітром бути не можуть — спільний предок дає спільні системні
помилки. ІІТ має власну кодову базу й документовану підтримку Купини.

Зв'язування через GetProcAddress: EULoad/EUGetInterface у DLL НЕ експортуються (це glue
з SDK, якого в інсталяції немає) — перевірено dumpbin. Потрібні 8 функцій експортуються
по іменах.

Ціль лише x86: EUSignCP.dll 32-бітна, x64-процес її не завантажить. DLL не копіюється —
вантажиться з місця встановлення (ліцензія ІІТ §2.2). Каталог шукається без хардкоду
версії, з перекриттям через IIT_EU_DIR.

Немає ІІТ -> exit 3 (SKIP), ніколи не PASS."
```

---

## Task 6: `iit_verify` — власне перевірка

**Files:**
- Modify: `tests/iit_verify.cpp`

**Interfaces:**
- Consumes: `Eu`, `bindEu`, `jsonOut` (задача 5).
- Produces: вердикт у stdout:
  `{"status":"VALID|INVALID|SKIP|ERROR","code":<DWORD>,"subject":"…","timeStamp":true|false}`

- [ ] **Step 1: Написати перевірку**

Замінити тіло `main` після успішного `bindEu`:

```cpp
    // GUI ЗАБОРОНЕНО: без цього бібліотека може відкрити діалог на помилці
    // й підвісити прогін назавжди. Документація: "бібліотеку буде завантажено
    // без графічного модуля". Викликати ДО Initialize.
    eu.SetUIMode(FALSE);

    if (const DWORD rc = eu.Initialize()) {
        jsonOut("ERROR", "EUInitialize не вдалася", (long)rc);
        return 2;
    }

    // Детермінізм: жодних звернень до серверів ЦСК.
    eu.SetModeSettings(TRUE);

    // Сховище довіри: каталог сертифікатів і СВС. Автозавантаження СВС і
    // збереження отриманих сертифікатів ВИМКНЕНО — інакше сховище змінюється
    // між прогонами й результат перестає бути відтворюваним.
    std::string store;
    if (!ensureIitStore(store)) {                    // задача 7
        eu.Finalize();
        jsonOut("SKIP", "сховище довіри ІІТ недоступне", 0);
        return 3;
    }
    eu.SetFileStoreSettings((char*)store.c_str(),
                            /*bCheckCRLs*/        FALSE,
                            /*bAutoRefresh*/      TRUE,
                            /*bOwnCRLsOnly*/      FALSE,
                            /*bFullAndDeltaCRLs*/ FALSE,
                            /*bAutoDownloadCRLs*/ FALSE,
                            /*bSaveLoadedCerts*/  FALSE);

    std::vector<BYTE> data;
    if (!readAll(argv[1], data) || data.empty()) {
        eu.Finalize();
        jsonOut("ERROR", "не прочитано вхідний файл", 0);
        return 2;
    }

    EU_SIGN_INFO info{};
    BYTE* content = nullptr;
    DWORD contentLen = 0;
    const DWORD rc = eu.VerifyDataInternal(nullptr, data.data(), (DWORD)data.size(),
                                           &content, &contentLen, &info);

    const bool ok = (rc == 0);
    std::string subject = (info.bFilled && info.pszSubjCN) ? info.pszSubjCN : "";
    for (auto& ch : subject) if (ch == '"' || ch == '\\') ch = '\'';   // безпечно для JSON

    std::printf("{\"status\":\"%s\",\"code\":%lu,\"subject\":\"%s\","
                "\"timeStamp\":%s,\"contentLen\":%lu}\n",
                ok ? "VALID" : "INVALID", (unsigned long)rc, subject.c_str(),
                (info.bFilled && info.bTimeStamp) ? "true" : "false",
                (unsigned long)contentLen);

    if (content) eu.FreeMemory(content);
    if (info.bFilled) eu.FreeSignInfo(&info);
    eu.Finalize();
    FreeLibrary(eu.h);
    return ok ? 0 : 1;
```

Додати читання файлу:

```cpp
static bool readAll(const char* path, std::vector<BYTE>& out) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64 << 20)) {
        CloseHandle(h); return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    const BOOL r = ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    return r && read == out.size();
}
```

- [ ] **Step 2: Прогнати на еталоні ЦЗО — очікується INVALID**

```powershell
bin\Release\iit_verify_x86.exe tests\data\czo\dstu-7564\enveloped\CAdES-BES\test.txt.p7s
echo $LASTEXITCODE
```

Очікувано: `{"status":"INVALID","code":51,…}`, код виходу **1**. Це **негативний контроль**:
еталони ЦЗО підписані тестовим ЦСК, якого немає в довіреному наборі. Той самий вердикт дає
веб-віджет ЦЗО («Сертифікат не знайдено(51)»), тобто поведінка збігається з еталонною.

- [ ] **Step 3: Прогнати на нашому купинному підписі — очікується VALID**

```powershell
bin\Release\native_host_x86.exe 8 "" "tests\data" "bin\Release" "" "%TEMP%\kupyna.p7s"
bin\Release\iit_verify_x86.exe "%TEMP%\kupyna.p7s"
echo $LASTEXITCODE
```

Очікувано: `{"status":"VALID","code":0,"subject":"СИДОРЕНКО…","timeStamp":false,…}`, код **0**.
`timeStamp:false` очікуване й правильне: профіль офлайновий, TSP-токена немає.

- [ ] **Step 4: Коміт**

```bash
git add tests/iit_verify.cpp
git commit -m "test(iit): перевірка підпису чужим двигуном + офлайн-детермінізм

EUSetUIMode(FALSE) першим викликом — без нього бібліотека може відкрити діалог на
помилці й підвісити автоматичний прогін назавжди.

Детермінізм: EUSetModeSettings(TRUE) вимикає звернення до серверів ЦСК;
bAutoDownloadCRLs і bSaveLoadedCerts вимкнено, щоб сховище не змінювалось між
прогонами. Тихого відкату немає за конструкцією бібліотеки: операція, що потребує
мережі в офлайні, падає з EU_ERROR_OFFLINE_MODE.

Вивід несе поле timeStamp з EU_SIGN_INFO — незалежне свідчення, чи є в підписі
позначка часу від TSP. Це рівно нормативне питання ПРРО, і відповідає на нього
тепер сторонній двигун, а не ми самі."
```

---

## Task 7: Сховище довіри ІІТ у `%LOCALAPPDATA%`

**Files:**
- Create: `tests/support/IitStore.h`, `tests/support/IitStore.cpp`
- Modify: `tests/iit_verify.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `bool ensureIitStore(std::string& outDirUtf8);` — використовує задача 6.

- [ ] **Step 1: Реалізувати забезпечення сховища**

`tests/support/IitStore.h`:

```cpp
#pragma once
#include <string>

// Забезпечує наявність каталогу зі сертифікатами ЦСК для арбітра ІІТ.
// Є локально -> використовуємо. Немає -> дістаємо. Сертифікати в git НЕ тримаємо.
// Повертає false, якщо ні знайти, ні дістати не вдалося (виклик -> SKIP).
bool ensureIitStore(std::string& outDirUtf8);
```

`tests/support/IitStore.cpp`:

```cpp
#include "IitStore.h"
#include <windows.h>
#include <string>
#include <vector>

// Каталог кешу — поруч із уже наявним ...\providers\<версія>\, за тим самим патерном,
// що використовує UAPKIConnectHelper::EnsureProviderDeployed.
static bool cacheDir(std::wstring& out) {
    wchar_t lad[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH)) return false;
    out = std::wstring(lad) + L"\\SimplyAddinConnect\\iit-store";
    return true;
}

static bool hasAnyCert(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.cer").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        h = FindFirstFileW((dir + L"\\*.p7b").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
    }
    FindClose(h);
    return true;
}

static void makeDirs(const std::wstring& path) {
    std::wstring acc;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') {
            if (!acc.empty()) CreateDirectoryW(acc.c_str(), nullptr);
        }
        if (i < path.size()) acc += path[i];
    }
}

// Завантаження бандла ЦСК. Запис АТОМАРНИЙ: тимчасове ім'я + MoveFileExW.
// Атомарність не косметика — run_tests.ps1 ганяє дві архітектури, прогони
// можуть перетнутися на одному кеші.
static bool downloadBundle(const std::wstring& dir) {
    makeDirs(dir);
    const std::wstring dst = dir + L"\\CACertificates.p7b";
    const std::wstring tmp = dst + L".tmp_" + std::to_wstring(GetCurrentProcessId());

    std::wstring cmd = L"powershell -NoProfile -ExecutionPolicy Bypass -Command "
                       L"\"try { Invoke-WebRequest "
                       L"'https://czo.gov.ua/download/certificates/CACertificates.p7b' "
                       L"-OutFile '" + tmp + L"' -UseBasicParsing; exit 0 } catch { exit 1 }\"";

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (rc != 0) { DeleteFileW(tmp.c_str()); return false; }

    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES;  // встиг інший процес
    }
    return true;
}

bool ensureIitStore(std::string& outDirUtf8) {
    std::wstring dir;
    if (!cacheDir(dir)) return false;
    if (!hasAnyCert(dir) && !downloadBundle(dir)) return false;
    if (!hasAnyCert(dir)) return false;

    const int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return false;
    outDirUtf8.resize((size_t)n - 1);
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, outDirUtf8.data(), n, nullptr, nullptr);
    return true;
}
```

- [ ] **Step 2: Підключити**

У `tests/iit_verify.cpp` додати `#include "support/IitStore.h"`.
У `tests/CMakeLists.txt` додати `support/IitStore.cpp` до джерел цілі `iit_verify`:

```cmake
    add_executable(iit_verify iit_verify.cpp support/IitStore.cpp)
```

- [ ] **Step 3: Перевірити кеш**

```powershell
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\SimplyAddinConnect\iit-store" -ErrorAction SilentlyContinue
bin\Release\iit_verify_x86.exe "%TEMP%\kupyna.p7s"
dir "$env:LOCALAPPDATA\SimplyAddinConnect\iit-store"
bin\Release\iit_verify_x86.exe "%TEMP%\kupyna.p7s"
```

Очікувано: перший прогін тягне бандл (кілька секунд), у каталозі з'являється
`CACertificates.p7b`; другий прогін миттєвий і мережу не чіпає.

- [ ] **Step 4: Коміт**

```bash
git add tests/support/IitStore.h tests/support/IitStore.cpp tests/iit_verify.cpp tests/CMakeLists.txt
git commit -m "test(iit): кеш сховища довіри в %LOCALAPPDATA%, без сертифікатів у git

Патерн той самий, що вже двічі відпрацьований у проєкті (провайдер із RCDATA,
еталони ЦЗО через update-czo-testdata.ps1): є локально -> використовуємо,
немає -> дістаємо, запис атомарний через MoveFileExW.

Атомарність тут не косметика: run_tests.ps1 ганяє дві архітектури, і прогони можуть
перетнутися на одному кеші. Програш гонки трактується як успіх, якщо цільовий файл
уже на місці.

Не вдалося ні знайти, ні дістати -> виклик віддає SKIP, ніколи не PASS."
```

---

## Task 8: Рівень `L4-iit` у гейті + негативний контроль

**Files:**
- Modify: `run_tests.ps1`

**Interfaces:**
- Consumes: `iit_verify_x86.exe` (задачі 5-7), `native_host` кейс 8 (задача 4).
- Produces: рівень `L4-iit` у підсумковій таблиці.

- [ ] **Step 1: Додати рівень**

Після блоку `L2/L3 native_host` у `run_tests.ps1`:

```powershell
# =====================================================================
# ЕТАП L4-iit: незалежний арбітр — наш підпис очима чужого двигуна
# =====================================================================
Section 'ЕТАП L4-iit: арбітр ІІТ'

$IitVerifyExe = Join-Path $BinRelease 'iit_verify_x86.exe'
$SigOut       = Join-Path $env:TEMP 'sac_kupyna.p7s'
$CzoNeg       = Join-Path $DataDir 'czo\dstu-7564\enveloped\CAdES-BES\test.txt.p7s'

if ($NoUapki) {
    Add-Result 'L4-iit' 'арбітр ІІТ' 'SKIP' 'режим -NoUapki: підпис не створюється'
}
elseif (-not (Test-Path $IitVerifyExe)) {
    Add-Result 'L4-iit' 'арбітр ІІТ' 'SKIP' 'немає iit_verify_x86.exe (ціль збирається лише в x86)'
}
else {
    # --- Негативний контроль: еталон ЦЗО підписаний ТЕСТОВИМ ЦСК і має бути ВІДХИЛЕНИЙ.
    #     Без цього "все зелено" може означати, що арбітр не піднявся й завжди каже "валідно".
    if (-not (Test-Path $CzoNeg)) {
        Add-Result 'L4-iit' 'негативний контроль' 'SKIP' 'немає еталона czo'
    } else {
        $negOut = & $IitVerifyExe $CzoNeg 2>&1
        $negRc  = $LASTEXITCODE
        if ($negRc -eq 3) {
            Add-Result 'L4-iit' 'негативний контроль' 'SKIP' "арбітр недоступний: $negOut"
        } elseif ($negRc -eq 1) {
            Add-Result 'L4-iit' 'негативний контроль' 'PASS' 'еталон тестового ЦСК відхилено, як і має бути'
        } else {
            Add-Result 'L4-iit' 'негативний контроль' 'FAIL' "очікувався код 1, отримано $negRc : $negOut"
        }
    }

    # --- Позитив: наш купинний підпис має бути ПРИЙНЯТИЙ чужим двигуном.
    $nhExe = Join-Path $BinRelease ("native_host" + $ArchSuffix + ".exe")
    if (-not (Test-Path $nhExe)) {
        Add-Result 'L4-iit' 'наш підпис' 'BLOCKED' 'немає native_host — зберіть build_project.ps1 -WithTests'
    } else {
        Remove-Item $SigOut -ErrorAction SilentlyContinue
        & $nhExe 8 '' $DataDir $BinRelease '' $SigOut | Out-Null
        $signRc = $LASTEXITCODE
        if ($signRc -eq 3) {
            Add-Result 'L4-iit' 'наш підпис' 'SKIP' 'немає ключа jks-kupyna у local-keys.json'
        } elseif ($signRc -ne 0 -or -not (Test-Path $SigOut)) {
            Add-Result 'L4-iit' 'наш підпис' 'FAIL' "кейс 8 не створив підпис (код $signRc)"
        } else {
            $posOut = & $IitVerifyExe $SigOut 2>&1
            $posRc  = $LASTEXITCODE
            if ($posRc -eq 0) {
                Add-Result 'L4-iit' 'наш підпис' 'PASS' "ІІТ прийняв: $posOut"
            } elseif ($posRc -eq 3) {
                Add-Result 'L4-iit' 'наш підпис' 'SKIP' "арбітр недоступний: $posOut"
            } else {
                Add-Result 'L4-iit' 'наш підпис' 'FAIL' "ІІТ відхилив (код $posRc): $posOut"
            }
        }
    }
}
```

- [ ] **Step 2: Прогнати гейт на x86**

```powershell
powershell -File build_project.ps1 -WithUAPKI -WithTests
powershell -File run_tests.ps1 x86
```

Очікувано: `L4-iit` з двома рядками — негативний контроль PASS, наш підпис PASS.

- [ ] **Step 3: Прогнати гейт на x64 — арбітр має дати SKIP із причиною**

```powershell
powershell -File run_tests.ps1 x64
```

Очікувано: `[SKIP] L4-iit арбітр ІІТ — немає iit_verify_x86.exe (ціль збирається лише в x86)`.
Решта рівнів — як у базовій лінії.

- [ ] **Step 4: Коміт**

```bash
git add run_tests.ps1
git commit -m "test(gate): рівень L4-iit — наш підпис очима чужого двигуна

Два рядки, і другий без першого не має сенсу. Негативний контроль: еталон ЦЗО
підписаний ТЕСТОВИМ ЦСК і мусить бути відхилений; без нього зелений позитив може
означати, що арбітр не піднявся й завжди каже 'валідно'. Позитив: наш купинний
підпис має прийняти сторонній двигун.

Усі гілки недоступності дають SKIP із названою причиною, жодна не дає PASS:
режим -NoUapki, відсутній iit_verify (x64), відсутній ключ у local-keys.json,
недоступний арбітр."
```

---

## Task 9: Матриця вердиктів по корпусу ЦЗО

**Files:**
- Modify: `tests/native_host.cpp` (кейс 9), `run_tests.ps1`

**Interfaces:**
- Consumes: `iit_verify_x86.exe`, `native_host`.
- Produces: кейс 9 — `native_host <exe> 9 "" <dataDir> <binDir> "" <file.p7s>` друкує один
  рядок JSON: `{"engine":"uapki","status":"<TOTAL-VALID|…>","errorCode":<n>}`.

- [ ] **Step 1: Додати кейс 9 — наш вердикт по одному файлу**

```cpp
// ========================================================================
// КЕЙС 9 — вердикт НАШОГО VERIFY по одному файлу (для матриці двох двигунів)
// ========================================================================
// Еталони ЦЗО лежать у репозиторії з 2026-09-01 і ЖОДНОГО разу не проганялися
// через нашу перевірку. Вердикт ІІТ для них відомий -> будуємо матрицю.
static bool case9_verifyOne(const std::wstring& binDir, const std::wstring& file) {
    if (file.empty()) { printf("FAIL: не задано файл\n"); return false; }
    std::vector<unsigned char> raw;
    if (!readFileBytes(file, raw) || raw.empty()) { printf("FAIL: файл не прочитано\n"); return false; }

    std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    std::string r = c.call("INIT", buildInit(true));
    if (errCode(r, j) != 0) { printf("FAIL: INIT\n"); c.unload(); return false; }

    json p;
    p["signature"]["bytes"]        = b64encode(std::string(raw.begin(), raw.end()));
    p["options"]["validationType"] = "STRUCT";
    r = c.call("VERIFY", p.dump());
    const long ec = errCode(r, j);

    std::string status = "NO-INFO";
    if (j.contains("result") && j["result"].is_object()) {
        auto& res = j["result"];
        if (res.contains("signatureInfos") && res["signatureInfos"].is_array()
            && !res["signatureInfos"].empty())
            status = res["signatureInfos"][0].value("status", std::string("NO-STATUS"));
    }
    printf("{\"engine\":\"uapki\",\"status\":\"%s\",\"errorCode\":%ld}\n", status.c_str(), ec);

    c.call("DEINIT", ""); c.unload();
    return true;
}
```

Зареєструвати: `case 9: pass = case9_verifyOne(binDir, outSig); break;` (сьомий аргумент
переносить шлях до файлу). Розширити usage до `<case 1..9>`.

- [ ] **Step 2: Додати матрицю в `run_tests.ps1`**

Одразу після блоку `L4-iit`:

```powershell
# --- Матриця вердиктів: корпус ЦЗО × два двигуни ---
# Мета не в тому, щоб обидва сказали "валідно" (еталони ЦЗО від тестового ЦСК),
# а в тому, щоб вони не РОЗХОДИЛИСЬ несподівано. Розбіжність — знахідка.
$czoDir = Join-Path $DataDir 'czo'
if ($NoUapki -or -not (Test-Path $czoDir) -or -not (Test-Path $IitVerifyExe)) {
    Add-Result 'L4-iit' 'матриця вердиктів' 'SKIP' 'немає корпусу czo, арбітра або режим -NoUapki'
} else {
    $files = Get-ChildItem -Path $czoDir -Recurse -Filter '*.p7s' | Sort-Object FullName
    $rows  = @()
    foreach ($f in $files) {
        $ourRaw = & $nhExe 9 '' $DataDir $BinRelease '' $f.FullName 2>&1 |
                  Where-Object { $_ -match '^\{' } | Select-Object -Last 1
        $iitRaw = & $IitVerifyExe $f.FullName 2>&1 |
                  Where-Object { $_ -match '^\{' } | Select-Object -Last 1
        $rows += [pscustomobject]@{
            File = $f.FullName.Substring($czoDir.Length + 1)
            Uapki = $ourRaw
            Iit   = $iitRaw
        }
    }
    $rows | ForEach-Object { Write-Host ("    {0,-46} uapki={1} iit={2}" -f $_.File, $_.Uapki, $_.Iit) }
    Add-Result 'L4-iit' 'матриця вердиктів' 'PASS' "$($rows.Count) файлів, вердикти обох двигунів зібрано"
}
```

> **Свідомо без автоматичного вироку.** Правило «яка розбіжність є дефектом» ухвалюється
> **після** першого прогону, на реальних даних — див. відкрите питання №6 у специфікації.
> Зараз матриця **збирає й показує** вердикти; перетворення її на FAIL-критерій — окрема
> задача після аналізу виводу.

- [ ] **Step 3: Прогнати й зафіксувати вивід**

```powershell
powershell -File run_tests.ps1 x86
```

Очікувано: 16 рядків матриці (8 `.p7s` × два двигуни — по 4 у кожній групі czo).
**Зберегти вивід** — він є вхідними даними для рішення по відкритому питанню №6.

- [ ] **Step 4: Коміт**

```bash
git add tests/native_host.cpp run_tests.ps1
git commit -m "test(gate): матриця вердиктів — корпус ЦЗО очима двох двигунів

Еталони ЦЗО лежали в репозиторії з 2026-09-01 і жодного разу не проганялися через
наш власний VERIFY. Тепер кожен файл отримує два незалежні вердикти.

Матриця свідомо НЕ виносить автоматичного вироку: правило 'яка розбіжність є
дефектом, а яка властивістю вхідних даних' ухвалюється після першого прогону на
реальних даних, а не вигадується наперед. Саме таке передчасне судження зашите
в кейс 5 (4161 вважається прийнятним) і саме тому підлягає перегляду."
```

---

## Task 10: Кейс 5 — з одного вердикту на порівняння двох

**Files:**
- Modify: `tests/native_host.cpp:594-680` (`case5_crossValidatePrro`)

**Interfaces:**
- Consumes: `iit_verify_x86.exe` (запускається з `run_tests.ps1`, не з C++).
- Produces: незмінений інтерфейс кейса 5; змінюється лише трактування.

- [ ] **Step 1: Зняти зашите припущення й друкувати вердикт машинно-читно**

У `case5_crossValidatePrro` замінити блок трактування `RET_CERT_NOT_FOUND` на друк вердикту
без вироку, лишивши FAIL лише для випадків, коли структурна частина зламана:

```cpp
        // РАНІШЕ: 4161 беззастережно вважався прийнятним, бо офлайн бракує сертифіката TSP.
        // Це судження, зашите в тест, і воно могло маскувати справжній дефект. Тепер вердикт
        // друкується машинно-читно, а звірка з другим двигуном робиться в run_tests.ps1.
        printf("  {\"engine\":\"uapki\",\"file\":\"%s\",\"status\":\"%s\","
               "\"statusSignature\":\"%s\",\"validDigests\":%s,\"errorCode\":%ld}\n",
               w2u8(f).c_str(), st.c_str(), ss.c_str(), validDig ? "true" : "false", ec);

        // FAIL лише там, де зламана САМА структура підпису: це вже не властивість
        // вхідних даних, а дефект нашого розбору.
        if (ss.rfind("VALID", 0) != 0 || !validDig) {
            printf("  FAIL: структурна частина невалідна\n");
            allOk = false;
        }
```

- [ ] **Step 2: Додати звірку з арбітром у `run_tests.ps1`**

У блоці `L4-iit`, після матриці:

```powershell
# --- Еталони ДПС очима двох двигунів ---
if ($NoUapki -or -not $PrroDocsDir -or -not (Test-Path $PrroDocsDir) -or -not (Test-Path $IitVerifyExe)) {
    Add-Result 'L4-iit' 'еталони ДПС × 2 двигуни' 'SKIP' 'немає prro_docs, арбітра або режим -NoUapki'
} else {
    $signed = Get-ChildItem -Path $PrroDocsDir -Recurse -Filter '*.signed' -ErrorAction SilentlyContinue
    if (-not $signed) {
        Add-Result 'L4-iit' 'еталони ДПС × 2 двигуни' 'SKIP' 'у prro_docs немає *.signed'
    } else {
        foreach ($s in $signed) {
            $iitRaw = & $IitVerifyExe $s.FullName 2>&1 |
                      Where-Object { $_ -match '^\{' } | Select-Object -Last 1
            Write-Host ("    {0,-40} iit={1}" -f $s.Name, $iitRaw)
        }
        Add-Result 'L4-iit' 'еталони ДПС × 2 двигуни' 'PASS' "$($signed.Count) еталонів, вердикт ІІТ зібрано"
    }
}
```

Використати наявну в скрипті змінну каталогу `prro_docs` (та, що вже передається в кейс 5);
якщо вона названа інакше — підставити її ім'я.

- [ ] **Step 3: Прогнати**

```powershell
powershell -File run_tests.ps1 x86
```

Очікувано: кейс 5 лишається PASS/SKIP як був; додається рядок із вердиктами ІІТ по тих самих
файлах. **Зберегти вивід** — вхід для рішення по відкритому питанню №6.

- [ ] **Step 4: Коміт**

```bash
git add tests/native_host.cpp run_tests.ps1
git commit -m "test(uapki): кейс 5 — прибрано зашите припущення про 4161

Раніше CERT_NOT_FOUND беззастережно зараховувався як прийнятний: мовляв, офлайн
бракує сертифіката TSP. Це судження жило всередині тесту й могло маскувати
справжній дефект.

Тепер вердикт друкується машинно-читно, FAIL лишається тільки там, де зламана сама
структура підпису (це вже дефект нашого розбору, а не властивість вхідних даних),
а ті самі файли паралельно отримують вердикт від ІІТ."
```

---

## Task 11: Self-test емулятора — зовнішній суддя в коло

**Files:**
- Modify: `tests/uapki_fiscal_emulator.cpp` (`runSelfTest`)

**Interfaces:**
- Consumes: `iit_verify_x86.exe` — запускається як зовнішній процес.
- Produces: нічого для інших задач.

- [ ] **Step 1: Додати зовнішній вердикт**

Додати помічник поруч із `signData`/`verifyCms`:

```cpp
// Зовнішній суддя: віддає CMS арбітру ІІТ окремим процесом.
// Повертає код виходу iit_verify: 0 валідний | 1 невалідний | 3 SKIP | інше — помилка.
// Не знайшли арбітра -> 3 (SKIP), НІКОЛИ не мовчазний успіх.
static int judgeByIit(const std::string& cms) {
    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring tmp = std::wstring(tmpDir) + L"sac_judge_"
                           + std::to_wstring(GetCurrentProcessId()) + L".p7s";
    {
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return 3;
        DWORD w = 0;
        WriteFile(h, cms.data(), (DWORD)cms.size(), &w, nullptr);
        CloseHandle(h);
    }
    const std::wstring exe = exeDir() + L"\\iit_verify_x86.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(tmp.c_str());
        return 3;
    }
    std::wstring cmd = L"\"" + exe + L"\" \"" + tmp + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DeleteFileW(tmp.c_str()); return 3;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD rc = 2;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileW(tmp.c_str());
    return (int)rc;
}
```

У `runSelfTest` після позитивної перевірки додати:

```cpp
    // Замикаємо коло: досі self-test підписував нашим стеком і перевіряв нашим же.
    // Тепер той самий CMS судить СТОРОННІЙ двигун.
    const int jGood = judgeByIit(sig);
    if (jGood == 3) {
        std::printf("  skip: арбітр ІІТ недоступний — зовнішня перевірка не виконана\n");
    } else {
        check(jGood == 0, "валідний CMS ПРИЙНЯТО зовнішнім двигуном (ІІТ)");
    }
```

Після негативу №1 (зіпсований вміст):

```cpp
    const int jBad = judgeByIit(bad);
    if (jBad == 3) {
        std::printf("  skip: арбітр ІІТ недоступний — зовнішній негатив не перевірено\n");
    } else {
        check(jBad != 0, "CMS зі зіпсованим вмістом ВІДХИЛЕНО і зовнішнім двигуном");
    }
```

- [ ] **Step 2: Прогнати self-test**

```powershell
powershell -File build_project.ps1 -WithUAPKI -WithTests
bin\Release\uapki_fiscal_emulator_x86.exe --self-test --key <шлях> --password <пароль>
```

Очікувано: наявні перевірки як були, плюс два рядки зовнішнього судді. Якщо ІІТ недоступний —
два `skip`, і жодного мовчазного успіху.

- [ ] **Step 3: Коміт**

```bash
git add tests/uapki_fiscal_emulator.cpp
git commit -m "test(uapki): self-test емулятора отримав зовнішнього суддю

Досі коло було замкнене: signData нашим стеком -> verifyCms нашим стеком. Негативні
кейси цінні, але доводили лише, що НАШ верифікатор помічає псування — не що наш
ПІДПИС прийнятний для стороннього двигуна.

Тепер і позитив, і негатив проходять через iit_verify. Якщо ІІТ прийме те, що ми
відхиляємо (або навпаки) — це розбіжність, про яку треба знати. Недоступність
арбітра дає явний skip-рядок, а не тихий успіх."
```

---

## Task 12: Документація — етикетки, час, свідомі відступи

**Files:**
- Modify: `tests/native_host.cpp:536` (заголовок блоку кейса 4)
- Modify: `docs/architecture/uapki.md`
- Modify: `docs/integration-1c/uapki.md`

**Interfaces:**
- Consumes: нічого. Produces: нічого. Суто документаційний deliverable.

- [ ] **Step 1: Перемаркувати блок кейса 4**

Замінити коментар-заголовок (`native_host.cpp:536`):

```cpp
    // --- L3.2: структурна перевірка ОФЛАЙН-профілю ПРРО (CAdES-BES) ---
    // МЕЖА ПОКРИТТЯ, читай уважно:
    //   * норматив ДПС вимагає CAdES-E-T із signature-time-stamp для онлайн-документів
    //     («Опис АРІ фіскального сервера (ЄВПЕЗ)», розділ «Порядок засвідчення повідомлень»);
    //   * позначка часу НЕ обов'язкова лише для документів, створених в офлайні — саме цей
    //     профіль тут і перевіряється;
    //   * тому signatureTS відсутній ПРАВОМІРНО, а не «так має бути завжди»;
    //   * онлайн-шлях (похід у TSP) не покритий ЖОДНИМ тестом: код
    //     extern/uapki/library/uapki/src/doc-sign.cpp:864-877 не виконувався ніколи.
    // Доказом коректності самого підпису є вердикт стороннього двигуна (рівень L4-iit),
    // а не наш власний VERIFY нижче — той є РЕГРЕСІЙНОЮ перевіркою.
```

- [ ] **Step 2: Додати межу покриття й відступи в `docs/architecture/uapki.md`**

Додати новий розділ наприкінці:

```markdown
## 8. Межа покриття тестами й свідомі відступи від нормативу

### 8.1. Що доводить зелений гейт, а що ні

Усі крипто-сценарії ганяються з `offline: true`, `ignoreCertStatus: true` і `VERIFY` у режимі
`STRUCT` (ланцюг не валідує). Отже зелений гейт доводить працездатність **офлайн-профілю
`CAdES-BES`** — і не доводить нічого про онлайн-шлях. Код TSP у підписі
(`extern/uapki/library/uapki/src/doc-sign.cpp:864-877`) не виконувався жодного разу.

Нюанс, що знадобиться при закритті цієї прогалини: `SharedData::setupTsp`
(`doc-sign.cpp:145-170`) бере TSP-адресу **з сертифіката підписувача**, а конфігураційні URL
використовує лише як запасні або за `tsp.forced`. Тобто `CAdES-T` веде в TSP того КНЕДП, що
видав ключ, а не ДПС.

Незалежний арбітр — рівень `L4-iit`, `iit_verify` на нативній бібліотеці АТ «ІІТ». Він потрібен
тому, що UAPKI є форком Cryptonite (`extern/uapki/README.md:4`), і Cryptonite-похідні двигуни
арбітром бути не можуть: спільний кодовий предок дає спільні системні помилки.

### 8.2. Свідомий відступ №1: `CAdES-BES` замість нормативного `CAdES-E-T`

**Норматив:** ДПС вимагає `CAdES-E-T` із `signature-time-stamp`.
**Що робимо:** віддаємо `CAdES-BES` + `includeTime` (локальний час).
**Підстава:** виміряно, що бойова система працює без TSP-токена й ДПС такі підписи приймає,
видаючи фіскальні номери.
**Умова перегляду:** ДПС починає відхиляти підписи без позначки часу; або змінюється опис API
ЄВПЕЗ.

### 8.3. Свідомий відступ №2: ланцюг довіри для ПРРО не перевіряємо

**Що робимо:** `certCache`/`trustedCerts` для ПРРО не наповнюємо.
**Підстава:** ми не обмінюємось юридично значущими документами з контрагентами — надсилаємо
власний документ на відомий нам ресурс і отримуємо технічну відповідь на власне звернення.
**Умова перегляду:** поява ЕДО з контрагентами; вимога ДПС перевіряти ланцюжок на клієнті;
розширення компоненти на приймання підписаних документів ззовні.

> Арбітра це не стосується: `iit_verify` сховище довіри **потребує**, інакше не відрізнить
> валідний підпис від підпису невідомого ЦСК. Різні ролі — різні вимоги.
```

- [ ] **Step 3: Додати розділ «Час підпису» в `docs/integration-1c/uapki.md`**

Вставити перед розділом «9. Тестування»:

```markdown
## 8-біс. Час підпису: три режими, і лише один із них довірений

| Режим | Джерело часу | Довіра | Мережа на кожен підпис | Як задати |
|---|---|---|---|---|
| локальний | годинник машини | ні | немає | `signParams.includeTime: true` |
| заданий ззовні | те, що передала 1С | ні | немає | `signedAttributes` з OID `1.2.840.113549.1.9.5` + `includeTime: false` |
| довірена позначка | TSP-сервер (RFC 3161) | **так** | так | `signatureFormat: "CAdES-T"` |

**Головне, щоб не помилитись:** «час із сервера» **не робить позначку довіреною**. `signingTime`
— це підписаний атрибут, тобто **твердження підписувача**; звідки взято значення, на його
юридичний статус не впливає. Довіру дає лише TSP-токен, бо там час засвідчує третя сторона.

**Що відтворює поточну бойову поведінку:** перший рядок. Виміряно на боці 1С: підпис іде з
`signingTime` від локального годинника, а `signature-time-stamp` відсутній повністю.

Компонента для всіх трьох режимів **правок не потребує** — вона passthrough, і `signParams`
повністю під контролем 1С.

**Діагностика зсуву годинника** належить туди, де вже відбувається HTTP-обмін — на бік 1С,
через заголовок `Date` у відповіді фіскального сервера. Це саме той годинник, за яким нас
судять, і він приходить безкоштовно з відповіддю, яку й так отримуємо.
```

- [ ] **Step 4: Перевірити, що документи узгоджені**

Прочитати обидва змінені документи цілком і переконатися, що нові розділи не суперечать
наявним (зокрема §7 архітектурного документа про сабмодуль і §9 інтеграційного про тестування).

- [ ] **Step 5: Коміт**

```bash
git add tests/native_host.cpp docs/architecture/uapki.md docs/integration-1c/uapki.md
git commit -m "docs(uapki): межа покриття, два свідомі відступи і три режими часу

Кейс 4 називався 'структурна крос-перевірка формату підпису ПРРО' без застереження,
що покрито лише офлайн-профіль. Перевірки в ньому коректні — хибною була етикетка,
через яку тест читався як доказ ширший за той, що дає.

Зафіксовано два свідомі відступи від нормативу з підставою й умовою перегляду:
CAdES-BES замість E-T (бойова система працює без TSP-токена, ДПС приймає) і
неперевірка ланцюга довіри для ПРРО. Без умови перегляду такий запис неповний, бо
наступний читач вважатиме його недоглядом і 'виправить'.

Розділ про час знімає термінологічну пастку: 'час із сервера' НЕ робить позначку
довіреною — довіру дає лише TSP-токен. Компонента правок не потребує: вона
passthrough, signParams під контролем 1С."
```

---

## Самоперевірка плану

**Покриття специфікації:**

| Розділ спеки | Задача |
|---|---|
| §4 Маскування пароля | 1 |
| §5 Чесний тест-контракт | 12 (крок 1) |
| §6.1 Локальний конфіг | 2 |
| §6.2 рівень 1 (OPEN) | 3 |
| §6.2 рівень 2 (SIGN + структура) | 4 |
| §6.2 рівень 3 (арбітр) | 5, 6, 7, 8 |
| §6.2-біс (1) кейс 5 | 10 |
| §6.2-біс (2) self-test емулятора | 11 |
| §6.2-біс (3) матриця по czo | 9 |
| §6.2-біс перемаркування | 4 (крок 1, коментар), 12 |
| §6.3 Ланцюг довіри | 12 (крок 2) |
| §7 Час підпису | 12 (крок 3) |
| §8 Свідомі відступи | 12 (крок 2) |
| §10 критерій 9 (негативний контроль) | 8 |

Прогалин немає.

**Консистентність імен між задачами:** `LocalKey`/`LoadLocalKeys`/`FindLocalKey` (задача 2 →
3, 4); `ensureIitStore` (7 → 6); `iit_verify_x86.exe` (5 → 8, 9, 10, 11); коди виходу
`0/1/2/3` єдині для `iit_verify` в задачах 5-11; `$IitVerifyExe`, `$nhExe`, `$SigOut`
визначаються в задачі 8 і використовуються в 9, 10.

**Відома межа плану:** задача 2 не має власного тесту — вона інфраструктурна й перевіряється
споживачем (задача 3). Це єдиний такий випадок, і він позначений у самій задачі.
