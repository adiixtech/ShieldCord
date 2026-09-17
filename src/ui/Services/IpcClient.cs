using System.Collections.Concurrent;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using ShieldCordUI.Protocol;

namespace ShieldCordUI.Services;

public enum ConnectionState { Disconnected, Connecting, Connected }

/// <summary>
/// The named-pipe client to the ShieldCord engine.
///
/// The pipe is duplex and UNSOLICITED: the engine broadcasts `alert` messages
/// to every connected client at arbitrary moments, interleaved with replies to
/// this client's own requests. So this class never assumes the next line it
/// reads answers the last request — every request carries an id, the reply
/// echoes it, and the reader demultiplexes on that. (The previous
/// implementation ignored ids entirely and simply parsed whatever arrived,
/// which is why it could never issue more than one kind of request.)
///
/// Reconnection is automatic with exponential backoff, because the engine is a
/// service that can be stopped, upgraded, or crash-restarted underneath us.
/// </summary>
public sealed class IpcClient : IAsyncDisposable
{
    private const string PipeName = "ShieldCord";

    /// <summary>How long to wait for a reply before giving up on a request.</summary>
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(5);

    private static readonly TimeSpan BackoffMin = TimeSpan.FromMilliseconds(250);
    private static readonly TimeSpan BackoffMax = TimeSpan.FromSeconds(5);

    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        PropertyNameCaseInsensitive = false,
        ReadCommentHandling = JsonCommentHandling.Skip,
    };

    private readonly ConcurrentDictionary<long, TaskCompletionSource<JsonElement?>> _pending = new();
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private readonly object _stateLock = new();

    /// <summary>Guards the two hold fields below. Never held while awaiting.</summary>
    private readonly object _holdLock = new();

    private bool _holdReconnect;
    private TaskCompletionSource<bool>? _resumeSignal;

    private NamedPipeClientStream? _pipe;
    private StreamWriter? _writer;
    private CancellationTokenSource? _cts;
    private Task? _loop;
    private long _nextId;
    private ConnectionState _state = ConnectionState.Disconnected;

    /// <summary>Raised on a background thread for every block/kill the engine reports.</summary>
    public event Action<AlertRecord>? AlertReceived;

    /// <summary>Raised on a background thread whenever the connection state changes.</summary>
    public event Action<ConnectionState>? StateChanged;

    public ConnectionState State
    {
        get { lock (_stateLock) return _state; }
    }

    // ── the reconnect hold ───────────────────────────────────
    //
    // Without this the run loop retries forever on an exponential backoff, so a
    // deliberately stopped engine left the UI alternating "Connecting…" and
    // "Engine offline" for as long as the app was open — an app visibly trying
    // to reach something the user had just turned off.
    //
    // The hold is a decision about TRYING, never a claim about connection: it
    // never touches _state or _pipe, so it cannot make the UI report anything.
    // While held the loop simply parks, and the honest resting state is plain
    // Disconnected with no pipe — which every reader of ConnectionState already
    // handles correctly. (That is why this is not a fourth enum member: every
    // consumer is a switch with a catch-all arm, so a new member would be
    // silently absorbed into the wrong bucket rather than failing loudly.)

    /// <summary>True while the loop is making no connection attempts at all.</summary>
    public bool ReconnectHeld
    {
        get { lock (_holdLock) return _holdReconnect; }
    }

    /// <summary>Stop trying to connect. Does not disturb an existing connection.</summary>
    public void HoldReconnect()
    {
        lock (_holdLock) _holdReconnect = true;
    }

    /// <summary>Resume trying to connect, and wake the loop if it is parked.</summary>
    public void ReleaseReconnect()
    {
        TaskCompletionSource<bool>? signal;
        lock (_holdLock)
        {
            _holdReconnect = false;
            signal = _resumeSignal;

            // Cleared here, under the same lock the waiter creates it under, so
            // a later park can never await a signal that is already completed.
            _resumeSignal = null;
        }

        signal?.TrySetResult(true);
    }

    /// <summary>
    /// Parks the loop while held. Returns false only if cancellation arrived
    /// while parked, which means the caller should stop for good.
    /// </summary>
    private async Task<bool> WaitWhileHeldAsync(CancellationToken ct)
    {
        TaskCompletionSource<bool> signal;

        lock (_holdLock)
        {
            if (!_holdReconnect) return true;

            _resumeSignal ??= new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            signal = _resumeSignal;
        }

        // Registered so DisposeAsync's cancel cannot leave the loop parked.
        using var registration = ct.Register(
            static state => ((TaskCompletionSource<bool>)state!).TrySetResult(true), signal);

        await signal.Task;
        return !ct.IsCancellationRequested;
    }

    /// <summary>
    /// One throwaway connection attempt that publishes no state.
    ///
    /// The SCM is the primary oracle for whether the engine is running, but it
    /// knows nothing about a console-mode engine — which is reachable over the
    /// pipe while no service exists at all — and it can be unreadable. A pipe
    /// that answers is direct evidence, so this is the escape hatch that stops a
    /// hold from turning into permanent deafness.
    /// </summary>
    public async Task<bool> ProbeAsync(TimeSpan timeout)
    {
        try
        {
            using var pipe = new NamedPipeClientStream(".", PipeName,
                PipeDirection.InOut, PipeOptions.Asynchronous);

            using var cts = new CancellationTokenSource(timeout);
            await pipe.ConnectAsync((int)timeout.TotalMilliseconds, cts.Token);
            return true;
        }
        catch
        {
            return false;
        }
    }

    // ── lifecycle ────────────────────────────────────────────

    public void Start()
    {
        if (_loop is not null) return;
        _cts = new CancellationTokenSource();
        _loop = Task.Run(() => RunAsync(_cts.Token));
    }

    public async ValueTask DisposeAsync()
    {
        try { _cts?.Cancel(); } catch (ObjectDisposedException) { }

        // Closing the pipe is what unblocks the reader parked in ReadLineAsync;
        // cancelling the token alone would not.
        ClosePipe();

        if (_loop is not null)
        {
            try { await _loop; } catch { /* shutdown */ }
            _loop = null;
        }
        _cts?.Dispose();
        _cts = null;
        _writeLock.Dispose();
    }

    private async Task RunAsync(CancellationToken ct)
    {
        TimeSpan backoff = BackoffMin;

        while (!ct.IsCancellationRequested)
        {
            // The gate goes FIRST, ahead of every side effect of an iteration:
            // while held, no Connecting is ever published and no pipe is even
            // constructed, so there is nothing to unwind. Putting it at the top
            // also covers the very first iteration, which is what stops a
            // relaunch into a stopped engine from flashing "Connecting…" once.
            if (!await WaitWhileHeldAsync(ct)) break;

            try
            {
                SetState(ConnectionState.Connecting);

                var pipe = new NamedPipeClientStream(".", PipeName,
                    PipeDirection.InOut, PipeOptions.Asynchronous);
                await pipe.ConnectAsync(2000, ct);

                _pipe = pipe;
                _writer = new StreamWriter(pipe, new UTF8Encoding(false)) { AutoFlush = true };

                SetState(ConnectionState.Connected);
                backoff = BackoffMin;                       // healthy again — reset

                await ReadLoopAsync(pipe, ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch
            {
                // Engine not running / pipe refused / connection dropped.
                // Fall through to backoff and retry — a stopped service is a
                // normal condition, not an error worth surfacing as one.
            }

            FailAllPending();
            SetState(ConnectionState.Disconnected);
            ClosePipe();

            try { await Task.Delay(backoff, ct); }
            catch (OperationCanceledException) { break; }

            backoff = TimeSpan.FromMilliseconds(
                Math.Min(backoff.TotalMilliseconds * 2, BackoffMax.TotalMilliseconds));
        }

        FailAllPending();
        SetState(ConnectionState.Disconnected);
    }

    private async Task ReadLoopAsync(NamedPipeClientStream pipe, CancellationToken ct)
    {
        using var reader = new StreamReader(pipe, Encoding.UTF8);

        while (!ct.IsCancellationRequested)
        {
            string? line = await reader.ReadLineAsync(ct);
            if (line is null) return;                       // server closed the pipe
            if (line.Length == 0) continue;
            Dispatch(line);
        }
    }

    private void ClosePipe()
    {
        _writer = null;
        var p = _pipe;
        _pipe = null;
        try { p?.Dispose(); } catch { /* already gone */ }
    }

    private void SetState(ConnectionState s)
    {
        lock (_stateLock)
        {
            if (_state == s) return;
            _state = s;
        }
        StateChanged?.Invoke(s);
    }

    private void FailAllPending()
    {
        // Resolve with NULL, not default(JsonElement). A default JsonElement has
        // ValueKind == Undefined, and both TryGetProperty and Deserialize throw
        // InvalidOperationException on it — so a timeout would surface as an
        // exception instead of the honest "no reply" the callers check for.
        foreach (var kv in _pending)
        {
            if (_pending.TryRemove(kv.Key, out var tcs)) tcs.TrySetResult(null);
        }
    }

    // ── message handling ─────────────────────────────────────

    private void Dispatch(string line)
    {
        JsonElement root;
        try
        {
            using var doc = JsonDocument.Parse(line);
            root = doc.RootElement.Clone();               // survives the using
        }
        catch (JsonException)
        {
            return;
        }

        if (root.ValueKind != JsonValueKind.Object) return;
        if (!root.TryGetProperty("type", out var typeEl) ||
            typeEl.ValueKind != JsonValueKind.String)
        {
            return;
        }

        // Unsolicited event — never carries a request-correlation id.
        if (typeEl.GetString() == Types.Alert)
        {
            var rec = Deserialize<AlertRecord>(root);
            if (rec is not null) AlertReceived?.Invoke(rec);
            return;
        }

        // A reply: hand it to whoever is waiting on that id.
        if (root.TryGetProperty("id", out var idEl) && idEl.TryGetInt64(out long id) &&
            _pending.TryRemove(id, out var tcs))
        {
            tcs.TrySetResult(root);
        }
    }

    private static T? Deserialize<T>(JsonElement el)
    {
        // Undefined means "no reply" leaked through somewhere — treat it as
        // absent rather than letting it throw.
        if (el.ValueKind == JsonValueKind.Undefined) return default;

        try { return el.Deserialize<T>(JsonOpts); }
        catch (JsonException) { return default; }
        catch (NotSupportedException) { return default; }
        catch (InvalidOperationException) { return default; }
    }

    // ── request plumbing ─────────────────────────────────────

    private async Task<JsonElement?> RequestAsync(JsonObject req, CancellationToken ct)
    {
        if (State != ConnectionState.Connected) return null;

        long id = Interlocked.Increment(ref _nextId);
        req["id"] = id;

        var tcs = new TaskCompletionSource<JsonElement?>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = tcs;

        try
        {
            await _writeLock.WaitAsync(ct);
            try
            {
                var writer = _writer;
                if (writer is null) return null;
                await writer.WriteLineAsync(req.ToJsonString().AsMemory(), ct);
            }
            finally
            {
                _writeLock.Release();
            }

            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(RequestTimeout);
            using var reg = timeout.Token.Register(() => tcs.TrySetResult(null));

            return await tcs.Task;
        }
        catch (OperationCanceledException)
        {
            return null;
        }
        catch (Exception)
        {
            // Broken pipe mid-write — the run loop will notice and reconnect.
            return null;
        }
        finally
        {
            _pending.TryRemove(id, out _);
        }
    }

    /// <summary>
    /// Sends a pre-built request and returns the raw reply JSON, or null.
    /// Used by the one-shot elevated helper process, which is handed a verb
    /// on its command line and must forward it verbatim.
    /// </summary>
    public async Task<string?> SendRawAsync(string requestJson, CancellationToken ct = default)
    {
        JsonObject? obj;
        try { obj = JsonNode.Parse(requestJson) as JsonObject; }
        catch (JsonException) { return null; }

        if (obj is null) return null;
        obj.Remove("id");                                  // correlation is ours to assign

        var reply = await RequestAsync(obj, ct);
        return reply?.GetRawText();
    }

    /// <summary>Waits until the pipe is connected, or the timeout elapses.</summary>
    public async Task<bool> WaitForConnectionAsync(TimeSpan timeout, CancellationToken ct = default)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (State == ConnectionState.Connected) return true;
            try { await Task.Delay(100, ct); }
            catch (OperationCanceledException) { return false; }
        }
        return State == ConnectionState.Connected;
    }

    // ── typed read verbs ─────────────────────────────────────

    public async Task<StatusSnapshot?> GetStatusAsync(CancellationToken ct = default)
    {
        var reply = await RequestAsync(new JsonObject { ["type"] = Verbs.GetStatus }, ct);
        return reply is null ? null : Deserialize<StatusSnapshot>(reply.Value);
    }

    public async Task<ShieldCordConfig?> GetConfigAsync(CancellationToken ct = default)
    {
        var reply = await RequestAsync(new JsonObject { ["type"] = Verbs.GetConfig }, ct);
        return reply is null ? null : Deserialize<ShieldCordConfig>(reply.Value);
    }

    public async Task<IReadOnlyList<AlertRecord>> GetAlertsAsync(long since = 0,
                                                                 CancellationToken ct = default)
    {
        var reply = await RequestAsync(
            new JsonObject { ["type"] = Verbs.GetAlerts, ["since"] = since }, ct);
        return ReadArray<AlertRecord>(reply, "records");
    }

    public async Task<IReadOnlyList<ProtectedApp>> GetProtectedAppsAsync(CancellationToken ct = default)
    {
        var reply = await RequestAsync(new JsonObject { ["type"] = Verbs.GetProtectedApps }, ct);
        return ReadArray<ProtectedApp>(reply, "apps");
    }

    public async Task<IReadOnlyList<ProtectedPath>> GetProtectedPathsAsync(CancellationToken ct = default)
    {
        var reply = await RequestAsync(new JsonObject { ["type"] = Verbs.GetProtectedPaths }, ct);
        return ReadArray<ProtectedPath>(reply, "paths");
    }

    private static IReadOnlyList<T> ReadArray<T>(JsonElement? reply, string property)
    {
        if (reply is null || reply.Value.ValueKind != JsonValueKind.Object)
            return Array.Empty<T>();

        if (!reply.Value.TryGetProperty(property, out var arr) ||
            arr.ValueKind != JsonValueKind.Array)
        {
            return Array.Empty<T>();
        }

        var list = new List<T>(arr.GetArrayLength());
        foreach (var el in arr.EnumerateArray())
        {
            var item = Deserialize<T>(el);
            if (item is not null) list.Add(item);
        }
        return list;
    }
}
