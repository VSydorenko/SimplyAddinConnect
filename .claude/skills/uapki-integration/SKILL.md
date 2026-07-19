---
name: uapki-integration
description: Use when integrating or calling the UAPKI Ukrainian crypto library (specinfo-ua) from SimplyAddinConnect — signing, verification, key storages, or provider deployment. Triggers on "UAPKI", "ЕЦП", "КЕП", "cm-pkcs12", "cmProviders", "DSTU 4145", "digital signature Ukraine", "process json_free".
---

# Інтеграція UAPKI

UAPKI (specinfo-ua) — українська бібліотека криптографії й PKI (ЕЦП/КЕП, ДСТУ 4145,
ДСТУ 7624/7564 тощо). У SimplyAddinConnect вона вбудована так, щоб головна DLL
компоненти 1С самодостатньо підписувала й перевіряла дані без зовнішніх залежностей.

Ключова ідея: **ядро UAPKI лінкується статично, а робота з особистими ключами
йде через окремий провайдер-DLL, який вантажиться в рантаймі**. Це не архітектурна
примха — це пряма вимога автора бібліотеки (див. нижче).

## Архітектура шарів

Три рівні бібліотеки:
- `uapkic` — примітиви криптографії (хеші, шифри, ЕЦП-алгоритми).
- `uapkif` — ASN.1-формати (сертифікати, CMS/CAdES, контейнери).
- `uapki` — високорівневий JSON-API поверх них.

У SimplyAddinConnect ці три (+ допоміжні `asn1`/`ba-utils`/`byte-array`/`parson`/
`stacktrace`/`dirent-internal`) збираються статично в об'єднану ціль
`uapki_full_static` → інтерфейсну `uapki_bundle`, яка лінкується в головну DLL
(`CMake/uapki_full_static.cmake`, `CMake/components.cmake`; вмикається опцією
`BUILD_WITH_UAPKI`). Дефайни `*_STATIC` (`UAPKI_STATIC`, `UAPKIF_STATIC`,
`UAPKIC_STATIC`, `BA_STATIC`, `ASN1_STATIC`) вимикають `dllimport`.

### Чому провайдер — завжди окрема DLL

Робота з особистими ключами в UAPKI навмисно винесена в **провайдери НКІ** (носіїв
ключової інформації), які підключаються за схемою «каталог провайдерів». У проєкті
це `cm-pkcs12` — провайдер для файлових контейнерів PKCS#12 (`.p12`/`.pfx`).

Загальне правило UAPKI: ядро саме собою підписати особистим ключем НЕ може — йому
потрібен окремий `cm-*` провайдер. Автор бібліотеки прямо заявив, що «для роботи з
особистими ключами обов'язкові зовнішні бібліотеки `cm-*`, без суттєвої переробки
інакше не вийде», і що механізм має бути саме «каталог провайдерів», а не хардкод
одного файлу — бо провайдерів планується щонайменше два: `cm-pkcs12` (файли) і
`cm-pkcs11` (апаратні токени).

Приклад SimplyAddinConnect: `cm-pkcs12` збирається як **окрема самодостатня SHARED
DLL** з арх-суфіксом імені виходу — `cm-pkcs12_x64.dll` / `cm-pkcs12_x86.dll`
(`CMake/uapki_full_static.cmake`, `OUTPUT_NAME`). Статичні `uapkic`/`uapkif`/`asn1`/
`parson`/`ba-utils`/`byte-array` лінкуються **всередину провайдера**, тож він не має
символьних залежностей від головної DLL. З системних — лише `bcrypt`/`crypt32`/
`ws2_32`. Провайдер експортує рівно 7 обов'язкових CM-API символів (див. нижче),
БЕЗ `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` і без `.def` — інакше витягнуло б тисячі
зайвих символів.

## JSON-API: `process()` / `json_free()`

Уся взаємодія з ядром — через дві C-функції:

```c
extern "C" {
    char* process(const char* request);  // приймає JSON-запит, повертає JSON-відповідь (UTF-8, null-terminated)
    void  json_free(char* json);          // звільняє пам'ять, яку виділив process()
}
```

