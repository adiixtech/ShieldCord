/* ============================================================
 * ShieldCord - watermark_hook.cpp
 *
 * Removes the Windows "Test Mode" desktop watermark.
 *
 * THE WATERMARK IS A USER-MODE SHELL THING. It is painted by
 * shell32.dll inside explorer.exe (CDesktopWatermark::s_DesktopBuildPaint).
 * Nothing in the kernel draws it, and patching win32k from the driver
 * would be a PatchGuard bugcheck - so this feature is entirely user mode
 * and the kernel driver is deliberately not involved.
 *
 * This DLL is loaded into explorer.exe by WatermarkSuppressor.cs. On
 * load it probes what the RUNNING process actually uses and patches only
 * those paths, so there is no Windows-build table to keep up to date:
 *
 *   LoadStringW  shell32   PRIMARY. The watermark text is a resource
 *                          string in shell32.dll.mui - 33088 is
 *                          "Test Mode", 33090-33093 are the build
 *                          strings, 33108 is "%ws Build %ws". Matching
 *                          on the RESOURCE ID is what makes this work
 *                          on a localised Windows: the text is
 *                          translated, the ID is not.
 *
 *   ExtTextOutW  shell32   Fallbacks. Matched against the same strings
 *   DrawTextW    explorer  read out of the resources at load, so they
 *                (+Ex)     need no locale knowledge either. These exist
 *                          in case a build paints the watermark without
 *                          going through LoadStringW at paint time.
 *
 *   UxTheme #126 shell32   DETECTED AND REPORTED, NOT HOOKED. From build
 *                28000+  the final draw is routed through this private
 *                          ordinal (DrawTextWithGlow). Its signature is
 *                          not public, so the only hook available would
 *                          suppress EVERY glow-drawn string in the
 *                          shell - too blunt to install unverified.
 *                          It is reported so the app can say "not
 *                          supported on this build" rather than fail
 *                          silently.
 *
 * WHY IAT PATCHING AND NOT AN INLINE HOOK: we only need to redirect a
 * call the module already imports, and rewriting one pointer-sized IAT
 * slot is atomic on x64. An inline patch would mean rewriting code bytes
 * while other threads may be executing them, which is a far worse trade
 * for no benefit here.
 * ============================================================ */

#include <windows.h>
#include <tlhelp32.h>        /* module enumeration */
#include <intrin.h>          /* _ReturnAddress */
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "watermark_result.h"

/* The exported hook wrappers are defined at file scope further down - they
 * cannot be file-local, because CFG only accepts exported functions as
 * indirect call targets (see the note above their definitions). The layer
 * table needs their addresses, so they are declared here. */
extern "C" {
__declspec(dllexport) int  WINAPI ScHookLoadStringW(HINSTANCE, UINT, LPWSTR, int);
__declspec(dllexport) BOOL WINAPI ScHookExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
__declspec(dllexport) int  WINAPI ScHookDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
__declspec(dllexport) int  WINAPI ScHookDrawTextExW(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
}

/* The C# side marshals this struct at a fixed 68 bytes (see ResultSize in
 * WatermarkSuppressor.cs). A drift between the two would surface as "the hook
 * did nothing" rather than as an error, so fail the BUILD instead. */
static_assert(sizeof(ScWmResult) == 68,
              "ScWmResult layout changed - update WatermarkSuppressor.cs to match");

namespace {

/* Resource IDs suppressed on this machine's build. Deliberately does NOT
 * include 33109 "Evaluation copy." / 33110 "For testing purposes only." /
 * the licensing and SecureBoot strings that share this resource block -
 * the brief was "Test Mode plus the build strings", and narrowing the
 * list is what keeps this from silently swallowing warnings the user may
 * actually want to see. */
constexpr unsigned int kWatermarkIds[] = { 33088u, 33090u, 33091u, 33092u, 33093u, 33108u };

/* ------------------------------------------------------------------
 * Module and trampoline state
 * ------------------------------------------------------------------ */

HMODULE g_self       = nullptr;
HMODULE g_shell32    = nullptr;
HMODULE g_explorer   = nullptr;

using LoadStringW_t = int  (WINAPI*)(HINSTANCE, UINT, LPWSTR, int);
using ExtTextOutW_t = BOOL (WINAPI*)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
using DrawTextW_t   = int  (WINAPI*)(HDC, LPCWSTR, int, LPRECT, UINT);
using DrawTextExW_t = int  (WINAPI*)(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);

LoadStringW_t g_origLoadStringW = nullptr;
ExtTextOutW_t g_origExtTextOutW = nullptr;
DrawTextW_t   g_origDrawTextW   = nullptr;
DrawTextExW_t g_origDrawTextExW = nullptr;

/* Every slot we rewrote, so ScWatermarkRemove can put it back exactly. */
struct PatchedSlot {
    void** slot;
    void*  original;
};
PatchedSlot g_slots[16];
int         g_slotCount = 0;

/* The watermark strings as this Windows actually spells them, read from
 * shell32's resources at load via OUR OWN import of LoadStringW. Our own
 * IAT is never patched, so this reads the real strings rather than
 * coming back through our hook. */
std::vector<std::wstring> g_watermarkTexts;

ScWmResult* g_result = nullptr;

/* ------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------ */

bool NameEquals(const char* a, const char* b) {
    return a && b && _stricmp(a, b) == 0;
}

bool ContainsNoCase(const char* hay, const char* needle) {
    if (!hay || !needle) return false;
    size_t n = strlen(needle);
    if (n == 0) return false;
    for (const char* p = hay; *p; ++p) {
        if (_strnicmp(p, needle, n) == 0) return true;
    }
    return false;
}

void CountHit(unsigned int layer) {
    if (g_result && layer < SCWM_LAYER_COUNT) {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_result->hits[layer]));
    }
}

