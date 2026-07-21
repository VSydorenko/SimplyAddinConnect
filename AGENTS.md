# AGENTS.md

Настанови для coding-агентів по цьому репозиторію. Архітектура — `docs/architecture/` (по документу на підсистему).
Мова коду й комітів — українська/російська (дотримуйся мови файлу, який редагуєш).

## Проєкт

**SimplyAddinConnect** — нативна зовнішня компонента для **1С:Підприємство** (Windows, C++17).
Збирається в одну DLL (x86 і x64), що реєструє кілька компонент:
- **AddinUAPKIConnect** — ЕЦП/крипто через бібліотеку UAPKI;
- **TestComponent** — демо/приклад реєстрації.

Старий драйвер **AddinECRPrivatJSON** (платіжний термінал ПриватБанку) на гілці `device-core`
**видалено як непрацездатний** — його заміняє **фундамент device-core** у `src/transport/`
(байтовий транспорт `ITransport` → кадрування `IFramer`/`NullTerminatedFramer` → класифікація
`IFrameClassifier` → сесія запит/відповідь `DeviceSession`), основа для драйверів обладнання.
Перший драйвер на цьому фундаменті — **ECRPrivatJSON** (`src/drivers/ecr_privatjson/`) поверх
платформи-каркаса `src/platform/` (`ResultEnvelope` — уніфікований результат). Реалізовано
**обидві частини**: wire-спину (Частина 1) — кодек JSON↔байти (`EcrJsonCodec`), класифікатор кадрів
(`EcrPrivatJsonClassifier`, кореляція за `method`/`msgType`), життєвий цикл
`EcrPrivatJsonDriver::Connect` за еталонною схемою (Ping+dc→Identify+dc→постійна сесія) — і
**операції з 1С-фасадом (Частина 2)**: синхронні/асинхронні операції (`Purchase`/`Refund`/
`CheckConnection`/`GetReceiptInfo`) поверх `JobEngine` (`src/platform/` — машина асинхронного
завдання), poller статусу `getLastStatMsgCode` + `interrupt` на service-доріжці, best-effort
відновлення після desync, і **зареєстрована компонента 1С `ECRPrivatJSON`** (фасад
`AddinECRPrivatJSON` у `src/components/`, `REGISTER_COMPONENT`, делегує драйверу; **poll-based**,
без подій — стан операції читається методами `OperationState`/`СостояниеОперации`, результат —
`OperationResult`/`РезультатОперацииJSON`, статус термінала — `LastStatus`/`СтатусТерминала`;
`EnableTrace`/`ВключитьТрассировку` вмикає wire-трасування драйвера, діє з наступного `Connect`).
Уся Privat-специфіка ізольована в кодеку+класифікаторі;
`DeviceSession`/транспорти лишаються загальними. Ручний тест із реальної 1С без обладнання —
standalone-емулятор термінала `ecr_terminal_emulator`.

Репозиторій: `github.com/VSydorenko/SimplyAddinConnect`. Версія — `VERSION.txt` + `version.h`.

## Збірка

Вимоги: **Visual Studio 2022** (C++ desktop), **CMake ≥ 3.16**, ініціалізовані сабмодулі:
```powershell
git submodule update --init --recursive
```

```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 [-WithUAPKI] [-WithTests]
```
- без прапорців — основний проєкт (2 головні DLL + `manifest.xml` у ZIP);
- `-WithUAPKI` — за один прохід збирає ядро UAPKI (`uapki`+`uapkic`+`uapkif`, статично в
  головну DLL) і окремо самодостатній провайдер `cm-pkcs12_x86.dll` / `_x64.dll`; кожна головна
  DLL додатково вбудовує РЕСУРСОМ (RCDATA) провайдер своєї архітектури й розгортає його сама
  при `INIT` (потрійний пошук каталогу — див. `docs/architecture/uapki.md`); у підсумковий ZIP
  потрапляють обидва варіанти — разом 5 файлів (див. `docs/architecture/uapki.md`, `build-and-packaging.md`);
- `-WithTests` — збирає тестові консольні exe з `tests/`. **`core_selftest`, `wire_selftest`,
  `ecr_privatjson_selftest`, `ecr_terminal_emulator` і `ecr_native_host` збираються завжди при
  `-WithTests`** (ядрові/ECR-харнеси без UAPKI); а `uapki_selftest`/`native_host` — **лише разом з
  `-WithUAPKI`** (залежать від крипто-ядра; без UAPKI ці дві цілі тихо пропущено, тека `tests/` на
  Windows конфігурується завжди).

