# Контракт ініціалізації провайдера НКІ — план імплементації

> **Для агентів-виконавців:** ОБОВ'ЯЗКОВИЙ СУБ-СКІЛ: `superpowers:subagent-driven-development`
> (рекомендовано) або `superpowers:executing-plans` — виконувати задача за задачею. Кроки
> позначені чекбоксами (`- [ ]`).

**Мета:** зробити ініціалізацію провайдера НКІ ідемпотентною й облікованою, щоб повторний
`INIT` у живому процесі 1С піднімав провайдер, а не мовчки віддавав нуль.

**Архітектура:** причина — процесо-широкий ресурс (глобал `CmPkcs12` у `cm-pkcs12_xNN.dll`) із
модульно-широким власником і неписаним контрактом. Виправляється лічильником посилань у самому
провайдері (C1) плюс записаним контрактом у `cm-api.h`; володіння в реєстрі UAPKI переводиться
на RAII (C2); `INIT` перестає мовчати про невдале завантаження (C3). Наш бік отримує політику
(C4: нуль провайдерів = помилка) та ідемпотентний `INIT` для 1С через пробу `PROVIDERS` (C5).

**Технології:** C++17, MSVC 2022, CMake, PowerShell 5.1, сабмодуль `extern/uapki` (форк
`VSydorenko/UAPKI`), тестові консольні exe в `tests/`.

**Спека:** `docs/superpowers/specs/2026-09-22-provider-init-contract-design.md`

## Глобальні обмеження

- **Гілки вже створено:** корінь — `uapki-provider-init-contract`; сабмодуль `extern/uapki` —
  `simplyaddin/provider-contract` (від `fda2148` = `upstream/main` = тег `v2.0.17`).
- **Правки в `extern/uapki` комітяться ВСЕРЕДИНІ сабмодуля**, не з кореня. У корені комітиться
  лише покажчик (gitlink). Джерело: `AGENTS.md`, Git-нюанси.
- **Комітити лише свої файли:** `git add <явний перелік>` + `git commit --only -- <ті самі
  шляхи>`. Ніколи `-a`/`-A`.
- **`version.h` руками не редагувати**, але **завжди комітити** — його перегенеровує
  `build_project.ps1`.
- **`run_tests.ps1` і `build_project.ps1` — UTF-8 З BOM.** Після будь-якої правки перевірити:
  `head -c 3 run_tests.ps1 | xxd -p` має дати `efbbbf`. Без BOM `powershell.exe` читає файл в
  ANSI, кирилиця перетворюється на сміття, і перший апостроф усередині слова валить парсер.
- **PCH:** `#include "../core/pch.h"` першим рядком у кожному `.cpp` у `src/`. У `tests/` —
  **НЕ підключати** (тестові exe лінкуються окремо).
- **`/utf-8` для MSVC:** кожна нова ціль у `tests/CMakeLists.txt` отримує
  `target_compile_options(<ціль> PRIVATE /utf-8)` під `if(MSVC)`.
- **Логування в `src/`** — лише через макроси `ServiceTools.h`; у статичних методах —
  `NEUTRAL_REPORT_*` з іменем компоненти першим аргументом. Прямий `spdlog` заборонено.
  Повідомлення — конкатенацією, з великої літери, без крапки в кінці, printf-стиль заборонено.
- **Збірка монопольна:** `build_project.ps1` і `run_tests.ps1` ділять `bin/Release`. Перед
  запуском переконатися, що ніхто інший не збирає. Збірка падає, якщо у `bin/Release` є
  запущені файли (емулятор, native_host).
- **Команда збірки для всіх задач:**
  `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests`
- **Гард якорів обов'язковий** перед будь-яким коммітом, що зачепив `docs/`, `AGENTS.md`,
  `CLAUDE.md`, `.claude/skills/` або перемістив рядки в `src/`/`tests/`:
  `python scripts/check-doc-anchors.py`
- **Пуш: СПЕРШУ сабмодуль, ПОТІМ корінь.** Кожен коміт у корені, що змінює gitlink
  `extern/uapki`, посилається на комміт, який існує лише у форку. Якщо запушити корінь раніше
  за сабмодуль, гілка в `origin` вказуватиме на комміт, якого на GitHub немає, — чужий клон і
  `git submodule update` зламаються. Порядок завжди:
  `git -C extern/uapki push -u origin simplyaddin/provider-contract`, потім `git push`.
  `CLAUDE.md` вимагає пуш після кожного закритого етапу — отже після Tasks 3, 4, 5 теж.
- **Правило негативної верифікації** (`docs/architecture/testing-rules.md`, правило 1): кожен
  новий `CHECK` треба побачити **червоним** до виправлення. У задачах 1–2 це виходить
  природно; де ні — крок «зробити червоне» виписаний явно.
- **Повідомлення коммітів** — українською, з рядком
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>` в кінці.
  Багаторядкове — через `git commit -F <файл>`.

**Нумерація нових кейсів `native_host`:** 12 (два екземпляри), 13 (безпека вивантаження),
14 (нуль провайдерів → 502), 15 (ідемпотентний `INIT`). Поточна межа в `main()` —
`if (kase < 1 || kase > 11)` (`tests/native_host.cpp:1643`) — піднімається до `> 15` один раз,
у Задачі 1, і далі не чіпається.

---

## Порядок виконання й контрольні точки з архітектором

**Архітектор рішення — сесія `provider-init-plan`.** Знайти: `ListAgents`; писати:
`SendMessage` з `to: "provider-init-plan"`. Архітектор тримає спеку й відповідає за рішення
про семантику; виконавець пише код і тримає дерево. Архітектор **не збирає й не ганяє гейт**,
поки йде виконання, — `bin/Release` належить виконавцю.

**П'ять фаз, у цьому порядку:**

| Фаза | Задачі | Де живуть комміти | Що назовні |
|---|---|---|---|
| А. Червоні докази | 1, 2 | наш репо | нічого |
| Б. Виправлення в бібліотеці | 3, 4, 5 | сабмодуль (гілка `simplyaddin/provider-contract`) + gitlink у нашому репо | пуш у **наш** форк і **нашу** гілку |
| В. Наш бік і документація | 6, 7, 8 | наш репо | пуш у нашу гілку |
| Г. Ворота | 9 | — | повний гейт x64 + x86 |
| Д. Назовні | 10, 11 | гілки PR у форку | PR в апстрім, issue, анонс, PR у `main` |

**Апстрім — ОСТАННІЙ, не перший.** Усе робиться, тестується й перевіряється у форку й у
нашому проєкті; PR в апстрім відкриваються лише після зеленого гейта на обох архітектурах.
Дві причини: (1) CI апстріму збирає лише Linux, тож наш Windows-вимір — єдиний доказ, який
ми можемо прикласти; (2) PR має нести **зміряний** результат, а не очікуваний — особливо
C2 (кейс 13).

**Наша поставка від апстріму НЕ залежить.** Сабмодуль вказує на інтеграційну гілку нашого
форку, тож PR у `main` цього репо мерджиться, щойно гейт зелений, — незалежно від того, чи
апстрім візьме свої PR через день, через два місяці чи ніколи. Повернення сабмодуля на
апстрімовий тег — окремий пізніший крок, після мерджу там.

**Обов'язкові контрольні точки — зупинитися й написати архітектору:**

1. **Task 1 крок 4:** кейс 12 **не** червоний. Уся модель дефекту під питанням.
2. **Task 2 крок 4:** контрактний тест червоний не там, де план передбачає.
3. **Task 3 крок 6:** контрактний тест зелений, а кейс 12 — ні.
4. **Task 4 крок 6:** рішення по C2 — надіслати вимір кейса 13 (вивід і exit-код) **до**
   коміту чи відкату.
5. **Task 9 крок 3:** підсумок гейта перед апстрімом.
6. **Task 10:** тексти PR і issue — на рев'ю до подання.

**Що виконавець вирішує сам:** механіку, що не змінює семантики, — імена локальних
змінних за сусіднім кодом (у плані це прямо дозволено для `uapki_selftest.cpp`), дрібні
помилки компіляції, форматування, порядок include.

**Що йде до архітектора завжди:** будь-яка зміна поведінки проти спеки; будь-який тест, що
поводиться інакше, ніж передбачає план (червоний там, де мав бути зелений, і навпаки); будь-яке
бажання пропустити негативну верифікацію; будь-яка незрозуміла зміна в дереві.

**Формат повідомлень:** кожне твердження про код — з `файл:рядок`, кожне твердження про
поведінку — з командою й виводом. Того ж чекати у відповідь.

**Субагенти (якщо виконання через `subagent-driven-development`):** модель задається явно
за роллю — механічні правки й читання `sonnet`, рев'ю `sonnet`; дорожча модель лише з
названою причиною. Правило користувача з `~/.claude/CLAUDE.md`.

---

## Task 1: Відтворювач дефекту — `native_host` кейс 12 (два екземпляри)

Головний доказ. Дві копії головної DLL **за різними шляхами** в одному процесі дають Windows
два модулі з власними статиками — точна модель сценарію 1С, без припущень про поведінку
платформи.

**Файли:**
- Modify: `tests/native_host.cpp` (новий кейс + диспетчер + `usage()`)

**Інтерфейси:**
- Споживає: `struct Component` (`tests/native_host.cpp:182`) — `load(path)`, `call(method,
  params)`, `unload()`; `makeTempDir(prefix)`; `CHECK(cond, msg)`; `errCode(resp, json&)`;
  `ARCH_W`; `w2u8`.
- Віддає: `case12_twoInstances(const std::wstring& mainDllSrc, const std::wstring& binDir)` →
  `bool`.

- [ ] **Крок 1: Додати кейс 12 перед `usage()`**

Вставити в `tests/native_host.cpp` безпосередньо перед `static void usage()`:

```cpp
// ========================================================================
// КЕЙС 12 — ДВА ЕКЗЕМПЛЯРИ головної DLL в одному процесі.
// Модель сценарію 1С, де компонента опинилась у процесі двічі (ExtCompT +
// тимчасова копія, або дві версії макета розширення). Два РІЗНІ шляхи до
// файла — для Windows два різні модулі, кожен зі своїми статиками, отже два
// незалежні екземпляри статично злінкованого UAPKI. Але глобал у
// cm-pkcs12_xNN.dll — ОДИН на процес.
//
// До C1: другий INIT дає countCmProviders == 0 — provider_init повернув
// ALREADY_INITIALIZED, loadProvider не зареєстрував провайдера, а
// setup_cm_providers з'їв код помилки.
// ========================================================================
static bool case12_twoInstances(const std::wstring& mainDllSrc, const std::wstring& binDir) {
    printf("== Case 12: два екземпляри головної DLL в одному процесі ==\n");

    std::wstring dllName  = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    std::wstring provName = std::wstring(L"cm-pkcs12") + ARCH_W + L".dll";

    // Обидві копії лежать у РІЗНИХ каталогах: однаковий шлях дав би один модуль
    // із лічильником посилань 2, а не два набори статиків.
    std::wstring dirA = makeTempDir(L"case12a");
    std::wstring dirB = makeTempDir(L"case12b");
    CHECK(!dirA.empty() && !dirB.empty(), "створено два тимчасові каталоги");
    CHECK(dirA != dirB, "каталоги різні");

    std::wstring dllA = dirA + L"\\" + dllName;
    std::wstring dllB = dirB + L"\\" + dllName;
    CHECK(CopyFileW(mainDllSrc.c_str(), dllA.c_str(), FALSE) != 0, "скопійовано головну DLL -> A");
    CHECK(CopyFileW(mainDllSrc.c_str(), dllB.c_str(), FALSE) != 0, "скопійовано головну DLL -> B");

    // Провайдер кладемо ПОРУЧ із кожною копією: так обидва екземпляри беруть
    // ОДИН і той самий файл за однаковим іменем, і тест не залежить від стану
    // %LOCALAPPDATA% і від ресурсного розгортання (це покривають кейси 1-2).
    CHECK(CopyFileW((binDir + L"\\" + provName).c_str(), (dirA + L"\\" + provName).c_str(), FALSE) != 0,
          "скопійовано провайдера поруч із A");
    CHECK(CopyFileW((binDir + L"\\" + provName).c_str(), (dirB + L"\\" + provName).c_str(), FALSE) != 0,
          "скопійовано провайдера поруч із B");

    // --- Екземпляр A -----------------------------------------------------
    Component a;
    if (!a.load(dllA)) return false;
    std::string respA = a.call("INIT", "");
    printf("  A INIT: %s\n", respA.c_str());
    json jA;
    CHECK(errCode(respA, jA) == 0, "A: INIT errorCode == 0");
    CHECK(jA["result"].contains("countCmProviders"), "A: result.countCmProviders присутній");
    CHECK(jA["result"]["countCmProviders"].get<long>() == 1, "A: countCmProviders == 1");

    // --- Екземпляр B — ОКРЕМИЙ модуль, свіжі статики UAPKI ---------------
    Component b;
    if (!b.load(dllB)) return false;
    std::string respB = b.call("INIT", "");
    printf("  B INIT: %s\n", respB.c_str());
    json jB;
    CHECK(errCode(respB, jB) == 0, "B: INIT errorCode == 0 (свіжі статики UAPKI)");
    CHECK(jB["result"].contains("countCmProviders"), "B: result.countCmProviders присутній");
    CHECK(jB["result"]["countCmProviders"].get<long>() == 1,
          "B: countCmProviders == 1 <- ЦЕ Й Є ДЕФЕКТ до C1");

    // Лічильник, що піднявся, ще не означає робочого провайдера. Друга
    // ознака: OPEN у ДРУГОМУ екземплярі не впирається в UNKNOWN_PROVIDER.
    // Контейнера навмисно не відкриваємо — досить, щоб помилка була ІНША:
    // 4102 UNKNOWN_PROVIDER означав би, що реєстрації не сталося.
    json op;
    op["provider"] = "PKCS12";
    op["storage"]  = "Z:\\nonexistent-by-design.p12";
    op["password"] = "x";
    op["mode"]     = "RO";
    std::string respOpen = b.call("OPEN", op.dump());
    printf("  B OPEN(неіснуючий): %s\n", respOpen.c_str());
    json jO;
    long ecOpen = errCode(respOpen, jO);
    CHECK(ecOpen != 4102, "B: OPEN не дає 4102 UNKNOWN_PROVIDER (провайдер зареєстрований)");

    a.unload();
    b.unload();
    return true;
}
```

- [ ] **Крок 2: Розширити диспетчер і `usage()`**

У `tests/native_host.cpp` знайти рядок

```cpp
    if (kase < 1 || kase > 11) { printf("Невідомий кейс: %s\n", w2u8(wargv[1]).c_str()); usage(); LocalFree(wargv); return 2; }
