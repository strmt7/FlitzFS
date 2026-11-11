# Historical experiment — retired from product scope

The archive reader/compatibility track described below has been removed. It
was outside the owner's filesystem objective. This document records earlier
work only; current mechanism research is in EXTENT_CODECS.md. Do not restore
archive commands, archive-backed mounts or archive compatibility requirements.

# Bounded FLZIP read-only compatibility

## Plan and acceptance gates — 7 November 2026

This milestone implements a deliberately bounded CPU reader, not the full
FlitzZip product or a new archive writer. Source of truth: `strmt7/FlitzZip` at
`4e6a1bb0d144611a09dcbc65276faa676e923882`, particularly `archive_index.cpp`,
`archive.cpp`, `archive_block_types.hpp` and `docs/native-flzip-format.md`.
No upstream project is modified, no HIP runtime is linked/initialized, and
`.flfs`/`.flcatalog` formats remain unchanged.

1. Preserve SUZP/SUZF layout, little-endian framing and version-kind rules.
2. Support all defined block kinds through bounded CPU decoding. Optional
   Deflate/Zstandard/LZ4 dependencies fail explicitly when unavailable.
3. Snapshot at most 34 MiB of archive bytes. Admit at most 1 MiB of index,
   256 records, 16 files, 8192 blocks, 257 virtual nodes, 16 MiB per decoded
   block and 32 MiB total logical output **before** decoding. No user option
   increases these caps; an optional logical budget may only lower them.
4. Validate every path/collision/extent/codec before decoding. Verify every
   whole-file CRC before returning an accessible snapshot. Keep no dependency
   on later backing-file mutation; CRC is not authentication.
5. Prove random reads/EOF/path rejection against deterministic fixtures and
   malformed metadata, frames, checksums, truncations and bounded mutations.
6. Compare archive metadata against the real pinned upstream index serializer,
   compiled as a test-only oracle. Do not call that full CLI interoperability.
7. Integrate the same reader into the catalog/projection/native callback route,
   then qualify it with actual mounted reads on disposable hosted Windows only.

## Research rounds

Round 1 inspected the pinned FlitzZip wire writer and metadata validator.
Block offsets are entry-relative; payload blocks are dense within each entry.
Fill offsets are unused. FLZIP persists a CRC for each file, not each block.
A small compressed-byte snapshot plus verification-before-exposure avoids
pretending a random range can validate an entire file. Empty directories are
represented; the existing conservative ASCII virtual namespace remains in use.

Round 2 read the official zlib and Zstandard APIs. Deflate means one complete
zlib-wrapped stream, not ZIP, gzip or raw DEFLATE. `uncompress2` reports consumed
source bytes; both input consumption and decoded length must match. Zstandard
requires standard frame magic, one exact frame, a known matching decoded size
and no required external dictionary. Codec results never set allocation caps.

Round 3 added byte-exact and cross-block tests, an independent bitwise CRC
reference, every truncation of a small valid archive, bounded one-byte mutations,
version/codec mismatches, malicious paths, collisions, overlapping payloads,
invalid counts, nonempty directory metadata and exact compressed-stream checks.
The tests also prove that changing the input memory or source file after opening
does not change the verified in-memory snapshot. Windows/serializer/live-mount
assertions still require the matching CI result, not a Linux-only inference.

Round 4 rechecked the release/security pages rather than relying on stale manual
version banners. zlib 1.4 was released on 4 November 2026 with security fixes;
the Windows/oracle pin was upgraded before publication. Its rewritten CMake
uses `ZLIB_BUILD_TESTING` and names the MSVC static library `libzs.lib`, so the
staging script selects that exact artifact. Unused gzip/minizip tools are not
built or invoked. Tag-to-commit resolution is recorded; GitHub marks the tag's
signature unverified (`bad_email`), so no verified-signature claim is made.

Primary sources:
- https://github.com/strmt7/FlitzZip/tree/4e6a1bb0d144611a09dcbc65276faa676e923882
- https://zlib.net/
- https://zlib.net/manual.html
- https://github.com/madler/zlib/blob/99623ef7097352da7993ba614161fd6a311e046e/CMakeLists.txt
- https://facebook.github.io/zstd/zstd_manual.html
- https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew

The zlib and Zstandard manuals currently served through different endpoints
carry different version banners. APIs are checked against pinned CI source;
no claim that a tested pin is the newest release is made.

## Implemented API and commands

`FlzipReader::open` reads an ordinary no-follow file into a bounded private byte
copy; `from_bytes` makes its own copy. Parsing and whole-file CRC verification
finish before either returns. `stat`, `list`, strict `read`, and `verify_all`
operate only on that copy. A single decoded block is cached, and old cache
storage is released before decoding a different block. The synchronized
`ReadOnlyCatalog::open_flzip` and `ReadOnlyProjection::open_flzip` route this
same reader to native callbacks without a second format parser.

```text
flitzfs flzip-list archive.flzip
flitzfs flzip-list archive.flzip Data
flitzfs flzip-verify archive.flzip
flitzfs flzip-probe archive.flzip Data/test.bin 0 64
```

