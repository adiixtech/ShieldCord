using System.Windows;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace ShieldCordUI.Controls;

/// <summary>
/// Slides an element between two positions as its toggle is switched, and SNAPS
/// to the correct position when it is first created.
///
/// That second half is the reason this exists. The obvious way to animate a
/// switch knob is a <c>Trigger Property="IsChecked" Value="True"</c> with
/// EnterActions — but EnterActions fire whenever the trigger is (re-)ENTERED, and
/// a trigger is re-entered whenever the template is rebuilt, which WPF does when
/// resources change. Every switch on screen would replay its slide at once, so a
/// theme change made the toggles visibly flick off and back on. Attaching to
/// Loaded distinguishes the two cases.
///
/// WHY EVERYTHING HERE GOES THROUGH A STORYBOARD
///
///   A <see cref="TranslateTransform"/> declared inside a ControlTemplate is
///   FROZEN once the template is sealed — the same reason brushes loaded from a
///   ResourceDictionary cannot be animated. So this throws:
///
///       shift.X = 18;                              // read-only state
///       shift.BeginAnimation(TranslateTransform.XProperty, anim);  // frozen
///
///   A Storyboard targeting a PROPERTY PATH does not: WPF substitutes a mutable
///   clone of the frozen transform for the duration of the animation. That is
///   why the original trigger-based version worked and why everything here is
///   routed through <see cref="Storyboard"/> — including the snap, which is a
///   zero-duration animation rather than an assignment.
///
/// Usage, on the moving element inside a ControlTemplate:
///
///     &lt;Ellipse x:Name="Knob" controls:ToggleSlide.On="True"&gt;
///         &lt;Ellipse.RenderTransform&gt;&lt;TranslateTransform/&gt;&lt;/Ellipse.RenderTransform&gt;
///     &lt;/Ellipse&gt;
///
/// The offset defaults to 18 because the Switch's track is 40 wide with a 1px
/// border — a 38px content box, with the knob inset 2px from each end.
/// </summary>
public static class ToggleSlide
{
    private static readonly TimeSpan Slide = TimeSpan.FromMilliseconds(150);

    /// <summary>The path every animation here walks. WPF resolves the frozen
    /// transform through this and clones it.</summary>
    private static readonly PropertyPath ShiftPath =
        new("(UIElement.RenderTransform).(TranslateTransform.X)");

    public static readonly DependencyProperty OnProperty =
        DependencyProperty.RegisterAttached(
            "On", typeof(bool), typeof(ToggleSlide),
            new PropertyMetadata(false, OnToggleSlideChanged));

    public static bool GetOn(DependencyObject element) => (bool)element.GetValue(OnProperty);
    public static void SetOn(DependencyObject element, bool value) => element.SetValue(OnProperty, value);

    /// <summary>How far to travel.</summary>
    public static readonly DependencyProperty OffsetProperty =
        DependencyProperty.RegisterAttached(
            "Offset", typeof(double), typeof(ToggleSlide), new PropertyMetadata(18.0));

    public static double GetOffset(DependencyObject element) => (double)element.GetValue(OffsetProperty);
    public static void SetOffset(DependencyObject element, double value) => element.SetValue(OffsetProperty, value);

    private static void OnToggleSlideChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not FrameworkElement knob) return;

        if (e.NewValue is not true)
        {
            knob.Loaded -= OnKnobLoaded;
            knob.Unloaded -= OnKnobUnloaded;
            return;
        }

        knob.Loaded += OnKnobLoaded;
        knob.Unloaded += OnKnobUnloaded;
    }

    private static void OnKnobLoaded(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement knob) return;
        if (knob.TemplatedParent is not ToggleButton toggle) return;

        // ── the snap ──────────────────────────────────────────
        // A ZERO-DURATION animation, not an assignment: the transform is frozen
        // and cannot be written to directly.
        //
        // This is what stops the glitch. A rebuilt template hands us a knob at
        // its declared position (0, i.e. OFF), so without this every switch that
        // is ON would appear OFF and then slide ON — all of them at once, every
        // time resources change.
        SlideTo(knob, toggle.IsChecked == true ? GetOffset(knob) : 0,
                duration: TimeSpan.Zero, easing: null);

        // Re-subscribed on every load: a rebuilt template hands us a brand-new
        // element with none of the old handlers attached.
        toggle.Checked   -= OnToggled;
        toggle.Unchecked -= OnToggled;
        toggle.Checked   += OnToggled;
        toggle.Unchecked += OnToggled;
    }

    private static void OnKnobUnloaded(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement knob) return;
        if (knob.TemplatedParent is not ToggleButton toggle) return;

        toggle.Checked   -= OnToggled;
        toggle.Unchecked -= OnToggled;
    }

    private static void OnToggled(object sender, RoutedEventArgs e)
    {
        if (sender is not ToggleButton toggle) return;

        // Found by name in the template rather than cached: a cache keyed by the
        // toggle would hold every control alive for the life of the process, and
        // this runs once per click, not per frame.
        if (toggle.Template?.FindName("Knob", toggle) is not FrameworkElement knob) return;

        bool on = toggle.IsChecked == true;

        // The overshoot is only on the way ON — arriving with a little weight,
        // leaving without ceremony. Bouncing both ways is a toy.
        SlideTo(knob, on ? GetOffset(knob) : 0, Slide,
                on ? new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.22 }
                   : new CubicEase { EasingMode = EasingMode.EaseOut });
    }

    private static void SlideTo(FrameworkElement knob, double to, TimeSpan duration, IEasingFunction? easing)
    {
        var slide = new DoubleAnimation(to, duration);

        if (easing is not null) slide.EasingFunction = easing;

        Storyboard.SetTarget(slide, knob);
        Storyboard.SetTargetProperty(slide, ShiftPath);

        var storyboard = new Storyboard();
        storyboard.Children.Add(slide);

        // Started from the knob so the storyboard has a containing object to
        // resolve against. A storyboard with no TargetName could be started
        // without one, but passing it is harmless and removes the ambiguity.
        storyboard.Begin(knob);
    }
}
