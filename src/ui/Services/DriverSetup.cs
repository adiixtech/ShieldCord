using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Win32;

namespace ShieldCordUI.Services;

/// <summary>
/// The kernel minifilter: is it installed, is it loaded, and what it takes to
/// get it there.
///
/// The driver cannot be shipped ready to run. It is signed with ShieldCord's own
/// test certificate, and Windows refuses to load a kernel driver that is not
/// attestation- or WHQL-signed unless the machine is in test-signing mode. So
/// "install the driver" is really two steps — turn test signing on, then
/// register and load — and the second one only takes effect after a restart.
///
/// This type owns the UNELEVATED half: what the tray can read about the current
/// state, and how to find the installer binary. The part that changes anything
/// runs in the elevated child — see <see cref="ElevatedVerbMode"/>.
/// </summary>
public static class DriverSetup
{
    /// <summary>
    /// The kernel filter's service name. Must match SC_FILTER_NAME in
    /// src/shared/common.h and SC_FILTER_SERVICE_NAME in src/installer/installer_common.h
    /// — they are C macros, so the value is repeated here.
    /// </summary>
    private const string FilterServiceName = "ShieldCordFilter";

    /// <summary>
    /// The request token the tray uses to ask the elevated child to install the
    /// driver, matching the pattern of <see cref="ServiceControl.StartRequestToken"/>.
    ///
    /// Deliberately NOT a member of Protocol.Verbs and deliberately absent from
    /// ipc_protocol.h: installing a driver is not something the engine can do for
    /// us (the engine is not running yet, or is running without the driver), so
    /// this never crosses the pipe.
    /// </summary>
    public const string InstallRequestToken = "ui.install_driver";

    /// <summary>The installed path, used when the app is not where we expect it.</summary>
    private const string InstalledSetupExe = @"C:\Program Files\ShieldCord\shieldcord_driver_setup.exe";

    /// <summary>What the SCM currently knows about the filter.</summary>
    public enum FilterState
    {
        /// <summary>Loaded and running — this is the state protection needs.</summary>
        Running,

        /// <summary>
        /// Registered but not running. Usually this means a restart is pending,
        /// because test signing was only just turned on.
        /// </summary>
        Stopped,

        /// <summary>No such service — setup has not been done on this machine.</summary>
        NotInstalled,

        /// <summary>The query itself failed. Say so rather than guess a state.</summary>
        Unknown,
    }

    /// <summary>
    /// Read the filter's state from the SCM.
    ///
    /// Safe unelevated: the service is created with the SCM's default security
    /// descriptor, which grants interactive users query access. This is the
    /// signal the dashboard uses to offer setup at all, so it must work before
    /// anything has been installed — which it does, because a missing service
    /// answers with ERROR_SERVICE_DOES_NOT_EXIST rather than an access denial.
    /// </summary>
    public static FilterState Query()
    {
        IntPtr scm = ServiceControl.OpenSCManagerW(null, null, ServiceControl.SC_MANAGER_CONNECT);
        if (scm == IntPtr.Zero) return FilterState.Unknown;

        try
        {
            IntPtr svc = ServiceControl.OpenServiceW(scm, FilterServiceName,
                                                     ServiceControl.SERVICE_QUERY_STATUS);
            if (svc == IntPtr.Zero)
            {
                return Marshal.GetLastWin32Error() == ServiceControl.ERROR_SERVICE_DOES_NOT_EXIST
                    ? FilterState.NotInstalled
                    : FilterState.Unknown;
            }

            try
            {
                if (!ServiceControl.QueryServiceStatus(svc, out var status))
                    return FilterState.Unknown;

                return status.dwCurrentState switch
                {
                    ServiceControl.SERVICE_RUNNING       => FilterState.Running,
                    ServiceControl.SERVICE_STOPPED       => FilterState.Stopped,
                    ServiceControl.SERVICE_START_PENDING => FilterState.Stopped,
                    ServiceControl.SERVICE_STOP_PENDING  => FilterState.Stopped,
                    _                                    => FilterState.Unknown,
                };
            }
            finally { ServiceControl.CloseServiceHandle(svc); }
        }
        finally { ServiceControl.CloseServiceHandle(scm); }
    }

