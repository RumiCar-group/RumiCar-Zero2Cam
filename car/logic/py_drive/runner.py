#!/usr/bin/env python3
"""Runs the user's driving program (drive.py of the active pack) for py_drive.

Started by the py_drive plug-in as `python3 -I -B -u runner.py <pack dir>`;
it talks to the plug-in over its standard input/output, one line per frame
(docs 3.12).  What drive.py prints goes to the core's log.

drive.py (written by the user) uses the RumiCar library API, the same as the
Arduino library RumiCar-lib and RasPi/RumiCar.py of RumiCar-group/RumiCar:

    from RumiCar import *

    def setup():         # optional: once, when the program starts
        RC_setup()
    def loop():          # every frame (20 times a second)
        d = RC_read(CENTER)        # 0..200 mm, -1 timeout, -3 nothing within 20 cm
        RC_steer(LEFT)             # LEFT / CENTER / RIGHT
        RC_drive(FORWARD, 150)     # FREE / REVERSE / FORWARD / BRAKE, 0..255

The RumiCar module is provided by this runner (the car's hardware is owned by
the core): RC_read returns the frame's sensor values, RC_steer / RC_drive set
the command for the frame, RC_setup does nothing.  The values set with
RC_steer / RC_drive stay until they are changed, as in an Arduino sketch.
Standard library only.
"""
import importlib.util
import json
import os
import sys
import time
import traceback
import types

LEFT, CENTER, RIGHT, REAR = 0, 1, 2, 3
FREE, REVERSE, FORWARD, BRAKE = 0, 1, 2, 3
PWM_MAX = 255

_warned = set()          # messages already written to the log (one line each, not every frame)


def _warn(key, message):
    if key not in _warned:
        _warned.add(key)
        print("drive.py: " + message, file=sys.stderr, flush=True)


class _Car:
    """The frame's sensor values and the command drive.py builds with RC_*."""

    def __init__(self, params):
        self.params = params
        self.tof = [-1, -1, -1]        # LEFT, CENTER, RIGHT (RC_read values)
        self.steer = CENTER
        self.drive_dir = FREE
        self.drive_pwm = 0
        self.state = "-"


_car = None


def RC_setup():
    """RumiCar のセンサとモータの初期化 (Zero2Cam では本体が行う: 何もしない)"""
    return None


def RC_steer(direc):
    """操舵の関数: LEFT / CENTER / RIGHT.  Returns 1, or 0 for a bad argument."""
    if direc is True or direc is False or direc not in (LEFT, CENTER, RIGHT):
        _warn(("steer", repr(direc)), "RC_steer(%r): not LEFT / CENTER / RIGHT; ignored" % (direc,))
        return 0
    _car.steer = int(direc)
    return 1


def RC_drive(direc, ipwm):
    """走行の関数: FREE / REVERSE / FORWARD / BRAKE, ipwm 0..255.  Returns 1, or 0 for a bad direction."""
    if direc is True or direc is False or direc not in (FREE, REVERSE, FORWARD, BRAKE):
        _warn(("drive", repr(direc)), "RC_drive(%r, ...): not FREE / REVERSE / FORWARD / BRAKE; ignored" % (direc,))
        return 0
    try:
        ipwm = int(ipwm)
    except (TypeError, ValueError):
        _warn(("pwm", repr(ipwm)), "RC_drive(..., %r): the value must be a number 0..255; 0 used" % (ipwm,))
        ipwm = 0
    if ipwm < 0:
        ipwm = 0
    if ipwm > PWM_MAX:
        ipwm = PWM_MAX
    _car.drive_dir = int(direc)
    _car.drive_pwm = ipwm
    return 1


def RC_read(direc):
    """測距の関数: LEFT / CENTER / RIGHT / REAR.
    0..200 = mm, -1 = タイムアウト (REAR: この車には無い), -2 = 引数エラー, -3 = 範囲外 (20 cm 以内に何もない)"""
    if direc is True or direc is False:
        return -2
    if direc in (LEFT, CENTER, RIGHT):
        return _car.tof[direc]
    if direc == REAR:
        return -1
    return -2


def RC_params():
    """params.json of the pack as a dict ({} when there is none)."""
    return _car.params


def RC_state(name):
    """Name of what the program is doing now (letters, digits and _, up to 24),
    shown as 判断状態 on the PC.  Returns 1, or 0 for an invalid name."""
    name = str(name).strip().upper().replace(" ", "_")
    if not name or len(name) > 24 or not name.isascii() or not all(c.isalnum() or c == "_" for c in name):
        _warn(("state", name), "RC_state(%r): use letters, digits and _ (up to 24); ignored" % (name,))
        return 0
    _car.state = name
    return 1


