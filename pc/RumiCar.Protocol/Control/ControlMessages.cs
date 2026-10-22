using System.Text.Json;
using System.Text.Json.Serialization;

namespace RumiCar.Protocol.Control;

/// <summary>Request types understood by the car's control server (protocol version 1).</summary>
public static class RequestType
{
    public const string Hello = "hello";
    public const string GetStatus = "get_status";
    public const string SetMode = "set_mode";
    public const string ManualDrive = "manual_drive";
    public const string Heartbeat = "heartbeat";
    public const string EStop = "estop";
    public const string EStopClear = "estop_clear";
    public const string ListVersions = "list_versions";
    public const string Activate = "activate";
    public const string ActivatePair = "activate_pair";
    public const string Rollback = "rollback";
    public const string SetAutoLimit = "set_auto_limit";
    public const string SetWifi = "set_wifi";
    public const string GetLog = "get_log";
    public const string UploadBegin = "upload_begin";
    public const string UploadChunk = "upload_chunk";
    public const string UploadEnd = "upload_end";
    public const string UploadAbort = "upload_abort";
    public const string DeleteVersion = "delete_version";
}

/// <summary>Driving modes selectable with set_mode.</summary>
public static class DriveMode
{
    public const string Auto = "auto";
    public const string Manual = "manual";
    public const string RecordOnly = "record_only";
}

/// <summary>Protocol version carried in hello.</summary>
public static class ControlProtocol
{
    public const int Version = 1;
}

/// <summary>A request from the PC. Serialised as one JSON object on one line.</summary>
public sealed class ControlRequest
{
    [JsonPropertyName("id")] public long Id { get; set; }
    [JsonPropertyName("type")] public string Type { get; set; } = "";
    [JsonPropertyName("params")] public Dictionary<string, JsonElement>? Params { get; set; }
}

/// <summary>The car's answer to one request (same id).</summary>
public sealed class ControlResponse
{
    [JsonPropertyName("id")] public long Id { get; set; }
    [JsonPropertyName("ok")] public bool Ok { get; set; }
    [JsonPropertyName("error")] public string? Error { get; set; }
    [JsonPropertyName("result")] public JsonElement? Result { get; set; }
}

/// <summary>An unsolicited message from the car (status, estop, activation ...).</summary>
public sealed class ControlEvent
{
    [JsonPropertyName("event")] public string Event { get; set; } = "";
    [JsonPropertyName("data")] public JsonElement? Data { get; set; }
}

/// <summary>Encodes and decodes control lines. Lines are UTF-8 JSON without embedded newlines.</summary>
public static class ControlCodec
{
    private static readonly JsonSerializerOptions Options = new()
    {
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        WriteIndented = false,
    };

    public static string Encode(ControlRequest request) => JsonSerializer.Serialize(request, Options);
    public static string Encode(ControlResponse response) => JsonSerializer.Serialize(response, Options);
    public static string Encode(ControlEvent evt) => JsonSerializer.Serialize(evt, Options);

