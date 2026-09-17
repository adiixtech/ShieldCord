# ============================================================
#  ShieldCord — build_all.ps1
#
#  Builds everything, in order, stopping at the first failure:
#    1. kernel driver   (cl.exe + WDK headers, signed with the test cert)
#    2. C++ binaries    (service, installer, uninstaller, ctl, driver setup)
#    3. tray application (dotnet publish, framework-dependent, win-x64)
#    4. Setup.exe       (Inno Setup)
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File build_all.ps1
#    powershell -ExecutionPolicy Bypass -File build_all.ps1 -SkipDriver
#
#  The product version is read from SC_VERSION in src/shared/common.h so the
#  engine, the UI and the installer can never disagree about it.
# ============================================================
[CmdletBinding()]
param(
    # Skip the driver step when iterating on the service or UI. The previously
    # built .sys is reused as-is.
    [switch]$SkipDriver,

    # Stop after the binaries, without invoking Inno Setup.
    [switch]$SkipInstaller,

    # Build configuration for the C++ targets.
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function Write-Step([string]$text) {
    Write-Host ''
    Write-Host "=== $text ===" -ForegroundColor Cyan
}

function Write-Ok([string]$text)   { Write-Host "  [+] $text" -ForegroundColor Green }
function Write-Warn2([string]$text){ Write-Host "  [!] $text" -ForegroundColor Yellow }
function Write-Fail([string]$text) { Write-Host "  [-] $text" -ForegroundColor Red }

function Assert-LastExit([string]$what) {
    if ($LASTEXITCODE -ne 0) {
        Write-Fail "$what failed (exit $LASTEXITCODE)."
        exit 1
    }
}

# ── 0. version ───────────────────────────────────────────────
Write-Step 'Reading the product version'

$commonHeader = Join-Path $root 'src\shared\common.h'
$match = Select-String -Path $commonHeader -Pattern '#define\s+SC_VERSION\s+L"([^"]+)"' |
         Select-Object -First 1

if (-not $match) {
    Write-Fail "SC_VERSION not found in $commonHeader"
    exit 1
}

$version = $match.Matches[0].Groups[1].Value
Write-Ok "version $version (from src\shared\common.h)"

# ── 0b. artwork ──────────────────────────────────────────────
Write-Step 'Generating artwork'

# Drawn from the app's own mark (src\ui\Controls\AppMark.xaml) so the icon, the
# wizard and the product cannot drift apart.
#
# This runs BEFORE the UI is published, not just before Setup.exe: the WPF
# project embeds installer\shieldcord.ico as its application icon, so on a clean
# checkout a missing .ico would fail the UI build rather than the installer's.
$artScript = Join-Path $root 'tools\make_artwork.ps1'
if (-not (Test-Path $artScript)) {
    Write-Fail "tools\make_artwork.ps1 not found."
    exit 1
}
& powershell -ExecutionPolicy Bypass -File $artScript
Assert-LastExit 'Artwork generation'

# ── locate CMake (VS-bundled first, then PATH) ───────────────
$cmake = $null
$cmakeCandidates = @(
    'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    'C:\Program Files\Microsoft Visual Studio\17\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    'C:\Program Files (x86)\Microsoft Visual Studio\17\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
)
foreach ($c in $cmakeCandidates) { if (Test-Path $c) { $cmake = $c; break } }
if (-not $cmake) {
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { $cmake = $onPath.Source }
}
if (-not $cmake) {
    Write-Fail 'CMake not found. Install Visual Studio with the C++ workload.'
    exit 1
}
Write-Ok "cmake: $cmake"

# ── 1. kernel driver ─────────────────────────────────────────
if ($SkipDriver) {
    Write-Step 'Skipping the driver build (-SkipDriver)'
} else {
    Write-Step 'Building the kernel driver'

    $driverScript = Join-Path $root 'build_driver_host.ps1'
    if (-not (Test-Path $driverScript)) {
        Write-Fail "build_driver_host.ps1 not found."
        exit 1
    }

    & powershell -ExecutionPolicy Bypass -File $driverScript
    Assert-LastExit 'Driver build'

    $sysPath = Join-Path $root 'build_driver\bin\Release\shieldcord_filter.sys'
    if (-not (Test-Path $sysPath)) {
        Write-Fail "Expected driver output missing: $sysPath"
        exit 1
    }
    Write-Ok "driver: $sysPath"
}

# ── 2. C++ binaries ──────────────────────────────────────────
Write-Step "Building C++ binaries ($Configuration x64)"

$buildDir = Join-Path $root 'build'
& $cmake -S $root -B $buildDir -A x64 | Out-Null
Assert-LastExit 'CMake configure'

& $cmake --build $buildDir --config $Configuration --parallel
Assert-LastExit 'C++ build'

$binDir = Join-Path $buildDir "bin\$Configuration"
$expected = @(
    'shieldcord_svc.exe',
    'shieldcord_ctl.exe',
    'shieldcord_installer.exe',
    'shieldcord_uninstaller.exe',
    'shieldcord_driver_setup.exe',
    'shieldcord_watermark.dll'
)
foreach ($exe in $expected) {
    $p = Join-Path $binDir $exe
    if (Test-Path $p) { Write-Ok $exe } else { Write-Fail "MISSING: $exe"; exit 1 }
}

# ── 3. tray application ──────────────────────────────────────
Write-Step 'Publishing the tray application'

$uiProject = Join-Path $root 'src\ui\ShieldCordUI.csproj'
Push-Location (Join-Path $root 'src\ui')
try {
    & dotnet publish $uiProject -c $Configuration -r win-x64 --self-contained true --nologo
    Assert-LastExit 'UI publish'
}
finally {
    Pop-Location
}

$uiPublish = Join-Path $root "src\ui\bin\$Configuration\net10.0-windows\win-x64\publish"
$uiExe = Join-Path $uiPublish 'shieldcordui.exe'
if (-not (Test-Path $uiExe)) {
    Write-Fail "Expected UI output missing: $uiExe"
    exit 1
}
Write-Ok "ui: $uiExe"

# The engine trusts its own UI by process name (DriverClient::TrustSelf). If the
# assembly name ever changes, the UI would be blocked from the decoy folder by
# the driver it is displaying — fail loudly here rather than ship that.
if ((Get-Item $uiExe).BaseName -ne 'shieldcordui') {
    Write-Fail "The UI must build as 'shieldcordui.exe' (DriverClient::TrustSelf looks for that name)."
    exit 1
}
Write-Ok 'UI assembly name matches what the driver trusts'

# ── 3b. watermark hook, staged into the UI ───────────────────
# The hook is a native DLL, but it belongs to the UI rather than the engine:
# the TRAY is what injects it (it shares a session and integrity level with
# explorer, so it needs no elevation), and the engine never touches it.
#
# Both packaging paths ship the UI from the publish folder — the installer via
# the "{#UiPublish}\*" wildcard in installer\shieldcord.iss and the VM package
# via an xcopy of the same directory — so staging it here is what makes both
# pick it up with no change of their own.
$watermarkDll = Join-Path $binDir 'shieldcord_watermark.dll'
if (-not (Test-Path $watermarkDll)) {
    Write-Fail "MISSING: shieldcord_watermark.dll"
    exit 1
}
Copy-Item $watermarkDll $uiPublish -Force
Write-Ok 'watermark hook staged into the UI folder'

# ── 4. Setup.exe ─────────────────────────────────────────────
if ($SkipInstaller) {
    Write-Step 'Skipping the installer (-SkipInstaller)'
} else {
    Write-Step 'Building Setup.exe'

    $iscc = $null
    $isccCandidates = @(
        'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
        'C:\Program Files\Inno Setup 6\ISCC.exe',
        # winget installs Inno Setup per-user, not machine-wide, so the two
        # Program Files paths above miss it. Without this the build reports
        # "not installed" on a machine where `winget install JRSoftware.InnoSetup`
        # has just succeeded.
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    )
    foreach ($c in $isccCandidates) { if (Test-Path $c) { $iscc = $c; break } }

    if (-not $iscc) {
        Write-Warn2 'Inno Setup 6 is not installed — skipping Setup.exe.'
        Write-Warn2 'Install it from https://jrsoftware.org/isdl.php then re-run, or run:'
        Write-Warn2 "  ISCC.exe installer\shieldcord.iss /DAppVersion=$version"
    } else {
        $iss = Join-Path $root 'installer\shieldcord.iss'
        & $iscc $iss "/DAppVersion=$version" | Out-Null
        Assert-LastExit 'Inno Setup'

        $setup = Join-Path $root "dist\ShieldCord-Setup-$version.exe"
        if (Test-Path $setup) {
            $sizeMb = [math]::Round((Get-Item $setup).Length / 1MB, 1)
            Write-Ok "setup: $setup ($sizeMb MB)"
        } else {
            Write-Fail "Setup.exe was not produced at $setup"
            exit 1
        }
    }
}

Write-Host ''
Write-Host "ShieldCord $version build complete." -ForegroundColor Green
Write-Host ''
