# AGENTS.md

Настанови для coding-агентів по цьому репозиторію. Архітектура — `docs/ARCHITECTURE.md`.
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
- без прапорців — основний проєкт;
- `-WithUAPKI` — + інтеграція UAPKI та провайдери `cm-pkcs12`;
- `-WithTests` — ⚠️ **зараз падає** (див. нижче).

Скрипт перегенеровує `version.h` (інкремент build), очищає `build_x86/`, `build_x64/`,
`bin/Release/`, збирає обидві архітектури в Release і пакує в `bin/Release/SimplyAddinConnectWin.zip`.
Опції CMake: `-DBUILD_WITH_UAPKI=ON|OFF`, `-DBUILD_TESTS=ON|OFF` (обидві default OFF).

## Тести ⚠️

Теки `tests/` у репозиторії **немає**, хоча `CMakeLists.txt` робить `add_subdirectory(tests)`
при `BUILD_TESTS=ON`. Тому `-WithTests` і `run_tests.ps1` **наразі падають** на генерації CMake.
Інфраструктура (ctest, `bin/Debug`) є — самих тестів нема. Не обіцяй робочі тести, поки не
створено `tests/` з власним `CMakeLists.txt`.

## Структура

```
CMake/              # модульна збірка; components.cmake — джерело правди щодо складу DLL
src/core/           # ядро AddInNative (міст до SDK 1С) + pch.h
src/components/     # компоненти-фасади для 1С
src/protocols/      # логіка протоколів (ECRPrivatJSON)
src/helpers/        # ServiceTools (логування/конвертації) + хелпери фіч
src/transport/      # канали: COM, TCP, WebSocket (client/server)
include/            # заголовки SDK 1С
docs/               # ARCHITECTURE.md + специфікації протоколів
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

Детальніше про модель ядра (VariantHelper, реєстрація, життєвий цикл) — `docs/ARCHITECTURE.md` §2.

## Git-нюанси

- Правки в сабмодулі `extern/uapki` (потрібні для статичної збірки) комітяться **всередині
  сабмодуля**, не з кореня — не загуби їх при `submodule update`.
- `version.h` перегенеровується скриптом при кожній збірці — очікуваний diff.
- У `.gitignore`: `bin/`, `build_*/` та інші build-теки (`/build`, `/build64Lin` тощо), `.vscode/`,
  `tmp/`, `manifest.xml`, `*.epf`, об'єктні файли (`*.o`, `*.d`, `*.so`).
