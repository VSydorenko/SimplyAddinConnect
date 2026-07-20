<#
    run_tests.ps1 — оркестратор тестування ЕЦП-стеку UAPKI (без 1С).

    Рівні:
      L0  — статичні інваріанти постачання (dumpbin): рівно 3 експорти головної DLL;
            самодостатній провайдер cm-pkcs12 (7 експортів, лише системні залежності);
            python-ctypes smoke provider_info (SKIP, якщо немає python).
      L1  — uapki_selftest.exe: JSON-сценарії напряму через статичне крипто-ядро
            (tests/scenarios/*.json), кожен — окремий процес у тимч. робочому каталозі.
        L0.6 — wire_selftest.exe: device-facing ядро (framer/classifier/DeviceSession
            над LoopbackTransport + TCP-echo смоук). Не залежить від UAPKI —
            збирається завжди при BUILD_TESTS=ON. Задокументовані [SKIP] (напр.
            ComRoundtrip: потрібна пара com0com) — це НЕ FAIL.
      L2/L3 — native_host.exe: e2e поверх ГОЛОВНОЇ DLL через IComponentBase (кейси 1..4),
            крос-валідація ПРРО (кейс 5) — лише за наявності еталонів.

    Запуск:  powershell -ExecutionPolicy Bypass -File run_tests.ps1 [x64|x86] [-NoUapki]
    Дефолт архітектури — x64. Ненульовий код виходу, якщо будь-що впало.

    Режим -NoUapki: збирає/ганяє лише ядро без крипто-стеку (core_selftest L0.5 +
    wire_selftest L0.6). Провайдер (L0.2), L1 та L2/L3 → SKIP (не FAIL); збірка
    БЕЗ -DBUILD_WITH_UAPKI=ON. Головна DLL без UAPKI все одно експортує рівно 3
    символи (L0.1 лишається активним).

    Тестові дані (tests/data) — read-only вхід; сценарії пишуть лише в тимч. каталог.
#>

[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')]
    [string]$Arch = 'x64',
    [switch]$NoUapki
)

$ErrorActionPreference = 'Continue'

# --- У корінь репозиторію (стійкість до cwd) ---
$Root = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $Root

# --- Похідні від архітектури значення ---
if ($Arch -eq 'x86') { $ArchSuffix = '_x86'; $CmakePlatform = 'Win32' }
else                 { $ArchSuffix = '_x64'; $CmakePlatform = 'x64'   }

$BinRelease   = Join-Path $Root 'bin/Release'
$MainDll      = Join-Path $BinRelease "SimplyAddinConnectWin$ArchSuffix.dll"
$ProviderName = "cm-pkcs12$ArchSuffix"
$ProviderDll  = Join-Path $BinRelease "$ProviderName.dll"
$SelfTestExe  = Join-Path $BinRelease "uapki_selftest$ArchSuffix.exe"
$NativeHostExe= Join-Path $BinRelease "native_host$ArchSuffix.exe"
$CoreSelftestExe = Join-Path $BinRelease ("core_selftest" + $ArchSuffix + ".exe")
$WireSelftestExe = Join-Path $BinRelease ("wire_selftest" + $ArchSuffix + ".exe")
$DataDir      = Join-Path $Root 'tests/data'
$ScenDir      = Join-Path $Root 'tests/scenarios'

# --- Збір результатів для підсумкової таблиці ---
$Results = New-Object System.Collections.Generic.List[object]
function Add-Result {
    param([string]$Level, [string]$Name, [string]$Status, [string]$Detail = '')
    $Results.Add([pscustomobject]@{ Level = $Level; Name = $Name; Status = $Status; Detail = $Detail })
    $color = switch ($Status) { 'PASS' { 'Green' } 'FAIL' { 'Red' } 'SKIP' { 'Yellow' } 'BLOCKED' { 'Red' } default { 'Gray' } }
    $line = "  [{0,-7}] {1,-4} {2}" -f $Status, $Level, $Name
    if ($Detail) { $line += "  — $Detail" }
    Write-Host $line -ForegroundColor $color
}

function Section([string]$Title) {
    Write-Host ''
    Write-Host "==== $Title ====" -ForegroundColor Cyan
}

$modeLabel = if ($NoUapki) { 'без UAPKI (core+wire)' } else { 'повний (UAPKI L0-L3 + wire L0.6)' }
Write-Host "run_tests: архітектура=$Arch, режим=$modeLabel, корінь=$Root" -ForegroundColor White

# =====================================================================
# ЕТАП 0 (L0): статичні інваріанти постачання через dumpbin
# =====================================================================
Section 'ЕТАП 0 (L0): dumpbin-інваріанти постачання'

# --- Пошук dumpbin у VS BuildTools/Community/... ---
$DumpBin = $null
$vsRoots = @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022"
)
$hostArch = if ($Arch -eq 'x86') { 'Hostx86' } else { 'Hostx64' }
$dumpArch = if ($Arch -eq 'x86') { 'x86' } else { 'x64' }
foreach ($vr in $vsRoots) {
    if (-not (Test-Path $vr)) { continue }
    $cand = Get-ChildItem -Path $vr -Recurse -Filter 'dumpbin.exe' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\$hostArch\\$dumpArch\\dumpbin\.exe$" } |
            Select-Object -First 1
    if (-not $cand) {
        $cand = Get-ChildItem -Path $vr -Recurse -Filter 'dumpbin.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if ($cand) { $DumpBin = $cand.FullName; break }
}

function Get-Exports([string]$dll) {
    # Повертає масив імен експортованих символів
    $out = & $DumpBin /nologo /EXPORTS $dll 2>$null
    $names = @()
    foreach ($ln in $out) {
        # рядок таблиці: "     1    0 00011D90 SymbolName"
        if ($ln -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)\s*$') {
            $names += $Matches[1]
        }
    }
    return $names
}

function Get-Dependents([string]$dll) {
    $out = & $DumpBin /nologo /DEPENDENTS $dll 2>$null
    $deps = @()
    foreach ($ln in $out) {
        if ($ln -match '^\s+(\S+\.dll)\s*$') { $deps += $Matches[1] }
    }
    return $deps
}

if (-not $DumpBin) {
    Add-Result 'L0' 'dumpbin' 'SKIP' 'dumpbin.exe не знайдено (VS BuildTools?) — L0-перевірки експортів пропущено'
}
else {
    Write-Host "  dumpbin: $DumpBin" -ForegroundColor DarkGray

    # --- L0.1: головна DLL — рівно 3 експорти ---
    if (-not (Test-Path $MainDll)) {
        Add-Result 'L0' 'main-dll-exports' 'FAIL' "немає файлу: $MainDll"
    }
    else {
        $exp = Get-Exports $MainDll
        $need = @('GetClassObject', 'DestroyObject', 'GetClassNames')
        $missing = $need | Where-Object { $exp -notcontains $_ }
        $extra   = $exp  | Where-Object { $need -notcontains $_ }
        if ($missing.Count -eq 0 -and $extra.Count -eq 0) {
            Add-Result 'L0' 'main-dll-exports' 'PASS' 'рівно 3: GetClassObject/DestroyObject/GetClassNames'
        }
        else {
            $d = @()
            if ($missing) { $d += "відсутні: $($missing -join ',')" }
            if ($extra)   { $d += "зайві: $($extra -join ',')" }
            Add-Result 'L0' 'main-dll-exports' 'FAIL' ($d -join '; ')
        }
    }

    # --- L0.2: провайдер — 7 обовʼязкових експортів, лише системні залежності ---
    if ($NoUapki) {
        Add-Result 'L0' 'provider-exports' 'SKIP' 'режим -NoUapki: провайдер cm-pkcs12 не збирається'
    }
    elseif (-not (Test-Path $ProviderDll)) {
        Add-Result 'L0' 'provider-exports' 'FAIL' "немає файлу: $ProviderDll"
    }
    else {
        $pexp = Get-Exports $ProviderDll
        $needP = @('provider_info', 'provider_init', 'provider_deinit', 'provider_open',
                   'provider_close', 'block_free', 'bytearray_free')
        $missP = $needP | Where-Object { $pexp -notcontains $_ }
        # захист від витоку символів (CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS/.def-регресія)
        $leak = $pexp | Where-Object { $_ -match '^(uapki|uapkic|uapkif|asn1|ba_|byte_|parson|BA_|EVP_)' }
        if ($missP.Count -eq 0 -and $leak.Count -eq 0) {
            Add-Result 'L0' 'provider-exports' 'PASS' "$($pexp.Count) експортів (7 обовʼязкових на місці, витоку немає)"
        }
        else {
            $d = @()
            if ($missP) { $d += "відсутні: $($missP -join ',')" }
            if ($leak)  { $d += "витік символів: $($leak.Count)" }
            Add-Result 'L0' 'provider-exports' 'FAIL' ($d -join '; ')
        }

        # залежності провайдера — лише системні
        $deps = Get-Dependents $ProviderDll
        $bad = $deps | Where-Object { $_ -match '^(uapki|uapkic|uapkif)' }
        if ($bad.Count -eq 0) {
            Add-Result 'L0' 'provider-imports' 'PASS' "залежності системні: $($deps -join ', ')"
        }
        else {
            Add-Result 'L0' 'provider-imports' 'FAIL' "несистемні залежності: $($bad -join ', ')"
        }
    }
}

# --- L0.3: python ctypes smoke provider_info ---
$python = (Get-Command python -ErrorAction SilentlyContinue)
if (-not $python) { $python = (Get-Command py -ErrorAction SilentlyContinue) }
# Розрядність python має збігатися з архітектурою провайдера: 64-біт python не
# може завантажити 32-біт DLL (WinError 193) — це середовищне обмеження, а не
# дефект провайдера, тож при розбіжності — SKIP, а не FAIL.
$pyBits = $null
# Розрядність python. БЕЗ рядкових літералів у -c: під Windows PowerShell 5.1 (саме нею
# запускається `powershell -File run_tests.ps1`) вкладені подвійні лапки в аргументі
# нативного виклику зʼїдаються, і `struct.calcsize("P")` ламався → $pyBits порожній →
# guard розрядності не спрацьовував і x86-смоук падав WinError 193. c_void_p лапок не має.
if ($python) { $pyBits = "$(& $python.Source -c 'import ctypes;print(ctypes.sizeof(ctypes.c_void_p)*8)' 2>$null)".Trim() }
$wantBits = if ($Arch -eq 'x86') { '32' } else { '64' }
if (-not $python) {
    Add-Result 'L0' 'py-provider_info' 'SKIP' 'python не в PATH'
}
elseif (-not (Test-Path $ProviderDll)) {
    Add-Result 'L0' 'py-provider_info' 'SKIP' "немає провайдера: $ProviderDll"
}
elseif ($pyBits -and ($pyBits -ne $wantBits)) {
    Add-Result 'L0' 'py-provider_info' 'SKIP' "python $pyBits-біт не може завантажити $Arch-провайдер (потрібен $wantBits-біт python)"
}
else {
    $pySmoke = @'
import sys, ctypes, ctypes.util
dll = sys.argv[1]
lib = ctypes.CDLL(dll)
fn = lib.provider_info            # cm_provider_info_f(CM_JSON_PCHAR* out) -> int
fn.restype = ctypes.c_int
fn.argtypes = [ctypes.POINTER(ctypes.c_char_p)]
out = ctypes.c_char_p()
rc = fn(ctypes.byref(out))
info = out.value.decode('utf-8', 'replace') if out.value else ''
if rc != 0:
    print("provider_info rc=%d" % rc); sys.exit(1)
if 'PKCS12' not in info and 'pkcs12' not in info and 'provider' not in info.lower():
    print("provider_info без очікуваного вмісту: %s" % info[:120]); sys.exit(1)
print("provider_info OK: %s" % info[:120])
sys.exit(0)
'@
    $tmpPy = Join-Path ([System.IO.Path]::GetTempPath()) ("prov_smoke_" + [guid]::NewGuid().ToString('N').Substring(0,8) + '.py')
    Set-Content -Path $tmpPy -Value $pySmoke -Encoding UTF8
    $so = & $python.Source $tmpPy $ProviderDll 2>&1
    $ok = ($LASTEXITCODE -eq 0)
    Remove-Item $tmpPy -ErrorAction SilentlyContinue
    if ($ok) { Add-Result 'L0' 'py-provider_info' 'PASS' ("$so".Trim()) }
    else     { Add-Result 'L0' 'py-provider_info' 'FAIL' ("$so".Trim()) }
}

# =====================================================================
# ЕТАП 1 (build): наявність тестових exe; за потреби — конфіг+збірка
# =====================================================================
Section 'ЕТАП 1 (build): тестові виконувані файли'

# У режимі -NoUapki потрібні лише ядрові exe (core+wire); UAPKI-стек не збирається.
if ($NoUapki) {
    $haveExes = (Test-Path $CoreSelftestExe) -and (Test-Path $WireSelftestExe)
    $exeLabel = 'core_selftest.exe та wire_selftest.exe'
}
else {
    $haveExes = (Test-Path $SelfTestExe) -and (Test-Path $NativeHostExe)
    $exeLabel = 'uapki_selftest.exe та native_host.exe'
}
if ($haveExes) {
    Add-Result 'build' 'test-exes' 'PASS' "$exeLabel вже зібрані"
}
else {
    # Список опцій CMake: під -NoUapki НЕ форсуємо BUILD_WITH_UAPKI (провайдер/крипто-ядро
    # свідомо не збираються), лише BUILD_TESTS — core_selftest+wire_selftest збираються завжди.
    $cmakeArgs = @('-S', $Root, '-B', $null, '-A', $CmakePlatform, '-DBUILD_TESTS=ON')
    if (-not $NoUapki) { $cmakeArgs += '-DBUILD_WITH_UAPKI=ON' }
    $buildDir = Join-Path $Root "build_$Arch"
    $cmakeArgs[3] = $buildDir
    $uapkiNote = if ($NoUapki) { 'BUILD_TESTS=ON (без UAPKI)' } else { 'BUILD_WITH_UAPKI=ON, BUILD_TESTS=ON' }
    Write-Host "  Тестові exe відсутні — конфігурую+збираю build_$Arch ($uapkiNote)..." -ForegroundColor Yellow
    & cmake @cmakeArgs | Out-Host
    & cmake --build $buildDir --config Release | Out-Host

    if ($NoUapki) { $haveExes = (Test-Path $CoreSelftestExe) -and (Test-Path $WireSelftestExe) }
    else          { $haveExes = (Test-Path $SelfTestExe) -and (Test-Path $NativeHostExe) }
    if ($haveExes) {
        Add-Result 'build' 'test-exes' 'PASS' 'зібрано'
    }
    else {
        Add-Result 'build' 'test-exes' 'BLOCKED' `
            "exe ($exeLabel) не зʼявилися після збірки. Перевірте, що конфіг пройшов з $uapkiNote і що add_subdirectory(tests) виконався (у виводі CMake має бути 'Test suite enabled')."
    }
}

# =====================================================================
# ЕТАП 0.5 (L0.5): core_selftest — L1-харнес ядра AddInNative (без 1С, без UAPKI)
# Проходить незалежно від -WithUAPKI: не потребує провайдера чи крипто-екзешників.
# =====================================================================
Section 'ЕТАП 0.5 (L0.5): core_selftest ядра'

if (-not (Test-Path $CoreSelftestExe)) {
    Add-Result 'L0.5' 'core_selftest' 'FAIL' `
        "немає core_selftest.exe: $CoreSelftestExe — зберіть з -WithTests (core_selftest збирається завжди при BUILD_TESTS=ON, без UAPKI)"
}
else {
    $outF = Join-Path ([System.IO.Path]::GetTempPath()) ("core_selftest_" + [guid]::NewGuid().ToString('N').Substring(0,6) + '.out')
    $p = Start-Process -FilePath $CoreSelftestExe `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
    $txt = ''
    if (Test-Path $outF) { $txt = Get-Content -Raw $outF }
    if ($p.ExitCode -eq 0) {
        $lastLine = ($txt -split "`n" | Where-Object { $_ -match '===' } | Select-Object -Last 1)
        Add-Result 'L0.5' 'core_selftest' 'PASS' ("$lastLine".Trim())
    }
    else {
        $fails = ($txt -split "`n" | Where-Object { $_ -match '\[FAIL\]' }) -join ' | '
        Add-Result 'L0.5' 'core_selftest' 'FAIL' "exit=$($p.ExitCode) $fails"
    }
    Remove-Item $outF, "$outF.err" -ErrorAction SilentlyContinue
}

# =====================================================================
# ЕТАП 0.6 (L0.6): wire_selftest — device-facing ядро (framer/classifier/session).
# Не залежить від -WithUAPKI: збирається завжди при BUILD_TESTS=ON. Задокументовані
# [SKIP]-рядки (напр. ComRoundtrip — потрібна пара com0com) — це НЕ FAIL: критерій
# лише exit-код 0 (усі активні CHECK — PASS).
# =====================================================================
Section 'ЕТАП 0.6 (L0.6): wire_selftest device-ядра'

if (-not (Test-Path $WireSelftestExe)) {
    Add-Result 'L0.6' 'wire_selftest' 'FAIL' `
        "немає wire_selftest.exe: $WireSelftestExe — зберіть з -WithTests (wire_selftest збирається завжди при BUILD_TESTS=ON, без UAPKI)"
}
else {
    $outF = Join-Path ([System.IO.Path]::GetTempPath()) ("wire_selftest_" + [guid]::NewGuid().ToString('N').Substring(0,6) + '.out')
    $p = Start-Process -FilePath $WireSelftestExe `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
    $txt = ''
    if (Test-Path $outF) { $txt = Get-Content -Raw $outF }
    $nSkipLines = ([regex]::Matches($txt, '\[SKIP\]')).Count
    if ($p.ExitCode -eq 0) {
        $lastLine = ($txt -split "`n" | Where-Object { $_ -match '===' } | Select-Object -Last 1)
        $detail = "$lastLine".Trim()
        if ($nSkipLines -gt 0) { $detail += "  (задокументованих SKIP: $nSkipLines)" }
        Add-Result 'L0.6' 'wire_selftest' 'PASS' $detail
    }
    else {
        $fails = ($txt -split "`n" | Where-Object { $_ -match '\[FAIL\]' }) -join ' | '
        Add-Result 'L0.6' 'wire_selftest' 'FAIL' "exit=$($p.ExitCode) $fails"
    }
    Remove-Item $outF, "$outF.err" -ErrorAction SilentlyContinue
}

# =====================================================================
# ЕТАП 2 (L1): uapki_selftest на кожному сценарії (окремий процес)
# =====================================================================
Section 'ЕТАП 2 (L1): uapki_selftest сценарії'

$WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) ("sac_tests_" + [guid]::NewGuid().ToString('N').Substring(0,8))
$cleanup = @()

if ($NoUapki) {
    Add-Result 'L1' 'uapki_selftest' 'SKIP' 'режим -NoUapki: крипто-стек UAPKI не збирається'
}
elseif (-not (Test-Path $SelfTestExe)) {
    Add-Result 'L1' 'uapki_selftest' 'SKIP' 'немає uapki_selftest.exe (див. ЕТАП 1)'
}
elseif (-not (Test-Path $ProviderDll)) {
    Add-Result 'L1' 'uapki_selftest' 'SKIP' "немає провайдера $ProviderDll"
}
else {
    New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
    $cleanup += $WorkDir
    # Робочий каталог: read-only дані + провайдер поруч (сценарії мають dir="./")
    Copy-Item -Path (Join-Path $DataDir '*') -Destination $WorkDir -Recurse -Force
    Copy-Item -Path $ProviderDll -Destination $WorkDir -Force
    # Копії сценаріїв у робочий каталог; для x86 — підмінити ім'я провайдера
    $wScen = Join-Path $WorkDir 'scenarios'
    New-Item -ItemType Directory -Force -Path $wScen | Out-Null
    Get-ChildItem -Path $ScenDir -Filter '*.json' | ForEach-Object {
        $txt = [System.IO.File]::ReadAllText($_.FullName)
        if ($Arch -eq 'x86') { $txt = $txt -replace 'cm-pkcs12_x64', 'cm-pkcs12_x86' }
        # UTF-8 БЕЗ BOM: parson не парсить JSON із BOM на початку, а Set-Content -Encoding UTF8
        # у Windows PowerShell 5.1 додав би BOM (усі сценарії тоді падали б exit=2).
        [System.IO.File]::WriteAllText((Join-Path $wScen $_.Name), $txt, (New-Object System.Text.UTF8Encoding($false)))
    }

    $scenarios = Get-ChildItem -Path $wScen -Filter '*.json' | Sort-Object Name
    foreach ($sc in $scenarios) {
        $p = Start-Process -FilePath $SelfTestExe -ArgumentList "`"$($sc.FullName)`"" `
                -WorkingDirectory $WorkDir -NoNewWindow -Wait -PassThru `
                -RedirectStandardOutput (Join-Path $WorkDir "$($sc.BaseName).out") `
                -RedirectStandardError  (Join-Path $WorkDir "$($sc.BaseName).err")
        $outTxt = ''
        $outFile = Join-Path $WorkDir "$($sc.BaseName).out"
        if (Test-Path $outFile) { $outTxt = Get-Content -Raw $outFile }
        if ($p.ExitCode -eq 0) {
            Add-Result 'L1' $sc.Name 'PASS' (($outTxt -split "`n" | Where-Object { $_ -match '====' } | Select-Object -Last 1))
        }
        else {
            $fails = ($outTxt -split "`n" | Where-Object { $_ -match '\[FAIL\]' }) -join ' | '
            Add-Result 'L1' $sc.Name 'FAIL' "exit=$($p.ExitCode) $fails"
        }
    }
}

# =====================================================================
# ЕТАП 3 (L2/L3): native_host кейси 1..4 (+5 за наявності ПРРО-еталонів)
# =====================================================================
Section 'ЕТАП 3 (L2/L3): native_host кейси'

if ($NoUapki) {
    Add-Result 'L2/L3' 'native_host' 'SKIP' 'режим -NoUapki: головна DLL без UAPKI, e2e-кейси не застосовні'
}
elseif (-not (Test-Path $NativeHostExe)) {
    Add-Result 'L2/L3' 'native_host' 'SKIP' 'немає native_host.exe (див. ЕТАП 1)'
}
elseif (-not (Test-Path $MainDll)) {
    Add-Result 'L2/L3' 'native_host' 'SKIP' "немає головної DLL $MainDll"
}
else {
    # native_host case4/5 указують UAPKI CerStore на dataDir\certs. CerStore іменує серти за
    # вмістом (thumbprint) — tests/data/certs зберігаються ВЖЕ в канонічній формі upstream,
    # тож повторне сканування ідемпотентне (не перейменовує, git-diff не зʼявляється).
    foreach ($kase in 1..4) {
        $argList = @("$kase", "`"$MainDll`"", "`"$DataDir`"", "`"$BinRelease`"")
        $outF = Join-Path ([System.IO.Path]::GetTempPath()) ("nh_${kase}_" + [guid]::NewGuid().ToString('N').Substring(0,6) + '.out')
        $p = Start-Process -FilePath $NativeHostExe -ArgumentList $argList `
                -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
        $txt = ''
        if (Test-Path $outF) { $txt = Get-Content -Raw $outF }
        $lastLine = ($txt -split "`n" | Where-Object { $_ -match '\S' } | Select-Object -Last 1)
        if ($p.ExitCode -eq 0) { Add-Result 'L2/L3' "native_host case $kase" 'PASS' $lastLine }
        else                   { Add-Result 'L2/L3' "native_host case $kase" 'FAIL' "exit=$($p.ExitCode) $lastLine" }
        Remove-Item $outF, "$outF.err" -ErrorAction SilentlyContinue
    }

    # Кейс 5 — крос-валідація ПРРО, лише за наявності еталонів
    $prro = $env:PRRO_DOCS_DIR
    if (-not $prro -and (Test-Path 'R:/github/prro_docs')) { $prro = 'R:/github/prro_docs' }
    if (-not $prro) {
        Add-Result 'L2/L3' 'native_host case 5' 'SKIP' 'немає PRRO_DOCS_DIR і R:/github/prro_docs'
    }
    else {
        $argList = @('5', "`"$MainDll`"", "`"$DataDir`"", "`"$BinRelease`"", "`"$prro`"")
        $outF = Join-Path ([System.IO.Path]::GetTempPath()) ("nh_5_" + [guid]::NewGuid().ToString('N').Substring(0,6) + '.out')
        $p = Start-Process -FilePath $NativeHostExe -ArgumentList $argList `
                -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outF -RedirectStandardError "$outF.err"
        $txt = if (Test-Path $outF) { Get-Content -Raw $outF } else { '' }
        $lastLine = ($txt -split "`n" | Where-Object { $_ -match '\S' } | Select-Object -Last 1)
        if ($p.ExitCode -eq 0) { Add-Result 'L2/L3' 'native_host case 5' 'PASS' $lastLine }
        else                   { Add-Result 'L2/L3' 'native_host case 5' 'FAIL' "exit=$($p.ExitCode) $lastLine" }
        Remove-Item $outF, "$outF.err" -ErrorAction SilentlyContinue
    }
}

# --- Прибирання тимч. каталогів ---
foreach ($d in $cleanup) { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue }

# =====================================================================
# ПІДСУМОК
# =====================================================================
Section 'ПІДСУМОК'
$Results | Format-Table -AutoSize Level, Status, Name, Detail | Out-Host

# @(...) обовʼязково: під Windows PowerShell 5.1 `(Where-Object ...).Count` на ОДНОМУ збігу
# повертає порожньо (не 1), тож `$nFail -gt 0` було б False і гейт зеленів би з реальним
# провалом. Масив-субвираз гарантує числовий .Count і для одного, і для нуля елементів.
$nFail = @($Results | Where-Object { $_.Status -in @('FAIL', 'BLOCKED') }).Count
$nPass = @($Results | Where-Object { $_.Status -eq 'PASS' }).Count
$nSkip = @($Results | Where-Object { $_.Status -eq 'SKIP' }).Count
Write-Host ''
Write-Host ("Разом: PASS=$nPass  FAIL/BLOCKED=$nFail  SKIP=$nSkip") -ForegroundColor White

if ($nFail -gt 0) {
    Write-Host 'РЕЗУЛЬТАТ: Є провали.' -ForegroundColor Red
    exit 1
}
else {
    Write-Host 'РЕЗУЛЬТАТ: Усі виконані перевірки пройшли.' -ForegroundColor Green
    exit 0
}
