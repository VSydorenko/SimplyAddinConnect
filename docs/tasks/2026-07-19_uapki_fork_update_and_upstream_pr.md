# Задача: Оновлення форку UAPKI до актуального upstream + підготовка PR-ів у specinfo-ua

**Дата постановки:** 2026-07-19
**Виконувати в:** окремій сесії (потребує чистого контексту, роботи з git-історією сабмодуля, мережі).
**Репозиторії:** форк `github.com/VSydorenko/UAPKI` (сабмодуль `extern/uapki`), upstream `github.com/specinfo-ua/UAPKI`.
**Передумова:** гілка `static-build` форку (коміти `ff8c679` — `*_STATIC` export-заголовки; `3760fc7` — LoadLibraryW) вже інтегрована й протестована в SimplyAddinConnect. Задача — синхронізувати форк з upstream і віддати наші напрацювання назад у спільноту правильно оформленими PR-ами.

---

## 1. Мета

1. **Оновити форк** `VSydorenko/UAPKI` до актуального стану `specinfo-ua/UAPKI` (на 2026-07-18 форк відставав на ~52 коміти; переперевірити актуальне число).
2. **Перенести наші правки** (`static-build`) на свіжий upstream, переконатися, що збірка й тести SimplyAddinConnect лишаються зеленими.
3. **Підготувати PR-и в upstream** — правильно, за прямими рекомендаціями ядрового розробника Віталія (з чату), максимально атомарними.

## 2. Прямі рекомендації Віталія з чату (джерело: `tmp/ChatExport_2026-07-18/result.json`, "Cryptonite++, UAPKI")

Це настанови від мейнтейнера — дотримуватися буквально при формуванні PR:

