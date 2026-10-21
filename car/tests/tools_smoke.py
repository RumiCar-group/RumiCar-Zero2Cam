#!/usr/bin/env python3
"""End-to-end check of the learning tools against the real plug-in:

  synthetic labelled sessions (a red light that grows, then green; a vehicle ahead;
  an intersection opening to the left; walls only)
  -> train_pack.py train            (Python: features, logistic regression, vision.bin, manifest)
  -> make_manifest.py verify
  -> train_pack.py replay           (Python perception)
  -> rumicar-replay                 (the actual libcam_drive.so with the pack, C++)

and asserts that the car approaches, stops at the red light and resumes on
green; and, with a second pack, that it follows the vehicle, passes it once it
is parked, and stops at the intersection.  Run by CI after the build; needs
numpy and Pillow.

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
    elif kind == "vehicle":
        # another RumiCar ahead in our lane: a dark blue body low in the centre
        x0, y0 = 130 + int(rng.integers(-6, 7)), 150 + int(rng.integers(-6, 7))
        d.rectangle((x0, y0, x0 + 60, y0 + 50), fill=(30, 40, 200))
        d.rectangle((x0 + 5, y0 + 35, x0 + 15, y0 + 42), fill=(230, 40, 40))   # tail lights
        d.rectangle((x0 + 45, y0 + 35, x0 + 55, y0 + 42), fill=(230, 40, 40))
        box = [x0, y0, x0 + 61, y0 + 51]
    elif kind == "xleft":
        # an intersection ahead with a road to the left: the left wall ends, light comes in
        d.rectangle((0, 70 + int(rng.integers(-5, 6)), 60, 170), fill=(235, 235, 235))
    else:
        for _ in range(3):
            x0 = int(rng.integers(0, W - 40))
            y0 = int(rng.integers(0, H - 40))
            d.rectangle((x0, y0, x0 + 40, y0 + 40), fill=(70 + int(rng.integers(0, 40)),) * 3)
        d.ellipse((20, 20, 50, 50), fill=(40, 60, 230))
    return img, box


OPEN = (8190, 8190, 8190)


def write_session(path, plan, scenario, seed):
    """plan: (kind, frames) or (kind, frames, (L, C, R) ToF readings) entries, 50 ms apart."""
    rng = np.random.default_rng(seed)
    os.makedirs(os.path.join(path, "frames"), exist_ok=True)
    meta, labels = [], []
    idx, t = 0, 5_000_000
    for entry in plan:
        kind, n = entry[0], entry[1]
        tof = list(entry[2]) if len(entry) > 2 else list(OPEN)
        for k in range(n):
            idx += 1
            img, box = make_frame(rng, kind, k, n)
            name = f"{idx:06d}.jpg"
            img.save(os.path.join(path, "frames", name), "JPEG", quality=80)
            hdr = {"seq": idx, "t_us": t, "tof_mm": tof, "steer": 0, "drive_permille": 900, "brake": False,
                   "state": 0, "state_name": "CRUISE", "flags": 0, "mode": "auto", "estop": False, "logic_steer": 0,
                   "logic_drive_permille": 900, "logic_name": "tof_drive", "logic_version": "0.2.0", "pack_id": "base-tof",
                   "pack_version": "0.1.1", "battery_mv": 0, "throttled": 0, "image_w": W, "image_h": H, "image_seq": idx,
                   "image_t_us": t + 100}
            meta.append({"index": idx, "image": name, "received": "x", "header": hdr})
            if kind == "red":
                labels.append({"index": idx, "objects": [{"name": "traffic_light_red", "box": box}]})
            elif kind == "green":
                labels.append({"index": idx, "objects": [{"name": "traffic_light_green", "box": box}]})
            elif kind == "vehicle":
                labels.append({"index": idx, "objects": [{"name": "vehicle", "box": box}]})
            elif kind == "xleft":
                labels.append({"index": idx, "scene": ["intersection_left"]})
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
    logic_dir = os.path.join(logic_dir, sorted(d for d in os.listdir(logic_dir) if d.startswith("cam_drive-"))[-1])
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
    with open(os.path.join(pack, "params.json"), encoding="utf-8") as f:
        params = json.load(f)
    assert "vehicle" not in params and "intersection" not in params, "behaviours without detectors must stay off"

    # --- a second pack: a vehicle and an intersection learned on top of the traffic lights -------------------
    # Every detector is learned again from all the sessions: the new scenes are negatives
    # for the old detectors (a vehicle's red tail lights are not a red light).
    s4, s5 = (os.path.join(sessions, n) for n in ("RumiCar_town1", "RumiCar_town2"))
    write_session(s4, [("walls", 20), ("vehicle", 40, (8190, 180, 8190)), ("walls", 10), ("xleft", 30, (120, 8190, 120)),
                       ("walls", 10, (120, 8190, 120))], "車両と交差点ケース1", 4)
    # a scripted drive: following a vehicle that stops (parked) with both sides open, then an intersection on the left
    write_session(s5, [("walls", 20), ("vehicle", 4, (8190, 240, 8190)), ("vehicle", 4, (8190, 200, 8190)),
                       ("vehicle", 70, (8190, 150, 8190)), ("walls", 10, (60, 8190, 8190)), ("walls", 30),
                       ("xleft", 20, (120, 8190, 120)), ("xleft", 25, (8190, 8190, 120)), ("walls", 20, (120, 8190, 120))],
                  "車両と交差点ケース1", 5)
    pack2 = os.path.join(work, "cam-town-0.1.0")
    out = run([py, "-I", os.path.join(TOOLS, "train_pack.py"), "train", pack2, "--parent", pack,
               "--sessions", s1, s4, s3, "--validate", s2, s5, "--negatives-from-scenario", "壁のみの走行データ",
               "--detector", "traffic_light_red", "--detector", "traffic_light_green",
               "--detector", "vehicle", "--detector", "intersection_left:global", "--id", "cam-town", "--version", "0.1.0",
               "--intersection", "turn=straight", "--iterations", "300"]).stdout
    print(out)
    assert "behaviours on: signal, vehicle, intersection" in out, out
    with open(os.path.join(pack2, "training.json"), encoding="utf-8") as f:
        report = json.load(f)
    for name in ("traffic_light_red", "traffic_light_green", "vehicle", "intersection_left"):
        v = report["detectors"][name]["validate"]
        assert v["recall"] >= 0.9, (name, v)
        assert name.startswith("traffic") or v["precision"] >= 0.9, (name, v)
    run([py, "-I", os.path.join(TOOLS, "make_manifest.py"), "verify", pack2])
    with open(os.path.join(pack2, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    assert manifest["capabilities"] == ["walls", "traffic_light", "vehicle", "intersection"], manifest["capabilities"]
    assert manifest["parent"] == "cam-signal-0.1.0"
    with open(os.path.join(pack2, "params.json"), encoding="utf-8") as f:
        params = json.load(f)
    assert params["vehicle"]["detector"] == "vehicle" and params["intersection"]["left_detector"] == "intersection_left"
    assert params["intersection"]["right_detector"] == "" and params["signal"]["red_detector"] == "traffic_light_red"
    out = run([py, "-I", os.path.join(TOOLS, "train_pack.py"), "show", pack2]).stdout
    print(out)
    assert "behaviours:   signal" in out and "vehicle (vehicle" in out and "intersection (intersection_left / -" in out, out
    out = run([py, "-I", os.path.join(TOOLS, "train_pack.py"), "replay", pack2, s5]).stdout
    print(out)
    assert "vehicle: active in 1 interval" in out and "intersection_left: active in 1 interval" in out, out
    assert "traffic_light_red: active in 0 interval" in out and "traffic_light_green: active in 0 interval" in out, out

    # The proposals that help labelling these scenes: frames with something close ahead, and
    # the stretches before a side opened up (the lines are labels.jsonl lines).
    out = run([py, "-I", os.path.join(TOOLS, "train_pack.py"), "propose", s5, "--detector", "vehicle"]).stdout
    print(out)
    near = [json.loads(l) for l in open(os.path.join(s5, "proposals_vehicle.jsonl"), encoding="utf-8")]
    assert len(near) == 78 and near[0]["objects"][0]["name"] == "vehicle" and near[0]["tof_mm"] == 240, (len(near), near[:1])
    out = run([py, "-I", os.path.join(TOOLS, "train_pack.py"), "propose", s5, "--detector", "intersection_left"]).stdout
    print(out)
    stretches = [json.loads(l) for l in open(os.path.join(s5, "proposals_intersection_left.jsonl"), encoding="utf-8")]
    assert [(x["from"], x["to"]) for x in stretches] == [(69, 109), (119, 159)] and stretches[1]["scene"] == ["intersection_left"], stretches

    out_path = os.path.join(work, "replay_town.jsonl")
    res = run([replay, logic_dir, pack2, s5, "--out", out_path])
    print(res.stderr)
    rows = [json.loads(l) for l in open(out_path, encoding="utf-8")]
    states = [r["state_name"] for r in rows]
    seq = [s for i, s in enumerate(states) if i == 0 or states[i - 1] != s]
    print("state sequence:", " -> ".join(seq))
    cam = [s for s in seq if s.startswith(("FOLLOW", "OVERTAKE", "INTERSECTION"))]
    assert cam == ["FOLLOW", "OVERTAKE_OUT", "OVERTAKE_PASS", "OVERTAKE_IN", "INTERSECTION_APPROACH", "INTERSECTION_STOP"], cam
    follow_rows = [r for r in rows if r["state_name"] == "FOLLOW"]
    assert follow_rows[-1]["drive_permille"] == 0 and follow_rows[-1]["flags"] & 1, follow_rows[-1]
    assert all(r["steer"] == 1 for r in rows if r["state_name"] == "OVERTAKE_OUT")   # both sides open: pass on the right
    assert all(r["steer"] == -1 for r in rows if r["state_name"] == "OVERTAKE_IN")
    istop = [r for r in rows if r["state_name"] == "INTERSECTION_STOP"]
    assert istop[-1]["drive_permille"] == 0 and istop[-1]["flags"] & 1, istop[-1]
    assert seq[-1] == "CRUISE" and rows[-1]["drive_permille"] > 0
    assert all(r["rc"] == 0 for r in rows)
    print("tools smoke test passed:", work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
