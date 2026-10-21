"""Image features, vision packs and detector evaluation of the cam_drive logic
— the Python side of car/logic/cam_drive/ (vision_features.cpp, vision_pack.cpp,
vision_eval.cpp).  Training (train_pack.py) and the golden tests use this
module; the car runs the C++.  Both must produce the same numbers, so every
step here mirrors the C++ definition (integer arithmetic, same rounding, same
order), and car/tests/data/vision holds golden data generated here that the
C++ test checks against.

Needs numpy and Pillow (pip install numpy pillow).
"""
from __future__ import annotations

import math
import struct
import zlib
from dataclasses import dataclass, field

import numpy as np

FEATURE_SET = 1
PACK_VERSION = 1
FEATURES_PER_CELL = 12
CLASS_COUNT = 8
LOCAL_DIMS = 9 * FEATURES_PER_CELL
MAX_DETECTORS = 32
MAX_NAME = 32
CLASS_NAMES = ["red", "yellow", "green", "cyan", "blue", "magenta", "bright", "dark"]
FEATURE_NAMES = CLASS_NAMES + ["sum_v", "sum_s", "sum_dx", "sum_dy"]
KIND_GLOBAL = 0
KIND_LOCAL = 1


@dataclass
class FeatureConfig:
    downscale: int = 5
    grid_x: int = 8
    grid_y: int = 6
    s_min: int = 64
    v_min: int = 48
    v_white: int = 160

    @property
    def dims(self) -> int:
        return self.grid_x * self.grid_y * FEATURES_PER_CELL

    def validate(self, w: int, h: int) -> str:
        if not 1 <= self.downscale <= 16:
            return "downscale must be 1..16"
        if not (1 <= self.grid_x <= 64 and 1 <= self.grid_y <= 64):
            return "grid must be 1..64 cells"
        if w <= 0 or h <= 0:
            return "image size must be positive"
        sw, sh = w // self.downscale, h // self.downscale
        if sw // self.grid_x < 2 or sh // self.grid_y < 2:
            return "cells must be at least 2x2 small pixels"
        return ""


# --- (1) downscale ------------------------------------------------------------------------------

