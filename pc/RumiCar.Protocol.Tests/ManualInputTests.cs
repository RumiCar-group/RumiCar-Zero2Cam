using RumiCar.Protocol.Control;
using Xunit;

namespace RumiCar.Protocol.Tests;

public class ManualInputTests
{
    [Fact]
    public void NothingTouchedIsStop()
    {
        var m = new ManualInput();
        Assert.Equal((0, 0, ManualSource.None), m.Command(0));
    }

    [Theory]
    [InlineData(0.0, 0)]
    [InlineData(0.12, 0)]       // dead zone
    [InlineData(-0.1, 0)]
    [InlineData(1.0, 600)]
    [InlineData(-1.0, -600)]
    [InlineData(0.56, 300)]     // (0.56 - 0.12) / 0.88 = 0.5
    [InlineData(-0.56, -300)]
    [InlineData(2.0, 600)]      // clamped
    public void PadHeightGivesProportionalDrive(double y, int expected)
    {
        Assert.Equal(expected, ManualInput.PadDrive(y, 600));
    }

    [Fact]
    public void PadDriveMovesInWholePercent()
    {
        for (var y = -1.0; y <= 1.0; y += 0.013)
        {
            var d = ManualInput.PadDrive(y, 730);
            Assert.Equal(0, d % ManualInput.Quantum);
            Assert.InRange(d, -730, 730);
        }
    }

    [Fact]
    public void PadSteersWithHysteresisAndStopsOnRelease()
    {
        var m = new ManualInput { MaxDrive = 600 };
        m.PadDown(0.0, 1.0);
        Assert.Equal((0, 600, ManualSource.Pad), m.Command(0));
        m.PadMove(-0.30, 1.0);                       // not yet past the enter line
        Assert.Equal(0, m.Command(0).Steer);
        m.PadMove(-0.40, 1.0);
        Assert.Equal(-1, m.Command(0).Steer);
        m.PadMove(-0.25, 1.0);                       // between exit and enter: stays left
        Assert.Equal(-1, m.Command(0).Steer);
        m.PadMove(-0.20, 1.0);                       // inside the exit line: centre
        Assert.Equal(0, m.Command(0).Steer);
        m.PadMove(0.9, -1.0);
        Assert.Equal((1, -600, ManualSource.Pad), m.Command(0));
        m.PadMove(-0.9, -1.0);                       // straight across to the other side
        Assert.Equal(-1, m.Command(0).Steer);
        m.PadUp();
        Assert.Equal((0, 0, ManualSource.None), m.Command(0));
    }

    [Fact]
    public void PressingThePadClearsTheScrollSetPoint()
    {
        var m = new ManualInput();
        m.Wheel(3 * ManualInput.WheelNotch, 0);
        Assert.Equal(150, m.SetPoint);
        m.PadDown(0.0, 0.0);
        Assert.Equal(0, m.SetPoint);
        m.Wheel(ManualInput.WheelNotch, 10);         // ignored while the pad is held
        m.PadUp();
        Assert.Equal((0, 0, ManualSource.None), m.Command(20));
    }

    [Fact]
    public void WheelSetsASpeedThatStaysAndIsLimited()
    {
        var m = new ManualInput { MaxDrive = 300 };
        m.Wheel(ManualInput.WheelNotch, 0);
        Assert.Equal((0, 50, ManualSource.Scroll), m.Command(0));
        for (var i = 0; i < 20; i++) m.Wheel(ManualInput.WheelNotch, i);
        Assert.Equal(300, m.SetPoint);
        Assert.Equal(300, m.Command(10_000).Drive);  // stays without further input
        m.MaxDrive = 200;                            // lowering the limit lowers the set-point
        Assert.Equal(200, m.SetPoint);
    }

    [Fact]
    public void SmallTouchpadDeltasAccumulate()
    {
        var m = new ManualInput();
        for (var i = 0; i < 12; i++) m.Wheel(10, i);  // 120 in total = one notch
        Assert.Equal(50, m.SetPoint);
    }

    [Fact]
    public void ScrollingThroughZeroRestsAtZeroUntilAPause()
    {
        var m = new ManualInput();
        m.Wheel(2 * ManualInput.WheelNotch, 0);
        Assert.Equal(100, m.SetPoint);
        m.Wheel(-ManualInput.WheelNotch, 100);
        Assert.Equal(50, m.SetPoint);
        m.Wheel(-3 * ManualInput.WheelNotch, 200);   // would be −100: stops at 0
        Assert.Equal(0, m.SetPoint);
        m.Wheel(-ManualInput.WheelNotch, 300);       // the same swipe goes on: still 0
        m.Wheel(-ManualInput.WheelNotch, 600);       // still scrolling (each event extends the rest)
        Assert.Equal(0, m.SetPoint);
        m.Wheel(-ManualInput.WheelNotch, 600 + ManualInput.ZeroDetentMs + 1);   // after a pause: reverse
        Assert.Equal(-50, m.SetPoint);
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
        Assert.Equal(0, m.Command(0).Steer);          // below the threshold
        m.HorizontalWheel(20, 10);
        Assert.Equal(1, m.Command(10).Steer);
        m.HorizontalWheel(30, 200);
        Assert.Equal(1, m.Command(200 + ManualInput.SwipeReleaseMs - 1).Steer);
        Assert.Equal(0, m.Command(200 + ManualInput.SwipeReleaseMs).Steer);
        m.HorizontalWheel(-60, 1000);
        Assert.Equal(-1, m.Command(1000).Steer);
        m.HorizontalWheel(30, 1050);                  // a short reversal is not enough
        Assert.Equal(-1, m.Command(1050).Steer);
        m.HorizontalWheel(20, 1060);
        Assert.Equal(1, m.Command(1060).Steer);
        m.InvertHorizontal = true;
        m.HorizontalWheel(60, 2000);
        Assert.Equal(-1, m.Command(2000).Steer);
    }

    [Fact]
    public void KeysOverrideTheSetPointAndSteerWithIt()
    {
        var m = new ManualInput { MaxDrive = 500 };
        m.Wheel(4 * ManualInput.WheelNotch, 0);
        Assert.Equal(200, m.SetPoint);
        m.Key(ManualKey.Left, true);                 // steering keys keep the cruise speed
        Assert.Equal((-1, 200, ManualSource.Keys), m.Command(0));
        m.Key(ManualKey.Right, true);                // the last pressed wins
        Assert.Equal(1, m.Command(0).Steer);
        m.Key(ManualKey.Right, false);
        Assert.Equal(-1, m.Command(0).Steer);
        m.Key(ManualKey.Left, false);
        Assert.Equal((0, 200, ManualSource.Scroll), m.Command(0));
        m.Key(ManualKey.Forward, true);              // drive keys clear the set-point
        Assert.Equal((0, 500, ManualSource.Keys), m.Command(0));
        m.Key(ManualKey.Forward, false);
        Assert.Equal((0, 0, ManualSource.None), m.Command(0));
        m.Key(ManualKey.Reverse, true);
        Assert.Equal(-500, m.Command(0).Drive);
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
        Assert.Equal((0, 0, ManualSource.None), m.Command(1));
        Assert.Equal(0, m.SetPoint);
    }

    [Fact]
    public void MaxDriveIsKeptInRange()
    {
        var m = new ManualInput { MaxDrive = 5 };
        Assert.Equal(ManualInput.MinMaxDrive, m.MaxDrive);
        m.MaxDrive = 5000;
        Assert.Equal(1000, m.MaxDrive);
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
