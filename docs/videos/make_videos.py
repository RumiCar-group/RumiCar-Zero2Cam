"""Explainer videos for RumiCar Zero2Cam (Japanese and English), 1280x720, 30 fps, silent.

Videos (each in ja and en, written as <out dir>/<name>-<lang>.mp4):
  01-tof-readings  what the ToF readings mean (0..200 mm / -3 / -1) and the "open" of tof_drive
  02-auto-start    start ramp and auto limit when auto mode is entered (no sudden start)
  03-overtaking    following and overtaking a parked vehicle (cam_drive vehicle behaviour)

Every number shown is a default in the sources (ABI 3, v0.4.0): core/src/main.cpp to_tof_value and
include/rumicar/logic_api.h (RUMI_TOF_MAX_MM 200, -3, -1), core.json (tof_read_timeout_ms 100,
auto_accel_pwm_per_s 287, max_drive_pwm 255), tof_logic.hpp (conf_max_mm 200, invalid_hold_ms 300),
cam_params.hpp (follow_gap_mm 150, follow_v 179, parked_confirm_ms 2000, side_clear_mm 120, overtake_v 128,
out_ms 500, pass_min_ms 800, pass_max_ms 3000, pass_clear_mm 150, pass_clear_ms 700, in_ms 500, ignore_ms 1500)
and pc/RumiCar.Protocol/RumiCarApi.cs TofText.  Change the texts here when the sources change.

Geometry: the three VL53L0X sit at the front of the body; the left and right ones look 65 degrees
outwards from the heading (RumiCarRos rumi_hw/urdf/rumi.urdf.xacro; the simulator uses the same),
not sideways.  car() draws them that way and the overtaking scenes measure along those rays.

Each scene is rendered to its own H.264 file (raw frames piped to ffmpeg) and the
scenes are joined with the concat demuxer.

Requires Python 3 with Pillow, ffmpeg (libx264) and the Noto Sans CJK fonts
(Debian/Ubuntu: fonts-noto-cjk; paths in FONT_R / FONT_B).

usage:
  python3 docs/videos/make_videos.py <out dir> [name ...] [--lang ja|en]
  python3 docs/videos/make_videos.py <out dir> --still <name> <scene index> <time s> [--lang ja|en]
"""
import math
import os
import shutil
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

W, H, FPS = 1280, 720, 30
FONT_R = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
FONT_B = "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"
NAVY = (31, 56, 100)
INK = (25, 25, 25)
GREY = (120, 128, 140)
LIGHT = (232, 238, 247)
BG = (250, 251, 253)
WALL = (90, 98, 110)
RED = (200, 40, 40)
GREEN = (30, 140, 70)
ORANGE = (220, 120, 20)
BLUE = (40, 110, 200)

_fonts = {}


def font(size, bold=False):
    key = (size, bold)
    if key not in _fonts:
        _fonts[key] = ImageFont.truetype(FONT_B if bold else FONT_R, size, index=0)
    return _fonts[key]


def clamp(x, a=0.0, b=1.0):
    return max(a, min(b, x))


def ease(x):
    x = clamp(x)
    return x * x * (3 - 2 * x)


def lerp(a, b, x):
    return a + (b - a) * x


def mix(c1, c2, x):
    return tuple(int(lerp(a, b, clamp(x))) for a, b in zip(c1, c2))


# ---------------------------------------------------------------------------
# common frame furniture
# ---------------------------------------------------------------------------
def base(title, step, total, lang):
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, W, 64], fill=NAVY)
    d.text((32, 12), title, font=font(28, True), fill=(255, 255, 255))
    tag = f"{step} / {total}"
    tw = d.textlength(tag, font=font(22))
    d.text((W - 32 - tw, 18), tag, font=font(22), fill=(200, 210, 230))
    return im, d


def caption(d, lines, t=1.0):
    """Bottom caption box with up to three lines; fades in over the first 0.4 s."""
    a = clamp(t / 0.4)
    size = 28
    while size > 20 and max(ImageDraw.Draw(Image.new("RGB", (1, 1))).textlength(ln, font=font(size)) for ln in lines) > W - 80:
        size -= 1
    y0 = H - 40 - 44 * len(lines)
    d.rectangle([0, y0 - 18, W, H], fill=mix(BG, (255, 255, 255), a), outline=None)
    d.line([0, y0 - 18, W, y0 - 18], fill=mix(BG, (210, 216, 226), a), width=2)
    for i, ln in enumerate(lines):
        d.text((40, y0 + i * 44), ln, font=font(size), fill=mix(BG, INK, a))


def note(d, text, xy=(40, 80)):
    d.text(xy, text, font=font(20), fill=GREY)


def label(d, xy, text, size=26, color=INK, bold=False, anchor="la"):
    d.text(xy, text, font=font(size, bold), fill=color, anchor=anchor)


