namespace RumiCar.Protocol.Recording;

/// <summary>
/// Turns the JPEG frames of a recorded session into a video (docs 6.4).  The
/// frames are placed on a fixed-rate timeline by their camera timestamps, so
/// the video runs in real time: a frame that arrived late is held (repeated)
/// for the slots it covers, up to <see cref="Options.MaxHoldSeconds"/>; frames
/// that fall into an already-used slot are left out.  The output is Motion-JPEG
/// in AVI (no re-encoding), split into numbered parts when a file would exceed
/// the AVI size limit.  car/tools/session_video.py does the same in Python.
/// </summary>
public static class SessionVideo
{
    public sealed record Options(int Fps = 20, double MaxHoldSeconds = 1.0, string BaseName = "video");

    public sealed record Result(IReadOnlyList<string> Files, int FramesUsed, int FramesRepeated, int FramesSkipped, int FramesMissingFile,
                                int Width, int Height, double DurationSeconds);

    /// <summary>
    /// Writes &lt;session&gt;/video.avi (and video-2.avi ... when split).  Throws
    /// <see cref="InvalidDataException"/> when the session holds no usable image.
    /// </summary>
    public static Result Build(string sessionDirectory, Options? options = null, IProgress<int>? progress = null, CancellationToken ct = default)
    {
        options ??= new Options();
        if (options.Fps is < 1 or > 240) throw new ArgumentOutOfRangeException(nameof(options), "fps must be 1..240");
        var lines = SessionMeta.Read(sessionDirectory, out _);
        var frames = lines.Where(l => !string.IsNullOrEmpty(l.Image) && l.Header.ImageWidth > 0 && l.Header.ImageHeight > 0).ToList();
        if (frames.Count == 0) throw new InvalidDataException("the session has no frames with an image");

        // One time base for the whole session: the camera's sensor clock when
        // every image frame carries it, otherwise the control loop's clock.
        var useSensorClock = frames.All(f => f.Header.ImageTimestampMicroseconds > 0);
        static long Stamp(SessionMetaLine f, bool sensor) =>
            (long)Math.Min(long.MaxValue, sensor ? f.Header.ImageTimestampMicroseconds : f.Header.TimestampMicroseconds);

        var width = frames[0].Header.ImageWidth;
        var height = frames[0].Header.ImageHeight;
        var periodUs = 1_000_000.0 / options.Fps;
        var maxHold = (int)Math.Max(0, Math.Round(options.MaxHoldSeconds * options.Fps));
        var framesDir = Path.Combine(sessionDirectory, SessionMeta.FramesDirectory);

        var files = new List<string>();
        MjpegAviWriter? writer = null;
        int used = 0, repeated = 0, skipped = 0, missing = 0;
        long slot = -1;
        long baseStamp = 0, lastStamp = 0;   // microseconds; baseStamp is the time of slot 0
        byte[]? previous = null;
        try
        {
            MjpegAviWriter Open()
            {
                var name = files.Count == 0 ? options.BaseName + ".avi" : $"{options.BaseName}-{files.Count + 1}.avi";
                var path = Path.Combine(sessionDirectory, name);
                files.Add(path);
                return new MjpegAviWriter(path, width, height, options.Fps);
            }
            void Emit(byte[] jpeg)
            {
                writer ??= Open();
                if (writer.BytesWritten + MjpegAviWriter.BytesFor(jpeg.Length) + 64 > MjpegAviWriter.MaxFileBytes)
                {
                    writer.Finish();
                    writer = Open();
                }
                writer.WriteFrame(jpeg);
            }

            for (var i = 0; i < frames.Count; i++)
            {
                ct.ThrowIfCancellationRequested();
                var f = frames[i];
                var path = Path.Combine(framesDir, f.Image!);
                if (!File.Exists(path)) { missing++; continue; }
                var stamp = Stamp(f, useSensorClock);
                long target;
                if (slot < 0)
                {
                    baseStamp = stamp;
                    target = 0;
                }
                else if (stamp < lastStamp)
                {
                    // Clock went backwards (the car restarted): continue right after the last frame.
                    target = slot + 1;
                    baseStamp = stamp - (long)Math.Round(target * periodUs);
                }
                else
                {
                    target = (long)Math.Round((stamp - baseStamp) / periodUs);
                }
                if (target <= slot) { skipped++; continue; }   // same slot as the previous frame
                var jpeg = File.ReadAllBytes(path);
                if (jpeg.Length == 0) { missing++; continue; }
                var gap = target - slot - 1;   // empty slots between the previous frame and this one
                if (gap > maxHold)
                {
                    // A long pause is cut, not padded: re-anchor the timeline so that
                    // this frame follows the held ones directly.
                    var shift = gap - maxHold;
                    baseStamp += (long)Math.Round(shift * periodUs);
                    target -= shift;
                    gap = maxHold;
                }
                if (previous is not null && gap > 0)
                {
                    for (var k = 0; k < gap; k++) Emit(previous);
                    repeated += (int)gap;
                }
                Emit(jpeg);
                used++;
                slot = target;
                lastStamp = stamp;
                previous = jpeg;
                if ((i & 31) == 0) progress?.Report((i + 1) * 100 / frames.Count);
            }
            writer?.Finish();
        }
        catch
        {
            writer?.Dispose();
            foreach (var f in files) { try { File.Delete(f); } catch (IOException) { } }
            throw;
        }
        progress?.Report(100);
        var totalFrames = used + repeated;
        return new Result(files, used, repeated, skipped, missing, width, height, totalFrames / (double)options.Fps);
    }
}
