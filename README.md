# InvariantFS-registry

Codec packs for [InvariantFS](https://github.com/AsmanovLev/InvariantFS) plus
`invfs-pack`, the pack manager. This repository is the **user-facing half** of
the codec policy: it makes the required-set gate satisfiable and gives the user
a choice of encoders per input family (FLAC vs APE vs WavPack, JXL vs the
generic raw-image lane, ...).

The FS itself is experimental; this registry is where packs are published,
verified and selected. No engine code lives here — the manager and the pack
metadata do.

> Status: WP60 seed. The engine's new pack-root / PCK0 integration lands in
> WP59; this repo is forward-compatible with it (see *Engine integration*).

## Layout

```
codecpacks/<name>/<version>/manifest      # key = value pack metadata
codecpacks/<name>/<version>/<helper>.c    # C helper source (or .py)
codecpacks/<name>/<version>/sha256        # sha256sum -c lines for the pack files
codecpacks/<name>/<version>/bin/<helper>  # built static helper (CI only; not committed)
codecpacks/index.json                     # all packs, all versions (machine index)
kernels/index.json                        # reserved (kernel-compat profiles)
schema/codecpack.schema.json              # manifest schema
schema/index.schema.json                  # index schema
tools/invfs-pack.c                        # the manager
tools/gen-index.py                        # sha256 + index generator
tools/build-helpers.sh                    # static helper builds
tools/package-release.sh                  # release tarballs
tools/test-invf-pack.sh                   # no-VM regression
```

## Adding / updating a pack

1. Create `codecpacks/<name>/<version>/manifest` (see the format below) and
   drop the helper source next to it. Name the C output after the source file;
   the build script compiles `*.c` to `bin/<basename>`.
2. `python3 tools/gen-index.py --root . --check` — writes `sha256` and
   `index.json`, validating required fields.
3. `bash tools/test-invf-pack.sh`.
4. Open a PR. CI builds the helpers **statically**, refreshes the index and,
   on a `v*` tag, attaches release tarballs + `index.json`.

`--version` in a manifest is the pack release; `pack_version` is the manifest
ABI the engine understands (currently `1`). The directory name must equal
`version`.

## Manifest format

The canonical form is a `key = value` text file (`#` comments, blank lines
ignored, unknown keys skipped). `sniff.magic` repeats; a following
`sniff.offset` applies to it. Schemas: `schema/codecpack.schema.json`.

| Field | Req | Meaning |
|---|---|---|
| `name` | yes | pack id; also the `codecpacks/<name>/` and install dir |
| `version` | yes | semver pack release (equals the directory) |
| `pack_version` | yes | manifest ABI (currently 1) |
| `min_read` | yes | minimum InvariantFS version able to read the blobs |
| `algo` | yes | numeric `INVFS_ALGO_*` id |
| `codec_id` | yes | symbolic id, e.g. `INVFS_ALGO_EXT4FS` |
| `format_id` | yes | input format claimed (`ext4`, `jpeg`, `7z`, ...) |
| `family` | yes | `raw_audio` \| `raw_image` \| `container` \| `archive` \| `code` |
| `category` | yes | `primary` \| `optional` |
| `priority` | yes | alternatives weight; higher is preferred |
| `type` | yes | `codec` (encode/decode) \| `container` (enumerate/extract/strip/rebuild) |
| `caps` | yes | `seek`, `batched`, `wholefile`, `container`, `external` |
| `provides` | no | capabilities/families, e.g. `raw_audio`, `container:ext4` |
| `replaces` | no | ids superseded, e.g. `builtin:jxl` (informational) |
| `conflicts` | no | ids that conflict — warned, **not** auto-resolved |
| `depends` | no | required external tools (`cjxl,djxl`, `mac`, ...) |
| `recommends` | no | optional external tools |
| `dec_mem`, `generation` | no | engine working-set / re-admission hints |
| `sniff.magic`, `sniff.offset`, `sniff.ext` | no | probe rules |
| `encode`/`decode`/`estimate` | codec | argv templates (`{in}` `{out}` `{pack}`) |
| `enumerate`/`extract`/`strip`/`rebuild`/`map` | container | argv templates |
| `requires` | no | legacy v1 alias for the comma-joined `depends` list |

`provides`/`replaces`/`priority` drive alternatives. Installing a second pack
that provides the same family keeps the first (see the conflict rule below);
`invfs-pack use <family> <pack>` flips the primary for future sweeps.

## `invfs-pack`

```
invfs-pack [--repo file://DIR|https://RAW_BASE] [--root DIR] [--on-volume MNT] <cmd>
```

| Command | Effect |
|---|---|
| `list [family]` | installed + available |
| `search <term>` | match name/format/provides/family/description |
| `info <pack>` | manifest fields |
| `install <pack>` | install; conflict default keeps the existing pack |
| `remove <pack>` | uninstall |
| `update [<pack>]` | install newer versions |
| `verify [<pack>]` | recompute sha256 against the installed manifest |
| `alternatives <family>` | candidates + current primary |
| `use <family> <pack>` | set primary for **future sweeps only** (no re-encode) |
| `where` | resolved repo + roots |

- **Repo**: `file://path` or an HTTP(S) raw base (defaults to
  `https://raw.githubusercontent.com/AsmanovLev/InvariantFS-registry/main`).
  Env `INVFS_PACK_REPO`.
- **Install root**: `/.invariantfs/codecpacks` (env `INVFS_PACK_ROOT`);
  `--on-volume <mnt>` installs under `<mnt>/.invariantfs/codecpacks`. A
  legacy `<root>/<name>.codecpack` symlink is created so the v1 engine probe
  keeps working.
- **Conflict default**: install but **do not touch the existing pack**. If two
  packs provide one family, both stay; `alternatives` shows the primary.
- **`use` never re-encodes**: it only changes what future sweeps pick.
- **Trust**: `sha256` is always checked; `sig`, when present, is **not**
  enforced. A tampered installed pack fails `verify`; a tampered repo pack is
  refused at install and no partial tree is left behind.
- **External tools** are never installed. `depends` are checked and, when
  missing, the manager prints the install command for the detected distro
  (Debian, Arch, Gentoo, Void, Fedora). Coverage is reported as degraded.

## Pack roots (WP59)

Resolution order: `$INVFS_CODECPACKS` → host `/.invariantfs/codecpacks`
(admin, **trusted**) → on-volume `/.invariantfs/codecpacks` (self-hosting).
Legacy `/.invfs/codecpacks` and `/usr/lib/invfs/codecpacks` remain read
fallbacks.

## Engine integration

The current engine reads `<dir>/<name>.codecpack/manifest`. `invfs-pack`
installs the versioned layout and bridges with a `<name>.codecpack` symlink,
so existing sweepboot probes keep working. The new PCK0 fields
(`min_read`/`version`/`provides`/...) and the `/.invariantfs/codecpacks`
root are consumed by **WP59**; until it lands, the registry is the source of
truth and the manager is fully functional standalone.

## Building

```
make manager      # build/invfs-pack  (host C11)
make helpers      # static C helpers, musl preferred, glibc-static fallback
make index        # refresh sha256 + codecpacks/index.json
make check        # validate required manifest fields
make test         # tools/test-invf-pack.sh (file:// repo, no VM)
```

Static is a **build** property of the registry, not a user requirement: a
musl binary runs on any host. CI installs `musl-tools`; locally, if `musl-gcc`
is absent, the script falls back to `cc -static`. In this container
`musl-gcc` was absent, so the helpers were verified as glibc-static
(`ldd` reports "not a dynamic executable").

## Payloads (WP619 — fat packs)

`ape`, `flacr` and `wavpack` used to be manifest-only: they named an
external tool (`depends = mac/flac/wavpack`) and failed wherever that
tool was missing. Their `1.1.0` releases bundle a statically built
payload instead: the upstream tarball is vendored under
`codecpacks/<name>/1.1.0/payload/`, pinned by `payload.version` /
`payload.url` / `payload.sha256` in the manifest, and built by
`bash tools/build-payloads.sh [verify|build|clean] [pack ...]`
(musl-static C, glibc-`static` C++ — no musl libstdc++ exists; the CLIs
do file IO only, so both are portable). The engine resolves
`<pack>/bin/<tool>` before PATH, so the bundled payload is preferred
and the external tool is fallback-only. `bin/` is never committed
(releases carry it via `tools/package-release.sh`).

## License

GPL-2.0-only (matches the FS).
