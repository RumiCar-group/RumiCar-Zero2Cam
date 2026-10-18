using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using RumiCar.Protocol;
using RumiCar.Protocol.Control;
using RumiCar.Protocol.Telemetry;

namespace RumiCar.DriveManager;

/// <summary>
/// Client side of the car's two TCP channels: control (JSON lines) and
/// telemetry (binary frames).  All callbacks are raised on the thread pool;
/// the window marshals them to the UI thread.
/// </summary>
public sealed class CarConnection : IAsyncDisposable
{
    private readonly string _host;
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

    public event Action<ControlEvent>? EventReceived;
    public event Action<TelemetryFrame>? FrameReceived;
    public event Action<string>? Log;
    public event Action<Exception?>? Disconnected;

    public CarConnection(string host) => _host = host;

    public bool IsConnected => _control?.Connected == true;

    public async Task ConnectAsync(string clientName, CancellationToken ct)
    {
        _control = new TcpClient { NoDelay = true };
        await _control.ConnectAsync(_host, Ports.Control, ct).ConfigureAwait(false);
        var stream = _control.GetStream();
        _controlWriter = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
        _controlReader = Task.Run(() => ReadControlLoopAsync(stream, _cts.Token));

        var hello = await RequestAsync(ControlCodec.Hello(NextId(), clientName), ct).ConfigureAwait(false);
        if (!hello.Ok) throw new IOException("hello rejected: " + hello.Error);
        Log?.Invoke($"control connected to {_host}:{Ports.Control}");

        _telemetry = new TcpClient();
        await _telemetry.ConnectAsync(_host, Ports.Telemetry, ct).ConfigureAwait(false);
        _telemetryReader = Task.Run(() => ReadTelemetryLoopAsync(_telemetry.GetStream(), _cts.Token));
        Log?.Invoke($"telemetry connected to {_host}:{Ports.Telemetry}");

        _heartbeat = Task.Run(() => HeartbeatLoopAsync(_cts.Token));
    }

    public long NextId() => Interlocked.Increment(ref _nextId);

    /// <summary>Sends a request and waits for the matching response.</summary>
    public async Task<ControlResponse> RequestAsync(ControlRequest request, CancellationToken ct)
    {
        var writer = _controlWriter ?? throw new InvalidOperationException("not connected");
        var tcs = new TaskCompletionSource<ControlResponse>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_pending) _pending[request.Id] = tcs;
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
        timeout.CancelAfter(TimeSpan.FromSeconds(5));
        try
        {
            return await tcs.Task.WaitAsync(timeout.Token).ConfigureAwait(false);
        }
        finally
        {
            lock (_pending) _pending.Remove(request.Id);
        }
    }

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
                if (!ControlCodec.TryDecode(line, out var msg))
                {
                    Log?.Invoke("control: unreadable line ignored");
                    continue;
                }
                switch (msg)
                {
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
        catch (OperationCanceledException) { }
        catch (Exception e) { failure = e; }
        Disconnected?.Invoke(failure);
    }

    private async Task ReadTelemetryLoopAsync(NetworkStream stream, CancellationToken ct)
    {
        try
        {
            var reader = new TelemetryFrameReader(stream);
            while (!ct.IsCancellationRequested)
            {
                var frame = await reader.ReadAsync(ct).ConfigureAwait(false);
                if (frame is null) break;
                FrameReceived?.Invoke(frame);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception e) { Log?.Invoke("telemetry: " + e.Message); }
    }

    private async Task HeartbeatLoopAsync(CancellationToken ct)
    {
        try
        {
            while (!ct.IsCancellationRequested)
            {
                await Task.Delay(250, ct).ConfigureAwait(false);
                await RequestAsync(ControlCodec.Heartbeat(NextId()), ct).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception e) { Log?.Invoke("heartbeat stopped: " + e.Message); }
    }

    public async ValueTask DisposeAsync()
    {
        _cts.Cancel();
        _control?.Close();
        _telemetry?.Close();
        foreach (var t in new[] { _controlReader, _telemetryReader, _heartbeat })
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
