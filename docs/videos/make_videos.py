"""Explainer videos for RumiCar Zero2Cam (Japanese and English), 1280x720, 30 fps, silent.

Videos (each in ja and en, written as <out dir>/<name>-<lang>.mp4):
  01-tof-readings  what the ToF readings mean (number / open / --) and the "open" of tof_drive
  02-auto-start    start ramp and auto limit when auto mode is entered (no sudden start)
  03-overtaking    following and overtaking a parked vehicle (cam_drive vehicle behaviour)

Every number shown is a default in the sources: vl53l0x.cpp, main.cpp, tof_logic.cpp and
car/packs/base/params.json, auto_output.hpp and car/deploy/core.json, cam_logic.cpp and the
vehicle defaults of train_pack.py.  Change the texts here when the sources change.

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


def car(d, cx, cy, ang=0.0, color=NAVY, length=110, width=56, steer=0, label_text=None):
    """Top view of a car centred at (cx, cy), heading `ang` radians (0 = +x)."""
    ca, sa = math.cos(ang), math.sin(ang)

    def p(x, y):
        return (cx + x * ca - y * sa, cy + x * sa + y * ca)

    hl, hw = length / 2, width / 2
    body = [p(-hl, -hw), p(hl - 14, -hw), p(hl, -hw + 14), p(hl, hw - 14), p(hl - 14, hw), p(-hl, hw)]
    d.polygon(body, fill=color)
    # wheels (front ones turned by `steer`: -1 left, 0, +1 right)
    for wx, wy, front in ((-hl + 18, -hw - 4, False), (-hl + 18, hw + 4, False), (hl - 22, -hw - 4, True), (hl - 22, hw + 4, True)):
        a2 = ang + (steer * -0.45 if front else 0.0)
        c2, s2 = math.cos(a2), math.sin(a2)
        mx, my = p(wx, wy)
        pts = [(mx + u * c2 - v * s2, my + u * s2 + v * c2) for u, v in ((-11, -5), (11, -5), (11, 5), (-11, 5))]
        d.polygon(pts, fill=(40, 40, 40))
    if label_text:
        d.text(p(0, 0), label_text, font=font(18, True), fill=(255, 255, 255), anchor="mm")
    return p


# ---------------------------------------------------------------------------
# video 1: ToF readings
# ---------------------------------------------------------------------------
T1 = {
    "ja": {
        "title": "ToF センサーの読み取り値と表示",
        "s0": ["RumiCar Zero2Cam：VL53L0X（ToF 距離センサー）×3 の値の意味", "数値・open・-- の違いと、走行ロジック tof_drive での扱い"],
        "s1": ["レーザーが対象物で反射して戻ると、往復時間から距離を算出します", "表示：測距値 [mm]（例 180）"],
        "s2": ["対象物を検出できなかった場合、センサーは 8190 を返します。多くは", "レーザーが対象物に当たって反射光が戻らなかったとき（測距範囲外など）", "表示：open　drive.py：car.FAR（8190）"],
        "s3": ["反射光が戻っても受信した信号が弱く、規定値に満たない場合も 8190", "（暗い面・浅い角度の面など）。表示：open"],
        "s4": ["既定 100 ms（i2c.tof_read_timeout_ms）以内に測定結果が得られない、", "または I2C 通信エラー → −1（読み取り失敗）。表示：--　drive.py：None", "tof_drive は前の値を 300 ms 保持し、超過すると SENSOR_FAULT で停止"],
        "s5": ["tof_drive は conf_max_mm（既定 250 mm）以上の読みと 8190 を「開放」", "（判断対象の距離内に障害物なし）として扱います", "open（センサー）と開放（ロジック）は別の概念です"],
        "mm": "測距値", "nores": "反射光なし", "weak": "受信信号 < 規定値", "timeout": "タイムアウト／I2C エラー",
        "sensor": "VL53L0X", "obj": "対象物", "dark": "暗い面・浅い角度", "range": "測距範囲",
        "open_zone": "開放", "dist_zone": "距離として使用", "axis": "読み取り値 [mm]",
        "schem": "模式図（縮尺は実際と異なる）", "hold": "前の値を保持 300 ms", "fault": "SENSOR_FAULT（停止）",
    },
    "en": {
        "title": "ToF sensor readings and their display",
        "s0": ["RumiCar Zero2Cam: what the values of the three VL53L0X (ToF distance sensors) mean", "number, open and --, and how the driving logic tof_drive uses them"],
        "s1": ["When the laser light is reflected by an object and returns,", "the distance is computed from the round-trip time. Display: range in mm (e.g. 180)"],
        "s2": ["When no target is detected, the sensor returns 8190. Typically the laser", "light was not reflected back by an object (e.g. beyond the measuring range).", "Display: open    drive.py: car.FAR (8190)"],
        "s3": ["Also 8190 when light returns but the received signal is too weak,", "below the configured limit (dark or grazing surfaces). Display: open"],
        "s4": ["No measurement within 100 ms (i2c.tof_read_timeout_ms, default), or an I2C", "error → −1 (read failure). Display: --    drive.py: None", "tof_drive holds the previous value for 300 ms, then stops in SENSOR_FAULT"],
        "s5": ["tof_drive treats readings at or above conf_max_mm (default 250 mm) and 8190", "as \"open\" (no obstacle within the judged distance).", "\"open\" on the display (sensor) and \"open\" in the logic are different concepts"],
        "mm": "range", "nores": "no reflected light", "weak": "received signal < limit", "timeout": "timeout / I2C error",
        "sensor": "VL53L0X", "obj": "object", "dark": "dark / grazing surface", "range": "measuring range",
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
            pill(d, (420, 440), f"{L['nores']} → 8190 → open", bg=ORANGE)
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
            pill(d, (300, 470), f"{L['weak']} → 8190 → open", bg=ORANGE)
    elif mode == "fail":
        d.line([sx - 110, sy - 40, sx, sy + 40], fill=RED, width=6)
        d.line([sx - 110, sy + 40, sx, sy - 40], fill=RED, width=6)
        pill(d, (300, 300), f"{L['timeout']} → −1 → --", bg=RED)
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
    mm_max = 400

    def X(mm):
        return lerp(x0, x1, mm / mm_max)

    d.rectangle([X(250), y - 70, x1, y + 18], fill=(222, 240, 226))
    d.rectangle([x0, y - 70, X(250), y + 18], fill=(232, 238, 247))
    label(d, ((X(250) + x1) / 2, y - 60), L["open_zone"], 26, GREEN, True, "ma")
    label(d, ((x0 + X(250)) / 2, y - 60), L["dist_zone"], 26, NAVY, True, "ma")
    d.line([x0, y + 18, x1, y + 18], fill=INK, width=3)
    for mm in range(0, 401, 50):
        d.line([X(mm), y + 18, X(mm), y + 30], fill=INK, width=2)
        label(d, (X(mm), y + 34), str(mm), 20, INK, False, "ma")
    label(d, ((X(250) + x1) / 2, y - 26), "+ 8190", 22, GREEN, True, "ma")
    d.line([X(250), y - 80, X(250), y + 30], fill=RED, width=3)
    label(d, (X(250), y - 84), "conf_max_mm = 250", 22, RED, True, "md")
    label(d, ((x0 + x1) / 2, y + 66), L["axis"], 22, GREY, False, "ma")
    v = 120 + 260 * (0.5 - 0.5 * math.cos(t * 1.3))
    d.polygon([(X(v), y + 12), (X(v) - 12, y - 10), (X(v) + 12, y - 10)], fill=ORANGE)
    pill(d, (X(v) - 70, y - 160), f"{int(v)} mm", bg=GREEN if v >= 250 else NAVY, size=24)


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
        "s0": ["RumiCar Zero2Cam：自動モードへの切り替え時の駆動の制限", "auto_accel_permille_per_s と「自動の最大出力」"],
        "s1": ["ロジックはどのモードでも動作しており、前方が開放なら最大出力を指令しています", "そのまま出力すると、自動への切り替え直後に急発進します"],
        "s2": ["本体は駆動の許容値を 0 から 1125 ‰/s で増加させ、出力を制限します", "許容値 = min(増加分, 自動の最大出力, max_drive_permille)", "最大出力 100% では約 0.89 秒（1000 ÷ 1125）で上限に達します"],
        "s3": ["「自動の最大出力」を 50% にすると、約 0.44 秒で 50% に達し、以後 50% で頭打ち", "許容値が上限に達した後は、ロジック自身の減速・ブレーキ・後退には影響しません", "手動・収録のみ・非常停止・step() の失敗のフレームで許容値は 0 に戻ります"],
        "cmd": "ロジックの指令", "ramp": "0 からの増加分（1125 ‰/s）", "lim": "自動の最大出力", "out": "モーターへの出力",
        "naive": "制限なしの出力", "x": "自動に切り替えてからの時間 [s]", "y": "駆動 [%]",
    },
    "en": {
        "title": "Start of auto mode (preventing a sudden start)",
        "s0": ["RumiCar Zero2Cam: limiting the drive when switching to auto mode", "auto_accel_permille_per_s and the auto limit (自動の最大出力)"],
        "s1": ["The logic runs in every mode; with the way ahead open it commands maximum output.", "Output as is, the car would start suddenly right after switching to auto"],
        "s2": ["The core raises the permitted drive from 0 at 1125 ‰/s and limits the output:", "permitted = min(increase, auto limit, max_drive_permille)", "At an auto limit of 100 % the limit is reached in about 0.89 s (1000 ÷ 1125)"],
        "s3": ["With the auto limit at 50 %, 50 % is reached in about 0.44 s and then held", "Once the limit is reached, the logic's own deceleration, braking and reversing are unaffected", "In manual, record-only, e-stop and failed-step frames the permitted value returns to 0"],
        "cmd": "logic command", "ramp": "increase from 0 (1125 ‰/s)", "lim": "auto limit", "out": "output to the motor",
        "naive": "output without limiting", "x": "time since switching to auto [s]", "y": "drive [%]",
    },
}


def graph(d, L, t, limit, show_naive, show_ramp):
    gx0, gx1, gy0, gy1 = 180, 1100, 520, 140
    tmax = 1.4

    def X(s):
        return lerp(gx0, gx1, s / tmax)

    def Y(pct):
        return lerp(gy0, gy1, pct / 100)

    d.line([gx0, gy0, gx1, gy0], fill=INK, width=2)
    d.line([gx0, gy0, gx0, gy1 - 10], fill=INK, width=2)
    for pct in (0, 25, 50, 75, 100):
        d.line([gx0 - 8, Y(pct), gx0, Y(pct)], fill=INK, width=2)
        label(d, (gx0 - 14, Y(pct)), str(pct), 20, INK, False, "rm")
        d.line([gx0, Y(pct), gx1, Y(pct)], fill=(228, 232, 238), width=1)
    for s10 in range(0, 15, 2):
        s = s10 / 10
        d.line([X(s), gy0, X(s), gy0 + 8], fill=INK, width=2)
        label(d, (X(s), gy0 + 12), f"{s:.1f}", 20, INK, False, "ma")
    label(d, ((gx0 + gx1) / 2, gy0 + 40), L["x"], 22, GREY, False, "ma")
    label(d, (gx0 - 60, gy1 - 44), L["y"], 22, GREY)
    cur = clamp(t / 4.0) * tmax          # the cursor sweeps the time axis in 4 s
    # logic command: 100 % all the time
    for s in [i * 0.02 for i in range(int(tmax / 0.02))][::2]:
        d.line([X(s), Y(100), X(s + 0.02), Y(100)], fill=GREY, width=3)
    label(d, (X(1.05), Y(100) - 8), L["cmd"], 20, GREY, False, "ld")
    if show_naive:
        d.line([X(0), Y(100), X(cur), Y(100)], fill=RED, width=6)
        label(d, (X(0.05), Y(100) + 10), L["naive"], 22, RED, True)
    if show_ramp:
        if limit < 100:
            d.line([X(0), Y(limit), X(tmax), Y(limit)], fill=ORANGE, width=3)
            label(d, (X(1.05), Y(limit) - 8), f"{L['lim']} {limit}%", 20, ORANGE, True, "ld")
        rend = min(tmax, 1000 / 1125)
        d.line([X(0), Y(0), X(rend), Y(100)], fill=(150, 170, 210), width=2)
        label(d, (X(0.30), Y(52)), L["ramp"], 20, BLUE, False, "rd")
        # output = min(ramp, limit); sampled every 50 ms as in the control loop
        pts = []
        s = 0.0
        while s <= cur + 1e-9:
            pts.append((X(s), Y(min(112.5 * s, limit))))
            s += 0.05
        if len(pts) > 1:
            d.line(pts, fill=GREEN, width=6)
        reach = limit / 112.5
        d.line([X(reach), Y(limit), X(reach), gy0], fill=(170, 200, 170), width=2)
        label(d, (X(reach), gy0 - 6), f"{reach:.2f} s", 20, GREEN, True, "md")
        lx = min(X(min(cur, tmax)) + 10, gx1 - d.textlength(L["out"], font=font(22, True)))
        label(d, (lx, Y(min(112.5 * cur, limit)) + 10), L["out"], 22, GREEN, True)
    d.line([X(cur), gy0, X(cur), gy1], fill=(200, 200, 200), width=1)


def video2(lang):
    L = T2[lang]
    n = 4
    return [
        (5.0, lambda t: title_scene(L["title"], L["s0"], t)),
        (6.0, lambda t: scene(L, 2, n, L["s1"], t, lambda d: graph(d, L, t, 100, True, False))),
        (8.0, lambda t: scene(L, 3, n, L["s2"], t, lambda d: graph(d, L, t, 100, False, True))),
        (9.0, lambda t: scene(L, 4, n, L["s3"], t, lambda d: graph(d, L, t, 50, False, True))),
    ]


# ---------------------------------------------------------------------------
# video 3: overtaking a parked vehicle (cam_drive vehicle behaviour)
# ---------------------------------------------------------------------------
T3 = {
    "ja": {
        "title": "駐車車両の追い越し（cam_drive の車両の挙動）",
        "s0": ["RumiCar Zero2Cam：前方車両への追従と駐車車両の追い越し", "状態 FOLLOW → OVERTAKE_OUT → OVERTAKE_PASS → OVERTAKE_IN（既定値で説明）"],
        "s1": ["自車線に車両を確定すると FOLLOW：旋回せずに追従し、車間（ToF 中央）で速度を制限", "車間 150 mm（follow_gap_mm）以下で速度の上限 0（停止）"],
        "s2": ["停止したまま車間が広がらない状態が 2 秒（parked_confirm_ms）続くと駐車車両と判定", "追い越し側の ToF が 120 mm（side_clear_mm）以上なら OVERTAKE_OUT、未満なら FOLLOW_WAIT"],
        "s3": ["OVERTAKE_OUT：追い越し側へ最大舵角で 500 ms（out_ms）進路変更", "速度の上限 50%（overtake_v）"],
        "s4": ["OVERTAKE_PASS：操舵を中立にして並走。内側の ToF が 150 mm 未満（車両を検出）の後、", "150 mm 以上が 300 ms 続き、かつ 800 ms 以上経過で通過完了（最長 3000 ms）"],
        "s5": ["OVERTAKE_IN：反対側へ最大舵角で 500 ms（in_ms）元の車線へ復帰", "その後 1.5 秒（ignore_ms）は車両の検出を無視。赤信号・交差点で中止"],
        "parked": "駐車車両", "own": "自車", "gap": "車間", "timer": "停止・車間不変", "side": "追い越し側 ToF",
        "inner": "内側 ToF", "schem": "模式図（時間は既定値、距離・速度は実際と異なる）", "ignore": "車両の検出を無視",
    },
    "en": {
        "title": "Overtaking a parked vehicle (cam_drive vehicle behaviour)",
        "s0": ["RumiCar Zero2Cam: following a vehicle ahead and overtaking a parked one", "States FOLLOW → OVERTAKE_OUT → OVERTAKE_PASS → OVERTAKE_IN (with the defaults)"],
        "s1": ["A vehicle confirmed in the own lane gives FOLLOW: no turning, speed limited by the gap", "(centre ToF). At 150 mm (follow_gap_mm) or less the speed cap is 0 (stop)"],
        "s2": ["Stopped with the gap unchanged for 2 s (parked_confirm_ms): classified as parked.", "Passing-side ToF ≥ 120 mm (side_clear_mm): OVERTAKE_OUT, otherwise FOLLOW_WAIT"],
        "s3": ["OVERTAKE_OUT: maximum steering towards the passing side for 500 ms (out_ms)", "speed cap 50 % (overtake_v)"],
        "s4": ["OVERTAKE_PASS: steering neutral, alongside. After the inner ToF reads < 150 mm (vehicle),", "≥ 150 mm for 300 ms and at least 800 ms elapsed: passed (at most 3000 ms)"],
        "s5": ["OVERTAKE_IN: maximum steering the other way for 500 ms (in_ms) back to the lane", "then vehicle detections are ignored for 1.5 s (ignore_ms). Red light / intersection aborts"],
        "parked": "parked", "own": "own", "gap": "gap", "timer": "stopped, gap unchanged", "side": "passing-side ToF",
        "inner": "inner ToF", "schem": "Schematic (times are the defaults; distances and speeds are not to scale)", "ignore": "vehicle detections ignored",
    },
}

LANE_Y = 400        # own lane centre
PASS_Y = 250        # passing lane centre
PARK_X = 760


def road(d, L):
    note(d, L["schem"])
    d.rectangle([0, 160, W, 172], fill=WALL)        # wall on the passing side
    d.rectangle([0, 468, W, 480], fill=WALL)        # wall on the own side
    for x in range(0, W, 60):
        d.line([x, 325, x + 30, 325], fill=(200, 205, 212), width=3)
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
    p = car(d, x, y, ang, color=NAVY, steer=steer, label_text=L["own"])
    state = {"follow": "FOLLOW", "wait": "FOLLOW", "out": "OVERTAKE_OUT", "pass": "OVERTAKE_PASS", "in": "OVERTAKE_IN", "done": ""}[phase]
    if state:
        pill(d, (40, 112), state, bg=NAVY)
    if phase in ("follow", "wait"):
        fx = p(55, 0)[0]
        gap_mm = int(lerp(600, 150, ease(u))) if phase == "follow" else 150
        d.line([fx, y, PARK_X - 55, y], fill=RED, width=3)
        label(d, ((fx + PARK_X - 55) / 2, y - 40), f"{L['gap']} {gap_mm} mm", 22, RED, True, "md")
    if phase == "wait":
        prog = clamp(t / 4.0)
        bx, by = 420, 540
        d.rectangle([bx, by, bx + 400, by + 22], outline=GREY, width=2)
        d.rectangle([bx, by, bx + 400 * prog, by + 22], fill=ORANGE)
        label(d, (bx, by - 8), f"{L['timer']} {2.0 * prog:.1f} / 2.0 s", 22, INK, False, "ld")
        sx, sy = p(10, -28)
        d.line([sx, sy, sx, 172], fill=GREEN, width=3)
        label(d, (sx + 10, 200), f"{L['side']} ≥ 120 mm", 22, GREEN, True)
    if phase == "pass":
        sx, sy = p(10, 28)
        alongside = abs(x - PARK_X) < 110
        d.line([sx, sy, sx, LANE_Y - 28 if alongside else 468], fill=RED if alongside else GREEN, width=3)
        label(d, (sx + 10, 300), f"{L['inner']} {'< 150 mm' if alongside else '≥ 150 mm'}", 22,
              RED if alongside else GREEN, True)
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
