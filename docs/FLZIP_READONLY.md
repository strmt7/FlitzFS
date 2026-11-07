# Bounded FLZIP read-only compatibility

## Plan and acceptance gates — 7 November 2026

This milestone implements a deliberately limited CPU reader, not all FlitzZip
codecs or a new archive writer. Source of truth: `strmt7/FlitzZip` at
`4e6a1bb0d144611a09dcbc65276faa676e923882`, particularly `archive_index.cpp`,
`archive.cpp`, `archive_block_types.hpp` and `docs/native-flzip-format.md`.
No upstream project is modified, no HIP runtime is linked/initialized, and
`.flfs`/`.flcatalog` formats remain unchanged.

1. Preserve SUZP/SUZF layout, little-endian framing and version-kind rules.
2. Support raw/fill/pattern and optional CPU Deflate/Zstandard blocks. Reject
   unimplemented native entropy/dictionary/composition codecs explicitly.
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

Versions 1–11 are recognized only when **every block** uses supported kinds:
raw (0), fill (1), pattern (3), optional zlib-wrapped Deflate (2), or optional
Zstandard (8; version 6+). Native prefix, adaptive prefix, dictionary, sparse,
Huffman, compound and byte-plane codecs are rejected with `unsupported_codec`.
No required-HIP fallback is claimed and no HIP code is executed.

Namespace limits are deliberately narrower than FlitzZip: conservative ASCII,
240-character paths, 100-character components, depth 16 and 16 ordinary files.
Directory records, including empty directories, are supported within 256
records/257 total nodes. Duplicate explicit directory records are rejected;
an explicit directory can appear after its descendants. This preview also
requires zero directory CRC and rejects overlapping entry payload windows.
Archives accepted by upstream may therefore exceed this reader's subset.

`FLITZFS_WITH_ZLIB` optionally enables Deflate support; `FLITZFS_REQUIRE_ZLIB`
turns a missing dependency into configuration failure. CPU-only minimal builds
can explicitly disable both zlib and Zstandard, retaining raw/fill/pattern.
The native Windows and serializer-oracle CI lanes build zlib v1.4 at
`99623ef7097352da7993ba614161fd6a311e046e`. Headers/static library are staged only
inside the disposable checkout; there is no system zlib installation. Other
Linux lanes exercise their distribution library; local compatibility results use
zlib 1.3.1 and Zstandard 1.5.7. Dependency availability and correctness tests are
not a security certification; production packaging remains unqualified.

## Independent oracle and provenance

The optional `FLITZFS_FLITZZIP_SOURCE` CMake path must identify the reviewed
upstream commit with no tracked core modifications. Only `archive_index.cpp`
and its headers are compiled, in a test executable. Upstream CMake scripts,
GUI and GPU code are not run. Fixtures are checked byte-for-byte against the
real upstream index writer and footer integer primitives, and their payloads
are independently decoded by FlitzFS. This is **serializer interoperability**,
not full FlitzZip CLI or native-GPU-codec compatibility. FlitzZip's AGPL-3.0
license remains upstream; no upstream code or binary is vendored or republished.

## Security and resource limits

CRC32 only detects accidental corruption. It does not authenticate metadata,
content, authorship or rollback. A copied archive may already be maliciously
modified; passing CRC does not establish authenticity. Copying is not a VSS or
atomic filesystem snapshot against concurrent writers; the admitted copy itself
is immutable after validation. Parent directories remain trusted, not sandboxed.

Limits cover individual buffers and collections, not arbitrary process RSS or
kernel memory. The live synthetic probe additionally retains its OS-enforced
128 MiB committed-memory/20% CPU/one-process/30-second job. Default CTest never
mounts or installs a driver. The new hosted `.flzip` probe covers Zstandard,
Deflate, fill/pattern, empty files/directories, random/mapped/concurrent reads,
write denial, clean shutdown and unchanged backing. The earlier `.flfs`
forced-termination recovery test remains separate; do not extend that evidence
to FLZIP crash recovery without its own executed test.
