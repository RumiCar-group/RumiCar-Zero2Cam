using System.Buffers.Binary;
using System.Text;

namespace RumiCar.Protocol.Recording;

/// <summary>
/// Writes an AVI file whose video stream is Motion-JPEG: every frame is a
/// recorded JPEG as it was received, stored without re-encoding.  Nothing
/// outside the .NET base library is needed, and the result plays in VLC,
/// Windows Media Player and most other players (docs 6.4).  The container is
/// AVI 1.0 (RIFF, one 'idx1' index), so a file is limited to <see cref="MaxFileBytes"/>.
/// </summary>
public sealed class MjpegAviWriter : IDisposable
{
    /// <summary>Limit of one file (AVI 1.0 players handle 1 GB reliably).</summary>
    public const long MaxFileBytes = 1L << 30;

    private const uint AvifHasIndex = 0x10;
    private const uint AvifTrustCkType = 0x800;
    private const uint AviifKeyFrame = 0x10;
    private const int HeaderListBytes = 4 + (8 + 56) + (8 + 4 + (8 + 56) + (8 + 40));   // 'hdrl' + avih + strl(strh, strf)

    private readonly FileStream _file;
    private readonly BinaryWriter _w;
    private readonly int _width;
    private readonly int _height;
    private readonly int _fps;
    private readonly List<(uint offset, uint size)> _index = new();
    private readonly long _moviListSizePos;
    private readonly long _moviDataStart;   // position of the 'movi' fourcc
    private int _maxFrameBytes;
    private bool _finished;

    public string Path { get; }
    public int Frames => _index.Count;
    /// <summary>Bytes written so far (frames and headers; the index comes at the end).</summary>
    public long BytesWritten => _file.Position;

    public MjpegAviWriter(string path, int width, int height, int fps)
    {
        if (width <= 0 || height <= 0) throw new ArgumentOutOfRangeException(nameof(width), "frame size must be positive");
        if (fps is < 1 or > 240) throw new ArgumentOutOfRangeException(nameof(fps), "fps must be 1..240");
        Path = path;
        _width = width;
        _height = height;
        _fps = fps;
        _file = new FileStream(path, FileMode.Create, FileAccess.ReadWrite, FileShare.None, 1 << 16);
        _w = new BinaryWriter(_file, Encoding.ASCII, leaveOpen: true);

        // RIFF header and 'hdrl' are written with placeholder sizes and counts, patched in Finish().
        Fourcc("RIFF"); _w.Write(0u); Fourcc("AVI ");
        Fourcc("LIST"); _w.Write((uint)HeaderListBytes); Fourcc("hdrl");
        Fourcc("avih"); _w.Write(56u);
        _w.Write((uint)(1_000_000L / fps));   // dwMicroSecPerFrame
        _w.Write(0u);                          // dwMaxBytesPerSec (patched)
        _w.Write(0u);                          // dwPaddingGranularity
        _w.Write(AvifHasIndex | AvifTrustCkType);
        _w.Write(0u);                          // dwTotalFrames (patched)
        _w.Write(0u);                          // dwInitialFrames
        _w.Write(1u);                          // dwStreams
        _w.Write(0u);                          // dwSuggestedBufferSize (patched)
        _w.Write((uint)width);
        _w.Write((uint)height);
        _w.Write(0u); _w.Write(0u); _w.Write(0u); _w.Write(0u);   // dwReserved

        Fourcc("LIST"); _w.Write((uint)(4 + (8 + 56) + (8 + 40))); Fourcc("strl");
        Fourcc("strh"); _w.Write(56u);
        Fourcc("vids"); Fourcc("MJPG");
        _w.Write(0u);                          // dwFlags
        _w.Write((ushort)0); _w.Write((ushort)0);   // wPriority, wLanguage
        _w.Write(0u);                          // dwInitialFrames
        _w.Write(1u);                          // dwScale
        _w.Write((uint)fps);                   // dwRate: fps = dwRate / dwScale
        _w.Write(0u);                          // dwStart
        _w.Write(0u);                          // dwLength in frames (patched)
        _w.Write(0u);                          // dwSuggestedBufferSize (patched)
        _w.Write(0xFFFFFFFFu);                 // dwQuality: default
        _w.Write(0u);                          // dwSampleSize: variable
        _w.Write((short)0); _w.Write((short)0); _w.Write((short)width); _w.Write((short)height);   // rcFrame

        Fourcc("strf"); _w.Write(40u);
        _w.Write(40u);                         // biSize
        _w.Write(width);
        _w.Write(height);
        _w.Write((ushort)1);                   // biPlanes
        _w.Write((ushort)24);                  // biBitCount
        Fourcc("MJPG");                        // biCompression
        _w.Write((uint)(width * height * 3));  // biSizeImage
        _w.Write(0); _w.Write(0);              // pixels per metre
        _w.Write(0u); _w.Write(0u);            // biClrUsed, biClrImportant

        Fourcc("LIST");
        _moviListSizePos = _file.Position;
        _w.Write(0u);                          // movi list size (patched)
        _moviDataStart = _file.Position;
        Fourcc("movi");
    }

