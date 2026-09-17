// ============================================================
// ShieldCord — Protocol.cs
//
// THE IPC CONTRACT, C# half. Mirror of src/shared/ipc_protocol.h.
//
// If you change ANYTHING here, change the C++ header to match.
// The two are kept in sync by hand; there is no code generator.
//
// See the C++ header for the full transport / correlation /
// authorization notes. The short version:
//   * newline-delimited JSON over \\.\pipe\ShieldCord
//   * requests carry "id", replies echo it, `alert` broadcasts do not
//   * state-changing verbs require an elevated caller process
// ============================================================

using System.Text.Json.Serialization;

namespace ShieldCordUI.Protocol;

/// <summary>Request verbs (client -> server).</summary>
public static class Verbs
{
    // Read — no elevation required.
    public const string GetStatus         = "get_status";
    public const string GetConfig         = "get_config";
    public const string GetAlerts         = "get_alerts";
    public const string GetProtectedApps  = "get_protected_apps";
    public const string GetProtectedPaths = "get_protected_paths";

    // Write — elevation required.
    public const string SetEnforcement    = "set_enforcement";
    public const string SetFeatures       = "set_features";
    public const string SetDecoy          = "set_decoy";
    public const string SetMemoryMonitor  = "set_memory_monitor";
    public const string KillProcess       = "kill_process";
    public const string TrustPublisher    = "trust_publisher";
    public const string UntrustPublisher  = "untrust_publisher";
    public const string Shutdown          = "shutdown";

    /// <summary>
    /// Re-load and re-arm the kernel minifilter. Sent after the driver has been
    /// installed while the engine was already running — the engine only brings
    /// the driver up once, at startup, so without this the driver would sit
    /// loaded and unarmed.
    /// </summary>
    public const string ReconnectDriver   = "reconnect_driver";
}

/// <summary>Reply / event types (server -> client).</summary>
public static class Types
{
    public const string StatusUpdate    = "status_update";
    public const string Config          = "config";
    public const string Alerts          = "alerts";
    public const string ProtectedApps   = "protected_apps";
    public const string ProtectedPaths  = "protected_paths";
    public const string KillResult      = "kill_result";
    public const string Ok              = "ok";
    public const string Error           = "error";
    public const string Alert           = "alert";

    /// <summary>Any reply that answers a request — as opposed to an unsolicited event.</summary>
    public static bool IsReply(string type) => type != Alert;
}

/// <summary>Error codes (the "error" field of a type=error reply).</summary>
public static class Errors
{
    public const string NotAuthorized = "not_authorized";
    public const string UnknownType   = "unknown_type";
    public const string ParseError    = "parse_error";
    public const string BadRequest    = "bad_request";

    /// <summary>
    /// Not a wire code. The engine reports a failed termination as
    /// <c>kill_result</c> with <c>success: false</c> — a successful REQUEST whose
    /// OUTCOME failed — so the client synthesises this code to carry that
    /// through the same error path.
    /// </summary>
    public const string KillFailed    = "kill_failed";
}

public static class Severity
{
    public const string Info    = "info";
    public const string Warning = "warning";
    public const string High    = "high";
}

/// <summary>Where an alert came from.</summary>
public static class Source
{
    public const string Driver = "driver";
    public const string Memory = "memory";
}

public static class Actions
{
    public const string Blocked      = "blocked";
    public const string Killed       = "killed";
    public const string KillFailed   = "kill_failed";
    public const string DetectedOnly = "detected_only";
}

public static class Modes
{
    public const string Service = "service";
    public const string Console = "console";
}

// ============================================================
// Wire models
//
// Every property is explicitly named: the protocol is snake_case
// and the C# convention is PascalCase, so nothing may be inferred.
// ============================================================

/// <summary>Reply to <see cref="Verbs.GetStatus"/>. Every field is defensive —
/// a reply from an older engine may omit some.</summary>
public sealed record StatusSnapshot
{
    [JsonPropertyName("driver_connected")]     public bool   DriverConnected     { get; init; }
    [JsonPropertyName("driver_enforcement")]   public bool   DriverEnforcement   { get; init; }
    [JsonPropertyName("driver_file_block")]    public bool   DriverFileBlock     { get; init; }
    [JsonPropertyName("driver_alerts")]        public bool   DriverAlerts        { get; init; }
    [JsonPropertyName("memory_monitor_active")]public bool   MemoryMonitorActive { get; init; }
    [JsonPropertyName("decoy_enabled")]        public bool   DecoyEnabled        { get; init; }
    [JsonPropertyName("decoy_folder_path")]    public string DecoyFolderPath     { get; init; } = "";
    [JsonPropertyName("uptime_seconds")]       public long   UptimeSeconds       { get; init; }
    [JsonPropertyName("threats_blocked")]      public int    ThreatsBlocked      { get; init; }
    [JsonPropertyName("trusted_process_count")]public int    TrustedProcessCount { get; init; }
    [JsonPropertyName("version")]              public string Version             { get; init; } = "";
    [JsonPropertyName("service_mode")]         public string ServiceMode         { get; init; } = Modes.Service;

