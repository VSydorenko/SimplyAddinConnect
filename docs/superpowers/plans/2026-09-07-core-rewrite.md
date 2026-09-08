# Переписування ядра `AddInNative` — план імплементації

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Замінити успадкований код `src/core/AddInNative.{h,cpp}` власною реалізацією
так, щоб публічний контракт для компонент і поведінка в 1С не змінилися.

**Architecture:** Ядро — міст між SDK 1С (`IComponentBase`) і компонентами-фасадами.
Переписуємо реалізацію, зберігаючи сигнатури, продиктовані платформою. Ключ до
«справді власного» коду — інша **структура даних**: замість вектора дескрипторів із
вкладеними векторами імен і лінійним пошуком — плоский вектор + `unordered_map`
індексу імен.

**Tech Stack:** C++17, MSVC 2022 (VS BuildTools), CMake ≥3.16, Windows-only,
`/utf-8`. Жодних нових залежностей.

**Spec:** `docs/superpowers/specs/2026-09-07-core-rewrite-design.md` — читати
**перед** початком, там доказова база, класифікація коду й критерій приймання.

## Global Constraints

- **Публічний контракт незмінний.** `VH`, `ParamSpec`, `AddFunction`, `AddProcedure`,
  `AddProperty`, `Ret`, `result`, `REGISTER_COMPONENT`, `AddError`,
  `PostExternalEvent`, `EnableLogging` — імена, сигнатури й семантика зберігаються.
- **Заборонено редагувати** `src/components/**`, `src/drivers/**`, `src/helpers/**`,
  `src/platform/**`, `src/transport/**`. Якщо здається, що правка там потрібна —
  зупинитись і повідомити: це означає зламаний контракт.
- **Рівно три експорти DLL:** `GetClassObject`, `DestroyObject`, `GetClassNames`.
- **PCH:** `#include "../core/pch.h"` першим рядком у кожному `.cpp`; у `.h` — ніколи.
- **Логування лише через макроси** `REPORT_*` / `NEUTRAL_REPORT_*` із `ServiceTools.h`;
  прямий `spdlog` заборонено. Повідомлення — конкатенацією, з великої літери, без
  крапки в кінці.
- **C++17**, без підняття стандарту. Без нових сабмодулів.
- **Кириличні `u"..."`-літерали** імен методів зберігати дослівно — від них залежить
  виклик з 1С.
- **Не редагувати `version.h`** — його перегенеровує `build_project.ps1`.
- **Гейт наприкінці:** x64 `PASS=29 / FAIL=0 / SKIP=0`, x86 `PASS=28 / FAIL=0 / SKIP=1`.

### Команди, які знадобляться в кожній задачі

```powershell
# Швидка збірка для ітерацій по ядру (БЕЗ UAPKI — набагато швидше)
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests

# Харнес ядра (основна сітка цього плану)
.\bin\Release\core_selftest_x64.exe

# Швидкий гейт без крипто-стека
powershell -File run_tests.ps1 -NoUapki x64
```

`run_tests.ps1` **не перезбирає** проєкт — після кожної правки спершу `build_project.ps1`.

---

### Task 1: Інструмент перевірки «чистоти» ядра

Без нього немає об'єктивного критерію завершення. Зараз він має **падати** —
це і є baseline.

**Files:**
- Create: `tools/check_core_heritage.py`
- Create: `tools/README.md`

**Interfaces:**
- Produces: скрипт CLI `python tools/check_core_heritage.py [--threshold N] [--report PATH]`;
  exit `0` — чисто, exit `1` — знайдено успадковані блоки.

- [ ] **Step 1: Написати скрипт**

```python
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
```

- [ ] **Step 2: Запустити — має ПАДАТИ (це baseline)**

Run: `python tools/check_core_heritage.py --report tools/heritage-baseline.txt`
Expected: exit `1`, у списку — десятки блоків, найбільші близько
`src/core/AddInNative.cpp:500` (37 рядків) і `:296` (36 рядків).

- [ ] **Step 3: Перевірити, що поріг не «самообманний»**

Run: `python tools/check_core_heritage.py --threshold 3`
Expected: блоків більше, ніж при `--threshold 5`. Якщо однаково — скрипт неправильно
рахує; полагодити перш ніж рухатись далі.

- [ ] **Step 4: Описати інструмент**

Створити `tools/README.md`:

```markdown
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
```

- [ ] **Step 5: Коміт**

```bash
git add tools/check_core_heritage.py tools/README.md tools/heritage-baseline.txt
git commit -m "tools: перевірка ядра на успадкований код (критерій приймання переписування)"
```

---

### Task 2: Закрити прогалини в тест-сітці ДО переписування

`core_selftest` покриває ядро (17 функцій, 70 CHECK), але не перевіряє саме те, що
найлегше зламати при переписуванні. Ці тести пишуться **проти чинного коду** й мають
пройти одразу — вони фіксують поточну поведінку як еталон.

**Files:**
- Modify: `tests/core_selftest.cpp` (додати функції + реєстрацію в `main`)

**Interfaces:**
- Consumes: наявні хелпери харнесу — `CHECK(cond, msg)`, `RunGuarded(name, fn)`,
  мок платформи 1С. Подивитись, як влаштований `TestSmokeLifecycle` (рядок ~59), і
  повторити той самий спосіб створення компоненти.
- Produces: 5 нових тест-функцій, зареєстрованих у `main`.

- [ ] **Step 1: Написати тести на конвертації рядків**

```cpp
// Межові дані конвертацій: саме тут ламаються самописні реалізації.
// Порожній рядок, кирилиця, символи поза BMP (сурогатна пара), вбудований \0.
static void TestStringConversionEdges() {
    // Порожній рядок в обидва боки
    CHECK(AddInNative::WCHAR2MB(u"").empty(), "WCHAR2MB: порожній -> порожній");
    CHECK(AddInNative::MB2WCHAR("").empty(),  "MB2WCHAR: порожній -> порожній");

    // Кирилиця: round-trip мусить бути точним
    const std::u16string ua = u"Підпис ЕЦП";
    const std::string    u8 = AddInNative::WCHAR2MB(ua);
    CHECK(AddInNative::MB2WCHAR(u8) == ua, "Round-trip кирилиці точний");

    // Поза BMP: U+1F600 — сурогатна пара в UTF-16, 4 байти в UTF-8
    const std::u16string emoji = u"\xD83D\xDE00";
    const std::string    e8    = AddInNative::WCHAR2MB(emoji);
    CHECK(e8.size() == 4, "Символ поза BMP -> 4 байти UTF-8");
    CHECK(AddInNative::MB2WCHAR(e8) == emoji, "Round-trip поза BMP точний");

    // Вбудований \0 не має обрізати рядок
    std::u16string withNul = u"a";
    withNul.push_back(u'\0');
    withNul.push_back(u'b');
    CHECK(AddInNative::WCHAR2MB(withNul).size() == 3, "Вбудований NUL не обрізає");
}
```

