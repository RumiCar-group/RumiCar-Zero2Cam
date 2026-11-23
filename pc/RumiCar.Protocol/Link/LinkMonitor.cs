using System.Text.Json;
using RumiCar.Protocol.Control;
using RumiCar.Protocol.Telemetry;

namespace RumiCar.Protocol.Link;

/// <summary>
/// What the drive manager keeps about the link to tell why it was lost
/// (docs 5.1): the car's run from hello, the last seconds of telemetry, and
/// what the PC's Wi-Fi did.  <see cref="Lost"/> gives the first diagnosis at
/// the loss, <see cref="Reconnected"/> the final one once the car answers
/// again.  Thread-safe: frames come on the telemetry thread.
/// </summary>
public sealed class LinkMonitor
{
    private static readonly TimeSpan KeepWifiEvents = TimeSpan.FromSeconds(30);

    private readonly object _lock = new();
    private readonly DriveHistory _history = new();
    private readonly List<PcWifiEvent> _recentWifi = new();
    private CarRun? _run;
    private uint _throttled;
    private LinkLoss? _pending;

    /// <summary>The PC's Wi-Fi is watched (WLAN notifications arrive): the absence of an event means something.</summary>
    public bool PcWifiWatched { get; set; }

    /// <summary>A loss waiting for the reconnect that completes its diagnosis.</summary>
    public bool HasPendingLoss
    {
        get { lock (_lock) return _pending is not null; }
    }

    /// <summary>A connection is established (the first one, or one after 「切断」): nothing pending.</summary>
    public void Connected(JsonElement? hello)
    {
        lock (_lock)
        {
            _run = CarRun.FromHello(hello);
            _pending = null;
            _throttled = 0;
            _history.Clear();
        }
    }

    public void OnFrame(TelemetryHeader header, DateTime at)
    {
        _history.Add(new DriveSample(at, header.Mode, header.DriveDir, header.DrivePwm, header.Throttled));
        lock (_lock) _throttled = header.Throttled;
    }

    /// <summary>The car's power flags from a status (get_status, status event).</summary>
    public void OnThrottled(uint flags)
    {
        lock (_lock) _throttled = flags;
    }

    public void OnPcWifi(PcWifiEvent e)
    {
        lock (_lock)
        {
            _recentWifi.Add(e);
            _recentWifi.RemoveAll(x => e.At - x.At > KeepWifiEvents);
            _pending?.PcWifi.Add(e);
        }
    }

    /// <summary>The connection was lost: the diagnosis from what is known now.</summary>
    public Diagnosis Lost(DateTime at, LinkEnd end, string message)
    {
        lock (_lock)
        {
            var loss = new LinkLoss
            {
                LostAt = at,
                End = end,
                Message = message,
                Before = _run,
                ThrottledBefore = _throttled,
                LastDrive = _history.Snapshot(),
                PcWifiWatched = PcWifiWatched,
            };
            loss.PcWifi.AddRange(_recentWifi.Where(e => e.At >= at - LinkDiagnosis.WifiLookBack));
            _pending = loss;
            _history.Clear();
            return LinkDiagnosis.AtLoss(loss);
        }
    }

    /// <summary>
    /// Connected again after a loss: the final diagnosis (null when no loss was
    /// pending).  <paramref name="throttled"/>: the power flags of the car's
    /// first status after the reconnect.
    /// </summary>
    public Diagnosis? Reconnected(JsonElement? hello, uint throttled, DateTime at)
    {
        lock (_lock)
        {
            var loss = _pending;
            _pending = null;
            var after = CarRun.FromHello(hello);
            _run = after;
            _throttled = throttled;
            _history.Clear();
            return loss is null ? null : LinkDiagnosis.AfterReconnect(loss, after, throttled, at);
        }
    }

    /// <summary>「切断」 or the window closing: a pending loss is dropped.</summary>
    public void Reset()
    {
        lock (_lock)
        {
            _pending = null;
            _run = null;
            _history.Clear();
        }
    }
}
