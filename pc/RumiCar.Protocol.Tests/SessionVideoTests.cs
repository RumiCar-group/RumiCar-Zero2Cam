using System.Buffers.Binary;
using System.Text;
using RumiCar.Protocol.Recording;
using RumiCar.Protocol.Telemetry;
using Xunit;

namespace RumiCar.Protocol.Tests;

public class SessionVideoTests : IDisposable
{
    private readonly string _tmp = Path.Combine(Path.GetTempPath(), "rumicar-video-" + Guid.NewGuid().ToString("N"));

    public SessionVideoTests() => Directory.CreateDirectory(_tmp);
    public void Dispose() => Directory.Delete(_tmp, recursive: true);

    // A minimal but well-formed JPEG is not needed: the writer stores the bytes
    // as they are, so distinct byte patterns are enough to check placement.
    private static byte[] Frame(int n) => Encoding.ASCII.GetBytes($"JPEG-{n:D4}-" + new string('x', n % 7));

    /// <summary>Writes a session with the given image timestamps (µs); frame i's file is {i+1:D6}.jpg.</summary>
    private string MakeSession(IReadOnlyList<ulong> stamps, IReadOnlySet<int>? withoutImage = null, IReadOnlySet<int>? missingFile = null)
    {
        var dir = Path.Combine(_tmp, "RumiCar_20261021-000000");
        Directory.CreateDirectory(Path.Combine(dir, "frames"));
        var lines = new List<string>();
        for (var i = 0; i < stamps.Count; i++)
        {
            var hasImage = withoutImage is null || !withoutImage.Contains(i);
            var name = hasImage ? $"{i + 1:D6}.jpg" : null;
            if (hasImage && (missingFile is null || !missingFile.Contains(i)))
                File.WriteAllBytes(Path.Combine(dir, "frames", name!), Frame(i));
            var header = new TelemetryHeader
            {
                Seq = (uint)(i + 1), TimestampMicroseconds = stamps[i] + 7, TofMm = new[] { 1, 2, 3 },
                ImageWidth = hasImage ? 320 : 0, ImageHeight = hasImage ? 240 : 0, ImageSeq = (uint)(i + 1), ImageTimestampMicroseconds = hasImage ? stamps[i] : 0,
            };
            lines.Add(SessionMeta.Serialize(new SessionMetaLine { Index = i + 1, Image = name, Received = "2026-10-21T00:00:00Z", Header = header }));
        }
        File.WriteAllText(Path.Combine(dir, "meta.jsonl"), string.Join("\n", lines) + "\n");
        return dir;
    }

