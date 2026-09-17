using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace ShieldCordUI.Controls;

/// <summary>
/// A staggered fade-and-rise entrance for list rows.
///
/// This is the one piece of motion that needs code rather than XAML, because a
/// storyboard cannot know where in a list an element sits. Doing it in XAML
/// alone gives every row the same delay, which reads as a single block fading
/// in — the stagger is what makes it read as a list arriving.
///
/// Usage, on the row's root element inside an ItemTemplate:
///
///     &lt;Border controls:Stagger.On="True"&gt;
///         &lt;Border.RenderTransform&gt;&lt;TranslateTransform/&gt;&lt;/Border.RenderTransform&gt;
///         ...
///
/// The row should declare a <see cref="TranslateTransform"/> as its
/// RenderTransform; one is attached automatically if it has none, but an
/// explicit one in XAML keeps the intent visible where the element is written.
/// </summary>
public static class Stagger
{
    /// <summary>
    /// Delay added per row. 22ms is short enough that a screenful is settled well
    /// inside a quarter of a second, and long enough that the cascade reads.
    /// </summary>
    private const double StepMilliseconds = 22;

    /// <summary>
    /// Rows past this index all get the same delay. Without a cap a 500-item
    /// activity list would take eleven seconds to arrive — the animation would
    /// outlive the user's attention, and every list rebuild would replay it.
    /// </summary>
    private const int MaxStaggeredRows = 12;

    /// <summary>Longest the entrance itself may take, after any delay.</summary>
    private const double DurationMilliseconds = 240;

    public static readonly DependencyProperty OnProperty =
        DependencyProperty.RegisterAttached(
            "On",
            typeof(bool),
            typeof(Stagger),
            new PropertyMetadata(false, OnStaggerChanged));

    public static bool GetOn(DependencyObject element) =>
        (bool)element.GetValue(OnProperty);

    public static void SetOn(DependencyObject element, bool value) =>
        element.SetValue(OnProperty, value);

    private static void OnStaggerChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not FrameworkElement row) return;

        if (e.NewValue is true)
        {
            // Loaded, not now: at the moment the attached property is set the
            // element has no visual parent, so its position in the list is not
            // yet knowable.
            row.Loaded += OnRowLoaded;
        }
        else
        {
            row.Loaded -= OnRowLoaded;
        }
    }

    private static void OnRowLoaded(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement row) return;

        // One-shot. Loaded fires again if the row is re-parented — a theme swap,
        // a panel rebuild — and replaying the entrance on a row the user is
        // already reading is worse than having no animation at all.
        row.Loaded -= OnRowLoaded;

        EnsureTransform(row);

        try
        {
            Play(row);
        }
        catch
        {
            // A row stuck at zero opacity is an alert nobody can see, which is a
            // far worse outcome than a missing animation. Whatever went wrong,
            // put it back on screen.
            row.Opacity = 1;
        }
    }

    private static void Play(FrameworkElement row)
    {
        // The base value has to be 0 before the storyboard starts, or the row is
        // briefly visible at full opacity during its BeginTime delay and then
        // blinks out to fade in.
        row.Opacity = 0;

        double delay = Math.Min(IndexInList(row), MaxStaggeredRows) * StepMilliseconds;
        var duration = TimeSpan.FromMilliseconds(DurationMilliseconds);

        var storyboard = new Storyboard { BeginTime = TimeSpan.FromMilliseconds(delay) };

        var fade = new DoubleAnimation(0, 1, duration)
        {
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
        };
        Storyboard.SetTarget(fade, row);
        Storyboard.SetTargetProperty(fade, new PropertyPath(UIElement.OpacityProperty));
        storyboard.Children.Add(fade);

        var rise = new DoubleAnimation(8, 0, duration)
        {
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
        };
        Storyboard.SetTarget(rise, row);
        Storyboard.SetTargetProperty(rise, new PropertyPath(
            "(UIElement.RenderTransform).(TranslateTransform.Y)"));
        storyboard.Children.Add(rise);

        storyboard.Begin();
    }

    /// <summary>
    /// How far down its list this row sits, or 0 when that cannot be determined.
    /// Returning 0 rather than throwing means a row in an unexpected container
    /// still gets its entrance, just without the offset.
    /// </summary>
    private static int IndexInList(FrameworkElement row)
    {
        DependencyObject? current = row;

        while (current is not null)
        {
            if (current is ItemsControl items)
            {
                DependencyObject? container = items.ContainerFromElement(row);
                return container is null
                    ? 0
                    : Math.Max(0, items.ItemContainerGenerator.IndexFromContainer(container));
            }

            current = VisualTreeHelper.GetParent(current);
        }

        return 0;
    }

    private static void EnsureTransform(FrameworkElement row)
    {
        if (row.RenderTransform is TranslateTransform) return;
        row.RenderTransform = new TranslateTransform();
    }
}
