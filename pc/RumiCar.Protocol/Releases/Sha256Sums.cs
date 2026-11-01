using System.Security.Cryptography;

namespace RumiCar.Protocol.Releases;

/// <summary>
/// SHA256SUMS.txt of a Release (one "hash  file name" line per file, the
/// output of sha256sum) and the digest of a local file, for checking a
/// download before it is written to a card.
/// </summary>
public static class Sha256Sums
{
    /// <summary>File name -> lowercase hex digest.  Lines that are not "hash  name" are ignored.</summary>
    public static IReadOnlyDictionary<string, string> Parse(string text)
    {
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.TrimEnd('\r').Trim();
            if (line.Length < 66) continue;
            var hash = line[..64];
            if (!IsHex(hash)) continue;
            var rest = line[64..].TrimStart();
            if (rest.StartsWith('*')) rest = rest[1..];   // sha256sum's binary-mode marker
            if (rest.Length == 0) continue;
            map[rest] = hash.ToLowerInvariant();
        }
        return map;
    }

    public static bool IsHex(string s) => s.Length == 64 && s.All(c => (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));

    /// <summary>Lowercase hex SHA-256 of a file, reading it in blocks so a 600 MB image does not load into memory.</summary>
    public static async Task<string> OfFileAsync(string path, IProgress<long>? progress = null, CancellationToken ct = default)
    {
        using var sha = SHA256.Create();
        await using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.Asynchronous | FileOptions.SequentialScan);
        var buf = new byte[1 << 20];
        long done = 0;
        int n;
        while ((n = await f.ReadAsync(buf, ct).ConfigureAwait(false)) > 0)
        {
            sha.TransformBlock(buf, 0, n, null, 0);
            done += n;
            progress?.Report(done);
        }
        sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
        return Convert.ToHexString(sha.Hash!).ToLowerInvariant();
    }
}