```

і замінити `> 11` на `> 15`.

У `switch (kase)` додати після рядка `case 11:`:

```cpp
            case 12: pass = case12_twoInstances(mainDll, binDir);              break;
```

У `usage()` замінити рядок `"native_host <case 1..11> [mainDll] [dataDir] [binDir] [prroDir] [outSig]\n"` на:

```cpp
        "native_host <case 1..15> [mainDll] [dataDir] [binDir] [prroDir] [outSig]\n"
```

- [ ] **Крок 3: Зібрати**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
```

- [ ] **Крок 4: Прогнати кейс і ПОБАЧИТИ ЧЕРВОНЕ**

```
bin\Release\native_host_x64.exe 12 "" "" "R:\github\SimplyAddinConnect\bin\Release"
```

Очікується: `FAIL: B: countCmProviders == 1 <- ЦЕ Й Є ДЕФЕКТ до C1`, exit-код 1.

**Якщо кейс проходить ЗЕЛЕНИМ — зупинитися й доповісти.** Це означало б, що модель двох
екземплярів не відтворюється на цій машині, і вся доказова база потребує перегляду; мовчки
йти далі не можна.

- [ ] **Крок 5: Коміт**

```bash
git add tests/native_host.cpp
git commit --only -m "test(native_host): кейс 12 — два екземпляри головної DLL, червоний доказ дефекту" -- tests/native_host.cpp
```

---

## Task 2: Контрактний харнес `provider_contract_selftest` (рівень L1.5)

Доводить дефект на рівні контракту провайдера — без головної DLL, без UAPKI, без 1С. Працює
з `cm-pkcs12_xNN.dll` напряму через `LoadLibraryW` + `GetProcAddress`.

**Файли:**
- Create: `tests/provider_contract_selftest.cpp`
- Modify: `tests/CMakeLists.txt` (нова ціль **до** гейта `BUILD_WITH_UAPKI`)

**Інтерфейси:**
- Споживає: `extern/uapki/library/common/cm-api/cm-api.h` (типи `CM_ERROR`, `CM_JSON_PCHAR`,
  `cm_provider_*_f`), `cm-errors.h` (`RET_CM_ALREADY_INITIALIZED`, `RET_CM_NOT_INITIALIZED`).
  Лінкування з UAPKI **не потрібне** — заголовки самодостатні (`stdint/stdbool/stddef`).
- Віддає: exe `provider_contract_selftest_x64.exe` / `_x86.exe`; exit `0` = усі CHECK пройшли,
  `1` = провал, `3` = SKIP (файл провайдера відсутній — зібрано без `-WithUAPKI`).

- [ ] **Крок 1: Створити `tests/provider_contract_selftest.cpp`**

```cpp
//
// @file tests/provider_contract_selftest.cpp
// @brief L1.5 — контракт ініціалізації провайдера НКІ.
//        Вантажить cm-pkcs12_xNN.dll напряму (LoadLibraryW + GetProcAddress) і
//        перевіряє, що provider_init ідемпотентний і веде облік посилань.
//        UAPKI НЕ лінкується: cm-api.h самодостатній.
//
// Це окремий консольний exe; PCH головного проєкту НЕ підключається, дозволено printf.
//

#ifndef _WIN32

#include <cstdio>
int main() {
    printf("provider_contract_selftest: Windows only\n");
    return 0;
}

#else // _WIN32

#include <windows.h>
#include <cstdio>
#include <string>

#include "cm-api.h"
#include "cm-errors.h"

#ifdef _WIN64
#  define ARCH_W L"_x64"
#else
#  define ARCH_W L"_x86"
#endif

#ifndef PCS_BIN_DIR
#  define PCS_BIN_DIR ""
#endif

static int g_fails = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("[FAIL] %s\n", (msg)); ++g_fails; } \
    else         { printf("[PASS] %s\n", (msg)); } \
} while (0)

// Перетворює UTF-8 у широкий рядок (шлях до провайдера приходить із CMake як char*).
static std::wstring u8to16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

struct ProviderApi {
    HMODULE                 h      = nullptr;
    cm_provider_info_f      info   = nullptr;
    cm_provider_init_f      init   = nullptr;
    cm_provider_deinit_f    deinit = nullptr;
    cm_provider_open_f      open   = nullptr;

    bool load(const std::wstring& path) {
        h = LoadLibraryW(path.c_str());
        if (!h) {
            printf("  LoadLibraryW('%ls') err=%lu\n", path.c_str(), GetLastError());
            return false;
        }
        info   = (cm_provider_info_f)   GetProcAddress(h, "provider_info");
        init   = (cm_provider_init_f)   GetProcAddress(h, "provider_init");
        deinit = (cm_provider_deinit_f) GetProcAddress(h, "provider_deinit");
        open   = (cm_provider_open_f)   GetProcAddress(h, "provider_open");
        return info && init && deinit && open;
    }
    ~ProviderApi() { if (h) FreeLibrary(h); }
};

// Пара предикатів, якою код розрізняє «об'єкт живий» від «об'єкта немає»
// (testing-rules, правило 2). provider_open іде через глобал: якщо глобала немає,
// відповідь РІВНО RET_CM_NOT_INITIALIZED; якщо є — будь-яка інша (файла свідомо
// не існує, тож це помилка відкриття, а не ініціалізації).
// Контейнер і пароль не потрібні — саме тому перевірка стабільна.
static bool providerAlive(ProviderApi& api) {
    CM_SESSION_API* session = nullptr;
    CM_ERROR err = api.open("Z:\\nonexistent-by-design.p12", OPEN_MODE_RO, nullptr, &session);
    printf("  [probe] provider_open -> 0x%04X\n", (unsigned)err);
    return err != RET_CM_NOT_INITIALIZED;
}

int main() {
    std::wstring binDir = u8to16(PCS_BIN_DIR);
    std::wstring path   = binDir + L"\\cm-pkcs12" + ARCH_W + L".dll";

    printf("provider_contract_selftest\n  provider=%ls\n", path.c_str());

    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("SKIP: немає файлу провайдера (зібрано без -WithUAPKI)\n");
        return 3;
    }

    ProviderApi api;
    if (!api.load(path)) { printf("[FAIL] завантаження провайдера й експортів\n"); return 1; }
    printf("[PASS] провайдер завантажено, усі потрібні експорти на місці\n");

    // 1. Перший init
    CHECK(api.init(nullptr) == RET_OK, "init #1 -> RET_OK");
    CHECK(providerAlive(api), "після init #1 провайдер живий");

    // 2. Повторний init — СЕРЦЕ ДЕФЕКТУ.
    CM_ERROR e2 = api.init(nullptr);
    printf("  init #2 -> 0x%04X\n", (unsigned)e2);
    CHECK(e2 == RET_OK, "init #2 -> RET_OK (ідемпотентність)");
    CHECK(providerAlive(api), "після init #2 провайдер живий");

    // 3. deinit при лічильнику 2 — об'єкт мусить ВИЖИТИ.
    CHECK(api.deinit() == RET_OK, "deinit #1 -> RET_OK");
    CHECK(providerAlive(api), "після deinit #1 провайдер ЖИВИЙ (лічильник 2 -> 1)");

    // 4. deinit до нуля — об'єкт зникає.
    CHECK(api.deinit() == RET_OK, "deinit #2 -> RET_OK");
    CHECK(!providerAlive(api), "після deinit #2 провайдера НЕМАЄ (лічильник 0)");

    // 5. deinit без init — поведінка не змінюється.
    CHECK(api.deinit() == RET_CM_NOT_INITIALIZED, "deinit #3 -> RET_CM_NOT_INITIALIZED");

    // 6. Повний цикл заново — провайдер має підніматися після повного звільнення.
    CHECK(api.init(nullptr) == RET_OK, "init #3 після повного deinit -> RET_OK");
    CHECK(providerAlive(api), "після init #3 провайдер живий");
    CHECK(api.deinit() == RET_OK, "фінальний deinit -> RET_OK");

    printf("\n=== provider_contract_selftest: %s (FAIL: %d) ===\n",
           g_fails == 0 ? "PASS" : "FAIL", g_fails);
    return g_fails == 0 ? 0 : 1;
}

#endif // _WIN32
```