- **[msg 3595, 2026-07-16] Про структуру PR (ГОЛОВНЕ):** *"краще робити pr з максимально атомарними комітами і обʼєднувати виключно якщо вони залежать один від одного і можуть виникати конфлікти у разі відхилення якихось. Просто не зручно з великих pr вирізати окремі коміти бо інші чимось не влаштовують"*. → Наші дві зміни НЕЗАЛЕЖНІ → **два окремі PR**, не один.
- **[msg 3597] / [msg 3608, 2026-07-17]:** upstream активно приймає PR-и з форків, зливає зі своїми напрацюваннями й тестує (згадані PR #18, #21, #22 — частину прийнято). Тобто процес реальний і робочий.
- **[msg 3619, 2026-07-17] КРИТИЧНО про статичну збірку:** *"Ми руками збирали ще 5 років тому, викинувши динамічне завантаження. Але оскільки цей варіант не заявлявся на експертизу — в загальний код не вносили. Там є окремі `ifdef emscripten`, але не всюди де потрібно для автоматичної збірки"*. → У upstream Є власний (неопублікований) статичний варіант; наші `*_STATIC` гілки в export-заголовках — це саме та інфраструктура, якої їм бракує "не всюди де потрібно". PR подавати як **build-only інфраструктуру, що НЕ чіпає сертифікований рантайм-код** (це знімає їхнє занепокоєння щодо експертизи).
- **[msg 3304, 2025-07-15] Про пошук провайдерів:** *"В наступній збірці зробимо rpath щоб шукав провайдери та uapkic/f в першу чергу у каталозі, звідки завантажена бібліотека"*. → upstream рухається до "шукати поруч із завантаженою бібліотекою" — наш LoadLibraryW-патч і `cmProviders.dir`-інжекція в цьому ж напрямі. У PR-описі згадати це як узгодженість напрямків (можливо, наш LoadLibraryW стане непотрібним після їхнього rpath — але зараз він виправляє реальну ваду).
- **[msg 3102, 2025-01-03] / [msg 3330, 2025-08-19]:** *"в lib тільки назва бібліотеки без декорацій (префіксів, суфіксів, розширень)"*; *"For cm-providers you can provide full path if needed"*. → підтверджує наш контракт `cmProviders.dir` + `lib`. Наш арх-суфікс (`cm-pkcs12_x64`) — частина базового імені (щоб уникнути колізії x86/x64 у плоскому каталозі), це не суперечить контракту.
- **[msg 1785, 2022-02-17]:** *"під win збирайте краще студією. У налаштуваннях перевірте що немає залежностей від runtime бібліотек"*. → наш `/MT` (статичний CRT) відповідає.
- **[msg 3266] / [msg 3338, 2025-09-29]:** наступні версії — провайдери `cm-pkcs12` + `cm-pkcs11` (апаратні носії); pkcs11 опубліковано ~вересень 2025. → після оновлення форку перевірити, чи з'явився `cm-pkcs11` і чи не змінилась структура `common/loaders`/`cm-api`, якої торкається наш патч.
- **[msg 2223]:** init — дорога операція (самотестування), робити раз; одночасно активний лише один ключ. → контекст, не для PR.

## 3. Наші зміни, що йдуть в upstream (з гілки `static-build`)

| Коміт | Суть | Чіпає рантайм? | PR |
|---|---|---|---|
| `ff8c679` | `#ifdef *_STATIC → порожній *_EXPORT` у `uapkic-export.h`, `uapki-export.h`, `uapkif-export.h`; guard `WIN32_LEAN_AND_MEAN` в `asn_system.h` | **Ні** (лише препроцесор/експорт-макроси) | **PR-A** |
| `3760fc7` | `dl_load_library_utf8` (`MultiByteToWideChar(CP_UTF8)` + `LoadLibraryW`) у `common/loaders/dl-macros.h`; виклик у `cm-loader.cpp` та `uapki-loader.cpp` | Так (шлях завантаження DLL на Windows), але це bugfix | **PR-B** |

## 4. Кроки реалізації

### Крок A. Оновлення форку до upstream

1. У сабмодулі `extern/uapki`: `git fetch upstream --tags`; оцінити відставання: `git rev-list --count static-build..upstream/main`, `git log --oneline static-build..upstream/main`.
2. Оновити гілку `main` форку до `upstream/main` (fast-forward): `git checkout main && git merge --ff-only upstream/main` (або скинути, якщо форк-main не має власних потрібних комітів — перевірити `git log upstream/main..main`; за фактами дослідження форк-коміти = лише доки + `build_uapki.ps1` + бампи cmake, C/C++ код не чіпали — вирішити, чи зберігати їх).
3. **ЗАПушити** оновлений `main` форку (з підтвердженням користувача).

### Крок B. Перенесення наших правок на свіжий upstream

1. Створити `static-build-v2` від свіжого `upstream/main`.
2. Перенести `ff8c679` і `3760fc7` (`git cherry-pick`). **Очікувані конфлікти**: export-заголовки могли змінитися upstream (див. [msg 3619] — вони самі додавали `ifdef emscripten`); `common/loaders/*` міг змінитися під нову архітектуру провайдерів/pkcs11 ([msg 3304] rpath, [msg 3338] pkcs11). Вирішити конфлікти, зберігши суть наших змін.
3. Якщо upstream уже додав частину нашого (напр. `*_STATIC` гілки або rpath, що робить LoadLibraryW зайвим) — зафіксувати це у звіті; можливо, відповідний PR стане непотрібним.

### Крок C. Перевірка сумісності SimplyAddinConnect

1. Перемкнути сабмодуль на `static-build-v2`, оновити указник у основному репо (гілка окрема, напр. `uapki-upstream-sync`, НЕ прямо в `add_UAPKI`).
2. Повна збірка `build_project.ps1 -WithUAPKI -WithTests` + `run_tests.ps1` (обидві архітектури) — усе має лишитися зеленим (PASS=17). Особлива увага: чи не змінився зовнішній JSON-контракт (`process`/`INIT`/`cmProviders`), від якого залежить `UAPKIConnectHelper`; чи не перейменували `cm-pkcs12` символи/структуру `cm-api`.
3. За потреби — адаптувати `CMake/uapki_full_static.cmake` (нові/перейменовані сирці) і хелпер (якщо змінився контракт).
4. Ручний тест у 1С (з користувачем) — `INIT` → `countCmProviders:1` на оновленому ядрі.

### Крок D. Підготовка PR-ів (НЕ надсилати без підтвердження користувача)

**PR-A — `*_STATIC` export headers (build infrastructure):**
- Гілка від свіжого `upstream/main`, лише коміт export-заголовків (виокремити з `ff8c679`: `asn_system.h` guard можна лишити в цьому ж PR або окремо — на розсуд, guard теж build-only).
- Опис (EN + UA): проблема (при статичному вбудовуванні uapkic/uapkif/uapki у стороннє DLL макроси `*_LIBRARY` форсують `dllexport`, забруднюючи таблицю експорту споживача; дефайни `*_STATIC` існують, але в заголовках не оброблені); рішення (гілка `#ifdef *_STATIC → порожній EXPORT`); сумісність (нічого не змінює для існуючих shared-збірок — гілка активна лише при явному `*_STATIC`); згадати [msg 3619] — узгодження з їхнім власним статичним варіантом.
- Наголосити: **не чіпає рантайм/сертифікований код** — лише препроцесор.

**PR-B — LoadLibraryW для завантаження провайдерів/лоадерів:**
- Гілка від свіжого `upstream/main`, коміт LoadLibraryW.
- Опис (EN + UA): проблема (`LoadLibraryA` не завантажує DLL за шляхом з не-ASCII символами — напр. кирилиця в імені користувача `%LOCALAPPDATA%\Users\Іван\...`; масова проблема для укр. користувачів); рішення (UTF-8→UTF-16 + `LoadLibraryW`); scope (тільки Windows-гілка `dl-macros.h`, non-Windows без змін); згадати узгодженість з їхнім планом rpath [msg 3304].
- Це **найсильніший самостійний внесок** — чистий bugfix, корисний усім, незалежний від статичної збірки.

**Загальне для обох PR** (за [msg 3595]): атомарні коміти, зрозумілі повідомлення EN/UA, кожен PR самодостатній і може бути прийнятий/відхилений незалежно. У тілі PR — посилання на цю мотивацію та тестування (наш харнес як доказ, що зміни не ламають SIGN/VERIFY).

### Крок E. Звіт

Розділ 9 у цьому файлі: фактичне відставання форку; конфлікти при перенесенні й як вирішені; чи щось із наших змін уже є в upstream; результати `run_tests`; посилання на створені (локальні) PR-гілки; що надіслано / очікує підтвердження.

## 5. Обмеження

- **Жодних push/PR без явного підтвердження користувача** (і форк, і upstream).
- `add_UAPKI` НЕ ламати — синхронізацію робити в окремій гілці основного репо; злиття — рішення користувача після зеленого `run_tests` + тесту в 1С.
- Мова PR-описів: EN + UA (upstream двомовний); коміти — за стилем гілки.
- НЕ подавати в один PR обидві зміни (пряма настанова [msg 3595]).
- Зберегти локальні гілки `static-build` (базову) до підтвердження, що v2 працює.

## 6. Ризики

- Оновлення на ~52+ комітів може містити зміни JSON-контракту або структури провайдерів (pkcs11) — тоді доведеться адаптувати `UAPKIConnectHelper`/CMake (окремий підпункт роботи, не лише механічний cherry-pick).
- upstream міг уже вирішити нашу проблему інакше (їхній http-helper, rpath) — тоді наш PR або непотрібен, або потребує узгодження; перевірити ПЕРЕД відкриттям PR.
- `build_uapki.ps1` та доки у форку (наші минулі коміти) можуть конфліктувати при ff-оновленні main — вирішити, чи вони ще потрібні (їхня цінність під питанням, оскільки основну збірку робить `CMake/uapki_full_static.cmake` основного репо).

## 7. Поза межами

- Реалізація нового функціоналу UAPKI.
- Зміна архітектури інтеграції SimplyAddinConnect (гібрид лишається).
- Апаратні провайдери (`cm-pkcs11`) — окремо, якщо/коли знадобляться.

## 9. Звіт виконання

**Дата виконання:** 2026-07-19. **Гілки:** сабмодуль `extern/uapki` — `static-build-v2`,
`static-export-headers` (PR-A), `loadlibraryw-utf8` (PR-B); основне репо — `uapki-upstream-sync`.

### 9.1. Фактичне відставання форку
- `static-build..upstream/main` = **52 коміти** (підтверджено; upstream HEAD `69053dc` — Android JNI + HttpHelper).
- merge-base(`static-build`,`upstream/main`) = `9fbc408` ("update cm-pkcs12 version to 1.0.13").
- `local main` форку (`9fbc408`) — чистий предок upstream (0 власних комітів); але `origin/main`
  (remote форку) = 13 комітів ПОПЕРЕДУ upstream (build_uapki.ps1, memleak fix, merges зі specinfo-ua)
  і 51 позаду → **чистий `--ff-only` для `origin/main` неможливий** (див. 9.6 — рішення за користувачем).

### 9.2. Перенесення наших правок (Крок B)
- Створено `static-build-v2` від `upstream/main`; cherry-pick `ff8c679` (STATIC export headers) і
  `3760fc7` (LoadLibraryW UTF-8).
- **Конфліктів — 0** (точно як прогнозував аналіз). `3760fc7` авто-змержився 3-way з upstream-змінами
  в loaders: збережено і наш `dl_load_library_utf8` у `CmLoader::load`/`UapkiLoader::load`, і upstream-код
  (`getDlError()` в uapki-loader, перейменування параметрів `providerInfo/storageInfo→outInfo` у cm-loader).
- Підсумок `static-build-v2`: рівно 2 наші коміти, рівно 7 файлів vs `upstream/main`.

### 9.3. Чи щось із наших змін уже є в upstream
- **Ні.** upstream/main не містить ЖОДНОЇ: ні `*_STATIC`-гілок у export-заголовках (усі 4 файли
  байт-ідентичні merge-base↔upstream; «неопублікований статичний варіант» Віталія в цю гілку не потрапив),
  ні UTF-8/`LoadLibraryW` у loaders, ні rpath-пошуку провайдера поруч із бібліотекою. **Обидва PR-и лишаються потрібними.**

### 9.4. Сумісність SimplyAddinConnect (Крок C) — одна детермінована адаптація
- Ядро `uapki/uapkic/uapkif` збирається через `file(GLOB)` → усі нові upstream-файли (5 методів 2.0.16:
  `BUILD_CMS_2PASS`, `BUILD_CSR_2PASS`, `GENERATE_CERTBUNDLE`, `MODIFY_CMS`, `VERIFY_CSR`;
  `extnreq-helper.cpp`, `ecdsa-params.c`, `DSTU7624Parameters.c`) підхоплюються автоматично — unresolved у ядрі немає.
- **Єдина поломка:** ціль `cm-pkcs12-provider` перелічує `common/pkix/*.c` **явним списком**, а upstream
  оновив `private-key.c` (він у цьому списку) → тепер `#include "ecdsa-params.h"` + виклик
  `ecdsa_ecparams_get_ecid()` (`private-key.c:37,322`). Без `ecdsa-params.c` провайдер падав би з
  unresolved external. **Виправлення — один рядок:** додано
  `library/common/pkix/ecdsa-params.c` до джерел `cm-pkcs12-provider` у `CMake/uapki_full_static.cmake`.
- Публічні контракти **стабільні**: 7 cm-api символів провайдера незмінні; структура `cm-api`/`cm-loader`
  без змін; `process()`/`json_free()` без змін; JSON-контракт `cmProviders`/`allowedProviders`/`setup_cm_providers`
  зворотно сумісний (лише additive-параметр INIT `skipSelfTest` + внутрішній `uapkic_init`). **`UAPKIConnectHelper.cpp` правити не треба.**
- `http-helper.cpp` (Android-правки upstream) — увесь Android-код під `#if !defined(ANDROID) && !defined(__ANDROID__)`;
  на Windows тягнеться `curl`, не `jni.h`; `set_jni` — в `#else`-гілці Android, на Windows не компілюється. Безпечно.
- Новий провайдер `cm-pkcs11` + каталог `common/cryptoki` — нас не зачіпають (не збираємо; ядро cryptoki не інклудить).

### 9.5. Результати збірки й тестів
- **Збірка `build_project.ps1 -WithUAPKI -WithTests` — ✅ ЗЕЛЕНА** для обох архітектур (x86+x64, Release),
  0 помилок/unresolved. Провайдери `cm-pkcs12_x64.dll`/`_x86.dll` зібрані чисто (фікс `ecdsa-params.c` спрацював),
  ZIP `bin/Release/SimplyAddinConnectWin.zip` створено (5 файлів).
- **`run_tests.ps1` (x64/x86):** L0 (крім x86 `py-provider_info`), **усі 7 L1-сценаріїв**, native_host
  **case 3/4/5** — PASS. **case 1/2 — FAIL, з ЄДИНОЇ середовищної причини, не регресії:**
  жива сесія 1С (`1cv8.exe` PID 16784, запущена 0:51) тримає завантаженою СТАРУ версію
  `%LOCALAPPDATA%\SimplyAddinConnect\providers\3.0.2.104\cm-pkcs12_x64.dll` → `rmrf(appDir)`
  харнеса не може прибрати каталог (Access denied) → падає precondition/leftover-assertion кейсів 1-2.
  У самих кейсах INIT успішний (`countCmProviders==1`) — розгортання й завантаження працюють.
  x86 `py-provider_info` FAIL — 64-бітний системний Python не може `ctypes.CDLL` 32-бітну DLL (WinError 193),
  теж середовищний артефакт харнеса, не код. **Для чистого PASS=17 треба закрити ту сесію 1С і перезапустити тести.**

### 9.6. Що зроблено / очікує підтвердження
**Рішення користувача (2026-07-19, друга ітерація) і виконані дії:**
- **Форк `main` — варіант (b):** `origin/main` **зресетовано** до `upstream/main` (`d58243a`→`69053dc`,
  force). Форк-тулінг (build_uapki.ps1, merged-DLL опція) відкинуто; форк — лише транслятор PR в upstream.
  UAPKI локально окремо не збираємо (гібрид у головному репо через `uapki_full_static.cmake` лишається).
- **Push у форк виконано:** `static-build-v2` (`cb39ea9`, ціль сабмодуля), `static-export-headers`
  (`a3e7a70`, PR-A), `loadlibraryw-utf8` (`010a5fc`, PR-B). Стара `static-build` (`3760fc7`) лишена в форку.
- **PR у upstream — драфти готові, гілки запушені; надсилання за користувачем.** URL порівняння:
  - PR-A: `https://github.com/specinfo-ua/UAPKI/compare/main...VSydorenko:UAPKI:static-export-headers?expand=1`
  - PR-B: `https://github.com/specinfo-ua/UAPKI/compare/main...VSydorenko:UAPKI:loadlibraryw-utf8?expand=1`
  - Тексти — `docs/tasks/2026-07-19_uapki_pr_drafts.md`.
- **Серти — прийнято канонічний неймінг upstream v2.0.16.** `tests/data/certs` перезбережено в канонічну
  форму `<subjKeyId8>-<issuer8>-<sha1>.cer` (ті самі 6 сертів); повторне сканування CerStore ідемпотентне
  (перевірено). Тимчасовий temp-copy-workaround у `run_tests.ps1` прибрано — обробка не потрібна.
- **Інтеграція в `add_UAPKI` виконана:** `add_UAPKI` fast-forward'нуто до синхронізації (указник сабмодуля
  → `cb39ea9`, включно з супутніми комітами й раніше незакоміченими WIP-файлами). `run_tests x64` на
  `add_UAPKI` = **PASS=17**, дерево чисте. `add_UAPKI` **не запушено** — PR у `main` робить користувач.
