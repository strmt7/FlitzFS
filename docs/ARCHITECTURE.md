# Architecture: implemented layers and remaining gates

## Implemented in 0.2

```text
CLI pack: ordinary source -> bounded CPU Zstd/raw chunks -> NEW .flfs v1
CLI read: validated index -> touched chunks -> CPU decode + CRC -> range

optional native WinFsp callbacks (no mount or dispatcher)
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

## Next: read-only mounted integration (not implemented)

A lifecycle layer must create/stop the filesystem and dispatcher, reserve a
nonconflicting mount point, prevent backing-path recursion, and enforce process
budgets. First integration belongs on a disposable Windows environment, using
copied synthetic data and an explicitly provisioned trusted driver. No automatic
driver installation or mount on the owner's PC is authorized by this preview.

Qualification needs clean unmount/restart, open/close races, cancellation,
memory-mapped reads, directory pagination, actual access checks and independent
CPU export. Callback tests alone do not pass those gates. Persistent catalog
configuration and FlitzZip format compatibility also remain separate work.

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
