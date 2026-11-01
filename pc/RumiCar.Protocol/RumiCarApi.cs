namespace RumiCar.Protocol;

/// <summary>
/// The values of the RumiCar library API (RC_read / RC_steer / RC_drive of
/// RumiCar-lib and RasPi/RumiCar.py), which the car uses on the wire and in
/// recordings from ABI 3 (car/include/rumicar/logic_api.h).
/// </summary>
public static class RumiCarApi
{
    /// <summary>ABI of the car's logic interface whose values this PC speaks ("abi" in every telemetry header).</summary>
    public const int Abi = 3;

    // RC_steer
    public const int Left = 0;
    public const int Center = 1;
    public const int Right = 2;

    // RC_drive: direction ...
    public const int Free = 0;
    public const int Reverse = 1;
    public const int Forward = 2;
    public const int Brake = 3;
    // ... and the PWM duty 0..255 (the share of full duty: 150 = 150/255).
    public const int PwmMax = 255;

    // RC_read: 0..TofMaxMm is a distance in mm.
    public const int TofMaxMm = 200;
    public const int TofTimeout = -1;        // no reading
    public const int TofOutOfRange = -3;     // nothing within 20 cm (open road)

    public static string SteerName(int steer) => steer switch
    {
        Left => "LEFT",
        Center => "CENTER",
        Right => "RIGHT",
        _ => steer.ToString(),
    };

    public static string DriveName(int dir) => dir switch
    {
        Free => "FREE",
        Reverse => "REVERSE",
        Forward => "FORWARD",
        Brake => "BRAKE",
        _ => dir.ToString(),
    };

    /// <summary>A ToF value as the user reads it: the distance, or the RC_read code with its meaning.</summary>
    public static string TofText(int mm) => mm switch
    {
        >= 0 => mm.ToString(),
        TofTimeout => "-1 タイムアウト",
        TofOutOfRange => "-3 範囲外",
        _ => mm.ToString(),
    };

    /// <summary>A command as the user would write it: RC_steer(LEFT) / RC_drive(FORWARD, 128).</summary>
    public static string CommandText(int steer, int driveDir, int drivePwm) =>
        driveDir is Free or Brake
            ? $"RC_steer({SteerName(steer)}) / RC_drive({DriveName(driveDir)}, 0)"
            : $"RC_steer({SteerName(steer)}) / RC_drive({DriveName(driveDir)}, {drivePwm})";
}