    /// <summary>
    /// Whether UEFI Secure Boot is on.
    ///
    /// This matters before anything else happens: Secure Boot protects the boot
    /// configuration, so `bcdedit /set testsigning on` is REFUSED outright on a
    /// machine with it enabled. There is no in-app way around that — it is a
    /// firmware-level policy — so the setup dialog reads this first and stops
    /// with an explanation rather than spending a UAC prompt on a command
    /// Windows is certain to reject.
    ///
    /// Readable unelevated, which is the point: the warning appears before the
    /// prompt. An absent key means legacy BIOS or an older build, where Secure
    /// Boot does not exist and test signing is therefore not blocked.
    /// </summary>
    public static bool SecureBootEnabled
    {
        get
        {
            try
            {
                using var key = Registry.LocalMachine.OpenSubKey(
                    @"SYSTEM\CurrentControlSet\Control\SecureBoot\State");
                return key?.GetValue("UEFISecureBootEnabled") is int v && v != 0;
            }
            catch
            {
                // Unreadable is not evidence of enabled. Reporting a Secure Boot
                // block we cannot substantiate would stop setup on machines where
                // it would have worked.
                return false;
            }
        }
    }

    /// <summary>
    /// Whether Memory Integrity (HVCI) is on.
    ///
    /// THIS IS THE BLOCKER THAT DOES NOT ANNOUNCE ITSELF.
    ///
    /// Test signing bypasses Driver Signature Enforcement, but it does NOT bypass
    /// HVCI's own signature checks in ci.dll. With Memory Integrity on, Windows
    /// refuses the driver even in Test Mode — so the user enables test signing,
    /// restarts, and nothing loads, with no message anywhere saying why. Without
    /// this check the likeliest failure in the whole setup is invisible.
    ///
    /// Readable unelevated, which is the point: it must gate the flow BEFORE a UAC
    /// prompt is spent on an install that cannot succeed.
    ///
    /// Absent key means the feature has never been configured, which is off.
    /// </summary>
    public static bool MemoryIntegrityEnabled
    {
        get
        {
            try
            {
                using var key = Registry.LocalMachine.OpenSubKey(
                    @"SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity");
                return key?.GetValue("Enabled") is int v && v != 0;
            }
            catch
            {
                // Unreadable is not evidence of enabled — reporting a block we
                // cannot substantiate would stop setup on machines where it works.
                return false;
            }
        }
    }

