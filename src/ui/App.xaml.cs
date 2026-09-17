using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;
using ShieldCordUI.Services;
using ShieldCordUI.ViewModels;
using ShieldCordUI.Views;

namespace ShieldCordUI;

public partial class App : Application
{
    private const string InstanceName = "ShieldCord.UI";

    private SingleInstance? _single;
    private ShellViewModel? _shell;
    private TrayIcon? _tray;
    private MainWindow? _window;
    private bool _systemThemeHooked;

    protected override void OnStartup(StartupEventArgs e)
    {
        // Installed FIRST, before anything else can throw. WPF reports most
        // failures — a missing resource key, a broken binding, a null view
        // model — by silently doing nothing, so without these two handlers a
        // broken window is just a blank window with no explanation anywhere.
        DispatcherUnhandledException += OnDispatcherException;
        AppDomain.CurrentDomain.UnhandledException += OnDomainException;

        base.OnStartup(e);

        // ── one-shot elevated helper: no window, no tray, just the verb ──
        if (ElevatedVerbMode.IsRequested(e.Args))
        {
            RunElevatedVerbAndExit(e.Args);
            return;
        }

        // ── single instance ─────────────────────────────────────
        _single = new SingleInstance(InstanceName);
        if (!_single.IsFirst)
        {
            SingleInstance.SignalExisting(InstanceName);   // raise the existing window
            Shutdown();
            return;
        }
        _single.Activated += ShowWindow;

        // ── view models + tray ──────────────────────────────────
        _shell = new ShellViewModel(Dispatcher);
        _shell.SettingsPage.ThemeChanged = ApplyTheme;
        ApplyTheme(_shell.Settings.Theme);

        _tray = new TrayIcon(_shell);
        _tray.OpenRequested += ShowWindow;
        _tray.ExitRequested += ExitApp;

        ApplyStartupPreference();
        HookSystemThemeChanges();

        _shell.Start();

        // ── window ──────────────────────────────────────────────
        _window = new MainWindow(_shell, _tray);

        // Launched by the login entry? Stay in the tray — a window popping up
        // at every sign-in is the fastest way to get an app uninstalled.
        bool silentStart = Array.Exists(e.Args, a =>
            string.Equals(a, StartupRegistration.StartupSwitch, StringComparison.OrdinalIgnoreCase));

        if (!silentStart) _window.Show();

        // ── driver setup ────────────────────────────────────────
        // The driver is installed from HERE, not by the installer, so the app has
        // to notice when it is missing — and notice when it has just finished, on
        // the far side of the restart. Runs regardless of silentStart so the
        // dashboard's status is right even at login.
        _ = InitializeSetupAsync(silentStart);

    }

    /// <summary>
    /// Find out whether this machine has the kernel driver, and offer to install
    /// it if it does not.
    ///
    /// The driver is deliberately not part of the Setup.exe any more: it cannot
    /// be loaded until test signing is on, and asking for a security change
    /// belongs in a screen that explains it, not in a silent install step whose
    /// failure nothing reports. So the installer installs the app, and this is
    /// where protection actually gets switched on.
    ///
    /// Two ways in, and the second is the interesting one:
    ///
    ///   * The driver has never been set up — offer to do it.
    ///   * The driver is now running, setup put it there, and the user has not yet
    ///     seen the result — show the closing screen. This is the path taken on the
    ///     first launch AFTER the restart, which is exactly why it has to be
    ///     recognised from the machine's state rather than remembered in memory:
    ///     the process that asked for the restart is long gone.
    /// </summary>
    private async Task InitializeSetupAsync(bool silentStart)
    {
        // Called with `_ =` from OnStartup, so anything thrown in here would
        // become an unobserved task exception and vanish — and the visible
        // symptom would be a setup screen that simply never appears, on the one
        // machine that has no protection. Report instead of swallowing.
        try
        {
            if (_shell is null) return;

            // Ask the SCM before deciding anything: it is what distinguishes
            // "driver missing" from "driver waiting for a restart".
            await _shell.RefreshFilterStateAsync();

            // Never throw a takeover at someone who is signing in.
            if (silentStart) return;

            if (_shell.FilterState == DriverSetup.FilterState.Running)
            {
                bool setupDone = _shell.Settings.TestSigningEnabledAt is not null;
                if (setupDone && !_shell.Settings.SetupAcknowledged)
                    _shell.OpenSetup(SetupStage.Complete);

                return;
            }

            if (_shell.FilterState != DriverSetup.FilterState.NotInstalled) return;

            // Only an explicit deferral stops this. An install that left the
            // machine unprotected is worth mentioning again next time the app opens.
            if (_shell.Settings.SetupPromptDismissed) return;

            _shell.OpenSetup(SetupStage.Welcome);
        }
        catch (Exception ex)
        {
            ReportCrash("driver setup prompt", ex);
        }
    }