- [ ] **Step 2: Написати тест регістронезалежного пошуку**

```cpp
// Пошук методу за іменем МУСИТЬ бути регістронезалежним в обох мовах —
// 1С кличе так, як написав прикладний розробник.
static void TestCaseInsensitiveLookup() {
    class Probe : public AddInNative {
    public:
        Probe() { AddFunction(u"DoWork", u"Работа", Ret([]() { return int64_t(1); })); }
    };
    Probe p;
    CHECK(p.FindMethod((const WCHAR_T*)u"DoWork") >= 0, "Точний збіг EN");
    CHECK(p.FindMethod((const WCHAR_T*)u"dowork") >= 0, "Нижній регістр EN");
    CHECK(p.FindMethod((const WCHAR_T*)u"DOWORK") >= 0, "Верхній регістр EN");
    CHECK(p.FindMethod((const WCHAR_T*)u"Работа") >= 0, "Точний збіг RU");
    CHECK(p.FindMethod((const WCHAR_T*)u"работа") >= 0, "Нижній регістр RU");
    CHECK(p.FindMethod((const WCHAR_T*)u"РАБОТА") >= 0, "Верхній регістр RU");
    CHECK(p.FindMethod((const WCHAR_T*)u"NoSuchMethod") == -1, "Неіснуючий -> -1");
}
```

- [ ] **Step 3: Написати тест дефолтів параметрів**

```cpp
// GetParamDefValue живить механізм необов'язкових параметрів 1С.
// Перевіряємо кожен тип дефолту, бо переписування зачіпає саме розбір.
static void TestParamDefaultsAllTypes() {
    class Probe : public AddInNative {
    public:
        Probe() {
            AddFunction(u"F", u"Ф", Ret([](VH a, VH b, VH c, VH d) { return int64_t(0); }),
                        { {0, DefaultHelper(u"text")}, {1, DefaultHelper(int64_t(42))},
                          {2, DefaultHelper(3.5)},     {3, DefaultHelper(true)} });
        }
    };
    Probe p;
    const long m = p.FindMethod((const WCHAR_T*)u"F");
    CHECK(m >= 0, "Метод зареєстровано");
    CHECK(p.GetNParams(m) == 4, "Арність 4");

    tVariant v; memset(&v, 0, sizeof(v));
    CHECK(p.GetParamDefValue(m, 1, &v), "Дефолт int читається");
    CHECK(TV_INT(&v) == 42, "Дефолт int == 42");

    memset(&v, 0, sizeof(v));
    CHECK(p.GetParamDefValue(m, 3, &v), "Дефолт bool читається");
    CHECK(TV_BOOL(&v) == true, "Дефолт bool == true");
}
```

- [ ] **Step 4: Написати тест прав доступу до властивостей**

```cpp
// Властивість лише для читання не має приймати запис, і навпаки.
static void TestPropertyAccessFlags() {
    class Probe : public AddInNative {
    public:
        std::u16string stored = u"init";
        Probe() {
            AddProperty(u"ReadOnly", u"ТолькоЧтение",
                        [&](VH v) { v = this->stored; });                  // без сетера
            AddProperty(u"ReadWrite", u"ЧтениеЗапись",
                        [&](VH v) { v = this->stored; },
                        [&](VH v) { this->stored = (std::u16string)v; });
        }
    };
    Probe p;
    const long ro = p.FindProp((const WCHAR_T*)u"ReadOnly");
    const long rw = p.FindProp((const WCHAR_T*)u"ReadWrite");
    CHECK(ro >= 0 && rw >= 0, "Обидві властивості знайдено");
    CHECK(p.IsPropReadable(ro),  "ReadOnly читається");
    CHECK(!p.IsPropWritable(ro), "ReadOnly НЕ пишеться");
    CHECK(p.IsPropReadable(rw),  "ReadWrite читається");
    CHECK(p.IsPropWritable(rw),  "ReadWrite пишеться");
    CHECK(p.FindProp((const WCHAR_T*)u"NoSuchProp") == -1, "Неіснуюча -> -1");
}
```

- [ ] **Step 5: Написати тест імен, що повертаються в 1С**

```cpp
// GetPropName/GetMethodName віддають пам'ять, виділену менеджером 1С.
// Перевіряємо і вміст, і те, що обидві мови доступні за індексом.
static void TestNamesByIndex() {
    class Probe : public AddInNative {
    public:
        Probe() { AddFunction(u"Alpha", u"Альфа", Ret([]() { return int64_t(1); })); }
    };
    Probe p;
    const long m = p.FindMethod((const WCHAR_T*)u"Alpha");
    const WCHAR_T* en = p.GetMethodName(m, 0);
    const WCHAR_T* ru = p.GetMethodName(m, 1);
    CHECK(en != nullptr && ru != nullptr, "Обидва імені віддані");
    CHECK(std::u16string((const char16_t*)en) == u"Alpha", "EN-ім'я збігається");
    CHECK(std::u16string((const char16_t*)ru) == u"Альфа", "RU-ім'я збігається");
}
```

- [ ] **Step 6: Зареєструвати тести в `main`**

Знайти в `tests/core_selftest.cpp` блок викликів `RunGuarded(...)` і додати:

```cpp
    RunGuarded("TestStringConversionEdges", TestStringConversionEdges);
    RunGuarded("TestCaseInsensitiveLookup", TestCaseInsensitiveLookup);
    RunGuarded("TestParamDefaultsAllTypes", TestParamDefaultsAllTypes);
    RunGuarded("TestPropertyAccessFlags", TestPropertyAccessFlags);
    RunGuarded("TestNamesByIndex", TestNamesByIndex);
```

