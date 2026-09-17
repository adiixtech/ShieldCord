// ============================================================
// ShieldCord — WatermarkSuppressor.cs
//
// Removes the Windows "Test Mode" desktop watermark by loading
// shieldcord_watermark.dll into explorer.exe, then PROVES it worked by
// looking at the pixels.
//
// WHY THE TRAY DOES THIS AND NOT THE SERVICE:
//   The watermark is per-session shell state. This app already runs in
//   the user's session, as the same user, at the same integrity level as
//   explorer — so OpenProcess + WriteProcessMemory succeed with NO
//   elevation at all. A session-0 service would have to cross the session
//   boundary to reach explorer, which is exactly what session isolation
//   exists to prevent. This is also why the feature never touches
//   ElevatedHelper/ElevatedVerbMode: it is not a privileged action.
//
// WHY IT VERIFIES:
//   Every hook in the DLL could silently fail to match — a resource ID
//   that moved, a cached string, a render path nobody anticipated — and
//   every one of those failures would otherwise report as success. The
//   capture before and after is the only part of this that cannot lie,
//   so it is not optional.
//
// NOTHING IS PERSISTED. There is no registry key, no service, no file in
// System32. The hook lives in explorer's memory and dies with explorer,
// which is what makes this trivial to undo: stop applying it and restart
// the shell.
// ============================================================

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace ShieldCordUI.Services;

/// <summary>How the removal attempt actually ended.</summary>
internal enum WatermarkOutcome
{
    /// <summary>The user has the feature switched off.</summary>
    Disabled,

    /// <summary>Nothing is being painted, so there is nothing to remove.</summary>
    NotApplicable,

    /// <summary>Applied, and the capture proves the watermark is gone.</summary>
    VerifiedRemoved,

    /// <summary>
    /// Applied, but the region could not be observed — usually because the
    /// desktop is covered. Reported honestly rather than claimed as
    /// success; the next poll will try again.
    /// </summary>
    AppliedUnverified,

    /// <summary>Attempted and did not work.</summary>
    Failed,
}

[Flags]
internal enum WatermarkLayer : uint
{
    None       = 0,
    LoadString = 0x0001,
    ExtTextOut = 0x0002,
    DrawText   = 0x0004,
    Glow       = 0x0008,
}

internal sealed record WatermarkReport(
    WatermarkOutcome Outcome,
    string Detail,
    WatermarkLayer Present = WatermarkLayer.None,
    WatermarkLayer Installed = WatermarkLayer.None,
    uint Status = 0,
    double PercentChanged = -1,
    uint LoadStringHits = 0,
    uint IdOnlyHits = 0)
{
    public bool IsSuccess => Outcome is WatermarkOutcome.VerifiedRemoved;

    /// <summary>
    /// One line for the settings page / banner. Says what was actually
    /// verified, not what was attempted.
    ///
    /// On anything other than success it also reports which render paths this
    /// Windows turned out to have and how many times the hook fired — that is
    /// the whole point of probing instead of hard-coding a build table, and
    /// without it a failure here is undiagnosable from the outside.
    /// </summary>
    public string Summary
    {
        get
        {
            string line = Outcome switch
            {
                WatermarkOutcome.Disabled          => "Test Mode watermark: not hidden.",
                WatermarkOutcome.NotApplicable     => "Test Mode watermark: not shown on this PC.",
                WatermarkOutcome.VerifiedRemoved   => "Test Mode watermark: hidden.",
                WatermarkOutcome.AppliedUnverified => "Test Mode watermark: hidden (could not confirm — desktop was covered).",
                _                                  => "Test Mode watermark: could not be hidden.",
            };

            if (Outcome is WatermarkOutcome.Failed or WatermarkOutcome.AppliedUnverified
                         or WatermarkOutcome.NotApplicable)
            {
                line += $" Paths on this PC: {Describe(Present)}. Installed: {Describe(Installed)}."
                      + $" Watermark string requests seen: {LoadStringHits}.";
            }
            return line;
        }
    }

    private static string Describe(WatermarkLayer mask)
    {
        if (mask == WatermarkLayer.None) return "none";

        var parts = new List<string>();
        if (mask.HasFlag(WatermarkLayer.LoadString)) parts.Add("LoadStringW");
        if (mask.HasFlag(WatermarkLayer.ExtTextOut)) parts.Add("ExtTextOutW");
        if (mask.HasFlag(WatermarkLayer.DrawText)) parts.Add("DrawTextW");
        if (mask.HasFlag(WatermarkLayer.Glow)) parts.Add("UxTheme#126(unhookable)");
        return string.Join("+", parts);
    }
}

