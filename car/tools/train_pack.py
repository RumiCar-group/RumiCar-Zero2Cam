#!/usr/bin/env python3
"""Make and check cam_drive packs from recorded sessions (docs 4.5, README 7章).

Subcommands (run from the repository root or anywhere with car/tools on the path):

  features <session>...                    compute and cache the image features of sessions
  propose  <session> --detector NAME        candidates to label from: red / green blobs (traffic lights),
                                            frames with something close ahead (vehicle), the seconds before
                                            a side opened up (intersection_left / intersection_right)
  train    <out-dir> --parent PACK --sessions S... [--validate S...] --detector NAME[:local|global]...
                                            learn detectors from labels.jsonl and write the pack; the
                                            behaviours (signal, vehicle, intersection) are switched on
                                            for the detectors the pack has
  eval     <pack-dir> --sessions S...       precision / recall of a pack's detectors on labelled sessions
  replay   <pack-dir> <session> [--out F]   per-frame detector outputs on a session (perception only;
                                            rumicar-replay runs the real .so for the whole behaviour)
  show     <pack-dir>                       what a pack contains

Labels live in <session>/labels.jsonl, one JSON object per line:
  {"index": 123, "objects": [{"name": "traffic_light_red", "box": [x0, y0, x1, y1]}]}   pixel box, x1/y1 exclusive
  {"index": 456, "objects": [{"name": "vehicle", "box": [x0, y0, x1, y1]}]}              a vehicle ahead (its body)
  {"from": 300, "to": 380, "scene": ["intersection_left"]}                                frames 300..380 (inclusive)
  {"index": 200, "negative": true}                                                        nothing of interest here
Boxes train Local detectors (where in the image), scenes train Global ones
(the whole view: NAME:global).  Other keys on a line are ignored, so the lines
`propose` writes can be copied as they are.  A frame with no label line is not
used for training.  Sessions given with --negatives SESSION (or whose scenario
matches --negatives-from-scenario) are negatives for every detector in all
their frames.

Needs numpy and Pillow.  Standard library otherwise.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_manifest  # noqa: E402
import rumicar_vision as rv  # noqa: E402

TOOL_VERSION = "2"


# --- sessions ----------------------------------------------------------------------------------------

class Session:
    """A recorded session: its meta lines, scenario, and (cached) features."""

    def __init__(self, path: str):
        self.path = os.path.abspath(path)
        self.name = os.path.basename(self.path.rstrip("/\\"))
        meta_path = os.path.join(self.path, "meta.jsonl")
        if not os.path.isfile(meta_path):
            raise SystemExit(f"error: {path} is not a session folder (meta.jsonl is missing)")
        self.lines = []
        with open(meta_path, encoding="utf-8") as f:
            for text in f:
                text = text.strip()
                if not text:
                    continue
                try:
                    line = json.loads(text)
                except json.JSONDecodeError:
                    continue
                if isinstance(line, dict) and isinstance(line.get("header"), dict):
                    self.lines.append(line)
        self.scenario = ""
        sj = os.path.join(self.path, "session.json")
        if os.path.isfile(sj):
            try:
                with open(sj, encoding="utf-8") as f:
                    self.scenario = str(json.load(f).get("scenario", "") or "")
            except (json.JSONDecodeError, OSError):
                pass
        self.image_lines = [l for l in self.lines if l.get("image") and l["header"].get("image_w", 0) > 0]
        self.labels = self.load_labels()

    def load_labels(self) -> dict:
        """index -> {"objects": [...], "scene": set(), "negative": bool}"""
        out: dict = {}
        path = os.path.join(self.path, "labels.jsonl")
        if not os.path.isfile(path):
            return out

        def entry(i):
            return out.setdefault(i, {"objects": [], "scene": set(), "negative": False})

        with open(path, encoding="utf-8") as f:
            for n, text in enumerate(f, 1):
                text = text.strip()
                if not text or text.startswith("#"):
                    continue
                try:
                    lab = json.loads(text)
                except json.JSONDecodeError as e:
                    raise SystemExit(f"error: {path}:{n}: {e}")
                if "from" in lab and "to" in lab:
                    indices = range(int(lab["from"]), int(lab["to"]) + 1)
                elif "index" in lab:
                    indices = [int(lab["index"])]
                else:
                    raise SystemExit(f"error: {path}:{n}: needs 'index' or 'from'/'to'")
                for i in indices:
                    e = entry(i)
                    for obj in lab.get("objects", []) or []:
                        name, box = obj.get("name"), obj.get("box")
                        if not (isinstance(name, str) and rv.valid_name(name) and isinstance(box, list) and len(box) == 4):
                            raise SystemExit(f"error: {path}:{n}: bad object {obj}")
                        e["objects"].append((name, [int(v) for v in box]))
                    for s in lab.get("scene", []) or []:
                        if not (isinstance(s, str) and rv.valid_name(s)):
                            raise SystemExit(f"error: {path}:{n}: bad scene name {s!r}")
                        e["scene"].add(s)
                    if lab.get("negative"):
                        e["negative"] = True
        return out

    def image_size(self):
        h = self.image_lines[0]["header"] if self.image_lines else {}
        return int(h.get("image_w", 0)), int(h.get("image_h", 0))

    def frame_path(self, line) -> str:
        return os.path.join(self.path, "frames", line["image"])

    def features(self, cfg: rv.FeatureConfig, log=print) -> tuple:
        """(indices int array, features int32 array (n, dims)), cached in features_v1.npz."""
        key = hashlib.sha256(json.dumps(cfg.__dict__, sort_keys=True).encode()).hexdigest()[:16]
        cache = os.path.join(self.path, "features_v1.npz")
        if os.path.isfile(cache):
            try:
                z = np.load(cache)
                if str(z["key"]) == key and z["features"].shape[0] == len(self.image_lines):
                    return z["indices"], z["features"]
            except (OSError, KeyError, ValueError):
                pass
        indices, feats = [], []
        t0 = time.time()
        for n, line in enumerate(self.image_lines):
            path = self.frame_path(line)
            if not os.path.isfile(path):
                continue
            rgb = rv.load_rgb(path)
            if cfg.validate(rgb.shape[1], rgb.shape[0]):
                raise SystemExit(f"error: {path}: {cfg.validate(rgb.shape[1], rgb.shape[0])}")
            feats.append(rv.extract_features(rgb, cfg))
            indices.append(int(line["index"]))
            if (n + 1) % 200 == 0:
                log(f"  {self.name}: {n + 1}/{len(self.image_lines)} frames")
        indices = np.array(indices, dtype=np.int64)
        feats = np.array(feats, dtype=np.int32).reshape(len(indices), cfg.dims)
        try:
            np.savez_compressed(cache, key=key, indices=indices, features=feats)
        except OSError:
            pass
        log(f"  {self.name}: {len(indices)} frames with images, features in {time.time() - t0:.1f} s")
        return indices, feats


def load_sessions(paths) -> list:
    return [Session(p) for p in paths]


# --- labels -> targets ---------------------------------------------------------------------------------

def cell_rects(cfg: rv.FeatureConfig, w: int, h: int):
    """Full-resolution rectangle (x0, y0, x1, y1) of every cell, row-major."""
    sw, sh = w // cfg.downscale, h // cfg.downscale
    cw, ch = sw // cfg.grid_x, sh // cfg.grid_y
    s = cfg.downscale
    rects = []
    for j in range(cfg.grid_y):
        for i in range(cfg.grid_x):
            rects.append((i * cw * s, j * ch * s, (i + 1) * cw * s, (j + 1) * ch * s))
    return rects


def overlap_fraction(rect, box) -> float:
    x0, y0, x1, y1 = rect
    bx0, by0, bx1, by1 = box
    ix = max(0, min(x1, bx1) - max(x0, bx0))
    iy = max(0, min(y1, by1) - max(y0, by0))
    area = (x1 - x0) * (y1 - y0)
    return (ix * iy) / area if area > 0 else 0.0


def local_targets(session: Session, indices, cfg, w, h, name, negative_session, overlap_min, ignore_min):
    """Per cell: +1 positive, 0 negative, -1 ignore (unlabelled frame or ambiguous cell).  Shape (n, cells)."""
    rects = cell_rects(cfg, w, h)
    n_cells = len(rects)
    t = np.full((len(indices), n_cells), -1, dtype=np.int8)
    for row, idx in enumerate(indices):
        lab = session.labels.get(int(idx))
        if lab is None:
            if negative_session:
                t[row, :] = 0
            continue
        t[row, :] = 0
        for (oname, box) in lab["objects"]:
            if oname != name:
                continue
            for c, rect in enumerate(rects):
                f = overlap_fraction(rect, box)
                if f >= overlap_min:
                    t[row, c] = 1
                elif f >= ignore_min and t[row, c] != 1:
                    t[row, c] = -1
    return t


def global_targets(session: Session, indices, name, negative_session):
    t = np.full(len(indices), -1, dtype=np.int8)
    for row, idx in enumerate(indices):
        lab = session.labels.get(int(idx))
        if lab is None:
            if negative_session:
                t[row] = 0
            continue
        t[row] = 1 if name in lab["scene"] else 0
    return t


# --- learning --------------------------------------------------------------------------------------------

def fit_logistic(x: np.ndarray, y: np.ndarray, weight: np.ndarray, l2: float, iters: int, lr: float, log=print):
    """Weighted L2-regularised logistic regression with Adam, full batch.  Returns (w, b)."""
    n, d = x.shape
    w = np.zeros(d, dtype=np.float64)
    b = 0.0
    m_w = np.zeros(d); v_w = np.zeros(d); m_b = 0.0; v_b = 0.0
    beta1, beta2, eps = 0.9, 0.999, 1e-8
    xs = x.astype(np.float64)
    ys = y.astype(np.float64)
    ws = weight.astype(np.float64) / max(weight.sum(), 1e-9)
    for it in range(1, iters + 1):
        z = xs @ w + b
        p = 1.0 / (1.0 + np.exp(-z))
        g = (p - ys) * ws
        gw = xs.T @ g + l2 * w
        gb = g.sum()
        m_w = beta1 * m_w + (1 - beta1) * gw; v_w = beta2 * v_w + (1 - beta2) * gw * gw
        m_b = beta1 * m_b + (1 - beta1) * gb; v_b = beta2 * v_b + (1 - beta2) * gb * gb
        mw_hat = m_w / (1 - beta1 ** it); vw_hat = v_w / (1 - beta2 ** it)
        mb_hat = m_b / (1 - beta1 ** it); vb_hat = v_b / (1 - beta2 ** it)
        w -= lr * mw_hat / (np.sqrt(vw_hat) + eps)
        b -= lr * mb_hat / (np.sqrt(vb_hat) + eps)
        if it % 100 == 0 or it == iters:
            loss = -(ws * (ys * np.log(p + 1e-12) + (1 - ys) * np.log(1 - p + 1e-12))).sum() + 0.5 * l2 * (w @ w)
            log(f"    iteration {it}: loss {loss:.4f}")
    return w.astype(np.float32), float(b)


def choose_threshold(scores: np.ndarray, y: np.ndarray):
    """thr_on maximising F1 on the given scores/targets (candidates 0.3..0.9).  When several
    thresholds tie (well separated classes) the middle one is taken, so the margin to both
    sides is largest.  Returns (thr, precision, recall, f1)."""
    results = []
    for thr in np.arange(0.30, 0.91, 0.05):
        pred = scores >= thr
        tp = int((pred & (y == 1)).sum()); fp = int((pred & (y == 0)).sum()); fn = int((~pred & (y == 1)).sum())
        prec = tp / (tp + fp) if tp + fp else 0.0
        rec = tp / (tp + fn) if tp + fn else 0.0
        f1 = 2 * prec * rec / (prec + rec) if prec + rec else 0.0
        results.append((float(round(thr, 2)), prec, rec, f1))
    best_f1 = max(r[3] for r in results)
    tied = [r for r in results if r[3] >= best_f1 - 1e-9]
    return tied[len(tied) // 2]


def choose_release(thr_on: float, hysteresis: float, neg_scores: np.ndarray) -> float:
    """thr_off: thr_on - hysteresis, but never down among the negatives' scores (a detector
    that never releases is worse than one that flickers): at least halfway from the
    negatives' 99th percentile up to thr_on, and always below thr_on."""
    thr_off = thr_on - hysteresis
    if neg_scores.size:
        q = float(np.quantile(neg_scores, 0.99))
        thr_off = max(thr_off, (q + thr_on) / 2.0)
    return float(round(min(max(thr_off, 0.05), thr_on - 0.02), 2))


