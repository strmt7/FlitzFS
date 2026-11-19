# Shared Windows memory-volume creation

[Owned WinFsp controller](WINFSP_OWNED_CONTROLLER.md) · [Helper](WINDOWS_BACKGROUND_MAINTENANCE.md)

## One backing factory

`MemoryVolumeBacking::create` collects fresh Windows physical and commit
headroom, computes an admitted layout, generates separate volume/image identities
with the system RNG and reserves address space for three new stores: head,
image and advice. Pages are committed and locked as each store grows.
Failure retires local owners and keeps existing data available. The provider
uses private Windows memory and requires each populated page to be locked.

Both `VolumeModel` and `WinFspMemoryVolume` now use this factory. They retain
their existing native engine and Windows callback semantics. The helper's
generic root and the WinFsp adapter's Windows ACL/time profile remain distinct
engine configurations; neither is a second filesystem or an archive interface.
The shared provider and factory are compiled once, including when both targets
are enabled, so their process-wide lease/accounting cannot be duplicated.

## Current adaptive allocation

The pure `plan_memory_volume` calculation rejects unknown/inconsistent or
insufficient observations; it does not allocate or authorize a provider. Actual
creation always collects the OS observation itself and the provider rechecks
headroom during allocation. A past observation is not a memory reservation.

Automatic sizing uses the smaller available physical and commit headroom after
setting aside the larger of 768 MiB and one eighth of detected physical memory.
The remaining budget can reserve address space for two equal images, a 128-KiB
head and four possible 128-KiB advisory bindings. The factory initially reserves
the first image, head and initial advice. Image reservations align to the
64-KiB head slots. A minimum image has room for those slots and aligned object
pages. An address reservation commits zero data pages.

Preflight rechecks current headroom and locks the required pages before
publication. Reservation and page commitment leave image bytes and its
visible extent unchanged. Direct writes that grow the image enforce the
same check. The quota grows within freshly observed headroom and restores
to its original value when the last owner retires. Paged records use the
Windows page size as the live memory provider's isolation unit. The head
still uses its existing fixed slots. After a failed lock the provider discards
the attempted commit. If cleanup cannot be confirmed, it refuses new
writers and retains readable data.

The controller checks working-set allowance in constant space and time per
opportunity. Creation, sealing and explicit retired inspection can verify
every committed page in fixed-size batches. The UI reports locked bytes,
while internal reservation accounting protects the staging image used by
automatic indexed compaction.
[Local checks](evidence/demand-locked-memory-2026-11-18.json)

## Automatic coherence catalog

