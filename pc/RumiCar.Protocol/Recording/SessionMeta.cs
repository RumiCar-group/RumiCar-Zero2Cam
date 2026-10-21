using System.Text.Json;
using System.Text.Json.Serialization;
using RumiCar.Protocol.Telemetry;

namespace RumiCar.Protocol.Recording;

/// <summary>One line of meta.jsonl: the frame's index, its image file (null when none) and the telemetry header as received.</summary>
public sealed class SessionMetaLine
{
    [JsonPropertyName("index")] public int Index { get; set; }
    [JsonPropertyName("image")] public string? Image { get; set; }
    [JsonPropertyName("received")] public string Received { get; set; } = "";
    [JsonPropertyName("header")] public TelemetryHeader Header { get; set; } = new();
}

/// <summary>Reads a session folder written by <see cref="SessionWriter"/>.</summary>
public static class SessionMeta
{
    public const string MetaFileName = "meta.jsonl";
    public const string FramesDirectory = "frames";

    private static readonly JsonSerializerOptions Options = new() { WriteIndented = false };

    public static string Serialize(SessionMetaLine line) => JsonSerializer.Serialize(line, Options);

    /// <summary>Lines of meta.jsonl in file order; unreadable lines are skipped and counted in <paramref name="skipped"/>.</summary>
    public static List<SessionMetaLine> Read(string sessionDirectory, out int skipped)
    {
        var path = Path.Combine(sessionDirectory, MetaFileName);
        if (!File.Exists(path)) throw new FileNotFoundException("not a session folder (meta.jsonl is missing)", path);
        var lines = new List<SessionMetaLine>();
        skipped = 0;
        foreach (var text in File.ReadLines(path))
        {
            if (string.IsNullOrWhiteSpace(text)) continue;
            SessionMetaLine? line;
            try { line = JsonSerializer.Deserialize<SessionMetaLine>(text, Options); }
            catch (JsonException) { line = null; }
            if (line is null || line.Header.TofMm.Length != 3) { skipped++; continue; }
            lines.Add(line);
        }
        return lines;
    }
}
