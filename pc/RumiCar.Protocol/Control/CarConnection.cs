using System.Diagnostics;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using RumiCar.Protocol.Telemetry;

namespace RumiCar.Protocol.Control;

/// <summary>
/// Client side of the car's two TCP channels: control (JSON lines) and
/// telemetry (binary frames).  All callbacks are raised on the thread pool;
/// the window marshals them to the UI thread.
///
/// The link is watched (docs 5.1): a heartbeat goes to the car 250 ms after the
/// previous one was answered (or found unanswered after <see cref="RequestTimeout"/>;
/// the heartbeats go on in any case), and when nothing at all
/// (telemetry frame, response or event) has come from the car for
/// <see cref="LinkTimeout"/> the connection is closed and
/// <see cref="Disconnected"/> is raised, so that the PC does not keep showing
/// a connection that the Wi-Fi has lost.  <see cref="Disconnected"/> is raised
/// at most once per connection, and never after <see cref="DisposeAsync"/>.
/// </summary>
public sealed class CarConnection : IAsyncDisposable
{
    /// <summary>Interval of the heartbeat requests (the car stops in manual mode after 1 s without one).</summary>
    public static readonly TimeSpan HeartbeatInterval = TimeSpan.FromMilliseconds(250);

    /// <summary>Link timeout used when the car does not tell its own (core 0.7.0 and earlier).</summary>
    public static readonly TimeSpan DefaultLinkTimeout = TimeSpan.FromMilliseconds(5000);

    /// <summary>How long a request waits for its response.</summary>
    public static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(5);

    private static readonly TimeSpan WatchInterval = TimeSpan.FromMilliseconds(100);

    private readonly string _host;
    private readonly int _controlPort;
    private readonly int _telemetryPort;
    private readonly CancellationTokenSource _cts = new();
    private TcpClient? _control;
    private StreamWriter? _controlWriter;
    private TcpClient? _telemetry;
    private readonly SemaphoreSlim _sendLock = new(1, 1);
    private readonly Dictionary<long, TaskCompletionSource<ControlResponse>> _pending = new();
    private long _nextId;
    private Task? _controlReader;
    private Task? _telemetryReader;
    private Task? _heartbeat;
    private Task? _linkWatch;
    private long _lastReceived = Stopwatch.GetTimestamp();
    private int _ended;   // 1 once the connection has failed or been disposed
    private volatile Exception? _endReason;

    public event Action<ControlEvent>? EventReceived;
    public event Action<TelemetryFrame>? FrameReceived;
    public event Action<string>? Log;

    /// <summary>The connection was lost (null: the car closed it).  Not raised after <see cref="DisposeAsync"/>.</summary>
    public event Action<Exception?>? Disconnected;

    public CarConnection(string host) : this(host, Ports.Control, Ports.Telemetry) { }

    /// <summary>For a car that listens on other ports (the defaults are <see cref="Ports"/>).</summary>
    public CarConnection(string host, int controlPort, int telemetryPort)
    {
        _host = host;
        _controlPort = controlPort;
        _telemetryPort = telemetryPort;
    }

    public bool IsConnected => Volatile.Read(ref _ended) == 0 && _control?.Connected == true;

    /// <summary>Result of the hello request (car name, core_version, protocol, client_timeout_ms), once connected.</summary>
    public JsonElement? HelloResult { get; private set; }

    /// <summary>
    /// No data from the car for this long ends the connection: the car's
    /// network.client_timeout_ms (told in hello), otherwise <see cref="DefaultLinkTimeout"/>.
    /// The car closes a silent control client after the same time.
    /// </summary>
    public TimeSpan LinkTimeout { get; private set; } = DefaultLinkTimeout;