/* ------------------------------------------------------------------
 * PE access. The module is already mapped into this process, so every
 * RVA resolves as base + rva - no section walking needed.
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------
 * Diagnostic log.
 *
 * Written only from ScWatermarkInit, never from a hook, so it can afford
 * file IO. It exists because the failure mode this feature actually has is
 * "installed but nothing changed", and from outside the process there is no
 * way to tell a wrong module from a wrong resource ID from a hook that never
 * fired. Reading a log beats rebuilding to add a print.
 *
 * %LOCALAPPDATA%\ShieldCord\watermark.log - the user's own profile, which is
 * where explorer runs, and the same folder the app's UI preferences live in.
 * ------------------------------------------------------------------ */

void LogLine(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    wcscat_s(path, L"\\ShieldCord");
    CreateDirectoryW(path, nullptr);
    wcscat_s(path, L"\\watermark.log");

    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    wchar_t body[1024] = {};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, args);
    va_end(args);

    wchar_t line[1200] = {};
    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 L"%04u-%02u-%02u %02u:%02u:%02u [pid %lu] %s\r\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 GetCurrentProcessId(), body);

    /* UTF-8, not UTF-16: cmd's `type` renders UTF-16 with interleaved nulls,
     * and a diagnostic log nobody can read with the obvious command is not
     * much of a diagnostic. */
    char utf8[2048] = {};
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (utf8Len > 1) {
        DWORD written = 0;
        WriteFile(f, utf8, static_cast<DWORD>(utf8Len - 1), &written, nullptr);
    }
    CloseHandle(f);
}

/* Every module currently loaded in THIS process.
 *
 * The whole set, not just shell32 and explorer. The watermark text is only
 * fetched by one module, and it is not necessarily either of those - 100-odd
 * modules in explorer import LoadStringW. Patching all of them is safe
 * because what a hook DOES is decided by the resource ID it matches, not by
 * which module it was reached from; a hook reached from an unrelated caller
 * costs one compare and a tail call. */
std::vector<HMODULE> LoadedModules() {
    std::vector<HMODULE> mods;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = {};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (me.hModule != g_self) mods.push_back(me.hModule);
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }

    if (mods.empty()) {
        if (g_shell32) mods.push_back(g_shell32);
        if (g_explorer) mods.push_back(g_explorer);
    }
    return mods;
}

bool ModuleHeaders(HMODULE mod, BYTE** outBase, IMAGE_NT_HEADERS** outNt) {
    if (!mod) return false;
    BYTE* base = reinterpret_cast<BYTE*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *outBase = base;
    *outNt   = nt;
    return true;
}

/* Does the IAT of `mod` import `funcName` from any of the accepted DLL
 * names? That question is answered either way; `apply` decides whether we
 * also rewrite the slot. Keeping detection and patching in one walk is
 * what lets the result struct report "this build has the path" and "we
 * hooked it" as two separate facts. */
