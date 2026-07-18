# Задача: Самодостатнє розгортання CM-провайдерів з ресурсів DLL + тестовий харнес ЕЦП (L0–L3)

**Дата постановки:** 2026-07-19
**Гілка:** `add_UAPKI` (сабмодуль `extern/uapki` — гілка `static-build`, базовий коміт `ff8c679`)
**Статус:** до виконання
**Базується на:** виконаній задачі `2026-07-18_uapki_hybrid_build.md` (гібридна збірка працює, верифіковано) + емпіричному тесті з 1С 2026-07-19.

---

## 1. Мета

1. Зробити компоненту **повністю самодостатньою для 1С**: провайдер `cm-pkcs12` вбудовується бінарним ресурсом у основну DLL і автоматично розгортається при `INIT` — після цього в реальній 1С `INIT` повертає `countCmProviders:1` без жодних дій користувача.
2. Створити **тестовий харнес L0–L3**, що перевіряє весь ланцюг ЕЦП (INIT → OPEN → SIGN → VERIFY) **без 1С**, з асертами, придатний для регулярного прогону.

## 2. Контекст — чому це потрібно (доведені факти, НЕ переперевіряти)

1. **1С НЕ розпаковує додаткові файли з ZIP компоненти.** Емпірично доведено 2026-07-19 на реальній 1С: у `%APPDATA%\1C\1cv8\ExtCompT\` потрапляє РІВНО ОДИН файл — DLL, що відповідає поточній ОС/архітектурі з `manifest.xml`. Додаткові записи в маніфесті (у т.ч. трюк з `arch="ARM"`) — ігноруються. Офіційна документація ITS (локально: `tmp/its.1c.ru_db_content_metod8dev_src_developers_platform_i8103221.htm#_print.pdf`) не містить жодного механізму додаткових файлів для десктопної платформи.
2. **UAPKI мовчки ковтає невдале завантаження провайдера**: `extern/uapki/library/uapki/src/api/library-init.cpp:82` — `(void)CmProviders::loadProvider(...)`; `setup_cm_providers` завжди повертає `RET_OK`. Тому в 1С зараз `INIT` дає `errorCode:0`, але `countCmProviders:0` — і `OPEN`/`SIGN` не працюватимуть.
3. **Автоінжекція `cmProviders` у хелпері вже реалізована і працює** (`UAPKIConnectHelper::InjectProviderConfig`) — поза 1С, коли провайдер лежить поруч із DLL, ланцюг `INIT→countCmProviders=1→PROVIDERS` доведено. Проблема лише в доставці файлу провайдера на диск.
4. Патерн "розгорнути у свій каталог і вантажити по повному шляху" — канонічний для UAPKI (так розробники пакують JAR-дистрибуцію).

## 3. Архітектурне рішення (ЗАТВЕРДЖЕНО — не переглядати)

**Потрійний порядок пошуку провайдера** в хелпері при `INIT` (заміна/розширення поточної логіки `InjectProviderConfig`):

1. Якщо викликач явно задав `cmProviders.dir` — поважаємо як є (нічого не розгортаємо, нічого не переписуємо, крім арх-суфіксів імен).
2. Якщо `cm-pkcs12_<arch>.dll` існує **поруч із власною DLL** — використовуємо каталог DLL (покриває тести, ручні розгортання, не-1С хости).
3. Інакше — **розгортаємо вбудований ресурс** у `%LOCALAPPDATA%\SimplyAddinConnect\providers\<VERSION_FULL>\cm-pkcs12_<arch>.dll` (якщо файл цієї версії ще не розгорнутий) і підставляємо цей каталог.

Кожна основна DLL вбудовує ЛИШЕ провайдер своєї архітектури (x64 → `cm-pkcs12_x64.dll`, +~1.1 МБ). ZIP-поставка ЛИШАЄТЬСЯ з 5 файлів (провайдери поруч корисні для не-1С розгортань і нічому не шкодять).

