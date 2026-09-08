<#
.SYNOPSIS
    Публікує реліз SimplyAddinConnect: збірка -> гейт -> тег -> GitHub Release.

.DESCRIPTION
    Один прохід від чистого робочого дерева до опублікованого релізу з артефактом.

    Порядок навмисний: реліз фізично не створюється, якщо гейт червоний. Саме
    цього не може дати CI на чужому runner'і — рівні L2/L3 і L4-iit потребують
    еталонів ПРРО й 32-бітної нативної бібліотеки ІІТ, яких у публічному
    середовищі немає.

    ВЕРСІЯ. MAJOR.MINOR.REVISION живуть у VERSION.txt і міняються РУКАМИ; BUILD
    інкрементує build_project.ps1 при кожній збірці й пише у version.h. Тег
    робиться за трьома числами (v3.1.3), повна чотиричленна версія (3.1.3.188)
    йде в ім'я артефакту й у нотатки. Тому ДВІЧІ випустити ту саму версію не
    вийде — скрипт зупиниться на перевірці тегу й скаже підняти REVISION.

    VERSION.H. Збірка його переписує, тобто після кроку збірки робоче дерево
    стає брудним. Скрипт комітить цей файл ПЕРЕД тегуванням: інакше тег указував
    би на коміт зі старим номером збірки, а артефакт містив би новий — і потім
    неможливо було б сказати, з чого зібрано конкретний реліз.

.PARAMETER DryRun
    Прогнати все, крім незворотного: без коміту version.h, без тегу, без push,
    без створення релізу. Артефакти збираються, гейт виконується.

.PARAMETER SkipGate
    НЕ прогонити гейт. Лише для випадку, коли його щойно прогнано вручну на
    цьому ж коді. Скрипт голосно про це попередить.

.PARAMETER NotesFile
    Файл із нотатками релізу. Без нього GitHub згенерує їх сам (--generate-notes).

.PARAMETER Draft
    Створити чернетку релізу замість публічного.

.PARAMETER PreRelease
    Позначити реліз як передрелізний.

.PARAMETER Branch
    Гілка, з якої дозволено релізити. За замовчуванням main.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File release.ps1 -DryRun
    Репетиція: збірка + гейт, нічого не публікується.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File release.ps1
    Повний реліз поточної версії з VERSION.txt.
#>

