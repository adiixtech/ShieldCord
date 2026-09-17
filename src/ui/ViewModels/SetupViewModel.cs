using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Windows.Threading;
using ShieldCordUI.Mvvm;
using ShieldCordUI.Services;

namespace ShieldCordUI.ViewModels;

/// <summary>How one line of the setup checklist reads.</summary>
public enum SetupStepState
{
    /// <summary>Not reached yet.</summary>
    Pending,

    /// <summary>Verified good.</summary>
    Done,

    /// <summary>This is the step that needs the user now, and it can proceed.</summary>
    Active,

    /// <summary>Cannot proceed until something outside the app changes.</summary>
    Blocked,

    /// <summary>Attempted and did not work. Needs diagnosing, not repeating.</summary>
    Failed,
}

/// <summary>One line of the setup checklist.</summary>
public sealed record SetupStep(
    int Number,
    string Title,
    string Status,
    SetupStepState State,
    string Detail = "",
    string ActionText = "")
{
    /// <summary>Only the step the user is on carries an explanation; the rest stay
    /// one line each, which is what keeps this from reading as a wall of text.</summary>
    public bool ShowsDetail => Detail.Length > 0;

    public bool HasAction => ActionText.Length > 0;
}

/// <summary>What the single action button does for the current step.</summary>
internal enum SetupAction
{
    None,
    OpenSecuritySettings,
    OpenHelp,
    Install,
    Restart,
    CancelRestart,
}

/// <summary>
/// Which of setup's three screens is showing.
///
/// Setup takes over the whole window rather than sitting in a page, because it is
/// a thing you complete rather than a place you visit — the sidebar, the status
/// and the navigation are all noise until it is done.
/// </summary>
public enum SetupStage
{
    /// <summary>The opening screen: what this is, and one button to start.</summary>
    Welcome,

    /// <summary>The checklist.</summary>
    Steps,

    /// <summary>Everything is up. One button into the dashboard.</summary>
    Complete,
}

/// <summary>
/// The setup page.
///
/// WHY THIS IS A PAGE AND NOT A DIALOG
///
///   Setup spans a reboot. A modal dialog cannot survive that — it would have to
///   be dismissed, and then the app has no way to say "now do this next". As a
///   page in the main window it is still there after the restart, showing what
///   happened and what is left.
///
/// WHY THE STATE IS DERIVED AND NOT STORED
///
///   Every launch rebuilds the checklist from what the machine actually reports —
///   Secure Boot, Memory Integrity, the driver's SCM state, and whether a restart
///   has happened since test signing was switched on. There is no "setup in
///   progress" flag to get out of step with reality. The only thing remembered is
///   WHEN test signing was enabled, which is a fact about the machine rather than
///   a claim about the UI.
/// </summary>
public sealed class SetupViewModel : ObservableObject
{
    private readonly ShellViewModel _shell;

    /// <summary>How long Windows waits before restarting. Long enough to save work.</summary>
    private const int RestartDelaySeconds = 60;

    private readonly DispatcherTimer _countdown;
    private int _secondsLeft;
    private bool _isRestarting;

    private SetupAction _action = SetupAction.None;
    private SetupStage _stage = SetupStage.Welcome;
    private string _headline = "";
    private string _summary = "";
    private bool _working;
    private bool _busy;

    /// <summary>Raised when setup is finished with and the window should hand back
    /// to the dashboard.</summary>
    public event Action? Finished;

    /// <summary>Raised when the user defers setup. The app remembers that so it
    /// does not reopen the takeover on every launch.</summary>
    public event Action? Deferred;

    public SetupViewModel(ShellViewModel shell)
    {
        _shell = shell;

        _countdown = new DispatcherTimer(DispatcherPriority.Normal)
        {
            Interval = TimeSpan.FromSeconds(1),
        };
        _countdown.Tick += OnCountdownTick;

        RunActionCommand = new AsyncRelayCommand(RunActionAsync, () => !_busy && _action != SetupAction.None);

        BeginCommand       = new RelayCommand(BeginSetup);
        DeferCommand       = new RelayCommand(Defer);
        AcknowledgeCommand = new RelayCommand(Acknowledge);

        Refresh();
    }

