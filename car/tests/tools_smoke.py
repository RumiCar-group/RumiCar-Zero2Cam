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
is parked, and stops at the intersection.  The sessions are those of cars of
ABI 5 (ToF readings up to 2000 mm), ABI 4 and ABI 3 (cut off at 200 mm by the
car): the same drive recorded by an ABI 5 and an ABI 4 car replays to the same
decisions with the default conf_max_mm (200), and a pack whose conf_max_mm is
wider uses the longer readings.  Run by CI after the build; needs numpy and
Pillow.

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


# What the three sensors give [mm] where the course is open: walls far away, beyond the
# 200 mm at which a car of ABI 3 / 4 cut its readings off (left, centre, right).
LF, CF, RF = 900, 1500, 700
FAR = (LF, CF, RF)
TOF_MAX_MM = {3: 200, 4: 200, 5: 2000}   # the largest distance a car of each ABI records; beyond it -3
CAR_OF_ABI = {3: ("0.3.0", "0.2.0"), 4: ("0.4.0", "0.3.0"), 5: ("0.6.0", "0.3.0")}   # tof_drive / base-tof versions in the header


def recorded_tof(sensor_mm, abi):
    """The RC_read value a car of `abi` records for what a sensor gives (core/src/main.cpp to_tof_value)."""
    return -3 if sensor_mm > TOF_MAX_MM.get(abi, 2000) else sensor_mm