- **Лишилось за користувачем:** надіслати 2 PR у specinfo-ua; push `add_UAPKI` + PR у `main`; ручний тест
  у реальній 1С на оновленому ядрі (INIT → `countCmProviders:1`).

---

## 10. Ітерація 2026-07-22 — доопрацювання PR #26 за рев'ю (виконано)

Обидва PR подано в specinfo-ua (2026-07-20). **PR #25** (`static-export-headers`) — **MERGED**
(upstream `c64181c`) без зауважень. **PR #26** (`loadlibraryw-utf8`) отримав FAIL SonarCloud +
коментарі; цикл виправлень нижче. Робота велась у гілці `docs-uapki`; правки коду — у сабмодулі
на гілці `loadlibraryw-utf8` (форк).

### 10.1. Фідбек по PR #26
- **SonarCloud Quality Gate — FAIL:** «B Maintainability Rating on New Code» (треба ≥ A) — на нових
  рядках хелпера (ручні `malloc`/`free` + C-касти).
- **Мейнтейнер `specinfo-ua` (OWNER):** дав готовий RAII-варіант через `std::wstring`+`static_cast`;
  попросив «залишити один dl-macros.h (`common/cryptoki/dl-macros.h` та `common/loaders/dl-macros.h`)».
- **Контриб'ютор `DJm00n`:** додати прапорець `MB_ERR_INVALID_CHAR`.