- [ ] **Step 7: Зібрати й запустити — усі мають пройти на ЧИННОМУ коді**

Run:
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
.\bin\Release\core_selftest_x64.exe
```
Expected: `=== OK (failed: 0) ===`, кількість CHECK зросла з 70 приблизно до 95.

Якщо якийсь тест падає — це **не привід його послаблювати**: він виявив реальну
розбіжність очікування й поведінки. Розібратись, виправити тест під фактичну
семантику й зафіксувати це в коміті.

- [ ] **Step 8: Коміт**

```bash
git add tests/core_selftest.cpp
git commit -m "test(core): характеристичні тести межових випадків перед переписуванням ядра"
```

---

### Task 3: Нова модель реєстру властивостей і методів

Серце переписування. Замінюємо успадковану структуру (вектор дескрипторів із
вкладеним вектором імен + лінійний пошук) на плоский вектор + індекс-мапу.

**Files:**
- Modify: `src/core/AddInNative.h` — `struct Prop`, `struct Meth`, поля-члени
- Modify: `src/core/AddInNative.cpp` — `AddProperty`, `AddProcedure`, `AddFunction`,
  `FindProp`, `FindMethod`, `GetNProps`, `GetNMethods`, `GetPropName`, `GetMethodName`

**Interfaces:**
- Consumes: `PropFunction`, `MethFunction`, `MethDefaults`, `ParamSpec` (не змінювати).
- Produces: приватні `struct PropDesc`, `struct MethDesc`, індекс
  `std::unordered_map<std::u16string, long> propIndex_, methIndex_`, хелпер
  `static std::u16string NormalizeName(std::u16string_view)`.

- [ ] **Step 1: Оголосити нову модель у заголовку**

У `src/core/AddInNative.h` замінити приватні `struct Prop; struct Meth;` і поля
`std::vector<Prop> properties; std::vector<Meth> methods;` на:

```cpp
private:
    // Дескриптор властивості: імена лежать ОКРЕМО в індексі, тут — лише дані.
    struct PropDesc {
        std::u16string nameEn;
        std::u16string nameRu;
        PropFunction   getter;
        PropFunction   setter;   // порожній -> властивість лише для читання
    };

    // Дескриптор методу. hasRetVal розрізняє функцію і процедуру для 1С.
    struct MethDesc {
        std::u16string nameEn;
        std::u16string nameRu;
        MethFunction   handler;
        MethDefaults   defaults;
        std::vector<ParamSpec> params;
        bool           hasRetVal = false;
    };

    std::vector<PropDesc> props_;
    std::vector<MethDesc> meths_;

    // Індекс імен -> позиція в векторі. Ключ нормалізований (верхній регістр),
    // обидві мови кладуться окремими ключами. Дає O(1) пошук замість обходу
    // вкладених векторів.
    std::unordered_map<std::u16string, long> propIndex_;
    std::unordered_map<std::u16string, long> methIndex_;

    // Нормалізація імені для індексу: верхній регістр для латиниці й кирилиці.
    static std::u16string NormalizeName(std::u16string_view name);
```

Додати `#include <unordered_map>` і `#include <string_view>` до наявних інклюдів.

`VariantHelper` посилається на `Prop*`/`Meth*` — тимчасово замінити на
`PropDesc*`/`MethDesc*`; повністю цей клас переписується в Задачі 4.

- [ ] **Step 2: Реалізувати нормалізацію імен**

У `src/core/AddInNative.cpp`:

```cpp
// Нормалізація для індексу імен. Латиниця — через ASCII-зсув; кирилиця —
// через таблицю діапазонів UTF-16, бо std::towupper залежить від локалі,
// а компонента мусить поводитись однаково незалежно від налаштувань машини.
std::u16string AddInNative::NormalizeName(std::u16string_view name) {
    std::u16string out;
    out.reserve(name.size());
    for (char16_t c : name) {
        if (c >= u'a' && c <= u'z')                 c = char16_t(c - u'a' + u'A');
        else if (c >= 0x0430 && c <= 0x044F)        c = char16_t(c - 0x20);   // а-я -> А-Я
        else if (c == 0x0451)                       c = 0x0401;               // ё -> Ё
        else if (c >= 0x0450 && c <= 0x045F)        c = char16_t(c - 0x50);   // ѐ-џ -> Ѐ-Џ (і, ї, є, ґ)
        out.push_back(c);
    }
    return out;
}
```

- [ ] **Step 3: Переписати реєстрацію**

```cpp
void AddInNative::AddProperty(const std::u16string& nameEn, const std::u16string& nameRu,
                              const PropFunction& getter, const PropFunction& setter) {
    const long pos = static_cast<long>(props_.size());
    props_.push_back(PropDesc{ nameEn, nameRu, getter, setter });
    propIndex_[NormalizeName(nameEn)] = pos;
    if (!nameRu.empty()) propIndex_[NormalizeName(nameRu)] = pos;
}

void AddInNative::AddProcedure(const std::u16string& nameEn, const std::u16string& nameRu,
                               const MethFunction& handler, const MethDefaults& defs) {
    RegisterMethod(nameEn, nameRu, handler, defs, {}, /*hasRetVal=*/false);
}

void AddInNative::AddFunction(const std::u16string& nameEn, const std::u16string& nameRu,
                              const MethFunction& handler, const MethDefaults& defs) {
    RegisterMethod(nameEn, nameRu, handler, defs, {}, /*hasRetVal=*/true);
}

// Спільна точка реєстрації — щоб індекс наповнювався в одному місці.
void AddInNative::RegisterMethod(const std::u16string& nameEn, const std::u16string& nameRu,
                                 const MethFunction& handler, const MethDefaults& defs,
                                 const std::vector<ParamSpec>& params, bool hasRetVal) {
    const long pos = static_cast<long>(meths_.size());
    meths_.push_back(MethDesc{ nameEn, nameRu, handler, defs, params, hasRetVal });
    methIndex_[NormalizeName(nameEn)] = pos;
    if (!nameRu.empty()) methIndex_[NormalizeName(nameRu)] = pos;
}
```

