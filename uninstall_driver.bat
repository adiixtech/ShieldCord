@echo off
:: ============================================================
:: uninstall_driver.bat — ShieldCord Kernel Filter Driver Remove
:: Must be run as Administrator
:: ============================================================

setlocal enabledelayedexpansion

echo.
echo  [*] Uninstalling ShieldCord Kernel Filter Driver...
echo.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [-] Must be run as Administrator.
    pause
    exit /b 1
)

:: Stop the driver
echo [*] Stopping ShieldCordFilter...
sc stop ShieldCordFilter >nul 2>&1
timeout /t 2 /nobreak >nul

:: Remove via pnputil. Once installed, a driver package is copied into the
:: driver store and RENAMED to oemNN.inf, so "/delete-driver shieldcord_filter.inf"
:: silently fails and leaks the package. Resolve the real published oemNN.inf by
:: matching our Original Name in `pnputil /enum-drivers`.
echo [*] Locating published driver package (oemNN.inf)...
set "PUBLISHED_INF="
for /f "tokens=1,2,*" %%A in ('pnputil /enum-drivers 2^>nul') do (
    if /i "%%A %%B"=="Published Name:" set "CUR_INF=%%C"
    if /i "%%A %%B"=="Original Name:" (
        echo %%C | findstr /i /c:"shieldcord_filter.inf" >nul && set "PUBLISHED_INF=!CUR_INF!"
    )
)

if defined PUBLISHED_INF (
    echo [*] Removing published package !PUBLISHED_INF! via pnputil...
    pnputil /delete-driver !PUBLISHED_INF! /uninstall /force >nul 2>&1
) else (
    echo [*] Published package not found; trying original name...
    pnputil /delete-driver shieldcord_filter.inf /uninstall /force >nul 2>&1
)

:: Delete the driver binary
echo [*] Removing driver binary from System32\drivers\...
del /F /Q "%SystemRoot%\System32\drivers\shieldcord_filter.sys" 2>nul

:: Remove the service entry
echo [*] Removing service entry...
sc delete ShieldCordFilter >nul 2>&1

echo.
echo [+] Driver uninstalled.
echo     Rebooting is recommended to fully unload the filter.
echo.
pause
exit /b 0
