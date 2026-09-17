using System.Collections.ObjectModel;
using System.Globalization;
using System.Text.Json.Nodes;
using ShieldCordUI.Mvvm;
using ShieldCordUI.Protocol;
using ShieldCordUI.Services;

namespace ShieldCordUI.ViewModels;

/// <summary>One row in the "protection layers" list.</summary>
public sealed record ModuleStatus(string Name, string Detail, bool Healthy);

/// <summary>One count in the breakdown line: what the engine did, and how often
/// over the records this app is holding.</summary>
public sealed record EventKind(string Label, int Count);

/// <summary>One column of the traffic chart.</summary>
public sealed record TrafficBucket(string Label, int Count, double Fraction, bool ShowLabel)
{
    /// <summary>
    /// The complement of <see cref="Fraction"/>.
    ///
    /// Each bar is a two-row Grid and BOTH rows are star-weighted, so the pair
    /// sums to one star and the bar's drawn height is the fraction itself. A
    /// bar whose second row was a bare "*" would instead be fraction/(fraction+1)
    /// tall — which still looks like a plausible chart, and is simply wrong.
    /// </summary>
    public double Remainder => 1.0 - Fraction;
}

/// <summary>
/// The dashboard: live status, what the engine has been doing, and the switches
/// that change it.
///
/// Every switch follows the same contract — flip the local value immediately
/// (so the control feels responsive), push the change through the elevated
/// path, and put the switch BACK if the engine did not accept it. A toggle
/// that silently shows "on" while the driver is still fail-open is worse than
/// no toggle at all.
///
/// The chart and the breakdown follow a matching contract: they are built only
/// from the alert records the UI actually holds, and each states the window it
/// covers. The engine caps its history at 500 records and so does the shell, so
/// there is no honest way to draw "all time" — only "the events still held".
/// </summary>
public sealed class DashboardViewModel : ObservableObject
{
    private readonly ShellViewModel _shell;

    /// <summary>Set while we are writing properties from a server reply, so the
    /// property setters do not mistake it for a user flip and push it back.</summary>
    private bool _suspendPush;

    private bool _enforcement = true;
    private bool _fileBlock   = true;
    private bool _alertsEnabled = true;
    private bool _memoryMonitor = true;
    private bool _decoy         = true;

    private string _statusHeadline = "Checking…";
    private string _statusDetail   = "Reading the engine's state.";
    private string _statusLevel    = "off";
    private long   _protectionEvents;

    /// <summary>
    /// The value currently ON SCREEN, which trails <see cref="_protectionEvents"/>
    /// while the count-up is running. Only <see cref="ProtectionEventsText"/> reads
    /// it — the charts and the "is there anything to show" checks use the real
    /// figure, so a half-way number can never reach a decision.
    /// </summary>
    private double _displayedEvents;

    private readonly CountUp _eventsCountUp;
    private string _uptimeText  = "—";
    private string _versionText = "—";
    private int    _trustedCount;
    private bool   _busy;

    private bool _driverConnected;
    private string _serviceMode = Modes.Service;

    /// <summary>
    /// The kernel filter's state as the SCM reports it, which is the only thing
    /// that can tell "never installed" from "installed but not loaded".
    /// </summary>
    private DriverSetup.FilterState _filterState = DriverSetup.FilterState.Unknown;

    /// <summary>
    /// The last status reply. Kept so the headline can be recomputed when the
    /// SCM's answer about the driver arrives, which is a separate call that may
    /// land before or after the status poll.
    /// </summary>
    private StatusSnapshot? _lastStatus;

    /// <summary>False until a status reply has actually been read, and again
    /// once the engine goes away. Gates every figure that would otherwise have
    /// to be shown as a zero it did not measure.</summary>
    private bool _haveStatus;

    /// <summary>True while the engine is deliberately stopped rather than
    /// unreachable. The switches keep reporting stored configuration, so this
    /// is what stops them reading as live protection.</summary>
    private bool _engineStopped;

    private string _trafficRange = Range24h;

    public const string Range24h = "24h";
    public const string Range7d  = "7d";
    public const string Range30d = "30d";

