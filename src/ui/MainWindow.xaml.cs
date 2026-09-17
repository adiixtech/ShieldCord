using System.ComponentModel;
using System.Windows;
using ShieldCordUI.Services;
using ShieldCordUI.ViewModels;

namespace ShieldCordUI;

public partial class MainWindow : Window
{
    // Segoe MDL2 Assets glyphs.
    private const string GlyphMaximize = "";
    private const string GlyphRestore  = "";

    private readonly ShellViewModel _shell;
    private readonly TrayIcon _tray;

    /// <summary>Set when the app really is quitting, so Closing stops hiding.</summary>
    private bool _allowClose;

    public MainWindow(ShellViewModel shell, TrayIcon tray)
    {
        InitializeComponent();

        _shell = shell;
        _tray  = tray;

        DataContext = shell;

        // The alert page needs a way to ask before it weakens protection, and
        // a clipboard target — both are view concerns, so they are wired here
        // rather than reached for from the view model.
        shell.AlertsPage.Confirm = Confirm;
        shell.AlertsPage.CopyToClipboard = CopyToClipboard;
        shell.AppsPage.Confirm = Confirm;
        shell.SettingsPage.Confirm = Confirm;

        StateChanged += (_, _) => UpdateMaximizeGlyph();
    }

    // ── window chrome ────────────────────────────────────────

    private void Minimize_Click(object sender, RoutedEventArgs e) =>
        WindowState = WindowState.Minimized;

    private void Maximize_Click(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized
            ? WindowState.Normal
            : WindowState.Maximized;

    private void Close_Click(object sender, RoutedEventArgs e) => Close();

    private void UpdateMaximizeGlyph() =>
        MaximizeButton.Content = WindowState == WindowState.Maximized ? GlyphRestore : GlyphMaximize;

    // ── lifetime ─────────────────────────────────────────────

    protected override void OnClosing(CancelEventArgs e)
    {
        if (!_allowClose && _shell.Settings.MinimizeToTrayOnClose)
        {
            // Closing means "get out of my way", not "stop protecting me".
            e.Cancel = true;
            Hide();
            return;
        }

        base.OnClosing(e);
    }

    protected override void OnClosed(EventArgs e)
    {
        base.OnClosed(e);

        // ShutdownMode is OnExplicitShutdown, so reaching here (either the user
        // turned off close-to-tray, or the tray's Exit was used) must end the
        // process explicitly or it would linger with no window and no icon.
        Application.Current.Shutdown();
    }

    /// <summary>Close without the hide-to-tray interception.</summary>
    public void CloseForReal()
    {
        _allowClose = true;
        Close();
    }

    // ── view services ────────────────────────────────────────

    private static bool Confirm(string title, string message) =>
        MessageBox.Show(message, title, MessageBoxButton.YesNo, MessageBoxImage.Question)
        == MessageBoxResult.Yes;

    private static void CopyToClipboard(string text)
    {
        try
        {
            Clipboard.SetText(text);
        }
        catch
        {
            // The clipboard can be locked by another process; losing a copy is
            // not worth surfacing an error dialog over.
        }
    }
}
