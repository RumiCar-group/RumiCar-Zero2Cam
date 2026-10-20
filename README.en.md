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

The car **starts driving as soon as it is powered on** (like the Arduino version). Put it on a stand so the wheels are off the floor for the first boot and while you check things.

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
3. `rumicar-core` then starts and **the car drives off if nothing is in front of it**. At the same time the Wi-Fi access point comes up.

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
| 状態 (status) | モード = mode; 非常停止 = e-stop (with reason); 判断状態 = logic state (CRUISE / TURN_LEFT / TURN_RIGHT / EMERGENCY_BRAKE / RECOVER_REVERSE / SENSOR_FAULT); ToF 左/中/右 = left/centre/right distance [mm] (`open` = nothing nearby, `--` = no reading); モーター出力 = what went to the motors; ロジックの判断 = what the logic decided; 版 = logic / pack versions; 受信コマ数 = frames received |
| 走行モード (drive mode) | **自動** auto: the car's logic drives / **手動** manual: arrow keys on the PC (↑ forward 50 %, ↓ reverse 50 %, ←→ steer; click the image first so the keys reach the car) / **収録のみ** record-only: motors stopped, images and status still streamed (push the car by hand to collect data) |
| 収録 (recording) | Destination folder (default `C:\Users\<you>\RumiCarData`) and 収録開始 / 収録停止 (start / stop). Every received frame is saved. The "(画像付き N)" count next to 受信コマ数 grows when camera images arrive |
| 版の管理 (versions) | Logic and packs installed on the car (`[使用中]` = active, `[直前]` = previous, with verification failures shown). 一覧更新 refresh, 送る… send, 有効化 activate, 削除 delete, ロジックを戻す roll back the logic, データを戻す roll back the pack (section 6) |
| ログ (log) | Connection, responses, events and errors with timestamps |

Press 「切断」 to disconnect. The car keeps driving after you close the app (it is designed to run without a PC). Use the emergency stop or switch the car off to stop it.

## 6. Collect driving data and improve the decision data

### 6.1 Record

「収録開始」 creates one folder per recording under the destination:

```
RumiCarData/
  sessions/
    RumiCar_20261018-123456/      <car name>_<start time, UTC>
      frames/000001.jpg …        images (numbered within the recording)
      meta.jsonl                 one line per frame: {"index":1,"image":"000001.jpg","received":"…","header":{…status from the car…}}
      session.json               summary of the recording (frame count, gaps)
```

`header` holds, for that frame, the ToF values, the logic state and decision (steer, drive), what was actually sent to the motors, the mode, the e-stop state, the versions, and the image's sequence number and timestamp.

### 6.2 Improve the data with the Claude desktop app

1. Open the `RumiCarData` folder (or its `sessions` folder) in the Claude desktop app, show it the recordings and ask for improvements.
2. The decision data used by the current driving logic `tof_drive` is a single parameter file, `params.json`. Spec section 3.7 lists every key with its meaning and default (for example `d_turn_enter_mm`, the distance ahead at which a turn starts; `v_max`, the top speed; `accel_permille_per_s`, the acceleration rate).
3. Put the result in a folder of this shape (`manifest.json` is produced by `car/tools/make_manifest.py`, which needs only Python 3):

```
base-tof-0.2.0/            folder name is <id>-<version>
  params.json              every key is required
  manifest.json            python car/tools/make_manifest.py pack base-tof-0.2.0 --id base-tof --version 0.2.0 --logic tof_drive
```

Use a version (`0.2.0` and so on) that is not yet installed on the car.

### 6.3 Send it to the car and switch

1. With the drive manager connected, press **「送る…」** in 版の管理 and choose the folder. The app checks the manifest before sending.
2. When the new version appears in the list, select it and press **「有効化」** (activate). The car verifies it and switches at the next frame, even while driving. If verification fails the car keeps the current version and the log shows why.
3. If it drives badly, **「データを戻す」** returns to the previous pack. Versions you no longer need can be selected and deleted with 「削除」 (the active and the previous version cannot be deleted).

A rebuilt driving logic (the `tof_drive` `.so`) is sent the same way (`make_manifest.py logic …`, spec section 4).

## 7. Logging in to the car (SSH)

```sh
ssh rumicar@10.42.0.1          # password rumicar
sudo journalctl -u rumicar-core -f     # core log (state changes, switches, errors)
systemctl status rumicar-core          # service status
rumicar-activate show                  # active / previous versions and the last switch result
sudo nano /etc/rumicar/core.json       # settings (then sudo systemctl restart rumicar-core)
```

Main settings in `/etc/rumicar/core.json` (spec 3.3): `car_name`, `max_drive_permille` (drive limit, 1000 = 100 %), `camera.enabled`, `camera.fps`, `network.heartbeat_timeout_ms`.

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
| The car does not drive | Does `sudo journalctl -u rumicar-core -b` start with `hardware bring-up failed`? (The I2C/PWM settings are appended to `/boot/firmware/config.txt`.) Is the e-stop latched (「非常停止解除」 in the app)? ToF readings of `--` mean SENSOR_FAULT, which stops the car |
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

- Car: ToF driving logic, control loop and safety behaviour, drive modes (auto / manual / record_only) and emergency stop, Camera Module 2 capture (libcamera, RGB24 320×240 at 20 fps) with JPEG telemetry, control server (TCP 5600) and telemetry server (TCP 5601), Wi-Fi access point (NetworkManager) and `set_wifi`, hot-swapping of logic / packs (from the PC or over ssh) verified by manifests, systemd service, Debian package
- PC: wire formats for control, telemetry, manifests and recording; the drive manager (connection, status and image, modes, e-stop, manual driving, recording, version management)
- Distribution: standard SD-card image, GitHub Releases from tags
- Not yet: driving logic that uses the camera images

## License

MIT License ([LICENSE](LICENSE))
