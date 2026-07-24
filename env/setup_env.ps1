<#
.SYNOPSIS
    Reconstructs the Arduino toolchain required to build NMEASimPanel.

.DESCRIPTION
    Installs the pinned ESP32 core and libraries, drops the project's lv_conf.h
    into the sketchbook libraries root, and applies the boards.local.txt PSRAM
    fix that the stock CrowPanel board definition needs.

    Safe to re-run: every step is idempotent and existing files are backed up
    before being overwritten.

.PARAMETER ArduinoCli
    Path to arduino-cli.exe. Auto-detected from PATH or the Arduino IDE bundle.

.PARAMETER SketchbookDir
    Arduino sketchbook ("user") directory. Auto-detected, else Documents\Arduino.

.PARAMETER Verify
    After setup, compile the sketch to prove the environment works.

.EXAMPLE
    .\setup_env.ps1 -Verify
#>

[CmdletBinding()]
param(
    [string]$ArduinoCli,
    [string]$SketchbookDir,
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'

# --- Pinned versions. These are the exact versions the project was built and
# --- tested against; newer ones may work but are unverified.
$CoreId       = 'esp32:esp32'
$CoreVersion  = '3.3.10'
$BoardUrl     = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'
$BoardName    = 'elecrow_crowpanel_7'
$Fqbn         = 'esp32:esp32:elecrow_crowpanel_7:PSRAM=enabled'
$Libraries    = @(
    'lvgl@8.3.3',
    'TAMC_GT911@1.0.2',
    'PCA9557-arduino@1.0.0'
)

$EnvDir     = $PSScriptRoot
$ProjectDir = Split-Path $EnvDir -Parent

function Write-Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    OK  $msg" -ForegroundColor Green }
function Write-Warn2($m)  { Write-Host "    !   $m" -ForegroundColor Yellow }

# ---------------------------------------------------------------- arduino-cli
Write-Step "Locating arduino-cli"
if (-not $ArduinoCli) {
    $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($cmd) {
        $ArduinoCli = $cmd.Source
    } else {
        $bundled = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
        if (Test-Path $bundled) { $ArduinoCli = $bundled }
    }
}
if (-not $ArduinoCli -or -not (Test-Path $ArduinoCli)) {
    throw "arduino-cli not found. Install the Arduino IDE or arduino-cli, or pass -ArduinoCli <path>."
}
Write-Ok $ArduinoCli

# ------------------------------------------------------------- sketchbook dir
Write-Step "Resolving sketchbook directory"
if (-not $SketchbookDir) {
    try {
        $dumped = & $ArduinoCli config get directories.user 2>$null
        if ($dumped -and (Test-Path $dumped)) { $SketchbookDir = $dumped.Trim() }
    } catch { }
    if (-not $SketchbookDir) {
        $ideCfg = Join-Path $env:USERPROFILE '.arduinoIDE\arduino-cli.yaml'
        if (Test-Path $ideCfg) {
            $m = Select-String -Path $ideCfg -Pattern '^\s*user:\s*(.+)$' | Select-Object -First 1
            if ($m) { $SketchbookDir = $m.Matches[0].Groups[1].Value.Trim() }
        }
    }
    if (-not $SketchbookDir) {
        $SketchbookDir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Arduino'
    }
}
$LibDir = Join-Path $SketchbookDir 'libraries'
if (-not (Test-Path $LibDir)) { New-Item -ItemType Directory -Path $LibDir -Force | Out-Null }
Write-Ok "sketchbook: $SketchbookDir"

# --------------------------------------------------------------- ESP32 core
Write-Step "Installing $CoreId@$CoreVersion"
& $ArduinoCli core update-index --additional-urls $BoardUrl | Out-Null
& $ArduinoCli core install "$CoreId@$CoreVersion" --additional-urls $BoardUrl
if ($LASTEXITCODE -ne 0) { throw "core install failed" }
Write-Ok "$CoreId@$CoreVersion"

# ----------------------------------------------------------------- libraries
Write-Step "Installing libraries"
& $ArduinoCli lib update-index | Out-Null
foreach ($lib in $Libraries) {
    & $ArduinoCli lib install $lib
    if ($LASTEXITCODE -ne 0) { throw "lib install failed: $lib" }
    Write-Ok $lib
}

# ------------------------------------------------------------------ lv_conf.h
# Must sit BESIDE the lvgl folder, not inside it: lvgl/src/lv_conf_internal.h
# includes "../../lv_conf.h", which resolves to the libraries root.
Write-Step "Installing lv_conf.h into the libraries root"
$src = Join-Path $EnvDir 'lv_conf.h'
$dst = Join-Path $LibDir 'lv_conf.h'
if (-not (Test-Path $src)) { throw "missing $src" }
if (Test-Path $dst) {
    $bak = "$dst.bak"
    Copy-Item $dst $bak -Force
    Write-Warn2 "existing lv_conf.h backed up to $bak"
}
Copy-Item $src $dst -Force
Write-Ok $dst

# ------------------------------------------------------------ boards.local.txt
# The stock elecrow_crowpanel_7 entry sets psram_type=opi but never
# build.memory_type, so the core links the QUAD psram driver against this
# board's OCTAL chip: PSRAM reports 0 MB and the display never comes up.
Write-Step "Applying boards.local.txt PSRAM fix"
$coreRoot = Join-Path $env:LOCALAPPDATA "Arduino15\packages\esp32\hardware\esp32"
if (-not (Test-Path $coreRoot)) { throw "ESP32 core dir not found at $coreRoot" }
$applied = 0
foreach ($verDir in Get-ChildItem $coreRoot -Directory) {
    $target = Join-Path $verDir.FullName 'boards.local.txt'
    $fix    = Join-Path $EnvDir 'boards.local.txt'
    if (Test-Path $target) {
        if ((Get-Content $target -Raw) -match [regex]::Escape("$BoardName.build.memory_type")) {
            Write-Ok "already present in core $($verDir.Name)"
            $applied++
            continue
        }
        Copy-Item $target "$target.bak" -Force
        Write-Warn2 "existing boards.local.txt backed up in core $($verDir.Name)"
    }
    Copy-Item $fix $target -Force
    Write-Ok "core $($verDir.Name)"
    $applied++
}
if ($applied -eq 0) { Write-Warn2 "no core versions found to patch" }

# --------------------------------------------------------------------- verify
if ($Verify) {
    Write-Step "Verifying: compiling the sketch"
    & $ArduinoCli compile --fqbn $Fqbn $ProjectDir
    if ($LASTEXITCODE -ne 0) { throw "verification compile FAILED" }
    Write-Ok "compile succeeded"
}

Write-Host "`nEnvironment ready." -ForegroundColor Green
Write-Host "  Build:  arduino-cli compile --fqbn $Fqbn `"$ProjectDir`""
Write-Host "  Upload: arduino-cli upload -p COM<n> --fqbn $Fqbn `"$ProjectDir`""
Write-Host ""
Write-Host "NOTE: boards.local.txt is removed by an ESP32 core upgrade or reinstall." -ForegroundColor Yellow
Write-Host "      Re-run this script after either, or PSRAM will report 0 MB and the" -ForegroundColor Yellow
Write-Host "      display will stay blank." -ForegroundColor Yellow
