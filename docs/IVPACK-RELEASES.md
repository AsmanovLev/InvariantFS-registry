# .ivpack release-asset contract (WP609 round 2, for the WP608r3 client team)

Binding owner decision (ADR-007, main repo): **`.ivpack` is THE pack
delivery format**. The v1.0.0 source tarballs stay as history; the client
consumes `.ivpack`, not tarballs. No tarballs are published after v1.0.0.

## Tag scheme

* `v1.0.0` — pack sources at 1.0.0 + 11 tarball assets. Frozen, immutable,
  never rewritten.
* **`v1.1.0` — the .ivpack deliverable.** Pack sources are UNCHANGED
  (still 1.0.0 in every manifest); the minor bump marks the new
  deliverable, not new pack sources. There are NO per-pack tags; one tag
  carries all packs.
* Future pack-source changes bump per-pack versions AND the tag together
  (e.g. v1.2.0); the tag always carries the full set.

## Asset file names (v1.1.0)

One asset per pack, `<name>-<version>.ivpack` (ADR-007 §2 on-volume
naming), plus the registry index:

| asset | packs covered |
|---|---|
| `ape-1.0.0.ivpack` | ape (manifest only) |
| `ext4fs-1.0.0.ivpack` | ext4fs |
| `fatfs-1.0.0.ivpack` | fatfs |
| `flacr-1.0.0.ivpack` | flacr (manifest only) |
| `jxl-1.0.0.ivpack` | jxl |
| `ntfs-1.0.0.ivpack` | ntfs |
| `p7z-1.0.0.ivpack` | p7z |
| `qcow2-1.0.0.ivpack` | qcow2 |
| `rawdisk-1.0.0.ivpack` | rawdisk |
| `raw_image-1.0.0.ivpack` | raw_image (py helper) |
| `splt_test-1.0.0.ivpack` | splt_test (py helper) |
| `vdi-1.0.0.ivpack` | vdi |
| `wavpack-1.0.0.ivpack` | wavpack (manifest only) |
| `xfs-1.0.0.ivpack` | xfs |
| `index.json` | registry index (same file as v1.0.0 shape) |
| `invfs-pack` | manager binary (same as v1.0.0 shape) |

14 packs, 14 `.ivpack` assets. No tarballs on v1.1.0.

## Zip layout (every bundle)

Uncompressed ZIP-0 (`zip -0 -X`), entries relative to the archive root:

```
<name>-<version>.ivpack
├── manifest          # the pack's own manifest, byte-verbatim
├── sha256            # `sha256sum -c`-able list of EVERY other entry
├── lib/<name>.so     # present IFF the "lib/.so" column below says yes
└── bin/*             # static CLI helper(s) as fallback, and/or *.py helpers
```

`sha256` covers every entry except itself, so `unzip` + `sha256sum -c
sha256` fully verifies a bundle. Verified for all 14 v1.1.0 bundles by
`tools/verify-ivpacks.sh` (also a CI step).

Normalizations vs the engine reference (`tools/pack-ivpack.sh`):

* The reference packs `lib/lib<name>.so` (engine-tree quirk: its build
  outputs are already named `lib<name>.so`); the registry ships the
  TASK-mandated **`lib/<name>.so`**.
* `bin/` carries every built static helper under its build name (e.g.
  the `jxl` pack's helper is `bin/jxlest`, matching its `estimate`
  manifest line) plus every `*.py` helper as `bin/<script>.py`.

## Which packs lack lib/.so (and why)

| pack | lib/<name>.so | bin/ contents | reason |
|---|---|---|---|
| ext4fs, fatfs, jxl, ntfs, p7z, qcow2, rawdisk, vdi, xfs (9) | YES | static helper(s) | `-fPIC -shared` build of the pack's C helper source |
| raw_image, splt_test (2) | NO | `bin/raw_image.py`, `bin/splt.py` | `.py` helpers cannot yield a `.so` (`NO_SO_PY` in the CI log) |
| ape, flacr, wavpack (3) | NO | (empty — manifest only) | no helper source at all (`NO_SO_NOSRC` in the CI log) |

The builder (`tools/package-ivpack.sh`) NEVER skips silently: every pack
without a `.so` gets a loud per-pack `NO_SO_PY` / `NO_SO_NOSRC` /
`NO_SO_BUILDFAIL` line in the CI log.

## What the .so files are (read this before dlopen)

The 9 shipped `.so` files are **`cc -fPIC -shared` builds of the packs'
CLI-helper C sources** — they load under `dlopen` (smoke-tested) and
export the helper's symbols (including `main`), but they do **NOT**
implement the ivpack C ABI plugin entry points: no registry pack source
is a plugin-ABI source, and the engine-tree ABI headers are not vendored
in this repo, so entry-point conformance cannot be proven here.

**Client (WP608r3) MUST still verify, per bundle with a `.so`:**

1. `unzip -l <name>-1.0.0.ivpack` — entries match the layout above.
2. Unpack and run `sha256sum -c sha256` — must print OK for every entry.
3. `cmp manifest codecpacks/<name>/1.0.0/manifest` against this repo —
   must be byte-identical.
4. Against the engine ABI headers: confirm the expected plugin entry
   symbol resolves (`dlsym`) once a true ABI plugin source replaces the
   CLI-helper build — until then, treat `lib/<name>.so` as a loadable
   build artifact and `bin/*` as the execution fallback.
5. For the 5 packs without a `.so`: consume `manifest` + `bin/*` only;
   do not expect `lib/`.

## Reproducing

```bash
make helpers          # static bin/ helpers (musl preferred)
bash tools/package-ivpack.sh   # -> dist/ivpack/*.ivpack (14)
bash tools/verify-ivpacks.sh   # unzip -l + sha256 -c + verbatim + dlopen
```

`make ivpacks` runs all three (helpers + pack + verify).
