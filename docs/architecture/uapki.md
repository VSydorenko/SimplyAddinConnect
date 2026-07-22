# Архітектура стеку UAPKI (ЕЦП/крипто)

Документ описує підсистему криптографії та ЕЦП зовнішньої компоненти 1С
**SimplyAddinConnect** — від виклику з 1С до завантаження провайдера НКІ у рантаймі.
Матеріал звірено з кодом.

Повний JSON-протокол UAPKI (методи, параметри, коди помилок) — авторитетна настанова бібліотеки
[`extern/uapki/doc/UAPKI-PM-2.0.16.md`](../../extern/uapki/doc/UAPKI-PM-2.0.16.md) (є й англійська
версія `UAPKI-PM-2.0.16.en.md`). Прикладна інтеграція з 1С —
[docs/integration-1c/uapki.md](../integration-1c/uapki.md). Переносне ноу-хау з інтеграції —
скіл [`.claude/skills/uapki-integration`](../../.claude/skills/uapki-integration/SKILL.md).

---

## 1. Ланцюг виконання: компонента → хелпер → `process()` / `json_free()`

Стек будується на трьох ланках. Компонента `AddinUAPKIConnect` — тонкий фасад для 1С;
уся логіка зосереджена в статичному хелпері, який спілкується з ядром UAPKI через
C-API `process()` / `json_free()`.

```
AddinUAPKIConnect  (компонента: методи CallUapki / ВызватьUAPKI, EnableLogging)
        │
        ▼
UAPKIConnectHelper::ExecuteUapkiCommand  (static)
        │  формує JSON-запит {method, parameters}
        ▼
process(request) / json_free(result)     (C-API UAPKI, статичний лінк, WITH_UAPKI)
        │
        ▼ (у method == INIT, рантайм)
cm-pkcs12_x86.dll / cm-pkcs12_x64.dll     (окрема самодостатня DLL, LoadLibraryW)
```

**Точка входу.** Усі методи UAPKI надходять з 1С через єдину точку — `CallUapki` /
`ВызватьUAPKI` компоненти `AddinUAPKIConnect`
(`src/components/AddinUAPKIConnect.cpp`). Компонента передає назву методу та параметри
у статичний метод `UAPKIConnectHelper::ExecuteUapkiCommand`.

**Формування запиту.** Хелпер збирає JSON-запит формату `{method, parameters}` через
`nlohmann::json`: `requestJson["method"] = method`, `requestJson["parameters"] = paramsJson`
(`UAPKIConnectHelper.cpp:617-618, 662`). Параметри можуть надходити або як JSON, або
у плоскому форматі `ключ=значение,...` (розбирається через `ParseParamsString`).

**Виклик C-API.** Оголошення функцій ядра — під `WITH_UAPKI`
(`UAPKIConnectHelper.cpp:20-28`):

```cpp
extern "C" {
    char* process(const char* request);
    void  json_free(char* json);
}
```

Далі хелпер викликає `char* response = ::process(requestStr.c_str())`
(`UAPKIConnectHelper.cpp:676`). Згідно з протоколом (Таблиця 3 настанови
[`UAPKI-PM-2.0.16.md`](../../extern/uapki/doc/UAPKI-PM-2.0.16.md)), `process()` повертає
нуль-термінований JSON у UTF-8, пам'ять якого **має завжди звільнятися** функцією `json_free()`.

**Звільнення пам'яті — до аналізу.** Хелпер копіює відповідь у `std::string responseJson`
(`UAPKIConnectHelper.cpp:681`) і **одразу** звільняє буфер: `::json_free(response)`
(`UAPKIConnectHelper.cpp:686`). Порядок навмисний — коментар у коді пояснює: звільнення
до будь-якого аналізу копії гарантує відсутність витоків на всіх гілках нижче
(`UAPKIConnectHelper.cpp:683-684`).

