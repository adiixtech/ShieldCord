using System.Collections.ObjectModel;
using System.Globalization;
using System.Security.Principal;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Threading;
using ShieldCordUI.Mvvm;
using ShieldCordUI.Protocol;
using ShieldCordUI.Services;

namespace ShieldCordUI.ViewModels;

/// <summary>Outcome of one privileged change, phrased for display.</summary>
/// <param name="ReplyJson">
/// The reply as it arrived, for callers whose verb answers with more than
/// success or failure — the driver install reports what it verified. Null when
/// nothing came back, which callers must read as "unknown", never as success.
/// </param>
public sealed record PrivilegedOutcome(bool Ok, bool Cancelled, string? Error,
                                       string? Message = null, string? ReplyJson = null)
{
    public static readonly PrivilegedOutcome Success = new(true, false, null);

    /// <summary>A sentence the user can act on — not a raw error code.</summary>
    public string Describe() =>
        // A caller-supplied sentence wins. A UI-local action (starting the
        // engine) knows exactly what went wrong and has no engine error code to
        // translate, so translating it anyway would invent a worse explanation.
        Message is not null ? Message
      : Cancelled ? "Administrator approval was declined, so nothing changed."
      : Error == Errors.NotAuthorized ? "The engine refused: this needs an Administrator."
      : Error == Errors.KillFailed ? "The engine could not terminate that process. It may have already exited, or be one Windows protects."
      : Error is not null ? $"The engine refused: {Error}."
      : "The engine did not answer. It may be stopped, still starting, or an older build "
        + "that does not know this command.";
}

/// <summary>
/// Owns everything shared across the app: the pipe connection, the status
/// poll, the alert feed, navigation, and the privileged-action path.
///
/// The three page view models are thin — they read from here rather than each
/// holding their own connection, which is what keeps a stopped service from
/// leaving three views disagreeing about whether it is running.
/// </summary>
public sealed class ShellViewModel : ObservableObject, IAsyncDisposable
{
    private readonly IpcClient _ipc = new();
    private readonly DispatcherTimer _poll;
    private readonly Dispatcher _dispatcher;

    /// <summary>Highest alert id already merged into <see cref="Alerts"/>.</summary>
    private long _lastAlertId;

    /// <summary>Last uptime the engine reported. A smaller reading means it restarted.</summary>
    private long _lastUptime = -1;

    /// <summary>
    /// True once the engine has actually been observed not running.
    ///
    /// The SCM keeps reporting RUNNING for a moment after a service
    /// acknowledges its own exit, so a bare Running reading must not be allowed
    /// to release a hold the user's stop just took — it would cancel the hold
    /// and let the flapping straight back in.
    /// </summary>
    private bool _sawEngineNotRunning;

    /// <summary>Throttles the direct pipe probe while the loop is parked.</summary>
    private int _heldProbeTicks;

    /// <summary>Throttles the SCM reconciliation while the engine is not visible.</summary>
    private int _engineStateTicks;

    /// <summary>Poll ticks between SCM reconciliations (~6 s at the default interval).</summary>
    private const int StatePollEvery = 3;

    /// <summary>Poll ticks between direct pipe probes while parked.</summary>
    private const int ProbeEvery = 3;

    /// <summary>
    /// Poll ticks between watermark reconciles.
    ///
    /// The watermark is SHELL state, not engine state, so this runs regardless
    /// of what the engine is doing — and the steady state is a PID compare, so
    /// it costs nothing until the shell restarts, which is precisely when the
    /// injected hooks die and have to go back in.
    /// </summary>
    private const int WatermarkEvery = 5;

    private ConnectionState _connection = ConnectionState.Disconnected;
    private bool _isSetupMode;
    private string _banner = "";
    private bool _bannerIsError;
    private bool _busy;
    private object _currentView = null!;
    private string _currentPage = "Dashboard";
    private int _unread;

    private readonly WatermarkSuppressor _watermark = new();
    private int _watermarkTicks;
    private string _watermarkStatus = "";

    public ShellViewModel(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher;
        Settings = AppSettings.Load();

        Dashboard    = new DashboardViewModel(this);
        AlertsPage   = new AlertsViewModel(this);
        AppsPage     = new ProtectedAppsViewModel(this);
        SettingsPage = new SettingsViewModel(this);
        HelpPage     = new HelpViewModel();
        SetupPage    = new SetupViewModel(this);

        NavigateCommand    = new RelayCommand(p => Navigate(p as string ?? "Dashboard"));
        ClearBannerCommand = new RelayCommand(ClearBanner);

        _ipc.AlertReceived += OnAlertReceived;
        _ipc.StateChanged   += OnStateChanged;

        _poll = new DispatcherTimer(DispatcherPriority.Background, dispatcher)
        {
            Interval = TimeSpan.FromMilliseconds(Math.Max(500, Settings.StatusPollMs)),
        };
        _poll.Tick += OnPollTick;

        // Restore what the user removed last session, so a relaunch does not
        // silently undo their tidy-up.
        LoadDismissedAlerts();

        CurrentView = Dashboard;
    }

