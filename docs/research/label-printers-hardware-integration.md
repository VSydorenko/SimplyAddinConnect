# Дослідження: керування принтерами етикеток як «залізом»

> Статус: **дослідження** (вхід для майбутнього брейнштормінгу архітектури).
> Дата: 2026-07-20. Метод: deep-research воркфлоу (8 кутів → 102 URL → 20 fetch →
> 72 твердження → топ-15 → адверсарна перевірка по 3 голоси) + власна верифікація
> несучих фактів і ліцензій оркестратором. Детальну архітектурну проробку відкладено
> до завершення поточних архітектурних змін проєкту.

**Мета.** Знайти спосіб друкувати етикетки з високою якістю **без** платних per-printer
драйверів 1С (Гексагон тощо) і без важких інструментів (BarTender), керуючи принтером
етикеток як спеціалізованим обладнанням із власного нативного компонента.

---

## 1. Головний висновок

Принтер етикеток — це **інтерпретатор командної мови**. Хост надсилає йому готовий
потік команд (переважно ASCII, місцями з бінарними вставками), а принтер сам верстає
й друкує етикетку. Наслідок:

> Дорогі per-printer драйвери 1С (**Гексагон**) і важкі інструменти (**BarTender**)
> **технічно не обов'язкові**. Їхню роль — «зібрати потік команд рідною мовою принтера
> й доставити байти» — можна відтворити у власному компоненті (та сама технологія
> зовнішньої компоненти 1С — Native API + COM — що й у SimplyAddinConnect).

Два стовпи рішення:
1. **Генерація** потоку рідною мовою принтера (ZPL / EPL / TSPL / …).
2. **Доставка** байтів будь-яким транспортом (TCP:9100 напряму, Windows-спулер у режимі
   **RAW**, USB, COM).

**Ключ до якості:** не віддавати друк GDI-драйверу, а текст/штрихкоди лишати нативним
командам принтера, а складну графіку рендерити у **1-bit растр у нативному DPI** й
вставляти командою графіки (`^GF` / `BITMAP` / `GW`).

---

## 2. Мови команд

| Мова | Вендор | Природа | Графіка |
|---|---|---|---|
| **ZPL / ZPL II** | Zebra | де-факто стандарт, ASCII, `^XA…^XZ` | `^GF` |
| **EPL / EPL2** | Zebra/Eltron (старіші desktop) | простіша рядкова | `GW` |
| **TSPL / TSPL2** | TSC (+ клони Godex/Argox/Xprinter) | ASCII: `SIZE/GAP/SPEED/DENSITY/TEXT/BARCODE/BITMAP/PRINT` | `BITMAP` (ASCII-заголовок + **бінарний** блок) |
| **CPCL** | Zebra (мобільні) | ASCII | native |
| **DPL / SBPL / Direct Protocol** | Datamax / SATO / Honeywell-Intermec | аналогічні за ідеологією | native |

Нюанси (верифіковано):
- **Мови взаємно несумісні.** Zebra перемикає їх SGD-налаштуванням `device.languages`.
  Багато моделей стартують у гібридному автовизначенні ZPL/EPL, але **для CPCL і
  мобільних мову треба задати явно** — інакше на виході текст команд замість етикетки.
- Потік **не суто текстовий**: графічні команди (`BITMAP`, `^GF`) несуть бінарний payload
  після ASCII-заголовка — важливо для роботи з рядками/кодуванням у C++.
- Існують мультимовні клони (заявлена сумісність «TSPL/ZPL/DPL/EPL» у дешевих моделях) —
  доказ, що мови є **відкрито задокументованими протоколами**, реалізовними незалежно
  від вендора.

Джерела мов: ZPL II Programming Guide [z-zplii], TSPL/TSPL2 Manual [t-tspl],
EPL2 Manual [e-epl2], CPCL Manual [c-cpcl], DPL [d-dpl], SBPL [s-sbpl],
Intermec Direct Protocol [i-idp].

---