bool PatchImport(HMODULE mod, const char* funcName,
                 const char* const* exactDlls, size_t exactCount,
                 const char* dllSubstring,
                 bool apply, void* hook, void** original, void** outSlot) {
    BYTE* base = nullptr;
    IMAGE_NT_HEADERS* nt = nullptr;
    if (!ModuleHeaders(mod, &base, &nt)) return false;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* dll = reinterpret_cast<const char*>(base + desc->Name);

        bool accepted = false;
        for (size_t i = 0; i < exactCount && !accepted; ++i) {
            accepted = NameEquals(dll, exactDlls[i]);
        }
        if (!accepted && dllSubstring) accepted = ContainsNoCase(dll, dllSubstring);
        if (!accepted) continue;

        /* OriginalFirstThunk is the names table; it can be absent in a
         * bound image, in which case FirstThunk holds the same layout
         * until the loader overwrites it. */
        DWORD namesRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + namesRva);
        auto* iat   = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;  /* by ordinal, no name */
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), funcName) != 0) continue;

            void** slot = reinterpret_cast<void**>(&iat->u1.Function);
            *outSlot = slot;
            if (!apply) return true;

            /* Both shell32 and explorer import these from the same DLL,
             * so the originals should agree. If they do not, refuse
             * rather than guess which trampoline is correct. */
            if (*original == nullptr) {
                *original = *slot;
            } else if (*slot != *original) {
                return false;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
            *slot = hook;
            VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);  /* best effort */

            /* Confirm the write landed before claiming it did. */
            if (*slot != hook) return false;

            if (g_slotCount < static_cast<int>(_countof(g_slots))) {
                g_slots[g_slotCount].slot     = slot;
                g_slots[g_slotCount].original = *original;
                ++g_slotCount;
            }
            return true;
        }
    }
    return false;
}

/* Is `dllName` delay-imported by `mod` with the given ordinal? Used only
 * to REPORT the build-28000+ glow path, never to patch it. */
bool HasDelayImportedOrdinal(HMODULE mod, const wchar_t* dllName, WORD ordinal) {
    BYTE* base = nullptr;
    IMAGE_NT_HEADERS* nt = nullptr;
    if (!ModuleHeaders(mod, &base, &nt)) return false;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress) return false;

    /* Defined here rather than taken from winnt.h: the header's union
     * member naming varies with NONAMELESSUNION, and this layout is fixed
     * by the PE spec. */
    struct DelayDesc {
        DWORD attributes, dllNameRva, moduleHandleRva, iatRva, intRva,
              boundIatRva, unloadIatRva, timeStamp;
    };

    auto* desc = reinterpret_cast<DelayDesc*>(base + dir.VirtualAddress);
    for (; desc->dllNameRva; ++desc) {
        /* Bit 0 clear means the addresses are VAs, not RVAs (pre-XP
         * format). Nothing current uses it, but the check is one line. */
        const bool rvaBased = (desc->attributes & 1) != 0;
        if (!rvaBased) continue;

        const wchar_t* name = reinterpret_cast<const wchar_t*>(base + desc->dllNameRva);
        if (_wcsicmp(name, dllName) != 0) continue;

        if (!desc->intRva) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->intRva);
        for (; names->u1.AddressOfData; ++names) {
            if (!(names->u1.Ordinal & IMAGE_ORDINAL_FLAG)) continue;
            if (static_cast<WORD>(names->u1.Ordinal & 0xFFFFu) == ordinal) return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------
 * The hooks live at FILE SCOPE, outside this anonymous namespace, and are
 * exported. That is a CFG requirement, not a style choice: an IAT call in
 * explorer.exe or shell32.dll is validated against the process's
 * valid-call-target bitmap, and only exported functions are added to it by
 * the loader. A file-local hook would fail that check and terminate the
 * shell. See the definitions after the namespace close.
 * ------------------------------------------------------------------ */

bool IsWatermarkText(LPCWSTR s, int len) {
    if (!s || len <= 0) return false;
    for (const std::wstring& wm : g_watermarkTexts) {
        if (static_cast<int>(wm.size()) == len &&
            wmemcmp(wm.c_str(), s, static_cast<size_t>(len)) == 0) {
            return true;
        }
    }
    return false;
}

/* shell32's address range, used to recognise a caller. Cached at install so
 * the hot path never has to walk PE headers. */
uintptr_t g_shell32Start = 0;
uintptr_t g_shell32End   = 0;

/* ------------------------------------------------------------------
 * Suppression bodies.
 *
 * Kept separate from the exported hook wrappers below so the wrappers can
 * be thin enough for SEH (MSVC refuses __try in a function that needs
 * object unwinding), and so that "on any fault, behave as though not
 * installed" is one line at each call site.
 * ------------------------------------------------------------------ */

bool TrySuppressLoadString(HINSTANCE hInstance, UINT uID, LPWSTR lpBuffer, int cchBufferMax) {
    bool wanted = false;
    for (unsigned int id : kWatermarkIds) {
        if (uID == id) { wanted = true; break; }
    }
    if (!wanted) return false;

    CountHit(SCWM_LAYER_LOADSTRING);

    if (g_result) {
        g_result->lastIdHit = uID;

        /* Matching on the ID alone is deliberate - if the hInstance or caller
         * assumption is ever wrong we still want the watermark gone. These
         * counters record when that happens so the assumption can be
         * corrected from evidence rather than guesswork. */
        HMODULE source = reinterpret_cast<HMODULE>(hInstance);
        if (source != g_shell32 && source != g_explorer) {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_result->idOnlyHits));
        }

        /* _ReturnAddress() names the module that made the call, which is
         * exact when we patch that module's own IAT slot. Recorded rather
         * than filtered on: if this ever reads non-zero while the watermark
         * still disappears, the filter is what should be tightened, not the
         * matching. */
        uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (caller < g_shell32Start || caller >= g_shell32End) {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                &g_result->callerNotShell32Hits));
        }
    }

    if (lpBuffer) {
        if (cchBufferMax == 0) {
            /* Pointer mode: lpBuffer is really an LPWSTR* that is meant to
             * receive a read-only pointer to the resource. Writing
             * lpBuffer[0] here would corrupt the caller's stack, so hand
             * back an empty string instead. It lives in this DLL's .rdata,
             * which stays mapped in the target for the process lifetime -
             * never NULL, because a caller that ignores the return value
             * will still dereference it. */
            *reinterpret_cast<LPCWSTR*>(lpBuffer) = L"";
        } else {
            lpBuffer[0] = L'\0';
        }
    }
    return true;   /* "no string" - callers treat this as nothing to draw */
}