    // ── shared state ─────────────────────────────────────────

    public AppSettings Settings { get; }
    public IpcClient Client => _ipc;

    public DashboardViewModel     Dashboard    { get; }
    public AlertsViewModel        AlertsPage   { get; }
    public ProtectedAppsViewModel AppsPage     { get; }
    public SettingsViewModel      SettingsPage { get; }
    public HelpViewModel          HelpPage     { get; }

    /// <summary>
    /// The setup checklist. Lives in the shell rather than being created on demand
    /// because it holds a restart countdown that must survive navigation — leaving
    /// the page mid-countdown and coming back should not lose the timer.
    /// </summary>
    public SetupViewModel         SetupPage    { get; }

    /// <summary>
    /// True while setup has taken over the window.
    ///
    /// Setup is a takeover rather than a page: it is a thing you complete, and the
    /// sidebar, the status footer and the navigation are all noise until it is
    /// done. The window itself never changes — only what is drawn inside it.
    /// </summary>
    public bool IsSetupMode
    {
        get => _isSetupMode;
        private set
        {
            if (!SetProperty(ref _isSetupMode, value)) return;
            OnPropertyChanged(nameof(IsNormalMode));
        }
    }

    /// <summary>The inverse, for the three layers the takeover covers. WPF has no
    /// inverse converter used here, and a named property reads better at the
    /// binding site than a converter chain.</summary>
    public bool IsNormalMode => !_isSetupMode;

    /// <summary>Live alert feed, newest first. Capped so a long session cannot grow without bound.</summary>
    public ObservableCollection<AlertRecord> Alerts { get; } = new();

    /// <summary>
    /// Signed applications allowed to read their own token stores, plus the
    /// publishers the user added at runtime.
    ///
    /// Filled by <see cref="LoadDetailsAsync"/> and never by the status poll:
    /// the service walks its whole process list to build this reply, so it is
    /// the most expensive thing the UI asks for.
    /// </summary>
    public ObservableCollection<ProtectedApp> ProtectedApps { get; } = new();

    /// <summary>The path suffixes the driver denies untrusted opens of.</summary>
    public ObservableCollection<ProtectedPath> ProtectedPaths { get; } = new();

    /// <summary>
    /// Raised on the UI thread for alerts that arrived LIVE — not for ones
    /// pulled in by a history backfill. The tray balloons on this, so a
    /// reconnect does not fire a burst of notifications for old events.
    /// </summary>
    public event Action<AlertRecord>? LiveAlert;

    /// <summary>
    /// Whether setup still needs doing — the driver is not registered at all, which
    /// is the state both "never set up" and "set up failed" present as.
    ///
    /// Drives the dashboard's own call to action rather than a sidebar entry:
    /// setup is a takeover, so it has no place in the navigation.
    /// </summary>
    public bool NeedsSetup => FilterState == DriverSetup.FilterState.NotInstalled;

    // ── kernel driver state ──────────────────────────────────

    /// <summary>
    /// What the SCM knows about the kernel filter.
    ///
    /// Read from the SCM rather than inferred from the engine's status reply.
    /// That reply says only whether the driver is CONNECTED, which cannot tell
    /// "never installed" from "installed but not loaded" — and those two need
    /// opposite things from the user: run setup, or restart.
    /// </summary>
    public DriverSetup.FilterState FilterState { get; private set; } = DriverSetup.FilterState.Unknown;

    /// <summary>
    /// Re-read the filter's state and hand it to the dashboard.
    ///
    /// Called at startup, on every connection change, and after the setup screen
    /// closes — the moments it can actually change. Deliberately NOT from the
    /// status poll: this is a cross-process SCM call and the answer only moves
    /// when someone installs or removes the driver.
    /// </summary>
    public async Task RefreshFilterStateAsync()
    {
        try
        {
            FilterState = await Task.Run(DriverSetup.Query);
        }
        catch
        {
            // Keep the previous value rather than assert "not installed" on the
            // strength of a failed query — that would offer setup for a driver
            // that is already there.
            return;
        }

        Dashboard.ApplyFilterState(FilterState);

        // Drives the sidebar's conditional Setup entry, which has to appear the
        // moment the driver turns out to be missing and disappear once it is not.
        OnPropertyChanged(nameof(NeedsSetup));
    }

