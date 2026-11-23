using RumiCar.Protocol.Control;
using RumiCar.Protocol.Link;
using RumiCar.Protocol.Telemetry;
using Xunit;

namespace RumiCar.Protocol.Tests;

public class LinkDiagnosisTests
{
    private static readonly DateTime T0 = new(2026, 11, 23, 14, 0, 0);
    private static readonly CarRun Run = new("boot-1", "inst-1", "clean");

    private static LinkLoss Loss(LinkEnd end = LinkEnd.Silent, uint throttled = 0, bool watched = true, params DriveSample[] drive) => new()
    {
        LostAt = T0,
        End = end,
        Message = "通信途絶（車から 5.1 秒間受信なし）",
        Before = Run,
        ThrottledBefore = throttled,
        LastDrive = drive,
        PcWifiWatched = watched,
    };

    [Fact]
    public void CarRebootAfterAPowerCut()
    {
        var loss = Loss(throttled: PowerFlags.UnderVoltageNow | PowerFlags.UnderVoltageSinceBoot,
            drive: new[] { new DriveSample(T0.AddSeconds(-1), DriveMode.Manual, RumiCarApi.Forward, 230, 0) });
        var d = LinkDiagnosis.AfterReconnect(loss, new CarRun("boot-2", "inst-9", "unclean"), 0, T0.AddSeconds(41));
        Assert.Equal("車が再起動していました（電源が落ちた後の起動）", d.Headline);
        Assert.Contains("通信断の時間: 約 41 秒", d.Details);
        Assert.Contains(d.Details, l => l.Contains("終了処理を経ずに"));
        Assert.Contains("切断の直前、車のファームウェアは低電圧（現在）を報告していました", d.Details);
        Assert.Contains("切断前 3 秒間の駆動の最大: RC_drive(FORWARD, 230)（手動）", d.Details);
        // After a reboot the since-boot flag starts again: it says nothing about the time of the loss.
        Assert.DoesNotContain(d.Details, l => l.StartsWith("通信断の間に"));
    }

    [Fact]
    public void CarRestartedCleanly()
    {
        var d = LinkDiagnosis.AfterReconnect(Loss(LinkEnd.ClosedByCar), new CarRun("boot-2", "inst-2", "clean"), 0, T0.AddSeconds(50));
        Assert.Equal("車が再起動されていました（正常に終了した後の起動）", d.Headline);
        Assert.DoesNotContain(d.Details, l => l.Contains("終了処理を経ずに"));
    }

    [Fact]
    public void CoreCrashedAndWasRestarted()
    {
        var d = LinkDiagnosis.AfterReconnect(Loss(LinkEnd.ClosedByCar), new CarRun("boot-1", "inst-2", "unclean"), 0, T0.AddSeconds(4));
        Assert.Equal("rumicar-core が異常終了し、自動で再起動していました", d.Headline);
        Assert.Contains(d.Details, l => l.Contains("journalctl -u rumicar-core -b"));
        var clean = LinkDiagnosis.AfterReconnect(Loss(LinkEnd.ClosedByCar), new CarRun("boot-1", "inst-2", "clean"), 0, T0.AddSeconds(4));
        Assert.Equal("rumicar-core が再起動されていました（停止・更新・切り替えなど）", clean.Headline);
    }

    [Fact]
    public void WindowsMovedThePcToAnotherNetwork()
    {
        var loss = Loss(LinkEnd.Aborted);
        loss.PcWifi.Add(new PcWifiEvent(T0, PcWifiChange.MovedToOtherNetwork, "Home-5G", ""));
        loss.PcWifi.Add(new PcWifiEvent(T0.AddSeconds(3), PcWifiChange.BackOnCarNetwork, "RumiCar", ""));
        var d = LinkDiagnosis.AfterReconnect(loss, Run, 0, T0.AddSeconds(6));
        Assert.Equal("PC の Wi-Fi が別のネットワーク（Home-5G）に切り替わっていました", d.Headline);
        Assert.Contains(d.Details, l => l.Contains("README 10 章"));
    }