Оголосити `RegisterMethod` у приватній секції `.h`. **Перевантаження
`AddFunction`/`AddProcedure` з `const std::vector<ParamSpec>&` зберегти** — вони
частина контракту (105 входжень `ParamSpec` у компонентах); вони теж викликають
`RegisterMethod`, передаючи `params`.

- [ ] **Step 4: Переписати пошук і лічильники**

```cpp
long AddInNative::GetNProps()   { return static_cast<long>(props_.size()); }
long AddInNative::GetNMethods() { return static_cast<long>(meths_.size()); }

long AddInNative::FindProp(const WCHAR_T* wsPropName) {
    if (!wsPropName) return -1;
    const auto it = propIndex_.find(NormalizeName(
        std::u16string(reinterpret_cast<const char16_t*>(wsPropName))));
    return (it == propIndex_.end()) ? -1 : it->second;
}

long AddInNative::FindMethod(const WCHAR_T* wsMethodName) {
    if (!wsMethodName) return -1;
    const auto it = methIndex_.find(NormalizeName(
        std::u16string(reinterpret_cast<const char16_t*>(wsMethodName))));
    return (it == methIndex_.end()) ? -1 : it->second;
}
```

- [ ] **Step 5: Переписати віддачу імен**

```cpp
// Пам'ять під рядок виділяє МЕНЕДЖЕР 1С — інакше платформа не зможе її звільнити.
const WCHAR_T* AddInNative::GetPropName(long lPropNum, long lPropAlias) {
    if (lPropNum < 0 || lPropNum >= static_cast<long>(props_.size())) return nullptr;
    const PropDesc& p = props_[lPropNum];
    const std::u16string& src = (lPropAlias == 0 || p.nameRu.empty()) ? p.nameEn : p.nameRu;
    return AllocString(src);
}

const WCHAR_T* AddInNative::GetMethodName(long lMethodNum, long lMethodAlias) {
    if (lMethodNum < 0 || lMethodNum >= static_cast<long>(meths_.size())) return nullptr;
    const MethDesc& m = meths_[lMethodNum];
    const std::u16string& src = (lMethodAlias == 0 || m.nameRu.empty()) ? m.nameEn : m.nameRu;
    return AllocString(src);
}

// Копія рядка в пам'яті менеджера 1С. nullptr, якщо менеджера ще немає.
WCHAR_T* AddInNative::AllocString(const std::u16string& src) {
    if (!m_iMemory) return nullptr;
    WCHAR_T* dst = nullptr;
    const size_t bytes = (src.size() + 1) * sizeof(WCHAR_T);
    if (!m_iMemory->AllocMemory(reinterpret_cast<void**>(&dst), static_cast<unsigned long>(bytes)))
        return nullptr;
    memcpy(dst, src.c_str(), bytes);
    return dst;
}
```

Оголосити `AllocString` у приватній секції `.h`.

- [ ] **Step 6: Зібрати й прогнати харнес**

Run:
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
.\bin\Release\core_selftest_x64.exe
```
Expected: `=== OK (failed: 0) ===`. Особливо мають пройти `TestCaseInsensitiveLookup`,
`TestNamesByIndex`, `TestIndexHardening`, `TestComponentRegistry`.

Якщо падає `TestIndexHardening` — перевірити межові індекси: усі `Get*` мусять
віддавати `nullptr`/`false` замість звернення за межі вектора.

- [ ] **Step 7: Коміт**

```bash
git add src/core/AddInNative.h src/core/AddInNative.cpp
git commit -m "refactor(core): власна модель реєстру — плоскі дескриптори + індекс імен"
```

---

### Task 4: Власний `VariantHelper`

**Files:**
- Modify: `src/core/AddInNative.h` — тіло класу `VariantHelper`
- Modify: `src/core/AddInNative.cpp` — реалізації операторів

**Interfaces:**
- Consumes: `tVariant`, `TYPEVAR` із `include/types.h`; `AllocString` із Задачі 3.
- Produces: `VariantHelper` із тим самим публічним набором операторів (контракт —
  117 входжень `VH` у компонентах), але з явними `Get<T>()`/`Set<T>()` усередині.

- [ ] **Step 1: Переписати клас у заголовку**

Публічний набір (`operator=` для `std::string`/`std::wstring`/`std::u16string`/
`int64_t`/`double`/`bool`, `operator T()` для тих самих, `size()`, `type()`,
`data()`, `clear()`, `AllocMemory()`) **зберегти дослівно** — його викликають
компоненти. Змінити внутрішній устрій:

```cpp
class VariantHelper {
private:
    tVariant*    pvar  = nullptr;
    AddInNative* addin = nullptr;

public:
    VariantHelper(tVariant* pvar, AddInNative* addin) : pvar(pvar), addin(addin) {}
    VariantHelper(const VariantHelper&) = default;

    // Ядро адаптера: два явні методи, через які йде ВСЯ робота з tVariant.
    // Оператори нижче — тонкі обгортки над ними.
    template <typename T> T    Get() const;
    template <typename T> void Set(const T& value);

    void     AllocMemory(unsigned long size);
    uint32_t size();
    TYPEVAR  type();
    char*    data();
    void     clear();

    VariantHelper& operator=(const std::string& v)    { Set(v); return *this; }
    VariantHelper& operator=(const std::wstring& v)   { Set(v); return *this; }
    VariantHelper& operator=(const std::u16string& v) { Set(v); return *this; }
    VariantHelper& operator=(int64_t v)               { Set(v); return *this; }
    VariantHelper& operator=(double v)                { Set(v); return *this; }
    VariantHelper& operator=(bool v)                  { Set(v); return *this; }

