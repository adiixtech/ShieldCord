/*
 * comm_port.h — ShieldCord Kernel Filter: Communication Port
 *
 * Wraps FltCreateCommunicationPort. The port is named \ShieldCordFilterPort
 * and is restricted to NT AUTHORITY\SYSTEM connections only, matching the
 * security context of shieldcord_svc.exe (a Windows Service).
 *
 * THREAD SAFETY:
 *   g_ClientPort is accessed by:
 *     - ConnectNotify / DisconnectNotify (serialised by Filter Manager)
 *     - CommSendBlockAlert work items (may run concurrently)
 *   Protected by a fast mutex (g_ClientPortMutex).
 */

#pragma once

#include <fltKernel.h>
#include "driver_protocol.h"

/*
 * CommPortInit
 * Creates the server-side communication port.
 * Called from DriverEntry after FltRegisterFilter succeeds.
 *
 * FilterHandle: the PFLT_FILTER returned by FltRegisterFilter.
 * Returns STATUS_SUCCESS or an NTSTATUS error.
 */
NTSTATUS CommPortInit(
    _In_ PFLT_FILTER         FilterHandle
);

/*
 * CommPortDestroy
 * Closes the server-side communication port and waits for any in-flight
 * FltSendMessage work items to complete.
 * Called from ScFilterUnload as the FIRST teardown step.
 */
VOID CommPortDestroy(VOID);

/*
 * CommIsClientConnected
 * Returns TRUE if the service currently has an active connection.
 * Used by pre_create.c to decide whether to enqueue block alerts.
 */
BOOLEAN CommIsClientConnected(VOID);

/*
 * CommSendBlockAlert
 * Enqueues a work item that calls FltSendMessage to deliver a SC_BLOCK_ALERT
 * to the service. Non-blocking; safe to call from a pre-create callback.
 *
 * FilterHandle: the global PFLT_FILTER (needed for IoAllocateWorkItem).
 * Alert: the alert to deliver (copied before this function returns).
 */
VOID CommSendBlockAlert(
    _In_ PFLT_FILTER         FilterHandle,
    _In_ PSC_BLOCK_ALERT     Alert
);