### 10.2. Аналіз (звірено з форком)
- Причина SonarCloud — ручне керування пам'яттю + C-касти; RAII-варіант мейнтейнера їх прибирає.
- **Виявлено дубль:** у v2.0.16 з'явився ДРУГИЙ `library/common/cryptoki/dl-macros.h` (для cm-pkcs11),
  майже байт-ідентичний, **той самий include-guard `DL_MACROS_H`**, досі з `LoadLibraryA` — той самий
  баг кириличних шляхів; `cryptoki-loader.cpp:80` вантажив PKCS#11-драйвер через `DL_LOAD_LIBRARY`.
  Звідси прохання мейнтейнера про єдиний файл.
- **Дефекти сніпета мейнтейнера, виправлені при адаптації:** (1) проєкт на **C++11**
  (`CMAKE_CXX_STANDARD 11` всюди), де `std::wstring::data()` повертає `const wchar_t*` → не годиться
  як OUT-буфер `MultiByteToWideChar` → використано `&wbuf[0]` (неконстантний з C++11); (2) `<string>`
  не можна включати всередині `extern "C"` → винесено під `#ifdef __cplusplus` перед блоком.

### 10.3. Рішення (за вибором користувача) і зміни
Обрано: **звести дубль в один фізичний файл** + додати `MB_ERR_INVALID_CHARS`.
- `loaders/dl-macros.h` — канонічний: RAII `std::wstring`, `static_cast`, `&wbuf[0]`,
  `MB_ERR_INVALID_CHARS` в обох викликах, самодостатній guarded `<string>`.
