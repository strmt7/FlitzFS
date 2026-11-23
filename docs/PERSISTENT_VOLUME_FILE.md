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

### Mounted publication and process recovery

An ordinary indexed write flushes the appended image data, then writes and
flushes each of the two paged roots. The mounted publication probe pauses
an 8 KiB Windows write at three observed boundaries. The client still holds
its handle and `GetOverlappedResult` reports a pending request when the
supervisor terminates the writer. The client then receives
`ERROR_OPERATION_ABORTED`.

| Boundary reached | Independent CPU recovery in both guest trials |
| --- | --- |
| Before the appended data flush | Previous file bytes and inode |
| After the first paged root flush | Complete replacement bytes and the same inode |
| After the second paged root flush | Complete replacement bytes and the same inode |

Each case preserved the separately acknowledged witness and continued writing
after a fresh writable mount. The optional `flitzfs_write_probe`
`--run-publication-cuts` mode runs these checks in the disposable Windows
Sandbox fixture. Physical power-loss qualification remains a separate gate.
[Mounted publication evidence](evidence/windows11-inflight-publication-cuts-2026-11-21.json)

### Paired sparse image ranges

A qualified range provider can create a paired volume with two aligned
image ranges of the same declared capacity. Capability 4 contains the
image pair. Capability 5 adds reserved catalog ranges. Capabilities 6 and
7 bind a sealed selection within those respective range layouts. The head and advice
retain the positions above. If the image capacity is C, the first image
range begins at 256 KiB, and the second begins at 256 KiB + C. Odd epochs
select the first range and even epochs select the second. The indexed
object and file record encodings are shared by both layouts.

### Reserved coherence catalog ranges

When a new paired volume has room for a catalog, the head records capability
5. The two catalog ranges follow the two image ranges. Each is sized to
one thirty-second of C, rounded down to 64 KiB and limited by the remaining
file address space. The minimum includes two roots and room for an initial
and an incremental allocation at the largest supported failure unit.
The ranges remain sparse until the funded scanner writes them. New readers
also support capability 4 and the linear layout.

The saved backing identity identifies the slot through its last byte's low
bit. The first eight bytes carry the advisory journal sequence checked when
the range was reserved. A paid journal publication advances that sequence
before any candidate writes. The catalog and its typed root verify the full
backing, volume and image identities on reopen.

