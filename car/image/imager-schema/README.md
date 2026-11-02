# Raspberry Pi Imager の OS 一覧（repository JSON）のスキーマ

`os-list-schema.json` は [raspberrypi/rpi-imager](https://github.com/raspberrypi/rpi-imager) の
`doc/json-schema/os-list-schema.json`（commit 1ae2398、2026-10-29）の写しです。
`build-image.sh` が書く `rumicar-zero2cam.rpi-imager-manifest` を `check_manifest.py`
がこのスキーマで検証します（CI の image ジョブ）。Imager の版に合わせて更新するときは
同じ場所から写し直します。ライセンスは rpi-imager と同じ Apache License 2.0（`LICENSE.txt`）。
