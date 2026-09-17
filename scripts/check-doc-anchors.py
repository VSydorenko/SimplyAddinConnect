#!/usr/bin/env python3
"""Гард якорів документації.

Перевіряє, що кожен якір, названий у документах, справді існує:
  1. шлях репо (`src/…`, `tests/…`, `docs/…`, `CMake/…`, `.claude/…`, `scripts/…`)
     — у беквотах або як ціль маркдаун-лінка;
  2. маркдаун-лінк на сусідній док (`[X](Y.md)`), зокрема відносний;
  3. вказівник «док § «Розділ»» / «скіл `x` § «Розділ»» — розділ шукається серед
     ЗАГОЛОВКІВ цільового файла, а не будь-де в тексті;
  4. посилання «файл:рядок» (`EcrPrivatJsonDriver.cpp:264`, `DeviceSession.cpp:436-447`)
     — файл існує, і номер рядка НЕ виходить за його межі.

Основа — scripts/check-doc-anchors.py з репозиторію metahub (перевірки 1-3);
перевірка 4 додана тут, бо в цьому репо посилання «файл:рядок» — основна форма
вказівника: їх понад 370, і саме вони протухають першими.

Чому це потрібно: документація тут навмисно посилається на код замість копіювати
його. Це працює, лише поки вказівник живий. Мертвий шлях або з'їхалий номер рядка
не бачить ні компілятор, ні гейт — його бачить тільки цей гард. Читач іде за
`Driver.cpp:264`, потрапляє в інше місце, і документ при цьому виглядає достовірним.

🔴 Межа: гард перевіряє ШЛЯХИ, РОЗДІЛИ і МЕЖІ ФАЙЛА — не зміст. Посилання на живий
файл і рядок, що існує, але описує вже інше, — зелене. Такий дрейф ловить лише
читання коду (див. docs/architecture/testing-rules.md про хибно-зелене — природа та сама).

⚪ Що НЕ перевіряється навмисно: посилання на код 1С-конфігурацій
(`МенеджерОборудованияКлиент:305`, `Form.xml:145`) — цих репозиторіїв тут немає, і
їхні номери рядків дійсні лише для конкретного вивантаження (див. CLAUDE.md,
«Кілька агентів в одній гілці»). Вони відсіюються за розширенням файла.

Використання:
  python scripts/check-doc-anchors.py            # звіт + exit 1, якщо є биті
  python scripts/check-doc-anchors.py --quiet    # лише биті
  python scripts/check-doc-anchors.py --list     # що саме перевірено (для калібрування)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# docs/tasks/ і docs/superpowers/ навмисно ПОЗА перевіркою: це історичні звіти, спеки й
# плани — вони фіксують стан на момент написання. Посилання на видалені відтоді файли там
# не помилка, а слід історії; «виправляти» їх означало б переписувати звіт заднім числом.
SOURCE_GLOBS = [
    "docs/*.md",
    "docs/architecture/*.md",
    "docs/integration-1c/*.md",
    ".claude/skills/**/*.md",
    ".claude/commands/*.md",
]
SOURCE_FILES = ["AGENTS.md", "CLAUDE.md", "README.md"]

REPO_ROOTS = (
    "src", "tests", "docs", "include", "CMake", "scripts",
    "extern", "ExtDataProcessors", ".claude",
)

# Плейсхолдер, glob або %ЗМІННА% — не якір: перевіряти нічого.
PLACEHOLDER = re.compile(r"[<>{}*|]|\.\.\.|\$\{|\$[A-Za-z_]|%[A-Za-z_]+%")
# Токен із пробілом — це команда або фраза в беквотах, не шлях.
HAS_SPACE = re.compile(r"\s")
# Хвіст «:рядки» в будь-якій формі: `:264`, `:79-80`, `:82, 85`, `:93,111`.
LINE_SUFFIX = re.compile(r":\s*\d+(?:\s*[-–]\s*\d+)?(?:\s*,\s*\d+(?:\s*[-–]\s*\d+)?)*$")
# Пара «заголовок + реалізація» часто згадується без розширення: `src/core/AddInNative`,
# `tests/support/TerminalEmulator`. Такий шлях вважаємо живим, якщо є будь-який із файлів.
PAIR_EXT = (".h", ".cpp", ".hpp", ".c", ".ps1", ".py", ".md")
# Документи, яких у репо немає й не буде: зовнішні специфікації, memory-індекс, локальні нотатки.
EXTERNAL_DOC_PREFIXES = ("UAPKI-PM-",)
# Шляхи, яких немає НАВМИСНО: документація згадує їх як видалені. Це не протухлий якір,
# а зафіксована історія — перевіряти нічого, але й мовчки ігнорувати клас «немає шляху»
# не можна, тому перелік явний.
KNOWN_REMOVED = {
    "src/protocols/",            # старий драйвер AddinECRPrivatJSON, видалений 2026-07
    "src/helpers/ECRPrivatJSON/",  # його ж хелпери — див. ecrprivatjson.md, історична примітка
}
# Наш стиль §-вказівника — номер, а не назва: `bpo-contract.md §2.7`, `ecrprivatjson.md) §5.2`.
# Ціль перевірки: у названому документі справді є заголовок із цим номером.
# Шлях перед іменем захоплюємо навмисно: `integration-1c/README.md §2.1` і
# `architecture/README.md §2.1` — різні документи, а імена однакові.
DOC_SECTION = re.compile(
    r"(?P<doc>(?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]+\.md)[`)\]]*[^.\n§]{0,15}?§+\s*(?P<num>\d+(?:\.\d+)*)"
)
# Заголовок після зняття решітки, зірочок і лапок: «## 2.6. Шарування…», «### **5.30 Get…**».
HEADING_NUM = re.compile(r"^[\s#*`_]*(\d+(?:\.\d+)*)")
# Токен шляху: усе до пробілу/беквота/дужки. Кирилиця дозволена — слеш-команди.
BACKTICKED = re.compile(r"`([^`\n]+)`")
MD_LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
# §-вказівник і найближче ПЕРЕД ним посилання на док/скіл (у рядку їх буває кілька).
SECTION_AT = re.compile(r"§{1,2}\s*(?P<sections>«[^»\n]+»(?:\s*[,і]\s*«[^»\n]+»)*)")
ANCHOR_BEFORE = re.compile(
    r"(?<![A-Za-z0-9_./-])"
    r"(?:(?P<doc>[A-Za-z0-9_./-]+\.md)|скіл\s+`(?P<skill>[a-z0-9-]+)`)"
    r"(?!.*(?:\.md|скіл\s+`))",
    re.S,
)
# Канон поза git: memory-індекс (машинно-локальний) і локальні нотатки;
# `SKILL.md` без шляху — згадка форми скіла, не посилання на конкретний файл.
EXTERNAL_DOCS = {"MEMORY.md", "SKILL.md", "CLAUDE.local.md"}
SECTION_NAME = re.compile(r"«([^»\n]+)»")
HEADING = re.compile(r"^\s{0,3}#{1,6}\s+(.+?)\s*$", re.M)
URL = re.compile(r"https?://")

# --- Перевірка 4: «файл:рядок» ---------------------------------------------
# Розширення НАШОГО коду. Навмисно без .xml/.json/.bsl: файли з такими іменами в
# документах — це переважно код 1С-конфігурацій, яких у репо немає.
CODE_EXT = ("cpp", "h", "hpp", "c", "cc", "ps1", "py", "cmake", "md")
FILE_LINE = re.compile(
    r"(?<![\w/.-])(?P<file>[A-Za-z_][\w.+-]*\.(?:" + "|".join(CODE_EXT) + r"))"
    r":(?P<line>\d+)(?:\s*[-–]\s*(?P<line2>\d+))?"
)
# Теки, у яких шукати файл за голим іменем не треба (артефакти збірки).
SKIP_DIRS = {".git", "build_x86", "build_x64", "build32Lin", "build64Lin", "bin", "tmp", "__pycache__"}

_index: dict[str, list[Path]] | None = None
_lines_cache: dict[Path, int] = {}


def file_index() -> dict[str, list[Path]]:
    """Ім'я файла -> усі його шляхи в репо. Сабмодулі (extern/uapki) включно:
    на них теж посилаються, а git ls-files їх не показує."""
    global _index
    if _index is None:
        _index = {}
        for path in ROOT.rglob("*"):
            if not path.is_file():
                continue
            if any(part in SKIP_DIRS for part in path.relative_to(ROOT).parts):
                continue
            _index.setdefault(path.name, []).append(path)
    return _index


def line_count(path: Path) -> int:
    if path not in _lines_cache:
        try:
            with path.open("rb") as fh:
                _lines_cache[path] = sum(1 for _ in fh)
        except OSError:
            _lines_cache[path] = 0
    return _lines_cache[path]


def norm(token: str) -> str:
    token = token.strip().rstrip(".,:;)»").lstrip("@")
    if token.startswith("./"):
        token = token[2:]
    return token


def resolve_doc(name: str, src: Path) -> Path | None:
    """Голе ім'я `.md` може означати сусідній файл, док архітектури,
    інструкцію для 1С або кореневий канон — приймаємо будь-який із варіантів."""
    if name.startswith(str(ROOT)):
        cand = Path(name)
        return cand if cand.exists() else None
    stripped = name.lstrip("/")
    for cand in (
        src.parent / stripped,
        src.parent.parent / stripped,
        ROOT / "docs/architecture" / stripped,
        ROOT / "docs/integration-1c" / stripped,
        ROOT / "docs/tasks" / stripped,
        ROOT / "docs" / stripped,
        ROOT / stripped,
        ROOT / "docs/architecture" / Path(stripped).name,
        ROOT / "docs/integration-1c" / Path(stripped).name,
        ROOT / "docs/tasks" / Path(stripped).name,
    ):
        if cand.exists():
            return cand
    return None


def resolve_doc_all(name: str, src: Path) -> list[Path]:
    """Усі файли, на які може вказувати це ім'я: точний резолв плюс однойменні
    документи в інших теках (`uapki.md` живе і в architecture/, і в integration-1c/)."""
    found: list[Path] = []
    exact = resolve_doc(name, src)
    if exact is not None:
        found.append(exact)
    for cand in file_index().get(Path(name).name, []):
        if cand.suffix == ".md" and cand not in found:
            found.append(cand)
    return found


def is_repo_path(token: str) -> bool:
    return token.split("/", 1)[0] in REPO_ROOTS and "/" in token


def repo_path_exists(token: str) -> bool:
    """Шлях живий, якщо він існує як є — або як пара «заголовок + реалізація»
    без розширення (`src/core/AddInNative` → AddInNative.h / AddInNative.cpp)."""
    if (ROOT / token).exists():
        return True
    if "." not in Path(token).name:
        return any((ROOT / (token + ext)).exists() for ext in PAIR_EXT)
    return False


def is_foreign_path(token: str) -> bool:
    """Шлях із чужого репозиторію (`SMP_SimplyConnect/…`, `cfe/src/…`) — не наш якір.
    Ознака: є слеш, але корінь не з REPO_ROOTS."""
    return "/" in token and token.split("/", 1)[0] not in REPO_ROOTS


def headings(path: Path) -> list[str]:
    try:
        return [h.strip() for h in HEADING.findall(path.read_text(encoding="utf-8"))]
    except OSError:
        return []


def heading_matches(name: str, pool: list[str]) -> bool:
    """Заголовок може нести номер («## 3. Хуки…») або лапки — звіряємо за входженням."""
    needle = name.strip().lower()
    return any(needle in h.lower() for h in pool)


def section_number_exists(num: str, pool: list[str]) -> bool:
    """§2.7 — це заголовок, чий номер дорівнює 2.7. Саме дорівнює: «2.7» не має
    задовольнятись заголовком «2.70», інакше перевірка пропустить перенумерацію."""
    return any((m := HEADING_NUM.match(h)) and m.group(1) == num for h in pool)


def sources() -> list[Path]:
    out: list[Path] = []
    for g in SOURCE_GLOBS:
        out += sorted(ROOT.glob(g))
    out += [ROOT / f for f in SOURCE_FILES if (ROOT / f).exists()]
    return out


def check_file_lines(text: str, rel: Path, broken: list[str]) -> tuple[int, int]:
    """Перевірка 4. Повертає (перевірено, пропущено-як-неоднозначні)."""
    checked = skipped = 0
    for m in FILE_LINE.finditer(text):
        name = m.group("file")
        # Шлях у посиланні (src/transport/DeviceSession.cpp:436) — беремо його як є.
        prefix = text[max(0, m.start() - 120):m.start()]
        path_m = re.search(r"([A-Za-z_][\w./+-]*/)$", prefix)
        target: Path | None = None
        if path_m:
            cand = ROOT / (path_m.group(1) + name)
            if cand.exists():
                target = cand
        if target is None:
            hits = file_index().get(name, [])
            if len(hits) == 1:
                target = hits[0]
            elif len(hits) > 1:
                # Однакове ім'я в кількох місцях — який саме мався на увазі, не видно.
                skipped += 1
                continue
            else:
                # Файла з таким іменем у репо немає взагалі. Для НАШИХ розширень це
                # майже напевно перейменований/видалений файл, а не чужий репозиторій.
                checked += 1
                broken.append(f"{rel}: немає файла `{name}` (посилання `{m.group(0)}`)")
                continue
        checked += 1
        last = line_count(target)
        want = int(m.group("line2") or m.group("line"))
        if want > last:
            broken.append(
                f"{rel}: `{m.group(0)}` — у файлі лише {last} рядків "
                f"({target.relative_to(ROOT).as_posix()})"
            )
    return checked, skipped


def main() -> int:
    quiet = "--quiet" in sys.argv
    listing = "--list" in sys.argv
    broken: list[str] = []
    checked_paths = checked_links = checked_sections = 0
    checked_lines = skipped_lines = 0

    for src in sources():
        rel = src.relative_to(ROOT)
        try:
            text = src.read_text(encoding="utf-8")
        except OSError:
            continue

        # 1 + 2. Шляхи репо і маркдаун-лінки.
        for raw in BACKTICKED.findall(text) + MD_LINK.findall(text):
            token = norm(raw)
            if not token or URL.search(token) or PLACEHOLDER.search(token):
                continue
            if HAS_SPACE.search(token):
                continue  # команда або фраза в беквотах
            # `src/foo.cpp:264` — це якір «файл:рядок», його бере перевірка 4.
            token = LINE_SUFFIX.sub("", token)
            if is_repo_path(token):
                if token in KNOWN_REMOVED:
                    continue
                checked_paths += 1
                if not repo_path_exists(token):
                    broken.append(f"{rel}: немає шляху `{token}`")
            elif (
                token.endswith(".md")
                and not is_foreign_path(token)
                and Path(token).name not in EXTERNAL_DOCS
                and not Path(token).name.startswith(EXTERNAL_DOC_PREFIXES)
                and not token.startswith(".")
            ):
                checked_links += 1
                if resolve_doc(token, src) is None:
                    broken.append(f"{rel}: немає дока `{token}`")

        # 3. §-вказівники: якір беремо НАЙБЛИЖЧИЙ перед §, бо в рядку їх буває кілька
        #    («док X, скіл Y § «Розділ»» — розділ належить Y, не X).
        for m in SECTION_AT.finditer(text):
            start = max(0, m.start() - 400)
            head = text[start : m.start()]
            if URL.search(head[-60:]):
                continue
            # «§ «X» нижче/вище» — вказівник на розділ ЦЬОГО ж файла
            if re.match(r"\s*(нижче|вище|тут)\b", text[m.end() : m.end() + 20]):
                pool, where = headings(src), f"`{rel.name}`"
                for name in SECTION_NAME.findall(m.group("sections")):
                    checked_sections += 1
                    if not heading_matches(name, pool):
                        broken.append(f"{rel}: {where} — немає власного розділу «{name}»")
                continue
            anchor = ANCHOR_BEFORE.search(head)
            # match на позиції 0 — вікно, найімовірніше, розрізало токен
            if anchor is not None and anchor.start() == 0 and start > 0:
                anchor = None
            # якір далеко (>120 симв.) або відсутній — це вказівник на власний розділ
            if anchor is None or len(head) - anchor.end() > 120:
                pool, where = headings(src), f"`{rel.name}`"
                for name in SECTION_NAME.findall(m.group("sections")):
                    checked_sections += 1
                    if not heading_matches(name, pool):
                        broken.append(f"{rel}: {where} — немає власного розділу «{name}»")
                continue
            if anchor.group("doc"):
                target = norm(anchor.group("doc"))
                path = resolve_doc(target, src)
                if path is None:
                    broken.append(f"{rel}: немає дока `{target}` для §-вказівника")
                    continue
                pool, where = headings(path), f"`{target}`"
            else:
                skill = anchor.group("skill")
                sdir = ROOT / ".claude/skills" / skill
                if not sdir.is_dir():
                    broken.append(f"{rel}: немає скіла `{skill}`")
                    continue
                pool = [h for f in sorted(sdir.rglob("*.md")) for h in headings(f)]
                where = f"скіл `{skill}`"

            for name in SECTION_NAME.findall(m.group("sections")):
                checked_sections += 1
                if not heading_matches(name, pool):
                    broken.append(f"{rel}: {where} — немає розділу «{name}»")

        # 3-біс. «док.md §N» — наш основний стиль вказівника на розділ.
        for m in DOC_SECTION.finditer(text):
            target = norm(m.group("doc"))
            if Path(target).name in EXTERNAL_DOCS or Path(target).name.startswith(EXTERNAL_DOC_PREFIXES):
                continue
            # Ім'я дока в репо може бути неоднозначним (`uapki.md` є і в architecture/,
            # і в integration-1c/). Беремо ВСІ кандидати: посилання живе, якщо розділ є
            # хоча б в одному з них — інакше гард лаявся б на кожну пару однойменних доків.
            candidates = resolve_doc_all(target, src)
            if not candidates:
                continue  # сам факт відсутності дока вже ловить перевірка 2
            checked_sections += 1
            if not any(section_number_exists(m.group("num"), headings(p)) for p in candidates):
                broken.append(f"{rel}: `{target}` — немає розділу §{m.group('num')}")

        # 4. «файл:рядок».
        c, s = check_file_lines(text, rel, broken)
        checked_lines += c
        skipped_lines += s

    if listing:
        print(
            f"джерел: {len(sources())} | шляхів: {checked_paths} | лінків: {checked_links} | "
            f"розділів: {checked_sections} | файл:рядок: {checked_lines} "
            f"(неоднозначних пропущено: {skipped_lines})"
        )

    if broken:
        for b in broken:
            print(f"  [X] {b}")
        print(f"\n{len(broken)} битих якорів")
        return 1

    if not quiet:
        print(
            f"OK: якорі документації цілі (шляхів {checked_paths}, лінків {checked_links}, "
            f"розділів {checked_sections}, файл:рядок {checked_lines})"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
