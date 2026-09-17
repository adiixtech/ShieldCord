/*
 * pre_create.c — ShieldCord Kernel Filter: IRP_MJ_CREATE Pre-Callback
 *
 * This is the hottest code path in the entire driver.
 * It fires for EVERY CreateFile() call in the system.
 *
 * ═══════════════════ PERFORMANCE RULES ═══════════════════════
 *   1. No heap allocations on ANY code path.
 *   2. No blocking (no waits, no FltSendMessage, no user-mode calls).
 *   3. Exit FAST for non-protected paths (95%+ of all file opens).
 *   4. FltReleaseFileNameInformation called on EVERY exit path.
 *   5. Kernel-mode opens always pass through (prevents deadlock/recursion).
 * ═════════════════════════════════════════════════════════════
 *
 * DECISION TREE:
 *   KernelMode?         → allow (skip our checks, prevents recursion)
 *   GetFileNameInfo fails? → allow (fail-open, can't check)
 *   IsPathProtected?    → no → allow (fast exit for 95%+ of opens)
 *   IsPidTrusted?       → yes → allow
 *                        no  → DENY (STATUS_ACCESS_DENIED) + queue alert
 */

#include <fltKernel.h>
#include "pre_create.h"
#include "process_cache.h"
#include "path_filter.h"
#include "comm_port.h"
#include "driver_protocol.h"

/* Create dispositions are defined in ntddk.h; keep this compilable if only
 * wdm/fltKernel is in scope. FILE_CREATE (2) = create-new; fails if the file
 * already exists, so it can never open (and never let us read) an existing
 * token file. */
#ifndef FILE_CREATE
#define FILE_CREATE 0x00000002u
#endif

/* Forward declaration for the global filter handle (defined in shieldcord_filter.c) */
extern PFLT_FILTER g_FilterHandle;