- Видалено `cryptoki/dl-macros.h`; `cryptoki-loader.h` → `#include "../loaders/dl-macros.h"`;
  `cryptoki-loader.cpp` → `dl_load_library_utf8` (фікс поширено й на cm-pkcs11).
- Архітектурний нюанс: додається залежність `common/cryptoki → common/loaders` (CMake cm-pkcs11
  не референсив `loaders/`, але quote-include резолвиться без правок CMake). У відповіді PR
  мейнтейнеру запропоновано перенести спільний файл у нейтральний `common/`, якщо він хоче
  зберегти ізоляцію cm-pkcs11.

### 10.4. Баг, спійманий локальним білдом (КРИТИЧНИЙ УРОК)
Перша збірка ВПАЛА: `MB_ERR_INVALID_CHAR: необъявленный идентификатор`. Правильний Win32-макрос —
**`MB_ERR_INVALID_CHARS`** (з 'S'); DJm00n написав розмовно в однині, я скопіював без 'S'. Виправлено.
**Урок:** upstream CI (`.github/workflows/native-test.yml`) = **тільки Linux** (`ubuntu-latest`,
`cmake -S library`) — Windows-гілку `dl-macros.h` (`#if defined(_WIN32)`) НЕ компілює взагалі
(активний `dlopen`-аліас). Windows-помилки ловить лише **локальний Windows-білд**. SonarCloud
аналізує нові рядки diff незалежно від платформи. SimplyAddinConnect не будує cm-pkcs11 → cryptoki-
сторону консолідації валідує лише Linux-CI. **Завжди білдити локально Windows перед пушем.**

