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

## Format: E4RCP002

```
   0   8   magic "E4RCP002"
   8   8   u64 image_size
  16   4   u32 block_size
  20   4   u32 n_members
  24   4   u32 n_ranges
  28   4   u32 n_extents
  32   4   u32 n_zranges            <- new
  36   4   u32 reserved0            <- new (keeps every table 8-aligned)
  40  ..   member table   n_members x 24 B
      ..   extent map     n_extents x 32 B
      ..   range table    n_ranges  x 16 B   (off, len) — only NON-zero runs
      ..   zrange table   n_zranges x 16 B   (off, len) — reproduced as zeros
      ..   range bytes    the bytes of every range, concatenated in order
```

`data_base = 40 + 24*M + 32*E + 16*R + 16*Z`. Zero ranges carry no bytes.

## Touch points

| where | change |
|---|---|
| `ext4fs.c:181` | `RECIPE_MAGIC`, `RECIPE_HDR` 32 → 40 |
| `ext4fs.c:1217` `recipe_data_base()` | add `16*n_zr` |
| `ext4fs.c:1240` `write_recipe_header()` | write `n_zranges`, the zrange table, and skip zero runs in the range bytes |
| `ext4fs.c:1516` `cmd_strip` | split each range with the existing `gcfg_split_zero()` (already written for `classify`) and copy only the non-zero runs |
| `ext4fs.c:1661` `cmd_rebuild` | write zeros over the zrange table, then ranges, then splice members |
| `ext4fs.c:1420` `cmd_map` | **decline** (`EX_DECLINE`) when `n_zranges > 0`: the MRMP map points at recipe-blob offsets, and a zero range has no bytes to point at. The core then falls back to rebuild-based reads instead of seeking into garbage. Making seek work again is the follow-up: a `type=2` ("zeros") MRMP entry plus a core-side reader (~30 lines, `cpack_map_parse`). |
| manifest | `min_read` bump if the core must know |

Fail-closed by construction: an old core reads `E4RCP002`, sees an unknown
magic, and refuses. Nothing silently mis-decodes.

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
