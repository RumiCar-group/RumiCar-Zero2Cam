using RumiCar.Protocol.Control;
using System.Text;
using System.Text.Json;
using RumiCar.Protocol.Packs;
using RumiCar.Protocol.Recording;
using RumiCar.Protocol.Telemetry;
using Xunit;

namespace RumiCar.Protocol.Tests;

public class ManifestAndRecordingTests : IDisposable
{
    private readonly string _tmp = Path.Combine(Path.GetTempPath(), "rumicar-tests-" + Guid.NewGuid().ToString("N"));

    public ManifestAndRecordingTests() => Directory.CreateDirectory(_tmp);
    public void Dispose() => Directory.Delete(_tmp, recursive: true);

    private string MakePack(string content, string? overrideSha = null, long? overrideBytes = null)
    {
        var dir = Path.Combine(_tmp, "pack-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var bytes = Encoding.UTF8.GetBytes(content);
        File.WriteAllBytes(Path.Combine(dir, "params.json"), bytes);
        var m = new Manifest
        {
            Schema = 1,
            Kind = "pack",
            Id = "base-tof",
            Version = "0.1.0",
            Logic = new ManifestLogicRef { Name = "tof_drive", MinAbi = 1 },
            Files = { new ManifestFile { Path = "params.json", Sha256 = overrideSha ?? ManifestVerifier.Sha256Hex(bytes), Bytes = overrideBytes ?? bytes.Length } },
        };
        File.WriteAllText(Path.Combine(dir, "manifest.json"), m.ToJson());
        return dir;
    }

    [Fact]
    public void Sha256KnownVector()
    {
        Assert.Equal("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", ManifestVerifier.Sha256Hex("abc"u8));
    }

    [Fact]
    public void ConsistentPackVerifies()
    {
        var dir = MakePack("{\"a\":1}\n");
        var (m, problems) = ManifestVerifier.Verify(dir);
        Assert.NotNull(m);
        Assert.Empty(problems);
        Assert.Equal("base-tof-0.1.0", m!.DirectoryName);
    }

    [Fact]
    public void TamperedFileIsReported()
    {
        var dir = MakePack("{\"a\":1}\n");
        File.AppendAllText(Path.Combine(dir, "params.json"), " ");
        var (_, problems) = ManifestVerifier.Verify(dir);
        Assert.Contains(problems, p => p.Contains("size mismatch"));
        Assert.Contains(problems, p => p.Contains("sha256 mismatch"));
    }

    [Fact]
    public void BadDigestAndMissingFileAreReported()
    {
        var dir = MakePack("{}", overrideSha: new string('0', 64));
        var (_, problems) = ManifestVerifier.Verify(dir);
        Assert.Single(problems, p => p.Contains("sha256 mismatch"));

        File.Delete(Path.Combine(dir, "params.json"));
        (_, problems) = ManifestVerifier.Verify(dir);
        Assert.Contains(problems, p => p.Contains("missing file"));
    }

    [Theory]
    [InlineData("../x", false)]
    [InlineData("/etc/passwd", false)]
    [InlineData("C:\\x", false)]
    [InlineData("manifest.json", false)]
    [InlineData("a/./b", false)]
    [InlineData("params.json", true)]
    [InlineData("sub/dir/file.bin", true)]
    public void SafeRelativePaths(string path, bool expected) => Assert.Equal(expected, Manifest.IsSafeRelativePath(path));

    [Fact]
    public void StructuralProblemsAreDetected()
    {
        Assert.Throws<FormatException>(() => Manifest.Parse("{\"schema\":2,\"kind\":\"pack\",\"id\":\"x\",\"version\":\"1\",\"logic\":{\"name\":\"l\",\"min_abi\":1},\"files\":[{\"path\":\"p\",\"sha256\":\"" + new string('a', 64) + "\",\"bytes\":1}]}"));
        Assert.Throws<FormatException>(() => Manifest.Parse("{\"schema\":1,\"kind\":\"logic\",\"name\":\"l\",\"version\":\"1\",\"abi\":1,\"entry\":\"lib.so\",\"files\":[{\"path\":\"other\",\"sha256\":\"" + new string('a', 64) + "\",\"bytes\":1}]}"));
        var ok = Manifest.Parse("{\"schema\":1,\"kind\":\"logic\",\"name\":\"tof_drive\",\"version\":\"0.1.0\",\"abi\":1,\"entry\":\"libtof_drive.so\",\"files\":[{\"path\":\"libtof_drive.so\",\"sha256\":\"" + new string('a', 64) + "\",\"bytes\":1}]}");
        Assert.True(ok.IsLogic);
        Assert.Equal("tof_drive-0.1.0", ok.DirectoryName);
    }

    [Fact]
    public void SessionWriterProducesDataFolderLayout()
    {
        var started = new DateTimeOffset(2026, 10, 18, 12, 34, 56, TimeSpan.Zero);
        string dir;
        using (var w = new SessionWriter(_tmp, "RumiCar", started))
        {
            dir = w.Directory;
            w.Write(new TelemetryFrame { Header = new TelemetryHeader { Seq = 10, TofMm = new[] { 1, 2, 3 }, ImageWidth = 320, ImageHeight = 240 }, Jpeg = new byte[] { 0xFF, 0xD8, 0xFF, 0xD9 } });
            w.Write(new TelemetryFrame { Header = new TelemetryHeader { Seq = 11, TofMm = new[] { 1, 2, 3 } } });
            w.Write(new TelemetryFrame { Header = new TelemetryHeader { Seq = 13, TofMm = new[] { 1, 2, 3 } } }); // gap
            Assert.Equal(3, w.FramesWritten);
            Assert.Equal(1, w.SequenceGaps);
        }
        Assert.EndsWith("RumiCar_20261018-123456", dir);
        Assert.True(File.Exists(Path.Combine(dir, "frames", "000001.jpg")));
        Assert.False(File.Exists(Path.Combine(dir, "frames", "000002.jpg")));
        var meta = File.ReadAllLines(Path.Combine(dir, "meta.jsonl"));
        Assert.Equal(3, meta.Length);
        Assert.Contains("\"image\":\"000001.jpg\"", meta[0]);
        Assert.Contains("\"image\":null", meta[1]);
        var session = File.ReadAllText(Path.Combine(dir, "session.json"));
        Assert.Contains("\"schema\": 2", session);
        Assert.Contains("\"frames\": 3", session);
        Assert.Contains("\"images\": 1", session);
        Assert.Contains("\"sequence_gaps\": 1", session);
        Assert.Contains("\"scenario\": \"\"", session);
        Assert.Throws<ArgumentException>(() => new SessionWriter(_tmp, "bad/name", started));
    }

    [Fact]
    public void SessionJsonCarriesScenarioAndCarLineage()
    {
        var status = JsonDocument.Parse("{\"mode\":\"record_only\",\"logic_name\":\"tof_drive\",\"logic_version\":\"0.1.0\",\"pack_id\":\"base-tof\",\"pack_version\":\"0.1.1\","
                                        + "\"pack_description\":\"壁のみ\",\"pack_capabilities\":[\"walls\"],\"pack_scenarios\":[],\"pack_parent\":\"\",\"throttled\":0}").RootElement;
        var info = SessionInfo.FromStatus(status, " 信号機ありケース1 ", "メモ", "RumiCar", "0.1.3", "RumiCarDriveManager 0.1.3");
        Assert.Equal("信号機ありケース1", info.Scenario);
        Assert.Equal("walls", Assert.Single(info.PackCapabilities));
        Assert.Equal("record_only", info.Mode);
        var started = new DateTimeOffset(2026, 10, 21, 1, 2, 3, TimeSpan.Zero);
        string dir;
        using (var w = new SessionWriter(_tmp, "RumiCar", started, info))
        {
            dir = w.Directory;
            w.Write(new TelemetryFrame { Header = new TelemetryHeader { Seq = 1, TofMm = new[] { 1, 2, 3 }, Throttled = 0x10001 } });
            w.Write(new TelemetryFrame { Header = new TelemetryHeader { Seq = 2, TofMm = new[] { 1, 2, 3 } } });
        }
        using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "session.json")));
        var root = doc.RootElement;
        Assert.Equal(2, root.GetProperty("schema").GetInt32());
        Assert.Equal("信号機ありケース1", root.GetProperty("scenario").GetString());
        Assert.Equal("メモ", root.GetProperty("note").GetString());
        Assert.Equal("record_only", root.GetProperty("mode_at_start").GetString());
        Assert.Equal("base-tof", root.GetProperty("pack").GetProperty("id").GetString());
        Assert.Equal("壁のみ", root.GetProperty("pack").GetProperty("description").GetString());
        Assert.Equal("walls", root.GetProperty("pack").GetProperty("capabilities")[0].GetString());
        Assert.Equal(0x10001u, root.GetProperty("throttled_seen").GetUInt32());
        Assert.Equal(2, root.GetProperty("frames").GetInt32());
        Assert.Equal(0, root.GetProperty("images").GetInt32());
        // The scenario name is stored as text, not escaped.
        Assert.Contains("信号機ありケース1", File.ReadAllText(Path.Combine(dir, "session.json")));
        // Missing status keys stay empty rather than failing.
        var bare = SessionInfo.FromStatus(null, "", "", "", "", "");
        Assert.Equal("", bare.PackId);
        Assert.Empty(bare.PackCapabilities);
    }

    [Fact]
    public void ScenarioHistoryKeepsNewestFirstWithCounts()
    {
        var root = Path.Combine(_tmp, "data");
        Assert.Empty(ScenarioHistory.Load(root));
        var t = new DateTimeOffset(2026, 10, 21, 0, 0, 0, TimeSpan.Zero);
        ScenarioHistory.RecordUse(root, "壁のみの走行データ", t);
        ScenarioHistory.RecordUse(root, "信号機ありケース1", t.AddMinutes(1));
        ScenarioHistory.RecordUse(root, " 壁のみの走行データ ", t.AddMinutes(2));
        ScenarioHistory.RecordUse(root, "   ", t.AddMinutes(3));   // ignored
        var list = ScenarioHistory.Load(root);
        Assert.Equal(2, list.Count);
        Assert.Equal("壁のみの走行データ", list[0].Name);
        Assert.Equal(2, list[0].Sessions);
        Assert.Equal("信号機ありケース1", list[1].Name);
        Assert.Equal(1, list[1].Sessions);
        Assert.True(File.Exists(Path.Combine(root, ScenarioHistory.FileName)));
        // An unreadable file is treated as empty, not as an error.
        File.WriteAllText(Path.Combine(root, ScenarioHistory.FileName), "{broken");
        Assert.Empty(ScenarioHistory.Load(root));
    }

    [Fact]
    public void TelemetryHeaderRoundTripsNewFields()
    {
        var header = new TelemetryHeader { Seq = 5, TofMm = new[] { 1, 2, 3 }, LogicName = "tof_drive", PackId = "base-tof", Throttled = 0x50001 };
        var back = TelemetryFrameCodec.HeaderFromJson(TelemetryFrameCodec.HeaderToJson(header));
        Assert.NotNull(back);
        Assert.Equal("tof_drive", back!.LogicName);
        Assert.Equal("base-tof", back.PackId);
        Assert.Equal(0x50001u, back.Throttled);
        // A header from an older car (without the new keys) still decodes.
        var old = TelemetryFrameCodec.HeaderFromJson("{\"seq\":1,\"tof_mm\":[1,2,3]}");
        Assert.NotNull(old);
        Assert.Equal("", old!.PackId);
        Assert.Equal(0u, old.Throttled);
    }
}

