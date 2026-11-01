using RumiCar.Protocol.Control;
using Xunit;

namespace RumiCar.Protocol.Tests;

/// <summary>
/// The manual inputs on the RC_drive scale (duty 0..255); the command is in
/// RC_steer / RC_drive values.  <c>Cmd(steer, drive)</c> builds the expected
/// command from the signed form (−1/0/+1, ±duty) the pad geometry uses.
/// </summary>
public class ManualInputTests
{
    private static ManualCommand Cmd(int steer, int drive, ManualSource source) => ManualCommand.FromSigned(steer, drive, source);

    [Fact]
    public void NothingTouchedIsStop()
    {
        var m = new ManualInput();
        Assert.Equal(Cmd(0, 0, ManualSource.None), m.Command(0));
        Assert.Equal(new ManualCommand(RumiCarApi.Center, RumiCarApi.Free, 0, ManualSource.None), m.Command(0));
    }

    [Fact]
    public void CommandsAreRumiCarValues()
    {
        Assert.Equal(new ManualCommand(RumiCarApi.Left, RumiCarApi.Forward, 150, ManualSource.Pad), Cmd(-1, 150, ManualSource.Pad));
        Assert.Equal(new ManualCommand(RumiCarApi.Right, RumiCarApi.Reverse, 77, ManualSource.Keys), Cmd(1, -77, ManualSource.Keys));
        Assert.Equal(new ManualCommand(RumiCarApi.Center, RumiCarApi.Free, 0, ManualSource.Scroll), Cmd(0, 0, ManualSource.Scroll));
        var c = Cmd(1, -77, ManualSource.Keys);
        Assert.Equal(1, c.SignedSteer);
        Assert.Equal(-77, c.SignedDrive);
    }

    [Theory]
    [InlineData(0.0, 0)]
    [InlineData(0.12, 0)]       // dead zone
    [InlineData(-0.1, 0)]
    [InlineData(1.0, 153)]
    [InlineData(-1.0, -153)]
    [InlineData(0.56, 77)]      // (0.56 - 0.12) / 0.88 = 0.5 -> 76.5, rounded away from zero
    [InlineData(-0.56, -77)]
    [InlineData(2.0, 153)]      // clamped
    public void PadHeightGivesProportionalDrive(double y, int expected)
    {
        Assert.Equal(expected, ManualInput.PadDrive(y, 153));
    }

    [Fact]
    public void PadDriveStaysWithinTheMaximum()
    {
        for (var y = -1.0; y <= 1.0; y += 0.013)
        {
            var d = ManualInput.PadDrive(y, 200);
            Assert.Equal(0, d % ManualInput.Quantum);
            Assert.InRange(d, -200, 200);
        }
    }

    [Fact]
    public void PadSteersWithHysteresisAndStopsOnRelease()
    {
        var m = new ManualInput { MaxDrive = 153 };
        m.PadDown(0.0, 1.0);
        Assert.Equal(Cmd(0, 153, ManualSource.Pad), m.Command(0));
        m.PadMove(-0.30, 1.0);                       // not yet past the enter line
        Assert.Equal(RumiCarApi.Center, m.Command(0).Steer);
        m.PadMove(-0.40, 1.0);
        Assert.Equal(RumiCarApi.Left, m.Command(0).Steer);
        m.PadMove(-0.25, 1.0);                       // between exit and enter: stays left
        Assert.Equal(RumiCarApi.Left, m.Command(0).Steer);
        m.PadMove(-0.20, 1.0);                       // inside the exit line: centre
        Assert.Equal(RumiCarApi.Center, m.Command(0).Steer);
        m.PadMove(0.9, -1.0);
        Assert.Equal(Cmd(1, -153, ManualSource.Pad), m.Command(0));
        m.PadMove(-0.9, -1.0);                       // straight across to the other side
        Assert.Equal(RumiCarApi.Left, m.Command(0).Steer);
        m.PadUp();
        Assert.Equal(Cmd(0, 0, ManualSource.None), m.Command(0));
    }

