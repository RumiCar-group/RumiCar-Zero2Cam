using System.Text.Json;
using RumiCar.Protocol.Control;
using Xunit;

namespace RumiCar.Protocol.Tests;

public class ControlCodecTests
{
    [Fact]
    public void RequestRoundTripsThroughOneLine()
    {
        var req = ControlCodec.ManualDrive(42, RumiCarApi.Left, 128, RumiCarApi.Forward, 89);
        var line = ControlCodec.Encode(req);
        Assert.DoesNotContain("\n", line);
        Assert.True(ControlCodec.TryDecode(line, out var decoded));
        var back = Assert.IsType<ControlRequest>(decoded);
        Assert.Equal(42, back.Id);
        Assert.Equal(RequestType.ManualDrive, back.Type);
        var p = back.Params;
        Assert.NotNull(p);
        Assert.Equal(0, p!["steer"].GetInt32());          // RC_steer LEFT ...
        Assert.Equal(128, p["steer_pwm"].GetInt32());     // ... with its strength (protocol 3)
        Assert.Equal(2, p["drive_dir"].GetInt32());       // RC_drive FORWARD
        Assert.Equal(89, p["drive_pwm"].GetInt32());
        Assert.False(p.ContainsKey("drive_permille"));
    }

    [Fact]
    public void ResponseAndEventAreDistinguished()
    {
        Assert.True(ControlCodec.TryDecode("{\"id\":7,\"ok\":false,\"error\":\"no such pack\"}", out var r));
        var resp = Assert.IsType<ControlResponse>(r);
        Assert.Equal(7, resp.Id);
        Assert.False(resp.Ok);
        Assert.Equal("no such pack", resp.Error);

        Assert.True(ControlCodec.TryDecode("{\"event\":\"status\",\"data\":{\"mode\":\"auto\"}}", out var e));
        var evt = Assert.IsType<ControlEvent>(e);
        Assert.Equal("status", evt.Event);
        Assert.Equal("auto", evt.Data!.Value.GetProperty("mode").GetString());
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("not json")]
    [InlineData("[1,2,3]")]
    [InlineData("{\"foo\":1}")]
    public void GarbageIsRejected(string line)
    {
        Assert.False(ControlCodec.TryDecode(line, out var m));
        Assert.Null(m);
    }

    [Fact]
    public void BuildersValidateArguments()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, 3, 255, RumiCarApi.Free, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, -1, 255, RumiCarApi.Free, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, RumiCarApi.Left, 256, RumiCarApi.Free, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, RumiCarApi.Left, -1, RumiCarApi.Free, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, RumiCarApi.Center, 0, 4, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, RumiCarApi.Center, 0, RumiCarApi.Forward, 256));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, RumiCarApi.Center, 0, RumiCarApi.Forward, -1));
        Assert.Equal(3, ControlProtocol.Version);
        Assert.Throws<ArgumentException>(() => ControlCodec.Activate(1, "firmware", "x"));
        Assert.Throws<ArgumentException>(() => ControlCodec.Activate(1, "pack", "../etc"));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.SetWifi(1, "RumiCar", 14));
        var ok = ControlCodec.Activate(3, "pack", "base-tof-0.1.0");
        Assert.Equal("base-tof-0.1.0", ok.Params!["name"].GetString());
    }

    [Fact]
    public void DriveSettingsAreReadFromStatus()
    {
        using var doc = JsonDocument.Parse(
            "{\"mode\":\"auto\",\"max_drive_pwm\":204,\"auto_limit\":115,\"manual_max_pwm\":153,\"manual_accel_pwm_per_s\":287,\"pad_steer_strength\":true}");
        Assert.True(DriveSettings.TryRead(doc.RootElement, out var s));
        Assert.Equal(new DriveSettings(153, 115, 287, true), s);
        // A car without them (or a partial object): false.
        using var old = JsonDocument.Parse("{\"mode\":\"auto\",\"auto_limit\":115,\"max_drive_pwm\":255}");
        Assert.False(DriveSettings.TryRead(old.RootElement, out _));
        using var bad = JsonDocument.Parse("{\"auto_limit\":115,\"manual_max_pwm\":153,\"manual_accel_pwm_per_s\":287,\"pad_steer_strength\":1}");
        Assert.False(DriveSettings.TryRead(bad.RootElement, out _));
        Assert.Equal("287（0→255 約 0.89 秒）", DriveSettings.AccelText(287));
        Assert.Equal("255（0→255 約 1.0 秒）", DriveSettings.AccelText(255));
        Assert.Equal("0（加速なし）", DriveSettings.AccelText(0));
        Assert.Equal(ManualInput.MinMaxDrive, DriveSettings.ManualMaxMin);
    }

    [Fact]
    public void HelloCarriesProtocolVersion()
    {
        var hello = ControlCodec.Hello(1, "DriveManager");
        Assert.Equal(ControlProtocol.Version, hello.Params!["protocol"].GetInt32());
        Assert.Equal(JsonValueKind.String, hello.Params["client"].ValueKind);
    }
}
