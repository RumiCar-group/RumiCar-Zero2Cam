using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace RumiCar.Protocol.Telemetry;

/// <summary>
/// Per-frame metadata sent by the car ahead of the JPEG image.
/// Field names match the JSON on the wire and the meta.jsonl recording format.
/// </summary>
public sealed class TelemetryHeader
{
    [JsonPropertyName("seq")] public uint Seq { get; set; }
    [JsonPropertyName("t_us")] public ulong TimestampMicroseconds { get; set; }
    [JsonPropertyName("tof_mm")] public int[] TofMm { get; set; } = new int[3];
    /// <summary>What was actually sent to the motors: steer -1/0/1, drive as PWM duty in per mille (negative = reverse), brake = both bridge inputs high.</summary>
    [JsonPropertyName("steer")] public int Steer { get; set; }
    [JsonPropertyName("drive_permille")] public int DrivePermille { get; set; }
    [JsonPropertyName("brake")] public bool Brake { get; set; }
    [JsonPropertyName("state")] public int State { get; set; }
    [JsonPropertyName("state_name")] public string StateName { get; set; } = "";
    [JsonPropertyName("flags")] public uint Flags { get; set; }
    [JsonPropertyName("mode")] public string Mode { get; set; } = "";
    /// <summary>Emergency stop latched on the car (estop request, heartbeat loss, disconnect in manual mode).</summary>
    [JsonPropertyName("estop")] public bool EStop { get; set; }
    /// <summary>What the driving logic decided this frame, even when it was not applied (manual / record_only / e-stop).</summary>
    [JsonPropertyName("logic_steer")] public int LogicSteer { get; set; }
    [JsonPropertyName("logic_drive_permille")] public int LogicDrivePermille { get; set; }
    /// <summary>Identity of the logic and pack that produced this frame (recorded with every frame so a session's lineage is known).</summary>
    [JsonPropertyName("logic_name")] public string LogicName { get; set; } = "";
    [JsonPropertyName("logic_version")] public string LogicVersion { get; set; } = "";
    [JsonPropertyName("pack_id")] public string PackId { get; set; } = "";
    [JsonPropertyName("pack_version")] public string PackVersion { get; set; } = "";
    [JsonPropertyName("battery_mv")] public int BatteryMillivolts { get; set; }
    /// <summary>Firmware throttling flags (bit 0 under-voltage now, bit 16 under-voltage since boot, ...); 0 when fine or unsupported.</summary>
    [JsonPropertyName("throttled")] public uint Throttled { get; set; }
    /// <summary>Auxiliary output of the driving logic (ABI 2): a format id and base64 bytes, absent when the logic has none. Format 1 = <see cref="AuxFormat1"/>.</summary>
    [JsonPropertyName("aux_format")] public int AuxFormat { get; set; }
    [JsonPropertyName("aux")] public string? Aux { get; set; }
    /// <summary>Width/height of the JPEG, 0 when the frame carries no image.</summary>
    [JsonPropertyName("image_w")] public int ImageWidth { get; set; }
    [JsonPropertyName("image_h")] public int ImageHeight { get; set; }
    /// <summary>Camera image counter and sensor timestamp (microseconds); 0 when the frame carries no image.</summary>
    [JsonPropertyName("image_seq")] public uint ImageSeq { get; set; }
    [JsonPropertyName("image_t_us")] public ulong ImageTimestampMicroseconds { get; set; }
}

/// <summary>One telemetry frame: header plus optional JPEG payload.</summary>
public sealed class TelemetryFrame
{
    public TelemetryHeader Header { get; init; } = new();
    public byte[] Jpeg { get; init; } = Array.Empty<byte>();
}

/// <summary>
/// Wire layout (little-endian):
///   "RMCT" (4) | version u8 (=1) | header_len u16 | payload_len u32 | header JSON (UTF-8) | payload (JPEG)
/// </summary>
public static class TelemetryFrameCodec
{
    public const byte Version = 1;
    public const int FixedHeaderBytes = 4 + 1 + 2 + 4;
    public const int MaxHeaderBytes = 16 * 1024;
    public const int MaxPayloadBytes = 4 * 1024 * 1024;
    private static readonly byte[] Magic = "RMCT"u8.ToArray();

