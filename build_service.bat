@echo off
:: ============================================================
:: ShieldCord — build_service.bat
:: Builds ONLY the C++ service (shieldcord_svc.exe).
:: Does NOT require WDK. Requires VS 2022/2026 + CMake.
:: Run from the ShieldCord project root as Administrator.
:: ============================================================

setlocal EnableDelayedExpansion

echo.
echo  ============================================================
echo   ShieldCord — Service Build Script (No WDK Required)
echo  ============================================================
echo.

:: ── Locate CMake (try VS-bundled first, then PATH) ──────────
set "CMAKE_EXE="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    "C:\Program Files\Microsoft Visual Studio\17\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    "C:\Program Files (x86)\Microsoft Visual Studio\17\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) do (
    if exist %%P (
        set "CMAKE_EXE=%%~P"
        goto :found_cmake
    )
)
where cmake >nul 2>&1
if %errorlevel% == 0 (
    set CMAKE_EXE=cmake
    goto :found_cmake
)

echo [-] CMake not found. Install Visual Studio with C++ workload.
pause
exit /b 1

:found_cmake
echo [+] CMake: %CMAKE_EXE%

:: ── Detect Visual Studio generator ────────────────────────────
set "VS_GEN="
if exist "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" (
    set "VS_GEN=Visual Studio 18 2026"
    set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
    goto :found_vs
)
if exist "C:\Program Files\Microsoft Visual Studio\17\Community\MSBuild\Current\Bin\MSBuild.exe" (
    set "VS_GEN=Visual Studio 17 2022"
    set "MSBUILD=C:\Program Files\Microsoft Visual Studio\17\Community\MSBuild\Current\Bin\MSBuild.exe"
    goto :found_vs
)
echo [-] Visual Studio not found.
pause
exit /b 1

:found_vs
echo [+] Generator: %VS_GEN%

:: ── Configure ─────────────────────────────────────────────────
set BUILD_DIR=build
echo.
echo [*] Configuring CMake...
"%CMAKE_EXE%" -S . -B "%BUILD_DIR%" -G "%VS_GEN%" -A x64 ^
    -DCMAKE_BUILD_TYPE=Release
if %errorlevel% neq 0 (
    echo [-] CMake configuration failed.
    pause
    exit /b 1
)

:: ── Build ─────────────────────────────────────────────────────
echo.
echo [*] Building Release x64...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --parallel
if %errorlevel% neq 0 (
    echo [-] Build failed. Check errors above.
    pause
    exit /b 1
)

:: ── Verify outputs ────────────────────────────────────────────
echo.
if exist "%BUILD_DIR%\bin\Release\shieldcord_svc.exe" (
    echo [+] shieldcord_svc.exe ........... OK
) else (
    echo [-] shieldcord_svc.exe ........... MISSING
)
if exist "%BUILD_DIR%\bin\Release\shieldcord_installer.exe" (
    echo [+] shieldcord_installer.exe ..... OK
) else (
    echo [-] shieldcord_installer.exe ..... MISSING
)
if exist "%BUILD_DIR%\bin\Release\shieldcord_uninstaller.exe" (
    echo [+] shieldcord_uninstaller.exe ... OK
) else (
    echo [-] shieldcord_uninstaller.exe ... MISSING
)

echo.
echo [+] Service build complete!
echo     Next: run package_for_vm.bat to copy everything to the Shared Folder.
echo.
pause
