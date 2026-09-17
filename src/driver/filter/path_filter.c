/*
 * path_filter.c — ShieldCord Kernel Filter: Protected Path Matching
 *
 * Converts Win32 paths (from the hardcoded list) to NT-native device paths
 * at DriverEntry time, stores them in non-paged memory, and provides
 * IsPathProtected() for zero-allocation prefix matching in the hot path.
 *
 * Native path conversion uses IoVolumeDeviceToDosName in reverse (via
 * RtlDosPathNameToNtPathName_U) to get the \Device\HarddiskVolume*\... form
 * that FltGetFileNameInformation returns.
 *
 * The service may later send an updated list via ScMsgSetProtectedPaths,
 * which replaces the built-in list atomically.
 */

#include <fltKernel.h>
#include "path_filter.h"

/* Pool tag: 'ScPF' */
#define SC_POOL_TAG_PATH    'FPcS'

/*
 * Built-in Win32 path suffixes appended to the user profile root.
 * These are the same directories as in token_paths.h in the service.
 * We only need the per-user AppData portions since we will resolve the
 * current user's profile at runtime.
 *
 * IMPORTANT: We protect DIRECTORY PREFIXES, not individual files.
 * Any file opened under these directories is subject to the trust check.
 */
static const WCHAR* sc_Win32Paths[] = {
    /* Discord */
    L"\\AppData\\Roaming\\discord\\Local Storage\\leveldb",
    L"\\AppData\\Roaming\\discord\\Session Storage",
    L"\\AppData\\Roaming\\discordptb\\Local Storage\\leveldb",
    L"\\AppData\\Roaming\\discordcanary\\Local Storage\\leveldb",

    /* Google Chrome */
    L"\\AppData\\Local\\Google\\Chrome\\User Data\\Default\\Network",
    L"\\AppData\\Local\\Google\\Chrome\\User Data\\Default\\Local Storage",
    L"\\AppData\\Local\\Google\\Chrome\\User Data\\Default\\Session Storage",

    /* Brave */
    L"\\AppData\\Local\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Network",
    L"\\AppData\\Local\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Local Storage",

    /* Microsoft Edge */
    L"\\AppData\\Local\\Microsoft\\Edge\\User Data\\Default\\Network",
    L"\\AppData\\Local\\Microsoft\\Edge\\User Data\\Default\\Local Storage",

    /* Opera */
    L"\\AppData\\Local\\Opera Software\\Opera Stable\\Network",
    L"\\AppData\\Local\\Opera Software\\Opera Stable\\Local Storage",

    /* Firefox */
    L"\\AppData\\Roaming\\Mozilla\\Firefox\\Profiles",

    /* ShieldCord decoy token folder (ProgramData — matched by the widened
       fast gate below; only ShieldCord's own processes may touch it) */
    L"\\ProgramData\\ShieldCord\\Decoy",
};

/* ── Stored native paths ─────────────────────────────────────── */
typedef struct _SC_NATIVE_PATH {
    UNICODE_STRING  Prefix;     /* NT-native path prefix or fallback substring in non-paged memory */
    BOOLEAN         Active;
} SC_NATIVE_PATH, *PSC_NATIVE_PATH;

static SC_NATIVE_PATH   g_Paths[SC_MAX_PATHS];
static ULONG            g_PathCount = 0;
static FAST_MUTEX       g_PathLock;     /* protects the path array during updates */
static BOOLEAN          g_Initialized = FALSE;

/* ── Internal helpers ────────────────────────────────────────── */

/*
 * AllocAndCopyUnicode
 * Copies a UNICODE_STRING into a new non-paged buffer.
 * Caller must free Dest->Buffer with ExFreePoolWithTag(..., SC_POOL_TAG_PATH).
 */
static NTSTATUS
AllocAndCopyUnicode(
    _Out_ PUNICODE_STRING Dest,
    _In_  PUNICODE_STRING Src
)
{
    PWCH buf = (PWCH)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        (SIZE_T)Src->Length + sizeof(WCHAR),
        SC_POOL_TAG_PATH);
    if (!buf) return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(buf, Src->Buffer, Src->Length);
    buf[Src->Length / sizeof(WCHAR)] = L'\0';

    Dest->Buffer        = buf;
    Dest->Length        = Src->Length;
    Dest->MaximumLength = (USHORT)(Src->Length + sizeof(WCHAR));
    return STATUS_SUCCESS;
}

/*
 * FreeNativePath
 * Frees a single entry's buffer.
 */
static VOID
FreeNativePath(_Inout_ PSC_NATIVE_PATH Path)
{
    if (Path->Active && Path->Prefix.Buffer) {
        ExFreePoolWithTag(Path->Prefix.Buffer, SC_POOL_TAG_PATH);
        RtlZeroMemory(&Path->Prefix, sizeof(UNICODE_STRING));
        Path->Active = FALSE;
    }
}

/*
 * AddNativePath
 * Adds a path string to g_Paths.
 * Must be called before g_Initialized = TRUE (no locking needed).
 */
static NTSTATUS
AddNativePath(_In_ PCWSTR PathString)
{
    if (g_PathCount >= SC_MAX_PATHS) return STATUS_TOO_MANY_NAMES;

    UNICODE_STRING str;
    RtlInitUnicodeString(&str, PathString);

    NTSTATUS status = AllocAndCopyUnicode(&g_Paths[g_PathCount].Prefix, &str);
    if (!NT_SUCCESS(status)) return status;

    g_Paths[g_PathCount].Active = TRUE;
    g_PathCount++;

    DbgPrint("ShieldCord: PathFilter - protecting: %wZ\n",
             &g_Paths[g_PathCount - 1].Prefix);

    return STATUS_SUCCESS;
}

