# Shared Windows memory-volume creation

[Owned WinFsp controller](WINFSP_OWNED_CONTROLLER.md) · [Helper](WINDOWS_BACKGROUND_MAINTENANCE.md)

## One backing factory

`MemoryVolumeBacking::create` collects fresh Windows physical and commit
headroom, computes an admitted layout, generates separate volume/image identities
with the system RNG and creates three new locked stores: head, image and reserved
advice. It verifies residency before returning. Failure retires local owners;
there is no source path, existing-volume overwrite or pageable fallback.

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

Within the provider's existing 40-MiB experimental envelope, admission uses at
most 1/256 of the smaller available physical/commit headroom after reserving the
larger of 768 MiB and one eighth of detected physical memory. Domain and image
capacities are rounded down to 64 KiB; creation refuses below a 9-MiB domain.
The layout funds two equal image reservations, one 128-KiB head and all four
possible 128-KiB advisory bindings. Only the current image/head/initial advice
are initially allocated. The same formula formerly lived inside the helper.

These are current provider qualification limits, not final product capacity,
assumptions about installed RAM or automatic large allocations. Scaling this
provider and persistent namespace remains separate required development.

## Retention and inspection

`WinFspMemoryVolume` composes that backing with the paged writable adapter and
`WinFspVolumeController`. It requires an explicit valid plan and provider table;
no native provider, driver installation or driver start is selected implicitly.
The backing outlives the controller. Its registry remains fixed while callbacks
run, allowing the background owner to verify working-set/locked residency before
an opportunity with valid resource observations.

`request_stop` wakes the owner. `stop_and_join` retires the dispatcher/controller
while preserving the backing. CPU inspection is refused until this owner has
explicitly joined, even if asynchronous status already says retired. The
independent volume-head reader then validates the retained image mapping and
native contents. A returned reader can outlive the wrapper because its stores
retain their provider lease. A surviving reader/store prevents a second domain
from being created until its last reference retires.

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

Current official API captures support the design: [memory observations are
volatile](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-globalmemorystatusex),
[locked pages retain physical residency](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtuallock)
and [the system RNG accepts a null algorithm handle](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom).
Older API publication dates are background, not new 2026 research or a physical
power-loss qualification.
