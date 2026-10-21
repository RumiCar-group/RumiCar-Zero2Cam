#!/usr/bin/env python3
"""Generates the golden data of car/tests/test_vision.cpp with the Python
implementation (car/tools/rumicar_vision.py), so the C++ and Python feature
pipelines are checked against each other:

  frame.ppm        a synthetic 320×240 frame (deterministic: gradients, coloured
                   discs, stripes, noise with a fixed seed)
  features.json    the configuration, the expected features of the frame, a few
                   pixel-level expectations (hue, class), and the expected
                   detector outputs for pack.bin
  pack.bin         a small vision pack with hand-made detectors and a self-test
                   block made from frame.ppm

Run from the repository root:  python3 car/tests/data/vision/make_golden.py
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
import rumicar_vision as rv  # noqa: E402

W, H = 320, 240


def synthetic_frame() -> np.ndarray:
    rng = np.random.default_rng(20261021)
    y, x = np.mgrid[0:H, 0:W]
    img = np.zeros((H, W, 3), dtype=np.float64)
    img[:, :, 0] = 40 + 120 * x / W            # red ramps left -> right
    img[:, :, 1] = 60 + 100 * y / H            # green ramps top -> bottom
    img[:, :, 2] = 90                          # flat blue

    def disc(cx, cy, r, color):
        m = (x - cx) ** 2 + (y - cy) ** 2 <= r * r
        img[m] = color

    disc(80, 70, 18, (230, 30, 30))       # red light
    disc(240, 70, 18, (30, 220, 60))      # green light
    disc(160, 150, 22, (240, 220, 40))    # yellow
    disc(50, 190, 15, (40, 60, 230))      # blue
    disc(270, 190, 15, (230, 40, 220))    # magenta
    img[200:240, 100:220] = (245, 245, 245)   # bright floor patch
    img[0:30, 120:200] = (12, 12, 12)         # dark band
    img[110:118, :] = (200, 200, 200)         # horizontal stripe (vertical gradient)
    img[:, 300:304] = (20, 20, 20)            # vertical stripe (horizontal gradient)
    img += rng.normal(0, 6, size=img.shape)   # sensor-like noise
    return np.clip(np.rint(img), 0, 255).astype(np.uint8)


def write_ppm(path: str, rgb: np.ndarray) -> None:
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (rgb.shape[1], rgb.shape[0]))
        f.write(np.ascontiguousarray(rgb).tobytes())


def main() -> int:
    rgb = synthetic_frame()
    write_ppm(os.path.join(HERE, "frame.ppm"), rgb)
    cfg = rv.FeatureConfig()
    assert cfg.validate(W, H) == ""
    small = rv.downscale(rgb, cfg.downscale)
    feats = rv.compute_features(small, cfg)

    # Pixel-level expectations (hue and class of chosen small-image pixels and of fixed colours).
    fixed = [(255, 0, 0), (255, 255, 0), (0, 255, 0), (0, 255, 255), (0, 0, 255), (255, 0, 255), (200, 200, 200),
             (30, 30, 30), (255, 128, 0), (128, 0, 255), (0, 0, 0), (120, 130, 125), (90, 200, 30), (200, 90, 90)]
    pixels = []
    for (r, g, b) in fixed:
        arr = np.array([[[r, g, b]]], dtype=np.uint8)
        cls, _, _ = rv.classify(arr, cfg)
        mx, mn = max(r, g, b), min(r, g, b)
        hue = int(rv.hue_degrees(np.array([r]), np.array([g]), np.array([b]))[0]) if mx > mn else -1
        pixels.append({"rgb": [r, g, b], "hue": hue, "class": int(cls[0, 0])})
    for (sy, sx) in [(0, 0), (14, 16), (14, 48), (30, 32), (38, 10), (38, 54), (44, 30), (23, 20), (47, 63)]:
        r, g, b = (int(v) for v in small[sy, sx])
        cls, _, _ = rv.classify(small[sy:sy + 1, sx:sx + 1], cfg)
        mx, mn = max(r, g, b), min(r, g, b)
        hue = int(rv.hue_degrees(np.array([r]), np.array([g]), np.array([b]))[0]) if mx > mn else -1
        pixels.append({"small_xy": [sx, sy], "rgb": [r, g, b], "hue": hue, "class": int(cls[0, 0])})

    # Statistics for standardisation from this one frame (std floored to 1).
    f3 = feats.reshape(cfg.grid_y, cfg.grid_x, rv.FEATURES_PER_CELL).astype(np.float64)
    mean = f3.reshape(-1, rv.FEATURES_PER_CELL).mean(axis=0)
    std = np.maximum(f3.reshape(-1, rv.FEATURES_PER_CELL).std(axis=0), 1.0)
    pack = rv.VisionPack(cfg, W, H, mean.astype(np.float32), std.astype(np.float32))

    # Hand-made detectors: a red-blob local detector (weight on the centre cell's
    # red count), a green one, and a global "bright scene" detector.
    red = rv.Detector("red_light", rv.KIND_LOCAL, np.zeros(rv.LOCAL_DIMS, dtype=np.float32), -1.5, 0.6, 0.4, 2, 3, 1, 0, 4)
    red.weights[4 * rv.FEATURES_PER_CELL + rv.CLASS_NAMES.index("red")] = 2.5    # centre cell of the 3×3 window
    red.weights[4 * rv.FEATURES_PER_CELL + rv.CLASS_NAMES.index("green")] = -1.0
    green = rv.Detector("green_light", rv.KIND_LOCAL, np.zeros(rv.LOCAL_DIMS, dtype=np.float32), -1.5, 0.6, 0.4, 2, 3, 1, 0, 4)
    green.weights[4 * rv.FEATURES_PER_CELL + rv.CLASS_NAMES.index("green")] = 2.5
    green.weights[1 * rv.FEATURES_PER_CELL + rv.CLASS_NAMES.index("green")] = 0.5   # cell above
    bright = rv.Detector("bright_scene", rv.KIND_GLOBAL, np.zeros(cfg.dims, dtype=np.float32), 0.2, 0.5, 0.3, 1, 1, 1, 0, 255)
    rng = np.random.default_rng(7)
    bright.weights[:] = rng.normal(0, 0.05, size=cfg.dims).astype(np.float32)
    bright.weights[rv.CLASS_NAMES.index("bright")::rv.FEATURES_PER_CELL] += 0.3
    pack.detectors = [red, green, bright]
    pack.selftest = rv.make_selftest(pack, rgb)
    data = rv.encode_pack(pack)
    with open(os.path.join(HERE, "pack.bin"), "wb") as f:
        f.write(data)
    back = rv.decode_pack(data)
    assert rv.encode_pack(back) == data

    outputs = [rv.evaluate_detector(pack, d, feats) for d in pack.detectors]
    doc = {
        "config": cfg.__dict__,
        "image": [W, H],
        "small": [small.shape[1], small.shape[0]],
        "dims": int(cfg.dims),
        "features": [int(v) for v in feats],
        "pixels": pixels,
        "mean": [float(v) for v in pack.mean],
        "std": [float(v) for v in pack.std],
        "detectors": [{"name": d.name, "kind": d.kind, "score": o["score"], "cx": o["cx"], "cy": o["cy"], "cells": o["cells"],
                       "present": o["present"]} for d, o in zip(pack.detectors, outputs)],
        "pack_crc32": int.from_bytes(data[-4:], "little"),
        "pack_bytes": len(data),
    }
    with open(os.path.join(HERE, "features.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    print(f"frame {W}x{H}, small {small.shape[1]}x{small.shape[0]}, {cfg.dims} features, pack {len(data)} bytes")
    for d, o in zip(pack.detectors, outputs):
        print(f"  {d.name}: score {o['score']:.4f} cell ({o['cx']},{o['cy']}) cells {o['cells']} present {o['present']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