    /// <summary>
    /// Take over the window with setup.
    ///
    /// Also subscribes to the two ways setup ends, once — reopening it must not
    /// stack a second pair of handlers on the same events.
    /// </summary>
    public void OpenSetup(SetupStage stage)
    {
        SetupPage.Open(stage);

        if (!_setupHandlersAttached)
        {
            _setupHandlersAttached = true;

            SetupPage.Finished += () =>
            {
                IsSetupMode = false;

                // Remember it. Without this, the next launch would see the driver
                // running and no acknowledgement, and reopen the closing screen
                // every single time.
                Settings.SetupAcknowledged = true;
                Settings.Save();

                Navigate("Dashboard");
            };

            SetupPage.Deferred += () =>
            {
                IsSetupMode = false;
                Settings.SetupPromptDismissed = true;
                Settings.Save();
            };
        }

        IsSetupMode = true;
    }

    private bool _setupHandlersAttached;

    /// <summary>The dashboard's "Set up protection" button comes through here.</summary>
    public void RequestSetup() => OpenSetup(SetupStage.Welcome);

    private const int MaxAlertsInList = 500;

    public RelayCommand NavigateCommand { get; }
    public RelayCommand ClearBannerCommand { get; }

    public object CurrentView
    {
        get => _currentView;
        private set => SetProperty(ref _currentView, value);
    }

    public string CurrentPage
    {
        get => _currentPage;
        private set => SetProperty(ref _currentPage, value);
    }

    public ConnectionState Connection
    {
        get => _connection;
        private set
        {
            if (!SetProperty(ref _connection, value)) return;
            OnPropertyChanged(nameof(IsConnected));
            RaiseConnectionLabels();

            // A stopped engine and an unreachable one are different situations,
            // and the dashboard verdict must not describe them the same way.
            if (_ipc.ReconnectHeld) Dashboard.MarkStopped();
            else                    Dashboard.OnConnectionChanged(value);
        }
    }

    public bool IsConnected => Connection == ConnectionState.Connected;

    /// <summary>
    /// True when the engine was stopped and this app has stopped trying to reach
    /// it.
    ///
    /// NOT the same as unreachable. One is a decision the user made, the other
    /// is a fault, and the UI must not describe them with the same words.
    /// </summary>
    public bool IsEngineStopped => _ipc.ReconnectHeld;

    public string ConnectionText => _ipc.ReconnectHeld
        ? "The ShieldCord engine is stopped"
        : Connection switch
        {
            ConnectionState.Connected    => "Connected to the ShieldCord engine",
            ConnectionState.Connecting   => "Connecting to the ShieldCord engine…",
            _                            => "ShieldCord engine unreachable",
        };

    /// <summary>The same states, sized for the 200px sidebar footer.</summary>
    public string ConnectionShort => _ipc.ReconnectHeld
        ? "Engine stopped"
        : Connection switch
        {
            ConnectionState.Connected  => "Engine connected",
            ConnectionState.Connecting => "Connecting…",
            _                          => "Engine offline",
        };

    private void RaiseConnectionLabels() =>
        OnPropertiesChanged(nameof(ConnectionText), nameof(ConnectionShort),
                            nameof(IsEngineStopped));

    /// <summary>Transient message shown under the header (success tone).</summary>
    public string Banner
    {
        get => _banner;

        // HasBanner is a separate computed property, so it needs its own
        // notification — SetProperty only raises the caller's own name. Without
        // this the view's Visibility binding is evaluated once at load and the
        // banner never appears at all, silently swallowing every message the
        // engine sends back about an action that did NOT take effect.
        private set { if (SetProperty(ref _banner, value)) OnPropertyChanged(nameof(HasBanner)); }
    }

    public bool BannerIsError
    {
        get => _bannerIsError;
        private set => SetProperty(ref _bannerIsError, value);
    }

    public bool HasBanner => !string.IsNullOrEmpty(Banner);

    /// <summary>True while an elevated action is in flight — the whole page disables.</summary>
    public bool Busy
    {
        get => _busy;
        private set { if (SetProperty(ref _busy, value)) OnPropertyChanged(nameof(IsInteractive)); }
    }

    public bool IsInteractive => !_busy;

    /// <summary>
    /// Events that arrived while the Activity page was not open — drives the
    /// bell badge. Cleared by navigating to Activity, because "there is
    /// something you have not looked at" is a claim the shell can actually keep.
    /// </summary>
    public int UnreadCount
    {
        get => _unread;
        private set
        {
            if (!SetProperty(ref _unread, value)) return;
            OnPropertiesChanged(nameof(HasUnread), nameof(UnreadBadge));
        }
    }

