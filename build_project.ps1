# Команда запуску компіляції проекту
# powershell -ExecutionPolicy Bypass -File build_project.ps1 [-WithUAPKI] [-WithTests]
#
# Только основной проект (без UAPKI и без тестов)
#./build_project.ps1
#
# Основной проект с UAPKI (без тестов)
#./build_project.ps1 -WithUAPKI
#
# Основной проект с тестами (без UAPKI)
#./build_project.ps1 -WithTests
#
# Основной проект с UAPKI и тестами
#./build_project.ps1 -WithUAPKI -WithTests
#####################

param(
    [switch]$WithUAPKI,
    [switch]$WithTests
)

####################

# --- Додаємо зміну директорії на директорію скрипта ---
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $ScriptDir

# Читаємо номер версії з файлу VERSION.txt
$versionFileContent = Get-Content "$PSScriptRoot\VERSION.txt" -Raw -ErrorAction Stop
$versionLines = $versionFileContent -replace '\r\n?', "`n" -split "`n"

$versionMajor = ($versionLines | Select-String -Pattern 'VERSION_MAJOR\s*=\s*(\d+)').Matches.Groups[1].Value
$versionMinor = ($versionLines | Select-String -Pattern 'VERSION_MINOR\s*=\s*(\d+)').Matches.Groups[1].Value
$versionRevision = ($versionLines | Select-String -Pattern 'VERSION_REVISION\s*=\s*(\d+)').Matches.Groups[1].Value

# Читаємо номер VERSION_BUILD з файлу version.h
$versionHContent = Get-Content "$PSScriptRoot\version.h" -Raw -ErrorAction Stop
$versionBuild = (($versionHContent | Select-String -Pattern '#define VERSION_BUILD\s*(\d+)').Matches.Groups[1].Value -as [int]) + 1

$version = "$versionMajor.$versionMinor.$versionRevision.$versionBuild"

# Формуємо новий файл version.h
$versionFile = "$PSScriptRoot\version.h"
@"
#define VER_FILENAME SimplyAddinConnect
#define VERSION_FULL $version
#define VERSION_MAJOR     $versionMajor
#define VERSION_MINOR     $versionMinor
#define VERSION_REVISION  $versionRevision
#define VERSION_BUILD     $versionBuild
"@ | Set-Content -Path $versionFile

# Запуск скрипта для оновлення manifest.xml
powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\manifest.ps1"

# Очистка попередніх збірок проекта (но не тестов!)
$foldersToRemove = @("build_x86", "build_x64", "build32Lin", "build64Lin")
Remove-Item -Recurse -Force $foldersToRemove -ErrorAction SilentlyContinue

# Очищаем только папку Release в bin, но сохраняем Debug для тестов
if (Test-Path -Path "$PSScriptRoot\bin\Release") {
    Remove-Item -Recurse -Force "$PSScriptRoot\bin\Release" -ErrorAction SilentlyContinue
} else {
    # Создаем директорию, если она не существует
    New-Item -Path "$PSScriptRoot\bin\Release" -ItemType Directory -Force | Out-Null
}

# Генерація проектів для обох архітектур
$architectureMap = @{
    "Win32" = "x86"
    "x64" = "x64"
}

# Определяем параметры для CMake
$cmakeParams = @()  # Используем массив вместо строки для параметров

# Настройка параметров зборки
if ($WithTests) {
    $cmakeParams += "-DBUILD_TESTS=ON"
    Write-Host "Test suite will be built" -ForegroundColor Green
} else {
    $cmakeParams += "-DBUILD_TESTS=OFF"
    Write-Host "Test suite disabled" -ForegroundColor Yellow
}

