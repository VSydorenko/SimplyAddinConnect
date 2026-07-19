# Архітектура SimplyAddinConnect

> **Цей документ розбито на кілька файлів.** Архітектурний опис тепер живе в каталозі
> [`docs/architecture/`](architecture/) — по одному документу на підсистему.

Починай з індексного огляду: **[`docs/architecture/README.md`](architecture/README.md)**.

Підсистеми:

- [`architecture/01-core.md`](architecture/01-core.md) — ядро `AddInNative` (міст до SDK 1С,
  реєстр компонент, `VariantHelper`) + наскрізні сервіси `ServiceTools`.
- [`architecture/02-ecrprivatjson.md`](architecture/02-ecrprivatjson.md) — драйвер платіжного
  термінала (протокол, хелпер, транспорт COM/TCP/WebSocket).
- [`architecture/03-uapki.md`](architecture/03-uapki.md) — стек ЕЦП/крипто через UAPKI
  (гібридна збірка, самодоставка провайдера).
- [`architecture/04-build-and-packaging.md`](architecture/04-build-and-packaging.md) —
  збіркова архітектура, ZIP-поставка, доставка в 1С.

Переносне ноу-хау (не прив'язане до цього коду) — у скілах `.claude/skills/`
(`1c-native-component`, `uapki-integration`, `ecp-testing-without-1c`, `prro-fiscal`).
