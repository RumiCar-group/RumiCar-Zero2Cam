using System.Text.Json;
using RumiCar.Protocol.Control;
using RumiCar.Protocol.Telemetry;

namespace RumiCar.Protocol.Link;

/// <summary>
/// One run of rumicar-core as hello tells it (core 0.8.0 and later): the Linux
/// boot (a new boot id: the car restarted), the start of the core (a new
/// instance id with the same boot id: the core restarted), and how the run
/// before it ended ("clean": through its own shutdown; "unclean": cut by a
/// power loss, crash or SIGKILL; "unknown").
/// </summary>
public sealed record CarRun(string BootId, string InstanceId, string PreviousExit)
{
    /// <summary>Null when hello does not carry boot_id and instance_id (core 0.7.x and earlier).</summary>
    public static CarRun? FromHello(JsonElement? hello)
    {
        var boot = CarConnection.GetString(hello, "boot_id");
        var instance = CarConnection.GetString(hello, "instance_id");
        if (string.IsNullOrEmpty(boot) || string.IsNullOrEmpty(instance)) return null;
        return new CarRun(boot, instance, CarConnection.GetString(hello, "previous_exit") ?? "unknown");
    }
}

/// <summary>What happened to the PC's Wi-Fi (from the Windows WLAN notifications).</summary>
public enum PcWifiChange
{
    /// <summary>The PC's Wi-Fi left the car's network (and is not on any network).</summary>
    LeftCarNetwork,
    /// <summary>The PC's Wi-Fi is now on another network (<see cref="PcWifiEvent.Network"/>).</summary>
    MovedToOtherNetwork,
    /// <summary>The PC's Wi-Fi is on the car's network again.</summary>
    BackOnCarNetwork,
    /// <summary>An attempt to join the car's network failed (<see cref="PcWifiEvent.Reason"/>).</summary>
    ConnectFailed,
}

public sealed record PcWifiEvent(DateTime At, PcWifiChange Change, string Network, string Reason);

/// <summary>One telemetry frame's driving and power state, kept for the seconds before a loss.</summary>
public sealed record DriveSample(DateTime At, string Mode, int DriveDir, int DrivePwm, uint Throttled);

/// <summary>
/// The last <see cref="Window"/> of telemetry (thread-safe: frames come on the
/// telemetry thread, the loss is handled on the UI thread).
/// </summary>
public sealed class DriveHistory
{
    public static readonly TimeSpan Window = TimeSpan.FromSeconds(3);
    private readonly Queue<DriveSample> _samples = new();

    public void Add(DriveSample sample)
    {
        lock (_samples)
        {
            _samples.Enqueue(sample);
            while (_samples.Count > 0 && sample.At - _samples.Peek().At > Window) _samples.Dequeue();
        }
    }

    public IReadOnlyList<DriveSample> Snapshot()
    {
        lock (_samples) return _samples.ToList();
    }

    public void Clear()
    {
        lock (_samples) _samples.Clear();
    }
}

/// <summary>A lost connection and what was known about it.</summary>
public sealed class LinkLoss
{
    public required DateTime LostAt { get; init; }
    public required LinkEnd End { get; init; }
    /// <summary>The loss as the connection reported it.</summary>
    public required string Message { get; init; }
    /// <summary>The car's run before the loss (null: the car did not tell it).</summary>
    public CarRun? Before { get; init; }
    /// <summary>The car's power flags last received before the loss.</summary>
    public uint ThrottledBefore { get; init; }
    /// <summary>Telemetry of the last seconds before the loss.</summary>
    public IReadOnlyList<DriveSample> LastDrive { get; init; } = Array.Empty<DriveSample>();
    /// <summary>The PC's Wi-Fi was being watched (WLAN notifications); without it, the absence of events says nothing.</summary>
    public bool PcWifiWatched { get; init; }
    /// <summary>What the PC's Wi-Fi did from <see cref="LinkDiagnosis.WifiLookBack"/> before the loss until the reconnect.</summary>
    public List<PcWifiEvent> PcWifi { get; } = new();
}

/// <summary>A headline for the window and the lines that support it, for the log.</summary>
public sealed record Diagnosis(string Headline, IReadOnlyList<string> Details);