The inactive range remains allocated while a staged writer or a reader
pins it. Retirement takes a separate idle opportunity. It checks that both
advice slots carry the cached sequence and selection, then reclaims at most
4 MiB from a single unselected catalog range. A failed retirement waits for
fresh verified journal activity. A journal awaiting repair permits file
I/O and holds both catalog ranges. The independent CPU data reader opens
the selected filesystem using its authoritative head and image.
[Catalog selector](OPTIMIZATION_STATE.md#identified-catalog-selection) ·
[Managed range validation](evidence/managed-coherence-catalog-ranges-2026-11-21.json)

### Data image compaction and retirement

The WinFsp maintenance owner stages a candidate when measured obsolete
history justifies the copy and the host admits idle work. It verifies the
packed image incrementally and reconciles later commits from immutable
index checkpoints. The owner requires a smaller candidate that has verified
the current generation before publishing the two head records. The old image remains
allocated until both durable records select the new epoch and readers
release the old backing. Each retirement opportunity rechecks the
selector and retires up to 4 MiB from the inactive range.

After a restart, the verified object-tree totals also provide a conservative
estimate of reclaimable committed space. The estimate compares the selected
image extent with an upper bound for the packed image, including mirrored
records, index pages, padding and rebinding the namespace root. The
controller can qualify a rewrite from prior history on its next admitted
idle opportunity without new foreground writes. It measures the actual
replacement and checks retained readers before any retirement.
An advance without a new image identity can finish existing work and leave
eligible new compaction pending.
[Reopen and reclamation evidence](evidence/reopened-reclamation-2026-11-21.json)

After grouping, backfill and scheduled scrub work have yielded, a quiet
volume can also qualify on compression gains. A read-only screen tries
the cold encoder on evenly spaced live data ranks, at most one record
per step. It uses twice the bit width of the record count as its sample
size, up to the number of available records. Together the sampled records
must save at least one byte per 32 compared bytes, including headers.
Negative screens write neither the image nor the advisory area. The
same foreground workload cannot repeatedly reopen a screen within the
session.

A positive screen still needs source verification and a fresh idle
opportunity to reserve durable input credit. The reservation covers the
candidate with both payload copies, root rewrites, the two selector
records and the two advice records. Each settled input byte can fund
up to two bytes of that write bound. Both advice records must confirm
the charge before candidate writes begin. The attempt remains charged
after interruption. Candidate storage grows one admitted quantum at a
time. A later foreground commit cancels this optional
copy. Publication requires a complete independently verified image
with a smaller occupied extent. The ordinary reclamation path can
still reconcile foreground writes.
[Screened compaction evidence](evidence/screened-cold-compaction-2026-11-20.json)

The Windows owner waits on its existing activity event when readers still
pin the old range. Shared snapshots of the same image retain one backing
pin. Releasing the final reference drops that pin before signaling the
owner. Release performs no storage I/O. The owner still requires an admitted
idle opportunity and rechecks both durable selector records before touching
the inactive range. Foreground I/O can continue on the new image during
the wait.
[Reader release and recovery evidence](evidence/paired-range-reader-wake-2026-11-20.json)

After interruption, independent CPU open follows the checked head. A
writable reopen verifies and repairs the selected head before attempting
to reclaim an interrupted candidate or old image. Reclamation checks
allocation after flushing the range. A failure leaves the active data
authority available and defers more range work until reopen. The
exclusive backing lease also covers independent paired-file inspection
so a retained reader cannot race a writable reopen.

### Sealed paired images

After ordinary reclamation and funded cold compression have yielded, the
same maintenance owner can prepare a sealed base and writable tail in the
inactive paired range. The existing advisory journal confirms the write
charge before preparation. The charge includes the base, tail, ordered
control writes and any workspace placed in the backing file. Advisory
scans leave room for a complete image operation at the file-workspace cost.
A fresh bulk write can leave insufficient credit for preparation,
especially for weakly compressible data.

The file workspace starts at the high end of the inactive range and the
base starts at its beginning. The fitted workspace and projected parity
layout establish the bounds before writes. The owner verifies the base and
rechecks both current selector peers before releasing a file workspace in
bounded spans. The tail starts immediately after the base at a boundary
admitted by the backing's failure-unit contract. Reclamation uses the
outer provider's coarser aligned ranges. A new foreground commit invalidates
the sealed candidate and leaves the acknowledged image selected.
[Windows storage geometry](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_storage_info) ·
[Windows range retirement](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_zero_data)

The Windows controller can place the workspace in locked memory when it
fits the current idle memory allowance after reserving ordinary
transaction scratch. The memory provider rechecks physical and commit
headroom and shares its existing process-wide lease. A refused reservation
selects the file workspace before charging either path. Both paths use the
same effective failure-unit geometry and write the protected base and tail
to the inactive file range. The build releases the workspace before tail
binding. When the current memory grant can no longer retain the workspace,
the owner abandons incomplete work and releases locked pages in bounded
steps. A verified base that has released its workspace references remains
eligible to finish later.
[Windows memory locking](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtuallock) ·
[Windows decommit and release](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree)

The Windows SDK fixture used that allocator against a sparse backing model.
Compared with the file workspace, locked memory reduced attempted backing
writes from 10,437,120 to 4,416,000 bytes at a 4 KiB failure unit and from
10,691,072 to 4,522,496 at 64 KiB. Each pair retained the same total
allocation and reopened the held client's edit. An occupied memory lease
selected the file workspace with the same write count as the file control.
The attempted byte counts include the model's writes for range zeroing.
Sixty modeled selector interruptions exercised both workspace paths and
both failure units. Retiring a partial candidate under pressure preserved
the previous image and returned its inactive allocation.
[Workspace placement evidence](evidence/volatile-paired-workspace-2026-11-23.json)

The candidate flush precedes the two independently flushed selector peers.
The CPU opener validates the selected image identity, range bounds,
protected base and tail binding. It can reopen from ordinary bytes even
when zeros left by sparse retirement extend the backing file's logical
length. Both components retain the same paired range for as long as an old
snapshot needs either one. A closed owner can rebuild a CPU-readable
degraded base into a verified mirror in the inactive range, then resume
writes. The old image survives until the new head is durable and the range
has no readers.
[Head format](VOLUME_HEAD.md#fixed-encoding) ·
[Windows file flush](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)

The in-memory sparse provider modeled 64 KiB allocation and exercised two
write-failure units. The occupied whole-file allocation after retirement was
4,456,448 bytes versus 6,291,456 bytes for the 4 KiB model. The 64 KiB model
settled at 4,521,984 bytes versus 7,995,392 bytes. Across 30 interruptions of
the candidate flush and selector operations, the CPU reader recovered the
complete previously acknowledged state. Both models rebuilt a CPU-readable
damaged base into an independently verified mirror. The Windows SDK fixture
kept the original handle open through selection, wrote to the new tail and
verified the edited file after dispatcher retirement. Real-device allocation
and power-loss behavior still require qualification.
[Paired placement evidence](evidence/paired-sealed-placement-2026-11-23.json)

#### Mounted process interruption

The signed WinFsp 2.2 probe ran the writable session and an independent
Win32 client on sparse NTFS in a disposable Windows 11 guest. The client
kept the same file handle and inode through live selection, then wrote
and flushed a change through that handle. A separate CPU reader reopened
the edited bytes. The provider observed complete release of the inactive
range after the old client retired.

Nine process cuts covered workspace retirement, the candidate flush and
both selector records. Each cut held a Win32 file handle open until the
source process retired. The CPU check read the payload and witness exactly
and verified the prepared image separately when its base and tail were
complete. Each recovered writable mount finished retirement and persisted
a new file that the independent CPU reader checked again.

| Interruption point | Expected selected image |
| --- | --- |
| Before workspace retirement through before the first selector write | Previous image |
| After the first selector write, before its flush | Either complete image. The guest exposed the successor in this run. |
| After the first selector flush through the second selector flush | Successor. Recovery verifies the selected image and reconciles the peer before reuse. |

The stale-candidate case changed the source after the replacement was
verified. The same open client wrote and flushed the edit. The source
stayed selected, the prepared range was released, and the edit survived
writable reopen. Three cuts through ordinary mirrored selection passed
under the same probe.

The opt-in probe allows up to 100 seconds for the exact sealed source
mode in the disposable guest so the normal maintenance governor can
advance between idle observations. Device power loss and removal require
separate checks of the medium and its cache policy.
[Mounted paired sealing evidence](evidence/windows11-mounted-paired-sealing-2026-11-23.json)

The locked-workspace NTFS probe also kept the original handle across
publication. Process cuts before the candidate flush and after the first
selector flush recovered the old and new image respectively. The observer
checked that the candidate passed through the inactive file range without a
file workspace. Each case verified the exact payload after a writable reopen.
[Locked workspace on mounted NTFS](evidence/volatile-paired-workspace-2026-11-23.json)

The AMD worker also served two verified LZ4 frames from a reopened
paired selection through ordinary SDK read callbacks. That fixture
replayed durable records in owned sparse memory. An unsupported worker
configuration left exact CPU reads available and stayed fenced for the
remainder of the attachment. Independent CPU inspection checked the
selected layout and file bytes after shutdown.
[Reopened paired AMD evidence](evidence/amd-reopened-paired-sealed-read-2026-11-23.json)

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
transaction before recovering detached lifetimes. During the initial
indexed namespace audit, the WinFsp bridge checks file information and
security descriptors from the already decoded reachable inodes. The root
supplies the persisted creation time. These checks finish before repair
or either startup barrier. Healthy attach
keeps the same image writer leased across the barriers and checks the
selected image and head again before exposing callbacks.
[Verified reopen validation](evidence/indexed-retained-reopen-2026-11-20.json)
[Startup repair validation](evidence/indexed-startup-repair-2026-11-20.json)
[Windows indexed startup evidence](evidence/winfsp-indexed-startup-audit-2026-11-20.json)

If the selected range refuses an append repair, the Windows provider can
rebuild it in the inactive range. Before reclaiming space left by an
interrupted attempt, the closed owner verifies the selected image and
checks that both head copies refer to it. Selection follows complete CPU
comparison and the existing flush protocol. The old selected range
remains available until retirement. A custom provider can supply its new
image identity through the native reopen API.
[Full startup repair validation](evidence/indexed-full-startup-repair-2026-11-20.json)

## Positioned Windows file I/O

The writable provider uses explicit offsets on its exclusive Windows handle.
Ordinary reads, writes and sparse controls share one completion event under
the serialized file owner. The owner confirms completion before proceeding,
and retains the established data, root and selector flush order.

An optional capture uses the same handle with a separate completion event.
It checks the last published file extent and the bounds of its pinned region.
Its owner can continue foreground reads, appends and publication. When
foreground activity supersedes the capture, or the controller stops, a pending
capture requests cancellation for that operation and waits for its terminal
result before releasing its buffer and image pin. Cancellation completion
depends on the Windows storage stack. A cancelled capture leaves ordinary
reads available through the existing checked CPU path.
[Windows positioned read contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile) ·
[Windows positioned writes](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile) ·
[Windows control completion](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-deviceiocontrol) ·
[Request cancellation](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex) ·
[Request retirement](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getoverlappedresultex)

The Windows 11 guest verified linear reopen, reuse of both sparse ranges, and
independent CPU readback after a foreground write during a held capture. The
held lease protected the former range across publication, and releasing the
lease allowed retirement. A local overlapped request fixture verified
cancellation of a pending read and reuse of the completion event. The mounted
guest also recovered at the data flush and both root flush boundaries with
positioned I/O, then verified a new write after reopen. Persistent device
performance and physical interruption remain separate qualification.
[Positioned file verification](evidence/positioned-volume-io-2026-11-22.json)

## Windows helper ownership

A qualified caller supplies the backing and creation geometry to the same
attachment worker that manages memory volumes. The Explorer helper can
request a new file or open an existing one. It pins the selected parent
and records its volume GUID before mount preflight. Acquisition rechecks
the selected GUID, opens beneath the verified GUID path, and verifies the
exclusive handle's normalized path. A stale volume GUID is refused before
creation or reopen. The native startup audit verifies the selected image
and Windows metadata on that handle. The helper retains it after the audit
and before the write fences or repair. A supplied expected identity must
match. The NTFS sparse profile checks the pinned parent and the file handle,
including linear images reopened under that profile.
A fence, repair, or mount failure after the audit keeps the checked backing
open for recovery. Reattachment uses the retained file handle and verifies
the selected volume before starting a new dispatcher. Creation is requested
once per acquired backing.
[Volume GUID binding evidence](evidence/windows-backing-binding-2026-11-22.json) ·
[Checked open and failure evidence](evidence/release-reconciliation-attachment-2026-11-21.json)

Detach joins the dispatcher and worker before permitting another attach.
An uncertain retirement keeps the attachment blocked. Closing a detached
volume file releases its handle and preserves the file. Ordinary app exit
can close a detached file without a discard confirmation. A fresh helper can
reopen the file by its checked identity.
[Helper controls and qualification](WINDOWS_VOLUME_APP.md#persistent-volume-files) ·
[WinFsp dispatcher and mount API](https://winfsp.dev/doc/WinFsp-API-winfsp.h/)

## Validation and remaining integration

The in-memory suite recreates the backing from durable bytes, checks old and
new file contents, and injects failures across the measured write and barrier
boundaries. The Windows SDK callback suite also exercises stop and reopen
with injected lifecycle operations. The optional real file-handle test uses
a newly created disposable fixture and belongs to a separately qualified
Windows test environment. [Validation record](evidence/windows-volume-file-recovery-2026-11-19.json)

The injected lifecycle tests exercise failed attachment, joined detach,
independent CPU readback, a fresh mount target and ordinary exit. A separate
Windows 11 guest with the signed WinFsp 2.2 driver exercised ordinary Win32
writes and flushes to the mounted NTFS-backed volume. It verified exclusive
creation, fresh-process reopen and independent CPU readback.
[Mounted file activation](evidence/windows11-mounted-volume-activation-2026-11-21.json)
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

User-facing persistent-volume selection, migration of existing linear and
capability-4 volumes, steady-state segment evacuation during concurrent
writes, persistent catalog qualification at production scales and physical
storage-failure qualification remain release gates.
