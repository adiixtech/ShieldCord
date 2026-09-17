namespace ShieldCordUI.Services;

/// <summary>
/// Ensures one tray icon per user session.
///
/// The second launch signals the first through a named event and exits, so
/// double-clicking the shortcut while ShieldCord is already in the tray opens
/// the existing window instead of starting a rival copy that would fight over
/// the tray and the pipe.
/// </summary>
public sealed class SingleInstance : IDisposable
{
    private readonly Mutex _mutex;
    private readonly EventWaitHandle? _signal;
    private readonly CancellationTokenSource _cts = new();
    private readonly Task? _waiter;

    public bool IsFirst { get; }

    /// <summary>Raised (on a background thread) when a later launch asks us to show.</summary>
    public event Action? Activated;

    public SingleInstance(string name)
    {
        // "Local\" scopes this to the session: two different signed-in users
        // may each run their own tray icon.
        _mutex  = new Mutex(initiallyOwned: true, @"Local\" + name, out bool created);
        IsFirst = created;

        if (!IsFirst) return;

        _signal = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\" + name + ".signal");
        _waiter = Task.Run(() =>
        {
            while (!_cts.IsCancellationRequested)
            {
                try
                {
                    if (_signal.WaitOne(500)) Activated?.Invoke();
                }
                catch (ObjectDisposedException)
                {
                    return;
                }
            }
        });
    }

    /// <summary>Tells an already-running instance to show itself.</summary>
    public static void SignalExisting(string name)
    {
        try
        {
            using var handle = EventWaitHandle.OpenExisting(@"Local\" + name + ".signal");
            handle.Set();
        }
        catch
        {
            // First instance is mid-shutdown — nothing to signal.
        }
    }

    public void Dispose()
    {
        _cts.Cancel();
        try { _signal?.Set(); } catch { }       // wake the waiter so it can exit
        try { _waiter?.Wait(TimeSpan.FromSeconds(1)); } catch { }
        _signal?.Dispose();
        _cts.Dispose();
        _mutex.Dispose();
    }
}