    public RelayCommand BeginCommand { get; }
    public RelayCommand DeferCommand { get; }
    public RelayCommand AcknowledgeCommand { get; }

    // ── state ────────────────────────────────────────────────

    public ObservableCollection<SetupStep> Steps { get; } = new();

    public AsyncRelayCommand RunActionCommand { get; }

    /// <summary>Which screen is showing.</summary>
    public SetupStage Stage
    {
        get => _stage;
        private set
        {
            if (!SetProperty(ref _stage, value)) return;
            OnPropertiesChanged(nameof(IsWelcome), nameof(IsSteps), nameof(IsComplete));
        }
    }

    public bool IsWelcome  => _stage == SetupStage.Welcome;
    public bool IsSteps    => _stage == SetupStage.Steps;
    public bool IsComplete => _stage == SetupStage.Complete;

    /// <summary>
    /// Open the takeover at a given screen. Called by the app on launch — either
    /// because the driver has never been set up, or because it just finished and
    /// the user has not seen the result yet.
    /// </summary>
    public void Open(SetupStage stage)
    {
        Stage = stage;

        // The checklist is derived, so it is rebuilt at the moment it is shown
        // rather than carried over from whenever it was last looked at.
        if (stage == SetupStage.Steps) Refresh();
    }

    /// <summary>The welcome screen's one button.</summary>
    public void BeginSetup()
    {
        Stage = SetupStage.Steps;
        Refresh();
    }

    /// <summary>Defer: leave setup without doing it. The dashboard keeps its own
    /// entry point, so this hides the takeover without hiding the way back.</summary>
    public void Defer() => Deferred?.Invoke();

    /// <summary>One line under the title saying where the machine is, not what to do.</summary>
    public string Summary
    {
        get => _summary;
        private set => SetProperty(ref _summary, value);
    }

    public string Headline
    {
        get => _headline;
        private set => SetProperty(ref _headline, value);
    }

    /// <summary>True while the install is running, which is the one long operation
    /// here — up to a minute. Drives the progress ring.</summary>
    public bool IsWorking
    {
        get => _working;
        private set { if (SetProperty(ref _working, value)) OnPropertyChanged(nameof(ShowProgress)); }
    }

    public bool ShowProgress => _working;

    /// <summary>True once a restart has been requested — switches the action row
    /// to the live countdown.</summary>
    public bool IsRestarting
    {
        get => _isRestarting;
        private set
        {
            if (!SetProperty(ref _isRestarting, value)) return;
            OnPropertyChanged(nameof(ShowCountdown));
            OnPropertyChanged(nameof(ShowAction));
        }
    }

    public bool ShowCountdown => _isRestarting;

    /// <summary>The action button steps aside while the countdown runs; the only
    /// thing to do then is cancel, which the countdown panel offers itself.</summary>
    public bool ShowAction => !_isRestarting && _action != SetupAction.None && !_working;

    public string ActionText { get; private set; } = "";

    public string CountdownText =>
        $"Restarting in {_secondsLeft / 60}:{_secondsLeft % 60:00}";

    // ── building the checklist ───────────────────────────────

