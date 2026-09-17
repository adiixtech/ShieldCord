using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace ShieldCordUI.Converters;

/// <summary>
/// Collapses an element when its bound string is null or whitespace.
///
/// Used for the optional fields on an alert: a file-block alert has a path and
/// no target process, a memory alert has a target and no path. Showing a
/// permanently empty "PATH" label for one of them looks like a bug.
/// </summary>
public sealed class NullOrEmptyToVisibilityConverter : IValueConverter
{
    /// <summary>When true, an empty string shows the element and a value hides it.</summary>
    public bool Invert { get; set; }

    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        bool hasText = value switch
        {
            string s => !string.IsNullOrWhiteSpace(s),
            null     => false,
            _        => true,
        };

        if (Invert) hasText = !hasText;
        return hasText ? Visibility.Visible : Visibility.Collapsed;
    }

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}

/// <summary>Inverts a bool (for IsEnabled="{Binding Busy, Converter=...}").</summary>
public sealed class InverseBoolConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        => value is bool b && !b;

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => value is bool b && !b;
}

/// <summary>
/// True when the bound string equals the parameter.
/// Lets the sidebar's radio buttons reflect the shell's current page without
/// each one needing its own bool property on the view model.
/// </summary>
public sealed class StringEqualsConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        => string.Equals(value as string, parameter as string, StringComparison.Ordinal);

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}

/// <summary>
/// Renders an alert's wire timestamp for display.
///
/// This lives here rather than as a computed property on
/// <see cref="Protocol.AlertRecord"/> so that Protocol.cs stays what its header
/// says it is — a hand-maintained mirror of the C++ contract, with no
/// presentation decisions in it.
/// </summary>
public sealed class AlertTimeConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        => Services.AlertTime.Display(value as string);

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}

/// <summary>
/// Renders an alert's <c>Action</c> for a human. The wire values are snake_case
/// tokens from the protocol; showing them raw puts "detected_only" on screen.
/// An unrecognised token is shown with its underscores opened out rather than
/// hidden, so a newer engine's new action is visible instead of blank.
/// </summary>
public sealed class ActionLabelConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        => (value as string) switch
        {
            Protocol.Actions.Blocked      => "blocked",
            Protocol.Actions.Killed       => "terminated",
            Protocol.Actions.KillFailed   => "kill failed",
            Protocol.Actions.DetectedOnly => "detected",
            null or ""                    => "",
            var other                     => other.Replace('_', ' '),
        };

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}

/// <summary>
/// Turns a 0..1 share into a proportional <see cref="GridLength"/>, so a bar
/// chart can be built out of star-sized rows.
///
/// Without a charting library a bar chart comes down to giving each bar its own
/// Grid whose first row is weighted by that bar's value and whose second row
/// takes the remainder. That only works if the weight is a real GridLength,
/// which is what this produces.
///
/// The result is always strictly positive: a row weighted "0*" collapses to
/// nothing, so a bucket with no events would vanish entirely instead of sitting
/// on the baseline as a hairline. A bar that renders as absent reads as missing
/// data rather than as a measured zero, which is a different claim.
/// </summary>
public sealed class StarWeightConverter : IValueConverter
{
    private const double MinWeight = 0.006;

    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        double share = value switch
        {
            double d => d,
            int i    => i,
            long l   => l,
            _        => 0,
        };

        if (double.IsNaN(share) || share < 0) share = 0;
        if (share > 1) share = 1;

        return new GridLength(Math.Max(share, MinWeight), GridUnitType.Star);
    }

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}
