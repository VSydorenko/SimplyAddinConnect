# AGENTS.md

Настанови для coding-агентів по цьому репозиторію. Архітектура — `docs/architecture/` (по документу на підсистему).
Мова коду й комітів — українська/російська (дотримуйся мови файлу, який редагуєш).

## Проєкт

**SimplyAddinConnect** — нативна зовнішня компонента для **1С:Підприємство** (Windows, C++17).
Збирається в одну DLL (x86 і x64), що реєструє кілька компонент:
- **AddinECRPrivatJSON** — платіжний термінал ПриватБанку (JSON-протокол, COM/TCP/WebSocket);
- **AddinUAPKIConnect** — ЕЦП/крипто через бібліотеку UAPKI.

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
  при `INIT` (потрійний пошук каталогу — див. `docs/architecture/03-uapki.md`); у підсумковий ZIP
  потрапляють обидва варіанти — разом 5 файлів (див. `docs/architecture/03-uapki.md`, `04-build-and-packaging.md`);
- `-WithTests` — збирає тестові консольні exe (`uapki_selftest`, `native_host`) з `tests/`
  (працює **лише разом з `-WithUAPKI`** — тести залежать від крипто-ядра; без UAPKI піддиректорію
  `tests/` тихо пропущено).

Скрипт перегенеровує `version.h` (інкремент build), очищає `build_x86/`, `build_x64/`,
`bin/Release/`, збирає обидві архітектури в Release і пакує в `bin/Release/SimplyAddinConnectWin.zip`.
Опції CMake: `-DBUILD_WITH_UAPKI=ON|OFF`, `-DBUILD_TESTS=ON|OFF` (обидві default OFF).

## Тести

Тека `tests/` **існує**: `tests/CMakeLists.txt` (окремі консольні exe, лінкуються НЕ в головну
DLL і НЕ підключають `src/core/pch.h`), `tests/scenarios/*.json` (7 сценаріїв L1),
`tests/data/` (тестовий контейнер `test-diia.p12`, сертифікати, CRL — read-only вхід).

Дві цілі (лише Windows, лише при `BUILD_WITH_UAPKI=ON`):
- **`uapki_selftest.exe`** (L1) — лінкує `uapki_bundle` напряму (без 1С, без завантаження DLL) і
  проганяє JSON-сценарії з `tests/scenarios/` через `process()`/`json_free()` статичного ядра.
- **`native_host.exe`** (L2/L3) — емулює платформу 1С: вантажить головну DLL через `LoadLibraryW`
  і викликає компоненту `AddinUAPKIConnect` через `IComponentBase`/`CallUapki`, e2e-кейси 1-4 і
  крос-валідація ПРРО (кейс 5, потребує `PRRO_DOCS_DIR`, інакше SKIP).

Запуск: `build_project.ps1 -WithUAPKI -WithTests`, потім
`powershell -File run_tests.ps1 [x64|x86]` (оркестратор: L0 dumpbin-інваріанти → L1 selftest по
сценаріях → L2/L3 native_host; підсумкова таблиця PASS/FAIL/SKIP/BLOCKED, ненульовий exit при
провалі).

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
src/protocols/      # логіка протоколів (ECRPrivatJSON)
src/helpers/        # ServiceTools (логування/конвертації) + хелпери фіч
src/transport/      # канали: COM, TCP, WebSocket (client/server)
include/            # заголовки SDK 1С
tests/              # uapki_selftest (L1) + native_host (L2/L3) + scenarios/ + data/
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

1. `src/components/Моя.{h,cpp}` — успадкуй `AddInNative`, зареєструй через `AddComponent`,
   методи опиши в `RegisterMethods()` (лямбди з аргументами `VH`, результат — `this->result`):
   ```cpp
   std::vector<std::u16string> Моя::names = { AddComponent(u"Моя", []() { return new Моя; }) };
   namespace { auto& _force = Моя::names; }        // проти відкидання лінкером
   Моя::Моя() { REPORT_INFO("Ініціалізація Моя"); RegisterMethods(); }
   ```
2. У `CMake/components.cmake`: додай файли до `HEADER_FILES`/`SOURCE_FILES`, за потреби окрему
   `add_library(... OBJECT ...)` з include-шляхами й `add_dependencies` (мінімум `base_component spdlog`),
   і `$<TARGET_OBJECTS:...>` до фінальної SHARED-цілі.
3. Перевір збірку `build_project.ps1` (+`-WithUAPKI`, якщо залежить від UAPKI).

Детальніше про модель ядра (VariantHelper, реєстрація, життєвий цикл) — `docs/architecture/01-core.md`.

## Git-нюанси

- Правки в сабмодулі `extern/uapki` (потрібні для статичної збірки) комітяться **всередині
  сабмодуля**, не з кореня — не загуби їх при `submodule update`.
- `version.h` перегенеровується скриптом при кожній збірці — очікуваний diff.
- У `.gitignore`: `bin/`, `build_*/` та інші build-теки (`/build`, `/build64Lin` тощо), `.vscode/`,
  `tmp/`, `manifest.xml`, `*.epf`, об'єктні файли (`*.o`, `*.d`, `*.so`).