    /// <summary>
    /// Re-read the machine and rebuild the checklist. Called on construction, when
    /// the page is opened, and after every action.
    /// </summary>
    public void Refresh()
    {
        // Readable unelevated on purpose: every blocking condition must be found
        // BEFORE a UAC prompt is spent on an install that cannot succeed.
        bool secureBootOn = DriverSetup.SecureBootEnabled;
        bool memoryIntegrityOn = DriverSetup.MemoryIntegrityEnabled;

        var filter = DriverSetup.Query();
        bool loaded = filter == DriverSetup.FilterState.Running;
        bool registered = loaded || filter == DriverSetup.FilterState.Stopped;

        bool rebootedSince = RebootedSinceEnablingTestSigning();
        bool waitingForRestart = registered && !loaded && !rebootedSince;

        Steps.Clear();

        // 1 — the app itself. Always true; it is here so the list reads as a
        // checklist with a beginning rather than starting mid-problem.
        Steps.Add(new SetupStep(
            1, "Application", "Installed", SetupStepState.Done));

        // 2 — Secure Boot. Firmware, so there is nothing the app can do but be clear.
        Steps.Add(secureBootOn
            ? new SetupStep(
                2, "Secure Boot", "On — blocking", SetupStepState.Blocked,
                "Windows will not load a driver Microsoft has not signed while Secure Boot is on. "
                + "Turn it off in your PC's firmware settings, then come back.",
                "What to do")
            : new SetupStep(
                2, "Secure Boot", "Off", SetupStepState.Done));

        // 3 — Memory Integrity. The one that fails silently without this check.
        Steps.Add(memoryIntegrityOn
            ? new SetupStep(
                3, "Memory Integrity", "On — blocking", SetupStepState.Blocked,
                "Windows refuses drivers it has not signed even in test mode, so turning on test "
                + "signing will not be enough. Turn Memory Integrity off, then restart.",
                "Open Windows Security")
            : new SetupStep(
                3, "Memory Integrity", "Off", SetupStepState.Done));

        // 4 — test signing and the driver, which are one step because they are one
        // action: enabling test signing with nothing to load would be pointless.
        if (loaded)
        {
            Steps.Add(new SetupStep(
                4, "Test signing & driver", "Installed and running", SetupStepState.Done));
        }
        else if (waitingForRestart)
        {
            Steps.Add(new SetupStep(
                4, "Test signing & driver", "Installed — waiting for a restart", SetupStepState.Done));
        }
        else if (registered)
        {
            // Registered, not loaded, and a restart has already happened. Repeating
            // the install would change nothing, so this says so instead.
            Steps.Add(new SetupStep(
                4, "Test signing & driver", "Not loading", SetupStepState.Failed,
                "Windows was expected to load the driver after the restart but has not. "
                + "Event Viewer → Windows Logs → System will have a CodeIntegrity entry naming the reason."));
        }
        else
        {
            Steps.Add(new SetupStep(
                4, "Test signing & driver", "Not set up", SetupStepState.Active,
                "Switches test signing on so Windows will load ShieldCord's driver, then installs it. "
                + "Test signing lowers this PC's kernel security — that is the trade for a driver "
                + "Microsoft has not signed.",
                "Set up"));
        }

        // 5 — the restart, only when one is actually needed.
        Steps.Add(waitingForRestart
            ? new SetupStep(
                5, "Restart", "Required", SetupStepState.Active,
                "Windows only reads the test-signing setting while booting, so the driver loads "
                + "the next time this PC starts.",
                "Restart now")
            : new SetupStep(
                5, "Restart", "Not needed", SetupStepState.Done));

        DecideAction(secureBootOn, memoryIntegrityOn, loaded, registered, waitingForRestart);

        // The driver is up and the steps are on screen — there is nothing left to
        // show, so move to the closing screen. Guarded on the current stage so this
        // only ever happens while the checklist is being worked through, and never
        // pulls someone back out of the dashboard to say "finished".
        if (loaded && Stage == SetupStage.Steps) Stage = SetupStage.Complete;
    }

