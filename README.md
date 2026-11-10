# FlitzFS

FlitzFS is an experimental **Windows-only compressed filesystem** under development,
targeting the latest stable Windows 11 on AMD64 with a Windows helper application
and qualified filesystem-driver integration. Older Windows and other operating
systems are outside the current product scope. Its goal
is high sustained read/write throughput combined with exceptional lossless
storage reduction across arbitrary file types, using modern AMD GPUs where
they provide a measured benefit and retaining independent CPU recovery.

This is an early filesystem implementation, not a production-ready volume.
Writable mounted storage and actual GPU execution remain unqualified. Keep
independent copies of valuable data. All current development and tests run
on disposable hosted machines; no storage, drivers or GPU tests run on the
owner's PC.

The Windows app, full writable driver integration and latest Windows 11 client
qualification remain unfinished. Existing Linux CPU/sanitizer jobs are internal
algorithm checks, not Linux product support. See [the Windows product contract](docs/WINDOWS_PRODUCT.md) and
[current Windows architecture research](docs/WINDOWS_ARCHITECTURE.md).

## Current filesystem components

- Native independently addressable raw/Zstandard chunk storage, with bounded
  reads, index validation, corruption checks and reused CPU codec workspaces.
- An experimental transactional object core: atomic bounded updates, immutable
  snapshots, integrity-bound metadata and independent CPU reopen. Native file
  transactions now publish replicated current checkpoints through three durable
  barriers, with separate root and manifest units; legacy object mode remains
  readable. See [the checkpoint protocol](docs/REPLICATED_CHECKPOINTS.md). It now supports native raw, zero, repeated-pattern and optional Zstandard
  records, with private CPU round-trip verification and decoded-content integrity.
  A verified compaction path copies live data into a new volume identity and
  retains the source; it does not replace/delete backing files or enable mounted
  writes. See [the format and assumptions](docs/OBJECT_STORE_FORMAT.md) and
  [copy protocol](docs/COMPACTED_COPY.md).
- A native transactional file layer with persistent directories and inode IDs,
  sparse 64 KiB extents, partial writes, truncate/regrow, rename/replace, deletion
  and immutable snapshots. Exact unchanged extent bytes retain their immutable
  records while metadata updates keep the full durability protocol, reducing
  redundant encoding and appended payload history. It currently admits 256 inodes and 255 nonzero extents;
  see [its semantics, format and limits](docs/NATIVE_FILESYSTEM.md).
- Requested Windows Cleanup archive/time effects now share one atomic native
  transaction with optional unlink. A user-mode SDK bridge fences failures
  across Cleanup's void ABI; owning contexts remain retained until Close.
  This does not enable writable mounts; see [the contract](docs/NATIVE_CLEANUP_METADATA.md).
- Owning-session stable-inode writes/truncates can now include requested
  archive/time effects in the same durable transaction, preserving detached
  paging identity, accepted counts and old snapshots; see [the contract](docs/NATIVE_MUTATION_METADATA.md).
- Commit-bound inode/basic/lifetime results now let the Windows engine obtain
  durable mutation facts without a second backing query after publication;
  see [the output contract](docs/NATIVE_MUTATION_RESULTS.md).
- A hosted read-only Windows space oracle now records standard/compression/
  volume API results separately from native backing usage; see [the reporting
  gate and driver limitation](docs/WINDOWS_SPACE_REPORTING.md).
- Opt-in compact checkpoint metadata reduces catalog padding under an explicit
  qualified backend failure-unit contract, preserving separate roots and three
  barriers; see [the protocol and qualification limits](docs/COMPACT_CHECKPOINT_METADATA.md).
- A read-only directory catalog and Windows path projection with bounded
  verification, per-file synchronization and conservative namespace validation.
- Optional native WinFsp read-only callbacks, disabled by default. Hosted VM
  probes exercise mounted reads, mapping, enumeration, write rejection and
  supervised process termination/recovery.
- Experimental compression mechanisms and a HIP kernel; hosted checks are
  configured for the complete selected ROCm SDK, with
  CPU references, bounded batches, isolated worker transport and evidence-aware
  GPU recovery policy. These are preparation for filesystem acceleration;
  ordinary operations do not initialize a GPU. An explicit
  [worker request controller](docs/GPU_WORKER_CONTROLLER.md) now connects recovery
  policy, terminal CPU fallback and exact asynchronous worker retirement; its
  hosted integration fixtures execute CPU semantics only.

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

