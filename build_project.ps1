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

# Перевірка наявності зібраних DLL-файлів та архіву
$requiredFiles = Get-ChildItem -Path $releaseFolder -Filter *.dll -ErrorAction SilentlyContinue
$zipFile = Get-Item -Path $zipFilePath -ErrorAction SilentlyContinue

if ($requiredFiles -and $zipFile) {
    Write-Host "All required files built successfully:"
    $requiredFiles | ForEach-Object { Write-Host $_.Name }
    Write-Host $zipFile.Name
} else {
    Write-Host "Some files are missing. Check the build."
}

# Если UAPKI включен, собираем провайдеры напрямую здесь
if ($WithUAPKI) {
    Write-Host "Building UAPKI providers..." -ForegroundColor Cyan
    
    # Проверяем наличие директории с библиотекой
    $uapkiDir = "$PSScriptRoot\extern\uapki\library"
    $cmPkcs12Dir = "$uapkiDir\cm-pkcs12"

    if (-Not (Test-Path -Path $cmPkcs12Dir)) {
        Write-Host "ERROR: UAPKI cm-pkcs12 directory not found at $cmPkcs12Dir" -ForegroundColor Red
        exit 1
    }
    
    # Создаем директории для сборки провайдеров
    $cmBuildFolderX86 = "$PSScriptRoot\cm_build_x86"
    $cmBuildFolderX64 = "$PSScriptRoot\cm_build_x64"
    
    # Очищаем старые сборки провайдеров
    Remove-Item -Recurse -Force $cmBuildFolderX86 -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $cmBuildFolderX64 -ErrorAction SilentlyContinue
    
    New-Item -Path $cmBuildFolderX86 -ItemType Directory -Force | Out-Null
    New-Item -Path $cmBuildFolderX64 -ItemType Directory -Force | Out-Null
    
    # Директория для выходных DLL провайдеров
    $providersDir = "$PSScriptRoot\bin\Release\providers"
    New-Item -Path $providersDir -ItemType Directory -Force | Out-Null
    
    # Параметры для сборки провайдера
    $cmParams = @(
        "-DUAPKI_LIBRARIES=$PSScriptRoot\bin\Release", 
        "-DUAPKI_INCLUDE_DIR=$uapkiDir\uapki\include",
        "-DUAPKIC_INCLUDE_DIR=$uapkiDir\uapkic\include",
        "-DUAPKIF_INCLUDE_DIR=$uapkiDir\uapkif\include"
    )
    
    # Собираем провайдер PKCS12 для x86
    Write-Host "Building PKCS12 provider for x86..." -ForegroundColor Cyan
    & cmake -G "Visual Studio 17 2022" -A Win32 -S $cmPkcs12Dir -B $cmBuildFolderX86 $cmParams
    
    if (-Not $?) {
        Write-Host "ERROR: Failed to generate CM-PKCS12 project for x86" -ForegroundColor Red
        exit 1
    }
    
    & cmake --build $cmBuildFolderX86 --config Release
    
    if (-Not $?) {
        Write-Host "ERROR: Failed to build CM-PKCS12 for x86" -ForegroundColor Red
        exit 1
    }
    
    # Копируем 32-битный провайдер в выходную директорию
    $providerX86Path = "$cmBuildFolderX86\Release\cm-pkcs12.dll"
    if (Test-Path $providerX86Path) {
        Copy-Item -Path $providerX86Path -Destination "$providersDir\cm-pkcs12_x86.dll" -Force
        Write-Host "Copied x86 PKCS12 provider to $providersDir\cm-pkcs12_x86.dll" -ForegroundColor Green
    } else {
        Write-Host "WARNING: x86 PKCS12 provider DLL not found at $providerX86Path" -ForegroundColor Yellow
    }
    
    # Собираем провайдер PKCS12 для x64
    Write-Host "Building PKCS12 provider for x64..." -ForegroundColor Cyan
    & cmake -G "Visual Studio 17 2022" -A x64 -S $cmPkcs12Dir -B $cmBuildFolderX64 $cmParams
    
    if (-Not $?) {
        Write-Host "ERROR: Failed to generate CM-PKCS12 project for x64" -ForegroundColor Red
        exit 1
    }
    
    & cmake --build $cmBuildFolderX64 --config Release
    
    if (-Not $?) {
        Write-Host "ERROR: Failed to build CM-PKCS12 for x64" -ForegroundColor Red
        exit 1
    }
    
    # Копируем 64-битный провайдер в выходную директорию
    $providerX64Path = "$cmBuildFolderX64\Release\cm-pkcs12.dll"
    if (Test-Path $providerX64Path) {
        Copy-Item -Path $providerX64Path -Destination "$providersDir\cm-pkcs12_x64.dll" -Force
        Write-Host "Copied x64 PKCS12 provider to $providersDir\cm-pkcs12_x64.dll" -ForegroundColor Green
    } else {
        Write-Host "WARNING: x64 PKCS12 provider DLL not found at $providerX64Path" -ForegroundColor Yellow
    }
    
    # Проверка наличия собранных провайдеров
    $providerFiles = Get-ChildItem -Path $providersDir -Filter *.dll -ErrorAction SilentlyContinue
    if ($providerFiles) {
        Write-Host "UAPKI providers built successfully:" -ForegroundColor Green
        $providerFiles | ForEach-Object { Write-Host " - $($_.Name)" -ForegroundColor Green }
    } else {
        Write-Host "WARNING: No UAPKI providers were built." -ForegroundColor Yellow
    }
}
