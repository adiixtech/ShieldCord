/*
 * path_filter.h — ShieldCord Kernel Filter: Protected Path Matching
 *
 * Maintains a list of NT-native path prefixes that the driver protects.
 * Matching is case-insensitive Unicode prefix comparison — zero allocations
 * in the hot path.
 *
 * NT native paths vs Win32 paths:
 *   Win32:  C:\Users\adich\AppData\Roaming\discord\Local Storage\leveldb
 *   Native: \Device\HarddiskVolume3\Users\adich\AppData\Roaming\discord\...
 *
 * FltGetFileNameInformation returns native paths. Protected paths MUST be
 * stored in native format. PathFilterInit converts them at driver load.
 */

#pragma once

#include <fltKernel.h>

/* Maximum number of protected path prefixes */
#define SC_MAX_PATHS        32u

/*
 * PathFilterInit
 * Converts the built-in Win32 path list to NT native paths and stores them.
 * Called from DriverEntry. Must succeed before filtering starts.
 *
 * Returns STATUS_SUCCESS or an NTSTATUS error.
 * On failure the driver should unload (no paths means no protection).
 */
NTSTATUS PathFilterInit(VOID);

/*
 * PathFilterDestroy
 * Frees all stored native path strings.
 * Called from ScFilterUnload before FltUnregisterFilter.
 */
VOID PathFilterDestroy(VOID);

/*
 * IsPathProtected
 * Checks whether FilePath starts with any of the protected NT native prefixes.
 * Case-insensitive. Zero allocations. Thread-safe (paths are read-only after init).
 *
 * FilePath: the Name field from a parsed FltGetFileNameInformation result.
 * Returns TRUE if this path should be subject to PID trust checks.
 */
BOOLEAN IsPathProtected(
    _In_ PUNICODE_STRING     FilePath
);

/*
 * PathFilterUpdateFromService
 * Called by the comm port message handler when the service sends
 * ScMsgSetProtectedPaths with a runtime-resolved path list.
 * Replaces the current path list atomically.
 * 
 * Paths: array of null-terminated wide-char NT native path strings.
 * Count: number of entries in Paths.
 */
NTSTATUS PathFilterUpdateFromService(
    _In_reads_(Count) PWCHAR* Paths,
    _In_ ULONG               Count
);
