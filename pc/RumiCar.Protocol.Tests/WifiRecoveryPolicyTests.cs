using RumiCar.Protocol.Link;
using Xunit;

namespace RumiCar.Protocol.Tests;

public class WifiRecoveryPolicyTests
{
    private static TimeSpan S(double seconds) => TimeSpan.FromSeconds(seconds);

    private static WifiRecoveryPolicy Connected()
    {
        var p = new WifiRecoveryPolicy();
        p.ObserveNetwork(S(0), PcNetwork.CarNetwork);
        p.ObserveLink(S(0), up: true);
        return p;
    }

    [Fact]
    public void NothingBeforeTheCarWasReached()
    {
        // Without a connection the car's network is not known: the PC's Wi-Fi is left alone.
        var p = new WifiRecoveryPolicy();
        p.ObserveNetwork(S(0), PcNetwork.OtherNetwork);
        Assert.Equal(WifiAction.None, p.Decide(S(60), enabled: true, wanted: true));
    }

    [Fact]
    public void BackToTheCarNetworkAfterWindowsMovedThePc()
    {
        var p = Connected();
        p.ObserveNetwork(S(10), PcNetwork.OtherNetwork);
        p.ObserveLink(S(10), up: false);
        Assert.Equal(WifiAction.None, p.Decide(S(10.5), true, true));      // Windows first
        Assert.Equal(WifiAction.Connect, p.Decide(S(11), true, true));
        Assert.Equal(WifiAction.None, p.Decide(S(13), true, true));        // not more often than every 5 s
        Assert.Equal(WifiAction.Connect, p.Decide(S(16), true, true));
        p.ObserveNetwork(S(17), PcNetwork.CarNetwork);
        Assert.Equal(WifiAction.None, p.Decide(S(18), true, true));
        p.ObserveLink(S(19), up: true);
        Assert.Equal(WifiAction.None, p.Decide(S(60), true, true));
    }

    [Fact]
    public void ANewDepartureIsAnsweredAfterTheShortDelay()
    {
        var p = Connected();
        p.ObserveNetwork(S(10), PcNetwork.NotConnected);
        Assert.Equal(WifiAction.Connect, p.Decide(S(11), true, true));
        p.ObserveNetwork(S(12), PcNetwork.CarNetwork);
        p.ObserveNetwork(S(13), PcNetwork.NotConnected);
        Assert.Equal(WifiAction.Connect, p.Decide(S(14), true, true));     // not held back by the attempt at 11 s
    }

    [Fact]
    public void RejoinWhenTheCarStaysUnreachableOnItsNetwork()
    {
        var p = Connected();
        p.ObserveLink(S(100), up: false);
        Assert.Equal(WifiAction.None, p.Decide(S(111), true, true));
        Assert.Equal(WifiAction.Rejoin, p.Decide(S(112), true, true));
        Assert.Equal(WifiAction.None, p.Decide(S(120), true, true));
        // The rejoin takes the PC off the network and back: it waits for the network to stay as long again.
        p.ObserveNetwork(S(113), PcNetwork.NotConnected);
        p.ObserveNetwork(S(116), PcNetwork.CarNetwork);
        Assert.Equal(WifiAction.None, p.Decide(S(127), true, true));      // on the network for 11 s only
        Assert.Equal(WifiAction.None, p.Decide(S(140), true, true));      // and not within 30 s of the last rejoin
        Assert.Equal(WifiAction.Rejoin, p.Decide(S(142), true, true));
        p.ObserveLink(S(145), up: true);
        p.ObserveLink(S(150), up: false);
        Assert.Equal(WifiAction.Rejoin, p.Decide(S(162), true, true));    // a reconnect starts the count again
    }

    [Fact]
    public void UnknownNetworkIsTreatedAsTheCarNetwork()
    {
        var p = new WifiRecoveryPolicy();
        p.ObserveLink(S(0), up: true);   // Windows hides the network name
        p.ObserveLink(S(20), up: false);
        Assert.Equal(PcNetwork.Unknown, p.Network);
        Assert.Equal(WifiAction.Rejoin, p.Decide(S(32), true, true));
    }

    [Fact]
    public void OffOrNotInUseDoesNothing()
    {
        var p = Connected();
        p.ObserveNetwork(S(10), PcNetwork.OtherNetwork);
        p.ObserveLink(S(10), up: false);
        Assert.Equal(WifiAction.None, p.Decide(S(30), enabled: false, wanted: true));
        Assert.Equal(WifiAction.None, p.Decide(S(30), enabled: true, wanted: false));   // after 「切断」 the PC is free to go elsewhere
        Assert.Equal(WifiAction.Connect, p.Decide(S(30), enabled: true, wanted: true));
    }
}
