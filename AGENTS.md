# AGENTS.md

Настанови для coding-агентів по цьому репозиторію. Архітектура — `docs/architecture/` (по документу на підсистему).
Мова коду й комітів — українська/російська (дотримуйся мови файлу, який редагуєш).

## Проєкт

**SimplyAddinConnect** — нативна зовнішня компонента для **1С:Підприємство** (Windows, C++17).
Збирається в одну DLL (x86 і x64), що реєструє кілька компонент, кожна доступна в 1С під власним
іменем:
- **AddinUAPKIConnect** — ЕЦП/крипто через бібліотеку UAPKI;
- **ECRPrivatJSON** — драйвер платіжного термінала ПриватБанк (перший драйвер обладнання);
- **LabelPrinter** — драйвер принтера етикеток (ZPL, БПО-фасад, XML) — другий драйвер обладнання,
  див. `docs/architecture/label_printer.md` / `docs/integration-1c/label_printer.md`;
- **TestComponent** — демо/приклад реєстрації.

Усі компоненти стоять на спільному ядрі-мості до SDK 1С (`src/core/AddInNative`). **Драйвери
обладнання** будуються на спільному **фундаменті device-core** (`src/transport/`: `ITransport` →
`IFramer`/`NullTerminatedFramer` → `IFrameClassifier` → `DeviceSession`) поверх платформи-каркаса
(`src/platform/`: `ResultEnvelope`, `JobEngine`). Кожен драйвер додає лише свою протокол-специфіку
(кодек + класифікатор) у `src/drivers/<name>/` і тонкий фасад-компоненту в `src/components/`.

> **Документація — джерело правди; НЕ дублюй її в цьому файлі.** Внутрішня архітектура підсистем —
> `docs/architecture/` (індекс `README.md`; фундамент драйверів — `device-core.md`; далі по документу
> на драйвер/підсистему). Прикладна інтеграція з 1С — `docs/integration-1c/` (по документу на драйвер).
> **`AGENTS.md`/`CLAUDE.md` лишай драйвер-НЕЗАЛЕЖНИМИ:** тут — стабільні конвенції збірки/коду й
> вказівники на `docs/`; конкретику драйвера (методи, коди, поля, потоки) описуй **у відповідному
> документі `docs/`** і оновлюй саме його, коли міняється код, — а не цей файл.

Репозиторій: `github.com/VSydorenko/SimplyAddinConnect`. Версія — `VERSION.txt` + `version.h`.

## Робота з «Подключаемым оборудованием» (БПО) — читай ПЕРЕД будь-яким фасадом

Драйвери обладнання підключаються в 1С через штатну підсистему БПО. Її контракт описано в
**`docs/architecture/bpo-contract.md`** — читай його першим, він джерело правди.

Три правила, кожне куплене помилкою; порушення жодного з них **не дає помилки збірки** й
ламається мовчки:

1. **Документація ІТС ВІДСТАЄ від бібліотеки.** Вона описує старіший контракт: інші (довгі)
   імена системних методів, інший рядок типу обладнання, іншу модель підключення. Драйвер,
   написаний за ІТС, конфігурацією **не викликається взагалі** — саме так сталося з
   `LabelPrinter`. **Імена, значення й сигнатури бери з КОДУ КОНФІГУРАЦІЇ**; з ІТС — лише
   формати XML і таблицю типів обладнання (там вони збіглися).
2. **Не вгадуй — питай компаньйона.** Конфігурації 1С у цьому репозиторії немає. Сесія, що
   працює з розширенням (`R:\github\SMP_SimplyConnect`), має вивантаження обох редакцій і може
   звірити сигнатуру за пів хвилини. Звіряй **позиція в позицію** й вимагай номери рядків:
   ціна помилки на одну позицію — не виняток, а тихо не ті дані в чеку.
3. **Ревізія інтерфейсу — це ВИБІР РОЗКЛАДКИ ПАРАМЕТРІВ, а не «наскільки ми сучасні».**
   `ПолучитьРевизиюИнтерфейса()` визначає, якою сигнатурою конфігурація кличе операції: для
   еквайрингу це 7 / 8 / 9 параметрів, причому на тій самій позиції стоїть різне за змістом.
   Одне ім'я методу в компоненті = одна арність, тож **на кожне сімейство сигнатур потрібен
   свій клас-фасад**; підібрати «спільний знаменник» означає свідомо втратити можливості.
   Різні редакції УНФ (ru / ua) розходяться саме тут — деталі й доказова база у §2 контракту.

## Збірка

