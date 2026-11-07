# FlitzFS

> **0.3.2 experimental CPU storage, native FLZIP decoding and Windows callbacks.**
> This is not yet a general-purpose mounted filesystem, encryption product or
> GPU-accelerated product. Do not store the only copy of valuable data in this format.
> No tests have been executed on the owner's Windows PC or AMD GPU.

FlitzFS develops transparent compressed Windows storage with eventual AMD HIP
acceleration and authenticated encryption. Current code provides independently
addressable `.flfs` chunks, a synchronized read-only directory catalog, a portable
Windows-path projection, and an optional native WinFsp callback adapter.

## Implemented

- C++20 streaming pack/read/extract CLI; CPU Zstandard with raw fallback.
- Independently checked chunks, 128 KiB default (4 KiB to 1 MiB, power of two),
  random access with a 4 MiB request limit and a one-chunk cache per reader.
- Read-only regular-file OS handles, final symlink/reparse-point rejection,
  strict offset reads, index validation and per-chunk CRC32 corruption checks.
- New-file-only pack/extract creation, no force/overwrite mode, 256 MiB default
  input/output budgets, explicit 4 GiB ceiling and 1 GiB free-space reserve.
- Up to 16 virtual files with nested directories, conservative ASCII names,
  case-insensitive lookup, collision rejection and per-file read locks.
- A 4 MiB aggregate serialized index limit enforced before index allocation.
  This limit is not a complete process-memory ceiling.
- Whole-catalog verification: 32 MiB default, 256 MiB maximum; Windows-style
  projection with bounded short reads and typed errors.
- **Optional native WinFsp read-only callbacks**, OFF by default: Open/Close,
  metadata, owner-only descriptors, reads, paginated directory enumeration and
  explicit write rejection. At most 256 open contexts and two active decodes;
  all contents verified under a 32 MiB total budget before callback exposure.

The native callback tests link the official user-mode SDK DLL but do **not**
create a filesystem, install/load a kernel driver, assign a mount point, start
a dispatcher or modify Windows services. See [native architecture, research,
bounds and tests](docs/WINFSP_CALLBACKS.md).

## Isolated integration work

An explicit lifecycle layer now handles startup and rollback, and a Windows Job
Object runner limits trusted CPU helpers. The optional live probe tests actual
read-only mounted I/O using synthetic files; see [its acceptance gates and limits](docs/LIVE_MOUNT_PROBE.md).
It is not part of default CTest and accepts no user data or mount paths.
The separate [process-crash probe](docs/CRASH_RECOVERY_PROBE.md) tests supervised
termination, unchanged backing data, independent CPU verification and remount.

## Persistent read-only collections

An 8 KiB/16-record `.flcatalog` manifest now maps virtual names to sibling
containers. Use `catalog-list`, `catalog-verify` and `catalog-probe` to inspect
and read a collection without mounting it. The loader is read-only and rejects
unsafe names and namespace collisions before opening backing containers.
See [the versioned manifest format and limitations](docs/CATALOG_MANIFEST.md).
This metadata is plaintext and unauthenticated, not an encrypted store.

## Bounded FlitzZip compatibility

The separate CPU FLZIP reader implements all block kinds in the pinned v1–11
contract, including native entropy/dictionary/composition encodings. Deflate,
Zstandard and dictionary decoding require their optional codec libraries.
Every file is CRC-checked before exposure. The immutable snapshot is limited to
34 MiB physical/32 MiB logical bytes and at most 16 files. Non-ASCII paths and
archives exceeding the documented limits remain unsupported. See [the exact
compatibility contract](docs/FLZIP_READONLY.md). This does not initialize a GPU.

`flzip-list`, `flzip-verify`, and `flzip-probe` are read-only. The same reader
feeds the catalog/projection/native callback API. This is not a general-purpose
mount command or authentication. Existing `.flfs`/`.flcatalog` formats are unchanged.

## Not implemented or qualified

There is no general-purpose mount application, virtual-drive product, custom
filter driver, AMD GPU initialization, encryption or authenticated storage. An
optional synthetic-only hosted-CI mount probe exists. Writable transactions,
power-loss recovery, authenticated snapshots and full FlitzZip product parity remain future work.
Synthetic read-only process-termination recovery is tested separately. CRC32
detects accidental corruption, not malicious tampering.

The `.flfs` v1 format is experimental. Keep source files and independent backups.
Direct Reader calls are single-threaded; catalog operations serialize per file.
Never move or destroy a catalog/adapter while callbacks are active. Callback
unit tests do not prove mounted Windows behavior or full NTFS compatibility.

