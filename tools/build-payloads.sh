#!/bin/bash
# build-payloads.sh — WP619: verify + statically build the fat-pack payloads.
#
# For each fat pack (ape/flacr/wavpack): read the payload.* pins from the
# pack's manifest (the manifest is the single source of truth), sha256-
# verify the colocated payload/<file>, build it statically, and install
# the payload.bin tools into <pack>/bin/ (gitignored, CI-only — releases
# carry them via tools/package-release.sh, which tars bin/ into the
# per-pack release tarball).
#
# Static rule (the build-helpers.sh shapes, extended to C++):
#   C payloads (wavpack): musl-gcc preferred, $CC -static fallback.
#   C++ payloads (flac, mac): no musl libstdc++ exists on a stock distro,
#     so $CXX -static (glibc-static). These CLIs do file IO only (no NSS,
#     no dlopen, no getaddrinfo), so a glibc-static binary is portable the
#     same way the README's documented glibc-static fallback is.
#
# The fetch is deliberately NOT done here: the tarball is vendored next to
# the manifest (payload/<file>) and hashed by the pack's sha256 listing,
# so a build needs no network — only a C/C++ toolchain and cmake for the
# cmake payloads. Upstream URLs live in the manifest for provenance.
#
# Usage:  bash tools/build-payloads.sh [verify|build|clean] [pack ...]
#   default action is `build` (which verifies first and fails closed on a
#   hash mismatch); default packs are ape flacr wavpack at their highest
#   versioned directory.
#
# Env: INVFS_PAYLOAD_JOBS (default nproc); INVFS_PAYLOAD_KEEP_TREE=1 keeps
#   the unpacked+build tree under build/payloads/ for debugging (default:
#   removed after a successful build — scratch is shared and small, and
#   prior WP619 attempts died from exactly this disk pressure).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

JOBS="${INVFS_PAYLOAD_JOBS:-$(nproc 2>/dev/null || echo 2)}"
KEEP="${INVFS_PAYLOAD_KEEP_TREE:-0}"
WORK="$ROOT/build/payloads"

FATPACKS_DEFAULT="ape flacr wavpack"

say()  { printf '%s\n' "$*"; }
fail() { printf 'build-payloads: FAIL: %s\n' "$*" >&2; exit 1; }
disk() { df -h /tmp 2>/dev/null | awk 'NR==2{print "  [disk] /tmp avail="$4" ("$5" used)"}'; }

# manifest value for `key` (exact `key = value`, last wins, like gen-index)
mget() {
    awk -F= -v k="$2" '
        { line=$0; sub(/^[ \t]+/, "", line); if (line=="" || substr(line,1,1)=="#") next;
          key=$1; gsub(/^[ \t]+|[ \t]+$/, "", key); if (key!=k) next;
          val=substr($0, index($0,"=")+1); gsub(/^[ \t]+|[ \t\r\n]+$/, "", val); v=val }
        END { print v }' "$1"
}

# highest versioned dir codecpacks/<name>/*/
packdir() {
    local d
    d="$(ls -d "codecpacks/$1/"*/ 2>/dev/null | sort -V | tail -n 1)"
    [ -n "$d" ] || fail "no version dir for pack $1"
    printf '%s' "${d%/}"
}

split_csv() { tr ',' ' '; }

verify_pack() {
    local name="$1" dir manifest url want got file f
    dir="$(packdir "$name")"
    manifest="$dir/manifest"
    [ -f "$manifest" ] || fail "$name: no manifest in $dir"
    url="$(mget "$manifest" payload.url)"
    want="$(mget "$manifest" payload.sha256)"
    [ -n "$url" ] && [ -n "$want" ] || fail "$name: no payload.url/sha256 pins in $manifest"
    file="$(ls "$dir"/payload/* 2>/dev/null | head -n 1)"
    [ -n "$file" ] || fail "$name: no vendored file under $dir/payload/"
    got="$(sha256sum "$file" | awk '{print $1}')"
    [ "$got" = "$want" ] || fail "$name: sha256 mismatch for $file (want $want, got $got)"
    say "  verify $name: $(basename "$file") OK ($got)"
}

is_static() { file "$1" 2>/dev/null | grep -q 'statically linked'; }

install_bins() {
    # $1=packdir $2=csv tools $3...=candidate search dirs (first hit wins)
    local dir="$1" csv="$2"; shift 2
    local t d hit
    mkdir -p "$dir/bin"
    for t in $(echo "$csv" | split_csv); do
        hit=""
        for d in "$@"; do
            if [ -x "$d/$t" ]; then hit="$d/$t"; break; fi
        done
        [ -n "$hit" ] || fail "$dir: built tool $t not found under: $*"
        cp -f "$hit" "$dir/bin/$t"
        chmod +x "$dir/bin/$t"
        is_static "$dir/bin/$t" \
            && say "  static OK: $t ($(file -b "$dir/bin/$t" | cut -d, -f1-2))" \
            || fail "$dir/bin/$t is NOT statically linked"
    done
}

musl_cc() {
    if command -v musl-gcc >/dev/null 2>&1; then printf 'musl-gcc';
    elif command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then printf 'x86_64-linux-musl-gcc';
    else printf ''; fi
}

