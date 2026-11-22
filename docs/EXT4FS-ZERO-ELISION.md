# Zero-range elision for the ext4fs recipe — design and acceptance

Status: **designed, not implemented.** The parts that exist are `classify`
(GCFG v1) and the `<recipe>.cls` sidecar written by `strip`; both are measured
below. This note exists so the remaining work is not re-derived.

## The problem it solves

The core's size guard declines every decomposition of this pack, at every size
tried (64 MiB, 2 GiB, 8 GiB):

```
64 MiB ext4: size guard refused, decomposition declined (the decomposition is
not a gain): 66751572 fixed + 303104 content + 3840 member-cost (3 members)
= 67058516 B vs 67108864 B original
```

`fixed` is the pack's recipe blob, and it is ~the whole partition because the
recipe stores every non-member byte verbatim — by design, since a rebuild must
reproduce the image bit-for-bit and free blocks may hold bit-rot. That is why
the container lane has never actually decomposed anything in these trees.

## The measurement that makes elision safe

`classify` splits the same bytes by what they are, and verifies zero-ness by
READING them (never assuming "unallocated => zero"):

| image (256 MiB, block 1024) | meta | garbage | zero_verified |
|---|---|---|---|
| clean ext4, one member | 16.1 MiB | 239.8 MiB | **239.5 MiB (99.9%)** |
| + 20 MiB random written into free space | 16.1 MiB | 239.8 MiB | 219.5 MiB (91.5%) |
| + a member with holes | 16.1 MiB | 239.7 MiB | 239.5 MiB |

So on a clean image 239.5 of 256 MiB can be reproduced without being stored, and
on a dirty image the pack *says so* instead of guessing. Measured sidecar cost:
**544 bytes** for a 128 MiB image (recipe 133 785 184 B).

## Format: E4RCP002 — and why it needs no header growth

The E4RCP blob is the PACK's recipe; the core stores it as opaque bytes and
hands it back. So a format change here is not a core ABI event at all — the
only code that reads it is this pack's own `rebuild`, which ships in the same
binary. (The core does look inside one place, the SPT0 savepoint walk, because
it tries to parse a container record as an AST — a known wart, and orthogonal:
whatever bytes the blob has, they are pinned as a range.)

That also means the zero ranges do not need to be written down:

    v2 semantics: the range table lists the NON-ZERO runs only.
                  The complement in [0, image_size) is zeros.

which is exactly what a sparse output file gives for free, since `rebuild`
already writes every range at its ABSOLUTE image offset (`copy_range(rfd, woff,
ofd, rr[i].off, rr[i].len)`), and creates the output with O_TRUNC. The gaps
become holes; holes read as zeros.

So: magic bump `E4RCP001` -> `E4RCP002`, header stays 32 bytes, no zrange
table, `recipe_data_base()` untouched. The only structural edit is the proof in
`rebuild`, which today requires ranges + member extents to TILE the image:

    /* partition proof: ranges + member extents must tile [0, image_size) */

In v2 that becomes "disjoint, in bounds, ascending" — gaps are legal and mean
zeros. Keeping "tile" would fail every v2 recipe, which is also why the magic
has to change rather than the meaning silently shifting.

Compatibility, the one that actually matters: volumes decomposed by the 1.0.0
pack carry E4RCP001 recipes, and their `rebuild` must keep restoring them. So
`rebuild` dispatches on the magic (001 = old tiling semantics, 002 = holes are
zeros) and writes 002 for new work. A pack upgrade must never orphan an
existing volume.

## Touch points

| where | change |
|---|---|
| `ext4fs.c:181` | `RECIPE_MAGIC` → `E4RCP002`; `RECIPE_HDR` stays 32 |
| `ext4fs.c:1240` `write_recipe_header()` | unchanged apart from the magic |
| `cmd_rebuild` | dispatch on the magic; v2 relaxes the tiling proof to disjointness |
| `ext4fs.c:1516` `cmd_strip` | split each range with the existing `gcfg_split_zero()` (already written for `classify`) and copy only the non-zero runs |
| `cmd_rebuild` | v2: nothing to write for holes — the gaps already read as zeros |
| `ext4fs.c:1420` `cmd_map` | **decline** (`EX_DECLINE`) when `n_zranges > 0`: the MRMP map points at recipe-blob offsets, and a zero range has no bytes to point at. The core then falls back to rebuild-based reads instead of seeking into garbage. Making seek work again is the follow-up: a `type=2` ("zeros") MRMP entry plus a core-side reader (~30 lines, `cpack_map_parse`). |
| manifest | `min_read` bump if the core must know |

Fail-closed within the pack: an unknown magic is refused by `rebuild`, and a
v1 recipe keeps working through the v1 branch.

## Acceptance (in the core tree)

1. `invf-ls` after a sweep shows `disk.img!mbr*` members — decomposition
   actually happened (today: zero member rows).
2. The sweep log no longer prints `size guard refused`.
3. `cmp` of the container read back against the source image: bit-exact.
4. 31 consecutive sweeps: `used`/`unclaimed` plateau, no drift (the harness that
   produced the numbers in `impl_docs/OPEN-ISSUES.md` still works).
5. Adversarial: an image with random bytes written into its free space must
   still round-trip bit-exactly, with those blocks in the *range* table (not the
   zrange table) — this is the case that proves the elision is a measurement and
   not an assumption.
6. `EXT4FS_ZERO_ELIDE=0` reproduces today's E4RCP001 behaviour byte for byte,
   so a regression can be bisected against it.

## Ordering, and why it is not "sidecar first"

The `.cls` sidecar is only produced when `strip` runs, and `strip` only runs
after the size guard accepts. So reporting "potential garbage" per container
this way is useful *after* elision lands, not before. Before that, the only way
to show the number for an undecomposed container is to materialise the image
(a full extract) — an explicit operator action, not a stats walk.