Додатково: патч сабмодуля `cm-loader.cpp` з `LoadLibraryA` на `LoadLibraryW` — знімає ANSI-ризик кирилічних шляхів (`%LOCALAPPDATA%` містить ім'я користувача); милиця `GetShortPathNameW` у `GetOwnModuleDir` після цього стає непотрібною (можна лишити як belt-and-suspenders, на розсуд).

## 4. Обмеження та заборони

- Сабмодуль `extern/uapki`: працюємо на гілці **`static-build`** (від `ff8c679`); upstream НЕ підтягувати; merged-схему форку (`BUILD_MERGED_UAPKI_DLL`) ігнорувати. Нові правки сабмодуля — окремими комітами в цю ж гілку.
- Конвенції `AGENTS.md` обов'язкові (PCH, `REPORT_*`/`NEUTRAL_REPORT_*` з іменем компоненти в статиці, конкатенація повідомлень, `return false` після `REPORT_ERROR`, `ServiceTools::Safe*` конвертації, мова файлу).
- `git push` (форк і основне репо) — НЕ виконувати без окремого підтвердження користувача.
- НЕ чіпати ECRPrivatJSON/транспорт. `version.h` — очікуваний diff від збірки.
- НЕ додавати `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` ніде; експорти основної DLL — рівно 3 символи з `AddInNative.def` (якщо для тестів L1 потрібні `process`/`json_free` — НЕ чіпати основну DLL, L1 лінкує `uapki_bundle` напряму).

## 5. Кроки реалізації

### Крок A. Патч сабмодуля: LoadLibraryW

1. `extern/uapki/library/common/loaders/cm-loader.cpp` (+ за потреби `dl-macros.h`): на Windows конвертувати `lib_name` (UTF-8) → UTF-16 (`MultiByteToWideChar(CP_UTF8, ...)`) і викликати `LoadLibraryW`. Non-Windows гілки не чіпати. Звірити, чи `uapki-loader.cpp` (використовується тестами) має ту саму проблему — за потреби виправити симетрично.
2. Коміт у сабмодуль (гілка `static-build`), українською, з поясненням.

### Крок B. Вбудовування провайдера ресурсом (CMake)

1. У `CMake/uapki_full_static.cmake` (або окремий модуль): згенерувати `.rc`-файл через `configure_file`/`file(GENERATE)` з рядком виду:
   `CM_PKCS12_PROVIDER RCDATA "<абсолютний шлях до $<TARGET_FILE:cm-pkcs12-provider>>"`
   Врахувати: generator expressions у `.rc` напряму не працюють — використати `file(GENERATE OUTPUT ... CONTENT ...)` з `$<TARGET_FILE:...>` (він підтримує genex) або відомий шлях `bin/Release/cm-pkcs12_<arch>.dll`.
2. Додати згенерований `.rc` до сирців цілі `${TARGET}` (основна DLL) при `BUILD_WITH_UAPKI=ON` + `add_dependencies(${TARGET} cm-pkcs12-provider)` — провайдер має бути зібраний ДО компіляції ресурсу основної DLL. УВАГА: перевірити на VS-генераторі, що RC-компіляція реально відбувається після збірки провайдера (можливо, знадобиться проміжна кастом-ціль).
3. Ім'я/тип ресурсу зафіксувати константами, доступними хелперу (наприклад, `#define UAPKI_PROVIDER_RESOURCE_NAME L"CM_PKCS12_PROVIDER"` у спільному заголовку).

### Крок C. Розгортання в хелпері

У `UAPKIConnectHelper` (усе під `#ifdef WITH_UAPKI`, Windows-гілки під `#ifdef _WINDOWS`):

1. Новий приватний метод `EnsureProviderDeployed(std::string& outUtf8Dir)`:
   - цільовий каталог: `%LOCALAPPDATA%\SimplyAddinConnect\providers\<VERSION_FULL>\` (версію взяти з `version.h` — макрос `VERSION_FULL`; стрингіфікація через допоміжний макрос). `SHGetKnownFolderPath(FOLDERID_LocalAppData)` або `GetEnvironmentVariableW("LOCALAPPDATA")`;
   - якщо `cm-pkcs12_<arch>.dll` вже існує в цільовому каталозі — успіх (без перезапису);
   - інакше: `FindResourceW`/`LoadResource`/`LockResource`/`SizeofResource` з власного HMODULE (той самий якір `ModuleAnchor`) → створити каталоги (`SHCreateDirectoryExW` або поетапно `CreateDirectoryW`) → записати у **тимчасове ім'я** (`cm-pkcs12_<arch>.dll.tmp_<pid>`) → `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)` — атомарність проти гонки двох процесів 1С (rphost/клієнти можуть ініціалізуватися паралельно);
   - якщо `MoveFileExW` програв гонку, але цільовий файл існує — це успіх (хтось розгорнув раніше); тимчасовий файл видалити;
   - помилки запису (права, антивірус) — `NEUTRAL_REPORT_ERROR` з кодом `GetLastError()` і `return false`.
2. Інтегрувати потрійний порядок пошуку (розділ 3) в `InjectProviderConfig`. Точка входу: якщо викликач НЕ задав `dir` — спершу перевірка "поруч із DLL" (`GetOwnModuleDir` + `PathFileExistsW`/`GetFileAttributesW` на `cm-pkcs12_<arch>.dll`), інакше `EnsureProviderDeployed`.
3. **Перевірка результату INIT**: після успішного `process()` для методу `INIT` розпарсити відповідь; якщо `result.countCmProviders` < кількості провайдерів у інжектованому/переданому `allowedProviders` — `NEUTRAL_REPORT_WARN` з деталями (який dir передавався, які lib, скільки завантажилось). JSON-відповідь UAPKI НЕ модифікувати (прозорість для 1С), `return` не міняти — операція формально успішна.

### Крок D. Тестовий харнес

Створити теку `tests/` (оживляє `-WithTests`; прибрати попередження в `AGENTS.md` про відсутність тестів — оновити розділ). `tests/CMakeLists.txt` збирає ДВІ консольні цілі (тільки Windows, тільки при `BUILD_WITH_UAPKI=ON`):

**D1. `uapki_selftest.exe` (L1 — крипто-ядро).** Лінкує `uapki_bundle` (той самий статичний бандл, що й основна DLL) + прямі `extern "C"` оголошення `process`/`json_free`. Функціонал:
- приймає шлях до JSON-сценарію (формат upstream `test.cpp`: `{comment, tasks:[{method, parameters, ...}]}`) — цикл портувати з `extern/uapki/library/test/test.cpp:54-89,172-297`, замінивши `UapkiLoader` на прямі виклики;
- парсер: parson з режимом коментарів (`ParsonHelper.parse(..., true)`) — сценарії містять `//`-коментарі; АБО передобробка. НЕ nlohmann для сценаріїв;
- **асерти** (відсутні в upstream): для кожного task очікування `errorCode==0`, окрім задач, помічених у сценарії полем `expectError` (додати підтримку); підсумковий exit code ≠ 0 при будь-якому провалі;
- спеціальні перевірки: у відповіді `INIT` — `assert countCmProviders == <очікуване>`; для `DIGEST` — порівняння з еталонними хешами; для негативного VERIFY-кейсу — статус підпису НЕ valid.