def pill(d, xy, text, fg=(255, 255, 255), bg=NAVY, size=26):
    x, y = xy
    tw = d.textlength(text, font=font(size, True))
    d.rounded_rectangle([x, y, x + tw + 32, y + size + 22], radius=(size + 22) // 2, fill=bg)
    d.text((x + 16, y + 9), text, font=font(size, True), fill=fg)


TOF_YAW = math.radians(65)   # left / right ToF: 65 degrees outwards from the heading (urdf: yaw +-65 deg)
# ToF mounting points in the car's frame (x forward, y to the right; px of the 110 x 56 body ~ 190 x 80 mm):
# centre 0.135 m ahead of the rear axle (~25 mm behind the front edge), left / right +-18 mm.
TOF_C, TOF_L, TOF_R = (41, 0), (38, -6), (38, 6)


def tof_dir(ang, which):
    """Unit vector (screen coordinates, y down) of the ToF `which` (L / C / R) of a car heading `ang`."""
    a = ang + (-TOF_YAW if which == "L" else TOF_YAW if which == "R" else 0.0)
    return math.cos(a), math.sin(a)


def car(d, cx, cy, ang=0.0, color=NAVY, length=110, width=56, steer=0, label_text=None, tof=False):
    """Top view of a car centred at (cx, cy), heading `ang` radians (0 = +x, screen y down).
    tof=True draws the three ToF directions (centre forward, left / right 65 degrees outwards)."""
    ca, sa = math.cos(ang), math.sin(ang)

    def p(x, y):
        return (cx + x * ca - y * sa, cy + x * sa + y * ca)

    hl, hw = length / 2, width / 2
    body = [p(-hl, -hw), p(hl - 14, -hw), p(hl, -hw + 14), p(hl, hw - 14), p(hl - 14, hw), p(-hl, hw)]
    d.polygon(body, fill=color)
    # wheels (front ones turned by `steer`: -1 left, 0, +1 right).  Screen y points down, so a left turn
    # (towards -y) is a negative rotation: a2 = ang + steer * 0.45 (steer = -1 -> -0.45).
    for wx, wy, front in ((-hl + 18, -hw - 4, False), (-hl + 18, hw + 4, False), (hl - 22, -hw - 4, True), (hl - 22, hw + 4, True)):
        a2 = ang + (steer * 0.45 if front else 0.0)
        c2, s2 = math.cos(a2), math.sin(a2)
        mx, my = p(wx, wy)
        pts = [(mx + u * c2 - v * s2, my + u * s2 + v * c2) for u, v in ((-11, -5), (11, -5), (11, 5), (-11, 5))]
        d.polygon(pts, fill=(40, 40, 40))
    if tof:
        for which, (tx, ty) in (("L", TOF_L), ("C", TOF_C), ("R", TOF_R)):
            sx, sy = p(tx, ty)
            dx, dy = tof_dir(ang, which)
            d.line([sx, sy, sx + 34 * dx, sy + 34 * dy], fill=(120, 190, 255), width=3)
            d.ellipse([sx - 4, sy - 4, sx + 4, sy + 4], fill=(120, 190, 255))
    if label_text:
        d.text(p(0, 0), label_text, font=font(18, True), fill=(255, 255, 255), anchor="mm")
    return p


def ray_hit(sx, sy, dx, dy, rects, y_wall):
    """First hit of the ray from (sx, sy) in direction (dx, dy) with one of the axis-aligned `rects`
    [(x0, y0, x1, y1), ...] or the horizontal wall y = y_wall.  Returns (x, y, hit_rect)."""
    best = None
    if dy != 0:
        t = (y_wall - sy) / dy
        if t > 0:
            best = (t, False)
    for x0, y0, x1, y1 in rects:
        tmin, tmax = -1e9, 1e9
        ok = True
        for s0, dd, lo, hi in ((sx, dx, x0, x1), (sy, dy, y0, y1)):
            if abs(dd) < 1e-9:
                if not (lo <= s0 <= hi):
                    ok = False
                    break
            else:
                t0, t1 = (lo - s0) / dd, (hi - s0) / dd
                tmin, tmax = max(tmin, min(t0, t1)), min(tmax, max(t0, t1))
        if ok and tmax >= max(tmin, 0) and tmin > 0 and (best is None or tmin < best[0]):
            best = (tmin, True)
    if best is None:
        return sx + 400 * dx, sy + 400 * dy, False
    t, hit = best
    return sx + t * dx, sy + t * dy, hit


# ---------------------------------------------------------------------------
# video 1: ToF readings
# ---------------------------------------------------------------------------
T1 = {
    "ja": {
        "title": "ToF センサーの読み取り値と表示",
        "s0": ["RumiCar Zero2Cam：VL53L0X（ToF 距離センサー）×3 の値の意味", "0〜200 mm・−3・−1 の違いと、走行ロジック tof_drive での扱い", "値は RumiCar ライブラリ（Arduino・RasPi・drive.py の RC_read）と同じ"],
        "s1": ["レーザーが対象物で反射して戻ると、往復時間から距離を算出します", "0〜200 mm はそのまま距離。表示：測距値 [mm]（例 180）　drive.py：RC_read() = 180"],
        "s2": ["対象物を検出できなかった場合（反射光が戻らない・測距範囲外）と、", "200 mm を超える距離は −3（20 cm 以内に何もない＝開けている）", "表示：-3 範囲外　drive.py：RC_read() = -3"],
        "s3": ["反射光が戻っても受信した信号が弱く、規定値に満たない場合も −3", "（暗い面・浅い角度の面など）。表示：-3 範囲外"],
        "s4": ["既定 100 ms（i2c.tof_read_timeout_ms）以内に測定結果が得られない、", "または I2C 通信エラー → −1（読めない）。表示：-1 タイムアウト　drive.py：RC_read() = -1", "tof_drive は前の値を 300 ms（invalid_hold_ms）保持し、超過すると SENSOR_FAULT で停止"],
        "s5": ["tof_drive は conf_max_mm（既定 200 mm）以上の読みと −3 を「開放」", "（判断対象の距離内に障害物なし）として扱います", "−3 は数としては最も小さいので、drive.py では比較の前に大きな値へ置き換えます"],
        "mm": "測距値", "nores": "反射光なし／200 mm 超", "weak": "受信信号 < 規定値", "timeout": "タイムアウト／I2C エラー",
        "sensor": "VL53L0X", "obj": "対象物", "dark": "暗い面・浅い角度", "range": "20 cm（RC_read の上限）",
        "open_zone": "開放", "dist_zone": "距離として使用", "axis": "読み取り値 [mm]",
        "schem": "模式図（縮尺は実際と異なる）", "hold": "前の値を保持 300 ms", "fault": "SENSOR_FAULT（停止）",
    },
    "en": {
        "title": "ToF sensor readings and their display",
        "s0": ["RumiCar Zero2Cam: what the values of the three VL53L0X (ToF distance sensors) mean", "0–200 mm, −3 and −1, and how the driving logic tof_drive uses them", "The values are those of the RumiCar library (Arduino, RasPi, RC_read in drive.py)"],
        "s1": ["When the laser light is reflected by an object and returns, the distance is", "computed from the round-trip time. 0–200 mm is the distance. Display: range in mm (e.g. 180)", "drive.py: RC_read() = 180"],
        "s2": ["When no target is detected (no light returns, beyond the measuring range) and", "for distances over 200 mm the value is −3 (nothing within 20 cm = open)", "Display: -3 範囲外 (out of range)    drive.py: RC_read() = -3"],
        "s3": ["Also −3 when light returns but the received signal is too weak,", "below the configured limit (dark or grazing surfaces). Display: -3 範囲外"],
        "s4": ["No measurement within 100 ms (i2c.tof_read_timeout_ms, default), or an I2C", "error → −1 (no reading). Display: -1 タイムアウト (timeout)    drive.py: RC_read() = -1", "tof_drive holds the previous value for 300 ms (invalid_hold_ms), then stops in SENSOR_FAULT"],
        "s5": ["tof_drive treats readings at or above conf_max_mm (default 200 mm) and −3", "as \"open\" (no obstacle within the judged distance).", "−3 is the smallest number, so drive.py replaces it with a large value before comparing"],
        "mm": "range", "nores": "no reflected light / over 200 mm", "weak": "received signal < limit", "timeout": "timeout / I2C error",
        "sensor": "VL53L0X", "obj": "object", "dark": "dark / grazing surface", "range": "20 cm (RC_read limit)",
        "open_zone": "open", "dist_zone": "used as a distance", "axis": "reading [mm]",
        "schem": "Schematic (not to scale)", "hold": "previous value held 300 ms", "fault": "SENSOR_FAULT (stop)",
    },
}


def beam_scene(d, L, t, mode):
    """Sensor at left, beam to the right.  mode: ok / none / weak / fail."""
    sx, sy = 230, 330
    d.rounded_rectangle([sx - 110, sy - 40, sx, sy + 40], radius=8, fill=NAVY)
    label(d, (sx - 55, sy), L["sensor"], 18, (255, 255, 255), True, "mm")
    note(d, L["schem"])
    period = 2.4
    ph = (t % period) / period
    if mode == "ok":
        wx = 820
        d.rectangle([wx, 150, wx + 26, 510], fill=WALL)
        label(d, (wx + 13, 140), L["obj"], 22, INK, False, "md")
        if ph < 0.45:
            x = lerp(sx, wx, ph / 0.45)
            d.ellipse([x - 10, sy - 10, x + 10, sy + 10], fill=RED)
        elif ph < 0.9:
            x = lerp(wx, sx, (ph - 0.45) / 0.45)
            d.ellipse([x - 10, sy - 10, x + 10, sy + 10], fill=ORANGE)
        d.line([sx, sy, wx, sy], fill=(230, 170, 170), width=2)
        if t > 1.0:
            pill(d, (470, 440), f"{L['mm']} 180 mm", bg=GREEN)
    elif mode == "none":
        d.line([sx, sy, 1180, sy], fill=(230, 170, 170), width=2)
        d.line([sx + 680, sy - 90, sx + 680, sy + 90], fill=GREY, width=2)
        label(d, (sx + 690, sy - 90), L["range"], 20, GREY)
        x = lerp(sx, 1200, ph)
        r = 10 * (1 - 0.6 * ph)
        d.ellipse([x - r, sy - r, x + r, sy + r], fill=mix(RED, BG, ph))
        if t > 1.0:
            pill(d, (420, 440), f"{L['nores']} → −3", bg=ORANGE)
    elif mode == "weak":
        pts = [(780, 160), (840, 160), (900, 520), (840, 520)]
        d.polygon(pts, fill=(60, 60, 64))
        label(d, (860, 140), L["dark"], 22, INK, False, "md")
        if ph < 0.45:
            x = lerp(sx, 800, ph / 0.45)
            d.ellipse([x - 10, sy - 10, x + 10, sy + 10], fill=RED)
        elif ph < 0.9:
            x = lerp(800, sx, (ph - 0.45) / 0.45)
            d.ellipse([x - 4, sy - 4, x + 4, sy + 4], fill=mix(ORANGE, BG, 0.5))
        d.line([sx, sy, 800, sy], fill=(230, 170, 170), width=2)
        if t > 1.0:
            pill(d, (300, 470), f"{L['weak']} → −3", bg=ORANGE)
    elif mode == "fail":
        d.line([sx - 110, sy - 40, sx, sy + 40], fill=RED, width=6)
        d.line([sx - 110, sy + 40, sx, sy - 40], fill=RED, width=6)
        pill(d, (300, 300), f"{L['timeout']} → −1", bg=RED)
        # hold timeline
        x0, x1, y = 300, 1100, 430
        d.line([x0, y, x1, y], fill=GREY, width=3)
        hold_end = lerp(x0, x1, 0.3 / 0.6)
        prog = clamp((t - 1.0) / 3.0)
        cur = lerp(x0, x1, prog)
        d.rectangle([x0, y - 14, min(cur, hold_end), y + 14], fill=(250, 220, 150))
        if cur > hold_end:
            d.rectangle([hold_end, y - 14, cur, y + 14], fill=(240, 160, 160))
        for ms in (0, 100, 200, 300, 400, 500, 600):
            xx = lerp(x0, x1, ms / 600)
            d.line([xx, y + 16, xx, y + 26], fill=GREY, width=2)
            label(d, (xx, y + 30), f"{ms} ms", 18, GREY, False, "ma")
        label(d, ((x0 + hold_end) / 2, y - 22), L["hold"], 22, INK, False, "md")
        if cur > hold_end:
            label(d, ((hold_end + x1) / 2, y - 22), L["fault"], 22, RED, True, "md")


def numberline(d, L, t):
    x0, x1, y = 160, 1120, 330
    mm_max = 250   # the sensor reading is cut off at 200 mm (RUMI_TOF_MAX_MM): beyond it the value is -3

    def X(mm):
        return lerp(x0, x1, mm / mm_max)

    cmax = 200
    d.rectangle([X(cmax), y - 70, x1, y + 18], fill=(222, 240, 226))
    d.rectangle([x0, y - 70, X(cmax), y + 18], fill=(232, 238, 247))
    label(d, ((X(cmax) + x1) / 2, y - 60), L["open_zone"], 26, GREEN, True, "ma")
    label(d, ((x0 + X(cmax)) / 2, y - 60), L["dist_zone"], 26, NAVY, True, "ma")
    d.line([x0, y + 18, x1, y + 18], fill=INK, width=3)
    for mm in range(0, 201, 50):
        d.line([X(mm), y + 18, X(mm), y + 30], fill=INK, width=2)
        label(d, (X(mm), y + 34), str(mm), 20, INK, False, "ma")
    label(d, ((X(cmax) + x1) / 2, y - 26), "−3", 22, GREEN, True, "ma")
    d.line([X(cmax), y - 80, X(cmax), y + 30], fill=RED, width=3)
    label(d, (X(cmax), y - 84), "conf_max_mm = 200 = RC_read の上限" if L["mm"] == "測距値" else "conf_max_mm = 200 = RC_read limit", 22, RED, True, "md")
    label(d, ((x0 + x1) / 2, y + 66), L["axis"], 22, GREY, False, "ma")
    v = 60 + 180 * (0.5 - 0.5 * math.cos(t * 1.3))   # true distance 60..240 mm; the reading is -3 beyond 200
    d.polygon([(X(v), y + 12), (X(v) - 12, y - 10), (X(v) + 12, y - 10)], fill=ORANGE)
    pill(d, (X(v) - 70, y - 160), "−3" if v > cmax else f"{int(v)} mm", bg=GREEN if v > cmax else NAVY, size=24)


def video1(lang):
    L = T1[lang]
    n = 6
    return [
        (5.0, lambda t: title_scene(L["title"], L["s0"], t)),
        (7.0, lambda t: scene(L, 2, n, L["s1"], t, lambda d: beam_scene(d, L, t, "ok"))),
        (8.0, lambda t: scene(L, 3, n, L["s2"], t, lambda d: beam_scene(d, L, t, "none"))),
        (7.0, lambda t: scene(L, 4, n, L["s3"], t, lambda d: beam_scene(d, L, t, "weak"))),
        (8.0, lambda t: scene(L, 5, n, L["s4"], t, lambda d: beam_scene(d, L, t, "fail"))),
        (9.0, lambda t: scene(L, 6, n, L["s5"], t, lambda d: numberline(d, L, t))),
    ]


# ---------------------------------------------------------------------------
# video 2: start of auto mode (start ramp and auto limit)
# ---------------------------------------------------------------------------
T2 = {
    "ja": {
        "title": "自動モードの発進制御（急発進の防止）",
        "s0": ["RumiCar Zero2Cam：自動モードへの切り替え時の駆動の制限", "auto_accel_pwm_per_s と「自動の最大出力」（駆動の値は RC_drive と同じ 0〜255）"],
        "s1": ["ロジックはどのモードでも動作しており、前方が開放なら最大出力を指令しています", "そのまま出力すると、自動への切り替え直後に急発進します"],
        "s2": ["本体は駆動の許容値を 0 から 287 /s（auto_accel_pwm_per_s）で増加させ、出力を制限します", "許容値 = min(増加分, 自動の最大出力, max_drive_pwm)", "最大出力 255（全開）では約 0.89 秒（255 ÷ 287）で上限に達します"],
        "s3": ["「自動の最大出力」を 128（約 50%）にすると、約 0.45 秒（128 ÷ 287）で 128 に達し、以後 128 で頭打ち", "許容値が上限に達した後は、ロジック自身の減速・ブレーキ・後退には影響しません", "手動・収録のみ・非常停止・step() の失敗のフレームで許容値は 0 に戻ります"],
        "cmd": "ロジックの指令", "ramp": "0 からの増加分（287 /s）", "lim": "自動の最大出力", "out": "モーターへの出力",
        "naive": "制限なしの出力", "x": "自動に切り替えてからの時間 [s]", "y": "駆動（RC_drive の値 0〜255）",
    },
    "en": {
        "title": "Start of auto mode (preventing a sudden start)",
        "s0": ["RumiCar Zero2Cam: limiting the drive when switching to auto mode", "auto_accel_pwm_per_s and the auto limit (自動の最大出力); drive values are RC_drive's 0–255"],
        "s1": ["The logic runs in every mode; with the way ahead open it commands maximum output.", "Output as is, the car would start suddenly right after switching to auto"],
        "s2": ["The core raises the permitted drive from 0 at 287 per second (auto_accel_pwm_per_s) and limits the output:", "permitted = min(increase, auto limit, max_drive_pwm)", "At an auto limit of 255 (full) the limit is reached in about 0.89 s (255 ÷ 287)"],
        "s3": ["With the auto limit at 128 (about 50 %), 128 is reached in about 0.45 s (128 ÷ 287) and then held", "Once the limit is reached, the logic's own deceleration, braking and reversing are unaffected", "In manual, record-only, e-stop and failed-step frames the permitted value returns to 0"],
        "cmd": "logic command", "ramp": "increase from 0 (287 per second)", "lim": "auto limit", "out": "output to the motor",
        "naive": "output without limiting", "x": "time since switching to auto [s]", "y": "drive (RC_drive value 0–255)",
    },
}


FULL, ACCEL = 255, 287   # RC_drive full scale and auto_accel_pwm_per_s (core.json)


def graph(d, L, t, limit, show_naive, show_ramp):
    """limit: the auto limit in RC_drive units (0..255)."""
    gx0, gx1, gy0, gy1 = 180, 1100, 460, 130   # the axis labels stay above the caption box (three lines from y = 530)
    tmax = 1.4

    def X(s):
        return lerp(gx0, gx1, s / tmax)

    def Y(pwm):
        return lerp(gy0, gy1, pwm / FULL)

    d.line([gx0, gy0, gx1, gy0], fill=INK, width=2)
    d.line([gx0, gy0, gx0, gy1 - 10], fill=INK, width=2)
    for pwm in (0, 64, 128, 192, 255):
        d.line([gx0 - 8, Y(pwm), gx0, Y(pwm)], fill=INK, width=2)
        label(d, (gx0 - 14, Y(pwm)), str(pwm), 20, INK, False, "rm")
        d.line([gx0, Y(pwm), gx1, Y(pwm)], fill=(228, 232, 238), width=1)
    for s10 in range(0, 15, 2):
        s = s10 / 10
        d.line([X(s), gy0, X(s), gy0 + 8], fill=INK, width=2)
        label(d, (X(s), gy0 + 12), f"{s:.1f}", 20, INK, False, "ma")
    label(d, ((gx0 + gx1) / 2, gy0 + 40), L["x"], 22, GREY, False, "ma")
    label(d, (gx0 - 60, gy1 - 44), L["y"], 22, GREY)
    cur = clamp(t / 4.0) * tmax          # the cursor sweeps the time axis in 4 s
    # logic command: full (255) all the time
    for s in [i * 0.02 for i in range(int(tmax / 0.02))][::2]:
        d.line([X(s), Y(FULL), X(s + 0.02), Y(FULL)], fill=GREY, width=3)
    label(d, (X(1.05), Y(FULL) - 8), L["cmd"], 20, GREY, False, "ld")
    if show_naive:
        d.line([X(0), Y(FULL), X(cur), Y(FULL)], fill=RED, width=6)
        label(d, (X(0.05), Y(FULL) + 10), L["naive"], 22, RED, True)
    if show_ramp:
        if limit < FULL:
            d.line([X(0), Y(limit), X(tmax), Y(limit)], fill=ORANGE, width=3)
            label(d, (X(1.05), Y(limit) - 8), f"{L['lim']} {limit}", 20, ORANGE, True, "ld")
        rend = min(tmax, FULL / ACCEL)
        d.line([X(0), Y(0), X(rend), Y(FULL)], fill=(150, 170, 210), width=2)
        label(d, (X(0.30), Y(0.52 * FULL)), L["ramp"], 20, BLUE, False, "rd")
        # output = min(ramp, limit); sampled every 50 ms as in the control loop
        pts = []
        s = 0.0
        while s <= cur + 1e-9:
            pts.append((X(s), Y(min(ACCEL * s, limit))))
            s += 0.05
        if len(pts) > 1:
            d.line(pts, fill=GREEN, width=6)
        reach = limit / ACCEL
        d.line([X(reach), Y(limit), X(reach), gy0], fill=(170, 200, 170), width=2)
        label(d, (X(reach), gy0 - 6), f"{reach:.2f} s", 20, GREEN, True, "md")
        lx = min(X(min(cur, tmax)) + 10, gx1 - d.textlength(L["out"], font=font(22, True)))
        label(d, (lx, Y(min(ACCEL * cur, limit)) + 10), L["out"], 22, GREEN, True)
    d.line([X(cur), gy0, X(cur), gy1], fill=(200, 200, 200), width=1)


def video2(lang):
    L = T2[lang]
    n = 4
    return [
        (5.0, lambda t: title_scene(L["title"], L["s0"], t)),
        (6.0, lambda t: scene(L, 2, n, L["s1"], t, lambda d: graph(d, L, t, FULL, True, False))),
        (8.0, lambda t: scene(L, 3, n, L["s2"], t, lambda d: graph(d, L, t, FULL, False, True))),
        (9.0, lambda t: scene(L, 4, n, L["s3"], t, lambda d: graph(d, L, t, 128, False, True))),
    ]


# ---------------------------------------------------------------------------
# video 3: overtaking a parked vehicle (cam_drive vehicle behaviour)
# ---------------------------------------------------------------------------
T3 = {
    "ja": {
        "title": "駐車車両の追い越し（cam_drive の車両の挙動）",
        "s0": ["RumiCar Zero2Cam：前方車両への追従と駐車車両の追い越し", "状態 FOLLOW → OVERTAKE_OUT → OVERTAKE_PASS → OVERTAKE_IN（既定値で説明）"],
        "s1": ["自車線に車両を確定すると FOLLOW：旋回せずに追従し、車間（ToF 中央）で速度を制限", "車間 150 mm（follow_gap_mm）以下で速度の上限 0（停止）、190 mm で 179（follow_v）", "ToF は 3 基とも車体の前にあり、左右は前方 65° 向き（真横ではない）"],
        "s2": ["停止したまま車間が広がらない状態が 2 秒（parked_confirm_ms）続くと駐車車両と判定", "追い越し側の ToF（前方 65°）が 120 mm（side_clear_mm）以上か −3 なら OVERTAKE_OUT、", "未満なら FOLLOW_WAIT"],
        "s3": ["OVERTAKE_OUT：追い越し側へ最大舵角で 500 ms（out_ms）進路変更", "速度の上限 128（overtake_v・全開 255 の約 50%）"],
        "s4": ["OVERTAKE_PASS：操舵を中立にして並走。内側の ToF が 150 mm 未満（車両を検出）の後、150 mm 以上", "か −3 が 700 ms（pass_clear_ms）続き、かつ 800 ms 以上経過で通過完了（最長 3000 ms）", "内側の ToF は前方 65° 向きなので、自車の前端が車両の前端を過ぎると見失う。700 ms で車体の残りが通過"],
        "s5": ["OVERTAKE_IN：反対側へ最大舵角で 500 ms（in_ms）元の車線へ復帰", "その後 1.5 秒（ignore_ms）は車両の検出を無視。赤信号・交差点で中止"],
        "parked": "駐車車両", "own": "自車", "gap": "車間", "timer": "停止・車間不変", "side": "追い越し側 ToF（前方 65°）",
        "inner": "内側 ToF（前方 65°）", "schem": "模式図（時間は既定値、距離・速度は実際と異なる）", "ignore": "車両の検出を無視",
        "tof3": "ToF ×3：中央は正面、左右は前方 65° 向き", "lost": "見失った（前端が車両の前端を過ぎた）→ 700 ms 計測",
    },
    "en": {
        "title": "Overtaking a parked vehicle (cam_drive vehicle behaviour)",
        "s0": ["RumiCar Zero2Cam: following a vehicle ahead and overtaking a parked one", "States FOLLOW → OVERTAKE_OUT → OVERTAKE_PASS → OVERTAKE_IN (with the defaults)"],
        "s1": ["A vehicle confirmed in the own lane gives FOLLOW: no turning, speed limited by the gap", "(centre ToF). At 150 mm (follow_gap_mm) or less the cap is 0 (stop); at 190 mm it is 179 (follow_v)", "All three ToFs sit at the front of the body; the left and right ones look 65° forward, not sideways"],
        "s2": ["Stopped with the gap unchanged for 2 s (parked_confirm_ms): classified as parked.", "Passing-side ToF (65° forward) ≥ 120 mm (side_clear_mm) or −3: OVERTAKE_OUT,", "otherwise FOLLOW_WAIT"],
        "s3": ["OVERTAKE_OUT: maximum steering towards the passing side for 500 ms (out_ms)", "speed cap 128 (overtake_v, about 50 % of the full 255)"],
        "s4": ["OVERTAKE_PASS: steering neutral, alongside. After the inner ToF reads < 150 mm (vehicle),", "≥ 150 mm or −3 for 700 ms (pass_clear_ms) and at least 800 ms elapsed: passed (at most 3000 ms)", "The inner ToF looks 65° forward, so it loses the vehicle once our front passes its front; 700 ms lets the rest pass"],
        "s5": ["OVERTAKE_IN: maximum steering the other way for 500 ms (in_ms) back to the lane", "then vehicle detections are ignored for 1.5 s (ignore_ms). Red light / intersection aborts"],
        "parked": "parked", "own": "own", "gap": "gap", "timer": "stopped, gap unchanged", "side": "passing-side ToF (65° fwd)",
        "inner": "inner ToF (65° fwd)", "schem": "Schematic (times are the defaults; distances and speeds are not to scale)", "ignore": "vehicle detections ignored",
        "tof3": "3 ToFs: centre straight ahead, left / right 65° forward", "lost": "lost (our front passed its front) → 700 ms timer",
    },
}

LANE_Y = 400        # own lane centre
PASS_Y = 310        # passing lane centre (90 px from the own lane: roughly the real lane offset relative to the car width,
                    # so that the 65-degree inner ToF loses the parked vehicle when our front passes its front)
PARK_X = 760


def road(d, L):
    note(d, L["schem"])
    d.rectangle([0, 160, W, 172], fill=WALL)        # wall on the passing side
    d.rectangle([0, 468, W, 480], fill=WALL)        # wall on the own side
    ym = (LANE_Y + PASS_Y) // 2
    for x in range(0, W, 60):
        d.line([x, ym, x + 30, ym], fill=(200, 205, 212), width=3)
    car(d, PARK_X, LANE_Y, 0, color=(110, 116, 126), label_text=L["parked"])


def ovt_pos(phase, u):
    """Position, heading and steering of the own car.  u in 0..1 within the phase."""
    if phase == "follow":
        x = lerp(140, PARK_X - 110 - 60, ease(u))
        return x, LANE_Y, 0.0, 0
    if phase == "wait":
        return PARK_X - 170, LANE_Y, 0.0, 0
    if phase == "out":
        x = lerp(PARK_X - 170, PARK_X - 90, u)
        y = lerp(LANE_Y, PASS_Y, ease(min(1.0, u * 1.4)))
        return x, y, -0.55 * math.sin(math.pi * u), -1
    if phase == "pass":
        return lerp(PARK_X - 90, PARK_X + 170, u), PASS_Y, 0.0, 0
    if phase == "in":
        x = lerp(PARK_X + 170, PARK_X + 300, u)
        y = lerp(PASS_Y, LANE_Y, ease(u))
        return x, y, 0.55 * math.sin(math.pi * u), 1
    return lerp(PARK_X + 300, 1180, u), LANE_Y, 0.0, 0


def ovt_scene(d, L, t, phase, dur):
    road(d, L)
    u = clamp(t / (dur - 0.8))
    x, y, ang, steer = ovt_pos(phase, u)
    p = car(d, x, y, ang, color=NAVY, steer=steer, label_text=L["own"], tof=True)
    parked_rect = (PARK_X - 55, LANE_Y - 28, PARK_X + 55, LANE_Y + 28)
    state = {"follow": "FOLLOW", "wait": "FOLLOW", "out": "OVERTAKE_OUT", "pass": "OVERTAKE_PASS", "in": "OVERTAKE_IN", "done": ""}[phase]
    if state:
        pill(d, (40, 112), state, bg=NAVY)
    if phase == "follow":
        label(d, (40, 200), L["tof3"], 22, BLUE, True)
    if phase in ("follow", "wait"):
        fx = p(TOF_C[0], 0)[0]
        gap_mm = int(lerp(600, 150, ease(u))) if phase == "follow" else 150
        d.line([fx, y, PARK_X - 55, y], fill=RED, width=3)
        label(d, ((fx + PARK_X - 55) / 2, y - 40), f"{L['gap']} {gap_mm if gap_mm <= 200 else '−3'}" + (" mm" if gap_mm <= 200 else ""), 22, RED, True, "md")
    if phase == "wait":
        prog = clamp(t / 4.0)
        bx, by = 300, 498   # between the lower wall (480) and the caption box (from 530 with three lines)
        d.rectangle([bx, by, bx + 360, by + 22], outline=GREY, width=2)
        d.rectangle([bx, by, bx + 360 * prog, by + 22], fill=ORANGE)
        label(d, (bx + 372, by + 11), f"{L['timer']} {2.0 * prog:.1f} / 2.0 s", 22, INK, False, "lm")
        # the passing side is the left (the passing lane is above): the left ToF looks 65 degrees forward-left
        sx, sy = p(*TOF_L)
        dx, dy = tof_dir(ang, "L")
        hx, hy, _ = ray_hit(sx, sy, dx, dy, [], 172)
        d.line([sx, sy, hx, hy], fill=GREEN, width=3)
        txt = f"{L['side']} ≥ 120 mm / −3"
        label(d, (min(hx + 12, W - 40 - d.textlength(txt, font=font(22, True))), 200), txt, 22, GREEN, True)
    if phase == "pass":
        # the inner side is the right (the parked vehicle is below): the right ToF looks 65 degrees forward-right
        sx, sy = p(*TOF_R)
        dx, dy = tof_dir(ang, "R")
        hx, hy, seen = ray_hit(sx, sy, dx, dy, [parked_rect], 468)
        d.line([sx, sy, hx, hy], fill=RED if seen else GREEN, width=3)
        txt = f"{L['inner']} {'< 150 mm' if seen else '≥ 150 mm / −3'}"
        lx = min(sx + 30, W - 40 - max(d.textlength(txt, font=font(22, True)), d.textlength(L["lost"], font=font(20))))
        label(d, (lx, 196), txt, 22, RED if seen else GREEN, True)
        if not seen and x > PARK_X - 60:
            label(d, (lx, 228), L["lost"], 20, GREEN, False)
    if phase == "done":
        pill(d, (40, 112), L["ignore"] + " 1.5 s", bg=GREY)


def video3(lang):
    L = T3[lang]
    n = 6
    return [
        (5.5, lambda t: title_scene(L["title"], L["s0"], t)),
        (7.0, lambda t: scene(L, 2, n, L["s1"], t, lambda d: ovt_scene(d, L, t, "follow", 7.0))),
        (8.0, lambda t: scene(L, 3, n, L["s2"], t, lambda d: ovt_scene(d, L, t, "wait", 8.0))),
        (6.0, lambda t: scene(L, 4, n, L["s3"], t, lambda d: ovt_scene(d, L, t, "out", 6.0))),
        (8.0, lambda t: scene(L, 5, n, L["s4"], t, lambda d: ovt_scene(d, L, t, "pass", 8.0))),
        (8.0, lambda t: scene(L, 6, n, L["s5"], t, lambda d: (ovt_scene(d, L, t, "in", 4.0) if t < 3.6 else ovt_scene(d, L, t - 3.6, "done", 4.4)))),
    ]


# ---------------------------------------------------------------------------
# scene helpers and rendering
# ---------------------------------------------------------------------------
def title_scene(title, lines, t):
    im = Image.new("RGB", (W, H), NAVY)
    d = ImageDraw.Draw(im)
    a = clamp(t / 0.6)
    d.text((80, 230), title, font=font(48, True), fill=mix(NAVY, (255, 255, 255), a))
    for i, ln in enumerate(lines):
        d.text((80, 330 + i * 50), ln, font=font(28), fill=mix(NAVY, (205, 215, 235), clamp((t - 0.4) / 0.6)))
    d.text((80, 620), "github.com/RumiCar-group/RumiCar-Zero2Cam", font=font(22), fill=mix(NAVY, (150, 170, 210), a))
    return im


def scene(L, step, total, cap, t, draw):
    im, d = base(L["title"], step, total, None)
    draw(d)
    caption(d, cap, t)
    return im


VIDEOS = {"01-tof-readings": video1, "02-auto-start": video2, "03-overtaking": video3}


def render(out_dir, name, lang):
    scenes = VIDEOS[name](lang)
    tmp = os.path.join(out_dir, f"_{name}_{lang}")
    os.makedirs(tmp, exist_ok=True)
    parts = []
    for i, (dur, fn) in enumerate(scenes):
        part = os.path.join(tmp, f"s{i:02d}.mp4")
        part_tmp = os.path.join(tmp, f"s{i:02d}.part.mp4")
        nfr = int(round(dur * FPS))
        p = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                              "-r", str(FPS), "-i", "-", "-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
                              "-pix_fmt", "yuv420p", "-r", str(FPS), part_tmp], stdin=subprocess.PIPE)
        for k in range(nfr):
            p.stdin.write(fn(k / FPS).tobytes())
        p.stdin.close()
        if p.wait() != 0:
            raise SystemExit(f"ffmpeg failed for {name} {lang} scene {i}")
        os.replace(part_tmp, part)
        parts.append(part)
    lst = os.path.join(tmp, "list.txt")
    with open(lst, "w") as f:
        for part in parts:
            f.write(f"file '{os.path.abspath(part)}'\n")
    out = os.path.join(out_dir, f"{name}-{lang}.mp4")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", lst, "-c", "copy",
                    "-movflags", "+faststart", out + ".part.mp4"], check=True)
    os.replace(out + ".part.mp4", out)
    shutil.rmtree(tmp)
    return out


def still(out_dir, name, lang, scene_idx, t):
    dur, fn = VIDEOS[name](lang)[scene_idx]
    path = os.path.join(out_dir, f"still-{name}-{lang}-{scene_idx}-{t}.png")
    fn(float(t)).save(path)
    return path


if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    args = sys.argv[2:]
    langs = ["ja", "en"]
    if "--lang" in args:
        i = args.index("--lang")
        langs = [args[i + 1]]
        del args[i:i + 2]
    if "--still" in args:
        i = args.index("--still")
        name, sc, t = args[i + 1], int(args[i + 2]), args[i + 3]
        for lg in langs:
            print(still(out, name, lg, sc, t))
        sys.exit(0)
    names = args or list(VIDEOS)
    for nm in names:
        for lg in langs:
            print(render(out, nm, lg))
