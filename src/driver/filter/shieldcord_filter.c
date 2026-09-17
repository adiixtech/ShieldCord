/*
 * shieldcord_filter.c — ShieldCord Kernel Minifilter Driver
 *
 * DriverEntry / DriverUnload and all Filter Manager registration.
 *
 * ══════════════════ TEARDOWN ORDER (BSOD PREVENTION) ══════════
 *
 *   If FltUnregisterFilter is called while the process-notify routine
 *   is still registered, the notify callback may fire AFTER the filter
 *   is unloaded, accessing freed memory → BSOD.
 *
 *   MANDATORY ORDER in ScFilterUnload:
 *     1. CommPortDestroy()                ← close comm port first
 *     2. PsSetCreateProcessNotifyRoutineEx(ScProcessNotifyCallback, TRUE) ← unregister notify
 *     3. PathFilterDestroy()              ← free path strings
 *     4. ProcessCacheDestroy()            ← free hash table
 *     5. FltUnregisterFilter(g_FilterHandle) ← MUST BE LAST
 *
 * ═════════════════════════════════════════════════════════════
 *
 * Minifilter altitude: 385200 (FSFilter Activity Monitor range).
 * This places us ABOVE AV filters (320000–329999) so we see the
 * open request before AV, which is correct for an access-control filter.
 */

#include <fltKernel.h>
#include "pre_create.h"
#include "process_cache.h"
#include "path_filter.h"
#include "comm_port.h"

/* ── Global filter handle ────────────────────────────────────── */
PFLT_FILTER g_FilterHandle = NULL;

/* ── Forward declarations ────────────────────────────────────── */
DRIVER_UNLOAD DriverUnload;     /* standard WDM unload — not used for filters */

static NTSTATUS FLTAPI ScFilterUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
);

static NTSTATUS FLTAPI ScInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS       FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS    Flags,
    _In_ DEVICE_TYPE                 VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE         VolumeFilesystemType
);

static NTSTATUS FLTAPI ScInstanceQueryTeardown(
    _In_ PCFLT_RELATED_OBJECTS              FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS  Flags
);

static VOID FLTAPI ScInstanceTeardownStart(
    _In_ PCFLT_RELATED_OBJECTS           FltObjects,
    _In_ FLT_INSTANCE_TEARDOWN_FLAGS     Flags
);

static VOID FLTAPI ScInstanceTeardownComplete(
    _In_ PCFLT_RELATED_OBJECTS           FltObjects,
    _In_ FLT_INSTANCE_TEARDOWN_FLAGS     Flags
);

static VOID
ScProcessNotifyCallback(
    _Inout_     PEPROCESS                Process,
    _In_        HANDLE                   ProcessId,
    _In_opt_    PPS_CREATE_NOTIFY_INFO   CreateInfo
);

/* ── FLT_REGISTRATION ────────────────────────────────────────── */

/*
 * We intercept only IRP_MJ_CREATE (file open).
 * No post-callback needed: the decision is final in the pre-callback.
 */
static const FLT_OPERATION_REGISTRATION g_Callbacks[] = {
    {
        IRP_MJ_CREATE,
        0,
        ScPreCreate,    /* pre-create: allow or deny */
        NULL            /* no post-create */
    },
    { IRP_MJ_OPERATION_END }
};

static const FLT_REGISTRATION g_FilterRegistration = {
    sizeof(FLT_REGISTRATION),       /* Size */
    FLT_REGISTRATION_VERSION,       /* Version */
    0,                              /* Flags */
    NULL,                           /* Context registrations */
    g_Callbacks,                    /* Operation callbacks */
    ScFilterUnload,                 /* FilterUnload */
    ScInstanceSetup,                /* InstanceSetup */
    ScInstanceQueryTeardown,        /* InstanceQueryTeardown */
    ScInstanceTeardownStart,        /* InstanceTeardownStart */
    ScInstanceTeardownComplete,     /* InstanceTeardownComplete */
    NULL,                           /* GenerateFileName */
    NULL,                           /* GenerateDestinationFileName */
    NULL                            /* NormalizeNameComponent */
};

/* ── Instance callbacks (required stubs) ─────────────────────── */

static NTSTATUS FLTAPI
ScInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS      FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS   Flags,
    _In_ DEVICE_TYPE                VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE        VolumeFilesystemType
)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(VolumeFilesystemType);

    /* Attach to all local disk volumes. Skip network and CD-ROM volumes. */
    if (VolumeDeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM ||
        VolumeDeviceType == FILE_DEVICE_CD_ROM ||
        VolumeDeviceType == FILE_DEVICE_CD_ROM_FILE_SYSTEM)
    {
        return STATUS_FLT_DO_NOT_ATTACH;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS FLTAPI
ScInstanceQueryTeardown(
    _In_ PCFLT_RELATED_OBJECTS              FltObjects,
    _In_ FLT_INSTANCE_QUERY_TEARDOWN_FLAGS  Flags
)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
    return STATUS_SUCCESS;  /* allow detach */
}

static VOID FLTAPI
ScInstanceTeardownStart(
    _In_ PCFLT_RELATED_OBJECTS       FltObjects,
    _In_ FLT_INSTANCE_TEARDOWN_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
}

static VOID FLTAPI
ScInstanceTeardownComplete(
    _In_ PCFLT_RELATED_OBJECTS       FltObjects,
    _In_ FLT_INSTANCE_TEARDOWN_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(Flags);
}

/* ── Process notify callback ─────────────────────────────────── */

