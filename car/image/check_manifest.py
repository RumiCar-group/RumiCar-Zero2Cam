#!/usr/bin/env python3
"""Check the Raspberry Pi Imager manifest that build-image.sh writes.

    python3 car/image/check_manifest.py dist/rumicar-zero2cam.rpi-imager-manifest [--image dist/x.img.xz]

1. The file is JSON of the shape Imager expects (validated against the schema in
   imager-schema/ when the jsonschema module is available; the required keys and
   types are checked here in any case).
2. Every OS entry names the image it stands for: the url ends with the image's
   file name, image_download_size / image_download_sha256 match the .img.xz
   next to the manifest (or --image), and the sizes are plausible.
3. The entry declares init_format "none" (the image is preconfigured; Imager
   must not offer OS customisation) and a device tag of the Zero 2 W.

Exit code 1 with the reason when something is off.  Standard library only;
jsonschema is optional.
"""
import argparse
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA = os.path.join(HERE, "imager-schema", "os-list-schema.json")
REQUIRED = {"name": str, "description": str, "icon": str, "url": str, "extract_size": int,
            "extract_sha256": str, "image_download_size": int, "release_date": str, "devices": list}
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fail(msg):
    print("manifest check: " + msg, file=sys.stderr)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest")
    ap.add_argument("--image", help="the .img.xz the manifest describes (default: the one next to the manifest)")
    args = ap.parse_args()

    with open(args.manifest, encoding="utf-8") as f:
        doc = json.load(f)

    try:
        import jsonschema  # type: ignore
        with open(SCHEMA, encoding="utf-8") as f:
            schema = json.load(f)
        jsonschema.validate(doc, schema)
        print("manifest check: schema OK (jsonschema)")
    except ImportError:
        print("manifest check: jsonschema module not available; checking the required keys only")

    os_list = doc.get("os_list")
    if not isinstance(os_list, list) or not os_list:
        fail("os_list must be a non-empty array")
    images = []
    if args.image:
        images = [args.image]
    else:
        d = os.path.dirname(os.path.abspath(args.manifest))
        images = [os.path.join(d, n) for n in sorted(os.listdir(d)) if n.endswith(".img.xz")]
    if not images:
        fail("no .img.xz found next to the manifest (use --image)")

    for entry in os_list:
        for key, typ in REQUIRED.items():
            if key not in entry:
                fail("entry %r lacks %s" % (entry.get("name"), key))
            if not isinstance(entry[key], typ) or (typ is int and isinstance(entry[key], bool)):
                fail("entry %r: %s must be %s" % (entry.get("name"), key, typ.__name__))
        if not SHA256.match(entry["extract_sha256"]):
            fail("extract_sha256 is not a lowercase hex SHA-256")
        if entry.get("init_format") != "none":
            fail("init_format must be \"none\": the image is preconfigured and Imager must not customise it")
        if "pi3-64bit" not in entry["devices"]:
            fail("devices must include pi3-64bit (the tag Imager uses for the Zero 2 W, 64-bit)")
        if not re.match(r"^\d{4}-\d{2}-\d{2}$", entry["release_date"]):
            fail("release_date must be YYYY-MM-DD")
        if not re.match(r"^https://", entry["url"]):
            fail("url must be https")
        name = entry["url"].rsplit("/", 1)[-1]
        match = [p for p in images if os.path.basename(p) == name]
        if not match:
            fail("url names %s but that image is not here (%s)" % (name, ", ".join(os.path.basename(p) for p in images)))
        img = match[0]
        size = os.path.getsize(img)
        if entry["image_download_size"] != size:
            fail("image_download_size %d but %s is %d bytes" % (entry["image_download_size"], name, size))
        digest = sha256_of(img)
        if entry.get("image_download_sha256") != digest:
            fail("image_download_sha256 %s but %s is %s" % (entry.get("image_download_sha256"), name, digest))
        if not (entry["extract_size"] > size and entry["extract_size"] < 64 * 1024 ** 3):
            fail("extract_size %d is not larger than the compressed image or is implausible" % entry["extract_size"])
        print("manifest check: %s -> %s (%d bytes, sha256 %s...) OK" % (entry["name"], name, size, digest[:12]))


if __name__ == "__main__":
    main()