    /// <summary>Bytes the next frame would add (chunk header, padding, and its index entry).</summary>
    public static long BytesFor(int jpegLength) => 8 + jpegLength + (jpegLength & 1) + 16;

    /// <summary>Appends one JPEG frame.  Throws when the file would exceed <see cref="MaxFileBytes"/>.</summary>
    public void WriteFrame(ReadOnlySpan<byte> jpeg)
    {
        ObjectDisposedException.ThrowIf(_finished, this);
        if (jpeg.Length == 0) throw new ArgumentException("empty frame", nameof(jpeg));
        if (_file.Position + BytesFor(jpeg.Length) + 8 + 16 > MaxFileBytes) throw new InvalidOperationException("AVI file would exceed 1 GB");
        var offset = checked((uint)(_file.Position - _moviDataStart));
        Fourcc("00dc");
        _w.Write((uint)jpeg.Length);
        _w.Write(jpeg);
        if ((jpeg.Length & 1) != 0) _w.Write((byte)0);
        _index.Add((offset, (uint)jpeg.Length));
        if (jpeg.Length > _maxFrameBytes) _maxFrameBytes = jpeg.Length;
    }

    /// <summary>Writes the index and patches the sizes; the file is complete afterwards.</summary>
    public void Finish()
    {
        if (_finished) return;
        _finished = true;
        var moviEnd = _file.Position;
        Fourcc("idx1");
        _w.Write((uint)(_index.Count * 16));
        Span<byte> entry = stackalloc byte[16];
        "00dc"u8.CopyTo(entry);
        foreach (var (offset, size) in _index)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(entry[4..], AviifKeyFrame);
            BinaryPrimitives.WriteUInt32LittleEndian(entry[8..], offset);
            BinaryPrimitives.WriteUInt32LittleEndian(entry[12..], size);
            _w.Write(entry);
        }
        var end = _file.Position;

        Patch(4, (uint)(end - 8));                                   // RIFF size
        Patch(_moviListSizePos, (uint)(moviEnd - _moviDataStart));   // movi list size
        var frames = (uint)_index.Count;
        var maxBytesPerSec = (uint)Math.Min(uint.MaxValue, (long)_maxFrameBytes * _fps);
        const long avih = 12 + 12;                                   // after RIFF(12) + LIST hdr(8) + 'hdrl'(4): 'avih' fourcc
        Patch(avih + 8 + 4, maxBytesPerSec);                         // dwMaxBytesPerSec
        Patch(avih + 8 + 16, frames);                                // dwTotalFrames
        Patch(avih + 8 + 28, (uint)_maxFrameBytes);                  // dwSuggestedBufferSize
        const long strh = avih + 8 + 56 + 12;                        // LIST hdr(8) + 'strl'(4) then 'strh'
        Patch(strh + 8 + 32, frames);                                // dwLength
        Patch(strh + 8 + 36, (uint)_maxFrameBytes);                  // dwSuggestedBufferSize
        _w.Flush();
        _file.Flush(true);
        _w.Dispose();
        _file.Dispose();
    }

    public void Dispose() => Finish();

    private void Fourcc(string code)
    {
        if (code.Length != 4) throw new ArgumentException("fourcc", nameof(code));
        _w.Write(Encoding.ASCII.GetBytes(code));
    }

    private void Patch(long position, uint value)
    {
        var keep = _file.Position;
        _file.Position = position;
        _w.Write(value);
        _file.Position = keep;
    }
}
