@echo off
:: ============================================================
:: ShieldCord — install.bat
:: Copy built binaries and run installer as Administrator.
:: Run this AFTER build.bat succeeds.
:: ============================================================

:: Self-elevate to Administrator
net session >nul 2>&1
if %ERRORLEVEL% NEq 0 (
    echo [*] Requesting Administrator privileges...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

setlocal
set BUILD_BIN=build\bin\Release

if not exist "%BUILD_BIN%\shieldcord_svc.exe" (
    echo [!] Build output not found. Run build.bat first.
    pause
    exit /b 1
)

echo.
echo  ============================================
echo   ShieldCord Installer
echo  ============================================
echo.

:: Copy to a temp staging directory alongside the installer
set STAGE=%TEMP%\ShieldCordStage
mkdir "%STAGE%" 2>nul
copy /Y "%BUILD_BIN%\shieldcord_svc.exe"          "%STAGE%\" >nul
copy /Y "%BUILD_BIN%\shieldcord_installer.exe"     "%STAGE%\" >nul
copy /Y "%BUILD_BIN%\shieldcord_uninstaller.exe"   "%STAGE%\" >nul

:: Run the installer
"%STAGE%\shieldcord_installer.exe"

echo.
echo [+] Done. ShieldCord is now protecting your tokens.
echo     Check logs at: C:\ProgramData\ShieldCord\logs\shieldcord.log
echo.
pause