    operator std::string()    const { return Get<std::string>(); }
    operator std::wstring()   const { return Get<std::wstring>(); }
    operator std::u16string() const { return Get<std::u16string>(); }
    operator int64_t()        const { return Get<int64_t>(); }
    operator double()         const { return Get<double>(); }
    operator bool()           const { return Get<bool>(); }
    operator int()            const { return static_cast<int>(Get<int64_t>()); }

private:
    std::exception TypeError(TYPEVAR expected) const;
};
```

**Увага:** якщо чинний `VariantHelper` має додаткові члени (`prop`, `meth`,
`number`) і вони десь використовуються — з'ясувати де (`grep -rn "\.number\|->number" src/`)
і зберегти лише те, що справді потрібне. Порожні поля не переносити.

- [ ] **Step 2: Реалізувати `Get`/`Set` спеціалізаціями**

У `.cpp` — по одній спеціалізації на тип. Приклад для двох:

```cpp
template <> std::u16string AddInNative::VariantHelper::Get<std::u16string>() const {
    if (!pvar) return std::u16string();
    switch (TV_VT(pvar)) {
    case VTYPE_PWSTR:
        return std::u16string(reinterpret_cast<const char16_t*>(pvar->pwstrVal), pvar->wstrLen);
    case VTYPE_EMPTY:
        return std::u16string();
    default:
        throw TypeError(VTYPE_PWSTR);
    }
}

template <> void AddInNative::VariantHelper::Set<std::u16string>(const std::u16string& value) {
    if (!pvar || !addin) return;
    // Рядок для 1С — лише в пам'яті її менеджера.
    WCHAR_T* dst = addin->AllocString(value);
    if (!dst) return;
    TV_VT(pvar)      = VTYPE_PWSTR;
    pvar->pwstrVal   = dst;
    pvar->wstrLen    = static_cast<uint32_t>(value.size());
}
```

Решта типів — за тим самим зразком: `int64_t` → `VTYPE_I4`/`VTYPE_I8`,
`double` → `VTYPE_R8`, `bool` → `VTYPE_BOOL`, `std::string`/`std::wstring` —
через конвертації із Задачі 5.

- [ ] **Step 3: Зібрати й прогнати харнес**

Run:
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
.\bin\Release\core_selftest_x64.exe
```
Expected: `=== OK (failed: 0) ===`; критичні — `TestRetConvention`,
`TestRetViaCallAsProc`, `TestWideArity`.

- [ ] **Step 4: Коміт**

```bash
git add src/core/AddInNative.h src/core/AddInNative.cpp
git commit -m "refactor(core): власний VariantHelper на явних Get/Set"
```

---

### Task 5: Конвертації рядків через WinAPI

**Files:**
- Modify: `src/core/AddInNative.cpp` — `WCHAR2MB`, `MB2WCHAR`, `WCHAR2WC`, `upper`

**Interfaces:**
- Produces: ті самі статичні методи з тими самими сигнатурами (їх обгортає
  `ServiceTools::SafeMB2WCHAR` / `SafeWCHAR2MB`, які чіпати не можна).

- [ ] **Step 1: Переписати на `MultiByteToWideChar` / `WideCharToMultiByte`**

`std::wstring_convert` — депрекейтед з C++17 і саме він зараз успадкований.
Проєкт Windows-only, тож переходимо на WinAPI:

```cpp
std::string AddInNative::WCHAR2MB(std::basic_string_view<WCHAR_T> src) {
    if (src.empty()) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0,
        reinterpret_cast<const wchar_t*>(src.data()), static_cast<int>(src.size()),
        nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0,
        reinterpret_cast<const wchar_t*>(src.data()), static_cast<int>(src.size()),
        out.data(), need, nullptr, nullptr);
    return out;
}

std::u16string AddInNative::MB2WCHAR(std::string_view src) {
    if (src.empty()) return std::u16string();
    const int need = ::MultiByteToWideChar(CP_UTF8, 0,
        src.data(), static_cast<int>(src.size()), nullptr, 0);
    if (need <= 0) return std::u16string();
    std::u16string out(static_cast<size_t>(need), u'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, src.data(), static_cast<int>(src.size()),
        reinterpret_cast<wchar_t*>(out.data()), need);
    return out;
}
```

**Важливо:** довжину передаємо явно (`src.size()`), а не `-1`. Саме це зберігає
вбудований `\0` — інакше рядок обрізається на першому нулі, і `TestStringConversionEdges`
це зловить.

`WCHAR2WC` — тривіальна перекодировка `char16_t` → `wchar_t` (на Windows обидва
2-байтні): скопіювати поелементно, без конвертації кодувань.

`upper` — реалізувати через `NormalizeName` із Задачі 3, щоб логіка регістру жила
в одному місці.

- [ ] **Step 2: Зібрати й прогнати**

Run:
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
.\bin\Release\core_selftest_x64.exe
```
Expected: `=== OK (failed: 0) ===`; критичний — `TestStringConversionEdges`
(усі 6 CHECK, включно з поза-BMP і вбудованим NUL).

- [ ] **Step 3: Коміт**

```bash
git add src/core/AddInNative.cpp
git commit -m "refactor(core): конвертації рядків через WinAPI замість deprecated wstring_convert"
```

---

### Task 6: Виклики методів, параметри та властивості

**Files:**
- Modify: `src/core/AddInNative.cpp` — `GetPropVal`, `SetPropVal`, `IsPropReadable`,
  `IsPropWritable`, `GetNParams`, `GetParamDefValue`, `HasRetVal`, `CallAsProc`,
  `CallAsFunc`, приватний `CallMethod`

**Interfaces:**
- Consumes: `props_`, `meths_` (Задача 3), `VariantHelper` (Задача 4),
  `ParamSpec`/`DefaultHelper` (наш наявний код — **не переписувати**).
- Produces: ті самі методи `IComponentBase`.

- [ ] **Step 1: Переписати доступ до властивостей**

```cpp
bool AddInNative::IsPropReadable(long n) {
    return n >= 0 && n < static_cast<long>(props_.size()) && bool(props_[n].getter);
}

bool AddInNative::IsPropWritable(long n) {
    return n >= 0 && n < static_cast<long>(props_.size()) && bool(props_[n].setter);
}

bool AddInNative::GetPropVal(const long n, tVariant* pvarPropVal) {
    if (!IsPropReadable(n) || !pvarPropVal) return false;
    try {
        props_[n].getter(VariantHelper(pvarPropVal, this));
        return true;
    }
    catch (const std::exception& e) { AddError(MB2WCHAR(e.what())); return false; }
    catch (...) { return false; }
}