/// <summary>
/// Why the connection to the car was lost (docs 5.1): first at the loss, from
/// how it ended, the PC's Wi-Fi and the last telemetry; then after the
/// reconnect, from comparing the car's run (hello) before and after and the
/// car's power flags.  The words say what was observed, and what it suggests
/// only as a possibility.
/// </summary>
public static class LinkDiagnosis
{
    /// <summary>PC Wi-Fi events this long before the loss count as part of it.</summary>
    public static readonly TimeSpan WifiLookBack = TimeSpan.FromSeconds(10);

    public static Diagnosis AtLoss(LinkLoss loss)
    {
        var details = new List<string>();
        AddWifi(loss, loss.LostAt + TimeSpan.FromSeconds(1), details, afterReconnect: false);
        AddPowerBefore(loss, details);
        AddDrive(loss, details);
        var headline = loss.End switch
        {
            LinkEnd.Silent => "車から受信が途絶えました",
            LinkEnd.ClosedByCar => "車が接続を閉じました",
            LinkEnd.Refused => "車が接続を拒否しました",
            LinkEnd.Aborted => loss.Message,
            _ => "通信エラーで切断されました",
        };
        return new Diagnosis(headline, details);
    }

    public static Diagnosis AfterReconnect(LinkLoss loss, CarRun? after, uint throttledAfter, DateTime reconnectedAt)
    {
        var details = new List<string> { $"通信断の時間: 約 {(reconnectedAt - loss.LostAt).TotalSeconds:F0} 秒" };
        var moved = Events(loss, reconnectedAt).LastOrDefault(e => e.Change == PcWifiChange.MovedToOtherNetwork);
        var left = Events(loss, reconnectedAt).Any(e => e.Change == PcWifiChange.LeftCarNetwork);
        string headline;

        if (after is null || loss.Before is null)
        {
            details.Add(after is null
                ? "車の本体が 0.8.0 より前のため、車の再起動の有無は判定できません"
                : "切断前の接続では車の起動の識別情報がなかったため、車の再起動の有無は判定できません");
            headline = AtLoss(loss).Headline;
            AddWifi(loss, reconnectedAt, details, afterReconnect: true);
        }
        else if (after.BootId != loss.Before.BootId)
        {
            headline = after.PreviousExit switch
            {
                "unclean" => "車が再起動していました（電源が落ちた後の起動）",
                "clean" => "車が再起動されていました（正常に終了した後の起動）",
                _ => "車が再起動していました",
            };
            if (after.PreviousExit == "unclean")
                details.Add("前回の rumicar-core は終了処理を経ずに止まっています（電源断、または電圧低下による Raspberry Pi のリセットなど）");
        }
        else if (after.InstanceId != loss.Before.InstanceId)
        {
            headline = after.PreviousExit switch
            {
                "unclean" => "rumicar-core が異常終了し、自動で再起動していました",
                "clean" => "rumicar-core が再起動されていました（停止・更新・切り替えなど）",
                _ => "rumicar-core が再起動していました",
            };
            if (after.PreviousExit == "unclean")
                details.Add("車で journalctl -u rumicar-core -b を実行すると終了の記録を確認できます");
            AddUnderVoltageDuring(loss, throttledAfter, details);
        }
        else
        {
            if (moved is not null)
            {
                headline = $"PC の Wi-Fi が別のネットワーク（{moved.Network}）に切り替わっていました";
                details.Add("Windows はインターネットに接続できないネットワークから、自動で別のネットワークへ切り替えることがあります（README 10 章）");
            }
            else if (left)
            {
                headline = "PC の Wi-Fi が車のネットワークから切断されていました（車は動作を続けていました）";
                details.Add("車の電波が届かなかった、または車の Wi-Fi（アクセスポイント）が一時的に止まった可能性があります");
            }
            else if (loss.End == LinkEnd.Refused)
            {
                headline = "車が接続を拒否しました（別の接続が残っていました）";
            }
            else if (loss.PcWifiWatched)
            {
                headline = "車は動作を続け、PC の Wi-Fi も接続したままで、通信だけが途絶えました";
                details.Add("電波の途切れ（距離・障害物・混信）、または車の Wi-Fi の一時的な停止の可能性があります");
            }
            else
            {
                headline = "車は動作を続けていました（通信だけが途絶えました）";
                details.Add("PC の Wi-Fi の状態は監視できませんでした");
            }
            AddUnderVoltageDuring(loss, throttledAfter, details);
            if (moved is null) AddWifi(loss, reconnectedAt, details, afterReconnect: true);
        }

        AddPowerBefore(loss, details);
        if ((throttledAfter & PowerFlags.UnderVoltageNow) != 0) details.Add("再接続の時点でも、車は低電圧（現在）を報告しています");
        AddDrive(loss, details);
        return new Diagnosis(headline, details);
    }