Owned indexed volumes set aside up to one thirty-second of the available
post-advice reservation for two equal catalog areas when two viable data
images still fit. The second catalog area allows a full catalog to stay
readable while its packed replacement is built. The scanner reserves each
store as needed and locks only the pages used. The advisory journal settles
the write allowance before catalog initialization or copying. The Windows
provider test measured the selected catalog shrink from 155,648 to 143,360
bytes after pruning stale postings. It verified release of the old
155,648-byte backing and its address reservation.
[Rotation validation](evidence/windows-catalog-compaction-2026-11-19.json)
[Catalog contract](OPTIMIZATION_STATE.md#paged-coherence-catalog)

The joined controller releases its reference to a healthy catalog. The
backing retains the exact writer lease for a remount of the same image, so
reattachment checks the typed root without an inventory scan. Read failure or
selection of a different image detaches the obsolete catalog. The same
background controller releases at most one object-sized span at each admitted
opportunity and releases the address reservation after the last span. Sealing
an independently checked read-only projection retires the advisory writer.
The Windows provider fixture verified remount retention, reclamation,
replacement on the successor image and sealing with exact CPU file readback.
[Windows validation](evidence/windows-catalog-provisioning-2026-11-19.json)

## Retention and inspection

`WinFspMemoryVolume` composes that backing with a legacy or indexed paged
adapter and `WinFspVolumeController`. Callers select the indexed layout
explicitly. They supply a valid plan and provider table. A live volume follows
the owning controller's [event-driven lifecycle](WINFSP_OWNED_CONTROLLER.md).
The backing outlives the controller. The background owner checks the
working-set lease before an opportunity with valid resource observations.
Full page checks use the serialized owner after writes have retired.

`request_stop` wakes the owner. `stop_and_join` retires the dispatcher/controller
while preserving the backing. CPU inspection is refused until this owner has
explicitly joined, even if asynchronous status already says retired. The
independent volume-head reader then validates the retained image mapping and
native contents. For an indexed volume this inspection can read retained
detached bytes from the checked old checkpoint. When the owner requests a
sealed projection after join, it closes the old lifetime records using the
existing indexed recovery transaction and reopens the checked successor
before sealing. Earlier snapshots retain their original view. A reader can
outlive the wrapper because its stores retain the provider lease.
[Indexed checks](evidence/indexed-owned-windows-2026-11-18.json)

### Remounting a retained volume

After an explicit join, the owner can supply a fresh provider and mount plan
to `remount`. The previous dispatcher and optional workers must have confirmed
retirement. Release outstanding inspection views before reopening the writable
backing. Sealing remains permanent.

The owner verifies the existing image selection and resumes the same volume
identity, capacity and layout. Indexed restart resolves acknowledged old-session
deletion state and uses the committed selector after an interrupted compaction.
A readable advisory checkpoint can resume group discovery. If a new attachment
fails, the backing remains owned and byte-readable after join, and a confirmed
retirement allows another attempt. The replacement runs the same maintenance
controller and installed AMD acquisition path. [WinFsp lifecycle API](https://winfsp.dev/doc/WinFsp-API-winfsp.h/)

The Windows SDK fixture exercised a collision followed by successful remount
for both paged layouts. It verified the original and subsequent files through
the independent CPU reader. The indexed case also resolved an old open-file
lifetime after acknowledged deletion. The fixture refused writable reopening
while a reader held the backing, after sealing, and after a provider omitted
the dispatcher stop notification.
[Remount validation](evidence/windows-retained-volume-remount-2026-11-18.json)

Destruction stops and joins before releasing backing references. The domain's
working-set settings restore after the last store owner retires, with existing
fail-closed cleanup accounting. Temporary memory contents do not survive process
termination or power loss. Stopping a volume does not itself discard those bytes;
destroying its last owner does. UI discard confirmation and persistent volume
durability remain separate product responsibilities.

## Automatic indexed compaction

The owning WinFsp controller can open a second memory image when observed
obsolete history dominates the current extent or exceeds the remaining append
space. The counter counts superseded page pairs and encoded records within
the current writer's lifetime. It is a lower bound and does not authorize
reusing any part of a selected or pinned image. A failed or superseded attempt
requires new obsolete history at least as large as its written candidate bytes
before staging again. The copy preserves existing object encodings and requires
a strictly smaller verified image before switching.

An indexed compression transaction can also report an append-space shortfall
before writing. The owner considers compaction if its known obsolete history
covers that lower bound. The observation applies to the selected image and
generation. The owner releases tentative compression work before evaluating
the replacement, and the existing source verification and reader-retirement
checks govern handoff.

The old mapping stays live while any CPU snapshot holds it. Its retirement
checks pins first and securely clears and decommits at most one object-sized
span per resource-admitted opportunity. This work shares the control lock
with callbacks. A failed selector keeps both mappings for independent
post-join inspection. Join releases any known unselected image that was still
staging or retiring. [Managed handoff contract](MANAGED_PAGED_VOLUMES.md#indexed-images)

## Executed scope and remaining work

[Indexed compaction and allocation evidence](evidence/indexed-background-compaction-2026-11-18.json)
records the controlled workload, matched CPU readback, selector interruptions,
reserved and resident bytes and exact retirement. The page-layout fixture
compares the same twelve small overwrites on a 64-KiB model and the real
Windows memory provider using 4-KiB pages. The compaction fixture checks actual
locked-page release and snapshot retention through explicit opportunities.
The extended SDK test drives the actual owned scheduler with live Windows
observations and injected lifecycle functions. It verifies open-handle access,
CPU inspection of epoch 2 and exact handle balance after native PDH setup.
Standalone mount supervisors retain separate short process deadlines.

[Source-bound checks](evidence/memory-volume-factory-2026-11-17.json) cover modeled
small/large/tight-commit observations, real locked allocation and retained store
ownership, startup refusal, explicit join admission, callback byte readback,
live resource opportunities and independent CPU inspection after wrapper exit.
The normal and AddressSanitizer modes record actual live grouping/deferral
outcomes rather than substituting modeled idle samples. Helper model/advice,
live-maintenance and GUI checks guard the shared-factory change.

First-use system RNG initialization retains native process-wide handles. Direct
`BCryptGenRandom` calls demonstrate its stabilization independently of the
factory; exact handle balance is then required across FlitzFS ownership cycles.
The initial failed cold-baseline check and diagnostic results are retained.
This is not permission to ignore an ownership leak or repeatedly warm a failing
counter until a test passes.

The helper's live test now accepts only a measured CPU/memory resource deferral
or actual completed grouping, and records which coverage occurred. Foreground
byte readback, unchanged content generation and retirement remain mandatory.
Modeled quiet-provider grouping is tested separately. An unavailable collector,
unexpected deferral or disabled scheduler still fails qualification.

Test process budgets account for measured preexisting private commit plus at
most one eighth of fresh residual physical/commit headroom. This envelope covers
sanitizer shadow/default quarantine separately from the smaller volume domain;
sanitizer settings are unchanged. The GUI child receives at most half the
remaining aggregate budget and is independently re-admitted by the existing
creation-time supervisor with its 1/16 share, finite deadline and CPU limit.
Parent/child admission failures are reported promptly. These test-tooling limits
do not change the GPU worker or product-volume allocation policy. Captured
out-of-memory, busy-host timeout and rejected child-budget runs remain evidence.

These tests use real SDK callbacks and injected lifecycle operations; they do
not invoke native mount/dispatcher creation, load/install a driver or execute
HIP. The follow-up [sealed memory integration](WINFSP_READONLY_MEMORY.md)
uses this backing for the read-only adapter and a disposable external-client
probe. Mounted Windows qualification, useful ordinary GPU work, reclamation,
scalable coherence and full Windows semantics remain open.

Windows documents the differences between reserving, committing and locking
pages and the working-set allowance used by page locking. The evidence binds
the current source to the live Windows measurements.
[VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc) ·
[VirtualLock](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtuallock) ·
[Working-set allowance](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-setprocessworkingsetsizeex) ·
[Headroom observation](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-globalmemorystatusex)
