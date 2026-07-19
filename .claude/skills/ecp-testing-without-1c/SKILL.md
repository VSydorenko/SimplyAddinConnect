---
name: ecp-testing-without-1c
description: Use when testing the crypto/digital-signature (UAPKI) stack of SimplyAddinConnect without a 1C host — "test signing without 1C", "UAPKI test", "native component test harness", "IComponentBase emulation", "offline SIGN VERIFY", "test-diia.p12".
---

# Тестування крипто/ЕЦП-стеку без хоста 1С

Мета — ганяти весь ланцюг ЕЦП (INIT → OPEN → SELECT_KEY → SIGN → VERIFY → …)
детерміновано, з CI-кодами виходу, без запуску 1С:Підприємства. Замість платформи
1С тестовий харнес або лінкує крипто-ядро статично, або сам вантажить головну DLL і
викликає компоненту через `IComponentBase` — точно так, як це робить 1С.

Архітектурний контекст стеку — `docs/architecture/03-uapki.md`.

## Піраміда рівнів L0–L3

Оркестратор описує чотири рівні (`run_tests.ps1`, шапка файлу; той самий поділ у
`tests/CMakeLists.txt`):

- **L0 — статичні інваріанти постачання** (`dumpbin`, без запуску коду): головна DLL
  експортує рівно 3 символи (контракт 1С); провайдер `cm-pkcs12` самодостатній —
  7 експортів і лише системні залежності (`bcrypt`/`crypt32`/`ws2_32`); python-ctypes
  smoke-виклик `provider_info` (SKIP, якщо python недоступний).
- **L1 — `uapki_selftest.exe`**: JSON-сценарії напряму через статично злінковане
  крипто-ядро (`tests/scenarios/*.json`). Кожен сценарій — окремий процес у власному
  тимчасовому робочому каталозі.
- **L2 — `native_host.exe`, кейси 1–4**: e2e поверх ГОЛОВНОЇ DLL через `IComponentBase`.
- **L3 — `native_host.exe`, кейс 5** (+ структурна перевірка формату підпису в кейсі 4):
  крос-валідація на еталонах ПРРО/ДФС; виконується лише за наявності еталонів.

Загальний принцип піраміди — типове ноу-хау: нижні рівні дешеві, детермінові й ловлять
регресії постачання ще до будь-якої криптографії; верхні дорожчі й ближчі до реального
хоста. Специфіка SimplyAddinConnect — саме цей набір L0–L3 і поділ «статичне ядро vs
головна DLL» між L1 і L2/L3.

## L1: як селфтест лінкує статичне ядро

`uapki_selftest` збирається з `target_link_libraries(uapki_selftest PRIVATE uapki_bundle
parson)` (`tests/CMakeLists.txt`). `uapki_bundle` — та сама інтерфейсна ціль, що тягне
об'єднане статичне крипто-ядро UAPKI у головну DLL; тут воно лінкується прямо в тестовий
exe. DLL не завантажується, 1С не потрібна.

Селфтест викликає ядро через два `extern "C"`-символи (`tests/uapki_selftest.cpp`):

```c
char* process(const char* request);   // приймає {method, parameters} як JSON
void  json_free(char* buf);           // звільняє відповідь, виділену process()
```

`main()` читає файл сценарію, знімає BOM, парсить із дозволеними `//`-коментарями,
ітерує масив `"tasks"`, для кожного task будує запит `{method, parameters}`, викликає
`process()`, перевіряє поля очікувань і звільняє відповідь через `json_free()`.

Загальне правило пам'яті (з протоколу UAPKI): буфер, повернений `process()`, ЗАВЖДИ
звільняється через `json_free()` — не `free()`.

## L2/L3: як `native_host` емулює 1С через `IComponentBase`

`native_host` крипто-ядро НЕ лінкує — усе йде через головну DLL, рівно як у продакшені.
Він відтворює платформу 1С (`tests/native_host.cpp`):

1. `LoadLibraryW(dllPath)` — вантажить головну DLL у рантаймі.
2. `GetProcAddress` на трьох експортах контракту 1С: `GetClassObject`, `DestroyObject`,
   `GetClassNames`.
3. `GetClassObject(L"AddinUAPKIConnect", &comp)` — отримує вказівник на `IComponentBase`.
4. `comp->Init()`, `comp->setMemManager()`, `comp->GetInfo()`,
   `comp->FindMethod(L"CallUapki")` — послідовність ініціалізації компоненти.