    private static IEnumerable<PcWifiEvent> Events(LinkLoss loss, DateTime until) =>
        loss.PcWifi.Where(e => e.At >= loss.LostAt - WifiLookBack && e.At <= until);

    private static void AddWifi(LinkLoss loss, DateTime until, List<string> details, bool afterReconnect)
    {
        var events = Events(loss, until).ToList();
        foreach (var e in events)
        {
            var when = e.At.ToString("HH:mm:ss");
            var reason = e.Reason.Length > 0 ? $"、理由: {e.Reason}" : "";
            switch (e.Change)
            {
                case PcWifiChange.LeftCarNetwork:
                    details.Add($"{when} PC の Wi-Fi が車のネットワークから切断されました{reason}");
                    break;
                case PcWifiChange.MovedToOtherNetwork:
                    details.Add($"{when} PC の Wi-Fi が {e.Network} に接続しました");
                    break;
                case PcWifiChange.BackOnCarNetwork:
                    if (afterReconnect) details.Add($"{when} PC の Wi-Fi が車のネットワークに戻りました");
                    break;
                case PcWifiChange.ConnectFailed:
                    if (afterReconnect) details.Add($"{when} 車のネットワークへの接続に失敗しました{reason}");
                    break;
            }
        }
        if (!afterReconnect && loss.PcWifiWatched && !events.Any(e => e.Change is PcWifiChange.LeftCarNetwork or PcWifiChange.MovedToOtherNetwork))
            details.Add("PC の Wi-Fi は車のネットワークに接続したままでした");
    }

    private static void AddPowerBefore(LinkLoss loss, List<string> details)
    {
        if ((loss.ThrottledBefore & PowerFlags.UnderVoltageNow) != 0)
            details.Add("切断の直前、車のファームウェアは低電圧（現在）を報告していました");
        else if ((loss.ThrottledBefore & PowerFlags.UnderVoltageSinceBoot) != 0)
            details.Add("車は切断より前に、起動後の低電圧を記録していました");
    }

    // The flag "under-voltage since boot" is cleared only by a reboot: newly set with the same boot means during the loss.
    private static void AddUnderVoltageDuring(LinkLoss loss, uint throttledAfter, List<string> details)
    {
        if ((throttledAfter & PowerFlags.UnderVoltageSinceBoot) != 0 && (loss.ThrottledBefore & PowerFlags.UnderVoltageSinceBoot) == 0)
            details.Add("通信断の間に、車で低電圧が発生していました（モーターの負荷などで電源電圧が下がり、車の Wi-Fi が不安定になった可能性があります）");
    }

    private static void AddDrive(LinkLoss loss, List<string> details)
    {
        if (loss.LastDrive.Count == 0) return;
        var moving = loss.LastDrive.Where(s => s.DriveDir is RumiCarApi.Forward or RumiCarApi.Reverse && s.DrivePwm > 0).ToList();
        var seconds = DriveHistory.Window.TotalSeconds;
        if (moving.Count == 0)
        {
            details.Add($"切断前 {seconds:F0} 秒間、駆動は止まっていました");
            return;
        }
        var top = moving.MaxBy(s => s.DrivePwm)!;
        var mode = top.Mode switch { DriveMode.Manual => "手動", DriveMode.Auto => "自動", DriveMode.RecordOnly => "収録のみ", _ => top.Mode };
        details.Add($"切断前 {seconds:F0} 秒間の駆動の最大: RC_drive({RumiCarApi.DriveName(top.DriveDir)}, {top.DrivePwm})（{mode}）");
    }
}
