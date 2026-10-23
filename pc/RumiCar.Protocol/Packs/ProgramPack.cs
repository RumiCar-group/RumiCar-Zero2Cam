using System.Globalization;
using System.Text;
using System.Text.Json;

namespace RumiCar.Protocol.Packs;

/// <summary>
/// Turns a folder with the user's own driving program (drive.py, and
/// optionally params.json and other .py files) into a pack for the py_drive
/// logic (docs 3.12, 6.2): the files are copied to a staging folder and a
/// manifest.json is written there, so the user only edits the program and
/// never deals with manifests.  The user's folder is not changed.
/// </summary>
public static class ProgramPack
{
    public const string LogicName = "py_drive";
    public const int MinAbi = 2;
    public const long MaxTotalBytes = 1024 * 1024;   // a program is text; anything bigger was probably chosen by mistake
    public const int MaxFiles = 64;

    public sealed record Result(string StagingDirectory, string Id, string Version, IReadOnlyList<string> Files);

    /// <summary>
    /// Checks <paramref name="sourceFolder"/> and writes the pack into a new
    /// folder under <paramref name="stagingRoot"/>.  The version is the local
    /// time <paramref name="now"/> (yyyyMMdd.HHmmss), so every send is a new
    /// version and older ones stay on the car for rollback.  Throws
    /// <see cref="InvalidDataException"/> with a message for the user.
    /// </summary>
    public static Result Prepare(string sourceFolder, string stagingRoot, DateTime now)
    {
        if (!Directory.Exists(sourceFolder)) throw new InvalidDataException($"フォルダ {sourceFolder} がありません");
        if (!File.Exists(Path.Combine(sourceFolder, "drive.py")))
            throw new InvalidDataException("選んだフォルダに drive.py がありません（見本は README「自分の走行プログラムを書く」）");

        var paramsPath = Path.Combine(sourceFolder, "params.json");
        if (File.Exists(paramsPath))
        {
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(paramsPath, Encoding.UTF8),
                    new JsonDocumentOptions { AllowTrailingCommas = false, CommentHandling = JsonCommentHandling.Disallow });
                if (doc.RootElement.ValueKind != JsonValueKind.Object)
                    throw new InvalidDataException("params.json は { ... } の形（名前と値の組）にします");
            }
            catch (JsonException e)
            {
                throw new InvalidDataException($"params.json の {e.LineNumber + 1} 行目が JSON として読めません: {e.Message}");
            }
        }

        var files = ProgramFiles(sourceFolder).ToList();
        if (files.Count > MaxFiles) throw new InvalidDataException($"ファイルが多すぎます（{files.Count} 個。{MaxFiles} 個まで）");
        var total = files.Sum(f => new FileInfo(Path.Combine(sourceFolder, f)).Length);
        if (total > MaxTotalBytes) throw new InvalidDataException($"ファイルが大きすぎます（合計 {total} バイト。{MaxTotalBytes} バイトまで）");

        var id = PackId(Path.GetFileName(Path.TrimEndingDirectorySeparator(sourceFolder)));
        var version = now.ToString("yyyyMMdd.HHmmss", CultureInfo.InvariantCulture);
        var staging = Path.Combine(stagingRoot, id + "-" + version);
        if (Directory.Exists(staging)) Directory.Delete(staging, true);
        Directory.CreateDirectory(staging);
        var manifest = new Manifest
        {
            Schema = 1,
            Kind = "pack",
            Id = id,
            Version = version,
            Created = now.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture),
            Description = $"自作の走行プログラム（{Path.GetFileName(Path.TrimEndingDirectorySeparator(sourceFolder))} フォルダ、{now:yyyy-MM-dd HH:mm} に送信）",
            Logic = new ManifestLogicRef { Name = LogicName, MinAbi = MinAbi },
        };
        foreach (var rel in files)
        {
            var src = Path.Combine(sourceFolder, rel);
            var dst = Path.Combine(staging, rel);
            Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
            File.Copy(src, dst);
            var bytes = File.ReadAllBytes(dst);
            manifest.Files.Add(new ManifestFile { Path = rel.Replace('\\', '/'), Sha256 = ManifestVerifier.Sha256Hex(bytes), Bytes = bytes.Length });
        }
        manifest.Files.Sort((a, b) => string.CompareOrdinal(a.Path, b.Path));
        File.WriteAllText(Path.Combine(staging, "manifest.json"), manifest.ToJson(), new UTF8Encoding(false));
        return new Result(staging, id, version, manifest.Files.Select(f => f.Path).ToList());
    }

    /// <summary>The files that make up the program: .py and .json files (manifest.json, hidden and cache folders excluded).</summary>
    public static IEnumerable<string> ProgramFiles(string folder)
    {
        foreach (var full in Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories).OrderBy(p => p, StringComparer.Ordinal))
        {
            var rel = Path.GetRelativePath(folder, full).Replace('\\', '/');
            var parts = rel.Split('/');
            if (parts.Any(p => p.StartsWith('.') || p == "__pycache__")) continue;
            if (rel == "manifest.json") continue;
            var ext = Path.GetExtension(rel).ToLowerInvariant();
            if (ext is not (".py" or ".json")) continue;
            yield return rel;
        }
    }

    /// <summary>A pack id from a folder name: lower-case letters, digits and '-' ("My Drive_2" -> "my-drive-2"); "my-drive" when nothing is left.</summary>
    public static string PackId(string folderName)
    {
        var sb = new StringBuilder();
        foreach (var c in folderName.ToLowerInvariant())
        {
            if (c is >= 'a' and <= 'z' or >= '0' and <= '9') sb.Append(c);
            else if (sb.Length > 0 && sb[^1] != '-') sb.Append('-');
        }
        var id = sb.ToString().Trim('-');
        if (id.Length > 40) id = id[..40].TrimEnd('-');
        return id.Length == 0 ? "my-drive" : id;
    }
}
