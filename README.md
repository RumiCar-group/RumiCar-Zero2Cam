# RumiCar Zero2Cam

Raspberry Pi Zero 2 W と Camera Module 2 を搭載した RumiCar のソフトウェアです。
車載側（C++）、PC 側の走行管理アプリ（C# / .NET 10）、両者の通信形式、配布形式、自動ビルドを一つのリポジトリで管理します。

仕様は [docs/RumiCar-Zero2Cam 仕様書.docx](docs/RumiCar-Zero2Cam%20仕様書.docx) にまとめています。

## 構成

| ディレクトリ | 内容 |
|---|---|
| `car/` | 車載側。`rumicar-core`（制御ループ・ハードウェア・差し替え機構）、走行ロジック `tof_drive`（共有ライブラリ）、同梱パック `base-tof`、単体テスト、配布用ファイル |
| `pc/` | PC 側。`RumiCar.Protocol`（通信形式・manifest・収録）、`RumiCar.Protocol.Tests`、`RumiCar.DriveManager`（WPF の走行管理アプリ） |
| `docs/` | 仕様書 |
| `.github/workflows/` | 自動ビルド（`car.yml`: Debian 13 arm64/amd64、`pc.yml`: Windows） |

## 車への導入（標準 SD カードイメージ）

[Releases](https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases) の `rumicar-zero2cam-<版>-<日付>.img.xz` を Raspberry Pi Imager の「Use custom」で SD カードに書き込むと、起動後すぐ走り出し、Wi-Fi AP `RumiCar`（開放、車は 10.42.0.1）が立ちます。初期ユーザーは `rumicar` / `rumicar`（SSH 有効、ホスト名 `rumicar`）。イメージは `car/image/build-image.sh` で作られ（公式 Raspberry Pi OS Lite 64bit ＋ パッケージ ＋ 初回起動設定）、`vX.Y.Z` のタグを push すると CI が Release を作ります。

## 車載側のビルド（Raspberry Pi OS Lite 64bit / Debian 13）

```sh
sudo apt install build-essential cmake pkg-config libgpiod-dev nlohmann-json3-dev libssl-dev libturbojpeg0-dev libcamera-dev
cmake -S car -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build --prefix /usr/local   # 手動インストール
```

車に入れるときは Debian パッケージを使います（CI の成果物 `rumicar-zero2cam-deb-arm64`、または `cmake -S car -B build -DCMAKE_INSTALL_PREFIX=/usr` で構成して `cd build && cpack -G DEB`）。`sudo apt install ./rumicar-zero2cam_<version>_arm64.deb` で本体・同梱ロジック／パック・systemd サービス・Wi-Fi AP（SSID `RumiCar`、10.42.0.1）のプロファイルが入り、状態ディレクトリ `/var/lib/rumicar` が用意されます。

## PC 側のビルド（Windows / .NET 10 SDK）

```sh
dotnet build pc/RumiCar.sln -c Release
dotnet test pc/RumiCar.Protocol.Tests -c Release
dotnet publish pc/RumiCar.DriveManager -c Release -r win-x64 --self-contained -p:PublishSingleFile=true
```

GitHub Actions が同じ手順で実行ファイルを作り、成果物（`car-arm64`、`RumiCarDriveManager-win-x64`）として保存します。

## 本版（0.1.0）の範囲

- 車：ToF 走行ロジック、制御ループと安全動作、走行モード（auto / manual / record_only）と非常停止、Camera Module 2 の取り込み（libcamera、RGB24 320×240・20 fps）と JPEG 送信、制御サーバー（TCP 5600）・テレメトリサーバー（TCP 5601）、Wi-Fi AP（NetworkManager）と `set_wifi`、ロジック／パックの走行中差し替え（ssh または PC から）、manifest による検証、systemd サービス、Debian パッケージ
- PC：制御・テレメトリ・manifest・収録の各形式と走行管理アプリの画面
- 配布：標準 SD カードイメージ（`car/image/`）、タグで GitHub Release（イメージ・.deb・PC アプリ）
- PC → 車：走行管理アプリからロジック／データを送って有効化・ロールバック・削除
- 未実装（次の版）：画像を使う走行ロジック

libcamera 無しの環境では `-DRUMICAR_WITH_CAMERA=OFF` でビルドできます（その本体はカメラを起動せず ToF だけで走ります）。

## ライセンス

MIT License（[LICENSE](LICENSE)）
