#!/usr/bin/env python3
"""End-to-end check of the learning tools against the real plug-in:

  synthetic labelled sessions (a red light that grows, then green; walls only)
  -> train_pack.py train            (Python: features, logistic regression, vision.bin, manifest)
  -> make_manifest.py verify
  -> train_pack.py replay           (Python perception)
  -> rumicar-replay                 (the actual libcam_drive.so with the pack, C++)

and asserts that the car approaches, stops at the red light and resumes on
green.  Run by CI after the build; needs numpy and Pillow.

  python3 car/tests/tools_smoke.py <build dir> [<work dir>]
"""
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOOLS = os.path.join(ROOT, "tools")
W, H = 320, 240


def make_frame(rng, kind, k, n):
    y = np.arange(H)[:, None]
    px = np.zeros((H, W, 3), dtype=np.float64)
    px[:, :, 0] = 110 + 20 * y / H
    px[:, :, 1] = 110 + 20 * y / H
    px[:, :, 2] = 115 + 20 * y / H
    px += rng.normal(0, 5, size=px.shape)
    img = Image.fromarray(np.clip(px, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(img)
    box = None
    if kind == "red":
        r = 6 + int(34 * k / max(n - 1, 1))
        cx, cy = 180 + int(rng.integers(-3, 4)), 100 + int(rng.integers(-3, 4))
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(225, 30, 30))
        d.rectangle((cx - r - 4, cy - r - 28, cx + r + 4, cy + r + 4), outline=(30, 30, 30), width=3)
        box = [cx - r, cy - r, cx + r + 1, cy + r + 1]
    elif kind == "green":
        r, cx, cy = 40, 180, 100
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(30, 220, 50))
        box = [cx - r, cy - r, cx + r + 1, cy + r + 1]
    else:
        for _ in range(3):
            x0 = int(rng.integers(0, W - 40))
            y0 = int(rng.integers(0, H - 40))
            d.rectangle((x0, y0, x0 + 40, y0 + 40), fill=(70 + int(rng.integers(0, 40)),) * 3)
        d.ellipse((20, 20, 50, 50), fill=(40, 60, 230))
    return img, box


def write_session(path, plan, scenario, seed):
    rng = np.random.default_rng(seed)
    os.makedirs(os.path.join(path, "frames"), exist_ok=True)
    meta, labels = [], []
    idx, t = 0, 5_000_000
    for kind, n in plan:
        for k in range(n):
            idx += 1
            img, box = make_frame(rng, kind, k, n)
            name = f"{idx:06d}.jpg"
            img.save(os.path.join(path, "frames", name), "JPEG", quality=80)
            hdr = {"seq": idx, "t_us": t, "tof_mm": [8190, 8190, 8190], "steer": 0, "drive_permille": 900, "brake": False,
                   "state": 0, "state_name": "CRUISE", "flags": 0, "mode": "auto", "estop": False, "logic_steer": 0,
                   "logic_drive_permille": 900, "logic_name": "tof_drive", "logic_version": "0.2.0", "pack_id": "base-tof",
                   "pack_version": "0.1.1", "battery_mv": 0, "throttled": 0, "image_w": W, "image_h": H, "image_seq": idx,
                   "image_t_us": t + 100}
            meta.append({"index": idx, "image": name, "received": "x", "header": hdr})
            if kind == "red":
                labels.append({"index": idx, "objects": [{"name": "traffic_light_red", "box": box}]})
            elif kind == "green":
                labels.append({"index": idx, "objects": [{"name": "traffic_light_green", "box": box}]})
            else:
                labels.append({"index": idx, "negative": True})
            t += 50_000
    with open(os.path.join(path, "meta.jsonl"), "w", encoding="utf-8") as f:
        f.write("\n".join(json.dumps(m) for m in meta) + "\n")
    with open(os.path.join(path, "labels.jsonl"), "w", encoding="utf-8") as f:
        f.write("\n".join(json.dumps(l) for l in labels) + "\n")
    with open(os.path.join(path, "session.json"), "w", encoding="utf-8") as f:
        json.dump({"schema": 2, "car": "RumiCar", "scenario": scenario}, f, ensure_ascii=False)