- [ ] **Крок 2: Додати ціль у `tests/CMakeLists.txt`**

Вставити **перед** рядком `if(NOT BUILD_WITH_UAPKI)` (`tests/CMakeLists.txt:254`) — ціль не
потребує крипто-ядра, а відсутність файлу провайдера віддає SKIP сама:

```cmake
# ---------------------------------------------------------------------------
# provider_contract_selftest — L1.5-харнес КОНТРАКТУ провайдера НКІ.
# Вантажить cm-pkcs12_*.dll напряму (LoadLibraryW+GetProcAddress) і перевіряє
# ідемпотентність provider_init та облік посилань. Крипто-ядро НЕ лінкується:
# cm-api.h самодостатній (stdint/stdbool/stddef). Тому ціль стоїть ДО гейта
# BUILD_WITH_UAPKI — без нього exe збереться й віддасть exit 3 (SKIP), бо
# самого файлу провайдера не буде.
# ---------------------------------------------------------------------------
add_executable(provider_contract_selftest provider_contract_selftest.cpp)

set_target_properties(provider_contract_selftest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "provider_contract_selftest${_TEST_ARCH_SUFFIX}"
)

target_include_directories(provider_contract_selftest PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/uapki/library/common/cm-api   # cm-api.h, cm-errors.h
)

target_compile_definitions(provider_contract_selftest PRIVATE
    PCS_BIN_DIR="${CMAKE_SOURCE_DIR}/bin/Release"
)

if(MSVC)
    target_compile_options(provider_contract_selftest PRIVATE /utf-8)
endif()
```

- [ ] **Крок 3: Зібрати**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
```

- [ ] **Крок 4: Прогнати і ПОБАЧИТИ ЧЕРВОНЕ**

```
bin\Release\provider_contract_selftest_x64.exe
```

Очікується провал щонайменше на:
- `[FAIL] init #2 -> RET_OK (ідемпотентність)` — сьогодні повертається
  `RET_CM_ALREADY_INITIALIZED` (`0x0404`);
- `[FAIL] після deinit #1 провайдер ЖИВИЙ (лічильник 2 -> 1)` — сьогодні перший же `deinit`
  знищує об'єкт.

Exit-код 1.

- [ ] **Крок 5: Коміт**

```bash
git add tests/provider_contract_selftest.cpp tests/CMakeLists.txt
git commit --only -m "test(provider): L1.5 контрактний харнес провайдера — червоний доказ неідемпотентності" -- tests/provider_contract_selftest.cpp tests/CMakeLists.txt
```

---

## Task 3: C1 — ідемпотентний init з обліком посилань (сабмодуль)

Виправлення суті. Після цієї задачі обидва червоні тести зеленіють.

**Файли:**
- Modify: `extern/uapki/library/cm-pkcs12/src/main-cm-pkcs12.cpp:67,86,108`
- Modify: `extern/uapki/library/cm-pkcs11/src/main-cm-pkcs11.cpp:61,85,91`
- Modify: `extern/uapki/library/common/cm-api/cm-api.h:61` (коментар-контракт)
- Modify: `run_tests.ps1` (рівень L1.5 + кейс 12 у цикл)

**Інтерфейси:**
- Віддає: `provider_init` повертає `RET_OK` на повторний виклик; `provider_deinit` звільняє
  ресурс лише на нулі лічильника. Сигнатури експортів **не змінюються** — ABI той самий.

- [ ] **Крок 1: Переконатися, що сабмодуль на потрібній гілці**

```bash
git -C extern/uapki branch --show-current   # має бути simplyaddin/provider-contract
git -C extern/uapki status --short          # має бути чисто
```

- [ ] **Крок 2: Патч `cm-pkcs12`**

У `extern/uapki/library/cm-pkcs12/src/main-cm-pkcs12.cpp` замінити рядок 67

```cpp
static CmPkcs12* cm_pkcs12 = nullptr;
```

на

```cpp
static CmPkcs12* cm_pkcs12 = nullptr;
//  Reference counter for the process-wide provider instance. The provider DLL is
//  loaded once per process but may be initialized by SEVERAL independent consumers
//  (e.g. two modules that statically link UAPKI). Without counting, the second
//  provider_init() used to fail with RET_CM_ALREADY_INITIALIZED and the consumer
//  ended up with no provider registered at all.
static size_t cm_pkcs12_refcnt = 0;
```

Замінити тіло `provider_init` (`main-cm-pkcs12.cpp:86`) на:

```cpp
CM_EXPORT CM_ERROR provider_init (
        CM_JSON_PCHAR providerParams
)
{
    DEBUG_OUTPUT("provider_init()");
    CM_ERROR cm_err = RET_CM_GENERAL_ERROR;
    if (!cm_pkcs12) {
        cm_pkcs12 = new CmPkcs12();
        if (cm_pkcs12) {
            cm_err = cm_pkcs12->parseConfig(providerParams, cm_pkcs12->getDefaultParam());
            if (cm_err != RET_OK) {
                delete cm_pkcs12;
                cm_pkcs12 = nullptr;
            }
            else {
                cm_pkcs12_refcnt = 1;
            }
        }
    }
    else {
        //  Idempotent: the post-condition ("provider is initialized") already holds.
        //  The configuration of the FIRST initialization wins; per-session parameters
        //  of provider_open() override the defaults anyway.
        cm_pkcs12_refcnt++;
        cm_err = RET_OK;
    }
    return cm_err;
}
```

Замінити тіло `provider_deinit` (`main-cm-pkcs12.cpp:108`) на:

```cpp
CM_EXPORT CM_ERROR provider_deinit (void)
{
    DEBUG_OUTPUT("provider_deinit()");
    if (!cm_pkcs12) return RET_CM_NOT_INITIALIZED;

    if (cm_pkcs12_refcnt > 0) cm_pkcs12_refcnt--;
    if (cm_pkcs12_refcnt == 0) {
        delete cm_pkcs12;
        cm_pkcs12 = nullptr;
    }
    return RET_OK;
}
```

- [ ] **Крок 3: Патч `cm-pkcs11` (симетрично)**

У `extern/uapki/library/cm-pkcs11/src/main-cm-pkcs11.cpp` знайти оголошення
`static CmCryptoki* cm_cryptoki = nullptr;` і додати одразу під ним:

```cpp
//  See the comment in cm-pkcs12/src/main-cm-pkcs12.cpp: process-wide provider
//  instance shared by several independent consumers.
static size_t cm_cryptoki_refcnt = 0;
```

У `provider_init` того файла замінити гілку

```cpp
    else {
        cm_err = RET_CM_ALREADY_INITIALIZED;
    }
```

на

```cpp
    else {
        //  Idempotent, see cm-pkcs12.
        cm_cryptoki_refcnt++;
        cm_err = RET_OK;
    }
```

і в успішній гілці, одразу після `cm_err = cm_cryptoki->init(jo_params);` та перевірки
`if (cm_err != RET_OK) { ... }`, додати `else { cm_cryptoki_refcnt = 1; }`.

Замінити тіло `provider_deinit` того файла на:

```cpp
CM_EXPORT CM_ERROR provider_deinit (void)
{
    DEBUG_OUTPUT("provider_deinit()");
    if (!cm_cryptoki) return RET_CM_NOT_INITIALIZED;

    if (cm_cryptoki_refcnt > 0) cm_cryptoki_refcnt--;
    if (cm_cryptoki_refcnt == 0) {
        delete cm_cryptoki;
        cm_cryptoki = nullptr;
    }
    return RET_OK;
}
```

- [ ] **Крок 4: Записати контракт у `cm-api.h`**

У `extern/uapki/library/common/cm-api/cm-api.h` перед рядком 61
(`typedef CM_ERROR (*cm_provider_info_f) ...`) вставити:

```c
/*
 * Provider lifecycle contract.
 *
 * A provider library is loaded once per process, but the number of INDEPENDENT
 * consumers that initialize it is not controlled by the provider: several modules
 * in one process may each link UAPKI statically and each call provider_init().
 *
 * Therefore an implementation of cm_provider_init_f MUST:
 *   1) be idempotent - when the provider is already initialized it returns RET_OK,
 *      NOT RET_CM_ALREADY_INITIALIZED. The configuration of the first successful
 *      initialization wins; per-session parameters of provider_open() override the
 *      defaults anyway;
 *   2) keep a reference count of init/deinit calls.
 *
 * An implementation of cm_provider_deinit_f MUST release the provider only when the
 * reference count reaches zero, and return RET_OK while it is still above zero.
 * It returns RET_CM_NOT_INITIALIZED when the provider was never initialized.
 *
 * RET_CM_ALREADY_INITIALIZED is kept in cm-errors.h for compatibility with
 * third-party providers that predate this contract.
 */
```