    [Fact]
    public void PcLeftTheCarNetworkWhileTheCarRanOn()
    {
        var loss = Loss();
        loss.PcWifi.Add(new PcWifiEvent(T0.AddSeconds(-2), PcWifiChange.LeftCarNetwork, "RumiCar", "ネットワークが利用できません"));
        loss.PcWifi.Add(new PcWifiEvent(T0.AddSeconds(5), PcWifiChange.BackOnCarNetwork, "RumiCar", ""));
        var atLoss = LinkDiagnosis.AtLoss(loss);
        Assert.Equal("車から受信が途絶えました", atLoss.Headline);
        Assert.Contains("13:59:58 PC の Wi-Fi が車のネットワークから切断されました、理由: ネットワークが利用できません", atLoss.Details);
        Assert.DoesNotContain(atLoss.Details, l => l.Contains("戻りました"));   // after the loss: not yet known at the loss

        var d = LinkDiagnosis.AfterReconnect(loss, Run, PowerFlags.UnderVoltageSinceBoot, T0.AddSeconds(8));
        Assert.Equal("PC の Wi-Fi が車のネットワークから切断されていました（車は動作を続けていました）", d.Headline);
        Assert.Contains("14:00:05 PC の Wi-Fi が車のネットワークに戻りました", d.Details);
        // The since-boot flag was not set before and is now, with the same boot: under-voltage during the loss.
        Assert.Contains(d.Details, l => l.StartsWith("通信断の間に、車で低電圧が発生していました"));
    }

    [Fact]
    public void OnlyTheLinkWasLost()
    {
        var atLoss = LinkDiagnosis.AtLoss(Loss(drive: new DriveSample(T0.AddSeconds(-1), DriveMode.Auto, RumiCarApi.Brake, 0, 0)));
        Assert.Contains("PC の Wi-Fi は車のネットワークに接続したままでした", atLoss.Details);
        Assert.Contains("切断前 3 秒間、駆動は止まっていました", atLoss.Details);

        var d = LinkDiagnosis.AfterReconnect(Loss(), Run, 0, T0.AddSeconds(7));
        Assert.Equal("車は動作を続け、PC の Wi-Fi も接続したままで、通信だけが途絶えました", d.Headline);
        Assert.DoesNotContain(d.Details, l => l.Contains("低電圧"));

        // Without the PC's Wi-Fi watched, nothing is said about it.
        var unwatched = LinkDiagnosis.AfterReconnect(Loss(watched: false), Run, 0, T0.AddSeconds(7));
        Assert.Equal("車は動作を続けていました（通信だけが途絶えました）", unwatched.Headline);
        Assert.DoesNotContain(LinkDiagnosis.AtLoss(Loss(watched: false)).Details, l => l.Contains("接続したまま"));
    }

    [Fact]
    public void OldCarCannotTellARestart()
    {
        var loss = new LinkLoss { LostAt = T0, End = LinkEnd.Silent, Message = "x", Before = null, PcWifiWatched = true };
        var d = LinkDiagnosis.AfterReconnect(loss, null, 0, T0.AddSeconds(9));
        Assert.Equal("車から受信が途絶えました", d.Headline);
        Assert.Contains("車の本体が 0.8.0 より前のため、車の再起動の有無は判定できません", d.Details);
    }

    [Fact]
    public void EventsLongBeforeTheLossDoNotCount()
    {
        var loss = Loss();
        loss.PcWifi.Add(new PcWifiEvent(T0 - LinkDiagnosis.WifiLookBack - TimeSpan.FromSeconds(1), PcWifiChange.MovedToOtherNetwork, "Old", ""));
        var d = LinkDiagnosis.AfterReconnect(loss, Run, 0, T0.AddSeconds(7));
        Assert.Equal("車は動作を続け、PC の Wi-Fi も接続したままで、通信だけが途絶えました", d.Headline);
    }

