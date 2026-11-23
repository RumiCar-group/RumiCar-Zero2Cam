using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using RumiCar.Protocol.Control;
using RumiCar.Protocol.Telemetry;
using Xunit;

namespace RumiCar.Protocol.Tests;

/// <summary>
/// CarConnection against a car stand-in that speaks the real wire formats over
/// TCP on 127.0.0.1 (control JSON lines on one port, telemetry frames on the other).
/// </summary>
public class CarConnectionTests
{
    /// <summary>The car side of the two channels; what it answers can be changed while connected.</summary>
    private sealed class FakeCar : IAsyncDisposable
    {
        private readonly TcpListener _control = new(IPAddress.Loopback, 0);
        private readonly TcpListener _telemetry = new(IPAddress.Loopback, 0);
        private readonly CancellationTokenSource _cts = new();
        private readonly List<Task> _tasks = new();
        private readonly List<TcpClient> _clients = new();
        private readonly int? _clientTimeoutMs;
        private readonly bool _refuseBusy;
        private TcpClient? _telemetryClient;

        public volatile bool AnswerHeartbeats = true;
        public volatile bool Silent;                  // sends nothing at all (the link is gone, the sockets stay open)
        public readonly ConcurrentQueue<long> HeartbeatTimes = new();   // Stopwatch timestamps of received heartbeats

        public FakeCar(int? clientTimeoutMs = 1000, bool refuseBusy = false)
        {
            _clientTimeoutMs = clientTimeoutMs;
            _refuseBusy = refuseBusy;
            _control.Start();
            _telemetry.Start();
            _tasks.Add(Task.Run(ControlAcceptAsync));
            _tasks.Add(Task.Run(TelemetryAcceptAsync));
        }

        public int ControlPort => ((IPEndPoint)_control.LocalEndpoint).Port;
        public int TelemetryPort => ((IPEndPoint)_telemetry.LocalEndpoint).Port;

        public void CloseTelemetry() => _telemetryClient?.Close();

        private async Task ControlAcceptAsync()
        {
            using var client = await _control.AcceptTcpClientAsync(_cts.Token);
            lock (_clients) _clients.Add(client);
            var stream = client.GetStream();
            var writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
            if (_refuseBusy)
            {
                // As the car answers a second connection: an error line with id 0, then close.
                await writer.WriteLineAsync("{\"id\":0,\"ok\":false,\"error\":\"busy: another client is connected\"}");
                client.Close();
                return;
            }
            using var reader = new StreamReader(stream, new UTF8Encoding(false));
            try
            {
                while (!_cts.IsCancellationRequested)
                {
                    var line = await reader.ReadLineAsync(_cts.Token);
                    if (line is null) return;
                    using var doc = JsonDocument.Parse(line);
                    var id = doc.RootElement.GetProperty("id").GetInt64();
                    var type = doc.RootElement.GetProperty("type").GetString();
                    if (type == RequestType.Heartbeat)
                    {
                        HeartbeatTimes.Enqueue(Stopwatch.GetTimestamp());
                        if (!AnswerHeartbeats) continue;
                    }
                    if (Silent) continue;
                    var result = type == RequestType.Hello
                        ? (_clientTimeoutMs is int ms
                            ? $"{{\"car\":\"Fake\",\"core_version\":\"9.9.9\",\"protocol\":3,\"client_timeout_ms\":{ms}}}"
                            : "{\"car\":\"Fake\",\"core_version\":\"0.7.0\",\"protocol\":3}")
                        : null;
                    await writer.WriteLineAsync(result is null ? $"{{\"id\":{id},\"ok\":true}}" : $"{{\"id\":{id},\"ok\":true,\"result\":{result}}}");
                }
            }
            catch (Exception) when (_cts.IsCancellationRequested) { }
            catch (IOException) { }
        }

        private async Task TelemetryAcceptAsync()
        {
            TcpClient client;
            try { client = await _telemetry.AcceptTcpClientAsync(_cts.Token); }
            catch (OperationCanceledException) { return; }
            _telemetryClient = client;
            lock (_clients) _clients.Add(client);
            var stream = client.GetStream();
            uint seq = 0;
            try
            {
                while (!_cts.IsCancellationRequested)
                {
                    await Task.Delay(50, _cts.Token);   // 20 frames per second, as the car
                    if (Silent) continue;
                    var frame = new TelemetryFrame { Header = new TelemetryHeader { Seq = seq++, Abi = RumiCarApi.Abi, Mode = "manual" }, Jpeg = Array.Empty<byte>() };
                    var bytes = TelemetryFrameCodec.Encode(frame);
                    await stream.WriteAsync(bytes, _cts.Token);
                }
            }
            catch (Exception) when (_cts.IsCancellationRequested) { }
            catch (IOException) { }
            catch (ObjectDisposedException) { }
        }

        public async ValueTask DisposeAsync()
        {
            _cts.Cancel();
            _control.Stop();
            _telemetry.Stop();
            lock (_clients) foreach (var c in _clients) c.Close();
            foreach (var t in _tasks)
            {
                try { await t; } catch (Exception) { }
            }
            _cts.Dispose();
        }
    }

    private static async Task<(CarConnection Car, TaskCompletionSource<Exception?> Lost)> ConnectAsync(FakeCar fake)
    {
        var car = new CarConnection("127.0.0.1", fake.ControlPort, fake.TelemetryPort);
        var lost = new TaskCompletionSource<Exception?>(TaskCreationOptions.RunContinuationsAsynchronously);
        car.Disconnected += ex => lost.TrySetResult(ex);
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        await car.ConnectAsync("test", cts.Token);
        return (car, lost);
    }

