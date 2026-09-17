# ShieldCord Kernel Minifilter Driver — WDK CMake Build
#
# PREREQUISITES:
#   1. WDK for Windows 11 installed: https://aka.ms/wdk
#   2. WDK Visual Studio Extension installed (comes with WDK)
#   3. FindWDK.cmake from: https://github.com/SergiusTheBest/FindWDK
#      Copy FindWDK.cmake into: <WDK_DIR>/cmake/FindWDK.cmake
#      OR use the one bundled with the WDK VS extension.
#
# BUILD COMMAND (from ShieldCord root, after WDK is installed):
#   cmake -B build_driver -S src/driver ^
#         -G "Visual Studio 17 2022" -A x64 ^
#         -DWDK_DIR="C:/Program Files (x86)/Windows Kits/10"
#   cmake --build build_driver --config Release
#
# ALTERNATIVE: Open Kernal Driver\shieldcord_filter.vcxproj directly
# in Visual Studio — this is the recommended approach.

cmake_minimum_required(VERSION 3.20)
project(ShieldCordFilter C)   # Kernel drivers are C, not C++

# ── Locate WDK ─────────────────────────────────────────────────
if(NOT DEFINED WDK_DIR)
    # Auto-detect WDK install path from registry
    get_filename_component(WDK_DIR
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
        ABSOLUTE)
endif()

if(NOT EXISTS "${WDK_DIR}")
    message(FATAL_ERROR
        "Windows Driver Kit not found at '${WDK_DIR}'.\n"
        "Install from https://aka.ms/wdk and set -DWDK_DIR=<path>")
endif()

message(STATUS "Using WDK at: ${WDK_DIR}")

list(APPEND CMAKE_MODULE_PATH "${WDK_DIR}/cmake")
find_package(WDK REQUIRED)

# ── Driver target ───────────────────────────────────────────────
wdk_add_driver(shieldcord_filter
    filter/shieldcord_filter.c
    filter/pre_create.c
    filter/process_cache.c
    filter/comm_port.c
    filter/path_filter.c
)

# ── Compile options ─────────────────────────────────────────────
target_compile_options(shieldcord_filter PRIVATE
    /W4             # Enable all warnings
    /WX             # Treat warnings as errors
    /GS-            # Disable security cookies (kernel doesn't support them)
    /Gz             # __stdcall calling convention (required for kernel ABI)
    /Zp8            # Pack structs on 8-byte boundary (matches user-mode side)
    /wd4201         # Allow nameless struct/union (common in WDK headers)
    /wd4214         # Allow bitfield types other than int (WDK headers)
)

# ── Preprocessor defines ────────────────────────────────────────
target_compile_definitions(shieldcord_filter PRIVATE
    POOL_NX_OPTIN=1         # Opt-in to non-executable pool by default
    _WIN64                  # Explicitly 64-bit
)

# ── Link options ─────────────────────────────────────────────────
target_link_options(shieldcord_filter PRIVATE
    /SUBSYSTEM:NATIVE
    /DRIVER
    /INTEGRITYCHECK         # REQUIRED for PsSetCreateProcessNotifyRoutineEx
    /NODEFAULTLIB
    /MERGE:.rdata=.text     # Merge read-only data into text section
    /SECTION:.init,d        # Mark INIT section as discardable
)

# ── Link libraries ───────────────────────────────────────────────
target_link_libraries(shieldcord_filter PRIVATE
    FltMgr.lib
    ntoskrnl.lib
)

# ── Output directory ────────────────────────────────────────────
set_target_properties(shieldcord_filter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

message(STATUS "ShieldCord Filter Driver configured.")
message(STATUS "  Altitude: 385200 (FSFilter Activity Monitor)")
message(STATUS "  Build: cmake --build . --config Release")
