/*
 * comm_port.c — ShieldCord Kernel Filter: Communication Port
 *
 * Implements the kernel-side of the driver ↔ service IPC using
 * Filter Manager's communication port API (FltCreateCommunicationPort).
 *
 * THREADING MODEL:
 *   - Only one client (our service) connects at a time (SC_FILTER_MAX_CONNS=1).
 *   - ConnectNotify and DisconnectNotify are called by Filter Manager
 *     on a system thread — never re-entrant for the same port.
 *   - Alert work items may fire concurrently from multiple pre-create callbacks.
 *   - g_ClientPort guarded by g_PortMutex (FAST_MUTEX).
 *
 * ALERT DELIVERY:
 *   FltSendMessage MUST NOT be called from the pre-create callback (deadlock).
 *   Instead, CommSendBlockAlert allocates a work context and uses a Filter
 *   Manager generic work item (FltQueueGenericWorkItem) so the send happens
 *   from a system worker thread.
 */

#include <fltKernel.h>
#include "comm_port.h"
#include "process_cache.h"
#include "path_filter.h"
#include "driver_protocol.h"

/* Pool tag: 'ScCo' */
#define SC_POOL_TAG_COMM    'oCcS'

/* ── Module globals ──────────────────────────────────────────── */
static PFLT_PORT    g_ServerPort  = NULL;
static PFLT_PORT    g_ClientPort  = NULL;
static PFLT_FILTER  g_Filter      = NULL;   /* saved for FltQueueGenericWorkItem / FltSendMessage */
static FAST_MUTEX   g_PortMutex;
static BOOLEAN      g_Initialized = FALSE;

/* ── Work-item context for async alert delivery ──────────────── */
typedef struct _SC_ALERT_WORK_ITEM {
    PFLT_GENERIC_WORKITEM  WorkItem;   /* FltMgr generic work item (rundown-safe) */
    SC_BLOCK_ALERT         Alert;
} SC_ALERT_WORK_ITEM, *PSC_ALERT_WORK_ITEM;

/* ── Forward declarations ────────────────────────────────────── */
static NTSTATUS FLTAPI ScConnectNotify(
    _In_  PFLT_PORT          ClientPort,
    _In_  PVOID              ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_  ULONG              SizeOfContext,
    _Flt_ConnectionCookie_Outptr_ PVOID* ConnectionCookie
);

static VOID FLTAPI ScDisconnectNotify(
    _In_opt_ PVOID           ConnectionCookie
);

static NTSTATUS FLTAPI ScMessageNotify(
    _In_opt_ PVOID           PortCookie,
    _In_reads_bytes_opt_(InputBufferLength)  PVOID InputBuffer,
    _In_     ULONG           InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_     ULONG           OutputBufferLength,
    _Out_    PULONG          ReturnOutputBufferLength
);

/*
 * Work-item routine prototype. Declared explicitly (rather than via the
 * FLT_GENERIC_WORKITEM_ROUTINE function-type typedef) so it compiles against
 * older WDKs that define only the PFLT_GENERIC_WORKITEM_ROUTINE pointer form.
 */
static VOID ScAlertWorkRoutine(
    _In_     PFLT_GENERIC_WORKITEM  WorkItem,
    _In_     PVOID                  FltObject,
    _In_opt_ PVOID                  Context
);

/* ── Connect / Disconnect callbacks ──────────────────────────── */

static NTSTATUS FLTAPI
ScConnectNotify(
    _In_  PFLT_PORT          ClientPort,
    _In_  PVOID              ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_  ULONG              SizeOfContext,
    _Flt_ConnectionCookie_Outptr_ PVOID* ConnectionCookie
)
{
    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);
    UNREFERENCED_PARAMETER(ConnectionCookie);

    ExAcquireFastMutex(&g_PortMutex);
    g_ClientPort = ClientPort;
    ExReleaseFastMutex(&g_PortMutex);

    DbgPrint("ShieldCord: CommPort — service connected.\n");
    return STATUS_SUCCESS;
}

static VOID FLTAPI
ScDisconnectNotify(
    _In_opt_ PVOID ConnectionCookie
)
{
    UNREFERENCED_PARAMETER(ConnectionCookie);

    ExAcquireFastMutex(&g_PortMutex);
    g_ClientPort = NULL;
    ExReleaseFastMutex(&g_PortMutex);

    /*
     * Service disconnected. Reset to fail-open: the driver will allow all
     * opens until the service reconnects and re-sends its trusted PID list.
     * This prevents Discord from being permanently locked out if the service
     * crashes or is manually stopped.
     */
    ProcessCacheSetEnforcement(FALSE);
    ProcessCacheSetFeatureFlags(SC_FEATURE_FILE_BLOCK | SC_FEATURE_ALERTS);
    DbgPrint("ShieldCord: CommPort — service disconnected. Reverting to fail-open.\n");
}

/* ── Message handler ─────────────────────────────────────────── */

