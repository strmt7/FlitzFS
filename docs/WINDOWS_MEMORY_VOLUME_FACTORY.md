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

Automatic admission uses 1/256 of the smaller available physical and commit
headroom after setting aside the larger of 768 MiB and one eighth of detected
physical memory. The resulting budget scales with the host. The layout funds
two equal 64-KiB-aligned image reservations, a 128-KiB head and four possible
128-KiB advisory bindings. The helper requires at least 4 MiB per image.
It reserves address space for the current image, head and initial advice.

Preflight rechecks current headroom and locks the required pages before
publication. Reservation and page commitment leave image bytes and its
visible extent unchanged. Direct writes that grow the image enforce the
same check. Working-set allowance grows with use, and the last owner
restores the original allowance. After a failed lock the provider discards
the attempted commit. If cleanup cannot be confirmed, it refuses new
writers and retains readable data.

The controller checks working-set allowance in constant space and time per
opportunity. Creation, sealing and explicit retired inspection can verify
every committed page in fixed-size batches. The UI reports locked bytes,
while internal reservation accounting protects the future compaction image.
[Local checks](evidence/demand-locked-memory-2026-11-18.json)

## Retention and inspection

`WinFspMemoryVolume` composes that backing with a legacy or indexed paged
adapter and `WinFspVolumeController`. Callers select the indexed layout
explicitly. They supply a valid plan and provider table.
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

Destruction stops and joins before releasing backing references. The domain's
working-set settings restore after the last store owner retires, with existing
fail-closed cleanup accounting. Temporary memory contents do not survive process
termination or power loss. Stopping a volume does not itself discard those bytes;
destroying its last owner does. UI discard confirmation and persistent volume
durability remain separate product responsibilities.

## Executed scope and remaining work

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
