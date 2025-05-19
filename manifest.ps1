# Читаємо назву проекту з файлу CMakeLists.txt
$cmakeFile = Get-Content "$PSScriptRoot\CMakeLists.txt"
$project = $cmakeFile | Select-String -Pattern 'project\((\w+)\)' | ForEach-Object { $_.Matches[0].Groups[1].Value }

# Формуємо шаблони імен файлів
$fileTemplateWin32 = "${project}Win_x86.dll"
$fileTemplateWin64 = "${project}Win_x64.dll"

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