public class VersionPushTests : IDisposable
{
    private readonly string _tmp = Path.Combine(Path.GetTempPath(), "rumicar-push-" + Guid.NewGuid().ToString("N"));

    public VersionPushTests() => Directory.CreateDirectory(_tmp);
    public void Dispose() => Directory.Delete(_tmp, recursive: true);

    private string MakePack(string version, int extraBytes)
    {
        var dir = Path.Combine(_tmp, "pack-" + version);
        Directory.CreateDirectory(Path.Combine(dir, "sub"));
        var p = Encoding.UTF8.GetBytes("{\"a\":1}\n");
        File.WriteAllBytes(Path.Combine(dir, "params.json"), p);
        var big = new byte[extraBytes];
        for (int i = 0; i < big.Length; i++) big[i] = (byte)(i * 31);
        File.WriteAllBytes(Path.Combine(dir, "sub", "data.bin"), big);
        var m = new Manifest
        {
            Schema = 1, Kind = "pack", Id = "base-tof", Version = version,
            Logic = new ManifestLogicRef { Name = "tof_drive", MinAbi = 1 },
            Files =
            {
                new ManifestFile { Path = "params.json", Sha256 = ManifestVerifier.Sha256Hex(p), Bytes = p.Length },
                new ManifestFile { Path = "sub/data.bin", Sha256 = ManifestVerifier.Sha256Hex(big), Bytes = big.Length },
            },
        };
        File.WriteAllText(Path.Combine(dir, "manifest.json"), m.ToJson());
        return dir;
    }