/* The bottom-right box the desktop watermark is painted into, and the window
 * that owns the desktop. Computed once at init. */
int  g_cornerLeft  = 0;
int  g_cornerTop   = 0;
bool g_cornerValid = false;
HWND g_desktopWnd  = nullptr;

/* Sized generously and clamped to the screen. The box only has to CONTAIN the
 * watermark; on the desktop device context there is nothing else in the
 * bottom-right corner for it to catch. No DPI scaling: a box this size covers
 * the text at any scale this will meet, and a hard dependency on
 * GetDpiForSystem would refuse to load on a build that lacks it. */
void ComputeWatermarkCorner() {
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) return;

    int boxW = 800; if (boxW > w / 2) boxW = w / 2;
    int boxH = 160; if (boxH > h / 3) boxH = h / 3;

    g_cornerLeft  = w - boxW;
    g_cornerTop   = h - boxH;
    g_cornerValid = true;
}

/* Is this draw landing in the corner where the desktop watermark is painted?
 *
 * This exists because the EDITION string ("Windows 11 Pro") is not one of
 * shell32's watermark resources - it comes from winbrand.dll's branding data,
 * so matching on the strings we can read out of shell32 misses it, and the
 * result is a lone "Windows 11 Pro" left sitting in the corner. Confirmed
 * exactly that way in the VM before this was added.
 *
 * Two things keep it from eating text it should not: the corner is empty on a
 * normal desktop, and the draw has to be on the DESKTOP's device context. An
 * application's own window is a different DC, and if the DC cannot be
 * identified at all the corner test stands alone - there is still nothing
 * else painting in the bottom-right of a bare desktop. */
bool InWatermarkCorner(int x, int y, HDC hdc) {
    if (!g_cornerValid) return false;
    if (x < g_cornerLeft || y < g_cornerTop) return false;

    HWND wnd = WindowFromDC(hdc);
    if (wnd && g_desktopWnd) {
        HWND root = GetAncestor(wnd, GA_ROOT);
        if (root && root != g_desktopWnd) return false;
    }
    return true;
}

bool TrySuppressText(LPCWSTR s, int len) {
    return IsWatermarkText(s, len);
}

