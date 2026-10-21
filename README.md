# RumiCar Zero2Cam

[English](README.en.md) | **日本語**

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

車は **電源を入れただけでは走りません**。起動すると「収録のみ」（モーターはブレーキ）で待ち、走行管理アプリで **「自動」** を押すと走り出します（v0.1.3 から。それまでは電源投入で走り出していました）。PC なしで電源投入だけで走らせたいときは `/etc/rumicar/core.json` の `startup_mode` を `auto` にします（7 章）。初回起動や確認のときは、車輪が床に着かないよう台に載せてください。

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
3. 起動すると `rumicar-core` が動き始め、センサーの初期化 → Wi-Fi のアクセスポイント → （PC がつながったら）カメラ の順に立ち上がります。乾電池での電圧低下を避けるため、電気を食う部分は同時には起動しません（9 章）。**この時点では走りません**（収録のみ）。走らせるには 4 章の手順で接続して「自動」を押します。

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
| 画像 | 車のカメラの最新画像（320×240、約 20 コマ/秒）。「画像を表示」のチェックを外すと表示だけ止まります（受信と収録は続きます。PC の負荷を下げたいときに） |
| 状態 | モード、非常停止（緑 = なし、赤 = 作動中・理由付き）、判断状態（CRUISE／TURN_LEFT／TURN_RIGHT／EMERGENCY_BRAKE／RECOVER_REVERSE／SENSOR_FAULT）、ToF 左／中／右 [mm]（`open` = 近くに何もない、`--` = 読めず）、モーター出力、ロジックの判断、版（ロジック／データ）、**判断データ**（使用中のパックの名前・版と説明）、**対応機能**（壁／信号機／車両／交差点）、**学習元**（そのパックを作るのに使った収録シナリオと元の版）、**電源**（車のファームウェアが報告する低電圧・処理速度制限。「正常（低電圧なし）」が通常） |
| モーター出力／ロジックの判断 | 「操舵 左／中央／右 / 前進 74.8% (191/255)」のように表示。% は駆動モーターの **PWM のデューティ比**（車は千分率 0〜1000 で扱い、1000 = 100%）、括弧内は同じ値を Arduino 版の `analogWrite` の 0〜255 に換算したもの（748‰ × 255 ÷ 1000 ≈ 191）。前進／後退／停止（惰行）／ブレーキ（両入力 High）を区別します。「モーター出力」は実際にモーターへ出した値、「ロジックの判断」はロジックがそのコマで決めた値（手動・収録のみ・非常停止中でも表示） |
| 走行モード | **自動**：車のロジックが走行／**手動**：PC の矢印キーで操作（↑前進 50%、↓後退 50%、←→操舵。画像をクリックしてから押す）／**収録のみ**：モーターを止めたまま画像と状態を受信（手で動かしてデータを集めるときに）。下に「起動時のモード」（既定は収録のみ）が出ます |
| 収録 | 保存先（既定 `C:\Users\<ユーザー>\RumiCarData`）、**シナリオ**（この収録が何の場面か。例：「壁のみの走行データ」「信号機ありケース1」。入力するか、前に使った名前を選ぶ）、**メモ**（任意）、「収録開始／収録停止」。**既定では接続すると自動で収録が始まり**（「接続したら自動で収録を開始する」のチェック）、切断で閉じます。シナリオとメモは収録開始時の値が `session.json` に書かれます。受信した全コマを保存します。「受信コマ数」の（画像付き N）が増えていればカメラの画像が届いています |
| 動画を作る… | 収録フォルダを選ぶと、その画像から `video.avi`（Motion JPEG、実時間、20 コマ/秒）を同じフォルダに作ります。接続していなくても使えます（6.4） |
| 版の管理 | 車に入っているロジック／データの一覧（`[使用中]`、`[直前]`、検証失敗の表示）。「一覧更新」「送る…」「有効化」「削除」「ロジックを戻す」「データを戻す」（6 章） |
| ログ | 接続・応答・イベント・エラーを時刻付きで表示 |

切断するときは「切断」を押します。アプリを閉じても車は走り続けます（PC が無くても走る設計です）。止めるときは非常停止を押すか、車の電源を切ります。

## 6. 走行データを集めて、判断データを改良する

### 6.1 収録する

接続すると自動で（チェックを外した場合は「収録開始」で）、保存先の下に 1 回の収録ごとのフォルダができます。