    public DashboardViewModel(ShellViewModel shell)
    {
        _shell = shell;

        // The display value eases; the authoritative one does not.
        _eventsCountUp = new CountUp(v =>
        {
            _displayedEvents = v;
            OnPropertyChanged(nameof(ProtectionEventsText));
        });

        SetTrafficRangeCommand = new RelayCommand(p => TrafficRange = p as string ?? Range24h);

        // The cards link into Activity, and the shell is the only thing that
        // owns navigation. Exposed here rather than reached for through the
        // visual tree, where a RelativeSource that silently fails to resolve
        // leaves a dead button with no error anywhere.
        NavigateCommand = new RelayCommand(p => _shell.Navigate(p as string ?? "Alerts"));
        SetUpCommand    = new RelayCommand(_ => _shell.RequestSetup());
    }

    public RelayCommand NavigateCommand { get; }

    /// <summary>
    /// Opens the driver setup screen. The shell raises an event rather than this
    /// creating a window: view models here do not own windows.
    /// </summary>
    public RelayCommand SetUpCommand { get; }

    /// <summary>
    /// Arming enforcement is owned by the settings page. The dashboard's status
    /// banner reuses that command rather than growing a second copy of the
    /// privileged path — two implementations of one state change is exactly how
    /// the two are guaranteed to disagree eventually.
    /// </summary>
    public AsyncRelayCommand ReArmCommand => _shell.SettingsPage.ReArmCommand;

    // ── status ───────────────────────────────────────────────

    public string StatusHeadline
    {
        get => _statusHeadline;
        private set => SetProperty(ref _statusHeadline, value);
    }

    public string StatusDetail
    {
        get => _statusDetail;
        private set => SetProperty(ref _statusDetail, value);
    }

    /// <summary>"ok" | "warn" | "off" — the view maps this to a colour, so the
    /// palette stays in XAML where it belongs rather than in a view model.</summary>
    public string StatusLevel
    {
        get => _statusLevel;
        private set
        {
            if (!SetProperty(ref _statusLevel, value)) return;
            OnPropertyChanged(nameof(CanReArm));
        }
    }

    /// <summary>
    /// Which single action the status banner offers.
    ///
    /// Arming is only offered when the driver is actually there to arm. The
    /// service answers set_enforcement with "ok" whether or not the minifilter
    /// loaded, so offering the button over a missing driver would let the user
    /// press it, be told protection is on, and have nothing change.
    /// </summary>
    public bool CanReArm => _statusLevel == "warn" && _driverConnected;

    /// <summary>
    /// Whether running setup could actually change anything — the driver is
    /// absent, or registered and waiting for its restart.
    ///
    /// Deliberately excludes the case where the driver is loaded and merely has
    /// enforcement switched off: that is what <see cref="CanReArm"/> is for, and
    /// offering "Set up" there would send the user through a UAC prompt to
    /// reinstall something that is already working.
    /// </summary>
    public bool CanSetUp => _haveStatus && !_driverConnected &&
                            _filterState is DriverSetup.FilterState.NotInstalled
                                         or DriverSetup.FilterState.Stopped;

    /// <summary>
    /// Every record the engine has appended since it started — NOT "threats
    /// blocked", which is what this used to be labelled.
    ///
    /// The engine reports AlertHistory::Total(), which counts every appended
    /// record: file opens it denied, processes it terminated, terminations that
    /// FAILED, and memory reads it only detected because the monitor was running
    /// with killing off. Calling that "threats blocked" overstates it, so the
    /// name and the label both say what it is.
    /// </summary>
    public long ProtectionEvents
    {
        get => _protectionEvents;
        private set
        {
            if (_protectionEvents == value) return;

            /*
             * A DECREASE is not a change, it is a discontinuity: the engine
             * restarted and its counter went back to zero.
             *
             * Easing down through that would animate events being undone — the
             * opposite of what happened — so the display snaps. _protectionEvents
             * itself stays the authoritative figure either way; only the number on
             * screen is animated, because the breakdown and the traffic chart read
             * the real value and must never see a half-way one.
             */
            bool isRestart = value < _protectionEvents;
            _protectionEvents = value;

            if (isRestart) _eventsCountUp.SnapTo(value);
            else           _eventsCountUp.To(value);

            OnPropertyChanged();
            OnPropertyChanged(nameof(ProtectionEventsText));
        }
    }

