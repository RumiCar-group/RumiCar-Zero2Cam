using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using RumiCar.Protocol.Releases;
using Xunit;

namespace RumiCar.Protocol.Tests;

public class ReleaseCatalogTests
{
    private static string Fixture(string name) => File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Fixtures", name), Encoding.UTF8);

    [Fact]
    public void ParsesTheGitHubResponseNewestFirstWithoutDrafts()
    {
        var releases = ReleaseCatalog.Parse(Fixture("releases.json"));
        Assert.Equal(new[] { "v0.4.2", "v0.4.1", "v0.4.0", "v0.3.2" }, releases.Select(r => r.Tag).ToArray());   // the v0.5.0 draft is gone
        var r = releases.Single(x => x.Tag == "v0.4.1");
        Assert.Equal("0.4.1", r.Version);
        Assert.Equal(new DateTimeOffset(2026, 11, 15, 9, 0, 0, TimeSpan.Zero), r.PublishedAt);
        Assert.Equal("rumicar-zero2cam-0.4.1-20261115.img.xz", r.Image!.Name);
        Assert.Equal(644872028, r.Image.Size);
        Assert.Equal("https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases/download/v0.4.1/rumicar-zero2cam-0.4.1-20261115.img.xz", r.Image.DownloadUrl);
        Assert.Equal("rumicar-zero2cam.rpi-imager-manifest", r.Manifest!.Name);
        Assert.Equal("SHA256SUMS.txt", r.Sums!.Name);
        Assert.Equal("RumiCarDriveManager-v0.4.1-win-x64.zip", r.App!.Name);
        Assert.True(r.SupportsImager);
        Assert.False(r.Prerelease);
    }

    [Fact]
    public void AReleaseWithoutTheManifestIsListedButNotForImager()
    {
        var releases = ReleaseCatalog.Parse(Fixture("releases.json"));
        var r = releases.Single(x => x.Tag == "v0.4.0");
        Assert.NotNull(r.Image);
        Assert.Null(r.Manifest);
        Assert.False(r.SupportsImager);
        Assert.Contains("Use custom", r.Display);
        Assert.StartsWith("v0.4.0", r.Display);
    }

    [Fact]
    public void LatestSkipsPrereleases()
    {
        var releases = ReleaseCatalog.Parse(Fixture("releases.json"));
        Assert.Equal("v0.4.1", ReleaseCatalog.Latest(releases)!.Tag);   // v0.4.2 is a pre-release
    }

    [Theory]
    [InlineData("0.4.1", "0.4.0", 1)]
    [InlineData("0.4.0", "0.4.1", -1)]
    [InlineData("0.4.0", "0.4.0+g1a2b3c4", 0)]
    [InlineData("0.10.0", "0.9.9", 1)]
    [InlineData("1.0.0", "0.99.99", 1)]
    [InlineData("0.4", "0.4.0", 0)]
    [InlineData("abc", "0.0.0", 0)]
    public void VersionsCompareNumerically(string a, string b, int expected)
    {
        Assert.Equal(expected, Math.Sign(ReleaseCatalog.CompareVersions(a, b)));
    }

    [Fact]
    public void VersionOfTagDropsTheV()
    {
        Assert.Equal("0.4.1", ReleaseCatalog.VersionOfTag("v0.4.1"));
        Assert.Equal("nightly", ReleaseCatalog.VersionOfTag("nightly"));
    }

    [Fact]
    public void AnArrayIsRequired()
    {
        Assert.Throws<InvalidDataException>(() => ReleaseCatalog.Parse("{\"message\":\"API rate limit exceeded\"}"));
    }
}

public class Sha256SumsTests
{
    [Fact]
    public void ParsesSha256sumOutput()
    {
        const string text = "0bb4e2c0a2f3c3e4f1a2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708  rumicar-zero2cam-0.4.1-20261115.img.xz\n" +
                            "BE6761AC6129E6CEA65BD603F898DFA4DD087B83453C0C2BFF5C4BE599274BB7 *SHA256SUMS.txt\r\n" +
                            "not a checksum line\n";
        var map = Sha256Sums.Parse(text);
        Assert.Equal(2, map.Count);
        Assert.Equal("0bb4e2c0a2f3c3e4f1a2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708", map["rumicar-zero2cam-0.4.1-20261115.img.xz"]);
        Assert.Equal("be6761ac6129e6cea65bd603f898dfa4dd087b83453c0c2bff5c4be599274bb7", map["SHA256SUMS.txt"]);
    }

