using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace ShieldCordUI.Controls;

/// <summary>
/// A cursor-following highlight, like the Fluent "reveal" hover on Windows 11.
///
/// A soft radial glow tracks the pointer across the surface, fading in on enter
/// and out on leave. It costs nothing when the pointer is elsewhere and it makes
/// a large flat surface feel like a physical object rather than a painted
/// rectangle.
///
/// Usage — on a <see cref="Grid"/>, or on a <see cref="Border"/> whose Child is a
/// Grid:
///
///     &lt;Border Background="Transparent"
///             controls:Reveal.On="True" controls:Reveal.CornerRadius="8"&gt;
///         &lt;Grid&gt;...&lt;/Grid&gt;
///     &lt;/Border&gt;
///
/// THE TWO CONSTRAINTS, both of which are WPF facts rather than choices:
///
///   1. The host must be hit-testable, which for a Panel means a non-null
///      Background. A Grid with no Background is transparent to the mouse and
///      would never raise MouseMove — so a host Grid needs
///      Background="Transparent" (visually identical, but hit-testable).
///
///   2. The overlay is added INTO a Grid, so the host must be a Grid or own one.
///      It is deliberately not added to a StackPanel: a StackPanel lays children
///      out in a line, so the overlay would become an extra row and shove the
///      real content down. A Grid can be spanned, which is what makes this work.
///
/// Anything else is left alone rather than throwing — a missing decoration must
/// never be a crash.
/// </summary>
public static class Reveal
{
    /// <summary>Radius of the glow, in pixels.</summary>
    private const double DefaultRadius = 150;

    /// <summary>
    /// Peak alpha of the glow. Kept low: this sits behind text on a nav row, and
    /// a highlight strong enough to notice on its own fights the label for
    /// attention.
    /// </summary>
    private const byte PeakAlpha = 38;

    public static readonly DependencyProperty OnProperty =
        DependencyProperty.RegisterAttached(
            "On", typeof(bool), typeof(Reveal),
            new PropertyMetadata(false, OnRevealChanged));

    public static bool GetOn(DependencyObject element) => (bool)element.GetValue(OnProperty);
    public static void SetOn(DependencyObject element, bool value) => element.SetValue(OnProperty, value);

    /// <summary>Corner radius of the overlay, so it matches a rounded host.</summary>
    public static readonly DependencyProperty CornerRadiusProperty =
        DependencyProperty.RegisterAttached(
            "CornerRadius", typeof(double), typeof(Reveal),
            new PropertyMetadata(0.0, OnCornerRadiusChanged));

    public static double GetCornerRadius(DependencyObject element) =>
        (double)element.GetValue(CornerRadiusProperty);
    public static void SetCornerRadius(DependencyObject element, double value) =>
        element.SetValue(CornerRadiusProperty, value);

    /// <summary>
    /// The overlay this behaviour created for a host.
    ///
    /// A private attached property rather than a field on the host (Tag, say):
    /// Tag is a public property that a view is entitled to set, and nav rows
    /// already put their glyph codepoint in one.
    /// </summary>
    private static readonly DependencyProperty OverlayProperty =
        DependencyProperty.RegisterAttached(
            "Overlay", typeof(Border), typeof(Reveal), new PropertyMetadata(null));