**Детекція успіху.** Успішність визначається за полем `errorCode` відповіді
(`IsOperationSuccess`, `UAPKIConnectHelper.cpp:545-607`): поле обов'язкове й має бути
цілим, успіх — коли `errorCode == 0`, інакше формується діагностика з `error`/`method`.
Це узгоджено з форматом відповіді протоколу (обов'язкове ціле `errorCode`; `method`/`result`/`error`).

**Спеціальна обробка `INIT`.** Метод `INIT` (регістронезалежно) — єдиний, що має
спеціальну обробку: перед відправкою хелпер автоматично інжектить конфігурацію
провайдерів (`InjectProviderConfig`, `UAPKIConnectHelper.cpp:650-659`), а після виклику
звіряє фактичну кількість завантажених провайдерів (`WarnIfProvidersNotLoaded`,
`UAPKIConnectHelper.cpp:707-709`). Решта методів (OPEN, SELECT_KEY, SIGN, VERIFY, CLOSE,
DEINIT та інші) проходять тим самим універсальним шляхом без спецобробки.

---

## 2. Гібридна збірка: статичне ядро + самодостатній провайдер окремою DLL

Ключове збіркове рішення — **гібридна схема**: ядро UAPKI лінкується статично в головну
DLL, а провайдер `cm-pkcs12` збирається як окрема самодостатня SHARED-DLL. Це визначено
в `CMake/uapki_full_static.cmake`.

### 2.1. Статичне ядро

Ядро (`uapki` + `uapkic` + `uapkif` + допоміжні `asn1` / `ba-utils` / `byte-array` /
`stacktrace` / `dirent-internal` / `parson`) збирається в об'єднану ціль
`uapki_full_static`, обгорнуту в інтерфейсну ціль `uapki_bundle`, яка підключається до
головної DLL:

- `add_library(uapki STATIC ...)` лінкує `uapkic stacktrace uapkif asn1 ba-utils parson
  dirent-internal` (`uapki_full_static.cmake:204-226`);
- `add_library(uapki_full_static STATIC ...)` — WIN32-гілка додає системні `wldap32
  crypt32 ws2_32 winmm bcrypt` (`uapki_full_static.cmake:249-268`);
- `add_library(uapki_bundle INTERFACE)` → `target_link_libraries(uapki_bundle INTERFACE
  uapki_full_static)` (`uapki_full_static.cmake:289-294`);
- підключення до головної цілі: `include(CMake/uapki_full_static.cmake)` +
  `target_link_libraries(${TARGET} PRIVATE uapki_bundle)` під `BUILD_WITH_UAPKI`
  (`CMake/components.cmake:357-359`).

Статична збірка вмикається `*_STATIC`-дефайнами (`BA_STATIC`, `ASN1_STATIC`,
`DRBG_STATIC`, `UAPKIC_STATIC`, `UAPKIF_STATIC`, `UAPKI_STATIC`, `CURL_STATICLIB`), які
вимикають `dllimport`; `BUILD_SHARED_LIBS` — `OFF` (`CMake/options.cmake:6-15`).

### 2.2. Провайдер `cm-pkcs12` — окрема SHARED-DLL

Провайдер **не лінкується статично в головну DLL** — це окрема самодостатня SHARED-DLL,
яка вантажиться ядром у рантаймі через `LoadLibrary` і не має символьних залежностей від
головної DLL (`uapki_full_static.cmake:130-134`):

- `add_library(cm-pkcs12-provider SHARED ...)` (`uapki_full_static.cmake:143`);
- усередину провайдера лінкуються **статичні** `uapkic uapkif asn1 parson ba-utils
  byte-array stacktrace dirent-internal` плюс системні `bcrypt crypt32 ws2_32`
  (`uapki_full_static.cmake:167-170`); `curl` навмисно не лінкується — він потрібен лише
  ядру `uapki`;
- ім'я виходу несе арх-суфікс: `OUTPUT_NAME "cm-pkcs12_x64"` (64-біт) /
  `"cm-pkcs12_x86"` (32-біт), вихід у `bin/Release` (`uapki_full_static.cmake:178-183`).

**Чисті експорти.** Провайдер експортує рівно 7 обов'язкових CM-API символів (позначених
`CM_EXPORT`), для чого зібраний з `CM_LIBRARY`, **без** `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS`
і без `.def` — інакше експортувалися б тисячі символів (`uapki_full_static.cmake:171-174`).
Мінімальний обов'язковий набір перевіряється при завантаженні в `CmLoader::load`:
`provider_info`, `provider_init`, `provider_deinit`, `provider_open`, `provider_close`,
`block_free`, `bytearray_free` (`extern/uapki/library/common/loaders/cm-loader.cpp:77-86`;
перевірка наявності — `cm-loader.cpp:89-90`). Символи `list_storages` / `storage_info` /
`format` — опціональні.

