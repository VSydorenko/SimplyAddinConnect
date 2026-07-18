# CLAUDE.md — інструкції для Claude Code

Цей файл Claude Code читає автоматично на старті сесії в цьому репозиторії.

## Головне

Усі загальні настанови по проєкту — збірка, архітектура, конвенції коду, git —
описані в **`AGENTS.md`**. **Прочитай його першим.** Тут — лише специфіка Claude Code,
щоб не дублювати.

@AGENTS.md

## Оточення цієї машини

- ОС: **Windows** (Windows Server 2022). Основна оболонка — **PowerShell 7+** (`pwsh`).
  Скрипти збірки/тестів — PowerShell (`build_project.ps1`, `run_tests.ps1`).
- Для команд використовуй PowerShell-синтаксис. Bash-інструмент теж доступний (Git Bash),
  але шляхи з Windows-дисками (`R:\...`) і `.ps1`-скрипти краще ганяти через PowerShell.
- Якщо потрібен інтерактивний вхід (напр. авторизація) — попроси користувача виконати
  команду через префікс `!` у промпті.

## Пам'ять між сесіями

Persistent-пам'ять Claude Code лежить у:
```
C:\Users\VSydorenko\.claude\projects\R--github-SimplyAddinConnect\memory\
```
Індекс — `MEMORY.md`. Там уже є нотатки про стан проєкту та інтеграцію UAPKI
(`project-overview.md`, `uapki-integration-state.md`). Звіряйся з ними, але пам'ятай:
нотатки відображають стан на момент запису — перевіряй актуальність у коді.

## Швидкі команди

```powershell
# Ініціалізація сабмодулів (один раз)
git submodule update --init --recursive

# Збірка основного проєкту
powershell -ExecutionPolicy Bypass -File build_project.ps1

# Збірка з UAPKI
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI
```

⚠️ Тести (`-WithTests` / `run_tests.ps1`) наразі не працюють — теки `tests/` немає.
Деталі в `AGENTS.md`, розділ 3.

## Особливості роботи в цьому репозиторії

- Проєкт **реально збирався** (готові DLL у `bin/Release/` від 06.2025). Перш ніж змінювати
  збіркову логіку — переконайся, що базова збірка ще відтворюється у поточному оточенні.
- Не редагуй `version.h` вручну під версію збірки — його перегенеровує `build_project.ps1`.
- Правки в сабмодулі `extern/uapki` комітяться **всередині сабмодуля**, не з кореня.
- Дотримуйся конвенцій логування (`REPORT_*` / `NEUTRAL_REPORT_*`) і PCH з `AGENTS.md` —
  вони перевірені по коду й компілятор/1С на них розраховують.