    public async Task ConnectAsync(string clientName, CancellationToken ct)
    {
        _control = new TcpClient { NoDelay = true };
        await _control.ConnectAsync(_host, _controlPort, ct).ConfigureAwait(false);
        var stream = _control.GetStream();
        _controlWriter = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
        MarkReceived();
        _controlReader = Task.Run(() => ReadControlLoopAsync(stream, _cts.Token));

        var hello = await RequestAsync(ControlCodec.Hello(NextId(), clientName), ct).ConfigureAwait(false);
        if (!hello.Ok) throw new IOException("hello rejected: " + hello.Error);
        HelloResult = hello.Result;
        LinkTimeout = LinkTimeoutFromHello(hello.Result);
        Log?.Invoke($"control connected to {_host}:{_controlPort}");

        _telemetry = new TcpClient();
        await _telemetry.ConnectAsync(_host, _telemetryPort, ct).ConfigureAwait(false);
        MarkReceived();
        _telemetryReader = Task.Run(() => ReadTelemetryLoopAsync(_telemetry.GetStream(), _cts.Token));
        Log?.Invoke($"telemetry connected to {_host}:{_telemetryPort}");

        _heartbeat = Task.Run(() => HeartbeatLoopAsync(_cts.Token));
        _linkWatch = Task.Run(() => LinkWatchLoopAsync(_cts.Token));
    }

    /// <summary>client_timeout_ms from the hello result (1000 ms or more), otherwise <see cref="DefaultLinkTimeout"/>.</summary>
    public static TimeSpan LinkTimeoutFromHello(JsonElement? hello)
    {
        if (hello is JsonElement e && e.ValueKind == JsonValueKind.Object && e.TryGetProperty("client_timeout_ms", out var v)
            && v.TryGetInt32(out var ms) && ms >= 1000)
            return TimeSpan.FromMilliseconds(ms);
        return DefaultLinkTimeout;
    }

    public long NextId() => Interlocked.Increment(ref _nextId);

    /// <summary>Sends a request and waits for the matching response (at most <see cref="RequestTimeout"/>).</summary>
    public async Task<ControlResponse> RequestAsync(ControlRequest request, CancellationToken ct)
    {
        var writer = _controlWriter ?? throw new InvalidOperationException("not connected");
        var tcs = new TaskCompletionSource<ControlResponse>(TaskCreationOptions.RunContinuationsAsynchronously);
        // Registered before the check, so a connection that ends meanwhile fails this request too.
        lock (_pending) _pending[request.Id] = tcs;
        try
        {
            if (Volatile.Read(ref _ended) != 0) throw Ended();
            await _sendLock.WaitAsync(ct).ConfigureAwait(false);
            try
            {
                await writer.WriteLineAsync(ControlCodec.Encode(request).AsMemory(), ct).ConfigureAwait(false);
            }
            finally
            {
                _sendLock.Release();
            }
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct, _cts.Token);
            timeout.CancelAfter(RequestTimeout);
            try
            {
                return await tcs.Task.WaitAsync(timeout.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (!ct.IsCancellationRequested && Volatile.Read(ref _ended) == 0)
            {
                throw new TimeoutException($"車から {RequestTimeout.TotalSeconds:F0} 秒以内に応答がありません");
            }
        }
        catch (Exception e) when (Volatile.Read(ref _ended) != 0 && !ct.IsCancellationRequested && e is not TimeoutException)
        {
            // The connection ended (lost, refused, closed by the car or disconnected): say why.
            throw ReferenceEquals(e, _endReason) ? e : Ended();
        }
        finally
        {
            lock (_pending) _pending.Remove(request.Id);
        }
    }

    private IOException Ended()
    {
        var reason = _endReason;
        return new IOException(reason?.Message ?? "車との接続は終了しています", reason);
    }

    private void MarkReceived() => Volatile.Write(ref _lastReceived, Stopwatch.GetTimestamp());

    /// <summary>Time since the last data from the car.</summary>
    public TimeSpan SinceLastReceived => Stopwatch.GetElapsedTime(Volatile.Read(ref _lastReceived));

    private async Task ReadControlLoopAsync(NetworkStream stream, CancellationToken ct)
    {
        Exception? failure = null;
        try
        {
            using var reader = new StreamReader(stream, new UTF8Encoding(false));
            while (!ct.IsCancellationRequested)
            {
                var line = await reader.ReadLineAsync(ct).ConfigureAwait(false);
                if (line is null) break;
                MarkReceived();
                if (!ControlCodec.TryDecode(line, out var msg))
                {
                    Log?.Invoke("control: unreadable line ignored");
                    continue;
                }
                switch (msg)
                {
                    case ControlResponse { Id: 0, Ok: false } refusal:
                        // Not an answer to a request: the car refused the connection
                        // ("busy: another client is connected") and closes it.
                        End(new IOException("車が接続を拒否しました: " + refusal.Error));
                        return;
                    case ControlResponse r:
                        TaskCompletionSource<ControlResponse>? tcs;
                        lock (_pending) _pending.TryGetValue(r.Id, out tcs);
                        tcs?.TrySetResult(r);
                        break;
                    case ControlEvent e:
                        EventReceived?.Invoke(e);
                        break;
                }
            }
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested) { return; }
        catch (Exception e) { failure = e; }
        End(failure);
    }

    private async Task ReadTelemetryLoopAsync(NetworkStream stream, CancellationToken ct)
    {
        Exception? failure = null;
        try
        {
            var reader = new TelemetryFrameReader(stream);
            while (!ct.IsCancellationRequested)
            {
                var frame = await reader.ReadAsync(ct).ConfigureAwait(false);
                if (frame is null) break;
                MarkReceived();
                FrameReceived?.Invoke(frame);
            }
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested) { return; }
        catch (Exception e) { failure = e; }
        // Without telemetry there is nothing to show or record: the connection is over.
        End(failure is null ? new IOException("車がテレメトリの接続を閉じました") : new IOException("テレメトリ: " + failure.Message, failure));
    }

    /// <summary>
    /// A heartbeat every <see cref="HeartbeatInterval"/>.  A heartbeat without an
    /// answer does not stop the loop: it goes on until the connection ends.
    /// </summary>
    private async Task HeartbeatLoopAsync(CancellationToken ct)
    {
        var failing = false;
        while (!ct.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(HeartbeatInterval, ct).ConfigureAwait(false);
                var r = await RequestAsync(ControlCodec.Heartbeat(NextId()), ct).ConfigureAwait(false);
                if (failing && r.Ok)
                {
                    failing = false;
                    Log?.Invoke("heartbeat: 車からの応答が戻りました");
                }
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested) { return; }
            catch (Exception e)
            {
                if (Volatile.Read(ref _ended) != 0) return;
                if (!failing)
                {
                    failing = true;
                    Log?.Invoke("heartbeat: " + e.Message);
                }
            }
        }
    }

