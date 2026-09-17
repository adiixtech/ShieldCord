cmake_minimum_required(VERSION 3.20)
project(ShieldCord LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static C/C++ runtime library (/MT) for standalone execution without VCRUNTIME140.dll
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ─── Compiler Flags ──────────────────────────────────────────
if (MSVC)
    add_compile_options(/W4 /MP /utf-8 /wd4005)
    # _CRT_SECURE_NO_WARNINGS: suppress safe-string warnings in MSVC
    # WIN32_LEAN_AND_MEAN: already in common.h but set globally for consistency
    # Do NOT set NOMINMAX here - common.h defines it to prevent min/max macro conflicts
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
endif()

# ─── Include paths ──────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/src/shared
    ${CMAKE_SOURCE_DIR}/src/service
    ${CMAKE_SOURCE_DIR}/src/driver/filter   # for driver_protocol.h
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/external/json    # nlohmann/json header-only
)

# ─── shieldcord_svc ─────────────────────────────────────────
set(SVC_SOURCES
    src/service/main.cpp
    src/service/service_controller.cpp
    src/service/dacl_gatekeeper.cpp
    src/service/process_validator.cpp
    src/service/process_notifier.cpp
    src/service/app_watcher.cpp
    src/service/memory_monitor.cpp
    src/service/token_vault.cpp
    src/service/ipc_server.cpp
    src/service/ipc_verbs.cpp        # NEW: shared verb handler (service + console)
    src/service/alert_history.cpp    # NEW: bounded alert history + the threat count
    src/service/logger.cpp
    src/service/config.cpp
    src/service/utils.cpp
    src/service/driver_client.cpp     # NEW: kernel driver IPC client
)

add_executable(shieldcord_svc ${SVC_SOURCES})

# Windows subsystem for the service (console so --console mode also works)
set_target_properties(shieldcord_svc PROPERTIES
    WIN32_EXECUTABLE FALSE      # keep as console subsystem for --console mode
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(shieldcord_svc PRIVATE
    advapi32    # SCM, security, DACL
    wintrust    # WinVerifyTrust
    crypt32     # CryptQueryObject, DPAPI
    bcrypt      # AES-GCM (future vault)
    ntdll       # NtQuerySystemInformation
    ws2_32      # Winsock (future)
    psapi       # GetProcessImageFileName
    shlwapi     # Path helpers
    fltlib      # NEW: Filter Manager user-mode API (FilterConnectCommunicationPort)
)

# ─── shieldcord_installer ───────────────────────────────────
add_executable(shieldcord_installer src/installer/install.cpp)
target_link_libraries(shieldcord_installer PRIVATE advapi32 shlwapi)
set_target_properties(shieldcord_installer PROPERTIES WIN32_EXECUTABLE FALSE)

# ─── shieldcord_driver_setup ────────────────────────────────
# Installs/uninstalls the minifilter through Win32 APIs. Replaces
# install_driver.bat, which could not be shipped (it shelled out to
# rundll32/pnputil/sc/reg and reported success regardless).
add_executable(shieldcord_driver_setup src/installer/driver_setup.cpp)
target_link_libraries(shieldcord_driver_setup PRIVATE advapi32 shlwapi crypt32 fltlib)
set_target_properties(shieldcord_driver_setup PROPERTIES WIN32_EXECUTABLE FALSE)

# ─── shieldcord_uninstaller ─────────────────────────────────
add_executable(shieldcord_uninstaller src/installer/uninstall.cpp)
target_link_libraries(shieldcord_uninstaller PRIVATE advapi32 shlwapi)
set_target_properties(shieldcord_uninstaller PROPERTIES WIN32_EXECUTABLE FALSE)

# ─── shieldcord_ctl ─────────────────────────────────────────
# CLI control tool — sends JSON commands over the ShieldCord named pipe.
# Self-contained (Win32 + stdlib only), no project deps.
add_executable(shieldcord_ctl src/ctl/shieldcord_ctl.cpp)
set_target_properties(shieldcord_ctl PROPERTIES WIN32_EXECUTABLE FALSE)

# ─── shieldcord_watermark ───────────────────────────────────
# A DLL, not an executable: the tray app loads it INTO explorer.exe to
# suppress the "Test Mode" desktop watermark. user32 is needed for the
# LoadStringW call used to read the watermark strings out of shell32.
# See src/watermark/watermark_hook.cpp for why this is a user-mode shell
# problem and deliberately not a driver one.
add_library(shieldcord_watermark SHARED src/watermark/watermark_hook.cpp)
target_link_libraries(shieldcord_watermark PRIVATE user32)
set_target_properties(shieldcord_watermark PROPERTIES
    PREFIX ""                   # shieldcord_watermark.dll, not libshieldcord_watermark.dll
)

# CFG, and it is not optional here. explorer.exe and shell32.dll are compiled
# with /guard:cf, which validates indirect call targets - including calls made
# through an IAT. Our hook functions ARE such targets, so if they are not in the
# process's valid-call-target bitmap the check raises
# FAST_FAIL_INVALID_INDIRECT_CALL_TARGET (0xC0000409) and TERMINATES EXPLORER.
# Exported functions are added to the bitmap by the loader, which is why
# ScHook* in watermark_hook.cpp are exported rather than file-local. Building
# with /guard:cf keeps our own image consistent on top of that.
target_compile_options(shieldcord_watermark PRIVATE /guard:cf)
target_link_options(shieldcord_watermark PRIVATE /guard:cf)

# ─── Output to build/Release ────────────────────────────────
set_target_properties(shieldcord_svc shieldcord_installer shieldcord_uninstaller
                      shieldcord_driver_setup shieldcord_ctl shieldcord_watermark
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

message(STATUS "ShieldCord build configured.")
message(STATUS "  Install nlohmann/json: external/json/nlohmann/json.hpp  ✓")