## 3. Транспорт: як доставити байти

### 3.1. TCP:9100 (RAW / JetDirect / AppSocket) — найпряміший
Мережеві принтери за замовчуванням слухають сирі завдання на порту 9100; «це не протокол —
дані обробляються принтером напряму, як паралельний кабель по TCP». Достатньо відкрити сокет
і записати ZPL без обгортки:

```
cat file.zpl | nc <printer-ip> 9100
```

Застереження: частина **мобільних** Zebra слухає **6101**, а не 9100; движок тримає з'єднання
до повного `^XA…^XZ` — неповний потік може «підвісити». **Порт не хардкодити** (він
конфігурований: `setvar ip.port`, веб-UI). Альтернативи: LPD/LPR (515, RFC 1179),
IPP (RFC 8010/8011) — але для етикеток найпоширеніший саме raw:9100.
Джерела: [zpl-net], [zebra-port], [hacktricks-9100].

### 3.2. Windows-спулер у режимі RAW
Канонічний код Microsoft `RawDataToPrinter` (KB138594):

```
OpenPrinter → StartDocPrinter → StartPagePrinter → WritePrinter → EndPagePrinter → EndDocPrinter → ClosePrinter
```

- `DOC_INFO_1.pDatatype = "RAW"` → спулер **не транслює** байти: у `WritePrinter` мають іти
  вже готові ZPL/TSPL/EPL.
- `pOutputFile = NULL` → завдання на реальний принтер, не у файл.
- Обробка помилок має **все одно виконувати teardown** уже відкритих ресурсів.
- `WritePrinter` **блокуючий** з непередбачуваним часом → Microsoft радить **не з UI-потоку**.
- Набір `pDatatype` формально краще відкривати через `EnumPrintProcessorDatatypes`, хоча на
  практиці `"RAW"` працює майже скрізь (принт-процесор `winprint`).
- Zero-code fallback: поставити принтер на драйвер **Generic / Text Only** — черга стає
  прозорим RAW-каналом, і ZPL друкується напряму.

Джерела: [ms-kb138594], [ms-docinfo1], [ms-writeprinter], [ms-openprinter],
[foldermill-generic], [honeywell-generic].

### 3.3. GDI PASSTHROUGH (`ExtEscape`)
Якщо друк через DC — сирі команди «протягуються» повз драйвер escape-кодом **PASSTHROUGH**
через `ExtEscape()`. Вимога: payload префіксувати **2-байтовим (WORD) полем довжини**
(повний розмір буфера, включно з префіксом, іде окремо як `cbInput`). Підтримку перевіряти
`QUERYESCSUPPORT`. Джерела: [ms-passthrough], [ms-queryesc], [ms-extescape].

### 3.4. USB
Windows працює з USB-принтерами через клас-драйвер **Usbprint.sys** (USB Printing Class v1.1);
пристрій стає звичайною чергою → RAW через спулер працює й для USB. Для Generic/Text — так само.
Джерела: [ms-usbprint], [usb-spec].

### 3.5. COM / RS-232 (+ USB-CDC емуляція)
Послідовний порт — окремий транспорт, який дає **двобічний зв'язок і статус-полінг**.
Zebra може емулювати COM поверх USB через CDC-драйвер; desktop-моделі мають знімні модулі RS-232.
Джерела: [zebra-cdc], [zebra-serial-module].

---

## 4. Ключ до якості: 1-bit растр у нативному DPI

Три стратегії, і саме вибір визначає різкість:

1. **Native-команди** (`TEXT`, `BARCODE`, `BOX`) — найрізкіше для тексту й **особливо
   штрихкодів** (рівні модулі, читабельність сканером). Використовувати завжди, де принтер
   уміє малювати сам.
2. **Графіка як 1-bit бітмап у native DPI** (`^GF` / `BITMAP` / `GW`) — для логотипів і
   складних макетів. Умова якості: рендерити растр **точно в роздільній здатності головки**
   (203/300/600 dpi), **чорно-білий без згладжування** (антиаліасинг термоголовка передає
   як шум; за потреби — дизеринг).
