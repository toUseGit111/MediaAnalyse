# MediaAnalyse diagnostics collector.
# Shipped next to the exe; run collect-logs.cmd (no Python / Qt / admin needed).
# Collects the rolling log files, the newest minidumps and a small info.txt
# into one zip the user can attach to a bug report.
param(
    [string]$OutDir = ""
)

$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$stage = Join-Path $env:TEMP ("MediaAnalyse-diag-" + $stamp)
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$stageLogs = Join-Path $stage "logs"
$stageDumps = Join-Path $stage "crashes"
New-Item -ItemType Directory -Force -Path $stageLogs | Out-Null
New-Item -ItemType Directory -Force -Path $stageDumps | Out-Null

# The logger prefers <exe dir>/logs and falls back to a per-user directory when
# the install location is read-only. Scan all of them, plus MA_LOG_DIR overrides.
$logDirs = @()
$logDirs += Join-Path $here "logs"
if ($env:MA_LOG_DIR) { $logDirs += $env:MA_LOG_DIR }
if ($env:LOCALAPPDATA) { $logDirs += Join-Path $env:LOCALAPPDATA "MediaAnalyse\logs" }
if ($env:TEMP) { $logDirs += Join-Path $env:TEMP "MediaAnalyse\logs" }

$crashDirs = @()
$crashDirs += Join-Path $here "crashes"
if ($env:LOCALAPPDATA) { $crashDirs += Join-Path $env:LOCALAPPDATA "MediaAnalyse\crashes" }
if ($env:TEMP) { $crashDirs += Join-Path $env:TEMP "MediaAnalyse\crashes" }

$logCount = 0
foreach ($d in ($logDirs | Select-Object -Unique)) {
    if (-not (Test-Path $d)) { continue }
    Get-ChildItem -Path $d -File | Where-Object { $_.Name -like "*.log" -or $_.Name -like "*.log.*" } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 20 |
        ForEach-Object {
            Copy-Item $_.FullName -Destination (Join-Path $stageLogs $_.Name) -Force -ErrorAction SilentlyContinue
            $logCount++
        }
}

# Minidumps can be tens of MB each; keep only the three most recent.
$dumpCount = 0
foreach ($d in ($crashDirs | Select-Object -Unique)) {
    if (-not (Test-Path $d)) { continue }
    Get-ChildItem -Path $d -File -Filter "*.dmp" |
        Sort-Object LastWriteTime -Descending | Select-Object -First 3 |
        ForEach-Object {
            Copy-Item $_.FullName -Destination (Join-Path $stageDumps $_.Name) -Force -ErrorAction SilentlyContinue
            $dumpCount++
        }
}

$exe = Join-Path $here "media-analyse.exe"
$exeVer = "media-analyse.exe not found"
if (Test-Path $exe) {
    $vi = (Get-Item $exe).VersionInfo
    $ver = [string]$vi.FileVersion
    if (-not $ver.Trim()) { $ver = "no embedded version" }
    $exeVer = $ver + " (built " + (Get-Item $exe).LastWriteTime.ToString("yyyy-MM-dd HH:mm") + ")"
}
$lines = @()
$lines += "MediaAnalyse diagnostics"
$lines += "collected : " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss")
$lines += "package   : " + $here
$lines += "gui exe   : " + $exeVer
$lines += "OS        : " + [Environment]::OSVersion.VersionString
$lines += "user      : " + $env:USERNAME
$lines += "MA_LOG_DIR: " + $env:MA_LOG_DIR
$lines += "log files : " + $logCount
$lines += "minidumps : " + $dumpCount
$lines += ""
$lines += "Log locations scanned:"
foreach ($d in ($logDirs | Select-Object -Unique)) { $lines += "  - " + $d }
$lines += "Dump locations scanned:"
foreach ($d in ($crashDirs | Select-Object -Unique)) { $lines += "  - " + $d }
$lines += ""
$lines += "Note: local file paths and host names may appear in the logs."
$lines += "URL credentials (user:pass@, token=, key=...) are masked automatically."
Set-Content -Path (Join-Path $stage "info.txt") -Value $lines -Encoding UTF8

if (-not $OutDir) { $OutDir = $here }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
$zip = Join-Path $OutDir ("MediaAnalyse-diagnostics-" + $stamp + ".zip")
try {
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -Force -ErrorAction Stop
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "Diagnostics bundle created:"
    Write-Host "  $zip"
} catch {
    $fallback = Join-Path $OutDir ("MediaAnalyse-diagnostics-" + $stamp)
    Move-Item $stage $fallback -Force -ErrorAction SilentlyContinue
    Write-Host "Compress-Archive unavailable; collected folder:"
    Write-Host "  $fallback"
}
Write-Host ""
Write-Host ("Included: " + $logCount + " log file(s), " + $dumpCount + " minidump(s).")
Write-Host "Attach the bundle to your issue report."
