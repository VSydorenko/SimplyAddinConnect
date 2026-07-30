# -Version: версія збірки (напр. 3.0.2.109). Потрапляє в ІМЕНА DLL усередині ZIP.
# ЧОМУ: 1С розпаковує компоненту в %APPDATA%\1C\1cv8\ExtCompT\ і перевикористовує вже
# розпаковану DLL, звіряючи ЛИШЕ ім'я файлу з <component path="...">. Формат <bundle> не має
# поля версії, вміст/дата не звіряються — тож незмінне ім'я означає, що 1С вічно вантажить
# ПЕРШУ розпаковану версію, ігноруючи нові збірки. Версія в імені робить кожну збірку унікальною
# (так само роблять інші вендори: ScanOPOSNativeWin32_10_6_1_7.dll, ReceiptPrinterNativeWin32_3_1_4_6.dll).
# Порожній -Version → імена без версії (стара поведінка, лише для ручного запуску).
param([string]$Version = "")

# Читаємо назву проекту з файлу CMakeLists.txt
$cmakeFile = Get-Content "$PSScriptRoot\CMakeLists.txt"
$project = $cmakeFile | Select-String -Pattern 'project\((\w+)\)' | ForEach-Object { $_.Matches[0].Groups[1].Value }

# Якщо версію не передали — беремо з version.h (щоб ручний запуск теж давав версіоновані імена)
if (-not $Version) {
    $vh = Join-Path $PSScriptRoot 'version.h'
    if (Test-Path $vh) {
        $m = (Get-Content $vh -Raw) | Select-String -Pattern '#define VERSION_FULL\s+([0-9.]+)'
        if ($m) { $Version = $m.Matches.Groups[1].Value }
    }
}

# Крапки в імені файлу замінюємо підкресленнями: 3.0.2.109 -> 3_0_2_109
$verTag = if ($Version) { "_" + ($Version -replace '\.', '_') } else { "" }

# Формуємо шаблони імен файлів
$fileTemplateWin32 = "${project}Win${verTag}_x86.dll"
$fileTemplateWin64 = "${project}Win${verTag}_x64.dll"

# Linux is not supported yet
# $fileTemplateLin32 = "${project}Lin_x86.so"
# $fileTemplateLin64 = "${project}Lin_x64.so"

# Створюємо файл manifest.xml
$manifestFile = "$PSScriptRoot\manifest.xml"
$encoding = [System.Text.Encoding]::UTF8
$writer = New-Object System.Xml.XmlTextWriter($manifestFile, $encoding)
$writer.Formatting = 'Indented'
$writer.Indentation = 1
$writer.IndentChar = "`t"
$writer.WriteStartDocument()
$writer.WriteStartElement('bundle')
$writer.WriteAttributeString('xmlns', 'http://v8.1c.ru/8.2/addin/bundle')

$writer.WriteStartElement('component')
$writer.WriteAttributeString('type', 'native')
$writer.WriteAttributeString('os', 'Windows')
$writer.WriteAttributeString('arch', 'i386')
$writer.WriteAttributeString('path', $fileTemplateWin32)
$writer.WriteEndElement();

$writer.WriteStartElement('component')
$writer.WriteAttributeString('type', 'native')
$writer.WriteAttributeString('os', 'Windows')
$writer.WriteAttributeString('arch', 'x86_64')
$writer.WriteAttributeString('path', $fileTemplateWin64)
$writer.WriteEndElement();

# Linux is not supported yet

# $writer.WriteStartElement('component')
# $writer.WriteAttributeString('type', 'native')
# $writer.WriteAttributeString('os', 'Linux')
# $writer.WriteAttributeString('arch', 'i386')
# $writer.WriteAttributeString('path', $fileTemplateLin32)
# $writer.WriteEndElement();

# $writer.WriteStartElement('component')
# $writer.WriteAttributeString('type', 'native')
# $writer.WriteAttributeString('os', 'Linux')
# $writer.WriteAttributeString('arch', 'x86_64')
# $writer.WriteAttributeString('path', $fileTemplateLin64)
# $writer.WriteEndElement();

$writer.WriteEndElement();
$writer.WriteEndDocument()
$writer.Flush()
$writer.Close()
