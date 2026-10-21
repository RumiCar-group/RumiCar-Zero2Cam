#!/usr/bin/env python3
"""Create or refresh manifest.json for a pack or logic directory.

Usage:
  make_manifest.py pack  <dir> --id <pack-id> --version <ver> --logic <logic-name> [--min-abi 1]
                         [--description TEXT] [--capability NAME ...] [--scenario NAME ...] [--parent DIR-NAME]
  make_manifest.py logic <dir> --name <logic-name> --version <ver> --entry <file.so> [--abi 1] [--description TEXT]
  make_manifest.py verify <dir>
  make_manifest.py show <dir>

Every regular file in <dir> except manifest.json is listed with its size and
SHA-256 digest.  The file is written atomically (temp file + rename).
Standard library only, so it runs on the PC, inside the Claude desktop app
session and on the Raspberry Pi alike.

Metadata (all optional, shown by the drive manager and kept with recordings):
  --description  what this version is (one or two sentences, up to 500 bytes)
  --capability   what a pack handles; repeat for several, e.g.
                 --capability walls --capability traffic_light
                 (known names: walls, traffic_light, vehicle, intersection)
  --scenario     name of a recorded scenario the pack was built from; repeatable
  --parent       directory name of the version this one was derived from
"""
import argparse
import datetime as dt
import hashlib
import json
import os
import sys


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def list_files(root: str):
    out = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if rel == "manifest.json" or rel.endswith(".tmp"):
                continue
            out.append({"path": rel, "sha256": sha256_of(full), "bytes": os.path.getsize(full)})
    return sorted(out, key=lambda e: e["path"])


def write_atomic(path: str, text: str) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


MAX_DESCRIPTION = 500   # bytes (UTF-8); the same limits as car/core/src/manifest.hpp
MAX_LIST_ENTRIES = 32
MAX_LIST_ENTRY = 64


def metadata(a) -> dict:
    """Optional metadata from the command line, validated like the car does."""
    out = {}
    description = getattr(a, "description", None)
    if description:
        if len(description.encode("utf-8")) > MAX_DESCRIPTION:
            raise SystemExit(f"error: --description is longer than {MAX_DESCRIPTION} bytes")
        out["description"] = description
    for key in ("capability", "scenario"):
        values = getattr(a, key, None) or []
        if len(values) > MAX_LIST_ENTRIES:
            raise SystemExit(f"error: more than {MAX_LIST_ENTRIES} --{key} entries")
        for v in values:
            if not v or len(v.encode("utf-8")) > MAX_LIST_ENTRY:
                raise SystemExit(f"error: --{key} entries must be 1..{MAX_LIST_ENTRY} bytes")
        if values:
            out["capabilities" if key == "capability" else "scenarios"] = values
    parent = getattr(a, "parent", None)
    if parent:
        if len(parent.encode("utf-8")) > MAX_LIST_ENTRY or "/" in parent or "\\" in parent:
            raise SystemExit("error: --parent must be a plain directory name")
        out["parent"] = parent
    return out


def cmd_pack(a) -> int:
    files = list_files(a.dir)
    if not files:
        print("error: directory has no files", file=sys.stderr)
        return 1
    m = {
        "schema": 1,
        "kind": "pack",
        "id": a.id,
        "version": a.version,
        "created": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "logic": {"name": a.logic, "min_abi": a.min_abi},
    }
    m.update(metadata(a))
    m["files"] = files
    write_atomic(os.path.join(a.dir, "manifest.json"), json.dumps(m, indent=2, ensure_ascii=False) + "\n")
    print(f"wrote {os.path.join(a.dir, 'manifest.json')} ({len(files)} files)")
    return 0


def cmd_logic(a) -> int:
    files = list_files(a.dir)
    if not any(f["path"] == a.entry for f in files):
        print(f"error: entry {a.entry} not found in {a.dir}", file=sys.stderr)
        return 1
    m = {
        "schema": 1,
        "kind": "logic",
        "name": a.name,
        "version": a.version,
        "abi": a.abi,
        "entry": a.entry,
        "created": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    m.update(metadata(a))
    m["files"] = files
    write_atomic(os.path.join(a.dir, "manifest.json"), json.dumps(m, indent=2, ensure_ascii=False) + "\n")
    print(f"wrote {os.path.join(a.dir, 'manifest.json')} ({len(files)} files)")
    return 0


def cmd_show(a) -> int:
    """Print the identity and metadata of a pack or logic directory."""
    with open(os.path.join(a.dir, "manifest.json"), encoding="utf-8") as f:
        m = json.load(f)
    kind = m.get("kind", "?")
    ident = m.get("id") if kind == "pack" else m.get("name")
    print(f"{kind} {ident}-{m.get('version', '?')}")
    if kind == "pack":
        print(f"  logic:        {m.get('logic', {}).get('name', '?')} (min ABI {m.get('logic', {}).get('min_abi', '?')})")
    print(f"  created:      {m.get('created', '-')}")
    print(f"  description:  {m.get('description', '-')}")
    print(f"  capabilities: {', '.join(m.get('capabilities', [])) or '-'}")
    print(f"  scenarios:    {', '.join(m.get('scenarios', [])) or '-'}")
    print(f"  parent:       {m.get('parent', '-')}")
    print(f"  files:        {len(m.get('files', []))}")
    return 0


def cmd_verify(a) -> int:
    path = os.path.join(a.dir, "manifest.json")
    with open(path, encoding="utf-8") as f:
        m = json.load(f)
    if m.get("schema") != 1 or m.get("kind") not in ("pack", "logic"):
        print("error: unsupported manifest", file=sys.stderr)
        return 1
    ok = True
    listed = {f["path"] for f in m["files"]}
    for f in m["files"]:
        full = os.path.join(a.dir, f["path"])
        if not os.path.isfile(full):
            print(f"missing: {f['path']}")
            ok = False
            continue
        if os.path.getsize(full) != f["bytes"]:
            print(f"size mismatch: {f['path']}")
            ok = False
        if sha256_of(full) != f["sha256"]:
            print(f"sha256 mismatch: {f['path']}")
            ok = False
    for f in list_files(a.dir):
        if f["path"] not in listed:
            print(f"unlisted file: {f['path']}")
            ok = False
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sp = sub.add_parser("pack")
    sp.add_argument("dir")
    sp.add_argument("--id", required=True)
    sp.add_argument("--version", required=True)
    sp.add_argument("--logic", required=True)
    sp.add_argument("--min-abi", type=int, default=1)
    sp.add_argument("--description")
    sp.add_argument("--capability", action="append")
    sp.add_argument("--scenario", action="append")
    sp.add_argument("--parent")
    sp.set_defaults(func=cmd_pack)
    sl = sub.add_parser("logic")
    sl.add_argument("dir")
    sl.add_argument("--name", required=True)
    sl.add_argument("--version", required=True)
    sl.add_argument("--entry", required=True)
    sl.add_argument("--abi", type=int, default=1)
    sl.add_argument("--description")
    sl.set_defaults(func=cmd_logic)
    sv = sub.add_parser("verify")
    sv.add_argument("dir")
    sv.set_defaults(func=cmd_verify)
    ss = sub.add_parser("show")
    ss.add_argument("dir")
    ss.set_defaults(func=cmd_show)
    a = p.parse_args()
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