- [ ] **Крок 5: Зібрати**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
```

- [ ] **Крок 6: Прогнати обидва тести — очікується ЗЕЛЕНЕ**

```
bin\Release\provider_contract_selftest_x64.exe
bin\Release\native_host_x64.exe 12 "" "" "R:\github\SimplyAddinConnect\bin\Release"
```

Обидва — exit 0. У першому всі `[PASS]`, `FAIL: 0`. У другому — `=== Case 12: PASS ===`.

Якщо `provider_contract_selftest` зелений, а кейс 12 ні — **зупинитися й доповісти**: це
означає, що на рівні контракту виправлено, а на рівні компоненти ні, тобто діє ще один
механізм, якого модель не передбачала.

- [ ] **Крок 7: Додати L1.5 і кейс 12 у гейт**

У `run_tests.ps1` у блок оголошення шляхів (поруч із `$SelfTestExe`, рядок ~61) додати:

```powershell
$ProviderContractExe = Join-Path $BinRelease ("provider_contract_selftest" + $ArchSuffix + ".exe")
```

Перед секцією `Section 'ЕТАП 3 (L2/L3): native_host кейси'` вставити:

```powershell
# =====================================================================
# ЕТАП L1.5: provider_contract_selftest — КОНТРАКТ провайдера НКІ.
# Ідемпотентність provider_init і облік посилань, напряму через LoadLibraryW.
# Крипто-ядро не лінкується; без -WithUAPKI файлу провайдера немає -> exit 3 (SKIP).
# =====================================================================
Section 'ЕТАП L1.5: provider_contract_selftest контракту провайдера'

