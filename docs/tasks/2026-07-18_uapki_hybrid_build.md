# Задача: Гібридна збірка UAPKI — статичне ядро + самодостатні провайдерні DLL одним скриптом

**Дата постановки:** 2026-07-18
**Гілка:** `add_UAPKI`
**Статус:** ✅ ВИКОНАНО (2026-07-18) — див. розділ 9 «Звіт виконання»
**Автор постановки:** архітектурна сесія (оркестратор) за результатами глибокого дослідження: збірковий експеримент, аналіз коду/CMake, документація UAPKI, diff форку vs upstream, чат розробників UAPKI (Telegram "Cryptonite++, UAPKI").

---

## 1. Мета

Зробити `build_project.ps1 -WithUAPKI` **повністю робочим за один прохід**, щоб на виході був один ZIP:

```
bin/Release/SimplyAddinConnectWin.zip
├── manifest.xml                      # без змін — 2 компоненти x86/x64
├── SimplyAddinConnectWin_x86.dll     # ядро uapki+uapkic+uapkif статично всередині
├── SimplyAddinConnectWin_x64.dll     # так само
├── cm-pkcs12_x86.dll                 # САМОДОСТАТНІЙ провайдер (uapkic+uapkif статично всередині)
└── cm-pkcs12_x64.dll                 # так само
```

І щоб хост-код автоматично "склеював" основну DLL з провайдерами в рантаймі (інжекція `cmProviders.dir` + арх-суфікса імені провайдера в метод `INIT`).

## 2. Архітектурне рішення (ЗАТВЕРДЖЕНО — не переглядати в рамках цієї задачі)

**Чому саме гібрид, а не "одна DLL" і не повністю динамічна схема:**

1. UAPKI **принципово** завантажує CM-провайдери (робота з приватними ключами: `OPEN`/`SIGN`/`CREATE_KEY`) через `LoadLibraryA` за іменем файлу — `extern/uapki/library/common/loaders/cm-loader.cpp:55-98` + `dl-macros.h:37-44` (`LIBNAME_PREFIX=""`, `LIBNAME_EXT="dll"`). Статичного реєстру провайдерів у коді немає. Ядровий розробник UAPKI (Vitalii) підтвердив письмово (чат, 2025-04-30): *"для роботи з особистими ключами обов'язково зовнішні бібліотеки, там без суттєвої переробки ніяк не вийде"*.
2. Статичне вбудовування ядра (uapki+uapkic+uapkif) у головну DLL — **працює вже зараз** (перевірено збіркою 2026-07-18: обидві архітектури компілюються і лінкуються). Хост викликає `process()`/`json_free()` напряму, без loader-коду.
3. Провайдер робимо **самодостатнім** (статичні uapkic/uapkif всередині SHARED DLL), бо штатний `cm-pkcs12.dll` імпортує `uapkic.dll`/`uapkif.dll` — довелося б класти ще 4 DLL з колізією імен x86/x64 у плоскому ZIP.
4. Імена з арх-суфіксом (`cm-pkcs12_x86.dll`/`cm-pkcs12_x64.dll`) — легальні: ім'я файлу формується з рядка `lib` у параметрах `INIT`, який контролює наш хост-код.
5. У наступних версіях UAPKI провайдерів буде два: `cm-pkcs12` (файлові ключі) і `cm-pkcs11` (апаратні токени) — тому механізм має бути "каталог з N провайдерів", без хардкоду на один файл.

## 3. Поточний стан (перевірено 2026-07-18)

### Що працює
- `build_project.ps1 -WithUAPKI`: основна DLL (x86+x64) зі статичним ядром UAPKI **збирається успішно**, ZIP з 2 DLL + manifest створюється.
- `CMake/uapki_full_static.cmake` збирає ~10 статичних бібліотек з сабмодуля і через `uapki_bundle` (INTERFACE) підключає до цілі `SimplyAddinConnect` (`CMake/components.cmake:357-360`).
- Хост: `src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp:16-19` — власні `extern "C"` оголошення `process`/`json_free`; виклики на `:254`, `:270`; все під `#ifdef WITH_UAPKI`.

