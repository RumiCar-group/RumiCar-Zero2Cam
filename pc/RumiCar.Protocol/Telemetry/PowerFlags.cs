namespace RumiCar.Protocol.Telemetry;

/// <summary>Firmware throttling flags (telemetry "throttled"), shown in Japanese (docs 3.9).</summary>
public static class PowerFlags
{
    public const uint UnderVoltageNow = 0x1;
    public const uint UnderVoltageSinceBoot = 0x10000;
    public const uint ThrottledNow = 0x4;
    public const uint SoftTemperatureLimit = 0x8;

    public static string Describe(uint flags)
    {
        if (flags == 0) return "正常（低電圧なし）";
        var parts = new List<string>();
        if ((flags & UnderVoltageNow) != 0) parts.Add("低電圧（現在）");
        else if ((flags & UnderVoltageSinceBoot) != 0) parts.Add("低電圧あり（起動後）");
        if ((flags & ThrottledNow) != 0) parts.Add("処理速度制限中");
        if ((flags & SoftTemperatureLimit) != 0) parts.Add("温度制限中");
        if (parts.Count == 0) parts.Add($"フラグ 0x{flags:X}");
        return string.Join("、", parts);
    }
}
