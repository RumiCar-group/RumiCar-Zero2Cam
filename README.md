# RumiCar Zero2Cam

[English](README.en.md) | **日本語**

Raspberry Pi Zero 2 W と Camera Module 2 を搭載した RumiCar のソフトウェアです。
車載側（C++）、PC 側の走行管理アプリ（C# / .NET 10）、両者の通信形式、配布形式、自動ビルドを一つのリポジトリで管理します。

- 使い方（SD カードの作成から走行・収録・データの更新まで）：この README の [使い方](#使い方) 以下
- 仕様（設定、通信形式、配布形式、ビルド）：[docs/RumiCar-Zero2Cam 仕様書.docx](docs/RumiCar-Zero2Cam%20仕様書.docx)
- 設計の記録（調べたこと、比べた案、決めた理由、画像ロジックの設計と実装の経緯。学習の参考に）：[docs/RumiCar-Zero2Cam 設計の記録.docx](docs/RumiCar-Zero2Cam%20設計の記録.docx)
- 配布物：[Releases](https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases)

## 構成

| ディレクトリ | 内容 |
|---|---|
| `car/` | 車載側。`rumicar-core`（制御ループ・ハードウェア・通信・差し替え機構）、走行ロジック `tof_drive`・`cam_drive`・`py_drive`（共有ライブラリ。`py_drive` は自作の Python プログラムを動かす）、同梱パック `base-tof`・`cam-base`・`py-sample`、学習・再生ツール、単体テスト、Debian パッケージと SD イメージの作成 |
| `pc/` | PC 側。`RumiCar.Protocol`（通信形式・manifest・収録）、`RumiCar.Protocol.Tests`、`RumiCar.DriveManager`（WPF の走行管理アプリ） |
| `docs/` | 仕様書と設計の記録（どちらも .docx。教育目的で公開） |
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

車は **電源を入れただけでは走りません**。起動すると「収録のみ」（モーターはブレーキ）で待ち、走行管理アプリで **「自動」** を押すと走り出します（v0.1.3 から。それまでは電源投入で走り出していました）。PC なしで電源投入だけで走らせたいときは `/etc/rumicar/core.json` の `startup_mode` を `auto` にします（8 章）。初回起動や確認のときは、車輪が床に着かないよう台に載せてください。

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
3. 起動すると `rumicar-core` が動き始め、センサーの初期化 → Wi-Fi のアクセスポイント → （PC がつながったら）カメラ の順に立ち上がります。乾電池での電圧低下を避けるため、電気を食う部分は同時には起動しません（10 章）。**この時点では走りません**（収録のみ）。走らせるには 4 章の手順で接続して「自動」を押します。

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
4. 走らせるには、走行モードの **「自動」** を押します。初めて走らせるときや、まだ車やコースに慣れていないときは、その前に **「自動の最大出力」を 30〜50% に下げておく** と安心です（v0.3.0 時点のおすすめの値です。様子を見ながら上げてください。5 章）。

## 5. 走行管理アプリの使い方

| 部位 | 内容 |
|---|---|
| 車両／名前 | 車のアドレス（既定 `10.42.0.1`）と、収録フォルダ名に使う車の名前（既定 `RumiCar`） |
| 非常停止／非常停止解除 | 非常停止はモードに関係なくブレーキし、解除するまで走りません。手動モードで PC との通信が 1 秒途絶えたときや、手動モード中に切断したときも自動でかかります |
| 画像 | 車のカメラの最新画像（320×240、約 20 コマ/秒）。「画像を表示」のチェックを外すと表示だけ止まります（受信と収録は続きます。PC の負荷を下げたいときに） |
| 状態 | モード、非常停止（緑 = なし、赤 = 作動中・理由付き）、判断状態（CRUISE／TURN_LEFT／TURN_RIGHT／EMERGENCY_BRAKE／RECOVER_REVERSE／SENSOR_FAULT）、ToF 左／中／右 [mm]（`open` = 近くに何もない、`--` = 読めず）、モーター出力、ロジックの判断、版（ロジック／データ）、**判断データ**（使用中のパックの名前・版と説明）、**対応機能**（壁／信号機／車両／交差点）、**学習元**（そのパックを作るのに使った収録シナリオと元の版）、**電源**（車のファームウェアが報告する低電圧・処理速度制限。「正常（低電圧なし）」が通常） |
| モーター出力／ロジックの判断 | 「操舵 左／中央／右 / 前進 74.8% (191/255)」のように表示。% は駆動モーターの **PWM のデューティ比**（車は千分率 0〜1000 で扱い、1000 = 100%）、括弧内は同じ値を Arduino 版の `analogWrite` の 0〜255 に換算したもの（748‰ × 255 ÷ 1000 ≈ 191）。前進／後退／停止（惰行）／ブレーキ（両入力 High）を区別します。「モーター出力」は実際にモーターへ出した値、「ロジックの判断」はロジックがそのコマで決めた値（手動・収録のみ・非常停止中でも表示） |
| 走行モード | **自動**：車のロジックが走行／**手動**：PC から操作（下の「手動での操作」）／**収録のみ**：モーターを止めたまま画像と状態を受信（手で動かしてデータを集めるときに）。下に「起動時のモード」（既定は収録のみ）が出ます |
| 操縦パッド | 手動モードのときに使えます（下の「手動での操作」）。下に送っている指令（例「操舵 左 / 前進 45.0%（パッド）」）、「最大出力」、「停止」、「左右スクロールの向きを反転」があります |
| 自動の最大出力 | 「自動」でロジックが出す駆動の上限（既定 100%）。車が覚え、車を再起動すると 100% に戻ります。車の設定 `max_drive_permille` より上にはできません。手動の「最大出力」とは別です。また「自動」に切り替えたときは、この上限まで 0 から少しずつ速くなります（手動や収録のみから切り替えても、いきなり走り出しません）。初めての走行や慣れないコースでは 30〜50% から始めるのがおすすめです（v0.3.0 時点。慣れたら上げてください） |
| 収録 | 保存先（既定 `C:\Users\<ユーザー>\RumiCarData`）、**シナリオ**（この収録が何の場面か。例：「壁のみの走行データ」「信号機ありケース1」。入力するか、前に使った名前を選ぶ）、**メモ**（任意）、「収録開始／収録停止」。**既定では接続すると自動で収録が始まり**（「接続したら自動で収録を開始する」のチェック）、切断で閉じます。シナリオとメモは収録開始時の値が `session.json` に書かれます。受信した全コマを保存します。「受信コマ数」の（画像付き N）が増えていればカメラの画像が届いています |
| 動画を作る… | 収録フォルダを選ぶと、その画像から `video.avi`（Motion JPEG、実時間、20 コマ/秒）を同じフォルダに作ります。接続していなくても使えます（6.4） |
| 版の管理 | 車に入っているロジック／データの一覧（`[使用中]`、`[直前]`、検証失敗の表示。選ぶと説明・対応・学習元・どのロジック用かが下に出ます）。「一覧更新」「送る…」「自分のプログラムを送る…」（11 章）「有効化」「削除」「ロジックを戻す」「データを戻す」（6 章）。データ（パック）はロジックごとに作られているので、別のロジック用のデータを「有効化」すると、そのロジックも一緒に切り替わります（逆も同じ）。戻すときも同様に両方が戻ります |
| ログ | 接続・応答・イベント・エラーを時刻付きで表示。接続時に車の本体とこのアプリの版を出し、違う Release 同士なら注意を出します（9.4） |

タイトル（ウィンドウと画面左上）にアプリの版が出ます。その隣に **PC の Wi-Fi**（この PC がいまつながっているネットワークの名前）が出ます。車に接続したときのネットワーク（通常 `RumiCar`）から外れると赤くなり、ログにも出ます。Windows はインターネットにつながらないアクセスポイントから勝手に別のネットワークへ移ることがあるので、「急に通信が切れた」ときはまずここを見てください（10 章）。Windows 11 24H2 以降で名前が「読めません」と出るときは、Windows の 設定 → プライバシーとセキュリティ → 位置情報 →「デスクトップ アプリに位置情報へのアクセスを許可する」をオンにします（Windows がネットワーク名を位置情報の一部として扱うため）。

### 手動での操作

「手動」を押すと、走行カードの**操縦パッド**と、マウス・タッチパッドのスクロール、矢印キーで動かせます。車の操舵は左／中央／右の 3 段（モーターのオン・オフ）、駆動は 0〜100% の連続値です。

| 操作 | 操舵 | 走行 |
|---|---|---|
| **パッドを押したまま動かす**（マウスの左ボタン、タッチパッドはクリックしたまま、または「タップしてドラッグ」） | 左右の 1/3 に入ると左／右（中央に戻すと中央。境目で切り替わりがばたつかないように少し幅を持たせています） | 上半分 = 前進、下半分 = 後退。中心の横線から離すほど速く、端で「最大出力」。横線のまわりは停止。**離すと止まります** |
| **上下スクロール**（ホイール、タッチパッドの 2 本指で上下。パッドか画像の上で） | — | 速度を設定し、**そのまま走り続けます**。ホイール 1 目盛りで 5%。前進から後退へは一度 0 で止まり、指を止めてからもう一度スクロールすると後退に入ります |
| **左右スクロール**（2 本指で横、横に倒せるホイール、Shift ＋ ホイール） | 動かしている間だけ左／右、止めて 0.25 秒で中央 | — |
| **矢印キー** | ← → 押している間 | ↑ ↓ 押している間「最大出力」で前進／後退 |
| **停止** | Space、パッドの右クリック・中クリック、「停止」ボタン。ウィンドウから離れたとき（別のアプリに切り替えたとき）も止まります | |

優先順位は、パッドを押している間はパッド、そうでなければ押しているキー、そうでなければスクロール（←→ キーとスクロールの速度は組み合わせられます：スクロールで一定速度にして、← → で曲がる）。パッドや ↑ ↓ を押すとスクロールの設定速度は 0 に戻るので、離したときに前の速度で走り出すことはありません。「最大出力」（既定 60%）は 3 つの操作すべての上限で、車の設定 `max_drive_permille` でも制限されます。タッチパッドの設定によって左右が逆になるときは「左右スクロールの向きを反転」をオンにします。

手動モードで PC との通信が 1 秒途絶えると、車は非常停止します（5 章の表）。

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

## 7. 画像で学習する（信号機・前の車両・交差点）

### 7.1 しくみ

車は ToF（距離センサー）の走行を土台にしたまま、カメラの画像を「小さな格子の特徴量」（320×240 を 5 倍に縮めて 8×6 のセルに分け、セルごとに色の割合・明るさ・輪郭の量を数えた 576 個の整数）に変え、**収録データから学習した検出器**を当てはめます。検出器には、画像の「どこに」何があるかを答える**位置つき**（赤信号・青信号・車両。3×3 セルの窓で見る）と、画像全体の様子を答える**全体**（交差点が前にある、など）の 2 種類があります。検出したものから、3 つの振る舞いが ToF の走行に「速度の上限」と「操舵のしかた」を与えます。

| 振る舞い | 使う検出器 | すること |
|---|---|---|
| 信号機 `signal` | 赤信号・青信号（位置つき） | 赤を何コマか続けて見つけると減速、近づいたら止まり、青を見つける（または赤が消えてしばらく経つ）と走り出す |
| 車両 `vehicle` | 車両（位置つき） | 自分の車線（画像の中央の列）に車両が見えたら、曲がらずに車間距離を保って追従。止まったまま動かなければ「駐車車両」として、左右の ToF が空いている側から追い越す（はみ出す → 横を通る → 戻る） |
| 交差点 `intersection` | 交差点（全体。左に道がある／右に道がある） | 交差点が見えたら減速し、横の壁が切れた（横の ToF が開いた）ところで止まり、設定に従って直進・左折・右折する |

3 つは独立に動き、重なったときは**いちばん低い速度の上限**が効きます。追い越しや右左折の途中で赤信号が出れば操作を打ち切って止まります。障害物の非常ブレーキ・後退・センサー異常は ToF の判断がつねに優先します（画像の判断は ToF の上に重ねるだけで、ToF の安全側の動きを止めることはありません）。

学習したものは「判断データ（パック）」です。中身は `params.json`（ToF と各振る舞いの設定、人が読める）と `vision.bin`（検出器の重みなど。車が速く読める二進形式）と `manifest.json`（説明・対応機能・学習元）で、6 章と同じ手順で車に送って切り替えます。走行ロジック `cam_drive` は車に同梱されています（`tof_drive` と並んで入っています）。使う検出器が無い振る舞いは自動的にオフになるので、最初は信号機だけ、次に車両、と段階的に育てられます。

### 7.2 必要なもの

- Python 3 と numpy、Pillow（`pip install numpy pillow`）。Claude のデスクトップアプリで作業する場合はその環境に、自分で動かす場合は PC に入れます
- このリポジトリの `car/tools/`（`train_pack.py`、`rumicar_vision.py`、`make_manifest.py`、`session_video.py`）
- 任意：`rumicar-replay`（GitHub Actions の成果物 `rumicar-replay-amd64`。収録を本物のロジックで再生して、送る前に止まる・進むを確かめられます）

### 7.3 手順

1. **収録する**：信号機を置いたコースを走らせ（走り出す前に赤・青を見せるなら「収録のみ」で手で押しても構いません）、シナリオ名を「信号機ありケース1」のように付けて収録します。信号機の無いコースの収録（「壁のみの走行データ」）は「信号機が写っていない例」として自動的に使われるので、そちらも用意します。学習用と確認用に、別々の収録を 2 回以上取ります。
2. **ラベルを付ける**：`python car/tools/train_pack.py propose <収録フォルダ> --detector traffic_light_red` が、赤い塊の位置（コマ番号と矩形）を候補として `proposals_traffic_light_red.jsonl` に出します。Claude にそのフォルダを見せて、候補のコマ（`frames/` の画像）を確認させ、正しいものを `labels.jsonl` に書かせます。青信号も同様（`--detector traffic_light_green`）。候補の行はそのまま `labels.jsonl` の行の形で、余分なキー（`image`、`pixels` など、なぜ候補かの情報）は無視されるので、要らない行を消して貼り付ければ済みます。形式は次のとおりです（1 行 1 つ、矩形は画素座標 [x0, y0, x1, y1]、x1/y1 は含まない）。

```
{"index": 123, "objects": [{"name": "traffic_light_red", "box": [160, 60, 200, 110]}]}   ← 位置つき（矩形）
{"index": 456, "objects": [{"name": "vehicle", "box": [120, 140, 200, 220]}]}            ← 前の車両（車体を囲む）
{"from": 300, "to": 380, "scene": ["intersection_left"]}                                 ← 場面（画像全体）：300〜380 コマは左に道がある交差点が前にある
{"index": 200, "negative": true}                                                         ← 何も写っていないと明示
```

   車両の候補は `--detector vehicle` で出ます（ToF の中央が近かったコマ。矩形は車線の位置を示すだけなので、車体に合わせて直します）。交差点の候補は `--detector intersection_left`（または `_right`）で出ます。横の ToF が「壁」から「開いた」に変わった瞬間＝交差点を通過した瞬間から逆算して、その前 2 秒（`--before-ms`）を「交差点が前に見えていた区間」として提案します。最初と最後のコマを見て、区間を詰めます。ラベルの無いコマは学習に使いません。20 コマに 1 枚程度を見れば十分です（特徴が似ている間のコマは `propose` の候補で埋まります）。ラベルを付けたコマは、そこに書かれていない検出器にとっては「写っていない例」になります（車両の赤いテールランプは赤信号ではない、と教えるのはこの仕組みです）。

3. **学習する**：

```
python car/tools/train_pack.py train cam-signal-0.1.0 \
    --parent <車から取った cam-base-0.1.0 のフォルダ、または前の版> \
    --sessions <学習用の収録>... --validate <確認用の収録>... \
    --negatives-from-scenario "壁のみの走行データ" \
    --detector traffic_light_red --detector traffic_light_green \
    --id cam-signal --version 0.1.0 --description "信号機ありケース1"
```

   `cam-base-0.1.0` はリポジトリの `car/packs/cam-base` にもあります。終わると `cam-signal-0.1.0/` に `params.json`・`vision.bin`・`training.json`（学習の記録）・`manifest.json` ができ、確認用の収録に対する検出の適合率・再現率が表示されます。再現率が低ければラベルを足し、誤検出が多ければ負例（壁のみ）を足します。車両と交差点を足すときは、検出器を並べて一度に学習します（`:global` は全体の検出器）。

```
python car/tools/train_pack.py train cam-town-0.1.0 --parent cam-signal-0.1.0 \
    --sessions <信号機の収録> <車両と交差点の収録>... --validate <確認用>... \
    --negatives-from-scenario "壁のみの走行データ" \
    --detector traffic_light_red --detector traffic_light_green \
    --detector vehicle --detector intersection_left:global --detector intersection_right:global \
    --intersection turn=left --id cam-town --version 0.1.0 --description "信号機と交差点ロジックあり"
```

   振る舞いは、その検出器がパックにあるときだけ `params.json` に入ります（`behaviours on: signal, vehicle, intersection` と表示されます）。片側の交差点しか学習していなければ、もう片側は空のまま（その側は無い扱い）になります。

4. **送る前に確かめる**：`python car/tools/train_pack.py show cam-signal-0.1.0`（中身と、オンになっている振る舞い）、`replay cam-signal-0.1.0 <収録>`（コマごとの検出）、`rumicar-replay <cam_drive のフォルダ> cam-signal-0.1.0 <収録>`（本物のロジックで再生。CRUISE → SIGNAL_APPROACH → SIGNAL_STOP → SIGNAL_RESUME と並べば止まって進んでいます。車両なら FOLLOW → OVERTAKE_OUT → OVERTAKE_PASS → OVERTAKE_IN、交差点なら INTERSECTION_APPROACH → INTERSECTION_STOP → (INTERSECTION_CROSS →) INTERSECTION_TURN）。再生は収録した ToF の値をそのまま使うので、追い越しのように「動いた結果 ToF が変わる」場面は、収録のとおりにしか進みません（止まる・減速する・はみ出すところまでが確かめられます）。
5. **車に送る**：走行管理アプリの「版の管理」で `cam_drive-0.2.0`（同梱）を選んで「有効化」し、「送る…」で `cam-signal-0.1.0` を送って「有効化」します。状態欄の「判断データ」「対応機能」「学習元」に、送った版の説明・「壁 / 信号機」・学習に使った収録のシナリオが出ます。「判断を重ねる」をオンにすると、画像の上に検出した枠（赤信号 = 赤、青信号 = 緑、車両 = 水色、交差点 = 橙で画像全体を囲む。確定すると太枠）と確からしさが描かれます。「判断状態」には `FOLLOW（前の車両に追従）` のように状態の意味も出ます。
6. **改良する**：収録を足して `--parent cam-signal-0.1.0 --version 0.2.0` で学習し直します。新しい場面（車両、交差点）の収録を足したときは、古い検出器も含めて**全部を学習し直す**のが基本です（新しい場面が古い検出器の負例になるため。たとえば車両のテールランプを赤信号と間違えない）。`--keep-parent-detectors` は、場面が増えないまま一部の検出器だけ直すときのものです（manifest の `parent` に系譜が残ります）。

### 7.4 信号機の振る舞いの設定（params.json の signal）

| キー | 既定 | 意味 |
|---|---|---|
| `red_detector` / `green_detector` | `traffic_light_red` / `traffic_light_green` | 使う検出器の名前。青が無いコースなら `green_detector` を `""` にし、`resume_without_green_ms` で発進します |
| `stop_min_cells` | 2 | 赤がこのセル数以上に大きく見えたら「近い」として停止（それまでは減速だけ） |
| `approach_v` | 500 | 赤が見えてから止まるまでの速度の上限（千分率） |
| `min_stop_ms` | 1000 | 最低これだけは止まる |
| `resume_without_green_ms` | 3000 | 赤が見えなくなってからこの時間で発進（0 = 青が出るまで待つ） |
| `resume_ignore_ms` | 1500 | 発進後、この時間は赤を無視する（通り過ぎる間に止まり直さない） |

`train` の `--signal キー=値` で書き換えられます（例 `--signal stop_min_cells=3`）。

### 7.5 前の車両の振る舞いの設定（params.json の vehicle）

この節（`vehicle`）が `params.json` に無ければ振る舞いはオフです。あるときは全部のキーが必要です（`train` が検出器 `vehicle` のあるパックに自動で入れます。`--vehicle キー=値` で書き換え）。

| キー | 既定 | 意味 |
|---|---|---|
| `detector` | `vehicle` | 使う位置つき検出器の名前 |
| `lane_cols` | 4 | 検出した車両の「いちばん確からしいセル」が、格子の中央のこの列数（8 列なら 2〜5 列目）にあるときだけ「自分の車線」とみなす |
| `follow_gap_mm` | 150 | 前の車両との最低車間（ToF 中央）。これ以下で速度上限 0（止まる）。`tof.d_emergency_mm` より大きく、`tof.conf_max_mm` より小さいこと |
| `follow_band_mm` | 80 | 車間が `follow_gap_mm` からこの幅だけ広がる間に、上限を 0 から `follow_v` まで上げる |
| `follow_v` | 700 | 追従中の速度上限（千分率）。車両がカメラにだけ見えて ToF にはまだ見えない間もこの上限 |
| `parked_confirm_ms` | 2000 | 車両の後ろで止まったまま、車間が広がらずにこの時間が経ったら「駐車車両」 |
| `still_delta_mm` | 20 | 待つ間に車間がこれより広がったら「動き出した」として待ち直す |
| `overtake_side` | `open` | 追い越す側。`left`／`right`／`open`（左右の ToF の広い側。同じなら右） |
| `side_clear_mm` | 120 | その側の ToF がこれ以上（または何も無い）なら追い越せる。無ければ `FOLLOW_WAIT` で待つ |
| `overtake_v` | 500 | 追い越し中の速度上限 |
| `out_ms` | 500 | はみ出す（追い越す側へ全切り）時間 |
| `pass_min_ms` / `pass_max_ms` | 800 / 3000 | 横を通る（舵を戻して直進）時間の最短／最長 |
| `pass_clear_mm` / `pass_clear_ms` | 150 / 300 | 内側（車両のある側）の ToF が一度これ未満（車両を横に見た）になってから、これ以上がこの時間続いたら「通り過ぎた」 |
| `in_ms` | 500 | 戻る（反対側へ全切り）時間 |
| `ignore_ms` | 1500 | 追い越した後、車両の検出をこの時間無視する。また、追従中にカメラが車両を見失ったままこの時間が経つと追従をやめ、ToF の判断に戻す（近すぎて車両に見えないときは、ToF が障害物として回り込む） |

追い越しは時間で区切った開ループの操作です（操舵は左／中央／右の 3 段しか無いため）。はみ出す・戻る時間はコースと車の速さに合わせて調整してください。

### 7.6 交差点の振る舞いの設定（params.json の intersection）

この節（`intersection`）が無ければオフ。あるときは全部のキーが必要です（`train` が検出器 `intersection_left`／`intersection_right` のどちらかがあるパックに自動で入れます。`--intersection キー=値` で書き換え）。

| キー | 既定 | 意味 |
|---|---|---|
| `left_detector` / `right_detector` | `intersection_left` / `intersection_right` | 左に道がある交差点／右に道がある交差点の検出器（全体）。片方は `""` でもよい（両方は不可） |
| `approach_v` | 500 | 交差点が見えてから止まるまでの速度上限 |
| `stop_trigger` | `side_open` | 止まる合図。`side_open` = 左右どちらかの ToF が `open_mm` 以上に開いた（壁が切れた）とき。`detector` = 検出器が確定した時点ですぐ（壁の無いコースで、近い交差点だけを学習させたとき） |
| `open_mm` | 200 | 横の ToF がこれ以上（または何も無い）なら「開いている」 |
| `stop_ms` | 1000 | 止まる時間（赤信号が出ていれば、青になるまで発進しない） |
| `turn` | `straight` | 止まった後。`straight` 直進／`left`／`right`／`detector`（見つけた側へ曲がる。両方見えたら直進） |
| `turn_v` | 500 | 直進して待つ間と曲がる間の速度上限 |
| `cross_max_ms` | 2000 | 曲がる側の ToF が開くまで直進して待つ最長時間（開かなければ直進して終わる） |
| `turn_ms` | 900 | 曲がる側へ全切りする時間 |
| `ignore_ms` | 3000 | 終わった後、交差点の検出をこの時間無視する |

### 7.7 振る舞いの組み合わせ

- 速度の上限は 3 つの振る舞いの最小値です。追従中に赤信号が出れば `SIGNAL_STOP`、交差点に着けば `INTERSECTION_STOP` で止まります。
- 「駐車車両」の判定は、赤信号や交差点の処理中には進みません（信号待ちの車を追い越さない）。追い越しの途中で赤信号や交差点が出れば操作を打ち切って止まり、その後また追従からやり直します。
- 交差点の「止まる」は赤信号が消える（青）まで発進せず、直進・右左折の途中で赤信号が出れば打ち切ります。
- 「判断状態」に出る名前は、速度と操舵を決めているものです：赤信号（`SIGNAL_APPROACH`／`SIGNAL_STOP`）→ 交差点 → 車両 → 発進中（`SIGNAL_RESUME`）の順。ToF の非常ブレーキ・後退・センサー異常のときはそれが出ます。

### 7.8 うまくいかないとき

| 症状 | 対応 |
|---|---|
| 赤以外のもので止まる | 「壁のみ」の収録や、その物が写った収録を負例として足す。`training.json` の精度を見る。`--thr-on` を上げる |
| 止まる位置が遠い／近い | `stop_min_cells` を増やす／減らす。カメラの取り付け角度が収録時と同じか確認 |
| 止まったまま動かない | 青信号の検出器が青を見つけているか（「検出」欄）。無ければ `resume_without_green_ms` を設定 |
| 車両の後ろで止まらず回り込む | 「判断を重ねる」で車両の枠が中央の列（`lane_cols`）に出ているか。近づくと車体が大きく写って検出が外れるなら、近い収録のラベルを足す。`follow_gap_mm` を上げる |
| 追い越しがぶつかる／大回り | `out_ms`・`in_ms`・`overtake_v` を調整。はみ出す側の壁が近いなら `side_clear_mm` を上げる（その側では待つ） |
| 駐車車両を追い越さない | 「判断状態」が `FOLLOW_WAIT` なら両側とも `side_clear_mm` 未満。`FOLLOW` のままなら車両の検出が切れている（上の行）か、赤信号・交差点の処理中 |
| 交差点で止まらない | `replay` で `intersection_*` が確定しているか。確定していて壁のあるコースなら `open_mm` が壁の距離より小さいか確認。壁の無いコースは `stop_trigger` を `detector` に |
| 交差点で曲がらない | `cross_max_ms` 以内に曲がる側の ToF が開いていない（`open_mm`）。`turn=detector` で両側が見えると直進になる |
| 送った版が「検証失敗」になる | `make_manifest.py verify`。`vision.bin` の自己診断が合わないときはカメラの設定（320×240）が学習時と違う。学習ツールと車の版が合っているか（ABI 2） |
| `rumicar-replay` と車で結果が違う | 収録は JPEG、車は生画像なので小さな差はあります。大きく違うときは照明が変わっている可能性があるので、その照明で収録を足す |
| 検出が確定したまま消えない | `show` で `thr` の 2 つ目（解除のしきい値）が「何も無いときのスコア」より高いか。`train` は負例のスコアの上位 1% より上に置きますが、`--hysteresis` を小さくして学習し直すと上がります |

## 8. 車に直接入る（SSH）

```sh
ssh rumicar@10.42.0.1          # パスワード rumicar
sudo journalctl -u rumicar-core -f     # 本体のログ（状態の遷移、切り替え、エラー）
systemctl status rumicar-core          # サービスの状態
rumicar-activate show                  # 使用中／直前の版と、最後の切り替え結果
sudo nano /etc/rumicar/core.json       # 設定（変更後は sudo systemctl restart rumicar-core）
```

主な設定（`/etc/rumicar/core.json`、仕様書 3.3）：`car_name`（車の名前）、`startup_mode`（起動時のモード：`record_only` 既定／`auto` 電源投入で走り出す）、`max_drive_permille`（駆動の上限、1000 = 100%）、`auto_accel_permille_per_s`（「自動」に切り替えたときの立ち上がり。既定 1125 = 0.8 秒で 100%。0 で制限なし）、`camera.enabled`、`camera.on_demand`（PC 接続中か自動モードのときだけカメラを動かす。既定 true）、`camera.fps`、`network.heartbeat_timeout_ms`、`network.ap_start`（アクセスポイントを `core` が立てる（既定）か `boot` で NetworkManager に任せるか）、`network.wifi_txpower_dbm`（無線の送信出力の上限。0 = 既定のまま。電池での電圧低下が気になるときに 10〜14 程度を試す）、`power.throttled_path`（低電圧・処理速度制限の監視。空で無効）。

## 9. 更新する

### 9.1 イメージごと

新しい Release のイメージを SD カードに書き直します（2 章）。収録データは PC 側にあるので失われません。

### 9.2 パッケージだけ

Release の `rumicar-zero2cam_<版>_arm64.deb` を車に送って入れます（走行中に入れると切り替えのためモーターは一度止まります）。

```sh
scp rumicar-zero2cam_<版>_arm64.deb rumicar@10.42.0.1:
ssh rumicar@10.42.0.1
sudo apt install ./rumicar-zero2cam_<版>_arm64.deb
```

必要なライブラリはイメージに入っているので、インターネット接続は不要です。`/etc/rumicar/core.json` やアクセスポイントの設定ファイルを自分で変えていた場合は「置き換えるか」と聞かれます（変えていなければ聞かれずに更新されます）。

### 9.3 車をインターネットにつなぐ必要があるとき

車は普段アクセスポイントなので外に出ません。`apt` で何かを入れたいときは、一時的に家庭の Wi-Fi に切り替えます（切り替えると `RumiCar` の AP は止まります。再起動で元に戻ります）。

```sh
sudo nmcli device wifi connect "<家庭のSSID>" password "<パスワード>"
```

### 9.4 版番号の読み方

版番号は 3 種類あり、それぞれ別々に進みます。

| 種類 | 例 | 何の番号か・どこに出るか |
|---|---|---|
| 本体（製品）の版 | `v0.3.0` | Release のタグ。SD カードイメージ・`.deb`・走行管理アプリ・車の `core_version` が同じ番号。接続時のログの `本体 <版>, このアプリ <版>`、アプリのタイトル |
| ロジックの版 | `tof_drive-0.2.0`、`cam_drive-0.2.0` | ロジックごとの番号（`car/CMakeLists.txt` の `TOF_DRIVE_VERSION`／`CAM_DRIVE_VERSION`、manifest の `version`）。「版の管理」の一覧と状態欄の「版（ロジック／データ）」 |
| 判断データ（パック）の版 | `base-tof-0.1.1`、`cam-base-0.1.0`、自分で作った `cam-signal-0.1.0` | パックごとの `manifest.json` の `version`。自分で学習させた版は、車に入っている版と重ならない番号を付けます（7 章） |

本体 0.3.0 の Release に同梱されているのは `tof_drive-0.2.0`・`cam_drive-0.2.0`・`py_drive-0.1.0`・`base-tof-0.1.1`・`cam-base-0.1.0`・`py-sample-0.1.0` です。本体とロジックの番号がたまたま同じでも、別の数え方です。

**Release ではないビルド**（GitHub Actions がタグ以外で作った `.deb`・SD カードイメージ・アプリ）は、版の後ろに `+g` と commit の先頭 7 文字が付きます（例 `0.2.1+g1a2b3c4`）。タグの付いた Release の物と取り違えないためです。Debian の版の比較では `0.2.1` ＜ `0.2.1+g1a2b3c4` ＜ `0.2.2` です。車の本体とアプリの版の数字（`+` より前）が違うと、接続時のログに注意が出ます。

## 10. 困ったとき

| 症状 | 確認すること |
|---|---|
| Wi-Fi に `RumiCar` が出ない | 起動後 1〜2 分待つ。SSH が使えるなら `nmcli connection show` に `RumiCar-AP` があるか、`sudo journalctl -u NetworkManager -b` にエラーが無いか、`iw reg get` が `JP` か |
| アプリが接続できない | PC が `RumiCar` につながり `10.42.0.x` のアドレスを得ているか（`ipconfig`）。ファイアウォールの許可。車で `systemctl status rumicar-core` |
| 画像が出ない | 車で `sudo journalctl -u rumicar-core -b | grep -i camera`。カメラのケーブルの向き。`rpicam-hello --list-cameras` でカメラが見えるか |
| 走らない | 起動直後は「収録のみ」で待っています（アプリで「自動」を押す。PC なしで走らせるなら `startup_mode` を `auto` に）。`sudo journalctl -u rumicar-core -b` の先頭に `hardware bring-up failed` が無いか（I2C／PWM の設定は `/boot/firmware/config.txt` の末尾に追記されています）。非常停止がかかっていないか（アプリの「非常停止解除」）。ToF の値が `--`（読めず）のときは SENSOR_FAULT で止まります |
| 起動の途中で電源が落ちる・再起動する（乾電池） | 起動時に電気を食う部分（Wi-Fi、カメラ、モーター）が重ならないよう、v0.1.3 から本体がセンサー → Wi-Fi → カメラの順に立ち上げ、モーターは「自動」を押すまで止めています。それでも落ちるときは、電池を新しいものか充電式（ニッケル水素）にする、`network.wifi_txpower_dbm` を 10 程度にして無線の送信出力を下げる、アプリの「電源」欄に「低電圧」と出ていないか見る（出る機種なら原因の切り分けに使えます）。走行中に落ちるなら `max_drive_permille` を下げて加速時の電流を抑えます |
| Wi-Fi が途中で消えた | 本体（rumicar-core）が止まるとアクセスポイントも立ちませんが、`rumicar-ap.timer` が起動 90 秒後から 60 秒ごとに立て直します。`systemctl status rumicar-ap.timer` と `nmcli connection show --active` で確認します |
| PC が `RumiCar` から勝手に外れる | 車の AP はインターネットにつながらないので、Windows が「使えないネットワーク」とみなして、覚えている別の Wi-Fi に移ることがあります。アプリの「PC の Wi-Fi」が赤くなったらこれです。対策：Windows の Wi-Fi 設定で `RumiCar` を「自動的に接続」にし、ほかのネットワーク（自宅や職場の Wi-Fi）の「自動的に接続」を走らせる間だけ外す。または、ほかのネットワークが届かない場所で走らせる。有線 LAN でインターネットにつないでいる PC は、Wi-Fi を車専用にできます |
| 「自動」にしたとたん飛び出す | v0.3.0 から、自動に切り替えたときは 0 から少しずつ速くなります（`auto_accel_permille_per_s`、既定 1125 = 約 0.9 秒で 100%）。速すぎるときはアプリの「自動の最大出力」を下げます（再起動で戻ります。初めての走行は 30〜50% から）。常に抑えるなら車の `max_drive_permille` |
| 自分のプログラムが動かない | アプリのログに理由（`drive.py の N 行目 …`）が出ます。走り出した後のエラーと `print()` は車のログ（`sudo journalctl -u rumicar-core -f`）。`loop()` が 30 ms 以内に終わらないコマはブレーキ、1 秒以上答えが無いと止まります（`time.sleep()` や無限ループは使わない）。11 章 |
| `hardware bring-up failed: open /dev/i2c-1` | v0.1.1 のイメージはカーネルモジュール `i2c-dev` を読み込む設定が無く、`/dev/i2c-1` ができませんでした。`sudo raspi-config` → Interface Options → I2C → Yes で直ります（再起動後も有効）。v0.1.2 以降のイメージとパッケージでは不要です |
| 切り替えに失敗する | アプリのログの理由（manifest の不一致、必須キー不足、ロジック名の不一致など）。車の `rumicar-activate show` |
| SSH のパスワードが通らない | 初回起動の設定が反映される前の可能性。再起動して数分待つ |

アプリのログ欄の内容か `sudo journalctl -u rumicar-core -b` の出力を添えて [Issues](https://github.com/RumiCar-group/RumiCar-Zero2Cam/issues) に書いてください。

## 11. 自分の走行プログラムを書く（Python）

同梱の走行ロジック（`tof_drive`、`cam_drive`）のほかに、**自分で書いた Python のプログラム**で車を走らせられます。ロジック `py_drive` が、送られたパックの `drive.py` を車の上で動かします。学習の仕組み（7 章）とは別に、「距離センサーの値を見て、操舵と駆動を決める」ところを自分の考えで書くためのものです。コンパイラや開発環境は要りません。テキストエディタ（メモ帳でも可）と走行管理アプリだけで書いて送れます。

### 11.1 はじめかた

1. リポジトリの `car/packs/py-sample/` にある見本（`drive.py` と `params.json`）を、PC の好きな場所のフォルダ（例 `C:\Users\<ユーザー>\RumiCarData\my-drive`）にコピーします。フォルダの名前がそのままデータの名前になります（英数字以外は `-` になります）。
2. `drive.py` を開いて、`loop(car)` の中を書き換えます（最初はそのままで構いません）。
3. 車に接続し、「版の管理」の **「自分のプログラムを送る…」** でそのフォルダを選びます。アプリが `manifest.json` を作って送り、ロジック `py_drive` とそのデータを有効にします（ログに「有効化: ロジックも py_drive-… に切り替えます」と出ます）。
4. 「自動」を押すと走ります。最初は「自動の最大出力」を 30〜50% にしておくと安心です。
5. 書き換えたら、もう一度「自分のプログラムを送る…」。送るたびに新しい版（送った日時が版の番号）になり、前の版は「データを戻す」で戻せます。

走行管理アプリのログに `drive.py の N 行目 …` と出たら、その行を直します。`print()` の出力と走り出した後のエラーは車のログに出ます（`ssh rumicar@10.42.0.1` で `sudo journalctl -u rumicar-core -f`）。

### 11.2 drive.py の書き方

```python
def setup(car):          # 始まるときに 1 回（無くてもよい）
    car.state("START")

def loop(car):           # 1 秒に 20 回（50 ms ごと）
    if car.center < 150:           # 前が近い
        car.steer(car.LEFT if car.left > car.right else car.RIGHT)   # 開いている側へ
        car.drive(400)
    else:
        car.steer(car.CENTER)
        car.drive(car.params["speed"])   # params.json の値
```

| 使えるもの | 意味 |
|---|---|
| `car.left` / `car.center` / `car.right` | 距離センサーの値（mm）。何も近くに無いときは `car.FAR`（8190）、読めないときは `None` |
| `car.params` | `params.json` の内容（辞書）。車やコースごとに変える数はここに書くと、プログラムを直さずに済みます |
| `car.time` / `car.dt` / `car.frame` | 始まってからの秒数／前回の `loop` からの秒数／コマ数 |
| `car.steer(向き)` | `car.LEFT`／`car.CENTER`／`car.RIGHT`（`"left"` などの文字でも可）。車の操舵は 3 段です |
| `car.drive(数)` | 駆動。`-1000`（全力で後退）〜 `0`（止まる）〜 `1000`（全力で前進）。`steer()` と `drive()` の値は次に変えるまで続きます |
| `car.brake()` / `car.stop()` | ブレーキ／駆動を 0 に（惰性で止まる） |
| `car.state("名前")` | 今していることの名前（英数字と `_`）。アプリの「判断状態」に出ます |
| `print(...)` | 車のログに出ます |

`drive.py` と同じフォルダに自分の `.py` を置いて `import` することもできます。使えるのは Python の標準ライブラリだけです（車には numpy などは入っていません）。カメラの画像は `py_drive` では使えません（画像は 7 章の学習の仕組みで）。

### 11.3 安全のために本体がしていること

- `loop()` 1 回に **30 ms** の時間を与え、間に合わなかったコマはブレーキします。**1 秒**答えが無いとプログラムを止め、車はブレーキしたままになります（もう一度「有効化」で再開）。`time.sleep()` や終わらない `while` は書かないでください。
- エラー（例外）が出たコマはブレーキし、プログラムは続きます。範囲外の値（`drive(5000)` など）もエラーです。
- 「自動」にしたときは 0 から少しずつ速くなり、「自動の最大出力」と車の `max_drive_permille` より速くは走りません。手動モード・収録のみ・非常停止は、どのロジックでも同じです。
- プログラムは本体とは別のプロセスで動くので、プログラムが固まっても本体の制御周期（50 ms）は守られます。

### 11.4 もう少し

- 送られたパックは `/var/lib/rumicar/packs/<フォルダ名>-<日時>/` に入り、`rumicar-activate pair py_drive-0.1.0 <パック>` で ssh からも切り替えられます（ロジックとデータは組で切り替えます）。
- ToF の走行（`tof_drive`）は C++ で書かれています。Python で書いたものを本物の走行ロジックに「昇格」させたいときは、`car/logic/tof_drive/` が手本です。
- 見本 `py-sample` は車にも同梱されているので、PC にリポジトリが無くても「版の管理」から `py-sample-0.1.0` を有効化して動きを見られます。

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

走行ロジックは `car/logic/` にあります：`tof_drive`（ToF 走行）、`cam_drive`（その上にカメラの判断を足したもの：`vision_features` 特徴量、`vision_pack` の `vision.bin`、`vision_eval` 検出器の評価と時間確定、`cam_logic` 信号機・車両・交差点の振る舞いと優先順位）、`py_drive`（パックの `drive.py` を `runner.py` が別プロセスで動かし、`py_process` がコマごとに 1 行で送受信。時間切れ・異常終了でブレーキ。`test_py_drive` が本物の runner で確かめます）。本体とロジックの契約は `car/include/rumicar/logic_api.h`（ABI 2：補助出力 `aux` と `aux_name`）です。特徴量と `vision.bin` の Python 実装は `car/tools/rumicar_vision.py` で、`car/tests/data/vision` の黄金データ（`make_golden.py` で生成）を介して C++ と完全一致することを `test_vision` が確かめます。`car/tests/tools_smoke.py <build dir>` は合成収録から学習して `rumicar-replay` で止まる・進む、追従して追い越す、交差点で止まるまでを通しで確かめます（CI でも実行）。

## PC 側のビルド（Windows / .NET 10 SDK）

```sh
dotnet build pc/RumiCar.sln -c Release
dotnet test pc/RumiCar.Protocol.Tests -c Release
dotnet publish pc/RumiCar.DriveManager -c Release -r win-x64 --self-contained -p:PublishSingleFile=true
```

## SD カードイメージと Release

- イメージは `car/image/build-image.sh` で作ります（root、arm64 ホスト。公式 Raspberry Pi OS Lite 64bit にパッケージを chroot で導入し、初回起動の設定を加えます）。CI では main への push とタグで `car.yml` の `image` ジョブが作ります。
- Release は、main のコミットに `vX.Y.Z` のタグを付けて push する（または GitHub の Releases ページで新しいタグを指定して Release を作る）と `release.yml` が走り、イメージ・.deb・PC アプリ・`SHA256SUMS.txt` を添付します。
- 本体の版は、**次に出す Release の番号をソースに先に書いてからタグを付けます**。手順：(1) プルリクエストで `car/CMakeLists.txt` の `RUMICAR_VERSION`、`pc/RumiCar.DriveManager/RumiCar.DriveManager.csproj` の `<Version>`、`pc/RumiCar.DriveManager/app.manifest` の `assemblyIdentity version`（`X.Y.Z.0`）を同じ番号にする、(2) マージする、(3) そのマージの commit に `vX.Y.Z` のタグを付けて push する。`release.yml` は最初にタグとこの 3 か所が一致し、タグが main の commit であることを確かめ、違えばビルドせずに止まります（タグと中身の番号が食い違った Release を作らないため）。
- タグ以外のビルド（main・プルリクエスト）では、CI が版の後ろに `+g<commit>` を付けます（`car.yml` の `-DRUMICAR_VERSION_SUFFIX`、`pc.yml` の `-p:InformationalVersion`）。手元でのビルドはソースの番号のままです。ロジックの版（`TOF_DRIVE_VERSION`／`CAM_DRIVE_VERSION`）は本体の版とは別に、ロジックを変えたときに上げます（9.4）。

## 本版（0.3.0）の範囲

- 車：ToF 走行ロジック、制御ループと安全動作、走行モード（auto / manual / record_only、起動時は record_only）と非常停止、Camera Module 2 の取り込み（libcamera、RGB24 320×240・20 fps、必要なときだけ動かす）と JPEG 送信、制御サーバー（TCP 5600）・テレメトリサーバー（TCP 5601）、Wi-Fi AP（NetworkManager、本体が順序立てて起動）と `set_wifi`、低電圧・処理速度制限の監視、ロジック／パックの走行中差し替え（PC または ssh から）、manifest による検証と説明・対応機能・学習元の記載、systemd サービス、Debian パッケージ
- PC：制御・テレメトリ・manifest・収録の各形式と走行管理アプリ（接続、状態・画像、モード、非常停止、手動操作（操縦パッド・スクロール・キー）、自動の最大出力、PC の Wi-Fi の表示、シナリオ付き収録、版の管理（ロジックとデータの組での切り替え）、自分のプログラムの送信、収録からの動画作成）
- 車：画像ロジック `cam_drive`（信号機で停止・発進、前の車両への追従と駐車車両の追い越し、交差点での停止・直進・右左折）、学習データ `vision.bin`、学習ツール `train_pack.py`、再生ツール `rumicar-replay`、ロジック契約 ABI 2
- 車：自作の走行プログラム（Python の `drive.py`）を走らせるロジック `py_drive` と見本 `py-sample`、自動モードの立ち上がり制限と最大出力（`set_auto_limit`）、ロジックとデータの組での切り替え（`activate_pair`）
- 配布：標準 SD カードイメージ、タグで GitHub Release（0.2.0：ロジック契約が ABI 1 から 2 に変わり、自作ロジックは作り直しが必要なため 0.1.x から繰り上げ。0.2.1：手動操作を操縦パッドとスクロールに、タグとソースの版の一致を Release で検査、Release でないビルドに `+g<commit>`。0.3.0：自作の走行プログラム、自動の最大出力と急発進の防止、PC の Wi-Fi 表示）
- これから：実車での収録と調整（追い越し・右左折の時間、交差点の見え方の学習）、場面に応じた右左折の選び方（今は版ごとに固定か、見つけた側へ）

## ライセンス

MIT License（[LICENSE](LICENSE)）