    /// <summary>
    /// The figure as displayed: an em dash until a status reply has been read.
    ///
    /// Zero is a claim of its own — "the engine has recorded nothing" — and it
    /// is not the same as "we do not know yet". Showing 0 in its place would be
    /// the app inventing a measurement.
    /// </summary>
    public string ProtectionEventsText =>
        _haveStatus ? ((long)Math.Round(_displayedEvents)).ToString(CultureInfo.InvariantCulture) : "—";

    public string UptimeText
    {
        get => _uptimeText;
        private set => SetProperty(ref _uptimeText, value);
    }

    public string VersionText
    {
        get => _versionText;
        private set => SetProperty(ref _versionText, value);
    }

    public int TrustedCount
    {
        get => _trustedCount;
        private set { if (SetProperty(ref _trustedCount, value)) OnPropertyChanged(nameof(TrustedCountText)); }
    }

    /// <summary>See <see cref="ProtectionEventsText"/> — same reasoning.</summary>
    public string TrustedCountText =>
        _haveStatus ? _trustedCount.ToString(CultureInfo.InvariantCulture) : "—";

    public ObservableCollection<ModuleStatus> Modules { get; } = new();

    /// <summary>Which build of the engine this is — "service" or "console". The
    /// distinction matters because console mode runs the memory monitor with
    /// killing disabled, so the same event count means something different.</summary>
    public string ServiceModeText => _serviceMode switch
    {
        Modes.Console => "Console mode",
        Modes.Service => "Service mode",
        _             => "",
    };

    public string ServiceModeHint => _serviceMode == Modes.Console
        ? "Memory threats are detected but not terminated in this mode."
        : "";

    public bool Busy
    {
        get => _busy;
        private set { if (SetProperty(ref _busy, value)) OnPropertyChanged(nameof(IsInteractive)); }
    }

    /// <summary>
    /// False while a privileged change is in flight, and while the engine is
    /// stopped.
    ///
    /// The switches report the engine's STORED configuration, which survives a
    /// stop — so without this they would sit enabled and reading ON over an
    /// engine that is not running, and pressing one would spend a UAC prompt
    /// reaching nothing.
    /// </summary>
    public bool IsInteractive => !_busy && !_engineStopped;

    /// <summary>
    /// The engine is answering but the minifilter is not loaded.
    ///
    /// The switches below still reflect — and still write — the engine's CONFIG,
    /// which is why they need a warning over them: an ON toggle sitting under
    /// "the driver is not loaded" reads as protection that is not there. The
    /// engine accepts set_enforcement with or without a driver, so the toggle
    /// would move and nothing would change.
    /// </summary>
    public bool DriverMissing => _haveStatus && !_driverConnected;

    /// <summary>
    /// The driver has never been installed on this machine — the SCM has no
    /// filter service at all.
    ///
    /// Distinct from <see cref="DriverMissing"/>, which is only "not connected".
    /// The two look identical in the status reply and need opposite advice: this
    /// one means run setup, the other means a restart is probably pending. The
    /// distinction comes from the SCM, via <see cref="ApplyFilterState"/>.
    /// </summary>
    public bool DriverNotInstalled =>
        _haveStatus && !_driverConnected && _filterState == DriverSetup.FilterState.NotInstalled;

    /// <summary>
    /// The driver is registered but not running. In practice this is a machine
    /// that has just been through setup and is waiting for the restart that
    /// makes test signing take effect.
    /// </summary>
    public bool DriverAwaitingStart =>
        _haveStatus && !_driverConnected && _filterState == DriverSetup.FilterState.Stopped;

