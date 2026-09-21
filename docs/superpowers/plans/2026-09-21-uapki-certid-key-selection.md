# Вибір ключа за сертифікатом (`SELECT_KEY` за `certId`) — план імплементації

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Довести чотирма новими перевірками, що підписний ключ із двоключового КЕП-контейнера обирається **за сертифікатом** (`SELECT_KEY` за `certId`), а не вгадуванням серед `keys[]`, — і виправити документацію, за якою сусідня команда зараз будує міграцію ПРРО.

**Архітектура:** Компонента **не змінюється** — уся робота в тестовому харнесі `tests/native_host.cpp` (два нові кейси, 10 і 11), у підключенні їх до гейта `run_tests.ps1` і у двох документах. Кейс 10 — універсальний, на закоміченому `test-diia.p12` плюс два сертифікати з `tests/data/certs/`, подані через `ADD_CERT`; кейс 11 — на особистому купинному JKS із `tests/data/local-keys.json`, зі SKIP (exit 3) там, де конфігу немає.

**Tech Stack:** C++17, MSVC (VS2022), CMake; `nlohmann/json`; харнес `tests/native_host.cpp` (компонента `AddinUAPKIConnect` через головну DLL, `LoadLibraryW` + `GetClassObject`); PowerShell 5.1 (`run_tests.ps1`); Python 3 (`scripts/check-doc-anchors.py`).

**Spec:** `docs/superpowers/specs/2026-09-21-uapki-certid-key-selection-design.md`

## Власність документа

Цей план має **одного власника — сесію-архітектора**, яка його написала. Виконання йде в **іншій сесії того самого робочого дерева й тієї самої гілки**.

- **Виконавцю дозволено рівно одну правку цього файла: відмітка кроку** `- [ ]` → `- [x]`. Це трекінг прогресу, а не зміна плану.
- **Будь-яка змістовна зміна — через архітектора.** Не переписуй крок, не прибирай `CHECK`, не міняй обсяг, навіть якщо впевнений, що так правильніше. Напиши архітектору через `SendMessage`, **з `file:line`**, у чому розбіжність — і дочекайся відповіді. Розбіжність із планом майже завжди цінна: або план помиляється, або помиляється припущення про код, і обидва випадки треба закрити явно.
- **Якщо реальність не збіглася з планом** (інша кількість ключів, інший код помилки, `CHECK` не червоніє там, де мав) — це **не привід підправити план під факт**. Зупинись, зафіксуй вивід, повідом. Кроки, де таке ймовірне, позначені явно.
- Архітектор попереджає виконавця перед тим, як правити цей файл: дерево спільне, і дві одночасні правки одного файла — відомий тут спосіб втратити роботу.

## Global Constraints

Діють у **кожному** завданні (джерела: `AGENTS.md`, `CLAUDE.md`, спека §3 і §9).