3. **GDI-друк вендорським драйвером** — найгірше: втрата контролю над DPI-вирівнюванням і
   щільністю, штрихкоди «пливуть». Саме цього уникаємо.

Оптимум — **гібрид**: текст/штрихкоди native-командами, складна графіка — попередньо
зрендерений 1-bit растр.

Параметри якості (TSPL; аналоги скрізь): `DENSITY` — **ціле 0–15** (дефолт 8, дискретна
шкала; недодрук штрихкодів лікується підняттям); `SPEED` — IPS, набір значень **залежить
від моделі**; `SIZE`/`GAP` — геометрія етикетки.
Джерела: [zebra-gf], [labelary], [htmltozpl-dither], [whizz-jagged].

---

## 5. Готові бібліотеки / SDK (ліцензії верифіковано)

### 5.1. Референс-алгоритм «зображення → команда графіки» (найцінніше для порту в C++)
| Проєкт | Мова | Ліцензія | Що дає |
|---|---|---|---|
| `metafloor/zpl-image` | JS | **MIT** | Еталон: image → `^GFA`/GRF із Z64/ACS-стисненням, поріг чорного, поворот, трим |
| `SimonWaldherr/zplgfa` | Go | **MIT** | PNG/JPEG/GIF → ZPL, чистий код-референс |
| `zplgrf` (PyPI) | Python | MIT | Конверсія GRF ↔ ZPL |

### 5.2. Генератори ZPL / друк
| Проєкт | Мова | Ліцензія | Що дає |
|---|---|---|---|
| `BinaryKits.Zpl` (+`.Viewer`) | .NET | **MIT** | Будує ZPL + image→Zebra-формат + **локальний рендер ZPL→картинка** (свій Labelary) |
| `chrishanzlik/ZPLForge` | C# | MIT | Генерація/серіалізація ZPL II |
| `sacherjj/simple_zpl2` | Python | MIT | Зручний білдер ZPL2 |
| `miikanissi/zebrafy` | Python | **LGPL-3.0** | PDF+image ↔ ZPL |
| `cod3monk/zpl` | Python | **AGPL-3.0 ⚠** | ZPL2 — копілефт, для пропрієтарного продукту небажано |
| `zero11it/…/RawPrinterHelper.cs` | C# | OSS | Обгортка спулера RAW (P/Invoke `WritePrinter`) |
| `DotNet.Util.Zpl` | .NET | OSS | Друк ZPL по **LPT/COM/USB/TCP** |

### 5.3. Рендер / тест без принтера
Labelary (онлайн API + вʼюер), `BinaryKits.Zpl.Viewer`, `ingridhq/zebrash` (Go),
`porrey/Virtual-ZPL-Printer`. **Браузер:** Zebra **BrowserPrint**.

### 5.4. Комерційні SDK
| Продукт | Модель | Ліцензія/ціна |
|---|---|---|
| **Neodynamic ThermalLabel SDK** | **code-first** .NET (ZPL/EPL + ESC/POS + Fingerprint) **без файлів-шаблонів** — найближче до бажаного | ~**$489+**, .NET-only |
| **BarTender** (Seagull) | шаблони `.btw` + Automation/SDK | платно, printer-based ліцензування |
| **NiceLabel / Loftware** | шаблони + Automation API | платно |
| **ZebraDesigner for Developers** | шаблони + ActiveX/API | безкоштовний дизайнер, інтеграція обмежена |

> **Критичний gap (верифіковано):** **дедикованої open-source C++ бібліотеки** «image → ZPL
> `^GF`» **немає**. Усі референси — C#/JS/Python/Go. Оскільки SimplyAddinConnect — C++17,
> алгоритм `^GF`/`BITMAP`-кодування доведеться **портувати самим** (він нескладний: 1bpp-растр
> → hex, опційно ACS/RLE-стиснення). Найкращі зразки для порту — `zpl-image` (MIT) і `zplgrf`.

