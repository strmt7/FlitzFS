# FlitzFS

FlitzFS is an experimental compressed filesystem under development. Its goal
is high sustained read/write throughput combined with exceptional lossless
storage reduction across arbitrary file types, using modern AMD GPUs where
they provide a measured benefit and retaining independent CPU recovery.

This is an early filesystem implementation, not a production-ready volume.
Writable mounted storage and actual GPU execution remain unqualified. Keep
independent copies of valuable data. All current development and tests run
on disposable hosted machines; no storage, drivers or GPU tests run on the
owner's PC.

## Current filesystem components

- Native independently addressable raw/Zstandard chunk storage, with bounded
  reads, index validation, corruption checks and reused CPU codec workspaces.
- An experimental transactional object core: atomic bounded updates, immutable
  snapshots, integrity-bound metadata, alternating recovery roots, two durable
  publication barriers and independent CPU reopen. It now supports native raw, zero, repeated-pattern and optional Zstandard
  records, with private CPU round-trip verification and decoded-content integrity.
  It has no writable mounted route. See [the format and assumptions](docs/OBJECT_STORE_FORMAT.md).
- A native transactional file layer with persistent directories and inode IDs,
  sparse 64 KiB extents, partial writes, truncate/regrow, rename/replace, deletion
  and immutable snapshots. It currently admits 64 inodes and 15 nonzero extents;
  see [its semantics, format and limits](docs/NATIVE_FILESYSTEM.md).
- A read-only directory catalog and Windows path projection with bounded
  verification, per-file synchronization and conservative namespace validation.
- Optional native WinFsp read-only callbacks, disabled by default. Hosted VM
  probes exercise mounted reads, mapping, enumeration, write rejection and
  supervised process termination/recovery.
- Experimental compression mechanisms and offline-compiled HIP kernels, with
  CPU references, bounded batches, isolated worker transport and evidence-aware
  GPU recovery policy. These are preparation for filesystem acceleration;
  ordinary operations do not initialize a GPU.

Read [the architecture](docs/ARCHITECTURE.md),
[compression objectives](docs/COMPRESSION_STRATEGY.md) and
[development gates](docs/ROADMAP.md) for the current boundaries.

## Development direction

Native filesystem extent layout, adaptive lossless compression, durable
namespace operations, crash recovery and conventional file semantics drive the
design. Codec selection is open; existing algorithms are candidates rather than
a fixed registry inherited from another application. Compression gains must
include metadata, padding, dictionaries, retained history and write amplification.

Compression research is evaluated against filesystem workloads: mixed file
types, partial updates, random reads, durable commits and independent CPU
recovery. Candidate mechanisms and their source attribution are documented in
[extent codec research](docs/EXTENT_CODECS.md).

## Building

Use a disposable development VM for this repository under the owner's current
host restriction:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --timeout 45 --parallel 1
```

The base requires C/C++ compilers and CMake. Zstandard enables CPU compressed
chunks; a minimal build uses raw storage. LZ4 is optional for experimental
dictionary mechanisms. Require dependencies explicitly with
FLITZFS_REQUIRE_ZSTD/FLITZFS_REQUIRE_LZ4; disable them with the corresponding
FLITZFS_WITH_* options. Configuration downloads or installs nothing.

The CLI is a diagnostic utility for native .flfs backing images and read-only
.flcatalog fixtures. Its current pack/extract commands construct a single native
test image or read its bytes for validation. Normal application access through
a writable mounted filesystem remains to be implemented. Outputs must be new
paths; original files remain unchanged. See [the native image format](docs/FORMAT-v1.md) and
[catalog diagnostics](docs/CATALOG_MANIFEST.md).

## Qualification still required

The object engine remains limited to 16 objects, 1 MiB per object, 4 MiB new
payload per transaction and 64 MiB retained physical history. There is no
public existing-file writable reopen, reclamation, production namespace scaling,
authenticated encryption or general-purpose writable mount application.

A successful flush is meaningful only under qualified backend/device
assumptions. RAM fault injection and hosted process tests do not prove survival
of every physical power failure. GPU hardware speedups and superiority over
other filesystems require controlled measurements; neither is established.

CI covers Linux raw/codec builds, ASan/UBSan, Windows minimal/native builds,
hosted-only file I/O, disposable read-only mount/recovery, offline HIP compilation
and test-only compression-mechanism comparisons. Match evidence to the exact
commit. No owner-PC drivers, mounts, raw-device writes, partition changes or
stress tests are authorized. See [safety boundaries](docs/SAFETY.md).

Development stays on main. Coherent implementation, tests and documentation are
batched into larger checkpoints; release decisions belong to the owner.

## Persistence continuation work

The native object core can resume a qualified exclusive backend after full CPU
verification, explicit identity/generation checks and a durable recovery fence.
It preserves orphan tails and repairs a missing root without overwriting the
survivor. Public file factories remain new-file-only. Read
[the protocol and hosted interruption gates](docs/STORE_RESUME_AND_INTERRUPTION.md).
This is storage-core development; the new native file layer remains separate from writable mounted callbacks.
