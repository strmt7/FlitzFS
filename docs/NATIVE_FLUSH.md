# Verified real backend Flush

Experimental CPU durability API for the future Windows writable engine.
It does not enable mounted writes, install a driver or start a helper/GPU.
All execution remains on disposable hosted VMs.

## Contract and layers

The selected current WinFsp v2.2B5 header was fetched again and compared at
14d6e0280ae15e2550c44d6dbc476be51649727f:
https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h

Its Flush callback is per-file or whole-volume (NULL context), and the FSD
flushes its own file/volume caches before invoking it. This engine therefore
must complete an actual backing barrier after all earlier serialized native
writes. It must not copy a passthrough example's volume-success shortcut.

ObjectStoreWriter::flush_checkpoint:

1. Require a live healthy writer.
2. Observe bounded raw backing length and load the current root/manifest state.
3. Require exact agreement with the writer's acknowledged baseline and healthy
   metadata, retaining admitted uncertain tails.
4. Invoke StoreByteIo::flush exactly once.
5. Reload/recheck the same metadata and require raw length unchanged across
   the barrier.
6. Return the existing StoreCommitInfo without changing publication state.

The ordinary-file backend's existing flush uses FlushFileBuffers on Windows.
The shared internal analysis backend uses its existing fsync path. No new OS
flush, raw-volume access, privilege or ordinary-file reopening factory is added.

NativeFilesystemWriter::flush verifies the namespace against its cached
admitted model before AND after that real object barrier. It returns the same
namespace sequence and storage metadata with an empty created-inode list.
It performs no repair, publication, extent write, padding, trimming or deletion.

NativeFileSession offers file-context and volume forms. The file form first
validates a still-owned context; it remains usable after Cleanup until Close.
Both synchronize the same shared image, a stronger scope than one file alone.
They preserve every context, pending request, latch and retained detached inode.
An invalid/foreign/closed context fails before backing I/O.

## Serialization, failures and resources

All methods retain the existing one-control-thread contract. No public snapshot
or unchecked writer view escapes. The future Windows adapter must serialize
kernel callbacks/control requests and drain its outstanding writes before
calling the barrier. It must not acknowledge buffers or asynchronous GPU
candidates as committed data.

Preflight allocation pressure before barrier entry returns failure and leaves
an otherwise healthy writer usable. Metadata/identity/extent observation or I/O
failure fences the writer. ANY error after entering the object barrier,
including inability to allocate/verify its post-state, terminally fences it.
The native layer also fences if it cannot verify its namespace after a
successful object barrier. Failed sessions never retry I/O automatically or
release file contexts as though a flush succeeded.

An extra barrier does not erase prior acknowledgements. A failed outcome must
be independently inspected under the existing backend contract; it is not
permission to roll back to an older object set. No generation/namespace
sequence advances and no new transaction is invented to implement Flush.
All existing mutation publication still uses its unchanged three barriers.

The metadata loads reuse the existing admitted root/manifest parser and
same_state comparison, including object IDs/generations/locators/digests,
selected root, checkpoint group and degradation flags. They are bounded by
the existing 256-object manifests and 64 MiB backing ceiling. Namespace checks
retain the 64 KiB metadata limit. No payload scan, sparse expansion, unbounded
queue, worker, discovery, timer or GPU call is introduced.

## Scope of assurance

Flush validates current checkpoint metadata and, at the native layer,
namespace integrity. It is NOT a scrub of every file payload. Corruption in an
untouched data record is still detected on a verified read or full snapshot/
scrub, as documented for incremental operations. Generic legacy root-format-1
images retain their historical single-publication semantics; the stronger
replicated-current guarantee is not retroactively fabricated.

Raw length is required stable across the barrier; admitted recovery tails are
retained. This does not attest all historical changes by a noncooperating actor.
Exclusive ownership and StoreIoContract remain qualified caller/backend
assumptions. A successful OS call is not physical power-cut qualification and
cannot establish honest device/controller persistence or whole-device survival.

The native layer still has no unflushed user-data cache: writes acknowledge
only after their normal replicated barriers. This additional real Flush is
nevertheless required at the Windows synchronization boundary. Actual cached/
mapped application behavior, cancellation, write-through handling and ABI
FileInfo outputs remain writable-adapter qualification work.

## Configured verification

The dedicated raw/automatic-codec suite checks:

- Initial empty, legacy and replicated stores; exactly one real backend barrier,
  no byte/write/generation/sequence changes, retained snapshots and repetition.
- File/volume synchronization with live detached contexts after Cleanup;
  neither form may retire data or alter context ownership.
- Every observed metadata-read and extent-observation boundary, three existing
  backend flush failure modes, preflight allocation pressure and unverified
  post-barrier allocation failures.
- Loss of either current root/manifest before or during Flush, changed extent,
  namespace corruption after the object barrier, moved/poisoned writers and
  unchanged admitted uncertain tails.
- Earlier acknowledged file/object bytes recover independently after modeled
  barrier/observation errors; poisoned sessions perform no further I/O.

A hosted Windows fixture creates two separate NEW deterministic native images,
one with the explicit barrier, then requires byte-identical complete images and
unchanged generations/sequences plus independent CPU verification. This uses the
real ordinary-file backend. It does not simulate real storage power loss.

Existing lifetime/context fault matrices, process interruption, CPU/raw/static
runtime/reference/sanitizer checks and read-only mount/crash/remount regression
remain required. Every configured check must be inspected at its exact commit;
a test definition alone is not a passing result. No owner-host test is authorized.
