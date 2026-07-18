# AGENTS.md — інструкції для AI-агентів

Універсальний файл настанов для будь-якого coding-агента, що працює над цим репозиторієм
(Claude Code, Copilot, Cursor тощо). Для специфіки Claude Code див. також `CLAUDE.md`.

> Мова коментарів і повідомлень у проєкті — українська/російська. Дотримуйся мови
> оточення у файлі, який редагуєш; нові тексти й коміти — українською.

---

## 1. Що це за проєкт

**SimplyAddinConnect** — нативна зовнішня компонента (Native AddIn) для платформи **1С:Підприємство**
під Windows. Написана на **C++17**, збирається у дві DLL (x86 і x64), які реєструють у 1С кілька
компонент під різними іменами.

- Репозиторій: `https://github.com/VSydorenko/SimplyAddinConnect`
- Поточна версія: див. `VERSION.txt` (major.minor.revision) + `version.h` (build)
- Функціональні лінії:
  - **AddinECRPrivatJSON** — інтеграція з платіжним терміналом ПриватБанку (JSON-протокол,
    транспорт COM/TCP/WebSocket). Найзріліша частина.
  - **AddinUAPKIConnect** — обгортка бібліотеки UAPKI (укр. ЕЦП/крипто) для 1С. У активній розробці.

---

## 2. Збірка

### Вимоги
- **Visual Studio 2022** з компонентом «Розробка класичних застосунків на C++»
  (скрипт збірки жорстко використовує генератор `Visual Studio 17 2022`).
- **CMake ≥ 3.16** (enforced у `CMakeLists.txt`).
- **Git** з ініціалізованими сабмодулями.

### Ініціалізація сабмодулів (обов'язково перед першою збіркою)
```powershell
git submodule update --init --recursive
```
Сабмодулі (`extern/`): `spdlog`, `nlohmann_json`, `ixwebsocket`, `uapki` (форк `VSydorenko/UAPKI`).

### Основна команда збірки
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 [-WithUAPKI] [-WithTests]
```

| Виклик | Що збирається |
|---|---|
| `build_project.ps1` | Лише основний проєкт, без UAPKI, без тестів |
| `build_project.ps1 -WithUAPKI` | Основний проєкт + інтеграція UAPKI + провайдери cm-pkcs12 |
| `build_project.ps1 -WithTests` | Основний проєкт + тести ⚠️ *(див. розділ 3 — зараз не працює)* |

Скрипт автоматично:
- інкрементує `VERSION_BUILD` у `version.h` і перегенеровує файл при **кожному** запуску;
- запускає `manifest.ps1` (генерує `manifest.xml`);
- **очищає** `build_x86/`, `build_x64/` і `bin/Release/` перед збіркою;
- генерує проєкти CMake для `Win32` (x86) та `x64`, збирає у конфігурації **Release**;
- пакує результат у `bin/Release/SimplyAddinConnectWin.zip` (обидві DLL + `manifest.xml`).

### Результат збірки
`bin/Release/`:
- `SimplyAddinConnectWin_x86.dll`, `SimplyAddinConnectWin_x64.dll`
- `SimplyAddinConnectWin.zip` — готовий пакет для підключення в 1С
- статичні `.lib` (uapki*, cm-pkcs12 тощо — при `-WithUAPKI`)

> `bin/` у `.gitignore` — артефакти збірки не комітяться.

### Ручна збірка через CMake (альтернатива скрипту)
```powershell
cmake -A x64 -S . -B build_x64          # або -A Win32 для x86
cmake --build build_x64 --config Release
```
Опції CMake: `-DBUILD_WITH_UAPKI=ON|OFF` (default OFF), `-DBUILD_TESTS=ON|OFF` (default OFF).

---

## 3. Тести ⚠️

Скрипт `run_tests.ps1` та прапорець `-WithTests` розраховують на теку `tests/`
(`add_subdirectory(tests)` у `CMakeLists.txt`).

**Наразі теки `tests/` у репозиторії НЕМАЄ**, тому:
- `build_project.ps1 -WithTests` — **впаде** на етапі генерації CMake;
- `run_tests.ps1 [x86|x64]` — **впаде** так само.

Інфраструктура тестів (CMake-гілка, скрипт запуску через `ctest`, окрема `bin/Debug`)
присутня, але самих тестів немає. Перш ніж вмикати тести — треба створити теку `tests/`
з власним `CMakeLists.txt`. Не документуй і не обіцяй робочі тести, поки цього не зроблено.

---

## 4. Структура репозиторію

```
CMakeLists.txt            # кореневий, підключає модулі з CMake/
CMake/                    # модульна конфігурація збірки
  options.cmake           #   опції (BUILD_TESTS, BUILD_WITH_UAPKI), статичні дефайни
  dependencies.cmake      #   сабмодулі-залежності
  components.cmake        #   ⭐ визначення всіх компонент і фінальної DLL
  compiler_settings.cmake #   C++17, прапорці компілятора
  platform_settings.cmake
  output_settings.cmake   #   куди складати артефакти
  link_settings.cmake
  uapki_full_static.cmake #   bundle статичного лінкування UAPKI (uapki_bundle)