/* ------------------------------------------------------------------
 * Shared section + the exported entry points
 * ------------------------------------------------------------------ */

bool AttachResultSection() {
    wchar_t name[64] = {};
    swprintf_s(name, L"Local\\ShieldCordWM_%lu", GetCurrentProcessId());

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!mapping) return false;

    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ScWmResult));
    CloseHandle(mapping);   /* the view keeps it alive */
    if (!view) return false;

    g_result = static_cast<ScWmResult*>(view);
    return true;
}

void ReadWatermarkTexts() {
    if (!g_shell32) return;

    /* Logged per ID, and loudly on failure. If 33088 does not resolve to
     * "Test Mode" here, the whole premise of the primary hook is wrong and
     * everything downstream is wasted effort - so this is the first thing
     * to read in the log. */
    for (unsigned int id : kWatermarkIds) {
        wchar_t buf[512] = {};
        int n = ::LoadStringW(reinterpret_cast<HINSTANCE>(g_shell32), id, buf,
                              static_cast<int>(_countof(buf)));
        if (n > 0) {
            g_watermarkTexts.emplace_back(buf, static_cast<size_t>(n));
            LogLine(L"resource %u = \"%s\"", id, buf);
        } else {
            LogLine(L"resource %u did NOT resolve (LoadStringW returned %d)", id, n);
        }
    }
}

void ProbeAndPatch() {
    static const char* kLoaderDlls[] = { "kernel32.dll", "kernelbase.dll", "user32.dll" };
    static const char* kGdiDlls[]    = { "gdi32.dll", "gdi32full.dll" };
    static const char* kUserDlls[]   = { "user32.dll" };

    const unsigned int skip = g_result ? g_result->inDisableMask : 0u;

    struct Layer {
        unsigned int       bit;
        const char*        func;
        const char* const* dlls;
        size_t             dllCount;
        const char*        dllSubstring;
        void*              hook;
        void**             original;
    };

    /* Every module, for every layer. The module that fetches the watermark
     * text is not necessarily shell32 and need not be explorer, and picking
     * the wrong one fails silently - so patch everything that imports the
     * call and let the resource-ID match decide what is actually suppressed.
     * An extra hooked call from an unrelated caller costs one compare. */
    const Layer layers[] = {
        { SCWM_LAYER_LOADSTRING, "LoadStringW", kLoaderDlls, _countof(kLoaderDlls), "libraryloader",
          reinterpret_cast<void*>(&ScHookLoadStringW),
          reinterpret_cast<void**>(&g_origLoadStringW) },
        { SCWM_LAYER_EXTTEXTOUT, "ExtTextOutW", kGdiDlls, _countof(kGdiDlls), nullptr,
          reinterpret_cast<void*>(&ScHookExtTextOutW),
          reinterpret_cast<void**>(&g_origExtTextOutW) },
        { SCWM_LAYER_DRAWTEXT, "DrawTextW", kUserDlls, _countof(kUserDlls), nullptr,
          reinterpret_cast<void*>(&ScHookDrawTextW),
          reinterpret_cast<void**>(&g_origDrawTextW) },
        { SCWM_LAYER_DRAWTEXT, "DrawTextExW", kUserDlls, _countof(kUserDlls), nullptr,
          reinterpret_cast<void*>(&ScHookDrawTextExW),
          reinterpret_cast<void**>(&g_origDrawTextExW) },
    };

    const std::vector<HMODULE> modules = LoadedModules();
    LogLine(L"probe: %u modules loaded, %u watermark string(s) resolved",
            static_cast<unsigned>(modules.size()),
            static_cast<unsigned>(g_watermarkTexts.size()));

    unsigned int present = 0;
    unsigned int installed = 0;

    for (const Layer& layer : layers) {
        const bool apply = (skip & layer.bit) == 0;
        int patched = 0;

        for (HMODULE mod : modules) {
            if (!mod) continue;
            void* slot = nullptr;
            if (!PatchImport(mod, layer.func, layer.dlls, layer.dllCount, layer.dllSubstring,
                             apply, layer.hook, layer.original, &slot)) {
                continue;   /* this module does not import it by name */
            }
            present |= layer.bit;
            if (apply) installed |= layer.bit;
            ++patched;
        }

        LogLine(L"probe: %S present in %d module(s), %s", layer.func, patched,
                apply ? L"patched" : L"NOT patched (disabled by mask)");
    }

    /* The build-28000+ path: detected and reported, never hooked. See the
     * file header for why the only available hook here would be too blunt
     * to install without a way to verify it. */
    if (g_shell32 && HasDelayImportedOrdinal(g_shell32, L"uxtheme.dll", 126)) {
        present |= SCWM_LAYER_GLOW;
        LogLine(L"probe: UxTheme ordinal 126 is delay-imported (build 28000+ path) - "
                L"detected only, not hooked");
    }

    LogLine(L"probe done: present=0x%X installed=0x%X", present, installed);

    if (g_result) {
        g_result->presentMask   = present;
        g_result->installedMask = installed;
        g_result->status        = installed ? SCWM_OK : SCWM_ERR_NOTHING_PATCHED;
    }
}

} /* namespace */

