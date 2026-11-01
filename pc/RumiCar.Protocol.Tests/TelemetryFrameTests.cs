using RumiCar.Protocol.Telemetry;
using Xunit;

namespace RumiCar.Protocol.Tests;

public class TelemetryFrameTests
{
    private static TelemetryFrame Sample(uint seq, int jpegBytes) => new()
    {
        Header = new TelemetryHeader
        {
            Seq = seq,
            TimestampMicroseconds = 1_000_000 + seq * 50_000UL,
            Abi = RumiCarApi.Abi,
            TofMm = new[] { 120, -3, -1 },
            Steer = RumiCarApi.Right,
            DriveDir = seq % 3 == 0 ? RumiCarApi.Brake : RumiCarApi.Forward,
            DrivePwm = seq % 3 == 0 ? 0 : 166,
            State = 2,
            StateName = "TURN_RIGHT",
            Flags = 0,
            Mode = "auto",
            EStop = seq % 2 == 0,
            LogicSteer = RumiCarApi.Left,
            LogicDriveDir = RumiCarApi.Forward,
            LogicDrivePwm = 107,
            LogicVersion = "0.1.0",
            PackVersion = "0.1.0",
            ImageWidth = jpegBytes > 0 ? 320 : 0,
            ImageHeight = jpegBytes > 0 ? 240 : 0,
            ImageSeq = jpegBytes > 0 ? seq : 0,
            ImageTimestampMicroseconds = jpegBytes > 0 ? 777_000UL + seq : 0,
        },
        Jpeg = Enumerable.Range(0, jpegBytes).Select(i => (byte)(i * 7)).ToArray(),
    };

    [Fact]
    public void EncodeDecodeRoundTrip()
    {
        var bytes = TelemetryFrameCodec.Encode(Sample(5, 1000));
        var result = TelemetryFrameCodec.TryDecode(bytes, out var frame, out var consumed);
        Assert.Equal(TelemetryFrameCodec.DecodeResult.Ok, result);
        Assert.Equal(bytes.Length, consumed);
        Assert.NotNull(frame);
        Assert.Equal(5u, frame!.Header.Seq);
        Assert.Equal(3, frame.Header.Abi);
        Assert.Equal(new[] { 120, -3, -1 }, frame.Header.TofMm);
        Assert.Equal("TURN_RIGHT", frame.Header.StateName);
        Assert.False(frame.Header.EStop);
        Assert.Equal(RumiCarApi.Right, frame.Header.Steer);
        Assert.Equal(RumiCarApi.Forward, frame.Header.DriveDir);
        Assert.Equal(166, frame.Header.DrivePwm);
        Assert.Equal(RumiCarApi.Left, frame.Header.LogicSteer);
        Assert.Equal(RumiCarApi.Forward, frame.Header.LogicDriveDir);
        Assert.Equal(107, frame.Header.LogicDrivePwm);
        Assert.Equal(1000, frame.Jpeg.Length);
        Assert.Equal(5u, frame.Header.ImageSeq);
        Assert.Equal(777_005UL, frame.Header.ImageTimestampMicroseconds);
        Assert.Equal((byte)(999 * 7 % 256), frame.Jpeg[999]);
    }

    [Fact]
    public void HeaderWrittenByTheCarDecodes()
    {
        // Exactly the key set rumicar-core writes (car/core/src/main.cpp), plus an unknown key a later version might add.
        const string json = "{\"abi\":3,\"battery_mv\":0,\"drive_dir\":3,\"drive_pwm\":0,\"estop\":true,\"flags\":0,\"image_h\":0,\"image_w\":0," +
                            "\"logic_drive_dir\":2,\"logic_drive_pwm\":179,\"logic_steer\":2,\"logic_version\":\"0.3.0\",\"mode\":\"manual\"," +
                            "\"pack_version\":\"0.2.0\",\"seq\":42,\"state\":2,\"state_name\":\"TURN_RIGHT\",\"steer\":1," +
                            "\"t_us\":123456789,\"tof_mm\":[120,-3,-1],\"future_key\":{\"x\":1}}";
        var h = TelemetryFrameCodec.HeaderFromJson(json);
        Assert.NotNull(h);
        Assert.Equal(3, h!.Abi);
        Assert.Equal(42u, h.Seq);
        Assert.True(h.EStop);
        Assert.Equal("manual", h.Mode);
        Assert.Equal(RumiCarApi.Center, h.Steer);
        Assert.Equal(RumiCarApi.Brake, h.DriveDir);
        Assert.Equal(RumiCarApi.Right, h.LogicSteer);
        Assert.Equal(RumiCarApi.Forward, h.LogicDriveDir);
        Assert.Equal(179, h.LogicDrivePwm);
        Assert.Equal(123456789UL, h.TimestampMicroseconds);
        // A recording of a 0.3.x car has no "abi" and the old keys: it reads as ABI 0 (the drive manager says so).
        var old = TelemetryFrameCodec.HeaderFromJson("{\"seq\":1,\"tof_mm\":[120,8190,-1],\"steer\":-1,\"drive_permille\":650,\"brake\":false}");
        Assert.NotNull(old);
        Assert.Equal(0, old!.Abi);
    }

