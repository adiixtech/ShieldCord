# build_driver_host.ps1 — Build ShieldCord Kernel Driver on Host Machine
#
# Uses cl.exe (already installed via VS 2026) + WDK kernel-mode headers
# to compile shieldcord_filter.sys WITHOUT needing the WDK VS Extension.
#
# REQUIREMENT: WDK kernel-mode components must be installed.
#   Download WDK (NOT VS Community — this is a separate ~1.5 GB download):
#   https://go.microsoft.com/fwlink/?linkid=2307981
#   During install, choose:
#     ✅ "Windows Driver Kit"  (the main option)
#     ✅ "Windows Driver Kit Visual Studio Extension" (optional, adds VS project support)
#
# After WDK installs, run this script as Administrator.

param(
    [string]$WdkVersion  = "",          # Leave empty to auto-detect
    [string]$OutDir      = "build_driver\bin\Release"
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Write-Step([string]$msg) { Write-Host "`n[*] $msg" -ForegroundColor Cyan }
function Write-Ok([string]$msg)   { Write-Host "[+] $msg" -ForegroundColor Green }
function Write-Fail([string]$msg) { Write-Host "[-] $msg" -ForegroundColor Red; exit 1 }
function Write-Warn([string]$msg) { Write-Host "[!] $msg" -ForegroundColor Yellow }

Write-Host ""
Write-Host "  ================================================" -ForegroundColor Cyan
Write-Host "   ShieldCord Kernel Driver - Host Build Script" -ForegroundColor Cyan
Write-Host "   Compiles .sys using cl.exe + WDK km headers" -ForegroundColor Cyan
Write-Host "  ================================================" -ForegroundColor Cyan
Write-Host ""

# ── Step 1: Find cl.exe (MSVC compiler) ─────────────────────────
Write-Step "Locating cl.exe (MSVC x64 compiler)..."

$clPath = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter "cl.exe" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "Hostx64\\x64" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1 -ExpandProperty FullName

if (-not $clPath) {
    Write-Fail "cl.exe not found. Visual Studio with C++ workload is required."
}
Write-Ok "cl.exe: $clPath"

# ── Step 2: Find MSVC include + lib dirs ─────────────────────────
$msvcRoot = Split-Path (Split-Path (Split-Path (Split-Path $clPath)))
$msvcInc  = "$msvcRoot\include"
$msvcLib  = "$msvcRoot\lib\x64"
Write-Ok "MSVC include: $msvcInc"
Write-Ok "MSVC lib:     $msvcLib"

# ── Step 3: Find WDK kernel-mode headers ────────────────────────
Write-Step "Locating WDK kernel-mode headers (km)..."

$wdkKmInc    = ""
$wdkSharedInc = ""
$wdkUmInc    = ""
$wdkKmLib    = ""
$wdkVer      = ""

if ($WdkVersion -ne "") {
    # User-specified
    $b = "C:\Program Files (x86)\Windows Kits\10"
    $wdkKmInc     = "$b\Include\$WdkVersion\km"
    $wdkSharedInc = "$b\Include\$WdkVersion\shared"
    $wdkUmInc     = "$b\Include\$WdkVersion\um"
    $wdkKmLib     = "$b\Lib\$WdkVersion\km\x64"
    $wdkVer       = $WdkVersion
} else {
    # --- Try WDK 10 km headers first ---
    $wdk10 = "C:\Program Files (x86)\Windows Kits\10"
    $found = Get-ChildItem "$wdk10\Include" -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path "$wdk10\Include\$($_.Name)\km\fltKernel.h" } |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($found) {
        $v = $found.Name
        $wdkKmInc     = "$wdk10\Include\$v\km"
        $wdkSharedInc = "$wdk10\Include\$v\shared"
        $wdkUmInc     = "$wdk10\Include\$v\um"
        $wdkKmLib     = "$wdk10\Lib\$v\km\x64"
        $wdkVer       = "10.$v"
        Write-Ok "WDK 10 ($v) kernel headers found."
    }

    # --- Fall back to WDK 8.0 if WDK 10 km not found ---
    if (-not $wdkVer) {
        $wdk8 = "C:\Program Files (x86)\Windows Kits\8.0"
        if (Test-Path "$wdk8\Include\km\fltKernel.h") {
            $wdkKmInc     = "$wdk8\Include\km"
            $wdkSharedInc = "$wdk8\Include\shared"
            $wdkUmInc     = "$wdk8\Include\um"
            $wdkKmLib     = "$wdk8\Lib\win8\km\x64"
            $wdkVer       = "8.0"
            Write-Ok "WDK 8.0 kernel headers found. Driver will run on Win8+ including Win11."
        }
    }
}

