using System.Collections.ObjectModel;
using ShieldCordUI.Mvvm;
using ShieldCordUI.Protocol;
using ShieldCordUI.Services;

namespace ShieldCordUI.ViewModels;

/// <summary>
/// Settings: what ShieldCord protects (read from the engine) and how this app
/// behaves (local preferences).
///
/// The two halves have different costs on purpose. Appearance and tray options
/// are local and instant, because making the user pass a UAC prompt to change
/// a theme would be absurd. Anything that changes what PROTECTION does is
/// privileged and goes through the elevated path.
/// </summary>
public sealed class SettingsViewModel : ObservableObject
{
    private readonly ShellViewModel _shell;
    private bool _loaded;

    private bool _startWithWindows;
    private bool _minimizeToTrayOnClose;
    private bool _muteAllAlertNotifications;
    private bool _notifyCriticalOnly;
    private bool _watermarkSuppressed;
    private string _theme;

    private ProtectedApp? _selectedPublisher;
    private bool _busy;
    private string _status = "";

    /// <summary>What the SCM says about the engine. Only consulted while the
    /// pipe is down — that is the only time the app needs to know.</summary>
    private ServiceControl.EngineState _serviceState = ServiceControl.EngineState.Unknown;
    private bool _startingEngine;

    public SettingsViewModel(ShellViewModel shell)
    {
        _shell = shell;

        var s = shell.Settings;
        _startWithWindows       = s.StartWithWindows;
        _minimizeToTrayOnClose  = s.MinimizeToTrayOnClose;
        _muteAllAlertNotifications = s.MuteAllAlertNotifications;
        _notifyCriticalOnly        = s.NotifyCriticalOnly;
        _watermarkSuppressed    = s.WatermarkSuppressed;
        _theme                  = s.Theme;
        _loaded = true;

        // The shell owns the watermark work — it has to run off the UI thread
        // and be tied to the poll — so this page mirrors its result rather
        // than doing any of it.
        _shell.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(ShellViewModel.WatermarkStatus))
                OnPropertyChanged(nameof(WatermarkStatus));
        };

        UntrustCommand   = new AsyncRelayCommand(p => UntrustAsync(p as ProtectedApp));
        DisarmCommand    = new AsyncRelayCommand(DisarmAsync);
        ReArmCommand     = new AsyncRelayCommand(ReArmAsync);
        ShutdownCommand  = new AsyncRelayCommand(ShutdownAsync);
        StartEngineCommand = new AsyncRelayCommand(StartEngineAsync);
        SetThemeCommand  = new RelayCommand(p => Theme = p as string ?? "system");
    }

    public ObservableCollection<ProtectedApp>  BuiltinApps       { get; } = new();
    public ObservableCollection<ProtectedApp>  TrustedPublishers { get; } = new();
    public ObservableCollection<ProtectedPath> ProtectedPaths    { get; } = new();

    public AsyncRelayCommand UntrustCommand  { get; }
    public AsyncRelayCommand DisarmCommand   { get; }
    public AsyncRelayCommand ReArmCommand    { get; }
    public AsyncRelayCommand ShutdownCommand { get; }
    public AsyncRelayCommand StartEngineCommand { get; }
    public RelayCommand      SetThemeCommand { get; }

    /// <summary>Set by the view so the shell can swap its resource dictionary.</summary>
    public Action<string>? ThemeChanged { get; set; }

    /// <summary>Set by the view; used before the two destructive actions below.</summary>
    public Func<string, string, bool>? Confirm { get; set; }

    // ── local preferences (no elevation) ─────────────────────

    public bool StartWithWindows
    {
        get => _startWithWindows;
        set
        {
            if (!SetProperty(ref _startWithWindows, value)) return;
            if (!_loaded) return;

            _shell.Settings.StartWithWindows = value;
            _shell.Settings.Save();

            // Report honestly if the registry write did not take, rather than
            // leaving a checkbox that lies about what will happen at login.
            if (StartupRegistration.Set(value) &&
                StartupRegistration.IsEnabled() == value)
            {
                Status = value ? "ShieldCord will start with Windows." : "ShieldCord will not start with Windows.";
            }
            else
            {
                Status = "Could not update the startup entry.";
            }
        }
    }

    public bool MinimizeToTrayOnClose
    {
        get => _minimizeToTrayOnClose;
        set
        {
            if (!SetProperty(ref _minimizeToTrayOnClose, value)) return;
            if (!_loaded) return;
            _shell.Settings.MinimizeToTrayOnClose = value;
            _shell.Settings.Save();
        }
    }

    /// <summary>
    /// Hide the Windows "Test Mode" desktop watermark.
    ///
    /// Applied by the shell's status poll rather than from this setter: the
    /// work is a cross-process injection plus a screen capture to confirm it,
    /// and neither belongs on the UI thread. Turning it OFF actively removes
    /// the hook rather than waiting for the next shell restart.
    ///
    /// Deliberately NOT in the danger-zone card. It does not change what
    /// protection does and needs no elevation — the tray shares a session and
    /// an integrity level with the shell it draws into.
    /// </summary>
    public bool WatermarkSuppressed
    {
        get => _watermarkSuppressed;
        set
        {
            if (!SetProperty(ref _watermarkSuppressed, value)) return;
            if (!_loaded) return;
            _shell.Settings.WatermarkSuppressed = value;
            _shell.Settings.Save();
        }
    }

    /// <summary>
    /// The shell's live result for the watermark feature, forwarded so the card
    /// can show what actually happened — including "could not hide it", which
    /// must not be swallowed.
    /// </summary>
    public string WatermarkStatus => _shell.WatermarkStatus;

    /// <summary>
    /// Suppress every alert balloon. Defaults on — see AppSettings for why.
    /// </summary>
    public bool MuteAllAlertNotifications
    {
        get => _muteAllAlertNotifications;
        set
        {
            if (!SetProperty(ref _muteAllAlertNotifications, value)) return;

            // The critical-only switch is meaningless while everything is
            // muted, so the view greys it out; that has to be re-evaluated
            // whether or not the value came from the user.
            OnPropertyChanged(nameof(CanChooseNotificationDetail));

            if (!_loaded) return;
            _shell.Settings.MuteAllAlertNotifications = value;
            _shell.Settings.Save();
        }
    }

    /// <summary>
    /// Only balloon for critical detections. Inert while the master mute is on.
    /// </summary>
    public bool NotifyCriticalOnly
    {
        get => _notifyCriticalOnly;
        set
        {
            if (!SetProperty(ref _notifyCriticalOnly, value)) return;
            if (!_loaded) return;
            _shell.Settings.NotifyCriticalOnly = value;
            _shell.Settings.Save();
        }
    }

    /// <summary>
    /// Whether the critical-only switch can do anything. Bound to its
    /// IsEnabled so the card cannot offer a choice that has no effect.
    /// </summary>
    public bool CanChooseNotificationDetail => !_muteAllAlertNotifications;

    public string Theme
    {
        get => _theme;
        set
        {
            if (!SetProperty(ref _theme, value)) return;
            OnPropertiesChanged(nameof(IsThemeSystem), nameof(IsThemeLight), nameof(IsThemeDark));
            if (!_loaded) return;
            _shell.Settings.Theme = value;
            _shell.Settings.Save();
            ThemeChanged?.Invoke(value);
        }
    }

    // Read by the theme selector. One-way, because Theme is the single source
    // of truth — a two-way radio binding would fight the other two buttons.
    public bool IsThemeSystem => _theme == "system";
    public bool IsThemeLight  => _theme == "light";
    public bool IsThemeDark   => _theme == "dark";

    // ── shared ───────────────────────────────────────────────

    public ProtectedApp? SelectedPublisher
    {
        get => _selectedPublisher;
        set
        {
            if (!SetProperty(ref _selectedPublisher, value)) return;
            UntrustCommand.RaiseCanExecuteChanged();
        }
    }

    public bool Busy
    {
        get => _busy;
        private set { if (SetProperty(ref _busy, value)) OnPropertyChanged(nameof(IsInteractive)); }
    }

    public bool IsInteractive => !_busy;

    public string Status
    {
        get => _status;
        private set { if (SetProperty(ref _status, value)) OnPropertyChanged(nameof(HasStatus)); }
    }

    public bool HasStatus => !string.IsNullOrEmpty(_status);

    // ── the engine itself ────────────────────────────────────

    // The button that stops the engine used to sit there doing nothing once the
    // engine was down: the pipe is gone, so the verb could never be delivered,
    // and there was no way back short of services.msc. These properties drive a
    // control that is always honest about which of the two things it can do.

    /// <summary>
    /// True when the engine is genuinely down rather than mid-handshake.
    ///
    /// Not simply "!IsConnected": that is also true during the connecting
    /// window, and offering to start an engine that is in the middle of
    /// answering would put a pointless UAC prompt in front of the user.
    /// </summary>
    private bool EngineIsDown => _shell.Connection == ConnectionState.Disconnected;

    /// <summary>The pipe is up, so stopping is possible.</summary>
    public bool CanStopEngine => _shell.IsConnected && !_startingEngine;

    /// <summary>
    /// The pipe is down, so the engine may need starting.
    ///
    /// Keyed on the CONNECTION rather than on the SCM state on purpose: "the
    /// pipe is down" is also true while the engine is starting up, and in that
    /// window the user needs a control that does something rather than a dead
    /// one. The SCM state only refines whether starting is possible at all.
    /// </summary>
    public bool CanStartEngine =>
        EngineIsDown && !_startingEngine &&
        _serviceState != ServiceControl.EngineState.NotInstalled;

    public bool IsStartingEngine => _startingEngine;

    /// <summary>Set only when starting is impossible from here, so the page can
    /// say why rather than offering a button that cannot work.</summary>
    public bool EngineNotInstalled =>
        EngineIsDown && _serviceState == ServiceControl.EngineState.NotInstalled;

    /// <summary>One sentence describing the state the buttons are in.</summary>
    public string EngineStateText =>
        _shell.IsConnected ? "The engine is running."
      : _startingEngine ? "Asking Windows to start the engine…"
      : _serviceState == ServiceControl.EngineState.NotInstalled
            ? "The ShieldCord service is not installed on this PC."
      : !EngineIsDown ? "Connecting to the engine…"
      : "The engine is stopped. Starting it needs administrator approval.";

    /// <summary>
    /// Re-read the engine state and re-evaluate which button applies.
    ///
    /// Called whenever the pipe connection changes — that is what tells us the
    /// engine has gone down or come back. The SCM query behind it is a blocking
    /// Win32 call, so it runs off the UI thread.
    /// </summary>
    public void OnConnectionChanged()
    {
        RaiseEngineState();
        if (!_shell.IsConnected) _ = RefreshServiceStateAsync();
    }

    private async Task RefreshServiceStateAsync()
    {
        _serviceState = await Task.Run(ServiceControl.Query);
        RaiseEngineState();
    }

    private void RaiseEngineState() =>
        OnPropertiesChanged(nameof(CanStopEngine), nameof(CanStartEngine),
                            nameof(IsStartingEngine), nameof(EngineNotInstalled),
                            nameof(EngineStateText));

    /// <summary>
    /// Ask Windows to start the engine.
    ///
    /// This is the one privileged action that cannot travel over the pipe —
    /// there is no pipe when the engine is down — so it goes to the Service
    /// Control Manager through the elevated helper instead.
    /// </summary>
    private async Task StartEngineAsync()
    {
        if (_startingEngine) return;

        _startingEngine = true;
        RaiseEngineState();

        try
        {
            var outcome = await _shell.RunPrivilegedAsync(ServiceControl.StartRequestToken);

            // Let the connection loop start trying again. It stays honest about
            // what that means: the sidebar flips to "Engine connected" only when
            // the pipe actually lands.
            if (outcome.Ok) _shell.NoteEngineStartRequested();

            // On success this deliberately does NOT say "the engine is running".
            // The SCM has only ACCEPTED the start; the service still has to load
            // its config, open its pipe and arm the driver. The sidebar flips to
            // "Engine connected" when that has actually happened, and that — not
            // this line — is what gets to claim it.
            Status = outcome.Ok
                ? "Starting the engine. This takes a few seconds, and the connection will come back on its own."
                : outcome.Describe();
        }
        finally
        {
            _startingEngine = false;
            RaiseEngineState();
            _ = RefreshServiceStateAsync();
        }
    }

    // ── engine state ─────────────────────────────────────────

    public async Task ReloadAsync()
    {
        if (!_shell.IsConnected)
        {
            // Two different situations, two different sentences. Saying "not
            // reachable" about an engine the user just switched off would read
            // as a fault.
            Status = _shell.IsEngineStopped
                ? "The engine is stopped, so these lists cannot be read."
                : "The engine is not reachable.";
            return;
        }

        Busy = true;
        try
        {
            // One fetch, shared with the Protected Apps page. This used to call
            // the engine for both lists directly, which meant two process scans
            // on the service for what is one set of data.
            await _shell.LoadDetailsAsync();

            BuiltinApps.Clear();
            TrustedPublishers.Clear();
            foreach (var app in _shell.ProtectedApps)
            {
                if (app.Builtin) BuiltinApps.Add(app);
                else             TrustedPublishers.Add(app);
            }

            ProtectedPaths.Clear();
            foreach (var p in _shell.ProtectedPaths) ProtectedPaths.Add(p);

            OnPropertiesChanged(nameof(HasTrustedPublishers), nameof(HasNoTrustedPublishers));
            Status = "";
        }
        finally
        {
            Busy = false;
        }
    }

    public bool HasTrustedPublishers => TrustedPublishers.Count > 0;

    /// <summary>
    /// Gated on the connection as well as on the list: with the engine
    /// unreachable both collections are empty because nothing could be read,
    /// and "None added" would turn that into a positive claim about what the
    /// user has and has not trusted.
    /// </summary>
    public bool HasNoTrustedPublishers => TrustedPublishers.Count == 0 && _shell.IsConnected;

    private async Task UntrustAsync(ProtectedApp? pub)
    {
        pub ??= SelectedPublisher;
        if (pub is null) return;

        var outcome = await _shell.RunPrivilegedAsync(Verbs.UntrustPublisher,
            o => o["publisher"] = pub.Publisher);

        if (outcome.Ok)
        {
            Status = $"\"{pub.Publisher}\" removed. Processes already running keep their "
                   + "access until they exit.";
            await ReloadAsync();
        }
        else
        {
            Status = outcome.Describe();
        }
    }

    private async Task DisarmAsync()
    {
        if (Confirm is not null &&
            !Confirm("Turn protection off?",
                     "ShieldCord will stop blocking anyone from reading your Discord and "
                     + "browser tokens until you turn it back on.\n\n"
                     + "Do this only to diagnose a problem."))
        {
            return;
        }

        var outcome = await _shell.RunPrivilegedAsync(Verbs.SetEnforcement, o => o["enabled"] = false);
        Status = outcome.Ok ? "Protection is OFF." : outcome.Describe();
        if (outcome.Ok) await _shell.RefreshConfigAsync();
    }

    private async Task ReArmAsync()
    {
        var outcome = await _shell.RunPrivilegedAsync(Verbs.SetEnforcement, o => o["enabled"] = true);
        Status = outcome.Ok ? "Protection is ON." : outcome.Describe();
        if (outcome.Ok) await _shell.RefreshConfigAsync();
    }

    private async Task ShutdownAsync()
    {
        if (Confirm is not null &&
            !Confirm("Stop the ShieldCord engine?",
                     "This stops the background service. Your tokens will be unprotected and "
                     + "the tray icon will disconnect until the service is started again "
                     + "(for example by rebooting)."))
        {
            return;
        }

        var outcome = await _shell.RunPrivilegedAsync(Verbs.Shutdown);

        // Park the reconnect loop at once, so the UI settles into a steady
        // "Engine stopped" instead of cycling Connecting/offline. If the reply
        // is lost — the service can tear its pipe down before it reaches this
        // elevated child — the shell's own SCM reconciliation parks it a few
        // seconds later instead.
        if (outcome.Ok) _shell.NoteDeliberateStop();

        Status = outcome.Ok
            ? "The engine is stopping, and it will stay stopped until you start it again from here."
            : outcome.Describe();
    }
}
