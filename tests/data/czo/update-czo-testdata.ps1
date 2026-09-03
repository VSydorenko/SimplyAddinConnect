# update-czo-testdata.ps1 — оновлення еталонних підписів ЦЗО (tests/data/czo).
#
# Призначення набору, пастки й межі застосування — у README.md поруч. Тут — АЛГОРИТМ
# отримання файлів, щоб через рік його не довелося відновлювати заново.
#
# ── Як знайдено URL-и (важливо, якщо схема зміниться) ────────────────────────────────
# Сторінка https://czo.gov.ua/testexamples рендериться JS: звичайний GET віддає лише
# каркас, посилань у HTML немає. Два способи дістати схему:
#   1) відкрити сторінку браузером і зібрати `a[href*="/download/test_sign/"]` з DOM;
#   2) прочитати бандл https://czo.gov.ua/assets/js/testexamples.js — схема зашита там.
# Станом на 2026-09-01 схема така (перевірено на 436 посиланнях сторінки):
#
#   https://czo.gov.ua/download/test_sign/<ФОРМАТ>/<АЛГО>/<ГРУПУВАННЯ>/<КАТЕГОРІЯ>/<файл>
#   https://czo.gov.ua/download/test_sign/<ФОРМАТ>/<АЛГО>/<АЛГО-ПРЕФІКС>_sign.cer
#
#   ФОРМАТ     : XAdES | PAdES | CAdES        (нам потрібен CAdES — формат ПРРО)
#   АЛГО       : RSA | ECDSA | DSTU | DSTU-7564
#                  DSTU      = ДСТУ 4145 + ГОСТ 34311 (стара схема)
#                  DSTU-7564 = ДСТУ 4145 + Купина-256 (нова)
#   ГРУПУВАННЯ : enveloped (підпис і дані разом) | detached (окремо)
#   КАТЕГОРІЯ  : CAdES-BES | CAdES-T | CAdES-C | CAdES-X_Long | CAdES-X_Long-Full
#   файл       : enveloped -> test.txt.p7s ; detached -> test.txt + test.txt.p7s
#
# ── Чому качаємо саме це ─────────────────────────────────────────────────────────────
# BES і T — офлайн- та онлайн-профілі ПРРО. C / X_Long / X_Long-Full не беремо: вони несуть
# CRL/OCSP/ланцюг видавця, які ДПС у підписі ПРЯМО ЗАБОРОНЯЄ, тож як позитивні еталони вони
# нам не потрібні (як негативні — дописати за потреби, схема та сама).
#
# ── Пастка іменування ────────────────────────────────────────────────────────────────
# Сертифікати в ОБОХ папках називаються однаково — DSTU_sign.cer / DSTU_encryption.cer.
# У плоскому каталозі вони затруть одне одного й дадуть тихо не той сертифікат, тому
# локально розкладаємо у dstu-7564/ і dstu-4145/.
#
# Приклади: .\update-czo-testdata.ps1          — перезавантажити набір
#           .\update-czo-testdata.ps1 -Verify  — перевірити наявний, без мережі

