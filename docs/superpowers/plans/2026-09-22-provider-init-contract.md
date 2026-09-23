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

## Статус виконання (2026-09-23)

- **Tasks 1–9, 9-біс, 9-тер, 12 — виконано**, гейт на `e9ccd6e`: x64 39 PASS, x86 38 PASS + 1 штатний SKIP.
- **Task 10 — подано в апстрім, описи двомовні EN+UA:** [PR #31](https://github.com/specinfo-ua/UAPKI/pull/31) (C1),
  [PR #32](https://github.com/specinfo-ua/UAPKI/pull/32) (C2), [PR #33](https://github.com/specinfo-ua/UAPKI/pull/33) (C3),
  [issue #34](https://github.com/specinfo-ua/UAPKI/issues/34). CI (Linux/Windows) зелений у всіх; у #31 червоний
  quality gate SonarCloud (`cpp:S5421` ×4, `cpp:S995` ×2) → **Task 13**.
- **Task 11:** крок 1 (анонс) — надіслано; крок 2 (живий прогін 1С) — виконано, Task 12 теж підтверджено в 1С
  (3.2.1.232); крок 3 — PR у `main`, після Task 13.
- **Гілки форку приведено до `docs/architecture/uapki.md` §7.1:** `main-dev` = `af63339` (наші правки), `main` =
  `fda2148` (дзеркало апстріму), сабмодуль на `main-dev`, `.gitmodules` без змін. Інтеграційну гілку
  `simplyaddin/provider-contract` видалено — **у тексті плану нижче читати її як `main-dev`**.
- **Після мерджу апстріму (окремо, пізніше):** sync-PR `main`→`main-dev` (§7.2 архітектурного документа),
  сабмодуль — на апстрімовий тег.

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
14 (нуль провайдерів → 502), 15 (ідемпотентний `INIT`), 16 (завершення процесу з
завантаженою DLL — додано в Task 4 за результатом контрольної точки 4). Поточна межа в `main()` —
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

- [ ] **Крок 1: Додати кейс 12 і хелпер перепису модулів**

> **Виправлено 2026-09-22 за зауваженням виконавця.** Перша редакція клала провайдера поруч
> із кожною копією. `ResolveProviderDir` першим дивиться в каталог самої DLL
> (`src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp:377`), тож провайдер вантажився б із
> ДВОХ шляхів -> два модулі провайдера з двома глобалами -> дефект не відтворюється, кейс
> зеленіє, нічого не довівши. Тепер провайдер береться з ОДНОГО явного `cmProviders.dir`, а
> перепис модулів процесу ДОВОДИТЬ передумову (testing-rules, правило 2).

У блок include на початку `tests/native_host.cpp`, одразу після `#include <shellapi.h>`:

```cpp
#include <psapi.h>      // EnumProcessModules — перепис модулів для кейса 12
```

і поруч із `#pragma comment(lib, "shell32.lib")`:

```cpp
#pragma comment(lib, "psapi.lib")    // EnumProcessModules
```

Вставити безпосередньо перед `static void usage()`:

```cpp
// Повні шляхи всіх завантажених у процес модулів із заданим БАЗОВИМ іменем
// (без урахування регістру). Потрібен, щоб ДОВЕСТИ стан перед перевіркою:
// скільки саме модулів головної DLL і провайдера живе в процесі й звідки.
static std::vector<std::wstring> loadedModulePaths(const std::wstring& baseName) {
    std::vector<std::wstring> out;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return out;
    const DWORD n = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < n && i < 1024; ++i) {
        wchar_t path[MAX_PATH * 2];
        const DWORD len = GetModuleFileNameW(mods[i], path, (DWORD)(sizeof(path) / sizeof(path[0])));
        if (len == 0) continue;
        const std::wstring full(path, len);
        const size_t slash = full.find_last_of(L"\\/");
        const std::wstring base = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
        if (_wcsicmp(base.c_str(), baseName.c_str()) == 0) out.push_back(full);
    }
    return out;
}

// ========================================================================
// КЕЙС 12 — ДВА екземпляри головної DLL, ОДИН модуль провайдера.
// Модель сценарію 1С: компонента в процесі двічі (ExtCompT + тимчасова копія
// v8_*_c.), а провайдера обидва беруть з ОДНОГО каталогу розгортання
// %LOCALAPPDATA%\SimplyAddinConnect\providers\<VERSION_FULL>\ — бо версія та
// сама. Два РІЗНІ шляхи до головної DLL -> два модулі зі своїми статиками UAPKI;
// ОДИН шлях до провайдера -> один модуль, один глобал cm_pkcs12.
//
// Пастка, яку кейс мусить виключити: провайдер із ДВОХ різних шляхів дав би
// ДВА модулі провайдера з двома глобалами — дефект не відтворився б, і кейс
// зеленів би, нічого не довівши. Тому провайдер — з явного СПІЛЬНОГО
// cmProviders.dir, а перепис модулів доводить передумову ДО перевірки.
//
// До C1: другий INIT дає countCmProviders == 0.
// ========================================================================
static bool case12_twoInstances(const std::wstring& mainDllSrc, const std::wstring& binDir) {
    printf("== Case 12: два екземпляри головної DLL, один модуль провайдера ==\n");

    const std::wstring dllName  = std::wstring(L"SimplyAddinConnectWin") + ARCH_W + L".dll";
    const std::wstring provName = std::wstring(L"cm-pkcs12") + ARCH_W + L".dll";

    std::wstring dirA = makeTempDir(L"case12a");
    std::wstring dirB = makeTempDir(L"case12b");
    CHECK(!dirA.empty() && !dirB.empty() && dirA != dirB, "створено два різні тимчасові каталоги");

    const std::wstring dllA = dirA + L"\\" + dllName;
    const std::wstring dllB = dirB + L"\\" + dllName;
    CHECK(CopyFileW(mainDllSrc.c_str(), dllA.c_str(), FALSE) != 0, "скопійовано головну DLL -> A");
    CHECK(CopyFileW(mainDllSrc.c_str(), dllB.c_str(), FALSE) != 0, "скопійовано головну DLL -> B");
    // Провайдера поруч із копіями НЕ кладемо: ResolveProviderDir узяв би каталог
    // кожної копії, і модулів провайдера стало б два.

    // Обидва екземпляри — з ОДНОГО каталогу провайдера (формат як у кейсі 3:
    // завершальний роздільник обов'язковий, арх-суфікс дописує хелпер).
    json p;
    p["offline"] = true;
    p["cmProviders"]["dir"] = fwd(binDir + L"\\");
    p["cmProviders"]["allowedProviders"] = json::array({ json{{"lib", "cm-pkcs12"}} });
    const std::string initParams = p.dump();

    // --- Екземпляр A -----------------------------------------------------
    Component a;
    if (!a.load(dllA)) return false;
    std::string respA = a.call("INIT", initParams);
    printf("  A INIT: %s\n", respA.c_str());
    json jA;
    CHECK(errCode(respA, jA) == 0, "A: INIT errorCode == 0");
    CHECK(jA["result"]["countCmProviders"].get<long>() == 1, "A: countCmProviders == 1");

    // --- Екземпляр B — ОКРЕМИЙ модуль, свіжі статики UAPKI ---------------
    Component b;
    if (!b.load(dllB)) return false;
    std::string respB = b.call("INIT", initParams);
    printf("  B INIT: %s\n", respB.c_str());
    json jB;
    CHECK(errCode(respB, jB) == 0, "B: INIT errorCode == 0 (свіжі статики UAPKI)");

    // --- Доказ передумови: процес саме в тому стані, який кейс моделює ---
    const std::vector<std::wstring> mains = loadedModulePaths(dllName);
    const std::vector<std::wstring> provs = loadedModulePaths(provName);
    for (const auto& m : mains) printf("  [module] %s\n", w2u8(m).c_str());
    for (const auto& m : provs) printf("  [module] %s\n", w2u8(m).c_str());
    CHECK(mains.size() == 2, "у процесі ДВА модулі головної DLL");
    CHECK(mains.size() == 2 && _wcsicmp(mains[0].c_str(), mains[1].c_str()) != 0,
          "модулі головної DLL — з РІЗНИХ шляхів");
    CHECK(provs.size() == 1, "у процесі ОДИН модуль провайдера (інакше кейс нічого не доводить)");

    // --- Власне перевірка -------------------------------------------------
    CHECK(jB["result"]["countCmProviders"].get<long>() == 1,
          "B: countCmProviders == 1 <- ЦЕ Й Є ДЕФЕКТ до C1");

    // Лічильник, що піднявся, ще не означає робочого провайдера. Друга ознака:
    // OPEN у ДРУГОМУ екземплярі не впирається в 4102 UNKNOWN_PROVIDER.
    // Контейнера навмисно не відкриваємо — досить, щоб помилка була ІНША.
    json op;
    op["provider"] = "PKCS12";
    op["storage"]  = "Z:\\nonexistent-by-design.p12";
    op["password"] = "x";
    op["mode"]     = "RO";
    std::string respOpen = b.call("OPEN", op.dump());
    printf("  B OPEN(неіснуючий): %s\n", respOpen.c_str());
    json jO;
    CHECK(errCode(respOpen, jO) != 4102, "B: OPEN не дає 4102 UNKNOWN_PROVIDER (провайдер зареєстрований)");

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

**Якщо впав будь-який CHECK передумови** (`ДВА модулі головної DLL`, `з РІЗНИХ шляхів`,
`ОДИН модуль провайдера`) — **зупинитися й доповісти архітектору** з рядками `[module]`: кейс
моделює не той стан, і червоне в ньому нічого не означає.

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

- [ ] **Крок 5-біс: Кейс 16 — завершення процесу з ЗАВАНТАЖЕНОЮ головною DLL**

> Додано на контрольній точці 4. Кейс 13 міряє **явний** `FreeLibrary` головної DLL. Але 1С
> може завершитись, не вивантаживши компоненту. Тоді статики UAPKI руйнуються на
> `DLL_PROCESS_DETACH` **під час завершення процесу**, а порядок detach зворотний до
> завантаження: провайдер, завантажений пізніше, отримує `DETACH` **раніше** за нашу DLL. З C2
> деструктор статика кличе `provider_deinit` і `FreeLibrary` у модуль, який уже пройшов
> `DETACH`. Без C2 цей шлях не виконувався взагалі (витік). Це другий шлях, який вмикає C2, і
> кейс 13 його не покриває.

Межу в `main()` підняти з `> 15` до `> 16`, у `usage()` — `<case 1..16>`.

Вставити перед `usage()`:

```cpp
// ========================================================================
// КЕЙС 16 — завершення процесу з ЗАВАНТАЖЕНОЮ головною DLL.
// Кейс 13 міряє явний FreeLibrary. Але 1С може завершитись, не вивантаживши
// компоненту: тоді статики UAPKI руйнуються на DLL_PROCESS_DETACH під час
// завершення процесу. Порядок detach — зворотний до завантаження: провайдер
// (завантажений пізніше) отримує DETACH РАНІШЕ за нашу DLL. З C2 деструктор
// статика кличе provider_deinit і FreeLibrary вже після DETACH провайдера.
// Без C2 цей шлях не виконувався (витік).
// Вердикт — ЛИШЕ exit-код процесу: падіння на завершенні дасть код винятку
// (напр. 0xC0000005), а зависання — таймаут, а не 0.
// ========================================================================
static bool case16_exitWithLoadedDll(const std::wstring& binDir) {
    printf("== Case 16: завершення процесу з завантаженою головною DLL ==\n");
    const std::wstring dllPath = binDir + L"\\SimplyAddinConnectWin" + ARCH_W + L".dll";

    // Навмисно НЕ через RAII: Component на купі й не звільняється, щоб DLL
    // лишилась завантаженою до завершення процесу — як у 1С, що не вивантажила
    // компоненту.
    Component* c = new Component();
    if (!c->load(dllPath)) return false;
    std::string resp = c->call("INIT", buildInit(true));
    json j;
    CHECK(errCode(resp, j) == 0, "INIT errorCode == 0");
    CHECK(j["result"]["countCmProviders"].get<long>() == 1, "countCmProviders == 1");
    printf("  процес завершується з завантаженою DLL; вердикт — exit-код (очікується 0)\n");
    fflush(stdout);
    return true;   // c свідомо не звільняється
}
```

У `switch (kase)`:

```cpp
            case 16: pass = case16_exitWithLoadedDll(binDir);                  break;
```

Прогін на **x64 і x86**, під таймаутом 60 с, **з C1+C2**:

```
bin\Release\native_host_x64.exe 16 "" "" "R:\github\SimplyAddinConnect\bin\Release"
echo exit=%ERRORLEVEL%
```

Критерій — **лише exit-код**: `0` = PASS. Рядок `=== Case 16: PASS ===` друкується **до**
завершення процесу й сам по собі нічого не доводить.

**Негативна верифікація — перевіряємо чутливість, а не дефект.** Природного «червоного» тут
немає: кейс ловить падіння, якого ми сподіваємось не побачити. Тому доводимо інше — що
падіння на завершенні **взагалі видно** в exit-коді. Тимчасово додати в `case16_exitWithLoadedDll`
перед `return true;`:

```cpp
    atexit([] { volatile int* z = nullptr; *z = 1; });   // ТИМЧАСОВО: негативна верифікація
```

Прогнати — exit-код має бути **ненульовим** (`-1073741819` = `0xC0000005`). Прибрати рядок,
перезібрати, переконатися, що exit знову `0`.

- [ ] **Крок 6: РІШЕННЯ за виміром**

**Якщо кейси 13 і 16 зелені на x64 і x86** (у 13 рядок `<- unload() повернувся`
надрукувався і провайдера в процесі немає; у 16 exit-код `0`) — C2 лишається. Додати обидва в
цикл `run_tests.ps1`: `foreach ($kase in 1,2,3,4,6,10,12,13,16)`. Перейти до кроку 7.

**Якщо будь-який із двох прогонів зависає** (60 секунд без завершення) **або падає** — C2
**знімається**. **НЕ через `git checkout --`/`git restore`/`git stash`** — у спільному дереві
вони заборонені (`~/.claude/CLAUDE.md`, розділ про Git), а незастейджена зміна після
`checkout --` зникає безповоротно. Замість цього — зберегти патч у файл і відкотити його
оборотно:

```bash
cd extern/uapki
mkdir -p ../../tmp
git diff -- library/uapki/src/cm-providers.cpp > ../../tmp/c2-raii.patch
git apply -R ../../tmp/c2-raii.patch
cd ../..
```

Патч лишається в `tmp/c2-raii.patch` (тека в `.gitignore`) — його можна прикласти до issue.

Кейси 13 і 16 лишаються в коді, але **не додаються** в цикл `run_tests.ps1` (без C2 кейс 13
червоний за побудовою — це зафіксований факт, а не регресія). У `docs/tech-debt.md` додається **TD-12** з
текстом виміру: що саме зависло/впало, на якій архітектурі. PR #2 в апстрім **не подається**;
натомість у issue (Task 10) додається абзац про виявлене. Далі — Task 5.

- [ ] **Крок 7: Коміт (лише якщо C2 лишилась)**

```bash
cd extern/uapki
git add library/uapki/src/cm-providers.cpp
git commit --only -m "UAPKI: own CmStorageProxy via unique_ptr to fix leak on static destruction" -- library/uapki/src/cm-providers.cpp
cd ../..
git add extern/uapki run_tests.ps1 version.h
git add tests/native_host.cpp
git commit --only -m "fix(uapki): C2 — RAII-володіння проксі провайдера; кейси 13 і 16 у гейт" -- extern/uapki run_tests.ps1 tests/native_host.cpp version.h
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

У `run_tests.ps1`: `foreach ($kase in 1,2,3,4,6,10,12,13,16,14)` (без `13` і `16`, якщо C2 знято).
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

`foreach ($kase in 1,2,3,4,6,10,12,13,16,14,15)` (без `13` і `16`, якщо C2 знято). Перевірити BOM.

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

- [ ] **Крок 5-біс: Виправити врізку задачі-входу про оновлення версії**

У `docs/tasks/2026-09-22_provider_reinit_defect.md`, §4.4, підрозділ «Умову ЗВУЖЕНО», врізка
«А ось де умова відкривається навстіж — оновлення версії розширення» стверджує, що
викочування нової збірки — найімовірніший момент дефекту. Це **хибно** для механізму двох
екземплярів: каталог розгортання провайдера версійний
(`src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp:251-255`, `providers\<VERSION_FULL>\`),
тож дві **різні** версії беруть провайдера з різних шляхів -> два модулі провайдера -> два
глобали -> дефекту немає. Небезпечні сценарії: дві копії **однієї** версії (рядок 3 таблиці
в тому ж підрозділі) і вивантаження/перезавантаження тієї самої DLL (витік сирого
вказівника). Дописати під врізкою абзац «**Виправлено 2026-09-22**» з цим поясненням і
посиланням на код; саму врізку не видаляти — історія міркування лишається видимою.

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

## Task 9-біс: Доробка C1 за контрольною точкою 5 — інша конфігурація й збірка `cm-pkcs11`

> Додано 2026-09-23 на контрольній точці 5. Дві знахідки архітектора в уже закоміченому C1
> (`7afb5e3`), обидві мусять бути закриті **до** апстріму:
>
> 1. **Правило «перемагає перша конфігурація» хибне для `cm-pkcs11`.** Там конфігурація —
>    перелік модулів PKCS#11, які провайдер вантажить
>    (`extern/uapki/library/cm-pkcs11/src/cm-cryptoki.cpp:110-117`); другий споживач мовчки
>    отримав би чужий набір драйверів токенів. Нове правило контракту (спека §4.2): та сама
>    конфігурація → `RET_OK` + лічильник; **інша** → `RET_CM_ALREADY_INITIALIZED`, стан і
>    лічильник без змін. Однаково для обох провайдерів.
> 2. **`cm-pkcs11` ніколи не компілювався нашою збіркою** — його немає ні в `CMake/`, ні в
>    `bin/Release`. Половину PR #1 ніхто не збирав. Подавати в апстрім незібраний код не можна.

**Файли:**
- Modify: `extern/uapki/library/cm-pkcs12/src/main-cm-pkcs12.cpp`
- Modify: `extern/uapki/library/cm-pkcs11/src/main-cm-pkcs11.cpp`
- Modify: `extern/uapki/library/common/cm-api/cm-api.h` (текст контракту)
- Modify: `tests/provider_contract_selftest.cpp` (CHECK на іншу конфігурацію + шлях провайдера з argv)
- Modify: `AGENTS.md` (рівень L1.5 — див. крок 7)

- [ ] **Крок 1: Червоне — CHECK на іншу конфігурацію в контрактному тесті**

У `tests/provider_contract_selftest.cpp` перед фінальним `printf("\n=== provider_contract_selftest ...`
додати блок (після кроку 6 наявного сценарію об'єкт уже звільнено фінальним `deinit`):

```cpp
    // 7. Інша конфігурація — НЕ та сама операція. Провайдер мусить відмовити
    //    ГУЧНО й не чіпати ні стану, ні лічильника (контракт cm-api.h).
    CHECK(api.init(nullptr) == RET_OK, "init з конфігурацією A (null) -> RET_OK");
    static const char CFG_B[] = "{\"differentConfig\":true}";
    CM_ERROR eB = api.init((CM_JSON_PCHAR)CFG_B);
    printf("  init з конфігурацією B -> 0x%04X\n", (unsigned)eB);
    CHECK(eB == RET_CM_ALREADY_INITIALIZED, "init з ІНШОЮ конфігурацією -> RET_CM_ALREADY_INITIALIZED");
    // Доказ, що відмова не збільшила лічильник: ОДИН deinit мусить звільнити об'єкт.
    CHECK(api.deinit() == RET_OK, "deinit після відмови -> RET_OK");
    CHECK(!providerAlive(api), "після ОДНОГО deinit провайдера немає (відмова не рахувалась)");
```

Також дозволити передати шлях провайдера аргументом (потрібно для кроку 5). Замінити на початку
`main()`:

```cpp
int main() {
    std::wstring binDir = u8to16(PCS_BIN_DIR);
    std::wstring path   = binDir + L"\\cm-pkcs12" + ARCH_W + L".dll";
```

на:

```cpp
int wmain(int argc, wchar_t** argv) {
    // argv[1] — повний шлях до провайдера (для разової перевірки cm-pkcs11);
    // без аргументу — cm-pkcs12 з bin/Release, як у гейті.
    std::wstring binDir = u8to16(PCS_BIN_DIR);
    std::wstring path   = (argc > 1) ? std::wstring(argv[1])
                                     : binDir + L"\\cm-pkcs12" + ARCH_W + L".dll";
```

> `wmain` у MSVC-консольній цілі працює без змін CMake. Якщо лінкер попросить точку входу —
> залишити `main()` і взяти аргумент через `CommandLineToArgvW`, як у `tests/native_host.cpp`.

Зібрати, прогнати `provider_contract_selftest_x64.exe` — очікується **червоне** рівно на
`init з ІНШОЮ конфігурацією -> RET_CM_ALREADY_INITIALIZED` (зараз `RET_OK`) і, як наслідок, на
`після ОДНОГО deinit провайдера немає`.

- [ ] **Крок 2: Зелене — правило конфігурації в обох провайдерах**

`main-cm-pkcs12.cpp`: під `static size_t cm_pkcs12_refcnt = 0;` додати

```cpp
//  Configuration text of the first successful initialization. A repeated
//  provider_init() is idempotent only for the SAME configuration; a different
//  one is a different request and is rejected without changing the state.
static std::string cm_pkcs12_initparams;

static std::string params_text (CM_JSON_PCHAR providerParams)
{
    return providerParams ? std::string((const char*)providerParams) : std::string();
}
```

(`#include <string>`, якщо його ще немає серед include файла.)

У `provider_init`: в успішній гілці поруч із `cm_pkcs12_refcnt = 1;` додати
`cm_pkcs12_initparams = params_text(providerParams);`. Гілку `else` замінити на:

```cpp
    else if (params_text(providerParams) == cm_pkcs12_initparams) {
        //  Idempotent for the SAME configuration: the post-condition already holds.
        cm_pkcs12_refcnt++;
        cm_err = RET_OK;
    }
    else {
        //  A different configuration is a different request. Reject it loudly and
        //  leave both the instance and the reference count untouched.
        cm_err = RET_CM_ALREADY_INITIALIZED;
    }
```

У `provider_deinit` поруч із `cm_pkcs12 = nullptr;` додати `cm_pkcs12_initparams.clear();`.
Прибрати з коментаря в `else`-гілці фразу про «configuration of the FIRST initialization wins».

`main-cm-pkcs11.cpp` — те саме дзеркально: `cm_cryptoki_initparams`, `params_text` (static у
цьому файлі), гілки `init`/`deinit`. Коментар «Idempotent, see cm-pkcs12» лишити, він коректний.

`cm-api.h` — у блоці контракту замінити пункт 1) на:

```c
 *   1) be idempotent for the SAME configuration - when the provider is already
 *      initialized with an identical providerParams text (NULL and "" are equal)
 *      it returns RET_OK and increments the reference count;
 *   2) reject a DIFFERENT configuration with RET_CM_ALREADY_INITIALIZED, leaving
 *      the instance and the reference count untouched. A provider configuration
 *      may select what the provider loads (e.g. the list of PKCS#11 modules in
 *      cm-pkcs11), so silently keeping the first one would hand a consumer
 *      something it did not ask for;
 *   3) keep a reference count of init/deinit calls.
```

і речення про `RET_CM_ALREADY_INITIALIZED` у кінці блоку — на:

```c
 * RET_CM_ALREADY_INITIALIZED therefore keeps a precise meaning: "already
 * initialized with a different configuration".
```

Зібрати, прогнати `provider_contract_selftest_x64.exe` і `_x86.exe` — усе `[PASS]`.

- [ ] **Крок 3: Коміт у сабмодулі (окремим коммітом, НЕ amend)**

```bash
cd extern/uapki
git add library/cm-pkcs12/src/main-cm-pkcs12.cpp library/cm-pkcs11/src/main-cm-pkcs11.cpp library/common/cm-api/cm-api.h
git commit --only -m "CM: reject re-initialization with a different configuration" -- library/cm-pkcs12/src/main-cm-pkcs12.cpp library/cm-pkcs11/src/main-cm-pkcs11.cpp library/common/cm-api/cm-api.h
cd ../..
```

Амендити `7afb5e3` не можна — він уже на `origin`. У Task 10 обидва комміти C1 ідуть у PR #1.

- [ ] **Крок 4: Зібрати `cm-pkcs11` окремо — поза `bin/Release` і поза деревом сабмодуля**

Власна збірка апстріму, в scratch-каталог. Мета — щоб **жоден рядок PR #1 не йшов
незібраним**:

```bash
cmake -S extern/uapki/library -B "<scratch>/uapki-pkcs11-x64" -A x64
cmake --build "<scratch>/uapki-pkcs11-x64" --config Release --target cm-pkcs11
cmake -S extern/uapki/library -B "<scratch>/uapki-pkcs11-x86" -A Win32
cmake --build "<scratch>/uapki-pkcs11-x86" --config Release --target cm-pkcs11
git -C extern/uapki status --short     # МУСИТЬ бути чисто — збірка не пише в дерево
```

`<scratch>` — будь-який каталог поза репозиторієм. Ім'я цілі взяти з
`extern/uapki/library/cm-pkcs11/CMakeLists.txt`, якщо воно не `cm-pkcs11`. Якщо збірка тягне
залежності, яких немає (прибудований libcurl вимагає Windows SDK 10.0.26100+ —
`docs/architecture/uapki.md` §7.2), — **зупинитися й написати архітектору**, не обходити.

- [ ] **Крок 5: Контрактний тест проти `cm-pkcs11`**

```
bin\Release\provider_contract_selftest_x64.exe "<шлях до зібраної cm-pkcs11 x64 dll>"
bin\Release\provider_contract_selftest_x86.exe "<шлях до зібраної cm-pkcs11 x86 dll>"
```

`provider_init(nullptr)` у `cm-pkcs11` з порожньою конфігурацією модулів повертає `RET_OK`
(`cm-cryptoki.cpp:103-133` — без модулів цикл завантаження порожній), тож увесь сценарій
застосовний. Очікується: усе `[PASS]`, крім, можливо, пробника `providerAlive`: у
`cm-pkcs11` `provider_open` на неіснуючому URI може повернути інший код, ніж у `cm-pkcs12`.
Критерій пробника той самий — **будь-що, крім `RET_CM_NOT_INITIALIZED`**, означає «живий».
Якщо якийсь CHECK червоний — **зупинитися й прислати вивід архітектору**.

Цей прогін — разовий доказ для PR #1, у гейт він **не** входить (гейт `cm-pkcs11` не збирає).
Вивід зберегти: він цитується в тілі PR.

- [ ] **Крок 6: Повний гейт повторно — x64 і x86**

C1 змінився, отже Task 9 крок 1 — наново. Цикл кейсів не міняється.

- [ ] **Крок 7: `AGENTS.md` — рівень L1.5**

Відкладено до мерджу апстріму лише зняття винятку «uapki за `main-dev`». Опис рівнів гейта —
**стабільна частина `AGENTS.md`**, і L1.5 має бути там зараз:

- таблиця тестових цілей (розділ «Тести»): рядок
  `| provider_contract_selftest.exe | L1.5 | ні (але без -WithUAPKI — SKIP, файлу провайдера немає) | контракт провайдера НКІ напряму через LoadLibraryW: ідемпотентний init для тієї самої конфігурації, відмова для іншої, облік посилань |`
- перелік «збираються завжди при `-WithTests`» — додати `provider_contract_selftest`;
- порядок у абзаці «Запуск» — `… → L1 selftest по сценаріях → L1.5 provider_contract_selftest → L2/L3 native_host → …`;
- абзац про `-NoUapki` — L1.5 → SKIP (exit 3), не FAIL;
- `tests/provider_contract_selftest.cpp` — у перелік файлів теки `tests/` (розділ «Тести»,
  перший абзац) і в дерево «Структура».

`python scripts/check-doc-anchors.py` — exit 0.

- [ ] **Крок 8: Коміт у корені, пуш (сабмодуль першим), контрольна точка**

```bash
git -C extern/uapki push origin simplyaddin/provider-contract
git add extern/uapki tests/provider_contract_selftest.cpp AGENTS.md version.h
git commit --only -m "fix(uapki): C1 — інша конфігурація провайдера відхиляється гучно; L1.5 в AGENTS.md" -- extern/uapki tests/provider_contract_selftest.cpp AGENTS.md version.h
git push
```

Надіслати архітектору: результат кроку 5 (обидві архітектури), таблиці гейта кроку 6, SHA
нового комміту в сабмодулі. **Task 10 — після відповіді.**

---

## Task 9-тер: Вердикт у лозі — ПІСЛЯ пост-обробки `INIT`

> Додано 2026-09-23 на контрольній точці 5-біс. Фінальне рев'ю гілки позначило як «косметику»,
> що WARN «завершилась с ошибкой» пишеться до перевороту `4106 → успіх`. Це не косметика, і
> сторін у неї дві. Вердикт логується на `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp:799-805`,
> а пост-обробка `INIT` (C5, C4) іде після нього, на `:810-816`:
>
> - повторний `INIT` (C5): у лозі «завершилась с ошибкой», а 1С отримує `errorCode: 0`;
> - нуль провайдерів (C4): у лозі «**выполнена успешно**», а 1С отримує `502`. Лог бреше
>   саме в той бік, який ця задача закриває.
>
> Весь дефект розслідувався за логом компоненти (задача-вхід §1). Лог, що суперечить відповіді,
> — пастка для наступного розслідування. Код відповіді 1С не змінюється, лише лог.

**Файли:**
- Modify: `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp:798-818`
- Modify: `tests/native_host.cpp` (`case14_zeroProvidersIsError`, `case15_idempotentInit`)

- [ ] **Крок 1: Червоне — перевірка логу в кейсах 14 і 15**

Механіка — як у `case6_passwordNotLogged` (`tests/native_host.cpp:853`): лог у тимчасовий
файл із PID в імені, `enableLogging` **до** `INIT`, `unload()` **до** читання (закриває лог).

На початку `case15_idempotentInit`, одразу після `if (!c.load(dllPath)) return false;`:

```cpp
    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring logPath = std::wstring(tmpDir) + L"sac_case15_" + std::to_wstring(GetCurrentProcessId()) + L".log";
    DeleteFileW(logPath.c_str());
    CHECK(c.enableLogging(L"Trace", logPath), "лог увімкнено");
```

Наприкінці, замість `c.unload(); return true;`:

```cpp
    c.unload();                              // закрити лог перед читанням
    std::string logText;
    CHECK(readFileText(logPath, logText), "лог прочитано");
    // Позитивний контроль ПЕРЕД перевіркою відсутності: доводить, що кодування логу й
    // літерала збігаються. Без нього «рядка немає» зеленіло б і тоді, коли пошук
    // просто не вміє знайти кирилицю (testing-rules, правило 2).
    CHECK(logText.find("Команда UAPKI INIT выполнена успешно") != std::string::npos,
          "у лозі є вердикт успіху INIT (позитивний контроль кодування)");
    CHECK(logText.find("Команда UAPKI INIT завершилась с ошибкой") == std::string::npos,
          "у лозі НЕМАЄ вердикту помилки INIT — лог каже те саме, що отримала 1С");
    DeleteFileW(logPath.c_str());
    return true;
```

У `case14_zeroProvidersIsError` — те саме з `sac_case14_`, але перевірки дзеркальні й
контроль кодування **інший**:

```cpp
    // Контроль кодування — рядок, що пишеться ЗАВЖДИ, незалежно від дефекту
    // (ProvidersLoadedOrFail логує його і до виправлення, і після).
    CHECK(logText.find("Провайдеры НКИ не загружены") != std::string::npos,
          "у лозі є рядок ProvidersLoadedOrFail (позитивний контроль кодування)");
    CHECK(logText.find("Команда UAPKI INIT завершилась с ошибкой") != std::string::npos,
          "у лозі ЗАПИСАНО вердикт помилки INIT — 1С отримала 502");
    CHECK(logText.find("Команда UAPKI INIT выполнена успешно") == std::string::npos,
          "у лозі НЕМАЄ вердикту успіху INIT — 1С отримала 502");
```

> **Виправлено 2026-09-23 за зупинкою виконавця.** Перша редакція брала контролем кодування в
> кейсі 14 рядок «завершилась с ошибкой». До виправлення він **не пишеться взагалі**:
> `ProvidersLoadedOrFail` робить `return false` уже після запису вердикту успіху
> (`UAPKIConnectHelper.cpp:799-805` → `:810-816`). Тобто той «контроль» вимірював сам дефект,
> а не кодування. Контроль кодування мусить бути рядком, **незалежним** від дефекту.

Зібрати, прогнати кейси 14 і 15. Очікується:
- кейс 15: контроль кодування зелений, червоне на «НЕМАЄ вердикту помилки»;
- кейс 14: контроль кодування зелений, червоне на «ЗАПИСАНО вердикт помилки» (до «НЕМАЄ»
  прогін не дійде — перший провал зупиняє кейс).

Якщо червоний саме **контроль кодування** — зупинитися й написати архітектору: тоді проблема
в кодуванні, а не в порядку логування.

- [ ] **Крок 2: Зелене — логувати вердикт після пост-обробки**

У `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp` замінити блок від
`bool isSuccess = IsOperationSuccess(responseJson);` до `return isSuccess;` включно на:

```cpp
            // Проверяем успешность операции по полю errorCode
            bool isSuccess = IsOperationSuccess(responseJson);

            // Для INIT: спершу робимо INIT ідемпотентним (4106 -> вимір PROVIDERS),
            // і лише потім застосовуємо політику нуля провайдерів. Порядок важливий:
            // після заміни відповіді вона вже несе реальний countCmProviders.
            if (methodUpper == "INIT") {
                if (HandleAlreadyInitialized(responseJson)) {
                    isSuccess = true;
                }
                if (!ProvidersLoadedOrFail(paramsJson, responseJson)) {
                    isSuccess = false;
                }
            }

            // Вердикт у лог — ПІСЛЯ пост-обробки INIT: лог мусить казати те саме, що
            // отримала 1С. Інакше повторний INIT логувався б як помилка, а INIT без
            // провайдерів — як успіх.
            if (isSuccess) {
                NEUTRAL_REPORT_INFO("UAPKIConnectHelper", "Команда UAPKI " + method + " выполнена успешно");
            } else {
                NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Команда UAPKI " + method + " завершилась с ошибкой");
            }

            return isSuccess;
```

Поведінка для 1С не змінюється: при `ProvidersLoadedOrFail == false` раніше був
`return false;`, тепер `isSuccess = false` і той самий `return isSuccess;`.

- [ ] **Крок 3: Зібрати, прогнати кейси 6, 14, 15 — зелене; потім повний гейт x64 і x86**

Кейс 6 — бо він теж читає той самий лог і не мусить зламатися.

- [ ] **Крок 4: Коміт і пуш**

```bash
git add src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp tests/native_host.cpp version.h
git commit --only -m "fix(uapki): вердикт INIT у лозі — після пост-обробки, лог каже те саме, що 1С" -- src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp tests/native_host.cpp version.h
git push
```

---

## Task 10: Апстрім — три PR і issue

**Передумова:** Tasks 9, 9-біс і 9-тер зелені на x64 і x86, архітектор підтвердив контрольні точки.

**Файли:** нові гілки у форку `VSydorenko/UAPKI` (в каталозі `extern/uapki`).

> **Це дія назовні, у чужому репозиторії.** Тексти PR і issue спершу надсилаються архітектору
> на рев'ю, фінальне «подавай» дає користувач. Без обох — не подавати.

- [ ] **Крок 1: Нарізати гілки під PR через `cherry-pick`**

Зміни файлово не перетинаються, тож конфліктів не буде за побудовою.

```bash
cd extern/uapki
git log --oneline fda2148..simplyaddin/provider-contract   # переписати SHA кожного комміта

# C1 — ДВА комміти: 7afb5e3 і комміт Task 9-біс кроку 3
git checkout -b fix/provider-init-refcount fda2148
git cherry-pick 7afb5e3 <SHA комміта Task 9-біс>
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
- **Fix:** reference counting in `cm-pkcs12` and `cm-pkcs11`, idempotent for the SAME
  configuration; a DIFFERENT configuration is rejected with `RET_CM_ALREADY_INITIALIZED`
  without touching the state (for `cm-pkcs11` the configuration selects which PKCS#11 modules
  are loaded). The contract is written down in `cm-api.h`.
- **Verified:** both providers built and exercised by the contract test on Windows x64/x86
  (quote the Task 9-біс step 5 output).
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

- [x] **Крок 2: Живий прогін у 1С** — ВИКОНАНО 2026-09-23 (сесія `simplyaddinconnect-4b`, збірка
  `3.2.1.230`). Друге відкриття обробки: «перша ініціалізація», провайдерів 1, `OPEN` ok (раніше
  0 і `4102`). Повторний `INIT` у тій самій формі: `alreadyInitialized: true`,
  `countCmProviders: 1`. Лог — `C:\log\SimplyAddinConnect.log:1647`, `:1691`, `:1709`;
  провайдер із `providers\3.2.1.230\` (`:1635`). **Перепис модулів процесу `1cv8c`**: дві копії
  головної DLL (`%TEMP%\12\v8_DC35_12.tmp`, `v8_DC35_28.tmp`, обидві 3.2.1.230) і **одна**
  `cm-pkcs12_x64.dll`. Механізм двох екземплярів тепер **виміряно в 1С**, не реконструйовано.
  Кожне відкриття зовнішньої обробки дає новий модуль, старі лишаються завантаженими. Живий
  прогін також виявив прогалину C5 — див. Task 12.

> **Перед збіркою для прогону — `VERSION_REVISION` 0 → 1 у `VERSION.txt`** (`3.2.0` → `3.2.1`),
> за спекою §10.5: змінилась поведінка, видима 1С (C4, C5). У першій редакції плану кроку не
> було. Живий прогін іде на тій збірці й під тим номером, що піде далі; нове ім'я DLL заодно
> закриває кеш `ExtCompT`. Перевірити в лозі компоненти, що завантажено саме нову версію.

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

## Task 12: C5 — та сама конфігурація успіх, інша — `4106` (спека §8.4)

> Додано 2026-09-23 за живим прогоном у 1С: повторний `INIT` з ІНШОЮ конфігурацією отримав
> `errorCode: 0` і `alreadyInitialized: true`. Це суперечить принципу C1 (спека §4.2, §8.4).
> **Виконувати лише коли сесія `simplyaddinconnect-4b` звільнить `bin/Release`** — вона веде
> живий ретест у 1С.

**Файли:**
- Modify: `src/helpers/UAPKIConnect/UAPKIConnectHelper.h` (сигнатура `HandleAlreadyInitialized`)
- Modify: `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp` (статик параметрів, порівняння, `DEINIT`)
- Modify: `tests/native_host.cpp` (кейс 17; межа `kase > 16` → `> 17`)
- Modify: `run_tests.ps1` (кейс 17 у цикл; BOM)
- Modify: `docs/integration-1c/uapki.md` §4.1

**Інтерфейси:**
- `static bool HandleAlreadyInitialized(const nlohmann::json& params, std::string& responseJson);`
  → `true`: відповідь замінено на успішну (та сама конфігурація, провайдери ≥ 1); `false`:
  відповідь або не зачеплено, або замінено діагностичною `4106` (інша конфігурація).
- Нове в анонімному namespace `UAPKIConnectHelper.cpp`:
  `std::mutex g_initMutex; bool g_hasInitParams = false; nlohmann::json g_initParams;`
  `nlohmann::json NormalizeInitParams(const nlohmann::json& p)` — копія без `skipSelfTest`.

- [ ] **Крок 1: Червоне — кейс 17**

```cpp
// ========================================================================
// КЕЙС 17 — повторний INIT з ІНШОЮ конфігурацією (спека §8.4).
// Та сама конфігурація -> alreadyInitialized (кейс 15). Інша -> 4106 з
// переліком ключів, що різняться: тиха підміна конфігурації — брехня про
// успіх (для ПРРО: офлайн замість онлайну з TSP).
// ========================================================================
static bool case17_initConfigMismatch(const std::wstring& binDir) {
    printf("== Case 17: повторний INIT з іншою конфігурацією ==\n");
    const std::wstring dllPath = binDir + L"\\SimplyAddinConnectWin" + ARCH_W + L".dll";
    Component c;
    if (!c.load(dllPath)) return false;

    json a; a["offline"] = true;
    json j;
    CHECK(errCode(c.call("INIT", a.dump()), j) == 0, "INIT(A) errorCode == 0");
    CHECK(!j["result"].contains("alreadyInitialized"), "INIT(A) — справжня ініціалізація");

    // skipSelfTest не є конфігурацією — та сама A.
    json a2 = a; a2["skipSelfTest"] = true;
    CHECK(errCode(c.call("INIT", a2.dump()), j) == 0, "INIT(A + skipSelfTest) errorCode == 0");
    CHECK(j["result"].value("alreadyInitialized", false), "INIT(A + skipSelfTest) -> alreadyInitialized");

    // Інша конфігурація: безпечний ключ, без мережі.
    json b = a; b["validationByCrl"] = true;
    std::string rb = c.call("INIT", b.dump());
    printf("  INIT(B) resp: %s\n", rb.c_str());
    CHECK(errCode(rb, j) == 4106, "INIT(B) errorCode == 4106 <- ЧЕРВОНЕ до Task 12 (буде 0)");
    CHECK(j["result"].value("alreadyInitialized", false), "INIT(B) несе alreadyInitialized");
    bool mentions = false;
    if (j["result"].contains("configMismatch") && j["result"]["configMismatch"].is_array())
        for (const auto& k : j["result"]["configMismatch"])
            if (k.is_string() && k.get<std::string>() == "validationByCrl") mentions = true;
    CHECK(mentions, "configMismatch називає validationByCrl");

    // Змінити конфігурацію — лише через DEINIT + INIT{skipSelfTest}.
    CHECK(errCode(c.call("DEINIT", ""), j) == 0, "DEINIT errorCode == 0");
    json b2 = b; b2["skipSelfTest"] = true;
    CHECK(errCode(c.call("INIT", b2.dump()), j) == 0, "INIT(B + skipSelfTest) після DEINIT == 0");
    CHECK(!j["result"].contains("alreadyInitialized"), "INIT(B) після DEINIT — справжня ініціалізація");
    CHECK(j["result"]["countCmProviders"].get<long>() == 1, "INIT(B) після DEINIT: countCmProviders == 1");

    c.unload();
    return true;
}
```

`switch`: `case 17: pass = case17_initConfigMismatch(binDir); break;`. Межа `kase > 17`, `usage` —
`1..17`. Зібрати, прогнати — червоне рівно на `INIT(B) errorCode == 4106`.

- [ ] **Крок 2: Зелене — реалізація**

В анонімному namespace `UAPKIConnectHelper.cpp` (поруч з `ModuleAnchor`):

```cpp
    // Параметри INIT, що СПРАВДІ ініціалізував бібліотеку в цьому модулі (після
    // автоінʼєкції). Час життя = статики UAPKI того самого модуля. Спека §8.4.
    std::mutex     g_initMutex;
    bool           g_hasInitParams = false;
    nlohmann::json g_initParams;

    // skipSelfTest — прапорець процедури, не конфігурація: у порівнянні не бере участі.
    nlohmann::json NormalizeInitParams(const nlohmann::json& p) {
        nlohmann::json c = p;
        if (c.is_object()) c.erase("skipSelfTest");
        return c;
    }
```

(`<mutex>` уже є в `src/core/pch.h:34`; додати `#include <set>` в include-блок файла — для переліку ключів.)

`HandleAlreadyInitialized` отримує параметр `const nlohmann::json& params`. Після того, як
встановлено `4106` і зміряно `count` через `PROVIDERS` (наявний код), **перед** формуванням
успішної відповіді:

```cpp
        bool same = false;
        nlohmann::json mismatch = nlohmann::json::array();
        {
            std::lock_guard<std::mutex> lock(g_initMutex);
            if (g_hasInitParams) {
                const nlohmann::json now  = NormalizeInitParams(params);
                const nlohmann::json then = NormalizeInitParams(g_initParams);
                same = (now == then);
                if (!same && now.is_object() && then.is_object()) {
                    std::set<std::string> keys;
                    for (auto it = now.begin(); it != now.end(); ++it)   keys.insert(it.key());
                    for (auto it = then.begin(); it != then.end(); ++it) keys.insert(it.key());
                    for (const auto& k : keys) {
                        const bool inNow = now.contains(k), inThen = then.contains(k);
                        if (inNow != inThen || (inNow && now[k] != then[k])) mismatch.push_back(k);
                    }
                }
            }
        }

        if (!same) {
            NEUTRAL_REPORT_WARN("UAPKIConnectHelper", "Повторный INIT с ДРУГОЙ конфигурацией отклонён: " + mismatch.dump());
            nlohmann::json err;
            err["errorCode"] = UAPKI_ALREADY_INITIALIZED;
            err["error"]     = "ALREADY_INITIALIZED: библиотека уже инициализирована с другой конфигурацией; сменить её можно только через DEINIT и INIT с skipSelfTest";
            err["method"]    = "INIT";
            err["result"]["alreadyInitialized"] = true;
            err["result"]["configMismatch"]     = mismatch;
            err["result"]["countCmProviders"]   = count;
            responseJson = err.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            return false;
        }
```

> Порядок у функції: спершу виміряти `count` (наявна проба `PROVIDERS`), потім порівняння. Якщо
> `count < 1` — наявна гілка `return false` без заміни лишається першою: нуль провайдерів
> обробляє C4 (`502`) і в разі невідповідності конфігурації.

У `ExecuteUapkiCommand`, блок `INIT`:

```cpp
            if (methodUpper == "INIT") {
                // Справжня ініціалізація (бібліотека сама відповіла 0) — запам'ятати
                // параметри, з якими підняли бібліотеку в цьому модулі (спека §8.4).
                if (isSuccess) {
                    std::lock_guard<std::mutex> lock(g_initMutex);
                    g_initParams    = paramsJson;
                    g_hasInitParams = true;
                }
                if (HandleAlreadyInitialized(paramsJson, responseJson)) {
                    isSuccess = true;
                }
                if (!ProvidersLoadedOrFail(paramsJson, responseJson)) {
                    isSuccess = false;
                }
            }
            else if (methodUpper == "DEINIT" && isSuccess) {
                std::lock_guard<std::mutex> lock(g_initMutex);
                g_hasInitParams = false;
                g_initParams    = nlohmann::json();
            }
```

Прогнати кейси 15 і 17 — зелене; кейс 12 (два модулі — у кожного свій статик) — зелене.

- [ ] **Крок 3: Документація для 1С — `docs/integration-1c/uapki.md` §4.1**

Абзац «**`INIT` ідемпотентний.**» замінити: повторний `INIT` з **тією самою** конфігурацією
(`skipSelfTest` не враховується) → `errorCode: 0` і `alreadyInitialized: true`; з **іншою** →
`errorCode: 4106`, у `result.configMismatch` — ключі, що різняться; змінити конфігурацію можна
лише `DEINIT` + `INIT` з `"skipSelfTest": true`. Гард якорів — exit 0.

- [ ] **Крок 4: Гейт x64 + x86, коміт, пуш**

Цикл `run_tests.ps1`: додати `17`. BOM перевірити.

```bash
git add src/helpers/UAPKIConnect/UAPKIConnectHelper.h src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp tests/native_host.cpp run_tests.ps1 docs/integration-1c/uapki.md version.h
git commit --only -m "fix(uapki): C5 — повторний INIT з іншою конфігурацією відхиляється 4106 (спека §8.4)" -- src/helpers/UAPKIConnect/UAPKIConnectHelper.h src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp tests/native_host.cpp run_tests.ps1 docs/integration-1c/uapki.md version.h
git push
```

- [ ] **Крок 5: Анонс (Task 11 крок 1) доповнити** цим правилом — сесії `prro-uapki-spec` і
  `simplyaddinconnect-4b` (тестова обробка отримає `4106` на своєму другому варіанті `INIT`).

---

## Task 13: Стан провайдера — всередині екземпляра (SonarCloud #31 + порядок руйнування)

> Додано 2026-09-23. **Дві причини, одне виправлення.**
>
> 1. **SonarCloud quality gate у PR #31 червоний:** `new_maintainability_rating` = B. Знахідки —
>    `cpp:S5421` «Global variables should be const» ×4 (`cm_pkcs12_refcnt`, `cm_pkcs12_initparams`,
>    `cm_cryptoki_refcnt`, `cm_cryptoki_initparams`) і `cpp:S995` ×2 (`params_text` приймає
>    `unsigned char*`, а не вказівник на const). Тести апстріму (Linux/Windows) зелені. Мейнтейнери
>    Sonar цінують — у них є злиті PR «fix SonarCloud», тож #31 має бути чистим.
> 2. **Невизначена поведінка при завершенні процесу (знайдено архітектором при розборі).**
>    `static std::string cm_pkcs12_initparams` має нетривіальний деструктор. При завершенні процесу
>    з завантаженою DLL хоста провайдер отримує `DLL_PROCESS_DETACH` **першим** (кейс 16), і рядок
>    руйнується; далі деструктор статика реєстру (C2, PR #32) кличе `provider_deinit()`, а той —
>    `.clear()` на зруйнованому об'єкті. Кейс 16 зелений, бо пам'ять модуля ще не звільнена, — але це
>    UB. Сама лише пара #31+#32 його створює, тож виправляємо в #31.
>
> **Рішення:** лічильник і текст конфігурації переїжджають **у сам екземпляр** (`CmPkcs12` /
> `CmCryptoki`). Їхній час життя збігається з часом життя провайдера — саме ті, що треба. Існуючий
> глобал-вказівник `cm_pkcs12`/`cm_cryptoki` лишається (тривіально руйнований, рядок не новий):
> після `DETACH` він цілий, а купа процесу жива, тож пізній `provider_deinit()` працює з валідним
> об'єктом. Нових глобалів немає, нових `new`/`delete` немає — рядки створення й видалення
> екземпляра не чіпаємо.

**Файли (сабмодуль, гілка `main-dev`):**
- Modify: `extern/uapki/library/cm-pkcs12/src/cm-pkcs12.h` (клас `CmPkcs12`)
- Modify: `extern/uapki/library/cm-pkcs12/src/main-cm-pkcs12.cpp`
- Modify: `extern/uapki/library/cm-pkcs11/src/cm-cryptoki.h` (клас `CmCryptoki`)
- Modify: `extern/uapki/library/cm-pkcs11/src/main-cm-pkcs11.cpp`

- [ ] **Крок 1: `CmPkcs12` — стан життєвого циклу в класі**

У `cm-pkcs12.h`, одразу під `FileStorageParam m_DefaultParam;` (до `public:`):

```cpp
    //  Lifecycle of the process-wide provider instance (see the contract in cm-api.h):
    //  how many consumers hold it and with which configuration it was created. Kept
    //  INSIDE the heap instance on purpose: a namespace-scope std::string would be
    //  destroyed on the provider's DLL_PROCESS_DETACH, while a consumer's static
    //  destructor may still call provider_deinit() later, at process exit.
    size_t      m_RefCount = 1;
    std::string m_InitParams;
```

і в `public:`-секцію (поруч із `getDefaultParam`):

```cpp
    void setInitParams (const std::string& params) {
        m_InitParams = params;
    }
    bool isSameInitParams (const std::string& params) const {
        return (m_InitParams == params);
    }
    void addRef (void) {
        m_RefCount++;
    }
    size_t release (void) {
        return (m_RefCount > 0) ? --m_RefCount : 0;
    }
```

(`#include <string>` у заголовку, якщо його там немає.)

- [ ] **Крок 2: `main-cm-pkcs12.cpp` — прибрати нові глобали**

Видалити `static size_t cm_pkcs12_refcnt = 0;` і `static std::string cm_pkcs12_initparams;` з їхніми
коментарями. Рядок `static CmPkcs12* cm_pkcs12 = nullptr;` — **не чіпати**.

`params_text` — параметр на const (закриває `cpp:S995`) і без C-приведення:

```cpp
static std::string params_text (const CM_UTF8_CHAR* providerParams)
{
    return providerParams ? std::string(reinterpret_cast<const char*>(providerParams)) : std::string();
}
```

`provider_init` — рядки `cm_pkcs12 = new CmPkcs12();`, `parseConfig` і `delete` у гілці помилки
**лишаються як є**; міняються лише гілки:

```cpp
            if (cm_err != RET_OK) {
                delete cm_pkcs12;
                cm_pkcs12 = nullptr;
            }
            else {
                cm_pkcs12->setInitParams(params_text(providerParams));
            }
        }
    }
    else if (cm_pkcs12->isSameInitParams(params_text(providerParams))) {
        //  Idempotent for the SAME configuration: the post-condition already holds.
        cm_pkcs12->addRef();
        cm_err = RET_OK;
    }
    else {
        //  A different configuration is a different request. Reject it loudly and
        //  leave both the instance and the reference count untouched.
        cm_err = RET_CM_ALREADY_INITIALIZED;
    }
    return cm_err;
```

`provider_deinit`:

```cpp
CM_EXPORT CM_ERROR provider_deinit (void)
{
    DEBUG_OUTPUT("provider_deinit()");
    if (!cm_pkcs12) return RET_CM_NOT_INITIALIZED;

    if (cm_pkcs12->release() == 0) {
        delete cm_pkcs12;
        cm_pkcs12 = nullptr;
    }
    return RET_OK;
}
```

- [ ] **Крок 3: `CmCryptoki` і `main-cm-pkcs11.cpp` — дзеркально**

У `cm-cryptoki.h`, у приватній частині класу `CmCryptoki` (поруч з наявними членами-даними) —
ті самі `m_RefCount`/`m_InitParams` з тим самим коментарем; у `public:` — ті самі чотири методи.
У `main-cm-pkcs11.cpp` — видалити `cm_cryptoki_refcnt`/`cm_cryptoki_initparams`, `params_text` на
const, гілки `provider_init`/`provider_deinit` — як у кроці 2, з `cm_cryptoki`. Рядок
`static CmCryptoki* cm_cryptoki = nullptr;` не чіпати. Решту експортів (`provider_open`,
`provider_list_storages` тощо) не чіпати — вони працюють через той самий вказівник.

- [ ] **Крок 4: Перевірка — нічого не змінилось у поведінці**

Повна збірка, потім:
- `provider_contract_selftest_x64.exe` і `_x86.exe` — усі 17 `[PASS]`;
- `cm-pkcs11` — зібрати зі знімка `git -C extern/uapki archive <новий SHA> | tar -x` у scratch (як у
  Task 9-біс, з `library/out/windows-*` і `<build>/out`), прогнати той самий тест із шляхом через
  argv[1] на x64 і x86 — усі `[PASS]`;
- кейси `native_host` 12, 13, 16, 17 — зелені на x64 і x86;
- повний гейт x64 і x86.

Негативної верифікації тут не буде: це рефакторинг без зміни поведінки, і червоне для нього — будь-яке
відхилення контрактного тесту. UB з пункту 2 тестом не ловиться (пам'ять модуля при завершенні
процесу не звільняється) — це виправлення за аналізом, так і написати в описі PR.

- [ ] **Крок 5: Комміт на `main-dev`, cherry-pick у гілку PR #31, пуш (без force)**

```bash
cd extern/uapki
git add library/cm-pkcs12/src/cm-pkcs12.h library/cm-pkcs12/src/main-cm-pkcs12.cpp library/cm-pkcs11/src/cm-cryptoki.h library/cm-pkcs11/src/main-cm-pkcs11.cpp
git commit --only -m "CM: keep provider lifecycle state inside the instance" -- library/cm-pkcs12/src/cm-pkcs12.h library/cm-pkcs12/src/main-cm-pkcs12.cpp library/cm-pkcs11/src/cm-cryptoki.h library/cm-pkcs11/src/main-cm-pkcs11.cpp
git push origin main-dev
cd ../..
```

Гілку PR — у **тимчасовому** `git worktree` (як у Task 10), не перемикаючи спільне дерево:
`cherry-pick <новий SHA>` на `fix/provider-init-refcount`, `git push origin fix/provider-init-refcount`
(fast-forward, **без** `--force`), worktree прибрати. Потім у корені: gitlink + `version.h`, коміт, пуш
(сабмодуль першим — він уже запушений).

Тіло коміту в сабмодулі (англійською, через `-F`): чому стан у екземплярі — обидві причини з шапки
задачі, коротко.

- [ ] **Крок 6: SonarCloud і опис PR #31**

Дочекатися перевірок PR #31 (`gh pr checks 31 -R specinfo-ua/UAPKI`). Quality gate має стати
зеленим. Якщо ні — зупинитися й прислати архітектору перелік знахідок
(`https://sonarcloud.io/api/issues/search?componentKeys=specinfo-ua_UAPKI&pullRequest=31&resolved=false`).

Опис PR #31 — **не переписувати**, лише додати короткий абзац в обидві мовні частини (EN у розділ
«Fix», UA у «Зміна»): третій комміт тримає лічильник і конфігурацію всередині екземпляра провайдера,
щоб `provider_deinit()`, який приходить після `DLL_PROCESS_DETACH` провайдера (можливо разом із #32),
не торкався зруйнованих статиків; заодно прибирає нові мутабельні глобали (SonarCloud). Фразу «Two
commits of one logical change» → «Three commits of one logical change». Тексти абзаців — на рев'ю
архітектору **до** `gh pr edit`.

- [ ] **Крок 7: Якорі документації**

`docs/architecture/uapki.md` §10 посилається на рядки `main-cm-pkcs12.cpp`/`main-cm-pkcs11.cpp` —
перевизначити за змістом; `python scripts/check-doc-anchors.py` — exit 0. Контрольна точка
архітектору: SHA, гейт x64/x86, результат `cm-pkcs11`, стан Sonar.

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
піднімається в Task 1, а в Task 4 (крок 5-біс) — до `> 16` для кейса 16.

**Залежності між задачами:** Task 3 потребує Tasks 1–2 (червоні тести). Task 7 крок 3 замінює
блок, створений Task 6 кроком 3 — виконувати в порядку. Task 10 потребує SHA коммітів із
Tasks 3, 4, 5 і зеленого Task 9. Task 8 крок «`AGENTS.md`» свідомо відкладено до мерджу апстріму.