    /// <summary>Decodes one line into a request, response or event; returns false for anything else.</summary>
    public static bool TryDecode(string line, out object? message)
    {
        message = null;
        if (string.IsNullOrWhiteSpace(line)) return false;
        try
        {
            using var doc = JsonDocument.Parse(line);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return false;
            if (root.TryGetProperty("event", out _))
            {
                message = JsonSerializer.Deserialize<ControlEvent>(line, Options);
            }
            else if (root.TryGetProperty("ok", out _))
            {
                message = JsonSerializer.Deserialize<ControlResponse>(line, Options);
            }
            else if (root.TryGetProperty("type", out _))
            {
                message = JsonSerializer.Deserialize<ControlRequest>(line, Options);
            }
            return message is not null;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    // --- typed builders ------------------------------------------------------

    public static ControlRequest Hello(long id, string clientName) =>
        Build(id, RequestType.Hello, new { client = clientName, protocol = ControlProtocol.Version });

    public static ControlRequest GetStatus(long id) => Build(id, RequestType.GetStatus, null);

    public static ControlRequest SetMode(long id, string mode) => Build(id, RequestType.SetMode, new { mode });

    /// <param name="steer">-1 left, 0 centre, +1 right</param>
    /// <param name="drivePermille">-1000..1000</param>
    public static ControlRequest ManualDrive(long id, int steer, int drivePermille)
    {
        if (steer is < -1 or > 1) throw new ArgumentOutOfRangeException(nameof(steer));
        if (drivePermille is < -1000 or > 1000) throw new ArgumentOutOfRangeException(nameof(drivePermille));
        return Build(id, RequestType.ManualDrive, new { steer, drive_permille = drivePermille });
    }

    public static ControlRequest Heartbeat(long id) => Build(id, RequestType.Heartbeat, null);
    public static ControlRequest EStop(long id) => Build(id, RequestType.EStop, null);
    public static ControlRequest EStopClear(long id) => Build(id, RequestType.EStopClear, null);
    public static ControlRequest ListVersions(long id) => Build(id, RequestType.ListVersions, null);

    /// <param name="kind">"logic" or "pack"</param>
    /// <param name="directoryName">e.g. base-tof-0.1.0</param>
    public static ControlRequest Activate(long id, string kind, string directoryName)
    {
        ValidateKind(kind);
        if (directoryName.Length == 0 || directoryName.Contains('/') || directoryName.Contains('\\') || directoryName.StartsWith('.'))
            throw new ArgumentException("bad directory name", nameof(directoryName));
        return Build(id, RequestType.Activate, new { kind, name = directoryName });
    }

    /// <summary>A logic and a pack for it in one step (switching to another logic, docs 5.2).</summary>
    public static ControlRequest ActivatePair(long id, string logicDirectory, string packDirectory)
    {
        ValidateDirectoryName(logicDirectory, nameof(logicDirectory));
        ValidateDirectoryName(packDirectory, nameof(packDirectory));
        return Build(id, RequestType.ActivatePair, new { logic = logicDirectory, pack = packDirectory });
    }

    public static ControlRequest Rollback(long id, string kind)
    {
        ValidateKind(kind);
        return Build(id, RequestType.Rollback, new { kind });
    }

    /// <summary>Back to the previous logic and pack together (undoes activate_pair).</summary>
    public static ControlRequest RollbackBoth(long id) => Build(id, RequestType.Rollback, new { kind = "both" });

    /// <summary>Cap of the drive in auto mode, per mille (the car clamps it to its max_drive_permille).</summary>
    public static ControlRequest SetAutoLimit(long id, int drivePermille)
    {
        if (drivePermille is < 0 or > 1000) throw new ArgumentOutOfRangeException(nameof(drivePermille));
        return Build(id, RequestType.SetAutoLimit, new { drive_permille = drivePermille });
    }

    /// <summary>Temporary (until reboot) access-point change.</summary>
    public static ControlRequest SetWifi(long id, string ssid, int channel)
    {
        if (ssid.Length is 0 or > 32) throw new ArgumentOutOfRangeException(nameof(ssid));
        if (channel is < 1 or > 13) throw new ArgumentOutOfRangeException(nameof(channel));
        return Build(id, RequestType.SetWifi, new { ssid, channel });
    }

    public static ControlRequest GetLog(long id, int lines) => Build(id, RequestType.GetLog, new { lines });

    /// <summary>One file announced in upload_begin (path relative to the directory, forward slashes).</summary>
    public sealed record UploadFile(string Path, long Bytes, string Sha256);

    /// <summary>Largest payload (before base64) the car accepts per upload_chunk.</summary>
    public const int UploadChunkBytes = 32 * 1024;

    public static ControlRequest UploadBegin(long id, string kind, string directoryName, IReadOnlyList<UploadFile> files)
    {
        ValidateKind(kind);
        if (directoryName.Length == 0 || directoryName.Contains('/') || directoryName.Contains('\\') || directoryName.StartsWith('.'))
            throw new ArgumentException("bad directory name", nameof(directoryName));
        if (files.Count == 0) throw new ArgumentException("no files", nameof(files));
        return Build(id, RequestType.UploadBegin, new
        {
            kind,
            name = directoryName,
            files = files.Select(f => new { path = f.Path, bytes = f.Bytes, sha256 = f.Sha256 }).ToArray(),
        });
    }

    public static ControlRequest UploadChunk(long id, string path, long offset, ReadOnlyMemory<byte> data)
    {
        if (data.Length > UploadChunkBytes) throw new ArgumentOutOfRangeException(nameof(data), "chunk too large");
        if (offset < 0) throw new ArgumentOutOfRangeException(nameof(offset));
        return Build(id, RequestType.UploadChunk, new { path, offset, data = Convert.ToBase64String(data.Span) });
    }

    public static ControlRequest UploadEnd(long id) => Build(id, RequestType.UploadEnd, null);
    public static ControlRequest UploadAbort(long id) => Build(id, RequestType.UploadAbort, null);

    public static ControlRequest DeleteVersion(long id, string kind, string directoryName)
    {
        ValidateKind(kind);
        if (directoryName.Length == 0 || directoryName.Contains('/') || directoryName.Contains('\\') || directoryName.StartsWith('.'))
            throw new ArgumentException("bad directory name", nameof(directoryName));
        return Build(id, RequestType.DeleteVersion, new { kind, name = directoryName });
    }

    private static void ValidateDirectoryName(string name, string param)
    {
        if (name.Length == 0 || name.Contains('/') || name.Contains('\\') || name.StartsWith('.'))
            throw new ArgumentException("bad directory name", param);
    }

    private static void ValidateKind(string kind)
    {
        if (kind is not ("logic" or "pack")) throw new ArgumentException("kind must be 'logic' or 'pack'", nameof(kind));
    }

    private static ControlRequest Build(long id, string type, object? parameters)
    {
        var req = new ControlRequest { Id = id, Type = type };
        if (parameters is not null)
        {
            var element = JsonSerializer.SerializeToElement(parameters, Options);
            req.Params = new Dictionary<string, JsonElement>();
            foreach (var p in element.EnumerateObject()) req.Params[p.Name] = p.Value.Clone();
        }
        return req;
    }
}