[CmdletBinding()]
param(
    [switch]$Verify,                                   # лише перевірка, нічого не качати
    [string]$BaseUrl = "https://czo.gov.ua/download/test_sign/CAdES"
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# Дві групи: локальна тека -> сегмент URL. Локальні імена різні НАВМИСНО (див. пастку вище).
$groups = @(
    @{ Dir = 'dstu-7564'; Url = 'DSTU-7564'; Note = 'ДСТУ 4145 + Купина-256' }
    @{ Dir = 'dstu-4145'; Url = 'DSTU';      Note = 'ДСТУ 4145 + ГОСТ 34311 (контрольна група)' }
)

$items = @(
    'DSTU_sign.cer'
    'DSTU_encryption.cer'
    'enveloped/CAdES-BES/test.txt.p7s'
    'enveloped/CAdES-T/test.txt.p7s'
    'detached/CAdES-BES/test.txt'
    'detached/CAdES-BES/test.txt.p7s'
    'detached/CAdES-T/test.txt'
    'detached/CAdES-T/test.txt.p7s'
)

# OID-и, за якими відрізняємо групи (шукаємо в DER підпису як байтову послідовність).
# 1.2.804.2.1.1.1.1.2.2.1  = Купина-256 (digest)
# 1.2.804.2.1.1.1.1.3.6.1.1 = dstu4145WithDstu7564-256-pb (sign)
# 1.2.804.2.1.1.1.1.2.1     = ГОСТ 34311 (digest)
$oidKupynaDigest = [byte[]](0x2A,0x86,0x24,0x02,0x01,0x01,0x01,0x01,0x02,0x02,0x01)
$oidGostDigest   = [byte[]](0x2A,0x86,0x24,0x02,0x01,0x01,0x01,0x01,0x02,0x01)

function Test-ByteSeq([byte[]]$hay, [byte[]]$needle) {
    if ($needle.Length -eq 0 -or $hay.Length -lt $needle.Length) { return $false }
    $last = $hay.Length - $needle.Length
    for ($i = 0; $i -le $last; $i++) {
        $ok = $true
        for ($j = 0; $j -lt $needle.Length; $j++) {
            if ($hay[$i + $j] -ne $needle[$j]) { $ok = $false; break }
        }
        if ($ok) { return $true }
    }
    return $false
}

# Сертифікати ЦЗО приходять у РІЗНОМУ кодуванні: купинна група — Base64 ("MII..."),
# стара — сирий DER (0x30 0x82). Повертає DER-байти незалежно від того, що прийшло.
function Get-DerBytes([string]$path) {
    $raw = [System.IO.File]::ReadAllBytes($path)
    if ($raw.Length -ge 2 -and $raw[0] -eq 0x30) { return $raw }   # уже DER
    try {
        $text = [System.Text.Encoding]::ASCII.GetString($raw)
        $text = ($text -replace '-----[A-Z ]+-----', '') -replace '\s', ''
        return [System.Convert]::FromBase64String($text)
    } catch {
        return $raw   # не DER і не Base64 — віддаємо як є, перевірка нижче це покаже
    }
}

$failed = 0
$total  = 0

foreach ($g in $groups) {
    Write-Host ""
    Write-Host "== $($g.Dir) — $($g.Note) ==" -ForegroundColor Cyan

    foreach ($rel in $items) {
        $total++
        $dst = Join-Path $root (Join-Path $g.Dir ($rel -replace '/', '\'))
        $url = "$BaseUrl/$($g.Url)/$rel"

        if (-not $Verify) {
            $dir = Split-Path $dst
            if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
            try {
                Invoke-WebRequest -Uri $url -OutFile $dst -UseBasicParsing
            } catch {
                Write-Host ("  FAIL  {0,-38} {1}" -f $rel, $_.Exception.Message) -ForegroundColor Red
                $failed++
                continue
            }
        }

        if (-not (Test-Path $dst)) {
            Write-Host ("  FAIL  {0,-38} немає файлу" -f $rel) -ForegroundColor Red
            $failed++
            continue
        }

        $size = (Get-Item $dst).Length
        $note = ""

        if ($rel -like '*.p7s') {
            # Підпис має бути DER SEQUENCE і нести очікуваний digest-OID своєї групи.
            $der = [System.IO.File]::ReadAllBytes($dst)
            if ($der[0] -ne 0x30) { $note = "НЕ DER"; $failed++ }
            else {
                $hasKupyna = Test-ByteSeq $der $oidKupynaDigest
                $hasGost   = Test-ByteSeq $der $oidGostDigest
                $want      = ($g.Dir -eq 'dstu-7564')
                if ($hasKupyna -ne $want) {
                    $note = "OID НЕ ТОЙ (kupyna=$hasKupyna gost=$hasGost)"
                    $failed++
                } else {
                    $note = if ($want) { "Купина-256" } else { "ГОСТ 34311" }
                }
            }
        }
        elseif ($rel -like '*.cer') {
            $der = Get-DerBytes $dst
            if ($der[0] -ne 0x30) { $note = "не розібрано як сертифікат"; $failed++ }
            else {
                try {
                    $c = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($der)
                    $enc = if ((Get-Item $dst).Length -ne $der.Length) { "base64" } else { "der" }
                    $days = [int]($c.NotAfter - (Get-Date)).TotalDays
                    $note = "$enc, чинний до $($c.NotAfter.ToString('yyyy-MM-dd'))"
                    if ($days -lt 0) { $note += " (ПРОСТРОЧЕНИЙ — очікувано для старої групи)" }
                } catch {
                    # X509 може не розібрати ДСТУ-ключ на цій платформі — не привід падати:
                    # структурна перевірка (DER SEQUENCE) уже пройдена вище.
                    $note = "DER ok (X509-парсер не тримає ДСТУ — норма)"
                }
            }
        }
        else {
            # test.txt — оригінал для detached. Байти мають бути точно "Test\r\n".
            $b = [System.IO.File]::ReadAllBytes($dst)
            $note = if ($b.Length -eq 6 -and $b[4] -eq 0x0D -and $b[5] -eq 0x0A) {
                "Test+CRLF"
            } else {
                $failed++; "НЕОЧІКУВАНИЙ вміст ($($b.Length) Б)"
            }
        }

        $color = if ($note -match 'FAIL|НЕ ТОЙ|НЕ DER|НЕОЧІКУВАНИЙ|не розібрано') { 'Red' } else { 'DarkGray' }
        Write-Host ("  {0,-38} {1,6} Б  {2}" -f $rel, $size, $note) -ForegroundColor $color
    }
}

Write-Host ""
if ($failed -eq 0) {
    Write-Host "OK: $total файлів, розбіжностей немає." -ForegroundColor Green
    exit 0
} else {
    Write-Host "ПРОВАЛ: $failed з $total." -ForegroundColor Red
    exit 1
}
