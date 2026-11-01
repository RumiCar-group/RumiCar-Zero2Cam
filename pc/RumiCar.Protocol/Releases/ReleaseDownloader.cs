using System.Net.Http.Headers;

namespace RumiCar.Protocol.Releases;

/// <summary>Progress of a download: bytes so far and the total when the server said it.</summary>
public sealed record DownloadProgress(long Done, long? Total);

/// <summary>
/// HTTP side of the SD-card feature (docs 6.5): the Release list from the
/// GitHub API, small text assets (the Imager manifest, SHA256SUMS.txt) and the
/// image itself, streamed to a file with progress.  The caller owns the
/// <see cref="HttpClient"/>; its Timeout should be infinite and cancellation
/// used instead, because an image is 600 MB.
/// </summary>
public sealed class ReleaseDownloader
{
    private readonly HttpClient _http;

    public ReleaseDownloader(HttpClient http, string userAgent)
    {
        _http = http;
        _http.DefaultRequestHeaders.UserAgent.Clear();
        _http.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("RumiCarDriveManager", userAgent));
        _http.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("(+https://github.com/RumiCar-group/RumiCar-Zero2Cam)"));
    }

    /// <summary>The Releases of the repository, newest first.</summary>
    public async Task<IReadOnlyList<ReleaseInfo>> FetchReleasesAsync(CancellationToken ct, string url = ReleaseCatalog.ApiUrl)
    {
        using var req = new HttpRequestMessage(HttpMethod.Get, url);
        req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        req.Headers.Add("X-GitHub-Api-Version", "2022-11-28");
        using var res = await _http.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
        if (!res.IsSuccessStatusCode)
            throw new HttpRequestException($"GitHub から Release の一覧を取得できません（HTTP {(int)res.StatusCode}）");
        var json = await res.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
        return ReleaseCatalog.Parse(json);
    }

    /// <summary>A small text asset (manifest, checksums).  Refuses anything over 1 MiB.</summary>
    public async Task<string> FetchTextAsync(string url, CancellationToken ct)
    {
        using var res = await _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
        if (!res.IsSuccessStatusCode) throw new HttpRequestException($"{Path.GetFileName(url)} を取得できません（HTTP {(int)res.StatusCode}）");
        if (res.Content.Headers.ContentLength is > 1024 * 1024) throw new InvalidDataException($"{Path.GetFileName(url)} が大きすぎます");
        var bytes = await res.Content.ReadAsByteArrayAsync(ct).ConfigureAwait(false);
        if (bytes.Length > 1024 * 1024) throw new InvalidDataException($"{Path.GetFileName(url)} が大きすぎます");
        return System.Text.Encoding.UTF8.GetString(bytes);
    }

    /// <summary>
    /// Downloads <paramref name="url"/> to <paramref name="destination"/> through
    /// a ".part" file that is renamed only when the whole body arrived (and
    /// matched <paramref name="expectedSize"/> when given), so an interrupted
    /// download never looks like a finished one.
    /// </summary>
    public async Task DownloadFileAsync(string url, string destination, long? expectedSize, IProgress<DownloadProgress>? progress, CancellationToken ct)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(destination))!);
        var part = destination + ".part";
        try
        {
            using var res = await _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
            if (!res.IsSuccessStatusCode) throw new HttpRequestException($"{Path.GetFileName(destination)} をダウンロードできません（HTTP {(int)res.StatusCode}）");
            var total = res.Content.Headers.ContentLength ?? expectedSize;
            long done = 0;
            await using (var body = await res.Content.ReadAsStreamAsync(ct).ConfigureAwait(false))
            await using (var file = new FileStream(part, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20, FileOptions.Asynchronous))
            {
                var buf = new byte[1 << 20];
                int n;
                while ((n = await body.ReadAsync(buf, ct).ConfigureAwait(false)) > 0)
                {
                    await file.WriteAsync(buf.AsMemory(0, n), ct).ConfigureAwait(false);
                    done += n;
                    progress?.Report(new DownloadProgress(done, total));
                }
                await file.FlushAsync(ct).ConfigureAwait(false);
            }
            if (expectedSize is not null && done != expectedSize)
                throw new InvalidDataException($"{Path.GetFileName(destination)} のサイズが違います（{done} バイト、{expectedSize} バイトのはず）。通信が途中で切れた可能性があります");
            File.Move(part, destination, overwrite: true);
        }
        catch
        {
            try { File.Delete(part); } catch { /* the .part file is only a leftover */ }
            throw;
        }
    }
}