    /// <summary>
    /// A sentence to show over the protection switches when they are reporting
    /// configuration that nothing is currently enforcing — empty when they are
    /// telling the truth unaided.
    /// </summary>
    public string ControlsWarningText =>
        _engineStopped
            ? "The engine is stopped, so these settings are not in effect. "
              + "They are stored and will apply again when you start it."
      : DriverMissing
            ? "The kernel driver is not loaded, so these settings cannot take effect. "
              + "They are stored and will apply once the driver is running again."
      : "";

    // ── switches ─────────────────────────────────────────────

    public bool Enforcement
    {
        get => _enforcement;
        set
        {
            bool previous = _enforcement;
            if (!SetProperty(ref _enforcement, value) || _suspendPush) return;
            _ = PushAsync(Verbs.SetEnforcement, o => o["enabled"] = value,
                          () => Restore(() => Enforcement = previous), "Enforcement");
        }
    }

    public bool FileBlock
    {
        get => _fileBlock;
        set
        {
            bool previous = _fileBlock;
            if (!SetProperty(ref _fileBlock, value) || _suspendPush) return;
            // set_features carries BOTH bits, so send the other one unchanged.
            _ = PushAsync(Verbs.SetFeatures,
                          o => { o["file_block"] = value; o["alerts"] = _alertsEnabled; },
                          () => Restore(() => FileBlock = previous), "File blocking");
        }
    }

    public bool AlertsEnabled
    {
        get => _alertsEnabled;
        set
        {
            bool previous = _alertsEnabled;
            if (!SetProperty(ref _alertsEnabled, value) || _suspendPush) return;
            _ = PushAsync(Verbs.SetFeatures,
                          o => { o["file_block"] = _fileBlock; o["alerts"] = value; },
                          () => Restore(() => AlertsEnabled = previous), "Alert reporting");
        }
    }

    public bool MemoryMonitor
    {
        get => _memoryMonitor;
        set
        {
            bool previous = _memoryMonitor;
            if (!SetProperty(ref _memoryMonitor, value) || _suspendPush) return;
            _ = PushAsync(Verbs.SetMemoryMonitor, o => o["enabled"] = value,
                          () => Restore(() => MemoryMonitor = previous), "Memory monitor");
        }
    }

    public bool Decoy
    {
        get => _decoy;
        set
        {
            bool previous = _decoy;
            if (!SetProperty(ref _decoy, value) || _suspendPush) return;
            _ = PushAsync(Verbs.SetDecoy, o => o["enabled"] = value,
                          () => Restore(() => Decoy = previous), "Decoy folder");
        }
    }

    private void Restore(Action apply)
    {
        _suspendPush = true;
        try { apply(); }
        finally { _suspendPush = false; }
    }

    private async Task PushAsync(string verb, Action<JsonObject> fill, Action onFail, string label)
    {
        Busy = true;
        try
        {
            var outcome = await _shell.RunPrivilegedAsync(verb, fill);
            if (outcome.Ok)
            {
                _shell.ReportSuccess($"{label} updated.");
                // Re-read rather than trusting our own guess: the decoy folder
                // can fail to be created, and the engine is the authority.
                await _shell.RefreshConfigAsync();
                return;
            }

            onFail();
            _shell.ReportActionFailure(label, outcome);
        }
        finally
        {
            Busy = false;
        }
    }

    // ── activity breakdown ───────────────────────────────────

    /// <summary>What the engine did over the records this app is holding.
    /// Empty when it is holding none, which is what keeps the breakdown line
    /// off the page rather than showing four blank labels.</summary>
    public ObservableCollection<EventKind> Breakdown { get; } = new();

    /// <summary>States the window the counts cover, so they cannot be read as a
    /// complete history.</summary>
    public string BreakdownCaption { get; private set; } = "";

    // ── traffic chart ────────────────────────────────────────

    public ObservableCollection<TrafficBucket> Traffic { get; } = new();

    public RelayCommand SetTrafficRangeCommand { get; }

    public string TrafficRange
    {
        get => _trafficRange;
        set
        {
            if (!SetProperty(ref _trafficRange, value)) return;
            OnPropertiesChanged(nameof(IsTraffic24h), nameof(IsTraffic7d), nameof(IsTraffic30d));
            RebuildActivity();
        }
    }