Правила (докладніше — `docs/UAPKI_Protokol.md`, Таблиця 3):
- `process()` повертає нуль-термінований JSON у UTF-8.
- Пам'ять цього результату **завжди звільняється через `json_free()`** — не `free()`,
  не `delete`.
- **Спершу копіюй відповідь у власний буфер, потім одразу звільняй** — це гарантує
  відсутність витоків на всіх гілках подальшого аналізу.

Приклад SimplyAddinConnect (`src/helpers/UAPKIConnect/UAPKIConnectHelper.cpp`):
`char* response = ::process(requestStr.c_str());` → копія в `responseJson` →
`::json_free(response);` (виклик і звільнення стоять поряд, до будь-якого аналізу
копії).

### Формат запиту/відповіді

Запит: `{ "method": "<METHOD>", "parameters": { ... } }`.
Відповідь: `{ "errorCode": <int>, "method": "<METHOD>", "result": { ... }, "error": "<опис>" }`
(`errorCode` == 0 — успіх; `error` присутнє лише за помилки).

Успіх операції визначається **лише за `errorCode == 0`** (не за наявністю `result`).
`docs/UAPKI_Protokol.md` фіксує `errorCode` як обов'язкове ціле поле відповіді;
коди помилок — у «Додатку А» протоколу.

## Потік методів: INIT → OPEN → SELECT_KEY → SIGN → VERIFY → CLOSE → DEINIT

Повний перелік — 34 методи (`docs/UAPKI_Protokol.md`, Таблиця 2). Типовий цикл
підпису й перевірки:

1. **INIT** — ініціалізація ядра. Тут передається конфіг провайдерів `cmProviders`
   (див. нижче) і, за потреби, офлайн-режим.
   ```json
   { "method": "INIT",
     "parameters": {
       "offline": true,
       "cmProviders": { "dir": "./", "allowedProviders": [ { "lib": "cm-pkcs12_x64" } ] } } }
   ```
2. **OPEN** — відкрити сховище ключів (контейнер `.p12`).
   ```json
   { "method": "OPEN",
     "parameters": { "provider": "PKCS12", "storage": "<шлях до .p12>",
                     "password": "<пароль>" } }
   ```
3. **SELECT_KEY** — обрати активний ключ у сховищі за ідентифікатором.
   ```json
   { "method": "SELECT_KEY", "parameters": { "id": "<KEY_ID hex>" } }
   ```
4. **SIGN** — підписати дані/хеш активним ключем.
   ```json
   { "method": "SIGN",
     "parameters": { "signParams": { "signatureFormat": "CAdES-BES" },
                     "dataTbs": [ { "bytes": "<base64>" } ],
                     "options": { "ignoreCertStatus": true } } }
   ```
5. **VERIFY** — перевірити підпис. Холістичний вердикт — у
   `signatureInfos[0].status` (`"TOTAL-VALID"` == валідно), а НЕ у `statusSignature`
   (останній лишається `"VALID"` навіть при пошкодженому detached-контенті).
6. **CLOSE** — закрити сховище.
7. **DEINIT** — деініціалізація ядра.

INIT/DEINIT/OPEN/CLOSE/SELECT_KEY — однопотокові («ОП»); SIGN/VERIFY —
багатопотокові («БП»). Точні поля кожного методу — `docs/UAPKI_Protokol.md`.

У SimplyAddinConnect усі методи проходять єдиною точкою входу
`AddinUAPKIConnect::CallUapki` → `UAPKIConnectHelper::ExecuteUapkiCommand`, яка й
будує `{method, parameters}` та кличе `process()`. Спеціально обробляється лише
`INIT` (нечутливо до регістру): у нього автоматично інʼєктується `cmProviders`,
а після виклику — звіряється, чи справді завантажились провайдери.

## Контракт `cmProviders`: `dir` + `allowedProviders[].lib`