### Що зламано (це і чинимо)
1. **Етап провайдера в `build_project.ps1:205-352` гарантовано падає**: повторний `cmake -S extern/uapki/library/cm-pkcs12 -B build_x86|build_x64` конфліктує з кешем основного проєкту → `"source does not match cache"` → `exit 1`. Передані змінні `UAPKI_LIBRARIES`/`UAPKI_*_INCLUDE_DIR` (`:240-249`) CMakeLists провайдера ігнорує. Standalone-збірка провайдера поза деревом також неможлива: нема include-шляху до `uapkic/include` (падає на `byte-array.h` у `parson-ba-utils.c`), `link_directories(../build)` не існує, POST_BUILD копіює в неіснуючі каталоги.
2. **Провайдери не потрапляють у ZIP навіть теоретично**: ZIP пакується на `:171-186` з `Get-ChildItem bin\Release -Filter *.dll` (НЕрекурсивно, тільки корінь), а провайдери збиралися ПІСЛЯ зіпування і в підкаталог `bin\Release\providers\`.
3. **Збірка невідтворювана з чистого клону**: тримається на двох НЕЗАКОММІЧЕНИХ правках сабмодуля `extern/uapki`:
   - `library/uapkif/include/uapkif-export.h` — додана гілка `#ifdef UAPKIF_STATIC` → порожній `UAPKIF_EXPORT`;
   - `library/uapkif/include/asn_system.h` — guard навколо `WIN32_LEAN_AND_MEAN`.
4. **Забруднення експортів**: головна DLL експортує ~283 символи (aes_alloc, aes_decrypt, …) замість 3 з `src/core/AddInNative.def` (`GetClassObject`, `DestroyObject`, `GetClassNames`). Причина: у upstream-заголовках `uapkic-export.h:31-41` і `uapki-export.h:34-46` НЕМАЄ гілок `*_STATIC` — дефайни `UAPKIC_STATIC`/`UAPKI_STATIC` є no-op, і збірка "рятується" дефайнами `UAPKIC_LIBRARY`/`UAPKI_LIBRARY` (= `__declspec(dllexport)`) — `uapki_full_static.cmake:36,96,198`.
5. **Статичний `cm-pkcs12` всередині головної DLL — мертвий вантаж** (див. п.2 рішення): лінкується в бандл (`uapki_full_static.cmake:130-165,197,221-243`), але ніколи не викликається, бо провайдер шукається через `LoadLibraryA`.
6. **Хелпер**: `UAPKIConnectHelper::IsOperationSuccess` перевіряє поля `status`=="success"/"error" та `code`, але UAPKI повертає `errorCode` (integer, 0=успіх) — детекція успіху не відповідає реальному формату відповіді (`docs/UAPKI_Protokol.md`). `ParseParamsString` розбирає лише плоский формат `ключ=значення,...` — вкладені об'єкти (`cmProviders`, `trustedCerts[]`), які потрібні для `INIT`/`SIGN`, задати неможливо.

## 4. Обмеження та заборони