def _make_module():
    """The `RumiCar` module drive.py imports (`from RumiCar import *`)."""
    m = types.ModuleType("RumiCar")
    for name in ("LEFT", "CENTER", "RIGHT", "REAR", "FREE", "REVERSE", "FORWARD", "BRAKE",
                 "RC_setup", "RC_steer", "RC_drive", "RC_read", "RC_params", "RC_state"):
        setattr(m, name, globals()[name])
    m.__all__ = ["LEFT", "CENTER", "RIGHT", "REAR", "FREE", "REVERSE", "FORWARD", "BRAKE",
                 "RC_setup", "RC_steer", "RC_drive", "RC_read", "RC_params", "RC_state"]
    return m


def _where(exc):
    """The drive.py line an exception came from, for the one-line message."""
    tb = traceback.extract_tb(exc.__traceback__)
    for fr in reversed(tb):
        if os.path.basename(fr.filename) == "drive.py":
            return f"drive.py の {fr.lineno} 行目"
    return "drive.py"


def main():
    global _car
    if len(sys.argv) != 2:
        print("usage: runner.py <pack dir>", file=sys.stderr)
        return 2
    pack_dir = sys.argv[1]
    # The protocol uses the original standard output; everything drive.py prints goes to stderr (the log).
    proto_out = os.fdopen(os.dup(1), "w", buffering=1, encoding="utf-8", newline="\n")
    os.dup2(2, 1)
    sys.stdout = sys.stderr
    proto_in = sys.stdin.buffer

    def reply(line):
        proto_out.write(line + "\n")
        proto_out.flush()

    # --- load drive.py and params.json ----------------------------------------------------------------
    try:
        params = {}
        params_path = os.path.join(pack_dir, "params.json")
        if os.path.isfile(params_path):
            with open(params_path, encoding="utf-8") as f:
                params = json.load(f)
            if not isinstance(params, dict):
                raise ValueError("params.json は JSON オブジェクト（{ ... }、名前と値の組）で記述してください")
        _car = _Car(params)
        sys.modules["RumiCar"] = _make_module()
        sys.path.insert(0, pack_dir)          # drive.py may import its own modules from the pack
        spec = importlib.util.spec_from_file_location("drive", os.path.join(pack_dir, "drive.py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        loop = getattr(module, "loop", None)
        if not callable(loop):
            raise ValueError("drive.py に def loop(): がありません（Zero2Cam では 1 フレームごとに loop() が呼ばれます）")
        setup = getattr(module, "setup", None)
        if callable(setup):
            setup()
    except SyntaxError as e:
        traceback.print_exc()
        reply(f"ERROR drive.py の {e.lineno} 行目に構文エラーがあります: {e.msg}")
        return 1
    except json.JSONDecodeError as e:
        reply(f"ERROR params.json の {e.lineno} 行目が JSON として解析できません: {e.msg}")
        return 1
    except Exception as e:   # noqa: BLE001 — any error while loading is reported, not raised
        traceback.print_exc()
        reply(f"ERROR {_where(e)}: {type(e).__name__}: {e}".replace("\n", " "))
        return 1
    reply("READY")
    print(f"[drive.py] started ({pack_dir})", file=sys.stderr, flush=True)

    # --- one line per frame -------------------------------------------------------------------------------
    last_error = ""
    last_error_time = -1e9
    while True:
        raw = proto_in.readline()
        if not raw:
            return 0                          # the plug-in closed the connection (deactivated, core stopped)
        parts = raw.decode("ascii", "replace").split()
        if not parts:
            continue
        if parts[0] == "P":
            reply("PONG")
            continue
        if parts[0] != "F" or len(parts) != 6:
            continue
        seq = parts[1]
        try:
            _car.tof = [int(p) for p in parts[3:6]]
            loop()
            reply(f"C {seq} {_car.steer} {_car.drive_dir} {_car.drive_pwm} {_car.state}")
        except Exception as e:   # noqa: BLE001 — the frame brakes; the program keeps running
            message = f"{_where(e)}: {type(e).__name__}: {e}".replace("\n", " ")
            # The same error every frame would flood the log: print it again only after 5 s.
            if message != last_error or time.monotonic() - last_error_time > 5.0:
                traceback.print_exc()
                last_error, last_error_time = message, time.monotonic()
            reply(f"X {seq} {message}")


if __name__ == "__main__":
    sys.exit(main())