/* ── Public API ──────────────────────────────────────────────── */

NTSTATUS
PathFilterInit(VOID)
{
    ExInitializeFastMutex(&g_PathLock);
    RtlZeroMemory(g_Paths, sizeof(g_Paths));
    g_PathCount = 0;

    /*
     * Register fallback path suffixes.
     * The service replaces these with fully-resolved NT device paths at startup.
     */
    for (ULONG i = 0; i < RTL_NUMBER_OF(sc_Win32Paths) && g_PathCount < SC_MAX_PATHS; i++) {
        AddNativePath(sc_Win32Paths[i]);
    }

    g_Initialized = TRUE;

    DbgPrint("ShieldCord: PathFilter initialized with %lu fallback prefix(es). "
             "Service will send accurate paths.\n", g_PathCount);

    return STATUS_SUCCESS;
}

VOID
PathFilterDestroy(VOID)
{
    if (!g_Initialized) return;

    ExAcquireFastMutex(&g_PathLock);
    for (ULONG i = 0; i < SC_MAX_PATHS; i++)
        FreeNativePath(&g_Paths[i]);
    g_PathCount = 0;
    ExReleaseFastMutex(&g_PathLock);

    g_Initialized = FALSE;
}

BOOLEAN
IsPathProtected(
    _In_ PUNICODE_STRING FilePath
)
{
    if (!g_Initialized || g_PathCount == 0) return FALSE;

    /*
     * Fast exit: if the path doesn't contain any protected marker it is
     * definitely not a protected token path. This eliminates >99% of opens
     * in the system without touching the lock.
     *
     * Markers:
     *   "AppData"    — Discord/browser token dirs under the user profile
     *   "Profiles"   — Firefox profile root
     *   "ShieldCord" — our decoy token folder under ProgramData
     *
     * Single bounded pass: each compare checks the remaining length first so
     * we never read past the end of the buffer.
     */
    BOOLEAN hasMarker = FALSE;
    ULONG charCount = FilePath->Length / sizeof(WCHAR);
    for (ULONG i = 0; i < charCount; i++) {
        ULONG rem = charCount - i;
        if (rem >= 7 && _wcsnicmp(FilePath->Buffer + i, L"AppData", 7) == 0) {
            hasMarker = TRUE;
            break;
        }
        if (rem >= 8 && _wcsnicmp(FilePath->Buffer + i, L"Profiles", 8) == 0) {
            hasMarker = TRUE;
            break;
        }
        if (rem >= 10 && _wcsnicmp(FilePath->Buffer + i, L"ShieldCord", 10) == 0) {
            hasMarker = TRUE;
            break;
        }
    }
    if (!hasMarker) return FALSE;

    /* Now check the stored path list */
    ExAcquireFastMutex(&g_PathLock);
    BOOLEAN found = FALSE;
    USHORT fileChars = FilePath->Length / sizeof(WCHAR);

    for (ULONG i = 0; i < SC_MAX_PATHS && !found; i++) {
        if (!g_Paths[i].Active) continue;

        /* Prefix match (for full NT device paths sent by service) */
        if (RtlPrefixUnicodeString(&g_Paths[i].Prefix, FilePath, TRUE)) {
            found = TRUE;
            break;
        }

        /* Substring match (for fallback relative paths like \AppData\Roaming\discord\...) */
        USHORT prefixChars = g_Paths[i].Prefix.Length / sizeof(WCHAR);
        if (fileChars >= prefixChars) {
            for (USHORT j = 0; j + prefixChars <= fileChars; j++) {
                if (_wcsnicmp(FilePath->Buffer + j, g_Paths[i].Prefix.Buffer, prefixChars) == 0) {
                    found = TRUE;
                    break;
                }
            }
        }
    }
    ExReleaseFastMutex(&g_PathLock);

    return found;
}

NTSTATUS
PathFilterUpdateFromService(
    _In_reads_(Count) PWCHAR* Paths,
    _In_ ULONG               Count
)
{
    if (Count > SC_MAX_PATHS) Count = SC_MAX_PATHS;

    /* Build new entries outside the lock */
    SC_NATIVE_PATH newPaths[SC_MAX_PATHS];
    RtlZeroMemory(newPaths, sizeof(newPaths));
    ULONG newCount = 0;

    for (ULONG i = 0; i < Count; i++) {
        if (!Paths[i] || Paths[i][0] == L'\0') continue;

        UNICODE_STRING src;
        RtlInitUnicodeString(&src, Paths[i]);

        NTSTATUS s = AllocAndCopyUnicode(&newPaths[newCount].Prefix, &src);
        if (NT_SUCCESS(s)) {
            newPaths[newCount].Active = TRUE;
            newCount++;
            DbgPrint("ShieldCord: PathFilter (service update) — protecting: %ws\n", Paths[i]);
        }
    }

    /* Atomically swap */
    ExAcquireFastMutex(&g_PathLock);
    for (ULONG i = 0; i < SC_MAX_PATHS; i++)
        FreeNativePath(&g_Paths[i]);
    RtlCopyMemory(g_Paths, newPaths, sizeof(newPaths));
    g_PathCount = newCount;
    ExReleaseFastMutex(&g_PathLock);

    DbgPrint("ShieldCord: PathFilter updated from service — %lu path(s) active.\n", newCount);
    return STATUS_SUCCESS;
}
