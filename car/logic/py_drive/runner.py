#!/usr/bin/env python3
"""Runs the user's driving program (drive.py of the active pack) for py_drive.

Started by the py_drive plug-in as `python3 -I -B -u runner.py <pack dir>`;
it talks to the plug-in over its standard input/output, one line per frame
(docs 3.12).  What drive.py prints goes to the core's log.

drive.py (written by the user) defines

    def setup(car):      # optional: once, when the program starts
        ...
    def loop(car):       # every frame (20 times a second)
        ...

and uses the `car` object below.  The values set with steer() / drive()
stay until they are changed, as in an Arduino sketch.
Standard library only.
"""
import importlib.util
import json
import os
import sys
import time
import traceback

FAR = 8190            # a distance sensor that sees nothing within its range
LEFT, CENTER, RIGHT = -1, 0, 1
_STEER_NAMES = {"left": LEFT, "center": CENTER, "centre": CENTER, "right": RIGHT}


class DriveError(Exception):
    """A wrong value given to the car object (shown with the line in drive.py)."""


class Car:
    """What drive.py sees.  Distances in millimetres; drive in per mille (1000 = 100 %)."""

    LEFT = LEFT
    CENTER = CENTER
    RIGHT = RIGHT
    FAR = FAR

    def __init__(self, params):
        self.params = params        # params.json of the pack (the program's own settings)
        self.left = FAR             # distance sensors: mm, FAR = nothing near, None = no reading
        self.center = FAR
        self.right = FAR
        self.time = 0.0             # seconds since the program started
        self.dt = 0.0               # seconds since the previous frame
        self.frame = 0              # frame counter
        self._steer = CENTER
        self._drive = 0
        self._brake = False
        self._state = "-"

    @property
    def tof(self):
        """(left, center, right) in mm."""
        return (self.left, self.center, self.right)

    def steer(self, direction):
        """Steering: car.LEFT / car.CENTER / car.RIGHT (or -1 / 0 / 1, or "left" / "center" / "right")."""
        if isinstance(direction, str):
            if direction.lower() not in _STEER_NAMES:
                raise DriveError(f"steer() に {direction!r} は使えません（\"left\" / \"center\" / \"right\"）")
            direction = _STEER_NAMES[direction.lower()]
        if isinstance(direction, bool) or direction not in (LEFT, CENTER, RIGHT):
            raise DriveError(f"steer() に {direction!r} は使えません（car.LEFT / car.CENTER / car.RIGHT）")
        self._steer = int(direction)

    def drive(self, permille):
        """Drive motor: -1000 (full reverse) .. 0 (stop) .. 1000 (full forward)."""
        if isinstance(permille, bool) or not isinstance(permille, (int, float)):
            raise DriveError(f"drive() には数を渡します（{permille!r} は使えません）")
        if not -1000 <= permille <= 1000:
            raise DriveError(f"drive() は -1000〜1000 です（{permille} は範囲外）")
        self._drive = int(round(permille))
        self._brake = False

    def stop(self):
        """Drive 0 (the motor coasts)."""
        self._drive = 0
        self._brake = False

    def brake(self):
        """Brake the drive motor (stays until drive() is called)."""
        self._drive = 0
        self._brake = True

    def state(self, name):
        """A name for what the program is doing now, shown as 判断状態 on the PC (letters, digits, _)."""
        name = str(name).strip().upper().replace(" ", "_")
        if not name or len(name) > 24 or not all(c.isalnum() or c == "_" for c in name) or not name.isascii():
            raise DriveError(f"state() の名前は英数字と _ で 24 文字までです（{name!r}）")
        self._state = name


def _where(exc):
    """The drive.py line an exception came from, for the one-line message."""
    tb = traceback.extract_tb(exc.__traceback__)
    for fr in reversed(tb):
        if os.path.basename(fr.filename) == "drive.py":
            return f"drive.py の {fr.lineno} 行目"
    return "drive.py"


def _distance(text):
    v = int(text)
    if v < 0:
        return None          # the sensor gave no reading
    return v


def main():
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
                raise DriveError("params.json は { ... } の形（名前と値の組）にします")
        sys.path.insert(0, pack_dir)          # drive.py may import its own modules from the pack
        spec = importlib.util.spec_from_file_location("drive", os.path.join(pack_dir, "drive.py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        loop = getattr(module, "loop", None)
        if not callable(loop):
            raise DriveError("drive.py に def loop(car): がありません")
        setup = getattr(module, "setup", None)
        car = Car(params)
        if callable(setup):
            setup(car)
    except SyntaxError as e:
        traceback.print_exc()
        reply(f"ERROR drive.py の {e.lineno} 行目に書き方の誤りがあります: {e.msg}")
        return 1
    except json.JSONDecodeError as e:
        reply(f"ERROR params.json の {e.lineno} 行目が JSON として読めません: {e.msg}")
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
    prev_t = None
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
            t_s = float(parts[2]) / 1000.0
            car.left, car.center, car.right = (_distance(p) for p in parts[3:6])
            car.dt = 0.0 if prev_t is None else t_s - prev_t
            prev_t = t_s
            car.time = t_s
            car.frame += 1
            loop(car)
            reply(f"C {seq} {car._steer} {car._drive} {1 if car._brake else 0} {car._state}")
        except Exception as e:   # noqa: BLE001 — the frame brakes; the program keeps running
            message = f"{_where(e)}: {type(e).__name__}: {e}".replace("\n", " ")
            # The same error every frame would flood the log: print it again only after 5 s.
            if message != last_error or time.monotonic() - last_error_time > 5.0:
                traceback.print_exc()
                last_error, last_error_time = message, time.monotonic()
            reply(f"X {seq} {message}")


if __name__ == "__main__":
    sys.exit(main())