    /// <summary>
    /// Opens Windows Security at the Core Isolation page, where Memory Integrity
    /// is switched off.
    ///
    /// The supported route on purpose. Memory Integrity can also be flipped by
    /// writing the same registry value this class reads, but that is a security
    /// setting the user should see and change in Windows' own UI — and a value we
    /// wrote can be overridden by policy or reverted, which would leave them
    /// believing it is off when it is not.
    ///
    /// Returns false when Windows does not take the URI, so the caller can fall
    /// back to written instructions instead of a button that does nothing.
    /// </summary>
    public static bool OpenMemoryIntegritySettings()
    {
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName        = "windowsdefender://coreisolation",
                UseShellExecute = true,
            });
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Find shieldcord_driver_setup.exe.
    ///
    /// The installed layout is tried first: the app lives in {app}\ui\ and the
    /// engine binaries in {app}\, so it is one level up. The other two
    /// candidates exist so the same binary works in the layouts this project is
    /// actually tested in — a dev tree, and the VM shared folder where the
    /// payload sits under service\. Mirrors what <c>FindPayload</c> does
    /// engine-side for the driver payload itself.
    /// </summary>
    public static string? LocateSetupExe()
    {
        try
        {
            string? exe = Environment.ProcessPath;
            if (!string.IsNullOrEmpty(exe))
            {
                string? dir = Path.GetDirectoryName(exe);
                if (!string.IsNullOrEmpty(dir))
                {
                    string[] candidates =
                    {
                        Path.Combine(dir, "..", "shieldcord_driver_setup.exe"),
                        Path.Combine(dir, "..", "service", "shieldcord_driver_setup.exe"),
                        Path.Combine(dir, "shieldcord_driver_setup.exe"),
                    };

                    foreach (string candidate in candidates)
                    {
                        string full = Path.GetFullPath(candidate);
                        if (File.Exists(full)) return full;
                    }
                }
            }
        }
        catch
        {
            // Fall through to the installed path.
        }

        return File.Exists(InstalledSetupExe) ? InstalledSetupExe : null;
    }

    /// <summary>
    /// Parse the elevated child's report. Returns null only when there was no
    /// reply, or it was not JSON — which the caller must treat as "the outcome
    /// is unknown", never as success.
    ///
    /// A report can come back as <c>ui_error</c> and still carry detail (a
    /// driver that registered but would not load, for example), so this does not
    /// filter on the type. Read <see cref="DriverSetupReport.Type"/> and
    /// <see cref="DriverSetupReport.DriverLoaded"/> to decide what to say.
    /// </summary>
    public static DriverSetupReport? ParseReport(string? replyJson)
    {
        if (string.IsNullOrWhiteSpace(replyJson)) return null;

        try
        {
            return JsonSerializer.Deserialize<DriverSetupReport>(replyJson);
        }
        catch (JsonException)
        {
            return null;
        }
    }
}

/// <summary>
/// What the elevated child actually did, as it reported it.
///
/// Every field is a fact the child verified, not one the parent inferred: the
/// driver's state comes from the SCM after the install attempt, and test signing
/// is read back after any change. The parent must not derive "loaded" from the
/// installer's exit code — a driver can register successfully and still not load.
/// </summary>
public sealed record DriverSetupReport
{
    [JsonPropertyName("type")]               public string Type { get; init; } = "";

    /// <summary>Test signing was off when setup started.</summary>
    [JsonPropertyName("test_signing_was_off")] public bool TestSigningWasOff { get; init; }

    /// <summary>Test signing is on now — read back after any change.</summary>
    [JsonPropertyName("test_signing_now_on")]  public bool TestSigningNowOn { get; init; }

    /// <summary>The filter service exists in the SCM.</summary>
    [JsonPropertyName("driver_registered")]    public bool DriverRegistered { get; init; }

    /// <summary>The filter reached RUNNING. This, not the exit code, is what
    /// proves the driver loaded.</summary>
    [JsonPropertyName("driver_loaded")]        public bool DriverLoaded { get; init; }

    /// <summary>
    /// The running engine connected to the driver and armed it.
    ///
    /// Loaded is NOT protected: the minifilter stays fail-open until an engine
    /// connects and pushes trust, the path list and enforcement. So this, not
    /// <see cref="DriverLoaded"/>, is what says tokens are actually being
    /// defended — and a driver that is loaded but unarmed must never be
    /// reported to the user as protection.
    /// </summary>
    [JsonPropertyName("engine_rearmed")]       public bool EngineRearmed { get; init; }

    /// <summary>
    /// A restart is required for the change to take effect. Only ever true when
    /// test signing was just enabled — that is the one thing a restart is
    /// definitely needed for.
    /// </summary>
    [JsonPropertyName("reboot_required")]      public bool RebootRequired { get; init; }

    /// <summary>Set when Secure Boot refused the change, so the dialog can
    /// explain the real reason instead of showing an exit code.</summary>
    [JsonPropertyName("secure_boot_blocked")]  public bool SecureBootBlocked { get; init; }

    /// <summary>A sentence to show the user, when something needs explaining.</summary>
    [JsonPropertyName("message")]              public string? Message { get; init; }
}
