using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace RumiCar.Protocol.Packs;

/// <summary>One entry of a manifest's file list.</summary>
public sealed class ManifestFile
{
    [JsonPropertyName("path")] public string Path { get; set; } = "";
    [JsonPropertyName("sha256")] public string Sha256 { get; set; } = "";
    [JsonPropertyName("bytes")] public long Bytes { get; set; }
}

public sealed class ManifestLogicRef
{
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("min_abi")] public int MinAbi { get; set; }
}

/// <summary>manifest.json of a pack (decision data) or a logic plug-in directory. Mirrors car/core/src/manifest.cpp.</summary>
public sealed class Manifest
{
    [JsonPropertyName("schema")] public int Schema { get; set; }
    [JsonPropertyName("kind")] public string Kind { get; set; } = "";
    // pack
    [JsonPropertyName("id")] public string? Id { get; set; }
    [JsonPropertyName("logic")] public ManifestLogicRef? Logic { get; set; }
    // logic
    [JsonPropertyName("name")] public string? Name { get; set; }
    [JsonPropertyName("abi")] public int? Abi { get; set; }
    [JsonPropertyName("entry")] public string? Entry { get; set; }
    // common
    [JsonPropertyName("version")] public string Version { get; set; } = "";
    [JsonPropertyName("created")] public string? Created { get; set; }
    [JsonPropertyName("description")] public string? Description { get; set; }
    [JsonPropertyName("files")] public List<ManifestFile> Files { get; set; } = new();

    public bool IsPack => Kind == "pack";
    public bool IsLogic => Kind == "logic";

    /// <summary>Directory name used under &lt;state_dir&gt;/packs or /logic on the car.</summary>
    public string DirectoryName => (IsPack ? Id : Name) + "-" + Version;

    private static readonly JsonSerializerOptions Options = new() { DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull };

    public static Manifest Parse(string json)
    {
        var m = JsonSerializer.Deserialize<Manifest>(json, Options) ?? throw new FormatException("manifest: empty");
        var problems = m.StructuralProblems().ToList();
        if (problems.Count > 0) throw new FormatException("manifest: " + problems[0]);
        return m;
    }

    public string ToJson() => JsonSerializer.Serialize(this, new JsonSerializerOptions(Options) { WriteIndented = true }) + "\n";

    /// <summary>Structural checks that need no file access.</summary>
    public IEnumerable<string> StructuralProblems()
    {
        if (Schema != 1) yield return $"unsupported schema {Schema}";
        if (IsPack)
        {
            if (string.IsNullOrEmpty(Id)) yield return "'id' is required";
            if (Logic is null || string.IsNullOrEmpty(Logic.Name)) yield return "'logic.name' is required";
            if (Logic is not null && Logic.MinAbi < 0) yield return "'logic.min_abi' must be >= 0";
        }
        else if (IsLogic)
        {
            if (string.IsNullOrEmpty(Name)) yield return "'name' is required";
            if (Abi is null || Abi < 0) yield return "'abi' is required";
            if (string.IsNullOrEmpty(Entry) || !IsSafeRelativePath(Entry)) yield return "'entry' is not a safe relative path";
            else if (!Files.Any(f => f.Path == Entry)) yield return "'entry' must be listed in 'files'";
        }
        else
        {
            yield return $"unknown kind '{Kind}'";
        }
        if (string.IsNullOrEmpty(Version)) yield return "'version' is required";
        if (Files.Count == 0) yield return "'files' must be a non-empty array";
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var f in Files)
        {
            if (!IsSafeRelativePath(f.Path)) yield return $"unsafe file path '{f.Path}'";
            if (!seen.Add(f.Path)) yield return $"duplicate file path '{f.Path}'";
            if (!IsHex64(f.Sha256)) yield return $"bad sha256 for '{f.Path}'";
            if (f.Bytes < 0) yield return $"bad size for '{f.Path}'";
        }
    }

    public static bool IsSafeRelativePath(string p)
    {
        if (string.IsNullOrEmpty(p) || p.StartsWith('/') || p.StartsWith('\\') || p == "manifest.json") return false;
        if (p.Length >= 2 && p[1] == ':') return false; // drive letter
        foreach (var part in p.Split('/', '\\'))
        {
            if (part is "" or "." or "..") return false;
        }
        return true;
    }

    private static bool IsHex64(string s)
    {
        if (s.Length != 64) return false;
        foreach (var c in s)
        {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }
}

/// <summary>Verifies a directory against its manifest.json, the same way rumicar-core does before activation.</summary>
public static class ManifestVerifier
{
    /// <summary>Returns the manifest and an empty problem list on success.</summary>
    public static (Manifest? manifest, List<string> problems) Verify(string directory)
    {
        var problems = new List<string>();
        var path = Path.Combine(directory, "manifest.json");
        if (!File.Exists(path))
        {
            problems.Add("manifest.json is missing");
            return (null, problems);
        }
        Manifest m;
        try
        {
            m = Manifest.Parse(File.ReadAllText(path));
        }
        catch (Exception e) when (e is JsonException or FormatException)
        {
            problems.Add(e.Message);
            return (null, problems);
        }
        foreach (var f in m.Files)
        {
            var full = Path.Combine(directory, f.Path.Replace('/', Path.DirectorySeparatorChar));
            if (!File.Exists(full)) { problems.Add($"missing file {f.Path}"); continue; }
            var info = new FileInfo(full);
            if (info.Length != f.Bytes) problems.Add($"size mismatch for {f.Path} (manifest {f.Bytes}, file {info.Length})");
            var digest = Sha256Hex(full);
            if (digest != f.Sha256) problems.Add($"sha256 mismatch for {f.Path}");
        }
        return (m, problems);
    }

    public static string Sha256Hex(string file)
    {
        using var stream = File.OpenRead(file);
        var hash = SHA256.HashData(stream);
        return Convert.ToHexStringLower(hash);
    }

    public static string Sha256Hex(ReadOnlySpan<byte> data) => Convert.ToHexStringLower(SHA256.HashData(data));
}