if ($WithUAPKI) {
    $cmakeParams += "-DBUILD_WITH_UAPKI=ON"
    Write-Host "Building with UAPKI integration" -ForegroundColor Green
    
    # Проверяем наличие директории UAPKI
    if (-Not (Test-Path -Path "$PSScriptRoot\extern\uapki\library")) {
        Write-Host "ERROR: UAPKI library directory not found at $PSScriptRoot\extern\uapki\library" -ForegroundColor Red
        Write-Host "Make sure submodules are initialized. Run: git submodule update --init --recursive" -ForegroundColor Red
        exit 1
    }
    
    # Проверяем наличие необходимых подкаталогов UAPKI
    $requiredDirs = @("uapkic", "uapkif", "uapki")
    $uapkiDirs = Get-ChildItem -Path "$PSScriptRoot\extern\uapki\library" -Directory | Select-Object -ExpandProperty Name
    $missingDirs = $requiredDirs | Where-Object { $_ -notin $uapkiDirs }
    
    if ($missingDirs) {
        Write-Host "ERROR: Missing required UAPKI directories: $($missingDirs -join ', ')" -ForegroundColor Red
        exit 1
    }
} else {
    $cmakeParams += "-DBUILD_WITH_UAPKI=OFF"
    Write-Host "Building without UAPKI integration" -ForegroundColor Yellow
}

Write-Host "CMake parameters: $($cmakeParams -join ' ')" -ForegroundColor Cyan

# Проверяем наличие директории CMake
if (-Not (Test-Path -Path "$PSScriptRoot\CMake")) {
    Write-Host "ERROR: CMake directory not found at $PSScriptRoot\CMake" -ForegroundColor Red
    Write-Host "Make sure you have created the CMake directory with all required module files" -ForegroundColor Red
    exit 1
}

# Проверяем наличие всех необходимых модульных файлов
$requiredCMakeFiles = @(
    "options.cmake", 
    "dependencies.cmake", 
    "components.cmake", 
    "compiler_settings.cmake", 
    "output_settings.cmake", 
    "platform_settings.cmake"
)

$missingCMakeFiles = $requiredCMakeFiles | Where-Object { -Not (Test-Path -Path "$PSScriptRoot\CMake\$_") }
if ($missingCMakeFiles) {
    Write-Host "ERROR: Missing required CMake module files: $($missingCMakeFiles -join ', ')" -ForegroundColor Red
    exit 1
}

# Если UAPKI включен, проверяем наличие соответствующего модульного файла
if ($WithUAPKI -and -Not (Test-Path -Path "$PSScriptRoot\CMake\uapki_full_static.cmake")) {
    Write-Host "ERROR: Missing required CMake module file: uapki_full_static.cmake" -ForegroundColor Red
    exit 1
}

# Генерируем проекты для каждой архитектуры
foreach ($arch in $architectureMap.Keys) {
    $buildFolder = "build_$($architectureMap[$arch])"
    Write-Host "Generating project for $arch in $buildFolder..." -ForegroundColor Cyan
    
    # Используем параметры CMake как массив
    & cmake -G "Visual Studio 17 2022" -A $arch -S . -B $buildFolder $cmakeParams
    
    # Проверяем успешность выполнения команды
    if (-Not $?) {
        Write-Host "Error generating CMake project for $arch" -ForegroundColor Red
        exit 1
    }
}

# Компиляція у Release-режимі
foreach ($arch in $architectureMap.Keys) {
    $buildFolder = "build_$($architectureMap[$arch])"
    Write-Host "Building $arch in Release mode..." -ForegroundColor Cyan
    & cmake --build $buildFolder --config Release
    
    # Проверяем успешность выполнения команды
    if (-Not $?) {
        Write-Host "Error building project for $arch" -ForegroundColor Red
        exit 1
    }
}

# Перевірка наявності всіх DLL-файлів у папці з релізами
$releaseFolder = "$PSScriptRoot\bin\Release"
$dllFiles = Get-ChildItem -Path $releaseFolder -Filter *.dll -ErrorAction SilentlyContinue