def frame_metrics(pack: rv.VisionPack, d: rv.Detector, feats: np.ndarray, targets: np.ndarray):
    """Per-frame precision/recall of a detector's single-frame 'present' decision.
    targets: (n, cells) for Local (frame positive if any cell positive), (n,) for Global."""
    tp = fp = fn = tn = 0
    for row in range(feats.shape[0]):
        if d.kind == rv.KIND_LOCAL:
            t = targets[row]
            if (t == -1).all():
                continue
            truth = bool((t == 1).any())
        else:
            if targets[row] == -1:
                continue
            truth = targets[row] == 1
        pred = rv.evaluate_detector(pack, d, feats[row])["present"]
        if pred and truth: tp += 1
        elif pred and not truth: fp += 1
        elif truth: fn += 1
        else: tn += 1
    prec = tp / (tp + fp) if tp + fp else 0.0
    rec = tp / (tp + fn) if tp + fn else 0.0
    return {"frames": tp + fp + fn + tn, "tp": tp, "fp": fp, "fn": fn, "tn": tn, "precision": prec, "recall": rec}


# --- subcommands ------------------------------------------------------------------------------------------

def cmd_features(a) -> int:
    cfg = rv.FeatureConfig(a.downscale, a.grid_x, a.grid_y, a.s_min, a.v_min, a.v_white)
    for s in load_sessions(a.sessions):
        s.features(cfg)
    return 0