    // One-way, read by the range pills. They cannot be two-way: the three
    // buttons would fight each other over a single source of truth.
    public bool IsTraffic24h => _trafficRange == Range24h;
    public bool IsTraffic7d  => _trafficRange == Range7d;
    public bool IsTraffic30d => _trafficRange == Range30d;

    public string TrafficCaption { get; private set; } = "";

    public bool HasTrafficBars => Traffic.Count > 0;

    /// <summary>
    /// True when alert reporting is switched off. Blocks still HAPPEN in that
    /// state — they are simply not recorded — so an empty chart would read as
    /// "nothing has tried", which is the exact opposite of the truth.
    /// </summary>
    public bool AlertsReportingOff { get; private set; }

    /// <summary>The chart is shown only when there is a real series to draw.</summary>
    public bool ShowTrafficChart => !AlertsReportingOff && Traffic.Count > 0;

    /// <summary>Otherwise the card explains itself in words instead.</summary>
    public bool ShowTrafficNotice => !ShowTrafficChart;

    // ── updates from the shell ───────────────────────────────

    public void Apply(StatusSnapshot s)
    {
        Restore(() =>
        {
            Enforcement   = s.DriverEnforcement;
            FileBlock     = s.DriverFileBlock;
            AlertsEnabled = s.DriverAlerts;
            MemoryMonitor = s.MemoryMonitorActive;
            Decoy         = s.DecoyEnabled;
        });

        _driverConnected = s.DriverConnected;
        _serviceMode     = s.ServiceMode;
        _haveStatus      = true;
        _engineStopped   = false;
        _lastStatus      = s;

        ProtectionEvents = s.ThreatsBlocked;
        UptimeText       = FormatUptime(s.UptimeSeconds);
        VersionText      = string.IsNullOrEmpty(s.Version) ? "—" : s.Version;
        TrustedCount     = s.TrustedProcessCount;

        RebuildModules(s);
        UpdateHeadline();
        RebuildActivity();

        OnPropertiesChanged(nameof(ServiceModeText), nameof(ServiceModeHint),
                            nameof(ProtectionEventsText), nameof(TrustedCountText),
                            nameof(CanReArm), nameof(DriverMissing),
                            nameof(IsInteractive), nameof(ControlsWarningText));
    }

    /// <summary>
    /// Take the SCM's word for the kernel filter's state.
    ///
    /// Arrives on its own schedule — it is a separate cross-process query, not
    /// part of the status reply — so the headline is recomputed here rather than
    /// only in <see cref="Apply"/>. Without that, a dashboard rendered before
    /// the SCM answered would keep offering setup on a machine whose driver is
    /// already installed, until the next status poll happened to refresh it.
    /// </summary>
    public void ApplyFilterState(DriverSetup.FilterState state)
    {
        _filterState = state;

        // Only when the status reply is already in hand: with no snapshot there
        // is nothing to recompute from, and MarkStopped/MarkUnreachable own the
        // headline in that case.
        if (_haveStatus) UpdateHeadline();

        OnPropertiesChanged(nameof(DriverMissing), nameof(DriverNotInstalled),
                            nameof(DriverAwaitingStart), nameof(CanSetUp),
                            nameof(CanReArm), nameof(ControlsWarningText));
    }

    /// <summary>Re-run the chart and the breakdown against the current clock. The
    /// traffic window is anchored to "now", so it goes stale the moment the page
    /// is left.</summary>
    public void OnActivated() => RebuildActivity();

    /// <summary>Keeps the switches honest when only the config was re-read.</summary>
    public void ApplyConfig(ShieldCordConfig c)
    {
        Restore(() =>
        {
            Enforcement   = c.Enforcement;
            FileBlock     = c.FileBlock;
            AlertsEnabled = c.Alerts;
            MemoryMonitor = c.MemoryMonitor;
            Decoy         = c.DecoyFolder;
        });
    }

