namespace RumiCar.Protocol.Telemetry;

/// <summary>One detector's result in the aux output of the cam_drive logic.</summary>
public sealed record Detection(int Index, double Score, int CellX, int CellY, int Cells, bool Present, bool Active);

/// <summary>
/// Aux output format 1 (cam_drive, docs 3.6): [grid_x, grid_y] then 5 bytes per
/// detector in pack order — score (0..255), cell x, cell y, cells (clamped to
/// 255), flags (bit 0 present this frame, bit 1 active after confirmation).
/// </summary>
public static class AuxFormat1
{
    public const int Format = 1;
    public const int BytesPerDetector = 5;

    public sealed record Decoded(int GridX, int GridY, IReadOnlyList<Detection> Detections);

    /// <summary>Decodes the base64 aux string of a telemetry header; null when absent, malformed or another format.</summary>
    public static Decoded? Decode(TelemetryHeader header)
    {
        if (header.AuxFormat != Format || string.IsNullOrEmpty(header.Aux)) return null;
        byte[] bytes;
        try { bytes = Convert.FromBase64String(header.Aux); }
        catch (FormatException) { return null; }
        return Decode(bytes);
    }

    public static Decoded? Decode(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length < 2) return null;
        int gx = bytes[0], gy = bytes[1];
        if (gx == 0 || gy == 0) return null;
        var n = (bytes.Length - 2) / BytesPerDetector;
        var list = new List<Detection>(n);
        for (var i = 0; i < n; i++)
        {
            var o = 2 + i * BytesPerDetector;
            list.Add(new Detection(i, bytes[o] / 255.0, bytes[o + 1], bytes[o + 2], bytes[o + 3], (bytes[o + 4] & 1) != 0, (bytes[o + 4] & 2) != 0));
        }
        return new Decoded(gx, gy, list);
    }
}