    private static readonly JsonSerializerOptions Options = new()
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        WriteIndented = false,
    };

    public static byte[] Encode(TelemetryFrame frame)
    {
        var header = JsonSerializer.SerializeToUtf8Bytes(frame.Header, Options);
        if (header.Length > MaxHeaderBytes) throw new InvalidOperationException("telemetry header too large");
        if (frame.Jpeg.Length > MaxPayloadBytes) throw new InvalidOperationException("telemetry payload too large");
        var buf = new byte[FixedHeaderBytes + header.Length + frame.Jpeg.Length];
        Magic.CopyTo(buf, 0);
        buf[4] = Version;
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(5, 2), (ushort)header.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(7, 4), (uint)frame.Jpeg.Length);
        header.CopyTo(buf, FixedHeaderBytes);
        frame.Jpeg.CopyTo(buf, FixedHeaderBytes + header.Length);
        return buf;
    }

    public enum DecodeResult { Ok, NeedMore, Corrupt }

    /// <summary>
    /// Tries to decode one frame from the start of <paramref name="data"/>.
    /// Returns NeedMore when the buffer holds only part of a frame; Corrupt when
    /// the bytes at the start cannot be a frame (the caller should resynchronise
    /// by discarding bytes until the next magic).
    /// </summary>
    public static DecodeResult TryDecode(ReadOnlySpan<byte> data, out TelemetryFrame? frame, out int consumed)
    {
        frame = null;
        consumed = 0;
        if (data.Length < FixedHeaderBytes)
        {
            int n = Math.Min(Magic.Length, data.Length);
            return data[..n].SequenceEqual(Magic.AsSpan(0, n)) ? DecodeResult.NeedMore : DecodeResult.Corrupt;
        }
        if (!data[..4].SequenceEqual(Magic)) return DecodeResult.Corrupt;
        if (data[4] != Version) return DecodeResult.Corrupt;
        int headerLen = BinaryPrimitives.ReadUInt16LittleEndian(data.Slice(5, 2));
        long payloadLen = BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(7, 4));
        if (headerLen == 0 || headerLen > MaxHeaderBytes || payloadLen > MaxPayloadBytes) return DecodeResult.Corrupt;
        long total = FixedHeaderBytes + headerLen + payloadLen;
        if (data.Length < total) return DecodeResult.NeedMore;

        TelemetryHeader? header;
        try
        {
            header = JsonSerializer.Deserialize<TelemetryHeader>(data.Slice(FixedHeaderBytes, headerLen), Options);
        }
        catch (JsonException)
        {
            return DecodeResult.Corrupt;
        }
        if (header is null || header.TofMm.Length != 3) return DecodeResult.Corrupt;

        var jpeg = data.Slice(FixedHeaderBytes + headerLen, (int)payloadLen).ToArray();
        frame = new TelemetryFrame { Header = header, Jpeg = jpeg };
        consumed = (int)total;
        return DecodeResult.Ok;
    }

    /// <summary>Index of the next magic at or after <paramref name="start"/>, or -1.</summary>
    public static int FindMagic(ReadOnlySpan<byte> data, int start)
    {
        if (start >= data.Length) return -1;
        var idx = data[start..].IndexOf(Magic);
        return idx < 0 ? -1 : idx + start;
    }

    /// <summary>Header JSON exactly as serialised on the wire (used by the recorder).</summary>
    public static string HeaderToJson(TelemetryHeader header) => JsonSerializer.Serialize(header, Options);

    public static TelemetryHeader? HeaderFromJson(string json)
    {
        try { return JsonSerializer.Deserialize<TelemetryHeader>(json, Options); }
        catch (JsonException) { return null; }
    }

    public static string DecodeHeaderText(ReadOnlySpan<byte> utf8) => Encoding.UTF8.GetString(utf8);
}

/// <summary>Reads consecutive frames from a stream (e.g. a TCP connection), resynchronising after corruption.</summary>
public sealed class TelemetryFrameReader
{
    private readonly Stream _stream;
    private byte[] _buffer = new byte[64 * 1024];
    private int _filled;

    public TelemetryFrameReader(Stream stream) => _stream = stream;

    /// <summary>Number of corrupt byte runs skipped so far.</summary>
    public int Resyncs { get; private set; }

    /// <summary>Returns the next frame, or null at end of stream.</summary>
    public async Task<TelemetryFrame?> ReadAsync(CancellationToken ct)
    {
        while (true)
        {
            var result = TelemetryFrameCodec.TryDecode(_buffer.AsSpan(0, _filled), out var frame, out var consumed);
            if (result == TelemetryFrameCodec.DecodeResult.Ok)
            {
                Shift(consumed);
                return frame;
            }
            if (result == TelemetryFrameCodec.DecodeResult.Corrupt)
            {
                Resyncs++;
                var next = TelemetryFrameCodec.FindMagic(_buffer.AsSpan(0, _filled), 1);
                Shift(next < 0 ? _filled : next);
                continue;
            }
            if (_filled == _buffer.Length) Array.Resize(ref _buffer, _buffer.Length * 2);
            int n = await _stream.ReadAsync(_buffer.AsMemory(_filled), ct).ConfigureAwait(false);
            if (n == 0) return null;
            _filled += n;
        }
    }

    private void Shift(int count)
    {
        if (count <= 0) return;
        Buffer.BlockCopy(_buffer, count, _buffer, 0, _filled - count);
        _filled -= count;
    }
}