def cmd_propose(a) -> int:
    """Candidates to label, written as labels.jsonl lines (with extra keys that say why)."""
    s = Session(a.session)
    out_path = a.out or os.path.join(s.path, f"proposals_{a.detector}.jsonl")
    if a.detector.startswith("vehicle"):
        count = propose_near_frames(s, a, out_path)
        print(f"{count} frames with something close ahead (centre ToF below {a.near_mm} mm) -> {out_path}")
        print("The box marks our lane; move/resize it onto the vehicle's body in the frames where one is, delete the other lines, "
              "and append the rest to labels.jsonl.")
    elif a.detector.startswith("intersection"):
        count = propose_side_openings(s, a, out_path)
        print(f"{count} stretches before a side opened up -> {out_path}")
        print("Look at the first and last frame of each stretch: keep the ones that show the intersection ahead, "
              "trim from/to, and append them to labels.jsonl.")
    else:
        count = propose_blobs(s, a, out_path)
        print(f"{count} candidate boxes -> {out_path}")
        print("Confirm them (look at the frames), delete the wrong lines and append the rest to labels.jsonl.")
    return 0


def propose_blobs(s: Session, a, out_path: str) -> int:
    """Colour blobs on the small image (traffic lights), one line per blob."""
    cfg = rv.FeatureConfig(a.downscale, a.grid_x, a.grid_y, a.s_min, a.v_min, a.v_white)
    want = {"traffic_light_red": 0, "traffic_light_green": 2}.get(a.detector)
    if want is None:
        want = int(a.color_class)
    count = 0
    with open(out_path, "w", encoding="utf-8") as out:
        for n, line in enumerate(s.image_lines):
            if a.every > 1 and n % a.every:
                continue
            path = s.frame_path(line)
            if not os.path.isfile(path):
                continue
            small = rv.downscale(rv.load_rgb(path), cfg.downscale)
            cls, _, _ = rv.classify(small, cfg)
            mask = cls == want
            for (x0, y0, x1, y1, px) in blobs(mask, a.min_pixels):
                box = [x0 * cfg.downscale, y0 * cfg.downscale, x1 * cfg.downscale, y1 * cfg.downscale]
                out.write(json.dumps({"index": int(line["index"]), "objects": [{"name": a.detector, "box": box}],
                                      "image": line["image"], "pixels": int(px)}, ensure_ascii=False) + "\n")
                count += 1
    return count


