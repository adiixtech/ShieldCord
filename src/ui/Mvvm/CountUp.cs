using System.Windows.Threading;

namespace ShieldCordUI.Mvvm;

/// <summary>
/// Eases a displayed number toward a target, for figures that should look like
/// they are counting rather than being replaced.
///
/// A timer rather than a WPF Storyboard, because what is being animated is a
/// view-model value, not a visual property — a Storyboard can only reach
/// dependency properties on the visual tree, so counting up a formatted string
/// would mean either animating a hidden element and reading it back (fragile) or
/// doing it here explicitly.
///
/// The interpolation itself is a cubic ease-out: fast at first, settling at the
/// end, which is the curve that reads as "arriving" rather than "sliding".
/// </summary>
public sealed class CountUp
{
    /// <summary>~60fps. The timer only runs while something is actually moving.</summary>
    private static readonly TimeSpan Tick = TimeSpan.FromMilliseconds(16);

    /// <summary>
    /// Long enough to be legible, short enough that the figure has settled before
    /// the next 2-second status poll arrives with a new value. A longer animation
    /// would be perpetually interrupted and never reach its target.
    /// </summary>
    private static readonly TimeSpan Duration = TimeSpan.FromMilliseconds(450);

    private readonly Action<double> _apply;
    private readonly DispatcherTimer _timer;

    private double _from;
    private double _to;
    private double _elapsed;

    public CountUp(Action<double> apply)
    {
        _apply = apply;

        // Created on the UI thread, so it ticks on the UI thread — the callback
        // writes to a view-model property that a binding reads.
        _timer = new DispatcherTimer(DispatcherPriority.Render) { Interval = Tick };
        _timer.Tick += OnTick;
    }

    /// <summary>The value currently on screen.</summary>
    public double Current { get; private set; }

    /// <summary>Whether something is still moving.</summary>
    public bool IsRunning => _timer.IsEnabled;

    /// <summary>
    /// Ease toward <paramref name="value"/>. Restarts from wherever the display
    /// currently is, not from the previous target — so a value that changes again
    /// mid-flight continues smoothly instead of jumping back.
    /// </summary>
    public void To(double value)
    {
        if (Math.Abs(value - _to) < 0.5 && _timer.IsEnabled) return; // already heading there
        if (!_timer.IsEnabled && Math.Abs(value - Current) < 0.5) return; // already there

        _from = Current;
        _to = value;
        _elapsed = 0;

        _timer.Start();
    }

    /// <summary>
    /// Jump straight to a value with no animation.
    ///
    /// For discontinuities. The protection-event counter resets when the engine
    /// restarts, and easing DOWN through that would animate a figure being undone
    /// — the opposite of what happened.
    /// </summary>
    public void SnapTo(double value)
    {
        _timer.Stop();
        _to = value;
        Current = value;
        _apply(value);
    }

    public void Stop()
    {
        _timer.Stop();
        _timer.Tick -= OnTick;
    }

    private void OnTick(object? sender, EventArgs e)
    {
        _elapsed += Tick.TotalMilliseconds;

        double t = Math.Clamp(_elapsed / Duration.TotalMilliseconds, 0, 1);

        // Cubic ease-out.
        double eased = 1 - Math.Pow(1 - t, 3);

        Current = _from + (_to - _from) * eased;
        _apply(Current);

        if (t < 1) return;

        // Land exactly on the target: the eased curve approaches it but floating
        // point will not guarantee the final tick equals it, and a figure stuck one
        // short of the real count is a wrong number on a security dashboard.
        Current = _to;
        _apply(_to);
        _timer.Stop();
    }
}