internal sealed class WatermarkSuppressor
{
    public const string DllName = "shieldcord_watermark.dll";
    private const string ExportInit = "ScWatermarkInit";
    private const string ExportRemove = "ScWatermarkRemove";

    // The marshalled mirror of ScWmResult in src/watermark/watermark_result.h.
    // EVERY field is 4 bytes so C and C# cannot disagree about padding. If
    // you change the header, change this - a layout drift here reads as
    // "the hook did nothing" rather than as an error.
    private const int ResultSize = 68;
    private const uint ResultMagic = 0x4D435753u;

    private const int OffInDisableMask = 12;   // magic, structVersion, status, inDisableMask

    private const uint ScwmOk = 0;

    // Below this, the capture is treated as unchanged. The watermark is
    // solid text roughly 200px across inside a ~420x80 region, so a real
    // removal moves several percent of the pixels; compositing noise moves
    // none of them.
    private const double ChangedThresholdPercent = 0.35;

    private const int SettleMs = 700;
    private const int ThreadWaitMs = 15000;

    private readonly object _gate = new();
    private int _appliedPid;
    private WatermarkReport? _appliedReport;
    private int _running;

    // Crash-loop guard. The one failure this feature must never have is a
    // machine that repeatedly kills its own desktop, so two shell restarts
    // inside the window stop it outright rather than re-injecting into the
    // replacement.
    private const int ShellRestartStrikeLimit = 2;
    private static readonly TimeSpan StrikeWindow = TimeSpan.FromMinutes(10);

    private int _shellRestarts;
    private DateTime _strikeWindowStartUtc;
    private bool _halted;

    // How many times to retry a failing apply for one explorer instance before
    // treating it as settled.
    private const int AttemptLimit = 3;
    private int _attemptPid;
    private int _attempts;

    /// <summary>The most recent outcome, for the settings page to display.</summary>
    public WatermarkReport LastReport { get; private set; } =
        new(WatermarkOutcome.NotApplicable, "Not attempted yet.");

    /// <summary>
    /// The shell was replaced while our hook was installed. Returns true once
    /// that has happened often enough that we should blame ourselves.
    ///
    /// This cannot tell a crash from the user deliberately restarting the
    /// shell, which is why it takes two inside ten minutes rather than one -
    /// a single restart is ordinary and must not disable the feature.
    /// </summary>
    private bool NoteShellRestart()
    {
        DateTime now = DateTime.UtcNow;
        if (_shellRestarts == 0 || now - _strikeWindowStartUtc > StrikeWindow)
        {
            _shellRestarts = 0;
            _strikeWindowStartUtc = now;
        }
        _shellRestarts++;
        return _shellRestarts >= ShellRestartStrikeLimit;
    }

    /// <summary>
    /// Apply to the current explorer if it has not been done already.
    ///
    /// BLOCKING: does Win32 waits and sleeps. Call it from Task.Run, the
    /// same way the app already wraps ServiceControl.Query - never from the
    /// UI thread.
    /// </summary>
    public WatermarkReport EnsureApplied(bool enabled)
    {
        var report = EnsureAppliedCore(enabled);
        LastReport = report;
        LogOutcome(report);
        return report;
    }

