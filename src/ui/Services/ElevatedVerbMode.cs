using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json.Nodes;
using ShieldCordUI.Protocol;

namespace ShieldCordUI.Services;

/// <summary>
/// The "one-shot privileged helper" side of <see cref="ElevatedHelper"/>.
///
/// When the UI relaunches itself with "runas", the child process lands here
/// before any window is created: it reads the verb from a temp file, forwards
/// it to the engine over the pipe, writes the reply next to it, and exits.
/// No window, no tray icon, no second copy of the app on screen.
/// </summary>
public static class ElevatedVerbMode
{
    /// <summary>True when this process was started only to run one verb.</summary>
    public static bool IsRequested(string[] args) =>
        args.Length > 0 && args[0] == ElevatedHelper.VerbSwitch;

    /// <summary>Reads the request/reply paths out of the command line.</summary>
    public static bool TryParse(string[] args, out string requestPath, out string replyPath)
    {
        requestPath = "";
        replyPath   = "";

        for (int i = 0; i < args.Length - 1; i++)
        {
            if (args[i] == ElevatedHelper.VerbSwitch) requestPath = args[i + 1];
            else if (args[i] == ElevatedHelper.OutSwitch) replyPath = args[i + 1];
        }

        return requestPath.Length > 0 && replyPath.Length > 0;
    }

    /// <summary>
    /// Forwards the verb and writes the reply. Always writes a file — even an
    /// empty one — because the parent is blocking on that file appearing, and
    /// a missing file is indistinguishable from a hung child.
    /// </summary>
    public static async Task RunAsync(string requestPath, string replyPath)
    {
        string reply = "";
        try
        {
            string request = await File.ReadAllTextAsync(requestPath);

            // A UI-local action runs entirely in this child and never opens the
            // pipe, so it MUST be recognised before the client is created.
            // Starting the engine is only ever needed when the engine is down;
            // waiting on a pipe that cannot answer would burn the full timeout
            // and then report a failure that never happened.
            string? type = RequestType(request);

            if (type == ServiceControl.StartRequestToken)
            {
                reply = await StartServiceReplyAsync();
            }
            else if (type == DriverSetup.InstallRequestToken)
            {
                // Same reasoning as starting the engine: the whole point of
                // installing the driver is that it is not there yet, so this
                // cannot depend on the engine answering.
                reply = await InstallDriverReplyAsync();
            }
            else
            {
                await using var ipc = new IpcClient();
                ipc.Start();

                // The engine is a service and should already be up, but a cold
                // boot race is possible — give it a moment rather than fail the
                // action.
                if (await ipc.WaitForConnectionAsync(TimeSpan.FromSeconds(8)))
                    reply = await ipc.SendRawAsync(request) ?? "";
            }
        }
        catch
        {
            // Fall through and write whatever we have; the parent turns an
            // empty reply into a neutral "did not complete" message.
        }

        try
        {
            await File.WriteAllTextAsync(replyPath, reply, new UTF8Encoding(false));
        }
        catch
        {
            // Nothing more we can do — the parent will time out and report it.
        }
    }

    private static string? RequestType(string requestJson)
    {
        try { return (JsonNode.Parse(requestJson) as JsonObject)?["type"]?.GetValue<string>(); }
        catch (Exception) { return null; }
    }

    /// <summary>
    /// Start the engine from this elevated child and describe the outcome.
    ///
    /// The reply is its own shape rather than the engine's "error" envelope: the
    /// engine was never involved, and rendering this through the engine's error
    /// path would blame a process that was not running.
    /// </summary>
    private static async Task<string> StartServiceReplyAsync()
    {
        string? failure = await ServiceControl.StartAsync();

        var reply = new JsonObject();

        if (failure is null) reply["type"] = "ok";
        else
        {
            reply["type"]    = ServiceControl.FailureReplyType;
            reply["message"] = failure;
        }

        return reply.ToJsonString();
    }

    // ── driver installation ──────────────────────────────────