build_project.ps1         # головний скрипт збірки
run_tests.ps1             # скрипт тестів (див. розділ 3)
manifest.ps1 / manifest.xml
version.h / VERSION.txt   # версіонування (version.h авто-генерується скриптом)
include/                  # заголовки SDK 1С (AddInDefBase, ComponentBase, IMemoryManager, types, com)
src/
  core/                   # каркас: AddInNative.{h,cpp,def,rc}, pch.h
  components/             # компоненти-фічі (AddinECRPrivatJSON, AddinUAPKIConnect)
  helpers/                # ServiceTools (логування) + хелпери під кожну фічу
  protocols/              # реалізація протоколів (ECRPrivatJSON)
  transport/              # транспорт: COM, TCP, WebSocket (client/server)
  TestComponent.*         # приклад/тестова компонента
docs/                     # опис протоколів (UAPKI_Protokol.md, ECR_Privat_JSON_Protokol.md)
extern/                   # git-сабмодулі
```

---

## 5. Архітектура компонент

Каркас — `src/core/AddInNative.*`: власна реалізація шаблону зовнішньої компоненти 1С
(без важких C++-шаблонів, реєстрація через лямбди). Одна DLL може реєструвати кілька компонент.

### Реєстрація компоненти
```cpp
// У .cpp компоненти: статичний член реєструє імена, під якими 1С її бачить.
std::vector<std::u16string> МояКомпонента::names = {
    AddComponent(u"МояКомпонента", []() { return new МояКомпонента; })
};
namespace { auto& _force = МояКомпонента::names; }   // проти відкидання лінкером

МояКомпонента::МояКомпонента() {
    REPORT_INFO("Ініціалізація компонента МояКомпонента");
    RegisterMethods();
}
```

Наразі в 1С реєструються імена: `AddinECRPrivatJSON`, `AddinUAPKIConnect`,
а також `AddInNative` / `SimplyAddinConnect` / `SimplyConnect` (через `TestComponent`).

### Реєстрація властивостей і методів
У `RegisterMethods()` через лямбди. Аргументи лямбд — тип **`VH`** (синонім `VariantHelper`),
що дозволяє повертати в 1С змінені значення параметрів. Результат функції — через член `this->result`.
Пошук методів **регістронезалежний**. Кожен метод має два імені (англ. + укр./рос.).

```cpp
AddProperty(u"Text", u"Текст",
    [&](VH prop) { prop = this->getText(); },        // getter
    [&](VH prop) { this->setText(prop); });          // setter

AddFunction(u"GetText", u"ОтриматиТекст",
    [&]() { this->result = this->getText(); });

AddProcedure(u"SetText", u"ВстановитиТекст",
    [&](VH param) { this->setText(param); },
    { {0, DefaultHelper(u"Default")} });             // значення параметрів за замовчанням
```

---

## 6. Конвенції коду (обов'язкові)

### 6.1. Прекомпільований заголовок (PCH)
- `src/core/pch.h` підключається автоматично через `target_precompile_headers`.
- У **кожному `.cpp`** файлі `pch.h` має бути **першим рядком** (`#include "../core/pch.h"`
  або відповідний відносний шлях).
- У `.h` файлах `pch.h` **не підключати ніколи**. Включай лише те, що безпосередньо потрібно
  цьому заголовку; де можна — forward declaration.