    /// <summary>A car that accepts uploads and reassembles the files, following the same rules as rumicar-core.</summary>
    private sealed class FakeCar
    {
        public readonly List<string> Types = new();
        public readonly Dictionary<string, MemoryStream> Files = new();
        public Dictionary<string, long> Declared = new();
        public string? FailAt;
        public bool Ended;

        public Task<ControlResponse> Send(ControlRequest req, CancellationToken ct)
        {
            Types.Add(req.Type);
            if (req.Type == FailAt) return Task.FromResult(new ControlResponse { Id = req.Id, Ok = false, Error = "simulated" });
            var p = req.Params!;
            switch (req.Type)
            {
                case RequestType.UploadBegin:
                    Declared = p["files"].EnumerateArray().ToDictionary(f => f.GetProperty("path").GetString()!, f => f.GetProperty("bytes").GetInt64());
                    foreach (var k in Declared.Keys) Files[k] = new MemoryStream();
                    break;
                case RequestType.UploadChunk:
                {
                    var path = p["path"].GetString()!;
                    var offset = p["offset"].GetInt64();
                    var data = Convert.FromBase64String(p["data"].GetString()!);
                    if (!Files.TryGetValue(path, out var ms)) return Task.FromResult(new ControlResponse { Id = req.Id, Ok = false, Error = "undeclared" });
                    if (ms.Length != offset) return Task.FromResult(new ControlResponse { Id = req.Id, Ok = false, Error = "offset" });
                    if (data.Length > ControlCodec.UploadChunkBytes) return Task.FromResult(new ControlResponse { Id = req.Id, Ok = false, Error = "chunk too large" });
                    ms.Write(data);
                    break;
                }
                case RequestType.UploadEnd:
                    foreach (var (k, n) in Declared)
                        if (Files[k].Length != n) return Task.FromResult(new ControlResponse { Id = req.Id, Ok = false, Error = "incomplete " + k });
                    Ended = true;
                    break;
            }
            return Task.FromResult(new ControlResponse { Id = req.Id, Ok = true });
        }
    }

