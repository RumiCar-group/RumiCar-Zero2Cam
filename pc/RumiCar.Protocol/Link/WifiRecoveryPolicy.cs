namespace RumiCar.Protocol.Link;

/// <summary>Where the PC's Wi-Fi is, as far as the PC can tell.</summary>
public enum PcNetwork
{
    /// <summary>Not known (no Wi-Fi state yet, or Windows hides the network name).</summary>
    Unknown,
    CarNetwork,
    OtherNetwork,
    NotConnected,
}

public enum WifiAction
{
    None,
    /// <summary>Join the car's network (the PC is off it).</summary>
    Connect,
    /// <summary>Leave and join the car's network again (the PC is on it, but the car cannot be reached).</summary>
    Rejoin,
}

/// <summary>
/// When the drive manager puts the PC's Wi-Fi back on the car's network
/// (docs 6.2).  Only while the PC is using the car (connected, or connecting
/// again after a loss) and the user has it on:
/// <list type="bullet">
/// <item>off the car's network (disconnected, or moved by Windows to another
/// network) for <see cref="ConnectDelay"/>: join it, again every
/// <see cref="ConnectInterval"/> until the PC is back;</item>
/// <item>on the car's network (or not known) but the car unreachable for
/// <see cref="RejoinAfter"/>, with the PC on that network as long: leave and
/// join again, as a user does by hand (at most every <see cref="RejoinInterval"/>).</item>
/// </list>
/// Times are passed in by the caller (a monotonic clock), so the policy has
/// no clock or thread of its own.
/// </summary>
public sealed class WifiRecoveryPolicy
{
    /// <summary>Windows reconnects by itself first when it can.</summary>
    public static readonly TimeSpan ConnectDelay = TimeSpan.FromSeconds(1);
    /// <summary>Each attempt makes the adapter scan: not more often.</summary>
    public static readonly TimeSpan ConnectInterval = TimeSpan.FromSeconds(5);
    /// <summary>The link timeout (5 s) plus a few reconnect attempts of the connection (every 2 s).</summary>
    public static readonly TimeSpan RejoinAfter = TimeSpan.FromSeconds(12);
    public static readonly TimeSpan RejoinInterval = TimeSpan.FromSeconds(30);

    private PcNetwork _network = PcNetwork.Unknown;
    private TimeSpan _networkSince;
    private bool _linkUp;
    private bool _linkSeen;          // connected to the car at least once: the car's network is known
    private TimeSpan _linkDownSince;
    private TimeSpan? _lastConnect;
    private TimeSpan? _lastRejoin;

    public PcNetwork Network => _network;

    public void ObserveNetwork(TimeSpan now, PcNetwork network)
    {
        if (network == _network) return;
        _network = network;
        _networkSince = now;
        if (network is PcNetwork.OtherNetwork or PcNetwork.NotConnected) _lastConnect = null;   // a new departure: first attempt after ConnectDelay
    }

    public void ObserveLink(TimeSpan now, bool up)
    {
        if (up)
        {
            _linkUp = true;
            _linkSeen = true;
            _lastRejoin = null;
        }
        else if (_linkUp)
        {
            _linkUp = false;
            _linkDownSince = now;
        }
    }

    /// <param name="enabled">The user's setting 「PC の Wi-Fi を自動でつなぎ直す」.</param>
    /// <param name="wanted">The PC is using the car: connected, or connecting again after a loss (not after 「切断」).</param>
    public WifiAction Decide(TimeSpan now, bool enabled, bool wanted)
    {
        if (!enabled || !wanted || !_linkSeen) return WifiAction.None;
        switch (_network)
        {
            case PcNetwork.OtherNetwork:
            case PcNetwork.NotConnected:
                if (now - _networkSince < ConnectDelay) return WifiAction.None;
                if (_lastConnect is TimeSpan c && now - c < ConnectInterval) return WifiAction.None;
                _lastConnect = now;
                return WifiAction.Connect;
            default:
                if (_linkUp || now - _linkDownSince < RejoinAfter || now - _networkSince < RejoinAfter) return WifiAction.None;
                if (_lastRejoin is TimeSpan r && now - r < RejoinInterval) return WifiAction.None;
                _lastRejoin = now;
                return WifiAction.Rejoin;
        }
    }
}
