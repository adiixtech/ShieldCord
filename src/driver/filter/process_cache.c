/*
 * process_cache.c — ShieldCord Kernel Filter: Process Trust Cache
 *
 * Implements a 256-bucket chained hash table keyed by PID.
 * Protected by a single ERESOURCE lock:
 *   - IsPidTrusted  acquires SHARED (many concurrent readers)
 *   - All writes     acquire EXCLUSIVE
 *
 * The lock is taken through the FltAcquireResourceExclusive/Shared and
 * FltReleaseResource wrappers, which enter/leave a critical region around the ERESOURCE so a
 * normal-mode APC cannot suspend a thread that holds it (Driver Verifier
 * flags a bare ExAcquireResourceLite that omits KeEnterCriticalRegion).
 *
 * All memory allocated from NonPagedPoolNx (safe in any callback context).
 */

#include <fltKernel.h>
#include "process_cache.h"
#include "driver_protocol.h"   /* SC_FEATURE_* bits */

/* ── Hash table configuration ───────────────────────────────── */
#define SC_CACHE_BUCKETS    256u
#define SC_CACHE_HASH(pid)  ((ULONG_PTR)(pid) & (SC_CACHE_BUCKETS - 1u))

/* Pool tag: 'ScPC' in little-endian = 0x43507353 */
#define SC_POOL_TAG_CACHE   'CPcS'

/* ── Hash table entry ────────────────────────────────────────── */
typedef struct _SC_CACHE_ENTRY {
    LIST_ENTRY         Link;
    HANDLE             ProcessId;
    SC_TRUST_STATE     TrustState;
    WCHAR              ImageName[64];   /* base name, best-effort, for logging */
} SC_CACHE_ENTRY, *PSC_CACHE_ENTRY;

/* ── Module globals ──────────────────────────────────────────── */
static LIST_ENTRY   g_Buckets[SC_CACHE_BUCKETS];
static ERESOURCE    g_Lock;
static BOOLEAN      g_Initialized = FALSE;

/*
 * Enforcement + feature flags.
 *
 * g_EnforcementEnabled is FALSE at load — the driver is fail-open until the
 * service connects and explicitly arms it (ScMsgSetEnforcementMode). The
 * comm port's disconnect handler resets it to FALSE, so a service crash can
 * never lock a real app out of its own data.
 *
 * g_FeatureFlags lets the service turn individual behaviors on/off at
 * runtime (SC_FEATURE_FILE_BLOCK, SC_FEATURE_ALERTS). Default: everything on.
 */
static volatile BOOLEAN g_EnforcementEnabled = FALSE;
static volatile ULONG   g_FeatureFlags       = SC_FEATURE_FILE_BLOCK | SC_FEATURE_ALERTS;

/* ── Internal helpers ────────────────────────────────────────── */

/* Caller must hold g_Lock (shared or exclusive). */
static PSC_CACHE_ENTRY
CacheLookup(
    _In_ HANDLE ProcessId
)
{
    ULONG bucket = (ULONG)SC_CACHE_HASH(ProcessId);
    PLIST_ENTRY entry = g_Buckets[bucket].Flink;

    while (entry != &g_Buckets[bucket]) {
        PSC_CACHE_ENTRY e = CONTAINING_RECORD(entry, SC_CACHE_ENTRY, Link);
        if (e->ProcessId == ProcessId)
            return e;
        entry = entry->Flink;
    }
    return NULL;
}

static VOID
CopySafeImageName(
    _Out_writes_(64) PWCHAR            Dest,
    _In_opt_         PCUNICODE_STRING  Src
)
{
    Dest[0] = L'\0';
    if (!Src || Src->Length == 0) return;

    /* Find the last backslash and copy only the base name */
    USHORT i;
    USHORT lastSlash = 0;
    USHORT charCount = Src->Length / sizeof(WCHAR);
    const WCHAR* buf = Src->Buffer;

    for (i = 0; i < charCount; i++) {
        if (buf[i] == L'\\') lastSlash = (USHORT)(i + 1);
    }

    USHORT copyCount = charCount - lastSlash;
    if (copyCount >= 64) copyCount = 63;

    RtlCopyMemory(Dest, buf + lastSlash, copyCount * sizeof(WCHAR));
    Dest[copyCount] = L'\0';
}