    [Fact]
    public void PartialBufferAsksForMore()
    {
        var bytes = TelemetryFrameCodec.Encode(Sample(1, 100));
        for (int cut = 0; cut < bytes.Length; cut += 7)
        {
            var r = TelemetryFrameCodec.TryDecode(bytes.AsSpan(0, cut), out var f, out _);
            Assert.Equal(TelemetryFrameCodec.DecodeResult.NeedMore, r);
            Assert.Null(f);
        }
    }

    [Fact]
    public void WrongMagicIsCorrupt()
    {
        var bytes = TelemetryFrameCodec.Encode(Sample(1, 10));
        bytes[0] = (byte)'X';
        Assert.Equal(TelemetryFrameCodec.DecodeResult.Corrupt, TelemetryFrameCodec.TryDecode(bytes, out _, out _));
        Assert.Equal(TelemetryFrameCodec.DecodeResult.Corrupt, TelemetryFrameCodec.TryDecode(new byte[] { (byte)'X' }, out _, out _));
    }

    [Fact]
    public async Task ReaderResynchronisesAfterGarbage()
    {
        var a = TelemetryFrameCodec.Encode(Sample(1, 50));
        var b = TelemetryFrameCodec.Encode(Sample(2, 0));
        var stream = new MemoryStream();
        stream.Write(a);
        stream.Write(new byte[] { 1, 2, 3, 4, 5, (byte)'R', (byte)'M' }); // junk, including a partial magic
        stream.Write(b);
        stream.Position = 0;

        var reader = new TelemetryFrameReader(stream);
        var f1 = await reader.ReadAsync(CancellationToken.None);
        var f2 = await reader.ReadAsync(CancellationToken.None);
        var end = await reader.ReadAsync(CancellationToken.None);
        Assert.Equal(1u, f1!.Header.Seq);
        Assert.Equal(2u, f2!.Header.Seq);
        Assert.Empty(f2.Jpeg);
        Assert.Null(end);
        Assert.True(reader.Resyncs >= 1);
    }

    [Fact]
    public async Task ReaderHandlesFramesSplitAcrossReads()
    {
        var frames = Enumerable.Range(1, 20).Select(i => TelemetryFrameCodec.Encode(Sample((uint)i, 3000))).ToArray();
        var all = frames.SelectMany(x => x).ToArray();
        var reader = new TelemetryFrameReader(new ChunkedStream(all, 1234));
        for (uint i = 1; i <= 20; i++)
        {
            var f = await reader.ReadAsync(CancellationToken.None);
            Assert.Equal(i, f!.Header.Seq);
            Assert.Equal(3000, f.Jpeg.Length);
        }
        Assert.Null(await reader.ReadAsync(CancellationToken.None));
        Assert.Equal(0, reader.Resyncs);
    }

    private sealed class ChunkedStream : Stream
    {
        private readonly byte[] _data;
        private readonly int _chunk;
        private int _pos;
        public ChunkedStream(byte[] data, int chunk) { _data = data; _chunk = chunk; }
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => _data.Length;
        public override long Position { get => _pos; set => throw new NotSupportedException(); }
        public override void Flush() { }
        public override int Read(byte[] buffer, int offset, int count)
        {
            int n = Math.Min(Math.Min(count, _chunk), _data.Length - _pos);
            Array.Copy(_data, _pos, buffer, offset, n);
            _pos += n;
            return n;
        }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }
}