## Build and bounded CPU tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --timeout 45 --parallel 1
```

With Zstandard headers/library available, CPU compression is enabled. Otherwise
the `.flfs` packer stores raw chunks and cannot decode existing Zstandard chunks.
Use `-DFLITZFS_REQUIRE_ZSTD=ON` to reject missing dependencies rather than silently
falling back. To disable external codec libraries, set `-DFLITZFS_WITH_ZSTD=OFF`,
`-DFLITZFS_WITH_ZLIB=OFF` and `-DFLITZFS_WITH_LZ4=OFF`. Self-contained native
FLZIP representations remain readable; unavailable codecs fail explicitly.
Windows raw-only builds require Visual Studio and CMake. The optional native
callback build and SDK layout are documented in [WINFSP_CALLBACKS.md](docs/WINFSP_CALLBACKS.md).
Normal configuration performs no downloads or installation.

### CLI — only on expendable test files

```sh
flitzfs pack sample.dat new-sample.flfs
flitzfs info new-sample.flfs
flitzfs verify new-sample.flfs
flitzfs probe new-sample.flfs 4096 2048
flitzfs extract new-sample.flfs newly-extracted.dat
```

Pack/extract outputs must not exist; exclusivity is enforced at OS creation.
`info` checks metadata, not all payloads. `verify` checks every decoded chunk
under a 256 MiB default limit. Explicit `--max-mib` accepts 1..4096 for pack,
extract and verify. Pack also accepts `--chunk-kib` (4..1024, power of two) and
`--level` (1..5). Interrupted pack/extract can leave a partial **new** output;
it never replaces the input or an existing output. Keep source data unmodified.

## Safety and evidence

There are no disk-formatting, partitioning, raw-sector or GPU operations.
Driver provisioning is isolated to the explicitly opted-in disposable hosted-CI
mount lane; it is forbidden on the owner machine. Test fixtures are uniquely
scoped and small; tests never recursively delete directories. Free-space checks and resource limits
reduce risk but cannot guarantee safety against concurrent disk use, hardware
failure or software defects. The owner machine is not used for testing.

GitHub CI separates Linux Zstandard, Linux raw-only, Windows raw-only, Linux
ASan/UBSan, and Windows native callbacks with commit-pinned Zstandard. SDK
provisioning is checksum-pinned extraction on disposable hosted runners, not
installer execution in those lanes. A separate gated `windows-mount` job
provisions the verified official driver on its disposable VM only; see the
[synthetic mount probe contract](docs/LIVE_MOUNT_PROBE.md). Check the exact
commit's Actions results; do not infer a Windows/native pass from Linux results or infer mount qualification from native
callback tests.

## Design references

- [Current native callback milestone and research](docs/WINFSP_CALLBACKS.md)
- [Wire format v1](docs/FORMAT-v1.md)
- [Safety and threat model](docs/SAFETY.md)
- [Architecture](docs/ARCHITECTURE.md) and [development gates](docs/ROADMAP.md)
- [Prior research and all 13 other repository reviews](docs/RESEARCH_2026-11-07.md)

Reuse FlitzZip only after source-level compatibility, provenance and safety
review. No private repository code, credentials, personal paths, generated
binaries or SDK payloads are committed. Development stays on `main` only.

## Native-format CPU recovery (0.3.1)

The bounded FLZIP reader now implements static/adaptive prefix, Huffman,
short/long sparse patterns, optional LZ4 dictionary segments, compound frames,
byte-plane transforms and independent byte-plane contexts. With the three
optional codec libraries present, all block kinds defined by the pinned v1–11
contract are recognized. This does not lift path, archive or resource limits,
and it is **CPU recovery of GPU-native encodings, not GPU acceleration**.

`FLITZFS_WITH_LZ4=OFF` removes dictionary decoding; dependent composed frames
fail explicitly. `FLITZFS_REQUIRE_LZ4=ON` makes missing LZ4 a configure error.
No build downloads dependencies automatically. See
[native-codec plan and evidence](docs/NATIVE_CODEC_PLAN.md) and
[bounded compatibility](docs/FLZIP_READONLY.md). Windows, upstream-reference
and mounted assertions must be matched to the exact commit's Actions results.

## Bounded HIP-kernel preparation (0.3.2)

The first standalone HIP kernel candidate decodes raw, fill, pattern, static
prefix, adaptive prefix and Huffman segments. It is not loaded by the product.
The CPU-tested batch planner admits at most eight 64 KiB blocks and 256 KiB
total output. An independent CPU result, complete per-task status, generation,
deadline and fresh adapter-matched memory budget are required before the result
gate accepts candidate bytes. Failed/late results cannot reopen the gate.

The Windows DXGI collector uses a verified LUID/node mask, not adapter order or
HIP's optimistic Windows free-memory estimate. The collector is compiled, but
ordinary tests reject invalid requests before querying an adapter. These tests
do not establish a successful query on AMD hardware. Default commands still
never initialize HIP or perform GPU work. All existing file formats are unchanged.

A separate offline CI lane compiles the actual HIP source into a gfx1201 AMDGPU
code object without loading it. CPU execution of the shared kernel body is
semantic testing, not a GPU benchmark. Live worker integration, hardware parity,
GPU timeout handling and Windows HIP toolchain qualification remain unpassed.
Read [the research, acceptance plan and limits](docs/HIP_BATCH_PLAN.md) and
[executed qualification evidence](docs/HIP_BATCH_VALIDATION.md).
