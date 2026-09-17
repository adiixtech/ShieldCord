using System.Runtime.InteropServices;

namespace ShieldCordUI.Services;

/// <summary>
/// Talking to the Windows Service Control Manager about the ShieldCord engine.
///
/// This exists because starting the engine cannot be an IPC verb. Every other
/// privileged action travels over the engine's named pipe — but the whole point
/// of starting the engine is that it is not running, so there is no pipe to
/// send anything down. The tray asks the SCM directly instead.
///
/// Almost all of it only works from the ELEVATED helper child, never from the
/// tray: the service is created with the SCM's default security descriptor,
/// which grants interactive users query and read access but NOT SERVICE_START.
/// <see cref="Query"/> is the exception — SERVICE_QUERY_STATUS is granted, so
/// the unelevated tray can read the state to label its button.
///
/// Raw advapi32 P/Invoke rather than System.ServiceProcess.ServiceController:
/// that type is not part of the in-box Windows Desktop runtime, and this
/// project deliberately carries zero NuGet dependencies.
/// </summary>
public static class ServiceControl
{
    /// <summary>
    /// The installed service name. Must match SC_SERVICE_NAME in
    /// src/shared/common.h — it is a C++ macro, so the value is repeated here.
    /// </summary>
    private const string ServiceName = "ShieldCordSvc";

    /// <summary>
    /// The request token the tray uses to ask the elevated child to start the
    /// engine.
    ///
    /// Deliberately NOT a member of Protocol.Verbs and deliberately absent from
    /// ipc_protocol.h: it never crosses the pipe, so adding it to the contract
    /// mirror would force a matching C++ change for a message the engine will
    /// never receive.
    /// </summary>
    public const string StartRequestToken = "ui.start_service";

    /// <summary>
    /// The reply type the elevated child writes when a UI-local action fails.
    ///
    /// Its own shape rather than the engine's <c>error</c> envelope, because the
    /// engine was never involved — routing this through the engine's error path
    /// would render it as "the engine refused", blaming a process that was not
    /// running.
    /// </summary>
    public const string FailureReplyType = "ui_error";

    // ── win32 ────────────────────────────────────────────────
    //
    // internal, not private: DriverSetup uses the same four calls to read the
    // kernel filter's state. Duplicating the P/Invoke block there would mean two
    // copies of the same SERVICE_STATUS layout, and a marshalling mistake in one
    // of them would read as "the driver is fine" while the other read the truth.

    internal const uint SC_MANAGER_CONNECT   = 0x0001;
    internal const uint SERVICE_QUERY_STATUS = 0x0004;
    internal const uint SERVICE_START        = 0x0010;

    internal const uint SERVICE_STOPPED      = 0x00000001;
    internal const uint SERVICE_START_PENDING = 0x00000002;
    internal const uint SERVICE_STOP_PENDING = 0x00000003;
    internal const uint SERVICE_RUNNING      = 0x00000004;

    internal const int ERROR_SERVICE_ALREADY_RUNNING = 1056;
    internal const int ERROR_SERVICE_DISABLED        = 1058;
    internal const int ERROR_SERVICE_DOES_NOT_EXIST  = 1060;

    /// <summary>What the SCM currently thinks of the engine.</summary>
    public enum EngineState
    {
        /// <summary>Running — the pipe may still be catching up.</summary>
        Running,

        /// <summary>Installed but stopped. This is the state the Turn-on button serves.</summary>
        Stopped,

        /// <summary>Coming up or going down; the truthful label is "in between".</summary>
        Pending,

        /// <summary>No such service. Nothing the user can do from here but install.</summary>
        NotInstalled,

        /// <summary>The query itself failed. Say so rather than guess a state.</summary>
        Unknown,
    }