Таким чином `bcrypt` / `crypt32` / `ws2_32` — єдині системні залежності провайдера, а сам
він не залежить від головної DLL.

---

## 3. Вбудований RCDATA-ресурс і потрійний пошук каталогу провайдера

Щоб для 1С компонента лишалася **одним файлом**, провайдер вбудовується в головну DLL як
ресурс і за потреби розгортається на диск у рантаймі.

### 3.1. Вбудований ресурс

CMake генерує per-`$<CONFIG>` `.rc`-файл через `file(GENERATE)` (genex `$<TARGET_FILE:...>`
не працює в `configure_file`) з вмістом `CM_PKCS12_PROVIDER RCDATA
"$<TARGET_FILE:cm-pkcs12-provider>"`, додає його через `target_sources(${TARGET} ...)` та
встановлює `add_dependencies(${TARGET} cm-pkcs12-provider)`
(`uapki_full_static.cmake:332-344`). У результаті кожна головна DLL несе провайдер **своєї**
архітектури (x86-DLL — `cm-pkcs12_x86.dll`, x64-DLL — `cm-pkcs12_x64.dll`).

Ім'я ресурсу зафіксовано в коді: `#define UAPKI_PROVIDER_RESOURCE_NAME L"CM_PKCS12_PROVIDER"`
(`src/helpers/UAPKIConnect/UAPKIProviderResource.h:6`) — той самий літерал, що у
згенерованому `.rc` (тримається синхронно).

### 3.2. Потрійний пошук (`ResolveProviderDir`)

При `method == INIT` хелпер визначає каталог провайдера трьома кроками в порядку
пріоритету (`ResolveProviderDir` + `InjectProviderConfig`):

