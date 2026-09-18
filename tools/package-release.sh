#!/bin/bash
# package-release.sh - build static helpers and stage release tarballs (WP60).
#
# For each codecpacks/<name>/<version> with a bin/ helper, emits
#   dist/<name>-<version>-<platform>.tar.gz
# containing <name>/<version>/{manifest,sha256,<sources>,bin/*}, and a
# refreshed codecpacks/index.json that records the artifact hashes.
#
# Run from the repo root:  tools/package-release.sh [platform]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PLATFORM="${1:-${INVFS_PACK_PLATFORM:-$(uname -m)-linux-musl}}"
DIST="$ROOT/dist"
rm -rf "$DIST"
mkdir -p "$DIST"

bash tools/build-helpers.sh
python3 tools/gen-index.py --root . --platform "$PLATFORM" --check

count=0
for dir in codecpacks/*/*; do
    [ -d "$dir" ] || continue
    name="$(basename "$(dirname "$dir")")"
    version="$(basename "$dir")"
    [ -d "$dir/bin" ] || continue
    stage="$DIST/stage"
    rm -rf "$stage"; mkdir -p "$stage/$name"
    cp -a "$dir" "$stage/$name/$version"
    tar -C "$stage" -czf "$DIST/${name}-${version}-${PLATFORM}.tar.gz" "$name/$version"
    echo "  packaged $name $version -> dist/${name}-${version}-${PLATFORM}.tar.gz"
    count=$((count + 1))
done
cp codecpacks/index.json "$DIST/index.json"
echo "package-release: $count pack(s), platform=$PLATFORM"
