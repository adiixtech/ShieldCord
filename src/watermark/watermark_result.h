/* ============================================================
 * ShieldCord - watermark_result.h
 *
 * THE SHARED-MEMORY CONTRACT between the watermark hook DLL and the
 * tray app that injects it.
 *
 * Mirrored by hand in src/ui/Services/WatermarkSuppressor.cs. If you
 * change ANYTHING here, change the marshalled struct there to match -
 * there is no code generator, and a layout drift would read as "the
 * hook did nothing" rather than as an error.
 *
 * RULES (same as driver_protocol.h, for the same reasons):
 *   - pure C99, no OS-specific types
 *   - every field is 4 bytes, so C and C# cannot disagree about
 *     alignment or padding on any architecture
 *   - #pragma pack(push,8) to pin the layout explicitly
 *
 * The section is named Local\ShieldCordWM_<pid> - one per explorer
 * instance - and is created by the INJECTOR before the DLL is loaded.
 * The DLL derives the same name from its own PID, so nothing has to be
 * passed across the process boundary.
 * ============================================================ */

#ifndef SHIELDCORD_WATERMARK_RESULT_H
#define SHIELDCORD_WATERMARK_RESULT_H

#pragma pack(push, 8)

/* Written into magic once the DLL has actually run. A zero here means
 * "the DLL was loaded but never initialised", which is a different
 * failure from "it ran and patched nothing" - worth being able to tell
 * apart when diagnosing a silent no-op. */
#define SCWM_MAGIC   0x4D435753u   /* 'SWCM' */

#define SCWM_STRUCT_VERSION 1u

/* ---- status ---------------------------------------------------- */
#define SCWM_OK                 0u  /* ran; see installedMask for what it did */
#define SCWM_ERR_NO_SECTION     1u  /* the injector did not create the section */
#define SCWM_ERR_NO_TARGET      2u  /* shell32/explorer not found in this process */
#define SCWM_ERR_NOTHING_PATCHED 3u /* probed, and found no path to hook */

/* ---- render paths ----------------------------------------------
 * One bit each. This is the answer to "which method suits this
 * Windows": the DLL probes the RUNNING process and reports which of
 * these it actually found, rather than consulting a build table. */
#define SCWM_LAYER_LOADSTRING   0x0001u  /* shell32  - primary, by resource ID */
#define SCWM_LAYER_EXTTEXTOUT   0x0002u  /* shell32 + explorer - fallback */
#define SCWM_LAYER_DRAWTEXT     0x0004u  /* shell32 + explorer - fallback */
#define SCWM_LAYER_GLOW         0x0008u  /* UxTheme ord 126, build 28000+ */

#define SCWM_LAYER_COUNT        4

typedef struct ScWmResult {
    /* ---- header ---- */
    unsigned int magic;          /* SCWM_MAGIC once the DLL has run */
    unsigned int structVersion;  /* SCWM_STRUCT_VERSION */
    unsigned int status;         /* SCWM_* status */

    /* ---- input: written by the injector BEFORE loading the DLL ----
     * Layers to skip, as SCWM_LAYER_* bits. 0 = install every layer that
     * is present. This exists so the VM probe can switch layers on and
     * off one at a time and find out which one actually removes the
     * watermark, instead of guessing from a static reading. */
    unsigned int inDisableMask;

    /* ---- output: written by the DLL ---- */
    unsigned int presentMask;    /* paths found in the target */
    unsigned int installedMask;  /* paths actually patched */
    unsigned int alreadyApplied; /* 1 when this was a re-injection */
    unsigned int reserved1;

    unsigned int shell32Base;    /* module bases, for diagnosis */
    unsigned int explorerBase;

    /* ---- live counters ------------------------------------------
     * Incremented by the hooks as they fire, and read back by the app
     * AFTER a desktop repaint. This is what tells us which layer did
     * the work: a non-zero hits[LOADSTRING] means the watermark text
     * really does come through LoadStringW on this build, which is the
     * assumption the whole design rests on. */
    unsigned int hits[SCWM_LAYER_COUNT];

    /* A matching resource ID arrived with an hInstance that was neither
     * shell32 nor explorer. We still suppressed it (matching on the ID
     * alone is what makes the feature robust if the hInstance
     * assumption is wrong), but a non-zero count here says that
     * assumption needs revisiting. */
    unsigned int idOnlyHits;

    unsigned int lastIdHit;      /* most recent matching resource ID */

    /* A matching resource ID was requested by code that is NOT inside
     * shell32. Patching shell32's own IAT slot means the caller should
     * always be shell32, so this reading zero is what confirms the caller
     * filter is sound. Non-zero is not a failure - it is a warning that
     * the filter, not the matching, is what should be tightened. */
    unsigned int callerNotShell32Hits;
} ScWmResult;

#pragma pack(pop)

#endif /* SHIELDCORD_WATERMARK_RESULT_H */