    [Fact]
    public async Task DigestOfAFileMatchesTheFramework()
    {
        var path = Path.Combine(Path.GetTempPath(), "rumicar-sha-" + Guid.NewGuid().ToString("N"));
        var bytes = new byte[3 * 1024 * 1024 + 17];
        new Random(1).NextBytes(bytes);
        await File.WriteAllBytesAsync(path, bytes);
        try
        {
            var seen = new List<long>();
            var digest = await Sha256Sums.OfFileAsync(path, new Progress<long>(seen.Add));
            Assert.Equal(Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(), digest);
        }
        finally { File.Delete(path); }
    }
}

public class ImagerManifestTests
{
    private static string Fixture(string name) => File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Fixtures", name), Encoding.UTF8);

    [Fact]
    public void ReadsTheReleaseEntry()
    {
        var e = ImagerManifest.Read(Fixture("rumicar-zero2cam.rpi-imager-manifest"));
        Assert.Equal("RumiCar Zero2Cam 0.4.1", e.Name);
        Assert.EndsWith("/v0.4.1/rumicar-zero2cam-0.4.1-20261101.img.xz", e.Url);
        Assert.Equal(32000000, e.ExtractSize);
        Assert.Equal(2005744, e.DownloadSize);
        Assert.Equal(64, e.ExtractSha256.Length);
        Assert.Equal("fc8558d0f9ebc3ffcef56936e90d63e19e646308ce71a001605a8492b0e98e44", e.DownloadSha256);
        Assert.Equal("none", e.InitFormat);
    }

    [Fact]
    public void LocalizeReplacesOnlyTheUrl()
    {
        var dir = Path.Combine(Path.GetTempPath(), "RumiCar Data テスト");   // a space and non-ASCII must survive as a URI
        var image = Path.Combine(dir, "rumicar-zero2cam-0.4.1-20261101.img.xz");
        var local = ImagerManifest.Localize(Fixture("rumicar-zero2cam.rpi-imager-manifest"), image);
        using var doc = JsonDocument.Parse(local);
        var entry = doc.RootElement.GetProperty("os_list")[0];
        var url = entry.GetProperty("url").GetString()!;
        Assert.StartsWith("file:///", url);
        Assert.EndsWith("/rumicar-zero2cam-0.4.1-20261101.img.xz", url);
        Assert.DoesNotContain(" ", url);
        Assert.Equal(Path.GetFullPath(image), new Uri(url).LocalPath);
        Assert.Equal(32000000, entry.GetProperty("extract_size").GetInt64());
        Assert.Equal("none", entry.GetProperty("init_format").GetString());
        Assert.Equal("Raspberry Pi Zero 2 W", doc.RootElement.GetProperty("imager").GetProperty("devices")[0].GetProperty("name").GetString());
        Assert.Contains("Raspberry Pi OS Lite（64-bit", local);   // not escaped to \uXXXX
    }

    [Fact]
    public void LocalizeRefusesAnotherFileName()
    {
        Assert.Throws<InvalidDataException>(() => ImagerManifest.Localize(Fixture("rumicar-zero2cam.rpi-imager-manifest"), Path.Combine(Path.GetTempPath(), "other.img.xz")));
    }

    [Theory]
    [InlineData("{\"os_list\":[]}")]
    [InlineData("{\"os_list\":[{\"name\":\"x\",\"url\":\"https://a/b.img.xz\",\"extract_size\":1,\"extract_sha256\":\"zz\",\"image_download_size\":1,\"init_format\":\"none\"}]}")]
    [InlineData("{\"os_list\":[{\"name\":\"x\",\"url\":\"https://a/b.img.xz\",\"extract_size\":1,\"extract_sha256\":\"0bb4e2c0a2f3c3e4f1a2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708\",\"image_download_size\":1,\"init_format\":\"systemd\"}]}")]
    [InlineData("[]")]
    [InlineData("not json")]
    public void ReadRejectsWhatImagerWouldMisuse(string json)
    {
        Assert.Throws<InvalidDataException>(() => ImagerManifest.Read(json));
    }
}

public class ReleaseDownloaderTests
{
    /// <summary>A one-shot local HTTP server: serves the given body for any request, with or without Content-Length.</summary>
    private sealed class Server : IDisposable
    {
        private readonly HttpListener _listener = new();
        public string Url { get; }
        public int Status = 200;
        public byte[] Body = Array.Empty<byte>();
        public bool Chunked;
        public int Truncate = -1;   // send only this many bytes (then close) when >= 0