---

## 6. Правда про драйвер 1С (Гексагон / БПО «Принтер этикеток»)

Офіційна сторінка 1С (v8.1c.ru, БПО) описує модель дослівно:
- «Работа з принтером… полягає у **вивантаженні в драйвер сформованого пакета даних про
  етикетки**»;
- «драйвер **перетворює отримані від конфігурації пакети даних у набір інструкцій рідною
  мовою принтера**».

Тобто вся суть БПО-драйвера принтера етикеток — **трансляція «пакет даних → нативний потік
команд»**. Гексагон — платна зовнішня компонента 1С (**Native API + COM — та сама технологія,
що й SimplyAddinConnect**), яка друкує «рідною мовою (ZPL/EPL/TSPL) за шаблонами, а
Windows-драйвер використовує лише як транспорт». Мануали вендорів навіть окремо документують
«Windows-driver» команди (у TSPL — `!B`/`!J`/`!N` для проштовхування растру й лічильника копій) —
це і є те, що обгортає «драйвер».

**Висновок: функцію Гексагона повністю відтворює власний компонент.**
Джерела: [1c-bpo-printer], [1c-bpo-models], [1c-driver-req], [geksagon].

---

## 7. Уточнення від адверсарної перевірки (застереження)

- Порт **не завжди 9100** (мобільні — 6101); робити налаштовуваним.
- ZPL друкується **лише в правильному мовному режимі** (`device.languages`); CPCL/мобільні —
  задавати явно.
- RAW-потік потребує **повної коректної послідовності**; неповний ZPL на 9100 може підвисити
  движок.
- `pDatatype="RAW"` на практиці працює хардкодом, але надійніше `EnumPrintProcessorDatatypes`.
- `WritePrinter` **блокуючий** — не з UI-потоку.
- У TSPL статус повертає **`<ESC>!?`** (1 байт прапорців: head open, paper jam,
  out of paper/ribbon, pause, printing…); **`<ESC>!R` — це RESET, не запит статусу**.
  Статус-полінг у мануалі приписаний до **RS-232**, не до TCP — по мережі не припускати
  ідентичну поведінку без перевірки.

---

## 8. Що обов'язково перевірити на реальному принтері

1. Фактичний порт і активна мова моделі (9100/6101, `device.languages`; чи потрібне явне
   перемикання на ZPL/TSPL/CPCL).
2. Реальний набір `SPEED`/`DENSITY` під конкретні матеріал + риббон + DPI головки.
3. Якість штрихкоду native-командою vs 1-bit растр — зняти відбитки 203/300/600 dpi,
   перевірити читабельність сканером (grade).
4. Поведінка при неповному потоці на 9100 (таймаут/закриття сокета).
5. Чи доступний статус лише по COM/USB-CDC, чи модель віддає його й по мережі.
6. `EnumPrintProcessorDatatypes` + поведінка Generic/Text Only на цільових ПК з реальними
   USB/мережевими принтерами.
7. Коректність бінарного payload у `BITMAP`/`^GF`/`!B` (байтів на рядок, вирівнювання,
   стиснення) на реальному відбитку.

---

## 9. Місток до архітектури SimplyAddinConnect (для майбутнього брейнштормінгу)

Технологія та сама, що вже є (Native API + COM). Ескіз мінімального компонента
`AddinLabelPrint` (деталі — на окремому брейнштормі):
1. приймає з 1С **дані етикетки** (а не файл-шаблон);
2. будує потік **ZPL** (спільний знаменник — його емулюють і TSC-клони, і Godex) або **TSPL**
   для TSC/Godex;
3. складну графіку кодує в `^GF`/`BITMAP` (порт алгоритму `zpl-image`);
4. надсилає через **spooler-RAW** (сумісно з поточними інсталяціями) або **TCP:9100**
   (мережеві).

