# Indexed native volume snapshots

An indexed native image uses mirrored paged storage and reads inode, name and
extent records on demand. The existing native snapshot API provides lookup,
directory pages, metadata and file reads. The current format accepts explicit
read-only images and supports export from a checked legacy snapshot. A separate
volume-head capability and writable transaction engine are required to select
this layout for a Windows mount.

## Root and keys

Object 1 is a 96-byte root. Its integers are little-endian.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | `FLTXPV01` |
| 8 | 4 | Indexed native capability 1 |
| 12 | 4 | Workload accounting flag, 0 or 1 |
| 16 | 8 | Native namespace sequence |
| 24, 32 | 8 each | Next inode ID and next extent ordinal |
| 40, 48, 56 | 8 each | Inode count, mapped extent count and charged extents |
| 64 | 8 | Committed input bytes |
| 72 | 16 | Exact paged store identity |
| 88, 92 | 4 each | Extent length 65,536 and zero reserved field |

The native namespace sequence preserves file provenance through a copy.
It is independent of the new object's publication generation. A reader
requires the explicit indexed capability for `open_image`.

Inode objects use `(1ULL << 63) | inode`. Data objects use
`(1ULL << 61) | ordinal`. Both sequences have a positive usable 61-bit ordinal.
The ordered index uses its existing reserved object domain at `3ULL << 61`.
Its capability-2 root covers two key types in the same protected tree.

| Key | Value |
| --- | --- |
| Parent inode, canonical name bytes | Child inode |
| `(1ULL << 61) | inode`, 8-byte big-endian block number | Data object ID |

The inode body uses the canonical format-7 header, stored spelling and optional
security descriptor. Inline extents and unresolved delete or detached state are
invalid. Lookup checks the name key against the referenced inode. A data object
starts with `FLTXPE01`, the owner inode ID and the logical block number. The
remainder is a nonempty byte prefix of that extent with trailing zero bytes
omitted. Readers check the owner, range and frame before copying. Missing
extents and omitted tails read as zeros. Data objects retain the paged store's
independent CPU decoders and mirrored-payload verification.

## Export and validation

`create_indexed_copy` takes a legacy snapshot with resolved file lifetimes and
a new empty backing with a new identity. It stages records in bounded
transactions. Each transaction publishes its owned records and corresponding
index edits together. The native root is absent during staging. The final
transaction publishes it after independent checked readback of the inventory,
parent graph, typed extent references and both payload copies. Failures can
leave a partial candidate to inspect or discard. The source retains its bytes
and selection throughout.

Opening the completed image checks the root, index root and root inode.
Lookups and range reads verify the pages and records they need. Full scrub
also checks both mirrors, exact reachable objects, mappings, ancestry and
charged extents without building an inventory-sized map. The public
`list_inode_page` cursor belongs to the exact immutable snapshot.

Legacy shared-base members are decoded through the existing CPU path and
re-encoded as individual extent records. The export preserves decoded bytes,
file IDs, timestamps, descriptors and reservations in the exercised fixtures.
The next transaction engine needs recoverable live mutations, collective
representations and verified volume-head handoff before automatic adoption.
The handoff must bind the source and destination checkpoints independently
of the preserved namespace sequence. It also needs independently verified
layout equivalence and enough retained source state for immediate rollback.

The raw 1,024-file fixture measured 3,588,096 occupied bytes including history
and redundancy. Open read 90,736 backing bytes and a path lookup read 179,292.
Full scrub read 292,261,348 bytes. These counts expose the current metadata
overhead and guide page-layout and scrub work. [Executed evidence](evidence/native-indexed-volume-2026-11-17.json)