Вимоги: **Visual Studio 2022** (C++ desktop), **CMake ≥ 3.16**, ініціалізовані сабмодулі.
Для `-WithUAPKI` додатково потрібен **Windows SDK 10.0.26100+** (прибудований libcurl у UAPKI
посилається на `volatileaccessu.lib`, якого немає в старіших SDK — див. `docs/architecture/uapki.md` §7.2):
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
  `ecr_privatjson_selftest`, `ecr_terminal_emulator`, `ecr_native_host`, `label_printer_selftest`,
  `label_printer_emulator` і `label_native_host` збираються завжди при `-WithTests`** (ядрові/ECR/
  LabelPrinter-харнеси без UAPKI); `iit_verify` теж **не** потребує UAPKI, але збирається **лише
  для x86** (нативна `EUSignCP.dll` 32-бітна); а `uapki_selftest`/`uapki_fiscal_emulator`/
  `native_host` — **лише разом з `-WithUAPKI`** (залежать від крипто-ядра; без UAPKI ці три цілі
  тихо пропущено, тека `tests/` на Windows конфігурується завжди).

Скрипт перегенеровує `version.h` (інкремент build), очищає `build_x86/`, `build_x64/`,
`bin/Release/`, збирає обидві архітектури в Release і пакує в `bin/Release/SimplyAddinConnectWin.zip`.
Опції CMake: `-DBUILD_WITH_UAPKI=ON|OFF`, `-DBUILD_TESTS=ON|OFF` (обидві default OFF).

Останнім кроком скрипт **необов'язково** перезбирає тестову зовнішню обробку 1С
`bin/Release/SimplyAddinConnect.epf` зі свіжою компонентою в макеті. Немає платформи 1С / вихідників обробки або
Конфігуратор упав — крок друкує `SKIP`/`WARNING` і **не змінює результат збірки**
(деталі — `docs/architecture/build-and-packaging.md` §2.7).

## Тести

Тека `tests/` **існує**: `tests/CMakeLists.txt` (окремі консольні exe, лінкуються НЕ в головну
DLL і НЕ підключають `src/core/pch.h`), `tests/core_selftest.cpp` (харнес ядра),
`tests/wire_selftest.cpp` (харнес device-ядра), `tests/ecr_privatjson_selftest.cpp` +
`tests/support/TerminalEmulator.{h,cpp}` (харнес пілотного драйвера ECRPrivatJSON + протокол-обізнаний
TCP-емулятор термінала), `tests/label_printer_selftest.cpp` (харнес драйвера LabelPrinter),
`tests/label_native_host.cpp` + `tests/label_printer_emulator.cpp` +
`tests/support/LabelEmulator.{h,cpp}` (компонента LabelPrinter через DLL + TCP-емулятор принтера
етикеток), `tests/uapki_fiscal_emulator.cpp` (HTTP-оракул ЕЦП — ручний тест-контур UAPKI із 1С,
опис нижче), `tests/iit_verify.cpp` + `tests/support/IitStore.{h,cpp}` (арбітр L4-iit на нативній
бібліотеці ІІТ + підготовка його сховища довіри), `tests/support/LocalKeys.{h,cpp}` (читання
`tests/data/local-keys.json` — особисті КЕП розробника поза git, зразок —
`tests/data/local-keys.example.json`), `tests/scenarios/*.json`
(7 сценаріїв L1), `tests/data/` (тестовий контейнер `test-diia.p12`, сертифікати, CRL, еталони ЦЗО
`czo/` — read-only вхід).

Тестові цілі (лише Windows; окремі exe). Детальний склад перевірок кожного драйвера — у
відповідному `docs/architecture/*` (не дублюй його тут); нижче — стабільні рівні гейта:

| Ціль | Рівень | UAPKI | Що перевіряє (коротко) |
|---|---|---|---|
| `core_selftest.exe` | L0.5 | ні | ядро `AddInNative`: реєстр, життєвий цикл, `Ret()`, `ParamSpec`, `PostExternalEvent`, захист індексів |
| `wire_selftest.exe` | L0.6 | ні | device-ядро: `NullTerminatedFramer`, класифікатор-подвійники, `DeviceSession` (happy/timeout/desync/reconnect) + смоук `TransportTCP` |
| `ecr_privatjson_selftest.exe` | L0.7 | ні | драйвер ECRPrivatJSON: кодек, класифікатор, e2e проти `TerminalEmulator`, `JobEngine`, операції, poller, interrupt, async, смоук фасаду |
| `ecr_native_host.exe` | L2-ecr | ні | компонента `ECRPrivatJSON` через головну DLL (`LoadLibraryW`+`GetClassObject`) проти `TerminalEmulator` |
| `ecr_terminal_emulator.exe` | — (ручний) | ні | standalone TCP-емулятор термінала для тесту з реальної 1С (`[port]`, default 2000) |
| `label_printer_selftest.exe` | L-p1 | ні | драйвер `LabelPrinter`: генератор ZPL, растр/штрихкоди, `LabelXml`, смоук БПО-фасаду `AddinLabelPrinter` |
| `label_native_host.exe` | L-p3 | ні | компонента `LabelPrinter` через головну DLL проти `LabelEmulator` (TCP-захоплювач ZPL) |
| `label_printer_emulator.exe` | — (ручний) | ні | standalone TCP-емулятор принтера етикеток (захоплює ZPL) для тесту з реальної 1С |
| `uapki_selftest.exe` | L1 | **так** | UAPKI-ядро: JSON-сценарії `tests/scenarios/` через `process()`/`json_free()` |
| `native_host.exe` | L2/L3 | **так** | компонента `AddinUAPKIConnect` через DLL, e2e + крос-валідація ПРРО (кейс 5 потребує `PRRO_DOCS_DIR`; шукає `*.signed` РЕКУРСИВНО, при їх відсутності віддає **exit 3 = SKIP**, а не PASS) |
| `uapki_fiscal_emulator.exe` | — (ручний) | **так** | HTTP-оракул ЕЦП (грає сервер ДПС/ЄВПЕЗ) для тесту UAPKI з реальної 1С: VERIFY вхідного CMS + підписана квитанція + еталони (`--self-test` — вбудовані перевірки без 1С) |
| `iit_verify_x86.exe` | L4-iit | ні (**лише x86**) | незалежний арбітр підпису на нативній `EUSignCP.dll` АТ «ІІТ»: один файл → один рядок JSON, exit `0`=VALID / `1`=INVALID / `2`=помилка / `3`=SKIP (арбітр недоступний). Деталі — `docs/architecture/uapki.md` §8.4 |

Цілі без UAPKI (`core`/`wire`/`ecr_*`, а також `iit_verify` — але той лише в x86) збираються
завжди при `BUILD_TESTS=ON`; `uapki_selftest`/`uapki_fiscal_emulator`/`native_host` — лише разом
з `-WithUAPKI` (без UAPKI тихо пропущені).
`[SKIP]`-рядки (напр. `ComRoundtrip` без пари com0com) — НЕ FAIL: гейт дивиться лише exit-код 0.
Виняток — `native_host` **кейс 5**: щоб відсутність вхідних еталонів не зараховувалась як покриття,
він віддає окремий **exit 3 (SKIPPED)**, і оркестратор показує його як `SKIP`, а не `PASS`.
Тест-контур із боку 1С (для прикладного розробника) — `docs/integration-1c/<driver>.md`.

Запуск: `build_project.ps1 -WithUAPKI -WithTests`, потім
`powershell -File run_tests.ps1 [x64|x86] [-NoUapki]` (оркестратор: L0 dumpbin-інваріанти →
L0.5 core_selftest ядра → L0.6 wire_selftest device-ядра → L0.7 ecr_privatjson_selftest драйвера →
L2-ecr ecr_native_host компоненти ECRPrivatJSON через DLL → L-p1 label_printer_selftest драйвера
LabelPrinter → L-p3 label_native_host компоненти LabelPrinter через DLL → L1 selftest по
сценаріях → L2/L3 native_host → L4-iit арбітр ІІТ (негативний контроль + наш підпис + матриця
вердиктів «наш двигун проти ІІТ» на корпусі ЦЗО та еталонах ДПС); підсумкова таблиця
PASS/FAIL/SKIP/BLOCKED, ненульовий exit при провалі). L0.5, L0.6, L0.7, L2-ecr, L-p1 і L-p3
проходять і без `-WithUAPKI`. L4-iit іде і з x64-прогону (`iit_verify_x86.exe` — окремий процес,
WOW64); SKIP там означає, що exe не зібрано або арбітр не піднявся, а не «не та архітектура».

**Режим без UAPKI (`-NoUapki`):** ганяє ядрові/ECR/LabelPrinter-рівні (L0.5 core + L0.6 wire +
L0.7 ecr + L2-ecr ecr_native_host + L-p1 label_printer_selftest + L-p3 label_native_host) —
збирає з `-DBUILD_TESTS=ON` **без** `-DBUILD_WITH_UAPKI=ON`; провайдер (L0.2), L1, L2/L3
native_host і L4-iit → SKIP (не FAIL). L0.1 (рівно 3 експорти головної DLL), L2-ecr (потребує головну DLL +
`ecr_native_host.exe`) і L-p3 (потребує головну DLL + `label_native_host.exe`) лишаються активними.
Швидкий гейт device-ядра+ECR+LabelPrinter без важкої статичної збірки крипто-стеку:
`powershell -File run_tests.ps1 -NoUapki [x64|x86]`.

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
src/transport/      # канали COM/TCP/спулер + device-core (IFramer/NullTerminatedFramer/
                    #   IFrameClassifier/DeviceSession — фундамент драйверів обладнання)