    private static List<byte[]> ReadAviFrames(string path, out int width, out int height, out int fps, out int indexedFrames)
    {
        var data = File.ReadAllBytes(path);
        Assert.Equal("RIFF", Encoding.ASCII.GetString(data, 0, 4));
        Assert.Equal((uint)(data.Length - 8), BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(4)));
        Assert.Equal("AVI ", Encoding.ASCII.GetString(data, 8, 4));
        Assert.Equal("avih", Encoding.ASCII.GetString(data, 24, 4));
        width = BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(32 + 32));
        height = BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(32 + 36));
        Assert.Equal("strh", Encoding.ASCII.GetString(data, 100, 4));
        Assert.Equal("vids", Encoding.ASCII.GetString(data, 108, 4));
        Assert.Equal("MJPG", Encoding.ASCII.GetString(data, 112, 4));
        var scale = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(128));
        var rate = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(132));
        fps = (int)(rate / scale);
        var totalFrames = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(48));
        Assert.Equal(totalFrames, BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(140)));   // strh.dwLength
        Assert.Equal("strf", Encoding.ASCII.GetString(data, 164, 4));
        Assert.Equal("MJPG", Encoding.ASCII.GetString(data, 172 + 16, 4));
        // movi list
        var moviList = 164 + 8 + 40;
        Assert.Equal("LIST", Encoding.ASCII.GetString(data, moviList, 4));
        var moviSize = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(moviList + 4));
        var movi = moviList + 8;
        Assert.Equal("movi", Encoding.ASCII.GetString(data, movi, 4));
        var idx1 = movi + (int)moviSize;
        Assert.Equal("idx1", Encoding.ASCII.GetString(data, idx1, 4));
        var idxBytes = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(idx1 + 4));
        indexedFrames = (int)(idxBytes / 16);
        Assert.Equal(totalFrames, (uint)indexedFrames);
        Assert.Equal(data.Length, idx1 + 8 + (int)idxBytes);
        var frames = new List<byte[]>();
        for (var i = 0; i < indexedFrames; i++)
        {
            var e = idx1 + 8 + i * 16;
            Assert.Equal("00dc", Encoding.ASCII.GetString(data, e, 4));
            Assert.Equal(0x10u, BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(e + 4)));
            var offset = (int)BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(e + 8));
            var size = (int)BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(e + 12));
            var chunk = movi + offset;
            Assert.Equal("00dc", Encoding.ASCII.GetString(data, chunk, 4));
            Assert.Equal((uint)size, BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(chunk + 4)));
            frames.Add(data.AsSpan(chunk + 8, size).ToArray());
        }
        return frames;
    }

    [Fact]
    public void WriterProducesConsistentAvi()
    {
        var path = Path.Combine(_tmp, "t.avi");
        using (var w = new MjpegAviWriter(path, 320, 240, 20))
        {
            w.WriteFrame(Frame(1));   // odd length: padded
            w.WriteFrame(Frame(2));
            Assert.Equal(2, w.Frames);
        }
        var frames = ReadAviFrames(path, out var width, out var height, out var fps, out var indexed);
        Assert.Equal(320, width);
        Assert.Equal(240, height);
        Assert.Equal(20, fps);
        Assert.Equal(2, indexed);
        Assert.Equal(Frame(1), frames[0]);
        Assert.Equal(Frame(2), frames[1]);
        Assert.Throws<ArgumentOutOfRangeException>(() => new MjpegAviWriter(Path.Combine(_tmp, "bad.avi"), 0, 240, 20));
        Assert.Throws<ArgumentOutOfRangeException>(() => new MjpegAviWriter(Path.Combine(_tmp, "bad.avi"), 320, 240, 0));
    }

    [Fact]
    public void FramesAreTimedHeldAndCut()
    {
        // 20 fps timeline: frames every 50 ms, one 10 ms after the previous
        // (same slot: left out), a 150 ms gap (two held frames), and a 5 s
        // pause (held for at most 1 s, then cut).
        var stamps = new List<ulong> { 0, 50_000, 100_000, 110_000, 150_000, 300_000, 5_300_000, 5_350_000 };
        var dir = MakeSession(stamps);
        var r = SessionVideo.Build(dir, new SessionVideo.Options(Fps: 20, MaxHoldSeconds: 1.0));
        Assert.Single(r.Files);
        Assert.EndsWith("video.avi", r.Files[0]);
        Assert.Equal(7, r.FramesUsed);
        Assert.Equal(1, r.FramesSkipped);
        Assert.Equal(0, r.FramesMissingFile);
        Assert.Equal(2 + 20, r.FramesRepeated);
        Assert.Equal(320, r.Width);
        Assert.Equal((7 + 22) / 20.0, r.DurationSeconds, 6);
        var frames = ReadAviFrames(r.Files[0], out _, out _, out _, out var indexed);
        Assert.Equal(29, indexed);
        Assert.Equal(Frame(0), frames[0]);
        Assert.Equal(Frame(1), frames[1]);
        Assert.Equal(Frame(2), frames[2]);
        Assert.Equal(Frame(4), frames[3]);        // frame 3 shared frame 2's slot
        Assert.Equal(Frame(4), frames[4]);        // held
        Assert.Equal(Frame(4), frames[5]);        // held
        Assert.Equal(Frame(5), frames[6]);        // 300 ms
        for (var i = 7; i < 27; i++) Assert.Equal(Frame(5), frames[i]);   // 1 s hold, then cut
        Assert.Equal(Frame(6), frames[27]);
        Assert.Equal(Frame(7), frames[28]);       // 50 ms after frame 6: the timeline continues
    }

    [Fact]
    public void FramesWithoutImageOrFileAreSkippedAndEmptySessionIsRejected()
    {
        var stamps = new List<ulong> { 0, 50_000, 100_000, 150_000 };
        var dir = MakeSession(stamps, withoutImage: new HashSet<int> { 1 }, missingFile: new HashSet<int> { 2 });
        var r = SessionVideo.Build(dir);
        Assert.Equal(2, r.FramesUsed);
        Assert.Equal(1, r.FramesMissingFile);
        Assert.Equal(2, r.FramesRepeated);   // frames 1 and 2 left a 2-slot hole filled with frame 0
        var frames = ReadAviFrames(r.Files[0], out _, out _, out _, out _);
        Assert.Equal(new[] { Frame(0), Frame(0), Frame(0), Frame(3) }, frames);

        var empty = MakeSession(new List<ulong> { 0, 50_000 }, withoutImage: new HashSet<int> { 0, 1 });
        Assert.Throws<InvalidDataException>(() => SessionVideo.Build(empty));
        Assert.Throws<FileNotFoundException>(() => SessionVideo.Build(_tmp));
    }

    [Fact]
    public void ControlLoopClockIsUsedWhenSensorTimeIsMissing()
    {
        var dir = MakeSession(new List<ulong> { 0, 50_000, 100_000 });
        // Rewrite meta.jsonl without image_t_us: the control loop's t_us (stamp + 7) must give the same slots.
        var lines = SessionMeta.Read(dir, out _);
        foreach (var l in lines) l.Header.ImageTimestampMicroseconds = 0;
        File.WriteAllText(Path.Combine(dir, "meta.jsonl"), string.Join("\n", lines.Select(SessionMeta.Serialize)) + "\n");
        var r = SessionVideo.Build(dir);
        Assert.Equal(3, r.FramesUsed);
        Assert.Equal(0, r.FramesRepeated);
        Assert.Equal(0, r.FramesSkipped);
    }
}