    private static void OnCornerRadiusChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is FrameworkElement host && TryGetOverlay(host) is { } overlay)
            overlay.CornerRadius = new CornerRadius((double)e.NewValue);
    }

    private static void OnRevealChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not FrameworkElement host) return;

        if (e.NewValue is not true)
        {
            host.MouseMove -= OnHostMouseMove;
            host.MouseLeave -= OnHostMouseLeave;
            return;
        }

        var layer = BuildOverlay(host);
        if (layer is null) return; // host shape not supported — no decoration, no crash

        host.MouseMove += OnHostMouseMove;
        host.MouseLeave += OnHostMouseLeave;
    }

    private static void OnHostMouseMove(object sender, MouseEventArgs e)
    {
        if (sender is not FrameworkElement host) return;
        if (TryGetOverlay(host) is not { } overlay) return;

        // Relative to the overlay, not the host: for a Border host the overlay
        // sits inside the padding, so a position measured against the Border
        // would be offset by the padding and the glow would trail the cursor.
        Point p = e.GetPosition(overlay);

        if (overlay.Background is RadialGradientBrush brush)
        {
            // A new brush each move would allocate ~60 objects a second and force a
            // re-render of the whole row. Moving the one we have is both cheaper and
            // what the composition layer can actually keep up with.
            if (brush.IsFrozen) overlay.Background = brush = brush.Clone();
            brush.Center = p;
            brush.GradientOrigin = p;
        }

        if (overlay.Opacity < 1) Fade(overlay, 1, TimeSpan.FromMilliseconds(140));
    }

    private static void OnHostMouseLeave(object sender, MouseEventArgs e)
    {
        if (sender is FrameworkElement host && TryGetOverlay(host) is { } overlay && overlay.Opacity > 0)
            Fade(overlay, 0, TimeSpan.FromMilliseconds(220));
    }

    private static void Fade(UIElement target, double to, TimeSpan duration)
    {
        var fade = new DoubleAnimation(to, duration)
        {
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
        };
        target.BeginAnimation(UIElement.OpacityProperty, fade);
    }

    /// <summary>Finds the overlay this behaviour added to a host, if any.</summary>
    private static Border? TryGetOverlay(FrameworkElement host) =>
        (Border?)host.GetValue(OverlayProperty);

    /// <summary>
    /// Builds the glow and puts it where it can actually be drawn.
    /// Returns null when the host has nowhere to put it.
    /// </summary>
    private static Border? BuildOverlay(FrameworkElement host)
    {
        var layer = new Border
        {
            // Never steals the cursor from the content underneath: without this
            // the host would stop receiving MouseMove the moment the overlay
            // appeared, and the glow would freeze.
            IsHitTestVisible = false,
            Opacity = 0,
            CornerRadius = new CornerRadius(GetCornerRadius(host)),
            Background = BuildBrush(host),
        };

        switch (host)
        {
            case Border { Child: Grid borderGrid }:
                // Spans the whole grid regardless of its row/column count. WPF
                // clamps a span larger than the grid, so a big number is safe.
                Grid.SetRowSpan(layer, 99);
                Grid.SetColumnSpan(layer, 99);
                Panel.SetZIndex(layer, 99);
                borderGrid.Children.Add(layer);
                break;

            case Panel panel:
                Grid.SetRowSpan(layer, 99);
                Grid.SetColumnSpan(layer, 99);
                Panel.SetZIndex(layer, 99);
                panel.Children.Add(layer);
                break;

            default:
                return null;
        }

        // Remembered on the host so MouseMove can find it without walking the
        // visual tree on every single mouse event.
        host.SetValue(OverlayProperty, layer);
        return layer;
    }

    /// <summary>
    /// The glow brush, tinted from the current theme's foreground colour rather
    /// than a hard-coded white. A white glow is invisible on the light theme and
    /// a black one is invisible on the dark, so reading App.Text means the same
    /// code works in both.
    /// </summary>
    private static RadialGradientBrush BuildBrush(FrameworkElement host)
    {
        Color tint = Colors.White;

        if (host.TryFindResource("App.Text") is SolidColorBrush themed)
            tint = themed.Color;

        var brush = new RadialGradientBrush
        {
            MappingMode = BrushMappingMode.Absolute,
            RadiusX = DefaultRadius,
            RadiusY = DefaultRadius,
            GradientStops =
            {
                new GradientStop(Color.FromArgb(PeakAlpha, tint.R, tint.G, tint.B), 0.0),
                new GradientStop(Color.FromArgb(0, tint.R, tint.G, tint.B), 1.0),
            },
        };

        return brush;
    }
}
