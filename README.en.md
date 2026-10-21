# RumiCar Zero2Cam

**English** | [日本語](README.md)

Software for the RumiCar built around a Raspberry Pi Zero 2 W and a Camera Module 2.
One repository holds the car side (C++), the PC drive manager (C# / .NET 10), the wire formats between them, the distribution formats, and the automated builds.

- How to use it (from writing the SD card to driving, recording, and updating decision data): the [How to use](#how-to-use) section below
- Specification (settings, wire formats, distribution formats, builds): [docs/RumiCar-Zero2Cam 仕様書.docx](docs/RumiCar-Zero2Cam%20仕様書.docx) (Japanese)
- Design record (what was investigated, the options compared, why things were decided, how the image logic was designed and built — for learning): [docs/RumiCar-Zero2Cam 設計の記録.docx](docs/RumiCar-Zero2Cam%20設計の記録.docx) (Japanese)
- Downloads: [Releases](https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases)

## Layout

| Directory | Contents |
|---|---|
| `car/` | Car side: `rumicar-core` (control loop, hardware, network, hot-swap of versions), the driving logic `tof_drive` (shared library), the bundled pack `base-tof`, unit tests, Debian package and SD-card image builds |
| `pc/` | PC side: `RumiCar.Protocol` (wire formats, manifests, recording), `RumiCar.Protocol.Tests`, `RumiCar.DriveManager` (WPF drive manager) |
| `docs/` | The specification and the design record (both .docx, in Japanese; published for learning) |
| `.github/workflows/` | CI (`car.yml`: Debian 13 arm64/amd64 and the SD-card image, `pc.yml`: Windows, `release.yml`: releases from tags) |

---

# How to use

## 1. What you need

| Item | Notes |
|---|---|
| A RumiCar chassis with a Raspberry Pi Zero 2 W | DRV8835 motor driver, three VL53L0X distance sensors (left, centre, right), Camera Module 2. Wiring is the same as the existing RumiCar (spec 3.4) |
| microSD card | 8 GB or more (Class 10 / A1 recommended) |
| Windows PC (64-bit, Windows 10/11) | With Wi-Fi; runs the drive manager |
| SD-card writer | [Raspberry Pi Imager](https://www.raspberrypi.com/software/) |

The car **does not drive just because it is powered on**: it boots into "record only" (motors braked) and starts driving when you press **「自動」** (auto) in the drive manager (since v0.1.3; earlier versions drove off at power-on). To have it drive at power-on without a PC, set `startup_mode` to `auto` in `/etc/rumicar/core.json` (section 8). Put the car on a stand so the wheels are off the floor for the first boot and while you check things.

## 2. Write the SD card

1. From the latest [Release](https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases) download:
   - `rumicar-zero2cam-<version>-<date>.img.xz` — the car's SD-card image (about 620 MB)
   - `RumiCarDriveManager-<version>-win-x64.zip` — the PC drive manager
2. Start Raspberry Pi Imager. **Device**: Raspberry Pi Zero 2 W. **OS**: "Use custom" (at the bottom of the list) and pick the downloaded `.img.xz`. **Storage**: your SD card.
3. Press Next. When asked "Would you like to apply OS customisation settings?" answer **No**. The user, SSH, Wi-Fi and hostname are already in the image; setting Wi-Fi here would conflict with the image's setup (the car is an access point).
4. When writing has finished, put the card in the car.

Optional check of the download: the values in the Release's `SHA256SUMS.txt` must match `Get-FileHash <file>` in PowerShell.

## 3. First boot

1. Put the car on a stand and switch it on.
2. The first boot expands the partition and reboots once by itself. Wait 2–3 minutes.
3. `rumicar-core` then starts and brings things up one after another: sensors → Wi-Fi access point → camera (once a PC connects). The power-hungry parts are deliberately not started at the same time, to avoid brown-outs on dry-cell batteries (section 10). **The car does not drive yet** (record only); connect as in section 4 and press 「自動」.

Defaults built into the image:

| Item | Value |
|---|---|
| Wi-Fi access point | SSID `RumiCar`, no encryption (the image is public; accepted for educational use) |
| Car address | `10.42.0.1` (the PC receives `10.42.0.x`) |
| Login | user `rumicar`, password `rumicar`, SSH enabled, hostname `rumicar` |
| Wireless regulatory country | JP (needed for the access point) |

## 4. Connect from the PC

1. Connect the PC's Wi-Fi to `RumiCar` (there is no password; connect even if Windows says the network is not secure).
2. Unzip `RumiCarDriveManager-<version>-win-x64.zip` and run `RumiCarDriveManager.exe` (no installation).
   - If "Windows protected your PC" (SmartScreen) appears the first time, choose "More info" → "Run anyway". The executable is not code-signed.
   - If Windows Defender Firewall asks, choose "Allow access".
3. Leave "車両" (car) at `10.42.0.1` and press **「接続」** (Connect). When the log shows `車両 RumiCar, 本体 <version>` and the status panel shows ToF values, the logic state and the camera image, you are connected.

The drive manager's user interface is in Japanese; the table below gives the meaning of each part.

## 5. The drive manager

| Part | Meaning |
|---|---|
| 車両 / 名前 | Car address (default `10.42.0.1`) and the car name used in recording folder names (default `RumiCar`) |
| 非常停止 / 非常停止解除 | Emergency stop / clear. The e-stop brakes in every mode and the car stays stopped until cleared. It is also latched automatically when, in manual mode, the PC's heartbeat stops for 1 s or the PC disconnects |
| Image | Latest camera image (320×240, about 20 frames/s). Untick 「画像を表示」 to stop only the display (frames are still received and recorded; lowers the PC load) |
| 状態 (status) | モード = mode; 非常停止 = e-stop (green = none, red = active, with reason); 判断状態 = logic state (CRUISE / TURN_LEFT / TURN_RIGHT / EMERGENCY_BRAKE / RECOVER_REVERSE / SENSOR_FAULT); ToF 左/中/右 = left/centre/right distance [mm] (`open` = nothing nearby, `--` = no reading); モーター出力 = what went to the motors; ロジックの判断 = what the logic decided; 版 = logic / pack versions; **判断データ** = the active pack's name, version and description; **対応機能** = what it handles (壁 walls / 信号機 traffic light / 車両 vehicle / 交差点 intersection); **学習元** = the recorded scenarios it was built from and its parent version; **電源** = under-voltage / throttling reported by the car's firmware (「正常（低電圧なし）」 is normal) |
| モーター出力 / ロジックの判断 | Shown as 「操舵 左/中央/右 / 前進 74.8% (191/255)」: steering left/centre/right, then 前進 forward / 後退 reverse / 停止 coast / ブレーキ brake. The percentage is the **PWM duty cycle** of the drive motor (the car uses per mille 0–1000) and the bracketed value is the same duty on the Arduino `analogWrite` scale 0–255 (748 × 255 ÷ 1000 ≈ 191). モーター出力 is what actually went to the motors; ロジックの判断 is what the logic decided for that frame (shown even in manual, record-only and e-stop) |
| 走行モード (drive mode) | **自動** auto: the car's logic drives / **手動** manual: arrow keys on the PC (↑ forward 50 %, ↓ reverse 50 %, ←→ steer; click the image first so the keys reach the car) / **収録のみ** record-only: motors stopped, images and status still streamed (push the car by hand to collect data). The line below shows the start-up mode (default: record only) |
| 収録 (recording) | Destination folder (default `C:\Users\<you>\RumiCarData`), **シナリオ** scenario (what this recording is: e.g. "壁のみの走行データ" walls only, "信号機ありケース1" traffic light case 1; type a name or pick one used before), **メモ** note (optional), and 収録開始 / 収録停止 (start / stop). **By default a recording starts automatically on every connection** (the 「接続したら自動で収録を開始する」 checkbox) and closes on disconnect. The scenario and note at the moment the recording starts are written to `session.json`. Every received frame is saved. The "(画像付き N)" count next to 受信コマ数 grows when camera images arrive |
| 動画を作る… (make video) | Pick a session folder; its images become `video.avi` (Motion JPEG, real time, 20 fps) in that folder. Works without a connection (6.4) |
| 版の管理 (versions) | Logic and packs installed on the car (`[使用中]` = active, `[直前]` = previous, with verification failures shown). 一覧更新 refresh, 送る… send, 有効化 activate, 削除 delete, ロジックを戻す roll back the logic, データを戻す roll back the pack (section 6) |
| ログ (log) | Connection, responses, events and errors with timestamps |

Press 「切断」 to disconnect. The car keeps driving after you close the app (it is designed to run without a PC). Use the emergency stop or switch the car off to stop it.

## 6. Collect driving data and improve the decision data

### 6.1 Record

Connecting (or 「収録開始」 when the automatic start is unchecked) creates one folder per recording under the destination:

```
RumiCarData/
  sessions/
    RumiCar_20261018-123456/      <car name>_<start time, UTC>
      frames/000001.jpg …        images (numbered within the recording)
      meta.jsonl                 one line per frame: {"index":1,"image":"000001.jpg","received":"…","header":{…status from the car…}}
      session.json               summary: scenario, note, the logic/pack the car ran at the start (with description, capabilities, source scenarios), frame and image counts, gaps, whether under-voltage was seen
  scenarios.json                 the scenario names used so far (newest first, with counts)
```

`header` holds, for that frame, the ToF values, the logic state and decision (steer, drive), what was actually sent to the motors, the mode, the e-stop state, the logic and pack names and versions (`logic_name` / `pack_id`), the power flags (`throttled`), and the image's sequence number and timestamp.

**Naming scenarios**: before recording, type the scene into the シナリオ box — "壁のみの走行データ" (walls only), "信号機ありケース1" (traffic light, case 1), "信号機と交差点ロジックあり" (traffic light and intersection) — so that the recordings to build a pack from can be picked later. Names used before are offered from `scenarios.json`.

### 6.2 Improve the data with the Claude desktop app

1. Open the `RumiCarData` folder (or its `sessions` folder) in the Claude desktop app, show it the recordings and ask for improvements.
2. The decision data used by the current driving logic `tof_drive` is a single parameter file, `params.json`. Spec section 3.7 lists every key with its meaning and default (for example `d_turn_enter_mm`, the distance ahead at which a turn starts; `v_max`, the top speed; `accel_permille_per_s`, the acceleration rate).
3. Put the result in a folder of this shape (`manifest.json` is produced by `car/tools/make_manifest.py`, which needs only Python 3):

```
base-tof-0.2.0/            folder name is <id>-<version>
  params.json              every key is required
  manifest.json            python car/tools/make_manifest.py pack base-tof-0.2.0 --id base-tof --version 0.2.0 --logic tof_drive \
                               --description "Walls only; turns start earlier (d_turn_enter_mm 220)" \
                               --capability walls --scenario "壁のみの走行データ" --parent base-tof-0.1.1
```

Use a version (`0.2.0` and so on) that is not yet installed on the car.

`--description` (what the version is), `--capability` (what it handles: `walls`, `traffic_light`, `vehicle`, `intersection`; repeatable), `--scenario` (the recorded scenarios it was built from; repeatable) and `--parent` (the version it was derived from) are optional, but with them the drive manager shows 判断データ / 対応機能 / 学習元 and the version list says what each pack is, so you always know which logic and data the car is driving on. `python car/tools/make_manifest.py show <folder>` prints them.

### 6.3 Send it to the car and switch

1. With the drive manager connected, press **「送る…」** in 版の管理 and choose the folder. The app checks the manifest before sending.
2. When the new version appears in the list, select it and press **「有効化」** (activate). The car verifies it and switches at the next frame, even while driving. If verification fails the car keeps the current version and the log shows why.
3. If it drives badly, **「データを戻す」** returns to the previous pack. Versions you no longer need can be selected and deleted with 「削除」 (the active and the previous version cannot be deleted).

A rebuilt driving logic (the `tof_drive` `.so`) is sent the same way (`make_manifest.py logic …`, spec section 4).

### 6.4 A video from a recording

「動画を作る…」 in the drive manager takes a session folder and writes `video.avi` from its `frames/` (Motion JPEG: the received JPEGs are stored as they are, so nothing is re-encoded; 20 fps on a real-time timeline from the image timestamps; where frames are missing the previous frame is held for up to 1 s and longer pauses are cut). VLC, Windows Media Player and ffmpeg play it. From the Claude desktop app, `python car/tools/session_video.py <session folder>` does the same (Python 3 only). For an MP4: `ffmpeg -i video.avi -c:v libx264 -pix_fmt yuv420p video.mp4`.

## 7. Learning from images (traffic lights, a vehicle ahead, intersections)

### 7.1 How it works

The car keeps driving on the ToF (distance) sensors; on top of that, every camera image is turned into a small grid of features (320×240 reduced 5× and split into 8×6 cells; per cell the share of each colour, the brightness and the amount of edges — 576 integers), and **detectors learned from your recordings** are applied to it. A detector either says *where* something is (local: red light, green light, vehicle — it looks at a 3×3 window of cells) or judges the *whole view* (global: an intersection ahead). Three behaviours turn the detections into a speed cap and a steering mode for the ToF driving:

| Behaviour | Detectors | What it does |
|---|---|---|
| `signal` | red / green light (local) | Red seen for a few frames: slow down; close: stop; green (or red gone for a while): drive on |
| `vehicle` | vehicle (local) | A vehicle in our lane (the centre columns of the grid): hold the lane and follow at a distance. If it stays stopped it is parked, so pass it on the side the ToF shows clear (out → alongside → in) |
| `intersection` | intersection with a road to the left / right (global) | Slow down when one is in view, stop where a side wall ends (a side ToF opens up), then go straight, turn left or right as configured |

The three are independent; where they meet the **lowest speed cap wins**, and a passing or turning manoeuvre is cut short by a red light. The ToF logic's emergency brake, reversing recovery and sensor fault always take precedence (the camera only adds to the ToF driving; it never overrides its safe moves).

What is learned is a pack: `params.json` (ToF and behaviour settings, human-readable), `vision.bin` (the detectors' weights, a binary the car reads quickly) and `manifest.json` (description, capabilities, source scenarios). It is sent to the car exactly as in section 6. The driving logic `cam_drive` ships with the car next to `tof_drive`. A behaviour whose detectors the pack does not have is simply off, so you can grow a pack step by step: lights first, vehicles next.

### 7.2 What you need

- Python 3 with numpy and Pillow (`pip install numpy pillow`) — in the Claude desktop app's environment or on your PC
- `car/tools/` from this repository (`train_pack.py`, `rumicar_vision.py`, `make_manifest.py`, `session_video.py`)
- Optional: `rumicar-replay` (GitHub Actions artifact `rumicar-replay-amd64`), which replays a recording through the real logic so you can see the car stop and go before sending anything

### 7.3 Steps

1. **Record**: drive a course with a traffic light (or push the car by hand in record-only mode to show it red and green) with a scenario name such as "信号機ありケース1". Recordings of a course without lights ("壁のみの走行データ") are used automatically as "no light here" examples, so make some of those too. Record at least twice, once for learning and once for checking.
2. **Label**: `python car/tools/train_pack.py propose <session> --detector traffic_light_red` writes candidate red blobs (frame index and box) to `proposals_traffic_light_red.jsonl`. Show the folder to Claude, have it look at the candidate frames in `frames/` and write the confirmed ones into `labels.jsonl`; the same for `--detector traffic_light_green`. The candidate lines already have the `labels.jsonl` shape (extra keys such as `image` or `pixels`, which say why the line is there, are ignored), so delete the wrong lines and append the rest. One JSON object per line; boxes are pixel coordinates [x0, y0, x1, y1] with x1/y1 exclusive:

```
{"index": 123, "objects": [{"name": "traffic_light_red", "box": [160, 60, 200, 110]}]}   <- a box (local)
{"index": 456, "objects": [{"name": "vehicle", "box": [120, 140, 200, 220]}]}            <- the vehicle ahead (its body)
{"from": 300, "to": 380, "scene": ["intersection_left"]}                                 <- a scene (whole view): frames 300..380 show an intersection ahead with a road to the left
{"index": 200, "negative": true}                                                         <- nothing of interest here
```

   `--detector vehicle` proposes the frames in which the centre ToF saw something close (the box only marks our lane; move it onto the vehicle's body). `--detector intersection_left` (or `_right`) proposes scenes from the ToF: the moment a side reading goes from "wall" to "open" is the moment the car passes an opening, so the 2 s before it (`--before-ms`) are proposed as "an intersection was ahead"; look at the first and last frame and trim. Unlabelled frames are not used for learning. Looking at about one frame in twenty is enough. A labelled frame is a negative example for every detector not named on it — that is how the red tail lights of a vehicle are taught not to be a red light.

3. **Learn**:

```
python car/tools/train_pack.py train cam-signal-0.1.0 \
    --parent <the cam-base-0.1.0 folder from the car, or the previous version> \
    --sessions <recordings to learn from>... --validate <recordings to check with>... \
    --negatives-from-scenario "壁のみの走行データ" \
    --detector traffic_light_red --detector traffic_light_green \
    --id cam-signal --version 0.1.0 --description "traffic light, case 1"
```

   `cam-base-0.1.0` is also in the repository at `car/packs/cam-base`. The result `cam-signal-0.1.0/` holds `params.json`, `vision.bin`, `training.json` (what was learned, with precision and recall on the validation recordings) and `manifest.json`. Low recall: label more frames; false detections: add more negative (walls-only) recordings. To add vehicles and intersections, learn all the detectors in one go (`:global` marks a whole-view detector):

```
python car/tools/train_pack.py train cam-town-0.1.0 --parent cam-signal-0.1.0 \
    --sessions <light recordings> <vehicle and intersection recordings>... --validate <check recordings>... \
    --negatives-from-scenario "壁のみの走行データ" \
    --detector traffic_light_red --detector traffic_light_green \
    --detector vehicle --detector intersection_left:global --detector intersection_right:global \
    --intersection turn=left --id cam-town --version 0.1.0 --description "lights and intersections"
```

   A behaviour goes into `params.json` only when the pack has its detectors (`behaviours on: signal, vehicle, intersection` is printed). With only one side of intersections learned, the other detector name stays empty (no such side).

4. **Check before sending**: `python car/tools/train_pack.py show cam-signal-0.1.0` (contents and the behaviours that are on), `replay cam-signal-0.1.0 <session>` (per-frame detections) and `rumicar-replay <cam_drive folder> cam-signal-0.1.0 <session>` (the real logic; a sequence CRUISE → SIGNAL_APPROACH → SIGNAL_STOP → SIGNAL_RESUME means it stopped and went on; for a vehicle FOLLOW → OVERTAKE_OUT → OVERTAKE_PASS → OVERTAKE_IN, for an intersection INTERSECTION_APPROACH → INTERSECTION_STOP → (INTERSECTION_CROSS →) INTERSECTION_TURN). A replay feeds the recorded ToF readings as they were, so where the car's own movement would change them (passing), it only goes as far as the recording did: slowing down, stopping and steering out can be checked.
5. **Send to the car**: in the drive manager's 版の管理 select `cam_drive-0.2.0` (shipped) and 有効化, then 送る… `cam-signal-0.1.0` and 有効化. The status card shows the pack's description, "壁 / 信号機" and the scenarios it was learned from. Tick 「判断を重ねる」 to draw the detections over the image (red box = red light, green = green light, sky blue = vehicle, orange frame around the whole image = intersection; thick = confirmed) with their scores. The 判断状態 field shows the state with its meaning, e.g. `FOLLOW（前の車両に追従）`.
6. **Improve**: record more and learn again with `--parent cam-signal-0.1.0 --version 0.2.0`. When you add recordings of new scenes (vehicles, intersections), **learn every detector again**, the old ones included: the new scenes are negatives for them (a vehicle's tail lights are not a red light). `--keep-parent-detectors` is for fixing some detectors while the scenes stay the same (the lineage is kept in the manifest's `parent`).

### 7.4 Traffic-light behaviour (the `signal` part of params.json)

| Key | Default | Meaning |
|---|---|---|
| `red_detector` / `green_detector` | `traffic_light_red` / `traffic_light_green` | Detector names. With no green light on the course set `green_detector` to `""` and rely on `resume_without_green_ms` |
| `stop_min_cells` | 2 | The red light is "close" (stop) once it covers this many cells; before that the car only slows down |
| `approach_v` | 500 | Speed cap (per mille) from seeing red until stopping |
| `min_stop_ms` | 1000 | Stay stopped at least this long |
| `resume_without_green_ms` | 3000 | Drive on this long after the red light disappeared (0 = wait for green) |
| `resume_ignore_ms` | 1500 | Ignore red for this long after moving off (so the light just passed does not stop the car again) |

Override at learning time with `--signal key=value` (e.g. `--signal stop_min_cells=3`).

### 7.5 Vehicle behaviour (the `vehicle` part of params.json)

Absent: the behaviour is off. Present: every key is required (`train` adds it to packs that have the `vehicle` detector; override with `--vehicle key=value`).

| Key | Default | Meaning |
|---|---|---|
| `detector` | `vehicle` | Name of the local detector |
| `lane_cols` | 4 | The detected vehicle counts as "in our lane" only when its best cell lies in the centre this many columns of the grid (columns 2–5 of 8) |
| `follow_gap_mm` | 150 | Minimum gap (centre ToF) to the vehicle ahead: speed cap 0 at or below it. Must be above `tof.d_emergency_mm` and below `tof.conf_max_mm` |
| `follow_band_mm` | 80 | The cap rises from 0 to `follow_v` as the gap grows by this much |
| `follow_v` | 700 | Speed cap while following (also while the vehicle is seen by the camera only, not yet by the ToF) |
| `parked_confirm_ms` | 2000 | Stopped behind the vehicle with the gap not growing for this long: it is parked |
| `still_delta_mm` | 20 | The gap growing by more than this during that wait means it is moving off (wait restarts) |
| `overtake_side` | `open` | Which side to pass on: `left`, `right` or `open` (the side with more ToF room; equal → right) |
| `side_clear_mm` | 120 | That side's ToF must read at least this (or nothing) to pass; otherwise `FOLLOW_WAIT` |
| `overtake_v` | 500 | Speed cap during the manoeuvre |
| `out_ms` | 500 | Steer out (full lock towards the passing side) this long |
| `pass_min_ms` / `pass_max_ms` | 800 / 3000 | Shortest / longest time alongside (wheels straight) |
| `pass_clear_mm` / `pass_clear_ms` | 150 / 300 | The inner side ToF, having read below this once (the vehicle beside us), then reads at least this for this long: passed |
| `in_ms` | 500 | Steer back in (full lock the other way) this long |
| `ignore_ms` | 1500 | Ignore the vehicle detector this long after passing. Also: following with the camera not seeing the vehicle for this long ends the following and hands back to the ToF logic (too close to be recognised: the ToF logic turns around it as an obstacle) |

Passing is an open-loop manoeuvre timed in steps, because the steering has only three positions (left, centre, right); tune the out/in times to your course and speed.

### 7.6 Intersection behaviour (the `intersection` part of params.json)

Absent: off. Present: every key is required (`train` adds it to packs that have `intersection_left` or `intersection_right`; override with `--intersection key=value`).

| Key | Default | Meaning |
|---|---|---|
| `left_detector` / `right_detector` | `intersection_left` / `intersection_right` | Global detectors for an intersection with a road to the left / right; one may be `""` (not both) |
| `approach_v` | 500 | Speed cap from seeing the intersection until stopping |
| `stop_trigger` | `side_open` | When to stop: `side_open` = a side ToF reads `open_mm` or more (a wall ended); `detector` = as soon as the detector is confirmed (courses without walls, with only close intersections labelled) |
| `open_mm` | 200 | A side reading at or above this (or nothing) is an opening |
| `stop_ms` | 1000 | Stop this long (and never move off into a red light) |
| `turn` | `straight` | Then: `straight`, `left`, `right`, or `detector` (towards the side whose detector saw it; both → straight) |
| `turn_v` | 500 | Speed cap while crossing and turning |
| `cross_max_ms` | 2000 | Drive straight at most this long waiting for the turn side to open (then go on straight) |
| `turn_ms` | 900 | Full lock towards the turn side this long |
| `ignore_ms` | 3000 | Ignore the intersection detectors this long afterwards |

### 7.7 How the behaviours combine

- The speed cap is the minimum over the behaviours: a red light while following gives `SIGNAL_STOP`, reaching an intersection `INTERSECTION_STOP`.
- The "parked" wait does not run while a red light or an intersection is being handled (no passing a car waiting at a light). A red light or an intersection during a pass cuts it short; following resumes afterwards.
- The intersection stop does not move off into a red light, and a red light during crossing or turning cuts it short.
- The reported state is the one deciding speed and steering: a red light (`SIGNAL_APPROACH` / `SIGNAL_STOP`) first, then intersection, then vehicle, then `SIGNAL_RESUME`; the ToF logic's emergency brake, reversing and sensor fault show as themselves.

### 7.8 When it does not work

| Symptom | What to do |
|---|---|
| Stops for things that are not red lights | Add walls-only recordings, or recordings with that object, as negatives; look at the precision in `training.json`; raise `--thr-on` |
| Stops too far from / too close to the light | Raise / lower `stop_min_cells`; check the camera is mounted at the same angle as when recording |
| Stays stopped | Is the green detector seeing green (検出 field)? Without one, set `resume_without_green_ms` |
| Turns around the vehicle instead of stopping behind it | With 判断を重ねる on, is the vehicle's box in the centre columns (`lane_cols`)? If the detection drops as the vehicle fills the view, label close-up frames too. Raise `follow_gap_mm` |
| Passing hits or swings wide | Tune `out_ms`, `in_ms`, `overtake_v`; with a wall close on the passing side raise `side_clear_mm` (it then waits) |
| Does not pass the parked vehicle | `FOLLOW_WAIT` means neither side reads `side_clear_mm`; `FOLLOW` means the detection dropped (row above) or a light / intersection is being handled |
| Does not stop at the intersection | Does `replay` show `intersection_*` confirmed? With walls, is `open_mm` below the wall distance? Without walls use `stop_trigger` = `detector` |
| Does not turn | The turn side did not open (`open_mm`) within `cross_max_ms`; with `turn=detector` both sides seen means straight |
| The sent version shows 検証失敗 | `make_manifest.py verify`; if the vision.bin self-test fails, the camera size (320×240) differs from the recordings; check the tool and car versions match (ABI 2) |
| `rumicar-replay` and the car disagree | Recordings are JPEG, the car sees raw frames, so small differences are normal; large ones usually mean different lighting — record under that lighting too |
| A detection stays confirmed and never clears | In `show`, is the second threshold (release) above the score of "nothing"? `train` keeps it above the negatives' top 1 % of scores; a smaller `--hysteresis` raises it |

## 8. Logging in to the car (SSH)

```sh
ssh rumicar@10.42.0.1          # password rumicar
sudo journalctl -u rumicar-core -f     # core log (state changes, switches, errors)
systemctl status rumicar-core          # service status
rumicar-activate show                  # active / previous versions and the last switch result
sudo nano /etc/rumicar/core.json       # settings (then sudo systemctl restart rumicar-core)
```

Main settings in `/etc/rumicar/core.json` (spec 3.3): `car_name`, `startup_mode` (`record_only`, the default, or `auto` to drive at power-on), `max_drive_permille` (drive limit, 1000 = 100 %), `camera.enabled`, `camera.on_demand` (run the camera only while a PC is connected or in auto mode; default true), `camera.fps`, `network.heartbeat_timeout_ms`, `network.ap_start` (`core`, the default: the core brings the access point up in sequence; `boot`: NetworkManager does it), `network.wifi_txpower_dbm` (transmit power limit; 0 = driver default; try 10–14 when batteries sag), `power.throttled_path` (under-voltage / throttling monitor; empty disables it).

## 9. Updating

### 9.1 Whole image

Write the image of a newer Release to the SD card (section 2). Recordings live on the PC, so nothing is lost.

### 9.2 Package only

Copy `rumicar-zero2cam_<version>_arm64.deb` from the Release to the car and install it (if the car is driving, the motors stop briefly while the service restarts).

```sh
scp rumicar-zero2cam_<version>_arm64.deb rumicar@10.42.0.1:
ssh rumicar@10.42.0.1
sudo apt install ./rumicar-zero2cam_<version>_arm64.deb
```

The required libraries are already in the image, so no Internet access is needed. If you edited `/etc/rumicar/core.json` or the access-point profile yourself, apt asks whether to replace them (otherwise they are updated silently).

### 9.3 When the car needs the Internet

The car is normally an access point and has no route to the Internet. To install something with `apt`, join your home Wi-Fi temporarily (this stops the `RumiCar` access point until the next reboot):

```sh
sudo nmcli device wifi connect "<home SSID>" password "<password>"
```

## 10. Troubleshooting

| Symptom | What to check |
|---|---|
| `RumiCar` does not appear in the Wi-Fi list | Wait 1–2 minutes after boot. Over SSH: `nmcli connection show` lists `RumiCar-AP`? `sudo journalctl -u NetworkManager -b` shows errors? `iw reg get` says `JP`? |
| The app cannot connect | Is the PC on `RumiCar` with a `10.42.0.x` address (`ipconfig`)? Firewall allowed? On the car: `systemctl status rumicar-core` |
| No image | On the car: `sudo journalctl -u rumicar-core -b \| grep -i camera`. Camera ribbon orientation. `rpicam-hello --list-cameras` shows the camera? |
| The car does not drive | After boot it waits in record-only mode (press 「自動」 in the app; set `startup_mode` to `auto` to drive without a PC). Does `sudo journalctl -u rumicar-core -b` start with `hardware bring-up failed`? (The I2C/PWM settings are appended to `/boot/firmware/config.txt`.) Is the e-stop latched (「非常停止解除」 in the app)? ToF readings of `--` mean SENSOR_FAULT, which stops the car |
| Power drops or the car reboots during start-up (dry cells) | Since v0.1.3 the core starts sensors → Wi-Fi → camera in sequence and keeps the motors stopped until 「自動」 is pressed, so the power-hungry parts never start together. If it still drops out: fresh or rechargeable (NiMH) cells; lower the radio's transmit power with `network.wifi_txpower_dbm` (about 10); watch the 電源 field in the app for 「低電圧」 (on boards that report it). Drops while driving: lower `max_drive_permille` to limit the current at acceleration |
| Wi-Fi disappeared | The access point is brought up by `rumicar-core`; if the core stops, `rumicar-ap.timer` re-activates it from 90 s after boot and then every 60 s. Check `systemctl status rumicar-ap.timer` and `nmcli connection show --active` |
| `hardware bring-up failed: open /dev/i2c-1` | The v0.1.1 image did not load the `i2c-dev` kernel module, so `/dev/i2c-1` was missing. `sudo raspi-config` → Interface Options → I2C → Yes fixes it (persists across reboots). Not needed with the v0.1.2 image and package |
| A switch fails | The reason in the app's log (manifest mismatch, missing keys, wrong logic name…). `rumicar-activate show` on the car |
| SSH password rejected | The first-boot setup may not have finished yet. Reboot and wait a few minutes |

Please report problems in [Issues](https://github.com/RumiCar-group/RumiCar-Zero2Cam/issues) with the app's log or the output of `sudo journalctl -u rumicar-core -b`.

---

# For developers

## Building the car side (Raspberry Pi OS Lite 64-bit / Debian 13)

```sh
sudo apt install build-essential cmake pkg-config libgpiod-dev nlohmann-json3-dev libssl-dev libturbojpeg0-dev libcamera-dev
cmake -S car -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build --prefix /usr/local   # manual install
```

To install on a car, use the Debian package (the CI artifact `rumicar-zero2cam-deb-arm64`, or configure with `cmake -S car -B build -DCMAKE_INSTALL_PREFIX=/usr` and run `cd build && cpack -G DEB`). `sudo apt install ./rumicar-zero2cam_<version>_arm64.deb` installs the core, the bundled logic and pack, the systemd service and the Wi-Fi access-point profile (SSID `RumiCar`, 10.42.0.1), and prepares the state directory `/var/lib/rumicar`.

Without libcamera, build with `-DRUMICAR_WITH_CAMERA=OFF` (that core does not start a camera and drives on the ToF sensors only).

The driving logics live in `car/logic/`: `tof_drive` (ToF driving) and `cam_drive` (the camera's judgement on top of it: `vision_features` features, `vision_pack` for `vision.bin`, `vision_eval` detector evaluation with temporal confirmation, `cam_logic` the traffic-light, vehicle and intersection behaviours and their precedence). The contract between the core and the logics is `car/include/rumicar/logic_api.h` (ABI 2: the `aux` output and `aux_name`). The Python implementation of the features and `vision.bin` is `car/tools/rumicar_vision.py`; `test_vision` checks it against the C++ bit for bit through the golden data in `car/tests/data/vision` (made by `make_golden.py`). `car/tests/tools_smoke.py <build dir>` learns from synthetic recordings and checks with `rumicar-replay` that the car stops and goes, follows and passes, and stops at an intersection (CI runs it too).

## Building the PC side (Windows / .NET 10 SDK)

```sh
dotnet build pc/RumiCar.sln -c Release
dotnet test pc/RumiCar.Protocol.Tests -c Release
dotnet publish pc/RumiCar.DriveManager -c Release -r win-x64 --self-contained -p:PublishSingleFile=true
```

## SD-card image and Releases

- The image is made by `car/image/build-image.sh` (root, arm64 host: the official Raspberry Pi OS Lite 64-bit image gets the package installed in a chroot plus the first-boot settings). In CI the `image` job of `car.yml` builds it on pushes to main and on tags.
- A Release is made by tagging a commit on main as `vX.Y.Z` and pushing the tag (or by creating a Release with a new tag on GitHub's Releases page): `release.yml` then attaches the image, the `.deb`, the PC app and `SHA256SUMS.txt`.

## Scope of this version (0.1.x)

- Car: ToF driving logic, control loop and safety behaviour, drive modes (auto / manual / record_only; record_only at start-up) and emergency stop, Camera Module 2 capture (libcamera, RGB24 320×240 at 20 fps, run only when needed) with JPEG telemetry, control server (TCP 5600) and telemetry server (TCP 5601), Wi-Fi access point (NetworkManager, brought up in sequence by the core) and `set_wifi`, under-voltage / throttling monitor, hot-swapping of logic / packs (from the PC or over ssh) verified by manifests that also carry a description, capabilities and source scenarios, systemd service, Debian package
- PC: wire formats for control, telemetry, manifests and recording; the drive manager (connection, status and image, modes, e-stop, manual driving, recording with scenario names, version management, videos from recordings)
- Car: the image logic `cam_drive` (stop at red and go on green; follow a vehicle ahead and pass a parked one; stop at intersections and go straight or turn), the learned data `vision.bin`, the learning tool `train_pack.py`, the replay tool `rumicar-replay`, logic ABI 2
- Distribution: standard SD-card image, GitHub Releases from tags
- Next: recordings and tuning on real cars (passing and turning times, how intersections look), choosing the turn by the situation (now fixed per pack, or towards the side seen)

## License

MIT License ([LICENSE](LICENSE))