def write_session(path, plan, scenario, seed, abi=5):
    """plan: (kind, frames) or (kind, frames, (L, C, R)) entries, 50 ms apart.  L / C / R are
    what the sensors give [mm] (FAR when left out); the session records what a car of `abi`
    makes of them: abi 5 (core 0.7.0) up to 2000 mm, abi 3 / 4 cut off at 200 mm (-3 above).
    abi 4 and 5 record the steering strength (steer_pwm, logic_steer_pwm); abi 3 (core 0.4.x)
    did not.  The recorded steering is RIGHT, at strength 128 from abi 4."""
    rng = np.random.default_rng(seed)
    os.makedirs(os.path.join(path, "frames"), exist_ok=True)
    meta, labels = [], []
    idx, t = 0, 5_000_000
    logic_version, pack_version = CAR_OF_ABI.get(abi, ("0.6.0", "0.3.0"))
    for entry in plan:
        kind, n = entry[0], entry[1]
        tof = [recorded_tof(v, abi) for v in (entry[2] if len(entry) > 2 else FAR)]
        for k in range(n):
            idx += 1
            img, box = make_frame(rng, kind, k, n)
            name = f"{idx:06d}.jpg"
            img.save(os.path.join(path, "frames", name), "JPEG", quality=80)
            hdr = {"abi": abi, "seq": idx, "t_us": t, "tof_mm": tof, "steer": 2, "drive_dir": 2, "drive_pwm": 230,
                   "state": 2, "state_name": "TURN_RIGHT", "flags": 0, "mode": "auto", "estop": False, "logic_steer": 2,
                   "logic_drive_dir": 2, "logic_drive_pwm": 230, "logic_name": "tof_drive",
                   "logic_version": logic_version, "pack_id": "base-tof",
                   "pack_version": pack_version, "battery_mv": 0, "throttled": 0, "image_w": W, "image_h": H,
                   "image_seq": idx, "image_t_us": t + 100}
            if abi >= 4:
                hdr["steer_pwm"] = 128
                hdr["logic_steer_pwm"] = 128
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
    # recorded by a core 0.4.x (ABI 3: ToF cut off at 200 mm, no steering strength): still read by the tools and replayed
    write_session(s2, [("walls", 10), ("red", 30), ("green", 15), ("walls", 10)], "信号機ありケース1", 2, abi=3)
    # recorded by a core 0.5.x / 0.6.x (ABI 4: ToF cut off at 200 mm): still read by the tools
    write_session(s3, [("walls", 40)], "壁のみの走行データ", 3, abi=4)
    with open(os.path.join(s1, "meta.jsonl"), encoding="utf-8") as f:
        assert json.loads(f.readline())["header"]["tof_mm"] == [LF, CF, RF]      # ABI 5: the distances as they are
    with open(os.path.join(s3, "meta.jsonl"), encoding="utf-8") as f:
        assert json.loads(f.readline())["header"]["tof_mm"] == [-3, -3, -3]      # ABI 4: beyond 200 mm is -3

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
    assert manifest["parent"] == "cam-base-0.3.0" and "信号機ありケース1" in manifest["scenarios"]
    # Every distance of its params.json is within 200 mm: the pack also runs on cam_drive of ABI 4.
    assert manifest["logic"] == {"name": "cam_drive", "min_abi": 4}, manifest["logic"]
    assert "min_abi 4" in out and "needs a car whose core is 0.7.0" not in out, out
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
    assert stop_rows[-1]["drive_dir"] == 3 and stop_rows[-1]["drive_pwm"] == 0, stop_rows[-1]   # BRAKE
    assert rows[-1]["drive_dir"] == 2 and rows[-1]["drive_pwm"] > 0
    assert all(r["rc"] == 0 for r in rows)
    # RC_steer's strength in the replay: CENTER carries 0, the ABI 3 recording's steering counts as full (255).
    assert all(r["steer_pwm"] == 0 for r in rows if r["steer"] == 1), [r for r in rows if r["steer"] == 1][:1]
    assert all(r["recorded"]["steer"] == 2 and r["recorded"]["steer_pwm"] == 255 for r in rows), rows[0]["recorded"]
    with open(os.path.join(pack, "params.json"), encoding="utf-8") as f:
        params = json.load(f)
    assert "vehicle" not in params and "intersection" not in params, "behaviours without detectors must stay off"
    # The ToF section comes from the parent, steering strengths included.
    assert all(params["tof"][k] == 255 for k in ("turn_steer_pwm", "side_steer_pwm", "recover_steer_pwm")), params["tof"]

    # A recording of a car older than core 0.4.0 (no "abi", other values) is refused by both tools,
    # and so is one of a car newer than these tools (an ABI they do not know).
    for other in (2, 6):
        odd = os.path.join(sessions, f"RumiCar_abi{other}")
        write_session(odd, [("walls", 5)], "壁のみの走行データ", 6, abi=other)
        r2 = subprocess.run([replay, logic_dir, pack, odd], text=True, capture_output=True)
        assert r2.returncode == 3 and f"ABI {other}" in r2.stderr, (r2.returncode, r2.stderr)
        r2 = subprocess.run([py, "-I", os.path.join(TOOLS, "train_pack.py"), "replay", pack, odd], text=True, capture_output=True)
        assert r2.returncode != 0 and f"ABI {other}" in r2.stderr, (r2.returncode, r2.stderr)

    # --- a second pack: a vehicle and an intersection learned on top of the traffic lights -------------------
    # Every detector is learned again from all the sessions: the new scenes are negatives
    # for the old detectors (a vehicle's red tail lights are not a red light).
    s4, s5 = (os.path.join(sessions, n) for n in ("RumiCar_town1", "RumiCar_town2"))
    write_session(s4, [("walls", 20), ("vehicle", 40, (LF, 180, RF)), ("walls", 10), ("xleft", 30, (120, CF, 120)),
                       ("walls", 10, (120, CF, 120))], "車両と交差点ケース1", 4)
    # a scripted drive: following a vehicle that stops (parked) with both sides open, then an intersection on the left
    town_drive = [("walls", 20), ("vehicle", 4, (LF, 195, RF)), ("vehicle", 4, (LF, 175, RF)),
                  ("vehicle", 70, (LF, 150, RF)), ("walls", 10, (60, CF, RF)), ("walls", 30),
                  ("xleft", 20, (120, CF, 120)), ("xleft", 25, (LF, CF, 120)), ("walls", 20, (120, CF, 120))]
    write_session(s5, town_drive, "車両と交差点ケース1", 5)
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
    assert len(near) == 78 and near[0]["objects"][0]["name"] == "vehicle" and near[0]["tof_mm"] == 195, (len(near), near[:1])
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
    assert follow_rows[-1]["drive_dir"] == 3 and follow_rows[-1]["drive_pwm"] == 0, follow_rows[-1]   # BRAKE
    assert all(r["steer"] == 2 and r["steer_pwm"] == 255 for r in rows if r["state_name"] == "OVERTAKE_OUT")   # pass on the RIGHT (2), full
    assert all(r["steer"] == 0 and r["steer_pwm"] == 255 for r in rows if r["state_name"] == "OVERTAKE_IN")    # back to the LEFT (0), full
    assert all(r["recorded"]["steer_pwm"] == 128 for r in rows), rows[0]["recorded"]   # recorded from ABI 4 on: its strength
    istop = [r for r in rows if r["state_name"] == "INTERSECTION_STOP"]
    assert istop[-1]["drive_dir"] == 3 and istop[-1]["drive_pwm"] == 0, istop[-1]
    assert seq[-1] == "CRUISE" and rows[-1]["drive_dir"] == 2 and rows[-1]["drive_pwm"] > 0
    assert all(r["rc"] == 0 for r in rows)
    assert rows[0]["tof_mm"] == [LF, CF, RF], rows[0]["tof_mm"]   # the recording of an ABI 5 car: walls beyond 200 mm as distances

    # --- the same drive recorded by a car of ABI 4 (ToF cut off at 200 mm) ------------------------------------
    # The pack trusts the ToF up to conf_max_mm 200, so the car's cut-off made no difference to it: every
    # frame replays to the same decision, and the proposals for labelling are the same.
    s5_abi4 = os.path.join(sessions, "RumiCar_town2_abi4")
    write_session(s5_abi4, town_drive, "車両と交差点ケース1", 5, abi=4)
    out4_path = os.path.join(work, "replay_town_abi4.jsonl")
    run([replay, logic_dir, pack2, s5_abi4, "--out", out4_path])
    rows4 = [json.loads(l) for l in open(out4_path, encoding="utf-8")]
    decision = ("rc", "state", "state_name", "steer", "steer_pwm", "drive_dir", "drive_pwm", "flags", "aux_format", "aux")
    assert len(rows4) == len(rows) and rows4[0]["tof_mm"] == [-3, -3, -3], (len(rows4), rows4[0]["tof_mm"])
    differ = [r["index"] for r, r4 in zip(rows, rows4) if any(r.get(k) != r4.get(k) for k in decision)]
    assert not differ, f"decisions differ between the ABI 5 and the ABI 4 recording at frames {differ[:10]}"
    assert sum(1 for r, r4 in zip(rows, rows4) if r["tof_mm"] != r4["tof_mm"]) > 100   # the recordings do differ beyond 200 mm
    for det, name in (("vehicle", "proposals_vehicle.jsonl"), ("intersection_left", "proposals_intersection_left.jsonl")):
        run([py, "-I", os.path.join(TOOLS, "train_pack.py"), "propose", s5_abi4, "--detector", det])
        with open(os.path.join(s5, name), encoding="utf-8") as fa, open(os.path.join(s5_abi4, name), encoding="utf-8") as fb:
            assert fa.read() == fb.read(), f"{name} differs between the ABI 5 and the ABI 4 recording"

    # --- a pack that trusts the ToF beyond 200 mm (needs a car of ABI 5) ----------------------------------------
    # conf_max_mm and the thresholds are widened by hand in a copy of the pack (README 6.2), which gets
    # --min-abi 5; a pack trained from it inherits the values, and train_pack.py gives it min_abi 5 by itself.
    wide_parent = os.path.join(work, "cam-wide-0.1.0")
    os.makedirs(wide_parent, exist_ok=True)
    with open(os.path.join(pack2, "params.json"), encoding="utf-8") as f:
        wide = json.load(f)
    wide["tof"].update({"conf_max_mm": 500, "d_max_mm": 450, "d_turn_exit_mm": 400})
    wide["vehicle"].update({"follow_gap_mm": 300, "follow_band_mm": 100})
    with open(os.path.join(wide_parent, "params.json"), "w", encoding="utf-8") as f:
        json.dump(wide, f, indent=2, ensure_ascii=False)
        f.write("\n")
    with open(os.path.join(pack2, "vision.bin"), "rb") as fi, open(os.path.join(wide_parent, "vision.bin"), "wb") as fo:
        fo.write(fi.read())
    run([py, "-I", os.path.join(TOOLS, "make_manifest.py"), "pack", wide_parent, "--id", "cam-wide", "--version", "0.1.0",
         "--logic", "cam_drive", "--min-abi", "5", "--capability", "walls", "--capability", "traffic_light", "--capability", "vehicle",
         "--capability", "intersection", "--parent", "cam-town-0.1.0",
         "--description", "cam-town 0.1.0 with the ToF trusted up to 500 mm and a gap of 300 mm behind a vehicle"])
    run([py, "-I", os.path.join(TOOLS, "make_manifest.py"), "verify", wide_parent])
    pack3 = os.path.join(work, "cam-wide-0.2.0")
    out = run([py, "-I", os.path.join(TOOLS, "train_pack.py"), "train", pack3, "--parent", wide_parent,
               "--sessions", s1, s4, s3, "--validate", s2, s5, "--negatives-from-scenario", "壁のみの走行データ",
               "--detector", "traffic_light_red", "--detector", "traffic_light_green",
               "--detector", "vehicle", "--detector", "intersection_left:global", "--id", "cam-wide", "--version", "0.2.0",
               "--iterations", "300"]).stdout
    print(out)
    assert "min_abi 5" in out and "needs a car whose core is 0.7.0 or later" in out, out
    run([py, "-I", os.path.join(TOOLS, "make_manifest.py"), "verify", pack3])
    with open(os.path.join(pack3, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    assert manifest["logic"] == {"name": "cam_drive", "min_abi": 5} and manifest["parent"] == "cam-wide-0.1.0", manifest
    with open(os.path.join(pack3, "params.json"), encoding="utf-8") as f:
        params = json.load(f)
    assert params["tof"]["conf_max_mm"] == 500 and params["vehicle"]["follow_gap_mm"] == 300, params
    # A vehicle 350 mm, then 300 mm ahead, recorded by a car of ABI 5.  The wide pack slows down and stops at
    # its gap of 300 mm; the default pack (conf_max_mm 200) does not see that far and keeps following at follow_v.
    s6 = os.path.join(sessions, "RumiCar_gap")
    write_session(s6, [("walls", 20), ("vehicle", 30, (LF, 350, RF)), ("vehicle", 30, (LF, 300, RF)), ("walls", 20)],
                  "車両と交差点ケース1", 7)
    wide_path, default_path = os.path.join(work, "replay_gap_wide.jsonl"), os.path.join(work, "replay_gap_default.jsonl")
    res = run([replay, logic_dir, pack3, s6, "--out", wide_path])
    print(res.stderr)
    run([replay, logic_dir, pack2, s6, "--out", default_path])
    wide_rows = [json.loads(l) for l in open(wide_path, encoding="utf-8")]
    default_rows = [json.loads(l) for l in open(default_path, encoding="utf-8")]
    follow_v = params["vehicle"]["follow_v"]
    at_350, at_300 = wide_rows[49], wide_rows[79]      # the last frame of each stretch
    assert at_350["tof_mm"][1] == 350 and at_300["tof_mm"][1] == 300, (at_350["tof_mm"], at_300["tof_mm"])
    assert at_350["state_name"] == "FOLLOW" and at_350["drive_dir"] == 2 and abs(at_350["drive_pwm"] - follow_v / 2) <= 1, at_350
    assert at_300["state_name"] == "FOLLOW" and at_300["drive_dir"] == 3 and at_300["drive_pwm"] == 0, at_300   # BRAKE at the gap
    assert default_rows[49]["state_name"] == "FOLLOW" and default_rows[49]["drive_pwm"] == follow_v, default_rows[49]
    assert default_rows[79]["state_name"] == "FOLLOW" and default_rows[79]["drive_pwm"] == follow_v, default_rows[79]
    assert all(r["rc"] == 0 for r in wide_rows + default_rows)
    print("tools smoke test passed:", work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
