<#
.SYNOPSIS
  Automatic ShieldCord stress workload for the Driver Verifier window.
  RUN ONLY INSIDE THE TEST VM - NEVER ON THE HOST.
  Elevated. The shieldcord service console must already be running.

  Run (VM, admin cmd):
    powershell -ExecutionPolicy Bypass -File Z:\ShieldCord-VM\tools\stress_test.ps1 -Minutes 30

  Every external step is hard-time-limited (killed on timeout), the deadline is
  enforced inside a cycle, and a heartbeat is logged after every phase.
  Expect the VM to be slow under Verifier - that is normal.
#>
param(
    [int]    $Minutes    = 30,
    [string] $Ctl        = "C:\ShieldCord\shieldcord_ctl.exe",
    [string] $Grabber    = "Z:\ShieldCord-VM\test_tools\grabber_test.exe",
    [string] $LogPath    = "C:\ShieldCord\stress_test.log"
)
$ErrorActionPreference = "Continue"

function Log([string]$msg) {
    $line = "{0}  {1}" -f (Get-Date -Format "HH:mm:ss"), $msg
    Write-Host $line
    Add-Content -Path $LogPath -Value $line -Encoding UTF8
}

# ---------- sanity ----------
if (-not (Test-Path $Ctl))     { Write-Host "shieldcord_ctl not found at: $Ctl";   exit 1 }
if (-not (Test-Path $Grabber)) { Write-Host "grabber_test not found at: $Grabber"; exit 1 }
if (-not ((& $Ctl status 2>&1 | Out-String) -notmatch "cannot connect")) {
    Write-Host "ShieldCord service not reachable - start the service console first."; exit 1
}

$script:deadline  = (Get-Date).AddMinutes($Minutes)
$script:cycle     = 0
$script:blocked   = 0
$script:opened    = 0
$script:notfound  = 0
$script:emptyRuns = 0
$workDir = Join-Path $env:TEMP "scstress"
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

Log ("=== STRESS START: {0} minute window. Ctrl+C to stop. ===" -f $Minutes)

# Run a ctl verb with a hard timeout.
function Invoke-Ctl([string]$argsLine) {
    $p = Start-Process -FilePath $Ctl -ArgumentList $argsLine -WindowStyle Hidden -PassThru
    if (-not $p.WaitForExit(20000)) {
        try { & taskkill /PID $p.Id /T /F 2>$null | Out-Null } catch {}
        Log "  [timeout] ctl '$argsLine' killed"
    }
}

# Run grabber_test, capture stdout, hard timeout. Direct .NET process (no shell
# quoting to mangle) with async reads so it can never deadlock.
function Invoke-Grabber {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName               = $Grabber
    $psi.UseShellExecute        = $false
    $psi.CreateNoWindow         = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    try {
        $p = New-Object System.Diagnostics.Process
        $p.StartInfo = $psi
        if (-not $p.Start()) { Log "  [err] grabber failed to start"; return "" }
    } catch { Log ("  [err] grabber start: " + $_); return "" }

    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(120000)) {
        try { $p.Kill() } catch {}
        Log "  [timeout] grabber killed after 120 s"
    }
    try { $p.WaitForExit() } catch {}            # let async readers finish
    $out = ""; $err = ""
    try { $out = $outTask.Result } catch {}
    try { $err = $errTask.Result } catch {}
    return ($(if ($out) { $out } else { $err }))
}

function TimeUp { return ((Get-Date) -ge $script:deadline) }

# Force every control back to SAFE: enforce/file-block/alerts ON.
function Reset-Safe {
    Invoke-Ctl "alerts on"
    Invoke-Ctl "file-block on"
    Invoke-Ctl "enforce on"
}

function Do-ToggleStorm {
    foreach ($s in @("off","on","off","on")) { Invoke-Ctl "enforce $s";    if (TimeUp) { return } }
    foreach ($s in @("off","on","off","on")) { Invoke-Ctl "file-block $s"; if (TimeUp) { return } }
    foreach ($s in @("off","on"))            { Invoke-Ctl "alerts $s";     if (TimeUp) { return } }
    Reset-Safe
}