if (-not (Test-Path $ProviderContractExe)) {
    Add-Result 'L1.5' 'provider_contract_selftest' 'BLOCKED' `
        "немає provider_contract_selftest.exe: $ProviderContractExe — зберіть з -WithTests"
}
else {
    $outF = Join-Path ([System.IO.Path]::GetTempPath()) ("pcs_" + [guid]::NewGuid().ToString('N').Substring(0,6) + '.out')
    $p = Start-Process -FilePath $ProviderContractExe `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
    $txt = ''
    if (Test-Path $outF) { $txt = Get-Content -Raw $outF }
    if ($p.ExitCode -eq 3) {
        Add-Result 'L1.5' 'provider_contract_selftest' 'SKIP' 'немає файлу провайдера (збірка без -WithUAPKI)'
    }
    elseif ($p.ExitCode -eq 0) {
        $nPassLines = ([regex]::Matches($txt, '\[PASS\]')).Count
        Add-Result 'L1.5' 'provider_contract_selftest' 'PASS' "усі CHECK пройшли (PASS: $nPassLines)"
    }
    else {
        $fails = ($txt -split "`n" | Where-Object { $_ -match '\[FAIL\]' }) -join ' | '
        Add-Result 'L1.5' 'provider_contract_selftest' 'FAIL' "exit=$($p.ExitCode) $fails"
    }
    Remove-Item $outF, "$outF.err" -ErrorAction SilentlyContinue
}
```

У циклі кейсів `foreach ($kase in 1,2,3,4,6,10) {` замінити перелік на `1,2,3,4,6,10,12`.

- [ ] **Крок 8: Перевірити BOM у `run_tests.ps1`**

```bash
head -c 3 run_tests.ps1 | xxd -p    # має бути efbbbf
```

Якщо не `efbbbf` — відновити BOM перед коммітом.

- [ ] **Крок 9: Коміт у сабмодулі, потім gitlink у корені**

```bash
cd extern/uapki
git add library/cm-pkcs12/src/main-cm-pkcs12.cpp library/cm-pkcs11/src/main-cm-pkcs11.cpp library/common/cm-api/cm-api.h
git commit --only -m "CM: make provider_init idempotent and reference-counted" -- library/cm-pkcs12/src/main-cm-pkcs12.cpp library/cm-pkcs11/src/main-cm-pkcs11.cpp library/common/cm-api/cm-api.h
cd ../..
git add extern/uapki run_tests.ps1 version.h
git commit --only -m "fix(uapki): C1 — ідемпотентний provider_init з обліком посилань; L1.5 у гейт" -- extern/uapki run_tests.ps1 version.h
```

---

## Task 4: C2 — RAII у реєстрі провайдерів + ВИМІР безпеки вивантаження

**Задача з умовою відмови.** C2 вмикає шлях, якого сьогодні не існує: `FreeLibrary` з
деструктора статика, тобто з `DLL_PROCESS_DETACH` під loader lock. Спершу вимір, потім
рішення.

**Файли:**
- Modify: `tests/native_host.cpp` (кейс 13)
- Modify: `extern/uapki/library/uapki/src/cm-providers.cpp:47,176,185,204,247,277,314`
- Modify: `run_tests.ps1` (кейс 13 у цикл — **лише якщо C2 лишається**)

**Інтерфейси:**
- Віддає: `case13_unloadSafety(const std::wstring& mainDllSrc, const std::wstring& binDir)` →
  `bool`. Після C2 — `CmProviders::deinit` звільняє через `unique_ptr::reset()`.

- [ ] **Крок 1: Додати кейс 13 (вимір, червоний ДО C2)**

Вставити в `tests/native_host.cpp` перед `usage()`:

```cpp
// ========================================================================
// КЕЙС 13 — безпека вивантаження головної DLL.
// Дві речі одним прогоном:
//   (а) чи не зависає/падає FreeLibrary головної DLL після INIT — з C2
//       деструктор статика UAPKI кличе FreeLibrary провайдера з-під
//       DLL_PROCESS_DETACH, тобто під loader lock;
//   (б) чи лишається cm-pkcs12_xNN.dll у процесі після вивантаження —
//       ДО C2 лишається (витік сирого вказівника), ПІСЛЯ C2 має зникнути.
// ========================================================================
static bool case13_unloadSafety(const std::wstring& mainDllSrc, const std::wstring& binDir) {
    printf("== Case 13: безпека вивантаження головної DLL ==\n");

    std::wstring dllName  = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    std::wstring provName = std::wstring(L"cm-pkcs12") + ARCH_W + L".dll";

    std::wstring tmp = makeTempDir(L"case13");
    CHECK(!tmp.empty(), "створено тимчасовий каталог");
    CHECK(CopyFileW(mainDllSrc.c_str(), (tmp + L"\\" + dllName).c_str(), FALSE) != 0,
          "скопійовано головну DLL");
    CHECK(CopyFileW((binDir + L"\\" + provName).c_str(), (tmp + L"\\" + provName).c_str(), FALSE) != 0,
          "скопійовано провайдера поруч");

    CHECK(GetModuleHandleW(provName.c_str()) == nullptr,
          "до старту провайдера в процесі НЕМАЄ");

    {
        Component c;
        if (!c.load(tmp + L"\\" + dllName)) return false;
        std::string resp = c.call("INIT", "");
        json j;
        CHECK(errCode(resp, j) == 0, "INIT errorCode == 0");
        CHECK(j["result"]["countCmProviders"].get<long>() == 1, "countCmProviders == 1");
        CHECK(GetModuleHandleW(provName.c_str()) != nullptr, "провайдер у процесі присутній");

        printf("  -> unload(): DestroyObject + FreeLibrary головної DLL\n");
        fflush(stdout);
        c.unload();     // якщо тут зависне — це і є відповідь виміру
        printf("  <- unload() повернувся\n");
        fflush(stdout);
    }

    CHECK(true, "FreeLibrary головної DLL повернувся без зависання й падіння");

    HMODULE stillThere = GetModuleHandleW(provName.c_str());
    printf("  після вивантаження GetModuleHandleW('%ls') = %p\n", provName.c_str(), (void*)stillThere);
    CHECK(stillThere == nullptr,
          "провайдера в процесі БІЛЬШЕ НЕМАЄ (витік закрито) <- ЦЕ ЧЕРВОНЕ до C2");
    return true;
}
```

Додати в `switch (kase)`:

```cpp
            case 13: pass = case13_unloadSafety(mainDll, binDir);              break;
```

- [ ] **Крок 2: Зібрати й прогнати ДО C2 — очікується ЧЕРВОНЕ на останньому CHECK**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
bin\Release\native_host_x64.exe 13 "" "" "R:\github\SimplyAddinConnect\bin\Release"
```

Очікується: усі CHECK до останнього — `ok`, останній — `FAIL: провайдера в процесі БІЛЬШЕ
НЕМАЄ`. Рядок `<- unload() повернувся` **мусить надрукуватись**.

- [ ] **Крок 3: Коміт виміру**

```bash
git add tests/native_host.cpp
git commit --only -m "test(native_host): кейс 13 — вимір безпеки вивантаження й витоку провайдера" -- tests/native_host.cpp
```

- [ ] **Крок 4: Патч C2 у сабмодулі**

У `extern/uapki/library/uapki/src/cm-providers.cpp` замінити структуру (рядок 47):

```cpp
typedef struct CM_PROVIDER_ST {
    const string                    id;
    std::unique_ptr<CmStorageProxy> storage;
    CM_PROVIDER_ST (const string& iId, CmStorageProxy* const iCmSession)
        : id(iId), storage(iCmSession) {
    }
    //  vector<> requires move-construction on reallocation. Copying is implicitly
    //  deleted by unique_ptr, which is exactly what we want: a provider proxy has
    //  a single owner.
    CM_PROVIDER_ST (CM_PROVIDER_ST&&) = default;
} CM_PROVIDER;
```

Додати `#include <memory>` до блоку include цього файла, якщо його там немає.

Замінити тіло `CmProviders::deinit` (рядок 185):

```cpp
void CmProviders::deinit (void)
{
    for (auto& it : lib_cmproviders.providers) {
        it.storage.reset();
    }
    lib_cmproviders.providers.clear();
}
```

Замінити всі читання сирого вказівника на `.get()`. Рядки й точні заміни:

- `:204` — `CmStorageProxy* storage = lib_cmproviders.providers[index].storage;`
  → `CmStorageProxy* storage = lib_cmproviders.providers[index].storage.get();`
- `:247` — `CmStorageProxy* storage = cm_provider->storage;`
  → `CmStorageProxy* storage = cm_provider->storage.get();`
- `:277` — те саме перетворення
- `:314` — те саме перетворення

Перевірити, що інших вживань не лишилось:

```bash
grep -n "\.storage\b\|->storage\b" extern/uapki/library/uapki/src/cm-providers.cpp
```

Кожне має бути або `.storage.get()`, або `.storage.reset()`, або в конструкторі.

- [ ] **Крок 5: Зібрати й прогнати кейс 13 — очікується ЗЕЛЕНЕ**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
bin\Release\native_host_x64.exe 13 "" "" "R:\github\SimplyAddinConnect\bin\Release"
```

- [ ] **Крок 6: РІШЕННЯ за виміром**

**Якщо кейс 13 зелений** (рядок `<- unload() повернувся` надрукувався, провайдера в процесі
немає, exit 0) — C2 лишається. Додати `13` у цикл `run_tests.ps1`:
`foreach ($kase in 1,2,3,4,6,10,12,13)`. Перейти до кроку 7.

**Якщо прогін зависає** (рядок `<- unload() повернувся` не з'явився протягом 60 секунд) **або
падає** — C2 **знімається**:

```bash
cd extern/uapki && git checkout -- library/uapki/src/cm-providers.cpp && cd ../..
```

Кейс 13 лишається в коді, але **не додається** в цикл `run_tests.ps1` (він червоний за
побудовою й це зафіксований факт, а не регресія). У `docs/tech-debt.md` додається **TD-12** з
текстом виміру: що саме зависло/впало, на якій архітектурі. PR #2 в апстрім **не подається**;
натомість у issue (Task 10) додається абзац про виявлене. Далі — Task 5.

- [ ] **Крок 7: Коміт (лише якщо C2 лишилась)**

```bash
cd extern/uapki
git add library/uapki/src/cm-providers.cpp
git commit --only -m "UAPKI: own CmStorageProxy via unique_ptr to fix leak on static destruction" -- library/uapki/src/cm-providers.cpp
cd ../..
git add extern/uapki run_tests.ps1 version.h
git commit --only -m "fix(uapki): C2 — RAII-володіння проксі провайдера; кейс 13 у гейт" -- extern/uapki run_tests.ps1 version.h
```

---

## Task 5: C3 — `INIT` звітує про кожного провайдера

**Файли:**
- Modify: `extern/uapki/library/uapki/src/api/library-init.cpp:63-86,234`
- Modify: `tests/uapki_selftest.cpp` (нове очікування `expectCmProvidersFailed`)
- Create: `tests/scenarios/08_provider_missing.json`

**Інтерфейси:**
- Віддає: `joResult["cmProviders"] = { "requested": N, "loaded": M, "failed": [ {"lib":…,
  "errorCode":…, "error":…} ] }`. Поле `countCmProviders` **не чіпається**.
- `uapki_selftest` отримує очікування `expectCmProvidersFailed` (ціле — очікувана довжина
  масиву `result.cmProviders.failed`).

- [ ] **Крок 1: Патч `setup_cm_providers`**

У `extern/uapki/library/uapki/src/api/library-init.cpp` замінити функцію (рядки 63–86) на:

```cpp
static int setup_cm_providers (JSON_Object* joParams, JSON_Object* joResult)
{
    const string s_dir = ParsonHelper::jsonObjectGetString(joParams, "dir");
    JSON_Array* ja_providers = json_object_get_array(joParams, "allowedProviders");
    const size_t cnt_providers = json_array_get_count(ja_providers);

    //  Loading a provider is TOLERANT by design: allowedProviders may legitimately
    //  list a provider that is absent on this machine. What used to be missing is
    //  the report - the outcome was discarded and INIT claimed plain success.
    JSON_Object* jo_report = nullptr;
    JSON_Array* ja_failed = nullptr;
    size_t cnt_loaded = 0;
    if (json_object_set_value(joResult, "cmProviders", json_value_init_object()) != JSONSuccess) {
        return RET_UAPKI_JSON_FAILURE;
    }
    jo_report = json_object_get_object(joResult, "cmProviders");
    if (json_object_set_value(jo_report, "failed", json_value_init_array()) != JSONSuccess) {
        return RET_UAPKI_JSON_FAILURE;
    }
    ja_failed = json_object_get_array(jo_report, "failed");

    for (size_t i = 0; i < cnt_providers; i++) {
        JSON_Object* jo_provider = json_array_get_object(ja_providers, i);
        if (!jo_provider) return RET_UAPKI_INVALID_JSON_FORMAT;

        string s_config;
        const string s_lib = ParsonHelper::jsonObjectGetString(jo_provider, "lib");
        JSON_Object* jo_config = json_object_get_object(jo_provider, "config");
        if (jo_config) {
            ParsonHelper json;
            json_object_copy_all_items(json.create(), jo_config);
            json.serialize(s_config);
        }

        const int ret_load = CmProviders::loadProvider(s_dir, s_lib, s_config);
        if (ret_load == RET_OK) {
            cnt_loaded++;
        }
        else {
            if (json_array_append_value(ja_failed, json_value_init_object()) != JSONSuccess) {
                return RET_UAPKI_JSON_FAILURE;
            }
            JSON_Object* jo_fail = json_array_get_object(ja_failed, json_array_get_count(ja_failed) - 1);
            (void)json_object_set_string(jo_fail, "lib", s_lib.c_str());
            (void)ParsonHelper::jsonObjectSetInt32(jo_fail, "errorCode", ret_load);
            (void)json_object_set_string(jo_fail, "error", error_code_to_str(ret_load));
        }
    }

    (void)ParsonHelper::jsonObjectSetUint32(jo_report, "requested", (uint32_t)cnt_providers);
    (void)ParsonHelper::jsonObjectSetUint32(jo_report, "loaded", (uint32_t)cnt_loaded);

    return RET_OK;
}   //  setup_cm_providers
```

Додати оголошення `error_code_to_str` у блок оголошень на початку файла (поруч з іншими
`extern "C"`; точно так само, як воно оголошене в `api-json.cpp:45`):

```cpp
extern "C" const char* error_code_to_str (int errorCode);
```

Замінити точку виклику (`library-init.cpp:234`):

```cpp
    DO(setup_cm_providers(json_object_get_object(jo_refparams, "cmProviders"), joResult));
```

- [ ] **Крок 2: Додати очікування в `tests/uapki_selftest.cpp`**

Поруч із `has_expect_count` (`tests/uapki_selftest.cpp:164`) додати:

```cpp
            const bool has_expect_failed      = (json_object_has_value(jo_task, "expectCmProvidersFailed") != 0);
            const int  expect_failed_cnt      = (int)json_object_get_number(jo_task, "expectCmProvidersFailed");
```

Одразу після блоку перевірки `expectCountCmProviders` (після рядка 235) додати:

```cpp
            //  2-біс) INIT: скільки провайдерів НЕ завантажилось (result.cmProviders.failed)
            if (task_ok && has_expect_failed) {
                JSON_Object* jo_res = json_object_get_object(jo_resp, "result");
                JSON_Object* jo_cmp = jo_res ? json_object_get_object(jo_res, "cmProviders") : nullptr;
                JSON_Array*  ja_f   = jo_cmp ? json_object_get_array(jo_cmp, "failed") : nullptr;
                const int got = ja_f ? (int)json_array_get_count(ja_f) : -1;
                if (got != expect_failed_cnt) {
                    task_ok = false;
                    detail += " [cmProviders.failed expected " + std::to_string(expect_failed_cnt)
                            + " got " + std::to_string(got) + "]";
                }
            }
```

> **Увага:** ім'я змінної для розібраної відповіді в цьому файлі може відрізнятись від
> `jo_resp`. Узяти те саме ім'я, яким користується сусідній блок `expectCountCmProviders`
> (рядки 228–236) — скопіювати спосіб доступу звідти, не вигадувати.

- [ ] **Крок 3: Створити сценарій `tests/scenarios/08_provider_missing.json`**

```json
{
  "comment": "L1: INIT із неіснуючим провайдером — толерантність збережена (errorCode 0), але невдача ВИДИМА в result.cmProviders.failed. x64.",
  "tasks": [
    {
      "method": "INIT",
      "comment": "cm-pkcs12 є, cm-nonexistent немає. x86: lib=cm-pkcs12_x86",
      "expectCountCmProviders": 1,
      "expectCmProvidersFailed": 1,
      "parameters": {
        "offline": true,
        "cmProviders": {
          "dir": "./",
          "allowedProviders": [
            { "lib": "cm-pkcs12_x64" },
            { "lib": "cm-nonexistent" }
          ]
        }
      }
    },
    {
      "method": "DEINIT"
    }
  ]
}
```

- [ ] **Крок 4: Зібрати, прогнати сценарій**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
cd bin\Release && uapki_selftest_x64.exe ..\..\tests\scenarios\08_provider_missing.json
```

Очікується PASS обох задач.

- [ ] **Крок 5: Негативна верифікація**

Тимчасово змінити в сценарії `"expectCmProvidersFailed": 1` на `99`, прогнати — має бути
`[FAIL] … [cmProviders.failed expected 99 got 1]`. Повернути `1`.

Це доводить, що значення справді **вимірюється**, а не приймається за замовчуванням.

- [ ] **Крок 6: Коміт**

```bash
cd extern/uapki
git add library/uapki/src/api/library-init.cpp
git commit --only -m "UAPKI: report per-provider load outcome in INIT result instead of discarding it" -- library/uapki/src/api/library-init.cpp
cd ../..
git add extern/uapki tests/uapki_selftest.cpp tests/scenarios/08_provider_missing.json version.h
git commit --only -m "fix(uapki): C3 — INIT звітує про невдале завантаження провайдера; сценарій 08" -- extern/uapki tests/uapki_selftest.cpp tests/scenarios/08_provider_missing.json version.h
```

> **`run_tests.ps1` правити не треба.** Сценарії підхоплюються автоматично —
> `Get-ChildItem -Path $ScenDir -Filter '*.json'` (`run_tests.ps1:521`), і для x86 той самий
> цикл підміняє `cm-pkcs12_x64` на `cm-pkcs12_x86` (`:523`). Саме тому в сценарії стоїть
> x64-ім'я з коментарем про x86 — так у всіх наявних сценаріях.
>
> **Файл сценарію мусить бути UTF-8 БЕЗ BOM:** `parson` не парсить JSON із BOM, сценарій із
> BOM дасть `exit=2`. Перевірка: `head -c 3 tests/scenarios/08_provider_missing.json | xxd -p`
> **не** має дати `efbbbf`.

---

## Task 6: C4 — політика «нуль провайдерів = помилка» (наш бік)

**Файли:**
- Modify: `src/helpers/UAPKIConnect/UAPKIConnectHelper.h:121`
- Modify: `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp:493,734`
- Modify: `tests/native_host.cpp` (кейс 14)
- Modify: `run_tests.ps1` (кейс 14 у цикл)

**Інтерфейси:**
- `WarnIfProvidersNotLoaded` замінюється на
  `static bool ProvidersLoadedOrFail(const nlohmann::json& injectedParams, std::string& responseJson);`
  → `true` = усе гаразд (відповідь не змінено); `false` = провайдерів нуль, `responseJson`
  **перезаписано** конвертом помилки `502`.
- Формат конверта помилки:
  `{"errorCode":502,"error":"NO_CM_PROVIDERS_LOADED: requested N, loaded 0","method":"INIT","uapkiResponse":{…}}`

- [ ] **Крок 1: Замінити оголошення в заголовку**

У `src/helpers/UAPKIConnect/UAPKIConnectHelper.h` замінити рядок 121 (і його коментар) на:

```cpp
    /**
     * @brief Перевіряє, що провайдери НКІ справді піднялись, і формує вердикт.
     *
     * Правило: просили провайдерів (cmProviders.allowedProviders не порожній), а
     * завантажилось НУЛЬ — INIT є помилкою для 1С. Недобір (1 з 2) помилкою не є:
     * перелік законно може містити відсутній на машині провайдер.
     *
     * Відповідь бібліотеки зберігається цілою у полі uapkiResponse — прозорість
     * не втрачається, втрачається брехня про успіх.
     *
     * @param injectedParams параметри INIT після автоінʼєкції
     * @param responseJson   [in,out] відповідь; при нулі провайдерів ПЕРЕЗАПИСУЄТЬСЯ
     * @return true — усе гаразд; false — провайдерів нуль, відповідь замінено
     */
    static bool ProvidersLoadedOrFail(const nlohmann::json& injectedParams, std::string& responseJson);
```

- [ ] **Крок 2: Замінити реалізацію**

У `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp` замінити функцію
`WarnIfProvidersNotLoaded` (починається на рядку 493) на:

```cpp
bool UAPKIConnectHelper::ProvidersLoadedOrFail(const nlohmann::json& injectedParams, std::string& responseJson) {
    size_t expected = 0;
    std::string dirInfo;
    std::string libInfo;
    try {
        if (injectedParams.is_object() && injectedParams.contains("cmProviders") && injectedParams["cmProviders"].is_object()) {
            const nlohmann::json& cm = injectedParams["cmProviders"];
            if (cm.contains("dir") && cm["dir"].is_string()) {
                dirInfo = cm["dir"].get<std::string>();
            }
            if (cm.contains("allowedProviders") && cm["allowedProviders"].is_array()) {
                expected = cm["allowedProviders"].size();
                for (const auto& prov : cm["allowedProviders"]) {
                    if (prov.is_object() && prov.contains("lib") && prov["lib"].is_string()) {
                        if (!libInfo.empty()) {
                            libInfo += ", ";
                        }
                        libInfo += prov["lib"].get<std::string>();
                    }
                }
            }
        }

        // Провайдерів не просили — перевіряти нічого
        if (expected == 0) {
            return true;
        }

        nlohmann::json resp = nlohmann::json::parse(responseJson);
        long long loaded = -1;
        if (resp.contains("result") && resp["result"].is_object() &&
            resp["result"].contains("countCmProviders") && resp["result"]["countCmProviders"].is_number_integer()) {
            loaded = resp["result"]["countCmProviders"].get<long long>();
        }

        if (loaded < 0) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Ответ INIT не содержит result.countCmProviders — невозможно подтвердить загрузку провайдеров; ожидалось: " + std::to_string(expected) + ", dir: " + dirInfo + ", lib: " + libInfo);
            return true;
        }

        if (loaded == 0) {
            // ЕДИНСТВЕННЫЙ случай, когда мы меняем ответ библиотеки: INIT доложил
            // успех, но крипто-стек непригоден — OPEN/SIGN дадут 4102/4121. Именно
            // эта ложь стоила месяца. Ответ библиотеки сохраняем целиком.
            const std::string message = "NO_CM_PROVIDERS_LOADED: requested "
                + std::to_string(expected) + ", loaded 0; dir: " + dirInfo + ", lib: " + libInfo;
            NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Провайдеры НКИ не загружены — INIT считается неуспешным: " + message);

            nlohmann::json envelope;
            envelope["errorCode"]     = 502;
            envelope["error"]         = message;
            envelope["method"]        = "INIT";
            envelope["uapkiResponse"] = resp;
            responseJson = envelope.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            return false;
        }

        if (static_cast<size_t>(loaded) < expected) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "UAPKI загрузил меньше провайдеров, чем ожидалось: загружено " + std::to_string(loaded) + " из " + std::to_string(expected) + "; dir: " + dirInfo + ", lib: " + libInfo);
        } else {
            NEUTRAL_REPORT_DEBUG("UAPKIConnectHelper", "INIT: загружено провайдеров " + std::to_string(loaded) + " из " + std::to_string(expected));
        }
        return true;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Не удалось проверить число загруженных провайдеров в ответе INIT: " + std::string(e.what()));
        return true;
    }
    catch (...) {
        NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Неизвестная ошибка при проверке числа загруженных провайдеров в ответе INIT");
        return true;
    }
}
```

> Виняток трактується як «перевірити не вдалося» → `true`: діагностика не має перетворювати
> робочий `INIT` на помилку через власний збій розбору.

- [ ] **Крок 3: Замінити точку виклику**

У `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp` замінити блок на рядку 733–735:

```cpp
            // Для INIT сверяем число реально загруженных провайдеров. Ноль при
            // непустом allowedProviders — ошибка для 1С (ответ библиотеки
            // сохраняется в uapkiResponse).
            if (methodUpper == "INIT") {
                if (!ProvidersLoadedOrFail(paramsJson, responseJson)) {
                    return false;
                }
            }
```

- [ ] **Крок 4: Додати кейс 14 у `tests/native_host.cpp`**

```cpp
// ========================================================================
// КЕЙС 14 — нуль провайдерів є ПОМИЛКОЮ для 1С (політика C4).
// INIT із єдиним свідомо неіснуючим провайдером: бібліотека віддасть
// errorCode 0 з countCmProviders 0, а обгортка мусить перетворити це на 502
// і зберегти відповідь бібліотеки цілою в uapkiResponse.
// ========================================================================
static bool case14_zeroProvidersIsError(const std::wstring& binDir) {
    printf("== Case 14: нуль провайдерів = помилка 502 ==\n");

    std::wstring dllPath = binDir + L"\\SimplyAddinConnectWin" + ARCH_W + L".dll";
    Component c;
    if (!c.load(dllPath)) return false;

    // Явний cmProviders вимикає автоінʼєкцію: компонента поважає непорожній dir як є.
    json p;
    p["offline"] = true;
    p["cmProviders"]["dir"] = fwd(binDir) + "/";
    p["cmProviders"]["allowedProviders"] = json::array({ json{{"lib", "cm-nonexistent"}} });

    std::string resp = c.call("INIT", p.dump());
    printf("  INIT resp: %s\n", resp.c_str());

    json j;
    long ec = errCode(resp, j);
    CHECK(ec == 502, "errorCode == 502 (обгортка не повторює брехню бібліотеки)");
    CHECK(j.contains("error") && j["error"].is_string()
          && j["error"].get<std::string>().rfind("NO_CM_PROVIDERS_LOADED", 0) == 0,
          "error починається з NO_CM_PROVIDERS_LOADED");
    CHECK(j.contains("uapkiResponse") && j["uapkiResponse"].is_object(),
          "відповідь бібліотеки збережена в uapkiResponse");
    CHECK(j["uapkiResponse"].contains("result")
          && j["uapkiResponse"]["result"].contains("countCmProviders")
          && j["uapkiResponse"]["result"]["countCmProviders"].get<long>() == 0,
          "uapkiResponse зберігає оригінальний countCmProviders == 0");

    c.unload();
    return true;
}
```

Додати в `switch (kase)`:

```cpp
            case 14: pass = case14_zeroProvidersIsError(binDir);               break;
```

- [ ] **Крок 5: Побачити ЧЕРВОНЕ до правки, зелене після**

Порядок для негативної верифікації: зібрати з кейсом 14, але **без** правок кроків 1–3 —
очікується `FAIL: errorCode == 502` (буде `0`). Потім застосувати правки, перезібрати —
очікується PASS.

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
bin\Release\native_host_x64.exe 14 "" "" "R:\github\SimplyAddinConnect\bin\Release"
```

- [ ] **Крок 6: Додати кейс у гейт і закомітити**

У `run_tests.ps1`: `foreach ($kase in 1,2,3,4,6,10,12,13,14)` (без `13`, якщо C2 знято).
Перевірити BOM.

```bash
git add src/helpers/UAPKIConnect/UAPKIConnectHelper.h src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp tests/native_host.cpp run_tests.ps1 version.h
git commit --only -m "feat(uapki): C4 — нуль провайдерів після INIT є помилкою 502 для 1С" -- src/helpers/UAPKIConnect/UAPKIConnectHelper.h src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp tests/native_host.cpp run_tests.ps1 version.h
```

---

## Task 7: C5 — `INIT` ідемпотентний для 1С

**Файли:**
- Modify: `src/helpers/UAPKIConnect/UAPKIConnectHelper.h` (нове оголошення)
- Modify: `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp` (обробка 4106)
- Modify: `tests/native_host.cpp` (кейс 15)
- Modify: `run_tests.ps1` (кейс 15 у цикл)

**Інтерфейси:**
- `static bool HandleAlreadyInitialized(std::string& responseJson);` → `true`, якщо відповідь
  замінено на успішну (бібліотека жива, провайдери є); `false` — залишити як є.
- Відповідь при спрацюванні:
  `{"errorCode":0,"method":"INIT","result":{"countCmProviders":N,"alreadyInitialized":true}}`

- [ ] **Крок 1: Оголошення в заголовку**

Додати в `src/helpers/UAPKIConnect/UAPKIConnectHelper.h` перед `MaskPasswords`:

```cpp
    /**
     * @brief Робить INIT ідемпотентним для 1С.
     *
     * UAPKI на повторний INIT у тому самому екземплярі віддає 4106
     * ALREADY_INITIALIZED із порожнім результатом, хоча бібліотека жива й
     * придатна. Постумова «бібліотеку ініціалізовано» досягнута, тож для 1С це
     * успіх. Стан провайдерів НЕ вгадується й НЕ кешується — він МІРЯЄТЬСЯ
     * методом PROVIDERS, який віддає живий CmProviders::count().
     *
     * @param responseJson [in,out] відповідь INIT; при спрацюванні замінюється
     * @return true — відповідь замінено на успішну; false — залишити як є
     */
    static bool HandleAlreadyInitialized(std::string& responseJson);
```

- [ ] **Крок 2: Реалізація**

Додати в `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp` одразу після
`ProvidersLoadedOrFail`:

```cpp
// Код UAPKI RET_UAPKI_ALREADY_INITIALIZED (0x100A) у десятковому вигляді —
// саме так він приходить у полі errorCode JSON-відповіді.
static const long long UAPKI_ALREADY_INITIALIZED = 4106;

bool UAPKIConnectHelper::HandleAlreadyInitialized(std::string& responseJson) {
    try {
        nlohmann::json resp = nlohmann::json::parse(responseJson);
        if (!resp.contains("errorCode") || !resp["errorCode"].is_number_integer()) {
            return false;
        }
        if (resp["errorCode"].get<long long>() != UAPKI_ALREADY_INITIALIZED) {
            return false;
        }

        // МІРЯЄМО живий стан, а не згадуємо минулий: PROVIDERS віддає
        // CmProviders::count() незалежно від прапорця ініціалізації.
        nlohmann::json probeReq;
        probeReq["method"] = "PROVIDERS";
        const std::string probeStr = probeReq.dump();
        char* probeRaw = ::process(probeStr.c_str());
        if (!probeRaw) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Проба PROVIDERS не вернула ответ — INIT остаётся с кодом 4106");
            return false;
        }
        const std::string probeResp(probeRaw);
        ::json_free(probeRaw);

        long long count = -1;
        nlohmann::json pj = nlohmann::json::parse(probeResp);
        if (pj.contains("result") && pj["result"].is_object()
            && pj["result"].contains("providers") && pj["result"]["providers"].is_array()) {
            count = static_cast<long long>(pj["result"]["providers"].size());
        }

        if (count < 1) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Библиотека уже инициализирована, но провайдеров нет: " + std::to_string(count) + " — INIT остаётся ошибкой");
            return false;
        }

        NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "Повторный INIT: библиотека уже инициализирована, провайдеров живых: " + std::to_string(count));

        nlohmann::json ok;
        ok["errorCode"] = 0;
        ok["method"]    = "INIT";
        ok["result"]["countCmProviders"]  = count;
        ok["result"]["alreadyInitialized"] = true;
        responseJson = ok.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        return true;
    }
    catch (const std::exception& e) {
        NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Не удалось обработать повторный INIT: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Неизвестная ошибка при обработке повторного INIT");
        return false;
    }
}
```

- [ ] **Крок 3: Вбудувати в `ExecuteUapkiCommand`**

Замінити блок з Task 6 кроку 3 на:

```cpp
            // Для INIT: спершу робимо INIT ідемпотентним (4106 -> вимір PROVIDERS),
            // і лише потім застосовуємо політику нуля провайдерів. Порядок важливий:
            // після заміни відповіді вона вже несе реальний countCmProviders.
            if (methodUpper == "INIT") {
                if (HandleAlreadyInitialized(responseJson)) {
                    isSuccess = true;
                }
                if (!ProvidersLoadedOrFail(paramsJson, responseJson)) {
                    return false;
                }
            }
