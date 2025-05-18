# Скрипт для сборки и запуска тестов
# powershell -ExecutionPolicy Bypass -File run_tests.ps1 [x86|x64]

# --- Переход в директорию скрипта ---
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $ScriptDir

Write-Host "Starting test build process..."

# Определение архитектуры (по умолчанию x64)
$architecture = "x64"
$archParam = "x64"
if ($args.Length -gt 0 -and $args[0] -eq "x86") {
    $architecture = "Win32"
    $archParam = "x86"
}

Write-Host "Building tests for architecture: $archParam"

# Очистка предыдущих сборок тестов, но сохранение сборки проекта
if (Test-Path -Path "build_tests") {
    Write-Host "Cleaning previous test build directory..."
    Remove-Item -Recurse -Force "build_tests" -ErrorAction SilentlyContinue
}

# Очищаем только папку Debug в bin, но сохраняем Release для основного проекта
if (Test-Path -Path "$PSScriptRoot\bin\Debug") {
    Write-Host "Cleaning Debug output directory..."
    Remove-Item -Recurse -Force "$PSScriptRoot\bin\Debug" -ErrorAction SilentlyContinue
} else {
    # Создаем директорию, если она не существует
    New-Item -Path "$PSScriptRoot\bin\Debug" -ItemType Directory -Force | Out-Null
}

# Создание директории для сборки тестов
Write-Host "Creating test build directory..."
New-Item -Path "build_tests" -ItemType Directory -Force | Out-Null

# Переход в директорию сборки
Set-Location -Path "build_tests"

# Генерация проекта с CMake с явным указанием платформы и включением тестов
Write-Host "Generating CMake project for tests..."
cmake .. -DBUILD_TESTS=ON -A $architecture

# Сборка тестов с добавлением флага /FS для синхронизации записи в PDB-файл
Write-Host "Building tests..."
cmake --build . --config Debug -- /p:CL_MPCount=1 /p:UseMultiToolTask=true /p:AdditionalOptions="/FS"

# Проверка успешности сборки
if ($LASTEXITCODE -eq 0) {
    # Запуск тестов
    Write-Host "Running tests..."
    ctest -C Debug --verbose
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host "All tests completed successfully." -ForegroundColor Green
    } else {
        Write-Host "Some tests failed. See log above for details." -ForegroundColor Red
    }
} else {
    Write-Host "Test build failed. Cannot run tests." -ForegroundColor Red
}

# Возврат в исходную директорию
Set-Location $ScriptDir

Write-Host "Test process completed."