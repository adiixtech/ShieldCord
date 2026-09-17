@echo off
:: ============================================================
:: package_for_vm.bat — ShieldCord: Package Everything for VirtualBox
::
:: Copies all built files + scripts into:
::   C:\Users\adich\OneDrive\Documents\Shared Folder\ShieldCord-VM\
::
:: What goes in the package:
::   Setup.exe         (install with this - it is the product)
::   Service binaries  (shieldcord_svc.exe, installer, uninstaller, ctl)
::   Driver files      (shieldcord_filter.sys, .inf, .cat, ShieldCordCert.cer)
::   Tray application  (ui\)
::   Test files        (grabber_test.c for compiling inside VM)
::   Config            (service\config.json, copied from tools\config.json)
::   Instructions      (README_INSTALL.txt, copied from tools\README_INSTALL.txt)
::
:: README_INSTALL.txt and service\config.json are COPIED from tools\, not
:: generated here. Both used to be written out as echo lines in this script,
:: and both drifted badly: the README was still telling people to run
:: install_driver.bat and `bcdedit /set nointegritychecks on` long after the
:: first had been superseded and the second forbidden, and the config set a key
:: the engine had removed. See the comments at the copy sites.
:: ============================================================

setlocal EnableDelayedExpansion

set "DEST=C:\Users\adich\OneDrive\Documents\Shared Folder\ShieldCord-VM"
set "SRC=C:\Users\adich\OneDrive\Desktop\ShieldCord"
set "SVC_BIN=%SRC%\build\bin\Release"
set "DRV_BIN=%SRC%\build_driver\bin\Release"

echo.
echo  ============================================================
echo   ShieldCord — Package for VirtualBox Shared Folder
echo  ============================================================
echo   Destination: %DEST%
echo.

:: ── Create folder structure ───────────────────────────────────
echo [*] Creating package structure...
if not exist "%DEST%" mkdir "%DEST%"
if not exist "%DEST%\service"     mkdir "%DEST%\service"
if not exist "%DEST%\driver"      mkdir "%DEST%\driver"
if not exist "%DEST%\scripts"     mkdir "%DEST%\scripts"
if not exist "%DEST%\test_tools"  mkdir "%DEST%\test_tools"
if not exist "%DEST%\ui"          mkdir "%DEST%\ui"
if not exist "%DEST%\setup"       mkdir "%DEST%\setup"

:: ── Service binaries ─────────────────────────────────────────
echo [*] Copying service binaries...
set "MISSING_SVC=0"

if exist "%SVC_BIN%\shieldcord_svc.exe" (
    copy /Y "%SVC_BIN%\shieldcord_svc.exe"         "%DEST%\service\" >nul
    echo [+]   shieldcord_svc.exe
) else (
    echo [-]   MISSING: shieldcord_svc.exe  -- run build_service.bat first!
    set MISSING_SVC=1
)

if exist "%SVC_BIN%\shieldcord_installer.exe" (
    copy /Y "%SVC_BIN%\shieldcord_installer.exe"   "%DEST%\service\" >nul
    echo [+]   shieldcord_installer.exe
)
if exist "%SVC_BIN%\shieldcord_uninstaller.exe" (
    copy /Y "%SVC_BIN%\shieldcord_uninstaller.exe" "%DEST%\service\" >nul
    echo [+]   shieldcord_uninstaller.exe
)
if exist "%SVC_BIN%\shieldcord_ctl.exe" (
    copy /Y "%SVC_BIN%\shieldcord_ctl.exe"         "%DEST%\service\" >nul
    echo [+]   shieldcord_ctl.exe
)
if exist "%SVC_BIN%\shieldcord_driver_setup.exe" (
    copy /Y "%SVC_BIN%\shieldcord_driver_setup.exe" "%DEST%\service\" >nul
    echo [+]   shieldcord_driver_setup.exe
)

:: ── Kernel driver files ───────────────────────────────────────
echo [*] Copying kernel driver files...
set "HAS_DRIVER=0"

if exist "%DRV_BIN%\shieldcord_filter.sys" (
    copy /Y "%DRV_BIN%\shieldcord_filter.sys" "%DEST%\driver\" >nul
    rem  No parens in this echo. An unescaped closing paren would end the
    rem  if-block early, run the else branch as well, and then report the file
    rem  as missing even though it had just been copied.
    echo [+]   shieldcord_filter.sys  - signed
    set HAS_DRIVER=1
) else (
    echo [!]   shieldcord_filter.sys NOT FOUND
    echo       Driver will be built inside VM. Copying source files instead.
    set HAS_DRIVER=0
)