[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$SkipGate,
    [string]$NotesFile,
    [switch]$Draft,
    [switch]$PreRelease,
    [string]$Branch = 'main'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-Step  ($msg) { Write-Host "`n=== $msg" -ForegroundColor Cyan }
function Write-Ok    ($msg) { Write-Host "    OK: $msg" -ForegroundColor Green }
function Write-Warn2 ($msg) { Write-Host "    УВАГА: $msg" -ForegroundColor Yellow }
function Fail        ($msg) { Write-Host "`nПОМИЛКА: $msg" -ForegroundColor Red; exit 1 }

$root = $PSScriptRoot
Push-Location $root
try {

# ---------------------------------------------------------------------------
# 0. Середовище
# ---------------------------------------------------------------------------
Write-Step 'Перевірка середовища'

foreach ($tool in @('git', 'gh')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool не знайдено в PATH"
    }
}
Write-Ok 'git і gh на місці'

# gh auth status пише в stderr навіть при успіху, тож дивимось лише код виходу
gh auth status *> $null
if ($LASTEXITCODE -ne 0) { Fail 'gh не авторизований — виконайте: gh auth login' }
Write-Ok 'gh авторизований'

# ---------------------------------------------------------------------------
# 1. Стан репозиторію
# ---------------------------------------------------------------------------
Write-Step 'Перевірка стану репозиторію'

$currentBranch = (git rev-parse --abbrev-ref HEAD).Trim()
if ($currentBranch -ne $Branch) {
    Fail "релізимо з '$Branch', а поточна гілка '$currentBranch'. Перемкніться або вкажіть -Branch"
}
Write-Ok "гілка $currentBranch"

# Робоче дерево мусить бути чистим ДО збірки: інакше не буде зрозуміло, що саме
# потрапило в артефакт. version.h тут ще не виняток — його змінить лише збірка.
$dirty = @(git status --porcelain)
if ($dirty.Count -gt 0) {
    Write-Host ($dirty -join "`n")
    Fail 'робоче дерево не чисте — закомітьте або приберіть зміни перед релізом'
}
Write-Ok 'робоче дерево чисте'

git fetch origin --quiet
$behind = (git rev-list --count "HEAD..origin/$Branch").Trim()
if ($behind -ne '0') { Fail "локальна гілка відстає від origin/$Branch на $behind комітів — зробіть git pull" }
$ahead = (git rev-list --count "origin/$Branch..HEAD").Trim()
if ($ahead -ne '0') { Write-Warn2 "незапушених комітів: $ahead — вони поїдуть при push" }
Write-Ok "синхронізовано з origin/$Branch"

# ---------------------------------------------------------------------------
# 2. Версія й тег
# ---------------------------------------------------------------------------
Write-Step 'Визначення версії'

$verTxt = Get-Content "$root\VERSION.txt" -Raw
$maj = ([regex]::Match($verTxt, 'VERSION_MAJOR\s*=\s*(\d+)')).Groups[1].Value
$min = ([regex]::Match($verTxt, 'VERSION_MINOR\s*=\s*(\d+)')).Groups[1].Value
$rev = ([regex]::Match($verTxt, 'VERSION_REVISION\s*=\s*(\d+)')).Groups[1].Value
if (-not $maj -or -not $min -or -not $rev) { Fail 'не вдалося розібрати VERSION.txt' }

$semver = "$maj.$min.$rev"
$tag    = "v$semver"
Write-Ok "версія $semver, тег $tag"

if ((git tag --list $tag)) {
    Fail "тег $tag уже існує. Підніміть VERSION_REVISION у VERSION.txt і повторіть"
}
git ls-remote --exit-code --tags origin "refs/tags/$tag" *> $null
if ($LASTEXITCODE -eq 0) {
    Fail "тег $tag уже є на origin. Підніміть VERSION_REVISION у VERSION.txt і повторіть"
}
Write-Ok "тег $tag вільний"

# ---------------------------------------------------------------------------
# 3. Збірка
# ---------------------------------------------------------------------------
Write-Step 'Збірка (x86 + x64, з UAPKI і тестами)'

& powershell -ExecutionPolicy Bypass -File "$root\build_project.ps1" -WithUAPKI -WithTests
if ($LASTEXITCODE -ne 0) { Fail "збірка завершилась з кодом $LASTEXITCODE" }

# Повна версія відома лише ПІСЛЯ збірки — build інкрементує саме вона
$verH  = Get-Content "$root\version.h" -Raw
$build = ([regex]::Match($verH, '#define VERSION_BUILD\s+(\d+)')).Groups[1].Value
$fullVersion = "$semver.$build"
Write-Ok "зібрано $fullVersion"

$zipPath = "$root\bin\Release\SimplyAddinConnectWin.zip"
if (-not (Test-Path $zipPath)) { Fail "не знайдено артефакт $zipPath" }

# Нотиси мусять бути В архіві: UAPKI і spdlog лінкуються статично, тож вимога
# зберігати текст ліцензії стосується бінарника. Якщо їх немає — реліз порушує
# умови BSD-2 і MIT, тому це FAIL, а не попередження.
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("sac_rel_" + [guid]::NewGuid().ToString('N').Substring(0,8))
Expand-Archive -Path $zipPath -DestinationPath $stage -Force
if (-not (Test-Path (Join-Path $stage 'THIRD-PARTY-NOTICES.md'))) {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    Fail 'в архіві немає THIRD-PARTY-NOTICES.md — реліз порушив би умови ліцензій статично злінкованих залежностей'
}
$zipContents = (Get-ChildItem $stage | Select-Object -ExpandProperty Name) -join ', '
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
Write-Ok "архів повний: $zipContents"

# ---------------------------------------------------------------------------
# 4. Гейт
# ---------------------------------------------------------------------------
if ($SkipGate) {
    Write-Step 'Гейт ПРОПУЩЕНО (-SkipGate)'
    Write-Warn2 'реліз публікується без перевірки. Робіть так, лише якщо гейт щойно прогнано вручну на цьому ж коді'
}
else {
    foreach ($arch in @('x64', 'x86')) {
        Write-Step "Гейт $arch"
        & powershell -ExecutionPolicy Bypass -File "$root\run_tests.ps1" $arch
        if ($LASTEXITCODE -ne 0) { Fail "гейт $arch провалився (код $LASTEXITCODE) — реліз скасовано" }
        Write-Ok "гейт $arch зелений"
    }
}

# ---------------------------------------------------------------------------
# 5. Фіксація номера збірки
# ---------------------------------------------------------------------------
Write-Step 'Фіксація version.h'

$versionDirty = @(git status --porcelain -- version.h)
if ($versionDirty.Count -gt 0) {
    if ($DryRun) {
        # Репетиція не лишає слідів: перегенерований version.h відкочуємо. Інакше
        # файл висів би брудним, а номер збірки був би «витрачений» на прогін, що
        # нічого не публікує. Саме відкіт, а не виняток у перевірці нижче: файл
        # фіксує номер збірки й ігнорувати його не можна ні на якому кроці.
        git checkout -- version.h
        if ($LASTEXITCODE -ne 0) { Fail 'не вдалося відкотити version.h' }
        Write-Warn2 "DryRun: version.h відкочено, номер $fullVersion не фіксується"
    }
    else {
        git add version.h
        git commit -q -m "chore: збірка $fullVersion для релізу $tag"
        if ($LASTEXITCODE -ne 0) { Fail 'не вдалося закомітити version.h' }
        Write-Ok "version.h закомічено ($fullVersion)"
    }
}
else {
    Write-Ok 'version.h не змінився'
}

# Після кроку вище дерево мусить бути чистим у ОБОХ режимах: у релізі version.h
# закомічено, у репетиції — відкочено. Будь-що інше означає, що збірка зачепила
# те, чого не мала, і це треба розібрати до публікації.
$stillDirty = @(git status --porcelain)
if ($stillDirty.Count -gt 0) {
    Write-Host ($stillDirty -join "`n")
    Fail 'після збірки лишилися незакомічені зміни — розберіться перед релізом'
}

# ---------------------------------------------------------------------------
# 6. Тег і публікація
# ---------------------------------------------------------------------------
$artifact = Join-Path ([System.IO.Path]::GetTempPath()) "SimplyAddinConnectWin-$fullVersion.zip"

if ($DryRun) {
    Write-Step 'DryRun — публікація пропущена'
    Write-Host @"
    Було б зроблено:
      git tag -a $tag -m "SimplyAddinConnect $fullVersion"
      git push origin $Branch --follow-tags
      gh release create $tag $artifact
    Артефакт: $zipPath
"@
    Write-Ok 'репетиція завершена без помилок'
    exit 0
}

Write-Step "Тег $tag"
git tag -a $tag -m "SimplyAddinConnect $fullVersion"
if ($LASTEXITCODE -ne 0) { Fail 'не вдалося створити тег' }
git push origin $Branch --follow-tags
if ($LASTEXITCODE -ne 0) { Fail 'не вдалося запушити гілку й тег' }
Write-Ok "тег $tag запушено"

Write-Step 'GitHub Release'

# Копія під версіонованим іменем: у bin/Release ім'я стабільне (на нього
# зав'язані тести), а користувач має качати файл, за яким видно версію.
Copy-Item $zipPath $artifact -Force

$ghArgs = @('release', 'create', $tag, $artifact,
            '--title', "SimplyAddinConnect $fullVersion")
if ($NotesFile) {
    if (-not (Test-Path $NotesFile)) { Fail "не знайдено файл нотаток: $NotesFile" }
    $ghArgs += @('--notes-file', $NotesFile)
}
else {
    $ghArgs += '--generate-notes'
}
if ($Draft)      { $ghArgs += '--draft' }
if ($PreRelease) { $ghArgs += '--prerelease' }

& gh @ghArgs
if ($LASTEXITCODE -ne 0) { Fail "gh release create завершився з кодом $LASTEXITCODE" }

Remove-Item $artifact -Force -ErrorAction SilentlyContinue

Write-Host "`nРЕЛІЗ $tag ОПУБЛІКОВАНО ($fullVersion)" -ForegroundColor Green
gh release view $tag --json url --jq '.url'

}
finally {
    Pop-Location
}