/* ── Public API ──────────────────────────────────────────────── */

NTSTATUS
ProcessCacheInit(VOID)
{
    NTSTATUS status;

    for (ULONG i = 0; i < SC_CACHE_BUCKETS; i++)
        InitializeListHead(&g_Buckets[i]);

    status = ExInitializeResourceLite(&g_Lock);
    if (!NT_SUCCESS(status)) return status;

    g_Initialized = TRUE;
    return STATUS_SUCCESS;
}

VOID
ProcessCacheDestroy(VOID)
{
    if (!g_Initialized) return;

    /* Drain all buckets */
    FltAcquireResourceExclusive(&g_Lock);
    for (ULONG i = 0; i < SC_CACHE_BUCKETS; i++) {
        while (!IsListEmpty(&g_Buckets[i])) {
            PLIST_ENTRY le = RemoveHeadList(&g_Buckets[i]);
            PSC_CACHE_ENTRY e = CONTAINING_RECORD(le, SC_CACHE_ENTRY, Link);
            ExFreePoolWithTag(e, SC_POOL_TAG_CACHE);
        }
    }
    FltReleaseResource(&g_Lock);
    ExDeleteResourceLite(&g_Lock);
    g_Initialized = FALSE;
}

VOID
ProcessCacheAdd(
    _In_     HANDLE              ProcessId,
    _In_opt_ PCUNICODE_STRING    ImageFileName
)
{
    if (!g_Initialized) return;

    FltAcquireResourceExclusive(&g_Lock);

    PSC_CACHE_ENTRY existing = CacheLookup(ProcessId);
    if (existing) {
        /* PID reuse: reset state and refresh image name */
        existing->TrustState = TrustUnknown;
        CopySafeImageName(existing->ImageName, ImageFileName);
        FltReleaseResource(&g_Lock);
        return;
    }

    FltReleaseResource(&g_Lock);

    /* Allocate outside the lock to minimise hold time */
    PSC_CACHE_ENTRY entry = (PSC_CACHE_ENTRY)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(SC_CACHE_ENTRY), SC_POOL_TAG_CACHE);
    if (!entry) return;

    RtlZeroMemory(entry, sizeof(*entry));
    entry->ProcessId  = ProcessId;
    entry->TrustState = TrustUnknown;
    CopySafeImageName(entry->ImageName, ImageFileName);

    ULONG bucket = (ULONG)SC_CACHE_HASH(ProcessId);

    FltAcquireResourceExclusive(&g_Lock);
    /* Re-check for a race: another thread may have inserted while we allocated */
    if (!CacheLookup(ProcessId)) {
        InsertHeadList(&g_Buckets[bucket], &entry->Link);
        entry = NULL; /* owned by the list now */
    }
    FltReleaseResource(&g_Lock);

    /* Free if we lost the race */
    if (entry)
        ExFreePoolWithTag(entry, SC_POOL_TAG_CACHE);
}

VOID
ProcessCacheTrustPid(
    _In_ HANDLE        ProcessId,
    _In_ SC_TRUST_STATE NewState
)
{
    if (!g_Initialized) return;

    FltAcquireResourceExclusive(&g_Lock);
    PSC_CACHE_ENTRY e = CacheLookup(ProcessId);
    if (e) {
        e->TrustState = NewState;
        FltReleaseResource(&g_Lock);
        return;
    }
    FltReleaseResource(&g_Lock);

    /*
     * PID not in cache yet — the service message arrived before the process-
     * notify callback fired (possible on a loaded system). Insert directly
     * with the desired trust state so the pre-create callback sees it
     * immediately.
     */
    PSC_CACHE_ENTRY entry = (PSC_CACHE_ENTRY)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(SC_CACHE_ENTRY), SC_POOL_TAG_CACHE);
    if (!entry) return;

    RtlZeroMemory(entry, sizeof(*entry));
    entry->ProcessId  = ProcessId;
    entry->TrustState = NewState;

    ULONG bucket = (ULONG)SC_CACHE_HASH(ProcessId);
    FltAcquireResourceExclusive(&g_Lock);
    if (!CacheLookup(ProcessId))
        InsertHeadList(&g_Buckets[bucket], &entry->Link);
    else {
        ExFreePoolWithTag(entry, SC_POOL_TAG_CACHE);
        entry = NULL;
        /* update the one that won the race */
        PSC_CACHE_ENTRY winner = CacheLookup(ProcessId);
        if (winner) winner->TrustState = NewState;
    }
    FltReleaseResource(&g_Lock);
}