    private async Task LinkWatchLoopAsync(CancellationToken ct)
    {
        try
        {
            while (!ct.IsCancellationRequested)
            {
                await Task.Delay(WatchInterval, ct).ConfigureAwait(false);
                var silent = SinceLastReceived;
                if (silent > LinkTimeout)
                {
                    End(new TimeoutException($"通信途絶（車から {silent.TotalSeconds:F1} 秒間受信なし）"));
                    return;
                }
            }
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested) { }
    }

    private void FailPending(Exception e)
    {
        List<TaskCompletionSource<ControlResponse>> waiting;
        lock (_pending) waiting = _pending.Values.ToList();
        foreach (var tcs in waiting) tcs.TrySetException(e);
    }

    /// <summary>The connection is over: close both channels, fail the waiting requests, tell once.</summary>
    private void End(Exception? reason)
    {
        if (Interlocked.Exchange(ref _ended, 1) != 0) return;
        _endReason = reason ?? new IOException("車が接続を閉じました");
        FailPending(_endReason);   // before the cancellation, so the waiting requests carry the reason
        try { _cts.Cancel(); } catch (ObjectDisposedException) { }
        _control?.Close();
        _telemetry?.Close();
        Disconnected?.Invoke(reason);
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _ended, 1) == 0)   // a disconnect asked for is not reported as lost
        {
            _endReason = new IOException("切断しました");
            FailPending(_endReason);
        }
        _cts.Cancel();
        _control?.Close();
        _telemetry?.Close();
        foreach (var t in new[] { _controlReader, _telemetryReader, _heartbeat, _linkWatch })
        {
            if (t is null) continue;
            try { await t.ConfigureAwait(false); } catch (Exception) { }
        }
        _cts.Dispose();
        _sendLock.Dispose();
    }

    /// <summary>Extracts a string field from a status event's data, if present.</summary>
    public static string? GetString(JsonElement? data, string name)
    {
        if (data is JsonElement e && e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String)
            return v.GetString();
        return null;
    }
}