static VOID
ScProcessNotifyCallback(
    _Inout_  PEPROCESS              Process,
    _In_     HANDLE                 ProcessId,
    _In_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
)
{
    UNREFERENCED_PARAMETER(Process);

    if (CreateInfo) {
        /* Process is starting — add with TrustUnknown (fail-open default) */
        ProcessCacheAdd(ProcessId, CreateInfo->ImageFileName);
    } else {
        /* Process is exiting — remove from cache */
        ProcessCacheRemove(ProcessId);
    }
}

/* ── Filter unload ───────────────────────────────────────────── */

static NTSTATUS FLTAPI
ScFilterUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(Flags);

    DbgPrint("ShieldCord: ScFilterUnload — beginning teardown.\n");

    /*
     * CRITICAL ORDER — see file header for rationale.
     *
     * Step 1: Close comm port (stops new service messages and alerts)
     */
    CommPortDestroy();

    /*
     * Step 2: Unregister the process-notify routine.
     * This must happen BEFORE FltUnregisterFilter. If a notify fires
     * after FltUnregisterFilter, it calls into freed filter memory → BSOD.
     */
    PsSetCreateProcessNotifyRoutineEx(ScProcessNotifyCallback, TRUE);

    /*
     * Step 3: Free path strings (no callbacks can call IsPathProtected
     * after the notify is unregistered and the comm port is closed).
     */
    PathFilterDestroy();

    /*
     * Step 4: Free process cache.
     */
    ProcessCacheDestroy();

    /*
     * Step 5: Unregister the filter — MUST be last.
     * This waits for all outstanding pre-create callbacks to finish
     * before returning.
     */
    FltUnregisterFilter(g_FilterHandle);
    g_FilterHandle = NULL;

    DbgPrint("ShieldCord: ScFilterUnload — teardown complete.\n");
    return STATUS_SUCCESS;
}

/* ── DriverEntry ─────────────────────────────────────────────── */

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS status;

    DbgPrint("ShieldCord: DriverEntry — loading ShieldCord Filter v1.0\n");
    DbgPrint("ShieldCord: Altitude 385200 (FSFilter Activity Monitor)\n");

    /* ── Step 1: Register the minifilter ─────────────────────── */
    status = FltRegisterFilter(DriverObject, &g_FilterRegistration, &g_FilterHandle);
    if (!NT_SUCCESS(status)) {
        DbgPrint("ShieldCord: FltRegisterFilter failed: 0x%08X\n", status);
        return status;
    }

    /* ── Step 2: Initialize the process cache ────────────────── */
    status = ProcessCacheInit();
    if (!NT_SUCCESS(status)) {
        DbgPrint("ShieldCord: ProcessCacheInit failed: 0x%08X\n", status);
        FltUnregisterFilter(g_FilterHandle);
        g_FilterHandle = NULL;
        return status;
    }

    /* ── Step 3: Initialize the path filter ─────────────────── */
    status = PathFilterInit();
    if (!NT_SUCCESS(status)) {
        DbgPrint("ShieldCord: PathFilterInit failed: 0x%08X\n", status);
        ProcessCacheDestroy();
        FltUnregisterFilter(g_FilterHandle);
        g_FilterHandle = NULL;
        return status;
    }

    /* ── Step 4: Register process-creation notify routine ───── */
    /*
     * PsSetCreateProcessNotifyRoutineEx requires the driver image to have
     * the INTEGRITYCHECK linker flag set. If this call fails with
     * STATUS_ACCESS_DENIED, the .sys was not built with /INTEGRITYCHECK.
     */
    status = PsSetCreateProcessNotifyRoutineEx(ScProcessNotifyCallback, FALSE);
    if (!NT_SUCCESS(status)) {
        DbgPrint("ShieldCord: PsSetCreateProcessNotifyRoutineEx failed: 0x%08X\n"
                 "            Did you link with /INTEGRITYCHECK?\n", status);
        PathFilterDestroy();
        ProcessCacheDestroy();
        FltUnregisterFilter(g_FilterHandle);
        g_FilterHandle = NULL;
        return status;
    }

    /* ── Step 5: Create the communication port ───────────────── */
    status = CommPortInit(g_FilterHandle);
    if (!NT_SUCCESS(status)) {
        DbgPrint("ShieldCord: CommPortInit failed: 0x%08X\n", status);
        PsSetCreateProcessNotifyRoutineEx(ScProcessNotifyCallback, TRUE);
        PathFilterDestroy();
        ProcessCacheDestroy();
        FltUnregisterFilter(g_FilterHandle);
        g_FilterHandle = NULL;
        return status;
    }

    /* ── Step 6: Start filtering I/O ─────────────────────────── */
    status = FltStartFiltering(g_FilterHandle);
    if (!NT_SUCCESS(status)) {
        DbgPrint("ShieldCord: FltStartFiltering failed: 0x%08X\n", status);
        CommPortDestroy();
        PsSetCreateProcessNotifyRoutineEx(ScProcessNotifyCallback, TRUE);
        PathFilterDestroy();
        ProcessCacheDestroy();
        FltUnregisterFilter(g_FilterHandle);
        g_FilterHandle = NULL;
        return status;
    }

    DbgPrint("ShieldCord: Driver loaded successfully. Protecting Discord & browser token paths.\n");
    DbgPrint("ShieldCord: Mode = FAIL-OPEN until service connects and sends trusted PID list.\n");
    return STATUS_SUCCESS;
}