```

> `isSuccess` оголошено вище в цій же функції (рядок ~723). Зміна його значення тут —
> свідома: відповідь замінено на успішну, і повертати `false` було б розбіжністю між
> результатом і поверненим прапорцем.

- [ ] **Крок 4: Кейс 15**

```cpp
// ========================================================================
// КЕЙС 15 — повторний INIT у ТОМУ САМОМУ екземплярі (політика C5).
// UAPKI віддає 4106 ALREADY_INITIALIZED із порожнім результатом; обгортка
// мусить зміряти живий стан через PROVIDERS і віддати 1С успіх із реальним
// числом провайдерів і ознакою alreadyInitialized.
// ========================================================================
static bool case15_idempotentInit(const std::wstring& binDir) {
    printf("== Case 15: повторний INIT ідемпотентний для 1С ==\n");

    std::wstring dllPath = binDir + L"\\SimplyAddinConnectWin" + ARCH_W + L".dll";
    Component c;
    if (!c.load(dllPath)) return false;

    std::string r1 = c.call("INIT", buildInit(true));
    json j1;
    CHECK(errCode(r1, j1) == 0, "INIT #1 errorCode == 0");
    CHECK(j1["result"]["countCmProviders"].get<long>() == 1, "INIT #1 countCmProviders == 1");

    std::string r2 = c.call("INIT", buildInit(true));
    printf("  INIT #2 resp: %s\n", r2.c_str());
    json j2;
    CHECK(errCode(r2, j2) == 0, "INIT #2 errorCode == 0 <- ЧЕРВОНЕ до C5 (буде 4106)");
    CHECK(j2["result"].contains("alreadyInitialized")
          && j2["result"]["alreadyInitialized"].get<bool>(),
          "INIT #2 несе alreadyInitialized == true");
    CHECK(j2["result"]["countCmProviders"].get<long>() == 1,
          "INIT #2 countCmProviders == 1 (зміряно через PROVIDERS)");

    // Друга ознака: після такого INIT крипто-стек справді придатний.
    json op;
    op["provider"] = "PKCS12";
    op["storage"]  = "Z:\\nonexistent-by-design.p12";
    op["password"] = "x";
    op["mode"]     = "RO";
    json jo;
    long ecOpen = errCode(c.call("OPEN", op.dump()), jo);
    CHECK(ecOpen != 4102, "OPEN не дає 4102 UNKNOWN_PROVIDER");

    c.unload();
    return true;
}
```

Додати в `switch (kase)`:

```cpp
            case 15: pass = case15_idempotentInit(binDir);                     break;
