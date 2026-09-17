# sign_driver.ps1 - ShieldCord Kernel Driver Signing Script
param(
    [string]$SysPath  = "build_driver\bin\Release\shieldcord_filter.sys",
    [string]$InfDir   = "src\driver",
    [string]$CertName = "ShieldCordDriverCert"
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Write-Step([string]$msg) { Write-Host "`n[*] $msg" -ForegroundColor Cyan }
function Write-Ok([string]$msg)   { Write-Host "[+] $msg" -ForegroundColor Green }
function Write-Fail([string]$msg) { Write-Host "[-] $msg" -ForegroundColor Red; exit 1 }
function Write-Warn([string]$msg) { Write-Host "[!] $msg" -ForegroundColor Yellow }

# ── Find signtool ───────────────────────────────────────────────
Write-Step "Locating signtool.exe..."

$wdkBin = "C:\Program Files (x86)\Windows Kits\10\bin"
$signtool = Get-ChildItem $wdkBin -Recurse -Filter "signtool.exe" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "x64" } |
    Sort-Object { [version]($_.FullName -replace '.*\\(\d+\.\d+\.\d+\.\d+)\\.*', '$1') } -Descending |
    Select-Object -First 1 -ExpandProperty FullName

if (-not $signtool) {
    # Try kits 8.0 or any x64 signtool
    $signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits" -Recurse -Filter "signtool.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "x64" } |
        Select-Object -First 1 -ExpandProperty FullName
}

if (-not $signtool) { Write-Fail "signtool.exe not found." }
Write-Ok "signtool: $signtool"

# ── Check .sys exists ────────────────────────────────────────────
if (-not (Test-Path $SysPath)) {
    Write-Fail ".sys not found at: $SysPath"
}
Write-Ok "Driver binary: $SysPath"

# ── Step 1: Create or reuse self-signed certificate ─────────────
Write-Step "Finding or creating code signing certificate '$CertName'..."

$cert = Get-ChildItem "Cert:\CurrentUser\My" -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -eq "CN=$CertName" } |
    Select-Object -First 1

if (-not $cert) {
    $cert = Get-ChildItem "Cert:\LocalMachine\My" -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -eq "CN=$CertName" } |
        Select-Object -First 1
}

if ($cert) {
    Write-Ok "Reusing existing certificate: $($cert.Thumbprint)"
} else {
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -DnsName $CertName `
        -Subject "CN=$CertName" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -HashAlgorithm SHA256 `
        -KeyLength 2048 `
        -KeyExportPolicy Exportable `
        -NotAfter (Get-Date).AddYears(10)
    Write-Ok "Created new certificate: $($cert.Thumbprint)"
}

# ── Step 2: Export certificate for VM installation ───────────────
Write-Step "Exporting certificate for VM installation..."
$cerPath1 = Join-Path (Split-Path $SysPath) "ShieldCordCert.cer"
$cerPath2 = Join-Path $InfDir "ShieldCordCert.cer"
Export-Certificate -Cert $cert -FilePath $cerPath1 -Force | Out-Null
Export-Certificate -Cert $cert -FilePath $cerPath2 -Force | Out-Null
Write-Ok "Exported: $cerPath1"
Write-Ok "Exported: $cerPath2"

# ── Step 3: Sign the .sys file ───────────────────────────────────
Write-Step "Signing $SysPath ..."

& $signtool sign /a /v /s My /n $CertName /fd sha256 $SysPath
if ($LASTEXITCODE -ne 0) {
    # If standard sign failed, try signing with thumbprint
    & $signtool sign /a /v /sha1 $($cert.Thumbprint) /fd sha256 $SysPath
    if ($LASTEXITCODE -ne 0) {
        Write-Fail "signtool failed on .sys file."
    }
}
Write-Ok ".sys signed successfully."

# ── Step 4: Generate catalog if inf2cat available ────────────────
$inf2cat = Get-ChildItem "C:\Program Files (x86)\Windows Kits" -Recurse -Filter "inf2cat.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName

if ($inf2cat) {
    Write-Step "Generating and signing catalog..."
    Copy-Item $SysPath $InfDir -Force
    & $inf2cat /driver:$InfDir /os:8_X64
    if ($LASTEXITCODE -eq 0) {
        $catPath = Join-Path $InfDir "shieldcord_filter.cat"
        if (Test-Path $catPath) {
            & $signtool sign /a /v /s My /n $CertName /fd sha256 $catPath
            Write-Ok "Catalog signed: $catPath"
        }
    } else {
        Write-Warn "inf2cat skipped catalog generation; driver will install directly via INF with test signing."
    }
} else {
    Write-Warn "inf2cat not found; driver will install via INF directly with test signing."
}

Write-Host ""
Write-Host "=================================================" -ForegroundColor Green
Write-Host " Driver successfully built and signed!" -ForegroundColor Green
Write-Host "=================================================" -ForegroundColor Green
