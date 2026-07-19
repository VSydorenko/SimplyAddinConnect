# Чернетки PR-ів у upstream specinfo-ua/UAPKI

> Заготовки для кроку D задачі `2026-07-19_uapki_fork_update_and_upstream_pr.md`.
> **НЕ надсилати без явного підтвердження користувача.** Два незалежні PR (настанова
> мейнтейнера Віталія [msg 3595]: атомарні коміти, обʼєднувати лише залежні).
>
> Локальні гілки (у сабмодулі `extern/uapki`, базуються на `upstream/main` = `69053dc`):
> - **PR-A** → гілка `static-export-headers` (1 коміт, 4 файли)
> - **PR-B** → гілка `loadlibraryw-utf8` (1 коміт, 3 файли)

---

## PR-A

**Title:** `build: support static linking via *_STATIC export macros (uapkic/uapkif/uapki)`

**Base:** `specinfo-ua/UAPKI:main` ← `VSydorenko/UAPKI:static-export-headers`

### Body

## Summary

Adds preprocessor support for statically linking `uapkic`/`uapkif`/`uapki` into a
consumer's own binary (DLL/EXE), instead of building them as standalone shared libraries.

### Problem

The `*_EXPORT` macros in the public headers unconditionally resolve to
`__declspec(dllexport)` on Windows when `*_LIBRARY` is defined. When these three libraries
are compiled as translation units directly into a third-party DLL (static embedding, no
separate `uapki*.dll`), every exported symbol leaks into the *consumer's* export table —
polluting it with thousands of internal ASN.1/crypto symbols that were never meant to be
public API of the consumer binary.

The `*_STATIC` defines already exist as a build concept, but the export headers never
branch on them.

### Change

- `library/uapki/include/uapki-export.h`, `library/uapkic/include/uapkic-export.h`,
  `library/uapkif/include/uapkif-export.h`: add `#ifdef UAPKI_STATIC` / `UAPKIC_STATIC` /
  `UAPKIF_STATIC` branches that resolve `*_EXPORT` to nothing (no `dllexport`/`dllimport`),
  falling through to the existing behavior otherwise.
- `library/uapkif/include/asn_system.h`: guard `#define WIN32_LEAN_AND_MEAN` with
  `#ifndef WIN32_LEAN_AND_MEAN` to avoid a redefinition warning when the consumer's own
  build already defines it before including this header (a natural side effect of static
  embedding, where translation units from both codebases share a compilation environment).

### Compatibility

- Purely preprocessor-level; **no runtime/certified crypto code paths are touched**.
- Zero effect on existing shared-library builds: the new branches only activate when a
  consumer explicitly defines `UAPKI_STATIC` / `UAPKIC_STATIC` / `UAPKIF_STATIC`, which no
  existing build does.
- The project already builds some targets in a non-shared configuration (e.g.
  Emscripten/WASM); this change makes the static path explicit and opt-in for the three
  core libraries via their conventional `*_STATIC` macros, instead of relying on
  target-specific handling.

### Testing

Validated by building `uapkic`+`uapkif`+`uapki` as object files linked statically into a
third-party Windows DLL (x86/x64, MSVC/CMake), then exercising SIGN/VERIFY against a UAPKI
CM provider through an L0–L3 test harness (symbol-export invariants → JSON-API scenario
tests → end-to-end host-emulation tests). All SIGN/VERIFY operations pass unchanged with
`*_STATIC` defined; the consumer DLL's export table no longer carries internal UAPKI symbols.

### Independence

Self-contained; does not depend on or conflict with the companion `LoadLibraryW`/UTF-8
provider-loading fix submitted separately.

---

## Підсумок (UA)

Додає підтримку статичного лінкування `uapkic`/`uapkif`/`uapki` у власний бінарник
споживача (DLL/EXE) замість збірки окремими shared-бібліотеками.

### Проблема

Макроси `*_EXPORT` у публічних заголовках беззастережно розгортаються в
`__declspec(dllexport)` на Windows при визначеному `*_LIBRARY`. Коли ці три бібліотеки
компілюються як трансляційні одиниці прямо в стороннє DLL (статичне вбудовування, без
окремого `uapki*.dll`), кожен експортований символ протікає в таблицю експорту *споживача* —
забруднюючи її тисячами внутрішніх ASN.1/крипто-символів, які ніколи не призначались як
публічне API бінарника-споживача.