    /// <summary>
    /// The engine was stopped on purpose.
    ///
    /// Deliberately NOT the same message as <see cref="MarkUnreachable"/>: a
    /// deliberate stop is a known fact, not an unknown one. Protection is off
    /// and the app knows it, so it says so instead of claiming the state cannot
    /// be determined.
    /// </summary>
    public void MarkStopped()
    {
        StatusLevel    = "off";
        StatusHeadline = "Engine stopped";
        StatusDetail   = "ShieldCord is not running, so nothing is blocking token theft. "
                       + "Start it again from Settings when you are done.";

        // One honest row rather than an empty card. The layer list is this page's
        // answer to "am I protected", and a blank card answers nothing.
        Modules.Clear();
        Modules.Add(new ModuleStatus(
            "Engine",
            "Stopped — no protection layer is running.",
            false));

        _haveStatus      = false;
        _driverConnected = false;
        _serviceMode     = "";
        _engineStopped   = true;
        UptimeText       = "—";

        OnPropertiesChanged(nameof(ProtectionEventsText), nameof(TrustedCountText),
                            nameof(ServiceModeText), nameof(ServiceModeHint),
                            nameof(CanReArm), nameof(DriverMissing),
                            nameof(IsInteractive), nameof(ControlsWarningText));

        RebuildActivity();
    }

    /// <summary>The engine went away — stop claiming everything is fine.</summary>
    public void MarkUnreachable()
    {
        StatusLevel    = "off";
        StatusHeadline = "Engine unreachable";
        StatusDetail   = "The ShieldCord service is not responding. Protection state is unknown.";
        Modules.Clear();

        // The tiles must stop asserting figures the app can no longer vouch for.
        // They go blank rather than to zero, because a zero is itself a claim —
        // "the engine has recorded nothing" — and that is not what happened.
        _haveStatus      = false;
        _driverConnected = false;
        _serviceMode     = "";
        _engineStopped   = false;
        UptimeText       = "—";

        OnPropertiesChanged(nameof(ProtectionEventsText), nameof(TrustedCountText),
                            nameof(ServiceModeText), nameof(ServiceModeHint),
                            nameof(CanReArm), nameof(DriverMissing),
                            nameof(IsInteractive), nameof(ControlsWarningText));

        RebuildActivity();
    }

    public void OnConnectionChanged(ConnectionState state)
    {
        if (state != ConnectionState.Connected) MarkUnreachable();
        else
        {
            StatusHeadline = "Checking…";
            StatusDetail   = "Reading the engine's state.";
            StatusLevel    = "warn";
        }
    }

    // ── activity computation ─────────────────────────────────

    /// <summary>
    /// Rebuild the chart and the breakdown from the alert records the shell
    /// holds.
    ///
    /// Deliberately NOT driven by an optimistic local counter. The shell used to
    /// bump its own total on every live alert while the engine counts a
    /// different set, so the two disagreed and the displayed number visibly fell
    /// on the next poll. Everything on this page now comes from one place: the
    /// last status reply, plus the records actually in hand.
    /// </summary>
    private void RebuildActivity()
    {
        var alerts = _shell.Alerts;

        // The four actions the protocol defines, in the order the legend reads.
        int blocked = 0, killed = 0, failed = 0, detected = 0;
        foreach (var a in alerts)
        {
            switch (a.Action)
            {
                case Actions.Blocked:      blocked++;  break;
                case Actions.Killed:       killed++;   break;
                case Actions.KillFailed:   failed++;   break;
                case Actions.DetectedOnly: detected++; break;
            }
        }

        int total = blocked + killed + failed + detected;

        Breakdown.Clear();
        if (total > 0)
        {
            Breakdown.Add(new EventKind("Blocked",       blocked));
            Breakdown.Add(new EventKind("Terminated",    killed));
            Breakdown.Add(new EventKind("Kill failed",   failed));
            Breakdown.Add(new EventKind("Detected only", detected));
        }

        // This caption describes what THIS APP is holding, never what the
        // engine has — the two genuinely differ. The timeline can be cleared,
        // and the app only ever holds the last 500 records, so a caption
        // claiming "the engine still holds" this would put the breakdown into
        // direct contradiction with the engine's own counter on the same page.
        BreakdownCaption = total > 0
            ? $"Of the {total} event{(total == 1 ? "" : "s")} this app is holding."
            : _protectionEvents > 0
                ? "This app is holding no events. The timeline may have been cleared."
                : "No events recorded yet.";

        RebuildTraffic(alerts);

        OnPropertiesChanged(nameof(BreakdownCaption));
    }

