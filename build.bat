@echo off
:: ============================================================
:: ShieldCord — build.bat
:: Configure and build using CMake + Visual Studio 2022
:: Run from the ShieldCord project root directory.
:: ============================================================

setlocal

set BUILD_DIR=build
set CONFIG=Release

echo.
echo  ============================================
echo   ShieldCord Build Script
echo  ============================================
echo.

:: Find CMake
where cmake >nul 2>&1
if %ERRORLEVEL% NEq 0 (
    :: Try the VS-bundled CMake
    set CMAKE_PATH="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) else (
    set CMAKE_PATH=cmake
)

:: Configure (generate VS 2022 solution)
echo [*] Configuring with CMake...
%CMAKE_PATH% -S . -B %BUILD_DIR% -G "Visual Studio 18 2026" -A x64
if %ERRORLEVEL% NEq 0 (
    echo [!] CMake configuration failed.
    pause
    exit /b 1
)

echo.
echo [*] Building %CONFIG% configuration...
%CMAKE_PATH% --build %BUILD_DIR% --config %CONFIG% --parallel
if %ERRORLEVEL% NEq 0 (
    echo [!] Build failed. Check the output above.
    pause
    exit /b 1
)

echo.
echo [+] Build succeeded!
echo     Outputs:
echo       %BUILD_DIR%\bin\Release\shieldcord_svc.exe
echo       %BUILD_DIR%\bin\Release\shieldcord_installer.exe
echo       %BUILD_DIR%\bin\Release\shieldcord_uninstaller.exe
echo.
echo [*] To install: run install.bat as Administrator
echo.
pause
