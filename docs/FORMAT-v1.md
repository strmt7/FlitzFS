# `.flfs` v1 experimental chunk container

**Status: unstable development format; do not use as a sole archival or recovery copy.**
All integer fields are unsigned, little-endian. No implicit packing or machine-dependent structures are used. Metadata is checked before allocating block buffers. Data CRC32 is not cryptographic authentication.

## Byte layout

| Region | Byte count | Description |
|---|---:|---|
| Header | 40 | Magic and fixed-sized container metadata |
| Chunk payloads | Variable | Payloads are concatenated in ascending logical-chunk order |
| Index | `32 * chunk_count` | Fixed-size records pointing to independent chunk payloads |
| Footer | 32 | Commit marker and CRC32 for the complete index |

### Header (40 bytes)

| Offset | Width | Value |
|---:|---:|---|
| 0 | 8 | ASCII `FLFS0001` |
| 8 | 4 | Version = 1 |
| 12 | 4 | Logical chunk size, 4 KiB–1 MiB, power of two |
| 16 | 8 | Original logical file length |
| 24 | 8 | Number of chunks = `ceil(logical_length / chunk_size)` |
| 32 | 8 | Reserved = 0 |

### Chunk-index record (32 bytes each)

| Offset within record | Width | Value |
|---:|---:|---|
| 0 | 8 | Offset of stored payload in the container |
| 8 | 4 | Stored length in bytes, 1..2 MiB |
| 12 | 4 | Decoded length, expected chunk size except the last chunk |
| 16 | 4 | CRC32 (IEEE 802.3 polynomial) of complete decoded chunk |
| 20 | 1 | Codec 0 = raw, 1 = Zstandard |
| 21 | 3 | Reserved = 0 |
| 24 | 8 | Reserved = 0 |

Payload records must be contiguous and ordered; no holes, overlaps, dangling payloads, or trailing bytes are accepted. Raw chunk length must equal decoded length. Zstd decoding requires exactly one complete frame whose declared decoded length equals the chunk index. Single-shot decompression is bounded by the index maximum of 1 MiB; `ZSTD_d_windowLogMax` does not cap the window in this mode.

### Footer (32 bytes)

| Offset | Width | Value |
|---:|---:|---|
| 0 | 8 | ASCII `FLFSIDX1` |
| 8 | 8 | Index start offset |
| 16 | 8 | Total index bytes |
| 24 | 4 | CRC32 of index bytes |
| 28 | 4 | Reserved = 0 |

An incomplete write without a valid footer will not be accepted as a container. The footer is **not a transactional filesystem journal** and does not make the new output automatically durable across every possible interruption.

## Bounded parsing invariants

- At most 1,048,576 chunks and 32 MiB of serialized index metadata.
- Input-derived sizes are checked before the corresponding buffers are allocated.
- Max decoded chunk = 1 MiB; max encoded chunk = 2 MiB; max single random read = 4 MiB.
- A header/index inconsistency is rejected before payload decoding.
- `info` is metadata validation only; `verify` is required to check all stored chunks.
- Zstd compression falls back to raw data whenever it does not save at least 17 bytes.

### Reader admission in release 0.2 (no wire-format change)

`Reader::open(path, max_index_bytes)` may impose a smaller serialized-index
allowance. The count is checked against it before the index buffer is read or
allocated. Empty indices can fit a zero allowance. Catalogs pass the remainder
of a 4 MiB aggregate allowance when opening each file. Parsed entry arrays and
other allocations are additional memory; this is not a total process-RAM cap.

## Compatibility and migration

This single-file container holds exactly one logical byte stream. It is **not** the `.flzip` native archive format, and it is not itself a directory tree. File paths, ACLs, links, timestamps, immutable object IDs, journaling, and encryption metadata belong to later format generations. Data must be migrated explicitly rather than silently reinterpreting v1 as a writable filesystem.

The 0.2 native callback backend supplies an in-memory namespace and synthetic
metadata around existing containers. It writes none of that metadata into v1,
and does not change its bytes, codec identifiers or integrity guarantees.

CPU workspace reuse does not introduce shared history, dictionaries or new
codec identifiers. Every Zstandard chunk remains a complete independent frame;
raw fallback still requires a saving of at least 17 bytes. The regression suite
compares each reused-context frame against the original one-shot encoder at
levels 1 through 5. Hosted comparison additionally requires identical complete
containers and extracted content; see `CPU_PIPELINE.md` for executed evidence.

## Historical external-format experiment (removed)

The former read-only FLZIP adapter described in `FLZIP_READONLY.md` was removed from product scope. It never changed
`.flfs` v1 bytes or codec identifiers. Its SUZP/SUZF framing and file-wide CRCs
are parsed independently, with verification before exposure. Never interpret
FLZIP as a `.flfs`, ZIP alias, or authenticated container.

## Test-only durability envelopes are a separate experiment

The RAM protocol model in PERSISTENCE_MODEL.md uses distinct test magic and
fixed envelopes to investigate publication ordering. It changes no v1 fields,
codec identifiers or migration rules and is not an accepted writable format.
Never interpret those model envelopes as .flfs containers.

## Separate transactional object experiment

OBJECT_STORE_FORMAT.md defines distinct FLTXR001/FLTXIDX1/FLTXDAT1 records for
an experimental native object transaction library. It does not modify .flfs v1,
its codecs, CRCs, CLI or callback route. No automatic format migration exists.

The separate transaction experiment now admits raw, zero, motif and bounded
Zstandard object records with decoded-content integrity. These are not .flfs
v1 codec changes; historical native image bytes and chunk semantics remain
unchanged. See OBJECT_STORE_FORMAT.md for the separate record contract.

## Native filesystem namespace is a separate client format

NATIVE_FILESYSTEM.md defines FLTXNS01 inode/extent metadata inside the separate
transactional object store. It does not reinterpret .flfs/.flcatalog as writable
namespace formats. No automatic migration or archive import exists.

Current .flfs and object-store Zstandard decoders require modern frame magic
0xFD2FB528 before calling library parsers. Writer-produced bytes remain unchanged;
legacy/skippable or unknown framing is rejected explicitly.

## Cleanup metadata has no new wire encoding

The requested Cleanup bridge in NATIVE_CLEANUP_METADATA.md uses the existing
optional FLTXNS03 tuple and replicated publication. It adds no container codec,
archive compatibility, namespace layout or capacity change. Legacy bytes and
read support remain; request sentinels are never persisted as timestamps.

Atomic stable-inode mutation metadata in NATIVE_MUTATION_METADATA.md reuses the
existing FLTXNS03 tuple and transaction. It adds no wire field, codec, format
version or capacity change; legacy readers/bytes and native product scope remain.

Commit-bound result values in NATIVE_MUTATION_RESULTS.md are in-memory API
output only. They add no wire field, layout, metadata format, codec or capacity
change. Modified namespace sequence is never a UTC time or allocation count.