if (-not $wdkVer) {
    Write-Host ""
    Write-Host "  +----------------------------------------------------------+" -ForegroundColor Red
    Write-Host "  |  WDK kernel-mode headers NOT found!                      |" -ForegroundColor Red
    Write-Host "  |                                                          |" -ForegroundColor Red
    Write-Host "  |  You need to install the WDK (NOT VS Community).         |" -ForegroundColor Red
    Write-Host "  |  It is a separate ~1.5 GB download:                      |" -ForegroundColor Red
    Write-Host "  |                                                          |" -ForegroundColor Red
    Write-Host "  |  https://go.microsoft.com/fwlink/?linkid=2307981         |" -ForegroundColor Red
    Write-Host "  |                                                          |" -ForegroundColor Red
    Write-Host "  |  During install select:                                  |" -ForegroundColor Red
    Write-Host "  |    [x] Windows Driver Kit                                |" -ForegroundColor Red
    Write-Host "  |    (VS Extension is optional for this script)            |" -ForegroundColor Red
    Write-Host "  +----------------------------------------------------------+" -ForegroundColor Red
    Write-Host ""
    Write-Host "  After installing the WDK, re-run this script." -ForegroundColor Yellow
    exit 1
}


Write-Ok "WDK version: $wdkVer"
Write-Ok "km include:  $wdkKmInc"
Write-Ok "km lib:      $wdkKmLib"

# Auto-detect Windows 10 SDK shared & um directories (contains specstrings.h, etc.)
$sdk10Base = "C:\Program Files (x86)\Windows Kits\10\Include"
$sdk10Ver = Get-ChildItem $sdk10Base -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
$sdk10Shared = if ($sdk10Ver) { "$sdk10Base\$sdk10Ver\shared" } else { "" }
$sdk10Um     = if ($sdk10Ver) { "$sdk10Base\$sdk10Ver\um" } else { "" }
if ($sdk10Shared) { Write-Ok "SDK 10 shared: $sdk10Shared" }

# ── Step 4: Verify critical headers exist ───────────────────────
foreach ($hdr in @("fltKernel.h", "ntddk.h", "wdm.h")) {
    if (-not (Test-Path "$wdkKmInc\$hdr")) {
        Write-Fail "Missing critical header: $wdkKmInc\$hdr"
    }
}
Write-Ok "Critical headers: OK"

# ── Step 5: Verify critical libs exist ──────────────────────────
foreach ($lib in @("FltMgr.lib", "ntoskrnl.lib", "BufferOverflowK.lib")) {
    if (-not (Test-Path "$wdkKmLib\$lib")) {
        Write-Warn "Missing lib: $wdkKmLib\$lib (build may fail)"
    } else {
        Write-Ok "  $lib : OK"
    }
}

# ── Step 6: Create output directory ─────────────────────────────
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outDirFull = Resolve-Path $OutDir

# ── Step 7: Collect source files ────────────────────────────────
Write-Step "Collecting kernel source files..."

$srcDir = "src\driver\filter"
$sources = @(
    "$srcDir\shieldcord_filter.c",
    "$srcDir\pre_create.c",
    "$srcDir\process_cache.c",
    "$srcDir\comm_port.c",
    "$srcDir\path_filter.c"
)

foreach ($s in $sources) {
    if (-not (Test-Path $s)) { Write-Fail "Source not found: $s" }
    Write-Ok "  $s"
}

# ── Step 8: Build compile command ───────────────────────────────
Write-Step "Compiling kernel driver..."

#
# Kernel compiler flags explained:
#   /kernel        — kernel-mode code generation (calls __fastcall, correct TEB, etc.)
#   /GS-           — disable stack cookies (kernel doesn't support them)
#   /Gz            — __stdcall calling convention (kernel ABI)
#   /W4 /WX        — all warnings, warnings as errors
#   /Zp8           — 8-byte struct packing
#   /FI <file>     — force-include ntddk.h as the first header
#   /GR-           — no RTTI (not available in kernel)
#   /EHs-c-        — no C++ exceptions
#   /Od /Zi        — debug info (use /O2 for release)
#   /D POOL_NX_OPTIN=1 — opt into NX (non-executable) pool globally
#