- **НЕ оновлювати** сабмодуль `extern/uapki` до свіжого upstream (він відстає на ~52 коміти — оновлення буде ОКРЕМОЮ задачею після цієї). Працюємо на поточному коміті `0bbc3da`.
- **Ігнорувати merged-схему форку.** У форку `VSydorenko/UAPKI` є коміт `58aba11` («объединенная UAPKI DLL» через опцію `BUILD_MERGED_UAPKI_DLL` + `WINDOWS_EXPORT_ALL_SYMBOLS`; поточний `origin/main` = `d58243a`). Це **альтернативна** схема поставки (окрема `uapki_merged.dll`, експорт усіх символів), СВІДОМО відхилена на користь гібриду (розділ 2). Її НЕ використовувати; сабмодуль лишається на `0bbc3da`. Коміти Кроку A робляться **на базі `0bbc3da`** (нова гілка від нього у форку), а НЕ поверх `main` форку з merged-роботою.
- **НЕ чіпати** компоненти/протоколи ECRPrivatJSON, транспортний шар, ядро AddInNative (крім, за потреби, `.def` — див. крок D).
- **Дотримуватися конвенцій `AGENTS.md`** (обов'язково прочитати): PCH першим рядком у кожному `.cpp`; логування ЛИШЕ через макроси `REPORT_*`/`NEUTRAL_REPORT_*`; повідомлення конкатенацією, без printf-стилю; після `REPORT_ERROR` — `return false`, без `throw` для бізнес-помилок; конвертації рядків через `ServiceTools::SafeMB2WCHAR`/`SafeWCHAR2MB`.
- Правки в сабмодулі `extern/uapki` комітяться **всередині сабмодуля** (окремим комітом у форк `VSydorenko/UAPKI`), потім у головному репо фіксується новий указник сабмодуля. `git push` форку та основного репо — НЕ робити без окремого підтвердження користувача.
- `version.h` перегенеровується скриптом при кожній збірці — очікуваний diff, не редагувати вручну.
- Мова коду/комітів — українська/російська (дотримуйся мови файлу, який редагуєш).

## 5. Кроки реалізації

### Крок A. Зафіксувати правки сабмодуля (відтворюваність збірки)

1. У `extern/uapki` закомітити поточні незакоммічені зміни (`uapkif-export.h`, `asn_system.h`) з осмисленим повідомленням (наприклад: `"Підтримка статичної збірки: гілка UAPKIF_STATIC в uapkif-export.h, guard WIN32_LEAN_AND_MEAN в asn_system.h"`).
2. **Додати аналогічні `*_STATIC`-гілки в решту export-заголовків** сабмодуля (це прибере dllexport-хак і забруднення експортів, див. крок D):
   - `library/uapkic/include/uapkic-export.h`: `#if defined(UAPKIC_STATIC) → #define UAPKIC_EXPORT` (порожній), інакше — як було;
   - `library/uapki/include/uapki-export.h`: аналогічно для `UAPKI_STATIC`/`UAPKI_EXPORT`;
   - перевірити `grep -r "dllexport" extern/uapki/library --include=*.h` на інші export-заголовки (byte-array, cm-api тощо) і за наявності обробити так само (`BA_STATIC`, ...).
   - Зразок — уже наявна правка `uapkif-export.h` (подивитись `git show` останнього коміту сабмодуля).
3. Закомітити в сабмодуль; у головному репо — коміт з оновленим указником сабмодуля.

### Крок B. CMake: провайдер як SHARED-ціль у тому ж дереві збірки

У `CMake/uapki_full_static.cmake`:

1. **Прибрати `cm-pkcs12` з бандла основної DLL**: видалити його з `target_link_libraries(uapki PUBLIC ...)` (`:197`) та зі списку `uapki_full_static` (`:221-243`). Саму STATIC-ціль `cm-pkcs12` можна лишити (вона стане основою провайдера) або перейменувати — на розсуд.
2. **Додати SHARED-ціль провайдера**, що збирається в тому ж CMake-проєкті (жодних повторних configure!):
   ```cmake
   # Ціль існує лише при BUILD_WITH_UAPKI=ON (файл і так include-иться умовно)
   add_library(cm-pkcs12-provider SHARED ${CM_PKCS12_SRC} <ті ж додаткові common-сирці, що в STATIC cm-pkcs12>)
   target_include_directories(... як у STATIC cm-pkcs12 ...)
   target_link_libraries(cm-pkcs12-provider PRIVATE uapkic uapkif parson stacktrace ba-utils byte-array bcrypt crypt32 ws2_32)
   target_compile_definitions(cm-pkcs12-provider PRIVATE CM_LIBRARY NOCRYPT _CRT_SECURE_NO_WARNINGS)
   if(CMAKE_SIZEOF_VOID_P EQUAL 8)
       set_target_properties(cm-pkcs12-provider PROPERTIES OUTPUT_NAME "cm-pkcs12_x64")
   else()
       set_target_properties(cm-pkcs12-provider PROPERTIES OUTPUT_NAME "cm-pkcs12_x86")
   endif()
   # Вихід — у bin/Release поруч з основною DLL (звірити механізм із CMake/output_settings.cmake,
   # щоб RUNTIME_OUTPUT_DIRECTORY_RELEASE збігався з основною ціллю)
   ```
3. **Важливо — експорт символів провайдера**: `CmLoader` шукає через `GetProcAddress`: `provider_info`, `provider_init`, `provider_deinit`, `provider_open`, `provider_close`, `block_free`, `bytearray_free` (+опційні `provider_list_storages`, `provider_storage_info`, `provider_format`). Перевірити, яким механізмом ці символи експортуються в upstream (`CM_LIBRARY` → dllexport у cm-api заголовках; upstream також використовує `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` — нам ЦЕ НЕ підходить, бо витягне і статичні uapkic/uapkif символи; якщо `CM_LIBRARY`-механізму недостатньо — зробити `.def`-файл для провайдера з цими ~10 символами).
4. **Простежити узгодженість дефайнів**: після кроку A статичні цілі мають збиратися з `UAPKIC_STATIC`/`UAPKIF_STATIC`/`UAPKI_STATIC`/`BA_STATIC` і БЕЗ `UAPKIC_LIBRARY`/`UAPKI_LIBRARY` (прибрати їх з `uapki_full_static.cmake:36,96,198` та перевірити `options.cmake:11-15`). Ціль: нуль dllexport з об'єктів, що лінкуються в основну DLL.
5. CRT: провайдер має успадкувати `/MT` (глобальні налаштування `CMake/dependencies.cmake:63-79` вже форсують) — просто перевірити, що нова SHARED-ціль їх отримує.

### Крок C. `build_project.ps1`: спростити і полагодити пакування

1. **Видалити повністю блок збірки провайдерів `:205-352`** (повторні cmake configure у зайняті build-теки) — тепер провайдер збирається як частина основної збірки кожної архітектури.
2. Переконатися, що після двох `cmake --build` у `bin\Release` лежать 4 DLL (2 основні + 2 провайдери) — і **лише потім** виконується наявний код зіпування `:171-191` (він і так бере всі `*.dll` з кореня `bin\Release` + `manifest.xml` — тепер автоматично захопить і провайдери).
3. Оновити перевірку результату (`:194-203`): очікувані файли — `SimplyAddinConnectWin_x86.dll`, `SimplyAddinConnectWin_x64.dll`, `cm-pkcs12_x86.dll`, `cm-pkcs12_x64.dll`, `SimplyAddinConnectWin.zip`; якщо чогось нема — явна помилка з переліком відсутнього і `exit 1`.
4. Каталог `bin\Release\providers` більше не потрібен — не створювати.
5. `manifest.xml` НЕ міняти: провайдери в ньому НЕ описуються (вони не є компонентами 1С; додаткові файли в ZIP допустимі).

### Крок D. Хост-код: автоінжекція провайдерів + виправлення хелпера

Файл: `src/helpers/UAPKIConnect/UAPKIConnectHelper.{h,cpp}` (все під `#ifdef WITH_UAPKI`; конвенції AGENTS.md обов'язкові).

1. **Сирий JSON у параметрах**: якщо рядок параметрів `CallUapki` після trim починається з `{` — трактувати його як ГОТОВИЙ JSON-об'єкт `parameters` (парсити nlohmann::json, він уже в проєкті) і НЕ проганяти через `ParseParamsString`. Плоский формат `ключ=значення,...` лишити для сумісності. Це розблоковує `INIT` з вкладеним `cmProviders`, `SIGN` зі складними параметрами тощо.
2. **Автоінжекція провайдерів у `INIT`**: якщо `method` == `"INIT"` (без урахування регістру), перед викликом `process()`:
   - обчислити каталог власної DLL: `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&якась_функція_цього_модуля, &hMod)` → `GetModuleFileNameW` → відрізати ім'я файлу; **не** використовувати `GetModuleHandle(NULL)` (це дасть шлях 1cv8.exe!);
   - визначити арх-суфікс: `#ifdef _WIN64` → `"_x64"`, інакше `"_x86"`;
   - якщо в `parameters` НЕМАЄ `cmProviders` — додати `{"dir": "<каталог DLL>", "allowedProviders": [{"lib": "cm-pkcs12<суфікс>"}]}`;
   - якщо `cmProviders` Є: якщо `dir` відсутній/порожній — підставити каталог DLL; для кожного `allowedProviders[i].lib`, що НЕ закінчується на `_x86`/`_x64` — дописати суфікс поточної архітектури;
   - шлях у `dir` передавати в UTF-8 (JSON API UAPKI приймає UTF-8; конвертація через ServiceTools). УВАГА: `CmLoader` використовує `LoadLibraryA` (ANSI) — якщо шлях містить не-ASCII символи (кирилиця в імені користувача!), можлива проблема кодування. Мінімум — залогувати `REPORT_WARN`-ом, якщо шлях містить не-ASCII; ідеально — перевірити поведінку і, за потреби, використати короткий шлях `GetShortPathNameW` як обхід;
   - залогувати фінальний (інжектований) конфіг через `REPORT_DEBUG` (без секретів: поле `password`, якщо є — маскувати).
3. **Виправити `IsOperationSuccess`**: успіх — `errorCode` присутній і == 0 (або, за докою, відсутній `errorCode` при деяких методах — звірити з `docs/UAPKI_Protokol.md`, розділ про формат відповіді). При `errorCode != 0` — `REPORT_ERROR` з `errorCode` + текстом з поля `error` → `return false`.
4. Перевірити, що `json_free` викликається на КОЖНІЙ гілці після успішного `process()` (включно з помилковими), без витоків.

### Крок E. Верифікація (обов'язкова, виконати ВСЮ)

1. **Повна збірка**: `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI` → exit 0, без помилок компіляції/лінкування (попередження C4005 про перевизначення `CURL_STATICLIB` — відоме, можна ігнорувати або прибрати дубль дефайна).
2. **Вміст ZIP**: рівно 5 файлів (2 основні DLL, 2 провайдери, manifest.xml).
3. **Експорти основної DLL** (`dumpbin /exports bin\Release\SimplyAddinConnectWin_x64.dll`, аналогічно x86): РІВНО 3 символи — `GetClassObject`, `DestroyObject`, `GetClassNames` (як в `src/core/AddInNative.def`). Якщо для smoke-тесту потрібні `process`/`json_free` — ДОЗВОЛЕНО додати їх у `.def` (сумарно 5 експортів), зафіксувати це рішення у звіті.
4. **Експорти провайдера** (`dumpbin /exports bin\Release\cm-pkcs12_x64.dll`): присутні `provider_info`, `provider_init`, `provider_deinit`, `provider_open`, `provider_close`, `block_free`, `bytearray_free`; ВІДСУТНІ масові uapkic-символи (aes_*, dstu4145_* тощо).
5. **Імпорти провайдера** (`dumpbin /imports`): ЛИШЕ системні DLL (kernel32, bcrypt, crypt32, ws2_32, msvcrt-сімейства НЕ має бути при /MT); НЕ має бути `uapkic.dll`/`uapkif.dll` — це доказ самодостатності.
6. **Runtime smoke-тест провайдера** (python ctypes, x64):
   ```python
   import ctypes, json
   p = ctypes.CDLL(r"bin\Release\cm-pkcs12_x64.dll")
   p.provider_info.restype = ctypes.c_void_p
   info = ctypes.cast(p.provider_info(), ctypes.c_char_p).value
   print(info)  # очікується JSON з описом провайдера
   ```
   (сигнатуру `provider_info` звірити з `extern/uapki/library/common/cm-api/cm-export.h` — якщо повертає інший тип/приймає аргументи, скоригувати тест.)
7. **Runtime smoke-тест ядра** (якщо додали `process`/`json_free` у .def): ctypes → `process('{"method":"VERSION"}')` → відповідь з версією; потім `process('{"method":"INIT","parameters":{"cmProviders":{"dir":"<abs шлях bin\\Release>","allowedProviders":[{"lib":"cm-pkcs12_x64"}]}}}')` → `errorCode==0`; потім `{"method":"PROVIDERS"}` → у відповіді видно PKCS12-провайдер. Це доводить весь ланцюг "ядро бачить і вантажить наш провайдер".
8. **Чистота робочого дерева**: `git status` — жодних незакоммічених правок у сабмодулі (все закомічено у форк на кроці A); в основному репо — тільки очікувані зміни (+ `version.h` від збірки).
9. **Регресія без UAPKI**: `build_project.ps1` (без прапорців) → збирається, ZIP з 2 DLL, компонента ECRPrivatJSON не зачеплена.

## 6. Відомі граблі (щоб не наступити)

- **НЕ додавати `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS`** ні до якої цілі — витягне тисячі статичних символів.
- Дублікати сирців між статичними цілями (`ba-utils.c`, `parson*.c`, `uapki-ns-util.cpp`, `asn1/*.c` — див. `uapki_full_static.cmake:110` коментарі агента: цілі `asn1`+`uapkif`, `ba-utils`+`cm-pkcs12`, `parson`+`cm-pkcs12`+`uapki`) — зараз лінкуються без LNK2005, бо MSVC бере перший символ; при перебудові цілей стежити, щоб у ФІНАЛЬНУ лінковку (основної DLL і провайдера) один об'єкт не приходив із двох .lib двічі. Якщо вилізе LNK2005 — прибирати дублікати зі складу цілей, а не глушити `/FORCE`.
- Для WIN32 у `uapki_full_static.cmake:228-243` НЕ застосовується whole-archive — це ОК для нашого випадку (C-API `process` тягне все потрібне транзитивно), НЕ намагатися додавати `/WHOLEARCHIVE` (роздує DLL і може зламати лінковку).
- `Compress-Archive` у PS 5.x не додає порожні каталоги і мовчки перезаписує з `-Force` — перевірка вмісту ZIP по факту обов'язкова.
- Провайдер лінкує prebuilt `libcurl.lib`? НІ — curl потрібен лише ядру uapki (`uapki_full_static.cmake:200-213`); у провайдер curl НЕ тягнути.
- `LoadLibraryA` в `CmLoader` — ANSI; кирилічні шляхи див. крок D.2.
- Після видалення `cm-pkcs12` з бандла основна DLL має далі лінкуватись — залежностей uapki→cm-pkcs12 на рівні символів немає (зв'язок лише через LoadLibrary), але перевірити лінковку обох архітектур.

## 7. Що зафіксувати у звіті виконання

1. Список комітів (сабмодуль + основне репо) з хешами.
2. Вивід перевірок кроку E (розміри DLL, списки експортів/імпортів, результати smoke-тестів).
3. Прийняті дрібні рішення, що відхиляються від цього ТЗ, з обґрунтуванням (наприклад, чи додано `process`/`json_free` у `.def`; як саме вирішено експорт символів провайдера — CM_LIBRARY чи .def).
4. Виявлені, але НЕ виправлені проблеми (окремим списком — підуть у наступні задачі).

## 8. Поза межами цієї задачі (свідомо; НЕ робити)

- Оновлення сабмодуля до свіжого upstream main (окрема задача після стабілізації).
- PR у upstream specinfo-ua/UAPKI з `*_STATIC`-гілками export-заголовків.
- Тестування з реальною 1С (`NativeAddIn_Н.epf`) і поведінка розпакування ZIP платформою — виконується користувачем разом з оркестратором після приймання цієї задачі.
- Fallback-механізм розгортання провайдерів у `%LOCALAPPDATA%` (якщо 1С не розпакує додаткові DLL поруч) — рішення після емпіричного тесту. **➜ ВИКОНАНО** у задачі `2026-07-19_uapki_embedded_providers_and_tests.md` (емпірично підтверджено 2026-07-19, що 1С розпаковує лише 1 DLL): провайдер вбудовано RCDATA-ресурсом і розгортається самою DLL (потрійний пошук каталогу).
- Розширення API компоненти (окремий метод для сирого JSON тощо) понад описане в D.1.

---

## 9. Звіт виконання (2026-07-18)

Виконано через оркестрацію Workflow (fan-out субагентів A/B+C/D/DOC з тірингом моделей) +
особиста валідація оркестратором (збірка як гейт). Усі кроки A–E закриті.

### 9.1. Коміти

- **Сабмодуль `extern/uapki`** (гілка від `0bbc3da`, detached; upstream НЕ оновлювався):
  - `ff8c679` — «Підтримка статичної збірки: гілки `*_STATIC` в export-заголовках
    (uapkic/uapkif/uapki), guard `WIN32_LEAN_AND_MEAN` в `asn_system.h`».
  - Робоче дерево сабмодуля чисте (`git status` порожній). `git push` НЕ виконувався.
- **Головний репо:** зміни лишені в робочому дереві **незакоммічені** (згідно з п.5 E.8 —
  «тільки очікувані зміни»), очікують рев'ю користувача перед комітом. Змінені файли:
  `CMake/uapki_full_static.cmake`, `build_project.ps1`,
  `src/helpers/UAPKIConnect/UAPKIConnectHelper.{cpp,h}`,
  `src/components/AddinUAPKIConnect.cpp`, `docs/ARCHITECTURE.md`, `AGENTS.md`,
  указник сабмодуля `extern/uapki`, `version.h` (від збірки). `src/core/AddInNative.def` —
  без нетто-змін (процес/json_free додавалися тимчасово для E.7 і повернуті). Коміт/push
  головного репо — за окремим підтвердженням користувача.

### 9.2. Результати верифікації (крок E)

| # | Перевірка | Результат |
|---|-----------|-----------|
| E.1 | `build_project.ps1 -WithUAPKI`, обидві архітектури, Release | ✅ exit 0, без помилок |
| E.2 | Вміст ZIP | ✅ рівно 5: `manifest.xml`, `SimplyAddinConnectWin_x86/x64.dll`, `cm-pkcs12_x86/x64.dll` |
| E.3 | Експорти основної DLL (x64+x86) | ✅ рівно 3: `GetClassObject`, `DestroyObject`, `GetClassNames` (було ~283) |
| E.4 | Експорти провайдера (x64+x86) | ✅ рівно 7: `provider_info/init/deinit/open/close`, `block_free`, `bytearray_free`; масових uapkic-символів немає |
| E.5 | Імпорти провайдера | ✅ лише системні (`KERNEL32.dll`, `ADVAPI32.dll`); немає `uapkic.dll`/`uapkif.dll` (самодостатній) і msvcrt (`/MT`) |
| E.6 | Runtime smoke провайдера (ctypes) | ✅ `provider_info` → rc=0, валідний JSON `"id":"PKCS12"`, libVersion 1.0.13 |
| E.7 | Runtime smoke ядра (повний ланцюг) | ✅ `VERSION`→errorCode 0 (UAPKI 2.0.12); `INIT`(cmProviders→dir+`cm-pkcs12_x64`)→errorCode 0, **countCmProviders=1**; `PROVIDERS`→список містить `PKCS12`. Доведено: ядро вантажить самодостатній провайдер через `LoadLibraryA` з інжектованого каталогу |
| E.8 | Чистота дерева | ✅ сабмодуль чистий (`ff8c679`); головний репо — тільки очікувані зміни |
| E.9 | Регресія без UAPKI (`build_project.ps1`) | ✅ exit 0, ZIP з 3 файлів (2 DLL + manifest), ECRPrivatJSON не зачеплено |

### 9.3. Прийняті рішення (відхилення/уточнення ТЗ)

- **Провайдер експортує через `CM_LIBRARY`, `.def` НЕ знадобився.** Усі 7 функцій у
  `main-cm-pkcs12.cpp` позначені `CM_EXPORT` (=`__declspec(dllexport)` при `CM_LIBRARY`);
  `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` свідомо НЕ вживається.
- **Поставкова основна DLL лишається на 3 символах** (за ТЗ E.3 і чистотою експортів).
  Для E.7 `process`/`json_free` **тимчасово** додавалися в `.def` (окрема throwaway-збірка),
  прогнано smoke, потім `.def` повернено до 3 символів. У поставці `process`/`json_free`
  НЕ експортуються (компонента викликає їх через внутрішній статичний лінк).
- **Крок B фікс складу (виявлено збіркою):** провайдеру довелося додати до glob
  `cm-pkcs12/src/storage/*.cpp` + `crypto/*.cpp` (класи `FileStorage`/`StoreBag` —
  раніше не лінкувались, бо стара STATIC-ціль була dead-code); ядру `uapki` — додати
  `common/pkix/*.c` (окрім `ba-utils.c`/`uapki-errors.c`, що вже в цілі `ba-utils`), бо ці
  символи (oids, oid-utils, key-wrap, private-key, iconv-utils, aid, dstu4145-params,
  iso15946) раніше приходили в основну DLL транзитивно через стару `cm-pkcs12`.
- **Кирилиця в шляху:** `GetOwnModuleDir` при не-ASCII символах у каталозі логує
  `WARN` і намагається взяти короткий (8.3) ASCII-safe шлях через `GetShortPathNameW`
  (бо `CmLoader` вантажить через `LoadLibraryA`/ANSI).
- **Додатково виправлено (поза початковим ТЗ, на запит користувача):** catch-блоки
  `AddinUAPKIConnect.cpp` віддавали `{"status":"error"}` — приведено до формату UAPKI
  `{"errorCode":500,...}` заради консистентності.

### 9.4. Виявлені, але НЕ виправлені проблеми (на наступні задачі)

- **Колізія статичних `.lib` обох архітектур у `bin/Release`** (гігієна збірки, передіснуюча):
  усі проміжні статичні бібліотеки (`uapkic.lib`, `uapkif.lib`, `asn1.lib`, `parson.lib`
  тощо) кладуться в `bin/Release` через глобальний `LIBRARY_OUTPUT_PATH`, тож x86-збірка
  перезаписує x64-`.lib` (і навпаки). На **чисту** поставкову збірку не впливає (кожна
  архітектура лінкується одразу після своїх `.lib`, до перезапису), але **інкрементальні**
  збірки `cmake --build build_x64` після `build_x86` падають з `LNK4272` (mismatch x86/x64).
  Фікс: перенаправити `ARCHIVE_OUTPUT_DIRECTORY` статичних цілей у build-локальні каталоги,
  лишивши в `bin/Release` лише фінальні DLL. Окрема задача.
- **Формат синтетичних помилок у 1С:** приведено до `errorCode` в компоненті UAPKI (9.3),
  але інші компоненти (ECRPrivatJSON) мають власні угоди — глобальна уніфікація формату
  відповідей поза межами цієї задачі.
- **Попередження `C4005` (перевизначення `CURL_STATICLIB`)** — відоме, не критичне
  (дубль дефайна в `options.cmake` та `uapki_bundle`).