/* ------------------------------------------------------------------
 * The hooks.
 *
 * FILE SCOPE AND EXPORTED, and that is a CFG requirement rather than a
 * style choice. explorer.exe and shell32.dll are built with /guard:cf,
 * which validates indirect call targets - including calls through an IAT.
 * Our hooks are exactly such targets, and the loader only adds EXPORTED
 * functions to the process's valid-call-target bitmap. A file-local hook
 * would fail that check with FAST_FAIL_INVALID_INDIRECT_CALL_TARGET
 * (0xC0000409), which terminates explorer and takes the desktop with it.
 *
 * Each wrapper exists only to hold SEH: MSVC refuses __try in a function
 * that needs object unwinding, so the real work is in the namespace above
 * and these stay object-free. On any fault they fall back to the original,
 * which is the right trade for a function whose only job is to blank a
 * string - "behave as though not installed" beats crashing the shell.
 * ------------------------------------------------------------------ */

extern "C" {

__declspec(dllexport) int WINAPI ScHookLoadStringW(HINSTANCE hInstance, UINT uID,
                                                   LPWSTR lpBuffer, int cchBufferMax) {
    bool suppressed = false;
    __try {
        suppressed = TrySuppressLoadString(hInstance, uID, lpBuffer, cchBufferMax);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        suppressed = false;
    }
    if (suppressed) return 0;
    return g_origLoadStringW(hInstance, uID, lpBuffer, cchBufferMax);
}

__declspec(dllexport) BOOL WINAPI ScHookExtTextOutW(HDC hdc, int x, int y, UINT options,
                                                    const RECT* lprect, LPCWSTR lpString,
                                                    UINT c, const INT* lpDx) {
    bool suppressed = false;
    __try {
        // By text where we can name the string, by position where we cannot
        // (the edition string, and any glyph-index draw where lpString is not
        // text at all).
        suppressed = TrySuppressText(lpString, static_cast<int>(c))
                     || InWatermarkCorner(x, y, hdc);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        suppressed = false;
    }
    if (suppressed) {
        CountHit(SCWM_LAYER_EXTTEXTOUT);
        return TRUE;   /* report success so the caller sees nothing wrong */
    }
    return g_origExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
}

__declspec(dllexport) int WINAPI ScHookDrawTextW(HDC hdc, LPCWSTR lpchText, int cchText,
                                                 LPRECT lprc, UINT format) {
    bool suppressed = false;
    __try {
        const int len = (cchText < 0)
            ? (lpchText ? static_cast<int>(wcslen(lpchText)) : 0)
            : cchText;
        suppressed = TrySuppressText(lpchText, len)
                     || InWatermarkCorner(lprc ? lprc->left : 0, lprc ? lprc->top : 0, hdc);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        suppressed = false;
    }
    if (suppressed) {
        CountHit(SCWM_LAYER_DRAWTEXT);
        return 0;
    }
    return g_origDrawTextW(hdc, lpchText, cchText, lprc, format);
}

__declspec(dllexport) int WINAPI ScHookDrawTextExW(HDC hdc, LPWSTR lpchText, int cchText,
                                                   LPRECT lprc, UINT format,
                                                   LPDRAWTEXTPARAMS lpdtp) {
    bool suppressed = false;
    __try {
        const int len = (cchText < 0)
            ? (lpchText ? static_cast<int>(wcslen(lpchText)) : 0)
            : cchText;
        suppressed = TrySuppressText(lpchText, len)
                     || InWatermarkCorner(lprc ? lprc->left : 0, lprc ? lprc->top : 0, hdc);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        suppressed = false;
    }
    if (suppressed) {
        CountHit(SCWM_LAYER_DRAWTEXT);
        return 0;
    }
    return g_origDrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
}

} /* extern "C" */

