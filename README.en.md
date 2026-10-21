# RumiCar Zero2Cam

**English** | [日本語](README.md)

Software for the RumiCar built around a Raspberry Pi Zero 2 W and a Camera Module 2.
One repository holds the car side (C++), the PC drive manager (C# / .NET 10), the wire formats between them, the distribution formats, and the automated builds.

- How to use it (from writing the SD card to driving, recording, and updating decision data): the [How to use](#how-to-use) section below
- Specification (settings, wire formats, distribution formats, builds): [docs/RumiCar-Zero2Cam 仕様書.docx](docs/RumiCar-Zero2Cam%20仕様書.docx) (Japanese)
- Downloads: [Releases](https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases)

## Layout

| Directory | Contents |
|---|---|
| `car/` | Car side: `rumicar-core` (control loop, hardware, network, hot-swap of versions), the driving logic `tof_drive` (shared library), the bundled pack `base-tof`, unit tests, Debian package and SD-card image builds |
| `pc/` | PC side: `RumiCar.Protocol` (wire formats, manifests, recording), `RumiCar.Protocol.Tests`, `RumiCar.DriveManager` (WPF drive manager) |
| `docs/` | Specification |
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

The car **does not drive just because it is powered on**: it boots into "record only" (motors braked) and starts driving when you press **「自動」** (auto) in the drive manager (since v0.1.3; earlier versions drove off at power-on). To have it drive at power-on without a PC, set `startup_mode` to `auto` in `/etc/rumicar/core.json` (section 7). Put the car on a stand so the wheels are off the floor for the first boot and while you check things.

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
3. `rumicar-core` then starts and brings things up one after another: sensors → Wi-Fi access point → camera (once a PC connects). The power-hungry parts are deliberately not started at the same time, to avoid brown-outs on dry-cell batteries (section 9). **The car does not drive yet** (record only); connect as in section 4 and press 「自動」.

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

## 7. Logging in to the car (SSH)

```sh
ssh rumicar@10.42.0.1          # password rumicar
sudo journalctl -u rumicar-core -f     # core log (state changes, switches, errors)
systemctl status rumicar-core          # service status
rumicar-activate show                  # active / previous versions and the last switch result
sudo nano /etc/rumicar/core.json       # settings (then sudo systemctl restart rumicar-core)
```

Main settings in `/etc/rumicar/core.json` (spec 3.3): `car_name`, `startup_mode` (`record_only`, the default, or `auto` to drive at power-on), `max_drive_permille` (drive limit, 1000 = 100 %), `camera.enabled`, `camera.on_demand` (run the camera only while a PC is connected or in auto mode; default true), `camera.fps`, `network.heartbeat_timeout_ms`, `network.ap_start` (`core`, the default: the core brings the access point up in sequence; `boot`: NetworkManager does it), `network.wifi_txpower_dbm` (transmit power limit; 0 = driver default; try 10–14 when batteries sag), `power.throttled_path` (under-voltage / throttling monitor; empty disables it).

## 8. Updating

### 8.1 Whole image

Write the image of a newer Release to the SD card (section 2). Recordings live on the PC, so nothing is lost.

### 8.2 Package only

Copy `rumicar-zero2cam_<version>_arm64.deb` from the Release to the car and install it (if the car is driving, the motors stop briefly while the service restarts).

```sh
scp rumicar-zero2cam_<version>_arm64.deb rumicar@10.42.0.1:
ssh rumicar@10.42.0.1
sudo apt install ./rumicar-zero2cam_<version>_arm64.deb
```

The required libraries are already in the image, so no Internet access is needed. If you edited `/etc/rumicar/core.json` or the access-point profile yourself, apt asks whether to replace them (otherwise they are updated silently).

### 8.3 When the car needs the Internet

The car is normally an access point and has no route to the Internet. To install something with `apt`, join your home Wi-Fi temporarily (this stops the `RumiCar` access point until the next reboot):

```sh
sudo nmcli device wifi connect "<home SSID>" password "<password>"
```

## 9. Troubleshooting

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
- Distribution: standard SD-card image, GitHub Releases from tags
- Not yet: driving logic that uses the camera images

## License

MIT License ([LICENSE](LICENSE))
