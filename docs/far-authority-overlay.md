# Far authority overlay (FSV1)

The far authority overlay carries edit-baked signed-distance bricks for the
volumetric far ladder described in [the distant-world contract](distant-world.md).
One file holds the bricks of one tile. It is derived render state: it never
enters a world hash and `validate_save` does not scan it. Older non-indexed
readers ignore the `far/` directory; indexed readers require explicit support
for derived far members.

This page describes the codec in `world/FarAuthorityOverlay.{h,cpp}` and the pure
edited bake in `world/FarEditedBake.{h,cpp}`. The world-open and transactional
publication hooks are separate pieces; the codec accepts any brick producer.

## File name

`<save>/far/v<tier>.<tx>.<tz>.lmr`, with the tier in 1..5 and negative tile
indices printed with `-`. Numbers are canonical decimal (no leading zeros, no
`+`, no `-0`). A file whose header key disagrees with its name is refused.

## Byte layout

All integers are little-endian with no padding.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `FSV1` |
| 4 | 2 | version (1) |
| 6 | 2 | flags (0, reserved) |
| 8 | 1 | tier (1..5) |
| 9 | 3 | zero |
| 12 | 8 | tx (i64) |
| 20 | 8 | tz (i64) |
| 28 | 8 | generation (u64, opaque to the codec) |
| 36 | 4 | brick_count |
| 40 | 4 | body_bytes |
| 44 | body_bytes | brick records |
| 44 + body_bytes | 4 | CRC32 over every preceding byte |

A brick record is `i32 bx | i32 by | i32 bz | u8 state | u8[3] zero`. States are
0 Present, 1 HomogeneousAir, 2 HomogeneousSolid. A Present record is followed by
125 `i16` densities (a 5 x 5 x 5 sample brick, scale 256, signed distance with
negative solid and positive air) and 125 `u8` material ids. A Present record is
391 bytes and a tombstone is 16 bytes. An absent record means no authoritative
override; a homogeneous record is a tombstone and is never confused with absence.

Records are sorted by (bz, bx, by) ascending with no duplicates. Brick indices
are `floor(world metres / brick edge)`; a tile spans 32 bricks per horizontal
axis, and `by` is any value with `|by| <= 2^20`.

## Limits

- At most 131,072 bricks per file.
- At most 64 MiB per file, checked from the header before any allocation and from
  the file size before any read.
- Tile indices up to 2^25 in magnitude (larger ones cannot be expressed as i32
  brick indices).

## Refusal table

Decoding returns a typed error and never a partial tile.

| Error | Cause |
|---|---|
| Truncated | fewer bytes than the header, the body or the trailer require |
| BadMagic | first four bytes are not `FSV1` |
| CorruptVersion / FutureVersion | version 0 / version above 1 |
| ReservedBits | flags or a reserved byte is non-zero |
| InvalidTier | header tier outside 1..5 |
| KeyMismatch | header key differs from the key in the file name |
| LimitExceeded | brick count or file size over the limits |
| TrailingBytes | bytes beyond the declared size |
| CrcMismatch | trailer does not match |
| SizeMismatch | records disagree with `body_bytes` |
| InvalidBrickState | state byte above 2 |
| BrickOutOfFootprint | brick outside the tile or `|by|` over the bound |
| BricksNotSorted | unsorted or duplicate bricks |
| ForbiddenDensity | a density of -32768 |

A corrupt or newer-format file is refused and is never treated as absent.

## Writing and generations

`WriteFarOverlay` encodes the tile and writes it through
`WorldSaveService::write_file_durably`: the parent directory is created, a
unique temporary file is flushed and then atomically replaces the destination.
On POSIX every ancestor directory entry is flushed on every call, so a retry
after a failed flush repeats the missing flushes. A failed or
interrupted write never leaves a partial file: the destination is the previous
complete file, or the new complete file when only the final directory flush
failed.

The generation is the save-level snapshot generation the bake was produced from.
It is not a chunk's process-local voxel revision. Each edited chunk uses its
contributing indexed region member's generation conservatively. `FarBakeDue`
derives bake obligations without remembering them: no edited chunks means nothing is due,
edited chunks with no overlay is due (rebuilt from durable chunk records, never
from pristine terrain), and otherwise a bake is due when the newest edited chunk
generation exceeds the overlay generation.

## Quantization and tombstones

`QuantizeFarDensity` rounds metres times 256 and clamps to +-32767, keeping the
sign of any non-zero input. `ClassifyFarBrick` marks a brick HomogeneousSolid when
all 125 densities are negative, HomogeneousAir when all are positive, and Present
otherwise (any exact zero keeps it Present).

## Pure edited bake and sampling

`FarEditedSourceView` supplies a complete immutable inventory of owned edited
lattices, including durable records and halo contributors, with prepared or
committed snapshot identity. Each lattice carries its indexed region member's
generation. Capture must refuse unreadable expected records. A prepared bake's
bytes become eligible for publication only after its transaction commits.
Frozen pristine observations are supplied separately with terrain/cave policy
already bound; workers never sample the live world or changing save paths.

`ComputeFarEditedAffectedSet` intersects closed edited footprints with closed
brick supports at all five tiers, including shared faces, edges and corners and
the six supports displaced by one spacing for central-difference normals.
Negative coordinates use floor division. `BakeFarEditedTiles` rebuilds every
affected tile's complete current edited-brick set, preserving earlier edits;
an empty inventory produces an empty overlay that removes previous overrides.
Returned extents include changed/removed records and halo dependencies. Callers
union them into the existing monotone span ledger. The member-generation
watermark includes halo-only contributors. This service writes no files.

Density interpolation uses double arithmetic in Z/Y/X order. Material selects
the nearest lattice node, choosing the lower node at a tie; absent material
arrays use id 0. Wrong array sizes and non-finite source values refuse before
pristine sampling. Shared authoritative nodes are checked before baking;
overlapping observations must also agree at every bake coordinate. Quantization
is always LegacySigned16; exact zero stays Present. A coarse lattice can miss a
small feature without removing its authoritative record.

`SampleFarOverlay` resolves one tier-lattice coordinate across codec-validated,
canonically sorted tiles. Agreeing Present boundary samples take precedence over
tombstones. Otherwise tombstones supply quantized signed brick-edge constants
and pristine material; conflicting tombstones refuse. Missing returns no sample
and requires authoritative inventory evidence before pristine substitution.

Caller caps bound source records/samples/probes, combined per-tier candidate
visits, affected sets, density evaluations, scratch storage and worst-case
brick/encoded output storage. They are admission limits, not qualified shipping
defaults. Allocation-free per-footprint counts include the six normal halos;
the largest footprint per tier bounds the union from below before enumeration.
Unique brick/tile insertion refuses before exceeding either cap, and vector
reservations never exceed admitted set storage. Optional affected-set work
counters expose reservation requests, candidate visits and unique counts,
including refusals. Failure returns a typed error and preserves the previous
whole result.
FSV1 version, reserved bytes and existing wire layout remain unchanged.
