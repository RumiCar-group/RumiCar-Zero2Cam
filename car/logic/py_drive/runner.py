#!/usr/bin/env python3
"""Runs the user's driving program (drive.py of the active pack) for py_drive.

Started by the py_drive plug-in as `python3 -I -B -u runner.py <pack dir>`;
it talks to the plug-in over its standard input/output, one line per frame
(docs 3.12).  What drive.py prints goes to the core's log.

drive.py (written by the user) uses the RumiCar library: RasPi/RumiCar.py of
RumiCar-group/RumiCar, the same API as the Arduino library RumiCar-lib.

    from RumiCar import *

    def setup():         # optional: once, when the program starts
        RC_setup()
    def loop():          # every frame (20 times a second)
        d = RC_read(CENTER)        # 0..200 mm, -1 timeout, -3 nothing within 20 cm
        RC_steer(LEFT, 128)        # LEFT / CENTER / RIGHT, strength 0..255 (255 when left out)
        RC_drive(FORWARD, 150)     # FREE / REVERSE / FORWARD / BRAKE, 0..255

RumiCar.py is not a copy kept here: the build takes it from a fixed commit of
the RumiCar repository, checks its SHA-256 and puts it next to this file
(car/logic/CMakeLists.txt).  Started with -I, Python does not look in this
directory for modules, so the runner loads RumiCar.py from its path and
registers it as the module `RumiCar` before drive.py is loaded.  The library's
Zero2Cam hardware layer (RumiCar._use_zero2cam) does no output: the runner
puts the frame's sensor values in it before each loop() and sends the values
the RC_* functions left in it (steer, steer_pwm, drive_dir, drive_pwm, state)
to the core after it.  Standard library only.
"""
import importlib.util
import json
import os
import sys
import time
import traceback


def _load_library():
    """RumiCar.py next to this file, as the module `RumiCar` (what `from RumiCar import *` gets)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "RumiCar.py")
    spec = importlib.util.spec_from_file_location("RumiCar", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules["RumiCar"] = module
    spec.loader.exec_module(module)
    return module


def _where(exc):
    """The drive.py line an exception came from, for the one-line message."""
    tb = traceback.extract_tb(exc.__traceback__)
    for fr in reversed(tb):
        if os.path.basename(fr.filename) == "drive.py":
            return f"drive.py の {fr.lineno} 行目"
    return "drive.py"


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

    # --- load the RumiCar library, params.json and drive.py ---------------------------------------------
    try:
        library = _load_library()
    except Exception as e:   # noqa: BLE001 — a broken logic directory: reported, not raised
        traceback.print_exc()
        reply(f"ERROR py_drive の RumiCar.py を読み込めません: {type(e).__name__}: {e}".replace("\n", " "))
        return 1
    try:
        params = {}
        params_path = os.path.join(pack_dir, "params.json")
        if os.path.isfile(params_path):
            with open(params_path, encoding="utf-8") as f:
                params = json.load(f)
            if not isinstance(params, dict):
                raise ValueError("params.json は JSON オブジェクト（{ ... }、名前と値の組）で記述してください")
        car = library._use_zero2cam(params)   # the Zero2Cam hardware layer: the frame's values
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
    print(f"[drive.py] started ({pack_dir}, RumiCar.py {library.__version__})", file=sys.stderr, flush=True)

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
            car.tof = [int(p) for p in parts[3:6]]
            loop()
            reply(f"C {seq} {car.steer} {car.steer_pwm} {car.drive_dir} {car.drive_pwm} {car.state}")
        except Exception as e:   # noqa: BLE001 — the frame brakes; the program keeps running
            message = f"{_where(e)}: {type(e).__name__}: {e}".replace("\n", " ")
            # The same error every frame would flood the log: print it again only after 5 s.
            if message != last_error or time.monotonic() - last_error_time > 5.0:
                traceback.print_exc()
                last_error, last_error_time = message, time.monotonic()
            reply(f"X {seq} {message}")


if __name__ == "__main__":
    sys.exit(main())
