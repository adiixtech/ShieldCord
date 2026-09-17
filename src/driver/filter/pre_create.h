/*
 * pre_create.h — ShieldCord Kernel Filter: IRP_MJ_CREATE Pre-Callback
 *
 * ScPreCreate is the ONLY active filter callback. It fires for every
 * CreateFile() in the system. Performance is critical.
 *
 * Hot-path rules enforced in pre_create.c:
 *   - No heap allocations
 *   - No blocking (no waits, no user-mode calls, no FltSendMessage)
 *   - FltReleaseFileNameInformation called on every code path
 *   - Kernel-mode opens always pass through (prevents recursion / deadlock)
 */

#pragma once

#include <fltKernel.h>

/*
 * ScPreCreate
 * Registered as the IRP_MJ_CREATE pre-operation callback in FLT_REGISTRATION.
 *
 * Returns:
 *   FLT_PREOP_SUCCESS_NO_CALLBACK  — allow, no post-callback needed
 *   FLT_PREOP_COMPLETE             — denied (STATUS_ACCESS_DENIED set in IoStatus)
 */
FLT_PREOP_CALLBACK_STATUS
ScPreCreate(
    _Inout_  PFLT_CALLBACK_DATA    Data,
    _In_     PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
);
