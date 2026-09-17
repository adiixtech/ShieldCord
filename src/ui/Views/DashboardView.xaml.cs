using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using System.Windows.Threading;
using ShieldCordUI.ViewModels;

namespace ShieldCordUI.Views;

public partial class DashboardView : UserControl
{
    private const double BarDurationMs = 380;
    private const double BarStaggerMs   = 14;

    /// <summary>
    /// The last status level seen, so the pulse can tell a genuine transition
    /// into "ok" from the first status reply of a session. Without it the icon
    /// pulses on every launch, celebrating something that did not just happen.
    /// </summary>
    private string? _lastLevel;

    public DashboardView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        Unloaded += OnUnloaded;
        Loaded += OnLoaded;
    }

    private DashboardViewModel? ViewModel => DataContext as DashboardViewModel;

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        // The view is built once per navigation (the ContentControl holds one
        // instance while the dashboard is current), so this fires on arriving at
        // the page — NOT on the 2-second status poll, which only replaces the
        // bucketed data behind it.
        ScheduleBarAnimation();
    }

    private void OnDataContextChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (e.OldValue is INotifyPropertyChanged old) old.PropertyChanged -= OnViewModelChanged;
        if (e.NewValue is INotifyPropertyChanged next) next.PropertyChanged += OnViewModelChanged;

        _lastLevel = ViewModel?.StatusLevel;
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        // The view model outlives this view, so a subscription left behind would
        // keep a dead view alive for the life of the process.
        if (ViewModel is INotifyPropertyChanged vm) vm.PropertyChanged -= OnViewModelChanged;
    }

    private void OnViewModelChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(DashboardViewModel.StatusLevel)) CheckForProtectionPulse();
        else if (e.PropertyName == nameof(DashboardViewModel.TrafficRange)) ScheduleBarAnimation();
    }

    private void CheckForProtectionPulse()
    {
        string? level = ViewModel?.StatusLevel;
        bool justProtected = level == "ok" && _lastLevel is not null and not "ok";
        _lastLevel = level;

        if (justProtected) PulseStatusIcon();
    }

    /// <summary>
    /// One-shot scale pulse on the status icon.
    ///
    /// A transform, not a size change: this runs while the status poll may be
    /// mid-update, and animating Width would put a layout pass on every frame.
    /// Fires and forgets — if it is interrupted by a theme swap or a navigation
    /// it simply stops, and the icon is at scale 1 either way because the
    /// animation holds its final value.
    /// </summary>
    private void PulseStatusIcon()
    {
        if (StatusIcon.RenderTransform is not ScaleTransform) return;

        var pulse = new Storyboard();

        foreach (string axis in new[] { "ScaleX", "ScaleY" })
        {
            var grow = new DoubleAnimation(1, 1.12, TimeSpan.FromMilliseconds(160))
            {
                AutoReverse = true,
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
            };
            Storyboard.SetTarget(grow, StatusIcon);
            Storyboard.SetTargetProperty(grow, new PropertyPath(
                $"(UIElement.RenderTransform).(ScaleTransform.{axis})"));
            pulse.Children.Add(grow);
        }

        pulse.Begin();
    }

    /// <summary>
    /// Defer the bar animation until after the next layout pass.
    ///
    /// On a range change the view model rebuilds the bucket collection, and the
    /// containers for the new bars do not exist until WPF has laid them out.
    /// Animating immediately would find an empty ItemsControl and silently do
    /// nothing — which is exactly how a chart animation ends up "not working"
    /// for reasons nobody can see.
    ///
    /// DispatcherPriority.Loaded runs after layout and after the Loaded event,
    /// which is the first point the bars are real.
    /// </summary>
    private void ScheduleBarAnimation() =>
        Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(AnimateBars));

    private void AnimateBars()
    {
        int index = 0;

        foreach (Rectangle bar in EnumerateBars())
        {
            if (bar.RenderTransform is not ScaleTransform) continue;

            double delay = Math.Min(index++, 12) * BarStaggerMs;

            var grow = new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(BarDurationMs))
            {
                BeginTime = TimeSpan.FromMilliseconds(delay),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
            };

            Storyboard.SetTarget(grow, bar);
            Storyboard.SetTargetProperty(grow, new PropertyPath(
                "(UIElement.RenderTransform).(ScaleTransform.ScaleY)"));

            var storyboard = new Storyboard();
            storyboard.Children.Add(grow);
            storyboard.Begin();
        }
    }

    /// <summary>
    /// The bar Rectangles inside the chart, in the order the chart draws them.
    ///
    /// Walks the generated containers rather than the bound collection: the
    /// collection holds the DATA, and what needs animating is the visuals WPF
    /// built from it. With virtualisation off (this is a fixed set of buckets)
    /// every container is present.
    /// </summary>
    private IEnumerable<Rectangle> EnumerateBars()
    {
        for (int i = 0; i < TrafficBars.Items.Count; i++)
        {
            if (TrafficBars.ItemContainerGenerator.ContainerFromIndex(i) is not DependencyObject container)
                continue;

            Rectangle? bar = FindBar(container);
            if (bar is not null) yield return bar;
        }
    }

    private static Rectangle? FindBar(DependencyObject node)
    {
        int count = VisualTreeHelper.GetChildrenCount(node);

        for (int i = 0; i < count; i++)
        {
            DependencyObject child = VisualTreeHelper.GetChild(node, i);

            if (child is Rectangle rect) return rect;

            Rectangle? deeper = FindBar(child);
            if (deeper is not null) return deeper;
        }

        return null;
    }
}
