using System.Text.Json;
using System.Text.Json.Nodes;

namespace RumiCar.Protocol.Releases;

/// <summary>
/// The Raspberry Pi Imager repository manifest of a Release
/// (rumicar-zero2cam.rpi-imager-manifest, written by car/image/build-image.sh;
/// docs 7.5) and the local copy the drive manager hands to Imager: the same
/// entry with its url pointing at the image already downloaded to this PC, so
/// Imager lists it, skips the download and verifies the written card against
/// the entry's extract_sha256.
/// </summary>
public static class ImagerManifest
{
    public const string Extension = ".rpi-imager-manifest";

    /// <summary>The single image entry of a Release manifest: what is listed and the digests Imager checks.</summary>
    public sealed record Entry(string Name, string Url, long ExtractSize, string ExtractSha256, long DownloadSize, string? DownloadSha256, string InitFormat);

    /// <summary>Reads the entry of the Release manifest, checking the fields the local manifest will rely on.</summary>
    public static Entry Read(string json)
    {
        var root = ParseObject(json);
        var list = root["os_list"] as JsonArray ?? throw new InvalidDataException("Imager の一覧（os_list）がありません");
        if (list.Count != 1) throw new InvalidDataException($"Imager の一覧に {list.Count} 件あります（1 件のはず）");
        var e = list[0] as JsonObject ?? throw new InvalidDataException("Imager の一覧の項目が不正です");
        var name = Str(e, "name");
        var url = Str(e, "url");
        var extractSize = Long(e, "extract_size");
        var extractSha = Str(e, "extract_sha256");
        var downloadSize = Long(e, "image_download_size");
        var downloadSha = e["image_download_sha256"]?.GetValue<string>();
        var init = e["init_format"]?.GetValue<string>() ?? "";
        if (!Sha256Sums.IsHex(extractSha)) throw new InvalidDataException("extract_sha256 が SHA-256 ではありません");
        if (downloadSha is not null && !Sha256Sums.IsHex(downloadSha)) throw new InvalidDataException("image_download_sha256 が SHA-256 ではありません");
        if (init != "none") throw new InvalidDataException($"init_format が {init} です（設定済みのイメージなので none のはず）");
        return new Entry(name, url, extractSize, extractSha.ToLowerInvariant(), downloadSize, downloadSha?.ToLowerInvariant(), init);
    }

    /// <summary>
    /// The manifest to give Imager on this PC: the Release manifest with the
    /// entry's url replaced by the file URI of <paramref name="localImagePath"/>
    /// (Imager reads file: URLs as local images).  Everything else — the
    /// device, the digests, init_format — is kept as the Release published it.
    /// </summary>
    public static string Localize(string releaseManifestJson, string localImagePath)
    {
        var root = ParseObject(releaseManifestJson);
        var list = root["os_list"] as JsonArray ?? throw new InvalidDataException("Imager の一覧（os_list）がありません");
        if (list.Count != 1) throw new InvalidDataException($"Imager の一覧に {list.Count} 件あります（1 件のはず）");
        var e = list[0] as JsonObject ?? throw new InvalidDataException("Imager の一覧の項目が不正です");
        var expected = Path.GetFileName(Str(e, "url").Split('?')[0]);
        if (!string.Equals(expected, Path.GetFileName(localImagePath), StringComparison.Ordinal))
            throw new InvalidDataException($"一覧のイメージ名 {expected} とファイル名 {Path.GetFileName(localImagePath)} が一致しません");
        e["url"] = FileUri(localImagePath);
        return root.ToJsonString(new JsonSerializerOptions { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }) + "\n";
    }

    /// <summary>"C:\Users\me\RumiCarData\images\x.img.xz" -> "file:///C:/Users/me/RumiCarData/images/x.img.xz" (percent-encoded as a URI).</summary>
    public static string FileUri(string path) => new Uri(Path.GetFullPath(path)).AbsoluteUri;

    private static JsonObject ParseObject(string json)
    {
        try
        {
            return JsonNode.Parse(json, documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Disallow }) as JsonObject
                   ?? throw new InvalidDataException("Imager の一覧が JSON オブジェクトではありません");
        }
        catch (JsonException ex)
        {
            throw new InvalidDataException("Imager の一覧が JSON として解析できません: " + ex.Message);
        }
    }

    private static string Str(JsonObject o, string key) =>
        o[key] is JsonValue v && v.TryGetValue<string>(out var s) && s.Length > 0 ? s : throw new InvalidDataException($"Imager の一覧に {key} がありません");

    private static long Long(JsonObject o, string key) =>
        o[key] is JsonValue v && v.TryGetValue<long>(out var n) && n > 0 ? n : throw new InvalidDataException($"Imager の一覧の {key} が不正です");
}