Набір сценаріїв — у `tests/scenarios/` (нові, за мотивами upstream `test/data/*.json`, але з нашими шляхами і полями очікувань):
| Сценарій | Зміст | Очікування |
|---|---|---|
| `01_version_init_providers.json` | VERSION → INIT{offline:true, cmProviders{dir:".", lib:"cm-pkcs12_x64"}} → PROVIDERS → DEINIT | countCmProviders==1, список містить PKCS12 |
| `02_digest.json` | порт upstream digest.json | еталонні хеші збігаються (ГОСТ 34311: `0f1355...` з upstream) |
| `03_sign_offline.json` | INIT{offline:true,...} → OPEN(test-diia.p12, password "testpassword") → KEYS → SELECT_KEY `5BC6C06E...315C` → SIGN CAdES-BES attached {signParams..., options:{ignoreCertStatus:true}} → CLOSE → DEINIT | errorCode==0 всюди, підпис не порожній |
| `04_verify.json` | порт upstream verify-dstu.json (5 кейсів, включно з підміненим контентом) | 4 valid, 5-й НЕ valid |
| `05_sign_verify_roundtrip.json` | SIGN (як 03) → VERIFY власного підпису тим самим процесом | valid, signer-сертифікат = diia-test-sign |
| `06_encrypt_decrypt.json` | порт encrypt.json+decrypt.json (ключ КЕП `6B1B77C0...B782`) | roundtrip: розшифроване == вихідне |
| `07_offline_negative.json` | INIT{offline:true} → SIGN CAdES-T (signatureTimeStamp) | очікувана помилка RET_UAPKI_OFFLINE_MODE (expectError) |