build_wavpack() {
    # WavPack 5.8.1: pure C, cmake or autotools. musl-static.
    local dir="$1" tarball="$2" t="$WORK/wavpack" cc
    cc="$(musl_cc)"; [ -n "$cc" ] || cc="${CC:-cc}"
    rm -rf "$t"; mkdir -p "$t"
    tar -xf "$tarball" -C "$t"
    local src="$t/$(ls "$t")"
    ( cd "$src" && \
      cmake -S . -B b -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$cc" -DCMAKE_C_FLAGS="-static -O2" \
        -DCMAKE_EXE_LINKER_FLAGS="-static" -DBUILD_SHARED_LIBS=OFF \
        >/tmp/invfs-wp619-wavpack-cmake.log 2>&1 && \
      cmake --build b -j "$JOBS" >/tmp/invfs-wp619-wavpack-build.log 2>&1 \
    ) || { tail -n 20 /tmp/invfs-wp619-wavpack-cmake.log /tmp/invfs-wp619-wavpack-build.log 2>/dev/null; fail "wavpack cmake build failed"; }
    install_bins "$dir" "$(mget "$dir/manifest" payload.bin)" \
        "$src/b/cli" "$src/b" "$src/cli"
    [ "$KEEP" = 1 ] || rm -rf "$t"
}

build_flac() {
    # FLAC 1.5.0: C + C++ (libFLAC++/flac CLI), autotools. glibc -static
    # C++ (no musl libstdc++ exists); ogg disabled — the pack handles
    # .flac/.wav, never Ogg FLAC (configure must accept --disable-ogg).
    local dir="$1" tarball="$2" t="$WORK/flac"
    rm -rf "$t"; mkdir -p "$t"
    tar -xf "$tarball" -C "$t"
    local src="$t/$(ls "$t")"
    ( cd "$src" && \
      ./configure CC="${CC:-cc}" CXX="${CXX:-g++}" \
        CFLAGS="-O2" CXXFLAGS="-O2 -static" LDFLAGS="-static" \
        --disable-shared --enable-static --disable-ogg \
        --disable-doxygen-docs --disable-examples --disable-cpplibs-test \
        >/tmp/invfs-wp619-flac-config.log 2>&1 && \
      make -j "$JOBS" >/tmp/invfs-wp619-flac-build.log 2>&1 \
    ) || { tail -n 20 /tmp/invfs-wp619-flac-config.log /tmp/invfs-wp619-flac-build.log 2>/dev/null; fail "flac build failed"; }
    install_bins "$dir" "$(mget "$dir/manifest" payload.bin)" \
        "$src/src/flac" "$src/src/flac/.libs"
    [ "$KEEP" = 1 ] || rm -rf "$t"
}

build_mac() {
    # Monkey's Audio SDK: C++, cmake (the Arch `mac` PKGBUILD shape:
    # plain `cmake -S . -B build && cmake --build build`). glibc -static
    # C++ for the same reason as flac. Produces the `mac` CLI.
    local dir="$1" tarball="$2" t="$WORK/mac"
    rm -rf "$t"; mkdir -p "$t"
    ( cd "$t" && unzip -q "$tarball" ) || fail "mac unzip failed"
    local src="$t"
    [ -f "$src/CMakeLists.txt" ] || src="$t/$(ls "$t" | head -n 1)"
    [ -f "$src/CMakeLists.txt" ] || fail "mac: no CMakeLists.txt under $t"
    ( cd "$src" && \
      cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-static -O2" \
        -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++" \
        >/tmp/invfs-wp619-mac-cmake.log 2>&1 && \
      cmake --build build -j "$JOBS" >/tmp/invfs-wp619-mac-build.log 2>&1 \
    ) || { tail -n 20 /tmp/invfs-wp619-mac-cmake.log /tmp/invfs-wp619-mac-build.log 2>/dev/null; fail "mac cmake build failed"; }
    install_bins "$dir" "$(mget "$dir/manifest" payload.bin)" \
        "$src/build" "$src/build/Console" "$src/build/Source/Console"
    [ "$KEEP" = 1 ] || rm -rf "$t"
}

build_pack() {
    local name="$1" dir tarball
    dir="$(packdir "$name")"
    verify_pack "$name"
    tarball="$(ls "$dir"/payload/* | head -n 1)"
    disk
    say "  build $name: $(basename "$tarball") ($(du -h "$tarball" | cut -f1))"
    case "$name" in
        ape)     build_mac "$dir" "$tarball" ;;
        flacr)   build_flac "$dir" "$tarball" ;;
        wavpack) build_wavpack "$dir" "$tarball" ;;
        *) fail "no payload builder for pack $name" ;;
    esac
    disk
}

clean_pack() {
    local dir
    dir="$(packdir "$1")"
    rm -rf "$dir/bin"
    say "  clean $1: removed $dir/bin"
}

ACTION="${1:-build}"; shift || true
case "$ACTION" in
    verify|build|clean) ;;
    *) fail "usage: $0 [verify|build|clean] [pack ...]" ;;
esac
PACKS="${*:-$FATPACKS_DEFAULT}"
mkdir -p "$WORK"
for p in $PACKS; do
    say "== $ACTION $p =="
    case "$ACTION" in
        verify) verify_pack "$p" ;;
        build)  build_pack "$p" ;;
        clean)  clean_pack "$p" ;;
    esac
done
say "build-payloads: $ACTION done ($PACKS)"