:: Always copy INF and CAT
if exist "%SRC%\src\driver\shieldcord_filter.inf" (
    copy /Y "%SRC%\src\driver\shieldcord_filter.inf" "%DEST%\driver\" >nul
    echo [+]   shieldcord_filter.inf
)
if exist "%SRC%\src\driver\shieldcord_filter.cat" (
    copy /Y "%SRC%\src\driver\shieldcord_filter.cat" "%DEST%\driver\" >nul
    echo [+]   shieldcord_filter.cat
)
if exist "%SRC%\src\driver\ShieldCordCert.cer" (
    copy /Y "%SRC%\src\driver\ShieldCordCert.cer" "%DEST%\driver\" >nul
    echo [+]   ShieldCordCert.cer
)

:: The driver helper looks for its payload in .\driver\ next to itself, so the
:: VM layout must mirror the installed layout for the test to mean anything.
if not exist "%DEST%\service\driver" mkdir "%DEST%\service\driver" >nul 2>&1
if exist "%DRV_BIN%\shieldcord_filter.sys" copy /Y "%DRV_BIN%\shieldcord_filter.sys" "%DEST%\service\driver\" >nul
if exist "%SRC%\src\driver\shieldcord_filter.inf" copy /Y "%SRC%\src\driver\shieldcord_filter.inf" "%DEST%\service\driver\" >nul
if exist "%SRC%\src\driver\shieldcord_filter.cat" copy /Y "%SRC%\src\driver\shieldcord_filter.cat" "%DEST%\service\driver\" >nul
if exist "%SRC%\src\driver\ShieldCordCert.cer"    copy /Y "%SRC%\src\driver\ShieldCordCert.cer"    "%DEST%\service\driver\" >nul

:: ── Tray application ─────────────────────────────────────────
echo [*] Copying the tray application...
set "UI_PUBLISH=%SRC%\src\ui\bin\Release\net10.0-windows\win-x64\publish"
if exist "%UI_PUBLISH%\shieldcordui.exe" (
    xcopy /Y /Q "%UI_PUBLISH%\*" "%DEST%\ui\" >nul
    echo [+]   shieldcordui.exe + dependencies
) else (
    echo [!]   shieldcordui.exe NOT FOUND
    echo       Run: dotnet publish src\ui -c Release -r win-x64 --self-contained false
)

:: ── Setup.exe (if Inno Setup produced one) ────────────────────
echo [*] Copying Setup.exe...
if exist "%SRC%\dist\ShieldCord-Setup-*.exe" (
    copy /Y "%SRC%\dist\ShieldCord-Setup-*.exe" "%DEST%\setup\" >nul
    echo [+]   ShieldCord-Setup.exe
) else (
    echo [i]   No Setup.exe in dist\ -- run build_all.ps1 to produce one
)

:: ── Scripts ──────────────────────────────────────────────────
echo [*] Copying scripts...
copy /Y "%SRC%\install_driver.bat"    "%DEST%\scripts\" >nul && echo [+]   install_driver.bat
copy /Y "%SRC%\uninstall_driver.bat"  "%DEST%\scripts\" >nul && echo [+]   uninstall_driver.bat
copy /Y "%SRC%\install.bat"           "%DEST%\scripts\" >nul && echo [+]   install.bat  (service)