Тестові дані: скопіювати в `tests/data/` з `extern/uapki/library/test/data/`: `test-diia.p12`, `test-fox.txt`, `certs/` (усі), `crls/` (не редагувати оригінали в сабмодулі). Пароль контейнера: `testpassword`. Сертифікати тестового ланцюга ПРОСТРОЧЕНІ (notAfter 2024-04-05) — тому скрізь `offline:true` + `ignoreCertStatus:true`, VERIFY без validationType (=STRUCT). УВАГА: `INIT` можливий раз на процес — selftest виконує ОДИН сценарій на запуск процесу (раннер-скрипт запускає по черзі); сценарії завершуються `DEINIT`.

**D2. `native_host.exe` (L2 — e2e як 1С).** Емуляція платформи: `LoadLibraryW(<шлях до SimplyAddinConnectWin_x64.dll>)` → `GetProcAddress("GetClassObject"/"GetClassNames"/"DestroyObject")` → створення об'єкта `AddinUAPKIConnect` через `IComponentBase` (заголовки SDK в `include/`; звірити точну сигнатуру `GetClassObject(const WCHAR_T* wsName, IComponentBase** pInterface)`) → виклик методу `CallUapki` через `CallAsFunc` з параметрами-варіантами (`tVariant`, рядки UTF-16). Кейси:
1. **Ресурсне розгортання**: скопіювати основну DLL у чистий тимчасовий каталог БЕЗ провайдера; видалити `%LOCALAPPDATA%\SimplyAddinConnect\providers\<поточна версія>\` якщо існує; `CallUapki("INIT","")` → у відповіді `countCmProviders==1`; перевірити, що файл провайдера з'явився в `%LOCALAPPDATA%`. Це ГОЛОВНИЙ тест — точна симуляція ситуації в 1С.
2. **Провайдер поруч**: DLL + `cm-pkcs12_x64.dll` в одному каталозі → INIT → `countCmProviders==1`; `%LOCALAPPDATA%`-каталог НЕ створюється (перевірити).
3. **Явний dir від викликача**: `CallUapki("INIT", "{\"cmProviders\":{\"dir\":\"<tests/data-шлях>\",...}}")` → поважається без розгортання.
4. Повний ланцюг через компоненту: INIT → OPEN(test-diia.p12) → SIGN → VERIFY (сирий JSON параметрів).
Exit code ≠ 0 при провалі будь-якого кейсу.

**D3. L3 — крос-валідація (у `native_host.exe` або окремий сценарій selftest):**
1. Наш VERIFY на еталонах ДФС: `R:/github/prro_docs/Єдине вікно подання електронної звітності/Приклади/Приклади з КЕП/чек.xml.signed`, `Z-звіт.xml.signed`, `запит_стану_РРО.json.signed` (справжні DER PKCS#7, прийняті бойовим сервером ДПС). Очікування: errorCode==0, STRUCT-валідний підпис, витягується сертифікат підписувача. Шлях до prro_docs — параметром/змінною оточення (репозиторій може бути відсутній — тоді skip з повідомленням, НЕ провал).
2. Структурна перевірка нашого підпису з кейсу SIGN: розпарсити відповідь VERIFY нашого ж ядра і перевірити: вкладений сертифікат присутній; content-time-stamp ВІДСУТНІЙ; CRL/OCSP у контейнері відсутні (вимоги ЄВПЕЗ до підпису ПРРО). Незалежний крос-чек через jkurwa (npm) — ОПЦІЙНИЙ бонус, не блокер.

**D4. Оркестрація**: оновити `run_tests.ps1`: (1) L0 — dumpbin-перевірки експортів/імпортів (очікування: основна DLL 3 експорти; провайдер 7; імпорти провайдера лише системні) + ctypes/python smoke `provider_info` якщо python доступний (інакше skip); (2) збірка tests (`-WithTests` тепер має працювати з `-WithUAPKI`); (3) прогін selftest по всіх сценаріях (окремий процес на сценарій); (4) native_host. Підсумок: зелений/червоний з переліком.

### Крок E. Документація

- `docs/ARCHITECTURE.md` §6: доповнити механізм розгортання провайдерів (потрійний порядок, `%LOCALAPPDATA%`, ресурс).
- `AGENTS.md`: розділ "Тести" переписати (тести тепер Є, як запускати); згадати `-WithTests`.
- `docs/tasks/2026-07-19_uapki_embedded_providers_and_tests.md` (цей файл): розділ 9 "Звіт виконання" за зразком попередньої задачі.

## 6. Верифікація (обов'язкова, вся)

1. `build_project.ps1 -WithUAPKI` → exit 0; ZIP 5 файлів; розмір основної DLL зріс на ~1.1 МБ (вбудований ресурс).
2. `dumpbin /exports` основної DLL — як і раніше РІВНО 3 символи (ресурс не впливає).
3. Головний e2e: чистий темп-каталог лише з `SimplyAddinConnectWin_x64.dll` + видалений `%LOCALAPPDATA%\SimplyAddinConnect` → `native_host` кейс 1 → `countCmProviders==1`, провайдер розгорнувся.
4. Повний прогін `run_tests.ps1` — усі рівні зелені; негативні кейси (04 п'ятий VERIFY, 07) реально провалюються "правильним" чином.
5. Гонка: запустити 2 екземпляри `native_host` кейс 1 ОДНОЧАСНО (після видалення `%LOCALAPPDATA%`-кешу) — обидва мають завершитись успішно.
6. Кирилиця: прогнати кейс 1 з тимчасовим каталогом, що містить кирилицю в шляху (перевірка LoadLibraryW-патчу).
7. Регресія: `build_project.ps1` (без UAPKI) → збірка ок; `-WithUAPKI -WithTests` → збірка тестів ок.
8. `git status`: сабмодуль чистий (нові коміти в `static-build`), основне репо — тільки очікувані зміни.

## 7. Відомі граблі

- **RC + genex**: `$<TARGET_FILE:...>` у `configure_file` НЕ працює — тільки `file(GENERATE)`. Шлях у `.rc` — з подвійними бекслешами або прямими слешами.
- **Порядок збірки**: без `add_dependencies` RC може скомпілюватися до появи провайдерної DLL → зламана/порожня вкладка ресурсу. Перевірити ЧИСТУ збірку (не інкрементальну).
- **Відома передіснуюча вада**: колізія проміжних `.lib` обох архітектур у `bin/Release` — інкрементальні збірки навхрест падають (LNK4272); чиста збірка коректна. НЕ чинити в цій задачі (окрема), але не сплутати з власними помилками.
- **Антивірус** може блокувати запис DLL у `%LOCALAPPDATA%` і/або LoadLibrary звідти — усі помилки логувати з `GetLastError()`; це саме той кейс, який діагностуватиметься в полі.
- `WCHAR_T` в SDK 1С — це `char16_t`-сумісний тип (див. `include/`); у native_host для рядків використовувати ті самі конвенції, що й платформа (звірити з `src/core/AddInNative.cpp`).
- Сценарії пишуть файли (наприклад, підписи) — використовувати темп-каталоги, не смітити в репо; `tests/data/` — read-only вхідні дані.
- `INIT` раз на процес (`RET_UAPKI_ALREADY_INITIALIZED`) — раннер: процес на сценарій.
- У сабмодулі `test/data/get-csr.json` зламаний (нема `test-dstu-2023.p12`) — НЕ портувати як є.

## 8. Поза межами (НЕ робити)

- PR в upstream (LoadLibraryW, *_STATIC) — окремо, після стабілізації.
- Оновлення сабмодуля до свіжого upstream.
- Фікс колізії проміжних `.lib` (окрема задача).
- ПРРО-специфіка L4 (підпис чеків за ЄВПЕЗ, тестовий fserver `cabinet.tax.gov.ua:9443`) — окрема спільна сесія.
- Прибирання старих версій з `%LOCALAPPDATA%\SimplyAddinConnect\providers\` — зафіксувати як TODO, не реалізовувати.

## 9. Звіт виконання

(заповнює виконавець: коміти з хешами; таблиця верифікації п.6; прийняті відхилення від ТЗ з обґрунтуванням; виявлені але не виправлені проблеми)
