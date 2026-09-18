#!/usr/bin/env python3
"""gen-index.py - InvariantFS registry index + per-pack sha256 generator (WP60).

Walks codecpacks/<name>/<version>/, parses each manifest, writes a
sha256sum-format '<version>/sha256' covering every file in the pack dir
except sha256 itself, then emits codecpacks/index.json.

Usage:
    tools/gen-index.py [--root .] [--platform x86_64-linux-musl] [--check]

--platform adds built artifacts found under <pack>/bin/ to the index
(used by CI release builds). --check additionally validates the required
manifest fields and exits non-zero on a malformed pack.
"""
import argparse
import hashlib
import json
import os
import re
import sys
from datetime import datetime, timezone

REQUIRED = ["name", "version", "pack_version", "min_read", "algo", "codec_id",
            "format_id", "family", "category", "priority", "type", "caps"]
FAMILIES = {"raw_audio", "raw_image", "container", "archive", "code"}
CATEGORIES = {"primary", "optional"}
TYPES = {"codec", "container"}
CAPS = {"seek", "batched", "wholefile", "container", "external"}


def split_list(value):
    parts = []
    for chunk in re.split(r"[,|]", value or ""):
        chunk = chunk.strip()
        if chunk:
            parts.append(chunk)
    return parts


def parse_manifest(path):
    """Parse a key = value manifest into a dict (last wins; sniff.* is a list)."""
    m = {}
    sniff_magic = []
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n").rstrip("\r").strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            key, val = line.split("=", 1)
            key = key.strip()
            val = val.strip()
            if key == "sniff.magic":
                sniff_magic.append(val)
            elif key == "sniff.ext":
                m["sniff.ext"] = split_list(val)
            else:
                m[key] = val
    if sniff_magic:
        m["sniff.magic"] = sniff_magic
    return m


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_sha256_manifest(packdir):
    files = []
    for dirpath, dirnames, filenames in os.walk(packdir):
        dirnames.sort()
        for fn in sorted(filenames):
            if fn == "sha256":
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, packdir)
            files.append(rel)
    files.sort()
    lines = []
    for rel in files:
        lines.append("%s  %s\n" % (sha256_file(os.path.join(packdir, rel)), rel))
    with open(os.path.join(packdir, "sha256"), "w", encoding="utf-8") as fh:
        fh.writelines(lines)
    return files


def num(m, key, default=0):
    try:
        return int(m.get(key, default))
    except (TypeError, ValueError):
        return default


def build_entry(root, name, version, platform):
    rel = os.path.join("codecpacks", name, version)
    packdir = os.path.join(root, rel)
    manifest_path = os.path.join(packdir, "manifest")
    if not os.path.isfile(manifest_path):
        return None
    m = parse_manifest(manifest_path)
    write_sha256_manifest(packdir)

    entry = {
        "name": m.get("name", name),
        "version": m.get("version", version),
        "pack_version": num(m, "pack_version", 1),
        "algo": num(m, "algo"),
        "codec_id": m.get("codec_id", ""),
        "format_id": m.get("format_id", ""),
        "family": m.get("family", ""),
        "category": m.get("category", ""),
        "priority": num(m, "priority"),
        "type": m.get("type", ""),
        "caps": split_list(m.get("caps", "")),
        "provides": split_list(m.get("provides", "")),
        "replaces": split_list(m.get("replaces", "")),
        "conflicts": split_list(m.get("conflicts", "")),
        "min_read": m.get("min_read", ""),
        "depends": split_list(m.get("depends", "")),
        "recommends": split_list(m.get("recommends", "")),
        "manifest": "%s/manifest" % rel.replace(os.sep, "/"),
        "sha256": "%s/sha256" % rel.replace(os.sep, "/"),
    }
    if m.get("description"):
        entry["description"] = m["description"]
    if m.get("sniff.ext"):
        entry["sniff_ext"] = m["sniff.ext"]

    artifacts = []
    bindir = os.path.join(packdir, "bin")
    if platform and os.path.isdir(bindir):
        for fn in sorted(os.listdir(bindir)):
            full = os.path.join(bindir, fn)
            if not os.path.isfile(full):
                continue
            artifacts.append({
                "platform": platform,
                "path": "%s/bin/%s" % (rel.replace(os.sep, "/"), fn),
                "sha256": sha256_file(full),
            })
    if artifacts:
        entry["artifacts"] = artifacts
    return entry


def validate(entry):
    errs = []
    for key in REQUIRED:
        if not entry.get(key) and entry.get(key) != 0:
            errs.append("missing required field '%s'" % key)
    if entry.get("family") and entry["family"] not in FAMILIES:
        errs.append("bad family '%s'" % entry["family"])
    if entry.get("category") and entry["category"] not in CATEGORIES:
        errs.append("bad category '%s'" % entry["category"])
    if entry.get("type") and entry["type"] not in TYPES:
        errs.append("bad type '%s'" % entry["type"])
    for c in entry.get("caps", []):
        if c not in CAPS:
            errs.append("bad cap '%s'" % c)
    if entry.get("version") and not re.match(r"^\d+\.\d+\.\d+", entry["version"]):
        errs.append("bad version '%s'" % entry["version"])
    return errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--platform", default=None,
                    help="add built bin/ artifacts under this platform tag")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    packs_dir = os.path.join(root, "codecpacks")
    if not os.path.isdir(packs_dir):
        sys.exit("gen-index: no codecpacks/ under %s" % root)

    entries = []
    had_err = False
    for name in sorted(os.listdir(packs_dir)):
        ndir = os.path.join(packs_dir, name)
        if not os.path.isdir(ndir):
            continue
        for version in sorted(os.listdir(ndir)):
            if not os.path.isdir(os.path.join(ndir, version)):
                continue
            entry = build_entry(root, name, version, args.platform)
            if not entry:
                continue
            if args.check:
                errs = validate(entry)
                for e in errs:
                    print("E: %s/%s: %s" % (name, version, e), file=sys.stderr)
                    had_err = True
            entries.append(entry)

    index = {
        "schema": 1,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "tool": "gen-index.py WP60",
        "packs": entries,
    }
    out = os.path.join(packs_dir, "index.json")
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(index, fh, indent=2, sort_keys=False)
        fh.write("\n")
    print("gen-index: wrote %s (%d packs)" % (out, len(entries)))
    if had_err:
        sys.exit(1)


if __name__ == "__main__":
    main()