Відкриті питання для брейнштормінгу: ZPL-only vs мультимовний генератор; модель шаблонів
(де верстка етикетки — у 1С чи в компоненті); рендер растру в C++ (свій 1-bit + дизеринг);
формат вхідних даних із 1С; статус/помилки; авто-визначення мови/порту.

---

## Джерела

**Мови команд**
- [z-zplii] ZPL II Programming Guide — https://www.zebra.com/content/dam/support-dam/en/documentation/unrestricted/guide/software/zplii-pm-vol1.pdf
- [t-tspl] TSPL/TSPL2 Programming Manual (TSC) — https://www.servopack.de/support/tsc/tspl_tspl2_programming.pdf
- [e-epl2] EPL2 Programmer's Manual — https://dl.waspbarcode.com/kb/printer/epl2-programmer-manual.pdf
- [c-cpcl] CPCL Programming Manual — https://www.zebra.com/content/dam/support-dam/en/documentation/unrestricted/guide/software/cpcl-pm-en.pdf
- [d-dpl] Datamax-O'Neil DPL Manual — https://www.manualsdir.com/manuals/327458/datamax-oneil-dpl-programmers-manual-e-class-mark-iii-dpl-programmers-manual.html
- [s-sbpl] SBPL — SATO Barcode Printer Language — https://sato-globalhelp.zendesk.com/hc/en-001/articles/4409701048473-sbpl-sato-barcode-printer-language-sbpl
- [i-idp] Intermec Direct Protocol Reference — https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/ppr/ja/public/products/printers/industrial/px4i/documents/sps-ppr-intermec-direct-protocol-860-programmers-reference-manual-60.pdf

**Транспорт**
- [zpl-net] How to send ZPL to a printer via network — https://zpl.ai/how-to-send-zpl-code-to-a-printer-via-network-2
- [zebra-port] Changing the Printer IP Port to 9100 or Others (Zebra) — https://supportcommunity.zebra.com/s/article/changing-the-printer-ip-port-to-9100-or-others?language=en_us
- [hacktricks-9100] 9100 — Raw Printing (JetDirect/AppSocket/PDL-datastream) — https://hacktricks.wiki/en/network-services-pentesting/9100-pjl.html
- [ms-kb138594] Send raw data to printers by using Win32 API (KB138594) — https://learn.microsoft.com/en-us/previous-versions/troubleshoot/windows/win32/win32-raw-data-to-printer
- [ms-docinfo1] DOC_INFO_1 structure — https://learn.microsoft.com/en-us/windows/win32/printdocs/doc-info-1
- [ms-writeprinter] WritePrinter function — https://learn.microsoft.com/en-us/windows/win32/printdocs/writeprinter
- [ms-openprinter] OpenPrinter function — https://learn.microsoft.com/en-us/windows/win32/printdocs/openprinter
- [foldermill-generic] Print ZPL via Generic Windows Text Printer — https://www.foldermill.com/kb/install-generic-text-driver-in-windows
- [honeywell-generic] Printing ZPL using the Generic Text Driver — https://sps-support.honeywell.com/s/article/printing-zpl-using-the-generic-text-driver
- [ms-passthrough] PASSTHROUGH Printer Escape — https://learn.microsoft.com/en-us/previous-versions/windows/desktop/legacy/dd162776(v=vs.85)
- [ms-queryesc] QUERYESCSUPPORT Printer Escape — https://learn.microsoft.com/en-us/previous-versions/windows/desktop/legacy/ff686811(v=vs.85)
- [ms-extescape] ExtEscape function — https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-extescape
- [ms-usbprint] Microsoft USB Printer Driver (Usbprint.sys) — https://learn.microsoft.com/en-us/windows-hardware/drivers/print/usb-printing
- [usb-spec] USB Device Class Definition for Printing Devices v1.1 — https://www.usb.org/sites/default/files/usbprint11a021811.pdf
- [zebra-cdc] Emulating a COM/Serial Port Over USB Using CDC Driver — https://support.zebra.com/article/emulating-a-com-serial-port-over-usb-using-cdc-driver
- [zebra-serial-module] Installing the Serial Port Module (ZD421/ZD621) — https://docs.zebra.com/us/en/printers/desktop/zd421-and-zd621-desktop-printers-user-guide/c-zd620-420-install-hardware-options/r-zd421-zd621-ug-printer-connectivity-modules/t-zd421-zd621-ug-installing-the-serial-port-module.html