src/platform/       # платформа-каркас драйверів (ResultEnvelope — уніфікований результат операції;
                    #   JobEngine — машина асинхронного завдання)
src/drivers/        # драйвери обладнання поверх device-core; ecr_privatjson/ — пілотний
                    #   ECRPrivatJSON (EcrJsonCodec/EcrPrivatJsonClassifier/EcrPrivatJsonDriver);
                    #   label_printer/ — другий драйвер, LabelPrinter (ZPL): LabelZplGenerator/
                    #   LabelRaster/GfEncoder/BarcodeZpl/LabelXml/LabelPrinterDriver (device-core
                    #   не задіяний — друк односпрямований, синхронний, поверх ITransport)
include/            # заголовки SDK 1С
tests/              # core_selftest (L0.5) + wire_selftest (L0.6) + ecr_privatjson_selftest (L0.7,
                    #   +support/TerminalEmulator) + ecr_native_host (L2-ecr, компонента через DLL) +
                    #   ecr_terminal_emulator (standalone EXE для 1С) + label_printer_selftest (L-p1,
                    #   драйвер LabelPrinter) + label_native_host (L-p3, компонента через DLL) +
                    #   label_printer_emulator (standalone EXE для 1С, +support/LabelEmulator) +
                    #   uapki_selftest (L1) + native_host (L2/L3) + uapki_fiscal_emulator
                    #   (— ручний, HTTP-оракул ЕЦП для тесту UAPKI з 1С, +support/MiniHttpServer
                    #   — власний HTTP/1.1-сервер) + scenarios/ + data/
ExtDataProcessors/  # тестова зовнішня обробка 1С у форматі platform XML (Designer) —
                    #   SimplyAddinConnect: форма з кнопками під усі компоненти + макет з DLL;
                    #   v8project.yaml описує цей 1С-воркспейс (source-set
                    #   EXTERNAL_DATA_PROCESSORS) для плагіна Unica / v8-runner
docs/architecture/    # архітектура по підсистемах (README + 01..04)
docs/tech-debt.md   # реєстр СВІДОМО відкладеного (з причиною й критерієм перевірки)
docs/               # специфікації протоколів (ECR/UAPKI), tasks/
extern/             # сабмодулі: spdlog, nlohmann_json, pugixml, uapki
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
2-біс. **`CMake/compiler_settings.cmake` — `target_compile_options(<ціль> PRIVATE /utf-8)`.**
   Перелік там **поіменний**, і про нього легко забути. Без `/utf-8` MSVC читає джерело в
   ANSI-кодуванні й **мовчки** спотворює кириличні `u"..."`-літерали: помилки збірки немає,
   англійські імена методів у DLL є, російські — ні, а 1С каже «Метод объекта не обнаружен».
3. Перевір збірку `build_project.ps1` (+`-WithUAPKI`, якщо залежить від UAPKI).

Детальніше про модель ядра (VariantHelper, реєстрація, життєвий цикл, платформенні механізми
Етапу 0) — `docs/architecture/core.md`.

## Git-нюанси

- Правки в сабмодулі `extern/uapki` (потрібні для статичної збірки) комітяться **всередині
  сабмодуля**, не з кореня — не загуби їх при `submodule update`.
- **Сабмодулі тримаємо на ТЕГАХ, не на плаваючих гілках.** `spdlog`, `nlohmann_json`,
  `pugixml` закріплені на релізних тегах; `extern/uapki` — виняток, він свідомо йде за
  `main-dev` форку. Інакше збірка залежить від того, який коміт `develop`/`master` випадково
  опинився в клоні: до 2026-09-05 `nlohmann_json` стояв на довільному комміті `develop`, а
  `pugixml` — на HEAD `master`. Оновлюючи залежність, перемикайся `git checkout <тег>`
  усередині сабмодуля й комить новий покажчик у корені.
- `version.h` перегенеровується скриптом при кожній збірці — очікуваний diff.
- У `.gitignore`: `bin/`, `build_*/` та інші build-теки (`/build`, `/build64Lin` тощо), `.vscode/`,
  `tmp/`, `manifest.xml`, `*.epf`, об'єктні файли (`*.o`, `*.d`, `*.so`).