```
RumiCarData/
  sessions/
    RumiCar_20261018-123456/      車の名前_開始時刻（UTC）
      frames/000001.jpg …        画像（番号は収録内の通し番号）
      meta.jsonl                 1 行 1 コマ：{"index":1,"image":"000001.jpg","received":"…","header":{…車から来た状態…}}
      session.json               収録の概要（シナリオ、メモ、開始時に車が使っていたロジック／パックとその説明・対応機能・学習元、コマ数、画像枚数、欠けた回数、低電圧の有無）
  scenarios.json                 これまでに使ったシナリオ名の一覧（新しい順、回数付き）
```

`header` には、そのコマの ToF 値、ロジックの状態と判断（操舵・駆動）、実際にモーターへ出した値、モード、非常停止、ロジックとパックの名前と版（`logic_name`／`pack_id`）、電源の状態（`throttled`）、画像の連番と時刻が入っています。

**シナリオの付け方**：収録の前に「シナリオ」欄に場面の名前を入れます。「壁のみの走行データ」「信号機ありケース1」「信号機と交差点ロジックあり」のように、あとで判断データを作るときにどの収録を使うか選べる名前にします。同じ名前は `scenarios.json` から選べます。

### 6.2 Claude のデスクトップアプリで改良する

1. Claude のデスクトップアプリで `RumiCarData` フォルダ（またはその中の `sessions`）を開き、収録を見せて改良を指示します。
2. 現在の走行ロジック `tof_drive` が使う判断データは、パラメータ 1 ファイル（`params.json`）です。キーの意味と既定値は仕様書 3.7 に一覧があります（例：`d_turn_enter_mm` 旋回を始める前方距離、`v_max` 最高速、`accel_permille_per_s` 加速の変化率）。
3. できあがったデータは、次の形のフォルダにします（manifest.json は `car/tools/make_manifest.py` で作れます。Python 3 だけで動きます）。

```
base-tof-0.2.0/            フォルダ名は <id>-<version>
  params.json              全キー必須
  manifest.json            python car/tools/make_manifest.py pack base-tof-0.2.0 --id base-tof --version 0.2.0 --logic tof_drive \
                               --description "壁のみ。旋回を早めに始めるよう d_turn_enter_mm を 220 に" \
                               --capability walls --scenario "壁のみの走行データ" --parent base-tof-0.1.1
```

版の名前（`0.2.0` など）は、車に入っているものと重ならない新しい番号にします。

`--description`（何の版か）、`--capability`（扱える場面：`walls` 壁、`traffic_light` 信号機、`vehicle` 車両、`intersection` 交差点。複数可）、`--scenario`（作るのに使った収録シナリオ。複数可）、`--parent`（元にした版）は任意ですが、付けておくと走行管理アプリの「判断データ」「対応機能」「学習元」と版の一覧に表示され、どのロジック／データで走っているかが分かります。`python car/tools/make_manifest.py show <フォルダ>` で確認できます。

### 6.3 車に送って切り替える

1. 走行管理アプリで車に接続し、「版の管理」の **「送る…」** でそのフォルダを選びます。アプリが manifest を確認してから送ります。
2. 一覧に新しい版が出たら、それを選んで **「有効化」**。車は検証してから次のフレームで切り替えます（走行中でも可）。検証に失敗した場合は今の版のまま走り続け、ログに理由が出ます。
3. 具合が悪ければ **「データを戻す」** で直前の版に戻ります。不要になった版は選んで「削除」（使用中と直前の版は削除できません）。

ロジック本体（`tof_drive` の .so）を作り直した場合も同じ手順で送れます（`make_manifest.py logic …`、仕様書 4 章）。

### 6.4 収録から動画を作る

走行管理アプリの「動画を作る…」で収録フォルダを選ぶと、`frames/` の画像から `video.avi` を作ります（Motion JPEG：受信した JPEG をそのまま並べるので画質は落ちません。20 コマ/秒、画像の時刻に沿った実時間。コマが届かなかった区間は直前のコマを最大 1 秒保持し、それより長い空白は詰めます）。VLC、Windows のメディア プレーヤー、ffmpeg で再生できます。Claude のデスクトップアプリから作るときは同じ処理の `python car/tools/session_video.py <収録フォルダ>` が使えます（Python 3 だけで動きます）。MP4 にしたいときは `ffmpeg -i video.avi -c:v libx264 -pix_fmt yuv420p video.mp4`。