- **Мова коду, коментарів і комітів — українська.** Повідомлення складай конкатенацією, з великої літери, без крапки в кінці.
- **Компоненту НЕ чіпати.** `src/` не редагується взагалі. `AddinUAPKIConnect` лишається тонким passthrough — свідоме рішення спеки §3 («Не входить»). Якщо здається, що потрібна правка в `src/` — це сигнал, що щось зрозуміло не так; зупинись і спитай.
- **Схему `tests/data/local-keys.json` НЕ розширювати.** Ключ `jks-kupyna` там уже є, сертифікати — всередині контейнера.
- **Наявні кейси 1–9 НЕ змінювати.** Нові твердження — новими кейсами. Кейс 8 (обхід через `keyId2`) лишається як є: він доводить, що обхід працює; кейс 11 доводить, що обхід непотрібний.
- **`tests/` не підключає PCH** (`src/core/pch.h`) — це окремі консольні exe. `printf` тут дозволено.
- **`tests/CMakeLists.txt` міняти не треба**: `native_host` уже збирається з `native_host.cpp support/LocalKeys.cpp` (`tests/CMakeLists.txt:342`) і вже має `/utf-8`.
- **Файли з кирилицею зберігати в тому кодуванні, у якому вони є.** `run_tests.ps1` — **UTF-8 З BOM**; правити його **точковими `Edit`**, ніколи не перезаписувати цілком через `Write`. Перевірка: `head -c 3 run_tests.ps1 | xxd -p` має дати `efbbbf`.
- **Гейт і збірка — монопольні.** Перед `build_project.ps1` / `run_tests.ps1` спитати людину, чи не збирає хтось інший; `build_project.ps1` падає, якщо у `bin/Release` є запущені файли (емулятор, native_host, 1С з підключеною компонентою).
- **`run_tests.ps1` НЕ перезбирає.** Після кожної правки `native_host.cpp` — збирати самому. Інкрементально (≈1 с проти ≈7 хв повної збірки):
  ```
  cmake --build build_x64 --config Release --target native_host
  cmake --build build_x86 --config Release --target native_host
  ```
  Повна збірка (потрібна один раз на початку, щоб з'явились `build_x64/`/`build_x86/` з `BUILD_WITH_UAPKI=ON` і `BUILD_TESTS=ON`):
  `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests`
- **Негативна верифікація обов'язкова** (`docs/architecture/testing-rules.md`, правило 1): кожен новий `CHECK` хоч раз побачено червоним. Тест, що не падає на дефекті, — не тест. Конкретні зломи прописані в кроках.
- **Охорона `CHECK` має накривати ВСІ операнди, а не той, з якого починається вираз.** Чотири підправила; за цю сесію кожне куплене окремим дефектом у коді **цього ж плану**, і всі чотири давали ЗЕЛЕНЕ:
  1. `contains()` — **з обох боків** порівняння. `k0`/`k1` — це `const json&`, і константний `operator[]` на відсутньому ключі не кидає винятку, а дає UB: `json.hpp:22182-22193` містить самий `JSON_ASSERT`, а той (`json.hpp:2571`) — звичайний `assert`, який під `NDEBUG` конфігурації Release зникає.
  2. **Сентинели в `value()` не рятують — вони маскують.** `k0.value("id", "a") != k1.value("id", "b")` зеленіє й тоді, коли `id` немає в **жодного** ключа: дефолти різні, отже «різні». Перевірка стверджує розрізнення, а дефолти гарантують його самі по собі — вона не читає нічого.
  3. **`CHECK`, що стверджує ЗБІГ, інвертувати й побачити червоним.** Інакше не відрізнити «однакові» від «обидва порожні»: `null == null` → `true`.
  4. **`const auto& x = j[...]` живе лише до наступного `errCode(r, j)`** — повне присвоєння `json` знищує вузли разом зі сховищем. Не переставляти блоки; треба безпечніше — копіювати значення в `std::string`/`bool`, а не посилання.
- **Доказ стану перед перевіркою** (правило 2): перш ніж стверджувати поведінку в стані X, `CHECK` має довести, що система в X.
- **Гейт кожного завдання — зелений на x64 **і** x86**: `powershell -ExecutionPolicy Bypass -File run_tests.ps1 x64` і те саме `x86`.
- **`version.h`** перегенеровує **тільки** `build_project.ps1` (`build_project.ps1:73`); руками не редагувати, але **завжди комітити** разом із роботою. У цій роботі він змінюється **рівно один раз** — на єдиній повній збірці (Task 1 крок 1), і далі лишається чистим: `run_tests.ps1` про `version.h` не знає взагалі (`grep -c version run_tests.ps1` → 0), CMake його не генерує (жодного `configure_file`), а `manifest.ps1`/`release.ps1` лише читають. Тож «на всяк випадок» додавати `version.h` у `git add` кожного коміта **не треба** — файл там не зʼявиться, а вказівка шукати неіснуючу зміну коштує дорожче, ніж її відсутність.
- **Комітити лише свої файли**: `git add <явний перелік>` + `git commit --only -- <ті самі шляхи>`. Ніколи `git add -A`, ніколи `git checkout --`/`restore`/`stash`/`clean` — у дереві можуть працювати інші сесії.
- **Номери рядків у цьому плані НЕ вважати чинними — шукати за вмістом.** Завдання 1–3 самі вставляють у `tests/native_host.cpp` і `run_tests.ps1` сотні рядків, тобто **рухають власні якорі плану**: після Task 1 усе нижче константи `KEY_ID` зʼїхало приблизно на 52 рядки, після Task 3 зʼїде ще раз. Числа в «Files» і в кроках дійсні на **момент написання плану** (`13d7bd5`, `tests/native_host.cpp` = 1235 рядків) і лишені як орієнтир порядку, не як адреса. Адресуйся за унікальним рядком коду або іменем функції; номер бери **щоразу свіжий і лише з `grep -n`**, ніколи не рахуючи у виводі `sed -n 'A,Bp'`. `scripts/check-doc-anchors.py` цього не впіймає: `docs/superpowers/` свідомо поза його перевіркою (`AGENTS.md`, розділ про гард).
- **Кожен коміт закінчувати рядком** `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>` (багаторядкове повідомлення — через `git commit -F <файл>`).

### Факти, звірені в коді — брати звідси, не перевимірювати

| Факт | Джерело |
|---|---|
| `RET_UAPKI_INVALID_KEY_USAGE` = `0x100D` = **4109**, рядок `error` = `"INVALID_KEY_USAGE"` | `uapki-errors.h:58` + `uapki-errors.c:170`, Додаток А `extern/uapki/doc/UAPKI-PM-2.0.16.md` |
| `RET_UAPKI_CERT_NOT_FOUND` = `0x1041` = **4161** | `uapki-errors.h:100` |
| SKI підписного сертифіката = `5BC6C06EE1E00C1700E92AA7A9AD75F82D3CB7A9B66E3A98023209B24513315C` (= наявна константа `KEY_ID`) | `openssl x509 -ext subjectKeyIdentifier` по `tests/data/certs/BED50831-5BC6C06E-*.cer` |
| SKI шифрувального = `6B1B77C0D1A1B60473A98DD6D4FE5302742AEDE101DAA21F2C83A67CCDEDB782` (= ключ у `tests/scenarios/06_encrypt_decrypt.json`) | те саме по `BED50831-6B1B77C0-*.cer` |
| `keyUsage` підписного = `Digital Signature, Non Repudiation`; шифрувального = `Key Agreement`; **суб'єкт обох однаковий** | `openssl x509 -subject -ext keyUsage` |
| `LIST_CERTS` із `showCertInfos:true` віддає `keyUsage` **окремим полем кожного `certInfos[]`** — окремий `CERT_INFO` по кожному `certId` не потрібен | `list-certs.cpp:135-136` |
| У `keyUsage` присутні **лише виставлені** біти; відсутнє поле = `false` | `extension-helper-json.cpp:410-418` |
| `isCa` у `certInfos[]` присутній **лише коли `cA` істинний** | `list-certs.cpp:55-58` |
| `SELECT_KEY` за `certId` бере SKI **самого сертифіката** й ним обирає ключ | `session-select-key.cpp:81-89` |
| Фаза 1 наповнює кеш **до** пошуку сертифіката: `keyGetCertificates`+`addCerts` на `:128-137`, пошук — на `:139-144`, ковтання `CERT_NOT_FOUND` — на `:152-154` | `session-select-key.cpp` |
| Перевірка `keyUsage` у `SIGN` **умовна**: `(формат != RAW) && (!sidUseKeyId \|\| includeCert)` | `sign.cpp:304-314` |
| `ignoreCertStatus:true` → `CertValidator::setValidationType` не кличеться → `m_ValidationType` лишається `UNDEFINED` (0) → `getStatus` виходить одразу з `RET_OK`, **ланцюг не будується** | `sign.cpp:316-322` + `cert-validator.cpp:334` |
| Компонента — чистий passthrough, білого списку методів немає: `ADD_CERT`/`CERT_INFO`/`LIST_CERTS` проходять як є | `UAPKIConnectHelper.cpp:615-676` |
| `ADD_CERT` без `permanent` → `false` → кеш лише на час сесії, на диск нічого не пишеться | Додаток «Метод ADD_CERT», `UAPKI-PM-2.0.16.md` |

---

## Структура файлів

| Файл | Відповідальність | Що змінюється |
|---|---|---|
| `tests/native_host.cpp` | L2/L3-харнес компоненти `AddinUAPKIConnect` через головну DLL | **+4 хелпери**, **+2 константи**, **+кейс 10**, **+кейс 11**, розширення CLI до `1..11` |
| `run_tests.ps1` | Оркестратор гейта | кейс 10 — у наявний цикл `foreach`; кейс 11 — окремий блок із SKIP-семантикою; шапка-коментар |
| `docs/integration-1c/uapki.md` | Прикладна інструкція для 1С-розробника | §4.3 переписано: `certId` — основний шлях, двофазний рецепт, страховка з умовою |
| `docs/architecture/uapki.md` | Механізми стеку UAPKI | §9.1: правило «перевіряй `certId`» прив'язано до гілки `id` |

Нових файлів немає. `tests/CMakeLists.txt`, `src/`, `tests/data/`, `tests/scenarios/` **не змінюються**.

---

## Завдання

### Task 1: Кейс 10 — позитивна частина (твердження 1–3)

**Files:**
- Modify: `tests/native_host.cpp` (константи ~`:63-67`; хелпери — після `writeFileBytes`, перед `#define CHECK` на `:418`; тіло кейса — перед блоком `// main / CLI` на `:1144`; `usage()` `:1147`; `main()` `:1176`, `:1215`)
- Test: сам кейс і є тестом; запуск — `bin\Release\native_host_x64.exe 10`

**Interfaces:**
- Consumes: наявні `Component::load/call`, `errCode`, `CHECK`, `buildInit`, `buildOpen`, `buildSign`, `readFileBytes`, `b64encode`, `pathExists`, `fwd`, константа `KEY_ID`, макроси `ARCH_W`.
- Produces: `static bool case10_selectByCertId(const std::wstring& binDir, const std::wstring& dataDir)` — Task 2 дописує в **те саме тіло** негативну частину; хелпери `findCertByPrefix`, `readCertB64`, `upperAscii`, `elideCert` і константи `KEY_ID_ENCRYPT`, `CERT_PREFIX_SIGN`, `CERT_PREFIX_ENCRYPT` — використовує і Task 3.

---

- [ ] **Крок 1: Зміряти базову лінію гейта ДО будь-якої правки**

Успадковане число виміром не є (спека §9, критерій 5). Спитати людину, чи вільні `bin/Release` й збірка, потім:

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
powershell -ExecutionPolicy Bypass -File run_tests.ps1 x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 x86
```

Записати в робочі нотатки: `PASS=… FAIL=… SKIP=… BLOCKED=…` окремо для x64 і x86, **і повний перелік не-PASS рядків поіменно**. Еталон — саме перелік рядків, а не підсумкові числа: середовище тут живе (запущена 1С, чужі сесії), і числа можуть рухатись із причин, до нашого коду не дотичних. Кожен наступний прогін звіряється **порядково**, а не за сумою.

**Якщо базова лінія червона — розрізнити дві причини, перш ніж зупинятись:**

- **Червоне від СЕРЕДОВИЩА — записати й продовжувати.** Найчастіший випадок: запущена 1С із раніше підключеною компонентою тримає `%LOCALAPPDATA%\SimplyAddinConnect\providers`, і кейси 1/2 `native_host` падають на прибиранні каталогу. Харнес друкує причину сам — `tests/native_host.cpp:529-534` (кейс 1) і `:581-586` (кейс 2). Такі рядки йдуть в еталон **як є**, зі своїм статусом і причиною; робота не блокується.

  > **Вимір 2026-09-21:** базову лінію знято при **трьох живих `1cv8`**, і кейси 1/2 були **зелені** на обох архітектурах. Отже сама лише наявність запущеної 1С цієї поломки не спричиняє — потрібна 1С, що **справді підключала компоненту** й тримає провайдера завантаженим у процесі. Переміряти «коли 1С звільниться» не треба: зелене при живій 1С — сильніший вимір, ніж зелене без неї.
  >
  > **Межа цього виміру:** він каже, що **ті три сеанси** каталог не тримали, а не що механізм блокування неможливий. Якщо кейси 1/2 почервоніють посеред роботи, ця пара лишається **першою** гіпотезою, а не відкинутою як «уже спростована».
- **Червоне від КОДУ — зупинитись і повідомити людину**, не починаючи правок. Ознака: рядок падає без середовищної причини в тексті, або падає те, що наші зміни взагалі не зачіпають (L0-інваріанти, L1-сценарії, ECR/LabelPrinter).

Плутати ці два випадки дорого в обидва боки: за першим сценарієм зупинка коштує годин очікування на чужу 1С, за другим продовження означає, що всю роботу зроблено поверх уже зламаного гейта.

- [ ] **Крок 2: Додати константи двох ключів і префікси імен сертифікатів**

У `tests/native_host.cpp` одразу **після** блоку з `KEY_ID` і `DATA_TBS_B64` (зараз рядки 62–67):

```cpp
// Другий ключ того самого контейнера — ШИФРУВАЛЬНИЙ (див. tests/scenarios/06_encrypt_decrypt.json).
// Український КЕП-контейнер завжди двоключовий; test-diia.p12 тут не виняток, а типовий зразок.
static const char* KEY_ID_ENCRYPT =
    "6B1B77C0D1A1B60473A98DD6D4FE5302742AEDE101DAA21F2C83A67CCDEDB782";

// Префікси імен сертифікатів у tests/data/certs/. CerStore іменує файли
// "<AKI>-<SKI>-<thumbprint>.cer", тож префікс однозначно адресує сертифікат за
// ідентифікатором його ключа, а хвіст (thumbprint) у тесті фіксувати не треба.
// Звірено openssl: SKI підписного == KEY_ID, SKI шифрувального == KEY_ID_ENCRYPT;
// keyUsage підписного — Digital Signature + Non Repudiation, шифрувального — Key Agreement;
// СУБ'ЄКТ В ОБОХ ОДНАКОВИЙ, тож розрізнити їх можна ВИКЛЮЧНО за keyUsage.
static const wchar_t* CERT_PREFIX_SIGN    = L"BED50831-5BC6C06E-";
static const wchar_t* CERT_PREFIX_ENCRYPT = L"BED50831-6B1B77C0-";
```

- [ ] **Крок 3: Додати файлові/рядкові хелпери**

Вставити **після** `writeFileBytes` (зараз закінчується на рядку 415) і **перед** `#define CHECK` (зараз 417–421):

```cpp
// Знаходить файл сертифіката за ПРЕФІКСОМ імені (CERT_PREFIX_*). Перший збіг —
// єдиний: у tests/data/certs/ на кожен SKI припадає рівно один файл.
static bool findCertByPrefix(const std::wstring& dir, const std::wstring& prefix,
                             std::wstring& found) {
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"\\" + prefix + L"*.cer").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return false;
    found = dir + L"\\" + fd.cFileName;
    FindClose(hf);
    return true;
}

// Читає DER-сертифікат і віддає base64 — рівно той формат, у якому сертифікат
// приходить із бази 1С у ADD_CERT.
static bool readCertB64(const std::wstring& path, std::string& b64) {
    std::vector<unsigned char> raw;
    if (!readFileBytes(path, raw) || raw.empty()) return false;
    b64 = b64encode(raw);
    return true;
}

// Порівняння hex-ідентифікаторів без урахування регістру: UAPKI віддає `id` у
// різних відповідях через різні шляхи, і покладатись на однаковий регістр не можна.
static std::string upperAscii(std::string s) {
    for (char& ch : s) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    return s;
}

// Друк відповіді SELECT_KEY без ВМІСТУ сертифіката: сам сертифікат — особистий
// документ розробника, а вивід гейта потрапляє у звіти. Усе, заради чого спека §9
// п.4 вимагає сирий друк (перелік полів і наявність certId), лишається видимим.
static std::string elideCert(const std::string& resp) {
    json j;
    try { j = json::parse(resp); } catch (...) { return resp; }
    if (j.contains("result") && j["result"].is_object() && j["result"].contains("certificate")) {
        const std::string cert = j["result"].value("certificate", std::string());
        j["result"]["certificate"] = "<" + std::to_string(cert.size()) + " символів base64>";
    }
    return j.dump();
}
```

- [ ] **Крок 4: Написати тіло кейса 10 (позитивна частина)**

Вставити **перед** блоком `// ======== main / CLI ========` (зараз рядок 1144):

```cpp
// ========================================================================
// КЕЙС 10 — вибір ключа за СЕРТИФІКАТОМ, універсальний (test-diia.p12)
// ========================================================================
// Український КЕП-контейнер завжди містить ДВІ ключові пари — підпис і шифрування.
// Кейс доводить три твердження, жодне з яких раніше не було покрите:
//   1. KEYS не дає ознаки, за якою можна обрати підписний ключ;
//   2. CERT_INFO розрізняє сертифікати за keyUsage — і розрізняє ЄДИНИЙ, бо решта
//      полів (зокрема суб'єкт) збігається;
//   3. SELECT_KEY(certId) обирає ключ САМЕ цього сертифіката й повертає certId.
// Зовнішніх залежностей немає: і контейнер, і обидва сертифікати лежать у git.
// Сертифікати подаються через ADD_CERT, а не через certCache.path — так робить 1С
// (вони приходять base64 з бази, не з каталогу), і так заразом покривається ADD_CERT,
// який досі не був покритий нічим.
static bool case10_selectByCertId(const std::wstring& binDir, const std::wstring& dataDir) {
    printf("== Case 10: вибір ключа за сертифікатом (SELECT_KEY за certId) ==\n");
    const std::wstring dllName  = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    const std::wstring p12      = dataDir + L"\\test-diia.p12";
    const std::wstring certsDir = dataDir + L"\\certs";
    CHECK(pathExists(p12), "test-diia.p12 присутній");

    std::wstring certSignPath, certEncPath;
    CHECK(findCertByPrefix(certsDir, CERT_PREFIX_SIGN, certSignPath),
          "файл ПІДПИСНОГО сертифіката знайдено");
    CHECK(findCertByPrefix(certsDir, CERT_PREFIX_ENCRYPT, certEncPath),
          "файл ШИФРУВАЛЬНОГО сертифіката знайдено");
    std::string certSignB64, certEncB64;
    CHECK(readCertB64(certSignPath, certSignB64), "підписний сертифікат прочитано в base64");
    CHECK(readCertB64(certEncPath,  certEncB64),  "шифрувальний сертифікат прочитано в base64");

    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    std::string r;

    // INIT БЕЗ certCache.path: постійного кеша немає, отже ADD_CERT кладе сертифікати
    // лише в пам'ять сесії — на диск нічого не пишеться, tests/data лишається read-only.
    r = c.call("INIT", buildInit(true));
    printf("  INIT: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "INIT errorCode == 0");

    // ADD_CERT: permanent НЕ задаємо -> default false (тимчасово).
    { json p; p["certificates"] = json::array({ certSignB64, certEncB64 });
      r = c.call("ADD_CERT", p.dump()); }
    printf("  ADD_CERT: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "ADD_CERT errorCode == 0");
    CHECK(j["result"].contains("added") && j["result"]["added"].is_array()
          && j["result"]["added"].size() == 2, "result.added містить рівно 2 записи");

    std::vector<std::string> certIds;
    for (const auto& a : j["result"]["added"]) {
        CHECK(a.value("errorCode", -1) == 0, "сертифікат додано (added[].errorCode == 0)");
        const std::string id = a.value("certId", std::string());
        CHECK(!id.empty(), "added[].certId не порожній");
        certIds.push_back(id);
    }

    // ТВЕРДЖЕННЯ 2. Класифікуємо за keyUsage, а НЕ за порядком у added[]: порядок —
    // деталь реалізації, а ознака призначення — контракт. keyUsage лежить у розширенні
    // 2.5.29.15; UAPKI кладе в decoded.value ЛИШЕ виставлені біти
    // (extension-helper-json.cpp:410-418), тож відсутність digitalSignature == false.
    auto certUsage = [&](const std::string& certId, bool& digitalSignature, json& subject) -> bool {
        json p; p["certId"] = certId;
        const std::string resp = c.call("CERT_INFO", p.dump());
        json ji;
        if (errCode(resp, ji) != 0) { printf("  CERT_INFO: %s\n", resp.c_str()); return false; }
        digitalSignature = false;
        subject = ji["result"].value("subject", json::object());
        if (ji["result"].contains("extensions") && ji["result"]["extensions"].is_array()) {
            for (const auto& e : ji["result"]["extensions"]) {
                if (e.value("extnId", std::string()) != "2.5.29.15") continue;
                if (!e.contains("decoded") || !e["decoded"].contains("value")) continue;
                digitalSignature = e["decoded"]["value"].value("digitalSignature", false);
            }
        }
        return true;
    };

    std::string certIdSign, certIdEnc;
    json subjSign, subjEnc;
    for (const auto& id : certIds) {
        bool ds = false; json subj;
        CHECK(certUsage(id, ds, subj), "CERT_INFO по certId відпрацював");
        printf("  CERT_INFO %s... digitalSignature=%s subject=%s\n",
               id.substr(0, 16).c_str(), ds ? "true" : "false", subj.dump().c_str());
        if (ds) { certIdSign = id; subjSign = subj; }
        else    { certIdEnc  = id; subjEnc  = subj; }
    }
    // Два РІЗНІ сертифікати + бінарна класифікація: «є підписний» і «є непідписний»
    // разом означають «рівно один кожного роду».
    CHECK(certIdSign != certIdEnc, "certId сертифікатів різні");
    CHECK(!certIdSign.empty(), "рівно один сертифікат має keyUsage.digitalSignature");
    CHECK(!certIdEnc.empty(),  "рівно один сертифікат НЕ має keyUsage.digitalSignature");
    // ...і keyUsage — ЄДИНА ознака: решта полів збігається. Якби механізм обирав за
    // іменем власника, обирати було б нема за чим — саме це тут і зафіксовано.
    CHECK(!subjSign.empty() && subjSign == subjEnc,
          "суб'єкт обох сертифікатів ОДНАКОВИЙ (розрізнення можливе лише за keyUsage)");

    r = c.call("OPEN", buildOpen(p12));
    printf("  OPEN: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "OPEN errorCode == 0");

    r = c.call("KEYS", "");
    printf("  KEYS: %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "KEYS errorCode == 0");
    CHECK(j["result"].contains("keys") && j["result"]["keys"].is_array()
          && j["result"]["keys"].size() == 2, "контейнер двоключовий (keys.size() == 2)");
    // ТВЕРДЖЕННЯ 1. Це не діагностика, а CHECK: якщо в KEYS колись з'явиться ознака
    // розрізнення (keyUsage у cm-pkcs12, різні signAlgo), кейс почервоніє — і ми
    // дізнаємось, що міркування «сертифікат — єдине джерело правди» застаріло,
    // а не продовжимо спиратися на нього мовчки.
    const auto& k0 = j["result"]["keys"][0];
    const auto& k1 = j["result"]["keys"][1];
    // Сентинели value() тут НЕ годяться: різні дефолти ("a" vs "b") дали б зелене
    // навіть тоді, коли `id` немає в ЖОДНОГО ключа. Наявність перевіряємо явно.
    CHECK(k0.contains("id") && k1.contains("id") && k0["id"] != k1["id"],
          "id ключів різні (є з чого обирати)");
    // contains() ОБОВ'ЯЗКОВО з ОБОХ боків: k0/k1 — це `const json&`, а константний
    // operator[] на відсутньому ключі — UB, не виняток (json.hpp:22182-22190; під
    // NDEBUG його JSON_ASSERT зникає). Перевірка лише по k0 лишала б k1["…"] голим.
    CHECK(k0.contains("mechanismId") && k1.contains("mechanismId")
          && k0["mechanismId"] == k1["mechanismId"],
          "mechanismId обох ключів ОДНАКОВИЙ");
    CHECK(k0.contains("signAlgo") && k1.contains("signAlgo")
          && k0["signAlgo"] == k1["signAlgo"],
          "signAlgo[] обох ключів ОДНАКОВИЙ");

    // ТВЕРДЖЕННЯ 3. CHECK не на саму НАЯВНІСТЬ certId, а на РІВНІСТЬ запитаному:
    // у test-diia keys[0] — підписний, тож кейс лишився б зеленим і тоді, коли
    // механізм ігнорує сертифікат і бере перший-ліпший ключ.
    { json p; p["certId"] = certIdSign;
      r = c.call("SELECT_KEY", p.dump()); }
    printf("  SELECT_KEY(certId підписного): %s\n", elideCert(r).c_str());
    CHECK(errCode(r, j) == 0, "SELECT_KEY(certId) errorCode == 0");
    CHECK(j["result"].contains("certId"), "SELECT_KEY(certId) повернув certId");
    CHECK(j["result"].value("certId", std::string()) == certIdSign,
          "повернутий certId == запитаному (зв'язка ключ<->сертифікат саме та)");
    CHECK(upperAscii(j["result"].value("id", std::string())) == upperAscii(KEY_ID),
          "обрано ПІДПИСНИЙ ключ (id == SKI підписного сертифіката)");

    // Наскрізна зв'язка ADD_CERT(permanent=false) -> SELECT_KEY(certId) -> SIGN(includeCert)
    // до цього прогону не була зміряна ніде.
    r = c.call("SIGN", buildSign());
    printf("  SIGN: %s\n", r.substr(0, 300).c_str());
    if (errCode(r, j) != 0) printf("  SIGN (повна відповідь): %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "SIGN errorCode == 0 (наскрізна зв'язка працює)");
    CHECK(j["result"].contains("signatures") && j["result"]["signatures"].is_array()
          && !j["result"]["signatures"].empty()
          && !j["result"]["signatures"][0].value("bytes", std::string()).empty(),
          "підпис не порожній");

    c.call("CLOSE", "");
    c.call("DEINIT", "");
    c.unload();
    return true;
}
```

- [ ] **Крок 5: Розширити CLI до кейса 10**

Три точкові правки в кінці файла.

У `usage()` (зараз рядок 1149) замінити рядок
```cpp
        "native_host <case 1..9> [mainDll] [dataDir] [binDir] [prroDir] [outSig]\n"
```
на
```cpp
        "native_host <case 1..10> [mainDll] [dataDir] [binDir] [prroDir] [outSig]\n"
```

У `main()` (зараз рядок 1176) замінити
```cpp
    if (kase < 1 || kase > 9) { printf("Невідомий кейс: %s\n", w2u8(wargv[1]).c_str()); usage(); LocalFree(wargv); return 2; }
```
на
```cpp
    if (kase < 1 || kase > 10) { printf("Невідомий кейс: %s\n", w2u8(wargv[1]).c_str()); usage(); LocalFree(wargv); return 2; }
```

У `switch (kase)` після рядка `case 9:` (зараз 1215) додати
```cpp
            case 10: pass = case10_selectByCertId(binDir, dataDir);            break;
```

- [ ] **Крок 6: Зібрати й прогнати кейс 10**

```
cmake --build build_x64 --config Release --target native_host
bin\Release\native_host_x64.exe 10
```
Очікується: `=== Case 10: PASS ===`, exit 0.

**Якщо `keys.size()` виявиться не 2** — не «підправляти під факт» мовчки: надрукувати повну відповідь `KEYS`, зупинитись і повідомити людину. Уся конструкція кейса спирається на двоключовість контейнера, і її зміна — це зміна вхідної картини, а не дрібниця.

- [ ] **Крок 7: Негативна верифікація твердження 2 (класифікація за keyUsage)**

Тимчасово подати ADD_CERT той самий сертифікат ДВІЧІ — у рядку
```cpp
    { json p; p["certificates"] = json::array({ certSignB64, certEncB64 });
```
замінити `certEncB64` на `certSignB64`. Зібрати, запустити `native_host_x64.exe 10`.

Очікується **червоне**: `added[]` міститиме два записи з ОДНАКОВИМ `certId` (`isUnique:false` на другому), обидва класифікуються як підписні, `certIdEnc` лишиться порожнім →
```
FAIL: рівно один сертифікат НЕ має keyUsage.digitalSignature
```
Це доводить, що `CHECK` справді класифікує за `keyUsage`, а не за кількістю чи індексом. **Повернути `certEncB64` назад**, зібрати, переконатись, що знову PASS.

- [ ] **Крок 8: Негативна верифікація твердження 3 (зв'язка ключ↔сертифікат)**

Тимчасово замінити блок вибору ключа
```cpp
    { json p; p["certId"] = certIdSign;
      r = c.call("SELECT_KEY", p.dump()); }
```
на
```cpp
    { json p; p["id"] = KEY_ID_ENCRYPT;
      r = c.call("SELECT_KEY", p.dump()); }
```
Зібрати, запустити. Очікується **червоне** на
```
FAIL: повернутий certId == запитаному (зв'язка ключ<->сертифікат саме та)
```
(бо повернеться `certId` шифрувального сертифіката) **або** на `обрано ПІДПИСНИЙ ключ (id == SKI підписного сертифіката)`. Достатньо будь-якого з двох — обидва показують, що `CHECK` перевіряє саме зв'язку, а не факт «щось прийшло». **Повернути блок назад**, зібрати, PASS.

- [ ] **Крок 9: Прогнати кейс 10 на x86**

```
cmake --build build_x86 --config Release --target native_host
bin\Release\native_host_x86.exe 10
```
Очікується `PASS`, exit 0.

- [ ] **Крок 10: Коміт**

```bash
git add tests/native_host.cpp
git commit --only -- tests/native_host.cpp -F - <<'EOF'
test(uapki): кейс 10 — вибір ключа за сертифікатом на test-diia.p12

Двоключовий КЕП-контейнер: KEYS не дає ознаки для вибору підписного ключа
(однакові mechanismId і signAlgo), CERT_INFO розрізняє сертифікати за keyUsage
при однаковому суб'єкті, SELECT_KEY(certId) обирає ключ саме цього сертифіката.
Сертифікати подаються через ADD_CERT (як із бази 1С) — заразом перше покриття
ADD_CERT. Зовнішніх залежностей немає: усе в git.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
```

---

### Task 2: Кейс 10 — негативна частина й підключення до гейта

**Files:**
- Modify: `tests/native_host.cpp` (кінець тіла `case10_selectByCertId`, перед `c.call("CLOSE", "")`)
- Modify: `run_tests.ps1` (шапка-коментар рядки 14–17; коментар і `foreach` рядки 564–566)
- Test: `bin\Release\native_host_x64.exe 10`, потім повний гейт x64 і x86

**Interfaces:**
- Consumes: `case10_selectByCertId` із Task 1, змінні `certIdEnc`, `r`, `j`, `c`, хелпер `upperAscii`, константа `KEY_ID_ENCRYPT`.
- Produces: кейс 10 у переліку `foreach ($kase in 1,2,3,4,6,10)` гейта.

---

- [ ] **Крок 1: Дописати негативну частину в тіло кейса 10**

Вставити **перед** завершальними рядками `c.call("CLOSE", "");` / `c.call("DEINIT", "");` / `c.unload();` у `case10_selectByCertId`:

```cpp
    // --- НЕГАТИВНА ЧАСТИНА — обов'язкова ---
    // testing-rules.md, правило 1: без неї кейс лишався б зеленим навіть тоді, коли
    // механізм ігнорує сертифікат. Тут доводиться протилежне: обраний ключ справді
    // визначає, чим підписують, і підпис ключем ШИФРУВАННЯ не проходить.
    { json p; p["certId"] = certIdEnc;
      r = c.call("SELECT_KEY", p.dump()); }
    printf("  SELECT_KEY(certId ШИФРУВАЛЬНОГО): %s\n", elideCert(r).c_str());
    CHECK(errCode(r, j) == 0, "SELECT_KEY(certId шифрувального) errorCode == 0 (ключ існує)");
    // Доказ стану ПЕРЕД перевіркою (правило 2): без нього падіння SIGN нижче могло б
    // означати що завгодно, у т.ч. «ключ не вибрався взагалі».
    CHECK(upperAscii(j["result"].value("id", std::string())) == upperAscii(KEY_ID_ENCRYPT),
          "обрано саме ШИФРУВАЛЬНИЙ ключ");
    CHECK(j["result"].value("certId", std::string()) == certIdEnc,
          "до нього прив'язано ШИФРУВАЛЬНИЙ сертифікат");

    // УМОВА ВХОДУ в перевірку keyUsage НЕ безумовна: (формат != RAW) && (!sidUseKeyId
    // || includeCert) — sign.cpp:304-314. buildSign() дає CAdES-BES + includeCert:true,
    // тобто саме ту гілку, де страховка працює; для CMS з ідентифікацією за keyId і без
    // вкладеного сертифіката перевірки не буде взагалі, і підпис пройшов би тихо.
    // Тест фіксує ГІЛКУ, а не «властивість SIGN».
    // Компонента законно зареєструє помилку для 1С через REPORT_ERROR, тож у вивід
    // піде рядок [AddError] — ЦЕ ОЧІКУВАНО. Попереджаємо В САМОМУ ЛОЗІ, а не лише
    // коментарем: хибно прочитає це той, хто дивиться ВИВІД ГЕЙТА, а не вихідний код.
    printf("  ОЧІКУВАНО ДАЛІ: [AddError] і errorCode 4109 — це НЕГАТИВНА частина кейса\n");
    r = c.call("SIGN", buildSign());
    printf("  SIGN шифрувальним ключем: %s\n", r.c_str());
    const long ecBadUsage = errCode(r, j);
    // 4109 == 0x100D == RET_UAPKI_INVALID_KEY_USAGE (uapki-errors.h:58; Додаток А
    // extern/uapki/doc/UAPKI-PM-2.0.16.md). Рядок error — uapki-errors.c:170.
    CHECK(ecBadUsage == 4109,
          "SIGN шифрувальним ключем ВПАВ з 4109 (RET_UAPKI_INVALID_KEY_USAGE)");
    CHECK(j.value("error", std::string()) == "INVALID_KEY_USAGE",
          "error == INVALID_KEY_USAGE");
```

- [ ] **Крок 2: Зібрати й прогнати**

```
cmake --build build_x64 --config Release --target native_host
bin\Release\native_host_x64.exe 10
```
Очікується `PASS`. У виводі має бути видно рядок `SIGN шифрувальним ключем:` з `"errorCode":4109`.

**Якщо код виявиться іншим** — узяти фактичний із прогону, звірити з Додатком А `extern/uapki/doc/UAPKI-PM-2.0.16.md` і `uapki-errors.h`, виправити `CHECK` і **написати в коментарі, звідки число**. Якщо `SIGN` натомість пройшов з `errorCode 0` — це знахідка рівня спеки (страховки немає там, де ми її обіцяємо); зупинитись і повідомити людину.

- [ ] **Крок 3: Негативна верифікація негативної частини (критерій приймання 2)**

Тимчасово подати підписний сертифікат замість шифрувального — у рядку
```cpp
    { json p; p["certId"] = certIdEnc;
```
замінити `certIdEnc` на `certIdSign`. Зібрати, запустити.

Очікується **червоне** на `обрано саме ШИФРУВАЛЬНИЙ ключ` (і, якби цей `CHECK` прибрати, — на `SIGN ... ВПАВ з 4109`, бо `SIGN` підписним ключем пройде з `errorCode 0`). Це доводить, що червоним кейс стає саме від підміни ключа, а не від чогось стороннього. **Повернути `certIdEnc`**, зібрати, PASS.

- [ ] **Крок 4: Підключити кейс 10 до `run_tests.ps1`**

Точковим `Edit` (файл у UTF-8 з BOM — не перезаписувати цілком!) замінити коментар і заголовок циклу (зараз рядки 564–566):

```powershell
    # Кейс 6 (пароль не в лозі) не потребує SKIP-семантики — завжди PASS/FAIL, тож іде в
    # тому ж циклі, що й 1..4. Кейс 5 (діапазон ПРРО) навмисно НЕ в переліку: йому потрібен
    # окремий аргумент-каталог і власне трактування exit 3, тому він — окремим блоком нижче.
    foreach ($kase in 1,2,3,4,6) {
```
на
```powershell
    # Кейс 6 (пароль не в лозі) і кейс 10 (вибір ключа за сертифікатом) не потребують
    # SKIP-семантики — у них усе вхідне лежить у git, тож завжди PASS/FAIL, і вони йдуть
    # у тому ж циклі, що й 1..4. Кейс 5 (діапазон ПРРО) навмисно НЕ в переліку: йому
    # потрібен окремий аргумент-каталог і власне трактування exit 3, тому він — окремим
    # блоком нижче. Кейс 11 — теж окремим блоком (потребує local-keys.json).
    foreach ($kase in 1,2,3,4,6,10) {
```

- [ ] **Крок 5: Оновити шапку-коментар `run_tests.ps1`**

Замінити (зараз рядки 14–17):
```powershell
      L2/L3 — native_host.exe: e2e поверх ГОЛОВНОЇ DLL через IComponentBase (кейси 1..4, 6),
            крос-валідація ПРРО (кейс 5) — лише за наявності еталонів, реальні контейнери
            КНЕДП (кейс 7) — лише за наявності tests/data/local-keys.json (особистий КЕП,
            поза git). Обидва — SKIP (exit 3), не PASS, якщо вхідних даних немає.
```
на
```powershell
      L2/L3 — native_host.exe: e2e поверх ГОЛОВНОЇ DLL через IComponentBase (кейси 1..4, 6
            і 10 — вибір ключа за сертифікатом у двоключовому контейнері), крос-валідація
            ПРРО (кейс 5) — лише за наявності еталонів, реальні контейнери КНЕДП (кейс 7)
            і купинний jks (кейс 11, доказ зняття пастки 4161 через SELECT_KEY за certId) —
            лише за наявності tests/data/local-keys.json (особистий КЕП, поза git).
            Усі три — SKIP (exit 3), не PASS, якщо вхідних даних немає.
```

> Кейс 11 згадано наперед свідомо: шапка описує рівень цілком, а Task 3 додає лише виконавчий блок. Якщо Task 3 із якоїсь причини не буде зроблено, цей рядок доведеться прибрати — але розривати опис рівня на два коміти гірше.

- [ ] **Крок 6: Перевірити, що BOM на місці**

```
head -c 3 run_tests.ps1 | xxd -p
```
Очікується `efbbbf`. Якщо ні — правка зіпсувала кодування; відновити файл із `git show HEAD:run_tests.ps1` і повторити редагування точковим `Edit`.

- [ ] **Крок 7: Повний гейт на обох архітектурах**

```
cmake --build build_x86 --config Release --target native_host
powershell -ExecutionPolicy Bypass -File run_tests.ps1 x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 x86
```
Очікується: у таблиці з'явився рядок `[PASS   ] L2/L3 native_host case 10`, і **жоден інший рядок не змінив статусу** проти еталона з Task 1 кроку 1. Звіряти саме порядково: підсумкове `PASS=` може зрушити й без нашої участі, якщо середовище змінилось (звільнилась 1С — кейси 1/2 з червоних стали зеленими). Зміна статусу рядка, якого ми не чіпали, у будь-який бік — привід назвати причину вголос, а не тихо прийняти.

- [ ] **Крок 8: Коміт**

```bash
git add tests/native_host.cpp run_tests.ps1
git commit --only -- tests/native_host.cpp run_tests.ps1 -F - <<'EOF'
test(uapki): негативна частина кейса 10 + підключення до гейта

SIGN ключем шифрування падає з 4109 INVALID_KEY_USAGE — але лише в гілці
(формат != RAW) && (!sidUseKeyId || includeCert), sign.cpp:304-314. Тест
фіксує саме цю гілку, а не «властивість SIGN». Кейс 10 доданий у цикл
native_host у run_tests.ps1: тест, якого гейт не запускає, покриттям не є.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
```

---

### Task 2-біс: фікс-раунд за підсумком рев'ю Task 1+2

Дописано **після** здачі Task 1+2 (`cde0402`, `28c1e9c`). Три правки в тілі `case10_selectByCertId`, усі три — дефекти **плану**, а не виконання: імплементер відтворив написане точно.

**Files:**
- Modify: `tests/native_host.cpp` (тіло `case10_selectByCertId`)
- Test: `bin\Release\native_host_x64.exe 10` і `_x86.exe 10`

---

- [ ] **Крок 1: Прибрати латентний UB у `CHECK` по `k0`/`k1`**

Код у Task 1 крок 4 уже виправлено — взяти звідти. Суть: `contains()` мусить накривати **обидва** операнди порівняння, а не лише той, з якого починається вираз. `k0`/`k1` — це `const json&`, і константний `operator[]` на відсутньому ключі **не кидає винятку**, а дає UB: `json.hpp:22182-22193` містить самий `JSON_ASSERT`, а той (`json.hpp:2571`) — звичайний `assert`, який під `NDEBUG` конфігурації Release зникає.

- [ ] **Крок 2: Додати попередження про `[AddError]` у сам лог**

Код у Task 2 крок 1 уже виправлено — взяти звідти. Причина: перед падінням негативного `SIGN` компонента законно реєструє помилку для 1С через `REPORT_ERROR`, і очікувана помилка виглядає у виводі гейта як **дві**. Попередження йде `printf`-ом у лог, а не коментарем: коментар бачить той, хто читає вихідний код, а хибно читає це той, хто дивиться вивід гейта.

- [ ] **Крок 2-біс: Прибрати сентинельну маскіровку в `CHECK` по `id` ключів**

Знахідка рев'ю, той самий клас, теж дефект плану. Було:
```cpp
CHECK(k0.value("id", std::string("a")) != k1.value("id", std::string("b")), …);
```
Дефолти **різні**, тож коли `id` немає в **жодного** ключа, `value()` віддає `"a"` і `"b"`, вони не рівні, і `CHECK` проходить **хибно**. Перевірка стверджує розрізнення, а сентинели гарантують його самі по собі. Стало:
```cpp
CHECK(k0.contains("id") && k1.contains("id") && k0["id"] != k1["id"],
      "id ключів різні (є з чого обирати)");
```
Негативна верифікація: інвертувати `!=` → `==`, очікується `FAIL: id ключів різні (є з чого обирати)`. Дослівний рядок — у звіт.

- [ ] **Крок 3: Негативна верифікація ТВЕРДЖЕННЯ 1 — прогалина плану**

`docs/architecture/testing-rules.md`, правило 1 вимагає бачити червоним **кожен** новий `CHECK`. Task 1 кроки 7 і 8 і Task 2 крок 3 закрили твердження 2, 3 і негативну частину — а `CHECK` твердження 1 (`mechanismId` і `signAlgo` обох ключів однакові) червоним **не бачили жодного разу**. Це недогляд плану, і закривається він тут.

Тимчасово інвертувати обидва порівняння: `k0["mechanismId"] == k1["mechanismId"]` → `!=`, і те саме для `signAlgo`. Зібрати, запустити `native_host_x64.exe 10`.

Очікується **червоне** на
```
FAIL: mechanismId обох ключів ОДНАКОВИЙ
```
Що саме це доводить: порівняння справді **читає дані обох ключів і розрізняє їх**, а не є тавтологією. Без цього кроку `CHECK` лишався б зеленим і в разі, якби обидва боки давали `null` — а саме так поводився б код із помилкою в імені поля до правки кроку 1. **Повернути `==`**, зібрати, `PASS`.

> **Чого перевіряти НЕ треба.** Окремо доводити, що `CHECK` зелений «саме через `contains()` з обох боків, а не через коротке замикання», сенсу немає: коротке замикання в кон'юнкції дає **хибність**, а не істину. Зелений `CHECK` уже означає, що всі три кон'юнкти істинні, тобто обидва `contains()` спрацювали. Перевірка, висновок якої випливає з самого факту зеленого результату, — ритуал, а не доказ.

- [ ] **Крок 4: Перезібрати обидві архітектури й прогнати кейс 10**

```
cmake --build build_x64 --config Release --target native_host
cmake --build build_x86 --config Release --target native_host
bin\Release\native_host_x64.exe 10
bin\Release\native_host_x86.exe 10
```
Очікується `=== Case 10: PASS ===` і exit 0 на обох. Повний гейт тут **не потрібен**: змінюється лише тіло кейса 10, статус якого вже зафіксовано еталоном, а `run_tests.ps1` не чіпається.

- [ ] **Крок 5: Коміт**

Разом із будь-якими іншими знахідками рев'ю по цьому ж файлу — **одним** фікс-раундом, щоб не плодити два диспатчі й два рев'ю по одному файлу.

```bash
git add tests/native_host.cpp
git commit --only -- tests/native_host.cpp -F - <<'EOF'
fix(test): кейс 10 — прибрати UB у CHECK по k0/k1 і попередити про [AddError]

contains() охороняло лише k0, тоді як k1["mechanismId"]/k1["signAlgo"] на
відсутньому ключі дають UB, а не виняток: json.hpp:22182-22193 має самий
JSON_ASSERT, який під NDEBUG (Release) зникає. Охорона тепер з обох боків.

Перед падінням негативного SIGN компонента законно друкує [AddError]
(REPORT_ERROR реєструє помилку для 1С), і очікувана помилка виглядає у
виводі гейта як дві. Попередження додано В САМ ЛОГ: коментар бачить той,
хто читає код, а хибно читає це той, хто дивиться вивід гейта.

Заразом закрито прогалину плану: CHECK твердження 1 (однакові mechanismId
і signAlgo) жодного разу не бачили червоним. Інверсія порівняння дає
"FAIL: mechanismId обох ключів ОДНАКОВИЙ" — отже перевірка читає дані
обох ключів, а не є тавтологією.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
```

---

### Task 3: Кейс 11 — `jks-kupyna`, двофазний шлях і зняття пастки 4161

**Files:**
- Modify: `tests/native_host.cpp` (тіло кейса — після `case10_selectByCertId`, перед `// main / CLI`; `usage()`; `main()` — діапазон, `switch`, група `skipped`)
- Modify: `run_tests.ps1` (новий блок між кінцем блоку кейса 7 і закриваючою `}` секції L2/L3 — якір за вмістом, див. крок 8)
- Test: `bin\Release\native_host_x64.exe 11`, повний гейт x64 і x86

**Interfaces:**
- Consumes: `LoadLocalKeys`/`FindLocalKey`/`LocalKey` з `tests/support/LocalKeys.h`, хелпери `elideCert`, `buildInit`, `errCode`, `CHECK`, `DATA_TBS_B64`.
- Produces: `static bool case11_jksSelectByCertId(const std::wstring& binDir, const std::wstring& dataDir, bool& skipped)` — семантика `skipped` така сама, як у кейсів 5/7/8: `true` → `main()` віддає **exit 3 (SKIPPED)**, ніколи не PASS.

---

- [ ] **Крок 1: Написати тіло кейса 11 із ЛОГУЮЧИМИ (ще не затягнутими) лічильниками**

Вставити після `case10_selectByCertId`, перед `// ======== main / CLI ========`:

```cpp
// ========================================================================
// КЕЙС 11 — jks-kupyna: SELECT_KEY(certId) знімає пастку 4161 за побудовою
// ========================================================================
// Кейс 10 твердження 4 довести не може: test-diia старої схеми, SKI там рахований
// ГОСТом і збігається з key.id — пастці немає де спрацювати. Тут контейнер нового
// зразка з КУПИННИМ SKI, тобто пастка жива (саме її кейс 8 обходить через keyId2).
// Доводиться, що шлях через certId робить обхід НЕПОТРІБНИМ.
//
// Сертифікати лежать УСЕРЕДИНІ контейнера, тому шлях двофазний: перший SELECT_KEY
// за id наповнює кеш сертифікатами контейнера НАВІТЬ тоді, коли сам повертає
// errorCode 0 без certId — addCerts іде на session-select-key.cpp:128-137, ДО пошуку
// сертифіката на :139-144, а ковтання CERT_NOT_FOUND — аж на :152-154.
//
// SKIP (exit 3) без tests/data/local-keys.json або без ключа 'jks-kupyna': купинного
// ключа в репозиторії немає й бути не може, тож поза цією машиною доказ не відтворюється.
// Це названо, а не замовчано.
static bool case11_jksSelectByCertId(const std::wstring& binDir, const std::wstring& dataDir,
                                     bool& skipped) {
    printf("== Case 11: jks-kupyna — двофазний шлях до SELECT_KEY(certId) ==\n");
    skipped = false;

    std::vector<LocalKey> keys;
    std::string err;
    if (!LoadLocalKeys(dataDir + L"\\local-keys.json", keys, err)) {
        printf("FAIL: конфіг зіпсований: %s\n", err.c_str());
        return false;                       // конфіг є -> наміри заявлені -> FAIL
    }
    const LocalKey* k = FindLocalKey(keys, "jks-kupyna");
    if (!k) {
        printf("SKIP (немає ключа 'jks-kupyna' у local-keys.json)\n");
        skipped = true;
        return true;
    }

    const std::wstring dllName = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    Component c;
    if (!c.load(binDir + L"\\" + dllName)) return false;

    json j;
    std::string r = c.call("INIT", buildInit(true));
    CHECK(errCode(r, j) == 0, "INIT errorCode == 0");

    { json op;
      op["provider"] = "PKCS12";            // провайдер один: детект іде за ВМІСТОМ
      op["storage"]  = k->path;
      op["password"] = k->password;         // пароль НЕ друкуємо — це особистий КЕП
      op["mode"]     = "RO";
      r = c.call("OPEN", op.dump()); }
    CHECK(errCode(r, j) == 0, "OPEN errorCode == 0");

    // Сирий KEYS друкуємо ПОВНІСТЮ: скільки ключів UAPKI бачить у цьому JKS — не міряно
    // ніде, а відповідь потрібна сусідній сесії (спека §7 крок 2, §9 критерій 4).
    r = c.call("KEYS", "");
    printf("  KEYS (сирий result): %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "KEYS errorCode == 0");
    CHECK(j["result"].contains("keys") && j["result"]["keys"].is_array()
          && !j["result"]["keys"].empty(), "result.keys непорожній");
    printf("  ВИМІР: keys.size() == %zu\n", j["result"]["keys"].size());
    const std::string keyId = j["result"]["keys"][0].value("id", std::string());
    CHECK(!keyId.empty(), "id першого ключа отримано");

    // --- ФАЗА 1: наповнити кеш ---
    // certId тут може НЕ прийти — це і є пастка. Вердикт ФІКСУЄМО ВИМІРОМ, а не CHECK:
    // обидва результати законні, і саме результат тут цікавий.
    { json p; p["id"] = keyId; r = c.call("SELECT_KEY", p.dump()); }
    printf("  SELECT_KEY фаза 1 (за id): %s\n", elideCert(r).c_str());
    CHECK(errCode(r, j) == 0, "SELECT_KEY фаза 1 errorCode == 0");
    const bool certIdInPhase1 = j["result"].contains("certId");
    printf("  ВИМІР: certId після фази 1 %s\n",
           certIdInPhase1 ? "ПРИЙШОВ — пастка 4161 на цьому ключі НЕ жива, зафіксувати й повідомити"
                          : "НЕ прийшов — пастка 4161 жива (очікувано)");

    // --- Доказ, що фаза 1 наповнила кеш попри відсутність certId ---
    { json lp; lp["showCertInfos"] = true; r = c.call("LIST_CERTS", lp.dump()); }
    CHECK(errCode(r, j) == 0, "LIST_CERTS errorCode == 0");
    CHECK(j["result"].contains("certInfos") && j["result"]["certInfos"].is_array()
          && !j["result"]["certInfos"].empty(),
          "кеш НЕ порожній після фази 1 (сертифікати контейнера вже там)");
    printf("  ВИМІР: certInfos.size() == %zu\n", j["result"]["certInfos"].size());

    // Підписний сертифікат = keyUsage.digitalSignature І НЕ сертифікат ЦСК.
    // ПІДСТАВА ФІЛЬТРА isCa — стандарт, а НЕ спостереження: RFC 5280 дозволяє
    // CA-сертифікату нести digitalSignature, але ЖОДЕН CA у цьому репозиторії його
    // не несе (зміряно openssl: обидва CA:TRUE у tests/data/certs/ мають рівно
    // "Certificate Sign, CRL Sign"). Тобто на наявних ланцюгах фільтр відсіює ті самі
    // сертифікати, що й сама умова digitalSignature, і розрізнювальним НЕ стає.
    // Лишаємо на випередження — на чужому ланцюгу він розрізнятиме; але не вдаємо,
    // що його перевірено. keyUsage іде ОКРЕМИМ полем certInfos[]
    // (list-certs.cpp:135-136), тож CERT_INFO по кожному certId не потрібен.
    // Суб'єкт у консоль НЕ виносимо — це особистий КЕП; для рішення досить keyUsage/isCa.
    std::vector<std::string> candidates;
    size_t caSeen = 0;   // скільки CA-сертифікатів фільтр реально відсіяв
    for (const auto& ci : j["result"]["certInfos"]) {
        const bool isCa = ci.value("isCa", false);
        if (isCa) ++caSeen;
        const json ku   = ci.value("keyUsage", json::object());
        const bool ds   = ku.value("digitalSignature", false);
        printf("  cert %s... isCa=%s keyAlgo=%s keyUsage=%s\n",
               ci.value("certId", std::string()).substr(0, 16).c_str(),
               isCa ? "true" : "false",
               ci.value("keyAlgo", std::string()).c_str(), ku.dump().c_str());
        if (ds && !isCa) candidates.push_back(ci.value("certId", std::string()));
    }
    printf("  ВИМІР: кандидатів на підпис (digitalSignature && !isCa) == %zu\n", candidates.size());
    printf("  ВИМІР: CA-сертифікатів у кеші == %zu\n", caSeen);
    // Доказ, що фільтр isCa не працює ВХОЛОСТУ: у кеші справді є сертифікати ЦСК.
    // Це НЕ заміна негативної верифікації — розрізнювальний випадок (CA З
    // digitalSignature) на цьому ланцюгу не трапляється, і крафтити його ми не будемо, —
    // але це відрізняє «фільтр відсіяв CA» від «фільтрувати не було чого».
    CHECK(caSeen > 0, "у кеші є сертифікати ЦСК — фільтру isCa є що відсівати");
    CHECK(!candidates.empty(), "є щонайменше один НЕ-CA сертифікат із keyUsage.digitalSignature");

    // --- ФАЗА 2: правильна зв'язка ---
    // У гілці certId certId присутній ЗАВЖДИ: getCertByCertId уже успішно відпрацював на
    // session-select-key.cpp:81, а між ним і повторним пошуком на :141 лише addCerts
    // (:131) — додає, не видаляє. Отже CERT_NOT_FOUND на :152 тут недосяжний.
    { json p; p["certId"] = candidates[0]; r = c.call("SELECT_KEY", p.dump()); }
    printf("  SELECT_KEY фаза 2 (за certId): %s\n", elideCert(r).c_str());
    CHECK(errCode(r, j) == 0, "SELECT_KEY фаза 2 errorCode == 0");
    CHECK(j["result"].contains("certId"), "фаза 2 повернула certId (ТВЕРДЖЕННЯ 4)");
    CHECK(j["result"].value("certId", std::string()) == candidates[0],
          "повернутий certId == запитаному");

    // --- Купинний підпис БЕЗ обхідного keyId2 і БЕЗ 4161 ---
    json sp;
    sp["signatureFormat"]  = "CAdES-BES";
    sp["signAlgo"]         = "1.2.804.2.1.1.1.1.3.6.1.1";   // ДСТУ4145 + Купина-256
    sp["detachedData"]     = false;                          // enveloping
    sp["includeCert"]      = true;
    sp["includeTime"]      = true;
    sp["includeContentTS"] = false;
    json d; d["id"] = "doc-0"; d["bytes"] = DATA_TBS_B64;
    json p;
    p["signParams"] = sp;
    p["dataTbs"]    = json::array({ d });
    p["options"]["ignoreCertStatus"] = true;
    r = c.call("SIGN", p.dump());
    if (errCode(r, j) != 0) printf("  SIGN (повна відповідь): %s\n", r.c_str());
    CHECK(errCode(r, j) == 0, "SIGN errorCode == 0 — БЕЗ 4161 і БЕЗ обхідного keyId2");
    // Структуру перевіряємо ДО витягання: .value() на null кидає type_error, і кейс
    // упав би FATAL-винятком замість зрозумілого FAIL. Охоронний порядок той самий,
    // що в кейсі 10.
    CHECK(j["result"].contains("signatures") && j["result"]["signatures"].is_array()
          && !j["result"]["signatures"].empty(), "result.signatures непорожній");
    const std::string sig = j["result"]["signatures"][0].value("bytes", std::string());
    CHECK(!sig.empty(), "підпис не порожній");

    // VERIFY тут — РЕГРЕСІЙНА перевірка (наш код підтверджує наш код); доказ коректності
    // купинного підпису дає арбітр ІІТ на рівні L4-iit через кейс 8.
    // Пастка ПРРО: алгоритм СЕРТИФІКАТА не визначає алгоритм ПІДПИСУ — дивимось саме
    // в SignerInfo, який UAPKI віддає в signatureInfos[].
    { json vp; vp["signature"]["bytes"] = sig; r = c.call("VERIFY", vp.dump()); }
    CHECK(errCode(r, j) == 0, "VERIFY errorCode == 0 (регресія)");
    CHECK(j["result"].contains("signatureInfos") && j["result"]["signatureInfos"].is_array()
          && !j["result"]["signatureInfos"].empty(), "signatureInfos є");
    // УВАГА: si — ПОСИЛАННЯ в j. Будь-який наступний errCode(r, j) зробить його
    // висячим: повне присвоєння json знищує вузли разом зі сховищем. Нижче j більше
    // не перезаписується — не переставляй блоки; треба безпечніше, копіюй значення.
    const auto& si = j["result"]["signatureInfos"][0];
    printf("  SignerInfo: signAlgo=%s digestAlgo=%s statusSignature=%s statusMessageDigest=%s\n",
           si.value("signAlgo", std::string()).c_str(),
           si.value("digestAlgo", std::string()).c_str(),
           si.value("statusSignature", std::string()).c_str(),
           si.value("statusMessageDigest", std::string()).c_str());
    CHECK(si.value("signAlgo", std::string()) == "1.2.804.2.1.1.1.1.3.6.1.1",
          "signAlgo у SignerInfo == ДСТУ4145 з Купиною-256");
    CHECK(si.value("digestAlgo", std::string()) == "1.2.804.2.1.1.1.1.2.2.1",
          "digestAlgo у SignerInfo == Купина-256");
    // Самого statusSignature недостатньо: він лишається VALID навіть при пошкодженому
    // вмісті. Підміну вмісту ловить саме statusMessageDigest.
    CHECK(si.value("statusMessageDigest", std::string()) == "VALID",
          "statusMessageDigest == VALID");

    c.call("CLOSE", "");
    c.call("DEINIT", "");
    c.unload();
    return true;
}
```

- [ ] **Крок 2: Розширити CLI до кейса 11**

У `usage()` замінити `<case 1..10>` на `<case 1..11>`.

У `main()` замінити `if (kase < 1 || kase > 10)` на `if (kase < 1 || kase > 11)`.

У `switch (kase)` після рядка `case 10:` додати:
```cpp
            case 11: pass = case11_jksSelectByCertId(binDir, dataDir, skipped); break;
```

У коментарі оголошення `skipped` (зараз рядок ~1199) замінити
```cpp
    bool skipped = false;   // кейси 5, 7, 8: вхідних даних немає -> це НЕ покриття (exit 3)
```
на
```cpp
    bool skipped = false;   // кейси 5, 7, 8, 11: вхідних даних немає -> це НЕ покриття (exit 3)
```

- [ ] **Крок 3: Зібрати й прогнати перший (вимірювальний) раз**

```
cmake --build build_x64 --config Release --target native_host
bin\Release\native_host_x64.exe 11
```

Виписати з виводу три виміри — це **вхід для наступного кроку** і водночас відповідь сусідній сесії (критерій приймання 4):
- `ВИМІР: keys.size() == N`
- `ВИМІР: certId після фази 1 …`
- `ВИМІР: certInfos.size() == M`, `ВИМІР: кандидатів на підпис … == C`

**Якщо `certId` після фази 1 ПРИЙШОВ** — це знахідка (пастка на цьому ключі не жива): зафіксувати повний вивід і повідомити людину перед тим, як іти далі. Кейс від цього не червоніє й лишається валідним доказом твердження 4 у частині фази 2, але висновок спеки про «пастку» для цього ключа потребує уточнення.

**Якщо `SIGN` упав з 4161** — теж зупинитись: це означало б, що шлях через `certId` пастку не знімає, тобто спростовано твердження 4. Зберегти повний вивід і повідомити.

- [ ] **Крок 4: Затягнути лічильники за виміром**

Замінити два «м'яких» `CHECK` на точні, підставивши зміряні числа `N` і `C` із кроку 3, і дописати коментар із походженням числа.

```cpp
    CHECK(j["result"].contains("keys") && j["result"]["keys"].is_array()
          && !j["result"]["keys"].empty(), "result.keys непорожній");
```
→
```cpp
    // Число з ПРОГОНУ 2026-__-__ на pb_*.jks, не з розрахунку: структурний розбір
    // контейнера показував один запис приватного ключа з ланцюгом із 4 сертифікатів,
    // а скільки з цього стане елементами keys[] — питання до UAPKI, не до ASN.1.
    CHECK(j["result"].contains("keys") && j["result"]["keys"].is_array()
          && j["result"]["keys"].size() == N, "keys.size() == N (зміряно)");
```

```cpp
    CHECK(!candidates.empty(), "є щонайменше один НЕ-CA сертифікат із keyUsage.digitalSignature");
```
→
```cpp
    // Так само за фактом прогону: у ланцюгу JKS лежать корінь, КНЕДП, підписний і
    // шифрувальний сертифікати, і рівно один із них НЕ-CA з digitalSignature.
    CHECK(candidates.size() == C, "рівно C кандидат(и) на підпис (зміряно)");
```

Замінити `N` і `C` реальними числами; дату в коментарі — реальною датою прогону. Перезібрати, прогнати — має лишитись `PASS`.

- [ ] **Крок 5: Негативна верифікація — фільтр `isCa`**

Тимчасово прибрати фільтр CA: замінити
```cpp
        if (ds && !isCa) candidates.push_back(ci.value("certId", std::string()));
```
на
```cpp
        if (ds) candidates.push_back(ci.value("certId", std::string()));
```
Зібрати, запустити.

**Передбачення: кейс лишиться ЗЕЛЕНИМ, і це очікуваний результат, а не невдача.** Підстава — вимір по репозиторію (`openssl x509 -ext basicConstraints -ext keyUsage` по всіх `tests/data/certs/*.cer`): обидва сертифікати з `CA:TRUE` несуть рівно `Certificate Sign, CRL Sign`, жоден не має `digitalSignature`. Отже на наявних ланцюгах умова `digitalSignature` і фільтр `!isCa` відсівають **ті самі** сертифікати, і розрізнювальним фільтр не стає.

Червоне тут означало б протилежне — що в ланцюгу JKS таки є CA з `digitalSignature`; це була б знахідка, і її треба зафіксувати повним виводом інвентарю.

У будь-якому разі: **фільтр `!isCa` лишити** (RFC 5280 дозволяє CA нести `digitalSignature`, і на чужому ланцюгу він розрізнятиме), **`!isCa` повернути**, а в звіті написати прямо: **червоним цей фільтр не бачили, його підстава — стандарт, а не спостереження**. Мовчки видавати неперевірену перевірку за перевірену не можна.

Те, що фільтр не працює **вхолосту**, доводить інша перевірка — `CHECK(caSeen > 0, …)` із кроку 1: вона стверджує, що CA-сертифікати в кеші справді є, тобто фільтру є що відсівати. Це не заміна негативної верифікації, а відповідь на інше питання: «відсіяв» проти «не було чого відсівати».

- [ ] **Крок 6: Негативна верифікація — твердження 4 (фаза 2 справді потрібна й справді працює)**

Тимчасово замінити фазу 2 на вибір за `id` (тобто зімітувати «ми не пішли через certId»):
```cpp
    { json p; p["certId"] = candidates[0]; r = c.call("SELECT_KEY", p.dump()); }
```
→
```cpp
    { json p; p["id"] = keyId; r = c.call("SELECT_KEY", p.dump()); }
```
Зібрати, запустити. Очікується **червоне** на `фаза 2 повернула certId (ТВЕРДЖЕННЯ 4)` — або, якщо `certId` усе ж прийде, на `повернутий certId == запитаному`. Це і є доказ, що зеленим кейс робить саме шлях через `certId`. **Повернути назад**, зібрати, PASS.

- [ ] **Крок 7: Перевірити SKIP-гілку харнеса — БЕЗ дотику до `local-keys.json`**

`tests/data/local-keys.json` — особистий КЕП розробника поза git; відновити його неможливо. Тому гілка SKIP перевіряється **підміною `dataDir`**, а не перейменуванням файла: сесія, що впаде між двома `mv`, лишила б файл зміщеним, і жоден git тут не допоможе.

Підміна законна, бо `dataDir` — **третій позиційний аргумент**: у `main()` це рядок
```cpp
    std::wstring dataDir = argAt(3)[0] ? std::wstring(argAt(3)) : u8to16(HOST_DATA_DIR);
```
а кейс 11 використовує `dataDir` **рівно в одному місці** — `LoadLocalKeys(dataDir + L"\\local-keys.json", keys, err)` (той самий шаблон, що в кейса 8). Контейнер адресується абсолютним шляхом із самого конфігу, тож більше `dataDir` кейсу 11 ні для чого не потрібен.

```
mkdir -p "$TEMP/sac_nokeys"
bin/Release/native_host_x64.exe 11 \
    "$PWD/bin/Release/SimplyAddinConnectWin_x64.dll" \
    "$TEMP/sac_nokeys" \
    "$PWD/bin/Release"
echo "exit=$?"
```

Усі чотири аргументи задаються **явно**. Порожній `""` на місці `mainDll` теоретично теж спрацював би (`if (argAt(2)[0]) mainDll = argAt(2); else mainDll = binDir + L"\\SimplyAddinConnectWin" + ARCH_W + L".dll";` у `main()` — `argAt(2)[0]` дав би `false`, і шлях порахувався б із `binDir`), але порожній аргумент у лапках по-різному переживає передачу через різні оболонки, і покладатись на це всередині перевірки, яка сама себе доводить, не варто.

Очікується `SKIP (немає ключа 'jks-kupyna' у local-keys.json)`, `=== Case 11: SKIPPED ===`, **exit 3**.

Довести, що `tests/data/` не змінився взагалі:
```
git status --short tests/data/
ls tests/data/local-keys.json
```

- [ ] **Крок 8: Підключити кейс 11 до `run_tests.ps1`**

Точковим `Edit` вставити новий блок **після блоку кейса 7 і перед закриваючою `}` секції L2/L3**. Адресуйся за вмістом, не за номером: Task 2 уже посунув цей фрагмент, і він посунеться ще. Унікальний якір — рядок
```powershell
    else                       { Add-Result 'L2/L3' 'native_host case 7' 'FAIL' "exit=$($p.ExitCode) $lastLine" }
```
(`grep -c "native_host case 7' 'FAIL'" run_tests.ps1` має дати рівно `1`). Одразу за ним іде `Remove-Item $outF, "$outF.err" -ErrorAction SilentlyContinue` — сам по собі він **не** якір, бо трапляється в кількох блоках, — а вже за ним закриваюча `}`. Новий блок іде **між тим `Remove-Item` і тією `}`**:

```powershell

    # Кейс 11 — jks-kupyna: доказ, що SELECT_KEY за certId знімає пастку 4161 на купинному
    # SKI (кейс 8 її обходить через keyId2 — обидва твердження цінні, кейс 8 не чіпаємо).
    # SKIP-семантика як у кейсів 5/7: купинного ключа в репо немає й бути не може, тож
    # поза цією машиною exit 3 — і це НЕ FAIL. Шлях у деталі SKIP навмисний: хто дивиться
    # в таблицю, має отримати готову дію, а не йти в код за поясненням.
    $argList = @('11', "`"$MainDll`"", "`"$DataDir`"", "`"$BinRelease`"")
    $outF = Join-Path ([System.IO.Path]::GetTempPath()) ("nh_11_" + [guid]::NewGuid().ToString('N').Substring(0,6) + '.out')
    $p = Start-Process -FilePath $NativeHostExe -ArgumentList $argList `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
    $txt = if (Test-Path $outF) { Get-Content -Raw $outF } else { '' }
    $lastLine = ($txt -split "`n" | Where-Object { $_ -match '\S' } | Select-Object -Last 1)
    # Виміри кейса (keys.size, certId після фази 1, кількість кандидатів) — єдине джерело
    # відповіді на питання «скільки ключів UAPKI бачить у цьому JKS», тож піднімаємо їх
    # у консоль гейта, а не лишаємо у видаленому тимчасовому файлі.
    foreach ($m in ($txt -split "`n" | Where-Object { $_ -match 'ВИМІР:' })) {
        Write-Host ("          " + $m.Trim()) -ForegroundColor DarkGray
    }
    if     ($p.ExitCode -eq 0) { Add-Result 'L2/L3' 'native_host case 11' 'PASS' $lastLine }
    elseif ($p.ExitCode -eq 3) { Add-Result 'L2/L3' 'native_host case 11' 'SKIP' "$LocalKeysJson відсутній або без ключа 'jks-kupyna'" }
    else                       { Add-Result 'L2/L3' 'native_host case 11' 'FAIL' "exit=$($p.ExitCode) $lastLine" }
    Remove-Item $outF, "$outF.err" -ErrorAction SilentlyContinue
```

- [ ] **Крок 9: Довести SKIP-гілку САМОГО гейта, а не лише харнеса**

Крок 7 довів, що харнес віддає `exit 3`. Це ще **не** доказ, що гейт покаже `SKIP`: між ними лежить гілка `elseif ($p.ExitCode -eq 3)` у щойно доданому блоці. На цій машині конфіг є, тож **ця гілка не виконається ніколи** — і помилка в ній доживе до чужої машини, де проявиться як «ваша зміна зробила мій гейт червоним». Найімовірніша помилка тут не логічна, а текстова: `case 7` у мітці `Add-Result`, скопійований разом зі структурою блоку.

Тимчасово підмінити `dataDir` у блоці кейса 11 — у рядку
```powershell
    $argList = @('11', "`"$MainDll`"", "`"$DataDir`"", "`"$BinRelease`"")
```
замінити `$DataDir` на шлях порожнього тимчасового каталогу з кроку 7. Прогнати **лише x64** — гілка PowerShell від архітектури не залежить:
```
powershell -ExecutionPolicy Bypass -File run_tests.ps1 x64
```
Очікується в таблиці рівно `[SKIP   ] L2/L3 native_host case 11 — …відсутній або без ключа 'jks-kupyna'`. **Повернути `$DataDir`**, перевірити BOM, і тільки потім іти на крок 10.

Якщо зайвий повний прогін гейта тут надто дорогий — допустима заміна: прочитати доданий блок очима й переконатися, що в усіх трьох `Add-Result` стоїть `case 11`, а в `$argList` — `'11'`. Але тоді **у звіті написати, що гілку перевірено читанням, а не прогоном**. Видавати одне за інше не можна: саме так з'являються перевірки, яких ніхто ніколи не бачив працюючими.

- [ ] **Крок 10: Перевірити BOM і повний гейт на обох архітектурах**

```
head -c 3 run_tests.ps1 | xxd -p
cmake --build build_x86 --config Release --target native_host
powershell -ExecutionPolicy Bypass -File run_tests.ps1 x64
powershell -ExecutionPolicy Bypass -File run_tests.ps1 x86
```
Очікується: `efbbbf`; у таблиці обох прогонів — `[PASS   ] L2/L3 native_host case 11` і рядки `ВИМІР:` під ним; і **жоден інший рядок не змінив статусу** проти еталона з Task 1 кроку 1 — звіряти порядково, не за сумою. Скопіювати рядки `ВИМІР:` у підсумкове повідомлення — це відповідь сусідній сесії `smp-simplyconnect-82`.

- [ ] **Крок 11: Коміт**

```bash
git add tests/native_host.cpp run_tests.ps1
git commit --only -- tests/native_host.cpp run_tests.ps1 -F - <<'EOF'
test(uapki): кейс 11 — SELECT_KEY за certId знімає пастку 4161 на купинному SKI

Двофазний шлях для контейнера з сертифікатами всередині: перший SELECT_KEY за id
наповнює кеш навіть без certId у відповіді (session-select-key.cpp:128-137 іде ДО
пошуку на :139-144), далі LIST_CERTS знаходить НЕ-CA сертифікат із
keyUsage.digitalSignature, і SELECT_KEY за його certId дає правильну зв'язку.
Купинний підпис проходить без обхідного keyId2 і без 4161. Без local-keys.json —
exit 3 (SKIP), ніколи не PASS. Лічильники — за фактом прогону, не за здогадом.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
```

---

### Task 4: Документація — `certId` як рекомендований шлях

**Files:**
- Modify: `docs/integration-1c/uapki.md` (§4.3, зараз рядки 148–193)
- Modify: `docs/architecture/uapki.md` (§9.1, зараз рядки 452–496)
- Test: `python scripts/check-doc-anchors.py`

**Interfaces:**
- Consumes: виміри з Task 3 крок 3 (чи приходить `certId` у фазі 1 на купинному ключі) — вони підтверджують або уточнюють формулювання двофазного рецепта.
- Produces: нічого для наступних завдань (останнє завдання плану).

---

- [ ] **Крок 1: Зафіксувати базову лінію гарду якорів**

```
python scripts/check-doc-anchors.py --quiet
echo $?
```
Очікується порожній вивід і `0`. Якщо вже червоно — зупинитись і повідомити: інакше неможливо буде відрізнити свій злам від успадкованого.

- [ ] **Крок 2: Переписати `docs/integration-1c/uapki.md` §4.3**

Замінити **весь** блок від заголовка `### 4.3. KEYS → SELECT_KEY — вибрати активний ключ` до (не включно) `### 4.4. SIGN — підписати дані` на:

````markdown
### 4.3. `KEYS` → `SELECT_KEY` — вибрати активний ключ

**Рекомендований шлях — `SELECT_KEY` за `certId`, а не за `id` ключа.**

Український КЕП-контейнер **завжди** містить дві ключові пари — підпис і шифрування, — і `KEYS`
розрізнити їх **не може**: в обох однаковий `mechanismId`, однаковий `signAlgo[]`, а `label`
береться з `friendlyName` і контрактом не є. Єдине джерело правди про призначення ключа —
**сертифікат** (розширення `keyUsage`). Подавши `certId` підписного сертифіката, ти водночас
(1) обираєш саме той ключ із двох і (2) обходиш пастку двох ідентифікаторів (врізка нижче) —
бо взагалі не вгадуєш, який із них передати.

#### Шлях А — сертифікат уже є в 1С (звичайний випадок)

Сертифікат зберігається в базі, у контейнері — лише ключі.

```bsl
// 1. Покласти сертифікат у кеш бібліотеки. permanent не задаємо -> тільки на час сесії,
//    на диск нічого не пишеться.
Р = UAPKI("ADD_CERT", "{""certificates"":[""" + СертифікатBase64 + """]}");
Если Р.errorCode <> 0 Тогда Сообщить("ADD_CERT: " + Р.error); Возврат; КонецЕсли;
CertId = Р.result.added[0].certId;
// Передавай по ОДНОМУ сертифікату: тоді added[0] однозначний. Для масиву з кількох
// сертифікатів порядок added[] у документації бібліотеки не зафіксовано — не спирайся
// на нього, а класифікуй за вмістом (див. шлях Б).

// 2. Обрати ключ ЗА СЕРТИФІКАТОМ.
Р = UAPKI("SELECT_KEY", "{""certId"":""" + CertId + """}");
Если Р.errorCode <> 0 Тогда Сообщить("SELECT_KEY: " + Р.error); Возврат; КонецЕсли;
// У цій гілці certId у відповіді присутній ЗАВЖДИ — механізм в
// [architecture/uapki.md](../architecture/uapki.md) §9.1
```

#### Шлях Б — сертифікати всередині контейнера (JKS і подібні)

Проблема курки і яйця: щоб зробити `SELECT_KEY` за `certId`, сертифікат має вже бути в кеші, а
дістає його з контейнера саме `SELECT_KEY`. Розв'язок — **два проходи**: перший `SELECT_KEY` за
`id` наповнює кеш сертифікатами контейнера **навіть тоді, коли сам повертає `errorCode: 0` без
`certId`**.

```bsl
Ключі = UAPKI("KEYS", "");
// Фаза 1: наповнити кеш. certId тут може НЕ прийти — це нормально й очікувано.
UAPKI("SELECT_KEY", "{""id"":""" + Ключі.result.keys[0].id + """}");

// Фаза 2: знайти підписний сертифікат серед тих, що потрапили в кеш.
Р = UAPKI("LIST_CERTS", "{""showCertInfos"":true}");
CertId = "";
Для Каждого Інфо Из Р.result.certInfos Цикл
    // Сертифікати ЦСК із ланцюга пропускаємо
    Если Інфо.Свойство("isCa") И Інфо.isCa Тогда Продолжить; КонецЕсли;   // стандарт дозволяє їм digitalSignature
    Если Інфо.Свойство("keyUsage") И Інфо.keyUsage.Свойство("digitalSignature") Тогда
        CertId = Інфо.certId;
        Прервать;
    КонецЕсли;
КонецЦикла;
Если ПустаяСтрока(CertId) Тогда Сообщить("Підписного сертифіката в контейнері немає"); Возврат; КонецЕсли;

Р = UAPKI("SELECT_KEY", "{""certId"":""" + CertId + """}");
```

`keyUsage` у `LIST_CERTS` з `showCertInfos: true` іде **окремим полем кожного `certInfos[]`**
(`list-certs.cpp:135-136`) — окремий `CERT_INFO` по кожному `certId` для цього не потрібен.
**У `keyUsage` присутні ЛИШЕ виставлені біти** (`extension-helper-json.cpp:410-418`): відсутність
`digitalSignature` означає «не підписний», а не «невідомо».

#### Запасний шлях — перебір `keyId2` → `id`

Лишається для контейнерів, до яких сертифіката немає **взагалі** — ні в базі, ні всередині.
Підписати з `includeCert: true` таким ключем усе одно не вийде, але сам ключ вибереться.

```bsl
Ключ = UAPKI("KEYS", "").result.keys[0];
Кандидати = Новый Массив;
Если Ключ.Свойство("keyId2") Тогда Кандидати.Добавить(Ключ.keyId2); КонецЕсли;
Кандидати.Добавить(Ключ.id);

СертифікатЗнайдено = Ложь;
Для Каждого Ид Из Кандидати Цикл
    Р = UAPKI("SELECT_KEY", "{""id"":""" + Ид + """}");
    // errorCode = 0 тут НЕ означає, що сертифікат знайдено: ключ без сертифіката легальний
    Если Р.errorCode = 0 И Р.result.Свойство("certId") Тогда
        СертифікатЗнайдено = Истина;
        Прервать;
    КонецЕсли;
КонецЦикла;

Если НЕ СертифікатЗнайдено Тогда
    Сообщить("Ключ вибрано, але сертифіката до нього немає — SIGN з includeCert:true дасть 4161");
КонецЕсли;
```

Після `SELECT_KEY` у `result` повертається сертифікат ключа (`certId` + `certificate`, Base64) —
якщо він є в контейнері/кеші **й знайшовся за переданим ідентифікатором** (див. врізку).

> **Пастка двох ідентифікаторів (тільки ДСТУ) — стосується ЗАПАСНОГО шляху.**
> ДСТУ-ключ має **два** ідентифікатори: `id` (геш ГОСТ 34311 відкритого ключа) і `keyId2`
> (Купина-256 того самого ключа). Сертифікати **нового зразка** несуть `SubjectKeyIdentifier`
> **на Купині**, а бібліотека шукає сертифікат за тим самим ідентифікатором, яким ти покликав
> `SELECT_KEY`. Тому виклик зі звичним `id` на новому ключі:
> - **ключ вибере** — сховище приймає обидва ідентифікатори;
> - **сертифікат не знайде** і поверне **`errorCode: 0` без поля `certId`**. Це легальна поведінка
>   бібліотеки (ключ без сертифіката дозволений), а не помилка, тому в лог нічого не потрапить;
> - а впаде вже наступний `SIGN` з `includeCert: true` — непрозорим **`4161 CERT_NOT_FOUND`**,
>   при тому що сертифікат Є і в контейнері, і в кеші.
>
> **Наявність `keyId2` НЕ означає, що пастка жива.** `keyId2` бібліотека **обчислює** з
> відкритого ключа для **кожного** ДСТУ-ключа (`session-list-keys.cpp:108-111`,
> `session-select-key.cpp:115-118`) — це не ознака «нового зразка» й не поле з контейнера.
> Пастка жива лише тоді, коли `SubjectKeyIdentifier` **сертифіката** порахований Купиною,
> а це з `KEYS` не видно взагалі. Зміряно на `test-diia.p12`: `keyId2` присутній в обох
> ключів, а пастка не жива — SKI там рахований ГОСТом і дорівнює `id`.
>
> З **01.09.2026** усі КНЕДП зобов'язані видавати ключі нового зразка, тож на першому ж перевипуску
> ключа шлях `keys[0].id → SELECT_KEY → SIGN` перестане працювати — і виглядатиме це як дефект
> компоненти, що з'явився нізвідки. **Шляхи А і Б від цього захищені за побудовою**: там
> ідентифікатор ключа не вгадується взагалі, його дає сам сертифікат. У запасному шляху бери
> `keys[].keyId2`, коли він є, і перевіряй **наявність `certId`**, а не `errorCode = 0`.
> Механізм — [architecture/uapki.md](../architecture/uapki.md) §9.1.

> **Страховка від підпису «не тим» ключем — з УМОВОЮ, а не завжди.**
> Якщо після `SELECT_KEY` активним виявиться ключ шифрування, `SIGN` поверне
> **`4109 INVALID_KEY_USAGE`** — але **лише** коли `signatureFormat` **не** `RAW` **і** (задано
> `includeCert` **або** ідентифікація підписувача не за `keyId`): `sign.cpp:304-314`. Для ПРРО
> страховка працює гарантовано, бо `includeCert: true` там нормативна вимога. Для
> `signatureFormat: "CMS"` з ідентифікацією за `keyId` і без вкладеного сертифіката перевірки
> **не буде взагалі**, і підпис ключем шифрування пройде тихо. Тобто це другий рубіж, а не
> привід не обирати ключ за сертифікатом.
````

- [ ] **Крок 3: Уточнити `docs/architecture/uapki.md` §9.1**

Замінити останній абзац §9.1 (той, що починається з `**Компоненту це не змінює:**` і закінчується рядком `Прикладна сторона — [integration-1c/uapki.md](../integration-1c/uapki.md) §4.3.`) на:

````markdown
**Гілка `certId` цього правила не потребує.** Правило «перевіряй наявність `certId`, а не
`errorCode == 0`» стосується **виклику за `id`** — і лише його. Коли `SELECT_KEY` кличуть за
`certId`, `cer_store->getCertByCertId` уже успішно відпрацював на
`session-select-key.cpp:81` — інакше метод повернув би помилку ще там; повторний пошук на `:141`
іде по тому самому сховищу з тим самим ідентифікатором, а між ними лише `addCerts` (`:131`), що
**додає, не видаляє**. Отже `RET_UAPKI_CERT_NOT_FOUND` на `:152` у цій гілці недосяжний, а
`certId`/`certificate` присутні завжди.

Розрізняти це треба в обидва боки. Хто прочитає правило як безумовне — або робитиме зайву
перевірку в гілці `certId`, або, що гірше, визнає її ритуалом і прибере **й у гілці `id`**, де
вона критична.

**Компоненту це не змінює:** `AddinUAPKIConnect` лишається тонким passthrough — свідоме рішення
дизайн-спек
([`2026-09-02-uapki-signature-closure-design.md`](../superpowers/specs/2026-09-02-uapki-signature-closure-design.md)
§3 і [`2026-09-21-uapki-certid-key-selection-design.md`](../superpowers/specs/2026-09-21-uapki-certid-key-selection-design.md)
§3). Правильний виклик формує 1С, і рекомендований шлях — **`SELECT_KEY` за `certId`**: він
обирає потрібний ключ із двох (`KEYS` для цього ознаки не має) і знімає пастку за побудовою.
Перебір `keyId2` → `id` лишається запасним — для контейнерів, до яких сертифіката немає взагалі.
Прикладна сторона — [integration-1c/uapki.md](../integration-1c/uapki.md) §4.3.

Покриття: кейси 10 і 11 `native_host` (`tests/native_host.cpp`) — універсальний на
`test-diia.p12` і купинний на `jks-kupyna` з `tests/data/local-keys.json` (поза цією машиною —
SKIP, exit 3).
````

- [ ] **Крок 4: Прогнати гард якорів**

```
python scripts/check-doc-anchors.py
echo $?
```
Очікується `0`. Нові посилання `list-certs.cpp:135-136`, `extension-helper-json.cpp:410-418`,
`sign.cpp:304-314`, `session-select-key.cpp:81/:131/:141/:152`, `tests/native_host.cpp` — усі
ці імена файлів у репозиторії **унікальні**, тож гард перевірить їх по-справжньому (існування
файла + межі рядків), а не пропустить як неоднозначні. Якщо щось червоне — звірити номер рядка
з реальним файлом і виправити **посилання**, не гард.

- [ ] **Крок 5: Перечитати обидва документи як сторонній читач**

Звірити три речі, яких гард не бачить (він перевіряє існування, не зміст):
1. У §4.3 немає обіцянки, ширшої за механізм: страховка `4109` названа **з умовою**, а не як властивість `SIGN`.
2. `certId` у прикладах — саме `Base64` (як віддає `ADD_CERT`/`LIST_CERTS`), а `id` — `Hex`; місцями вони не переплутані.
3. §9.1 більше не читається так, ніби перевірка `certId` потрібна завжди.

- [ ] **Крок 6: Коміт**

```bash
git add docs/integration-1c/uapki.md docs/architecture/uapki.md
git commit --only -- docs/integration-1c/uapki.md docs/architecture/uapki.md -F - <<'EOF'
docs(uapki): SELECT_KEY за certId — рекомендований шлях вибору ключа

§4.3 integration-1c: шлях А (сертифікат із бази через ADD_CERT) і шлях Б
(двофазний, для контейнерів із сертифікатами всередині) стають основними;
перебір keyId2 -> id лишається запасним. Страховка 4109 INVALID_KEY_USAGE
названа З УМОВОЮ (sign.cpp:304-314), а не як властивість SIGN: обіцянка,
ширша за механізм, уже коштувала проєкту Critical.

§9.1 architecture: правило «перевіряй наявність certId, а не errorCode»
прив'язане до гілки id — у гілці certId воно за побудовою зайве.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
EOF
```

- [ ] **Крок 7: Фінальна звірка з критеріями приймання спеки §9**

Пройтись по всіх семи пунктах і для кожного назвати доказ (номер прогону, рядок таблиці гейта, вивід команди). Окремо переконатись, що `version.h` закомічено, якщо `build_project.ps1` його перегенерував:
```
git status --short
```
Не має лишитись незакомічених **своїх** файлів. Чужі незакомічені зміни в дереві — **не чіпати** і не згадувати в комітах.

---

## Що НЕ входить у план (свідомо, за спекою §3 і §10)

- **Правки в `src/`.** Компонента лишається passthrough.
- **Розширення схеми `local-keys.json`.**
- **Бойовий `.ZS2` клієнта в гейті.** Він старої схеми з сертифікатами зовні — майже точний дубль осі кейса 10, нового покриття не дає, а зробив би гейт залежним від робочого столу конкретної машини. Разовий вимір по ньому — окрема дія поза цією роботою.
- **Зміна кейсів 1–9**, зокрема кейса 8: він доводить, що обхід через `keyId2` працює; кейс 11 доводить інше — що обхід непотрібний. Обидва твердження цінні.
- **Онлайн-профіль `CAdES-T`/TSP.** Межа покриття не рухається.
- **Правки в `AGENTS.md`/`CLAUDE.md`.** Рядок про `native_host.exe` там описує рівень, а не перелічує кейси, тож нічого не застаріває. Джерело правди про покриття UAPKI — `docs/architecture/uapki.md`; дублювати його в системних інструкціях заборонено (`AGENTS.md`, врізка «Документація — джерело правди»).

## Відомі межі результату — назвати у фінальному звіті

- **Кейс 11 поза цією машиною дає SKIP.** Доказ твердження 4 відтворюється лише там, де лежить купинний ключ. Альтернативи немає: купинного ключа в репозиторії немає й бути не може.
- **`certificate` у виводі кейса 11 елідується** (розмір замість вмісту) — свідомий відступ від буквального «сирого друку» спеки §9 п.4: вивід гейта потрапляє у звіти, а сертифікат — особистий документ розробника. Усе, заради чого той пункт вимагає сирий друк (перелік полів і наявність `certId`), лишається видимим. Суб'єкт сертифікатів у кейсі 11 у консоль теж не виноситься; у кейсі 10 виноситься — там сертифікати тестові й лежать у git.
- **Кількість ключів у JKS і кількість кандидатів — числа з одного прогону** на одному контейнері. Інший КНЕДП може дати іншу картину; `CHECK` на них живе рівно доти, доки живе цей ключ.
