#!/usr/bin/env python3
"""Make a video from a recorded session (the frames/*.jpg of one session folder).

Usage:
  session_video.py <session-dir> [--fps 20] [--max-hold 1.0] [--out video.avi]

The JPEG frames are placed on a fixed-rate timeline by their camera timestamps
(image_t_us in meta.jsonl; the control loop's t_us when a frame has none), so
the video runs in real time: a frame that arrived late is held (repeated) for
the slots it covers, up to --max-hold seconds; a longer pause is cut; frames
that fall into an already-used slot are left out.  The output is Motion-JPEG
in an AVI container (the recorded JPEGs as they are, no re-encoding), which
VLC, Windows Media Player and ffmpeg play; for an MP4 use
  ffmpeg -i video.avi -c:v libx264 -pix_fmt yuv420p video.mp4
Standard library only; this is the same algorithm as the drive manager's
「動画を作る」 (RumiCar.Protocol SessionVideo).  An AVI 1.0 file is limited to
1 GB; longer sessions are split into video.avi, video-2.avi, ...
"""
import argparse
import json
import os
import struct
import sys

MAX_FILE_BYTES = 1 << 30


class MjpegAviWriter:
    """AVI 1.0 with one MJPG video stream and an idx1 index (see MjpegAviWriter.cs)."""

    def __init__(self, path, width, height, fps):
        if width <= 0 or height <= 0:
            raise ValueError("frame size must be positive")
        if not 1 <= fps <= 240:
            raise ValueError("fps must be 1..240")
        self.path = path
        self.fps = fps
        self.f = open(path, "wb")
        self.index = []
        self.max_frame = 0
        w = self.f.write
        w(b"RIFF" + struct.pack("<I", 0) + b"AVI ")
        hdrl = 4 + (8 + 56) + (8 + 4 + (8 + 56) + (8 + 40))
        w(b"LIST" + struct.pack("<I", hdrl) + b"hdrl")
        w(b"avih" + struct.pack("<I", 56))
        w(struct.pack("<IIIIIIIIII", 1_000_000 // fps, 0, 0, 0x10 | 0x800, 0, 0, 1, 0, width, height))
        w(struct.pack("<IIII", 0, 0, 0, 0))
        w(b"LIST" + struct.pack("<I", 4 + (8 + 56) + (8 + 40)) + b"strl")
        w(b"strh" + struct.pack("<I", 56))
        w(b"vids" + b"MJPG" + struct.pack("<IHHIIIIIIIIhhhh", 0, 0, 0, 0, 1, fps, 0, 0, 0, 0xFFFFFFFF, 0, 0, 0, width, height))
        w(b"strf" + struct.pack("<I", 40))
        w(struct.pack("<IiiHH4sIiiII", 40, width, height, 1, 24, b"MJPG", width * height * 3, 0, 0, 0, 0))
        w(b"LIST")
        self.movi_size_pos = self.f.tell()
        w(struct.pack("<I", 0))
        self.movi_start = self.f.tell()
        w(b"movi")

    @staticmethod
    def bytes_for(n):
        return 8 + n + (n & 1) + 16

    def write_frame(self, jpeg):
        if not jpeg:
            raise ValueError("empty frame")
        if self.f.tell() + self.bytes_for(len(jpeg)) + 8 + 16 > MAX_FILE_BYTES:
            raise OverflowError("AVI file would exceed 1 GB")
        offset = self.f.tell() - self.movi_start
        self.f.write(b"00dc" + struct.pack("<I", len(jpeg)) + jpeg)
        if len(jpeg) & 1:
            self.f.write(b"\0")
        self.index.append((offset, len(jpeg)))
        self.max_frame = max(self.max_frame, len(jpeg))

    def finish(self):
        if self.f is None:
            return
        movi_end = self.f.tell()
        self.f.write(b"idx1" + struct.pack("<I", 16 * len(self.index)))
        for offset, size in self.index:
            self.f.write(b"00dc" + struct.pack("<III", 0x10, offset, size))
        end = self.f.tell()
        frames = len(self.index)
        max_bps = min(0xFFFFFFFF, self.max_frame * self.fps)

        def patch(pos, value):
            self.f.seek(pos)
            self.f.write(struct.pack("<I", value))

        patch(4, end - 8)
        patch(self.movi_size_pos, movi_end - self.movi_start)
        avih = 24
        patch(avih + 8 + 4, max_bps)
        patch(avih + 8 + 16, frames)
        patch(avih + 8 + 28, self.max_frame)
        strh = avih + 8 + 56 + 12
        patch(strh + 8 + 32, frames)
        patch(strh + 8 + 36, self.max_frame)
        self.f.close()
        self.f = None


def read_meta(session_dir):
    path = os.path.join(session_dir, "meta.jsonl")
    if not os.path.isfile(path):
        raise FileNotFoundError(f"not a session folder (meta.jsonl is missing): {session_dir}")
    lines = []
    with open(path, encoding="utf-8") as f:
        for text in f:
            text = text.strip()
            if not text:
                continue
            try:
                line = json.loads(text)
            except json.JSONDecodeError:
                continue
            if isinstance(line, dict) and isinstance(line.get("header"), dict):
                lines.append(line)
    return lines


def build(session_dir, fps=20, max_hold_s=1.0, base_name="video", log=print):
    lines = read_meta(session_dir)
    frames = [l for l in lines if l.get("image") and l["header"].get("image_w", 0) > 0 and l["header"].get("image_h", 0) > 0]
    if not frames:
        raise ValueError("the session has no frames with an image")
    use_sensor = all(f["header"].get("image_t_us", 0) > 0 for f in frames)

    def stamp(f):
        return int(f["header"]["image_t_us"] if use_sensor else f["header"].get("t_us", 0))

    width, height = frames[0]["header"]["image_w"], frames[0]["header"]["image_h"]
    period = 1_000_000.0 / fps
    max_hold = max(0, round(max_hold_s * fps))
    frames_dir = os.path.join(session_dir, "frames")
    files = []
    writer = None
    used = repeated = skipped = missing = 0
    slot = -1
    base = last = 0
    previous = None

    def open_writer():
        name = f"{base_name}.avi" if not files else f"{base_name}-{len(files) + 1}.avi"
        path = os.path.join(session_dir, name)
        files.append(path)
        return MjpegAviWriter(path, width, height, fps)

    def emit(jpeg):
        nonlocal writer
        if writer is None:
            writer = open_writer()
        if writer.f.tell() + MjpegAviWriter.bytes_for(len(jpeg)) + 64 > MAX_FILE_BYTES:
            writer.finish()
            writer = open_writer()
        writer.write_frame(jpeg)

    try:
        for f in frames:
            path = os.path.join(frames_dir, f["image"])
            if not os.path.isfile(path):
                missing += 1
                continue
            t = stamp(f)
            if slot < 0:
                base, target = t, 0
            elif t < last:
                target = slot + 1
                base = t - round(target * period)
            else:
                target = round((t - base) / period)
            if target <= slot:
                skipped += 1
                continue
            with open(path, "rb") as fh:
                jpeg = fh.read()
            if not jpeg:
                missing += 1
                continue
            gap = target - slot - 1
            if gap > max_hold:
                shift = gap - max_hold
                base += round(shift * period)
                target -= shift
                gap = max_hold
            if previous is not None and gap > 0:
                for _ in range(gap):
                    emit(previous)
                repeated += gap
            emit(jpeg)
            used += 1
            slot, last, previous = target, t, jpeg
        if writer is not None:
            writer.finish()
    except BaseException:
        if writer is not None and writer.f is not None:
            writer.f.close()
        for p in files:
            try:
                os.remove(p)
            except OSError:
                pass
        raise
    total = used + repeated
    log(f"{len(frames)} image frames -> {total} video frames ({used} used, {repeated} held, {skipped} same-slot, "
        f"{missing} missing), {width}x{height} @ {fps} fps, {total / fps:.1f} s")
    for p in files:
        log(f"wrote {p} ({os.path.getsize(p)} bytes)")
    return files


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("session_dir")
    p.add_argument("--fps", type=int, default=20)
    p.add_argument("--max-hold", type=float, default=1.0, help="seconds a frame is held across a gap (default 1.0)")
    p.add_argument("--out", default="video", help="base name of the output inside the session folder (default video)")
    a = p.parse_args()
    try:
        build(a.session_dir, a.fps, a.max_hold, a.out)
    except (FileNotFoundError, ValueError, OverflowError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