    /// <summary>
    /// Pick the one thing the user should do now, and the sentence above it.
    ///
    /// Order matters: a blocked precondition outranks an actionable step, because
    /// offering "Set up" on a machine where Memory Integrity will refuse the driver
    /// spends a UAC prompt to produce a failure.
    /// </summary>
    private void DecideAction(bool secureBootOn, bool memoryIntegrityOn,
                              bool loaded, bool registered, bool waitingForRestart)
    {
        if (secureBootOn)
        {
            // The step row carries the "What to do" button, so the action has to
            // match it — a row button with nothing behind it is worse than none.
            SetAction(SetupAction.OpenHelp, "What to do");
            Headline = "Secure Boot is blocking setup";
            Summary = "Turn it off in your PC's firmware settings, then reopen this page.";
            return;
        }

        if (memoryIntegrityOn)
        {
            SetAction(SetupAction.OpenSecuritySettings, "Open Windows Security");
            Headline = "Memory Integrity is blocking setup";
            Summary = "Turn it off, restart, then reopen this page.";
            return;
        }

        if (loaded)
        {
            SetAction(SetupAction.None, "");
            Headline = "Protection is on";
            Summary = "The driver is running and tokens are protected.";
            return;
        }

        if (waitingForRestart)
        {
            SetAction(SetupAction.Restart, "Restart now");
            Headline = "Restart to finish";
            Summary = "The driver is installed. Windows loads it at the next start.";
            return;
        }

        if (registered)
        {
            // Registered, restarted, still not up. The steps above already say why
            // this is unexpected, so there is no action to offer — repeating the
            // install would not help and would only churn.
            SetAction(SetupAction.None, "");
            Headline = "The driver is not loading";
            Summary = "Windows refused it after the restart. See step 4 for where to look.";
            return;
        }

        SetAction(SetupAction.Install, "Set up");
        Headline = "Set up protection";
        Summary = "Two Windows settings have to allow it first — both are already checked above.";
    }

    private void SetAction(SetupAction action, string text)
    {
        _action = action;
        ActionText = text;

        OnPropertyChanged(nameof(ActionText));
        OnPropertyChanged(nameof(ShowAction));
        RunActionCommand.RaiseCanExecuteChanged();
    }

    /// <summary>
    /// Whether the machine has restarted since ShieldCord enabled test signing.
    ///
    /// Derived from uptime rather than stored as a "we asked for a restart" flag,
    /// so a restart the user did on their own — for any reason, days later — is
    /// recognised just the same. TickCount64 is milliseconds since boot and needs
    /// no privilege, which matters because reading the setting it refers to would.
    /// </summary>
    private bool RebootedSinceEnablingTestSigning()
    {
        if (_shell.Settings.TestSigningEnabledAt is not { } enabledUtc) return false;

        DateTime bootUtc = DateTime.UtcNow - TimeSpan.FromMilliseconds(Environment.TickCount64);
        return bootUtc > enabledUtc;
    }

    // ── the action ───────────────────────────────────────────

    private async Task RunActionAsync()
    {
        if (_busy) return;
        _busy = true;
        RunActionCommand.RaiseCanExecuteChanged();

        try
        {
            switch (_action)
            {
                case SetupAction.OpenHelp:
                    _shell.Navigate("Help");
                    break;

                case SetupAction.OpenSecuritySettings:
                    // Falls back to the written path so a refused URI never leaves
                    // the user staring at a button that does nothing.
                    if (!DriverSetup.OpenMemoryIntegritySettings())
                    {
                        Summary = "Open Windows Security → Device Security → Core Isolation, "
                                + "turn Memory Integrity off, then restart.";
                    }
                    break;

                case SetupAction.Install:
                    await InstallAsync();
                    break;

                case SetupAction.Restart:
                    RequestRestart();
                    break;

                case SetupAction.CancelRestart:
                    CancelRestart();
                    break;
            }
        }
        finally
        {
            _busy = false;
            RunActionCommand.RaiseCanExecuteChanged();
        }
    }

    /// <summary>Close the takeover and go back to the dashboard.</summary>
    public void Acknowledge()
    {
        // Pause, not Stop: setup can legitimately be reopened (the dashboard's
        // button goes straight back to the checklist), and unsubscribing the timer
        // here would leave the next countdown frozen.
        _countdown.Stop();
        IsRestarting = false;
        Finished?.Invoke();
    }

