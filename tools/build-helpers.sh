#!/bin/bash
# build-helpers.sh - build the C codecpack helpers STATICALLY (WP60).
#
# Static is a *build* property of the registry, not a user-facing
# requirement: a musl or glibc-static binary runs on any host. Preference
# order:
#   1. musl-gcc / x86_64-linux-musl-gcc   (fully static, portable)
#   2. $CC -static                        (glibc-static fallback)
#   3. $CC                                (dynamic; WARN, dev/test only)
#
# Each codecpacks/<name>/<version>/<file>.c is compiled to
# codecpacks/<name>/<version>/bin/<file>. Python/mainless packs are skipped.
# After building, run tools/gen-index.py to refresh sha256 + index.json.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CHOICE="${INVFS_CC:-}"
if [ -z "$CHOICE" ]; then
    if command -v musl-gcc >/dev/null 2>&1; then CHOICE="musl-gcc"
    elif command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then CHOICE="x86_64-linux-musl-gcc"
    fi
fi

CFLAGS="${CFLAGS:--std=c11 -O2 -Wall -Wextra}"

is_static() { file "$1" 2>/dev/null | grep -q 'statically linked'; }

built=0
for src in codecpacks/*/*/*.c; do
    [ -e "$src" ] || continue
    dir="$(dirname "$src")"
    base="$(basename "$src" .c)"
    outdir="$dir/bin"
    out="$outdir/$base"
    mkdir -p "$outdir"

    ok=0
    if [ -n "$CHOICE" ]; then
        echo "  [musl] $src -> $out"
        if $CHOICE $CFLAGS -static -o "$out" "$src" 2>/tmp/gen-build.err; then ok=1; fi
    fi
    if [ "$ok" -eq 0 ]; then
        CC="${CC:-cc}"
        echo "  [static] $src -> $out ($CC -static)"
        if $CC $CFLAGS -static -o "$out" "$src" 2>/tmp/gen-build.err; then
            ok=1
        else
            echo "  [warn] -static failed for $src; building dynamically (dev/test only)" >&2
            sed 's/^/    /' /tmp/gen-build.err >&2 || true
            $CC $CFLAGS -o "$out" "$src"
        fi
    fi
    chmod +x "$out"
    if is_static "$out"; then
        echo "    static OK: $(file -b "$out" | cut -d, -f1-2)"
    else
        echo "    WARNING: $out is NOT statically linked" >&2
    fi
    built=$((built + 1))
done

echo "build-helpers: built $built helper(s)"
echo "next: tools/gen-index.py --root . --platform ${INVFS_PACK_PLATFORM:-$(uname -m)-linux-musl}"