```

- [ ] **Крок 5: Червоне до, зелене після**

Зібрати з кейсом 15 **до** правок кроків 1–3 → `FAIL: INIT #2 errorCode == 0` (буде 4106).
Застосувати правки, перезібрати → PASS.

- [ ] **Крок 6: Гейт і коміт**

`foreach ($kase in 1,2,3,4,6,10,12,13,14,15)`. Перевірити BOM.

```bash
git add src/helpers/UAPKIConnect/UAPKIConnectHelper.h src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp tests/native_host.cpp run_tests.ps1 version.h
git commit --only -m "feat(uapki): C5 — повторний INIT ідемпотентний для 1С через вимір PROVIDERS" -- src/helpers/UAPKIConnect/UAPKIConnectHelper.h src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp tests/native_host.cpp run_tests.ps1 version.h
```

---

## Task 8: Документація

**Файли:**
- Modify: `docs/architecture/uapki.md` (новий підрозділ)
- Modify: `docs/integration-1c/uapki.md:118-129` (§4.1), `:345-366` (§5), §7
- Modify: `docs/tech-debt.md` (TD-9, TD-10, TD-11 і, за результатом Task 4, TD-12)
- Modify: `docs/tasks/2026-09-22_provider_reinit_defect.md` (шапка статусу)
- Modify: `AGENTS.md` — **лише після** мерджу в апстрім (Task 10), не зараз

- [ ] **Крок 1: `docs/architecture/uapki.md` — новий підрозділ**

Додати підрозділ «Контракт ініціалізації провайдера НКІ» з таким змістом:

- глобал `CmPkcs12` у провайдерській DLL — процесо-широкий, власник — модульно-широкий;
- контракт: `provider_init` ідемпотентний, веде облік; `provider_deinit` звільняє на нулі;
  записано в `extern/uapki/library/common/cm-api/cm-api.h`;
- **чому `DEINIT` НЕ викликається з `~AddinUAPKIConnect`**: у сеансі законно живуть кілька
  об'єктів компоненти (маршрут ПРРО й маршрут еквайрингу), руйнування одного погасило б
  бібліотеку решті; через `DllMain(DLL_PROCESS_DETACH)` теж не можна — `FreeLibrary` під
  loader lock. Це **рішення з причиною**, не недогляд;
- `INIT` для 1С ідемпотентний: `4106` перетворюється на успіх із `alreadyInitialized: true`
  після виміру через `PROVIDERS`;
- нуль провайдерів = помилка `502` на нашому боці.

- [ ] **Крок 2: `docs/integration-1c/uapki.md` §4.1**

Замінити абзац про поля `result` на:

```markdown
Поля `result` `INIT`: `countCmProviders` (скільки провайдерів завантажено), `offline`,
`certCache`, `crlCache` тощо.

**`INIT` ідемпотентний.** Повторний виклик у тому самому сеансі поверне `errorCode: 0` з
`result.alreadyInitialized = true` — бібліотека вже піднята, провайдери живі (компонента
звіряє це виміром, а не пам'яттю). **Важливо:** параметри повторного `INIT` при цьому **НЕ
застосовуються** — бібліотека не переініціалізовується. Щоб змінити параметри, потрібен
`DEINIT`, а потім `INIT` із `"skipSelfTest": true`.

**Нуль провайдерів — помилка.** Якщо провайдерів просили, а піднявся нуль, компонента віддає
`errorCode: 502` з `error`, що починається на `NO_CM_PROVIDERS_LOADED`. Відповідь бібліотеки
зберігається цілою в полі `uapkiResponse`. Окремо звіряти `countCmProviders` з нулем більше
не треба — це тепер механізм, а не угода.
```

- [ ] **Крок 3: `docs/integration-1c/uapki.md` §5**

Замінити абзац «**Єдина перевірка з боку 1С:**» на:

```markdown
**Перевіряти вручну більше не треба.** Якщо провайдер не завантажився (антивірус заблокував
запис/завантаження, немає прав на `%LOCALAPPDATA%` тощо), компонента віддає `errorCode: 502`
з деталями замість того, щоб повторити `errorCode: 0` бібліотеки. Оригінальна відповідь
лишається в `uapkiResponse` — для діагностики.
```

- [ ] **Крок 4: `docs/integration-1c/uapki.md` §7 (Помилки)**

Додати в таблицю/перелік помилок рядок про `502 NO_CM_PROVIDERS_LOADED` — «провайдери НКІ не
піднялись; `OPEN`/`SIGN` працювати не будуть; дивись `uapkiResponse` і лог компоненти».

- [ ] **Крок 5: `docs/tech-debt.md` — три (або чотири) записи**

Додати перед секцією `## Закрито`, у стилі наявних TD (заголовок `## TD-N. <назва>`, далі
**Де:**, **Що не так.**, **Чому відкладено.**, **Критерій перевірки.**):

- **TD-9. Контекст-хендли замість глобала у CM-провайдерах.** Де: `cm-api.h:61-71`. Що не
  так: провайдер тримає процесо-широкий глобал, ізоляція споживачів досягається лічильником,
  а не роздільним станом. Чому відкладено: шість нових експортів і подвійні шляхи в лоадері,
  проксі й обох провайдерах; для `cm-pkcs12` виграш нульовий (клас тримає лише
  `m_DefaultParam`, сесії вже незалежні). Критерій: якщо з'явиться провайдер зі справжнім
  спільним станом між сесіями.
- **TD-10. Гонка двох потоків у `provider_init`.** Де: `main-cm-pkcs12.cpp:86`,
  `main-cm-pkcs11.cpp:61`. Що не так: ні глобал, ні лічильник не захищені; два одночасні
  перші `init` дали б два `new`. Чому відкладено: гонки ніхто не міряв; мьютекс змінив би
  поведінку в незміряному сценарії й розширив поверхню рев'ю апстрім-PR. Критерій: вимір, що
  показує одночасні `INIT` з різних потоків у реальному хості.