**Якість / графіка**
- [zebra-gf] ^GF Graphic Field — ZPL Command Reference — https://docs.zebra.com/us/en/printers/software/zpl-pg/c-zpl-zpl-commands/r-zpl-gf.html
- [labelary] Labelary Online ZPL Viewer / API — https://labelary.com
- [htmltozpl-dither] Dithering — Color Conversion to Monochrome — https://www.htmltozpl.com/docs/dithering
- [whizz-jagged] Zebra ZPL Logos Printing Jagged? Fix GRF — https://whizz-tech.com/support/printers/zebra-zpl-image-logos-jagged-grf-dpi-dithering

**Бібліотеки / SDK**
- metafloor/zpl-image (MIT) — https://github.com/metafloor/zpl-image
- SimonWaldherr/zplgfa (MIT) — https://github.com/SimonWaldherr/zplgfa
- zplgrf (Python) — https://pypi.org/project/zplgrf
- BinaryKits/BinaryKits.Zpl (MIT) — https://github.com/BinaryKits/BinaryKits.Zpl
- chrishanzlik/ZPLForge (C#) — https://github.com/chrishanzlik/ZPLForge
- sacherjj/simple_zpl2 (MIT) — https://github.com/sacherjj/simple_zpl2
- miikanissi/zebrafy (LGPL-3.0) — https://github.com/miikanissi/zebrafy
- cod3monk/zpl (AGPL-3.0) — https://github.com/cod3monk/zpl
- zero11it/zpl-printer (RawPrinterHelper.cs) — https://github.com/zero11it/zpl-printer/blob/master/zplprinter/rawprinterhelper.cs
- cuiwenyuan/DotNet.Util.Zpl — https://github.com/cuiwenyuan/DotNet.Util.Zpl
- ingridhq/zebrash (Go, рендер ZPL) — https://github.com/ingridhq/zebrash
- porrey/Virtual-ZPL-Printer — https://github.com/porrey/virtual-zpl-printer
- Zebra BrowserPrint — https://developer.zebra.com/tags/browserprint
- Neodynamic ThermalLabel SDK (.NET, комерц.) — https://www.neodynamic.com/products/printing/thermal-label/
- BarTender pricing (Seagull) — https://www.bartendersoftware.com/product/pricing
- NiceLabel programmable integration — https://help.nicelabel.com/hc/en-001/articles/10989437474961-programmable-integration
- ZebraDesigner for Developers UG — https://www.zebra.com/content/dam/zebra_new_ia/en-us/manuals/software/zebradesigner/ug-zebradesignerfordevelopers-en.pdf

**1С БПО / драйвери**
- [1c-bpo-printer] Принтер этикеток | 1С:БПО — https://v8.1c.ru/tekhnologii/standartnye-biblioteki/1s-biblioteka-podklyuchaemogo-oborudovaniya/printer-etiketok
- [1c-bpo-models] Сертифіковані моделі принтерів етикеток — https://v8.1c.ru/tekhnologii/standartnye-biblioteki/1s-biblioteka-podklyuchaemogo-oborudovaniya/printer-etiketok/sertifitsirovannye-i-podderzhivaemye-modeli-printerov-etiketok
- [1c-driver-req] Вимоги до розробки драйверів БПО (в. 5.0) — https://its.1c.ru/db/content/metod8dev/src/developers/additional/guides/i8106038.htm
- [geksagon] Драйвер для принтерів етикеток — ЦШК «Гексагон» — https://geksagon.ru/ru/page/716-draiver-dlia-printerov-etiketok
