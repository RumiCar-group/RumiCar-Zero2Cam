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

## 車載側のビルド（Raspberry Pi OS Lite 64bit / Debian 13）

```sh
sudo apt install build-essential cmake pkg-config libgpiod-dev nlohmann-json3-dev libssl-dev
cmake -S car -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build --prefix /usr/local
```

## PC 側のビルド（Windows / .NET 10 SDK）

```sh
dotnet build pc/RumiCar.sln -c Release
dotnet test pc/RumiCar.Protocol.Tests -c Release
dotnet publish pc/RumiCar.DriveManager -c Release -r win-x64 --self-contained -p:PublishSingleFile=true
```

GitHub Actions が同じ手順で実行ファイルを作り、成果物（`car-arm64`、`RumiCarDriveManager-win-x64`）として保存します。

## 本版（0.1.0）の範囲

- 車：ToF 走行ロジック、制御ループと安全動作、ロジック／パックの走行中差し替え、manifest による検証、systemd サービス
- PC：制御・テレメトリ・manifest・収録の各形式と走行管理アプリの画面
- 未実装（次の版）：車側の制御／テレメトリサーバー、カメラ取り込み、Wi-Fi AP 設定、SD カードイメージ

## ライセンス

MIT License（[LICENSE](LICENSE)）
