/*
 * driver_protocol.h — ShieldCord Kernel Filter IPC Protocol
 *
 * Shared between:
 *   - shieldcord_filter.sys  (kernel C code, include ntddk.h before this)
 *   - shieldcord_svc.exe     (user C++ code, include windows.h before this)
 *
 * RULES FOR THIS FILE:
 *   - No OS-specific types (no ULONG, HANDLE, etc.) — use C primitives only
 *   - No C++ features — pure C99
 *   - #pragma pack(push,8) ensures identical layout in both environments
 */

#pragma once

/* ── Port identity ─────────────────────────────────────────── */
#define SC_FILTER_PORT_NAME     L"\\ShieldCordFilterPort"
#define SC_FILTER_MAX_CONNS     1   /* only our one service may connect */

/* ── Service → Driver messages ──────────────────────────────── */
typedef enum _SC_MSG_TYPE {
    ScMsgAddTrustedPid      = 1,    /* trust a specific PID              */
    ScMsgRemoveTrustedPid   = 2,    /* untrust / remove a specific PID   */
    ScMsgSetProtectedPaths  = 3,    /* update the protected path list     */
    ScMsgSetEnforcementMode = 4,    /* globally enable / disable blocking */
    ScMsgSetFeatureFlags    = 5,    /* set SC_FEATURE_* bitmask          */
} SC_MSG_TYPE;

/* ── Driver → Service alerts ────────────────────────────────── */
typedef enum _SC_ALERT_TYPE {
    ScAlertFileBlocked = 1,         /* an untrusted process was blocked   */
} SC_ALERT_TYPE;

#pragma pack(push, 8)

/*
 * SC_TRUSTED_PID_MSG
 * Sent by the service to add or remove a PID from the trusted table.
 * The filter's comm port MessageNotify callback receives this.
 */
typedef struct _SC_TRUSTED_PID_MSG {
    SC_MSG_TYPE   Type;             /* ScMsgAddTrustedPid or ScMsgRemoveTrustedPid */
    unsigned long Pid;              /* the process ID                               */
    wchar_t       ImageName[64];    /* base image name, e.g. L"discord.exe"        */
} SC_TRUSTED_PID_MSG, *PSC_TRUSTED_PID_MSG;

/*
 * SC_ENFORCEMENT_MSG
 * Sent by the service to enable / disable enforcement globally.
 * When disabled the driver behaves as if every PID is trusted.
 */
typedef struct _SC_ENFORCEMENT_MSG {
    SC_MSG_TYPE   Type;             /* ScMsgSetEnforcementMode */
    unsigned long Enable;           /* 1 = enforce, 0 = pass-through */
} SC_ENFORCEMENT_MSG, *PSC_ENFORCEMENT_MSG;

/*
 * Driver feature bits (ScMsgSetFeatureFlags). The service turns individual
 * driver behaviors on/off at runtime; both default ON.
 */
#define SC_FEATURE_FILE_BLOCK   0x00000001u   /* deny untrusted opens of protected paths */
#define SC_FEATURE_ALERTS       0x00000002u   /* send block alerts to the service         */

/*
 * SC_FEATURE_FLAGS_MSG
 * Sent by the service to set the driver feature bitmask (SC_FEATURE_*).
 */
typedef struct _SC_FEATURE_FLAGS_MSG {
    SC_MSG_TYPE   Type;             /* ScMsgSetFeatureFlags */
    unsigned long Flags;            /* SC_FEATURE_* bitmask */
} SC_FEATURE_FLAGS_MSG, *PSC_FEATURE_FLAGS_MSG;

/*
 * SC_BLOCK_ALERT
 * Sent by the driver to the service when it blocks an untrusted open.
 * Delivered via FltSendMessage from a kernel work item (never in-line
 * from the pre-create callback — that would cause a deadlock).
 */
typedef struct _SC_BLOCK_ALERT {
    SC_ALERT_TYPE AlertType;           /* always ScAlertFileBlocked for now  */
    unsigned long BlockedPid;          /* PID of the process that was denied  */
    wchar_t       BlockedImageName[64];/* base image name of the blocked proc */
    wchar_t       BlockedPath[512];    /* NT-native path that was blocked      */
} SC_BLOCK_ALERT, *PSC_BLOCK_ALERT;

#pragma pack(pop)