    [Fact]
    public void HistoryKeepsTheLastSeconds()
    {
        var h = new DriveHistory();
        for (var i = 0; i <= 100; i++) h.Add(new DriveSample(T0.AddMilliseconds(50 * i), DriveMode.Manual, RumiCarApi.Forward, i, 0));
        var s = h.Snapshot();
        Assert.Equal(100, s[^1].DrivePwm);
        Assert.True(s[^1].At - s[0].At <= DriveHistory.Window);
        Assert.Equal(61, s.Count);   // 3 s of frames at 20 per second, both ends included
        h.Clear();
        Assert.Empty(h.Snapshot());
    }

    [Fact]
    public void RunFromHelloNeedsBothIds()
    {
        static System.Text.Json.JsonElement J(string s) => System.Text.Json.JsonDocument.Parse(s).RootElement;
        Assert.Null(CarRun.FromHello(J("{\"boot_id\":\"b\"}")));
        Assert.Equal(new CarRun("b", "i", "unknown"), CarRun.FromHello(J("{\"boot_id\":\"b\",\"instance_id\":\"i\"}")));
        Assert.Null(CarRun.FromHello(null));
    }
}

public class LinkMonitorTests
{
    private static readonly DateTime T0 = new(2026, 11, 23, 14, 0, 0);
    private static System.Text.Json.JsonElement Hello(string boot, string instance, string previous) =>
        System.Text.Json.JsonDocument.Parse($"{{\"boot_id\":\"{boot}\",\"instance_id\":\"{instance}\",\"previous_exit\":\"{previous}\"}}").RootElement;

    [Fact]
    public void LossThenReconnectGivesTheFinalDiagnosisOnce()
    {
        var m = new LinkMonitor { PcWifiWatched = true };
        m.Connected(Hello("b1", "i1", "clean"));
        m.OnFrame(new TelemetryHeader { Mode = DriveMode.Manual, DriveDir = RumiCarApi.Forward, DrivePwm = 180 }, T0.AddSeconds(-1));
        m.OnPcWifi(new PcWifiEvent(T0.AddSeconds(-0.5), PcWifiChange.LeftCarNetwork, "RumiCar", ""));
        var first = m.Lost(T0, LinkEnd.Silent, "通信途絶");
        Assert.True(m.HasPendingLoss);
        Assert.Contains("切断前 3 秒間の駆動の最大: RC_drive(FORWARD, 180)（手動）", first.Details);
        Assert.Contains(first.Details, l => l.Contains("車のネットワークから切断されました"));
        m.OnPcWifi(new PcWifiEvent(T0.AddSeconds(4), PcWifiChange.BackOnCarNetwork, "RumiCar", ""));   // during the loss
        var final = m.Reconnected(Hello("b1", "i1", "clean"), PowerFlags.UnderVoltageSinceBoot, T0.AddSeconds(6));
        Assert.NotNull(final);
        Assert.Equal("PC の Wi-Fi が車のネットワークから切断されていました（車は動作を続けていました）", final!.Headline);
        Assert.Contains("14:00:04 PC の Wi-Fi が車のネットワークに戻りました", final.Details);
        Assert.Contains(final.Details, l => l.StartsWith("通信断の間に、車で低電圧が発生していました"));
        Assert.False(m.HasPendingLoss);
        Assert.Null(m.Reconnected(Hello("b1", "i1", "clean"), 0, T0.AddSeconds(7)));   // nothing pending any more
    }

    [Fact]
    public void ResetDropsThePendingLoss()
    {
        var m = new LinkMonitor();
        m.Connected(Hello("b1", "i1", "clean"));
        m.Lost(T0, LinkEnd.Silent, "x");
        m.Reset();
        Assert.False(m.HasPendingLoss);
        m.Connected(Hello("b2", "i2", "clean"));   // a new connection by the user: not compared with the old run
        Assert.Null(m.Reconnected(Hello("b2", "i2", "clean"), 0, T0.AddSeconds(9)));
    }
}
