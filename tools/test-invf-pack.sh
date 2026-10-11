#!/bin/bash
# test-invf-pack.sh — WP60 invfs-pack manager regression (no VM, file:// repo).
#
# Covers: list/info/install/remove/verify; alternatives (flacr+ape coexist);
# `use` flips the primary for future sweeps only; tampered installed pack is
# refused by verify; tampered repo pack is refused at install; on-volume
# install; external-dep hint; and a statically built C helper runs.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PACK="$ROOT/build/invfs-pack"
WORK="$(mktemp -d /tmp/invfspack-test.XXXXXX)"
ROOTDIR="$WORK/root"
BADREPO="$WORK/badrepo"
PASS=0; FAIL=0

say()  { printf '\n== %s ==\n' "$*"; }
ok()   { printf '  PASS: %s\n' "$*"; PASS=$((PASS+1)); }
bad()  { printf '  FAIL: %s\n' "$*"; FAIL=$((FAIL+1)); }
check(){ if [ "$1" -eq 0 ]; then ok "$2"; else bad "$2"; fi; }

[ -x "$PACK" ] || { echo "build/invfs-pack missing; run make" >&2; exit 2; }
mkdir -p "$ROOTDIR"

ip() { "$PACK" --repo "file://$ROOT" --root "$ROOTDIR" "$@"; }

say "list + info"
OUT="$(ip list)"; echo "$OUT"
echo "$OUT" | grep -q '^  ext4fs' && ok "list shows ext4fs" || bad "list missing ext4fs"
echo "$OUT" | grep -q '^  flacr' && ok "list shows flacr" || bad "list missing flacr"
ip info jxl | grep -q 'INVFS_ALGO_JXL' && ok "info jxl codec_id" || bad "info jxl"
ip list raw_audio | grep -q wavpack && ok "list raw_audio filters" || bad "list family filter"

say "install flacr then ape (alternatives coexist)"
ip install flacr >/dev/null 2>&1
[ -f "$ROOTDIR/flacr/1.0.0/manifest" ] && ok "flacr installed" || bad "flacr install"
ip install ape >/dev/null 2>&1
[ -f "$ROOTDIR/ape/1.0.0/manifest" ] && ok "ape installed" || bad "ape install"
[ -f "$ROOTDIR/flacr/1.0.0/manifest" ] && ok "flacr kept (not clobbered by ape)" || bad "flacr clobbered"
[ -L "$ROOTDIR/flacr.codecpack" ] && ok "legacy <name>.codecpack symlink" || bad "legacy symlink"

say "alternatives + use (future sweeps only)"
ALT="$(ip alternatives raw_audio)"; echo "$ALT"
echo "$ALT" | grep -q 'flacr' && echo "$ALT" | grep -q 'ape' && echo "$ALT" | grep -q 'wavpack' \
  && ok "alternatives lists all three" || bad "alternatives list"
echo "$ALT" | grep -q 'Primary: flacr' && ok "priority picks flacr primary" || bad "default primary"
ip use raw_audio ape >/dev/null 2>&1
ip alternatives raw_audio | grep -q 'Primary: ape' && ok "use flips primary" || bad "use flip"
ip use raw_audio wavpack >/dev/null 2>&1
ip alternatives raw_audio | grep -q 'Primary: ape' && ok "use refuses uninstalled pack" || bad "use guard"

say "install container pack + verify"
ip install ext4fs >/dev/null 2>&1
[ -f "$ROOTDIR/ext4fs/1.0.0/ext4fs.c" ] && ok "ext4fs helper source installed" || bad "ext4fs source"
ip verify ext4fs >/dev/null 2>&1 && ok "verify ext4fs PASS" || bad "verify ext4fs"

say "tampered installed pack refused"
echo "# tampered" >> "$ROOTDIR/ext4fs/1.0.0/manifest"
if ip verify ext4fs >/dev/null 2>&1; then bad "tamper not detected"; else ok "verify detects tamper"; fi

say "tampered REPO pack refused at install"
mkdir -p "$BADREPO"
cp -a "$ROOT/codecpacks" "$BADREPO/codecpacks"
echo "# tamper" >> "$BADREPO/codecpacks/ext4fs/1.0.0/ext4fs.c"
if "$PACK" --repo "file://$BADREPO" --root "$WORK/badroot" install ext4fs >/dev/null 2>&1; then
  bad "tampered repo accepted"
else
  ok "tampered repo install refused"
fi
[ -d "$WORK/badroot/ext4fs" ] && bad "tampered partial install left behind" || ok "no partial install"

say "remove"
ip remove ext4fs >/dev/null 2>&1
[ -d "$ROOTDIR/ext4fs" ] && bad "remove left dir" || ok "remove cleans dir"
[ -e "$ROOTDIR/ext4fs.codecpack" ] && bad "remove left symlink" || ok "remove cleans symlink"

say "on-volume install"
VOL="$WORK/mnt"; mkdir -p "$VOL"
"$PACK" --repo "file://$ROOT" --on-volume "$VOL" install jxl >/dev/null 2>&1
[ -f "$VOL/.invariantfs/codecpacks/jxl/1.0.0/manifest" ] && ok "on-volume layout" || bad "on-volume install"
if "$PACK" --repo "file://$ROOT" --on-volume "$WORK/notdir" install jxl >/dev/null 2>&1; then
  bad "on-volume accepted non-dir"
else
  ok "on-volume rejects non-mounted path"
fi

say "external depends hint (printed, never installed)"
# Hermetic: CI images may ship 7zz; hide PATH so the dep reads as missing.
EMPTYBIN="$WORK/emptybin"; mkdir -p "$EMPTYBIN"
HINT="$(PATH="$EMPTYBIN" ip install p7z 2>&1)"
echo "$HINT" | grep -qi '7zz' && echo "$HINT" | grep -qiE 'apt|pacman|emerge|xbps|dnf' \
  && ok "missing dep prints distro command" || bad "dep hint"

say "static C helper builds + runs"
if command -v musl-gcc >/dev/null 2>&1 || command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then
  note="musl-gcc present"
else
  note="musl-gcc ABSENT (using glibc -static)"
fi
CC_KIND=""
if command -v musl-gcc >/dev/null 2>&1; then CC_KIND="musl-gcc";
elif command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then CC_KIND="x86_64-linux-musl-gcc"; fi
HB="$WORK/helper"; mkdir -p "$HB"
if [ -n "$CC_KIND" ]; then
  "$CC_KIND" -std=c11 -O2 -static -o "$HB/rawdisk" "$ROOT/codecpacks/rawdisk/1.0.0/rawdisk.c" 2>/dev/null
else
  cc -std=c11 -O2 -static -o "$HB/rawdisk" "$ROOT/codecpacks/rawdisk/1.0.0/rawdisk.c" 2>/dev/null
fi
if [ -x "$HB/rawdisk" ]; then
  ok "helper built ($note)"
  file "$HB/rawdisk" | grep -q 'statically linked' && ok "helper is static" || bad "helper not static"
  "$HB/rawdisk" >/dev/null 2>&1; rc=$?
  [ "$rc" -eq 2 ] || [ "$rc" -eq 3 ] || [ "$rc" -eq 0 ] && ok "static helper runs on host (rc=$rc)" || bad "helper run rc=$rc"
else
  bad "helper build failed"
fi

printf '\n============================\n'
printf 'test-invf-pack: %d passed, %d failed (%s)\n' "$PASS" "$FAIL" "$note"
printf '============================\n'
[ "$FAIL" -eq 0 ] || exit 1
exit 0
