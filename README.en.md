# RumiCar Zero2Cam

**English** | [日本語](README.md)

Software for the RumiCar built around a Raspberry Pi Zero 2 W and a Camera Module 2.
One repository holds the car side (C++), the PC drive manager (C# / .NET 10), the wire formats between them, the distribution formats, and the automated builds.

- Usage (from writing the SD card to driving, recording, and updating the decision data): the [How to use](#how-to-use) section below
- Specification (settings, wire formats, distribution formats, builds): [docs/RumiCar-Zero2Cam 仕様書.docx](docs/RumiCar-Zero2Cam%20仕様書.docx) (Japanese)
- Design record (investigations, options compared, rationale for decisions, design and implementation history of the image logic; reference material for learning): [docs/RumiCar-Zero2Cam 設計の記録.docx](docs/RumiCar-Zero2Cam%20設計の記録.docx) (Japanese)
- Downloads: [Releases](https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases)
- Explanatory videos (3, Japanese and English: ToF readings, start control in auto mode, overtaking a parked vehicle): [12. Explanatory videos](#12-explanatory-videos)

## Layout

| Directory | Contents |
|---|---|
| `car/` | Car side: `rumicar-core` (control loop, hardware control, networking, hot-swap mechanism), the driving logics `tof_drive`, `cam_drive` and `py_drive` (shared libraries; `py_drive` executes a Python program written by the user), the bundled packs `base-tof`, `cam-base` and `py-sample`, the learning and replay tools, unit tests, Debian package and SD-card image builds |
| `pc/` | PC side: `RumiCar.Protocol` (wire formats, manifests, recording), `RumiCar.Protocol.Tests`, `RumiCar.DriveManager` (WPF drive manager) |
| `docs/` | The specification and the design record (both .docx, in Japanese; published for educational use), the figure sources (`docs/figures`, Mermaid, Japanese and English) |
| `.github/workflows/` | CI (`car.yml`: Debian 13 arm64/amd64 and the SD-card image, `pc.yml`: Windows, `release.yml`: releases from tags) |

**Figure 1** System overview (car, PC, Claude desktop app)

<!-- figure: en/01-system.mmd -->
```mermaid
flowchart TB
    subgraph CAR["Car (Raspberry Pi Zero 2 W)"]
        direction TB
        SEN["VL53L0X ×3<br/>(left, centre, right)"] --> CORE
        CAM["Camera Module 2<br/>(320×240, 20 fps)"] --> CORE
        CORE["rumicar-core<br/>control loop 50 ms"] <--> LOGIC["Driving logic (.so)<br/>tof_drive / cam_drive / py_drive"]
        PACK["Decision data (pack)<br/>params.json / vision.bin / drive.py"] --> LOGIC
        CORE --> MOT["DRV8835<br/>steering and drive motors"]
    end
    subgraph PCSIDE["PC (Windows)"]
        direction TB
        APP["Drive manager<br/>status and image display, drive mode,<br/>emergency stop, version management"] --> REC["Recording folder<br/>frames/, meta.jsonl, session.json"]
    end
    CORE <-->|"Wi-Fi AP (SSID RumiCar, 10.42.0.1)<br/>control TCP 5600 / telemetry TCP 5601"| APP
    REC --> CL["Claude desktop app<br/>labelling, learning with train_pack.py"]
    CL --> NEW["New pack<br/>(with manifest.json)"]
    NEW -->|"送る… (send) → 有効化 (activate)<br/>(swappable while driving)"| APP
```

---

# How to use

## 1. Required equipment

| Item | Notes |
|---|---|
| A RumiCar chassis with a Raspberry Pi Zero 2 W | DRV8835 motor driver, three VL53L0X distance sensors (left, centre, right), Camera Module 2. Wiring identical to the existing RumiCar (spec 3.4) |
| microSD card | 8 GB or more (Class 10 / A1 recommended) |
| Windows PC (64-bit, Windows 10/11) | Wi-Fi required; runs the drive manager |
| SD-card writer | [Raspberry Pi Imager](https://www.raspberrypi.com/software/) |

The car **does not start driving on power-on**: it boots into record-only mode (motors braked) and starts driving when **「自動」** (auto) is selected in the drive manager (since v0.1.3; earlier versions started driving at power-on). To start driving at power-on without a PC, set `startup_mode` to `auto` in `/etc/rumicar/core.json` (section 8). For the first boot and for checks, place the car on a stand so that the wheels do not touch the floor.

## 2. Writing the SD card

1. From the latest [Release](https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases) download:
   - `rumicar-zero2cam-<version>-<date>.img.xz` — the car's SD-card image (about 620 MB)
   - `RumiCarDriveManager-<version>-win-x64.zip` — the PC drive manager
2. Start Raspberry Pi Imager. **Device**: Raspberry Pi Zero 2 W. **OS**: "Use custom" (at the bottom of the list), then select the downloaded `.img.xz`. **Storage**: the SD card.
3. Press Next. To "Would you like to apply OS customisation settings?" answer **No**. User, SSH, Wi-Fi and hostname are preconfigured in the image; configuring Wi-Fi here conflicts with the image's configuration (the car operates as an access point).
4. When writing is complete, insert the card into the car.

Optional verification of the download: the values in the Release's `SHA256SUMS.txt` must match the output of `Get-FileHash <file>` in PowerShell.

**Choosing it from Imager's list** (Raspberry Pi Imager 2.0 or later; Imager then downloads and verifies the image itself): in Imager open **App Options → Content Repository → EDIT** (bottom left), enter the URL below and press **APPLY & RESTART**. The OS list for the device "Raspberry Pi Zero 2 W" then shows "RumiCar Zero2Cam <version>": pick it, choose the storage and write (no OS customisation step is shown).

```
https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases/latest/download/rumicar-zero2cam.rpi-imager-manifest
```

This list (`rumicar-zero2cam.rpi-imager-manifest` of the Release) always points at the latest Release; Imager checks the image's SHA-256 before writing and verifies the card afterwards. Downloading the file and double-clicking it opens Imager with the same list. The same screen takes you back to the official Raspberry Pi list.

## 3. First boot

1. Place the car on a stand and switch it on.
2. The first boot expands the partition and reboots once automatically. Wait 2–3 minutes.
3. `rumicar-core` then initialises the sensors → starts the Wi-Fi access point → starts the camera (after a PC connects), in this order. Functions with high current consumption are not started simultaneously, to avoid voltage drops on dry-cell batteries (section 10). **The car does not drive at this point** (record only); connect as in section 4 and select 「自動」.

**Figure 2** Start-up sequence (high-current functions started one after another)

<!-- figure: en/02-startup.mmd -->
```mermaid
flowchart TD
    P["Power on<br/>(systemd starts rumicar-core)"] --> M["1. Brake the motors"]
    M --> S["2. Initialise VL53L0X ×3<br/>(on failure: exit code 3, systemd restarts after 5 s)"]
    S --> L["3. Load the logic / pack and run self_test<br/>(on failure: previous/* is tried once)"]
    L --> W["4. Start the Wi-Fi AP (network.ap_start = core, up to 20 s)<br/>(on failure: rumicar-ap.timer retries every 60 s from 90 s after boot)"]
    W --> N["5. Start the control server (TCP 5600) and telemetry server (TCP 5601)"]
    N --> LOOP["6. Start the control loop<br/>(startup_mode: waits in record_only by default)"]
    LOOP -.->|"PC connected or auto mode<br/>(camera.on_demand = true)"| C["Start the camera (separate thread)<br/>(on failure: retried every 30 s)"]
    LOOP -->|"自動 (auto) in the drive manager"| A["Start driving<br/>(permitted drive increases from 0)"]
```

Image defaults:

| Item | Value |
|---|---|
| Wi-Fi access point | SSID `RumiCar`, no encryption (the image is public; accepted for educational use) |
| Car IP address | `10.42.0.1` (the PC is assigned `10.42.0.x` by DHCP) |
| Login | user `rumicar`, password `rumicar`, SSH enabled, hostname `rumicar` |
| Wireless regulatory country | JP (required for access-point operation) |

## 4. Connecting from the PC

1. Connect the PC's Wi-Fi to `RumiCar` (no password; connect even if Windows reports the network as not secure).
2. Extract `RumiCarDriveManager-<version>-win-x64.zip` and run `RumiCarDriveManager.exe` (no installation required).
   - If "Windows protected your PC" (SmartScreen) appears the first time, choose "More info" → "Run anyway". The executable is not code-signed.
   - If Windows Defender Firewall prompts, choose "Allow access".
3. Leave "車両" (car) at `10.42.0.1` and press **「接続」** (Connect). The connection is established when `車両 RumiCar, 本体 <version>` is logged and the status panel shows the ToF values, the logic state and the camera image.
4. To drive, press **「自動」** (auto) under 走行モード. For a first drive, or before the behaviour of the car and course is known, **set 「自動の最大出力」 (auto limit) to 30–50 % beforehand** (the recommended value as of v0.3.0; raise it while checking the behaviour; section 5). The value is saved on the car and kept across a restart of the core (v0.6.0 and later).

The drive manager's user interface is in Japanese; the table below gives the meaning of each part.

## 5. The drive manager

| Part | Meaning |
|---|---|
| 車両 / 名前 | Car IP address (default `10.42.0.1`) and the car name used in recording folder names (default `RumiCar`) |
| 非常停止 / 非常停止解除 | Emergency stop / clear. The e-stop brakes in every drive mode, and the car does not drive until it is cleared. It is also latched automatically when, in manual mode, the heartbeat is lost for 1 s (`network.heartbeat_timeout_ms`) or the PC disconnects |
| Image | Latest camera image (320×240, about 20 frames/s). Unticking 「画像を表示」 stops only the display (reception and recording continue; use it to reduce the PC load) |
| 状態 (status) | モード = drive mode; 非常停止 = e-stop (green = none, red = active, with reason); 判断状態 = logic state (CRUISE / TURN_LEFT / TURN_RIGHT / EMERGENCY_BRAKE / RECOVER_REVERSE / SENSOR_FAULT, etc.); ToF 左/中/右 = left/centre/right distance [mm] (see the table below); モーター出力 = motor output; ロジックの判断 = logic decision; 版 = logic / pack versions; **判断データ** = the active pack's name, version and description; **対応機能** = supported scenes (壁 walls / 信号機 traffic light / 車両 vehicle / 交差点 intersection); **学習元** = the recorded scenarios used to build the pack and its parent version; **電源** = under-voltage / throttling reported by the car's firmware (「正常（低電圧なし）」 is normal) |
| モーター出力 / ロジックの判断 | Displayed as `RC_steer(LEFT, 128) / RC_drive(FORWARD, 191)`: the calls of the RumiCar library (Arduino RumiCar-lib, RumiCar.py for the Raspberry Pi, `drive.py`) that would give that command, with the same words and numbers. The second argument of `RC_steer` is the **strength** (the share of the signal, 0–255; 0 is neutral, the same as CENTER); at 255 (full) the one-argument form `RC_steer(LEFT)` is shown, as it gives the same output (CENTER has no strength). The drive value is the **PWM duty** 0–255 (255 = 100 %; 150 means a signal of 150/255 ≈ 59 %), the direction `FREE` (coast) / `REVERSE` / `FORWARD` / `BRAKE` (both inputs high). モーター出力 is the value actually output to the motors; ロジックの判断 is the value computed by the logic for that frame (displayed in manual, record-only and e-stop as well) |
| 走行モード (drive mode) | **自動** auto: the car's logic controls driving / **手動** manual: operated from the PC (see "Manual operation" below) / **収録のみ** record-only: motors stopped, images and status still received (use it to collect data while moving the car by hand). The line below shows the start-up mode (default: record only) |
| 操縦パッド (driving pad) | Active in manual mode (see below). Underneath: the command being sent (e.g. 「`RC_steer(LEFT) / RC_drive(FORWARD, 115)`（パッド）」), 最大出力 (maximum output), 停止 (stop), 手動の加速 (manual acceleration), パッドの横位置で操舵の強さを変える (steering strength from the pad's horizontal position) and 左右スクロールの向きを反転 (invert horizontal scrolling). 最大出力, 手動の加速, パッドの横位置で操舵の強さを変える and 自動の最大出力 below are **settings saved on the car** ("Saving the drive settings" below); they cannot be changed until the application is connected and has the car's values |
| 自動の最大出力 (auto limit) | Upper limit of the drive (the `RC_drive` value 0–255) output by the logic in auto mode (default 255). Saved on the car and kept across a restart of the core (up to v0.5.x a restart reset it to 255). It cannot be set above `max_drive_pwm`. Independent of the manual 最大出力. The start kick (`kick_pwm`, spec 3.4) does not exceed it either. On entering auto mode the permitted drive increases from 0 at the rate `auto_accel_pwm_per_s` and is capped at this limit (no sudden start when switching from manual, record-only or after clearing an e-stop). For a first drive or an unfamiliar course, starting at 77–128 (30–50 %) is recommended (raise it while checking the behaviour) |
| 収録 (recording) | Destination folder (default `C:\Users\<you>\RumiCarData`), **シナリオ** scenario (name of the recorded scene: e.g. "壁のみの走行データ" walls only, "信号機ありケース1" traffic light case 1; type a name or select one used before), **メモ** note (optional), and 収録開始 / 収録停止 (start / stop). **By default recording starts automatically on connection** (「接続したら自動で収録を開始する」) and ends on disconnection. The scenario and note at the start of the recording are written to `session.json`. Every received frame is saved. If the "(画像付き N)" count next to 受信フレーム数 increases, camera images are being received |
| 動画を作る… (make video) | Select a session folder to create `video.avi` (Motion JPEG, real time, 20 frames/s) from its images in that folder. Usable without a connection (6.4) |
| 版の管理 (versions) | Logic and packs installed on the car (`[使用中]` = active, `[直前]` = previous, verification failures shown; selecting one shows its description, capabilities, source scenarios and target logic). 一覧更新 refresh, 送る… send, 自分のプログラムを送る… send my program (section 11), 有効化 activate, 削除 delete, ロジックを戻す roll back the logic, データを戻す roll back the pack (section 6). A pack is built for a specific logic, so activating a pack for another logic switches the logic at the same time (and vice versa); rollback likewise restores both |
| SD カード (SD card, updating the car) | Shows the newest Release against this application and the connected car (checked on GitHub at start-up and on connection; not possible while the PC has no internet). "SD カードを作る…" (make an SD card) lists the Releases: pick a version and the image is downloaded to `<data folder>\images`, checked with SHA-256, and Raspberry Pi Imager opens with a list holding just that version (section 2). Imager does the writing. Used both for a new card and to update the car (9.1) |
| ログ (log) | Connection, responses, events and errors with timestamps. On connection it records the versions of the car's core and of this application, and records a warning if they belong to different releases (9.4) |

ToF readings (the return values of `RC_read()` of the RumiCar library with no limit set):

| Display | Meaning |
|---|---|
| number | Measured distance [mm] (0–2000: up to the maximum range in the specification of the VL53L0X) |
| `-3 範囲外` | Out of range: the range exceeded 2000 mm (including the 8190 the sensor reports when no light comes back and no target is detected). Driving logic treats it as far (open road) |
| `-1 タイムアウト` | Read failure (no measurement within `i2c.tof_read_timeout_ms`, or an I2C communication error) |

The core does not cut the readings off (up to v0.6.0 it turned every reading above 200 mm into −3). How far a reading is used for decisions is up to the driving logic (see "How far the ToF readings are trusted" below). What is displayed and recorded is the core's own value: the limit a driving logic sets (`conf_max_mm` in the decision data, `RC_setReadLimit` in `drive.py`) does not change it.

**Figure 3** Handling of ToF readings (display and the decisions of the driving logic)

<!-- figure: en/03-tof-reading.mmd -->
```mermaid
flowchart TD
    R["Read VL53L0X"] --> Q{"Measurement within<br/>i2c.tof_read_timeout_ms?<br/>I2C OK?"}
    Q -->|"No"| F["Read failure → −1 (timeout)<br/>display -1 タイムアウト"]
    Q -->|"Yes"| V{"Range beyond 2000 mm?<br/>(including no target, 8190)"}
    V -->|"Yes"| O["Out of range → −3<br/>display -3 範囲外"]
    V -->|"No"| D["Range 0..2000 [mm]<br/>(the core does not cut it off;<br/>this is what is displayed and recorded)"]
    D --> T{"Beyond the limit set by the driving logic?<br/>tof_drive, cam_drive: ≥ conf_max_mm<br/>drive.py: above the limit of RC_setReadLimit"}
    T -->|"Yes"| OPEN["Treated as open (far)<br/>in drive.py RC_read() gives −3"]
    O --> OPEN
    T -->|"No"| USE["Used as a distance<br/>(tof_drive: median filter, exponential smoothing)"]
    F --> HOLD["tof_drive: hold the previous value for invalid_hold_ms (default 300 ms)<br/>then SENSOR_FAULT (stop)<br/>drive.py: RC_read() gives −1"]
```

The window title and the header show the application's version. Next to it the header shows **PC の Wi-Fi**, the name of the network this PC is currently connected to. It turns red, and the event is logged, when the PC leaves the network it was on when it connected to the car (normally `RumiCar`). Windows may automatically switch from an access point without Internet access to another network, so check this display first when communication is interrupted unexpectedly (section 10). If the network name cannot be obtained on Windows 11 24H2 or later, turn on Settings → Privacy & security → Location → "Let desktop apps access your location" (Windows treats network names as location data).

### Manual operation

In manual mode the car is operated with the **driving pad** in the driving card, mouse or touchpad scrolling, and the arrow keys. Steering is left / centre / right with a strength for left and right (the second argument of `RC_steer`, 1–255; 255 by default = `RC_steer(LEFT)` / `RC_steer(RIGHT)`); the drive is the `RC_drive` value 0–255 (PWM duty, in steps of 1). Pad coordinates are normalised with the centre as the origin and the edges at ±1.

| Input | Steering | Drive |
|---|---|---|
| **Press and drag on the pad** (left mouse button; on a touchpad, click and hold, or tap-and-drag) | Left / right at \|x\| ≥ 0.33 (left / right third), back to centre at \|x\| < 0.22 (hysteresis). Strength 255; with パッドの横位置で操舵の強さを変える ticked, proportional to the horizontal position (\|x\| × 255: 128 at 0.5, 255 at the edge) | Upper half forward, lower half reverse. \|y\| ≤ 0.12 is stop; beyond it the drive rises linearly to 最大出力 at the edge. **Releasing stops the car** |
| **Vertical scroll** (wheel, two fingers up/down on a touchpad; over the pad or the image) | — | Changes a speed set-point and **keeps driving at that speed**: 13 (about 5 %) per wheel notch. Between forward and reverse the set-point stops at 0; scrolling again after a 0.4 s pause continues in the other direction |
| **Horizontal scroll** (two fingers sideways, a tilting wheel, Shift + wheel) | Left / right while scrolling (strength 255), centre 0.25 s after the last scroll event | — |
| **Arrow keys** | ← → while held (strength 255) | ↑ ↓ forward / reverse at 最大出力 while held |
| **Stop** | Space, right or middle click on the pad, the 停止 button. Also when the window is deactivated (switching to another application) | |

Precedence: the pad while pressed, otherwise held keys, otherwise scrolling (← → can be combined with the scroll set-point: set a speed by scrolling and steer with ← →). Pressing the pad or ↑ ↓ resets the scroll set-point to 0, so releasing never resumes a previous speed. 最大出力 (default 153, about 60 %) limits all three inputs, the car also limits the manual command to it, and the car's `max_drive_pwm` limits further. If horizontal scrolling steers in the wrong direction with your touchpad settings, tick 左右スクロールの向きを反転.

**Manual acceleration** (手動の加速; default 287 = 0 to 255 in about 0.89 s): the car raises the manual drive only at this rate (increase of the `RC_drive` value per second). Lowering it, stopping and braking happen at once, and a change between forward and reverse starts again from 0. Pressing the pad's edge or ↑ no longer puts out the maximum at once, which limits the current at a start. Depending on the motor and gears, starting from rest at a high output draws a large current that pulls the battery voltage down, and the core (the Raspberry Pi) restarts and the Wi-Fi drops (section 10); lower the value then. 0 = no limit (as up to v0.5.x: the command at once).

The **steering strength** is the share of the signal (not compensated), **not an angle**. The RumiCar steering has no sensor of its angle, so it cannot be told an angle and stop there as a servo does. How far a middle strength turns the wheels (or whether it moves them at all) depends on the car's steering mechanism (return spring, friction) and on the battery voltage. Tick パッドの横位置で操舵の強さを変える to try it on each car.

**Saving the drive settings** (v0.6.0 and later): the manual 最大出力, 手動の加速, パッドの横位置で操舵の強さを変える and 自動の最大出力 are sent to the car 0.3 s after they are changed, and the car saves them in `/var/lib/rumicar/settings/drive.json`. They stay after a restart of the core and apply whichever PC connects (cars differ in motor, gears and steering, so the car keeps its settings). The two maximum outputs never exceed the car's `max_drive_pwm`. Writing the image to the SD card again resets them to the defaults (9.1).

In manual mode the car makes an emergency stop when the heartbeat is lost for 1 s (table above).

Press 「切断」 to disconnect. In auto mode the car continues driving after the application is closed (it is designed to drive without a PC). To stop it, use the emergency stop or switch the car off.

### Priorities of the decisions in auto mode

What the car gives priority to in auto mode, highest first. When a row applies, the rows below it are not used (the values are the defaults of the bundled decision data `base-tof-0.3.0`; spec 3.7 gives the meaning of each key).

| Priority | Decision (shown as 判断状態) | Condition | Action |
|---|---|---|---|
| 1 | Emergency stop (core) | 「非常停止」 in the application; a lost heartbeat or a disconnected PC in manual mode | Brakes whatever the logic says; does not drive until cleared |
| 2 | Sensor fault (`SENSOR_FAULT`) | A ToF read failure (`-1`) lasting longer than `invalid_hold_ms` (300 ms) | Brakes; back to cruising when the readings recover (`-3 範囲外` is a normal value, not a fault) |
| 3 | Emergency brake (`EMERGENCY_BRAKE`) | Distance ahead (centre) < `d_emergency_mm` (100 mm) | Brakes at once and holds it for `brake_ms` (150 ms); then cruises if `d_min_mm` (150 mm) or more is free ahead, else reverses |
| 4 | Reversing out (`RECOVER_REVERSE`) | Still blocked ahead after the emergency brake, or **stuck** (driving forward at `stuck_min_drive` (77) or more with something nearer than `conf_max_mm` (200 mm) ahead, whose distance stays within `stuck_delta_mm` (15 mm) for `stuck_time_ms` (1.5 s)) | Reverses at `v_reverse` (179), steering away from the side the left / right ToF see open (the nose swings towards it; when they differ by 20 mm or less, against the direction steered last), at strength `recover_steer_pwm`. Cruises after `reverse_ms` (600 ms), or after at least 200 ms of reversing once `d_turn_exit_mm` (190 mm) is free ahead |
| 5 | Turning (`TURN_LEFT` / `TURN_RIGHT`) | Distance ahead < `d_turn_enter_mm` (170 mm); ends above `d_turn_exit_mm` (190 mm) | Steers towards the side whose ToF reads farther (strength `turn_steer_pwm`); when they differ by `side_deadband_mm` (20 mm) or less, the direction steered last. Speed at most `v_turn_cap` (179) |
| 6 | Avoiding a side wall (`TURN_LEFT` / `TURN_RIGHT`) | Not turning, and the left or right ToF < `d_side_mm` (60 mm) | Steers away from the near wall at `side_steer_pwm` (stronger the nearer the wall with `side_steer_pwm_min`, 6.2). Both near: towards the farther side (centre when they are about equal) |
| 7 | Cruising (`CRUISE`) | None of the above | Steering centred |

- In priorities 5–7 the speed follows the distance ahead: `v_min` (128) at `d_min_mm` (150 mm) or less, `v_max` (230) at `d_max_mm` (200 mm) or more (including `-3 範囲外`), linear in between; `v_min` when the time to collision from the closing speed is below `ttc_slow_s` (0.5 s). Acceleration and deceleration are limited to `accel_pwm_per_s` / `decel_pwm_per_s`.
- Once changed, the steering direction stays for `steer_hold_ms` (150 ms) (the strength follows at once).
- After the logic's command, the core applies 自動の最大出力, `max_drive_pwm` and the start ramp of auto mode (`auto_accel_pwm_per_s`) (Figure 10).
- The left and right ToF point ±65° from straight ahead. Driving parallel to a wall, about 0.9 times (sin 65°) the side reading is the lateral distance to the wall (`d_side_mm` 60 mm is about 54 mm sideways, `conf_max_mm` 200 mm about 181 mm).
- The image logic `cam_drive` adds the traffic-light, vehicle and intersection behaviours on top of these decisions (7.7). With `py_drive`, `drive.py` decides and only the core's safety control (11.3) is shared.

### How far the ToF readings are trusted

The laser of the VL53L0X spreads out in a cone (a field of view of about 25°), so, depending on where and how the sensor is mounted and on the height of the car, it picks up the floor beyond some distance. That distance differs from car to car, so the core and the library do not cut the values off at one fixed distance: **the driving logic decides how far a reading is trusted (the limit)**. Beyond the limit a reading counts as far (open).

| Driving logic | Where the limit is set | How the limit works |
|---|---|---|
| `tof_drive`, `cam_drive` | `conf_max_mm` in the decision data `params.json` (`tof.conf_max_mm` for `cam_drive`); 1–2000 | A reading **at or above** `conf_max_mm`, and −3, is open. One value for the three directions |
| `py_drive` (`drive.py`) | `RC_setReadLimit(limit)` in `setup()` (`RC_setReadLimit(direction, limit)` for one direction); 1–2000, 0 = no limit. The sample `py-sample` takes it from `read_limit_mm` in `params.json` (11.2) | For a reading **above** the limit `RC_read()` returns −3 (the limit itself is still a distance). Not called: no limit (up to 2000 mm) |
| A C++ logic of your own | The logic decides for itself (`car/include/rumicar/logic_api.h`) | The core passes 0–2000 mm, −1 and −3 on as they are |

**Defaults**: `conf_max_mm` of the bundled decision data (`base-tof`, `cam-base`) and `read_limit_mm` of the sample `py-sample` are both 200. That is where the core cut the readings off up to v0.6.0, so with the defaults the decisions are those of v0.6.0 (`test_tof_logic`, `test_cam_logic`, `test_py_drive` and `tools_smoke.py` check that the values with and without the cut-off lead to the same decisions).

**Choosing the limit**:

- The recommended example is 200 mm. Reason: on the RumiCar model 32 the floor starts to be picked up at roughly 30 cm (a consideration given in the hands-on text); 20 cm leaves a margin. Another chassis, height or sensor mounting changes that distance.
- To choose it for your own car, put the car on a flat floor with nothing within 1 m ahead and to the sides, and look at the ToF values in the drive manager (the core's values, which no limit affects). A direction that shows a number although nothing is there is picking up the floor (or something else) at that distance. Take a limit that is smaller, with a margin, than the smallest value shown in the three directions. If all three show `-3 範囲外`, the floor is not picked up in that state. The result depends on the floor's material and on the car pitching as it accelerates and brakes, so check on the course you drive on, and check the values while driving in a recording (`tof_mm` in `meta.jsonl`).
- The left and right sensors point 65° forward, so the distance at which they start to pick up the floor may differ from the centre one (not verified; check each direction as above). Only `RC_setReadLimit(direction, limit)` of `drive.py` sets a limit per direction; `conf_max_mm` is one value for the three, so go by the shortest direction.
- Keep the distance thresholds of the decisions at or below the limit: beyond it a reading only says "open". `tof_drive` and `cam_drive` refuse, at activation, decision data in which `d_max_mm`, `d_turn_exit_mm`, `vehicle.follow_gap_mm + follow_band_mm` and so on exceed `conf_max_mm`. In `drive.py` it is up to you (`stop_mm`, `turn_mm` and `side_mm` in the sample).
- Widening the limit (on a wide course where the walls are farther away than the limit, all three directions read open and the car does not start to steer): make decision data with a larger `conf_max_mm` and larger turning and speed thresholds (`d_turn_enter_mm`, `d_turn_exit_mm`, `d_min_mm`, `d_max_mm`) (6.2; give `--min-abi 5`). In `drive.py`, raise `read_limit_mm` and the thresholds.

**Moving from an earlier version**:

- Decision data (packs): a pack of v0.6.0 or earlier (`min_abi` 4) is used as it is. Its `conf_max_mm` is 200 or less, so it decides as on v0.6.0.
- `drive.py`: a program written for v0.6.0 or earlier runs, but `RC_read()` now also returns distances beyond 200 mm. Adding the line `RC_setReadLimit(200)` after `RC_setup()` in `setup()` gives it the values it got up to v0.6.0 (−3 beyond 200 mm).
- A C++ logic of your own: the interface is ABI 5 now, so it must be rebuilt (the core does not load a logic whose ABI differs from its own). It receives values beyond 200 mm and has to decide how far it trusts them.
- Recordings: recordings of v0.6.0 or earlier (ABI 3 and 4) are used for learning and replay as they are. Their ToF values were cut off at 200 mm by the car, however, so check decision data whose `conf_max_mm` is larger than 200 with recordings of core 0.7.0 or later.

## 6. Recording driving data and improving the decision data

### 6.1 Recording

On connection (or with 「収録開始」 when automatic recording is off), one folder per recording is created under the destination:

```
RumiCarData/
  sessions/
    RumiCar_20261018-123456/      <car name>_<start time, UTC>
      frames/000001.jpg …        images (sequence number within the recording)
      meta.jsonl                 one line per frame: {"index":1,"image":"000001.jpg","received":"…","header":{…status received from the car…}}
      session.json               summary: scenario, note, the logic/pack the car used at the start (with description, capabilities, source scenarios), frame and image counts, number of gaps, whether under-voltage occurred
  scenarios.json                 scenario names used so far (newest first, with use counts)
```

`header` contains, for that frame, the ToF values, the logic state and decision (steer with its strength, drive), the values actually output to the motors, the drive mode, the e-stop state, the logic and pack names and versions (`logic_name` / `pack_id`), the power flags (`throttled`), and the image's sequence number and timestamp.

**Naming scenarios**: before recording, enter the name of the scene in the シナリオ box — "壁のみの走行データ" (walls only), "信号機ありケース1" (traffic light, case 1), "信号機と交差点ロジックあり" (traffic light and intersection) — so that the recordings to use can be selected when building decision data. Names used before can be selected from `scenarios.json`.

### 6.2 Improving the data with the Claude desktop app

1. Open the `RumiCarData` folder (or its `sessions` folder) in the Claude desktop app, let it read the recordings and instruct the improvements.
2. The decision data of the driving logic `tof_drive` is a single parameter file, `params.json`. Spec section 3.7 lists every key with its meaning and default (for example `d_turn_enter_mm`, the forward distance at which a turn starts; `v_max`, the top speed as an `RC_drive` value 0–255; `accel_pwm_per_s`, the rate of change when accelerating). Distances are `RC_read` values (0–2000 mm; a value at or above `conf_max_mm`, and −3, is open), and the distance thresholds stay at or below `conf_max_mm` (section 5, "How far the ToF readings are trusted"). The steering strength (the second argument of `RC_steer`, 0–255) is set per situation: `turn_steer_pwm` (turning away from an obstacle ahead), `side_steer_pwm` (moving away from a side wall) and `recover_steer_pwm` (reversing out); the bundled value is 255 = full. With `side_steer_pwm_min` (`tof_drive` 0.5.0 and later), the strength of moving away from a side wall is `side_steer_pwm_min` with the wall at `d_side_mm` and `side_steer_pwm` at 0 mm, proportional to the distance in between (the nearer, the stronger; left out, `side_steer_pwm` at every distance). The priorities of the decisions are in section 5, "Priorities of the decisions in auto mode". The strength is the share of the signal without compensation, not an angle, and the same value steers differently on different cars, so judge its effect from `steer_pwm` / `logic_steer_pwm` in the recordings together with the change of the images and the ToF values, and adjust it by rough steps such as "1.3 times when it does not turn enough, half when it overshoots".
3. Put the result in a folder with this structure (`manifest.json` can be created with `car/tools/make_manifest.py`, which needs only the Python 3 standard library):

```
base-tof-0.3.1/            folder name is <id>-<version>
  params.json              every key is required (the four steering strengths may be left out: 255; side_steer_pwm_min as side_steer_pwm)
  manifest.json            python car/tools/make_manifest.py pack base-tof-0.3.1 --id base-tof --version 0.3.1 --logic tof_drive --min-abi 4 \
                               --description "Walls only; turns start earlier (d_turn_enter_mm 180)" \
                               --capability walls --scenario "壁のみの走行データ" --parent base-tof-0.3.0
```

Give the version (`0.3.1` etc.) a new number that does not duplicate a version installed on the car. `--min-abi 4` says the pack needs a logic that reads the steering strengths (ABI 4, core 0.5.0 or later). Give `--min-abi 5` when `conf_max_mm` or another distance exceeds 200 mm (core 0.7.0 or later: an earlier core cuts the ToF readings off at 200 mm, and its logics refuse a distance above 200).

`--description` (description of the version), `--capability` (supported scenes: `walls`, `traffic_light`, `vehicle`, `intersection`; repeatable), `--scenario` (the recorded scenarios used; repeatable) and `--parent` (the version it was derived from) are optional; when given, the drive manager shows them in 判断データ / 対応機能 / 学習元 and in the version list, which identifies the logic and data in use. `python car/tools/make_manifest.py show <folder>` displays them.

### 6.3 Sending to the car and switching

1. With the drive manager connected, press **「送る…」** in 版の管理 and select the folder. The application verifies the manifest before sending.
2. When the new version appears in the list, select it and press **「有効化」** (activate). The car verifies it and switches at the next frame (also while driving). If verification fails, the car continues with the current version and the reason is logged.
3. If there is a problem, **「データを戻す」** returns to the previous pack. Versions no longer needed can be selected and deleted with 「削除」 (the active and the previous version cannot be deleted).

A rebuilt driving logic (the `tof_drive` `.so`) is sent the same way (`make_manifest.py logic …`, spec section 4).

### 6.4 Creating a video from a recording

「動画を作る…」 in the drive manager takes a session folder and creates `video.avi` from its `frames/` (Motion JPEG: the received JPEGs are stored without re-encoding, so there is no quality loss; 20 frames/s on a real-time timeline from the image timestamps; where frames are missing the previous frame is held for up to 1 s, and longer gaps are removed). VLC, Windows Media Player and ffmpeg can play it. From the Claude desktop app use `python car/tools/session_video.py <session folder>`, which performs the same processing (Python 3 standard library only). To convert to MP4: `ffmpeg -i video.avi -c:v libx264 -pix_fmt yuv420p video.mp4`.

## 7. Learning from images (traffic lights, a vehicle ahead, intersections)

### 7.1 How it works

The car keeps the ToF (distance sensor) driving as its base. In addition, each camera image is converted into features (320×240 reduced to 1/5 and divided into 8×6 cells; per cell, the pixel counts of 8 colour classes (6 hues plus bright and dark) and the sums of value, saturation and value gradients (horizontal, vertical) — 576 integers), to which **detectors learned from the recordings** are applied. There are two kinds of detector: **local detectors**, which output a position in the image (red light, green light, vehicle; evaluated on a 3×3 window of cells), and **global detectors**, which output a state of the whole image (an intersection ahead, etc.). Based on the detection results, three behaviours give the ToF driving a "speed cap" and a "steering mode".

**Figure 4** Feature extraction and detectors

<!-- figure: en/04-features.mmd -->
```mermaid
flowchart TD
    I["Camera image RGB24<br/>320×240"] --> R["Reduce to 1/5 → 64×48<br/>(mean of 5×5 pixels)"]
    R --> C["Pixel classes: 6 hues, bright, dark<br/>value V, saturation S, value gradients (horizontal, vertical)"]
    C --> G["8×6 cells (8×8 pixels each)<br/>12 values per cell: pixel counts of 8 classes, ΣV, ΣS, Σ|dx|, Σ|dy|"]
    G --> F["576 features (integers) → standardised z"]
    F --> L["Local detector<br/>each cell with its 3×3 neighbourhood (108 values)<br/>→ score per cell p = σ(w·z + b)"]
    F --> GL["Global detector<br/>576 values → score p = σ(w·z + b)"]
    L --> T["Temporal confirmation<br/>p ≥ thr_on (local: also cells ≥ min_cells) for confirm_frames → confirmed<br/>p < thr_off for release_frames → released"]
    GL --> T
    T --> B["To the behaviours (traffic light, vehicle, intersection)"]
```

| Behaviour | Detectors | Action |
|---|---|---|
| `signal` | red / green light (local) | When the red light is detected for the configured number of frames: decelerate; when close: stop. When the green light is detected (or a configured time has passed since the red light was last detected): start |
| `vehicle` | vehicle (local) | When a vehicle is detected in the own lane (the centre columns of the image): follow at a gap without turning. If the gap stays unchanged while stopped, classify it as a parked vehicle and overtake on the side with more clearance on the left/right ToF (lane change to the passing side → alongside → return to the lane) |
| `intersection` | intersection with a road to the left / right (global) | When an intersection is detected: decelerate, stop where a left/right ToF (which looks 65° forward) catches the end of the wall (reads open), i.e. with the car's front just before the corner, then go straight, turn left or turn right as configured |

The three behaviours operate independently; where they overlap, the **lowest speed cap applies**. If a red light is detected during overtaking or turning, that manoeuvre is aborted and the car stops. The ToF logic's emergency brake, reversing recovery and sensor fault always take precedence (the image-based decisions only add constraints to the ToF driving and never disable its safe-side actions).

The learned result is a "decision data pack": `params.json` (ToF and behaviour parameters, text format), `vision.bin` (detector weights etc., a binary format the car can load quickly) and `manifest.json` (description, capabilities, source scenarios). It is sent to the car and switched exactly as in section 6. The driving logic `cam_drive` is bundled with the car (installed alongside `tof_drive`). A behaviour whose detectors are absent is disabled automatically, so functions can be added step by step: traffic lights first, then vehicles.

### 7.2 Required environment

- Python 3 with numpy and Pillow (`pip install numpy pillow`), in the Claude desktop app's environment or on the PC
- `car/tools/` from this repository (`train_pack.py`, `rumicar_vision.py`, `make_manifest.py`, `session_video.py`)
- Optional: `rumicar-replay` (GitHub Actions artifact `rumicar-replay-amd64`), which replays a recording through the actual logic so that stop/start decisions can be checked before sending

### 7.3 Procedure

**Figure 5** From learning to the car

<!-- figure: en/05-learning.mmd -->
```mermaid
flowchart TD
    A["1. Record (with a scenario name)<br/>for learning, for validation, walls only (negatives)"] --> B["2. Propose candidates<br/>train_pack.py propose"]
    B --> C["3. Label (Claude checks the candidates)<br/>labels.jsonl"]
    C --> D["4. Learn<br/>train_pack.py train<br/>→ params.json, vision.bin, training.json, manifest.json"]
    D --> E{"5. Check before sending<br/>show, replay, rumicar-replay"}
    E -->|"Low recall / many false detections"| X["Add labels / negatives"]
    X --> D
    E -->|"OK"| F["6. 送る… (send) → 有効化 (activate) in the drive manager"]
    F --> G["7. Drive and record on the real car"]
    G -->|"New scenes added:<br/>re-learn all detectors"| A
```

1. **Record**: drive a course with a traffic light (to capture red and green before starting, the car may be moved by hand in record-only mode) with a scenario name such as "信号機ありケース1". Recordings of a course without traffic lights ("壁のみの走行データ") are used automatically as "no traffic light in view" examples (negatives), so prepare those as well. Make at least two separate recordings, one for learning and one for validation.
2. **Label**: `python car/tools/train_pack.py propose <session> --detector traffic_light_red` writes candidate red regions (frame index and box) to `proposals_traffic_light_red.jsonl`. Let Claude read that folder, check the candidate frames (images in `frames/`) and write the correct ones into `labels.jsonl`. Likewise for the green light (`--detector traffic_light_green`). Candidate lines have the same format as `labels.jsonl` lines, and extra keys (`image`, `pixels`, etc., which record why the line was proposed) are ignored, so the lines can be used after deleting the unwanted ones. The format is as follows (one object per line; boxes are pixel coordinates [x0, y0, x1, y1] with x1/y1 exclusive):

```
{"index": 123, "objects": [{"name": "traffic_light_red", "box": [160, 60, 200, 110]}]}   <- local (box)
{"index": 456, "objects": [{"name": "vehicle", "box": [120, 140, 200, 220]}]}            <- vehicle ahead (enclose its body)
{"from": 300, "to": 380, "scene": ["intersection_left"]}                                 <- scene (whole image): frames 300..380 show an intersection ahead branching to the left
{"index": 200, "negative": true}                                                         <- explicitly no target in view
```

   `--detector vehicle` proposes vehicle candidates (frames in which the centre ToF reading was below `--near-mm`; the box only marks the lane position, so adjust it to the vehicle's body). `--detector intersection_left` (or `_right`) proposes intersection candidates: counting back from the moment a left/right ToF reading (65° forward) changes from "wall" (below `--near-mm`) to "open" (at or above `--near-mm`, or −3) — the moment the car's front reaches the corner of the intersection — the preceding 2 s (`--before-ms`) are proposed as "intersection visible ahead"; check the first and last frames and adjust the interval. The default of `--near-mm` is the `conf_max_mm` of the bundled decision data; give the pack's own value when you make a pack with another `conf_max_mm`. Unlabelled frames are not used for learning. Checking about one frame in twenty is sufficient (frames in intervals with similar features are filled in by the `propose` candidates). A labelled frame becomes a negative example for every detector not named on it (this is how the red tail lights of a vehicle are learned not to be a red light).

3. **Learn**:

```
python car/tools/train_pack.py train cam-signal-0.1.0 \
    --parent <the cam-base-0.3.0 folder taken from the car, or the previous version> \
    --sessions <recordings for learning>... --validate <recordings for validation>... \
    --negatives-from-scenario "壁のみの走行データ" \
    --detector traffic_light_red --detector traffic_light_green \
    --id cam-signal --version 0.1.0 --description "traffic light, case 1"
```

   `cam-base-0.3.0` is also in the repository at `car/packs/cam-base`. Recordings of core 0.7.0 (ABI 5), of 0.5.x / 0.6.x (ABI 4) and of 0.4.x (ABI 3) can all be used (the images are the same; in recordings of ABI 3 and 4 the car cut the ToF values off at 200 mm, beyond which they are −3). The `min_abi` of the pack written is 4 (usable from core 0.5.0) when every distance in its `params.json` is 200 mm or less, and 5 (core 0.7.0 or later) when `conf_max_mm` or another distance exceeds 200 mm (`train` prints it). On completion `cam-signal-0.1.0/` contains `params.json`, `vision.bin`, `training.json` (learning record) and `manifest.json`, and the precision and recall of the detections on the validation recordings are displayed. If recall is low, add labels; if there are many false detections, add negatives (walls only). To add vehicles and intersections, learn all detectors together (`:global` marks a global detector):

```
python car/tools/train_pack.py train cam-town-0.1.0 --parent cam-signal-0.1.0 \
    --sessions <traffic-light recordings> <vehicle and intersection recordings>... --validate <validation recordings>... \
    --negatives-from-scenario "壁のみの走行データ" \
    --detector traffic_light_red --detector traffic_light_green \
    --detector vehicle --detector intersection_left:global --detector intersection_right:global \
    --intersection turn=left --id cam-town --version 0.1.0 --description "traffic lights and intersections"
```

   A behaviour's settings are added to `params.json` only when the pack contains its detectors (`behaviours on: signal, vehicle, intersection` is printed). If only one side of intersections is learned, the other detector name is left empty (no intersection on that side).

4. **Check before sending**: `python car/tools/train_pack.py show cam-signal-0.1.0` (contents and enabled behaviours), `replay cam-signal-0.1.0 <session>` (per-frame detections) and `rumicar-replay <cam_drive folder> cam-signal-0.1.0 <session>` (replay through the actual logic; a transition CRUISE → SIGNAL_APPROACH → SIGNAL_STOP → SIGNAL_RESUME means stopping and starting work; for a vehicle FOLLOW → OVERTAKE_OUT → OVERTAKE_PASS → OVERTAKE_IN, for an intersection INTERSECTION_APPROACH → INTERSECTION_STOP → (INTERSECTION_CROSS →) INTERSECTION_TURN). A replay feeds in the recorded ToF values unchanged, so scenes in which the car's own motion would change the ToF values (overtaking) only progress as far as the recording does (stopping, deceleration and the start of the lane change can be checked).
5. **Send to the car**: in the drive manager's 版の管理, select `cam_drive-0.4.0` (bundled) and 有効化, then 送る… `cam-signal-0.1.0` and 有効化. The status panel's 判断データ / 対応機能 / 学習元 show the sent version's description, "壁 / 信号機" and the scenarios used for learning. With 「判断を重ねる」 on, detection boxes (red light = red, green light = green, vehicle = sky blue, intersection = orange frame around the whole image; thick when confirmed) and scores are drawn over the image. 判断状態 also shows the meaning of the state, e.g. `FOLLOW（前方車両に追従）`.
6. **Improve**: add recordings and learn again with `--parent cam-signal-0.1.0 --version 0.2.0`. When recordings of new scenes (vehicles, intersections) are added, the rule is to **re-learn all detectors**, including the existing ones (the new scenes become negatives for the existing detectors, e.g. a vehicle's tail lights are not falsely detected as a red light). `--keep-parent-detectors` is for correcting some detectors without adding scenes (the lineage is kept in the manifest's `parent`).

### 7.4 Traffic-light behaviour (the `signal` part of params.json)

| Key | Default | Meaning |
|---|---|---|
| `red_detector` / `green_detector` | `traffic_light_red` / `traffic_light_green` | Detector names. On a course without a green light set `green_detector` to `""` and start with `resume_without_green_ms` |
| `stop_min_cells` | 2 | When the red light is detected in at least this many cells it is judged "close" and the car stops (before that it only decelerates) |
| `approach_v` | 128 | Speed cap (`RC_drive` value 0–255) from detecting the red light until stopping |
| `min_stop_ms` | 1000 | Minimum stop time |
| `resume_without_green_ms` | 3000 | Time from the red light no longer being detected until starting (0 = wait until the green light is detected) |
| `resume_ignore_ms` | 1500 | Time after starting during which red-light detections are ignored (to avoid stopping again while passing) |

Override with `--signal key=value` of `train` (e.g. `--signal stop_min_cells=3`).

**Figure 6** State transitions of the traffic-light behaviour

<!-- figure: en/06-signal.mmd -->
```mermaid
stateDiagram-v2
    state "Normal (tof_drive states)" as NORMAL
    [*] --> NORMAL
    NORMAL --> SIGNAL_APPROACH: red light confirmed<br/>(cells < stop_min_cells)
    NORMAL --> SIGNAL_STOP: red light confirmed<br/>(cells ≥ stop_min_cells)
    SIGNAL_APPROACH --> SIGNAL_STOP: cells ≥ stop_min_cells
    SIGNAL_APPROACH --> NORMAL: red light released
    SIGNAL_STOP --> SIGNAL_RESUME: after min_stop_ms, green light confirmed<br/>or resume_without_green_ms elapsed<br/>since the red light was released
    SIGNAL_RESUME --> NORMAL: resume_ignore_ms elapsed
    note right of SIGNAL_APPROACH: speed cap approach_v
    note right of SIGNAL_STOP: speed cap 0 (brake held once stopped)
    note right of SIGNAL_RESUME: red-light detections ignored
```

### 7.5 Vehicle behaviour (the `vehicle` part of params.json)

If this section (`vehicle`) is absent from `params.json` the behaviour is disabled. If present, every key is required (`train` adds it automatically to packs that contain the `vehicle` detector; override with `--vehicle key=value`). All distances are values after `tof_drive`'s processing; readings at or above `tof.conf_max_mm` and −3 (out of range) are treated as "open". A distance may be set up to 2000, but a value at or above `tof.conf_max_mm` only tells open from not open (section 5, "How far the ToF readings are trusted").

| Key | Default | Meaning |
|---|---|---|
| `detector` | `vehicle` | Name of the local detector |
| `lane_cols` | 4 | The detected vehicle is judged to be in the own lane only when its best cell (the cell with the highest score) lies in this many centre columns of the grid (columns 2–5 of 8) |
| `follow_gap_mm` | 150 | Minimum gap (centre ToF) to the vehicle ahead; at or below it the speed cap is 0 (stop). Must be greater than `tof.d_emergency_mm` and less than `tof.conf_max_mm` |
| `follow_band_mm` | 40 | While the gap widens from `follow_gap_mm` by this amount, the cap rises linearly from 0 to `follow_v` (`follow_gap_mm` + `follow_band_mm` must not exceed `tof.conf_max_mm`: beyond it a reading counts as open) |
| `follow_v` | 179 | Speed cap while following (`RC_drive` value 0–255); also applies while the vehicle is detected only by the camera and not yet by the ToF |
| `parked_confirm_ms` | 2000 | When the car has been stopped behind the vehicle for this long without the gap widening, the vehicle is classified as parked |
| `still_delta_mm` | 20 | If the gap widens by more than this while waiting, the vehicle is judged to have started moving and the judgement restarts |
| `overtake_side` | `open` | Side to overtake on: `left`, `right` or `open` (the side with the larger left/right ToF distance; equal → right) |
| `side_clear_mm` | 120 | Overtaking is possible if that side's ToF reads at least this (or open); otherwise wait in `FOLLOW_WAIT` |
| `overtake_v` | 128 | Speed cap during overtaking |
| `out_ms` | 500 | Duration of the lane change to the passing side (maximum steering towards the passing side) |
| `pass_min_ms` / `pass_max_ms` | 800 / 3000 | Minimum / maximum duration of driving alongside (steering back to neutral, straight ahead) |
| `pass_clear_mm` / `pass_clear_ms` | 150 / 700 | After the inner (vehicle-side) ToF has read below this once (vehicle detected), a reading at or above it (or open) for this long means passing is complete. The left/right ToFs look 65° forward, so they lose the vehicle when the car's front passes the vehicle's front; this time lets the rest of the car pass it (provisional; check and tune on the real car) |
| `in_ms` | 500 | Duration of the return to the lane (maximum steering to the opposite side) |
| `ignore_ms` | 1500 | Time after overtaking during which vehicle detections are ignored. Also, if the camera does not detect the vehicle for this long while following, following ends and control returns to the ToF decisions (if the car is too close for the vehicle to be detected, the ToF logic avoids it as an obstacle) |

Overtaking is open-loop control divided by time (there is no sensor of the steering angle, and the lane change and the return steer at full lock, strength 255; the steering strengths of the `tof` section do not apply to them). Adjust the lane-change and return durations to the course and the speed.

**Figure 7** State transitions of the vehicle behaviour

<!-- figure: en/07-vehicle.mmd -->
```mermaid
stateDiagram-v2
    state "Normal" as NORMAL
    [*] --> NORMAL
    NORMAL --> FOLLOW: vehicle confirmed in the own lane<br/>(after the ignore time ignore_ms)
    FOLLOW --> NORMAL: vehicle not detected and ToF open<br/>or not detected for ignore_ms
    FOLLOW --> OVERTAKE_OUT: classified as parked<br/>(stopped, gap unchanged for parked_confirm_ms)<br/>and passing-side ToF ≥ side_clear_mm
    FOLLOW --> FOLLOW_WAIT: classified as parked<br/>no side to pass on
    FOLLOW_WAIT --> FOLLOW: gap widened by more than still_delta_mm
    FOLLOW_WAIT --> OVERTAKE_OUT: passing-side ToF ≥ side_clear_mm
    FOLLOW_WAIT --> NORMAL: vehicle not detected and ToF open<br/>or not detected for ignore_ms
    OVERTAKE_OUT --> OVERTAKE_PASS: out_ms elapsed
    OVERTAKE_PASS --> OVERTAKE_IN: passed (inner ToF below pass_clear_mm, then<br/>at or above it for pass_clear_ms, and ≥ pass_min_ms)<br/>or pass_max_ms elapsed
    OVERTAKE_IN --> NORMAL: in_ms elapsed
    note right of OVERTAKE_PASS
        During OVERTAKE_OUT / PASS / IN, a red light or an intersection being handled,
        or the ToF logic's emergency brake, reversing or sensor fault,
        aborts the manoeuvre (back to Normal).
        After returning to Normal, vehicle detections are ignored for ignore_ms
    end note
```

### 7.6 Intersection behaviour (the `intersection` part of params.json)

If this section (`intersection`) is absent the behaviour is disabled. If present, every key is required (`train` adds it automatically to packs that contain `intersection_left` or `intersection_right`; override with `--intersection key=value`).

| Key | Default | Meaning |
|---|---|---|
| `left_detector` / `right_detector` | `intersection_left` / `intersection_right` | Detectors (global) for an intersection branching to the left / right. One may be `""` (not both) |
| `approach_v` | 128 | Speed cap from detecting the intersection until stopping |
| `stop_trigger` | `side_open` | Stop condition. `side_open` = when the left or right ToF reads `open_mm` or more, or open (the wall ends); `detector` = immediately when the detector is confirmed (courses without walls, where only images just before the intersection were learned) |
| `open_mm` | 200 | A left/right ToF reading (65° forward) at or above this, or open, is judged "open" (1–2000; at or above `tof.conf_max_mm` only an open reading — at or above `tof.conf_max_mm`, or −3 — counts. The default equals the default of `tof.conf_max_mm`) |
| `stop_ms` | 1000 | Stop time (while a red light is detected, the car does not start until the light turns green) |
| `turn` | `straight` | Path after stopping: `straight`, `left`, `right`, or `detector` (turn towards the detected side; both sides detected → straight) |
| `turn_v` | 128 | Speed cap while driving straight waiting for the turn side to open, and while turning |
| `cross_max_ms` | 2000 | Maximum time to drive straight waiting for the turn side's ToF to become open (if it does not, the manoeuvre ends going straight) |
| `turn_ms` | 900 | Duration of maximum steering towards the turn side |
| `ignore_ms` | 3000 | Time after completion during which intersection detections are ignored |

**Figure 8** State transitions of the intersection behaviour

<!-- figure: en/08-intersection.mmd -->
```mermaid
stateDiagram-v2
    state "Normal" as NORMAL
    [*] --> NORMAL
    NORMAL --> INTERSECTION_APPROACH: intersection detector confirmed<br/>(stop_trigger = side_open)
    NORMAL --> INTERSECTION_STOP: intersection detector confirmed<br/>(stop_trigger = detector)
    INTERSECTION_APPROACH --> NORMAL: confirmation released
    INTERSECTION_APPROACH --> INTERSECTION_STOP: left or right ToF ≥ open_mm
    INTERSECTION_STOP --> NORMAL: stop_ms elapsed, no red light<br/>path straight
    INTERSECTION_STOP --> INTERSECTION_CROSS: stop_ms elapsed, no red light<br/>path left / right
    INTERSECTION_CROSS --> INTERSECTION_TURN: turn-side ToF ≥ open_mm
    INTERSECTION_CROSS --> NORMAL: cross_max_ms elapsed (ends straight)<br/>or red light / ToF emergency action
    INTERSECTION_TURN --> NORMAL: turn_ms elapsed<br/>or red light / ToF emergency action
    note right of INTERSECTION_STOP
        The path is decided on stopping: turn = straight / left / right / detector
        (detector: the detected side; both sides → straight).
        APPROACH does not start during overtaking.
        After returning to Normal from STOP, CROSS or TURN, intersection detections are ignored for ignore_ms
    end note
```

### 7.7 How the behaviours combine

- The speed cap is the minimum over the three behaviours. A red light detected while following gives `SIGNAL_STOP`; reaching an intersection gives `INTERSECTION_STOP`.
- The parked-vehicle judgement does not progress while a red light or an intersection is being handled (so that a vehicle waiting at a light is not overtaken). If a red light or an intersection is detected during overtaking, the manoeuvre is aborted and the car stops; following then restarts.
- At an intersection stop the car does not start until the red light is no longer detected (green), and a red light detected while crossing or turning aborts the manoeuvre.
- The state shown as 判断状態 is that of the behaviour deciding speed and steering, in the priority order red light (`SIGNAL_APPROACH` / `SIGNAL_STOP`) → intersection → vehicle → starting (`SIGNAL_RESUME`). During the ToF logic's emergency brake, reversing or sensor fault, that state is shown.

### 7.8 Troubleshooting

| Symptom | Action |
|---|---|
| Stops for objects other than red lights | Add walls-only recordings, or recordings containing that object, as negatives. Check the precision in `training.json`. Raise `--thr-on` |
| Stop position too far from / too close to the light | Increase / decrease `stop_min_cells`. Check that the camera is mounted at the same angle as during recording |
| Does not start after stopping | Check that the green detector detects the green light (検出 field). If not, set `resume_without_green_ms` |
| Avoids the vehicle instead of stopping behind it | With 判断を重ねる on, check that the vehicle's box appears in the centre columns (`lane_cols`). If detection is lost when the vehicle body fills the view at close range, add labels from close-range recordings. Raise `follow_gap_mm` |
| Contact or wide swing during overtaking | Adjust `out_ms`, `in_ms`, `overtake_v`. If the wall on the passing side is close, raise `side_clear_mm` (the car then waits) |
| Does not overtake the parked vehicle | `FOLLOW_WAIT` means both sides are below `side_clear_mm`; `FOLLOW` means the vehicle detection is lost (row above) or a red light / intersection is being handled |
| Does not stop at the intersection | Check with `replay` that `intersection_*` is confirmed. If it is and the course has walls, check that `open_mm` is less than the distance to the wall. On courses without walls set `stop_trigger` to `detector` |
| Does not turn at the intersection | The turn side's ToF did not become open (`open_mm` or more) within `cross_max_ms`. With `turn=detector`, detection on both sides means straight |
| The sent version shows 検証失敗 | Run `make_manifest.py verify`. If the vision.bin self-test does not match, the camera setting (320×240) differs from the one used for learning. Check that the learning tool and the car versions correspond (a pack with `min_abi` 4 needs core 0.5.0 or later, one with `min_abi` 5 core 0.7.0 or later). When a distance is outside its range the log gives the reason, e.g. `must be 1..2000` |
| `rumicar-replay` and the car give different results | Recordings are JPEG while the car uses uncompressed images, so small differences occur. Large differences suggest changed lighting; add recordings under that lighting |
| A detection stays confirmed and is never released | In `show`, check that the second threshold (release) is above the score when no target is present. `train` places it above the top 1 % of negative scores; re-learning with a smaller `--hysteresis` raises it |

## 8. Logging in to the car (SSH)

```sh
ssh rumicar@10.42.0.1          # password rumicar
sudo journalctl -u rumicar-core -f     # core log (state transitions, switches, errors)
systemctl status rumicar-core          # service status
rumicar-activate show                  # active / previous versions and the last switch result
sudo nano /etc/rumicar/core.json       # settings (then sudo systemctl restart rumicar-core)
```

Main settings in `/etc/rumicar/core.json` (spec 3.3): `car_name`, `startup_mode` (start-up drive mode: `record_only`, the default, or `auto` to drive at power-on), `max_drive_pwm` (drive limit as an `RC_drive` value, 255 = 100 %), `gpio.steer_pwm_frequency_hz` (frequency of the software PWM that outputs a steering strength of 1–254; default 500 Hz), `auto_accel_pwm_per_s` (rate at which the permitted drive increases on entering auto mode; the default 287 takes about 0.89 s from 0 to 255; 0 = no limit), `pwm.kick_pwm` / `pwm.kick_ms` (the kick that overcomes static friction when starting from rest or reversing; default 200 for 20 ms; never above the mode's maximum output; 0 = no kick), `camera.enabled`, `camera.on_demand` (run the camera only while a PC is connected or in auto mode; default true), `camera.fps`, `network.heartbeat_timeout_ms`, `network.ap_start` (`core`, the default: the core starts the access point; `boot`: NetworkManager does it), `network.wifi_txpower_dbm` (upper limit of the radio's transmit power; 0 = driver default; try about 10–14 if battery voltage drops are a problem), `power.throttled_path` (under-voltage / throttling monitor; an empty string disables it). The manual and auto maximum outputs and the manual acceleration are not in `core.json`: they are set from the application and saved by the car in `/var/lib/rumicar/settings/drive.json` (section 5, "Saving the drive settings").

## 9. Updating

### 9.1 Whole image

Write the image of a newer Release to the SD card. "SD カードを作る…" in the drive manager (section 5) downloads and verifies the image and opens Imager for the chosen version (the manual steps of section 2 do the same). Recordings are stored on the PC and are not lost. Packs and programs sent to the car, the drive settings (maximum outputs, manual acceleration and so on) and changes to `core.json` or the access point are gone — the card is back to what the Release bundles — so send what you need again from the PC afterwards.

### 9.2 Package only

Transfer `rumicar-zero2cam_<version>_arm64.deb` from the Release to the car and install it (if installed while driving, the motors stop once as the core restarts).

```sh
scp rumicar-zero2cam_<version>_arm64.deb rumicar@10.42.0.1:
ssh rumicar@10.42.0.1
sudo apt install ./rumicar-zero2cam_<version>_arm64.deb
```

The required libraries are included in the image, so no Internet connection is needed. If `/etc/rumicar/core.json` or the access-point profile was modified, apt asks whether to replace it (unmodified files are updated without asking). Bundled logic / pack versions are not overwritten if a directory of the same version already exists on the car (only new versions are added). The drive settings (section 5) are kept.

### 9.3 When the car needs an Internet connection

The car normally operates as an access point and does not connect to external networks. To install packages with `apt`, temporarily switch to the home Wi-Fi (this stops the `RumiCar` access point; a reboot restores it):

```sh
sudo nmcli device wifi connect "<home SSID>" password "<password>"
```

### 9.4 Version numbering

There are three kinds of version number, each updated independently.

| Kind | Example | Scope and where it appears |
|---|---|---|
| Core (product) version | `v0.7.0` | The Release tag. The SD-card image, the `.deb`, the drive manager and the car's `core_version` share it. Shown as `本体 <version>, このアプリ <version>` in the log on connection, and in the application's title |
| Logic version | `tof_drive-0.6.0`, `cam_drive-0.6.0` | One number per logic (`TOF_DRIVE_VERSION` / `CAM_DRIVE_VERSION` / `PY_DRIVE_VERSION` in `car/CMakeLists.txt`, `version` in its manifest). Shown in the version list and in the status panel's 版（ロジック／データ） |
| Pack (decision data) version | `base-tof-0.3.0`, `cam-base-0.3.0`, your own `cam-signal-0.1.0` | `version` in each pack's `manifest.json`. Give a pack you learned yourself a number that does not duplicate a version installed on the car (section 7) |

The core 0.7.0 Release bundles `tof_drive-0.6.0`, `cam_drive-0.6.0`, `py_drive-0.4.0` (with RumiCar.py 0.7.0), `base-tof-0.3.0`, `cam-base-0.3.0` and `py-sample-0.4.0` (`base-tof` and `cam-base` are unchanged, hence the same versions as in 0.6.0). Even when the core and a logic carry the same number, the two numbers are independent.

The car's core and the drive manager connect only when their control protocol versions match. Core 0.6.0 / 0.7.0 and application 0.6.0 / 0.7.0 speak protocol 3 (the steering strength in manual operation and the saved drive settings) and cannot be combined with a 0.4.x or 0.5.x core or application (protocol 2); the reason is logged when they cannot connect. A core 0.6.0 and an application 0.7.0 share the protocol and do connect, but the core 0.6.0 (ABI 4) sends its ToF values cut off at 200 mm, which the application logs on connection; and a pack made by the application 0.7.0's 「自分のプログラムを送る…」 (`min_abi` 5) cannot be activated on a core 0.6.0. Use the SD-card image (or `.deb`) and the application of the same Release.

**Builds that are not a release** (the `.deb`, SD-card image and application made by GitHub Actions for anything other than a tag) carry `+g` and the first 7 characters of the commit after the number (e.g. `0.2.1+g1a2b3c4`), to distinguish them from tagged releases. Debian orders them `0.2.1` < `0.2.1+g1a2b3c4` < `0.2.2`. If the numeric parts (before the `+`) of the car's core version and the application version differ, a warning is logged on connection.

## 10. Troubleshooting

| Symptom | What to check |
|---|---|
| `RumiCar` does not appear in the Wi-Fi list | Wait 1–2 minutes after boot. If SSH is available, check that `nmcli connection show` lists `RumiCar-AP`, that `sudo journalctl -u NetworkManager -b` shows no errors, and that `iw reg get` reports `JP` |
| The application cannot connect | Is the PC connected to `RumiCar` with a `10.42.0.x` address (`ipconfig`)? Firewall permission. On the car: `systemctl status rumicar-core` |
| No image | On the car: `sudo journalctl -u rumicar-core -b \| grep -i camera`. Camera ribbon orientation. Is the camera listed by `rpicam-hello --list-cameras`? |
| The car does not drive | After boot it waits in record-only mode (press 「自動」 in the application; to drive without a PC set `startup_mode` to `auto`). Does `sudo journalctl -u rumicar-core -b` begin with `hardware bring-up failed`? (The I2C/PWM settings are appended to `/boot/firmware/config.txt`.) Is the e-stop active (「非常停止解除」 in the application)? If a ToF reading stays `-1 タイムアウト` (read failure) for longer than `invalid_hold_ms` (default 300 ms), the car stops in SENSOR_FAULT (`-3 範囲外` is a normal value) |
| Power cuts out or the car reboots during start-up (dry cells) | Since v0.1.3 the core starts sensors → Wi-Fi → camera in sequence and keeps the motors stopped until 「自動」 is selected, so that high-current functions (Wi-Fi, camera, motors) do not operate simultaneously at start-up. If it still occurs: replace the cells with fresh or rechargeable (NiMH) ones; reduce the transmit power with `network.wifi_txpower_dbm` (about 10); check whether the 電源 field in the application shows 「低電圧」 (on boards that report it, this helps isolate the cause). If it occurs while driving, lower the manual 最大出力 and 手動の加速 and 自動の最大出力 to limit the current at a start (the start kick does not exceed them either); to limit it permanently, `max_drive_pwm`; to weaken the kick, `pwm.kick_pwm` |
| Starting in manual mode drops the Wi-Fi, which comes back after 2–3 minutes | The large current of the start pulls the battery voltage down and the core (the Raspberry Pi) restarts (booting and bringing up the access point takes 2–3 minutes). Starting from rest draws close to the stall current until the motor turns, and how much depends on the motor's winding and the gears (the car model). From v0.6.0, 手動の加速 (default 287) raises the drive gradually. If it still occurs, lower 手動の加速 (e.g. 100), lower the manual 最大出力, or lower `pwm.kick_pwm`. Whether the car restarted shows in `journalctl --list-boots` (the boot history) on the car. The real cure is on the power side: separate supplies for the motors and the Raspberry Pi, or a capacitor on the supply |
| Wi-Fi disappeared | If the core (rumicar-core) stops, the access point is not started either, but `rumicar-ap.timer` retries from 90 s after boot and then every 60 s. Check with `systemctl status rumicar-ap.timer` and `nmcli connection show --active` |
| The PC disconnects from `RumiCar` by itself | The car's access point has no Internet access, so Windows may judge it as having no Internet connection and switch to another registered Wi-Fi network. If the application's PC の Wi-Fi turns red, this is the cause. Countermeasures: in Windows' Wi-Fi settings set `RumiCar` to "Connect automatically" and turn "Connect automatically" off for other networks (home, office) only while driving; or drive where other networks are out of range. A PC connected to the Internet over wired LAN can dedicate its Wi-Fi to the car |
| Sudden start immediately after switching to auto | Since v0.3.0 the permitted drive increases from 0 when auto mode is entered (`auto_accel_pwm_per_s`; the default 287 takes about 0.89 s from 0 to 255). If it is too fast, lower 自動の最大出力 in the application (from v0.6.0 saved on the car and kept across a restart; start a first drive at 77–128). To limit it permanently, use the car's `max_drive_pwm` |
| On a wide course the car does not steer or slow down until it is close to a wall (all three ToF read open) | The driving logic treats everything beyond its limit (`conf_max_mm` of the bundled decision data, `read_limit_mm` of the sample `py-sample`) as open. Check in the application's ToF display (the core's values) that the distance to the walls is shown as a number, and make decision data or a `drive.py` with a wider limit and thresholds (section 5, "How far the ToF readings are trusted"). A core of v0.6.0 or earlier cuts the readings off at 200 mm, so this is not possible there |
| My own program does not run | Errors at load time are logged in the application with the reason (`drive.py の N 行目 …`, the line number in drive.py). Errors after the start and `print()` output are recorded in the car's log (`sudo journalctl -u rumicar-core -f`). If `loop()` does not complete within 30 ms, that frame brakes; if there is no response for 1 s or more, the program is stopped (`time.sleep()` and endless loops must not be used). Section 11 |
| `hardware bring-up failed: open /dev/i2c-1` | The v0.1.1 image had no setting to load the `i2c-dev` kernel module, so `/dev/i2c-1` was not created. `sudo raspi-config` → Interface Options → I2C → Yes resolves it (persists across reboots). Not needed with the v0.1.2 and later images and packages |
| A switch fails | The reason recorded in the application's log (manifest mismatch, missing required keys, logic-name mismatch, etc.). `rumicar-activate show` on the car |
| SSH password rejected | The first-boot setup may not have been applied yet. Reboot and wait a few minutes |

Please report problems in [Issues](https://github.com/RumiCar-group/RumiCar-Zero2Cam/issues) with the contents of the application's log or the output of `sudo journalctl -u rumicar-core -b`.

## 11. Your own driving program (Python)

Besides the bundled driving logics (`tof_drive`, `cam_drive`), the car can be driven by **a Python program written by the user**. The logic `py_drive` executes the `drive.py` of the pack sent to the car. This is independent of the learning pipeline (section 7); it is for writing the "decide steering and drive from the distance sensor values" part yourself. No compiler or development environment is required: a text editor (even Notepad) and the drive manager are sufficient to write and send a program.

### 11.1 Procedure

1. Copy the sample in `car/packs/py-sample/` (`drive.py` and `params.json`) to any folder on the PC (e.g. `C:\Users\<you>\RumiCarData\my-drive`). The folder name becomes the name of the data (pack) (characters other than letters and digits are replaced by `-`).
2. Open `drive.py` and rewrite the body of `loop()` (it works unchanged for a first try).
3. Connect to the car and select that folder with **自分のプログラムを送る…** in 版の管理. The application creates the `manifest.json`, sends the folder and activates the logic `py_drive` together with the data (「有効化: ロジックも py_drive-… に切り替えます」 is logged).
4. Press 自動 to start driving. At first, setting 自動の最大出力 to 77–128 (30–50 %) is recommended.
5. After editing, run 自分のプログラムを送る… again. Every send creates a new version (the version number is the send time), and データを戻す returns to the previous one.

If `drive.py の N 行目 …` is logged in the drive manager, fix that line of drive.py. `print()` output and errors after the start are recorded in the car's log (`ssh rumicar@10.42.0.1`, then `sudo journalctl -u rumicar-core -f`).

### 11.2 Writing drive.py

`drive.py` is written with the RumiCar library. It is the Raspberry Pi library itself (`RasPi/RumiCar.py` 0.7.0 in the RumiCar repository), with the same function names, constants and values as the Arduino library RumiCar-lib v0.7.0 (the Zero2Cam keeps no equivalent of its own: the build takes the file of a fixed commit, checks its SHA-256 and bundles it with `py_drive`). On the Zero2Cam the core owns the sensors and motors, so the core calls `loop()` every 50 ms; `RC_read()` returns that frame's sensor values and `RC_steer()` / `RC_drive()` set the command for the frame (the Zero2Cam hardware layer of RumiCar.py).

```python
from RumiCar import *

FAR = 9999               # stands in for -3 (out of range): larger than any distance RC_read returns (2000 at most)

def setup():             # once at the start (optional)
    RC_setup()           # does nothing on the Zero2Cam (the core has initialized the hardware)
    RC_setReadLimit(200) # how far RC_read is trusted [mm]; beyond it RC_read returns -3 (far)
    RC_state("START")

def far(d):              # -3 is the smallest number, so make it "far" before comparing
    return FAR if d == -3 else d

def loop():              # every 50 ms (20 Hz)
    left, center, right = RC_read(LEFT), RC_read(CENTER), RC_read(RIGHT)
    if -1 in (left, center, right):    # a sensor gave no reading: stop
        RC_drive(BRAKE, 0)
        return
    left, center, right = far(left), far(center), far(right)
    if center < 150:                   # close ahead (keep the thresholds at or below the limit)
        RC_steer(LEFT if left > right else RIGHT, 192)   # towards the side with the larger distance, strength 192
        RC_drive(FORWARD, 102)
    else:
        RC_steer(CENTER)
        RC_drive(FORWARD, RC_params()["speed"])   # a value from params.json
```

| Name | Description |
|---|---|
| `RC_read(LEFT / CENTER / RIGHT)` | Distance sensor value: `0`–limit = distance in mm (the limit is the one set with `RC_setReadLimit`, `2000` when none is set), `-1` = read failure, `-3` = out of range (beyond the limit, or no target), to be treated as far when driving. `-3` is the smallest number, so before comparing replace it with a value larger than any distance `RC_read` returns (larger than the limit when one is set, larger than 2000 otherwise). `RC_read(REAR)` is `-1` on this car; a bad argument gives `-2` |
| `RC_setReadLimit(limit)`<br/>`RC_setReadLimit(direction, limit)` | Sets how far `RC_read` is trusted, in mm (call it in `setup()` after `RC_setup()`). The limit is `1`–`2000`; `0` is no limit (the default). For a reading above the limit `RC_read` returns `-3`; the limit itself stays a distance, and `-1` and `-3` are unchanged. Out-of-range values are clamped to 0–2000, fractions are cut towards 0, a value that is not a number counts as 0 (no limit). With a direction (`LEFT` / `CENTER` / `RIGHT` / `REAR`) only that direction is set (`REAR` can be set, but `RC_read(REAR)` stays `-1` on this car). Returns 1; a bad direction changes nothing and returns 0. `RC_setup()` does not reset the limit. The sample takes the value from `params.json`: `RC_setReadLimit(RC_params().get("read_limit_mm", 200))`. Section 5, "How far the ToF readings are trusted", explains how to choose it |
| `RC_steer(LEFT / CENTER / RIGHT, strength)` | Steering. The strength is the share of the signal, `0`–`255` (not compensated); left out it is `255` (full, the output of earlier versions). `0` is the same as `CENTER` (neutral), `1`–`254` is slow decay (`RIGHT` holds AIN1 high and puts `255 − strength` on AIN2, `LEFT` the other way round), `CENTER` ignores the strength. Out-of-range values are clamped to 0–255, fractions are cut towards 0, a value that is not a number counts as 0. Returns 1; a bad direction is ignored and returns 0. The same value steers differently on different cars, so adjust it while driving, e.g. "1.3 times when it does not turn enough, half when it overshoots" (`RasPi/Exercise-3.3` in the RumiCar repository does this; sent as `drive.py` it runs as is) |
| `RC_drive(direction, value)` | Drive: direction `FREE` (coast to a stop) / `REVERSE` / `FORWARD` / `BRAKE`, value = PWM duty `0`–`255` (150 means a signal of 150/255 ≈ 59 %). Values outside 0–255 are clamped. The values of `RC_steer()` and `RC_drive()` are held until changed |
| `RC_params()` | The contents of `params.json` (a dict). Values that differ per car or course can be put here, so that tuning does not require changing the program |
| `RC_state("NAME")` | Name of the current processing state (letters, digits and `_`, up to 24 characters), shown as 判断状態 in the application |
| `print(...)` | Recorded in the car's log |

The ToF values the drive manager shows and records are the core's (no limit, 0–2000 mm); `RC_setReadLimit` in `drive.py` does not affect them (it only changes what `RC_read()` returns inside `drive.py`).

For elapsed time use Python's `time.monotonic()`. A program that waits inside `loop()` (such as `RasPi/Exercise-2.5`, which changes the steering with `time.sleep()`) does not run as is because of the time limit in 11.3; switch states by elapsed time instead. Your own `.py` files can be placed next to `drive.py` and imported. Only the Python standard library is available (numpy etc. are not installed on the car). Camera images are not available in `py_drive` (images are handled by the learning pipeline of section 7).

### 11.3 Safety control by the core

- The processing time of one `loop()` call is limited to **30 ms**; a frame whose processing does not complete within this time brakes (a late response is discarded). If there is no response for **1 s** or more, the program is stopped and the car stays braked (activate again to restart). Do not use `time.sleep()` or `while` loops that do not terminate.
- A frame in which an exception occurs brakes, and the program continues. Out-of-range values do not raise: as in RumiCar-lib, `RC_drive(FORWARD, 5000)` is clamped to 255, and a bad direction or state name is ignored and returns 0 (logged once on the car).
- On entering auto mode the permitted drive increases from 0, and the car never exceeds 自動の最大出力 or its `max_drive_pwm`. Manual mode, record-only and the emergency stop behave the same for every logic.
- The program runs in a process separate from the core, so the core's control period (50 ms) is maintained even if the program stops responding.

**Figure 9** Per-frame exchange between the core and drive.py (runner.py)

<!-- figure: en/09-py-drive.mmd -->
```mermaid
sequenceDiagram
    participant C as rumicar-core (libpy_drive.so)
    participant R as runner.py (separate process)
    Note over C,R: Activation (create)
    C->>R: start: python3 -I -B -u runner.py pack
    R->>R: load RumiCar.py (next to runner.py), params.json and drive.py, setup()
    R-->>C: READY (within 15 s) or ERROR message
    loop every 50 ms
        C->>R: F seq t_ms left centre right
        alt response within 30 ms
            R-->>C: C seq steer steer_pwm drive_dir drive_pwm state
            C->>C: output the command (core limits applied)
        else exception in loop()
            R-->>C: X seq message
            C->>C: brake this frame (program continues)
        else no response within 30 ms
            C->>C: brake this frame (late response discarded)
        end
    end
    Note over C,R: No response for 1 s or more since the last one → SIGKILL<br/>(also when the program ends: braked until the next activation)
```

### 11.4 Notes

- A sent pack is stored in `/var/lib/rumicar/packs/<folder name>-<time>/`; `rumicar-activate pair py_drive-0.4.0 <pack>` switches to it over SSH (logic and data are switched as a pair).
- A pack made by 「自分のプログラムを送る…」 has `min_abi` 5 (a logic with `RC_setReadLimit` and `RC_read` up to 2000 mm of RumiCar.py 0.7.0: core 0.7.0 or later).
- To use a `drive.py` written for core 0.6.0 or earlier on core 0.7.0, add `RC_setReadLimit(200)` to `setup()`: it then gets the values it got before (−3 beyond 200 mm) (section 5, "How far the ToF readings are trusted").
- The ToF driving (`tof_drive`) is written in C++. To port processing written in Python to a full driving logic, use `car/logic/tof_drive/` as the reference.
- The sample `py-sample` is also bundled with the car, so `py-sample-0.4.0` can be activated from 版の管理 to check its behaviour without the repository on the PC.

## 12. Explanatory videos

Short videos (30–45 s each, `docs/videos/`, 1280×720, no audio; a click plays them on GitHub's file page) of the points that are easiest to misread. Every number is a default from this version's sources.

| Video | Content | 日本語 | English |
|---|---|---|---|
| ToF readings and their display | 0–2000 mm, −3 (out of range) and −1 (timeout), and how they are treated with the limit the driving logic sets (`conf_max_mm` of `tof_drive`) (the table in section 5, `RC_read()` values) | [01-tof-readings-ja.mp4](docs/videos/01-tof-readings-ja.mp4) | [01-tof-readings-en.mp4](docs/videos/01-tof-readings-en.mp4) |
| Start control in auto mode (no sudden starts) | The permitted drive rising at `auto_accel_pwm_per_s` (287/s) and the auto limit (section 5) | [02-auto-start-ja.mp4](docs/videos/02-auto-start-ja.mp4) | [02-auto-start-en.mp4](docs/videos/02-auto-start-en.mp4) |
| Overtaking a parked vehicle (`cam_drive` vehicle behaviour) | FOLLOW → OVERTAKE_OUT → OVERTAKE_PASS → OVERTAKE_IN; the side ToFs look 65° forward, so `pass_clear_ms` lets the rest of the car pass after the inner ToF loses the vehicle (7.5) | [03-overtaking-ja.mp4](docs/videos/03-overtaking-ja.mp4) | [03-overtaking-en.mp4](docs/videos/03-overtaking-en.mp4) |

The videos are made by `docs/videos/make_videos.py` (Pillow + ffmpeg). When a default in the sources changes, the constants and captions in the script are updated and the videos regenerated (`python3 docs/videos/make_videos.py <out dir> [--lang ja|en]`). Values that may change after checks on the real car (`pass_clear_ms` and others, specification 8.3) are updated once confirmed.

---

# For developers

## Building the car side (Raspberry Pi OS Lite 64-bit / Debian 13)

```sh
sudo apt install build-essential cmake pkg-config libgpiod-dev nlohmann-json3-dev libssl-dev libturbojpeg0-dev libcamera-dev
cmake -S car -B build -DCMAKE_BUILD_TYPE=Release      # needs an Internet connection to download RumiCar.py (9.3)
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build --prefix /usr/local   # manual install
```

To install on a car, use the Debian package (the CI artifact `rumicar-zero2cam-deb-arm64`, or configure with `cmake -S car -B build -DCMAKE_INSTALL_PREFIX=/usr` and run `cd build && cpack -G DEB`). `sudo apt install ./rumicar-zero2cam_<version>_arm64.deb` installs the core, the bundled logics / packs, the systemd services and the Wi-Fi access-point profile (SSID `RumiCar`, 10.42.0.1), and creates the state directory `/var/lib/rumicar`.

Without libcamera, build with `-DRUMICAR_WITH_CAMERA=OFF` (that core does not start a camera and drives on the ToF sensors only).

The driving logics are in `car/logic/`: `tof_drive` (ToF driving), `cam_drive` (camera-based decisions added to `tof_drive`: `vision_features` features, `vision_pack` for `vision.bin`, `vision_eval` detector evaluation with temporal confirmation, `cam_logic` the traffic-light, vehicle and intersection behaviours and their priorities) and `py_drive` (`runner.py` executes the pack's `drive.py` in a separate process, and `py_process` exchanges one line per frame; timeouts and abnormal termination brake; `test_py_drive` verifies it with the actual runner and RumiCar.py). The interface between the core and the logics is defined in `car/include/rumicar/logic_api.h` (ABI 5: the ToF values are 0–2000 mm, −1 and −3, and the core does not cut them off at 200 mm; the steering strength `steer_pwm` in `rumi_command` dates from ABI 4). The core loads a logic only when its ABI (`abi` in the manifest and the value the library itself reports) equals the core's own, so a logic (.so) of your own must be rebuilt when the ABI goes up. Decision data (a pack) loads when its `min_abi` is at most the core's ABI.

The RumiCar.py of `py_drive` is not kept in this repository: configuring (`cmake -S car …`) takes it from a fixed commit of the RumiCar repository. It is accepted only when `RUMICAR_PY_COMMIT` (the commit), `RUMICAR_PY_SHA256` (the file's SHA-256) and `RUMICAR_PY_VERSION` (its `__version__`) in `car/CMakeLists.txt` all match (otherwise the configuration stops); it is placed next to `runner.py` in the `py_drive` folder and its SHA-256 is recorded in the manifest (the car checks it on every activation). To move to another version of RumiCar.py, change the three together.

A steering strength of 1–254 is output on AIN1/AIN2 of the DRV8835 (GPIO 17/27; the hardware PWM channels are used by the drive) by software PWM (`hw/soft_pwm`: 500 Hz by default, a `SCHED_FIFO` thread; at strength 0 or 255 and for CENTER the thread does not run and the GPIOs are set directly). `test_soft_pwm` checks the input values (`Drv8835::steer_inputs`, the same as RumiCar-lib) and the duty, frequency and switching of the real thread. The kick on a start or reversal never exceeds the frame's limit of the driving mode (`kick_cap`), and a thread of `Drv8835` (`SCHED_FIFO`) ends it after `kick_ms` (independent of the 50 ms control period). `FrameOutputStage` in `core/src/frame_output.hpp` decides each mode's output (limits, acceleration, steering strength, the kick's cap); the control loop and `test_drive_output` run the same code. `test_drive_output` lets the real `Drv8835` write PWM files laid out as in sysfs and checks the kick's length and cap in real thread time. The Python implementation of the features and `vision.bin` is `car/tools/rumicar_vision.py`; `test_vision` verifies that it matches the C++ implementation exactly through the golden data in `car/tests/data/vision` (generated by `make_golden.py`). `car/tests/tools_smoke.py <build dir>` learns from synthetic recordings and verifies end to end with `rumicar-replay` stopping and starting, following and overtaking, and stopping at an intersection (also run in CI).

## Control loop and the ToF driving logic

Every 50 ms the core performs the processing of Figure 10. The state transitions of the driving logic `tof_drive` are shown in Figure 11 (spec 3.5, 3.7). The priorities of its decisions are summarised in section 5, "Priorities of the decisions in auto mode".

**Figure 10** Processing of one control-loop frame (50 ms)

<!-- figure: en/10-frame.mmd -->
```mermaid
flowchart TD
    A["Read ToF ×3<br/>(each within i2c.tof_read_timeout_ms, default 100 ms)<br/>RC_read values (no limit set): 0..2000 [mm]<br/>−3 = out of range (beyond 2000 mm, no target)<br/>−1 = read failure"] --> B["Attach the latest camera image<br/>(only frames with a new image)"]
    B --> C["Logic step()<br/>(every frame, in every mode)"]
    C --> E{"Emergency stop?<br/>Drive mode?"}
    E -->|"E-stop active"| B1["Brake"]
    E -->|"Record only"| B2["Brake"]
    E -->|"Manual"| H["PC's manual_drive<br/>(RC_steer's direction and strength, RC_drive values)<br/>drive = min(command, manual limit, max_drive_pwm)<br/>rising at the manual acceleration (falling and stopping at once)"]
    E -->|"Auto"| F{"step() succeeded?"}
    F -->|"Failed"| B3["Brake"]
    F -->|"Succeeded"| G["Limit the drive to the permitted value<br/>permitted = min(increase from 0 at auto_accel_pwm_per_s,<br/>auto limit, max_drive_pwm)<br/>brake if the command is BRAKE or has FAULT"]
    B1 --> MO["Output to DRV8835<br/>(a steering strength of 1..254 by software PWM)<br/>on a start or reversal, min(kick_pwm, the mode's limit) for kick_ms"]
    B2 --> MO
    B3 --> MO
    H --> MO
    G --> MO
    MO --> T["Status and JPEG to telemetry (if a client is receiving)<br/>→ next period (50 ms)"]
```

**Figure 11** State transitions of tof_drive

<!-- figure: en/11-tof-states.mmd -->
```mermaid
stateDiagram-v2
    state "TURN_LEFT / TURN_RIGHT" as TURN
    [*] --> CRUISE
    CRUISE --> TURN: steering left / right<br/>(turning or side-wall avoidance)
    TURN --> CRUISE: steering back to centre
    CRUISE --> EMERGENCY_BRAKE: centre < d_emergency_mm
    TURN --> EMERGENCY_BRAKE: centre < d_emergency_mm
    EMERGENCY_BRAKE --> CRUISE: brake_ms elapsed and<br/>centre ≥ d_min_mm
    EMERGENCY_BRAKE --> RECOVER_REVERSE: brake_ms elapsed and<br/>centre < d_min_mm
    CRUISE --> RECOVER_REVERSE: stuck detected<br/>(stuck_time_ms)
    TURN --> RECOVER_REVERSE: stuck detected
    RECOVER_REVERSE --> CRUISE: reverse_ms elapsed or<br/>(reversed ≥ 200 ms and centre ≥ d_turn_exit_mm)
    note right of CRUISE
        A read failure of any sensor longer than
        invalid_hold_ms gives SENSOR_FAULT (stop);
        on recovery, CRUISE
    end note
```

The figure sources are the `.mmd` (Mermaid) files in `docs/figures/ja` (Japanese) and `docs/figures/en` (English). The figures in the READMEs are copies of them; after changing a source, run `python3 docs/figures/sync_readme.py --write` (CI checks that they match with `--check`). The figures in the specification (.docx) are PNGs drawn from the same sources with the Mermaid CLI (`mmdc -c docs/figures/mermaid-config.json -i <source> -o <PNG> -s 3.8 -b white`; colours and font (Noto Sans CJK JP) in `mermaid-config.json`).

## Building the PC side (Windows / .NET 10 SDK)

```sh
dotnet build pc/RumiCar.sln -c Release
dotnet test pc/RumiCar.Protocol.Tests -c Release
dotnet publish pc/RumiCar.DriveManager -c Release -r win-x64 --self-contained -p:PublishSingleFile=true
```

## SD-card image and Releases

- The image is created by `car/image/build-image.sh` (root, arm64 host: the package is installed into the official Raspberry Pi OS Lite 64-bit image in a chroot and the first-boot settings are added). In CI the `image` job of `car.yml` creates it on pushes to main and on tags.
- A Release is created by tagging a commit on main as `vX.Y.Z` and pushing the tag (or by creating a Release with a new tag on GitHub's Releases page): `release.yml` then runs and attaches the image, the `.deb`, the PC application and `SHA256SUMS.txt`.
- **Write the number of the next Release into the sources first, then tag.** (1) In a pull request set `RUMICAR_VERSION` in `car/CMakeLists.txt`, `<Version>` in `pc/RumiCar.DriveManager/RumiCar.DriveManager.csproj` and the `assemblyIdentity version` (`X.Y.Z.0`) in `pc/RumiCar.DriveManager/app.manifest` to the same number, (2) merge it, (3) tag that merge commit `vX.Y.Z` and push the tag. `release.yml` first verifies that the tag matches those three places and is a commit on main, and stops without building if not (so that no Release is created whose tag and contents disagree).
- Builds other than a tag (main, pull requests) get `+g<commit>` appended by CI (`-DRUMICAR_VERSION_SUFFIX` in `car.yml`, `-p:InformationalVersion` in `pc.yml`); a local build keeps the number in the sources. The logic versions (`TOF_DRIVE_VERSION` / `CAM_DRIVE_VERSION` / `PY_DRIVE_VERSION`) are independent of the core version and are raised when a logic changes. They are also raised when the contents of a logic or pack change (including files other than the .so and the manifest's description), because the car does not overwrite a directory of the same version (9.2).

## Scope of this version (0.7.0)

- Car: ToF driving logic, control loop and safety control, drive modes (auto / manual / record_only; record_only at start-up) and emergency stop, Camera Module 2 capture (libcamera, RGB24 320×240 at 20 fps, operated only when needed) with JPEG transmission, control server (TCP 5600) and telemetry server (TCP 5601), Wi-Fi access point (NetworkManager, started in sequence by the core) and `set_wifi`, under-voltage / throttling monitoring, hot-swapping of logic / packs while driving (from the PC or over SSH), verification by manifests that also carry a description, capabilities and source scenarios, systemd services, Debian package
- PC: wire formats for control, telemetry, manifests and recording; the drive manager (connection, status and image, drive modes, e-stop, manual operation with pad, scrolling and keys, auto limit, display of the PC's Wi-Fi, recording with scenarios, version management with paired logic/pack switching, sending your own program, video creation from recordings)
- Car: the image logic `cam_drive` (stopping and starting at traffic lights, following a vehicle ahead and overtaking a parked vehicle, stopping at intersections and going straight or turning), the learned data `vision.bin`, the learning tool `train_pack.py`, the replay tool `rumicar-replay`, logic interface ABI 5 (ToF 0–2000 mm; the steering strength dates from ABI 4)
- Car: the logic `py_drive` that executes a Python program written by the user (`drive.py`) with the sample `py-sample`, limiting of the rate of increase of the permitted drive and the maximum output in auto mode, paired switching of logic and data (`activate_pair`)
- Car: the drive settings saved on the car (manual and auto maximum outputs, manual acceleration, the pad's steering strength; `set_drive_settings`), the manual acceleration, a 20 ms start kick that never exceeds the mode's limit, the steering strength in manual operation, and the side-wall steering strength proportional to the distance (`side_steer_pwm_min`)
- Distribution: standard SD-card image, GitHub Releases from tags (0.2.0: the logic interface changed from ABI 1 to ABI 2 and self-built logic must be rebuilt, hence the step up from 0.1.x; 0.2.1: manual operation changed to the driving pad and scrolling, the release verifies that tag and sources agree, builds that are not a release carry `+g<commit>`; 0.3.0: your own driving program, the auto limit and prevention of sudden starts, display of the PC's Wi-Fi; 0.3.1: wording in documents, screens and messages unified to technically precise terms (`py_drive` 0.1.1, `py-sample` 0.1.1, `cam-base` 0.1.1; no change in behaviour; 0.3.2: figures added to the READMEs and the specification, Japanese and English; no change in behaviour; 0.4.0: the values now follow the RumiCar library (Arduino RumiCar-lib, RumiCar.py for the Raspberry Pi): ToF as `RC_read` values (0–200 mm, −1 timeout, −3 out of range = nothing within 20 cm), steering as `RC_steer` LEFT=0 / CENTER=1 / RIGHT=2, drive as `RC_drive` FREE / REVERSE / FORWARD / BRAKE with a duty 0–255; logic interface ABI 3, control protocol 2, `core.json` keys `max_drive_pwm` / `auto_accel_pwm_per_s` / `kick_pwm`, `drive.py` written with the RumiCar library API. Not compatible with the 0.3.x PC application, recordings or self-built logic; recordings must be made again; 0.4.1: SD cards made and the car updated from the drive manager — the Release carries a Raspberry Pi Imager list (`rumicar-zero2cam.rpi-imager-manifest`) and "SD カードを作る…" downloads and verifies the image and opens Imager; check for the newest Release. No change on the car; 0.5.0: steering strength — `RC_steer(direc, ipwm = 255)` (as in RumiCar-lib v0.6.0 and RasPi/RumiCar.py 0.6.0: 0–255, 0 neutral, 255 and the one-argument form give the earlier output) through the logic interface (ABI 4, `rumi_command.steer_pwm`), software PWM of the DRV8835 steering, telemetry and recordings (`steer_pwm` / `logic_steer_pwm`), the drive manager's display (`RC_steer(LEFT, 128)`) and the decision data (`turn_steer_pwm` / `side_steer_pwm` / `recover_steer_pwm` of `tof_drive`). `drive.py` runs on RumiCar.py 0.6.0 of the RumiCar repository itself (checked by SHA-256 and bundled at build time). ABI 3 logics no longer load, so the bundled logics and packs are new versions. ABI 3 recordings (0.4.x) can be used for learning and replay as they are (their steering counts as strength 255); 0.4.x decision data (a `params.json` without the strength keys) loads with strength 255 as well; 0.6.0: the drive settings saved on the car (manual maximum output, auto maximum output, manual acceleration, the pad's steering strength; `/var/lib/rumicar/settings/drive.json`, `set_drive_settings` replaces `set_auto_limit`), the manual acceleration (default 287/s, limiting the rise only), the start kick within each mode's limit and ended after 20 ms (it used to last the 50 ms control period), the steering strength in manual operation (the application's パッドの横位置で操舵の強さを変える, off by default), `side_steer_pwm_min` of `tof_drive` 0.5.0 / `cam_drive` 0.5.0 (the earlier constant strength by default), and the description of the priorities of the decisions in auto mode (section 5). The control protocol is 3: no connection with a 0.5.x or earlier core or application. The logic interface (ABI 4), decision data and recordings are as in 0.5.0; 0.7.0: the fixed 200 mm cut-off of the ToF readings is withdrawn — the core passes the readings to the logic, and displays and records them, as 0–2000 mm (the range in the specification of the VL53L0X), as `RC_read` of RumiCar-lib v0.7.0 and RasPi/RumiCar.py 0.7.0 does (the cut-off introduced in 0.4.0 was reconsidered because the distance at which the floor is picked up differs from car to car and readings beyond 200 mm can be usable). How far a reading is trusted is decided by the driving logic: `tof_drive` 0.6.0 and `cam_drive` 0.6.0 by `conf_max_mm` of the decision data (now 1–2000; with the default 200 the decisions are those of 0.6.0), `drive.py` by `RC_setReadLimit` of RumiCar.py 0.7.0 (`py_drive` 0.4.0; the sample `py-sample` 0.4.0 takes it from `read_limit_mm` in `params.json`). The logic interface is ABI 5 (the structs of ABI 4, another range of the ToF values), so a logic of your own must be rebuilt. Decision data (`min_abi` 4) and recordings (ABI 3 and 4) of 0.6.0 or earlier are used as they are. The control protocol stays 3)
- Future work: recording and tuning on real cars (overtaking and turning durations, learning intersection images), choosing the turn according to the situation (currently fixed per pack, or towards the detected side)

## License

MIT License ([LICENSE](LICENSE))