bool AddInNative::SetPropVal(const long n, tVariant* varPropVal) {
    if (!IsPropWritable(n) || !varPropVal) return false;
    try {
        props_[n].setter(VariantHelper(varPropVal, this));
        return true;
    }
    catch (const std::exception& e) { AddError(MB2WCHAR(e.what())); return false; }
    catch (...) { return false; }
}
```

- [ ] **Step 2: Переписати роботу з параметрами**

`GetNParams` бере арність із `std::variant`-індексу `MethFunction`
(`handler.index()` == кількість аргументів, бо `MethFunction0..16` лежать по
порядку — це гарантують `static_assert`-и в `.h`, які **зберегти**).

`GetParamDefValue` читає з `MethDesc::defaults` (тип `MethDefaults` — наш
`std::map<long, DefaultHelper>`), делегуючи розбір `DefaultHelper`. Власного
`std::variant`-розбору тут більше немає.

```cpp
long AddInNative::GetNParams(const long n) {
    if (n < 0 || n >= static_cast<long>(meths_.size())) return 0;
    return static_cast<long>(meths_[n].handler.index());
}

bool AddInNative::GetParamDefValue(const long method, const long param,
                                   tVariant* pvarParamDefValue) {
    if (!pvarParamDefValue) return false;
    TV_VT(pvarParamDefValue) = VTYPE_EMPTY;
    if (method < 0 || method >= static_cast<long>(meths_.size())) return true;
    const auto& defs = meths_[method].defaults;
    const auto it = defs.find(param);
    if (it == defs.end()) return true;
    it->second.Apply(VariantHelper(pvarParamDefValue, this));   // див. крок 3
    return true;
}

bool AddInNative::HasRetVal(const long n) {
    return n >= 0 && n < static_cast<long>(meths_.size()) && meths_[n].hasRetVal;
}
```

- [ ] **Step 3: Перенести розбір дефолту в `DefaultHelper`**

`DefaultHelper` — **наш** клас (`src/core/AddInNative.h`, оголошений до
`AddInNative`). Додати йому метод, який сам кладе значення у `VariantHelper`:

```cpp
    // Записує збережений дефолт у tVariant через адаптер. Порожній дефолт
    // лишає VTYPE_EMPTY — саме так 1С розуміє «параметр не задано».
    void Apply(AddInNative::VariantHelper vh) const;
```

Реалізація — `switch` по `variant.index()` без копіювання чужої структури розбору.
Оскільки `VariantHelper` вкладений у `AddInNative`, оголосити `Apply` після
визначення `AddInNative` (inline у `.h` після класу або в `.cpp`).

- [ ] **Step 4: Переписати диспетчер викликів**

```cpp
// Єдина точка виклику хендлера: розкриває std::variant за арністю й підставляє
// VariantHelper на кожен параметр. Валідація обов'язкових параметрів (ParamSpec)
// відбувається ДО виклику — це наш наявний механізм, не чіпати.
//
// Арність = index() варіанта: MethFunction0..16 лежать у std::variant по порядку,
// що закріплено static_assert-ами в заголовку. Розгортка макросом, щоб 17 гілок
// не розповзалися копіпастом.
bool AddInNative::CallMethod(const MethDesc& m, tVariant* params, long paramCount) {
    const size_t arity = m.handler.index();

    // 1С може передати менше параметрів, ніж арність, лише якщо решта мають
    // дефолти — їх платформа підставляє сама через GetParamDefValue. Якщо
    // масив коротший, читати за його межами не можна.
    if (paramCount < 0 || static_cast<size_t>(paramCount) < arity) {
        AddError(u"Невідповідність кількості параметрів методу " + m.nameEn);
        return false;
    }

    // Аргумент за індексом; params може бути nullptr для арності 0.
    auto A = [&](size_t i) { return VariantHelper(params ? &params[i] : nullptr, this); };

#define SAC_CALL(N, ...)                                   \
    case N:                                                \
        std::get<MethFunction##N>(m.handler)(__VA_ARGS__); \
        return true;

    switch (arity) {
    case 0: std::get<MethFunction0>(m.handler)(); return true;
    SAC_CALL(1,  A(0))
    SAC_CALL(2,  A(0), A(1))
    SAC_CALL(3,  A(0), A(1), A(2))
    SAC_CALL(4,  A(0), A(1), A(2), A(3))
    SAC_CALL(5,  A(0), A(1), A(2), A(3), A(4))
    SAC_CALL(6,  A(0), A(1), A(2), A(3), A(4), A(5))
    SAC_CALL(7,  A(0), A(1), A(2), A(3), A(4), A(5), A(6))
    SAC_CALL(8,  A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7))
    SAC_CALL(9,  A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8))
    SAC_CALL(10, A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9))
    SAC_CALL(11, A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9), A(10))
    SAC_CALL(12, A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9), A(10), A(11))
    SAC_CALL(13, A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9), A(10), A(11), A(12))
    SAC_CALL(14, A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9), A(10), A(11), A(12), A(13))
    SAC_CALL(15, A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9), A(10), A(11), A(12), A(13), A(14))
    SAC_CALL(16, A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9), A(10), A(11), A(12), A(13), A(14), A(15))
    default:
        return false;
    }

#undef SAC_CALL
}

bool AddInNative::CallAsProc(const long n, tVariant* paParams, const long lSizeArray) {
    if (n < 0 || n >= static_cast<long>(meths_.size())) return false;
    try { return CallMethod(meths_[n], paParams, lSizeArray); }
    catch (const std::exception& e) { AddError(MB2WCHAR(e.what())); return false; }
    catch (...) { return false; }
}

bool AddInNative::CallAsFunc(const long n, tVariant* pvarRetValue,
                             tVariant* paParams, const long lSizeArray) {
    if (n < 0 || n >= static_cast<long>(meths_.size())) return false;
    // result вказує на комірку повернення на час виклику; після — відв'язати,
    // інакше хендлер, викликаний як процедура, писатиме у звільнену пам'ять.
    result = VariantHelper(pvarRetValue, this);
    const bool ok = CallAsProc(n, paParams, lSizeArray);
    result = VariantHelper(nullptr, this);
    return ok;
}
```

**Критично:** семантику `result` зберегти точно — на ній стоїть `Ret()` і
конвенція «`Ret()` обгортає лише хендлери, чиє `return`-значення є результатом».
`TestRetConvention` і `TestRetViaCallAsProc` це перевіряють.

- [ ] **Step 5: Зібрати й прогнати повний харнес ядра**

Run:
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
.\bin\Release\core_selftest_x64.exe
```
Expected: `=== OK (failed: 0) ===`, усі ~95 CHECK.