INIT читає провайдери з:
```json
"cmProviders": {
  "dir": "<каталог, де лежать провайдер-DLL>",
  "allowedProviders": [ { "lib": "<ім'я без префікса й розширення>", "config": { } } ]
}
```
На боці ядра це розбирає `setup_cm_providers`
(`extern/uapki/library/uapki/src/api/library-init.cpp`): бере `dir`, ітерує
`allowedProviders`, для кожного бере `lib` і викликає завантаження.

Формування імені файлу: `CmLoader::getLibName` = `LIBNAME_PREFIX + lib + "." +
LIBNAME_EXT` (на Windows префікс порожній, розширення `dll`), повний шлях =
`dir + ім'я` (`extern/uapki/library/common/loaders/cm-loader.cpp`). Тобто
`lib: "cm-pkcs12_x64"` + `dir: "./"` → `./cm-pkcs12_x64.dll`.

7 обов'язкових символів провайдера (перевіряються в `CmLoader::load`):
`provider_info`, `provider_init`, `provider_deinit`, `provider_open`,
`provider_close`, `block_free`, `bytearray_free`. Якщо бракує хоч одного —
провайдер вивантажується. `provider_list_storages`, `provider_storage_info`,
`provider_format` — опціональні.

### Автоінʼєкція в SimplyAddinConnect

Хелпер сам заповнює `cmProviders` в INIT, якщо викликач цього не зробив, і
дописує арх-суфікс `_x64`/`_x86` до `lib`, у якого його ще немає
(`UAPKIConnectHelper::InjectProviderConfig`). Якщо викликач задав `dir` явно й
непорожнім — його поважають як є, розгортання ресурсу не виконують.

## Офлайн-режими

Для роботи без мережі (немає доступу до OCSP/CRL/TSP):
- `"offline": true` у параметрах **INIT** — вимикає онлайн-перевірки статусу.
- `"options": { "ignoreCertStatus": true }` у **SIGN** — не звіряти статус
  сертифіката (потрібно, коли тестовий/прострочений ланцюг).
- **VERIFY у режимі STRUCT** (без `validationType`) — чисто структурна офлайн-
  перевірка підпису без звернень у мережу.
- Якщо запросити онлайн-формат в офлайні (напр. CAdES-**T**, якому потрібен TSP),
  ядро поверне помилку `RET_UAPKI_OFFLINE_MODE` (код 4120) — це очікувана поведінка,
  а не збій.

Приклади налаштувань — у тестових сценаріях `tests/scenarios/*.json`.

## Граблі статичного вбудовування

Це найтонше місце. UAPKI НЕ розрахований на «одну статичну DLL з усім усередині»:

1. **Немає статичного реєстру провайдерів.** Провайдер підключається ТІЛЬКИ через
   рантаймний `LoadLibrary` за шляхом із `cmProviders.dir`. Не можна злінкувати
   `cm-pkcs12` статично й очікувати, що ядро його «побачить» — реєстрація йде через
   завантаження DLL і резолв 7 символів. Тому провайдер завжди лишається окремим
   файлом-DLL.

2. **`LoadLibraryW` по шляху.** На Windows завантаження йде через
   `dl_load_library_utf8` → `MultiByteToWideChar(CP_UTF8)` → `LoadLibraryW`
   (`extern/uapki/library/common/loaders/dl-macros.h`). UTF-8 → UTF-16 конверсія
   потрібна, щоб працювали кириличні шляхи незалежно від активної ANSI-кодової
   сторінки. Канонічний патерн від авторів: розпакувати провайдери у свій каталог і
   вантажити по повному шляху (як роблять для JAR); на Windows допомагає
   `SetDllDirectory`. Лоадер кешує вже завантажені бібліотеки.

3. **Арх-колізія імен.** x86 і x64 провайдери НЕ можуть мати однакове ім'я в
   плоскому каталозі/ZIP — звідси обов'язковий арх-суфікс (`cm-pkcs12_x64` /
   `cm-pkcs12_x86`).