def run(args, **kw):
    print("+", " ".join(args), flush=True)
    return subprocess.run(args, check=True, text=True, capture_output=True, **kw)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    build = os.path.abspath(sys.argv[1])
    work = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="rumicar-tools-")
    sessions = os.path.join(work, "sessions")
    s1, s2, s3 = (os.path.join(sessions, n) for n in ("RumiCar_sig1", "RumiCar_sig2", "RumiCar_walls"))
    write_session(s1, [("walls", 20), ("red", 40), ("green", 20), ("walls", 10)], "信号機ありケース1", 1)
    write_session(s2, [("walls", 10), ("red", 30), ("green", 15), ("walls", 10)], "信号機ありケース1", 2)
    write_session(s3, [("walls", 40)], "壁のみの走行データ", 3)

    pack = os.path.join(work, "cam-signal-0.1.0")
    py = sys.executable
    out = run([py, "-I", os.path.join(TOOLS, "train_pack.py"), "train", pack, "--parent", os.path.join(ROOT, "packs", "cam-base"),
               "--sessions", s1, s3, "--validate", s2, "--negatives-from-scenario", "壁のみの走行データ",
               "--detector", "traffic_light_red", "--detector", "traffic_light_green", "--id", "cam-signal", "--version", "0.1.0",
               "--iterations", "300"]).stdout
    print(out)
    assert "2 detectors" in out
    run([py, "-I", os.path.join(TOOLS, "make_manifest.py"), "verify", pack])
    with open(os.path.join(pack, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    assert manifest["capabilities"] == ["walls", "traffic_light"], manifest["capabilities"]
    assert manifest["parent"] == "cam-base-0.1.0" and "信号機ありケース1" in manifest["scenarios"]
    with open(os.path.join(pack, "training.json"), encoding="utf-8") as f:
        report = json.load(f)
    for name in ("traffic_light_red", "traffic_light_green"):
        v = report["detectors"][name]["validate"]
        assert v["recall"] >= 0.9, (name, v)

    out = run([py, "-I", os.path.join(TOOLS, "train_pack.py"), "replay", pack, s2]).stdout
    print(out)
    assert "traffic_light_red: active in 1 interval" in out
    assert "traffic_light_green: active in 1 interval" in out

    logic_dir = os.path.join(build, "logic", "stage")
    logic_dir = os.path.join(logic_dir, sorted(d for d in os.listdir(logic_dir) if d.startswith("cam_drive-"))[0])
    replay = os.path.join(build, "core", "rumicar-replay")
    out_path = os.path.join(work, "replay.jsonl")
    res = run([replay, logic_dir, pack, s2, "--out", out_path])
    print(res.stderr)
    states = [json.loads(l)["state_name"] for l in open(out_path, encoding="utf-8")]
    seq = [s for i, s in enumerate(states) if i == 0 or states[i - 1] != s]
    print("state sequence:", " -> ".join(seq))
    assert seq == ["CRUISE", "SIGNAL_APPROACH", "SIGNAL_STOP", "SIGNAL_RESUME"] or seq == ["CRUISE", "SIGNAL_STOP", "SIGNAL_RESUME"], seq
    rows = [json.loads(l) for l in open(out_path, encoding="utf-8")]
    stop_rows = [r for r in rows if r["state_name"] == "SIGNAL_STOP"]
    assert stop_rows[-1]["drive_permille"] == 0 and stop_rows[-1]["flags"] & 1, stop_rows[-1]
    assert rows[-1]["drive_permille"] > 0
    assert all(r["rc"] == 0 for r in rows)
    print("tools smoke test passed:", work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