- [ ] **Step 6: Коміт**

```bash
git add src/core/AddInNative.h src/core/AddInNative.cpp
git commit -m "refactor(core): власний диспетчер викликів, параметрів і властивостей"
```

---

### Task 7: Службовий шар — типи, помилки, експорти, реєстр компонент

**Files:**
- Modify: `src/core/AddInNative.cpp` — `typeinfo`, `AddError`, `DllMain`,
  `GetClassObject`, `DestroyObject`, `GetClassNames`, `AddComponent`,
  `CreateObject`, `getComponentNames`, `Init`, `setMemManager`, `GetInfo`, `Done`,
  `RegisterExtensionAs`, `SetLocale`

**Interfaces:**
- Consumes: `components()` — Meyers-singleton (наш, зберегти).
- Produces: ті самі три експорти DLL.

- [ ] **Step 1: Переписати таблицю типів**

Замість успадкованої `typeinfo()` — таблиця, що будується один раз:

Перелік типів — рівно той, який ядро вживає сьогодні (звірено по чинному
`AddInNative.cpp`): `VTYPE_EMPTY`, `VTYPE_I2`, `VTYPE_I4`, `VTYPE_UI1`,
`VTYPE_ERROR`, `VTYPE_R4`, `VTYPE_R8`, `VTYPE_BOOL`, `VTYPE_PSTR`, `VTYPE_PWSTR`,
`VTYPE_DATE`, `VTYPE_TM`, `VTYPE_BLOB`. Нічого не додавати «про запас».

```cpp
// Людські назви типів для повідомлень про помилку. Пара {EN, RU} на тип.
// Цілочисельні різновиди 1С показує користувачеві однаково — «Целое число»;
// це не спрощення, а те, як платформа їх подає у своїх повідомленнях.
static const std::pair<const char16_t*, const char16_t*>* TypeNames(TYPEVAR vt) {
    static const std::map<TYPEVAR, std::pair<const char16_t*, const char16_t*>> table = {
        { VTYPE_EMPTY, { u"Undefined",    u"Неопределено"   } },
        { VTYPE_I2,    { u"Integer",      u"Целое число"    } },
        { VTYPE_I4,    { u"Integer",      u"Целое число"    } },
        { VTYPE_UI1,   { u"Integer",      u"Целое число"    } },
        { VTYPE_ERROR, { u"Integer",      u"Целое число"    } },
        { VTYPE_R4,    { u"Number",       u"Число"          } },
        { VTYPE_R8,    { u"Number",       u"Число"          } },
        { VTYPE_BOOL,  { u"Boolean",      u"Булево"         } },
        { VTYPE_PSTR,  { u"String",       u"Строка"         } },
        { VTYPE_PWSTR, { u"String",       u"Строка"         } },
        { VTYPE_DATE,  { u"Date",         u"Дата"           } },
        { VTYPE_TM,    { u"Date",         u"Дата"           } },
        { VTYPE_BLOB,  { u"BinaryData",   u"ДвоичныеДанные" } },
    };
    const auto it = table.find(vt);
    return (it == table.end()) ? nullptr : &it->second;
}
```

- [ ] **Step 2: Переписати життєвий цикл і експорти**

`Init`, `setMemManager`, `GetInfo`, `Done`, `RegisterExtensionAs`, `SetLocale` —
короткі методи, сигнатури продиктовані SDK. Написати їх заново від сигнатури,
**не дивлячись у поточний файл**, щоб не переформулювати успадкований текст.

`GetInfo` повертає `2000` (версія компонентної технології) — це вимога платформи.

Експорти:

```cpp
long GetClassObject(const WCHAR_T* wsName, IComponentBase** pInterface) {
    if (!pInterface || *pInterface) return 0;
    *pInterface = AddInNative::CreateObject(
        std::u16string(reinterpret_cast<const char16_t*>(wsName)));
    return reinterpret_cast<long>(*pInterface);
}

long DestroyObject(IComponentBase** pInterface) {
    if (!pInterface || !*pInterface) return -1;
    delete *pInterface;
    *pInterface = nullptr;
    return 0;
}

const WCHAR_T* GetClassNames() {
    // Статичний буфер: платформа читає рядок одразу після виклику.
    static const std::u16string names = AddInNative::getComponentNames();
    return reinterpret_cast<const WCHAR_T*>(names.c_str());
}
```

- [ ] **Step 3: Перевірити рівно три експорти**

