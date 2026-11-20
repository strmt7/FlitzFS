# Indexed volume files

[Managed paged volumes](MANAGED_PAGED_VOLUMES.md) · [Windows memory volumes](WINDOWS_MEMORY_VOLUME_FACTORY.md)

## File layout

A volume file carries an indexed filesystem and its durable selector and
funded advice on one exclusively owned stream. The regions are aligned to the
existing 64 KiB failure-unit ceiling.

| Offset | Region | Extent |
| --- | --- | --- |
| 0 | Paired native volume head | 128 KiB |
| 128 KiB | Paired funded optimization records | 128 KiB |
| 256 KiB | Selected indexed data image | Current file extent minus 256 KiB |

The linear layout has a capability-3 head at epoch 1 and one append stream.
The head declares the volume and image identities and the indexed capacity.
Each bounded metadata region and the growing image have disjoint write
limits. File creation commits only the bytes used by initialization.
Creation publishes the image before the paired selector. Missing or
damaged advice leaves the data image usable.

### Paired sparse image ranges

A qualified range provider can create a capability-4 volume with two
aligned image ranges of the same declared capacity. The head and advice
retain the positions above. If the image capacity is C, the first image
range begins at 256 KiB, and the second begins at 256 KiB + C. Odd epochs
select the first range and even epochs select the second. The indexed
object and file record encodings are shared by both layouts.

The WinFsp maintenance owner stages a candidate when measured obsolete
history justifies the copy and the host admits idle work. It verifies the
packed image incrementally and reconciles later commits from immutable
index checkpoints. The owner requires a smaller candidate that has verified
the current generation before publishing the two head records. The old image remains
allocated until both durable records select the new epoch and readers
release the old backing. Each retirement opportunity rechecks the
selector and retires up to 4 MiB from the inactive range.

After interruption, independent CPU open follows the checked head. A
writable reopen verifies and repairs the selected head before attempting
to reclaim an interrupted candidate or old image. Reclamation checks
allocation after flushing the range. A failure leaves the active data
authority available and defers more range work until reopen. The
exclusive backing lease also covers independent paired-file inspection
so a retained reader cannot race a writable reopen.

Physical allocation is separate from logical file length. The Windows
provider requires an explicit isolated range reclamation contract, a
sparse-capable NTFS volume and compatible live cluster geometry. It
queries allocated suffixes with a bounded binary search and uses the
Windows sparse range operation only for the checked retired region.
[Windows sparse allocation](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_sparse) ·
[Allocated range query](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_query_allocated_ranges) ·
[Range retirement](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_zero_data)

## Create, inspect and resume

The Windows provider opens an explicit local path. It pins the parent,
checks the normalized paths of the opened objects, and verifies a
single-link regular file. Creation uses exclusive creation. Reopen requires
an existing file and an exclusive OS handle. A pinned read-only inspection
excludes write sharing. The file's declared capacity supports paged storage
beyond the older flat-store cap. Actual growth requires a separate live
free-space check before each admitted mutation.

Store admission compares the live logical and physical sector information
and the filesystem's effective atomicity size with the independently qualified
failure-unit contract. The code uses the existing file flush for barriers.
Live geometry and successful API calls are inputs to the provider boundary.
Physical failure-unit isolation, device cache behavior and the durability
of the new directory entry require qualification for the actual storage chain.
[Windows creation and sharing](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew) ·
[Windows storage geometry](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_storage_info) ·
[Windows flush contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)

Open verifies both head peers and binds the selected data to the declared layout
and epoch. A supplied expected volume identity must match. Without that identity,
the checked peers must belong to the same volume. The independent CPU reader
validates the entire image and can inspect the durable state of detached
lifetimes. After the previous provider's confirmed retirement, a writable
reopen can restore recoverable payload damage with a verified append
transaction before recovering detached lifetimes. The WinFsp bridge
validates the Windows root ACL and timestamp profile during admission.
[Startup repair validation](evidence/indexed-startup-repair-2026-11-20.json)

If the selected range refuses an append repair, the Windows provider can
rebuild it in the inactive range. Before reclaiming space left by an
interrupted attempt, the closed owner verifies the selected image and
checks that both head copies refer to it. Selection follows complete CPU
comparison and the existing flush protocol. The old selected range
remains available until retirement. A custom provider can supply its new
image identity through the native reopen API.
[Full startup repair validation](evidence/indexed-full-startup-repair-2026-11-20.json)

## Windows helper ownership

A qualified caller supplies the backing, the expected volume identity and
optional creation geometry to the same attachment worker that manages memory
volumes. It acquires the file exclusively before asking WinFsp to attach.
A mount failure keeps the backing open for recovery. Reattachment uses the
retained file handle and verifies the selected volume before starting a new
dispatcher. Creation is requested once per acquired backing.

Detach joins the dispatcher and worker before permitting another attach.
An uncertain retirement keeps the attachment blocked. Closing a detached
volume file releases its handle and preserves the file. Ordinary app exit
can close a detached file without a discard confirmation. A fresh helper can
reopen the file by its checked identity.
[WinFsp dispatcher and mount API](https://winfsp.dev/doc/WinFsp-API-winfsp.h/)

## Validation and remaining integration

The in-memory suite recreates the backing from durable bytes, checks old and
new file contents, and injects failures across the measured write and barrier
boundaries. The Windows SDK callback suite also exercises stop and reopen
with injected lifecycle operations. The optional real file-handle test uses
a newly created disposable fixture and belongs to a separately qualified
Windows test environment. [Validation record](evidence/windows-volume-file-recovery-2026-11-19.json)

The helper tests exercise failed attachment, joined detach, independent CPU
readback, a fresh mount target and ordinary exit. The Windows 11 guest also
exercises the exclusive file handle and reopen after close. These helper
checks use injected WinFsp lifecycle operations.
[Helper validation](evidence/windows-volume-file-attachment-2026-11-19.json)

The paired-range model covers interrupted replay, head publication and
reclamation, pinned old readers, late foreground edits and later reuse.
[Reconciliation validation](evidence/indexed-compaction-reconciliation-2026-11-19.json) The disposable
Windows 11 fixture verified release of actual NTFS allocation, reuse of
both image ranges and independent CPU readback after each switch.
[Reclamation validation](evidence/windows-sparse-volume-reclamation-2026-11-19.json)

The paired-range reclaimer also considers a checked append-space shortfall
from indexed compression. The current image stays selected until the
replacement has completed independent verification and selector publication.
[Append-pressure validation](evidence/amd-maintenance-parity-2026-11-19.json)

A verified payload repair that cannot append to the selected range can
stage in the unused range. The CPU verifier reconfirms the fault and
both restored copies. The owner may select the replacement at the same
occupied size for the exact generation with verified damage and a
capacity refusal. The paired-range model exercises this when the
backing rejects writes that extend the file.
[Repair rotation validation](evidence/indexed-capacity-repair-rotation-2026-11-20.json)

User-facing persistent-volume selection, in-place migration of existing
linear volumes, steady-state segment evacuation during concurrent writes,
persistent catalog compaction and physical storage-failure qualification
remain release gates.