    /// <summary>
    /// Install the kernel minifilter, turning test signing on first if it is off
    /// and the machine will allow it.
    ///
    /// This runs in the elevated child because every step needs administrator:
    /// reading and writing the boot configuration, copying the driver into
    /// System32\drivers, adding the test certificate to the machine stores, and
    /// registering the filter service.
    ///
    /// The report it writes carries only facts it verified. In particular
    /// <c>driver_loaded</c> comes from the SCM after the attempt, NOT from the
    /// installer's exit code: a driver can register perfectly and still fail to
    /// load, which is precisely what happens when test signing was off and the
    /// restart has not happened yet. Reporting that as success is how the
    /// installer has been lying about this machine's protection all along.
    /// </summary>
    private static async Task<string> InstallDriverReplyAsync()
    {
        var reply = new JsonObject();

        // ── 0. can this job be done at all? ──────────────────
        /*
         * FIRST, before anything on this machine is modified.
         *
         * This check used to sit after the test-signing step, which meant a
         * machine without the installer payload would have test signing switched
         * ON — a security downgrade for the whole PC, needing a restart — and
         * only then discover it could not install the driver and give up. The
         * user would be left less secure than they started, for nothing.
         *
         * Establish that the job is completable before changing anything.
         */
        string? setupExe = DriverSetup.LocateSetupExe();
        if (setupExe is null)
        {
            reply["type"]    = ServiceControl.FailureReplyType;
            reply["message"] =
                "shieldcord_driver_setup.exe is missing from the ShieldCord install "
                + "folder, so the driver could not be installed. Nothing on this PC "
                + "was changed. Re-run the ShieldCord installer.";
            return reply.ToJsonString();
        }

        // ── Secure Boot blocks test signing outright ──────────
        // Checked here as well as in the dialog: the dialog reads it unelevated
        // to warn early, but this is the authoritative check, and bcdedit must
        // not be run blindly on the strength of a registry read.
        if (DriverSetup.SecureBootEnabled)
        {
            reply["type"]                = ServiceControl.FailureReplyType;
            reply["secure_boot_blocked"] = true;
            reply["message"] =
                "This PC has Secure Boot switched on, and Windows refuses to change test "
                + "signing while it is enabled. The ShieldCord driver cannot be loaded on "
                + "this machine until it is signed by Microsoft instead.";
            return reply.ToJsonString();
        }

        // ── 1. test signing ──────────────────────────────────
        bool? wasOn = await ReadTestSigningAsync();

        if (wasOn is null)
        {
            // Could not read it. Enabling it anyway would be making a security
            // change to a machine whose state we cannot see, and the consent the
            // user gave was specifically "only if it is off". Refuse instead.
            reply["type"]    = ServiceControl.FailureReplyType;
            reply["message"] =
                "Test signing could not be read on this PC, so ShieldCord did not change "
                + "it. Open a Command Prompt as Administrator, run "
                + "\"bcdedit /enum {current}\", and check the testsigning line.";
            return reply.ToJsonString();
        }

        bool wasOff = wasOn == false;

        if (wasOff)
        {
            await RunProcessAsync("bcdedit.exe", "/set testsigning on", TimeSpan.FromSeconds(30));
        }

        // Read back rather than trusting the set: on a policy-managed machine
        // the write can be accepted and then not take effect.
        bool nowOn = await ReadTestSigningAsync() == true;

        reply["test_signing_was_off"] = wasOff;
        reply["test_signing_now_on"]  = nowOn;

        // The restart is for the boot-configuration change, and only that. A
        // driver that loads now needs no restart at all.
        bool rebootRequired = wasOff && nowOn;

        // ── 2. register and load the driver ──────────────────
        var (exitCode, output) = await RunProcessAsync(
            setupExe, "--install", TimeSpan.FromMinutes(2));

        // ── 3. ask the system what actually happened ─────────
        var state = DriverSetup.Query();

        bool registered = state is DriverSetup.FilterState.Running
                                or DriverSetup.FilterState.Stopped;
        bool loaded     = state == DriverSetup.FilterState.Running;

        reply["driver_registered"] = registered;
        reply["driver_loaded"]     = loaded;
        reply["reboot_required"]   = rebootRequired && !loaded;

        // ── 4. make the RUNNING engine pick it up ────────────
        // The engine connects to the driver once, at startup, so a driver that
        // appeared afterwards is loaded but UNARMED — verifyable as "connected"
        // while blocking nothing. Ask it to arm now so the user does not have to
        // restart the service as well as the machine.
        if (loaded)
        {
            bool armed = await ReconnectDriverAsync();
            reply["engine_rearmed"] = armed;
        }

        if (loaded)
        {
            reply["type"] = "ok";
            return reply.ToJsonString();
        }

        /*
         * Registered but not running, or not registered at all. Either way
         * protection is NOT active, so this is a failure — and it says which one
         * plus whatever the installer printed, because until now that diagnostic
         * went to a hidden console window and reached nobody.
         */
        reply["type"]    = ServiceControl.FailureReplyType;
        reply["message"] = rebootRequired
            ? "The driver is installed, but Windows will not load it until this PC "
              + "restarts — test signing only takes effect at boot."
            : (!registered
                ? "The driver could not be installed. " + Summarize(output)
                : "The driver is installed but Windows refused to load it. "
                  + Summarize(output));
        return reply.ToJsonString();
    }

