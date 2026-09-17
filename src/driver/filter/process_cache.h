/*
 * process_cache.h — ShieldCord Kernel Filter: Process Trust Cache
 *
 * Maintains a kernel-resident hash table: PID → SC_TRUST_STATE.
 * All public functions are thread-safe (ERESOURCE-protected).
 *
 * IRQL CONTRACT:
 *   All functions run at IRQL <= APC_LEVEL.
 *   Do NOT call these at DISPATCH_LEVEL (spinlock held).
 *   This is safe: FltGetFileNameInformation (called just before these)
 *   already requires IRQL <= APC_LEVEL.
 */

#pragma once

#include <fltKernel.h>

/* ── Trust levels ───────────────────────────────────────────── */
typedef enum _SC_TRUST_STATE {
    TrustUnknown = 0,   /* process has started, service not yet verified it */
    TrustAllowed = 1,   /* service confirmed: signed by trusted publisher    */
    TrustDenied  = 2,   /* service confirmed: untrusted                      */
} SC_TRUST_STATE;

/*
 * ProcessCacheInit
 * Must be called from DriverEntry before any other cache function.
 * Returns STATUS_SUCCESS or an error status.
 */
NTSTATUS ProcessCacheInit(VOID);

/*
 * ProcessCacheDestroy
 * Frees all cache entries and the ERESOURCE.
 * Must be called from DriverUnload BEFORE FltUnregisterFilter.
 */
VOID ProcessCacheDestroy(VOID);

/*
 * ProcessCacheAdd
 * Called by ScProcessNotifyCallback when a new process starts.
 * Adds the PID with TrustUnknown state.
 * If the PID already exists (re-use), updates its image name.
 */
VOID ProcessCacheAdd(
    _In_     HANDLE              ProcessId,
    _In_opt_ PCUNICODE_STRING    ImageFileName   /* may be NULL for system processes */
);

/*
 * ProcessCacheTrustPid
 * Called by the comm port message handler when the service sends
 * ScMsgAddTrustedPid. Promotes a PID to TrustAllowed.
 * If the PID is not found, adds it as TrustAllowed directly
 * (handles the case where the message arrived before the notify callback).
 */
VOID ProcessCacheTrustPid(
    _In_ HANDLE              ProcessId,
    _In_ SC_TRUST_STATE      NewState
);

/*
 * ProcessCacheRemove
 * Called when a process exits or the service sends ScMsgRemoveTrustedPid.
 * Safe to call for non-existent PIDs.
 */
VOID ProcessCacheRemove(
    _In_ HANDLE              ProcessId
);

/*
 * ProcessCacheSetEnforcement
 * Arms (Enable=TRUE) or disarms (Enable=FALSE) global enforcement.
 *   FALSE -> fail-open: every PID is allowed (boot / service-down safety).
 *   TRUE  -> default-deny: only TrustAllowed PIDs pass IsPidTrusted.
 * Written via interlocked exchange; read lock-free on the hot path.
 */
VOID ProcessCacheSetEnforcement(
    _In_ BOOLEAN              Enable
);

/*
 * ProcessCacheSetFeatureFlags
 * Sets the SC_FEATURE_* bitmask (see driver_protocol.h). Feature bits let
 * the service turn individual driver behaviors on/off at runtime.
 */
VOID ProcessCacheSetFeatureFlags(
    _In_ ULONG                Flags
);

/*
 * ProcessCacheFileBlockEnabled
 * TRUE if SC_FEATURE_FILE_BLOCK is set. Hot path — called only after a
 * path has already been identified as protected.
 */
BOOLEAN ProcessCacheFileBlockEnabled(VOID);

/*
 * ProcessCacheAlertsEnabled
 * TRUE if SC_FEATURE_ALERTS is set. If FALSE, block alerts are suppressed.
 */
BOOLEAN ProcessCacheAlertsEnabled(VOID);

/*
 * ProcessCacheGetImageName
 * Best-effort copy of a PID's cached base image name (max 63 chars + NUL).
 * Returns FALSE if the PID is not in the cache. Used to fill block-alert
 * image names so the service can signature-check the blocked process.
 */
BOOLEAN ProcessCacheGetImageName(
    _In_  HANDLE              ProcessId,
    _Out_writes_(64) PWCHAR   Dest
);

/*
 * IsPidTrusted
 * The hot path — called for every protected-path file open.
 * Returns TRUE if the PID should be allowed access.
 *
 * Policy (with enforcement armed):
 *   TrustAllowed  → TRUE  (service confirmed, allow)
 *   TrustUnknown  → FALSE (not yet verified — denied when armed)
 *   TrustDenied   → FALSE (service confirmed, block)
 *
 * When enforcement is NOT armed the driver is fail-open: every PID is
 * allowed so a boot or a service crash can never lock a real app out.
 */
BOOLEAN IsPidTrusted(
    _In_ HANDLE              ProcessId
);