        public Server()
        {
            var port = FreePort();
            Url = $"http://127.0.0.1:{port}/";
            _listener.Prefixes.Add(Url);
            _listener.Start();
            _ = Task.Run(async () =>
            {
                while (_listener.IsListening)
                {
                    HttpListenerContext ctx;
                    try { ctx = await _listener.GetContextAsync(); } catch { return; }
                    try
                    {
                        ctx.Response.StatusCode = Status;
                        var body = Truncate >= 0 ? Body[..Truncate] : Body;
                        if (!Chunked) ctx.Response.ContentLength64 = Body.Length;   // what the server claims
                        else ctx.Response.SendChunked = true;
                        await ctx.Response.OutputStream.WriteAsync(body);
                        if (Truncate >= 0) ctx.Response.Abort(); else ctx.Response.Close();
                    }
                    catch { /* client went away */ }
                }
            });
        }

        private static int FreePort()
        {
            var l = new System.Net.Sockets.TcpListener(IPAddress.Loopback, 0);
            l.Start();
            var port = ((IPEndPoint)l.LocalEndpoint).Port;
            l.Stop();
            return port;
        }

        public void Dispose() { try { _listener.Stop(); _listener.Close(); } catch { } }
    }

    private static ReleaseDownloader Client() => new(new HttpClient { Timeout = Timeout.InfiniteTimeSpan }, "0.0.0-test");

    [Fact]
    public async Task FetchesAndParsesReleases()
    {
        using var server = new Server();
        server.Body = File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "Fixtures", "releases.json"));
        var releases = await Client().FetchReleasesAsync(CancellationToken.None, server.Url + "releases");
        Assert.Equal("v0.4.2", releases[0].Tag);
    }

    [Fact]
    public async Task AFailedStatusIsAnError()
    {
        using var server = new Server { Status = 403, Body = Encoding.UTF8.GetBytes("{\"message\":\"rate limit\"}") };
        var ex = await Assert.ThrowsAsync<HttpRequestException>(() => Client().FetchReleasesAsync(CancellationToken.None, server.Url + "releases"));
        Assert.Contains("403", ex.Message);
    }

    [Fact]
    public async Task DownloadsThroughAPartFileWithProgress()
    {
        using var server = new Server();
        server.Body = new byte[2_500_000];
        new Random(7).NextBytes(server.Body);
        var dest = Path.Combine(Path.GetTempPath(), "rumicar-dl-" + Guid.NewGuid().ToString("N"), "image.img.xz");
        try
        {
            var reports = new List<DownloadProgress>();
            await Client().DownloadFileAsync(server.Url + "image.img.xz", dest, server.Body.Length, new Progress<DownloadProgress>(reports.Add), CancellationToken.None);
            Assert.True(File.Exists(dest));
            Assert.False(File.Exists(dest + ".part"));
            Assert.Equal(server.Body, await File.ReadAllBytesAsync(dest));
            Assert.Equal(await Sha256Sums.OfFileAsync(dest), Convert.ToHexString(SHA256.HashData(server.Body)).ToLowerInvariant());
            // Progress ends at the full size and the total is known from Content-Length.
            await Task.Delay(50);   // Progress<T> posts to the thread pool
            Assert.Contains(reports, r => r.Done == server.Body.Length && r.Total == server.Body.Length);
        }
        finally { Directory.Delete(Path.GetDirectoryName(dest)!, true); }
    }

    [Fact]
    public async Task AShortBodyIsRejectedAndLeavesNoFile()
    {
        using var server = new Server { Chunked = true, Truncate = 1000 };
        server.Body = new byte[200_000];
        var dest = Path.Combine(Path.GetTempPath(), "rumicar-dl-" + Guid.NewGuid().ToString("N"), "image.img.xz");
        try
        {
            await Assert.ThrowsAnyAsync<Exception>(() => Client().DownloadFileAsync(server.Url + "image.img.xz", dest, server.Body.Length, null, CancellationToken.None));
            Assert.False(File.Exists(dest));
            Assert.False(File.Exists(dest + ".part"));
        }
        finally { if (Directory.Exists(Path.GetDirectoryName(dest)!)) Directory.Delete(Path.GetDirectoryName(dest)!, true); }
    }
}
