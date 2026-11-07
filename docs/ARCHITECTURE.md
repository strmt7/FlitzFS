# Architecture: implemented layers and remaining gates

## Implemented in 0.2

```text
CLI pack: ordinary source -> bounded CPU Zstd/raw chunks -> NEW .flfs v1
CLI read: validated index -> touched chunks -> CPU decode + CRC -> range

optional WinFsp session (explicit lifecycle provider)
    -> native read-only callbacks
    -> immutable metadata and owner-only descriptors
    -> bounded opaque open contexts and active reads
    -> ReadOnlyProjection (Windows virtual path + EOF semantics)
    -> ReadOnlyCatalog (namespace + per-file synchronization)
    -> Reader (indexed chunks + one decoded-chunk cache)
    -> pinned ReadOnlyFile handle (ordinary existing file only)
```

### Container and host I/O

The v1 container holds one logical byte stream. It uses independent chunks and
CRC32, not encryption or authentication. `Reader::open(path, index_budget)`
validates header/counts and rejects excess serialized-index bytes before index
allocation. Its absolute ceiling is 32 MiB; catalogs pass their remaining 4 MiB
aggregate allowance. Entry vectors and caches consume additional memory.

Physical host paths are screened separately from virtual names. Windows host
paths reject dangerous namespaces, DOS device aliases and stream/NUL syntax
before OS operations; input/output handles are validated as ordinary files.
POSIX reads reject final symlinks and blocking special files. Parent-directory
link containment and malicious concurrent mutations remain limitations.

Pack/extract only create NEW files. Existing outputs cannot be replaced; no
source deletion occurs. Interrupted operations may leave a partial new file.
There is no transactional writable filesystem, snapshot or recovery journal.

### Catalog and projection

The optional 8 KiB plaintext `.flcatalog` manifest binds up to 16 virtual names
to sibling `.flfs` filenames. Pure parsing validates all names and collisions
before the loader opens backing files. CLI list/verify/probe operations and the
hosted live probe use this layer. Metadata is not authenticated or encrypted;
parents must be trusted. See `CATALOG_MANIFEST.md`.

The catalog accepts up to 16 `.flfs` sources and builds a conservative ASCII,
case-insensitive directory tree. Name validation covers the entire component,
not just the stem. Paths are at most 240 characters, components 100, depth 16.
Conflicting file/directory names and duplicate case-folded names are rejected.

Construction validates metadata, not every payload. Reads check touched chunks;
`verify_all` explicitly checks payloads under an aggregate 32 MiB default / 256
MiB maximum budget. Per-file mutexes protect the reader cache and Windows file
position. Direct Reader calls remain single-threaded.

`ReadOnlyProjection` translates root-relative Windows virtual names without
resolving real OS paths. It admits reads up to 4 MiB and returns short reads at
EOF. Typed errors allow native status translation without parsing messages.

### Native WinFsp read-only callbacks

The OFF-by-default `WinFspReadOnly` backend implements the actual native SDK
interface. It verifies at most 32 MiB total content before exposure, returns
metadata/security/directory records, handles short/EOF reads and rejects all
implemented mutations. `Cleanup` preserves contexts until `Close`, allowing
subsequent paging-style reads. Fixed handle slots use monotonically increasing
tokens so a stale close cannot affect a reused slot.

There are at most 256 contexts and two active decodes. The callback boundary
converts failures to NTSTATUS and never throws through the C ABI. Directory
continuation uses one order: `.` then `..`, then ASCII case-folded ordinary
names. Explicit dot ranks prevent leading-space or punctuation names from being
skipped, and prevent dot entries from repeating on later pages.

Metadata is synthetic and lifetime-local: file IDs last for one adapter, times
reflect construction, and allocation sizes are logical sizes rounded to 4096.
The process user gets a protected read/execute ACE; source ACLs are not retained.
The caller must stop all callbacks before destroying the adapter.

Direct callback tests link the official SDK/DLL but do not create a filesystem,
install a driver, start a dispatcher or mount anything. See
[WINFSP_CALLBACKS.md](WINFSP_CALLBACKS.md) for ABI research and dependency pins.

## Read-only mounted integration and process supervision

The implemented lifecycle layer takes an explicit WinFsp API provider and
starts at most two dispatcher workers before exposing a namespace. It stops and
joins those callbacks before releasing the adapter. Injected tests exercise
startup failures, rollback, stop notifications and single-use ownership.
A separate Windows Job Object runner limits trusted CPU helpers before they
execute: default 128 MiB committed memory, maximum 20% CPU, one active process,
below-normal priority and bounded waits. This is not a security or kernel sandbox.

The optional hosted-only synthetic probe uses the real provider, a fresh
pinned fixture parent and create-only directory leaf. It verifies ordinary,
random and mapped reads, enumeration, mutation rejection and orderly shutdown.
A second child is forcibly terminated after proving working mounted I/O; a
third verifies unchanged backing data and remounts it. See `LIVE_MOUNT_PROBE.md`
and `CRASH_RECOVERY_PROBE.md` for exact qualification scope and run evidence.
Only its explicit disposable hosted-Windows lane may provision the driver.
No automatic driver installation or mount on the owner's PC is authorized.

A general-purpose mount application, in-flight cancellation/open-close races,
cross-user authorization, Windows 11, broad application compatibility and
FlitzZip format compatibility remain separate gates. Persistent plaintext
configuration does not satisfy transactional or authenticated metadata needs.

## Later: adaptive HIP (not implemented)

HIP will be optional; CPU decoding must always remain available. Initial GPU
experiments must be opt-in, without a mount, with tiny synthetic buffers, one
queued batch, bounded allocation and duration, and byte parity against CPU.
The proposed first memory cap is at most 128 MiB, not permission to allocate it
without checking actual headroom. Do not alter clocks, voltage, TDR or drivers.
Measure complete host/device transfers, kernels, synchronization and application
handoff; do not infer throughput from GPU-local bandwidth. Busy GPUs and small
requests require measured CPU fallback policies.

## Later: authenticated and writable storage (not implemented)

Design versioned AEAD records, authenticated metadata, key hierarchy, password
unlocking and nonce safety across crashes/clones/restores with a vetted crypto
library. Compress before encryption. Keep CPU-only recovery independent of GPU
availability. Writable support additionally needs an append-only landing log,
copy-on-write metadata, explicit flush semantics, compaction/free-space reserves
and fault injection at every commit point before any mounted write preview.

## Verified external FLZIP view

`FlzipReader` privately owns bounded compressed bytes, parsed nodes and at most
one decoded block. No caller gets a view until every file CRC passes. The
catalog's explicit `open_flzip` factory serializes mutable cache operations;
immutable namespace queries are safe while the catalog remains alive. The
projection and native callback layers consume the same catalog API. This avoids
forking Windows I/O semantics for another archive format. There is no GPU
initialization, writable adapter or implicit FLZIP import in `.flcatalog`.
The test-only upstream index serializer is not linked into the product.