### 6.2. Логування — лише через макроси
Визначені в `src/helpers/ServiceTools.h`. **Прямий `spdlog` заборонений.**

- **`REPORT_*`** — у нестатичних методах компоненти (доступний `this`):
  `REPORT_TRACE / REPORT_DEBUG / REPORT_INFO / REPORT_WARN / REPORT_ERROR`.
- **`NEUTRAL_REPORT_*`** — у статичних/const-методах або поза контекстом `AddInNative`;
  **перший аргумент — ім'я компонента (рядок)**:
  ```cpp
  NEUTRAL_REPORT_ERROR("UAPKIConnectHelper", "Порожня відповідь від UAPKI");
  ```
- `REPORT_ERROR` автоматично реєструє помилку для показу в 1С.

### 6.3. Формування повідомлень
- Складай рядок **заздалегідь**, конкатенацією: `std::string msg = "Текст: " + var + ", n=" + std::to_string(n);`
- **Заборонено** printf-стиль усередині макросів: ~~`REPORT_ERROR("Помилка: %s", s)`~~.
- Повідомлення — з великої літери, без крапки в кінці (окрім динамічних даних).

### 6.4. Обробка помилок
- Після `REPORT_ERROR` зазвичай `return false` (якщо функція індикує успіх/невдачу).
- Не кидай `throw` для бізнес-помилок, які треба повідомити 1С — логуй і повертай `false`.
- Код, що може кинути C++-виняток, обгортай у `try-catch`; у `catch` — `REPORT_ERROR`
  (або `NEUTRAL_REPORT_ERROR`) з `e.what()`.

### 6.5. Заборонені практики
- Прямий `spdlog`.
- Прямі конвертації рядків `AddInNative::MB2WCHAR` / `WCHAR2MB` —
  використовуй `ServiceTools::SafeMB2WCHAR` / `ServiceTools::SafeWCHAR2MB`.
- printf-стиль форматування в логах.

---

## 7. Як додати нову компоненту

1. Створи `src/components/МояКомпонента.{h,cpp}` (успадкуй `AddInNative`, зареєструй через
   `AddComponent`, методи — у `RegisterMethods()`), за потреби хелпер у `src/helpers/`.
2. У `CMake/components.cmake`:
   - додай `.h` до `HEADER_FILES`, `.cpp` до `SOURCE_FILES`;
   - за потреби створи `add_library(<component> OBJECT ...)` з `target_include_directories`
     та `set_target_properties(... CXX_STANDARD 17)`;
   - пропиши `add_dependencies` (мінімум `base_component spdlog`);
   - додай `$<TARGET_OBJECTS:<component>>` до фінальної `add_library(${TARGET} SHARED ...)`.
3. Пиши код за конвенціями розділу 6.
4. Перевір збірку: `build_project.ps1` (додай `-WithUAPKI`, якщо компонента залежить від UAPKI).

---

## 8. Git і гілки

- Основна гілка — `main`. Розробка UAPKI — `add_UAPKI`.
- Коміт-меседжі — українською, у наказовому/описовому стилі (як в історії репозиторію).
- Комітити/пушити — лише коли про це просить користувач. Не працюй прямо в `main` без потреби.

### Підводні камені
- **Сабмодуль `extern/uapki`** може містити локальні (незакомічені) правки в `asn_system.h`
  та `uapkif-export.h`, потрібні для статичної збірки. Правки в сабмодулі комітяться **всередині
  самого сабмодуля**, а не з батьківського репозиторію. Не «загубь» їх при `submodule update`.
- `version.h` **перегенеровується** скриптом збірки при кожному запуску — не дивуйся diff'у.
- `bin/`, `build_*`, `.vscode/`, `tmp/`, `manifest.xml`, `*.epf` — у `.gitignore`.

---

## 9. Довідники
- `docs/UAPKI_Protokol.md` — повний JSON-протокол бібліотеки UAPKI (методи, параметри, коди помилок).
- `docs/ECR_Privat_JSON_Protokol.md` — протокол платіжного терміналу ПриватБанку.
- `README.md` — опис шаблону компоненти та приклад реєстрації.
- `BUILD.md` — коротка інструкція збірки через CMake.