    public bool HasUnread => _unread > 0;

    /// <summary>The badge figure — exact up to 99, then "99+". A badge that grew
    /// without bound would deform the caption strip it sits in.</summary>
    public string UnreadBadge => _unread > 99 ? "99+" : _unread.ToString(CultureInfo.InvariantCulture);

    // ── the signed-in account ────────────────────────────────

    // Read from the process token rather than asked for. ShieldCord has no
    // accounts of its own, so the mockup's invented "John Doe / Admin" is
    // precisely the kind of thing this chrome must not put on screen.

    public string UserName { get; } = Environment.UserName;

    public string UserInitial
    {
        get
        {
            string name = Environment.UserName;
            return name.Length > 0 ? char.ToUpperInvariant(name[0]).ToString() : "?";
        }
    }

    public string UserRole { get; } = DescribeRole();

    private static string DescribeRole()
    {
        try
        {
            using var identity = WindowsIdentity.GetCurrent();
            return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator)
                ? "Administrator"
                : "Standard user";
        }
        catch
        {
            // A restricted token can decline the query. Say less, not more.
            return "Signed in";
        }
    }

    // ── lifecycle ────────────────────────────────────────────

    public void Start()
    {
        // Start PARKED, and release only once the SCM has said what the engine
        // situation actually is. Letting the loop run first would publish
        // "Connecting…" for the moment before the first answer arrives — the
        // exact flash a relaunch into a stopped engine must never produce.
        _ipc.HoldReconnect();

        _ipc.Start();
        _poll.Start();

        _ = PrimeEngineStateAsync();

        // The watermark is applied at startup rather than waiting for the
        // first poll tick, so a sign-in launch hides it as soon as the window
        // appears instead of a few seconds later.
        _ = ReconcileWatermarkAsync();
    }

    /// <summary>
    /// Ask the SCM once, at startup, and release the hold if the engine is
    /// there. The query is a blocking cross-process call, so it runs off the UI
    /// thread — a synchronous query here would stall the first frame.
    /// </summary>
    private async Task PrimeEngineStateAsync()
    {
        ServiceControl.EngineState state;
        try
        {
            state = await Task.Run(ServiceControl.Query);
        }
        catch
        {
            // Never leave the app parked because the query itself failed. Fail
            // toward trying, which is the behaviour this replaces.
            _ipc.ReleaseReconnect();
            RaiseConnectionLabels();
            return;
        }

        // Conclusive: this app has not asked for a stop, so a Running reading
        // settles it and there is no lag to wait out.
        SyncEngineHold(state, conclusive: true);
        RaiseConnectionLabels();

        if (_ipc.ReconnectHeld) Dashboard.MarkStopped();
    }

    /// <summary>
    /// Reconcile the reconnect hold with what the SCM reports.
    ///
    /// <paramref name="conclusive"/> distinguishes the two callers: at startup a
    /// Running reading settles the matter, but during the periodic probe it does
    /// NOT — see <see cref="_sawEngineNotRunning"/>.
    /// </summary>
    private void SyncEngineHold(ServiceControl.EngineState state, bool conclusive)
    {
        switch (state)
        {
            case ServiceControl.EngineState.Running:
                if (conclusive || _sawEngineNotRunning)
                {
                    _sawEngineNotRunning = false;
                    _heldProbeTicks      = 0;
                    _ipc.ReleaseReconnect();
                }
                break;

            case ServiceControl.EngineState.Stopped:
            case ServiceControl.EngineState.NotInstalled:
                _sawEngineNotRunning = true;
                _ipc.HoldReconnect();
                break;

            // Pending collapses STOP_PENDING and START_PENDING into one value, so
            // it cannot tell "going down" from "coming up"; Unknown is not
            // evidence of anything. Neither is allowed to move the hold.
            default:
                break;
        }
    }

    /// <summary>
    /// The user stopped the engine from this app.
    ///
    /// Takes the hold immediately rather than waiting for the pipe to drop or
    /// the SCM to catch up. Called on any non-cancelled stop outcome, not only
    /// on a success reply: the service can tear its pipe down before the reply
    /// reaches the elevated child, and a lost reply must not leave the flap
    /// running.
    /// </summary>
    public void NoteDeliberateStop()
    {
        // Clear it, so the SCM's lingering RUNNING reading — it keeps reporting
        // RUNNING for a moment after the service acknowledges its own exit —
        // cannot release this hold on the very next probe.
        _sawEngineNotRunning = false;
        _heldProbeTicks      = 0;

        _ipc.HoldReconnect();

        Dashboard.MarkStopped();
        RaiseConnectionLabels();
    }

    /// <summary>The user asked for the engine to be started. Let the loop try.</summary>
    public void NoteEngineStartRequested()
    {
        _ipc.ReleaseReconnect();
        RaiseConnectionLabels();
    }

    /// <summary>
    /// What the watermark feature last actually achieved, or verified it
    /// achieved. Surfaced in Settings so that "could not hide it" is visible
    /// rather than silent — a failure this app cannot explain is worse than
    /// one it reports.
    /// </summary>
    public string WatermarkStatus
    {
        get => _watermarkStatus;
        private set => SetProperty(ref _watermarkStatus, value);
    }

    /// <summary>
    /// Apply or remove the "Test Mode" desktop watermark.
    ///
    /// Runs off the UI thread on purpose: it injects into explorer and, when
    /// verifying, captures the screen and sleeps to let the shell repaint.
    /// Neither belongs on the thread drawing the window.
    /// </summary>
    private async Task ReconcileWatermarkAsync()
    {
        try
        {
            bool wanted = Settings.WatermarkSuppressed;

            var report = await Task.Run(() =>
            {
                // Switching it off takes the hook back out now, rather than
                // leaving the watermark hidden until the shell next restarts.
                if (!wanted) _watermark.Disable();
                return _watermark.EnsureApplied(wanted);
            });

            WatermarkStatus = report.Summary;
        }
        catch (Exception ex)
        {
            // Called from the poll, so anything thrown here would become an
            // unobserved task exception and disappear.
            System.Diagnostics.Debug.WriteLine($"[ShieldCord] watermark reconcile failed: {ex}");
        }
    }

    public async ValueTask DisposeAsync()
    {
        _poll.Stop();
        SetupPage.Stop();
        _ipc.AlertReceived -= OnAlertReceived;
        _ipc.StateChanged   -= OnStateChanged;
        await _ipc.DisposeAsync();
    }

    public void Navigate(string page)
    {
        CurrentPage = page;
        CurrentView = page switch
        {
            "Alerts"   => AlertsPage,
            "Apps"     => AppsPage,
            "Settings" => SettingsPage,
            "Help"     => HelpPage,
            _          => Dashboard,
        };

        // Landing on Activity is what "seen" means for the badge. The counts are
        // recomputed here too: they are otherwise only refreshed when the feed
        // changes, which leaves the header reading "0 of 124" on a page whose
        // list is plainly full.
        if (page == "Alerts")
        {
            UnreadCount = 0;
            AlertsPage.RaiseCounts();
        }

        // The detail lists are only fetched when the page that shows them is
        // actually opened — they cost a process scan server-side and are rarely
        // looked at.
        if (page == "Settings")  _ = SettingsPage.ReloadAsync();
        if (page == "Apps")      _ = LoadDetailsAsync();

        // The traffic chart's window is anchored to "now", so it is stale the
        // moment the page is left sitting. Recompute on the way back in.
        if (page == "Dashboard") Dashboard.OnActivated();
    }

    /// <summary>
    /// Fetch the two detail lists the Protected Apps and Settings pages render.
    ///
    /// Both are expensive on the service side — BuildProtectedAppsJson walks the
    /// whole process list — so this runs once per connection and whenever a page
    /// that shows them is opened. It is never called from the status poll.
    /// </summary>
    public async Task LoadDetailsAsync()
    {
        if (_ipc.State != ConnectionState.Connected) return;

        var apps  = await _ipc.GetProtectedAppsAsync();
        var paths = await _ipc.GetProtectedPathsAsync();

        Replace(ProtectedApps, apps);
        Replace(ProtectedPaths, paths);

        AppsPage.OnDetailsChanged();
    }

    private static void Replace<T>(ObservableCollection<T> target, IReadOnlyList<T> source)
    {
        target.Clear();
        foreach (T item in source) target.Add(item);
    }

    // ── polling ──────────────────────────────────────────────

    private async void OnPollTick(object? sender, EventArgs e)
    {
        // Runs on the UI thread (DispatcherTimer), and every await below
        // resumes here because WPF installs a SynchronizationContext — so the
        // view models are only ever mutated from the UI thread.
        try
        {
            // Shell state, not engine state — so this runs BEFORE the early
            // returns below, and a parked or stopped engine does not stop the
            // watermark being looked after.
            if (++_watermarkTicks >= WatermarkEvery)
            {
                _watermarkTicks = 0;
                await ReconcileWatermarkAsync();
            }

            if (_ipc.State == ConnectionState.Connected && !_ipc.ReconnectHeld)
            {
                await RefreshAsync();
                return;
            }

            // Not connected. Reconcile the reconnect hold with the SCM — this is
            // what parks the app when the engine is stopped and what un-parks it
            // when the engine comes back. It runs whether or not we are already
            // held, so a stop whose reply was lost still ends up parked instead
            // of retrying forever.
            if (++_engineStateTicks >= StatePollEvery)
            {
                _engineStateTicks = 0;
                SyncEngineHold(await Task.Run(ServiceControl.Query), conclusive: false);
                RaiseConnectionLabels();
            }

            if (!_ipc.ReconnectHeld) return;

            // While parked, also try the pipe directly now and then. The SCM
            // cannot see a console-mode engine — reachable over the pipe with no
            // service registered at all — and a pipe that answers is direct
            // evidence the engine is up whatever the SCM thinks.
            if (++_heldProbeTicks < ProbeEvery) return;
            _heldProbeTicks = 0;

            if (await _ipc.ProbeAsync(TimeSpan.FromMilliseconds(400)))
            {
                _ipc.ReleaseReconnect();
                RaiseConnectionLabels();
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[ShieldCord] poll failed: {ex}");
        }
    }

    /// <summary>
    /// Re-read the engine's status once.
    ///
    /// Shared by the poll timer and the manual Refresh action, so a refresh the
    /// user asked for produces exactly the same state as a tick — there is no
    /// second path that could drift from the first.
    /// </summary>
    public async Task RefreshAsync()
    {
        if (_ipc.State != ConnectionState.Connected) return;

        // A privileged change is a UAC round trip that takes hundreds of
        // milliseconds. Applying a status reply that was read BEFORE it landed
        // would overwrite the value the user just set and make the switch
        // visibly spring back to where it was.
        if (Busy) return;

        var status = await _ipc.GetStatusAsync();
        if (status is null) return;

        NoteEngineRestart(status);
        Dashboard.Apply(status);
    }

    private void OnStateChanged(ConnectionState state)
    {
        _dispatcher.InvokeAsync(() =>
        {
            Connection = state;
            if (state == ConnectionState.Disconnected)
            {
                // Do not leave a green dashboard on screen for a service that
                // is not there. The engine going away is exactly the moment the
                // user most needs to be told the truth.
                Dashboard.MarkUnreachable();
            }

            AppsPage.OnConnectionChanged();
            AlertsPage.OnConnectionChanged();
            SettingsPage.OnConnectionChanged();

            // The driver's own state can move with the engine's: after a restart
            // the service loads and arms it, so "installed but not running"
            // becomes "running" without anything in this app having acted.
            _ = RefreshFilterStateAsync();

            if (state == ConnectionState.Connected) _ = OnConnectedAsync();
        });
    }

    /// <summary>
    /// Everything that has to happen on a fresh connection, in order.
    ///
    /// Status comes FIRST so a restarted engine can be detected before its
    /// history is merged. Its alert ids begin at 1 again, so folding them into
    /// the previous run's records would collide on id and misdescribe both.
    /// </summary>
    private async Task OnConnectedAsync()
    {
        var status = await _ipc.GetStatusAsync();
        if (status is not null)
        {
            NoteEngineRestart(status);
            Dashboard.Apply(status);
        }

        await BackfillAlertsAsync();

        // Once per connection, so the Protected Apps page has content the first
        // time it is opened rather than being empty until the user happens to
        // visit Settings. Deliberately not per poll.
        await LoadDetailsAsync();
    }

    /// <summary>
    /// A smaller uptime than the previous reading means the engine restarted.
    ///
    /// That is not a cosmetic detail: AlertHistory ids begin at 1 again in a new
    /// engine process, so its fresh events would collide with the previous run's
    /// ids, and the feed would spend its life skipping real events as duplicates
    /// of ones that no longer exist. A restart is a new history, so the old one
    /// is dropped rather than blended.
    /// </summary>
    private void NoteEngineRestart(StatusSnapshot status)
    {
        if (_lastUptime >= 0 && status.UptimeSeconds < _lastUptime)
        {
            Alerts.Clear();
            _lastAlertId = 0;

            // Ids restart at 1 with the engine, so a set remembered from the
            // previous run would suppress unrelated records that happen to reuse
            // an id. The records it was hiding died with the old engine anyway.
            if (_dismissedAlertIds.Count > 0)
            {
                _dismissedAlertIds.Clear();
                PersistDismissedAlerts();
            }

            AlertsPage.RaiseCounts();
        }

        _lastUptime = status.UptimeSeconds;
    }

    private void OnAlertReceived(AlertRecord record)
    {
        _dispatcher.InvokeAsync(() =>
        {
            MergeAlert(record);

            // Only traffic that arrived while the user was somewhere else counts
            // as unseen. The reconnect backfill goes straight to MergeAlert and
            // deliberately does not come through here, so reconnecting does not
            // light the bell up with history the user already lived through.
            if (CurrentPage != "Alerts") UnreadCount++;

            LiveAlert?.Invoke(record);
        });
    }

    /// <summary>
    /// Pull history the client missed (first connect, or a reconnect after the
    /// engine restarted) so the timeline is not blank just because the window
    /// was open before the events happened.
    /// </summary>
    private async Task BackfillAlertsAsync()
    {
        try
        {
            var history = await _ipc.GetAlertsAsync(_lastAlertId);
            foreach (var rec in history) MergeAlert(rec);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[ShieldCord] backfill failed: {ex}");
        }
    }

    /// <summary>Insert newest-first, skipping ids already present.</summary>
    private void MergeAlert(AlertRecord record)
    {
        if (record.Id <= _lastAlertId && _lastAlertId != 0)
        {
            // The live broadcast and the history reply can overlap; dedupe on id.
            foreach (var existing in Alerts)
                if (existing.Id == record.Id) return;
        }

        if (record.Id > _lastAlertId) _lastAlertId = record.Id;

        // Dismissed detections must not come back. Without this check they would
        // return on the next full backfill — a UI restart re-reads `Since(0)`,
        // and "remove" that undoes itself on relaunch is worse than no remove.
        if (_dismissedAlertIds.Contains(record.Id)) return;

        Alerts.Insert(0, record);
        while (Alerts.Count > MaxAlertsInList) Alerts.RemoveAt(Alerts.Count - 1);

        // Only the activity page follows the live feed now. The dashboard's
        // charts are rebuilt from the status poll instead, so that every figure
        // on that page comes from one place and cannot contradict the others.
        AlertsPage.OnAlertSeen(record);
    }

    // ── dismissing detections ────────────────────────────────

    /// <summary>
    /// Detections the user has removed from the timeline.
    ///
    /// UI-SIDE ONLY, and deliberately so. The engine keeps its own record: this
    /// hides an entry from the list, it does not erase evidence. That matters for
    /// a tool whose whole value is being the account of what happened — and it
    /// means the "protection events" figure does not drop when someone tidies the
    /// list, because that number is the engine's count of what it did, not a
    /// count of what is currently on screen.
    ///
    /// Ids are only unique WITHIN one run of the engine (they restart at 1), which
    /// is why <see cref="NoteEngineRestart"/> clears this set — keeping it would
    /// suppress unrelated records that happened to reuse an id.
    /// </summary>
    private readonly HashSet<long> _dismissedAlertIds = new();

    /// <summary>
    /// Bounded by the engine's own history cap. The engine keeps at most 500
    /// records, so remembering more dismissed ids than that can never hide
    /// anything that could still arrive — and unbounded growth in a settings file
    /// is how a config quietly becomes a problem.
    /// </summary>
    private const int MaxDismissedIds = 500;

    public bool HasDismissedAlerts => _dismissedAlertIds.Count > 0;

    /// <summary>Remove one detection from the timeline.</summary>
    public void DismissAlert(long id)
    {
        if (id <= 0) return;
        if (!_dismissedAlertIds.Add(id)) return;

        // Oldest ids are the ones least likely to reappear, so drop them first
        // once the cap is reached.
        while (_dismissedAlertIds.Count > MaxDismissedIds)
            _dismissedAlertIds.Remove(_dismissedAlertIds.Min());

        for (int i = 0; i < Alerts.Count; i++)
        {
            if (Alerts[i].Id != id) continue;
            Alerts.RemoveAt(i);
            break;
        }

        PersistDismissedAlerts();
        AlertsPage.RaiseCounts();
    }

    /// <summary>
    /// Bring back everything the user dismissed.
    ///
    /// The record comes from the engine, so this needs no stored copy — the next
    /// backfill re-reads whatever is still in its history. Anything the engine has
    /// since dropped is genuinely gone, which is the honest outcome rather than a
    /// promise the app cannot keep.
    /// </summary>
    public void RestoreDismissedAlerts()
    {
        if (_dismissedAlertIds.Count == 0) return;

        _dismissedAlertIds.Clear();
        PersistDismissedAlerts();

        // Re-read the engine's history from the beginning so the restored records
        // actually come back.
        _lastAlertId = 0;
        _ = BackfillAlertsAsync();
    }

    private void PersistDismissedAlerts()
    {
        Settings.DismissedAlertIds = _dismissedAlertIds.ToList();
        Settings.Save();
        OnPropertyChanged(nameof(HasDismissedAlerts));
    }

    private void LoadDismissedAlerts()
    {
        foreach (long id in Settings.DismissedAlertIds.Where(id => id > 0))
            _dismissedAlertIds.Add(id);
    }


    // ── privileged actions ───────────────────────────────────

    /// <summary>
    /// Run one state-changing verb through a short-lived elevated copy of this
    /// app, and interpret the engine's reply.
    /// </summary>
    /// <param name="timeout">
    /// How long to wait for the elevated child. Defaults to the standard budget,
    /// which suits every verb that answers from memory. Installing the driver
    /// passes its own, because it runs bcdedit and then an installer that waits
    /// up to a minute for the driver to reach RUNNING.
    /// </param>
    public async Task<PrivilegedOutcome> RunPrivilegedAsync(string verb,
                                                            Action<JsonObject>? fill = null,
                                                            TimeSpan? timeout = null)
    {
        var request = new JsonObject { ["type"] = verb };
        fill?.Invoke(request);

        Busy = true;
        try
        {
            var result = await ElevatedHelper.RunAsync(request.ToJsonString(), timeout: timeout);
            if (result.Cancelled) return new PrivilegedOutcome(false, true, null);
            if (!result.Started)  return new PrivilegedOutcome(false, false, null);

            // Carry the raw reply alongside the interpretation: verbs that report
            // their own outcome in extra fields (the driver install) need it, and
            // re-parsing from the caller would duplicate this decision.
            return Interpret(result.ReplyJson) with { ReplyJson = result.ReplyJson };
        }
        finally
        {
            Busy = false;
        }
    }

    private static PrivilegedOutcome Interpret(string? replyJson)
    {
        /*
         * An empty reply is NOT success.
         *
         * The engine always answers an authorized verb, so silence means the
         * command never reached it — the service is stopped, an older build
         * that does not know the verb, or the elevated helper could not
         * connect. Reporting that as success is exactly how a switch ends up
         * sitting in the "on" position over a driver that is still fail-open,
         * which is the one lie this UI must never tell.
         */
        if (string.IsNullOrWhiteSpace(replyJson))
            return new PrivilegedOutcome(false, false, null);

        try
        {
            using var doc = JsonDocument.Parse(replyJson);
            var root = doc.RootElement;
            if (!root.TryGetProperty("type", out var typeEl)) return PrivilegedOutcome.Success;

            if (typeEl.GetString() == Types.KillResult)
            {
                // A kill_result answers the REQUEST with "ok" even when the
                // termination itself failed — the outcome is in its own
                // "success" field. Reading only the envelope would report every
                // failed kill as a success, which is the one thing a tool that
                // just claimed to end a threat must not do.
                bool ok = !root.TryGetProperty("success", out var okEl)
                       || okEl.ValueKind != JsonValueKind.False;

                return ok ? PrivilegedOutcome.Success
                          : new PrivilegedOutcome(false, false, Errors.KillFailed);
            }

            if (typeEl.GetString() == ServiceControl.FailureReplyType &&
                root.TryGetProperty("message", out var msgEl))
            {
                // A UI-local action reporting its own failure. Carry the sentence
                // through untouched rather than dressing it as an engine error.
                return new PrivilegedOutcome(false, false, null, msgEl.GetString());
            }

            if (typeEl.GetString() != Types.Error) return PrivilegedOutcome.Success;

            string? code = root.TryGetProperty("error", out var errEl) ? errEl.GetString() : null;
            return new PrivilegedOutcome(false, false, code);
        }
        catch (JsonException)
        {
            return PrivilegedOutcome.Success;
        }
    }

    /// <summary>Tell the user a change did not stick, without a modal dialog.</summary>
    public void ReportActionFailure(string label, PrivilegedOutcome outcome)
    {
        BannerIsError = true;
        Banner = $"{label}: {outcome.Describe()}";
    }

    public void ReportSuccess(string message)
    {
        BannerIsError = false;
        Banner = message;
    }

    public void ClearBanner()
    {
        Banner = "";
    }

    /// <summary>Re-read config after a privileged change so the UI matches reality.</summary>
    public async Task RefreshConfigAsync()
    {
        var cfg = await _ipc.GetConfigAsync();
        if (cfg is not null) Dashboard.ApplyConfig(cfg);
        await SettingsPage.ReloadAsync();
    }
}