    /// <summary>
    /// Bucket the retained events into the selected window.
    ///
    /// The window is anchored to the current clock, not to the newest event.
    /// Anchoring on the data would make "last 24 hours" quietly mean "the 24
    /// hours up to whenever the last thing happened", which overstates how
    /// recent the activity is exactly when there has been none.
    /// </summary>
    private void RebuildTraffic(IReadOnlyCollection<AlertRecord> alerts)
    {
        AlertsReportingOff = !_alertsEnabled;
        Traffic.Clear();

        // With reporting off nothing is being written down, so there is no
        // series to draw at all — not even an empty one. Say why in words.
        if (AlertsReportingOff)
        {
            TrafficCaption = "Alert reporting is off, so nothing is being recorded.";
            RaiseTrafficChanged();
            return;
        }

        int buckets = _trafficRange switch { Range7d => 7, Range30d => 30, _ => 24 };
        bool hourly = _trafficRange == Range24h;

        var now   = DateTime.Now;
        var start = hourly
            ? new DateTime(now.Year, now.Month, now.Day, now.Hour, 0, 0).AddHours(-(buckets - 1))
            : now.Date.AddDays(-(buckets - 1));

        var counts = new int[buckets];
        foreach (var a in alerts)
        {
            if (!AlertTime.TryParse(a.Time, out var when)) continue;

            double offset = hourly ? (when - start).TotalHours : (when - start).TotalDays;
            int index = (int)Math.Floor(offset);
            if (index >= 0 && index < buckets) counts[index]++;
        }

        int peak = 0;
        foreach (int c in counts) if (c > peak) peak = c;

        // Label roughly one column in eight, so 30 columns do not become a smear.
        int step = Math.Max(1, (int)Math.Ceiling(buckets / 8.0));

        for (int i = 0; i < buckets; i++)
        {
            var when = hourly ? start.AddHours(i) : start.AddDays(i);

            string label = _trafficRange switch
            {
                Range24h => when.ToString("HH:mm"),
                Range7d  => when.ToString("ddd"),
                _        => when.ToString("d/M"),
            };

            Traffic.Add(new TrafficBucket(
                label,
                counts[i],
                peak == 0 ? 0 : (double)counts[i] / peak,
                i % step == 0));
        }

        int inWindow = 0;
        foreach (int c in counts) inWindow += c;

        string window = _trafficRange switch
        {
            Range7d  => "the last 7 days",
            Range30d => "the last 30 days",
            _        => "the last 24 hours",
        };

        if (AlertsReportingOff)
        {
            TrafficCaption = "Alert reporting is off, so nothing is being recorded.";
        }
        else if (alerts.Count == 0)
        {
            TrafficCaption = _protectionEvents > 0
                ? "This app is holding no events. The timeline may have been cleared."
                : "No events recorded yet.";
        }
        else if (inWindow == 0)
        {
            TrafficCaption = $"Nothing recorded in {window}.";
        }
        else
        {
            TrafficCaption = $"{inWindow} of the {alerts.Count} events this app is holding, in {window}.";
        }

        RaiseTrafficChanged();
    }

    private void RaiseTrafficChanged() =>
        OnPropertiesChanged(nameof(TrafficCaption), nameof(HasTrafficBars),
                            nameof(AlertsReportingOff),
                            nameof(ShowTrafficChart), nameof(ShowTrafficNotice));

    // ── presentation helpers ─────────────────────────────────