Дефайни `*_STATIC` уже існують як концепт збірки, але export-заголовки на них не гілкуються.

### Зміна

- `library/uapki/include/uapki-export.h`, `library/uapkic/include/uapkic-export.h`,
  `library/uapkif/include/uapkif-export.h`: додано гілки `#ifdef UAPKI_STATIC` /
  `UAPKIC_STATIC` / `UAPKIF_STATIC`, які розгортають `*_EXPORT` у порожнечу (без
  `dllexport`/`dllimport`), з падінням на попередню поведінку в іншому разі.
- `library/uapkif/include/asn_system.h`: `#define WIN32_LEAN_AND_MEAN` обгорнуто в
  `#ifndef WIN32_LEAN_AND_MEAN`, щоб уникнути попередження повторного визначення, коли
  власна збірка споживача вже визначає цей макрос до підключення заголовка.

### Сумісність

- Суто препроцесорний рівень; **сертифікований/рантайм крипто-код не зачіпається**.
- Нульовий вплив на наявні shared-збірки: нові гілки активуються лише коли споживач явно
  визначає `UAPKI_STATIC` / `UAPKIC_STATIC` / `UAPKIF_STATIC`, чого жодна наявна збірка не
  робить.
- Проєкт уже збирає деякі цілі в не-shared конфігурації (напр. Emscripten/WASM); ця зміна
  робить статичний шлях явним і opt-in для трьох ядрових бібліотек через їхні конвенційні
  макроси `*_STATIC`, замість прив'язки до конкретної цілі.

### Тестування

Перевірено збіркою `uapkic`+`uapkif`+`uapki` як об'єктних файлів, статично залінкованих у
стороннє Windows DLL (x86/x64, MSVC/CMake), із подальшою перевіркою SIGN/VERIFY через
CM-провайдер UAPKI харнесом L0–L3. Усі операції SIGN/VERIFY проходять без змін при
визначеному `*_STATIC`; таблиця експорту DLL-споживача більше не несе внутрішніх символів
UAPKI.

### Незалежність

Самодостатній; не залежить від і не конфліктує з супутнім виправленням завантаження
провайдера через `LoadLibraryW`/UTF-8, поданим окремо.

---

## PR-B

**Title:** `fix(windows): load CM/UAPKI providers via LoadLibraryW to support non-ASCII paths`

**Base:** `specinfo-ua/UAPKI:main` ← `VSydorenko/UAPKI:loadlibraryw-utf8`

### Body

## Summary

Fixes provider loading on Windows when the DLL path contains non-ASCII characters (e.g.
Cyrillic in the Windows username, `%LOCALAPPDATA%\Users\Іван\...`).

### Problem

`CmLoader::load` and `UapkiLoader::load` call `LoadLibraryA` (via `DL_LOAD_LIBRARY`) with a
UTF-8 encoded path string. `LoadLibraryA` interprets its input using the process's active
ANSI code page, not UTF-8. On a non-English Windows install where the active code page isn't
UTF-8, any path segment containing non-ASCII characters (most commonly the Windows username
itself) fails to resolve, and provider loading fails outright. This affects a large share of
Ukrainian users, since Cyrillic usernames are common and Windows still defaults to a
non-UTF-8 ANSI code page in most locales.

### Change

- `library/common/loaders/dl-macros.h`: adds `dl_load_library_utf8(const char*)`. On Windows
  it converts the UTF-8 path to UTF-16 via `MultiByteToWideChar(CP_UTF8, ...)` and calls
  `LoadLibraryW`. On non-Windows platforms it's a plain macro alias to the existing
  `DL_LOAD_LIBRARY` (i.e. `dlopen`) — no behavior change there.
- `library/common/loaders/cm-loader.cpp` (`CmLoader::load`) and
  `library/common/loaders/uapki-loader.cpp` (`UapkiLoader::load`): switched from
  `DL_LOAD_LIBRARY` to `dl_load_library_utf8`.

### Scope