    [Fact]
    public void PressingThePadClearsTheScrollSetPoint()
    {
        var m = new ManualInput();
        m.Wheel(3 * ManualInput.WheelNotch, 0);
        Assert.Equal(39, m.SetPoint);
        m.PadDown(0.0, 0.0);
        Assert.Equal(0, m.SetPoint);
        m.Wheel(ManualInput.WheelNotch, 10);         // ignored while the pad is held
        m.PadUp();
        Assert.Equal(Cmd(0, 0, ManualSource.None), m.Command(20));
    }

    [Fact]
    public void WheelSetsASpeedThatStaysAndIsLimited()
    {
        var m = new ManualInput { MaxDrive = 77 };
        m.Wheel(ManualInput.WheelNotch, 0);
        Assert.Equal(Cmd(0, 13, ManualSource.Scroll), m.Command(0));
        for (var i = 0; i < 20; i++) m.Wheel(ManualInput.WheelNotch, i);
        Assert.Equal(77, m.SetPoint);
        Assert.Equal(77, m.Command(10_000).DrivePwm);  // stays without further input
        Assert.Equal(RumiCarApi.Forward, m.Command(10_000).DriveDir);
        m.MaxDrive = 51;                             // lowering the limit lowers the set-point
        Assert.Equal(51, m.SetPoint);
    }

    [Fact]
    public void SmallTouchpadDeltasAccumulate()
    {
        var m = new ManualInput();
        for (var i = 0; i < 12; i++) m.Wheel(10, i);  // 120 in total = one notch
        Assert.Equal(13, m.SetPoint);
    }

    [Fact]
    public void ScrollingThroughZeroRestsAtZeroUntilAPause()
    {
        var m = new ManualInput();
        m.Wheel(2 * ManualInput.WheelNotch, 0);
        Assert.Equal(26, m.SetPoint);
        m.Wheel(-ManualInput.WheelNotch, 100);
        Assert.Equal(13, m.SetPoint);
        m.Wheel(-3 * ManualInput.WheelNotch, 200);   // would be −26: stops at 0
        Assert.Equal(0, m.SetPoint);
        m.Wheel(-ManualInput.WheelNotch, 300);       // the same swipe goes on: still 0
        m.Wheel(-ManualInput.WheelNotch, 600);       // still scrolling (each event extends the rest)
        Assert.Equal(0, m.SetPoint);
        m.Wheel(-ManualInput.WheelNotch, 600 + ManualInput.ZeroDetentMs + 1);   // after a pause: reverse
        Assert.Equal(-13, m.SetPoint);
        Assert.Equal(Cmd(0, -13, ManualSource.Scroll), m.Command(700 + ManualInput.ZeroDetentMs));
        m.Wheel(ManualInput.WheelNotch, 1100);       // back up to zero, then forward right away is the other direction
        Assert.Equal(0, m.SetPoint);
        m.Wheel(ManualInput.WheelNotch, 1150);
        Assert.Equal(0, m.SetPoint);
    }

    [Fact]
    public void HorizontalScrollSteersWhileMovingAndCentresAfterwards()
    {
        var m = new ManualInput();
        m.HorizontalWheel(20, 0);
        Assert.Equal(RumiCarApi.Center, m.Command(0).Steer);          // below the threshold
        m.HorizontalWheel(20, 10);
        Assert.Equal(RumiCarApi.Right, m.Command(10).Steer);
        m.HorizontalWheel(30, 200);
        Assert.Equal(RumiCarApi.Right, m.Command(200 + ManualInput.SwipeReleaseMs - 1).Steer);
        Assert.Equal(RumiCarApi.Center, m.Command(200 + ManualInput.SwipeReleaseMs).Steer);
        m.HorizontalWheel(-60, 1000);
        Assert.Equal(RumiCarApi.Left, m.Command(1000).Steer);
        m.HorizontalWheel(30, 1050);                  // a short reversal is not enough
        Assert.Equal(RumiCarApi.Left, m.Command(1050).Steer);
        m.HorizontalWheel(20, 1060);
        Assert.Equal(RumiCarApi.Right, m.Command(1060).Steer);
        m.InvertHorizontal = true;
        m.HorizontalWheel(60, 2000);
        Assert.Equal(RumiCarApi.Left, m.Command(2000).Steer);
    }