:: ── Grabber test source ───────────────────────────────────────
echo [*] Creating grabber simulation test app...
(
echo #include ^<windows.h^>
echo #include ^<stdio.h^>
echo #include ^<wchar.h^>
echo.
echo // ShieldCord Grabber Simulation Test
echo // Compile: cl grabber_test.c /Fe:grabber_test.exe
echo // Tests whether the kernel driver is blocking file access correctly.
echo.
echo int wmain^(void^) {
echo     wchar_t expanded[512] = {0};
echo     ExpandEnvironmentStringsW^(
echo         L"%%APPDATA%%\\discord\\Local Storage\\leveldb\\CURRENT",
echo         expanded, 512^);
echo.
echo     wprintf^(L"[*] Testing path: %%s\n", expanded^);
echo.
echo     HANDLE h = CreateFileW^(expanded, GENERIC_READ, FILE_SHARE_READ,
echo                             NULL, OPEN_EXISTING, 0, NULL^);
echo.
echo     if ^(h == INVALID_HANDLE_VALUE^) {
echo         DWORD err = GetLastError^(^);
echo         if ^(err == 5^) {
echo             wprintf^(L"[+] BLOCKED ^(ACCESS DENIED^) — Driver is working!\n"^);
echo         } else if ^(err == 2^) {
echo             wprintf^(L"[?] File not found ^(Discord not installed in VM^)\n"^);
echo         } else {
echo             wprintf^(L"[-] Error %%lu\n", err^);
echo         }
echo     } else {
echo         wprintf^(L"[!] OPENED — Driver NOT blocking! BUG!\n"^);
echo         CloseHandle^(h^);
echo     }
echo.
echo     // Also test Chrome cookies
echo     ExpandEnvironmentStringsW^(
echo         L"%%LOCALAPPDATA%%\\Google\\Chrome\\User Data\\Default\\Network\\Cookies",
echo         expanded, 512^);
echo     wprintf^(L"\n[*] Testing Chrome path: %%s\n", expanded^);
echo.
echo     h = CreateFileW^(expanded, GENERIC_READ, FILE_SHARE_READ,
echo                      NULL, OPEN_EXISTING, 0, NULL^);
echo     if ^(h == INVALID_HANDLE_VALUE^) {
echo         DWORD err = GetLastError^(^);
echo         if ^(err == 5^) wprintf^(L"[+] BLOCKED ^(ACCESS DENIED^) — Driver is working!\n"^);
echo         else if ^(err == 2^) wprintf^(L"[?] Chrome not installed in VM\n"^);
echo         else wprintf^(L"[-] Error %%lu\n", err^);
echo     } else {
echo         wprintf^(L"[!] OPENED — Driver NOT blocking!\n"^);
echo         CloseHandle^(h^);
echo     }
echo.
echo     wprintf^(L"\nPress Enter to exit...\n"^);
echo     getchar^(^);
echo     return 0;
echo }
) > "%DEST%\test_tools\grabber_test.c"
echo [+]   grabber_test.c

:: ── Config file for the service ───────────────────────────────
:: COPIED, not generated. This script used to write out its own JSON, and by
:: the time anyone looked it had drifted completely: it set
:: "daclGatekeeperEnabled", a key the engine dropped when the DACL layer was
:: removed, and it omitted every driver* key. tools\config.json is the
:: documented schema, so it is the one that ships.
::
:: The file is optional and the engine says so: ConfigManager::Load applies a
:: default to every key it does not find, and the Config struct's own defaults
:: are all-true for the protection flags. A machine with no config.json at all
:: is fully protected - which matters, because "is protection on?" must never
:: depend on whether a file happened to be copied.
::
:: It goes to C:\ProgramData\ShieldCord\config.json, which is what SC_CONFIG_FILE
:: points at - NOT next to the exe.
echo [*] Copying the service config...
if exist "%SRC%\tools\config.json" (
    copy /Y "%SRC%\tools\config.json" "%DEST%\service\config.json" >nul
    echo [+]   config.json  - copied from tools\config.json
) else (
    echo [!]   tools\config.json NOT FOUND
)

:: ── README ────────────────────────────────────────────────────
:: Also copied rather than generated. The old version was a few hundred echo
:: lines inside this script, which is how it came to still be telling people to
:: run install_driver.bat and `bcdedit /set nointegritychecks on` long after
:: both had stopped being true. A plain file can be read and corrected by
:: anyone, and cannot be broken by an unescaped bracket.
echo [*] Copying the README...
if exist "%SRC%\tools\README_INSTALL.txt" (
    copy /Y "%SRC%\tools\README_INSTALL.txt" "%DEST%\README_INSTALL.txt" >nul
    echo [+]   README_INSTALL.txt  - copied from tools\README_INSTALL.txt
) else (
    echo [!]   tools\README_INSTALL.txt NOT FOUND
)

echo.
echo  ============================================================
if %MISSING_SVC% == 1 (
    echo  [!] WARNING: shieldcord_svc.exe was missing!
    echo      Run build_service.bat first, then re-run this script.
)
if %HAS_DRIVER% == 0 (
    echo  [!] NOTE: Kernel driver .sys was not found.
    echo      See README_INSTALL.txt for instructions to build in VM.
)
echo  [+] Package ready at:
echo      %DEST%
echo.
echo  Map this folder as a Shared Folder in VirtualBox:
echo    Settings ^> Shared Folders ^> Add
echo    Host Path: C:\Users\adich\OneDrive\Documents\Shared Folder
echo    Name: ShieldCord
echo    Auto-mount: YES  ^|  Make Permanent: YES
echo  ============================================================
echo.
pause
exit /b 0
