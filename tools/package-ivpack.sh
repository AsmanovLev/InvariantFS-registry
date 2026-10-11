#!/bin/bash
# package-ivpack.sh - build .ivpack deliverables, one per pack (WP609 round 2).
#
# Layout mirrors the canonical engine flow
# (main-repo tools/pack-ivpack.sh + ADR-007 §3: uncompressed ZIP-0):
#   <name>-<version>.ivpack (zip -0 -X)
#   ├── manifest          # the pack's own manifest, verbatim
#   ├── sha256            # `sha256sum -c`-able list of every other entry
#   ├── lib/<name>.so     # -fPIC -shared build of the pack's helper source,
#   │                     # where the source supports it (see below)
#   └── bin/*             # the existing static helper(s) as fallback, plus
#                         # any *.py helper scripts (as bin/<script>.py)
#
# .so policy (loud, never silent):
#   * pack with a *.c helper source -> compile `cc -fPIC -shared` into
#     lib/<name>.so. NOTE: these sources are CLI-helper mains, NOT ivpack
#     C ABI plugins; the .so is a loadable shared object built from that
#     source (dlopen smoke in verify-ivpacks.sh). Engine-ABI entry points
#     are the client team's (WP608r3) verification duty -- see
#     docs/IVPACK-RELEASES.md.
#   * pack with only *.py helpers (raw_image, splt_test) -> manifest +
#     bin/*.py only; the CI log says NO_SO_PY per pack.
#   * pack with no helper source at all (ape, flacr, wavpack) ->
#     manifest only; the CI log says NO_SO_NOSRC per pack.
#
# Asset file names are <name>-<version>.ivpack (ADR-007 §2 on-volume
# naming, e.g. qcow2-1.0.0.ivpack). Pack sources stay at their registry
# versions (1.0.0); the v1.1.0 tag marks the new deliverable, not new
# pack sources.
#
# Output goes to dist/ivpack/ WITHOUT wiping dist/ (package-release.sh
# owns dist/ for tarballs; this script only adds dist/ivpack/).
#
# Run from the repo root:  bash tools/package-ivpack.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

for t in zip sha256sum; do
    command -v "$t" >/dev/null || { echo "Error: $t not installed" >&2; exit 1; }
done

OUTDIR="$ROOT/dist/ivpack"
mkdir -p "$OUTDIR"

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=c11 -O2 -Wall -Wextra}"

count=0; with_so=0; no_so_py=0; no_so_nosrc=0
for dir in codecpacks/*/*; do
    [ -d "$dir" ] || continue
    name="$(basename "$(dirname "$dir")")"
    version="$(basename "$dir")"
    [ -f "$dir/manifest" ] || { echo "  SKIP $name $version: no manifest"; continue; }

    STAGE="$(mktemp -d "${TMPDIR:-/tmp}/ivpack_build.XXXXXX")"
    CONTENT="$STAGE/content"
    mkdir -p "$CONTENT/lib" "$CONTENT/bin"

    # manifest, verbatim
    cp "$dir/manifest" "$CONTENT/manifest"

    # lib/<name>.so from the pack's C helper source, where one exists
    csrc=( "$dir"/*.c )
    if [ -e "${csrc[0]}" ]; then
        echo "  [so] $name: building lib/$name.so from ${csrc[*]##*/} ($CC -fPIC -shared)"
        if $CC $CFLAGS -fPIC -shared -o "$CONTENT/lib/$name.so" "$dir"/*.c 2>"$STAGE/so-build.err"; then
            chmod 0755 "$CONTENT/lib/$name.so"
            echo "  [so] $name: OK ($(stat -c%s "$CONTENT/lib/$name.so") bytes)"
            with_so=$((with_so + 1))
        else
            echo "  [so] $name: SHARED BUILD FAILED -- shipping without lib/ (see below)" >&2
            sed 's/^/    /' "$STAGE/so-build.err" >&2 || true
            rm -f "$CONTENT/lib/$name.so"
            echo "  NO_SO_BUILDFAIL $name $version: -fPIC -shared failed, manifest+bin only"
            no_so_nosrc=$((no_so_nosrc + 1))
        fi
        rmdir "$CONTENT/lib" 2>/dev/null || true
    else
        if ls "$dir"/*.py >/dev/null 2>&1; then
            echo "  NO_SO_PY $name $version: .py helper(s) cannot yield a .so; manifest+bin only"
            no_so_py=$((no_so_py + 1))
        else
            echo "  NO_SO_NOSRC $name $version: no helper source at all; manifest only"
            no_so_nosrc=$((no_so_nosrc + 1))
        fi
        rmdir "$CONTENT/lib" 2>/dev/null || true
    fi

    # bin/: every built static helper + every *.py helper script
    if [ -d "$dir/bin" ]; then
        for b in "$dir"/bin/*; do
            [ -f "$b" ] || continue
            cp "$b" "$CONTENT/bin/$(basename "$b")"
            chmod 0755 "$CONTENT/bin/$(basename "$b")"
        done
    fi
    for extra in "$dir"/*.py; do
        [ -e "$extra" ] || continue
        cp "$extra" "$CONTENT/bin/$(basename "$extra")"
    done
    rmdir "$CONTENT/bin" 2>/dev/null || true

    # sha256 covers every entry except itself, paths relative to the
    # archive root so `sha256sum -c sha256` works in an unpacked tree.
    ( cd "$CONTENT" && find . -type f ! -name sha256 -print0 \
        | sort -z | sed -z 's|^\./||' \
        | xargs -0 sha256sum > sha256 )

    out="$OUTDIR/${name}-${version}.ivpack"
    rm -f "$out"
    # -X: no extra file attributes, so the same tree packs to the same bytes.
    ( cd "$CONTENT" && zip -q -0 -X -r "$out" . )
    echo "  packaged $name $version -> dist/ivpack/${name}-${version}.ivpack ($(stat -c%s "$out") bytes)"

    rm -rf "$STAGE"
    count=$((count + 1))
done

echo "package-ivpack: $count pack(s): $with_so with lib/.so, $no_so_py py-only, $no_so_nosrc without .so"