    [Fact]
    public void KeysOverrideTheSetPointAndSteerWithIt()
    {
        var m = new ManualInput { MaxDrive = 128 };
        m.Wheel(4 * ManualInput.WheelNotch, 0);
        Assert.Equal(52, m.SetPoint);
        m.Key(ManualKey.Left, true);                 // steering keys keep the cruise speed
        Assert.Equal(Cmd(-1, 52, ManualSource.Keys), m.Command(0));
        m.Key(ManualKey.Right, true);                // the last pressed wins
        Assert.Equal(RumiCarApi.Right, m.Command(0).Steer);
        m.Key(ManualKey.Right, false);
        Assert.Equal(RumiCarApi.Left, m.Command(0).Steer);
        m.Key(ManualKey.Left, false);
        Assert.Equal(Cmd(0, 52, ManualSource.Scroll), m.Command(0));
        m.Key(ManualKey.Forward, true);              // drive keys clear the set-point
        Assert.Equal(Cmd(0, 128, ManualSource.Keys), m.Command(0));
        m.Key(ManualKey.Forward, false);
        Assert.Equal(Cmd(0, 0, ManualSource.None), m.Command(0));
        m.Key(ManualKey.Reverse, true);
        Assert.Equal(new ManualCommand(RumiCarApi.Center, RumiCarApi.Reverse, 128, ManualSource.Keys), m.Command(0));
    }

    [Fact]
    public void StopClearsEverything()
    {
        var m = new ManualInput();
        m.Wheel(5 * ManualInput.WheelNotch, 0);
        m.HorizontalWheel(100, 0);
        m.Key(ManualKey.Forward, true);
        m.PadDown(0.9, 0.9);
        m.Stop();
        Assert.False(m.Dragging);
        Assert.Equal(Cmd(0, 0, ManualSource.None), m.Command(1));
        Assert.Equal(0, m.SetPoint);
    }

    [Fact]
    public void MaxDriveIsKeptInRange()
    {
        var m = new ManualInput { MaxDrive = 5 };
        Assert.Equal(ManualInput.MinMaxDrive, m.MaxDrive);
        m.MaxDrive = 5000;
        Assert.Equal(RumiCarApi.PwmMax, m.MaxDrive);
    }
}

public class ProductVersionTests
{
    [Theory]
    [InlineData("0.2.1", "0.2.1")]
    [InlineData("0.2.1+g1a2b3c4", "0.2.1")]
    [InlineData(" 0.2.1 ", "0.2.1")]
    [InlineData("", "")]
    public void ReleasePartDropsBuildMetadata(string version, string expected)
    {
        Assert.Equal(expected, ProductVersion.Release(version));
    }

    [Theory]
    [InlineData("0.2.1", "0.2.1", true)]
    [InlineData("0.2.1+g1a2b3c4", "0.2.1", true)]
    [InlineData("0.2.0", "0.2.1", false)]
    [InlineData("0.2.1", "0.2.10", false)]
    [InlineData("", "", false)]
    public void SameReleaseComparesTheNumbers(string a, string b, bool expected)
    {
        Assert.Equal(expected, ProductVersion.SameRelease(a, b));
    }

    [Fact]
    public void DevelopmentBuildsAreMarked()
    {
        Assert.True(ProductVersion.IsDevelopmentBuild("0.2.1+g1a2b3c4"));
        Assert.False(ProductVersion.IsDevelopmentBuild("0.2.1"));
    }
}