function Do-GrabberRun {
    $text = Invoke-Grabber
    if (-not $text) {
        $script:emptyRuns++
        Log "  [warn] grabber produced NO output"
        return
    }
    # Count ONLY real result lines (start "==> "), never the legend text.
    $den = ([regex]::Matches($text, "(?m)^\s*==>\s*ACCESS DENIED")).Count
    $opn = ([regex]::Matches($text, "(?m)^\s*==>\s*OPENED")).Count
    $nfn = ([regex]::Matches($text, "(?m)^\s*==>\s*(NOT FOUND|SHARING VIOLATION)")).Count

    if (($den + $opn + $nfn) -eq 0) {
        $head = (($text -split "`r?`n") | Select-Object -First 3) -join "  |  "
        Log "  [warn] grabber output had no result lines -> $head"
    }

    $script:blocked  += $den
    $script:opened   += $opn
    $script:notfound += $nfn

    if ($opn -gt 0) {                       # report exactly which path leaked
        $lines = $text -split "`r?`n"
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match "==>\s*OPENED") {
                $prev = if ($i -ge 1) { $lines[$i-1].Trim() } else { "" }
                Log "  !! OPENED on: $prev"
            }
        }
    }
}

function Do-Churn {
    for ($i = 0; $i -lt 15; $i++) {
        Start-Process "powershell.exe" -ArgumentList "-NoProfile -Command exit" `
            -WindowStyle Hidden | Out-Null
    }
    Start-Sleep -Seconds 3
}

function Do-IoChurn {
    try {
        $src = Join-Path $workDir "tree"
        if (Test-Path $src) { Remove-Item $src -Recurse -Force -ErrorAction SilentlyContinue }
        New-Item -ItemType Directory -Force -Path $src | Out-Null
        for ($d = 0; $d -lt 8; $d++) {
            $dir = Join-Path $src ("d" + $d)
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
            for ($f = 0; $f -lt 100; $f++) {
                [System.IO.File]::WriteAllText((Join-Path $dir ("f{0}.dat" -f $f)), ("x"*128))
            }
        }
        $copyTgt = Join-Path $workDir "copy1"
        if (Test-Path $copyTgt) { Remove-Item $copyTgt -Recurse -Force -ErrorAction SilentlyContinue }
        $p = Start-Process robocopy.exe -ArgumentList ('"'+$src+'" "'+$copyTgt+'" /E /R:0 /W:0 /NFL /NDL /NJH /NJS /NP') `
                -WindowStyle Hidden -PassThru
        if (-not $p.WaitForExit(180000)) {
            try { & taskkill /PID $p.Id /T /F 2>$null | Out-Null } catch {}
            Log "  [timeout] robocopy killed"
        }
        if (Test-Path $copyTgt) { Remove-Item $copyTgt -Recurse -Force -ErrorAction SilentlyContinue }
        Log "  io: wrote+copied 800 small files"
    } catch { Log ("  io error: " + $_) }
}

# ---------- main loop (deadline enforced inside the cycle) ----------
while (-not (TimeUp)) {
    $script:cycle++
    Log ("--- cycle {0} ---" -f $script:cycle)

    Log "  toggle storm...";  Do-ToggleStorm; if (TimeUp) { break }
    Log "  grabber probe..."; Do-GrabberRun;  if (TimeUp) { break }
    Log "  grabber probe..."; Do-GrabberRun;  if (TimeUp) { break }
    Log "  churn...";         Do-Churn;       if (TimeUp) { break }
    Log "  file io...";       Do-IoChurn;     if (TimeUp) { break }
    Log ("  [tally] blocked={0} opened={1} (no-result runs={2})" `
            -f $script:blocked,$script:opened,$script:emptyRuns)

    Start-Sleep -Seconds 2
}

Reset-Safe
Log ""
Log "=== STRESS COMPLETE ==="
Log ("  cycles run        : {0}" -f $script:cycle)
Log ("  grabber BLOCKED   : {0}" -f $script:blocked)
Log ("  grabber OPENED    : {0}" -f $script:opened)
Log ("  NOT FOUND/etc     : {0}" -f $script:notfound)
Log ("  empty grabber runs: {0}" -f $script:emptyRuns)
Log "Result: no blue screens + no OPENED rows = driver passed."
Log "  (blocked should be >> 0 - if it is 0, grabber capture still broken.)"
Log "Next: verifier /reset  then reboot to turn Driver Verifier off."