    [Fact]
    public void PrepareListsEveryFileIncludingTheManifest()
    {
        var dir = MakePack("0.2.0", 10);
        var plan = VersionPush.Prepare(dir);
        Assert.Equal("pack", plan.Kind);
        Assert.Equal("base-tof-0.2.0", plan.DirectoryName);
        Assert.Equal(3, plan.Files.Count);
        Assert.Contains(plan.Files, f => f.Path == "manifest.json");
        Assert.Contains(plan.Files, f => f.Path == "sub/data.bin" && f.Bytes == 10);
        Assert.Equal(plan.Files.Sum(f => f.Bytes), plan.TotalBytes);

        File.AppendAllText(Path.Combine(dir, "params.json"), " ");
        Assert.Throws<InvalidDataException>(() => VersionPush.Prepare(dir));
    }

    [Fact]
    public async Task PushSendsEveryByteInOrderedChunks()
    {
        var dir = MakePack("0.3.0", ControlCodec.UploadChunkBytes * 2 + 1234);
        var plan = VersionPush.Prepare(dir);
        var car = new FakeCar();
        long id = 0;
        var reported = new List<long>();
        var progress = new SynchronousProgress(reported.Add);
        await VersionPush.PushAsync(dir, plan, car.Send, () => ++id, progress, CancellationToken.None);
        Assert.True(car.Ended);
        Assert.Equal(RequestType.UploadBegin, car.Types[0]);
        Assert.Equal(RequestType.UploadEnd, car.Types[^1]);
        Assert.DoesNotContain(RequestType.UploadAbort, car.Types);
        foreach (var f in plan.Files)
            Assert.Equal(File.ReadAllBytes(Path.Combine(dir, f.Path.Replace('/', Path.DirectorySeparatorChar))), car.Files[f.Path].ToArray());
        Assert.Equal(plan.TotalBytes, reported[^1]);
        Assert.True(reported.Count >= 4);   // the big file alone needs three chunks

        // A refusal mid-way aborts the upload.
        var car2 = new FakeCar { FailAt = RequestType.UploadChunk };
        await Assert.ThrowsAsync<IOException>(() => VersionPush.PushAsync(dir, plan, car2.Send, () => ++id, null, CancellationToken.None));
        Assert.Equal(RequestType.UploadAbort, car2.Types[^1]);
        Assert.False(car2.Ended);
    }

    private sealed class SynchronousProgress : IProgress<long>
    {
        private readonly Action<long> _handler;
        public SynchronousProgress(Action<long> handler) => _handler = handler;
        public void Report(long value) => _handler(value);
    }
}
