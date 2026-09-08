#!/usr/bin/env python3
"""Перевірка ядра на успадкований код AddinTemplate.

Порівнює src/core/AddInNative.{h,cpp} з останнім комітом оригінального автора
(366214d:src/AddInNative.{h,cpp}) і падає, якщо лишився спільний блок >= threshold
рядків. Обґрунтування й допустимі винятки — docs/superpowers/specs/2026-09-07-core-rewrite-design.md §6.
"""
import argparse, difflib, io, subprocess, sys

ORIGIN_REF = "366214d"
PAIRS = [
    ("src/AddInNative.cpp", "src/core/AddInNative.cpp"),
    ("src/AddInNative.h",   "src/core/AddInNative.h"),
]

# Винятки: продиктовані платформою або канонічні. Кожен — з обґрунтуванням.
ALLOW_SUBSTRINGS = [
    "switch (ul_reason_for_call)",   # канонічний каркас DllMain від Microsoft
]

def git_show(ref, path):
    out = subprocess.run(["git", "show", f"{ref}:{path}"],
                         capture_output=True)
    if out.returncode != 0:
        sys.exit(f"не вдалося прочитати {ref}:{path} — репозиторій без історії?")
    return out.stdout.decode("utf-8", errors="ignore").splitlines()

def meaningful(line):
    s = line.strip()
    if len(s) <= 3:                      # порожні, поодинокі дужки
        return False
    if s.startswith(("//", "*", "/*")):  # коментарі
        return False
    if s.startswith("#include"):         # директиви препроцесора
        return False
    return True

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--threshold", type=int, default=5)
    ap.add_argument("--report", default=None)
    args = ap.parse_args()

    findings, lines_out = [], []
    for old_path, new_path in PAIRS:
        old = git_show(ORIGIN_REF, old_path)
        new = io.open(new_path, encoding="utf-8", errors="ignore").read().splitlines()
        sm = difflib.SequenceMatcher(None, old, new, autojunk=False)
        for b in sm.get_matching_blocks():
            if b.size < args.threshold:
                continue
            seg = old[b.a:b.a + b.size]
            body = [l for l in seg if meaningful(l)]
            if len(body) < args.threshold:
                continue
            if any(any(sub in l for sub in ALLOW_SUBSTRINGS) for l in seg):
                continue
            findings.append((new_path, b.b + 1, b.size, body[0].strip()[:70]))

    for path, line, size, head in sorted(findings, key=lambda x: -x[2]):
        lines_out.append(f"{path}:{line}  {size} рядків  | {head}")

    text = "\n".join(lines_out) if lines_out else "чисто: успадкованих блоків не знайдено"
    print(text)
    if args.report:
        io.open(args.report, "w", encoding="utf-8", newline="\n").write(text + "\n")
    return 1 if findings else 0

if __name__ == "__main__":
    sys.exit(main())
