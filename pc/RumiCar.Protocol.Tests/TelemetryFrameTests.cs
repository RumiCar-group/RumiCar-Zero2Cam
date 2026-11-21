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
            TofMm = new[] { 120, 1500, -1 },   // ABI 5: distances up to 2000 mm, not cut off at 200
            Steer = RumiCarApi.Right,
            SteerPwm = 140,
            DriveDir = seq % 3 == 0 ? RumiCarApi.Brake : RumiCarApi.Forward,
            DrivePwm = seq % 3 == 0 ? 0 : 166,
            State = 2,
            StateName = "TURN_RIGHT",
            Flags = 0,
            Mode = "auto",
            EStop = seq % 2 == 0,
            LogicSteer = RumiCarApi.Left,
            LogicSteerPwm = 96,
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
        Assert.Equal(5, frame.Header.Abi);
        Assert.Equal(new[] { 120, 1500, -1 }, frame.Header.TofMm);
        Assert.Equal("TURN_RIGHT", frame.Header.StateName);
        Assert.False(frame.Header.EStop);
        Assert.Equal(RumiCarApi.Right, frame.Header.Steer);
        Assert.Equal(140, frame.Header.SteerPwm);
        Assert.Equal(96, frame.Header.LogicSteerPwm);
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
        const string json = "{\"abi\":5,\"battery_mv\":0,\"drive_dir\":3,\"drive_pwm\":0,\"estop\":true,\"flags\":0,\"image_h\":0,\"image_w\":0," +
                            "\"logic_drive_dir\":2,\"logic_drive_pwm\":179,\"logic_steer\":2,\"logic_steer_pwm\":128,\"logic_version\":\"0.6.0\",\"mode\":\"manual\"," +
                            "\"pack_version\":\"0.3.0\",\"seq\":42,\"state\":2,\"state_name\":\"TURN_RIGHT\",\"steer\":1,\"steer_pwm\":0," +
                            "\"t_us\":123456789,\"tof_mm\":[120,1500,-3],\"future_key\":{\"x\":1}}";
        var h = TelemetryFrameCodec.HeaderFromJson(json);
        Assert.NotNull(h);
        Assert.Equal(RumiCarApi.Abi, h!.Abi);
        Assert.Equal(new[] { 120, 1500, -3 }, h.TofMm);   // the car's values, not cut off at 200 mm (ABI 5)
        Assert.Equal("120 / 1500 / -3 範囲外", string.Join(" / ", h.TofMm.Select(RumiCarApi.TofText)));
        Assert.Equal(0, h.SteerPwm);
        Assert.Equal(128, h.LogicSteerPwm);
        Assert.Equal(42u, h.Seq);
        Assert.True(h.EStop);
        Assert.Equal("manual", h.Mode);
        Assert.Equal(RumiCarApi.Center, h.Steer);
        Assert.Equal(RumiCarApi.Brake, h.DriveDir);
        Assert.Equal(RumiCarApi.Right, h.LogicSteer);
        Assert.Equal(RumiCarApi.Forward, h.LogicDriveDir);
        Assert.Equal(179, h.LogicDrivePwm);
        Assert.Equal(123456789UL, h.TimestampMicroseconds);
        // A header of a 0.5.x / 0.6.x car (ABI 4): the same keys; that car cut the ToF readings off at 200 mm (-3 beyond).
        var abi4 = TelemetryFrameCodec.HeaderFromJson("{\"abi\":4,\"seq\":8,\"tof_mm\":[200,-3,-1],\"steer\":2,\"steer_pwm\":128,\"logic_steer\":2,\"logic_steer_pwm\":128,\"drive_dir\":2,\"drive_pwm\":150}");
        Assert.NotNull(abi4);
        Assert.Equal(RumiCarApi.AbiWithTofCutOff, abi4!.Abi);
        Assert.Equal(new[] { RumiCarApi.TofMaxMmAbi4, RumiCarApi.TofOutOfRange, RumiCarApi.TofTimeout }, abi4.TofMm);
        Assert.Equal(128, abi4.SteerPwm);
        // A header of a 0.4.x car (ABI 3) has no steering strength: it reads as full (255), as its steering was.
        var abi3 = TelemetryFrameCodec.HeaderFromJson("{\"abi\":3,\"seq\":7,\"tof_mm\":[120,-3,-1],\"steer\":0,\"logic_steer\":2,\"drive_dir\":2,\"drive_pwm\":150}");
        Assert.NotNull(abi3);
        Assert.Equal(RumiCarApi.AbiWithoutSteerStrength, abi3!.Abi);
        Assert.Equal(255, abi3.SteerPwm);
        Assert.Equal(255, abi3.LogicSteerPwm);
        Assert.Equal("RC_steer(LEFT) / RC_drive(FORWARD, 150)", RumiCarApi.CommandText(abi3.Steer, abi3.SteerPwm, abi3.DriveDir, abi3.DrivePwm));
        // A recording of a 0.3.x car has no "abi" and the old keys: it reads as ABI 0 (the drive manager says so).
        var old = TelemetryFrameCodec.HeaderFromJson("{\"seq\":1,\"tof_mm\":[120,8190,-1],\"steer\":-1,\"drive_permille\":650,\"brake\":false}");
        Assert.NotNull(old);
        Assert.Equal(0, old!.Abi);
    }

    [Theory]
    [InlineData(RumiCarApi.Left, 255, RumiCarApi.Forward, 150, "RC_steer(LEFT) / RC_drive(FORWARD, 150)")]        // full: the one-argument form
    [InlineData(RumiCarApi.Right, 128, RumiCarApi.Forward, 150, "RC_steer(RIGHT, 128) / RC_drive(FORWARD, 150)")]
    [InlineData(RumiCarApi.Left, 0, RumiCarApi.Reverse, 80, "RC_steer(LEFT, 0) / RC_drive(REVERSE, 80)")]          // 0 = neutral, shown as written
    [InlineData(RumiCarApi.Center, 0, RumiCarApi.Brake, 0, "RC_steer(CENTER) / RC_drive(BRAKE, 0)")]
    [InlineData(RumiCarApi.Center, 200, RumiCarApi.Free, 90, "RC_steer(CENTER) / RC_drive(FREE, 0)")]              // CENTER ignores the strength
    [InlineData(RumiCarApi.Right, 1, RumiCarApi.Forward, 255, "RC_steer(RIGHT, 1) / RC_drive(FORWARD, 255)")]
    public void CommandTextIsTheLibraryCall(int steer, int steerPwm, int dir, int pwm, string expected)
    {
        Assert.Equal(expected, RumiCarApi.CommandText(steer, steerPwm, dir, pwm));
    }

    [Theory]
    [InlineData(0, "0")]
    [InlineData(200, "200")]
    [InlineData(201, "201")]                      // beyond the 200 mm a car of ABI 3 / 4 cut off at: a distance from ABI 5
    [InlineData(RumiCarApi.TofMaxMm, "2000")]
    [InlineData(RumiCarApi.TofTimeout, "-1 タイムアウト")]
    [InlineData(RumiCarApi.TofOutOfRange, "-3 範囲外")]
    public void TofTextIsTheRcReadValue(int mm, string expected)
    {
        Assert.Equal(expected, RumiCarApi.TofText(mm));
    }

    [Fact]
    public void RecordedHeaderKeepsTheSteeringStrength()
    {
        // The recorder writes the header as decoded (meta.jsonl): steer_pwm and logic_steer_pwm are in it.
        var json = TelemetryFrameCodec.HeaderToJson(Sample(3, 0).Header);
        Assert.Contains("\"steer_pwm\":140", json);
        Assert.Contains("\"logic_steer_pwm\":96", json);
        Assert.Contains("\"abi\":5", json);
        Assert.Contains("\"tof_mm\":[120,1500,-1]", json);
        var back = TelemetryFrameCodec.HeaderFromJson(json)!;
        Assert.Equal(140, back.SteerPwm);
        Assert.Equal(96, back.LogicSteerPwm);
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