Windows-only code path in `dl-macros.h`; non-Windows builds get an unchanged `dlopen`-based
path via a macro alias, so there is no behavioral change on Linux/Android/etc.

### Alignment with upstream direction

Consistent with the previously discussed plan to have loaders resolve providers relative to
the loaded library's own path (rpath-style resolution) — this fix addresses the same class
of path-handling issue (correct, encoding-safe resolution of provider paths) without
conflicting with that follow-up work; it's a minimal, orthogonal correctness fix for the
current `LoadLibraryA`/UTF-8 mismatch.

### Testing

Reproduced against a synthetic profile path containing Cyrillic characters, confirming
`LoadLibraryA`-based loading fails while `dl_load_library_utf8` succeeds. Validated
end-to-end via an L0–L3 harness: CM provider (`cm-pkcs12`) and core UAPKI loading, followed
by SIGN/VERIFY scenarios, all pass unchanged from both ASCII and non-ASCII installation paths.

### Independence

Self-contained Windows bugfix; does not depend on or conflict with the companion `*_STATIC`
export-header PR submitted separately.

---

## Підсумок (UA)

Виправляє завантаження провайдерів на Windows, коли шлях до DLL містить не-ASCII символи
(наприклад, кирилицю в імені користувача Windows, `%LOCALAPPDATA%\Users\Іван\...`).

### Проблема

`CmLoader::load` та `UapkiLoader::load` викликають `LoadLibraryA` (через `DL_LOAD_LIBRARY`)
зі шляхом у кодуванні UTF-8. `LoadLibraryA` інтерпретує вхідний рядок за активною
ANSI-кодовою сторінкою процесу, а не як UTF-8. На неанглійській збірці Windows, де активна
кодова сторінка не UTF-8, будь-який сегмент шляху з не-ASCII символами (найчастіше — саме
ім'я користувача Windows) не резолвиться, і завантаження провайдера повністю провалюється.
Це зачіпає значну частку українських користувачів, оскільки кириличні імена користувачів
поширені, а Windows у більшості локалей досі за замовчуванням використовує не-UTF-8
ANSI-кодову сторінку.

### Зміна

- `library/common/loaders/dl-macros.h`: додано `dl_load_library_utf8(const char*)`. На
  Windows функція конвертує шлях з UTF-8 у UTF-16 через `MultiByteToWideChar(CP_UTF8, ...)`
  і викликає `LoadLibraryW`. На не-Windows платформах це звичайний макро-аліас на наявний
  `DL_LOAD_LIBRARY` (тобто `dlopen`) — жодних змін поведінки там немає.
- `library/common/loaders/cm-loader.cpp` (`CmLoader::load`) та
  `library/common/loaders/uapki-loader.cpp` (`UapkiLoader::load`): перемкнено з
  `DL_LOAD_LIBRARY` на `dl_load_library_utf8`.

### Область дії

Лише Windows-гілка в `dl-macros.h`; на не-Windows збірках шлях на основі `dlopen`
лишається незмінним через макро-аліас, тож на Linux/Android/тощо поведінка не змінюється.

### Узгодженість із напрямом upstream

Узгоджується з раніше обговореним планом, щоб завантажувачі шукали провайдери відносно шляху
самої завантаженої бібліотеки (rpath-подібне вирішення) — цей фікс стосується того ж класу
проблем з обробкою шляхів і не конфліктує з цією подальшою роботою; це мінімальний,
ортогональний фікс коректності для наявної невідповідності `LoadLibraryA`/UTF-8.

### Тестування

Відтворено на синтетичному шляху профілю з кириличними символами: підтверджено, що
завантаження через `LoadLibraryA` провалюється, тоді як `dl_load_library_utf8` спрацьовує.
Перевірено наскрізно харнесом L0–L3: завантаження CM-провайдера (`cm-pkcs12`) та ядра UAPKI,
а далі сценарії SIGN/VERIFY — усе проходить без змін як з ASCII-, так і з не-ASCII шляхами
інсталяції.

### Незалежність

Самодостатній Windows-багфікс; не залежить від і не конфліктує з супутнім PR по `*_STATIC`
export-заголовках, поданим окремо.