FLT_PREOP_CALLBACK_STATUS
ScPreCreate(
    _Inout_  PFLT_CALLBACK_DATA    Data,
    _In_     PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    /* ── Step 1: Skip kernel-mode opens ─────────────────────────
     *
     * Kernel-mode opens are made by:
     *   - NTFS itself (internal I/O)
     *   - Our own driver (if we ever call FltCreateFile)
     *   - Other filter drivers
     *
     * Blocking these causes deadlocks. Always pass through.
     */
    if (Data->RequestorMode == KernelMode)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    /* ── Step 1b: Skip pure new-file creation ───────────────────
     *
     * A FILE_CREATE cannot open an existing file (it fails if the target
     * exists), so it can never reach a pre-existing token file and nothing
     * can be stolen. Skipping the filename query here keeps churny
     * "create brand-new file" workloads (temp/log/unique-name generation)
     * from paying for a full path lookup. Existing-file opens — the only
     * ones that matter for token theft — are unaffected.
     */
    if (((Data->Iopb->Parameters.Create.Options >> 24) & 0x000000FFu) == FILE_CREATE)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;

    /* ── Step 1c: Only an open that can READ is a threat ────────
     *
     * Token theft requires reading file DATA, and a handle opened without
     * FILE_READ_DATA cannot be used to read the file at all — access is fixed
     * at open time. Denying those adds nothing to the threat model while
     * breaking the app that owns the file: a browser creating, rewriting or
     * migrating its own store opens it for WRITE, and used to be refused here.
     * That is exactly what left Brave's network-data migration half-done and
     * its cookies unloaded.
     *
     * MAXIMUM_ALLOWED is treated as a read request — it asks for everything the
     * object permits, reading included. FILE_READ_EA is deliberately NOT
     * included: extended attributes are not token data.
     *
     * Pure arithmetic, no allocation, no I/O — the callback's performance rules
     * are unaffected, and it also skips the filename query below for every
     * write-only open.
     */
    {
        ACCESS_MASK desired =
            Data->Iopb->Parameters.Create.SecurityContext
                ? Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess
                : 0;

        if ((desired & (FILE_READ_DATA | FILE_EXECUTE |
                        GENERIC_READ | MAXIMUM_ALLOWED)) == 0)
        {
            return FLT_PREOP_SUCCESS_NO_CALLBACK;   /* cannot read it — allow */
        }
    }

    /* ── Step 2: Get the target file path ───────────────────────
     *
     * FLT_FILE_NAME_NORMALIZED  — resolve any volume links/junctions
     * FLT_FILE_NAME_QUERY_DEFAULT — standard query mode
     *
     * May fail (e.g. paging file, unnamed pipes). Fail-open.
     */
    PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
    NTSTATUS status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo);

    if (!NT_SUCCESS(status) || !nameInfo)
        return FLT_PREOP_SUCCESS_NO_CALLBACK;    /* fail-open, can't inspect */

    /* ── Step 3: Parse the file name ─────────────────────────── */
    status = FltParseFileNameInformation(nameInfo);
    if (!NT_SUCCESS(status)) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;    /* fail-open */
    }

    /* ── Step 4: Protected path check ───────────────────────────
     *
     * IsPathProtected does a two-stage check:
     *   a) Cheap substring search for "AppData"/"Profiles"/"ShieldCord" (no lock)
     *   b) If passes, full prefix/substring match against the stored list
     *
     * For the vast majority of opens (system files, exe loads, etc.)
     * this returns FALSE after the substring check — extremely fast.
     */
    if (!IsPathProtected(&nameInfo->Name)) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;    /* not our concern, fast exit */
    }

    /* ── Step 5: Feature gate — file blocking enabled? ──────────
     *
     * The service can turn the block behavior off at runtime (e.g. for a
     * maintenance window). When off, protected paths are allowed and no
     * further work is done — alerts are suppressed along with blocks.
     */
    if (!ProcessCacheFileBlockEnabled()) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;    /* blocking toggled off by the app */
    }

    /* ── Step 6: Get requesting process ID ──────────────────────
     *
     * PsGetCurrentProcessId() returns the PID of the process that called
     * CreateFile(). In pre-create context this is always the true caller.
     */
    HANDLE pid = PsGetCurrentProcessId();

    /* ── Step 7: PID trust check (O(1) hash lookup) ─────────── */
    if (IsPidTrusted(pid)) {
        FltReleaseFileNameInformation(nameInfo);
        return FLT_PREOP_SUCCESS_NO_CALLBACK;    /* trusted, allow */
    }

    /* ── Step 8: Deny the open ───────────────────────────────── */

    /*
     * Capture info for the alert BEFORE releasing nameInfo.
     * We copy into a stack-allocated SC_BLOCK_ALERT; CommSendBlockAlert
     * will copy it again into a heap-allocated work-item context.
     * Total: zero dynamic allocations in this callback.
     */
    SC_BLOCK_ALERT alert;
    RtlZeroMemory(&alert, sizeof(alert));
    alert.AlertType   = ScAlertFileBlocked;
    alert.BlockedPid  = (unsigned long)(ULONG_PTR)pid;

    /* Copy at most 511 chars of the path */
    USHORT copyChars = nameInfo->Name.Length / sizeof(WCHAR);
    if (copyChars > 511) copyChars = 511;
    RtlCopyMemory(alert.BlockedPath, nameInfo->Name.Buffer, copyChars * sizeof(WCHAR));
    alert.BlockedPath[copyChars] = L'\0';

    /* CRITICAL: release BEFORE doing any other work */
    FltReleaseFileNameInformation(nameInfo);
    nameInfo = NULL;

    /* Base image name from the process cache — lets the service signature-check
     * the blocked process (self-heal) without us doing any I/O here. */
    ProcessCacheGetImageName(pid, alert.BlockedImageName);

    DbgPrint("ShieldCord: BLOCKED — pid=%lu img=%ws path=%ws\n",
             alert.BlockedPid, alert.BlockedImageName, alert.BlockedPath);

    /* Queue an async work item to send the alert (never block here).
     * Suppressed entirely when the app has turned SC_FEATURE_ALERTS off. */
    if (ProcessCacheAlertsEnabled())
        CommSendBlockAlert(g_FilterHandle, &alert);

    /* Deny the open */
    Data->IoStatus.Status      = STATUS_ACCESS_DENIED;
    Data->IoStatus.Information = 0;
    return FLT_PREOP_COMPLETE;
}
