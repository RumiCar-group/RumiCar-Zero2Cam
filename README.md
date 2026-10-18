# RumiCar Zero2Cam

Raspberry Pi Zero 2 W と Camera Module 2 を搭載した RumiCar のソフトウェアです。
車載側（C++）、PC 側の走行管理アプリ（C# / .NET 10）、両者の通信形式、配布形式、自動ビルドを一つのリポジトリで管理します。

- 使い方（SD カードの作成から走行・収録・データの更新まで）：この README の [使い方](#使い方) 以下
- 仕様（設定、通信形式、配布形式、ビルド）：[docs/RumiCar-Zero2Cam 仕様書.docx](docs/RumiCar-Zero2Cam%20仕様書.docx)
- 配布物：[Releases](https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases)

## 構成

| ディレクトリ | 内容 |
|---|---|
| `car/` | 車載側。`rumicar-core`（制御ループ・ハードウェア・通信・差し替え機構）、走行ロジック `tof_drive`（共有ライブラリ）、同梱パック `base-tof`、単体テスト、Debian パッケージと SD イメージの作成 |
| `pc/` | PC 側。`RumiCar.Protocol`（通信形式・manifest・収録）、`RumiCar.Protocol.Tests`、`RumiCar.DriveManager`（WPF の走行管理アプリ） |
| `docs/` | 仕様書 |
| `.github/workflows/` | 自動ビルド（`car.yml`: Debian 13 arm64/amd64 と SD イメージ、`pc.yml`: Windows、`release.yml`: タグで Release） |

---

# 使い方

## 1. 用意するもの

| もの | 備考 |
|---|---|
| RumiCar の車体（Raspberry Pi Zero 2 W 搭載モデル） | DRV8835 モータードライバ、VL53L0X 距離センサー×3（左・中央・右）、Camera Module 2。配線は既存の RumiCar と同じ（仕様書 3.4） |
| microSD カード | 8 GB 以上（Class 10 / A1 推奨） |
| Windows PC（64bit、Windows 10/11） | Wi-Fi 付き。走行管理アプリを動かす |
| SD カードを書くソフト | [Raspberry Pi Imager](https://www.raspberrypi.com/software/) |

車は **電源を入れるとすぐ走り出します**（Arduino 版と同じ動作）。初回起動や確認のときは、車輪が床に着かないよう台に載せてください。

## 2. SD カードを作る

1. [Releases](https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases) の最新版から次の 2 つをダウンロードします。
   - `rumicar-zero2cam-<版>-<日付>.img.xz` … 車の SD カードイメージ（約 620 MB）
   - `RumiCarDriveManager-<版>-win-x64.zip` … PC の走行管理アプリ
2. Raspberry Pi Imager を起動し、**デバイス**で「Raspberry Pi Zero 2 W」、**OS**で一番下の「Use custom」を選んでダウンロードした `.img.xz` を指定、**ストレージ**で SD カードを選びます。
3. 「次へ」で「OS customisation settings を適用しますか」と聞かれたら **「いいえ（No）」** を選びます。ユーザー名・SSH・Wi-Fi・ホスト名はイメージに入っています。ここで Wi-Fi などを設定するとイメージの設定（車がアクセスポイントになる）と衝突します。
4. 書き込みが終わったら SD カードを車に差します。

ダウンロードしたファイルの確認（任意）：Release の `SHA256SUMS.txt` の値と、PowerShell の `Get-FileHash <ファイル名>` の値が一致すれば正常です。

## 3. 初回起動

1. 車を台に載せ、電源を入れます。
2. 初回はパーティションの拡張のため 1 回自動で再起動します。2〜3 分待ちます。
3. 起動すると `rumicar-core` が動き始め、**前方に障害物が無ければ走り出します**。同時に Wi-Fi のアクセスポイントが立ちます。

イメージに入っている初期設定：

| 項目 | 値 |
|---|---|
| Wi-Fi アクセスポイント | SSID `RumiCar`、暗号化なし（公開イメージのため。教育用途として許容） |
| 車のアドレス | `10.42.0.1`（PC には `10.42.0.x` が配られます） |
| ログイン | ユーザー `rumicar`、パスワード `rumicar`、SSH 有効、ホスト名 `rumicar` |
| 無線の国設定 | JP（アクセスポイントに必要） |

## 4. PC から接続する

1. PC の Wi-Fi で `RumiCar` に接続します（パスワードはありません。「このネットワークは安全ではありません」と出ても接続します）。
2. `RumiCarDriveManager-<版>-win-x64.zip` を展開し、`RumiCarDriveManager.exe` を実行します（インストール不要）。
   - 初回に「Windows によって PC が保護されました」（SmartScreen）が出たら「詳細情報」→「実行」。署名の無い実行ファイルのためです。
   - 「Windows Defender ファイアウォール」の確認が出たら「アクセスを許可する」。
3. 画面上部の「車両」が `10.42.0.1` のまま **「接続」** を押します。ログ欄に `車両 RumiCar, 本体 <版>` と出て、状態欄に ToF の値、判断状態、画像が表示されれば接続できています。

## 5. 走行管理アプリの使い方

| 部位 | 内容 |
|---|---|
| 車両／名前 | 車のアドレス（既定 `10.42.0.1`）と、収録フォルダ名に使う車の名前（既定 `RumiCar`） |
| 非常停止／非常停止解除 | 非常停止はモードに関係なくブレーキし、解除するまで走りません。手動モードで PC との通信が 1 秒途絶えたときや、手動モード中に切断したときも自動でかかります |
| 画像 | 車のカメラの最新画像（320×240、約 20 コマ/秒） |
| 状態 | モード、非常停止（理由付き）、判断状態（CRUISE／TURN_LEFT／TURN_RIGHT／EMERGENCY_BRAKE／RECOVER_REVERSE／SENSOR_FAULT）、ToF 左／中／右 [mm]（`open` = 近くに何もない、`--` = 読めず）、モーター出力、ロジックの判断、版（ロジック／データ）、受信コマ数 |
| 走行モード | **自動**：車のロジックが走行／**手動**：PC の矢印キーで操作（↑前進 50%、↓後退 50%、←→操舵。画像をクリックしてから押す）／**収録のみ**：モーターを止めたまま画像と状態を受信（手で動かしてデータを集めるときに） |
| 収録 | 保存先（既定 `C:\Users\<ユーザー>\RumiCarData`）と「収録開始／収録停止」。受信した全コマを保存します |
| 版の管理 | 車に入っているロジック／データの一覧（`[使用中]`、`[直前]`、検証失敗の表示）。「一覧更新」「送る…」「有効化」「削除」「ロジックを戻す」「データを戻す」（6 章） |
| ログ | 接続・応答・イベント・エラーを時刻付きで表示 |

切断するときは「切断」を押します。アプリを閉じても車は走り続けます（PC が無くても走る設計です）。止めるときは非常停止を押すか、車の電源を切ります。

## 6. 走行データを集めて、判断データを改良する

### 6.1 収録する

「収録開始」で、保存先の下に 1 回の収録ごとのフォルダができます。

```
RumiCarData/
  sessions/
    RumiCar_20261018-123456/      車の名前_開始時刻（UTC）
      frames/000001.jpg …        画像（番号は収録内の通し番号）
      meta.jsonl                 1 行 1 コマ：{"index":1,"image":"000001.jpg","received":"…","header":{…車から来た状態…}}
      session.json               収録の概要（コマ数、欠けた回数）
```

`header` には、そのコマの ToF 値、ロジックの状態と判断（操舵・駆動）、実際にモーターへ出した値、モード、非常停止、版、画像の連番と時刻が入っています。

### 6.2 Claude のデスクトップアプリで改良する

1. Claude のデスクトップアプリで `RumiCarData` フォルダ（またはその中の `sessions`）を開き、収録を見せて改良を指示します。
2. 現在の走行ロジック `tof_drive` が使う判断データは、パラメータ 1 ファイル（`params.json`）です。キーの意味と既定値は仕様書 3.7 に一覧があります（例：`d_turn_enter_mm` 旋回を始める前方距離、`v_max` 最高速、`accel_permille_per_s` 加速の変化率）。
3. できあがったデータは、次の形のフォルダにします（manifest.json は `car/tools/make_manifest.py` で作れます。Python 3 だけで動きます）。

```
base-tof-0.2.0/            フォルダ名は <id>-<version>
  params.json              全キー必須
  manifest.json            python car/tools/make_manifest.py pack base-tof-0.2.0 --id base-tof --version 0.2.0 --logic tof_drive
```

版の名前（`0.2.0` など）は、車に入っているものと重ならない新しい番号にします。

### 6.3 車に送って切り替える

1. 走行管理アプリで車に接続し、「版の管理」の **「送る…」** でそのフォルダを選びます。アプリが manifest を確認してから送ります。
2. 一覧に新しい版が出たら、それを選んで **「有効化」**。車は検証してから次のフレームで切り替えます（走行中でも可）。検証に失敗した場合は今の版のまま走り続け、ログに理由が出ます。
3. 具合が悪ければ **「データを戻す」** で直前の版に戻ります。不要になった版は選んで「削除」（使用中と直前の版は削除できません）。

ロジック本体（`tof_drive` の .so）を作り直した場合も同じ手順で送れます（`make_manifest.py logic …`、仕様書 4 章）。

## 7. 車に直接入る（SSH）

```sh
ssh rumicar@10.42.0.1          # パスワード rumicar
sudo journalctl -u rumicar-core -f     # 本体のログ（状態の遷移、切り替え、エラー）
systemctl status rumicar-core          # サービスの状態
rumicar-activate show                  # 使用中／直前の版と、最後の切り替え結果
sudo nano /etc/rumicar/core.json       # 設定（変更後は sudo systemctl restart rumicar-core）
```

主な設定（`/etc/rumicar/core.json`、仕様書 3.3）：`car_name`（車の名前）、`max_drive_permille`（駆動の上限、1000 = 100%）、`camera.enabled`、`camera.fps`、`network.heartbeat_timeout_ms`。

## 8. 更新する

### 8.1 イメージごと

新しい Release のイメージを SD カードに書き直します（2 章）。収録データは PC 側にあるので失われません。

### 8.2 パッケージだけ

Release の `rumicar-zero2cam_<版>_arm64.deb` を車に送って入れます（走行中に入れると切り替えのためモーターは一度止まります）。

```sh
scp rumicar-zero2cam_<版>_arm64.deb rumicar@10.42.0.1:
ssh rumicar@10.42.0.1
sudo apt install ./rumicar-zero2cam_<版>_arm64.deb
```

必要なライブラリはイメージに入っているので、インターネット接続は不要です。`/etc/rumicar/core.json` やアクセスポイントの設定ファイルを自分で変えていた場合は「置き換えるか」と聞かれます（変えていなければ聞かれずに更新されます）。

### 8.3 車をインターネットにつなぐ必要があるとき

車は普段アクセスポイントなので外に出ません。`apt` で何かを入れたいときは、一時的に家庭の Wi-Fi に切り替えます（切り替えると `RumiCar` の AP は止まります。再起動で元に戻ります）。

```sh
sudo nmcli device wifi connect "<家庭のSSID>" password "<パスワード>"
```

## 9. 困ったとき

| 症状 | 確認すること |
|---|---|
| Wi-Fi に `RumiCar` が出ない | 起動後 1〜2 分待つ。SSH が使えるなら `nmcli connection show` に `RumiCar-AP` があるか、`sudo journalctl -u NetworkManager -b` にエラーが無いか、`iw reg get` が `JP` か |
| アプリが接続できない | PC が `RumiCar` につながり `10.42.0.x` のアドレスを得ているか（`ipconfig`）。ファイアウォールの許可。車で `systemctl status rumicar-core` |
| 画像が出ない | 車で `sudo journalctl -u rumicar-core -b | grep -i camera`。カメラのケーブルの向き。`rpicam-hello --list-cameras` でカメラが見えるか |
| 走らない | `sudo journalctl -u rumicar-core -b` の先頭に `hardware bring-up failed` が無いか（I2C／PWM の設定は `/boot/firmware/config.txt` の末尾に追記されています）。非常停止がかかっていないか（アプリの「非常停止解除」）。ToF の値が `--`（読めず）のときは SENSOR_FAULT で止まります |
| 切り替えに失敗する | アプリのログの理由（manifest の不一致、必須キー不足、ロジック名の不一致など）。車の `rumicar-activate show` |
| SSH のパスワードが通らない | 初回起動の設定が反映される前の可能性。再起動して数分待つ |

アプリのログ欄の内容か `sudo journalctl -u rumicar-core -b` の出力を添えて [Issues](https://github.com/RumiCar-group/RumiCar-Zero2Cam/issues) に書いてください。

---

# 開発者向け

## 車載側のビルド（Raspberry Pi OS Lite 64bit / Debian 13）

```sh
sudo apt install build-essential cmake pkg-config libgpiod-dev nlohmann-json3-dev libssl-dev libturbojpeg0-dev libcamera-dev
cmake -S car -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build --prefix /usr/local   # 手動インストール
```

車に入れるときは Debian パッケージを使います（CI の成果物 `rumicar-zero2cam-deb-arm64`、または `cmake -S car -B build -DCMAKE_INSTALL_PREFIX=/usr` で構成して `cd build && cpack -G DEB`）。`sudo apt install ./rumicar-zero2cam_<version>_arm64.deb` で本体・同梱ロジック／パック・systemd サービス・Wi-Fi AP（SSID `RumiCar`、10.42.0.1）のプロファイルが入り、状態ディレクトリ `/var/lib/rumicar` が用意されます。

libcamera 無しの環境では `-DRUMICAR_WITH_CAMERA=OFF` でビルドできます（その本体はカメラを起動せず ToF だけで走ります）。

## PC 側のビルド（Windows / .NET 10 SDK）

```sh
dotnet build pc/RumiCar.sln -c Release
dotnet test pc/RumiCar.Protocol.Tests -c Release
dotnet publish pc/RumiCar.DriveManager -c Release -r win-x64 --self-contained -p:PublishSingleFile=true
```

## SD カードイメージと Release

- イメージは `car/image/build-image.sh` で作ります（root、arm64 ホスト。公式 Raspberry Pi OS Lite 64bit にパッケージを chroot で導入し、初回起動の設定を加えます）。CI では main への push とタグで `car.yml` の `image` ジョブが作ります。
- Release は、main のコミットに `vX.Y.Z` のタグを付けて push する（または GitHub の Releases ページで新しいタグを指定して Release を作る）と `release.yml` が走り、イメージ・.deb・PC アプリ・`SHA256SUMS.txt` を添付します。

## 本版（0.1.x）の範囲

- 車：ToF 走行ロジック、制御ループと安全動作、走行モード（auto / manual / record_only）と非常停止、Camera Module 2 の取り込み（libcamera、RGB24 320×240・20 fps）と JPEG 送信、制御サーバー（TCP 5600）・テレメトリサーバー（TCP 5601）、Wi-Fi AP（NetworkManager）と `set_wifi`、ロジック／パックの走行中差し替え（PC または ssh から）、manifest による検証、systemd サービス、Debian パッケージ
- PC：制御・テレメトリ・manifest・収録の各形式と走行管理アプリ（接続、状態・画像、モード、非常停止、手動操作、収録、版の管理）
- 配布：標準 SD カードイメージ、タグで GitHub Release
- 未実装（次の版）：画像を使う走行ロジック

## ライセンス

MIT License（[LICENSE](LICENSE)）
