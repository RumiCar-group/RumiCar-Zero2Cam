namespace RumiCar.Protocol;

/// <summary>
/// The values of the RumiCar library API (RC_read / RC_steer / RC_drive of
/// RumiCar-lib v0.7.0 and RasPi/RumiCar.py 0.7.0), which the car uses on the
/// wire and in recordings from ABI 3; ABI 4 adds RC_steer's strength; from
/// ABI 5 the car no longer cuts the ToF readings off at 200 mm
/// (car/include/rumicar/logic_api.h).
/// </summary>
public static class RumiCarApi
{
    /// <summary>ABI of the car's logic interface whose values this PC speaks ("abi" in every telemetry header).</summary>
    public const int Abi = 5;

    /// <summary>ABI 4 (core 0.5.x / 0.6.x): the same values, but the car cut the ToF readings off at <see cref="TofMaxMmAbi4"/> (a longer distance arrived as <see cref="TofOutOfRange"/>).</summary>
    public const int AbiWithTofCutOff = 4;

    /// <summary>ABI 3 (core 0.4.x): as ABI 4 without RC_steer's strength. Its steering was always full (<see cref="PwmMax"/>).</summary>
    public const int AbiWithoutSteerStrength = 3;

    // RC_steer: direction ...
    public const int Left = 0;
    public const int Center = 1;
    public const int Right = 2;
    // ... and strength 0..PwmMax (the share of the signal, not compensated):
    // 0 is neutral (the same output as CENTER), PwmMax is full steering and what
    // RC_steer(direc) with one argument gives; CENTER ignores the strength.

    // RC_drive: direction ...
    public const int Free = 0;
    public const int Reverse = 1;
    public const int Forward = 2;
    public const int Brake = 3;
    // ... and the PWM duty 0..255 (the share of full duty: 150 = 150/255).
    public const int PwmMax = 255;

    // RC_read: 0..TofMaxMm is a distance in mm (the range of the VL53L0X).  These
    // are the values of the car itself, with no limit: a limit that a program on
    // the car sets for its own decisions (RC_setReadLimit in drive.py, conf_max_mm
    // in params.json) does not change what the car sends and what is shown here.
    public const int TofMaxMm = 2000;
    /// <summary>Where a car of ABI 3 / 4 cut its ToF readings off.</summary>
    public const int TofMaxMmAbi4 = 200;
    public const int TofTimeout = -1;        // no reading
    public const int TofOutOfRange = -3;     // out of range: beyond TofMaxMm or no target (far, open road)

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

    /// <summary>
    /// RC_steer as the user would write it: RC_steer(CENTER), RC_steer(LEFT) for
    /// full steering (255, the one-argument form gives the same output),
    /// otherwise RC_steer(LEFT, 128).  RC_steer(LEFT, 0) is neutral.
    /// </summary>
    public static string SteerText(int steer, int steerPwm) =>
        steer is Left or Right && steerPwm != PwmMax
            ? $"RC_steer({SteerName(steer)}, {steerPwm})"
            : $"RC_steer({SteerName(steer)})";

    /// <summary>A command as the user would write it: RC_steer(LEFT, 128) / RC_drive(FORWARD, 150).</summary>
    public static string CommandText(int steer, int steerPwm, int driveDir, int drivePwm) =>
        driveDir is Free or Brake
            ? $"{SteerText(steer, steerPwm)} / RC_drive({DriveName(driveDir)}, 0)"
            : $"{SteerText(steer, steerPwm)} / RC_drive({DriveName(driveDir)}, {drivePwm})";
}
