using System.Text.Json;
using System.Text.Json.Serialization;

namespace RumiCar.Protocol.Recording;

/// <summary>
/// What a recording session is about, written into session.json so the Claude
/// desktop session (and the person) can tell sessions apart: the scenario the
/// person chose ("壁のみの走行データ", "信号機ありケース1", ...), a free note,
/// and the logic/pack the car was running when the session started.
/// </summary>
public sealed class SessionInfo
{
    /// <summary>Scenario name chosen in the drive manager (empty = not given).</summary>
    public string Scenario { get; init; } = "";
    /// <summary>Free text (empty = none).</summary>
    public string Note { get; init; } = "";
    /// <summary>Car name reported in hello (may differ from the folder name's car).</summary>
    public string CarReported { get; init; } = "";
    public string CoreVersion { get; init; } = "";
    public string LogicName { get; init; } = "";
    public string LogicVersion { get; init; } = "";
    public string PackId { get; init; } = "";
    public string PackVersion { get; init; } = "";
    public string PackDescription { get; init; } = "";
    public IReadOnlyList<string> PackCapabilities { get; init; } = Array.Empty<string>();
    public IReadOnlyList<string> PackScenarios { get; init; } = Array.Empty<string>();
    public string PackParent { get; init; } = "";
    /// <summary>Mode at the start of the session (auto / manual / record_only).</summary>
    public string Mode { get; init; } = "";
    /// <summary>Drive manager version that wrote the session.</summary>
    public string Recorder { get; init; } = "";

    /// <summary>Builds the car part from a get_status result (missing keys stay empty).</summary>
    public static SessionInfo FromStatus(JsonElement? status, string scenario, string note, string carReported, string coreVersion, string recorder)
    {
        static string Str(JsonElement? e, string key) =>
            e is JsonElement o && o.ValueKind == JsonValueKind.Object && o.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
        static IReadOnlyList<string> List(JsonElement? e, string key)
        {
            if (e is not JsonElement o || o.ValueKind != JsonValueKind.Object || !o.TryGetProperty(key, out var v) || v.ValueKind != JsonValueKind.Array)
                return Array.Empty<string>();
            var list = new List<string>();
            foreach (var item in v.EnumerateArray())
                if (item.ValueKind == JsonValueKind.String && item.GetString() is { Length: > 0 } s) list.Add(s);
            return list;
        }
        return new SessionInfo
        {
            Scenario = scenario.Trim(),
            Note = note.Trim(),
            CarReported = carReported,
            CoreVersion = coreVersion,
            LogicName = Str(status, "logic_name"),
            LogicVersion = Str(status, "logic_version"),
            PackId = Str(status, "pack_id"),
            PackVersion = Str(status, "pack_version"),
            PackDescription = Str(status, "pack_description"),
            PackCapabilities = List(status, "pack_capabilities"),
            PackScenarios = List(status, "pack_scenarios"),
            PackParent = Str(status, "pack_parent"),
            Mode = Str(status, "mode"),
            Recorder = recorder,
        };
    }
}

/// <summary>
/// The list of scenario names used so far, kept in &lt;data root&gt;/scenarios.json so
/// the drive manager can offer them again and the Claude desktop session can see
/// which scenarios exist.  Names are kept in order of last use (newest first).
/// </summary>
public static class ScenarioHistory
{
    public const string FileName = "scenarios.json";
    public const int MaxEntries = 100;

    private sealed class Doc
    {
        [JsonPropertyName("schema")] public int Schema { get; set; } = 1;
        [JsonPropertyName("scenarios")] public List<Entry> Scenarios { get; set; } = new();
    }

    public sealed class Entry
    {
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("sessions")] public int Sessions { get; set; }
        [JsonPropertyName("last_used")] public string LastUsed { get; set; } = "";
    }

    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    /// <summary>Names in order (newest first); empty when the file is missing or unreadable.</summary>
    public static IReadOnlyList<Entry> Load(string dataRoot)
    {
        var path = Path.Combine(dataRoot, FileName);
        if (!File.Exists(path)) return Array.Empty<Entry>();
        try
        {
            var doc = JsonSerializer.Deserialize<Doc>(File.ReadAllText(path), Options);
            return doc?.Scenarios.Where(e => !string.IsNullOrWhiteSpace(e.Name)).ToList() ?? new List<Entry>();
        }
        catch (Exception e) when (e is JsonException or IOException)
        {
            return Array.Empty<Entry>();
        }
    }

    /// <summary>Records one more session for <paramref name="name"/> (no-op for an empty name) and rewrites the file atomically.</summary>
    public static void RecordUse(string dataRoot, string name, DateTimeOffset when)
    {
        name = name.Trim();
        if (name.Length == 0) return;
        var entries = Load(dataRoot).ToList();
        var existing = entries.FirstOrDefault(e => string.Equals(e.Name, name, StringComparison.Ordinal));
        if (existing is null)
        {
            existing = new Entry { Name = name };
        }
        else
        {
            entries.Remove(existing);
        }
        existing.Sessions++;
        existing.LastUsed = when.ToUniversalTime().ToString("o");
        entries.Insert(0, existing);
        if (entries.Count > MaxEntries) entries.RemoveRange(MaxEntries, entries.Count - MaxEntries);
        Directory.CreateDirectory(dataRoot);
        var path = Path.Combine(dataRoot, FileName);
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(new Doc { Scenarios = entries }, Options) + "\n");
        File.Move(tmp, path, overwrite: true);
    }
}