### 10.5. Валідація
- Збірка `-WithUAPKI -WithTests` x86+x64 — ✅ зелена (5 файлів + ZIP).
- `run_tests` x64 = **PASS=21**/FAIL=2; x86 = **PASS=20**/FAIL=2/SKIP=1. Усі крипто-рівні (L0 експорти
  3/7 без витоку, L1×7 SIGN/VERIFY/encrypt, native_host case 3/4/5) — PASS. FAIL = `native_host`
  case 1/2: precondition-очищення `%LOCALAPPDATA%\SimplyAddinConnect` заблоковане живими сесіями 1С
  (`Case 1: FAIL — прибрано`) — середовищне, до крипто-логіки, не код. SKIP x86 `py-provider_info` —
  64-біт Python не вантажить 32-біт DLL. Ідентично базлайну §9.5.

### 10.6. Результат
- PR-гілку `loadlibraryw-utf8` перероблено в один атомарний коміт **`ac41747`**, force-запушено
  (`--force-with-lease`) у форк `origin/loadlibraryw-utf8` — PR #26 оновлено.
- Технічну відповідь опубліковано в тред PR #26 (issuecomment-5044045064): що прийнято, що і чому
  підправлено (C++11, макрос), питання по консолідації мейнтейнеру.
- **CI на оновленому PR #26 — усе зелене:** `SonarCloud Code Analysis` **pass** (гейт A — B-зауваження
  знято), `test` (Linux native build) **pass** (консолідація компілюється, включно з `cryptoki-loader.cpp`).