def tof_mm(line, i: int):
    """Raw ToF reading i of a meta line: an int, or None when invalid / no target."""
    v = line["header"].get("tof_mm")
    if not isinstance(v, list) or len(v) <= i or not isinstance(v[i], (int, float)):
        return None
    v = int(v[i])
    return v if 0 <= v < NO_TARGET_MM else None


NO_TARGET_MM = 8190


def propose_near_frames(s: Session, a, out_path: str) -> int:
    """Frames where the centre ToF saw something close: a vehicle ahead is one of the things that
    look like that.  The box is our lane (centre half, lower half of the image)."""
    w, h = s.image_size()
    box = [w // 4, h // 2, w - w // 4, h]
    count = 0
    with open(out_path, "w", encoding="utf-8") as out:
        for n, line in enumerate(s.image_lines):
            if a.every > 1 and n % a.every:
                continue
            c = tof_mm(line, 1)
            if c is None or c >= a.near_mm:
                continue
            out.write(json.dumps({"index": int(line["index"]), "objects": [{"name": a.detector, "box": box}],
                                  "image": line["image"], "tof_mm": c}, ensure_ascii=False) + "\n")
            count += 1
    return count


def propose_side_openings(s: Session, a, out_path: str) -> int:
    """The moments a side ToF went from a wall (below --near-mm) to open: the car was passing an
    opening on that side, so the --before-ms before it show the intersection ahead."""
    side = 0 if a.detector.endswith("left") else 2 if a.detector.endswith("right") else None
    if side is None:
        raise SystemExit("error: --detector must be intersection_left or intersection_right (which side opens)")
    lines = s.image_lines
    count = 0
    walled = False
    with open(out_path, "w", encoding="utf-8") as out:
        for n, line in enumerate(lines):
            v = tof_mm(line, side)
            if v is not None and v < a.near_mm:
                walled = True
                continue
            if not walled:
                continue
            walled = False   # a wall just ended here
            t_open = int(line["header"].get("t_us", 0))
            first = n
            while first > 0 and t_open - int(lines[first - 1]["header"].get("t_us", 0)) <= a.before_ms * 1000:
                first -= 1
            out.write(json.dumps({"from": int(lines[first]["index"]), "to": int(line["index"]), "scene": [a.detector],
                                  "images": [lines[first]["image"], line["image"]]}, ensure_ascii=False) + "\n")
            count += 1
    return count


def blobs(mask: np.ndarray, min_pixels: int):
    """4-connected components of a boolean mask: (x0, y0, x1, y1, pixels), x1/y1 exclusive."""
    h, w = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    out = []
    for y in range(h):
        for x in range(w):
            if not mask[y, x] or seen[y, x]:
                continue
            stack = [(y, x)]
            seen[y, x] = True
            xs, ys, n = [], [], 0
            while stack:
                cy, cx = stack.pop()
                n += 1
                xs.append(cx); ys.append(cy)
                for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                    if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
            if n >= min_pixels:
                out.append((min(xs), min(ys), max(xs) + 1, max(ys) + 1, n))
    return out


def load_parent(parent_dir: str):
    """(params dict, VisionPack or None, manifest dict) of a cam_drive pack, or a tof_drive pack wrapped."""
    with open(os.path.join(parent_dir, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    with open(os.path.join(parent_dir, "params.json"), encoding="utf-8") as f:
        params = json.load(f)
    logic = manifest.get("logic", {}).get("name")
    if logic == "tof_drive":
        params = {"tof": params, "signal": DEFAULT_SIGNAL.copy()}
    elif logic != "cam_drive" or "tof" not in params:
        raise SystemExit(f"error: {parent_dir} is not a cam_drive (or tof_drive) pack")
    pack = None
    vb = os.path.join(parent_dir, "vision.bin")
    if os.path.isfile(vb):
        with open(vb, "rb") as f:
            pack = rv.decode_pack(f.read())
    return params, pack, manifest


# The behaviour parameters and their defaults, as in car/logic/cam_drive/cam_params.hpp (docs 3.11).
DEFAULT_SIGNAL = {"red_detector": "traffic_light_red", "green_detector": "traffic_light_green", "stop_min_cells": 2,
                  "approach_v": 500, "min_stop_ms": 1000, "resume_without_green_ms": 3000, "resume_ignore_ms": 1500}
DEFAULT_VEHICLE = {"detector": "vehicle", "lane_cols": 4, "follow_gap_mm": 150, "follow_band_mm": 80, "follow_v": 700,
                   "parked_confirm_ms": 2000, "still_delta_mm": 20, "overtake_side": "open", "side_clear_mm": 120,
                   "overtake_v": 500, "out_ms": 500, "pass_min_ms": 800, "pass_max_ms": 3000, "pass_clear_mm": 150,
                   "pass_clear_ms": 300, "in_ms": 500, "ignore_ms": 1500}
DEFAULT_INTERSECTION = {"left_detector": "intersection_left", "right_detector": "intersection_right", "approach_v": 500,
                        "stop_trigger": "side_open", "open_mm": 200, "stop_ms": 1000, "turn": "straight", "turn_v": 500,
                        "cross_max_ms": 2000, "turn_ms": 900, "ignore_ms": 3000}
BEHAVIOUR_DEFAULTS = {"signal": DEFAULT_SIGNAL, "vehicle": DEFAULT_VEHICLE, "intersection": DEFAULT_INTERSECTION}
DETECTOR_KEYS = {"signal": ("red_detector", "green_detector"), "vehicle": ("detector",),
                 "intersection": ("left_detector", "right_detector")}


def behaviour_section(section: str, parent_params: dict, overrides, pack: rv.VisionPack):
    """The params.json section of one behaviour for a new pack: the parent's values (or the defaults)
    with the --<section> key=value overrides, switched on only for detectors the pack has.
    Returns the dict, or None when the section is to be left out (the behaviour off)."""
    defaults = BEHAVIOUR_DEFAULTS[section]
    values = dict(parent_params.get(section) or defaults)
    for k in defaults:
        values.setdefault(k, defaults[k])
    explicit = set()
    for kv in overrides or []:
        k, _, v = kv.partition("=")
        if k not in defaults:
            raise SystemExit(f"error: unknown {section} parameter {k}")
        values[k] = v if isinstance(defaults[k], str) else int(v)
        explicit.add(k)
    keys = DETECTOR_KEYS[section]
    inherited = section in parent_params   # the parent had this behaviour: say so when it goes off
    for key in keys:
        name = values[key]
        if name and pack.find(name) < 0:
            if key in explicit:
                raise SystemExit(f"error: {section}.{key} = {name!r} but the pack has no such detector")
            values[key] = ""
            if inherited:
                print(f"note: {section}.{key} {name!r} is not in this pack: left empty")
    if section == "signal":
        if not values["red_detector"] and values["green_detector"]:
            values["green_detector"] = ""
        return values   # always present ("" = off)
    if not any(values[k] for k in keys):
        if explicit:
            raise SystemExit(f"error: --{section} given but the pack has none of its detectors")
        if inherited:
            print(f"note: no {section} detector in this pack: the {section} behaviour is off")
        return None
    return values


def cmd_train(a) -> int:
    params, parent_pack, parent_manifest = load_parent(a.parent)
    train = load_sessions(a.sessions)
    validate = load_sessions(a.validate or [])
    negatives = set(os.path.abspath(p) for p in (a.negatives or []))
    if a.negatives_from_scenario:
        for s in train + validate:
            if s.scenario in a.negatives_from_scenario:
                negatives.add(s.path)
    if parent_pack is not None and not a.config_from_args:
        cfg = parent_pack.config
    else:
        cfg = rv.FeatureConfig(a.downscale, a.grid_x, a.grid_y, a.s_min, a.v_min, a.v_white)
    w, h = train[0].image_size()
    if cfg.validate(w, h):
        raise SystemExit("error: " + cfg.validate(w, h))
    for s in train + validate:
        if s.image_size() != (w, h):
            raise SystemExit(f"error: {s.name} has images of {s.image_size()}, expected {(w, h)}")
    print(f"features: downscale {cfg.downscale}, grid {cfg.grid_x}x{cfg.grid_y}, image {w}x{h}")
    data = {}
    for s in train + validate:
        print(f"session {s.name} (scenario {s.scenario!r}, {len(s.labels)} labelled frames{', negatives' if s.path in negatives else ''})")
        data[s.path] = s.features(cfg)

    # Standardisation over every training cell.
    all_cells = np.concatenate([data[s.path][1].reshape(-1, rv.FEATURES_PER_CELL) for s in train]).astype(np.float64)
    mean = all_cells.mean(axis=0)
    std = np.maximum(all_cells.std(axis=0), 1.0)
    pack = rv.VisionPack(cfg, w, h, mean.astype(np.float32), std.astype(np.float32))
    kept = []
    if parent_pack is not None and a.keep_parent_detectors:
        if (parent_pack.config.__dict__ != cfg.__dict__) or (parent_pack.image_w, parent_pack.image_h) != (w, h):
            raise SystemExit("error: --keep-parent-detectors needs the parent's feature configuration and image size")
        # Keep the parent's standardisation so its detectors still mean the same.
        pack.mean, pack.std = parent_pack.mean.copy(), parent_pack.std.copy()
        kept = [d for d in parent_pack.detectors if d.name not in {n.split(":")[0] for n in a.detector}]
        pack.detectors = list(kept)
        print(f"kept {len(kept)} detectors from the parent: {', '.join(d.name for d in kept) or '-'}")

    report = {"tool": TOOL_VERSION, "parent": parent_manifest.get("id", "") + "-" + parent_manifest.get("version", ""),
              "sessions": [s.name for s in train], "validate": [s.name for s in validate], "detectors": {}}
    for spec in a.detector:
        name, _, kind_s = spec.partition(":")
        kind = rv.KIND_GLOBAL if kind_s == "global" else rv.KIND_LOCAL
        if not rv.valid_name(name):
            raise SystemExit(f"error: bad detector name {name!r}")
        print(f"detector {name} ({'global' if kind == rv.KIND_GLOBAL else 'local'})")
        xs, ys, src = [], [], []
        for s in train:
            idx, feats = data[s.path]
            z = ((feats.astype(np.float32).reshape(len(idx), cfg.grid_y, cfg.grid_x, rv.FEATURES_PER_CELL) - pack.mean) / pack.std)
            if kind == rv.KIND_LOCAL:
                t = local_targets(s, idx, cfg, w, h, name, s.path in negatives, a.cell_overlap, a.cell_ignore)
                x_all = np.stack([rv.local_inputs(pack, z[r]).reshape(-1, rv.LOCAL_DIMS) for r in range(len(idx))])
                sel = t.reshape(-1) >= 0
                xs.append(x_all.reshape(-1, rv.LOCAL_DIMS)[sel]); ys.append(t.reshape(-1)[sel])
            else:
                t = global_targets(s, idx, name, s.path in negatives)
                sel = t >= 0
                xs.append(z.reshape(len(idx), -1)[sel]); ys.append(t[sel])
            src.append((s, idx, feats, t))
        x = np.concatenate(xs) if xs else np.zeros((0, rv.LOCAL_DIMS if kind == rv.KIND_LOCAL else cfg.dims), dtype=np.float32)
        y = np.concatenate(ys) if ys else np.zeros(0, dtype=np.int8)
        n_pos, n_neg = int((y == 1).sum()), int((y == 0).sum())
        print(f"  samples: {n_pos} positive, {n_neg} negative")
        if n_pos == 0 or n_neg == 0:
            raise SystemExit(f"error: detector {name} needs both positive and negative samples (label more frames or add negative sessions)")
        weight = np.where(y == 1, 0.5 / n_pos, 0.5 / n_neg)
        wv, bias = fit_logistic(x, y, weight, a.l2, a.iterations, a.learning_rate)
        scores = 1.0 / (1.0 + np.exp(-(x.astype(np.float64) @ wv.astype(np.float64) + bias)))
        thr, prec, rec, f1 = choose_threshold(scores.astype(np.float32), y)
        thr_on = a.thr_on if a.thr_on is not None else thr
        thr_off = choose_release(thr_on, a.hysteresis, scores[y == 0])
        det = rv.Detector(name, kind, wv, bias, thr_on, thr_off, a.confirm_frames, a.release_frames, a.min_cells,
                          a.row_min, min(a.row_max, cfg.grid_y - 1) if a.row_max is not None else cfg.grid_y - 1)
        pack.detectors.append(det)
        entry = {"kind": "global" if kind == rv.KIND_GLOBAL else "local", "positives": n_pos, "negatives": n_neg,
                 "thr_on": thr_on, "thr_off": thr_off, "sample_precision": prec, "sample_recall": rec, "sample_f1": f1}
        print(f"  cells/frames at thr_on {thr_on}: precision {prec:.3f} recall {rec:.3f} f1 {f1:.3f}")
        for label, group in (("train", train), ("validate", validate)):
            agg = {"frames": 0, "tp": 0, "fp": 0, "fn": 0, "tn": 0}
            for s in group:
                idx, feats = data[s.path]
                t = (local_targets(s, idx, cfg, w, h, name, s.path in negatives, a.cell_overlap, a.cell_ignore) if kind == rv.KIND_LOCAL
                     else global_targets(s, idx, name, s.path in negatives))
                m = frame_metrics(pack, det, feats, t)
                for k in agg:
                    agg[k] += m[k]
            if agg["frames"]:
                p_ = agg["tp"] / (agg["tp"] + agg["fp"]) if agg["tp"] + agg["fp"] else 0.0
                r_ = agg["tp"] / (agg["tp"] + agg["fn"]) if agg["tp"] + agg["fn"] else 0.0
                entry[label] = dict(agg, precision=p_, recall=r_)
                print(f"  {label} frames: {agg['frames']} labelled, precision {p_:.3f} recall {r_:.3f} (tp {agg['tp']} fp {agg['fp']} fn {agg['fn']})")
        report["detectors"][name] = entry

    # Self-test block from a frame of the first training session (a positive one when possible).
    st_line = None
    for s in train:
        for line in s.image_lines:
            lab = s.labels.get(int(line["index"]))
            if lab and lab["objects"] and os.path.isfile(s.frame_path(line)):
                st_line = (s, line)
                break
        if st_line:
            break
    if st_line is None:
        for s in train:
            for line in s.image_lines:
                if os.path.isfile(s.frame_path(line)):
                    st_line = (s, line)
                    break
            if st_line:
                break
    if st_line:
        pack.selftest = rv.make_selftest(pack, rv.load_rgb(st_line[0].frame_path(st_line[1])))
        report["selftest_frame"] = f"{st_line[0].name}/{st_line[1]['image']}"

    # Behaviour parameters: the parent's (or the defaults), with overrides, for the detectors the pack has.
    new_params = {"tof": params["tof"], "signal": behaviour_section("signal", params, a.signal, pack)}
    for section, overrides in (("vehicle", a.vehicle), ("intersection", a.intersection)):
        values = behaviour_section(section, params, overrides, pack)
        if values is not None:
            new_params[section] = values
    params = new_params
    behaviours = ["signal"] if params["signal"]["red_detector"] else []
    behaviours += [k for k in ("vehicle", "intersection") if k in params]
    print(f"behaviours on: {', '.join(behaviours) or '- (ToF only)'}")

    # Write the pack directory.
    os.makedirs(a.out_dir, exist_ok=True)
    with open(os.path.join(a.out_dir, "params.json"), "w", encoding="utf-8") as f:
        json.dump(params, f, indent=2, ensure_ascii=False)
        f.write("\n")
    with open(os.path.join(a.out_dir, "vision.bin"), "wb") as f:
        f.write(rv.encode_pack(pack))
    with open(os.path.join(a.out_dir, "training.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
        f.write("\n")
    capabilities = ["walls"]
    if any(d.name.startswith("traffic_light") for d in pack.detectors):
        capabilities.append("traffic_light")
    if any(d.name.startswith("vehicle") for d in pack.detectors):
        capabilities.append("vehicle")
    if any(d.name.startswith("intersection") for d in pack.detectors):
        capabilities.append("intersection")
    scenarios = []
    for s in train:
        if s.scenario and s.scenario not in scenarios:
            scenarios.append(s.scenario)
    parent_name = parent_manifest.get("id", "") + "-" + parent_manifest.get("version", "")
    ns = argparse.Namespace(dir=a.out_dir, id=a.id, version=a.version, logic="cam_drive", min_abi=2,
                            description=a.description or f"{', '.join(d.name for d in pack.detectors)} learned from {', '.join(s.name for s in train)}",
                            capability=capabilities, scenario=scenarios, parent=parent_name)
    make_manifest.cmd_pack(ns)
    print(f"pack {a.id}-{a.version} written to {a.out_dir}: {len(pack.detectors)} detectors, capabilities {capabilities}, "
          f"behaviours {', '.join(behaviours) or '-'}")
    print("next: rumicar-replay or `train_pack.py replay` on a session you did not train on, then send it with the drive manager")
    return 0


def cmd_eval(a) -> int:
    with open(os.path.join(a.pack_dir, "vision.bin"), "rb") as f:
        pack = rv.decode_pack(f.read())
    sessions = load_sessions(a.sessions)
    negatives = set(os.path.abspath(p) for p in (a.negatives or []))
    for s in sessions:
        idx, feats = s.features(pack.config)
        w, h = s.image_size()
        print(f"session {s.name}: {len(idx)} frames, {len(s.labels)} labelled")
        for d in pack.detectors:
            t = (local_targets(s, idx, pack.config, w, h, d.name, s.path in negatives, a.cell_overlap, a.cell_ignore)
                 if d.kind == rv.KIND_LOCAL else global_targets(s, idx, d.name, s.path in negatives))
            m = frame_metrics(pack, d, feats, t)
            if m["frames"]:
                print(f"  {d.name}: {m['frames']} labelled frames, precision {m['precision']:.3f} recall {m['recall']:.3f} (tp {m['tp']} fp {m['fp']} fn {m['fn']})")
            else:
                print(f"  {d.name}: no labelled frames")
    return 0


def cmd_replay(a) -> int:
    with open(os.path.join(a.pack_dir, "vision.bin"), "rb") as f:
        pack = rv.decode_pack(f.read())
    s = Session(a.session)
    idx, feats = s.features(pack.config)
    ev = rv.Evaluator(pack)
    out_path = a.out or os.path.join(s.path, "replay_perception.jsonl")
    active_runs = {d.name: [] for d in pack.detectors}
    current = {d.name: None for d in pack.detectors}
    with open(out_path, "w", encoding="utf-8") as out:
        for row in range(len(idx)):
            outs = ev.step(feats[row])
            rec = {"index": int(idx[row]), "detectors": [{k: (round(v, 4) if isinstance(v, float) else v) for k, v in o.items()} for o in outs]}
            out.write(json.dumps(rec) + "\n")
            for o in outs:
                name = o["name"]
                if o["active"] and current[name] is None:
                    current[name] = int(idx[row])
                elif not o["active"] and current[name] is not None:
                    active_runs[name].append((current[name], int(idx[row - 1])))
                    current[name] = None
    for name, start in current.items():
        if start is not None:
            active_runs[name].append((start, int(idx[-1])))
    print(f"{len(idx)} frames -> {out_path}")
    for name, runs in active_runs.items():
        print(f"  {name}: active in {len(runs)} interval(s) " + ", ".join(f"{a0}-{b0}" for a0, b0 in runs[:20]))
    return 0


def cmd_show(a) -> int:
    make_manifest.cmd_show(argparse.Namespace(dir=a.pack_dir))
    pj = os.path.join(a.pack_dir, "params.json")
    if os.path.isfile(pj):
        with open(pj, encoding="utf-8") as f:
            params = json.load(f)
        if isinstance(params, dict) and "signal" in params:
            on = []
            sig = params.get("signal") or {}
            if sig.get("red_detector"):
                on.append(f"signal (red {sig['red_detector']}, green {sig.get('green_detector') or '-'})")
            if "vehicle" in params:
                v = params["vehicle"]
                on.append(f"vehicle ({v.get('detector')}, gap {v.get('follow_gap_mm')} mm, pass on the {v.get('overtake_side')})")
            if "intersection" in params:
                i = params["intersection"]
                on.append(f"intersection ({i.get('left_detector') or '-'} / {i.get('right_detector') or '-'}, "
                          f"stop on {i.get('stop_trigger')}, then {i.get('turn')})")
            print(f"  behaviours:   {'; '.join(on) or '- (ToF only)'}")
    vb = os.path.join(a.pack_dir, "vision.bin")
    if not os.path.isfile(vb):
        print("  vision.bin:   none (ToF only)")
        return 0
    with open(vb, "rb") as f:
        pack = rv.decode_pack(f.read())
    c = pack.config
    print(f"  vision.bin:   features downscale {c.downscale}, grid {c.grid_x}x{c.grid_y}, image {pack.image_w}x{pack.image_h}, "
          f"self-test {'yes' if pack.selftest is not None else 'no'}")
    for d in pack.detectors:
        print(f"    {d.name}: {'global' if d.kind == rv.KIND_GLOBAL else 'local'}, thr {round(float(d.thr_on), 3)}/{round(float(d.thr_off), 3)}, "
              f"confirm {d.confirm_frames} release {d.release_frames}, min_cells {d.min_cells}, rows {d.row_min}-{d.row_max}")
    tj = os.path.join(a.pack_dir, "training.json")
    if os.path.isfile(tj):
        with open(tj, encoding="utf-8") as f:
            rep = json.load(f)
        for name, e in rep.get("detectors", {}).items():
            v = e.get("validate") or e.get("train") or {}
            print(f"    {name}: {e.get('positives')} pos / {e.get('negatives')} neg samples; frames precision {v.get('precision', 0):.3f} recall {v.get('recall', 0):.3f}")
    return 0


def add_feature_args(p):
    p.add_argument("--downscale", type=int, default=5)
    p.add_argument("--grid-x", type=int, default=8)
    p.add_argument("--grid-y", type=int, default=6)
    p.add_argument("--s-min", type=int, default=64)
    p.add_argument("--v-min", type=int, default=48)
    p.add_argument("--v-white", type=int, default=160)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    f = sub.add_parser("features", help="compute and cache features")
    f.add_argument("sessions", nargs="+")
    add_feature_args(f)
    f.set_defaults(func=cmd_features)

    pr = sub.add_parser("propose", help="candidate boxes to label")
    pr.add_argument("session")
    pr.add_argument("--detector", default="traffic_light_red")
    pr.add_argument("--color-class", default=0, help="pixel class to look for when --detector is not a known name (0 red .. 5 magenta)")
    pr.add_argument("--min-pixels", type=int, default=3, help="smallest blob in small-image pixels")
    pr.add_argument("--near-mm", type=int, default=250, help="vehicle: centre ToF below this is 'close'; intersection: a side below this is a wall")
    pr.add_argument("--before-ms", type=int, default=2000, help="intersection: how long before the side opened the stretch starts")
    pr.add_argument("--every", type=int, default=1, help="look at every n-th frame")
    pr.add_argument("--out")
    add_feature_args(pr)
    pr.set_defaults(func=cmd_propose)

    t = sub.add_parser("train", help="learn detectors and write a pack")
    t.add_argument("out_dir")
    t.add_argument("--parent", required=True, help="pack to derive from (cam_drive, or a tof_drive pack for its ToF parameters)")
    t.add_argument("--sessions", nargs="+", required=True)
    t.add_argument("--validate", nargs="*", help="sessions used only to measure")
    t.add_argument("--negatives", nargs="*", help="sessions whose every frame is a negative example")
    t.add_argument("--negatives-from-scenario", nargs="*", help="scenario names whose sessions are negatives")
    t.add_argument("--detector", action="append", required=True, help="NAME or NAME:global (default local); repeatable")
    t.add_argument("--keep-parent-detectors", action="store_true", help="carry the parent's detectors over (except retrained names)")
    t.add_argument("--id", required=True)
    t.add_argument("--version", required=True)
    t.add_argument("--description")
    t.add_argument("--signal", action="append", help="signal parameter override key=value, e.g. stop_min_cells=3")
    t.add_argument("--vehicle", action="append", help="vehicle parameter override key=value, e.g. overtake_side=right")
    t.add_argument("--intersection", action="append", help="intersection parameter override key=value, e.g. turn=left")
    t.add_argument("--cell-overlap", type=float, default=0.25, help="box must cover this fraction of a cell for a positive")
    t.add_argument("--cell-ignore", type=float, default=0.05, help="cells covered less than --cell-overlap but at least this are ignored")
    t.add_argument("--l2", type=float, default=1e-3)
    t.add_argument("--iterations", type=int, default=600)
    t.add_argument("--learning-rate", type=float, default=0.05)
    t.add_argument("--thr-on", type=float, help="fixed threshold instead of the F1-optimal one")
    t.add_argument("--hysteresis", type=float, default=0.2, help="thr_off = thr_on - this (raised when the negatives' scores reach that high)")
    t.add_argument("--confirm-frames", type=int, default=3)
    t.add_argument("--release-frames", type=int, default=5)
    t.add_argument("--min-cells", type=int, default=1)
    t.add_argument("--row-min", type=int, default=0)
    t.add_argument("--row-max", type=int)
    t.add_argument("--config-from-args", action="store_true", help="use the feature arguments instead of the parent's configuration")
    add_feature_args(t)
    t.set_defaults(func=cmd_train)

    e = sub.add_parser("eval", help="measure a pack on labelled sessions")
    e.add_argument("pack_dir")
    e.add_argument("--sessions", nargs="+", required=True)
    e.add_argument("--negatives", nargs="*")
    e.add_argument("--cell-overlap", type=float, default=0.25)
    e.add_argument("--cell-ignore", type=float, default=0.05)
    e.set_defaults(func=cmd_eval)

    r = sub.add_parser("replay", help="detector outputs on a session (perception only)")
    r.add_argument("pack_dir")
    r.add_argument("session")
    r.add_argument("--out")
    r.set_defaults(func=cmd_replay)

    s = sub.add_parser("show", help="what a pack contains")
    s.add_argument("pack_dir")
    s.set_defaults(func=cmd_show)

    a = p.parse_args()
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