Скрипт перегенеровує `version.h` (інкремент build), очищає `build_x86/`, `build_x64/`,
`bin/Release/`, збирає обидві архітектури в Release і пакує в `bin/Release/SimplyAddinConnectWin.zip`.
Опції CMake: `-DBUILD_WITH_UAPKI=ON|OFF`, `-DBUILD_TESTS=ON|OFF` (обидві default OFF).

## Тести

Тека `tests/` **існує**: `tests/CMakeLists.txt` (окремі консольні exe, лінкуються НЕ в головну
DLL і НЕ підключають `src/core/pch.h`), `tests/core_selftest.cpp` (харнес ядра),
`tests/wire_selftest.cpp` (харнес device-ядра), `tests/ecr_privatjson_selftest.cpp` +
`tests/support/TerminalEmulator.{h,cpp}` (харнес пілотного драйвера ECRPrivatJSON + протокол-обізнаний
TCP-емулятор термінала), `tests/scenarios/*.json`
(7 сценаріїв L1), `tests/data/` (тестовий контейнер `test-diia.p12`, сертифікати, CRL — read-only вхід).

Сім цілей (лише Windows; окремі цілі мають власні умови):
- **`core_selftest.exe`** (L0.5) — **збирається завжди при `BUILD_TESTS=ON`, без UAPKI**. Лінкує
  OBJECT-бібліотеки ядра (`base_component`+`helpers_component`) напряму й ганяє перевірки ядра
  `AddInNative` через мок платформи 1С (`MockConnect : IAddInDefBase`, `MockMemory : IMemoryManager`):
  реєстр компонент, життєвий цикл, `Ret()`, `REGISTER_COMPONENT`, базовий `EnableLogging`, валідація
  `ParamSpec`, потокобезпечний `PostExternalEvent`, захист індексів, `ShutdownLogging` без дедлоку.
- **`wire_selftest.exe`** (L0.6) — **збирається завжди при `BUILD_TESTS=ON`, без UAPKI** (лінкує
  `wire_component`+`transport_component`+`helpers`+`base`). Ганяє device-facing ядро без обладнання:
  байтові вектори `NullTerminatedFramer`, `IFrameClassifier`-подвійники, `DeviceSession` над
  детермінованим `LoopbackTransport` (happy/timeout/desync/reconnect/reentrancy/precedence/
  wire-trace, §14-інваріанти) + смоук реального `TransportTCP` (localhost-echo). Задокументовані
  `[SKIP]`-рядки (напр. `ComRoundtrip` — потрібна пара com0com, ручний смоук) — це НЕ FAIL: гейт
  дивиться лише exit-код 0.
- **`ecr_privatjson_selftest.exe`** (L0.7) — **збирається завжди при `BUILD_TESTS=ON`, без UAPKI**
  (лінкує `ecr_facade_component`+`driver_ecr_privatjson_component`+`platform_component`+`transport`+
  `wire`+`helpers`+`base` + `support/TerminalEmulator.cpp`). Ганяє пілотний драйвер ECRPrivatJSON
  без обладнання: кодек (`BuildRequest`/`Parse`/`PeekMethod`), класифікатор кадрів (§5 — усі гілки),
  transport-e2e поверх `DeviceSession`+`TransportTCP` проти `TerminalEmulator` (Winsock-сервер на
  localhost, скриптовані відповіді за `method`) — `Connect` за еталонною схемою, розбір рядка
  підключення, збереження vendor/model; **Частина 2:** `JobEngine` (Idle→Running→Done/Error,
  повторний Start→false, виняток→Error), синхронні операції `Purchase`/`Refund` + `MapResult`,
  poller `getLastStatMsgCode` під час операції, `interrupt` (скасування), асинхронний API
  (`StartPurchase`/`OperationState`/`TryGetOperationResult`) і смоук 1С-фасаду `ECRPrivatJSON` через
  `AddInNative::CreateObject` (реєстрація методів). Критерій — exit-код 0 (усі CHECK — PASS).
- **`ecr_terminal_emulator.exe`** (ручний інструмент, не рівень гейта) — **збирається завжди при
  `BUILD_TESTS=ON`, без UAPKI**. Standalone-EXE протокол-обізнаного TCP-емулятора термінала
  (`ecr_terminal_emulator[_x64].exe [port]`, default 2000): слухає localhost і віддає скриптовані
  JSON-відповіді (`PingDevice`/`ServiceMessage`/`Purchase`/`Refund`/`GetReceiptInfo`/…), доки не
  Ctrl-C. Призначення — тест компоненти `ECRPrivatJSON` з **реальної 1С без обладнання**.