    /// <summary>
    /// Append the outcome to %LOCALAPPDATA%\ShieldCord\watermark-app.log.
    ///
    /// The DLL's own log says what it hooked; this one says what was observed
    /// afterwards, which is the half that decides whether any of it worked.
    /// Both exist because the failure this feature actually has is a silent
    /// one, and reading a file beats rebuilding to add a print.
    /// </summary>
    private static void LogOutcome(WatermarkReport report)
    {
        try
        {
            string dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "ShieldCord");
            Directory.CreateDirectory(dir);

            File.AppendAllText(
                Path.Combine(dir, "watermark-app.log"),
                $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {report.Outcome} | "
                + $"present={report.Present} installed={report.Installed} status={report.Status} "
                + $"changed={report.PercentChanged:0.0}% loadStringHits={report.LoadStringHits} "
                + $"idOnlyHits={report.IdOnlyHits}{Environment.NewLine}",
                System.Text.Encoding.UTF8);
        }
        catch
        {
            // Diagnostics must never be the reason the feature fails.
        }
    }

    private WatermarkReport EnsureAppliedCore(bool enabled)
    {
        if (!enabled)
        {
            Reset();
            return new WatermarkReport(WatermarkOutcome.Disabled, "Switched off.");
        }

        // One attempt at a time. The status poll can tick while a previous
        // attempt is still working, and two injections racing would mean two
        // sets of IAT writes for no reason.
        if (Interlocked.CompareExchange(ref _running, 1, 0) != 0)
        {
            return new WatermarkReport(WatermarkOutcome.AppliedUnverified,
                                       "An attempt is already in progress.");
        }

        try
        {
            int pid = FindExplorerPid();
            if (pid == 0)
            {
                return new WatermarkReport(WatermarkOutcome.Failed, "explorer.exe is not running.");
            }

            lock (_gate)
            {
                if (_appliedPid == pid && _appliedReport is not null)
                {
                    // Already settled for THIS explorer instance. Report what
                    // was actually observed then, verbatim.
                    //
                    // Synthesizing a success here is exactly how an earlier
                    // version claimed "hidden" for runs that had failed: the
                    // retry limit caches the PID for a FAILED outcome too, so
                    // a canned VerifiedRemoved turned a failure into a
                    // permanent claim of success.
                    return _appliedReport;
                }

                if (_halted)
                {
                    return new WatermarkReport(WatermarkOutcome.Failed,
                        "Stopped after the shell restarted twice in a row. "
                        + "Turn the setting off and on again to retry.");
                }

                // explorer was replaced after we had hooked it. Once is
                // ordinary - people restart the shell. Twice inside the
                // window is our DLL killing it, and re-injecting into the
                // replacement would build a machine that repeatedly destroys
                // its own desktop. That is the one failure this feature must
                // never have, so it stops and says so.
                if (_appliedPid != 0 && NoteShellRestart())
                {
                    _halted = true;
                    _appliedPid = 0;
                    return new WatermarkReport(WatermarkOutcome.Failed,
                        "The shell restarted twice after the watermark hook was applied, "
                        + "so ShieldCord has stopped trying. The watermark is back and will "
                        + "stay until you turn this on again from Settings.");
                }
            }

            var report = ApplyAndVerify(pid);

            lock (_gate)
            {
                // Settled outcomes are cached, so the steady state is a cheap
                // PID compare rather than a screen capture on every tick.
                //
                // NotApplicable is settled because test signing can only be
                // turned on across a REBOOT — which restarts both the shell and
                // this app — so "nothing to remove" cannot become stale within
                // one explorer's lifetime.
                //
                // A failure is retried, but only a bounded number of times: a
                // machine where this genuinely cannot work should not have its
                // screen grabbed every few seconds forever.
                bool settled = report.Outcome is WatermarkOutcome.VerifiedRemoved
                                              or WatermarkOutcome.NotApplicable;

                if (_attemptPid != pid)
                {
                    _attemptPid = pid;
                    _attempts = 0;
                }
                _attempts++;

                bool cache = settled || _attempts >= AttemptLimit;
                _appliedPid = cache ? pid : 0;

                // Kept alongside the PID so a cached explorer reports the
                // outcome that was actually reached, not a canned one.
                _appliedReport = cache ? report : null;
            }
            return report;
        }
        finally
        {
            Interlocked.Exchange(ref _running, 0);
        }
    }

    /// <summary>
    /// Forget the applied PID, so the next call re-applies.
    ///
    /// Also clears the crash-loop guard: turning the setting off and back on
    /// IS the deliberate retry the halted message asks for.
    /// </summary>
    public void Reset()
    {
        lock (_gate)
        {
            _appliedPid = 0;
            _appliedReport = null;
            _halted = false;
            _shellRestarts = 0;
            _attemptPid = 0;
            _attempts = 0;
        }
    }

    public static int FindExplorerPid()
    {
        // The shell that owns the desktop, not some other explorer.exe the
        // user launched to browse files — that one has no desktop to paint.
        IntPtr desktop = GetShellWindow();
        if (desktop == IntPtr.Zero) return 0;

        GetWindowThreadProcessId(desktop, out uint pid);
        return pid > 0 ? (int)pid : 0;
    }

    private WatermarkReport ApplyAndVerify(int pid)
    {
        string shipped = Path.Combine(AppContext.BaseDirectory, DllName);
        if (!File.Exists(shipped))
        {
            return new WatermarkReport(WatermarkOutcome.Failed,
                $"Missing {DllName} next to the app.");
        }

        // Inject a build-stamped copy, never the shipped file - see
        // StagedDllPath for why the shipped one must stay replaceable.
        string? dllPath = StagedDllPath(shipped);
        if (dllPath is null)
        {
            return new WatermarkReport(WatermarkOutcome.Failed,
                "Could not stage the hook for injection.");
        }

        var region = DesktopCapture.WatermarkRegion();
        byte[]? before = DesktopCapture.Capture(region);

        var (status, present, installed, already, hits, idOnly, detail) = InjectAndInit(pid, dllPath);
        if (status is null)
        {
            return new WatermarkReport(WatermarkOutcome.Failed, detail);
        }

        // "Already applied" from the DLL is NOT evidence the watermark is gone.
        // It only means we already tried against this process — the hooks can
        // be installed and never fire, which is exactly what an earlier
        // version of this got wrong: it returned VerifiedRemoved here and the
        // app cheerfully reported "hidden." while the watermark was plainly
        // still on screen. There is deliberately NO early return; everything
        // goes through the same pixel check, because pixels are the only part
        // of this that cannot be talked into lying.
        _ = already;

        if (status.Value != ScwmOk)
        {
            return new WatermarkReport(WatermarkOutcome.Failed,
                $"The hook reported status {status.Value}: {Describe(status.Value, present)}.",
                present, installed, status.Value, -1, hits, idOnly);
        }

        // Give explorer a reason to repaint the desktop, then look. If no
        // repaint could be forced, a stale frame would compare identical to
        // the baseline and this would read as a failed removal when the truth
        // is that nothing was observable — so that fact is carried into the
        // report rather than guessed at.
        bool repainted = DesktopCapture.ForceRepaint();
        Thread.Sleep(SettleMs);

        // Stability, not a single frame: a wallpaper re-apply can fade, and a
        // frame caught mid-transition differs from the baseline in every pixel
        // — which would read as success for the wrong reason.
        byte[]? after = DesktopCapture.CaptureStable(region);
        double changed = DesktopCapture.PercentChanged(before, after);
        bool visible = DesktopCapture.IsDesktopVisibleAt(region);

        // The hook firing is the strongest single signal: it means shell32
        // asked for one of the watermark resource IDs, i.e. the watermark
        // painter genuinely ran.
        bool painterRan = hits > 0;

        if (!visible)
        {
            return new WatermarkReport(WatermarkOutcome.AppliedUnverified,
                "Applied. The desktop was covered, so it could not be confirmed.",
                present, installed, status.Value, changed, hits, idOnly);
        }

        if (!painterRan && changed <= ChangedThresholdPercent)
        {
            return new WatermarkReport(WatermarkOutcome.NotApplicable,
                "Test Mode is not on, so there is no watermark to remove.",
                present, installed, status.Value, changed, hits, idOnly);
        }

        if (changed > ChangedThresholdPercent)
        {
            return new WatermarkReport(WatermarkOutcome.VerifiedRemoved,
                $"Removed and confirmed ({changed:0.0}% of the region changed).",
                present, installed, status.Value, changed, hits, idOnly);
        }

        // The painter ran and the pixels did not move. Either the region we
        // watch is wrong for this build, or the watermark is drawn from
        // somewhere other than the resource we blanked.
        return new WatermarkReport(WatermarkOutcome.Failed,
            "The watermark string was suppressed but the desktop still shows it"
            + (repainted ? "" : " (and no repaint could be forced, so the screen may just be stale)")
            + $". Present paths: {present}.",
            present, installed, status.Value, changed, hits, idOnly);
    }

    private static string Describe(uint status, WatermarkLayer present) => status switch
    {
        1 => "the tray could not create the shared section",
        2 => "shell32/explorer were not found in the target",
        3 => $"no render path could be hooked (found: {present})",
        _ => "unknown",
    };

    private readonly record struct InitOutcome(
        uint? Status, WatermarkLayer Present, WatermarkLayer Installed,
        bool Already, uint Hits, uint IdOnly, string Detail);

    /// <summary>
    /// Create the shared section, load the DLL into explorer, and call its
    /// init export on a thread of its own.
    /// </summary>
    private static InitOutcome InjectAndInit(int pid, string dllPath)
    {
        string sectionName = $"Local\\ShieldCordWM_{pid}";
        var fail = new InitOutcome(null, WatermarkLayer.None, WatermarkLayer.None, false, 0, 0, "");

        IntPtr mapping = IntPtr.Zero;
        IntPtr view = IntPtr.Zero;
        IntPtr process = IntPtr.Zero;
        IntPtr remotePath = IntPtr.Zero;
        IntPtr loadThread = IntPtr.Zero;
        IntPtr initThread = IntPtr.Zero;

        try
        {
            // The DLL derives this name from its own PID, so nothing has to
            // cross the process boundary.
            mapping = CreateFileMappingW(new IntPtr(-1), IntPtr.Zero, PAGE_READWRITE, 0,
                                         (uint)ResultSize, sectionName);
            if (mapping == IntPtr.Zero)
            {
                return fail with { Detail = $"Could not create the result section ({Marshal.GetLastWin32Error()})." };
            }

            view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, (UIntPtr)ResultSize);
            if (view == IntPtr.Zero)
            {
                return fail with { Detail = "Could not map the result section." };
            }

            process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                                  PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_READ, false, pid);
            if (process == IntPtr.Zero)
            {
                int err = Marshal.GetLastWin32Error();
                return fail with { Detail = $"Could not open explorer.exe ({err})." };
            }

            // Confirm by image path and session that this really is our own
            // shell before writing into it. Never inject into a PID that was
            // only identified by name.
            if (!IsOwnShellProcess(process, pid))
            {
                return fail with { Detail = "The desktop's owner is not this session's explorer.exe." };
            }

            IntPtr loadLibrary = GetProcAddress(GetModuleHandleW("kernel32.dll"), "LoadLibraryW");
            if (loadLibrary == IntPtr.Zero)
            {
                return fail with { Detail = "Could not resolve LoadLibraryW." };
            }

            byte[] pathBytes = System.Text.Encoding.Unicode.GetBytes(dllPath + "\0");
            remotePath = VirtualAllocEx(process, IntPtr.Zero, (UIntPtr)(uint)pathBytes.Length,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (remotePath == IntPtr.Zero)
            {
                return fail with { Detail = "Could not allocate in explorer.exe." };
            }

            if (!WriteProcessMemory(process, remotePath, pathBytes, (UIntPtr)(uint)pathBytes.Length, out _))
            {
                return fail with { Detail = $"Could not write the DLL path ({Marshal.GetLastWin32Error()})." };
            }

            loadThread = CreateRemoteThread(process, IntPtr.Zero, 0, loadLibrary, remotePath, 0, IntPtr.Zero);
            if (loadThread == IntPtr.Zero)
            {
                return fail with { Detail = $"Could not start the loader thread ({Marshal.GetLastWin32Error()})." };
            }
            if (WaitForSingleObject(loadThread, ThreadWaitMs) != WAIT_OBJECT_0)
            {
                return fail with { Detail = "Loading the DLL timed out." };
            }

            // NOT from GetExitCodeThread. It returns a DWORD, and x64 module
            // bases live above 4 GB, so the value comes back truncated - and
            // a truncated base plus an export RVA is an address we would then
            // start a thread at INSIDE EXPLORER. Ask the loader where the
            // module actually landed instead.
            // The staged name is what the loader knows it by in the target.
            IntPtr remoteBase = FindRemoteModuleBase(process, Path.GetFileName(dllPath));
            if (remoteBase == IntPtr.Zero)
            {
                // The classic causes are a blocked DLL (antivirus) or an
                // architecture mismatch.
                return fail with
                {
                    Detail = "explorer.exe did not load the DLL. Antivirus is the usual reason.",
                };
            }

            // Address of the export, in the TARGET process: module base there
            // plus the export's RVA here.
            long? rva = ExportRva(dllPath, ExportInit);
            if (rva is null)
            {
                return fail with
                {
                    Detail = $"{ExportInit} is not exported, or the DLL cannot be mapped.",
                };
            }

            initThread = CreateRemoteThread(process, IntPtr.Zero, 0,
                                            new IntPtr(remoteBase.ToInt64() + rva.Value),
                                            IntPtr.Zero, 0, IntPtr.Zero);
            if (initThread == IntPtr.Zero)
            {
                return fail with { Detail = $"Could not start the init thread ({Marshal.GetLastWin32Error()})." };
            }
            if (WaitForSingleObject(initThread, ThreadWaitMs) != WAIT_OBJECT_0)
            {
                return fail with { Detail = "Initialising the hook timed out." };
            }

            var result = Marshal.PtrToStructure<ScWmResultNative>(view);
            if (result.Magic != ResultMagic)
            {
                return fail with { Detail = "The DLL loaded but never initialised." };
            }

            return new InitOutcome(
                result.Status,
                (WatermarkLayer)result.PresentMask,
                (WatermarkLayer)result.InstalledMask,
                result.AlreadyApplied != 0,
                result.HitLoadString,
                result.IdOnlyHits,
                "");
        }
        catch (Exception ex)
        {
            return fail with { Detail = ex.Message };
        }
        finally
        {
            if (initThread != IntPtr.Zero) CloseHandle(initThread);
            if (loadThread != IntPtr.Zero) CloseHandle(loadThread);
            if (remotePath != IntPtr.Zero && process != IntPtr.Zero)
            {
                VirtualFreeEx(process, remotePath, UIntPtr.Zero, MEM_RELEASE);
            }
            if (process != IntPtr.Zero) CloseHandle(process);
            if (view != IntPtr.Zero) UnmapViewOfFile(view);
            if (mapping != IntPtr.Zero) CloseHandle(mapping);
        }
    }

    /// <summary>
    /// Ask a resident hook DLL to restore the IAT slots it patched, so turning
    /// the setting off takes effect now instead of at the next shell restart.
    ///
    /// Safe to do live. Restoring a slot is an atomic swap between two valid
    /// callable addresses, and the DLL pins itself so it stays mapped — a
    /// thread already inside a hook simply finishes its call. What is NOT
    /// possible is UNLOADING the DLL; that is why the file stays locked and
    /// why the app says a shell restart is what fully clears it.
    /// </summary>
    public bool Disable()
    {
        int pid = FindExplorerPid();
        if (pid == 0) return false;

        string shipped = Path.Combine(AppContext.BaseDirectory, DllName);
        if (!File.Exists(shipped)) return false;

        // Same staged file the apply used, so the module we look for in the
        // target is the one that was actually loaded there.
        string? dllPath = StagedDllPath(shipped);
        if (dllPath is null) return false;

        IntPtr process = IntPtr.Zero;
        IntPtr thread = IntPtr.Zero;
        try
        {
            process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                                  PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, pid);
            if (process == IntPtr.Zero) return false;

            IntPtr remoteBase = FindRemoteModuleBase(process, Path.GetFileName(dllPath));
            if (remoteBase == IntPtr.Zero) return false;   // never injected here

            long? rva = ExportRva(dllPath, ExportRemove);
            if (rva is null) return false;

            thread = CreateRemoteThread(process, IntPtr.Zero, 0,
                                        new IntPtr(remoteBase.ToInt64() + rva.Value),
                                        IntPtr.Zero, 0, IntPtr.Zero);
            if (thread == IntPtr.Zero) return false;

            return WaitForSingleObject(thread, ThreadWaitMs) == WAIT_OBJECT_0;
        }
        catch
        {
            return false;
        }
        finally
        {
            if (thread != IntPtr.Zero) CloseHandle(thread);
            if (process != IntPtr.Zero) CloseHandle(process);
            Reset();
        }
    }

    /// <summary>
    /// RVA of one of our exports, read WITHOUT running the DLL in this
    /// process.
    ///
    /// DONT_RESOLVE_DLL_REFERENCES maps the image without calling DllMain and
    /// without resolving imports, so this costs nothing and has no side
    /// effects here — we only ever call GetProcAddress on it. Subtracting the
    /// local base turns the address into an RVA, which is what makes it valid
    /// in the target too, whatever ASLR did there.
    /// </summary>
    private static long? ExportRva(string dllPath, string exportName)
    {
        IntPtr local = LoadLibraryExW(dllPath, IntPtr.Zero, DONT_RESOLVE_DLL_REFERENCES);
        if (local == IntPtr.Zero) return null;

        try
        {
            IntPtr proc = GetProcAddress(local, exportName);
            return proc == IntPtr.Zero ? null : proc.ToInt64() - local.ToInt64();
        }
        finally
        {
            FreeLibrary(local);
        }
    }

    /// <summary>
    /// Copy the shipped hook aside and return the path to inject.
    ///
    /// The DLL pins itself inside explorer, so whichever file we load stays
    /// LOCKED for as long as the shell runs. Injecting the shipped file
    /// directly would therefore mean every rebuild or upgrade needed a shell
    /// restart just to replace a file. Injecting a copy stamped with the
    /// build leaves the shipped file replaceable at any time, and a new build
    /// simply gets a new name — the old one stays lazily mapped until explorer
    /// next restarts, which costs nothing.
    ///
    /// Returns null if the copy could not be made.
    /// </summary>
    private static string? StagedDllPath(string shippedPath)
    {
        try
        {
            var info = new FileInfo(shippedPath);
            string stamp = $"{info.Length}_{info.LastWriteTimeUtc.Ticks}";

            string dir = Path.Combine(Path.GetTempPath(), "ShieldCord");
            Directory.CreateDirectory(dir);
            string staged = Path.Combine(dir, $"shieldcord_watermark_{stamp}.dll");

            // Reuse if present: it is either already the loaded one (locked,
            // same content, so nothing to do) or free to overwrite.
            if (!File.Exists(staged))
            {
                File.Copy(shippedPath, staged);
            }

            // Best effort tidy-up of superseded builds. The current one and
            // anything still mapped by a live explorer refuse to delete,
            // which is expected rather than an error.
            foreach (string old in Directory.GetFiles(dir, "shieldcord_watermark_*.dll"))
            {
                if (string.Equals(old, staged, StringComparison.OrdinalIgnoreCase)) continue;
                try { File.Delete(old); } catch { /* still mapped - fine */ }
            }

            return staged;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// Where did the loader put the DLL inside the target?
    ///
    /// Enumerating the target's modules is the reliable answer. The usual
    /// shortcut — reading the LoadLibraryW thread's exit code — is wrong on
    /// x64, where that value is a DWORD and module bases sit above 4 GB.
    /// </summary>
    private static IntPtr FindRemoteModuleBase(IntPtr process, string moduleName)
    {
        var modules = new IntPtr[1024];
        int bytes = IntPtr.Size * modules.Length;
        if (!K32EnumProcessModulesEx(process, modules, (uint)bytes, out uint needed, LIST_MODULES_ALL))
        {
            return IntPtr.Zero;
        }

        int count = (int)(needed / (uint)IntPtr.Size);
        if (count > modules.Length) count = modules.Length;

        var name = new char[260];
        for (int i = 0; i < count; i++)
        {
            uint len = K32GetModuleBaseNameW(process, modules[i], name, (uint)name.Length);
            if (len == 0) continue;
            if (string.Equals(new string(name, 0, (int)len), moduleName,
                              StringComparison.OrdinalIgnoreCase))
            {
                return modules[i];
            }
        }
        return IntPtr.Zero;
    }

    /// <summary>
    /// Confirm the process we are about to write into really is this
    /// session's shell — checked by image path and by session, never by the
    /// name we happened to look the PID up under. Windows has been moving
    /// shell pieces into their own processes, so a future build could hand
    /// the desktop to something else; this turns that into a refusal rather
    /// than an injection into the wrong process.
    /// </summary>
    private static bool IsOwnShellProcess(IntPtr process, int pid)
    {
        var path = new System.Text.StringBuilder(1024);
        uint size = (uint)path.Capacity;
        if (!QueryFullProcessImageNameW(process, 0, path, ref size)) return false;

        if (!path.ToString().EndsWith("\\explorer.exe", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        if (!ProcessIdToSessionId((uint)pid, out uint targetSession)) return false;
        if (!ProcessIdToSessionId((uint)Environment.ProcessId, out uint mine)) return false;
        return targetSession == mine;
    }

    // ── interop ──────────────────────────────────────────────────

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct ScWmResultNative
    {
        public uint Magic;
        public uint StructVersion;
        public uint Status;
        public uint InDisableMask;
        public uint PresentMask;
        public uint InstalledMask;
        public uint AlreadyApplied;
        public uint Reserved1;
        public uint Shell32Base;
        public uint ExplorerBase;
        // hits[SCWM_LAYER_COUNT] — spelled out rather than ByValArray so the
        // layer each counter belongs to is visible at the point of use.
        public uint HitLoadString;
        public uint HitExtTextOut;
        public uint HitDrawText;
        public uint HitGlow;
        public uint IdOnlyHits;
        public uint LastIdHit;
        public uint CallerNotShell32Hits;
    }

    private const uint PAGE_READWRITE = 0x04;
    private const uint FILE_MAP_ALL_ACCESS = 0x000F001F;
    private const uint MEM_COMMIT = 0x1000;
    private const uint MEM_RESERVE = 0x2000;
    private const uint MEM_RELEASE = 0x8000;
    private const uint PROCESS_CREATE_THREAD = 0x0002;
    private const uint PROCESS_VM_OPERATION = 0x0008;
    private const uint PROCESS_VM_READ = 0x0010;
    private const uint PROCESS_VM_WRITE = 0x0020;
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint LIST_MODULES_ALL = 0x03;
    private const uint WAIT_OBJECT_0 = 0x00000000;
    private const uint DONT_RESOLVE_DLL_REFERENCES = 0x00000001;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateFileMappingW(IntPtr hFile, IntPtr attributes,
                                                    uint protect, uint maxHigh, uint maxLow,
                                                    string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr MapViewOfFile(IntPtr mapping, uint access,
                                               uint offsetHigh, uint offsetLow, UIntPtr bytes);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool UnmapViewOfFile(IntPtr address);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address,
                                                UIntPtr size, uint allocationType, uint protect);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualFreeEx(IntPtr process, IntPtr address,
                                             UIntPtr size, uint freeType);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(IntPtr process, IntPtr address,
                                                  byte[] buffer, UIntPtr size, out UIntPtr written);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateRemoteThread(IntPtr process, IntPtr attributes,
                                                    UIntPtr stackSize, IntPtr startAddress,
                                                    IntPtr parameter, uint flags, IntPtr threadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, int milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    // PROCESS_VM_READ is requested for exactly these calls and nothing else:
    // module enumeration is how we learn where the DLL landed in the target,
    // since the LoadLibraryW exit code cannot carry an x64 base. It is safe
    // with respect to ShieldCord's own memory monitor — that terminates
    // processes holding a VM_READ handle on a PROTECTED APP (Discord,
    // browsers), and its IsSafeToKill refuses anything running out of
    // %WINDIR%, so explorer is never a candidate.
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool K32EnumProcessModulesEx(IntPtr process, [Out] IntPtr[] modules,
                                                       uint size, out uint needed, uint filter);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern uint K32GetModuleBaseNameW(IntPtr process, IntPtr module,
                                                     [Out] char[] name, uint size);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags,
                                                          System.Text.StringBuilder name,
                                                          ref uint size);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ProcessIdToSessionId(uint pid, out uint sessionId);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandleW(string moduleName);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibraryExW(string fileName, IntPtr file, uint flags);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeLibrary(IntPtr module);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern IntPtr GetProcAddress(IntPtr module, string procName);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetShellWindow();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
}
