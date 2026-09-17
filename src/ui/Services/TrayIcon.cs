using System.ComponentModel;
using System.Drawing;
using System.Windows.Forms;
using ShieldCordUI.Protocol;
using ShieldCordUI.ViewModels;

namespace ShieldCordUI.Services;

/// <summary>
/// The notification-area icon.
///
/// Its icon reflects real protection state (shield / warning / error) and its
/// menu drives the same switches as the dashboard — bound to the SAME view
/// model properties, so the tray and the window can never disagree about
/// whether enforcement is on.
/// </summary>
public sealed class TrayIcon : IDisposable
{
    private readonly NotifyIcon _notify;
    private readonly ShellViewModel _shell;

    private readonly ToolStripMenuItem _statusItem;
    private readonly ToolStripMenuItem _enforcementItem;
    private readonly ToolStripMenuItem _fileBlockItem;
    private readonly ToolStripMenuItem _alertsItem;
    private readonly ToolStripMenuItem _memoryItem;

    public event Action? OpenRequested;
    public event Action? ExitRequested;

    public TrayIcon(ShellViewModel shell)
    {
        _shell = shell;

        _statusItem = new ToolStripMenuItem("ShieldCord") { Enabled = false };

        // The handlers read Checked off the sender rather than capturing the
        // field: WinForms has already flipped it by the time Click fires, and
        // capturing a field assigned later in this constructor would leave the
        // compiler unable to prove it non-null.
        _enforcementItem = new ToolStripMenuItem("Default-deny enforcement");
        _enforcementItem.Click += (s, _) =>
            _shell.Dashboard.Enforcement = ((ToolStripMenuItem)s!).Checked;

        _fileBlockItem = new ToolStripMenuItem("File blocking");
        _fileBlockItem.Click += (s, _) =>
            _shell.Dashboard.FileBlock = ((ToolStripMenuItem)s!).Checked;

        _alertsItem = new ToolStripMenuItem("Alert reporting");
        _alertsItem.Click += (s, _) =>
            _shell.Dashboard.AlertsEnabled = ((ToolStripMenuItem)s!).Checked;

        _memoryItem = new ToolStripMenuItem("Memory monitor");
        _memoryItem.Click += (s, _) =>
            _shell.Dashboard.MemoryMonitor = ((ToolStripMenuItem)s!).Checked;

        var menu = new ContextMenuStrip();
        menu.Items.Add(_statusItem);
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(new ToolStripMenuItem("Open ShieldCord", null, (_, _) => OpenRequested?.Invoke()));
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(_enforcementItem);
        menu.Items.Add(_fileBlockItem);
        menu.Items.Add(_alertsItem);
        menu.Items.Add(_memoryItem);
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(new ToolStripMenuItem("Exit", null, (_, _) => ExitRequested?.Invoke()));

        // Reflect state every time the menu is about to show, so it is never
        // stale even if something changed while it was closed.
        menu.Opening += (_, _) => SyncMenuFromState();

        _notify = new NotifyIcon
        {
            Visible         = true,
            Icon            = SystemIcons.Shield,
            Text            = "ShieldCord",
            ContextMenuStrip = menu,
        };
        _notify.DoubleClick += (_, _) => OpenRequested?.Invoke();

        _shell.Dashboard.PropertyChanged += OnDashboardChanged;
        _shell.LiveAlert += OnLiveAlert;

        SyncMenuFromState();
        UpdateIcon();
    }

    private void OnDashboardChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(DashboardViewModel.StatusLevel)
                            or nameof(DashboardViewModel.StatusHeadline))
        {
            UpdateIcon();
        }

        if (e.PropertyName is nameof(DashboardViewModel.IsInteractive))
        {
            bool enabled = _shell.Dashboard.IsInteractive;
            _enforcementItem.Enabled = enabled;
            _fileBlockItem.Enabled   = enabled;
            _alertsItem.Enabled      = enabled;
            _memoryItem.Enabled      = enabled;
        }
    }

    private void SyncMenuFromState()
    {
        var d = _shell.Dashboard;

        _statusItem.Text = _shell.IsConnected ? d.StatusHeadline
                         : _shell.IsEngineStopped ? "Engine stopped"
                         : "Engine unreachable";

        _enforcementItem.Checked = d.Enforcement;
        _fileBlockItem.Checked   = d.FileBlock;
        _alertsItem.Checked      = d.AlertsEnabled;
        _memoryItem.Checked      = d.MemoryMonitor;

        bool enabled = d.IsInteractive && _shell.IsConnected;
        _enforcementItem.Enabled = enabled;
        _fileBlockItem.Enabled   = enabled;
        _alertsItem.Enabled      = enabled;
        _memoryItem.Enabled      = enabled;
    }

    private void UpdateIcon()
    {
        _notify.Icon = _shell.Dashboard.StatusLevel switch
        {
            "ok"   => SystemIcons.Shield,
            "warn" => SystemIcons.Warning,
            _      => SystemIcons.Error,
        };

        // NotifyIcon.Text is capped at 63 characters; longer throws.
        string text = $"ShieldCord — {_shell.Dashboard.StatusHeadline}";
        _notify.Text = text.Length > 62 ? text[..62] : text;
    }

    private void OnLiveAlert(AlertRecord record)
    {
        var settings = _shell.Settings;

        /*
         * The master mute. It silences the DESKTOP only — the record still
         * reaches the Activity timeline, because "stop interrupting me" is not
         * the same request as "stop telling me", and the timeline is where
         * someone looks to find out what happened while they were away.
         */
        if (settings.MuteAllAlertNotifications) return;

        /*
         * A self-healed trust race does NOT raise a balloon.
         *
         * It recovered on its own — a trusted app opened its storage a moment
         * before our trust landed, and was let through on the retry. The balloon
         * exists to pull someone away from what they are doing, and interrupting
         * them to say "nothing is wrong" is how the notification that matters
         * gets ignored. It is still in the timeline for anyone who looks.
         */
        if (record.Severity == Severity.Info) return;

        /*
         * Priority-only mode. "Critical" is the protocol's high severity — a
         * block or a kill, something ShieldCord actually did about a threat.
         * There is no critical/error level in the protocol: alerts are info,
         * warning or high. A warning is still recorded, it just does not
         * interrupt.
         */
        if (settings.NotifyCriticalOnly && record.Severity != Severity.High) return;

        string title = record.Action switch
        {
            Actions.Killed      => "Blocked and terminated",
            Actions.KillFailed  => "Blocked — could not terminate",
            Actions.DetectedOnly => "Memory read detected",
            _                   => "Blocked",
        };

        string body = string.IsNullOrWhiteSpace(record.ProcessName)
            ? record.Message
            : $"{record.ProcessName} (PID {record.Pid}) — {record.Message}";

        _notify.ShowBalloonTip(4000, title, body, ToolTipIcon.Warning);
    }

    public void ShowBalloon(string title, string body) =>
        _notify.ShowBalloonTip(3000, title, body, ToolTipIcon.Info);

    public void Dispose()
    {
        _shell.Dashboard.PropertyChanged -= OnDashboardChanged;
        _shell.LiveAlert -= OnLiveAlert;

        // Hide before disposing, or the icon can linger in the tray until the
        // user hovers over it.
        _notify.Visible = false;
        _notify.Dispose();
    }
}