/* ------------------------------------------------------------------
 * Exports
 * ------------------------------------------------------------------ */

extern "C" __declspec(dllexport) unsigned int WINAPI ScWatermarkInit(void) {
    if (g_result && g_result->magic == SCWM_MAGIC) {
        /* Re-injected into the same process: patching twice would save
         * our own hook as the "original" and lose the real trampoline. */
        g_result->alreadyApplied = 1;
        LogLine(L"ScWatermarkInit: already applied, nothing to do");
        return g_result->status;
    }

    LogLine(L"ScWatermarkInit: enter (disable mask 0x%X)",
            g_result ? g_result->inDisableMask : 0u);

    if (!AttachResultSection()) {
        LogLine(L"ScWatermarkInit: FAILED to open the result section");
        return SCWM_ERR_NO_SECTION;
    }

    g_shell32  = GetModuleHandleW(L"shell32.dll");
    g_explorer = GetModuleHandleW(nullptr);
    if (!g_shell32 && !g_explorer) {
        g_result->status = SCWM_ERR_NO_TARGET;
        return g_result->status;
    }

    LogLine(L"modules: shell32=0x%p explorer=0x%p",
            reinterpret_cast<void*>(g_shell32), reinterpret_cast<void*>(g_explorer));

    /* Cache shell32's range so the hot path's caller test is two compares
     * and never a PE walk. */
    {
        BYTE* base = nullptr;
        IMAGE_NT_HEADERS* nt = nullptr;
        if (ModuleHeaders(g_shell32, &base, &nt)) {
            g_shell32Start = reinterpret_cast<uintptr_t>(base);
            g_shell32End   = g_shell32Start + nt->OptionalHeader.SizeOfImage;
        }
    }

    /* Before patching, and via our own import of LoadStringW rather than
     * the IAT slot we are about to rewrite. */
    ReadWatermarkTexts();

    /* The corner filter's two inputs. GetShellWindow is the desktop that owns
     * the watermark; on Windows 11 the visible surface is often a WorkerW
     * child of it, which is why the paint can land on a DC whose root is not
     * the shell window itself - the filter allows for that. */
    g_desktopWnd = GetShellWindow();
    ComputeWatermarkCorner();
    LogLine(L"corner filter: x>=%d y>=%d desktop=0x%p",
            g_cornerLeft, g_cornerTop, reinterpret_cast<void*>(g_desktopWnd));

    ProbeAndPatch();

    g_result->shell32Base  = static_cast<unsigned int>(reinterpret_cast<ULONG_PTR>(g_shell32));
    g_result->explorerBase = static_cast<unsigned int>(reinterpret_cast<ULONG_PTR>(g_explorer));
    g_result->structVersion = SCWM_STRUCT_VERSION;
    g_result->magic = SCWM_MAGIC;

    LogLine(L"ScWatermarkInit: done, status=%u", g_result->status);
    return g_result->status;
}

extern "C" __declspec(dllexport) unsigned int WINAPI ScWatermarkRemove(void) {
    for (int i = 0; i < g_slotCount; ++i) {
        void** slot = g_slots[i].slot;
        if (!slot) continue;
        DWORD oldProtect = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) continue;
        *slot = g_slots[i].original;
        VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    }
    g_slotCount = 0;

    if (g_result) g_result->installedMask = 0;
    return SCWM_OK;
}

/* ------------------------------------------------------------------
 * DllMain
 *
 * Deliberately does almost nothing. Real work happens in
 * ScWatermarkInit, which the injector calls on a thread of its own AFTER
 * LoadLibraryW has returned - so it runs with the loader lock released
 * and the CRT initialised. Doing this in DllMain would mean running STL
 * and PE walks under the loader lock for no reason.
 * ------------------------------------------------------------------ */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);

        /* PIN OURSELVES. Our hooks live in this image, so the moment it
         * unloads, the next ExtTextOutW in explorer jumps into freed
         * memory and the shell dies. Pinning means FreeLibrary from the
         * injector cannot unload us, and the image goes away only when
         * explorer itself exits. The cost is a few tens of KB resident
         * for the life of the shell process. */
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
        g_self = pinned;
    }
    return TRUE;
}