if ($dllFiles) {
    # Створення zip архіву з файлами .dll та manifest.xml
    $manifestFile = "$PSScriptRoot\manifest.xml"
    $filesToZip = @($dllFiles.FullName)
    if (Test-Path $manifestFile) {
        $filesToZip += $manifestFile
        Write-Host "Adding manifest file: $manifestFile"
    } else {
        Write-Host "Warning: Manifest file not found at $manifestFile"
    }

    $zipFilePath = "$releaseFolder\SimplyAddinConnectWin.zip"
    Compress-Archive -Path $filesToZip -DestinationPath $zipFilePath -Force
    Write-Host "Archive created: $zipFilePath"
    
} else {
    Write-Host "DLL files not found. Archive not created."
}

# Перевірка наявності зібраних DLL-файлів та архіву.
# Провайдери cm-pkcs12_x86/_x64.dll збираються як частина основної cmake-збірки
# (ціль cm-pkcs12-provider) і лягають у bin/Release самі — окремого configure немає.
$expectedFiles = @(
    "SimplyAddinConnectWin_x86.dll",
    "SimplyAddinConnectWin_x64.dll"
)
if ($WithUAPKI) {
    $expectedFiles += "cm-pkcs12_x86.dll"
    $expectedFiles += "cm-pkcs12_x64.dll"
}
$expectedFiles += "SimplyAddinConnectWin.zip"

$missingFiles = $expectedFiles | Where-Object { -Not (Test-Path -Path (Join-Path $releaseFolder $_)) }

if ($missingFiles) {
    Write-Host "ERROR: Missing expected build artifacts in $releaseFolder :" -ForegroundColor Red
    $missingFiles | ForEach-Object { Write-Host " - $_" -ForegroundColor Red }
    exit 1
} else {
    Write-Host "All required files built successfully:" -ForegroundColor Green
    $expectedFiles | ForEach-Object { Write-Host " - $_" -ForegroundColor Green }
}

#############################################
# Тестова зовнішня обробка 1С (.epf) — НЕОБОВ'ЯЗКОВИЙ крок
#############################################
# Збирає ExtDataProcessors\SimplyAddinConnect_test у .epf, вбудувавши в макет NativeAddIn
# СВІЖИЙ SimplyAddinConnectWin.zip — щоб після кожної збірки не міняти макет руками.
#
# Крок навмисно НЕ впливає на результат збірки: якщо в оточенні немає платформи 1С,
# немає вихідників обробки або Конфігуратор повернув помилку — друкуємо попередження
# і йдемо далі. Просто не буде нової обробки; попередня (якщо є) лишається як була.
#
# Збірка йде зі СТЕЙДЖ-копії вихідників у build_epf\src: макет у Git важить 4+ МБ,
# і оновлення його на місці давало б бінарний diff на кожну збірку.
# Платформу можна задати явно через змінну оточення SAC_1CV8_PATH.

function Get-1CPlatformPath {
    if ($env:SAC_1CV8_PATH -and (Test-Path $env:SAC_1CV8_PATH)) { return $env:SAC_1CV8_PATH }

    $roots = @("$env:ProgramFiles\1cv8", "${env:ProgramFiles(x86)}\1cv8")
    $candidates = foreach ($root in $roots) {
        if (Test-Path $root) {
            Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
                ForEach-Object {
                    $exe = Join-Path $_.FullName 'bin\1cv8.exe'
                    if (Test-Path $exe) { [PSCustomObject]@{ Version = [version]$_.Name; Path = $exe } }
                }
        }
    }
    ($candidates | Sort-Object Version -Descending | Select-Object -First 1).Path
}

$epfSourceRoot = "$PSScriptRoot\ExtDataProcessors\SimplyAddinConnect_test"
$epfWorkDir    = "$PSScriptRoot\build_epf"
$componentZip  = Join-Path $releaseFolder "SimplyAddinConnectWin.zip"
$platformExe   = Get-1CPlatformPath
# Ім'я дескриптора містить кирилицю - беремо його з файлової системи, а не з коду скрипта
$epfDescriptor = if (Test-Path $epfSourceRoot) {
    Get-ChildItem $epfSourceRoot -Filter *.xml -File -ErrorAction SilentlyContinue | Select-Object -First 1
} else { $null }