- **`ecr_native_host.exe`** (L2-ecr) — **збирається завжди при `BUILD_TESTS=ON`, без UAPKI**. Емулює
  платформу 1С: вантажить головну DLL через `LoadLibraryW`+`GetClassObject`, створює компоненту
  **`ECRPrivatJSON`** через `IComponentBase`, викликає `Подключить(tcp://127.0.0.1:<port>)`+`Оплата`
  проти in-process `TerminalEmulator` і звіряє результат (маршалінг `tVariant`). Exit 0 = OK.
- **`uapki_selftest.exe`** (L1, лише при `BUILD_WITH_UAPKI=ON`) — лінкує `uapki_bundle` напряму
  (без 1С, без завантаження DLL) і проганяє JSON-сценарії з `tests/scenarios/` через
  `process()`/`json_free()` статичного ядра.
- **`native_host.exe`** (L2/L3, лише при `BUILD_WITH_UAPKI=ON`) — емулює платформу 1С: вантажить
  головну DLL через `LoadLibraryW` і викликає компоненту `AddinUAPKIConnect` через
  `IComponentBase`/`CallUapki`, e2e-кейси 1-4 і крос-валідація ПРРО (кейс 5, потребує
  `PRRO_DOCS_DIR`, інакше SKIP).

Запуск: `build_project.ps1 -WithUAPKI -WithTests`, потім
`powershell -File run_tests.ps1 [x64|x86] [-NoUapki]` (оркестратор: L0 dumpbin-інваріанти →
L0.5 core_selftest ядра → L0.6 wire_selftest device-ядра → L0.7 ecr_privatjson_selftest драйвера →
L2-ecr ecr_native_host компоненти ECRPrivatJSON через DLL → L1 selftest по сценаріях →
L2/L3 native_host; підсумкова таблиця PASS/FAIL/SKIP/BLOCKED, ненульовий exit при провалі).
L0.5, L0.6, L0.7 і L2-ecr проходять і без `-WithUAPKI`.

**Режим без UAPKI (`-NoUapki`):** ганяє ядрові/ECR-рівні (L0.5 core + L0.6 wire + L0.7 ecr +
L2-ecr ecr_native_host) — збирає з `-DBUILD_TESTS=ON` **без** `-DBUILD_WITH_UAPKI=ON`; провайдер
(L0.2), L1 та L2/L3 native_host → SKIP (не FAIL). L0.1 (рівно 3 експорти головної DLL) і L2-ecr
(потребує головну DLL + `ecr_native_host.exe`) лишаються активними. Швидкий гейт device-ядра+ECR
без важкої статичної збірки крипто-стеку: `powershell -File run_tests.ps1 -NoUapki [x64|x86]`.

Тестові exe лягають у `bin/Release` (через `EXECUTABLE_OUTPUT_PATH`, `output_settings.cmake`) —
саме там їх шукає `run_tests.ps1`.

> Історична примітка: раніше `CMake/dependencies.cmake` містив
> `set(BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)` (нібито для ixwebsocket) — але ixwebsocket
> цю змінну не читає, тож рядок лише FORCE-затирав однойменну опцію проєкту й ламав
> `-DBUILD_TESTS=ON`. Рядок прибрано; `-WithTests` тепер справді збирає `tests/`.

## Структура

```
CMake/              # модульна збірка; components.cmake — джерело правди щодо складу DLL
src/core/           # ядро AddInNative (міст до SDK 1С) + pch.h
src/components/     # компоненти-фасади для 1С
src/helpers/        # ServiceTools (логування/конвертації) + хелпери фіч
src/transport/      # канали COM/TCP/WS-client + device-core (IFramer/NullTerminatedFramer/
                    #   IFrameClassifier/DeviceSession — фундамент драйверів обладнання)
src/platform/       # платформа-каркас драйверів (ResultEnvelope — уніфікований результат операції;
                    #   JobEngine — машина асинхронного завдання)
src/drivers/        # драйвери обладнання поверх device-core; ecr_privatjson/ — пілотний
                    #   ECRPrivatJSON (EcrJsonCodec/EcrPrivatJsonClassifier/EcrPrivatJsonDriver)
include/            # заголовки SDK 1С
tests/              # core_selftest (L0.5) + wire_selftest (L0.6) + ecr_privatjson_selftest (L0.7,
                    #   +support/TerminalEmulator) + ecr_native_host (L2-ecr, компонента через DLL) +
                    #   ecr_terminal_emulator (standalone EXE для 1С) + uapki_selftest (L1) +
                    #   native_host (L2/L3) + scenarios/ + data/
docs/architecture/    # архітектура по підсистемах (README + 01..04)
docs/               # специфікації протоколів (ECR/UAPKI), tasks/
extern/             # сабмодулі: spdlog, nlohmann_json, ixwebsocket, uapki
```

## Конвенції коду (обов'язкові)