    /// <summary>True when the engine considers tokens actively protected: the driver
    /// is connected AND enforcement is armed. Everything else is degraded.</summary>
    [JsonIgnore]
    public bool IsProtecting => DriverConnected && DriverEnforcement && DriverFileBlock;
}

/// <summary>Reply to <see cref="Verbs.GetConfig"/>.</summary>
public sealed record ShieldCordConfig
{
    [JsonPropertyName("enforcement")]          public bool   Enforcement       { get; init; }
    [JsonPropertyName("file_block")]           public bool   FileBlock         { get; init; }
    [JsonPropertyName("alerts")]               public bool   Alerts            { get; init; }
    [JsonPropertyName("decoy_folder")]         public bool   DecoyFolder       { get; init; }
    [JsonPropertyName("memory_monitor_active")]public bool   MemoryMonitor     { get; init; }
    [JsonPropertyName("decoy_folder_path")]    public string DecoyFolderPath   { get; init; } = "";
}

/// <summary>One blocked (or killed) event. Used both for the <c>alert</c>
/// broadcast and for the <see cref="Verbs.GetAlerts"/> history reply.</summary>
public sealed record AlertRecord
{
    /// <summary>Monotonic record id. NOTE: on an unsolicited <c>alert</c> broadcast
    /// this is the record id, NOT a request-correlation echo.</summary>
    [JsonPropertyName("id")]           public long   Id          { get; init; }
    [JsonPropertyName("time")]         public string Time        { get; init; } = "";
    [JsonPropertyName("severity")]     public string Severity    { get; init; } = Protocol.Severity.High;
    [JsonPropertyName("source")]       public string Source      { get; init; } = Protocol.Source.Driver;
    // (Severity/Source are qualified because the property names shadow the types.)
    [JsonPropertyName("message")]      public string Message     { get; init; } = "";
    [JsonPropertyName("pid")]          public int    Pid         { get; init; }
    [JsonPropertyName("process_name")] public string ProcessName { get; init; } = "";
    [JsonPropertyName("publisher")]    public string Publisher   { get; init; } = "";
    [JsonPropertyName("path")]         public string Path        { get; init; } = "";
    [JsonPropertyName("target")]       public string Target      { get; init; } = "";
    [JsonPropertyName("action")]       public string Action      { get; init; } = Protocol.Actions.Blocked;

    /// <summary>When the offending process is still identifiable we can offer a
    /// "trust this publisher" action; without a publisher there is nothing to trust.</summary>
    [JsonIgnore]
    public bool CanTrustPublisher => !string.IsNullOrWhiteSpace(Publisher);
}

/// <summary>Element of the <see cref="Verbs.GetProtectedApps"/> reply.</summary>
public sealed record ProtectedApp
{
    [JsonPropertyName("process_name")] public string ProcessName { get; init; } = "";
    [JsonPropertyName("publisher")]    public string Publisher   { get; init; } = "";
    [JsonPropertyName("app_tag")]      public string AppTag      { get; init; } = "";
    /// <summary>True for entries from the built-in whitelist; false for publishers
    /// the user added at runtime via <see cref="Verbs.TrustPublisher"/>.</summary>
    [JsonPropertyName("builtin")]      public bool   Builtin     { get; init; } = true;
    /// <summary>Whether a process matching this entry is running right now, and
    /// whether the driver has been told to trust it.</summary>
    [JsonPropertyName("running")]      public bool   Running     { get; init; }
    [JsonPropertyName("trusted")]      public bool   Trusted     { get; init; }
}

/// <summary>Element of the <see cref="Verbs.GetProtectedPaths"/> reply.</summary>
public sealed record ProtectedPath
{
    [JsonPropertyName("suffix")] public string Suffix { get; init; } = "";
    [JsonPropertyName("label")]  public string Label  { get; init; } = "";
}