| Крок | Джерело каталогу | Поведінка | Код |
|---|---|---|---|
| 1 | Явний непорожній `cmProviders.dir` від викликача | Використовується як є, розгортання не виконується (лише дописується арх-суфікс до `lib`) | `UAPKIConnectHelper.cpp:436-452` |
| 2 | Провайдер `cm-pkcs12_<arch>.dll` **поруч із власною DLL** | Каталог визначається через `GetOwnModuleDir`; покриває тести й не-1С розгортання | `UAPKIConnectHelper.cpp:359-396` (крок — `369-387`) |
| 3 | Розгортання вбудованого ресурсу в `%LOCALAPPDATA%\SimplyAddinConnect\providers\<VERSION_FULL>\` | `EnsureProviderDeployed`: `FindResourceW` / `LoadResource` / `LockResource` → `CreateDirectoryW` → запис у тимчасове ім'я → атомарний `MoveFileExW` | `UAPKIConnectHelper.cpp:220-357` |

**Крок 2 — визначення власного каталогу.** `GetOwnModuleDir`
(`UAPKIConnectHelper.cpp:173-217`) отримує дескриптор **саме своєї DLL** через
`GetModuleHandleExW` з прапорцями `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT` та адресою функції-якоря `ModuleAnchor`
(`UAPKIConnectHelper.cpp:179-182`). Якір — порожня функція в анонімному просторі імен,
що слугує виключно як адреса всередині поточного модуля (`UAPKIConnectHelper.cpp:30-36`).
Це критично: `GetModuleFileNameW` без аргументів (тобто через `GetModuleHandle(NULL)`)
повернув би шлях до хост-процесу `1cv8.exe`, а не до нашої DLL.

**Крок 3 — атомарне розгортання.** `EnsureProviderDeployed` створює директорії поетапно
(`CreateDirectoryW`, `UAPKIConnectHelper.cpp:296-305`), пише бінарник у тимчасовий файл
`<файл>.tmp_<PID>` і переміщує його атомарно через
`MoveFileExW(MOVEFILE_REPLACE_EXISTING)` (`UAPKIConnectHelper.cpp:308-347`), з обробкою
програної гонки між процесами 1С. Якщо файл цієї версії вже розгорнутий — повторно не
пишеться. Той самий якір `ModuleAnchor` повторно використовується тут для отримання
дескриптора модуля під пошук ресурсу (`UAPKIConnectHelper.cpp:265-273`).

Докстрінги, що описують увесь порядок і компоненти, — `UAPKIConnectHelper.h:59-97`.

> **Чому знадобився крок 3.** 1С не розпаковує з ZIP додаткові DLL — у цільовому каталозі
> лишаються лише файли, описані в `manifest.xml`. Тому провайдер, покладений поруч, у
> реальному 1С-розгортанні не знаходився, і `INIT` віддавав `errorCode:0` з
> `countCmProviders:0` (мовчазний збій, див. §5). Розгортання вбудованого ресурсу в
> `%LOCALAPPDATA%` робить компоненту повністю самодостатньою.

---

## 4. Завантаження провайдера ядром: `LoadLibraryW`

Ядро UAPKI завантажує провайдер у рантаймі під час обробки `INIT` (за полем `cmProviders`).

`CmLoader::load` формує ім'я файлу через `getLibName` (`LIBNAME_PREFIX + libName + "." +
LIBNAME_EXT`; на Windows `LIBNAME_PREFIX=""`, `LIBNAME_EXT="dll"`) і викликає
`dl_load_library_utf8(lib_name.c_str())` (`cm-loader.cpp:55-74`, рядок 73).

На Windows `dl_load_library_utf8` конвертує UTF-8-шлях у UTF-16 через
`MultiByteToWideChar(CP_UTF8, ...)` і викликає `LoadLibraryW`
(`extern/uapki/library/common/loaders/dl-macros.h:47-59`). Це навмисно: шляхи з не-ASCII
символами (наприклад, кирилицею в `%LOCALAPPDATA%\<Користувач>\...`) обробляються коректно
незалежно від активної ANSI-кодової сторінки. На Linux/macOS цей символ — макрос-аліас на
`dlopen` (`dl-macros.h:71`).

**Конфіг `cmProviders`.** Коли викликач не задав `cmProviders`, хелпер підставляє типову
конфігурацію (`UAPKIConnectHelper.cpp:412-427`):

```jsonc
"cmProviders": {
    "dir": "<providerDir>",
    "allowedProviders": [ { "lib": "cm-pkcs12<archSuffix>" } ]
}
```

Якщо `cmProviders` заданий, але без `dir` — `dir` підставляється, а до `lib` кожного
елемента `allowedProviders` без суфікса дописується `_x86` / `_x64`
(`UAPKIConnectHelper.cpp:436-468`). На боці ядра поля `cmProviders.dir` /
`allowedProviders[].lib` / `allowedProviders[].config` розбираються в `setup_cm_providers`
(`extern/uapki/library/uapki/src/api/library-init.cpp:63-86`).

---

## 5. Чому «однієї DLL» бути не може

Спокуслива альтернатива — упакувати все в один бінарник — **неможлива** з двох незалежних
причин: технічної (архітектура завантажувача UAPKI) і продуктової (заявлений напрям
розвитку бібліотеки).

**Технічно: `CmLoader` завжди робить `LoadLibrary`.** Провайдер НКІ під'єднується
виключно як зовнішня бібліотека, яку `CmLoader::load` завантажує через
`dl_load_library_utf8` → `LoadLibraryW` (§4). У ядрі немає шляху, який би дозволив
використати провайдер, статично злінкований у той самий модуль: `setup_cm_providers`
розбирає `cmProviders` і викликає завантажувач за іменем файлу. Отже, провайдер **мусить**
існувати як окремий файл на диску на момент `INIT` — саме тому головна DLL несе його
вбудованим ресурсом і розгортає перед завантаженням.

**Підтверджено ядровим розробником UAPKI.** Розробник бібліотеки прямо зазначив: «uapkic
та uapkif статикою збирали, саму uapki ні… для роботи з особистими ключами обов'язково
зовнішні бібліотеки [cm-*], без суттєвої переробки ніяк не вийде». Механізм задумано як
**каталог провайдерів**, а не хардкод одного файлу — у наступній версії провайдерів буде
щонайменше два: `cm-pkcs12` (файлові контейнери) і `cm-pkcs11` (апаратні токени). Тобто
навіть якби «одна DLL» була технічно можлива сьогодні, вона суперечила б напряму розвитку.

**Наслідок для пакування.** Гібридна схема дає рівно те, що потрібно 1С: у цільовий каталог
розпаковується один файл (сама DLL за архітектурою), а провайдер своєї архітектури вже
вбудований у неї та самостійно розгортається при `INIT` (§3). Окремі файли
`cm-pkcs12_x86.dll` / `cm-pkcs12_x64.dll` лишаються в дистрибутивному ZIP для не-1С
сценаріїв (ручний запуск, тестовий харнес, діагностика) — компонента підхопить їх кроком 2
потрійного пошуку, якщо покласти поруч із DLL.

---

## 6. Мовчазний збій завантаження провайдера й компенсація в хелпері

Важлива особливість поведінки ядра: **невдале завантаження провайдера ковтається мовчки**.

`setup_cm_providers` ігнорує код повернення `CmProviders::loadProvider(...)` через явний
`(void)`-каст і **завжди** повертає `RET_OK`, незалежно від успіху завантаження кожного
окремого провайдера (`extern/uapki/library/uapki/src/api/library-init.cpp:82, 85`):

```cpp
(void)CmProviders::loadProvider(s_dir, s_lib, s_config);
...
return RET_OK;
```

Тому `INIT` може повернути `errorCode:0`, хоча жоден провайдер не завантажився
(`countCmProviders:0`). Щоб це не лишалося непоміченим, хелпер має компенсуючий механізм
`WarnIfProvidersNotLoaded` (`UAPKIConnectHelper.h:110-121`; реалізація —
`UAPKIConnectHelper.cpp:487-541`): **після** `INIT` він звіряє `result.countCmProviders`
з очікуваною кількістю (`cmProviders.allowedProviders`) і логує WARN при недоборі —
**не змінюючи** ні відповідь, ні код успішності операції.

---

## 7. Робота з сабмодулем UAPKI: гілкова модель форку й синхронізація

Сабмодуль `extern/uapki` — форк `github.com/VSydorenko/UAPKI` від upstream
`github.com/specinfo-ua/UAPKI`. Форк тримаємо як **транслятор змін в upstream**, а не як
паралельну кодову базу: власного функціоналу в ньому не накопичуємо. UAPKI **не збираємо
локально окремою бібліотекою** — гібридна збірка головного репо (`CMake/uapki_full_static.cmake`)
статично тягне сирці ядра прямо з дерева сабмодуля (`file(GLOB)`), тож власна система збірки
сабмодуля нам не потрібна.

### 7.1. Гілки форку

| Гілка | Роль | Base | Указник сабмодуля |
|---|---|---|---|
| `main` | **Дзеркало upstream.** Власних правок немає; оновлюється reset/ff до `upstream/main`. | `upstream/main` | ні |
| `main-dev` | **Гілка розробки/адаптації.** Upstream + наші правки, ще НЕ прийняті в upstream (наразі `*_STATIC` export-заголовки + `LoadLibraryW` UTF-8) + за потреби адаптація складу збірки. | `main` | **так** (`.gitmodules: branch = main-dev`) |
| topic-гілки (напр. `static-export-headers`, `loadlibraryw-utf8`) | **PR у upstream** — по одній атомарній зміні. | `upstream/main` | ні; живуть, доки відкритий відповідний PR (видалення гілки закриває PR) |

Указник сабмодуля в гілці головного репо завжди вказує на коміт **`main-dev`**. Правки в сабмодулі
комітяться **всередині сабмодуля** (не з кореня) — не загубити при `submodule update`.

### 7.2. Синхронізація з новою версією upstream

Коли в upstream виходить нова версія:

1. `git -C extern/uapki fetch upstream --tags`; оновити `main` (reset/ff до `upstream/main`); push у форк.
2. **PR з `main` у `main-dev` (у форку)** — надійніше за ручний rebase: конфлікти видно явно,
   історія збережена, адаптацію легко ревʼюити. (Саме тому `main` — чисте дзеркало: щоб такий PR був
   зрозумілим diff'ом «нове ядро vs наші правки».)
3. Вирішити конфлікти в `main-dev` (наші правки vs оновлене ядро), за потреби адаптувати
   `CMake/uapki_full_static.cmake` — **особлива увага до ЯВНИХ (не-glob) посилань на сирці**: ядро
   тягнеться `file(GLOB)`, а ось ціль `cm-pkcs12-provider` перелічує `common/pkix/*.c` поіменно
   (напр. при оновленні 2026-07 довелось додати `ecdsa-params.c`, який почав викликати `private-key.c`).
4. Оновити указник сабмодуля в гілці головного репо; `build_project.ps1 -WithUAPKI -WithTests` +
   `run_tests` мають лишатись зеленими (PASS=17). Ручний тест у 1С (`INIT` → `countCmProviders:1`).

### 7.3. Додавання функціоналу, що зачіпає UAPKI, і внесок назад в upstream

- **Функціонал у головному репо:** окрема гілка головного репо → зміни за конвенціями `AGENTS.md`
  → за потреби правки в `main-dev` сабмодуля → PR у головному репо → чекати accept/reject.
- **Внесок в upstream (за прямими рекомендаціями мейнтейнера):** окрема **atomic** topic-гілка від
  `upstream/main`, **один незалежний коміт → окремий PR** (не змішувати незалежні зміни в один PR);
  **build-only інфраструктуру відділяти від рантайм/сертифікованого коду** (знімає занепокоєння щодо
  експертизи); описи двомовні **EN+UA**. Після прийняття PR — зміна приходить у `main`, і наступний
  sync-PR `main`→`main-dev` робить її частиною бази (наш topic-патч тоді стає непотрібним).

---

## Куди дивитись далі

- [`extern/uapki/doc/UAPKI-PM-2.0.16.md`](../../extern/uapki/doc/UAPKI-PM-2.0.16.md) — повний
  JSON-протокол UAPKI (методи, параметри, коди помилок; Таблиці 1-3, формат запиту/відповіді; є й
  англійська версія).
- [docs/integration-1c/uapki.md](../integration-1c/uapki.md) — прикладна інтеграція з 1С
  (єдина точка входу `ВызватьUAPKI`, приклади коду, тестування).
- [`.claude/skills/uapki-integration`](../../.claude/skills/uapki-integration/SKILL.md) —
  переносне ноу-хау з інтеграції UAPKI.
- [`README.md`](README.md) — індексний огляд усієї архітектури компоненти.
- `CMake/uapki_full_static.cmake` — джерело правди щодо гібридної збірки ядра й провайдера.