    private void RebuildModules(StatusSnapshot s)
    {
        Modules.Clear();

        Modules.Add(new ModuleStatus(
            "Kernel driver",
            s.DriverConnected ? "Connected to the filter driver" : "Not loaded — no file protection",
            s.DriverConnected));

        // The three layers below are meaningless without the driver, and the
        // service keeps reporting its CONFIGURED flags either way — ipc_verbs.cpp
        // reads them straight from ConfigManager, where they default to true, and
        // nothing clears them when the minifilter is absent. Without this guard
        // the page would show a green "Armed — untrusted processes are denied"
        // sitting directly under its own amber "Driver not loaded" banner, which
        // is precisely the contradiction this app exists not to produce.
        const string unavailable = "Unavailable — the driver is not loaded";

        Modules.Add(new ModuleStatus(
            "Default-deny enforcement",
            !s.DriverConnected ? unavailable
            : s.DriverEnforcement ? "Armed — untrusted processes are denied"
                                  : "Disarmed — any process may read tokens",
            s.DriverConnected && s.DriverEnforcement));

        Modules.Add(new ModuleStatus(
            "File blocking",
            !s.DriverConnected ? unavailable
            : s.DriverFileBlock ? "Denying opens of protected files" : "Off — opens are allowed",
            s.DriverConnected && s.DriverFileBlock));

        Modules.Add(new ModuleStatus(
            "Alert reporting",
            !s.DriverConnected ? unavailable
            : s.DriverAlerts ? "Reporting blocks to this app" : "Off — blocks happen silently",
            s.DriverConnected && s.DriverAlerts));

        // The memory monitor and the decoy folder are service-side; they work
        // whether or not the minifilter loaded, so they are reported on their own
        // flags rather than being dragged down with the driver.
        Modules.Add(new ModuleStatus(
            "Memory monitor",
            s.MemoryMonitorActive ? "Watching for memory reads on Discord and browsers"
                                  : "Off — memory reads are not watched",
            s.MemoryMonitorActive));

        Modules.Add(new ModuleStatus(
            "Decoy token folder",
            s.DecoyEnabled
                ? (string.IsNullOrEmpty(s.DecoyFolderPath) ? "Armed" : s.DecoyFolderPath)
                : "Off — no bait tokens planted",
            s.DecoyEnabled));
    }

    private void UpdateHeadline()
    {
        var s = _lastStatus;
        if (s is null) return;

        if (!s.DriverConnected)
        {
            StatusLevel = "warn";

            // Three different situations reach this branch, and they need three
            // different sentences. The status reply cannot tell them apart — only
            // the SCM can — which is what _filterState is for.
            if (DriverNotInstalled)
            {
                StatusHeadline = "Driver not installed";
                StatusDetail   = "This PC has no ShieldCord kernel driver yet, so nothing is "
                               + "blocking token theft. Setup installs it and takes about a minute.";
            }
            else if (DriverAwaitingStart)
            {
                StatusHeadline = "Restart to finish setup";
                StatusDetail   = "The driver is installed but Windows will not load it until "
                               + "this PC restarts.";
            }
            else
            {
                StatusHeadline = "Driver not loaded";
                StatusDetail   = "The kernel driver is not running, so tokens are not protected. "
                               + "Run setup to install it again.";
            }
            return;
        }

        if (!s.DriverEnforcement || !s.DriverFileBlock)
        {
            StatusLevel    = "warn";
            StatusHeadline = "Protection is off";
            StatusDetail   = "The driver is running but is not blocking anything. "
                           + "Turn enforcement back on to protect your tokens.";
            return;
        }

        StatusLevel    = "ok";
        StatusHeadline = "Tokens protected";
        StatusDetail   = "Untrusted processes are denied access to Discord and browser token stores.";
    }

    private static string FormatUptime(long seconds)
    {
        if (seconds <= 0) return "—";
        var t = TimeSpan.FromSeconds(seconds);
        if (t.TotalDays   >= 1) return $"{(int)t.TotalDays}d {t.Hours}h";
        if (t.TotalHours  >= 1) return $"{(int)t.TotalHours}h {t.Minutes}m";
        if (t.TotalMinutes >= 1) return $"{(int)t.TotalMinutes}m {t.Seconds}s";
        return $"{(int)t.TotalSeconds}s";
    }
}
