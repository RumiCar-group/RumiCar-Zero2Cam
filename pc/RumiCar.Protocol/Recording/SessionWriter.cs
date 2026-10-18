using System.Text;
using System.Text.Json;
using RumiCar.Protocol.Telemetry;

namespace RumiCar.Protocol.Recording;

/// <summary>
/// Writes received telemetry into the data folder layout read by the Claude
/// desktop app session (docs: データフォルダ):
///   sessions/&lt;car&gt;_&lt;yyyyMMdd-HHmmss&gt;/frames/000001.jpg
///   sessions/&lt;car&gt;_&lt;yyyyMMdd-HHmmss&gt;/meta.jsonl   (one header JSON per frame, in wire order)
///   sessions/&lt;car&gt;_&lt;yyyyMMdd-HHmmss&gt;/session.json
/// Frames without an image are still recorded in meta.jsonl (image_w = 0).
/// </summary>
public sealed class SessionWriter : IDisposable
{
    private readonly StreamWriter _meta;
    private readonly string _sessionJsonPath;
    private readonly DateTimeOffset _started;
    private int _frameIndex;
    private int _droppedGaps;
    private uint? _lastSeq;
    private bool _disposed;

    public string Directory { get; }
    public string CarName { get; }
    public int FramesWritten => _frameIndex;
    public int SequenceGaps => _droppedGaps;

    public SessionWriter(string dataRoot, string carName, DateTimeOffset started)
    {
        if (string.IsNullOrWhiteSpace(carName) || carName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            throw new ArgumentException("bad car name", nameof(carName));
        CarName = carName;
        _started = started;
        Directory = Path.Combine(dataRoot, "sessions", $"{carName}_{started.ToUniversalTime():yyyyMMdd-HHmmss}");
        System.IO.Directory.CreateDirectory(Path.Combine(Directory, "frames"));
        _meta = new StreamWriter(Path.Combine(Directory, "meta.jsonl"), append: false, new UTF8Encoding(false)) { NewLine = "\n" };
        _sessionJsonPath = Path.Combine(Directory, "session.json");
        WriteSessionJson(null);
    }

    public void Write(TelemetryFrame frame)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        _frameIndex++;
        if (_lastSeq is uint last && frame.Header.Seq != last + 1) _droppedGaps++;
        _lastSeq = frame.Header.Seq;

        string? imageFile = null;
        if (frame.Jpeg.Length > 0)
        {
            imageFile = $"{_frameIndex:D6}.jpg";
            File.WriteAllBytes(Path.Combine(Directory, "frames", imageFile), frame.Jpeg);
        }
        var line = new MetaLine
        {
            Index = _frameIndex,
            Image = imageFile,
            Received = DateTimeOffset.UtcNow.ToString("o"),
            Header = frame.Header,
        };
        _meta.WriteLine(JsonSerializer.Serialize(line, MetaOptions));
    }

    public void Flush() => _meta.Flush();

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _meta.Flush();
        _meta.Dispose();
        WriteSessionJson(DateTimeOffset.UtcNow);
    }

    private void WriteSessionJson(DateTimeOffset? ended)
    {
        var doc = new
        {
            schema = 1,
            car = CarName,
            started = _started.ToUniversalTime().ToString("o"),
            ended = ended?.ToString("o"),
            frames = _frameIndex,
            sequence_gaps = _droppedGaps,
        };
        var tmp = _sessionJsonPath + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(doc, new JsonSerializerOptions { WriteIndented = true }) + "\n");
        File.Move(tmp, _sessionJsonPath, overwrite: true);
    }

    private static readonly JsonSerializerOptions MetaOptions = new() { WriteIndented = false };

    private sealed class MetaLine
    {
        [System.Text.Json.Serialization.JsonPropertyName("index")] public int Index { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("image")] public string? Image { get; set; }
        [System.Text.Json.Serialization.JsonPropertyName("received")] public string Received { get; set; } = "";
        [System.Text.Json.Serialization.JsonPropertyName("header")] public TelemetryHeader Header { get; set; } = new();
    }
}
