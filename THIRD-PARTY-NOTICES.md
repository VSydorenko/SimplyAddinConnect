# Third-Party Notices / Ліцензії третіх сторін

**SimplyAddinConnect** includes and links the components listed below. Each is
distributed under its own license; the full texts are reproduced verbatim.

**SimplyAddinConnect** включає й лінкує перелічені нижче компоненти. Кожен
поширюється за власною ліцензією; повні тексти наведено дослівно.

Цей файл входить до архіву поставки (`SimplyAddinConnectWin.zip`), бо UAPKI і
spdlog лінкуються **статично** — тобто їхній код фізично присутній у DLL, і
вимога зберігати текст ліцензії поширюється на бінарник, а не лише на репозиторій.

Заголовки SDK 1С у `include/` третьою стороною в цьому переліку **не є** — вони
власність фірми «1С» і не мають окремої ліцензії; про них див. `LICENSE`, розділ
«SCOPE / МЕЖІ ДІЇ».

## Зведення

| Компонент | Версія | Ліцензія | Як підключено |
|---|---|---|---|
| spdlog | v1.17.0 | MIT | статичний лінк у DLL |
| nlohmann/json | v3.12.0 | MIT | header-only |
| pugixml | v1.16 | MIT | вихідники в DLL |
| UAPKI | 2.0.17 | BSD 2-Clause | статичний лінк у DLL (лише `-WithUAPKI`) |

---

## spdlog v1.17.0

* **Ліцензія:** MIT
* **Джерело:** https://github.com/gabime/spdlog
* **Роль у проєкті:** Логування. Лінкується СТАТИЧНО в головну DLL.

```
The MIT License (MIT)

Copyright (c) 2016 - present, Gabi Melman and spdlog contributors.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

-- NOTE: Third party dependency used by this software --
This software depends on the fmt lib (MIT License),
and users must comply to its license: https://raw.githubusercontent.com/fmtlib/fmt/master/LICENSE
```

---

## nlohmann/json v3.12.0

* **Ліцензія:** MIT
* **Джерело:** https://github.com/nlohmann/json
* **Роль у проєкті:** Розбір і формування JSON. Header-only, вкомпільовується в DLL.

```
MIT License 

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## pugixml v1.16

* **Ліцензія:** MIT
* **Джерело:** https://github.com/zeux/pugixml
* **Роль у проєкті:** Розбір XML у драйвері LabelPrinter. Вихідники вкомпільовуються в DLL.

```
MIT License

Copyright (c) 2006-2026 Arseny Kapoulkine

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
```

---

## UAPKI 2.0.17

* **Ліцензія:** BSD 2-Clause
* **Джерело:** https://github.com/specinfo-ua/UAPKI
* **Роль у проєкті:** Криптографія за українськими стандартами (ДСТУ 4145, ДСТУ 7564).
  Ядро лінкується СТАТИЧНО в головну DLL; провайдер cm-pkcs12 постачається
  окремою DLL. Підключається лише при складанні з -WithUAPKI.

```
BSD 2-Clause License

Copyright (c) 2016-2021, The UAPKI Authors.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

