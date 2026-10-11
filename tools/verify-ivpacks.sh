#!/bin/bash
# verify-ivpacks.sh - prove dist/ivpack/*.ivpack are consumable (WP609 round 2).
#
# Per bundle:
#   1. `unzip -l` lists the entries (consumable as a ZIP).
#   2. unzip to a temp dir, `sha256sum -c sha256` (bundled checksums cover
#      the bundle contents).
#   3. manifest inside == manifest in codecpacks/<name>/<version> (verbatim).
#   4. where a lib/<name>.so was built: dlopen smoke via python3 ctypes
#      (library loads). The engine-tree ABI headers are NOT vendored here,
#      so plugin-entry-point conformance is NOT proven by this script --
#      docs/IVPACK-RELEASES.md tells the client team (WP608r3) what to verify.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail=0
n=0
for iv in dist/ivpack/*.ivpack; do
    [ -e "$iv" ] || { echo "ERROR: no .ivpack files in dist/ivpack/"; exit 1; }
    base="$(basename "$iv" .ivpack)"
    name="${base%-*}"; version="${base##*-}"
    echo "=== $base ==="
    n=$((n + 1))

    echo "--- unzip -l:"
    unzip -l "$iv"

    # entry-name contract: top-level manifest + sha256, lib/<name>.so only
    # under lib/, helpers only under bin/
    bad="$(unzip -Z1 "$iv" | grep -v '/$' | grep -v -e '^manifest$' -e '^sha256$' -e "^lib/$name\.so\$" -e '^bin/[^/]*$' || true)"
    if [ -n "$bad" ]; then
        echo "ERROR: $base has unexpected entries: $bad"; fail=1
    fi

    D="$(mktemp -d "${TMPDIR:-/tmp}/ivpack_verify.XXXXXX")"
    unzip -q -o "$iv" -d "$D"

    ( cd "$D" && sha256sum -c sha256 ) || { echo "ERROR: $base sha256 check FAILED"; fail=1; }

    if ! cmp -s "$D/manifest" "codecpacks/$name/$version/manifest"; then
        echo "ERROR: $base manifest is not verbatim"; fail=1
    else
        echo "manifest: verbatim OK"
    fi

    if [ -f "$D/lib/$name.so" ]; then
        python3 - "$D/lib/$name.so" <<'PY'
import ctypes, sys
p = sys.argv[1]
try:
    ctypes.CDLL(p)
except OSError as e:
    print(f"ERROR: dlopen failed for {p}: {e}")
    sys.exit(1)
print(f"dlopen: OK ({p})")
PY
        [ $? -eq 0 ] || fail=1
        # report which well-known symbols the object carries (informational:
        # these sources are CLI-helper mains, so no engine ABI entry point
        # is expected here)
        nm -D --defined-only "$D/lib/$name.so" 2>/dev/null | head -8 || true
    else
        echo "(no lib/$name.so in bundle -- expected per docs/IVPACK-RELEASES.md)"
    fi

    rm -rf "$D"
done

echo "verify-ivpacks: $n bundle(s) checked"
[ "$fail" -eq 0 ] || { echo "verify-ivpacks: FAILURES PRESENT"; exit 1; }
echo "verify-ivpacks: ALL OK"