VOID
ProcessCacheRemove(
    _In_ HANDLE ProcessId
)
{
    if (!g_Initialized) return;

    FltAcquireResourceExclusive(&g_Lock);
    PSC_CACHE_ENTRY e = CacheLookup(ProcessId);
    if (e) {
        RemoveEntryList(&e->Link);
        FltReleaseResource(&g_Lock);
        ExFreePoolWithTag(e, SC_POOL_TAG_CACHE);
        return;
    }
    FltReleaseResource(&g_Lock);
}

/* ── Enforcement + feature flags ─────────────────────────────── */

VOID
ProcessCacheSetEnforcement(
    _In_ BOOLEAN Enable
)
{
    /* Single byte, single field — a lock-free interlocked exchange is enough. */
    InterlockedExchange8((volatile CHAR*)&g_EnforcementEnabled,
                         Enable ? (CHAR)TRUE : (CHAR)FALSE);
}

VOID
ProcessCacheSetFeatureFlags(
    _In_ ULONG Flags
)
{
    InterlockedExchange((volatile LONG*)&g_FeatureFlags, (LONG)Flags);
}

BOOLEAN
ProcessCacheFileBlockEnabled(VOID)
{
    return (g_FeatureFlags & SC_FEATURE_FILE_BLOCK) ? TRUE : FALSE;
}

BOOLEAN
ProcessCacheAlertsEnabled(VOID)
{
    return (g_FeatureFlags & SC_FEATURE_ALERTS) ? TRUE : FALSE;
}

BOOLEAN
ProcessCacheGetImageName(
    _In_  HANDLE ProcessId,
    _Out_writes_(64) PWCHAR Dest
)
{
    if (!g_Initialized || !Dest) return FALSE;

    Dest[0] = L'\0';

    FltAcquireResourceShared(&g_Lock);
    PSC_CACHE_ENTRY e = CacheLookup(ProcessId);
    if (e) {
        /* Copy the stored base name (zero-padded, already NUL-terminated). */
        RtlCopyMemory(Dest, e->ImageName, sizeof(e->ImageName) - sizeof(WCHAR));
        Dest[63] = L'\0';
        FltReleaseResource(&g_Lock);
        return TRUE;
    }
    FltReleaseResource(&g_Lock);
    return FALSE;
}

BOOLEAN
IsPidTrusted(
    _In_ HANDLE ProcessId
)
{
    if (!g_Initialized) return TRUE; /* fail-open if cache not ready */

    /*
     * Fail-open until the service connects and arms enforcement. This is the
     * boot / service-down safety valve: while the flag is FALSE, every open
     * passes and no legitimate app can ever be locked out.
     */
    if (!g_EnforcementEnabled)
        return TRUE;

    FltAcquireResourceShared(&g_Lock);
    PSC_CACHE_ENTRY e = CacheLookup(ProcessId);
    SC_TRUST_STATE state = e ? e->TrustState : TrustUnknown;
    FltReleaseResource(&g_Lock);

    /*
     * Default-deny once armed: only explicitly TrustAllowed PIDs pass.
     * TrustUnknown (service not yet verified) and TrustDenied are blocked —
     * this is the flip that makes the deny path in pre_create actually fire.
     */
    return (state == TrustAllowed);
}