    /// <summary>
    /// Handles this process's entire life when it was launched only to relay
    /// one verb, then exits.
    ///
    /// The work runs on a thread-pool thread and this thread blocks on it, for
    /// two reasons: WPF has already installed a dispatcher SynchronizationContext
    /// that is not pumping yet (so awaiting here would deadlock), and the
    /// message loop is never going to run, so there is nothing to unwind.
    /// Environment.Exit rather than Shutdown() because this process has no
    /// state worth tearing down and must never linger holding a UAC elevation.
    /// </summary>
    private static void RunElevatedVerbAndExit(string[] args)
    {
        if (ElevatedVerbMode.TryParse(args, out string request, out string reply))
        {
            try
            {
                Task.Run(() => ElevatedVerbMode.RunAsync(request, reply))
                    .Wait(TimeSpan.FromSeconds(20));
            }
            catch
            {
                // The reply file is written by RunAsync itself on every path
                // except a hard crash; the parent times out and reports that.
            }
        }

        Environment.Exit(0);
    }

    // ── crash reporting ──────────────────────────────────────

    /// <summary>Where UI failures are written. Same folder as the engine log,
    /// so a bug report only ever needs one directory.</summary>
    private static readonly string CrashLogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
        "ShieldCord", "logs", "shieldcord-ui-error.log");

    private void OnDispatcherException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        ReportCrash("UI thread", e.Exception);

        // Keep running when we can: a single failed binding or command should
        // not take the tray icon down with it.
        e.Handled = true;
    }

    private static void OnDomainException(object sender, UnhandledExceptionEventArgs e)
    {
        if (e.ExceptionObject is Exception ex) ReportCrash("background thread", ex);
    }

    private static void ReportCrash(string origin, Exception ex)
    {
        string text = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {origin}: {ex}";

        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(CrashLogPath)!);
            File.AppendAllText(CrashLogPath, text + Environment.NewLine + Environment.NewLine);
        }
        catch
        {
            // If we cannot even write the log, the dialog below is still useful.
        }

        try
        {
            MessageBox.Show(
                $"ShieldCord hit an unexpected error ({origin}).\n\n" +
                $"{ex.GetType().Name}: {ex.Message}\n\n" +
                $"Full details were written to:\n{CrashLogPath}",
                "ShieldCord — unexpected error",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
        catch
        {
            // Nothing left to do; never let the reporter itself throw.
        }
    }

    private void ShowWindow()
    {
        if (_window is null) return;
        _window.Show();
        if (_window.WindowState == WindowState.Minimized)
            _window.WindowState = WindowState.Normal;

        _window.Activate();
        _window.Topmost = true;      // bring to front…
        _window.Topmost = false;     // …without leaving it pinned
        _window.Focus();
    }

    private void ExitApp()
    {
        _window?.CloseForReal();
        Shutdown();
    }


    /// <summary>Last light/dark state applied, so the Fluent dictionary is only
    /// swapped when it actually changes.</summary>
    private bool? _fluentDark;

    /// <summary>
    /// Apply a theme, crossfading between the old look and the new one.
    ///
    /// WHY THIS USES A BITMAP SNAPSHOT — it is not a preference.
    ///
    /// The obvious approach is to animate the palette brushes in place, which
    /// would transition every colour at once and is what a CSS transition on
    /// custom properties does. That is IMPOSSIBLE in WPF: `ResourceDictionary`
    /// runs `SealValues()` over its contents, which freezes every Freezable. A
    /// brush reachable from `Application.Resources` — merged or top-level, both
    /// were measured — is frozen and `BeginAnimation` on it throws
    /// "the object is sealed or frozen".
    ///
    /// So the palette must be SWAPPED, and a swap is instantaneous: every element
    /// repaints in the same frame. Rendering the window to a bitmap first and
    /// fading that over the new theme is the only way to get a transition at all.
    ///
    /// Order matters, and each step is here for a reason:
    ///
    ///   1. Drop any overlay still fading from a previous change. Capturing while
    ///      one is on screen would bake a ghost of it into the new snapshot.
    ///   2. Capture the window as it looks now.
    ///   3. Swap the palette, and Fluent with it.
    ///   4. Fade the snapshot out over the new theme.
    /// </summary>
    /// <param name="theme">"light", "dark" or "system".</param>
    public void ApplyTheme(string theme)
    {
        bool dark = theme == "dark" || (theme == "system" && IsSystemDark());

        RemoveCrossfadeOverlay();
        FrameworkElement? snapshot = CaptureForCrossfade();

        SetPalette(dark ? "Themes/Dark.xaml" : "Themes/Light.xaml",
                   isPalette: src => src.EndsWith("/Light.xaml", StringComparison.OrdinalIgnoreCase)
                                  || src.EndsWith("/Dark.xaml", StringComparison.OrdinalIgnoreCase));

        // Guarded: re-merging Fluent on every call would churn the resource graph
        // for no visual difference, and churn is what re-runs trigger storyboards.
        if (_fluentDark != dark)
        {
            _fluentDark = dark;

            SetPalette(dark
                           ? "pack://application:,,,/PresentationFramework.Fluent;component/Themes/Fluent.Dark.xaml"
                           : "pack://application:,,,/PresentationFramework.Fluent;component/Themes/Fluent.Light.xaml",
                       isPalette: src => src.Contains("PresentationFramework.Fluent", StringComparison.OrdinalIgnoreCase));
        }

        if (snapshot is not null) CrossfadeOut(snapshot);
    }

    /// <summary>
    /// Snapshot the live window so the theme change can be a crossfade rather
    /// than a hard cut.
    ///
    /// Returns null — never throws — when there is nothing worth capturing. Every
    /// guard here is a case where the honest outcome is an instant swap.
    /// </summary>
    private FrameworkElement? CaptureForCrossfade()
    {
        try
        {
            if (_window is null || !_window.IsVisible) return null;
            if (_window.Content is not Grid root) return null;

            double width = root.ActualWidth;
            double height = root.ActualHeight;
            if (width < 1 || height < 1) return null;

            // The bitmap is sized in DEVICE pixels: rendering a DIP-sized visual
            // into a DIP-sized bitmap produces a blurry or cropped snapshot on a
            // scaled display.
            double scaleX = 1.0, scaleY = 1.0;
            if (PresentationSource.FromVisual(_window)?.CompositionTarget is { } target)
            {
                Matrix toDevice = target.TransformToDevice;
                scaleX = toDevice.M11;
                scaleY = toDevice.M22;
            }

            var bitmap = new RenderTargetBitmap(
                (int)Math.Ceiling(width * scaleX),
                (int)Math.Ceiling(height * scaleY),
                96 * scaleX, 96 * scaleY,
                PixelFormats.Pbgra32);

            bitmap.Render(root);

            // The window's Background is on the Window, not on this Grid, so the
            // snapshot does not include it. Painting it underneath stops the
            // crossfade revealing a transparent hole where the page colour was.
            var oldBackground = TryFindResource("App.Background") as Brush ?? Brushes.Transparent;

            var overlay = new Grid { IsHitTestVisible = false };
            overlay.Children.Add(new Border { Background = oldBackground });
            overlay.Children.Add(new Image { Source = bitmap, Stretch = Stretch.Fill });

            return overlay;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>Where the crossfade overlay is parked while it fades.</summary>
    private Grid? _crossfadeOverlay;

    private void RemoveCrossfadeOverlay()
    {
        if (_crossfadeOverlay is null) return;
        if (_window?.Content is Grid root) root.Children.Remove(_crossfadeOverlay);
        _crossfadeOverlay = null;
    }

    private void CrossfadeOut(FrameworkElement snapshot)
    {
        // Defensive on purpose. This is a decoration: if anything about it fails,
        // the correct outcome is a theme that changed without an animation, never
        // an exception thrown out of a settings toggle.
        try
        {
            if (_window?.Content is not Grid root) return;

            Grid overlay = _crossfadeOverlay = new();

            /*
             * THE SPAN IS LOAD-BEARING.
             *
             * MainWindow's root Grid has two columns (sidebar 200, content *) and
             * two rows (top bar 44, content *). A child with no Grid.Row / Column
             * set lands in row 0, column 0 — the 200x44 top-left corner — and this
             * full-window snapshot was being squeezed into exactly that cell. It
             * looked like a small rectangle of noise in the upper-left of the
             * window, which is a strange way for a theme bug to present and was
             * misread as a rendering artefact for a while.
             *
             * WPF clamps a span larger than the grid, so a big number is safe.
             */
            Grid.SetRowSpan(overlay, 99);
            Grid.SetColumnSpan(overlay, 99);

            Panel.SetZIndex(overlay, 9999);
            overlay.Children.Add(snapshot);
            root.Children.Add(overlay);

            // Force measure/arrange NOW, before the fade starts. Without this the
            // overlay is still 0x0 for its first frame — it would fade in from
            // nothing and could flash the bare new theme underneath.
            root.UpdateLayout();

            var fade = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(240))
            {
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
            };

            fade.Completed += (_, _) =>
            {
                // Only remove if this is still the current overlay: a second
                // crossfade may have replaced it, and removing that one would
                // leave its bitmap pinned over the window.
                if (!ReferenceEquals(_crossfadeOverlay, overlay)) return;
                root.Children.Remove(overlay);
                _crossfadeOverlay = null;
            };

            overlay.BeginAnimation(UIElement.OpacityProperty, fade);
        }
        catch
        {
            // Swallowed deliberately — see above.
        }
    }

    /// <summary>
    /// Replace the merged dictionary matching <paramref name="existing"/>, or add
    /// it if it is not there yet.
    ///
    /// Inserted at index 0 when absent, which is safe for both callers: nothing
    /// in the app reaches these two by StaticResource, and the only ordering
    /// constraint in the merged set is that Motion.xaml precedes Controls.xaml —
    /// which inserting at the front does not disturb.
    /// </summary>
    private void SetPalette(string source, Func<string, bool> isPalette)
    {
        var merged = Resources.MergedDictionaries;

        // RelativeOrAbsolute because the two callers pass different kinds: a
        // relative path for the palette ("Themes/Dark.xaml") and an absolute
        // pack:// URI for the Fluent dictionary. UriKind.Absolute throws on the
        // first; UriKind.Relative throws on the second.
        var replacement = new ResourceDictionary
        {
            Source = new Uri(source, UriKind.RelativeOrAbsolute),
        };

        for (int i = 0; i < merged.Count; i++)
        {
            string? existing = merged[i].Source?.OriginalString;
            if (existing is null || !isPalette(existing)) continue;

            merged[i] = replacement;
            return;
        }

        merged.Insert(0, replacement);
    }

    /// <summary>
    /// Bring in (or swap) the .NET 10 Fluent theme, which styles the controls
    /// this app does NOT style itself: scrollbars, tooltips, text boxes, context
    /// menus. Everything in Themes/Controls.xaml has a full ControlTemplate of
    /// its own and is unaffected.
    ///
    /// Still zero NuGet packages — the dictionary is part of
    /// PresentationFramework, which ships with the Desktop Runtime the installer
    /// already bundles.
    /// </summary>


    private static bool IsSystemDark()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            // 0 = dark apps, 1 = light apps. Absent on older builds -> light.
            return key?.GetValue("AppsUseLightTheme") is int v && v == 0;
        }
        catch
        {
            return false;
        }
    }

    private void HookSystemThemeChanges()
    {
        try
        {
            SystemEvents.UserPreferenceChanged += OnUserPreferenceChanged;
            _systemThemeHooked = true;
        }
        catch
        {
            // Not fatal: the user can still pick a theme explicitly.
        }
    }

    private void OnUserPreferenceChanged(object sender, UserPreferenceChangedEventArgs e)
    {
        if (e.Category != UserPreferenceCategory.General) return;
        if (_shell?.Settings.Theme != "system") return;
        Dispatcher.Invoke(() => ApplyTheme("system"));
    }

    private void ApplyStartupPreference()
    {
        if (_shell is null) return;

        // Make the stored preference true on disk. Without this, a fresh
        // install ships with the checkbox ticked and nothing behind it.
        bool wanted  = _shell.Settings.StartWithWindows;
        if (StartupRegistration.IsEnabled() != wanted)
            StartupRegistration.Set(wanted);
    }

    protected override void OnExit(ExitEventArgs e)
    {
        if (_systemThemeHooked)
        {
            try { SystemEvents.UserPreferenceChanged -= OnUserPreferenceChanged; } catch { }
        }

        _tray?.Dispose();

        try { _shell?.DisposeAsync().AsTask().Wait(TimeSpan.FromSeconds(2)); } catch { }

        _single?.Dispose();
        base.OnExit(e);
    }
}