4. **Колізія однойменних символів.** Автор бібліотеки окремо попереджав про
   конфлікти функцій з однаковими іменами з різних бібліотек у одному процесі —
   тому провайдер експортує лише свої 7 CM-символів, а головна DLL 1С — лише свої
   3 (`GetClassObject`/`DestroyObject`/`GetClassNames`). Чисті експорти = менше
   шансів на зіткнення в адресному просторі `1cv8.exe`.

5. **Мовчазний збій завантаження.** `setup_cm_providers` ігнорує код повернення
   `CmProviders::loadProvider(...)` (явний `(void)`-каст) і **завжди повертає
   `RET_OK`** (`library-init.cpp`). Тобто INIT віддасть `errorCode: 0` навіть якщо
   жоден провайдер не завантажився. Єдиний надійний індикатор — поле
   `result.countCmProviders` у відповіді INIT: його треба звіряти з очікуваною
   кількістю. Приклад: `UAPKIConnectHelper::WarnIfProvidersNotLoaded` логує WARN
   при недоборі (не змінюючи ні відповідь, ні код).

## Патерн самодоставки провайдера ресурсом

Проблема, що спричинила цей патерн у SimplyAddinConnect: 1С НЕ розпаковує додаткові
DLL із ZIP-компоненти — у робочому каталозі лишаються лише файли з `manifest.xml`.
Тому провайдер «поруч» просто не знаходився, і INIT повертав `errorCode:0` з
`countCmProviders:0` (той самий мовчазний збій).

Рішення — головна DLL **несе провайдер у собі як RCDATA-ресурс** і розгортає його
в рантаймі. При збірці `.rc` генерується через `file(GENERATE)` з рядком
`CM_PKCS12_PROVIDER RCDATA "<шлях до cm-pkcs12-provider.dll>"`
(`CMake/uapki_full_static.cmake`); ім'я ресурсу — `CM_PKCS12_PROVIDER`
(`src/helpers/UAPKIConnect/UAPKIProviderResource.h`).

Потрійний пошук каталогу провайдера (`ResolveProviderDir`):
1. Явний непорожній `cmProviders.dir` від викликача — як є.
2. `cm-pkcs12_<arch>.dll` поруч із власною DLL.
3. Розгортання вбудованого ресурсу в
   `%LOCALAPPDATA%\SimplyAddinConnect\providers\<VERSION_FULL>\` — атомарно: запис
   у `<файл>.tmp_<PID>` + `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`.

Критична деталь: каталог власної DLL визначається через
`GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)` на адресі функції-якоря
`ModuleAnchor` усередині свого модуля — а НЕ через `GetModuleHandle(NULL)`, бо
хост-процес 1С інакше повернув би шлях до `1cv8.exe`, а не до нашої DLL.

## Настанови розробника ядра (Vitalii, specinfo-ua)

Практичні правила від автора бібліотеки — дотримуйся їх:

- **INIT робимо раз.** Не викликай INIT/DEINIT на кожну операцію — ініціалізація
  глобальна.
- **Активним може бути лише один ключ.** `SELECT_KEY` глобальний; попри
  багатопотоковість ядра (з 2023-09), паралельний підпис РІЗНИМИ ключами
  неможливий. Хочеш інший ключ — спершу переобери його.
- **Атомарні PR / малі зміни.** Тримай зміни інтеграції зернистими й перевірними
  окремо (провайдер, ресурс, конфіг — різними кроками), щоб регресію можна було
  локалізувати.
- **Сертифікація на бінарники, не на код.** Експертний висновок Держспецзв'язку
  видано на конкретні БІНАРНИКИ; самозбірка з сирців у будь-якому разі втрачає
  сертифікаційний статус (хеш не збігається). У сертифікованих релізах є відомі
  баги — розробник радить збирати з сирців. Чи потрібна додаткова сертифікація —
  залежить від вимог кінцевого продукту.

## Джерела

- `docs/architecture/uapki.md` — детальна архітектура інтеграції в SimplyAddinConnect.
- `docs/UAPKI_Protokol.md` — офіційний протокол: таблиці методів, формат
  запиту/відповіді, коди помилок (Додаток А).