    /// <summary>
    /// Ask the running engine to re-load and re-arm the driver.
    ///
    /// Best-effort by design: the install has already succeeded by the time this
    /// is called, so a failure here means only that the engine has not noticed
    /// yet. It is reported as its own flag rather than failing the whole action,
    /// and the engine picks the driver up at its next start regardless.
    /// </summary>
    private static async Task<bool> ReconnectDriverAsync()
    {
        try
        {
            await using var ipc = new IpcClient();
            ipc.Start();

            if (!await ipc.WaitForConnectionAsync(TimeSpan.FromSeconds(8))) return false;

            var request = new JsonObject { ["type"] = Verbs.ReconnectDriver };

            // Read the reply's driver_connected rather than treating any answer
            // as success: the verb reports whether the driver actually came up.
            string? raw = await ipc.SendRawAsync(request.ToJsonString());
            if (string.IsNullOrWhiteSpace(raw)) return false;

            return (JsonNode.Parse(raw) as JsonObject)?["driver_connected"]?.GetValue<bool>() == true;
        }
        catch
        {
            return false;
        }
    }

    // ── process / bcdedit helpers ────────────────────────────

    /// <summary>
    /// Runs a child process hidden and returns its exit code and combined output.
    /// Never throws: a failure to launch is reported as exit code -1.
    /// </summary>
    private static async Task<(int ExitCode, string Output)> RunProcessAsync(
        string exe, string args, TimeSpan timeout)
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName               = exe,
                Arguments              = args,
                UseShellExecute        = false,   // required for stream redirection
                CreateNoWindow         = true,
                RedirectStandardOutput = true,
                RedirectStandardError  = true,
            };

            using var proc = Process.Start(psi);
            if (proc is null) return (-1, "");

            // Read both streams to completion BEFORE waiting. A child that fills
            // a pipe buffer while we are blocked on WaitForExit would deadlock.
            Task<string> stdout = proc.StandardOutput.ReadToEndAsync();
            Task<string> stderr = proc.StandardError.ReadToEndAsync();

            using var cts = new CancellationTokenSource(timeout);
            try
            {
                await proc.WaitForExitAsync(cts.Token);
            }
            catch (OperationCanceledException)
            {
                try { proc.Kill(entireProcessTree: true); } catch { }
                return (-1, "the install timed out");
            }

            return (proc.ExitCode, (await stdout) + (await stderr));
        }
        catch
        {
            return (-1, "");
        }
    }

    /// <summary>
    /// Read the testsigning boot setting. Null means it could not be determined.
    ///
    /// bcdedit is the only supported way to read this, and it requires
    /// administrator — which is why this lives in the elevated child and not in
    /// the tray. Same parsing approach as sci::IsTestSigningEnabled in
    /// src/installer/installer_common.h, so the app and the installer agree
    /// about what they are looking at.
    ///
    /// Caveat inherited from bcdedit: its output is localised. The element
    /// keyword `testsigning` is not translated, so finding the line is
    /// reliable — only the value word is. That is what the null return guards:
    /// the line is present and said neither Yes nor No.
    ///
    /// ABSENT MEANS OFF, NOT UNKNOWN. bcdedit prints only the BCD elements that
    /// are set, so a machine that has never had test signing on — every fresh
    /// machine, which is exactly what this flow runs on — prints no
    /// `testsigning` line at all. Reading that absence as "unknown" made the
    /// caller refuse before it had attempted anything: setup spent a UAC prompt
    /// and changed nothing, with no explanation anywhere. The installer's
    /// sci::IsTestSigningEnabled already defaults to false in this case, so the
    /// two had also silently disagreed.
    /// </summary>
    private static async Task<bool?> ReadTestSigningAsync()
    {
        var (exitCode, output) = await RunProcessAsync(
            "bcdedit.exe", "/enum \"{current}\"", TimeSpan.FromSeconds(30));

        // bcdedit did not run to completion, or printed nothing at all. That is
        // a genuine "cannot tell", and a different thing from a store we read
        // successfully that simply has no testsigning element in it.
        if (exitCode != 0 || string.IsNullOrWhiteSpace(output)) return null;

        foreach (string line in output.Split('\n'))
        {
            if (!line.Contains("testsigning", StringComparison.OrdinalIgnoreCase)) continue;

            // The line is "<name>   Yes" / "<name>   No".
            if (line.Contains("Yes", StringComparison.OrdinalIgnoreCase)) return true;
            if (line.Contains("No",  StringComparison.OrdinalIgnoreCase)) return false;

            // Found the element but could not read its value — a localised
            // value word. This is what the caller refuses on.
            return null;
        }

        // The store read cleanly and has no testsigning element, so it was
        // never configured. Unset is off.
        return false;
    }

    /// <summary>Keep the installer's own diagnosis short enough for a dialog.</summary>
    private static string Summarize(string output)
    {
        if (string.IsNullOrWhiteSpace(output)) return "It did not say why.";

        string[] lines = output
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Where(l => l.StartsWith("[-]") || l.StartsWith("[!]"))
            .ToArray();

        return lines.Length > 0
            ? string.Join(" ", lines.Take(3))
            : "It did not say why.";
    }
}