**PCH.** `src/core/pch.h` підключається першим рядком у **кожному `.cpp`**
(`#include "../core/pch.h"`). У `.h` — **ніколи**; там лише потрібні заголовки / forward declaration.

**Логування — лише через макроси** з `ServiceTools.h` (прямий `spdlog` заборонено):
- `REPORT_*` — у нестатичних методах компоненти (`TRACE/DEBUG/INFO/WARN/ERROR`);
- `NEUTRAL_REPORT_*` — у статичних/const-методах; **1-й аргумент — ім'я компоненти (рядок)**;
- `REPORT_ERROR` автоматично реєструє помилку для 1С.

**Повідомлення** складай заздалегідь конкатенацією (`"Текст: " + var + std::to_string(n)`),
з великої літери, без крапки в кінці. printf-стиль у макросах **заборонено**.

**Помилки:** після `REPORT_ERROR` — зазвичай `return false`; не кидай `throw` для бізнес-помилок,
які треба показати в 1С; код, що може кинути виняток, обгортай `try-catch` з `REPORT_ERROR` у `catch`.

**Конвертації рядків:** `ServiceTools::SafeMB2WCHAR` / `SafeWCHAR2MB` (не прямі `AddInNative::*`).

## Як додати компоненту

1. `src/components/Моя.{h,cpp}` — успадкуй `AddInNative`, у `.h` оголоси
   `static std::vector<std::u16string> names;`, а реєстрацію в `.cpp` (на файловому рівні) зроби
   макросом `REGISTER_COMPONENT` (він і визначає `names`, і додає анти-стрип reference проти
   відкидання лінкером). Методи опиши в `RegisterMethods()` (лямбди з аргументами `VH`):
   ```cpp
   REGISTER_COMPONENT(u"Моя", Моя)                 // src/components/Моя.cpp, файловий рівень
   Моя::Моя() { REPORT_INFO("Ініціалізація Моя"); RegisterMethods(); }
   ```
   (Для компоненти з кількома іменами макрос не підходить — залиш ручний `names = { AddComponent(...),
   ... }` і **обов'язково** додай `namespace { [[maybe_unused]] auto& _force = Моя::names; }`.)

   Конвенції методів:
   - **Повернення значення в 1С — через `Ret(...)`**: `AddFunction(u"F", u"Ф", Ret([](VH a){ return ...; }))`.
     `MethFunction` — це `std::function<void(...)>`, тож значення, повернуте «голою» лямбдою, мовчки
     відкидається; `Ret()` загортає value-лямбду у хендлер, що присвоює `this->result`.
     **Правило:** обгортай `Ret()` ЛИШЕ хендлери, чиє `return`-значення і є результатом. **НЕ обгортай**
     хендлери, які самі ставлять `this->result` (як `CallUapki`), а повертають службовий `bool` —
     `Ret()` перезаписав би корисний результат.
   - **Параметри — декларативно через `ParamSpec`**: перевантаження `AddFunction`/`AddProcedure` з
     `const std::vector<ParamSpec>&` описує `required`/`byDefault` (`DefaultHelper`); обов'язкові без
     дефолту валідуються ДО виклику хендлера — при порожньому аргументі ядро само робить `AddError`
     з ім'ям параметра й повертає `false`.
   - **`EnableLogging`/`ИспользоватьЛогирование` реєструвати не треба** — метод успадкований з базового
     `AddInNative` (делегат у `ServiceTools::EnableComponentLogging`); у деструкторі похідного —
     `ServiceTools::DisableComponentLogging(this)`.
2. У `CMake/components.cmake`: додай файли до `HEADER_FILES`/`SOURCE_FILES`, за потреби окрему
   `add_library(... OBJECT ...)` з include-шляхами й `add_dependencies` (мінімум `base_component spdlog`),
   і `$<TARGET_OBJECTS:...>` до фінальної SHARED-цілі.
3. Перевір збірку `build_project.ps1` (+`-WithUAPKI`, якщо залежить від UAPKI).

Детальніше про модель ядра (VariantHelper, реєстрація, життєвий цикл, платформенні механізми
Етапу 0) — `docs/architecture/core.md`.

## Git-нюанси

- Правки в сабмодулі `extern/uapki` (потрібні для статичної збірки) комітяться **всередині
  сабмодуля**, не з кореня — не загуби їх при `submodule update`.
- `version.h` перегенеровується скриптом при кожній збірці — очікуваний diff.
- У `.gitignore`: `bin/`, `build_*/` та інші build-теки (`/build`, `/build64Lin` тощо), `.vscode/`,
  `tmp/`, `manifest.xml`, `*.epf`, об'єктні файли (`*.o`, `*.d`, `*.so`).
