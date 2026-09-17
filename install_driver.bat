@echo off
:: ============================================================
:: install_driver.bat - ShieldCord Kernel Filter Driver Install
:: Run this INSIDE the VirtualBox VM as Administrator.
:: ============================================================

setlocal EnableDelayedExpansion

echo.
echo  +---------------------------------------------------+
echo  ^|   ShieldCord Kernel Filter Driver Installer       ^|
echo  ^|   Altitude 385200 - FSFilter Activity Monitor     ^|
echo  +---------------------------------------------------+
echo.

:: ── Check for Administrator ──────────────────────────────────
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [-] ERROR: This script must be run as Administrator.
    echo     Right-click install_driver.bat -^> Run as administrator
    pause
    exit /b 1
)

:: ── Find driver files (check current folder, ..\driver, or driver\) ─────
set "SCRIPT_DIR=%~dp0"
set "SYS_SRC="
set "INF_SRC="
set "CER_SRC="

if exist "%SCRIPT_DIR%shieldcord_filter.sys" (
    set "SYS_SRC=%SCRIPT_DIR%shieldcord_filter.sys"
    set "INF_SRC=%SCRIPT_DIR%shieldcord_filter.inf"
    set "CER_SRC=%SCRIPT_DIR%ShieldCordCert.cer"
) else if exist "%SCRIPT_DIR%..\driver\shieldcord_filter.sys" (
    set "SYS_SRC=%SCRIPT_DIR%..\driver\shieldcord_filter.sys"
    set "INF_SRC=%SCRIPT_DIR%..\driver\shieldcord_filter.inf"
    set "CER_SRC=%SCRIPT_DIR%..\driver\ShieldCordCert.cer"
) else if exist "%SCRIPT_DIR%driver\shieldcord_filter.sys" (
    set "SYS_SRC=%SCRIPT_DIR%driver\shieldcord_filter.sys"
    set "INF_SRC=%SCRIPT_DIR%driver\shieldcord_filter.inf"
    set "CER_SRC=%SCRIPT_DIR%driver\ShieldCordCert.cer"
) else (
    echo [-] Driver binary not found.
    echo     Looking for shieldcord_filter.sys in:
    echo       %SCRIPT_DIR%
    echo       %SCRIPT_DIR%..\driver\
    echo       %SCRIPT_DIR%driver\
    pause
    exit /b 1
)

if not exist "%INF_SRC%" (
    echo [-] INF file not found: %INF_SRC%
    pause
    exit /b 1
)

echo [+] Driver:  %SYS_SRC%
echo [+] INF:     %INF_SRC%

:: ── Check test signing ────────────────────────────────────────
echo.
echo [*] Checking test signing mode...
bcdedit | findstr /i "testsigning" | findstr /i "Yes" >nul
if %errorlevel% neq 0 (
    echo [!] WARNING: Test signing does not appear to be enabled!
    echo     Run as Admin then reboot:
    echo       bcdedit /set testsigning on
    echo       bcdedit /set nointegritychecks on
    echo.
    choice /C YN /M "Continue anyway (will likely fail)?"
    if errorlevel 2 exit /b 1
) else (
    echo [+] Test signing: ENABLED
)

:: ── Stop existing driver (if already running) ────────────────
echo.
echo [*] Stopping existing ShieldCordFilter (if running)...
fltMC unload ShieldCordFilter >nul 2>&1
sc stop ShieldCordFilter >nul 2>&1
timeout /t 1 /nobreak >nul

:: ── Install driver certificate into VM Root & TrustedPublisher ──
echo.
if exist "%CER_SRC%" (
    echo [*] Trusting driver certificate...
    certutil -addstore -f "Root" "%CER_SRC%" >nul 2>&1
    certutil -addstore -f "TrustedPublisher" "%CER_SRC%" >nul 2>&1
    echo [+] Certificate trusted.
) else (
    echo [!] Note: ShieldCordCert.cer not found. Relying on test signing mode.
)

:: ── Copy .sys to System32\drivers\ ───────────────────────────
echo [*] Installing driver binary to System32\drivers\...
copy /Y "%SYS_SRC%" "%SystemRoot%\System32\drivers\shieldcord_filter.sys" >nul
if %errorlevel% neq 0 (
    echo [-] Failed to copy driver to System32\drivers\.
    echo     Make sure you ran as Administrator.
    pause
    exit /b 1
)
echo [+] Driver binary copied.

:: ── Register with Filter Manager ─────────────────────────────
echo.
echo [*] Registering driver with Filter Manager...

:: Method 1: SetupAPI DefaultInstall
rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall 128 "%INF_SRC%" >nul 2>&1

:: Method 2: Stage in Driver Store with pnputil
pnputil /add-driver "%INF_SRC%" >nul 2>&1

:: Method 3: Direct Service and Filter Manager registry keys
sc create ShieldCordFilter type= filesys start= demand binPath= "%SystemRoot%\System32\drivers\shieldcord_filter.sys" group= "FSFilter Activity Monitor" depend= FltMgr >nul 2>&1
sc config ShieldCordFilter type= filesys start= demand binPath= "%SystemRoot%\System32\drivers\shieldcord_filter.sys" group= "FSFilter Activity Monitor" depend= FltMgr >nul 2>&1
reg add "HKLM\System\CurrentControlSet\Services\ShieldCordFilter" /v "SupportedFeatures" /t REG_DWORD /d 3 /f >nul 2>&1
reg add "HKLM\System\CurrentControlSet\Services\ShieldCordFilter\Instances" /v "DefaultInstance" /t REG_SZ /d "ShieldCordFilter Instance" /f >nul 2>&1
reg add "HKLM\System\CurrentControlSet\Services\ShieldCordFilter\Instances\ShieldCordFilter Instance" /v "Altitude" /t REG_SZ /d "385200" /f >nul 2>&1
reg add "HKLM\System\CurrentControlSet\Services\ShieldCordFilter\Instances\ShieldCordFilter Instance" /v "Flags" /t REG_DWORD /d 0 /f >nul 2>&1

echo [+] Driver registered.

:: ── Start the driver ─────────────────────────────────────────
echo.
echo [*] Starting ShieldCordFilter...
fltMC load ShieldCordFilter >nul 2>&1
if %errorlevel% neq 0 (
    sc start ShieldCordFilter >nul 2>&1
)

:: Check status
sc query ShieldCordFilter | findstr /i "RUNNING" >nul
if %errorlevel% equ 0 (
    echo [+] ShieldCordFilter driver is RUNNING!
) else (
    echo [!] Starting via fltMC...
    fltMC load ShieldCordFilter
    sc query ShieldCordFilter
)

:: ── Verify it loaded ─────────────────────────────────────────
echo.
echo [*] Filter Manager active filters:
fltMC filters
echo.
echo  ------------------------------------------------------
echo  Look for "ShieldCordFilter" at altitude "385200" above.
echo  ------------------------------------------------------
echo.
echo [+] Installation process finished!
echo.
echo NEXT STEPS:
echo   1. Run the service:
echo      C:\ShieldCord\shieldcord_svc.exe --console
echo.
echo   2. Run grabber_test.exe to verify token protection.
echo.
pause
exit /b 0
