#!/usr/bin/env python3
"""Write the Raspberry Pi Imager repository manifest for one RumiCar Zero2Cam image.

Called by build-image.sh after it compressed the image:

    write_manifest.py --out dist/rumicar-zero2cam.rpi-imager-manifest \
        --version 0.4.1 --image dist/rumicar-zero2cam-0.4.1-20261101.img.xz \
        --extract-size 3367763968 --extract-sha256 <sha256 of the .img> \
        --url https://github.com/RumiCar-group/RumiCar-Zero2Cam/releases/download/v0.4.1/rumicar-zero2cam-0.4.1-20261101.img.xz \
        --base-name 2026-10-01-raspios-trixie-arm64-lite --user rumicar --password rumicar

The format is Imager's "Repository JSON" (Imager 2.0; schema in imager-schema/):
Imager lists the entry under the device "Raspberry Pi Zero 2 W", downloads the
.img.xz from url, checks image_download_sha256, writes it and verifies the card
against extract_sha256.  init_format "none" tells Imager the image is
preconfigured (user, SSH, hostname, access point) so it offers no OS
customisation.  Standard library only.
"""
import argparse
import datetime
import hashlib
import json
import os

REPO = "https://github.com/RumiCar-group/RumiCar-Zero2Cam"
ICON = "https://raw.githubusercontent.com/RumiCar-group/RumiCar-Zero2Cam/main/car/image/icon.png"


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", required=True, help="package version, e.g. 0.4.1")
    ap.add_argument("--image", required=True, help="the compressed image (.img.xz)")
    ap.add_argument("--extract-size", required=True, type=int, help="bytes of the uncompressed image")
    ap.add_argument("--extract-sha256", required=True, help="SHA-256 of the uncompressed image")
    ap.add_argument("--url", required=True, help="where Imager downloads the .img.xz from")
    ap.add_argument("--base-name", required=True, help="the official image the build started from")
    ap.add_argument("--user", required=True)
    ap.add_argument("--password", required=True)
    ap.add_argument("--date", default=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"))
    args = ap.parse_args()

    doc = {
        "imager": {
            "devices": [
                {
                    "name": "Raspberry Pi Zero 2 W",
                    "description": "RumiCar Zero2Cam（Raspberry Pi Zero 2 W ＋ Camera Module 2）",
                    "tags": ["pi3-64bit", "pi3-32bit"],
                    "icon": "",
                    "default": True,
                    "matching_type": "inclusive",
                    "capabilities": [],
                }
            ]
        },
        "os_list": [
            {
                "name": "RumiCar Zero2Cam %s" % args.version,
                "description": ("Raspberry Pi OS Lite（64-bit、%s）＋ 車載ソフト rumicar-zero2cam %s。"
                                "ユーザー %s / %s、SSH 有効、Wi-Fi アクセスポイント「RumiCar」（車 10.42.0.1）。"
                                "設定済みのため OS customisation は不要"
                                % (args.base_name, args.version, args.user, args.password)),
                "icon": ICON,
                "website": REPO,
                "url": args.url,
                "extract_size": args.extract_size,
                "extract_sha256": args.extract_sha256,
                "image_download_size": os.path.getsize(args.image),
                "image_download_sha256": sha256_of(args.image),
                "release_date": args.date,
                "init_format": "none",
                "architecture": "armv8",
                "devices": ["pi3-64bit"],
            }
        ],
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
