using System.Globalization;
using System.Text.Json;

namespace RumiCar.Protocol.Releases;

/// <summary>One file attached to a GitHub Release.</summary>
public sealed record ReleaseAsset(string Name, long Size, string DownloadUrl);

/// <summary>
/// One Release of RumiCar-Zero2Cam as the drive manager sees it (docs 6.5): the
/// tag, the version it stands for and the files the SD-card feature needs —
/// the image (.img.xz), the Raspberry Pi Imager manifest, SHA256SUMS.txt and
/// the PC application.  A Release without an image is not usable for a card.
/// </summary>
public sealed record ReleaseInfo(string Tag, string Version, string Name, DateTimeOffset? PublishedAt,
                                 bool Draft, bool Prerelease, IReadOnlyList<ReleaseAsset> Assets)
{
    public const string ManifestFileName = "rumicar-zero2cam.rpi-imager-manifest";
    public const string SumsFileName = "SHA256SUMS.txt";

    public ReleaseAsset? Image => Assets.FirstOrDefault(a => a.Name.StartsWith("rumicar-zero2cam-", StringComparison.Ordinal)
                                                             && a.Name.EndsWith(".img.xz", StringComparison.Ordinal));
    public ReleaseAsset? Manifest => Assets.FirstOrDefault(a => a.Name == ManifestFileName);
    public ReleaseAsset? Sums => Assets.FirstOrDefault(a => a.Name == SumsFileName);
    public ReleaseAsset? App => Assets.FirstOrDefault(a => a.Name.StartsWith("RumiCarDriveManager-", StringComparison.Ordinal)
                                                           && a.Name.EndsWith("-win-x64.zip", StringComparison.Ordinal));

    /// <summary>True when Imager can take this Release from its list: image, manifest and checksums are all attached (0.4.1 and later).</summary>
    public bool SupportsImager => Image is not null && Manifest is not null && Sums is not null;

    public string Display
    {
        get
        {
            var date = PublishedAt?.ToLocalTime().ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) ?? "";
            var size = Image is null ? "イメージなし" : $"{Image.Size / (1024.0 * 1024.0):F0} MB";
            var note = SupportsImager ? "" : "（Imager の一覧は非対応。Use custom で書き込み）";
            return $"{Tag}　{date}　{size}{note}";
        }
    }
}

/// <summary>
/// Reads the Releases of the repository from the GitHub REST API response
/// (GET /repos/{owner}/{repo}/releases).  Drafts are skipped; pre-releases
/// are kept but flagged.  Pure parsing, so it can be tested with a captured
/// response; the HTTP part is <see cref="ReleaseDownloader"/>.
/// </summary>
public static class ReleaseCatalog
{
    public const string Owner = "RumiCar-group";
    public const string Repo = "RumiCar-Zero2Cam";
    public const string ApiUrl = "https://api.github.com/repos/" + Owner + "/" + Repo + "/releases?per_page=30";
    public const string ReleasesPageUrl = "https://github.com/" + Owner + "/" + Repo + "/releases";

    /// <summary>The URL that always serves the newest Release's Imager manifest (README 2).</summary>
    public const string LatestManifestUrl = "https://github.com/" + Owner + "/" + Repo + "/releases/latest/download/" + ReleaseInfo.ManifestFileName;

    public static IReadOnlyList<ReleaseInfo> Parse(string json)
    {
        using var doc = JsonDocument.Parse(json);
        if (doc.RootElement.ValueKind != JsonValueKind.Array) throw new InvalidDataException("GitHub の応答が Release の一覧ではありません");
        var list = new List<ReleaseInfo>();
        foreach (var r in doc.RootElement.EnumerateArray())
        {
            var tag = GetString(r, "tag_name");
            if (tag is null) continue;
            if (r.TryGetProperty("draft", out var d) && d.ValueKind == JsonValueKind.True) continue;
            var assets = new List<ReleaseAsset>();
            if (r.TryGetProperty("assets", out var arr) && arr.ValueKind == JsonValueKind.Array)
            {
                foreach (var a in arr.EnumerateArray())
                {
                    var name = GetString(a, "name");
                    var url = GetString(a, "browser_download_url");
                    if (name is null || url is null) continue;
                    var size = a.TryGetProperty("size", out var s) && s.TryGetInt64(out var n) ? n : 0;
                    assets.Add(new ReleaseAsset(name, size, url));
                }
            }
            DateTimeOffset? published = null;
            var pub = GetString(r, "published_at");
            if (pub is not null && DateTimeOffset.TryParse(pub, CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal, out var p)) published = p;
            list.Add(new ReleaseInfo(tag, VersionOfTag(tag), GetString(r, "name") ?? tag, published, false,
                                     r.TryGetProperty("prerelease", out var pre) && pre.ValueKind == JsonValueKind.True, assets));
        }
        // Newest first by version (the API's order is by creation, which is the same for this repository).
        return list.OrderByDescending(x => x, ReleaseVersionComparer.Instance).ToList();
    }

    /// <summary>"v0.4.1" -> "0.4.1"; a tag that is not vX.Y.Z is returned as is.</summary>
    public static string VersionOfTag(string tag) => tag.StartsWith('v') && tag.Length > 1 && char.IsDigit(tag[1]) ? tag[1..] : tag;

    /// <summary>The newest Release that has an SD-card image, or null.</summary>
    public static ReleaseInfo? Latest(IEnumerable<ReleaseInfo> releases) =>
        releases.Where(r => !r.Prerelease && r.Image is not null).OrderByDescending(r => r, ReleaseVersionComparer.Instance).FirstOrDefault();

    /// <summary>
    /// Compares two product versions numerically ("0.4.1" &gt; "0.4.0" &gt; "0.3.2";
    /// build metadata after + is ignored).  Non-numeric parts compare as 0, so
    /// an unexpected string never throws.
    /// </summary>
    public static int CompareVersions(string a, string b)
    {
        var pa = Parts(a);
        var pb = Parts(b);
        for (var i = 0; i < Math.Max(pa.Length, pb.Length); i++)
        {
            var x = i < pa.Length ? pa[i] : 0;
            var y = i < pb.Length ? pb[i] : 0;
            if (x != y) return x.CompareTo(y);
        }
        return 0;
    }

    private static int[] Parts(string version)
    {
        var v = Control.ProductVersion.Release(version);
        return v.Split('.').Select(p => int.TryParse(p, NumberStyles.None, CultureInfo.InvariantCulture, out var n) ? n : 0).ToArray();
    }

    private static string? GetString(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    private sealed class ReleaseVersionComparer : IComparer<ReleaseInfo>
    {
        public static readonly ReleaseVersionComparer Instance = new();
        public int Compare(ReleaseInfo? x, ReleaseInfo? y) => CompareVersions(x?.Version ?? "", y?.Version ?? "");
    }
}
