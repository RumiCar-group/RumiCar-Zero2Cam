using System.Globalization;
using System.Text.Json;

namespace RumiCar.Protocol.Control;

/// <summary>
/// The drive settings the car keeps across a restart (core 0.6.0, docs 3.4 /
/// 5.2): reported in get_status and status events, changed with
/// <see cref="ControlCodec.SetDriveSettings"/>.  They are per car, because cars
/// differ in their motor and gears (the current a start draws) and in their steering.
/// </summary>
/// <param name="ManualMaxPwm">Cap of the drive duty in manual mode (RC_drive's value, 26..255): the pad's edge, the scroll and ↑ ↓ reach it.</param>
/// <param name="AutoLimit">Cap of the drive duty in auto mode (0..255).</param>
/// <param name="ManualAccelPwmPerS">Growth of the manual drive duty per second (0..25500; 287 = 0 to 255 in about 0.9 s; 0 = none).</param>
/// <param name="PadSteerStrength">Whether the pad gives RC_steer's strength from how far to the side it is held (else full).</param>
public sealed record DriveSettings(int ManualMaxPwm, int AutoLimit, int ManualAccelPwmPerS, bool PadSteerStrength)
{
    /// <summary>The lowest manual cap (about a tenth of full duty), as <see cref="ManualInput.MinMaxDrive"/>.</summary>
    public const int ManualMaxMin = ManualInput.MinMaxDrive;
    /// <summary>The highest acceleration the car takes (as auto_accel_pwm_per_s in core.json).</summary>
    public const int AccelMax = 25500;
    /// <summary>The car's default manual acceleration (that of the auto start ramp).</summary>
    public const int DefaultManualAccel = 287;

    /// <summary>The four settings from a get_status result, a status event or a set_drive_settings result; false when any is missing.</summary>
    public static bool TryRead(JsonElement data, out DriveSettings settings)
    {
        settings = new DriveSettings(0, 0, 0, false);
        if (data.ValueKind != JsonValueKind.Object) return false;
        if (!data.TryGetProperty("manual_max_pwm", out var mm) || !mm.TryGetInt32(out var manualMax)) return false;
        if (!data.TryGetProperty("auto_limit", out var al) || !al.TryGetInt32(out var autoLimit)) return false;
        if (!data.TryGetProperty("manual_accel_pwm_per_s", out var ma) || !ma.TryGetInt32(out var accel)) return false;
        if (!data.TryGetProperty("pad_steer_strength", out var ps) || ps.ValueKind is not (JsonValueKind.True or JsonValueKind.False)) return false;
        settings = new DriveSettings(manualMax, autoLimit, accel, ps.ValueKind == JsonValueKind.True);
        return true;
    }

    /// <summary>The acceleration in words: "287（0→255 約 0.9 秒）", "0（加速なし）".</summary>
    public static string AccelText(int pwmPerS) => pwmPerS <= 0
        ? "0（加速なし）"
        : string.Format(CultureInfo.InvariantCulture, "{0}（0→255 約 {1:0.0#} 秒）", pwmPerS, 255.0 / pwmPerS);
}
