#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Мінімальний синтаксичний сторож BSL для файлів 1С у цьому репозиторії.

ЧОМУ ВІН ІСНУЄ. Безголової перевірки модулів ЗОВНІШНЬОЇ ОБРОБКИ тут немає:

  * `DESIGNER /CheckModules` обробку НЕ бачить — вона не входить у конфігурацію бази.
    Гірше: платформа МОВЧКИ ІГНОРУЄ невідомі ключі, тож `-ExternalDataProcessorFile`
    не помилка, а порожня дія — перевіряється конфігурація одноразової бази, яка
    порожня, і результат завжди «Синтаксических ошибок не обнаружено!» з exit 0.
    Перевірено негативною верифікацією 2026-09-22: прогін із завідомо неіснуючим
    ключем і неіснуючим файлом дав той самий зелений вивід.
  * `DESIGNER /LoadExternalDataProcessorOrReportFromFiles` збирає `.epf` з XML, але
    BSL НЕ компілює — збірка проходить і на модулі з синтаксичною помилкою.
  * `unica.runtime.execute operation=syntax` не приймає source-set зовнішніх обробок.
  * `ENTERPRISE /Execute` модуль компілює, але потребує GUI й зависає без нього.

Тобто єдиною справжньою перевіркою лишається відкриття обробки людиною. Цей скрипт
не замінює компілятор — він ловить рівно той вузький клас, на якому тут уже двічі
спіткнулись, і робить це за секунди:

  1. фігурні дужки `{` / `}` у коді — синтаксис C++, у BSL їх не існує;
  2. українські двійники ключових слів (`Якщо` замість `Если` тощо);
  3. неспарені блоки: Процедура/Функция, Если, Цикл, Попытка.

Рядкові літерали та коментарі з аналізу вилучаються, тому текст українською
всередині них не дає хибних спрацювань.

Запуск:
    python scripts/check-bsl-syntax.py            # усі .bsl у ExtDataProcessors/
    python scripts/check-bsl-syntax.py <файл>...  # конкретні файли
Код повернення: 0 — чисто, 1 — є знахідки.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROOT = ROOT / "ExtDataProcessors"

# Українські двійники ключових слів BSL. Ліворуч — те, що пише людина, яка думає
# українською; праворуч — те, що розуміє платформа. Компілятор на такому каже
# «Неопознанный оператор», і причина з повідомлення не очевидна.
LOOKALIKES = {
    "Якщо": "Если",
    "Тоді": "Тогда",
    "Інакше": "Иначе",
    "ІнакшеЯкщо": "ИначеЕсли",
    "КінецьЯкщо": "КонецЕсли",
    "КінецьЦиклу": "КонецЦикла",
    "КінецьПроцедури": "КонецПроцедуры",
    "КінецьФункції": "КонецФункции",
    "Повернути": "Возврат",
    "Функція": "Функция",
    "Процедура_": "Процедура",
    "Істина": "Истина",
    "Хибність": "Ложь",
    "Невизначено": "Неопределено",
    "ДляКожного": "Для Каждого",
    "Перервати": "Прервать",
    "Продовжити": "Продолжить",
}

# Парні блоки: відкривач -> закривач. Рахуються як цілі слова, поза рядками й коментарями.
PAIRS = [
    (r"(?:^|\s)(?:Процедура|Функция)\b", r"\bКонецПроцедуры\b|\bКонецФункции\b", "Процедура/Функция"),
    (r"\bЕсли\b", r"\bКонецЕсли\b", "Если"),
    (r"\bЦикл\b", r"\bКонецЦикла\b", "Цикл"),
    (r"\bПопытка\b", r"\bКонецПопытки\b", "Попытка"),
]


def strip_noise(text: str) -> str:
    """Прибирає рядкові літерали й коментарі, зберігаючи розбивку на рядки.

    Довжина рядків не зберігається — номери рядків зберігаються, і цього досить:
    всі повідомлення адресують рядок, а не колонку.
    """
    out_lines = []
    for line in text.split("\n"):
        # Рядкові літерали BSL: "..." з подвоєнням лапок усередині.
        line = re.sub(r'"(?:[^"]|"")*"', '""', line)
        # Коментар до кінця рядка.
        pos = line.find("//")
        if pos >= 0:
            line = line[:pos]
        out_lines.append(line)
    return "\n".join(out_lines)


def check_file(path: Path) -> list[str]:
    try:
        raw = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        return [f"{path}: не читається ({exc})"]

    code = strip_noise(raw)
    lines = code.split("\n")
    rel = path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else str(path)
    found: list[str] = []

    # 1. Фігурні дужки — у BSL їх немає взагалі.
    for i, line in enumerate(lines, 1):
        for ch in "{}":
            if ch in line:
                found.append(
                    f"{rel}:{i}: символ `{ch}` — у BSL блоки не оформлюються фігурними "
                    f"дужками (це синтаксис C++)"
                )
                break

    # 2. Українські двійники ключових слів.
    for i, line in enumerate(lines, 1):
        for bad, good in LOOKALIKES.items():
            if re.search(r"\b" + re.escape(bad) + r"\b", line):
                found.append(
                    f"{rel}:{i}: `{bad}` — український двійник; BSL знає лише `{good}`"
                )

    # 3. Баланс блоків.
    for open_rx, close_rx, name in PAIRS:
        n_open = len(re.findall(open_rx, code))
        n_close = len(re.findall(close_rx, code))
        if n_open != n_close:
            found.append(
                f"{rel}: блок `{name}` не збалансований: відкривачів {n_open}, "
                f"закривачів {n_close}"
            )

    return found


def collect(args: list[str]) -> list[Path]:
    if args:
        return [Path(a).resolve() for a in args]
    if not DEFAULT_ROOT.exists():
        return []
    return sorted(
        p for p in DEFAULT_ROOT.rglob("*.bsl")
        if ".build" not in p.parts  # робочий кеш unica/v8-runner — не наш код
    )


def main() -> int:
    targets = collect([a for a in sys.argv[1:] if not a.startswith("-")])
    if not targets:
        print("BSL-файлів не знайдено — нічого перевіряти")
        return 0

    problems: list[str] = []
    for path in targets:
        problems.extend(check_file(path))

    if problems:
        for line in problems:
            print(line)
        print(f"\nBSL: знайдено {len(problems)} проблем(и) у {len(targets)} файл(ах)")
        return 1

    print(f"OK: BSL чистий ({len(targets)} файл(ів))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
