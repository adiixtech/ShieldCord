using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace ShieldCordUI.Services;

/// <summary>
/// UI-only preferences, stored separately from the engine's config.json.
///
/// The split matters: config.json is owned by the service and describes what
/// PROTECTION does (enforcement, file blocking, decoy). This file describes
/// what the WINDOW does (theme, tray behaviour, start-with-Windows) and needs
/// no elevation to change, so the UI never has to elevate to remember a theme.
/// </summary>
public sealed class AppSettings
{
    [JsonPropertyName("start_with_windows")]
    public bool StartWithWindows { get; set; } = true;

    /// <summary>"light", "dark" or "system".</summary>
    [JsonPropertyName("theme")]
    public string Theme { get; set; } = "system";

    [JsonPropertyName("minimize_to_tray_on_close")]
    public bool MinimizeToTrayOnClose { get; set; } = true;

    /// <summary>
    /// Suppress EVERY alert balloon.
    ///
    /// Defaults ON, so a fresh install is quiet. That is a deliberate trade:
    /// staying silent about a block is a poor way to learn an app is working,
    /// but a security tool that interrupts someone mid-task for a detection they
    /// did not ask about is how a tray icon gets muted wholesale — and then the
    /// one that mattered is missed too.
    ///
    /// This silences the DESKTOP only. Every detection still reaches the
    /// Activity timeline, which is where someone goes to find out what happened
    /// while they were away.
    /// </summary>
    [JsonPropertyName("mute_all_alert_notifications")]
    public bool MuteAllAlertNotifications { get; set; } = true;

    /// <summary>
    /// When the master mute is off, notify only about critical detections — a
    /// block or a kill, something ShieldCord actually did.
    ///
    /// "Critical" is the protocol's <c>high</c> severity. The protocol has no
    /// critical/error level of its own: alerts are <c>info</c>, <c>warning</c>
    /// or <c>high</c>, and <c>info</c> is already kept out of balloons because a
    /// self-healed trust race has nothing worth interrupting anyone for.
    ///
    /// Defaults ON, matching the quiet posture above: turning the master mute
    /// off gives back the half that matters rather than all of it.
    /// </summary>
    [JsonPropertyName("notify_critical_only")]
    public bool NotifyCriticalOnly { get; set; } = true;

    /// <summary>How often the dashboard re-reads status, in milliseconds.</summary>
    [JsonPropertyName("status_poll_ms")]
    public int StatusPollMs { get; set; } = 2000;

    /// <summary>
    /// The user has seen the driver setup prompt and closed it without completing
    /// setup. Stops the app reopening that window on every launch.
    ///
    /// Set only by an explicit dismissal. A machine with no driver and no
    /// dismissal keeps being offered setup, because "installed but unprotected"
    /// is the state this whole flow exists to get people out of — and it is
    /// indistinguishable from a first run.
    /// </summary>
    [JsonPropertyName("setup_prompt_dismissed")]
    public bool SetupPromptDismissed { get; set; }

    /// <summary>
    /// Detections the user removed from the Activity timeline, by record id.
    ///
    /// UI-side suppression only — the engine keeps its own record, and the
    /// "protection events" figure is unaffected. Persisted so a dismissal
    /// survives an app restart; without that, relaunching would re-read the
    /// engine's history and quietly undo every removal.
    ///
    /// Ids restart at 1 with each run of the engine, so the shell clears this
    /// list when it detects a restart rather than let stale ids suppress
    /// unrelated new records.
    /// </summary>
    [JsonPropertyName("dismissed_alert_ids")]
    public List<long> DismissedAlertIds { get; set; } = new();

    /// <summary>
    /// When ShieldCord turned test signing on, in UTC. Null if it never has.
    ///
    /// Setup spans a restart and the app dies in between, so "have we rebooted
    /// since?" cannot be held in memory. Comparing this against the machine's boot
    /// time answers it — and it has to be answered, because "waiting for the
    /// restart" and "restarted and it still failed" look identical from the
    /// outside and need opposite messages.
    ///
    /// A timestamp is an observation, not a UI state: it records a fact about the
    /// machine, which is why storing it is honest where a "setup in progress" flag
    /// would not be.
    /// </summary>
    [JsonPropertyName("test_signing_enabled_at")]
    public DateTime? TestSigningEnabledAt { get; set; }

    /// <summary>
    /// The user has seen the "you're protected" screen and dismissed it.
    ///
    /// Needed because setup finishes on the far side of a restart: the app comes
    /// back up with the driver running and setup legitimately done, and without
    /// this the closing screen would be shown again on every single launch.
    /// </summary>
    [JsonPropertyName("setup_acknowledged")]
    public bool SetupAcknowledged { get; set; }

    /// <summary>
    /// Hide the Windows "Test Mode" desktop watermark.
    ///
    /// Test signing is required for ShieldCord's own driver, and Windows paints
    /// the watermark to say that driver signature enforcement is off. Hiding it
    /// does NOT change that state — it only stops the reminder being drawn — so
    /// this is a window preference rather than a protection setting, which is
    /// why it lives here and needs no elevation.
    ///
    /// Defaults ON: anyone who has enabled test signing has already accepted
    /// the trade, and is the person most likely to want the corner of their
    /// screen back. Applies automatically whenever the watermark is on screen;
    /// turning it off stops ShieldCord applying it, and the watermark returns
    /// on the next shell restart.
    /// </summary>
    [JsonPropertyName("watermark_suppressed")]
    public bool WatermarkSuppressed { get; set; } = true;

    // ── persistence ──────────────────────────────────────────

    private static string FilePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "ShieldCord", "ui.json");

    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    public static AppSettings Load()
    {
        try
        {
            if (File.Exists(FilePath))
            {
                string json = File.ReadAllText(FilePath);
                var loaded = JsonSerializer.Deserialize<AppSettings>(json, Options);
                if (loaded is not null) return loaded.Sanitized();
            }
        }
        catch
        {
            // Corrupt or unreadable — fall back to defaults rather than refuse
            // to start over a preferences file.
        }
        return new AppSettings();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, Options));
        }
        catch
        {
            // Losing a preference is not worth crashing the tray icon over.
        }
    }

    /// <summary>Clamps values that arrive from a hand-edited file.</summary>
    private AppSettings Sanitized()
    {
        if (StatusPollMs < 500)  StatusPollMs = 500;
        if (StatusPollMs > 30000) StatusPollMs = 30000;
        if (Theme is not ("light" or "dark" or "system")) Theme = "system";
        return this;
    }
}