    /// <summary>
    /// Install the driver, enabling test signing first.
    ///
    /// One elevated child does the whole thing, so this is one UAC prompt rather
    /// than one per setting. It cannot succeed at loading the driver yet — test
    /// signing only applies at boot — so a restart follows, and that is expected
    /// rather than a failure.
    /// </summary>
    private async Task InstallAsync()
    {
        IsWorking = true;
        Summary = "Approve the Administrator prompt. This takes a moment.";

        // Longer than the default privileged-call budget: this runs bcdedit and then
        // an installer that by itself waits up to a minute for the driver to reach
        // RUNNING. A timeout here would kill a working install and report the
        // failure as its own.
        var outcome = await _shell.RunPrivilegedAsync(
            DriverSetup.InstallRequestToken, timeout: TimeSpan.FromMinutes(3));

        var report = DriverSetup.ParseReport(outcome.ReplyJson);

        // Record when test signing was switched on, so a later launch can tell
        // "waiting for the restart" from "restarted and still broken".
        if (report is { TestSigningWasOff: true, TestSigningNowOn: true })
        {
            _shell.Settings.TestSigningEnabledAt = DateTime.UtcNow;
            _shell.Settings.Save();
        }

        IsWorking = false;

        await _shell.RefreshFilterStateAsync();
        Refresh();

        if (outcome.Cancelled)
        {
            Summary = "Setup was cancelled. Nothing was changed.";
            return;
        }

        if (report is null)
        {
            Summary = "ShieldCord could not tell what happened, so it will not claim the driver is installed.";
            return;
        }

        // The steps above now report the truth; this line only adds the one thing
        // they cannot say, which is what to do about it.
        if (!report.DriverLoaded && !report.EngineRearmed && report.DriverRegistered)
            Summary = "Installed. Restart to let Windows load the driver.";
    }

    // ── restart ──────────────────────────────────────────────

    /// <summary>
    /// Ask Windows to restart after a delay rather than immediately.
    ///
    /// The delay is the point: a setup screen that reboots without warning loses
    /// whatever the user has open. Windows shows its own notice and the countdown
    /// here offers a cancel, so there are two ways to stop it.
    /// </summary>
    private void RequestRestart()
    {
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = "shutdown.exe",
                Arguments = $"/r /t {RestartDelaySeconds} /c \"ShieldCord is finishing setup\"",
                UseShellExecute = false,
                CreateNoWindow = true,
            });

            _secondsLeft = RestartDelaySeconds;
            IsRestarting = true;
            OnPropertyChanged(nameof(CountdownText));
            _countdown.Start();

            SetAction(SetupAction.CancelRestart, "Cancel restart");
            Summary = "Save anything you are working on. ShieldCord finishes by itself "
                    + "when Windows comes back.";
        }
        catch (Exception ex)
        {
            // Say so rather than pretend. A machine that never restarts would leave
            // the driver unloaded with no explanation.
            Summary = "Windows would not schedule the restart. Please restart this PC yourself "
                    + "to finish setup. " + ex.Message;
        }
    }

    private void CancelRestart()
    {
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = "shutdown.exe",
                Arguments = "/a",
                UseShellExecute = false,
                CreateNoWindow = true,
            });
        }
        catch
        {
            // If the abort failed the restart is still scheduled, and the countdown
            // below keeps telling the truth about that.
        }

        _countdown.Stop();
        IsRestarting = false;
        Refresh();
    }

    private void OnCountdownTick(object? sender, EventArgs e)
    {
        if (--_secondsLeft > 0)
        {
            OnPropertyChanged(nameof(CountdownText));
            return;
        }

        _countdown.Stop();
        IsRestarting = false;
        OnPropertyChanged(nameof(CountdownText));
    }

    public void Stop()
    {
        _countdown.Stop();
        _countdown.Tick -= OnCountdownTick;
    }
}