- **TD-11. `activeProvider` — сирий вказівник усередину вектора.** Де: `cm-providers.cpp:57`.
  Що не так: `push_back` при реалокації підвісив би його. Чому відкладено: сьогодні
  недосяжно — провайдери вантажаться лише в `INIT`, коли сховище не відкрите; змішувати
  загартування з виправленням в одному PR — знижувати шанси мерджу. Критерій: одноряд
  `providers.reserve(cnt_providers)` перед циклом у `setup_cm_providers`.
- **TD-12** — додається **лише якщо** Task 4 крок 6 дав відмову від C2; текст — фактичний
  результат виміру.

- [ ] **Крок 6: Оновити шапку задачі-входу**

У `docs/tasks/2026-09-22_provider_reinit_defect.md` замінити рядок статусу на:

```markdown
*Дата: 2026-09-22. Статус: **рішення ухвалено — див.
`docs/superpowers/specs/2026-09-22-provider-init-contract-design.md` і план
`docs/superpowers/plans/2026-09-22-provider-init-contract.md`.** Обрано п'ятий напрямок
(рефлічильник у провайдері), якого в цьому документі немає; напрямки A/B/D відкинуто з
причинами — §9 спеки.*
```

- [ ] **Крок 7: Гард якорів і коміт**

```bash
python scripts/check-doc-anchors.py
```

exit 0 обов'язковий.

```bash
git add docs/architecture/uapki.md docs/integration-1c/uapki.md docs/tech-debt.md docs/tasks/2026-09-22_provider_reinit_defect.md
git commit --only -m "docs(uapki): контракт ініціалізації провайдера, ідемпотентний INIT, 502; TD-9..TD-11" -- docs/architecture/uapki.md docs/integration-1c/uapki.md docs/tech-debt.md docs/tasks/2026-09-22_provider_reinit_defect.md
```

---

## Task 9: Повний гейт і пуш — ворота перед апстрімом

- [ ] **Крок 1: Повний гейт на обох архітектурах**

```
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
powershell -File run_tests.ps1 x64
powershell -File run_tests.ps1 x86
```

Обидва — без FAIL і BLOCKED, ненульових exit немає. `SKIP` допустимі (еталони ПРРО,
`local-keys.json`, арбітр ІІТ).

- [ ] **Крок 2: Пуш — СПЕРШУ сабмодуль, ПОТІМ корінь**

```bash
git -C extern/uapki push -u origin simplyaddin/provider-contract
git add version.h
git commit --only -m "chore: version.h — номер збірки після гейта" -- version.h
git push
```

- [ ] **Крок 3: Контрольна точка з архітектором**

Надіслати архітектору (сесія `provider-init-plan`) підсумкову таблицю `run_tests.ps1` для x64
і x86 (числа PASS/FAIL/SKIP/BLOCKED), рішення по C2 з виміром кейса 13, і SHA коммітів C1/C2/C3
у сабмодулі. **Апстрім (Task 10) не починати до відповіді.**

## Task 10: Апстрім — три PR і issue

**Передумова:** Task 9 зелений на x64 і x86, архітектор підтвердив контрольну точку.

**Файли:** нові гілки у форку `VSydorenko/UAPKI` (в каталозі `extern/uapki`).

> **Це дія назовні, у чужому репозиторії.** Тексти PR і issue спершу надсилаються архітектору
> на рев'ю, фінальне «подавай» дає користувач. Без обох — не подавати.

- [ ] **Крок 1: Нарізати гілки під PR через `cherry-pick`**

Зміни файлово не перетинаються, тож конфліктів не буде за побудовою.

```bash
cd extern/uapki
git log --oneline fda2148..simplyaddin/provider-contract   # переписати SHA кожного комміта

# C1
git checkout -b fix/provider-init-refcount fda2148
git cherry-pick <SHA комміта C1>
git push -u origin fix/provider-init-refcount

# C2 — ПРОПУСТИТИ, якщо Task 4 крок 6 дав відмову
git checkout -b fix/cm-providers-raii fda2148
git cherry-pick <SHA комміта C2>
git push -u origin fix/cm-providers-raii

# C3
git checkout -b feat/cm-providers-report fda2148
git cherry-pick <SHA комміта C3>
git push -u origin feat/cm-providers-report

git checkout simplyaddin/provider-contract
git push -u origin simplyaddin/provider-contract
cd ../..
```

- [ ] **Крок 2: Відкрити PR #1 (C1)**

```bash
gh pr create -R specinfo-ua/UAPKI --base main --head VSydorenko:fix/provider-init-refcount \
  --title "CM: make provider_init idempotent and reference-counted" \
  --body-file <файл>
```

Текст тіла (англійською, бо репозиторій англомовний) мусить містити:
- **Problem:** a provider DLL is loaded once per process, but several independent consumers
  may initialize it (two modules statically linking UAPKI in one process). The second
  `provider_init()` returns `RET_CM_ALREADY_INITIALIZED`, `CmProviders::loadProvider()`
  registers nothing, and `INIT` reports success with `countCmProviders: 0`.
- **Root cause:** the CM provider API never specified how many owners a process-wide provider
  may have; both bundled providers independently assumed exactly one.
- **Fix:** reference counting in `cm-pkcs12` and `cm-pkcs11`, plus the contract written down
  in `cm-api.h`. `RET_CM_ALREADY_INITIALIZED` is kept for compatibility.
- **Precedent:** the same problem in the PKCS#11 world is solved by p11-kit's managed mode;
  `CKR_CRYPTOKI_ALREADY_INITIALIZED` is already mapped in `cryptoki-storage.cpp:2054`.
- **Test:** the repeated-init scenario, and what it returns before/after.
- Посилання на PR #27 як на сусідній сценарій «library loaded and unloaded multiple times».

- [ ] **Крок 3: Відкрити PR #2 (C2) і PR #3 (C3)**

PR #2 — `UAPKI: own CmStorageProxy via unique_ptr to fix leak on static destruction`. У тілі
**обов'язково** згадати вимір із Task 4: що `FreeLibrary` тепер виконується з деструктора
статика, і що це зміряно на Windows x64/x86 без зависання (навести результат кейса 13).
Якщо вимір дав відмову — **PR не подається**.

PR #3 — `UAPKI: report per-provider load outcome in INIT result`. Наголосити: зміна
**адитивна**, толерантність не змінено, `countCmProviders` не зачеплено.

- [ ] **Крок 4: Створити issue про ідемпотентний `uapki_init`**

```bash
gh issue create -R specinfo-ua/UAPKI \
  --title "Should uapki_init be idempotent, like provider_init?" \
  --body-file <файл>
```

Зміст: `library-init.cpp:224` віддає `RET_UAPKI_ALREADY_INITIALIZED` із порожнім результатом,
хоча бібліотека жива. Це той самий принцип, що й у PR #1, одним шаром вище. Пропозиція:
повертати `RET_OK`, заповнювати `joResult` поточним станом і додати
`"alreadyInitialized": true`, щоб сигнал не загубився. Явно написати, що патч готовий і буде
надісланий на запит — **PR наосліп не подаємо**, бо це зміна поведінки публічного API.

- [ ] **Крок 5: Повернути сабмодуль на інтеграційну гілку**

```bash
git -C extern/uapki checkout simplyaddin/provider-contract
git status --short    # gitlink у корені не має показувати змін
```

Нарізання гілок перемикало спільне дерево сабмодуля — його треба повернути, інакше наступна
збірка піде з однією зміною замість трьох.

---

## Task 11: Анонс, живий прогін, PR у `main`

- [ ] **Крок 1: Анонс зміни контракту сусідній сесії**

Через `ListAgents` знайти сесію `prro-uapki-spec` і надіслати `SendMessage` з:
- новий формат помилки `INIT`: `errorCode 502`, `error` на `NO_CM_PROVIDERS_LOADED`,
  оригінал у `uapkiResponse` — з посиланням на `UAPKIConnectHelper.cpp` і номер рядка;
- `INIT` тепер ідемпотентний: повторний виклик дає `errorCode 0` з
  `result.alreadyInitialized = true`, і параметри такого виклику **не застосовуються**;
- окремо: звіряти `countCmProviders` вручну більше не треба.

Якщо сесії немає в списку — записати текст анонсу у
`docs/tasks/2026-09-22_provider_reinit_defect.md` розділом «Анонс для споживачів» і сказати
про це користувачу.

- [ ] **Крок 2: Живий прогін у 1С**

Відкрити `bin/Release/SimplyAddinConnect.epf` → `Подключить компоненту` →
`Створити обʼєкт UAPKI` → `INIT`. Закрити обробку **не закриваючи 1С**, відкрити заново,
повторити `INIT`. Очікується: обидва рази провайдер піднімається
(`countCmProviders: 1`), помилки `4102`/`4121` не виникають.

**Це єдиний пункт, що потребує живої 1С.** Якщо платформи немає — роботу вважати
завершеною з **явною відміткою** «живий прогін не перевірено», а не мовчки.

- [ ] **Крок 3: Створити PR у `main`**

```bash
gh pr create --base main --title "Контракт ініціалізації провайдера НКІ: ідемпотентність і облік посилань" --body-file <файл>
```

У тілі: посилання на спеку й план, перелік C1–C5, результат гейта (числа PASS для x64 і x86),
стан апстрім-PR, і **явно** — що з кроком 2 (живий прогін) і що з рішенням по C2.

---

## Самоперевірка плану

**Покриття спеки:** C1 → Task 3; C2 → Task 4 (з умовою відмови, §5.4 спеки); C3 → Task 5;
C4 → Task 6; C5 → Task 7; §11 тестування → Tasks 1, 2, 4, 5, 6, 7 (рівень L1.5 і кейси
12–15); §12 документація → Task 8; §13 критерії приймання → Task 9 (гейт) і Task 11; §10 доставка → Task 10.
Критерій 9 спеки (живий прогін) — Task 11 крок 2. Критерій 8 (анонс) — Task 11 крок 1.

**Узгодженість імен:** `ProvidersLoadedOrFail` оголошено в Task 6 кроці 1 й ужито в Task 6
кроці 3 та Task 7 кроці 3. `HandleAlreadyInitialized` — Task 7 кроки 1–3.
`case12_twoInstances`, `case13_unloadSafety`, `case14_zeroProvidersIsError`,
`case15_idempotentInit` — оголошені й додані в `switch` у своїх задачах. Межа `kase > 15`
піднімається один раз (Task 1) і покриває всі чотири кейси.

**Залежності між задачами:** Task 3 потребує Tasks 1–2 (червоні тести). Task 7 крок 3 замінює
блок, створений Task 6 кроком 3 — виконувати в порядку. Task 10 потребує SHA коммітів із
Tasks 3, 4, 5 і зеленого Task 9. Task 8 крок «`AGENTS.md`» свідомо відкладено до мерджу апстріму.