    /// <summary>
    /// Read the engine's state from the SCM. Safe to call unelevated, and safe
    /// to call while the pipe is down — which is the only time it is needed.
    /// </summary>
    public static EngineState Query()
    {
        IntPtr scm = OpenSCManagerW(null, null, SC_MANAGER_CONNECT);
        if (scm == IntPtr.Zero) return EngineState.Unknown;

        try
        {
            IntPtr svc = OpenServiceW(scm, ServiceName, SERVICE_QUERY_STATUS);
            if (svc == IntPtr.Zero)
            {
                return Marshal.GetLastWin32Error() == ERROR_SERVICE_DOES_NOT_EXIST
                    ? EngineState.NotInstalled
                    : EngineState.Unknown;
            }

            try
            {
                if (!QueryServiceStatus(svc, out SERVICE_STATUS status))
                    return EngineState.Unknown;

                return status.dwCurrentState switch
                {
                    SERVICE_RUNNING       => EngineState.Running,
                    SERVICE_STOPPED       => EngineState.Stopped,
                    SERVICE_START_PENDING => EngineState.Pending,
                    SERVICE_STOP_PENDING  => EngineState.Pending,
                    _                     => EngineState.Unknown,
                };
            }
            finally { CloseServiceHandle(svc); }
        }
        finally { CloseServiceHandle(scm); }
    }

    /// <summary>
    /// Ask the SCM to start the engine.
    ///
    /// Returns null on success, or a sentence the user can act on. Note that
    /// this returns as soon as the SCM ACCEPTS the start — the service reaches
    /// RUNNING later, so callers must not report "connected" from here. The
    /// pipe client reconnecting is what proves the engine is actually up.
    /// </summary>
    public static async Task<string?> StartAsync() => await Task.Run(TryStart);

    private static string? TryStart()
    {
        IntPtr scm = OpenSCManagerW(null, null, SC_MANAGER_CONNECT);
        if (scm == IntPtr.Zero)
            return Fail("open the service manager");

        try
        {
            IntPtr svc = OpenServiceW(scm, ServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
            if (svc == IntPtr.Zero)
            {
                int err = Marshal.GetLastWin32Error();
                return err == ERROR_SERVICE_DOES_NOT_EXIST
                    ? "ShieldCord's service is not installed, so there is nothing to start. "
                      + "Run the installer to restore it."
                    : Fail("open the ShieldCord service", err);
            }

            try
            {
                // Already running is the outcome the user asked for, so it is a
                // success — the SCM just got there first.
                if (StartServiceW(svc, 0, IntPtr.Zero)) return null;

                int err = Marshal.GetLastWin32Error();
                return err switch
                {
                    ERROR_SERVICE_ALREADY_RUNNING => null,
                    ERROR_SERVICE_DISABLED =>
                        "The ShieldCord service is disabled, so it cannot be started. "
                        + "Re-run the installer to restore it.",
                    ERROR_SERVICE_DOES_NOT_EXIST =>
                        "ShieldCord's service is not installed, so there is nothing to start. "
                        + "Run the installer to restore it.",
                    _ => Fail("start the ShieldCord service", err),
                };
            }
            finally { CloseServiceHandle(svc); }
        }
        finally { CloseServiceHandle(scm); }
    }

    private static string Fail(string what, int? code = null) =>
        $"Could not {what} (Windows error {code ?? Marshal.GetLastWin32Error()}).";

    // ── win32 ────────────────────────────────────────────────

    [StructLayout(LayoutKind.Sequential)]
    internal struct SERVICE_STATUS
    {
        public uint dwServiceType;
        public uint dwCurrentState;
        public uint dwControlsAccepted;
        public uint dwWin32ExitCode;
        public uint dwServiceSpecificExitCode;
        public uint dwCheckPoint;
        public uint dwWaitHint;
    }

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    internal static extern IntPtr OpenSCManagerW(string? machineName, string? databaseName, uint access);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    internal static extern IntPtr OpenServiceW(IntPtr scManager, string serviceName, uint access);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool StartServiceW(IntPtr service, uint numArgs, IntPtr args);

    [DllImport("advapi32.dll", SetLastError = true)]
    internal static extern bool QueryServiceStatus(IntPtr service, out SERVICE_STATUS status);

    [DllImport("advapi32.dll", SetLastError = true)]
    internal static extern bool CloseServiceHandle(IntPtr handle);
}
