namespace RumiCar.Protocol;

/// <summary>TCP ports served by the car (see docs: 通信仕様).</summary>
public static class Ports
{
    /// <summary>Control channel: one JSON object per line, request/response plus events.</summary>
    public const int Control = 5600;

    /// <summary>Telemetry channel: binary frames (header JSON + JPEG), car -> PC only.</summary>
    public const int Telemetry = 5601;

    /// <summary>Maintenance channel (OpenSSH): file transfer and rumicar-activate.</summary>
    public const int Ssh = 22;
}

/// <summary>Default address of the car when it runs its own access point.</summary>
public static class CarDefaults
{
    public const string Host = "10.42.0.1";
    public const string Ssid = "RumiCar";
}
