using RumiCar.Protocol.Control;

namespace RumiCar.Protocol.Packs;

/// <summary>
/// Sends a logic or pack directory to the car over the control channel
/// (upload_begin / upload_chunk / upload_end, docs 5.2) after verifying it
/// locally the same way the car will.
/// </summary>
public static class VersionPush
{
    public sealed record Plan(string Kind, string DirectoryName, Manifest Manifest, IReadOnlyList<ControlCodec.UploadFile> Files, long TotalBytes);

    /// <summary>
    /// Verifies <paramref name="directory"/> against its manifest.json and
    /// lists the files to send (manifest.json included).  Throws
    /// <see cref="InvalidDataException"/> with the first problem found.
    /// </summary>
    public static Plan Prepare(string directory)
    {
        var (manifest, problems) = ManifestVerifier.Verify(directory);
        if (manifest is null || problems.Count > 0)
            throw new InvalidDataException(problems.Count > 0 ? problems[0] : "manifest.json could not be read");
        var kind = manifest.IsPack ? "pack" : "logic";
        var files = new List<ControlCodec.UploadFile>();
        long total = 0;
        foreach (var f in manifest.Files)
        {
            files.Add(new ControlCodec.UploadFile(f.Path, f.Bytes, f.Sha256));
            total += f.Bytes;
        }
        var manifestPath = Path.Combine(directory, "manifest.json");
        var manifestBytes = File.ReadAllBytes(manifestPath);
        files.Add(new ControlCodec.UploadFile("manifest.json", manifestBytes.Length, ManifestVerifier.Sha256Hex(manifestBytes)));
        total += manifestBytes.Length;
        return new Plan(kind, manifest.DirectoryName, manifest, files, total);
    }

    /// <summary>
    /// Pushes a prepared directory.  <paramref name="send"/> performs one
    /// request/response round trip; <paramref name="progress"/> receives the
    /// bytes sent so far.  Throws <see cref="IOException"/> when the car
    /// refuses a step (after sending upload_abort).
    /// </summary>
    public static async Task PushAsync(string directory, Plan plan,
        Func<ControlRequest, CancellationToken, Task<ControlResponse>> send, Func<long> nextId,
        IProgress<long>? progress, CancellationToken ct)
    {
        var begin = await send(ControlCodec.UploadBegin(nextId(), plan.Kind, plan.DirectoryName, plan.Files), ct).ConfigureAwait(false);
        if (!begin.Ok) throw new IOException("upload_begin: " + begin.Error);
        try
        {
            long sent = 0;
            var buffer = new byte[ControlCodec.UploadChunkBytes];
            foreach (var f in plan.Files)
            {
                var full = Path.Combine(directory, f.Path.Replace('/', Path.DirectorySeparatorChar));
                using var stream = File.OpenRead(full);
                if (stream.Length != f.Bytes) throw new IOException($"{f.Path} changed size since it was prepared");
                long offset = 0;
                while (offset < f.Bytes)
                {
                    var n = await stream.ReadAsync(buffer.AsMemory(0, buffer.Length), ct).ConfigureAwait(false);
                    if (n <= 0) throw new IOException($"{f.Path} ended early");
                    var r = await send(ControlCodec.UploadChunk(nextId(), f.Path, offset, buffer.AsMemory(0, n)), ct).ConfigureAwait(false);
                    if (!r.Ok) throw new IOException($"upload_chunk {f.Path}@{offset}: {r.Error}");
                    offset += n;
                    sent += n;
                    progress?.Report(sent);
                }
            }
            var end = await send(ControlCodec.UploadEnd(nextId()), ct).ConfigureAwait(false);
            if (!end.Ok) throw new IOException("upload_end: " + end.Error);
        }
        catch
        {
            try { await send(ControlCodec.UploadAbort(nextId()), CancellationToken.None).ConfigureAwait(false); }
            catch (Exception) { /* the connection may already be gone */ }
            throw;
        }
    }
}