The final filesystem must learn from file contents and observed workloads,
automatically improve compression/layout, and repair from verified redundancy.
Background work must run at very low priority with strict resource caps and
yield to applications, while using qualified AMD acceleration where beneficial.
These mechanisms remain unfinished; [the required architecture and
qualification gates](docs/AUTONOMOUS_MAINTENANCE.md) distinguish them from
implemented storage primitives.

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

The object engine remains limited to 256 objects, 16 MiB total decoded live data,
1 MiB per object, 4 MiB new
payload per transaction and 64 MiB retained physical history. There is no
public existing-file writable reopen, automatic reclaim/handoff, production namespace scaling,
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

The explicit Windows worker transport now has an asynchronous session: verified
CPU bytes can be selected while a separately supervised helper is still live.
Cancellation and exact process/thread retirement stay outside CPU selection.
This remains CPU-tested infrastructure; no normal filesystem read loads HIP or
automatically starts a worker. See docs/WORKER_CHANNEL.md for its ownership and
qualification boundaries.


Current ROCm qualification at 35dfac90503e46b9f988b8ecfbe40f255ca9b809
[passed on a disposable hosted Windows VM](https://github.com/strmt7/FlitzFS/actions/runs/42684196735):
the complete 10.1.0 distribution compiled the filesystem kernel and linked its
separate module; actual headers reported HIP 7.16.26385-7da43026e4. The module
was never loaded and no GPU work ran. The next typed-entry/recovery changes
require their own exact-commit checks; see docs/HIP_RUNTIME_WORKER.md.


The Windows read-only adapter now has an explicit source for the native
transactional inode/extent image, with UUID-bound admission, persistent IDs,
serialized verified reads and bounded sparse files. The hosted mount/recovery
probe uses that native image; results require the matching commit's checks.
See [native Windows integration](docs/NATIVE_WINDOWS_PROJECTION.md).
Writable mounted volumes, the helper app and live GPU execution remain unfinished.


The native engine also has stable-inode reads/mutations and atomic append/
constrained-write entry points; see [NATIVE_INODE_IO.md](docs/NATIVE_INODE_IO.md).
They prevent old-path reuse from redirecting an inode write and retain the same
replicated durability path. Full Windows handle/delete lifetime and writable
mounting remain unfinished; qualification applies only to inspected commits.


Persistent delete intent/cancellation and detached-inode data retention now
have CPU engine primitives: [NATIVE_FILE_LIFETIME.md](docs/NATIVE_FILE_LIFETIME.md).
They preserve inode access after unlink and separate final retirement from
name removal. The Windows handle owner, writable callbacks and app remain
unfinished; unresolved states cannot enter ordinary resume or read-only mounts.


An owning CPU file-context layer now combines per-open deletion requests,
retains post-Cleanup I/O and controls final inode retirement:
[NATIVE_FILE_SESSION.md](docs/NATIVE_FILE_SESSION.md). Verified committed-view
queries avoid full-volume scans per range request. The Windows writable adapter,
helper app and external context-retirement qualification remain unfinished.


Native file/volume Flush now reaches a real backing barrier with checkpoint
and namespace validation; see [NATIVE_FLUSH.md](docs/NATIVE_FLUSH.md). It preserves
generations and live contexts and fences failed/unverified completion. Writable
Windows ABI/cache/application qualification remains unfinished.

The native layer now provides explicit optional Windows attributes and four
persisted UTC times, atomic file-plus-metadata transactions, separate owning
context attribute-write grants, and read-only projection through the actual
SDK fields. See [persistent basic metadata](docs/NATIVE_BASIC_METADATA.md).
Legacy bytes and absence are preserved. Automatic Windows timestamp policy,
complete ACL/privilege policy, allocation accounting and writable integration remain
unfinished; configured tests require exact hosted validation.

A separate hosted Windows metadata writer campaign now configures exact
process-kill/recovery/continuation coverage for upgrade, tuple replacement and
compound transactions, without replacing legacy cases. Its decoded-state
digests and strict reports are described in
[the basic metadata contract](docs/NATIVE_BASIC_METADATA.md). A configured
campaign is not passing evidence until its exact hosted jobs are inspected.


Opaque Windows security descriptors can now persist atomically with file/basic
metadata and reach the read-only security callbacks. Separate context grants,
bounded framing and Windows validation keep persistence distinct from access
checking. See [native security](docs/NATIVE_SECURITY.md). Complete ACL/inheritance/
privilege policy and writable/client qualification remain required.


The CPU integrity path now has source-verified runtime SIMD selection with
retained scalar fallback and unchanged digest bytes. No GPU or worker is needed.
See [CPU digest acceleration](docs/CPU_DIGEST_ACCELERATION.md) for the independent
byte/guard checks, same-source hosted measurement plan and qualification limits.


The experimental Windows engine can now atomically create a NEW file/directory
with explicit basic/security metadata, optional delete-on-close intent and its
owned context. Context capacity is checked before publication; returned facts
need no post-commit query. See [native create/open](docs/NATIVE_CREATE_OPEN.md).
Configured hosted tests must be inspected before qualification. Writable
driver/app integration, Windows 11 client and live AMD remain unfinished.


The experimental Windows engine also provides atomic existing-inode overwrite:
EOF reset and attribute/time policy share one durable event, preserving
creation/security/identity and handles. Its operation authorization follows the
selected FSD's actual implicit-access contract. See [overwrite](docs/NATIVE_OVERWRITE.md).
Allocation, full FileInfo, failed-open lifecycle, writable callbacks/helper,
Windows 11 client and live AMD qualification remain unfinished.


The experimental Windows engine now has a provisional Open/Overwrite lifecycle:
healthy canceled opens close without fabricated Cleanup or storage mutation,
while failed publication retains fenced ownership. The real user-mode SDK
Overwrite/Close/Cleanup dispatcher is covered by a separate hosted-only group.
See [provisional opens](docs/NATIVE_PROVISIONAL_OPEN.md). Writable driver/app,
allocation, initial delete-on-close overwrite and client/AMD integration remain
unfinished.


The experimental engine now separates the initial FILE_DELETE_ON_CLOSE option
from explicit durable delete intent. General Open/Create preparation permits
healthy cancellation; real Cleanup atomically publishes eligible intent,
metadata and unlink, while nonempty directories retain their contents.
See [initial deletion](docs/NATIVE_INITIAL_DELETE.md). Configured engine/SDK
tests require exact-commit inspection; writable driver/app/client integration,
allocation/full FileInfo and live AMD remain unfinished.

The experimental native engine now has explicit persistent live-extent reservations, atomic allocation shrink/EOF/tail handling, and shared reservation accounting. These native capacity units are separate from host allocation and compressed physical bytes; they do not yet enable Windows allocation callbacks. See [NATIVE_ALLOCATION.md](docs/NATIVE_ALLOCATION.md).

Native Create/Overwrite reservations and trusted Cleanup trimming now share their existing atomic publications; write/truncate can return captured reservation facts. The separate SDK trim bridge remains unwired from mounted callbacks. See [NATIVE_ALLOCATED_EVENTS.md](docs/NATIVE_ALLOCATED_EVENTS.md). Development must leave no orphaned processes, unintended services/tasks or retained resources. The latest PC permission is limited to a qualified, verified RAM-backed synthetic test volume, without driver changes or system-drive artifacts; current development remains hosted.

The experimental Windows data bridge now translates complete FileInfo for its admitted metadata/reservation profile and handles actual SDK Write/SetFileSize/query/real Flush dispatch. Its allocation value is an explicit native cache reservation, not compressed physical usage; it remains unwired from mounted writes. See [NATIVE_WINDOWS_IO.md](docs/NATIVE_WINDOWS_IO.md).


An experimental hosted-only writable callback candidate now combines native ownership,
transactions, FileInfo, security reads, quota and controller-led teardown. Default
product routes and mounted tests remain read-only. Its unmounted tests and
remaining backing/cache/adoption/ACL/client gates are described in
[NATIVE_WRITABLE_ADAPTER.md](docs/NATIVE_WRITABLE_ADAPTER.md). Windows-global deletion
cancellation is explicit; the older aggregate policy remains available unchanged.
Careful GPU use is authorized only under the existing qualified RAM-only,
no-driver-change PC rules; no live GPU or owner-PC test has started.


A separate OFF-by-default disposable-VM writable trial now exercises real Win32
creation, cached writing plus explicit FlushFileBuffers, readback, orderly
helper retirement and independent CPU verification. See
[WRITABLE_MOUNT_TRIAL.md](docs/WRITABLE_MOUNT_TRIAL.md). Its image is capped at
4 MiB before writes; configured trials require exact hosted execution before
passing claims. No owner-PC, live AMD or complete writable-product qualification
follows, and the old read-only probe remains unchanged.