## 7. 車に直接入る（SSH）

```sh
ssh rumicar@10.42.0.1          # パスワード rumicar
sudo journalctl -u rumicar-core -f     # 本体のログ（状態の遷移、切り替え、エラー）
systemctl status rumicar-core          # サービスの状態
rumicar-activate show                  # 使用中／直前の版と、最後の切り替え結果
sudo nano /etc/rumicar/core.json       # 設定（変更後は sudo systemctl restart rumicar-core）
```

主な設定（`/etc/rumicar/core.json`、仕様書 3.3）：`car_name`（車の名前）、`startup_mode`（起動時のモード：`record_only` 既定／`auto` 電源投入で走り出す）、`max_drive_permille`（駆動の上限、1000 = 100%）、`camera.enabled`、`camera.on_demand`（PC 接続中か自動モードのときだけカメラを動かす。既定 true）、`camera.fps`、`network.heartbeat_timeout_ms`、`network.ap_start`（アクセスポイントを `core` が立てる（既定）か `boot` で NetworkManager に任せるか）、`network.wifi_txpower_dbm`（無線の送信出力の上限。0 = 既定のまま。電池での電圧低下が気になるときに 10〜14 程度を試す）、`power.throttled_path`（低電圧・処理速度制限の監視。空で無効）。

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
| 走らない | 起動直後は「収録のみ」で待っています（アプリで「自動」を押す。PC なしで走らせるなら `startup_mode` を `auto` に）。`sudo journalctl -u rumicar-core -b` の先頭に `hardware bring-up failed` が無いか（I2C／PWM の設定は `/boot/firmware/config.txt` の末尾に追記されています）。非常停止がかかっていないか（アプリの「非常停止解除」）。ToF の値が `--`（読めず）のときは SENSOR_FAULT で止まります |
| 起動の途中で電源が落ちる・再起動する（乾電池） | 起動時に電気を食う部分（Wi-Fi、カメラ、モーター）が重ならないよう、v0.1.3 から本体がセンサー → Wi-Fi → カメラの順に立ち上げ、モーターは「自動」を押すまで止めています。それでも落ちるときは、電池を新しいものか充電式（ニッケル水素）にする、`network.wifi_txpower_dbm` を 10 程度にして無線の送信出力を下げる、アプリの「電源」欄に「低電圧」と出ていないか見る（出る機種なら原因の切り分けに使えます）。走行中に落ちるなら `max_drive_permille` を下げて加速時の電流を抑えます |
| Wi-Fi が途中で消えた | 本体（rumicar-core）が止まるとアクセスポイントも立ちませんが、`rumicar-ap.timer` が起動 90 秒後から 60 秒ごとに立て直します。`systemctl status rumicar-ap.timer` と `nmcli connection show --active` で確認します |
| `hardware bring-up failed: open /dev/i2c-1` | v0.1.1 のイメージはカーネルモジュール `i2c-dev` を読み込む設定が無く、`/dev/i2c-1` ができませんでした。`sudo raspi-config` → Interface Options → I2C → Yes で直ります（再起動後も有効）。v0.1.2 以降のイメージとパッケージでは不要です |
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

- 車：ToF 走行ロジック、制御ループと安全動作、走行モード（auto / manual / record_only、起動時は record_only）と非常停止、Camera Module 2 の取り込み（libcamera、RGB24 320×240・20 fps、必要なときだけ動かす）と JPEG 送信、制御サーバー（TCP 5600）・テレメトリサーバー（TCP 5601）、Wi-Fi AP（NetworkManager、本体が順序立てて起動）と `set_wifi`、低電圧・処理速度制限の監視、ロジック／パックの走行中差し替え（PC または ssh から）、manifest による検証と説明・対応機能・学習元の記載、systemd サービス、Debian パッケージ
- PC：制御・テレメトリ・manifest・収録の各形式と走行管理アプリ（接続、状態・画像、モード、非常停止、手動操作、シナリオ付き収録、版の管理、収録からの動画作成）
- 配布：標準 SD カードイメージ、タグで GitHub Release
- 未実装（次の版）：画像を使う走行ロジック

## ライセンス

MIT License（[LICENSE](LICENSE)）
