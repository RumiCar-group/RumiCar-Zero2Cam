using System.Text;
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
        Assert.Contains("\"frames\": 3", session);
        Assert.Contains("\"sequence_gaps\": 1", session);
        Assert.Throws<ArgumentException>(() => new SessionWriter(_tmp, "bad/name", started));
    }
}