All commands fully verify the admitted snapshot before listing/reading. There
is no FLZIP writer, extract-overwrite command or general-purpose mount command.
Detection uses framing, not the extension, so a structurally valid renamed
archive works. The `.flcatalog` v1 grammar still references only `.flfs` files.

## Supported subset, explicitly not full FlitzZip support

Versions 1–11 enforce their original version/kind rules. CPU implementations
cover raw (0), fill (1), pattern (3), static prefix (4), adaptive prefix (5),
short sparse (7), long sparse (9), Huffman (10), compound (11), byte-plane (12)
and independent byte-plane contexts (13). Deflate (2), dictionary (6) and
Zstandard (8) require zlib, LZ4 and Zstandard respectively. Compositions may
contain dictionary stages and fail explicitly without LZ4, even though their
outer kind is recognized. No required-HIP fallback or GPU execution is claimed.

Namespace limits are deliberately narrower than FlitzZip: conservative ASCII,
240-character paths, 100-character components, depth 16 and 16 ordinary files.
Directory records, including empty directories, are supported within 256
records/257 total nodes. Duplicate explicit directory records are rejected;
an explicit directory can appear after its descendants. This preview also
requires zero directory CRC and rejects overlapping entry payload windows.
Archives accepted by upstream may therefore exceed this reader's subset.

`FLITZFS_WITH_ZLIB` optionally enables Deflate support; `FLITZFS_REQUIRE_ZLIB`
turns a missing dependency into configuration failure. CPU-only minimal builds
can explicitly disable all three external codecs, retaining the self-contained
CPU native encodings and compositions that do not need dictionary decoding.
All codec-enabled hosted CI lanes build zlib v1.4 at
`99623ef7097352da7993ba614161fd6a311e046e`. Headers/static library are staged only
inside the disposable checkout; there is no system zlib installation. Windows/oracle lanes also pin LZ4 1.11.0 at
`c72c7c0ec132ad2ce2fe9fad7b0c728d9faee2fc`; other Linux lanes exercise their
distribution LZ4/Zstandard. Local compatibility results use LZ4 1.10.0,
zlib 1.3.1 and Zstandard 1.5.7. Dependency availability and correctness tests are
not a security certification; production packaging remains unqualified.

## Independent oracle and provenance

The optional `FLITZFS_FLITZZIP_SOURCE` CMake path must identify the reviewed
upstream commit with no tracked core modifications. The real `archive_index.cpp`
and headers check metadata serialization. A separate test-only oracle extracts
exact primitive CPU decoder functions from the reviewed `archive_blocks.cpp`
blob, verifies its Git blob identity, and uses the upstream composition parsers.
No upstream runtime, GPU, GUI or CMake script is linked or executed. The generated
reference unit stays in the test build directory; it is not product code.

Independent fixture encoders are compared byte-for-byte with the pinned index
writer, original bytes, the independent FlitzFS decoder and the reference CPU
materializers. This is bounded serializer/decoder interoperability, not the full
FlitzZip CLI or actual HIP encoder qualification. Preserve upstream AGPL-3.0
provenance for generated reference code; no upstream source/binary is vendored.

## Security and resource limits

CRC32 only detects accidental corruption. It does not authenticate metadata,
content, authorship or rollback. A copied archive may already be maliciously
modified; passing CRC does not establish authenticity. Copying is not a VSS or
atomic filesystem snapshot against concurrent writers; the admitted copy itself
is immutable after validation. Parent directories remain trusted, not sandboxed.

Limits cover individual buffers and collections, not arbitrary process RSS or
kernel memory. The live synthetic probe additionally retains its OS-enforced
128 MiB committed-memory/20% CPU/one-process/30-second job. Default CTest never
mounts or installs a driver. The hosted `.flzip` probe covers native entropy/dictionary/composition families, Zstandard,
Deflate, fill/pattern, empty files/directories, random/mapped/concurrent reads,
write denial, clean shutdown and unchanged backing. The earlier `.flfs`
forced-termination recovery test remains separate; do not extend that evidence
to FLZIP crash recovery without its own executed test.

### Native decoder memory and framing

Entropy segments decode 4 KiB at a time; dictionary segments decode at most
64 KiB through `LZ4_decompress_safe`. Tables are bounded by the admitted decoded
extent, never by an unchecked input count. Huffman tables must be complete and
prefix-consistent. Sparse corrections are ordered, in range and nonredundant.
Composition stages form a closed, nonrecursive set, with exact dense framing.
The byte-plane inverse is an exact permutation including incomplete tails.

Composed codecs use at most one extra decoded-block-sized temporary (16 MiB
maximum), in addition to existing snapshot/output/cache allowances. This is
not a global process-memory limit. Legacy unused prefix-segment padding remains
accepted for compatibility. Whole-file CRC still precedes exposure and is not
cryptographic authentication. See NATIVE_CODEC_PLAN.md for source pins and tests.