static NTSTATUS FLTAPI
ScMessageNotify(
    _In_opt_ PVOID           PortCookie,
    _In_reads_bytes_opt_(InputBufferLength)  PVOID InputBuffer,
    _In_     ULONG           InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_     ULONG           OutputBufferLength,
    _Out_    PULONG          ReturnOutputBufferLength
)
{
    UNREFERENCED_PARAMETER(PortCookie);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    *ReturnOutputBufferLength = 0;

    if (!InputBuffer || InputBufferLength < sizeof(SC_MSG_TYPE))
        return STATUS_INVALID_PARAMETER;

    SC_MSG_TYPE msgType = *(SC_MSG_TYPE*)InputBuffer;

    switch (msgType) {

    case ScMsgAddTrustedPid: {
        if (InputBufferLength < sizeof(SC_TRUSTED_PID_MSG))
            return STATUS_INVALID_PARAMETER;
        PSC_TRUSTED_PID_MSG msg = (PSC_TRUSTED_PID_MSG)InputBuffer;
        ProcessCacheTrustPid((HANDLE)(ULONG_PTR)msg->Pid, TrustAllowed);
        DbgPrint("ShieldCord: CommPort — TrustedPid ADD pid=%lu img=%ws\n",
                 msg->Pid, msg->ImageName);
        break;
    }

    case ScMsgRemoveTrustedPid: {
        if (InputBufferLength < sizeof(SC_TRUSTED_PID_MSG))
            return STATUS_INVALID_PARAMETER;
        PSC_TRUSTED_PID_MSG msg = (PSC_TRUSTED_PID_MSG)InputBuffer;
        ProcessCacheRemove((HANDLE)(ULONG_PTR)msg->Pid);
        DbgPrint("ShieldCord: CommPort — TrustedPid REMOVE pid=%lu\n", msg->Pid);
        break;
    }

    case ScMsgSetEnforcementMode: {
        if (InputBufferLength < sizeof(SC_ENFORCEMENT_MSG))
            return STATUS_INVALID_PARAMETER;
        PSC_ENFORCEMENT_MSG msg = (PSC_ENFORCEMENT_MSG)InputBuffer;
        ProcessCacheSetEnforcement(msg->Enable != 0);
        DbgPrint("ShieldCord: CommPort — EnforcementMode = %s\n",
                 msg->Enable ? "ENFORCE (default-deny)" : "FAIL-OPEN");
        break;
    }

    case ScMsgSetFeatureFlags: {
        if (InputBufferLength < sizeof(SC_FEATURE_FLAGS_MSG))
            return STATUS_INVALID_PARAMETER;
        PSC_FEATURE_FLAGS_MSG msg = (PSC_FEATURE_FLAGS_MSG)InputBuffer;
        ProcessCacheSetFeatureFlags(msg->Flags);
        DbgPrint("ShieldCord: CommPort — FeatureFlags = 0x%08lX\n", msg->Flags);
        break;
    }

    case ScMsgSetProtectedPaths: {
        /*
         * The service sends resolved NT-native paths as a packed array of
         * null-terminated wide strings: path1\0path2\0...pathN\0\0
         * We parse and forward to PathFilterUpdateFromService.
         */
        if (InputBufferLength < sizeof(SC_MSG_TYPE) + sizeof(WCHAR) * 2)
            return STATUS_INVALID_PARAMETER;

        PWCHAR pathBuf = (PWCHAR)((PUCHAR)InputBuffer + sizeof(SC_MSG_TYPE));
        ULONG  remaining = (InputBufferLength - sizeof(SC_MSG_TYPE)) / sizeof(WCHAR);

        PWCHAR paths[SC_MAX_PATHS];
        ULONG  count = 0;
        ULONG  i = 0;

        while (i < remaining && count < SC_MAX_PATHS) {
            if (pathBuf[i] == L'\0') break;
            paths[count++] = &pathBuf[i];
            /* Advance past this string */
            while (i < remaining && pathBuf[i] != L'\0') i++;
            i++; /* skip the null terminator */
        }

        PathFilterUpdateFromService(paths, count);
        break;
    }

    default:
        DbgPrint("ShieldCord: CommPort — unknown message type %d\n", (int)msgType);
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

/* ── Work item: send alert from worker thread ────────────────── */

static VOID
ScAlertWorkRoutine(
    _In_ PFLT_GENERIC_WORKITEM  WorkItem,
    _In_ PVOID                  FltObject,
    _In_opt_ PVOID              Context
)
{
    UNREFERENCED_PARAMETER(FltObject);

    PSC_ALERT_WORK_ITEM ctx = (PSC_ALERT_WORK_ITEM)Context;
    if (!ctx) {
        /* Should never happen, but never leak the work item if it does. */
        if (WorkItem) FltFreeGenericWorkItem(WorkItem);
        return;
    }

    ExAcquireFastMutex(&g_PortMutex);
    PFLT_PORT clientPort = g_ClientPort;
    ExReleaseFastMutex(&g_PortMutex);

    if (clientPort) {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -1 * 10 * 1000 * 50; /* 50 ms timeout */

        ULONG replyLen = 0;
        FltSendMessage(
            g_Filter,
            &clientPort,
            &ctx->Alert,
            sizeof(SC_BLOCK_ALERT),
            NULL,
            &replyLen,
            &timeout);
        /* Ignore send errors — the service may have disconnected by now */
    }

    FltFreeGenericWorkItem(ctx->WorkItem);
    ExFreePoolWithTag(ctx, SC_POOL_TAG_COMM);
}

/* ── Public API ──────────────────────────────────────────────── */

NTSTATUS
CommPortInit(
    _In_ PFLT_FILTER FilterHandle
)
{
    NTSTATUS        status;
    OBJECT_ATTRIBUTES oa = { 0 };
    UNICODE_STRING  portName;
    PSECURITY_DESCRIPTOR sd = NULL;

    g_Filter = FilterHandle;
    ExInitializeFastMutex(&g_PortMutex);

    /* Create a security descriptor that allows only SYSTEM to connect */
    status = FltBuildDefaultSecurityDescriptor(&sd, FLT_PORT_ALL_ACCESS);
    if (!NT_SUCCESS(status)) {
        DbgPrint("ShieldCord: CommPort — FltBuildDefaultSecurityDescriptor failed: 0x%08X\n", status);
        return status;
    }

    RtlInitUnicodeString(&portName, SC_FILTER_PORT_NAME);
    InitializeObjectAttributes(&oa, &portName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, sd);

    status = FltCreateCommunicationPort(
        FilterHandle,
        &g_ServerPort,
        &oa,
        NULL,               /* server port cookie */
        ScConnectNotify,
        ScDisconnectNotify,
        ScMessageNotify,
        SC_FILTER_MAX_CONNS
    );

    FltFreeSecurityDescriptor(sd);

    if (!NT_SUCCESS(status)) {
        DbgPrint("ShieldCord: CommPort — FltCreateCommunicationPort failed: 0x%08X\n", status);
        return status;
    }

    g_Initialized = TRUE;
    DbgPrint("ShieldCord: CommPort — listening on %wZ\n", &portName);
    return STATUS_SUCCESS;
}

VOID
CommPortDestroy(VOID)
{
    if (!g_Initialized) return;

    if (g_ServerPort) {
        FltCloseCommunicationPort(g_ServerPort);
        g_ServerPort = NULL;
    }

    ExAcquireFastMutex(&g_PortMutex);
    g_ClientPort = NULL;
    ExReleaseFastMutex(&g_PortMutex);

    g_Initialized = FALSE;
}

BOOLEAN
CommIsClientConnected(VOID)
{
    ExAcquireFastMutex(&g_PortMutex);
    BOOLEAN connected = (g_ClientPort != NULL);
    ExReleaseFastMutex(&g_PortMutex);
    return connected;
}

VOID
CommSendBlockAlert(
    _In_ PFLT_FILTER     FilterHandle,
    _In_ PSC_BLOCK_ALERT Alert
)
{
    UNREFERENCED_PARAMETER(FilterHandle); /* we use the module-global g_Filter */

    /* Don't bother allocating if nobody is listening */
    ExAcquireFastMutex(&g_PortMutex);
    BOOLEAN connected = (g_ClientPort != NULL);
    ExReleaseFastMutex(&g_PortMutex);
    if (!connected) return;

    PSC_ALERT_WORK_ITEM ctx = (PSC_ALERT_WORK_ITEM)ExAllocatePoolWithTag(
        NonPagedPoolNx, sizeof(SC_ALERT_WORK_ITEM), SC_POOL_TAG_COMM);
    if (!ctx) return;

    ctx->Alert = *Alert; /* copy the alert before the pre-create stack unwinds */

    /*
     * Use a Filter Manager generic work item rather than IoAllocateWorkItem.
     * FltAllocateGenericWorkItem takes no device object (avoiding the previous
     * FltGetDiskDeviceObject / IoGetCurrentIrpStackLocation(NULL) NULL-deref)
     * and holds a rundown reference on the filter, so FltUnregisterFilter waits
     * for any in-flight alert to finish before the driver unloads.
     */
    ctx->WorkItem = FltAllocateGenericWorkItem();
    if (!ctx->WorkItem) {
        ExFreePoolWithTag(ctx, SC_POOL_TAG_COMM);
        return;
    }

    NTSTATUS status = FltQueueGenericWorkItem(
        ctx->WorkItem,
        g_Filter,
        ScAlertWorkRoutine,
        DelayedWorkQueue,
        ctx);

    if (!NT_SUCCESS(status)) {
        FltFreeGenericWorkItem(ctx->WorkItem);
        ExFreePoolWithTag(ctx, SC_POOL_TAG_COMM);
    }
}
