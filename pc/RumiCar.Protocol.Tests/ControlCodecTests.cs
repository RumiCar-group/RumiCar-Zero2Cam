using System.Text.Json;
using RumiCar.Protocol.Control;
using Xunit;

namespace RumiCar.Protocol.Tests;

public class ControlCodecTests
{
    [Fact]
    public void RequestRoundTripsThroughOneLine()
    {
        var req = ControlCodec.ManualDrive(42, RumiCarApi.Left, RumiCarApi.Forward, 89);
        var line = ControlCodec.Encode(req);
        Assert.DoesNotContain("\n", line);
        Assert.True(ControlCodec.TryDecode(line, out var decoded));
        var back = Assert.IsType<ControlRequest>(decoded);
        Assert.Equal(42, back.Id);
        Assert.Equal(RequestType.ManualDrive, back.Type);
        var p = back.Params;
        Assert.NotNull(p);
        Assert.Equal(0, p!["steer"].GetInt32());          // RC_steer LEFT
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
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, 3, RumiCarApi.Free, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, -1, RumiCarApi.Free, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, RumiCarApi.Center, 4, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, RumiCarApi.Center, RumiCarApi.Forward, 256));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.ManualDrive(1, RumiCarApi.Center, RumiCarApi.Forward, -1));
        Assert.Equal(2, ControlProtocol.Version);
        Assert.Throws<ArgumentException>(() => ControlCodec.Activate(1, "firmware", "x"));
        Assert.Throws<ArgumentException>(() => ControlCodec.Activate(1, "pack", "../etc"));
        Assert.Throws<ArgumentOutOfRangeException>(() => ControlCodec.SetWifi(1, "RumiCar", 14));
        var ok = ControlCodec.Activate(3, "pack", "base-tof-0.1.0");
        Assert.Equal("base-tof-0.1.0", ok.Params!["name"].GetString());
    }

    [Fact]
    public void HelloCarriesProtocolVersion()
    {
        var hello = ControlCodec.Hello(1, "DriveManager");
        Assert.Equal(ControlProtocol.Version, hello.Params!["protocol"].GetInt32());
        Assert.Equal(JsonValueKind.String, hello.Params["client"].ValueKind);
    }
}
