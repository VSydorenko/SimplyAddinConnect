# SimplyAddinConnect

Нативна зовнішня компонента для **1С:Підприємство** (Windows, C++17). Збирається в одну DLL
(окремо x86 і x64), що реєструє **кілька компонент**, кожна доступна в 1С під власним іменем:

- **AddinUAPKIConnect** — ЕЦП/криптографія через бібліотеку [UAPKI](https://github.com/specinfo-ua/UAPKI);
- **ECRPrivatJSON** — драйвер платіжного термінала ПриватБанк (перший драйвер обладнання);
- **TestComponent** — демо/приклад реєстрації.

Компоненти стоять на спільному ядрі-мості до SDK 1С (`src/core/AddInNative`). Драйвери обладнання
будуються на спільному фундаменті **device-core** (`src/transport/`) + платформі-каркасі
(`src/platform/`); кожен драйвер додає лише свою протокол-специфіку.

## Швидкий старт

```powershell
git submodule update --init --recursive
powershell -ExecutionPolicy Bypass -File build_project.ps1 [-WithUAPKI] [-WithTests]
```

Результат — `bin/Release/SimplyAddinConnectWin.zip` (обидві DLL + `manifest.xml`), готовий до
підключення в 1С як зовнішня нативна компонента. Вимоги: Visual Studio 2022 (C++ desktop),
CMake ≥ 3.16.

## Документація

| Кому | Де |
|---|---|
| **1С-розробнику** (користуюсь готовою компонентою) | [docs/integration-1c/](docs/integration-1c/README.md) — методи компонент, приклади коду, тестування проти емуляторів |
| **C++-розробнику** (розвиваю компоненту) | [docs/architecture/](docs/architecture/README.md) — архітектура по підсистемах (ядро, device-core, драйвери, UAPKI, збірка) |
| **Coding-агенту** | [AGENTS.md](AGENTS.md) — збірка, конвенції коду, як додати компоненту, тести |
| Специфікації протоколів | [docs/](docs/) — `ECR_Privat_JSON_Protokol.md`, `UAPKI_Protokol.md` та ін. |

## Ліцензія та репозиторій

Репозиторій: `github.com/VSydorenko/SimplyAddinConnect`. Версія — `VERSION.txt` + `version.h`
(перегенерується `build_project.ps1` при кожній збірці).
