# Zero-range elision for the ext4fs recipe — design, and what it measured

Status: **implemented and measured** in this pack (`E4RCP002`, `classify` GCFG v1,
`kind 3` in the map). The core side is `docs/adr/ADR-014-container-map-is-the-abi.md`
in the engine tree; that document, not this one, owns the map format.

Everything below marked "(measured)" was run on a 128 MiB ext4 image with two
small members; the acceptance transcript is at the end.

## The problem it solved

The core's size guard declined every decomposition of this pack, at every size
tried (64 MiB, 2 GiB, 8 GiB):

```
64 MiB ext4: size guard refused, decomposition declined (the decomposition is
not a gain): 66751572 fixed + 303104 content + 3840 member-cost (3 members)
= 67058516 B vs 67108864 B original
```

`fixed` is the pack's recipe blob, and it was ~the whole partition because the
recipe stored every non-member byte verbatim — necessarily, since a rebuild must
reproduce the image bit-for-bit and free blocks may hold bit-rot. So the
container lane never decomposed anything (measured: zero `!mbr*` member rows
after a sweep, at every size).

## The measurement that makes elision safe

`classify` splits those same bytes by what they are, and verifies zero-ness by
READING them — never by assuming "unallocated => zero":

| image (256 MiB, block 1024) | meta | garbage | zero_verified |
|---|---|---|---|
| clean ext4, one member | 16.1 MiB | 239.8 MiB | **239.5 MiB (99.9%)** |
| + 20 MiB random written into free space | 16.1 MiB | 239.8 MiB | 219.5 MiB (91.5%) |
| + a member with holes | 16.1 MiB | 239.7 MiB | 239.5 MiB |

On a clean image 239.5 of 256 MiB can be reproduced without being stored, and on
a dirty image the pack *says so* instead of guessing.

## Format: E4RCP002

```
v2 semantics: the range table lists the NON-ZERO runs of the non-member bytes.
              The complement in [0, image_size) is zeros.
```

That needs no new field: `rebuild` already writes every range at its ABSOLUTE
image offset into an `O_TRUNC`'d file, so the gaps become holes, and holes read
as zeros. Header stays 32 bytes (`ext4fs.c:208-209`), no zrange table,
`recipe_data_base()` unchanged. The proof in `rebuild` relaxes from "ranges +
member extents TILE `[0, image_size)`" to "disjoint, in bounds, ascending"; the
magic has to change with the meaning, or a v1 reader would read a v2 recipe as
if the gaps were damage.

Measured: the recipe of a 128 MiB image falls from **133 785 184 B** (whole
partition verbatim) to **60 652 B**, and with 8 MiB of random bytes written into
the image's free space — proved free with `debugfs testb`, image still
`e2fsck -fn` clean — it rises to **8 449 276 B**, and the `kind 3` total falls by
exactly 8 388 608 B. That is the difference between a measurement and an
assumption, and it is a permanent fixture (`zl.ext4` in the engine's
`tools/test-ext4fs.sh`).

### What the relaxed proof costs

Because the gaps are not written down, `image_size` can no longer be PROVEN from
the recipe body: v1's ranges tiled `[0, image_size)` exactly, v2's only have to
be disjoint and inside it. A corrupted `image_size` that is merely TOO LARGE
therefore rebuilds without complaint — measured: `ZZZZ` at header offset 8 gives
exit 0 and a 1.4 GiB sparse file from a 64 MiB recipe. The wrong-size output is
caught one layer up (`pack_container_rebuild` in the engine requires the rebuilt
file to be exactly the record's length), so a sweep cannot publish it, but
"fail-closed within the pack" no longer covers that field. The fix is to stop
reading the size from a corruptible header: `rebuild` should take the expected
size from its caller (the engine knows it) and refuse a mismatch.

## No compatibility with E4RCP001, on purpose

`rebuild` refuses `E4RCP001` **by name** — it is a real generation, not damage —
with a message that names the way out: re-import the container from its source
image. There is no v1 branch and no migration path. That is deliberate: one
operator, no volumes in the field, and a migration path is code nobody will ever
exercise.

## The map half — and why it is NOT a pack-private detail

The recipe is pack-owned: the engine stores it as opaque bytes, so a recipe
format change is not a core ABI event (volume/registry ADR-014 has the one
caveat: the engine does parse a container record as an AST in the SPT0 savepoint
walk, but the bytes are pinned as a range either way).

The **map is the opposite**. `!mbrmap` is FS-owned: its wire format, parser,
validator and serve path all live in the engine (`src/core/vol_cpack.c`). Elision
needs a map entry that says "these bytes are zeros" — `kind 3` — and adding a
kind is therefore an ABI step, whatever the pack thinks. Measured consequence of
treating it as a pack detail: an engine one commit older than `kind 3` produced

```
[cpack] get disk.img: map read/parse failed
ext4fs: mapped container read failed for disk.img
```

and 0 of 134 217 728 bytes, from a volume that reads bit-exactly with the
current binary. Since ADR-014 the map declares its vocabulary in its magic
(`MRK1`/`MRK2` for any map carrying a kind ≥ 3) and an older reader refuses at
byte 0 by name.

`cmd_map` no longer declines when a recipe has elided ranges. The gate that did
(`if (z && !getenv("EXT4FS_MAP_ELIDE")) EX_DECLINE`) could never be opened in
production either: the engine spawns pack children through `invfs_helper_exec`,
which scrubs the environment, so no sweep could ever set it.

## The classification sidecar is not wired yet

`strip` writes the per-block classification next to its output as
`<recipe>.cls` (`ext4fs.c:1597-1617`) — it is the last moment the image exists.
That file is a scratch artifact today: it is not stored in the volume and
nothing reads it, so "garbage N MiB, of which zeros M MiB" cannot yet be
reported per container. Storing it as `!mbrcls` and surfacing it in `invf-stats`
is follow-up work, useful only now that elision lands.

## Touch points

| where | change |
|---|---|
| `ext4fs.c:208-209` | `RECIPE_MAGIC` → `E4RCP002`; `RECIPE_HDR` stays 32 |
| `ext4fs.c:1422` `gcfg_split_zero()` | the block-granular zero proof (reads, does not assume) |
| `ext4fs.c:1530` `cmd_strip` | copies only the non-zero runs; writes `<out>.cls` |
| `ext4fs.c:1646` `cmd_map` | emits `kind 3` per elided run |
| `ext4fs.c:1818` `cmd_classify` | GCFG v1 sidecar for `classify` |
| `ext4fs.c:1855` `cmd_rebuild` | refuses 001 by name; v2 relaxes tiling to disjointness |

## Acceptance (measured, engine tree, 128 MiB image)

1. one sweep, then read the container back: `extracted 'disk.img' -> out1.img
   (134217728 bytes)`, `cmp` silent;
2. members really exist: `disk.img` (inode 2), `!mbr0012-ino12` (131072 B),
   `!mbr0015-note.txt` (1100 B), `!mbr0016-payload.bin` (512000 B), `!mbrmap`
   (472 B), `!mbrt` (55 B);
3. three further sweeps, each followed by the same read + `cmp`: silent every
   time, `fsck` CLEAN, 0 bad pages, 0 torn slots;
4. adversarial: 8 MiB of random bytes in free space moved that coverage out of
   the elided side (window `[67108864, 75497472)` became one `kind 0` entry), and
   the round trip stayed bit-exact.