    [Fact]
    public void LinkTimeoutComesFromHello()
    {
        Assert.Equal(TimeSpan.FromMilliseconds(4000), CarConnection.LinkTimeoutFromHello(JsonDocument.Parse("{\"client_timeout_ms\":4000}").RootElement));
        // A car that does not tell it (core 0.7.0 and earlier), or a value out of range: the default.
        Assert.Equal(CarConnection.DefaultLinkTimeout, CarConnection.LinkTimeoutFromHello(JsonDocument.Parse("{\"car\":\"RumiCar\"}").RootElement));
        Assert.Equal(CarConnection.DefaultLinkTimeout, CarConnection.LinkTimeoutFromHello(JsonDocument.Parse("{\"client_timeout_ms\":500}").RootElement));
        Assert.Equal(CarConnection.DefaultLinkTimeout, CarConnection.LinkTimeoutFromHello(null));
        Assert.Equal(TimeSpan.FromMilliseconds(5000), CarConnection.DefaultLinkTimeout);
    }

    [Fact]
    public async Task ConnectsAndUsesTheCarsTimeout()
    {
        await using var fake = new FakeCar(clientTimeoutMs: 2000);
        var (car, lost) = await ConnectAsync(fake);
        await using (car)
        {
            Assert.Equal(TimeSpan.FromMilliseconds(2000), car.LinkTimeout);
            Assert.True(car.IsConnected);
            await Task.Delay(1500);
            Assert.False(lost.Task.IsCompleted);
            Assert.True(fake.HeartbeatTimes.Count >= 3, $"heartbeats sent: {fake.HeartbeatTimes.Count}");
        }
        await Task.Delay(200);
        Assert.False(lost.Task.IsCompleted);   // a disconnect asked for is not reported as lost
    }

    [Fact]
    public async Task SilentLinkIsReportedAsLost()
    {
        await using var fake = new FakeCar(clientTimeoutMs: 1000);
        var (car, lost) = await ConnectAsync(fake);
        await using (car)
        {
            await Task.Delay(500);
            var silentFrom = Stopwatch.GetTimestamp();
            fake.Silent = true;   // nothing more from the car, the connection is not closed
            var finished = await Task.WhenAny(lost.Task, Task.Delay(5000));
            Assert.Same(lost.Task, finished);
            var after = Stopwatch.GetElapsedTime(silentFrom);
            Assert.True(after >= TimeSpan.FromMilliseconds(900), $"reported after {after.TotalMilliseconds} ms");
            Assert.True(after < TimeSpan.FromMilliseconds(2000), $"reported after {after.TotalMilliseconds} ms");
            var ex = await lost.Task;
            Assert.IsType<TimeoutException>(ex);
            Assert.False(car.IsConnected);
            await Assert.ThrowsAnyAsync<Exception>(() => car.RequestAsync(ControlCodec.GetStatus(car.NextId()), CancellationToken.None));
        }
    }

    [Fact]
    public async Task HeartbeatsGoOnAfterOneIsNotAnswered()
    {
        // The link stays up (telemetry flows) while the car leaves heartbeats unanswered for longer than a request waits.
        await using var fake = new FakeCar(clientTimeoutMs: 60000);
        var (car, lost) = await ConnectAsync(fake);
        await using (car)
        {
            var logs = new ConcurrentQueue<string>();
            car.Log += logs.Enqueue;
            fake.AnswerHeartbeats = false;
            await Task.Delay(CarConnection.RequestTimeout + TimeSpan.FromMilliseconds(500));
            fake.AnswerHeartbeats = true;
            var resumedFrom = Stopwatch.GetTimestamp();
            await Task.Delay(1500);
            var after = fake.HeartbeatTimes.Count(t => t > resumedFrom);
            Assert.True(after >= 3, $"heartbeats after the unanswered one: {after}");
            Assert.False(lost.Task.IsCompleted);
            Assert.Contains(logs, l => l.StartsWith("heartbeat:", StringComparison.Ordinal) && l.Contains("応答がありません"));
            Assert.Contains(logs, l => l.Contains("応答が戻りました"));
        }
    }

    [Fact]
    public async Task BusyCarFailsTheConnectionAtOnce()
    {
        await using var fake = new FakeCar(refuseBusy: true);
        var car = new CarConnection("127.0.0.1", fake.ControlPort, fake.TelemetryPort);
        await using (car)
        {
            var started = Stopwatch.GetTimestamp();
            var ex = await Assert.ThrowsAnyAsync<Exception>(() => car.ConnectAsync("test", CancellationToken.None));
            Assert.Contains("busy", ex.Message);
            Assert.True(Stopwatch.GetElapsedTime(started) < TimeSpan.FromSeconds(2), "not after the 5 s request timeout");
        }
    }

    [Fact]
    public async Task ClosedTelemetryEndsTheConnection()
    {
        await using var fake = new FakeCar(clientTimeoutMs: 60000);
        var (car, lost) = await ConnectAsync(fake);
        await using (car)
        {
            await Task.Delay(300);
            fake.CloseTelemetry();
            var finished = await Task.WhenAny(lost.Task, Task.Delay(3000));
            Assert.Same(lost.Task, finished);
            Assert.IsAssignableFrom<IOException>(await lost.Task);
        }
    }
}