5. Виклик методу: `comp->CallAsFunc(callIdx, &ret, params, 2)` з двома параметрами
   `VTYPE_PWSTR` (ім'я UAPKI-методу + JSON-параметри) — точнісінько як 1С викликає
   `CallUapki`/`ВызватьUAPKI`.

SDK 1С (`ComponentBase.h`, `AddInDefBase.h`) підключається з `include/`. Аргументи
командного рядка беруться широкими через `CommandLineToArgvW`, щоб коректно проходили
кириличні шляхи.

CLI: `native_host <case 1..5> [mainDll] [dataDir] [binDir] [prroDir]`.

Кейси:

- **Кейс 1** — ресурсне розгортання: DLL у чистому темп-каталозі БЕЗ провайдера поруч,
  `INIT` з порожніми параметрами; очікуємо `countCmProviders==1` і появу файлу провайдера
  під `%LOCALAPPDATA%\SimplyAddinConnect\providers\`.
- **Кейс 2** — провайдер поруч із DLL: `countCmProviders==1`, розгортання не відбувається.
- **Кейс 3** — явний `cmProviders.dir` від викликача поважається без розгортання.
- **Кейс 4** — повний ланцюг `INIT→OPEN→KEYS→SELECT_KEY→SIGN→VERIFY→CLOSE→DEINIT`
  (L2 + структурна крос-перевірка формату підпису).
- **Кейс 5** — L3 крос-валідація на еталонах ДФС (SKIP, а не провал, якщо каталог
  еталонів недоступний).

## Тестовий ключ і офлайн-режими

**Ключ**: `tests/data/test-diia.p12`, пароль `testpassword`.

- ID ключа підпису: `5BC6C06EE1E00C1700E92AA7A9AD75F82D3CB7A9B66E3A98023209B24513315C`.
- ID ключа КЕП (отримувач `envelopedData`):
  `6B1B77C0D1A1B60473A98DD6D4FE5302742AEDE101DAA21F2C83A67CCDEDB782`.
- Відповідні сертифікати — у `tests/data/certs/` (імена за thumbprint).

**Офлайн-режими** (тестовий ланцюг сертифікатів прострочений, тому онлайн-валідація
неможлива):

- `INIT` з `"offline": true` — вимикає мережеві звернення ядра.
- `SIGN` з `"options": { "ignoreCertStatus": true }` — ігнорує статус (прострочений)
  сертифіката.
- `VERIFY` без `validationType` = режим `STRUCT`: чисто структурна офлайн-перевірка
  (лише `signature.bytes`/`content`, без побудови й перевірки ланцюга онлайн).
- Негативний офлайн-кейс: `CAdES-T` в offline очікувано падає з `expectError: true`
  (код `4120`, `RET_UAPKI_OFFLINE_MODE`) — часова мітка потребує мережі.

## Формат сценаріїв з асертами

Сценарій (`tests/scenarios/NN_*.json`) — JSON-об'єкт з масивом `"tasks"`; дозволені
`//`-коментарі. Кожен task:

- `method` (String) — метод ядра. Порожній або з префіксом `_` — пропускається.
- `skip` (Bool, опц.) — пропустити task.
- `comment` (String, опц.) — для логів.
- `parameters` (Object, опц.) — копіюються as-is у запит.

Поля **очікувань** (siblings до `method`, читаються ДО побудови запиту, у `parameters`
НЕ потрапляють):

- `expectError` (Bool) — очікуємо `errorCode != 0`. Default false. Будь-який метод.
- `expectCountCmProviders` (Int) — для `INIT`: `result.countCmProviders` має дорівнювати.
- `expectHashHex` (String) — для `DIGEST`: `result.bytes` (base64) декодується → hex,
  порівняння регістронезалежне.
- `expectSignatureValid` (Bool) — для `VERIFY`: вердикт підпису (див. нижче).

Кілька полів очікувань в одному task допускаються — усі перевіряються.

Приклад (скорочено, `03_sign_offline.json`):

```jsonc
{
  "comment": "L1: офлайн-підпис CAdES-BES ключем test-diia.p12.",
  "tasks": [
    { "method": "VERSION" },
    {
      "method": "INIT",
      "expectCountCmProviders": 1,
      "parameters": {
        "offline": true,
        "cmProviders": { "dir": "./", "allowedProviders": [ { "lib": "cm-pkcs12_x64" } ] },
        "certCache": { "path": "certs/" },
        "crlCache":  { "path": "crls/" }
      }
    },
    { "method": "OPEN", "parameters": { "provider": "PKCS12", "storage": "test-diia.p12",
      "password": "testpassword", "mode": "RO" } },
    { "method": "SELECT_KEY", "parameters": { "id": "5BC6C06E...315C" } }
    // SIGN ...
  ]
}
```

## Вердикт VERIFY: TOTAL-VALID

Для P7S/CAdES джерело вердикту — `result.signatureInfos[0].status`; валідно, коли
рядок дорівнює `"TOTAL-VALID"`. Це холістичний статус: він ловить пошкоджений
detached-content, коли `statusSignature` лишається `"VALID"`, а `status` стає
`"TOTAL-FAILED"`. Якщо поля `status` немає — резерв `statusSignature`. Для RAW-підпису —
`result.statusSignature` (валідно, коли починається з `"VALID"`).

Практичний висновок: не покладайся на `statusSignature` як на фінальний вердикт для
CAdES — бери `signatureInfos[0].status`.

## Крос-валідація на еталонах ДФС

Кейс 5 `native_host` проганяє `VERIFY` на файлах `*.signed` з каталогу еталонів ПРРО.
Критерій успіху: `errorCode==0`, непорожній `signatureInfos`, `statusSignature`
починається з `"VALID"` і присутній `signerCertId`. Каталог задається аргументом CLI
`prroDir` або змінною `PRRO_DOCS_DIR`; за замовчуванням `run_tests.ps1` бере
`$env:PRRO_DOCS_DIR` або `R:/github/prro_docs`, якщо він існує. Немає каталогу — SKIP,
не провал. Сенс L3 — довести, що наш стек приймає підписи, згенеровані незалежними
(еталонними) інструментами, а не лише власні.

## Як запускати

```powershell
powershell -ExecutionPolicy Bypass -File run_tests.ps1 [x64|x86]
```

- Архітектура за замовчуванням — `x64`.
- Ненульовий код виходу, якщо будь-що впало (CI-friendly).
- Якщо тестові exe відсутні, `run_tests.ps1` сам конфігурує й збирає:
  `cmake -S <root> -B build_<arch> -A <platform> -DBUILD_WITH_UAPKI=ON -DBUILD_TESTS=ON`.
- `tests/data` — read-only вхід; сценарії пишуть лише в тимчасовий каталог.

Зелений стан на дату фіксації фактів — `PASS=17` (L0 dumpbin ×4, L1 селфтест ×7,
L2/L3 native_host ×5, плюс build-етап), FAIL/SKIP=0.

## Типові пастки

- **INIT — раз на процес.** Ядро UAPKI — процесний singleton (активним може бути лише
  один ключ; `SELECT_KEY` глобальний). Тому L1 запускає КОЖЕН сценарій окремим процесом
  у власному темп-каталозі. Не намагайся зробити повторний `INIT`/кілька незалежних
  сесій в одному процесі — стан протече між тестами.
- **Прострочені сертифікати.** Тестовий ланцюг прострочений умисно; без `offline: true`
  + `ignoreCertStatus: true` валідація «правильно» провалиться на статусі сертифіката.
  Це не баг тесту — це причина офлайн-режимів.
- **`//`-коментарі в JSON.** Сценарії — НЕ строгий JSON: парсер увімкнено з дозволом
  `//`. Штатний `JSON.parse` такий файл не прочитає; для утиліт використовуй режим
  jsonc або зніми коментарі.
- **BOM.** Селфтест явно знімає BOM перед парсингом. Якщо пишеш власний зчитувач
  сценаріїв — теж зніми BOM, інакше перший токен «зіпсується».

## Джерела в коді

- `run_tests.ps1` — оркестратор, шапка з описом рівнів, логіка збірки/exit-кодів.
- `tests/CMakeLists.txt` — цілі `uapki_selftest` / `native_host`, лінкування.
- `tests/uapki_selftest.cpp` — L1: `process()`/`json_free()`, контракт формату сценарію
  (докстрінг наприкінці файлу), логіка асертів і вердикту VERIFY.
- `tests/native_host.cpp` — L2/L3: емуляція 1С через `IComponentBase`, кейси 1–5,
  `KEY_ID`, пароль.
- `tests/scenarios/*.json` — приклади сценаріїв (01–07).
- `tests/data/` — `test-diia.p12`, `certs/`, `test-fox.txt`.
- `docs/architecture/03-uapki.md` — архітектура UAPKI-стеку.