def downscale(rgb: np.ndarray, factor: int) -> np.ndarray:
    """Box average (integer floor) of factor×factor blocks; right/bottom remainders dropped.
    rgb: uint8 array (h, w, 3).  Returns uint8 (sh, sw, 3)."""
    h, w = rgb.shape[0], rgb.shape[1]
    sh, sw = h // factor, w // factor
    if sh == 0 or sw == 0:
        return np.zeros((sh, sw, 3), dtype=np.uint8)
    block = rgb[: sh * factor, : sw * factor, :].astype(np.uint32)
    block = block.reshape(sh, factor, sw, factor, 3).sum(axis=(1, 3))
    return (block // (factor * factor)).astype(np.uint8)


# --- (2) pixel classes ---------------------------------------------------------------------------

def hue_degrees(r: np.ndarray, g: np.ndarray, b: np.ndarray) -> np.ndarray:
    """Integer hue 0..359 (uint32 arrays; only valid where max > min)."""
    r = r.astype(np.int64)
    g = g.astype(np.int64)
    b = b.astype(np.int64)
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    d = np.maximum(mx - mn, 1)   # avoid /0 where unused
    h = np.zeros_like(r)
    is_r = mx == r
    is_g = (~is_r) & (mx == g)
    is_b = ~is_r & ~is_g
    # red sector
    h = np.where(is_r & (g >= b), 60 * (g - b) // d, h)
    h = np.where(is_r & (g < b), 360 - 60 * (b - g) // d, h)
    h = np.where(is_g & (b >= r), 120 + 60 * (b - r) // d, h)
    h = np.where(is_g & (b < r), 120 - 60 * (r - b) // d, h)
    h = np.where(is_b & (r >= g), 240 + 60 * (r - g) // d, h)
    h = np.where(is_b & (r < g), 240 - 60 * (g - r) // d, h)
    return h


def classify(small: np.ndarray, cfg: FeatureConfig):
    """Returns (cls uint8, v uint8, s uint8) arrays of shape (sh, sw)."""
    r = small[:, :, 0].astype(np.int64)
    g = small[:, :, 1].astype(np.int64)
    b = small[:, :, 2].astype(np.int64)
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    d = mx - mn
    s = np.where(mx == 0, 0, d * 255 // np.maximum(mx, 1))
    hue = hue_degrees(r, g, b)
    hue_bin = ((hue + 30) % 360) // 60
    saturated = (d > 0) & (s >= cfg.s_min) & (mx >= cfg.v_min)
    cls = np.where(saturated, hue_bin, np.where(mx >= cfg.v_white, 6, 7))
    return cls.astype(np.uint8), mx.astype(np.uint8), s.astype(np.uint8)


# --- (3) cell features ----------------------------------------------------------------------------

def compute_features(small: np.ndarray, cfg: FeatureConfig) -> np.ndarray:
    """Features of a small image: int32 array of cfg.dims values (cell-major, 12 per cell)."""
    sh, sw = small.shape[0], small.shape[1]
    cw, ch = sw // cfg.grid_x, sh // cfg.grid_y
    out = np.zeros((cfg.grid_y, cfg.grid_x, FEATURES_PER_CELL), dtype=np.int64)
    if cw == 0 or ch == 0:
        return out.reshape(-1).astype(np.int32)
    cls, v, s = classify(small, cfg)
    vi = v.astype(np.int64)
    dx = np.zeros_like(vi)
    dy = np.zeros_like(vi)
    dx[:, :-1] = np.abs(vi[:, 1:] - vi[:, :-1])
    dy[:-1, :] = np.abs(vi[1:, :] - vi[:-1, :])
    H, W = cfg.grid_y * ch, cfg.grid_x * cw

    def cells(a):
        return a[:H, :W].reshape(cfg.grid_y, ch, cfg.grid_x, cw)

    c = cells(cls)
    for k in range(CLASS_COUNT):
        out[:, :, k] = (c == k).sum(axis=(1, 3))
    out[:, :, 8] = cells(vi).sum(axis=(1, 3))
    out[:, :, 9] = cells(s.astype(np.int64)).sum(axis=(1, 3))
    out[:, :, 10] = cells(dx).sum(axis=(1, 3))
    out[:, :, 11] = cells(dy).sum(axis=(1, 3))
    return out.reshape(-1).astype(np.int32)


def extract_features(rgb: np.ndarray, cfg: FeatureConfig) -> np.ndarray:
    return compute_features(downscale(rgb, cfg.downscale), cfg)


def load_rgb(path: str) -> np.ndarray:
    """Reads an image file (JPEG, PNG, PPM ...) as uint8 (h, w, 3)."""
    from PIL import Image
    with Image.open(path) as im:
        return np.asarray(im.convert("RGB"), dtype=np.uint8)


# --- pack ---------------------------------------------------------------------------------------------

@dataclass
class Detector:
    name: str
    kind: int = KIND_LOCAL
    weights: np.ndarray = field(default_factory=lambda: np.zeros(LOCAL_DIMS, dtype=np.float32))
    bias: float = 0.0
    thr_on: float = 0.5
    thr_off: float = 0.3
    confirm_frames: int = 3
    release_frames: int = 5
    min_cells: int = 1
    row_min: int = 0
    row_max: int = 255


@dataclass
class SelfTest:
    image: np.ndarray            # small image uint8 (sh, sw, 3)
    features: np.ndarray         # int32
    scores: np.ndarray           # float32 per detector


@dataclass
class VisionPack:
    config: FeatureConfig = field(default_factory=FeatureConfig)
    image_w: int = 320
    image_h: int = 240
    mean: np.ndarray = field(default_factory=lambda: np.zeros(FEATURES_PER_CELL, dtype=np.float32))
    std: np.ndarray = field(default_factory=lambda: np.ones(FEATURES_PER_CELL, dtype=np.float32))
    detectors: list = field(default_factory=list)
    selftest: SelfTest | None = None

    def find(self, name: str) -> int:
        for i, d in enumerate(self.detectors):
            if d.name == name:
                return i
        return -1


def valid_name(name: str) -> bool:
    return 1 <= len(name) <= MAX_NAME and all(c.islower() and c.isascii() or c.isdigit() or c == "_" for c in name)


def encode_pack(p: VisionPack) -> bytes:
    out = bytearray(b"RCV1")
    out += struct.pack("<HHHHHBBBBHHH", PACK_VERSION, FEATURE_SET, p.config.downscale, p.config.grid_x, p.config.grid_y,
                       p.config.s_min, p.config.v_min, p.config.v_white, 1 if p.selftest is not None else 0,
                       len(p.detectors), p.image_w, p.image_h)
    out += np.asarray(p.mean, dtype="<f4").tobytes()
    out += np.asarray(p.std, dtype="<f4").tobytes()
    for d in p.detectors:
        if not valid_name(d.name):
            raise ValueError(f"invalid detector name {d.name!r}")
        name = d.name.encode("ascii")
        out += struct.pack("<BB", d.kind, len(name)) + name
        w = np.asarray(d.weights, dtype="<f4")
        out += struct.pack("<H", w.size) + w.tobytes()
        out += struct.pack("<fffHHHBB", d.bias, d.thr_on, d.thr_off, d.confirm_frames, d.release_frames, d.min_cells, d.row_min, d.row_max)
    if p.selftest is not None:
        img = p.selftest.image
        out += struct.pack("<HH", img.shape[1], img.shape[0]) + np.ascontiguousarray(img, dtype=np.uint8).tobytes()
        out += np.asarray(p.selftest.features, dtype="<i4").tobytes()
        out += np.asarray(p.selftest.scores, dtype="<f4").tobytes()
    out += struct.pack("<I", zlib.crc32(bytes(out)) & 0xFFFFFFFF)
    return bytes(out)


def decode_pack(data: bytes) -> VisionPack:
    if len(data) < 124 or data[:4] != b"RCV1":
        raise ValueError("vision.bin: bad magic or too short")
    if zlib.crc32(data[:-4]) & 0xFFFFFFFF != struct.unpack("<I", data[-4:])[0]:
        raise ValueError("vision.bin: CRC mismatch")
    pos = 4
    (version, feature_set, ds, gx, gy, s_min, v_min, v_white, flags, n_det, iw, ih) = struct.unpack_from("<HHHHHBBBBHHH", data, pos)
    pos += struct.calcsize("<HHHHHBBBBHHH")
    if version != PACK_VERSION or feature_set != FEATURE_SET:
        raise ValueError("vision.bin: unsupported version or feature set")
    p = VisionPack(FeatureConfig(ds, gx, gy, s_min, v_min, v_white), iw, ih)
    err = p.config.validate(iw, ih)
    if err:
        raise ValueError("vision.bin: " + err)
    p.mean = np.frombuffer(data, dtype="<f4", count=FEATURES_PER_CELL, offset=pos).astype(np.float32); pos += 48
    p.std = np.frombuffer(data, dtype="<f4", count=FEATURES_PER_CELL, offset=pos).astype(np.float32); pos += 48
    if not np.all(p.std > 0):
        raise ValueError("vision.bin: std must be positive")
    for _ in range(n_det):
        kind, name_len = struct.unpack_from("<BB", data, pos); pos += 2
        name = data[pos:pos + name_len].decode("ascii"); pos += name_len
        (dims,) = struct.unpack_from("<H", data, pos); pos += 2
        expected = p.config.dims if kind == KIND_GLOBAL else LOCAL_DIMS
        if dims != expected:
            raise ValueError(f"vision.bin: detector {name} has {dims} weights, expected {expected}")
        w = np.frombuffer(data, dtype="<f4", count=dims, offset=pos).astype(np.float32); pos += 4 * dims
        bias, thr_on, thr_off, confirm, release, min_cells, row_min, row_max = struct.unpack_from("<fffHHHBB", data, pos)
        pos += struct.calcsize("<fffHHHBB")
        p.detectors.append(Detector(name, kind, w, bias, thr_on, thr_off, confirm, release, min_cells, row_min, row_max))
    if flags & 1:
        sw, sh = struct.unpack_from("<HH", data, pos); pos += 4
        img = np.frombuffer(data, dtype=np.uint8, count=sw * sh * 3, offset=pos).reshape(sh, sw, 3).copy(); pos += sw * sh * 3
        feats = np.frombuffer(data, dtype="<i4", count=p.config.dims, offset=pos).astype(np.int32); pos += 4 * p.config.dims
        scores = np.frombuffer(data, dtype="<f4", count=n_det, offset=pos).astype(np.float32); pos += 4 * n_det
        p.selftest = SelfTest(img, feats, scores)
    if pos != len(data) - 4:
        raise ValueError("vision.bin: trailing data")
    return p


# --- evaluation ------------------------------------------------------------------------------------------

def standardise(p: VisionPack, features: np.ndarray) -> np.ndarray:
    """float32 (grid_y, grid_x, 12) standardised features."""
    f = np.asarray(features, dtype=np.float32).reshape(p.config.grid_y, p.config.grid_x, FEATURES_PER_CELL)
    return (f - p.mean.astype(np.float32)) / p.std.astype(np.float32)


def local_inputs(p: VisionPack, z: np.ndarray) -> np.ndarray:
    """(grid_y, grid_x, 108): the 3×3 neighbourhood of every cell, zeros outside the grid."""
    gy, gx = p.config.grid_y, p.config.grid_x
    padded = np.zeros((gy + 2, gx + 2, FEATURES_PER_CELL), dtype=np.float32)
    padded[1:-1, 1:-1, :] = z
    parts = []
    for dj in (-1, 0, 1):
        for di in (-1, 0, 1):
            parts.append(padded[1 + dj: 1 + dj + gy, 1 + di: 1 + di + gx, :])
    return np.concatenate(parts, axis=2)


def _sigmoid(x: np.ndarray) -> np.ndarray:
    return (1.0 / (1.0 + np.exp(-x.astype(np.float64)))).astype(np.float32)


def score_global(p: VisionPack, d: Detector, features: np.ndarray) -> float:
    z = standardise(p, features).reshape(-1)
    acc = np.float32(d.bias)
    for wk, zk in zip(d.weights.astype(np.float32), z):   # same accumulation order as the C++
        acc = np.float32(acc + np.float32(wk * zk))
    return float(_sigmoid(np.array([acc]))[0])


def score_local_map(p: VisionPack, d: Detector, features: np.ndarray) -> np.ndarray:
    """(grid_y, grid_x) float32 scores of a Local detector at every cell."""
    x = local_inputs(p, standardise(p, features))
    logits = x.reshape(-1, LOCAL_DIMS).astype(np.float64) @ d.weights.astype(np.float64) + float(d.bias)
    return _sigmoid(logits.astype(np.float32)).reshape(p.config.grid_y, p.config.grid_x)


def evaluate_detector(p: VisionPack, d: Detector, features: np.ndarray) -> dict:
    """Per-frame output like vision_eval.cpp: score, cx, cy, cells, present."""
    if d.kind == KIND_GLOBAL:
        s = score_global(p, d, features)
        return {"score": s, "cx": 0, "cy": 0, "cells": 0, "present": s >= d.thr_on}
    m = score_local_map(p, d, features)
    row_end = min(p.config.grid_y - 1, d.row_max)
    if d.row_min > row_end:
        return {"score": 0.0, "cx": 0, "cy": 0, "cells": 0, "present": False}
    sub = m[d.row_min: row_end + 1, :]
    idx = int(np.argmax(sub))                   # first maximum in row-major order
    cy, cx = divmod(idx, p.config.grid_x)
    best = float(sub[cy, cx])
    cells = int((sub >= d.thr_on).sum())
    return {"score": best, "cx": int(cx), "cy": int(cy + d.row_min), "cells": cells,
            "present": best >= d.thr_on and cells >= d.min_cells}


class Evaluator:
    """Temporal confirmation/release, mirroring VisionEvaluator::step."""

    def __init__(self, pack: VisionPack):
        self.pack = pack
        self.state = [{"above": 0, "below": 0, "active": False} for _ in pack.detectors]

    def reset(self):
        for s in self.state:
            s.update(above=0, below=0, active=False)

    def step(self, features: np.ndarray) -> list:
        outs = []
        for d, t in zip(self.pack.detectors, self.state):
            o = evaluate_detector(self.pack, d, features)
            if not t["active"]:
                t["above"] = t["above"] + 1 if o["present"] else 0
                if t["above"] >= d.confirm_frames:
                    t["active"], t["below"] = True, 0
            else:
                t["below"] = 0 if o["score"] >= d.thr_off else t["below"] + 1
                if t["below"] >= d.release_frames:
                    t["active"], t["above"] = False, 0
            o["active"] = t["active"]
            o["name"] = d.name
            outs.append(o)
        return outs


def make_selftest(p: VisionPack, rgb: np.ndarray) -> SelfTest:
    """Self-test block from a full-size frame: its small image, features and per-detector scores."""
    small = downscale(rgb, p.config.downscale)
    feats = compute_features(small, p.config)
    scores = np.array([evaluate_detector(p, d, feats)["score"] for d in p.detectors], dtype=np.float32)
    return SelfTest(small, feats, scores)
