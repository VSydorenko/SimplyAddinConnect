# tools

## check_core_heritage.py

Перевіряє, що в `src/core/AddInNative.{h,cpp}` не лишилось коду, успадкованого від
проєкту-предка (коміт `366214d`). Exit 0 — чисто, exit 1 — знайдено блоки.

    python tools/check_core_heritage.py                      # поріг 5 рядків
    python tools/check_core_heritage.py --threshold 3        # суворіше
    python tools/check_core_heritage.py --report out.txt     # звіт у файл

Критерій приймання й допустимі винятки — `docs/superpowers/specs/2026-09-07-core-rewrite-design.md` §5-§6.
Виняток додається В КОД скрипта (`ALLOW_SUBSTRINGS`) з коментарем-обґрунтуванням,
поріг НЕ підвищується.

Чинні винятки (обидва — §6 спеки):

| Підрядок | Чому неможливо інакше |
|---|---|
| `switch (ul_reason_for_call)` | канонічний каркас `DllMain` від Microsoft |
| `) override final;` | оголошення перевизначень `IComponentBase`: сигнатури задані SDK 1С, будь-яке відхилення — і платформа не викличе метод. Реалізації цих методів переписані й у звіті не фігурують |