if (-Not $platformExe) {
    Write-Host "SKIP: 1C:Enterprise platform not found - test data processor (.epf) not rebuilt" -ForegroundColor Yellow
} elseif (-Not $epfDescriptor) {
    Write-Host "SKIP: no external data processor sources in $epfSourceRoot - .epf not rebuilt" -ForegroundColor Yellow
} elseif (-Not (Test-Path $componentZip)) {
    Write-Host "SKIP: $componentZip not found - .epf not rebuilt" -ForegroundColor Yellow
} else {
    try {
        Write-Host "Building test data processor using $platformExe ..." -ForegroundColor Cyan
        New-Item $epfWorkDir -ItemType Directory -Force | Out-Null

        # 1. Стейдж-копія вихідників обробки
        $stageSrc = Join-Path $epfWorkDir "src"
        Remove-Item $stageSrc -Recurse -Force -ErrorAction SilentlyContinue
        New-Item $stageSrc -ItemType Directory -Force | Out-Null
        Copy-Item "$epfSourceRoot\*" $stageSrc -Recurse -Force

        # 2. Свіжий архів компоненти в макет NativeAddIn (TemplateType = BinaryData)
        $stageTemplate = Get-ChildItem $stageSrc -Recurse -File -Filter "Template.bin" -ErrorAction SilentlyContinue |
            Where-Object { $_.Directory.Parent.Name -eq "NativeAddIn" } | Select-Object -First 1
        if ($stageTemplate) {
            Copy-Item $componentZip $stageTemplate.FullName -Force
            Write-Host " - component archive embedded into NativeAddIn template"
        } else {
            Write-Host " - WARNING: NativeAddIn binary template not found, .epf keeps the template from sources" -ForegroundColor Yellow
        }

        # 3. Одноразова файлова ІБ - платформі потрібна база, щоб зібрати .epf з XML.
        #    1cv8.exe - GUI-застосунок: без Start-Process -Wait код завершення не отримати.
        $stageIb = Join-Path $epfWorkDir "ib"
        if (-Not (Test-Path (Join-Path $stageIb "1Cv8.1CD"))) {
            New-Item $stageIb -ItemType Directory -Force | Out-Null
            $proc = Start-Process -FilePath $platformExe -Wait -PassThru -NoNewWindow -ArgumentList @(
                'CREATEINFOBASE', "File=""$stageIb""",
                '/DisableStartupDialogs', '/DisableStartupMessages',
                "/Out""$epfWorkDir\create.log""")
            if ($proc.ExitCode -ne 0) { throw "CREATEINFOBASE returned exit code $($proc.ExitCode)" }
        }

        # 4. Збірка .epf з XML-джерел
        $stageDescriptor = Join-Path $stageSrc $epfDescriptor.Name
        $stageEpf = Join-Path $epfWorkDir ($epfDescriptor.BaseName + ".epf")
        Remove-Item $stageEpf -Force -ErrorAction SilentlyContinue
        $proc = Start-Process -FilePath $platformExe -Wait -PassThru -NoNewWindow -ArgumentList @(
            'DESIGNER', "/F""$stageIb""",
            '/DisableStartupDialogs', '/DisableStartupMessages',
            '/LoadExternalDataProcessorOrReportFromFiles', """$stageDescriptor""", """$stageEpf""",
            "/Out""$epfWorkDir\load.log""")
        if ($proc.ExitCode -ne 0 -or -Not (Test-Path $stageEpf)) {
            throw "Designer returned exit code $($proc.ExitCode); see $epfWorkDir\load.log"
        }

        # 5. Публікація поруч зі скриптом - саме звідти обробку відкривають у 1С
        $publishedEpf = Join-Path $PSScriptRoot ($epfDescriptor.BaseName + ".epf")
        Copy-Item $stageEpf $publishedEpf -Force
        Write-Host "Test data processor built: $publishedEpf" -ForegroundColor Green
    } catch {
        Write-Host "WARNING: test data processor was not rebuilt - $($_.Exception.Message)" -ForegroundColor Yellow
        Write-Host "Build result is not affected." -ForegroundColor Yellow
    }
}