- Сабмодуль повернуто на `main-dev` (`cb39ea9`) — git основного репо чистий; PR-покращення живуть на
  `loadlibraryw-utf8` у форку.

### 10.7. Лишилось (за іншими) — **ЗАКРИТО 2026-08-28, див. §11**
- ~~Рішення мейнтейнера по консолідації~~ → прийнято як є: `cryptoki-loader` тепер інклудить
  `common/loaders/dl-macros.h`, окремого файлу в `common/cryptoki/` більше немає.
- ~~Прийняття PR #26~~ → **MERGED 2026-07-26** (`e9bb7fa`).
- ~~Інтеграція покращень PR #26 назад у `main-dev`~~ → виконано разом із синхронізацією §11.

---

## 11. Ітерація 2026-08-28 — синхронізація з релізом upstream v2.0.16 (виконано)

**Гілка головного репо:** `uapki-sync-v2.0.16` (від `main`). **Сабмодуль:** `main-dev` → `325be0d`.

### 11.1. Обидва PR прийняті в upstream — форк більше не тримає власних правок
| PR | Гілка форку | Merged | Коміт upstream |
|---|---|---|---|
| [#25](https://github.com/specinfo-ua/UAPKI/pull/25) `*_STATIC` export headers | `static-export-headers` | 2026-07-20 | `c64181c` |
| [#26](https://github.com/specinfo-ua/UAPKI/pull/26) `LoadLibraryW` UTF-8 + консолідація `dl-macros.h` | `loadlibraryw-utf8` | 2026-07-26 | `e9bb7fa` |

Звірено пофайлово: `*_STATIC`-гілки є в усіх трьох export-заголовках, `WIN32_LEAN_AND_MEAN`-guard —
в `asn_system.h` (upstream переніс його в `library/uapkif/include/`), `dl_load_library_utf8` —
у `common/loaders/dl-macros.h`, дубля `common/cryptoki/dl-macros.h` немає. Тобто **наші два локальні
коміти (`f5ca33a`, `cb39ea9`) стали зайвими** — `main` і `main-dev` форку виставлено рівно на
`upstream/main`; локальної дельти немає (`git rev-list --count upstream/main..main-dev` = 0).

Upstream уже переписав нашу правку далі («Some rewrites on loaders macro»): виклик іде через
макрос `DL_LOAD_LIBRARY`, який на Windows розкривається в `dl_load_library_utf8`, на Linux/macOS —
в `dlopen`, для Emscripten — інертна заглушка. Поведінка на Windows та сама.

### 11.2. Що приїхало з upstream (28 комітів, `69053dc` → `325be0d`)
Реліз **v2.0.16** (тег = `12d8ff2`) + 1 коміт по їхніх build-скриптах. Версії: ядро `uapki` 2.0.16,
провайдер `cm-pkcs12` 1.0.24, `cm-pkcs11` 1.0.12.

**Функціонального впливу на нас — нуль.** 39 методів JSON-API незмінні (нових `api/*.cpp` не
додано — лише правки в `verify-csr.cpp`, `cm-storage-proxy.cpp`, `store-json.cpp`, здебільшого
косметика const-кастів); `process()`/`json_free()`, 7 обов'язкових cm-api символів провайдера,
поля `cmProviders`/`countCmProviders` — без змін, `UAPKIConnectHelper` правити не довелось.
Решта — платформи, яких ми не збираємо: Android/JNI, iOS, Emscripten/WASM, Windows ARM64,
PKCS#11-інтерфейс (узгоджений з «Автор»), SonarCloud-фікси.

### 11.3. Дві поломки збірки й фікси (`CMake/uapki_full_static.cmake`)
Обидві — від того, що upstream підмінив прибудований `common/curl/builds/windows_*/libcurl.lib`
на **libcurl 8.21.0**:

1. **`unresolved __imp_if_nametoindex`** → додано `iphlpapi` (і `normaliz` — для паритету з
   переліком у власному `library/uapki/CMakeLists.txt` upstream) у WIN32-гілки лінковки
   `uapki` / `uapki_full_static` / `uapki_bundle`.
2. **`LINK : fatal error LNK1104: не удается открыть файл "volatileaccessu.lib"`** на фінальному
   лінку головної DLL. Причина: **кожен** об'єктний файл нового libcurl несе
   `/DEFAULTLIB:volatileaccessu.lib` і посилається на `RtlSetVolatileMemory` — у Windows SDK
   **10.0.26100+** саме так реалізовано `SecureZeroMemory`. Збірка тут іде проти SDK 10.0.20348
   (дефолт на Windows Server 2022), де такого файлу немає. Фікс: CMake шукає `volatileaccessu.lib`
   потрібної арх у **всіх** встановлених Windows Kits, бере найновіший і лінкує повним шляхом;
   якщо не знайдено — зрозумілий `FATAL_ERROR` замість `LNK1104`. Вимогу «Windows SDK 10.0.26100+
   для `-WithUAPKI`» додано в `AGENTS.md`.

Діагностика, яка це виявила (варто повторювати при кожній підміні libcurl):
`dumpbin /DIRECTIVES libcurl.lib` (які `/DEFAULTLIB`) + `dumpbin /SYMBOLS libcurl.lib | findstr UNDEF`.

### 11.4. Результати
- **Версію піднято `3.0.2` → `3.0.3`** (`VERSION.txt`). Причина не косметична: змінилося
  крипто-ядро, а версіоновані імена DLL усередині ZIP (`..._3_0_3_122_x64.dll`) — єдиний
  надійний спосіб не дати 1С підхопити стару компоненту з кешу `ExtCompT` при ручному тесті.
- **Збірка `build_project.ps1 -WithUAPKI -WithTests`** — ✅ зелена, x86+x64, 0 помилок; ZIP із
  5 файлів + тестова обробка `.epf` (версія 3.0.3.122).
- **`run_tests.ps1 x64` = PASS=23, FAIL=0, SKIP=0** — усе, включно з `native_host` **case 5**
  (L3-крос-валідація на еталонах ПРРО) і всіма 7 сценаріями L1.
- **`run_tests.ps1 x86` = PASS=22, FAIL=0, SKIP=1** — єдиний SKIP задокументований
  (`py-provider_info`: 64-бітний системний python не вантажить 32-бітну DLL).
- Дерево чисте: `tests/data/certs` цього разу **не** зачепило (канонічний неймінг з §9.6 тримається).

### 11.5. Оновлена документація
- `docs/architecture/uapki.md`: §2.1 — нові системні залежності libcurl + механізм пошуку
  `volatileaccessu.lib`; §4 — виклик через `DL_LOAD_LIBRARY`, актуальні номери рядків, помітка що це
  вже upstream-код (PR #26), а не наш патч; §7 — поточний указник (v2.0.16), таблиця прийнятих PR,
  `main-dev` без власних правок, у чек-лист синхронізації додано пункт про системні залежності
  прибудованого libcurl.
- `AGENTS.md` — вимога Windows SDK 10.0.26100+ для `-WithUAPKI`.
- `.claude/skills/ecp-testing-without-1c` — прибрано застаріле «зелений стан = PASS=17»
  (число росте з новими драйверами; гейт — exit-код).

### 11.6. Стан репозиторіїв — закрито
- **Форк `VSydorenko/UAPKI`:** `main` оновлено (`69053dc`→`325be0d`), `main-dev` force-оновлено
  (`cb39ea9`→`325be0d`). Злиті topic-гілки `static-export-headers` / `loadlibraryw-utf8` GitHub
  видалив при merge; локальні копії теж прибрано. У форку лишились рівно `main` і `main-dev`.
- **Головне репо:** гілка `uapki-sync-v2.0.16` запушена, PR у `main` відкрито.

**Лишилось за користувачем:** ручний тест у реальній 1С на оновленому ядрі
(`INIT` → `countCmProviders:1`) — версія 3.0.3.122 гарантовано не візьметься з кешу.

**Окремою задачею (обговорюється):** другий провайдер `cm-pkcs11` (v1.0.12) — підтримка
апаратних носіїв КЕП і HSM.