$cl = $clPath
$incFlags = @(
    "/I`"$wdkKmInc`"",
    "/I`"$wdkKmInc\crt`"",
    "/I`"$wdkSharedInc`"",
    "/I`"$sdk10Shared`"",
    "/I`"$wdkUmInc`"",
    "/I`"$sdk10Um`"",
    "/I`"src\driver\filter`""
)

$defineFlags = @(
    "/D_AMD64_",
    "/D_WIN64",
    "/DPOOL_NX_OPTIN=1",
    "/D_WIN32_WINNT=0x0602",
    "/DWINVER=0x0602",
    "/DNTDDI_VERSION=0x06020000",
    "/DWINNT=1"
)

$codegenFlags = @(
    "/kernel",          # kernel-mode code generation
    "/GS-",             # no security cookies
    "/Gz",              # __stdcall
    "/GR-",             # no RTTI
    "/EHs-", "/EHc-",  # no C++ exceptions
    "/Zp8",             # 8-byte struct packing
    "/W4",              # all warnings
    "/wd4201",          # nameless struct/union (WDK headers use this)
    "/wd4214",          # bitfield type != int
    "/wd4100",          # unreferenced formal parameter
    "/wd4324",          # structure was padded due to alignment specifier
    "/O2",              # optimize for speed
    "/Zi"               # debug info (for BSOD analysis)
)

# Compile all .c files to .obj in the output directory
$objFiles = @()
foreach ($src in $sources) {
    $objName = "$outDirFull\$([System.IO.Path]::GetFileNameWithoutExtension($src)).obj"
    $objFiles += "`"$objName`""

    $compileArgs = @("/nologo", "/c", "/TC") +   # /TC = compile as C
                   $incFlags + $defineFlags + $codegenFlags +
                   @("/Fo`"$objName`"", "`"$(Resolve-Path $src)`"")

    Write-Host "  Compiling: $([System.IO.Path]::GetFileName($src))..." -ForegroundColor DarkGray
    $result = & $cl @compileArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host $result -ForegroundColor Red
        Write-Fail "Compilation failed for: $src"
    }
}
Write-Ok "All .c files compiled."

# ── Step 9: Link the driver ─────────────────────────────────────
Write-Step "Linking shieldcord_filter.sys..."

$linkPath = Join-Path (Split-Path $cl) "link.exe"
$sysPath  = "$outDirFull\shieldcord_filter.sys"

$linkLibs = @(
    "`"$wdkKmLib\FltMgr.lib`"",
    "`"$wdkKmLib\ntoskrnl.lib`"",
    "`"$wdkKmLib\BufferOverflowK.lib`""
)

$msvcLibPath = $msvcLib
$linkArgs = @(
    "/nologo",
    "/SUBSYSTEM:NATIVE",
    "/DRIVER",
    "/KERNEL",
    "/INTEGRITYCHECK",          # REQUIRED for PsSetCreateProcessNotifyRoutineEx
    "/ENTRY:GsDriverEntry",     # WDK entry point wrapper (handles /GS setup)
    "/NODEFAULTLIB",
    "/MERGE:.rdata=.text",
    "/SECTION:.init,d",         # INIT section is discardable
    "/DEBUG",
    "/INCREMENTAL:NO",
    "/MACHINE:X64",
    "/LIBPATH:`"$msvcLibPath`"",
    "/LIBPATH:`"$wdkKmLib`"",
    "/OUT:`"$sysPath`""
) + $objFiles + $linkLibs

Write-Host "  Linking..." -ForegroundColor DarkGray
$result = & $linkPath @linkArgs 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host $result -ForegroundColor Red
    Write-Fail "Linking failed."
}

Write-Ok "Linked: $sysPath"

# ── Step 10: Verify the output ──────────────────────────────────
if (-not (Test-Path $sysPath)) {
    Write-Fail ".sys file not produced at: $sysPath"
}

$size = (Get-Item $sysPath).Length
Write-Ok "Output: shieldcord_filter.sys ($size bytes)"

# ── Step 11: Sign the driver ─────────────────────────────────────
Write-Step "Signing the driver..."
& "$PSScriptRoot\sign_driver.ps1" -SysPath $sysPath -InfDir "src\driver"

if ($LASTEXITCODE -ne 0) {
    Write-Warn "Signing failed or was skipped. The driver won't load without a signature."
} else {
    Write-Ok "Driver signed successfully."
}

# ── Step 12: Copy to shared folder ───────────────────────────────
$sharedDest = "C:\Users\adich\OneDrive\Documents\Shared Folder\ShieldCord-VM\driver"
if (Test-Path $sharedDest) {
    Write-Step "Copying to shared folder..."
    Copy-Item $sysPath $sharedDest -Force
    $catSrc = "src\driver\shieldcord_filter.cat"
    if (Test-Path $catSrc) { Copy-Item $catSrc $sharedDest -Force }
    Write-Ok "Copied to: $sharedDest"
} else {
    Write-Warn "Shared folder not found at $sharedDest - skipping copy."
    Write-Warn "Manually copy $sysPath to your shared folder."
}

Write-Host ""
Write-Host "  ================================================" -ForegroundColor Green
Write-Host "   Build complete!" -ForegroundColor Green
Write-Host "   $sysPath" -ForegroundColor Green
Write-Host "   Now run install_driver.bat in the VM." -ForegroundColor Green
Write-Host "  ================================================" -ForegroundColor Green