Run:
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithTests
dumpbin /exports .\bin\Release\SimplyAddinConnectWin_x64.dll | findstr /C:"GetClassObject" /C:"DestroyObject" /C:"GetClassNames"
```
Expected: рівно три рядки. Якщо `dumpbin` не в PATH — узяти з
`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\<ver>\bin\Hostx64\x64\`.

- [ ] **Step 4: Прогнати харнес**

Run: `.\bin\Release\core_selftest_x64.exe`
Expected: `=== OK (failed: 0) ===`; критичні — `TestComponentRegistry`,
`TestRegisterComponentMacro`, `TestSmokeLifecycle`, `TestBootFixes`.

- [ ] **Step 5: Коміт**

```bash
git add src/core/AddInNative.cpp src/core/AddInNative.h
git commit -m "refactor(core): власний службовий шар — типи, життєвий цикл, експорти"
```

---

### Task 8: Верифікація чистоти й повний гейт

**Files:**
- Modify: `tools/check_core_heritage.py` — за потреби додати обґрунтовані винятки

- [ ] **Step 1: Перевірка чистоти — тепер має ПРОЙТИ**

Run: `python tools/check_core_heritage.py`
Expected: `чисто: успадкованих блоків не знайдено`, exit `0`.

Якщо лишились блоки — переписати саме їх. **Не підвищувати поріг.** Якщо блок
справді неможливо написати інакше (продиктований платформою) — додати підрядок у
`ALLOW_SUBSTRINGS` з коментарем, чому саме цей код безальтернативний.

- [ ] **Step 2: Суворіша перевірка**

Run: `python tools/check_core_heritage.py --threshold 3`
Expected: список або порожній, або містить лише короткі безальтернативні
конструкції. Це діагностика, не гейт — але подивитись варто.

- [ ] **Step 3: Компоненти не чіпані**

Run: `git diff --stat main -- src/components src/drivers src/helpers src/platform src/transport`
Expected: **порожньо**. Якщо ні — контракт зламано; зупинитись і повідомити.

- [ ] **Step 4: Повна збірка з UAPKI**

Run: `powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests`
Expected: exit 0, у хвості — 5 файлів і `SimplyAddinConnectWin.zip`.

- [ ] **Step 5: Повний гейт, обидві архітектури**

Run:
```powershell
powershell -File run_tests.ps1 x64
powershell -File run_tests.ps1 x86
```
Expected: x64 `Разом: PASS=29  FAIL/BLOCKED=0  SKIP=0`;
x86 `Разом: PASS=28  FAIL/BLOCKED=0  SKIP=1`.

Будь-яке відхилення — регресія: розбиратись, а не приймати нові числа.

- [ ] **Step 6: Ручна перевірка в 1С**

Автоматика не покриває шар, де ламається `/utf-8`: імена методів у DLL є
англійською, немає кирилицею, і 1С каже «Метод объекта не обнаружен».

1. Підключити `bin/Release/SimplyAddinConnectWin.zip` у 1С як зовнішню компоненту.
2. Створити об'єкт і викликати метод **за кириличним іменем**.
3. Переконатись, що виклик проходить і повертає значення.

Зафіксувати результат у повідомленні коміту.

- [ ] **Step 7: Коміт**

```bash
git add -A
git commit -m "chore(core): верифікація переписаного ядра — чистота, гейт x64/x86, ручний тест у 1С"
```

---

### Task 9: Ліцензування й публікаційні файли

Виконувати **лише після** зеленої Задачі 8.

**Files:**
- Create: `LICENSE`
- Create: `THIRD-PARTY-NOTICES.md`
- Modify: `README.md`
- Modify: `build_project.ps1` — покласти `THIRD-PARTY-NOTICES.md` у ZIP

- [ ] **Step 1: Створити `LICENSE`**

MIT з єдиним копірайтом власника плюс секція-застереження:

```
MIT License

Copyright (c) 2025-2026 Volodymyr Sydorenko

[канонічний текст MIT — узяти дослівно з extern/spdlog/LICENSE,
 замінивши лише рядок Copyright; не переписувати з пам'яті]

---

ЗАСТЕРЕЖЕННЯ ЩОДО СТОРОННІХ КОМПОНЕНТІВ

Ця ліцензія поширюється на код цього репозиторію, АЛЕ НЕ на:

* include/*.h — заголовки SDK зовнішніх компонент 1С:Підприємство
  (AddInDefBase.h, ComponentBase.h, IMemoryManager.h, com.h, types.h).
  Це власність ТОВ «1С»; вони включені для складання й поширюються
  на умовах їхнього правовласника.

* extern/* — сабмодулі зі своїми ліцензіями (див. THIRD-PARTY-NOTICES.md).
```

- [ ] **Step 2: Створити `THIRD-PARTY-NOTICES.md`**

Для кожної залежності — назва, версія, URL, ліцензія й **повний текст**:

| Компонент | Версія | Ліцензія |
|---|---|---|
| spdlog | v1.17.0 | MIT |
| nlohmann/json | v3.12.0 | MIT |
| pugixml | v1.16 | MIT |
| UAPKI | 2.0.17 | BSD 2-Clause |

Тексти брати з `extern/<name>/LICENSE*` — не переказувати своїми словами.

- [ ] **Step 3: Класти файл у поставку**

У `build_project.ps1` знайти крок пакування ZIP і додати
`THIRD-PARTY-NOTICES.md` до списку файлів. UAPKI і spdlog лінкуються **статично**
в DLL — тому їхні ліцензії мусять їхати з бінарником.

- [ ] **Step 4: Актуалізувати `README.md`**

Зараз описано 3 компоненти з 6 реальних. Додати `LabelPrinter`,
`ECRPrivatBPO3004`, `ECRPrivatBPO4000`; у розділі «Ліцензія» дати посилання на
`LICENSE` і `THIRD-PARTY-NOTICES.md`.

- [ ] **Step 5: Перевірити, що ZIP містить нотиси**

Run:
```powershell
powershell -ExecutionPolicy Bypass -File build_project.ps1 -WithUAPKI -WithTests
Expand-Archive -Path .\bin\Release\SimplyAddinConnectWin.zip -DestinationPath $env:TEMP\sac-check -Force
Get-ChildItem $env:TEMP\sac-check
```
Expected: серед файлів — `THIRD-PARTY-NOTICES.md`.

- [ ] **Step 6: Коміт**

```bash
git add LICENSE THIRD-PARTY-NOTICES.md README.md build_project.ps1
git commit -m "docs: MIT-ліцензія, нотиси третіх сторін і актуалізований README"
```

---

## Порядок і залежності задач

```
Task 1 (інструмент) ─┐
Task 2 (тест-сітка) ─┴─> Task 3 (реєстр) ─> Task 4 (VariantHelper) ─> Task 5 (рядки)
                                                                          │
                          Task 8 (верифікація) <─ Task 7 (службове) <─ Task 6 (виклики)
                                   │
                                   └─> Task 9 (ліцензування)
```

Задачі 1 і 2 незалежні між собою — можна в будь-якому порядку, але **обидві до**
Задачі 3. Задачі 3–7 строго послідовні: кожна спирається на структури попередньої.

## Що робити, якщо щось пішло не так

- **`core_selftest` падає після задачі** — не рухатись далі. Тест описує поведінку,
  на яку спираються компоненти; полагодити реалізацію, а не тест.
- **Знадобилось правити `src/components/`** — зупинитись і повідомити. Це означає
  зламаний публічний контракт (Global Constraints).
- **1С каже «Метод объекта не обнаружен»** — перевірити `/utf-8` для
  `base_component` у `CMake/compiler_settings.cmake`: перелік там поіменний, і
  без прапорця кириличні літерали мовчки псуються.
- **`check_core_heritage.py` не проходить після всіх задач** — переписати конкретні
  блоки, які він показує. Підвищення порогу — не рішення.
