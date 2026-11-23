# Owned WinFsp background controller

[Paged callback engine](WINFSP_PAGED_VOLUMES.md) · [Windows observations](WINDOWS_BACKGROUND_MAINTENANCE.md)

## Implemented ownership

`WinFspVolumeController` owns one thread and a `WinFspNativeSession`. Construction
requires an explicit adapter, provider operation table and valid mount plan;
there is no default native provider or automatic driver selection. That thread
alone starts the session, supplies background opportunities and stops it.
The writable volume stays owned until explicit stop, dispatcher termination
or a session fence. A quiet controller blocks on the owner stop and activity
events. Independent test supervisors enforce the deadlines for mount trials.
[Windows wait contract](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)

The controller uses the same writable callbacks, paged engine and independent
CPU reader for either supported layout. Legacy paged volumes run shared-base
maintenance. An indexed volume advances queued file retirement and, with owned memory
backing, automatic staged compaction through the same owner and admission. The tray can observe the synchronized
view and request stop while the owner performs storage work.
External owner calls must be serialized, and joining from dispatcher/callback
threads is forbidden.

An independently qualified AMD read profile also permits the same owner to
supervise one speculative decode batch. Ordinary callbacks retrieve verified
ready bytes or perform their usual CPU read. The owner releases the package
when the volume stops.
[AMD read-ahead ownership](NATIVE_AMD_LZ4_READ.md#amd-read-ahead-in-the-volume-controller)

## Ordinary AMD write demand

With a configured uniform package, the controller enables CPU write
observation through the ordinary attachment path. The adapter publishes
complete rolling windows keyed by aligned request size, compression policy
and backing isolation. The controller takes the newest observation at a
quiet boundary and exposes it in the synchronized view. The installed uniform
package and an admitted idle opportunity let the controller capture that exact
committed revision, along with funded recent writes from other files. The
controller can capture closed files, using one immutable backing snapshot.
The replay uses a private protected image, independent CPU verification and
the controller's existing cancellation and pacing. Single-write and grouped
CPU observations are tracked independently. A qualified offer requires a
winning matched GPU comparison.
[Measurement and replay contract](OWNED_DEFERRED_WRITES.md#cpu-service-cost-from-ordinary-writes)

## Content events and bounded work

Before binding callbacks, the adapter duplicates a same-process event handle
with signal-only rights and inheritance disabled. It owns that duplicate until
adapter destruction. The controller owns separate noninheritable wait handles,
which remain open through worker join and session destruction. Replacement or
late event binding is refused. An auto-reset activity event collapses callback
notifications without a queue; a separate manual-reset stop event takes priority.

Callbacks signal after releasing the engine control lock and dropping their
foreground count. Dispatcher termination, deferred write retirement and a
deferred publication failure also signal. The owner rechecks the terminal
state and queued work after waking. This covers asynchronous storage failures
even if a foreground callback has already returned. The owner checks the
cached namespace sequence before collecting telemetry.
[Windows event behavior](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setevent) Read-only activity on a
completed unchanged volume does not sample before its next integrity pass is due.
A deferred service's pending
publication or outstanding response remains pending even before its content
sequence changes, preventing a delayed CPU commit from being missed.

While changed work is pending, native processor-time deltas or the aggregate
PDH counter combine with fresh physical and commit observations to admit
cooperative group opportunities. Windows
current-thread background resource mode is entered outside view locks and ended
before publishing status or stopping. The adapter still tries the callback lock
and refuses queued/active foreground calls and deferred publication/retirement.
Collector, priority, optional-controller and event failures disable optional work;
they never enable a GPU path or reinterpret unknown headroom as idle capacity.

The owner can continue group preparation, indexed compaction preparation or
read-only replica verification inside one
fresh CPU sample. It releases the callback lock between steps, refreshes memory
headroom, and checks foreground activity and stop before each continuation.
Each continuation stays with the preparation that received the opportunity.
Replica verification shares one checked path across a bounded group of
objects. The object-work allowance covers both encoded copies and possible
decoded bytes. The owner checks waiting foreground calls between records
and yields after the current record when the two-millisecond slice has
expired. A synchronous backing read can extend that record's duration.
The controller records examined objects, completed passes and the last
exact checkpoint observation. [Replica scan contract](MIRRORED_PAYLOADS.md#automatic-integrity-scan)

A device launch or route acquisition ends the sequence. Opening a compaction
candidate, publishing its private roots, switching the selected image and
reclaiming the old backing each require a fresh CPU capture. The indexed
compactor can catch up with foreground commits in bounded private steps.
The selector waits for the current generation, and a late commit returns the
same candidate to preparation. Group publication,
indexed inode retirement and advisory checkpoints retain their own fresh
capture requirement. Indexed final Close records retirement and signals the
controller.

Opt-in indexed LZ4 read-ahead can consume spare idle credit after other
maintenance. A backing-cost refusal drops the prediction and records the target extent.
A subsequent ordinary CPU read of that extent can update the backing
cost the next time demand produces a prediction.
[Sequential read-ahead](NATIVE_AMD_LZ4_READ.md#qualifying-sequential-read-ahead)

Complete elapsed time across the admitted steps, including wrapper and
collector overhead, is charged against the 1% pacing target. Activity cannot
erase the next due time. Timed waits pace pending maintenance and GPU budget
observations.
A reaper transaction removes at most 64 mappings. An indexed query that
recovers bytes from a peer queues one repair observation. The existing
foreground activity event wakes the owner. Repair takes priority over
compression preparation and uses a fresh admission before committing.
The controller checks foreground activity, deferred response ownership and
worker retirement before entering either transaction. The owner sleeps
when its maintenance queues are empty and the integrity pass is dormant.

Older indexed directories gain verified summaries during fresh idle
opportunities. The owner alternates this work with eligible compression and
scrub work and preserves the priority of repair, retirement and active group
preparation. When an optional scrub or directory backfill declines under
its CPU policy, pending compression can evaluate its own budget using the
same fresh observation. The declined scan has done no backing I/O. Every
scan or publication returns control to the owner before the next opportunity.
[Automatic backfill](INDEXED_NATIVE_VOLUME.md#automatic-backfill) ·
[Admission validation](evidence/indexed-scale-reopen-2026-11-20.json)

When a verified replica repair cannot append, the controller yields
private group preparation to the provider's repair rotation. The normal
resource and foreground checks still apply, and selector publication
requires a fresh observation.
On a populated indexed volume, scrub checks take turns with eligible group
work and use the remainder of a current CPU window only after an intact
record. Repair, reclamation and already admitted GPU work keep their
existing ownership boundaries. The integrity pass leaves the callback
lock between records. Its first unverified object or index notifies
the helper through the existing lifecycle event.
[Mirrored payload repair](MIRRORED_PAYLOADS.md#automatic-repair-after-a-verified-read)
[Indexed checks](evidence/indexed-owned-windows-2026-11-18.json) ·
[Memory reclamation](WINDOWS_MEMORY_VOLUME_FACTORY.md#automatic-indexed-compaction)

The paired file provider waits for its inactive image to become unpinned
without scheduling retirement attempts. The last image reference wakes the
same controller activity event used by callbacks. A weak notification
reference allows an independent reader to outlive the controller and releases
the controller's duplicate signal handle at shutdown. Selector checks and
fresh resource admission still precede reclamation.
[Persistent range ownership](PERSISTENT_VOLUME_FILE.md#paired-sparse-image-ranges) ·
[Windows auto-reset events](https://learn.microsoft.com/en-us/windows/win32/sync/event-objects)

## AMD shared-base search

A configured indexed controller can supply a qualified shared-base worker
route and workload profile. The route binds the exact worker, anchor module,
system runtime, adapter and node. The worker supervisor owns a reference to
the file leases. A package-enabled indexed controller starts its route
acquisition once live observations admit a real group candidate. The qualified
work range governs the combined member count at compute admission. One scoped
control task checks the installed runtime and device while CPU work continues.
A later admitted group can trigger a fresh attempt after a relevant input
changes, a previously busy transport retires or a fresh measurement clears
documented memory pressure. The controller performs this check through
its existing low-priority group scheduler. Stop cancels acquisition and
joins the task.
[Installed AMD package](AMD_RUNTIME_DISCOVERY.md#shared-base-package-profile) ·
[Route recovery](GPU_RECOVERY.md#installed-windows-routes)

The owner can stage several independent group windows within the same CPU
and memory admission used for group preparation. The qualified member range
applies to their total work. The owner checks device headroom, sizes the
complete immutable request and starts one coalesced worker outside the
callback lock. The worker reuses its runtime, stream and device allocation
across groups. CPU telemetry continues while the submitted groups await the
reply. The owner holds the session that owns the exact supervisor
handle while waiting alongside the stop and activity events.
[Windows wait contract](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)

An approved copy-search profile can queue multiple dispatches within the
same admitted transfer slab. The controller enables the approved depth
when a slab contains a full wait group and the existing phase record fits
in the reply budget. Other exchanges use one dispatch per wait, including
those with a worker deadline beyond the phase record's duration range.
The worker checks live device headroom between dispatches. After confirmed
retirement the controller verifies the record's request digest and geometry. CPU validation of every returned copy range still precedes use of the reply.
The verifier combines contiguous or overlapping suggestions only when they
describe the same source-to-target displacement in the same member. It checks
every resulting run against the immutable request. A different displacement
gets its own comparison even when the normalized advice would omit it. The
reply format and the CPU encoding alternatives remain compatible.
[Shared-base package profile](AMD_RUNTIME_DISCOVERY.md#shared-base-package-profile) ·
[Verification work and full readback](evidence/amd-coalesced-copy-verification-2026-11-20.json)

An indexed batch can span several scan windows. The aggregate preparation
budget follows current physical and commit headroom and the qualified worker
grant. Each group still passes through its own bounded transaction.
Collection ends when the grant fills, the scan ends or the admitted number
of further extent visits has been spent. No GPU worker waits during
collection. The saved scan cursor pauses while tentative groups remain in
memory so a restart can reconsider that work.

At launch, a fixed-output plan selects the largest affordable prefix of
complete groups. The estimate includes child commitment, the transfer
slab, channel mappings and copies. The qualified minimum member count
still applies. Reply lookup uses the original source window identity.
The CPU can prepare and publish unsubmitted groups while AMD executes. Once
the staged suffix is exhausted, disjoint candidates already ranked in the
current discovery window can also advance, including when AMD reserved the
entire staged batch. The owner checks every reserved window's identity and
charges the remaining CPU work against the same memory grant and idle credit.
Publication requires a fresh CPU observation. The discovery window, deferred
intersections and durable scan cursor wait for the reservation to settle.
[Budget-limited AMD and CPU readback evidence](evidence/windows-adaptive-amd-launch-2026-11-20.json)
· [CPU progress with a full live AMD batch](evidence/windows-amd-full-batch-overlap-2026-11-21.json)

A partially covered candidate or one that exceeds the remaining extent
budget defers its intersecting candidates in the current window. Independent
candidates can still fill the batch. If the window has deferred candidates at
the end of the ranking, the owner settles the batch and recomputes the
ranking before leaving the window. Outstanding extents remain eligible for
CPU grouping or a later funded AMD batch. Deferred identifiers are advisory,
bounded by the resident discovery window and retained representatives, and
release with the staged batch.

A foreground callback can wake the owner while a GPU request is outstanding.
The owner checks the exact supervisor and worker retirement before deciding
whether to cancel. If the exchange is still executing, it requests cancellation
and releases the group reservation. The supervisor owns that worker until
retirement, and CPU work can resume while it retires. A foreground yield
carries no device-fault conclusion.

A successful retired reply remains reserved with its original immutable
windows. Polling and foreground activity can observe completion. Collection
and CPU comparison require a later admitted idle opportunity. Packed advice
can stay staged while its members receive independent CPU checks. A selected
content edit invalidates the affected group before publication. Other groups
can use advice approved against their own revisions and bytes. Current
resource refusal, cancelled maintenance and exhausted work release the reply.
Runtime and integrity failures follow the route's recovery policy. Elapsed
worker time paces later launches.
[Windows wait contract](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject) ·
[Windows thread priorities](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority) ·
[Worker ownership](WORKER_CHANNEL.md#asynchronous-sessions-and-independent-cpu-completion)

On the installed AMD route, a foreground read after confirmed worker exit
left five groups reserved for a later idle check. The new check published
advice for 90 members. When the fixture edited a selected extent, four other
groups used the retained advice and the independent CPU reader recovered all
15 files, including the edit. Active-exchange cancellation also completed
through CPU grouping.
[Retired AMD reply validation](evidence/amd-retired-replies-2026-11-22.json)

The live SDK fixture exercised two controller lifetimes with five files each.
The first published four AMD-advised members. A foreground callback in the
second cancelled its outstanding advice, then CPU preparation published the
group. Both reopened snapshots matched the input. The process had 328 handles
between the two lifetimes and after the second, and the supervised process
tree peaked at 105,611,264 bytes of commitment. The fixture supplied a 50%
CPU-load admission profile after the standard profile deferred on measured
load. Both trials used actual host observations and a real AMD worker with
injected WinFsp lifecycle operations. [Source-bound owner evidence](evidence/amd-owned-group-search-2026-11-18.json)

The packaged batch fixture acquired the installed route once and sent two
independent three-file groups in one AMD exchange. Each group contributed
two extents toward the four-member profile. The controller published both
groups and verified their CPU readback. A separate explicit two-group trial
yielded on a real callback event and completed both groups on CPU. The
missing-package control finished on CPU with zero metadata queries or compute
launches. These fixtures use the Windows host and SDK callback lifecycle.
[Batch, cancellation and fallback evidence](evidence/amd-owned-group-batch-2026-11-18.json)

In the larger fixture, five scan windows supplied 310 packed-XOR members
from 45 files to one authenticated AMD compute worker. The five group
transactions completed independently. A fresh CPU reader verified every
byte after compaction. The protected image shrank from 41,623,552 bytes to
11,411,456 bytes on this two-plane related-data workload. The fixture used
actual host memory admission and GPU execution with modeled idle timing.
[Multiwindow evidence](evidence/amd-multiwindow-maintenance-2026-11-19.json)

An append-space refusal releases tentative groups and their advice. Group
preparation pauses on that image generation, while the storage owner can
select bounded compaction when obsolete history covers the reported
shortfall. A changed generation or verified image handoff makes group
preparation eligible again.
[Capacity and matched route evidence](evidence/amd-maintenance-parity-2026-11-19.json)

## Retirement and qualification

The 18 November SDK fixture held the quiet indexed controller for 31 seconds,
then stopped and joined it. The owner issued no maintenance quanta or budget
queries during the wait. The post-join handle count matched the native baseline.
A separate direct service test verified one CPU response and an owner wake for
both a successful commit and a storage fence. The fenced write preserved the
prior verified checkpoint. [Executed checks](evidence/owned-writable-lifetime-2026-11-18.json)

Stop interrupts waits, then the same owner joins deferred work and the provider
dispatcher, removes only its owned mount, deletes its filesystem object and
retires the adapter. [WinFsp lifecycle API](https://winfsp.dev/doc/WinFsp-API-winfsp.h/) Retirement status is exposed after those owners retire;
`stop_and_join` additionally confirms the controller thread has exited. A
retained owner can reopen for writes only after confirmed dispatcher and worker
retirement and release of inspection views.
[Retained volume remount](WINDOWS_MEMORY_VOLUME_FACTORY.md#remounting-a-retained-volume)

Destruction also joins. A native provider stop can block inside WinFsp, so
live experiments still require an independent bounded process supervisor.

[Source-bound validation](evidence/owned-winfsp-controller-2026-11-16.json) separates
actual SDK callbacks, live local telemetry/background priority and injected
lifecycle operations. Tests cover event ownership, unchanged-content quiescence,
invalid plans, startup rollback, stop-before-retirement ordering, destructor
cleanup, unexpected dispatcher termination and lifetime expiry. Populated live
opportunities preserve byte-exact CPU recovery and accepted-input accounting.
Busy or unavailable hosts may correctly defer; captures report actual outcomes
rather than requiring fabricated idle observations.

No test in this checkpoint invokes the native filesystem-create, dispatcher or
mount provider, installs/loads a driver or runs HIP. Existing callbacks and the
unmounted GUI are also checked after sharing the collector library. This does
not qualify current-source Explorer access, mounted interference, physical
power loss, a deployment package or useful ordinary AMD acceleration.

The installed WinFsp driver was inspected read-only and left unchanged. Its
2.1.25156.ddca7bd artifact is distinct from the pinned v2.2B5 development SDK;
a valid Microsoft signature alone does not prove that combination qualified.

## Next integration

The [shared adaptive memory factory](WINDOWS_MEMORY_VOLUME_FACTORY.md) now
composes this controller with retained locked backing and post-join CPU inspection.
The [sealed read-only path](WINFSP_READONLY_MEMORY.md) now connects the linked
provider and the shared memory factory. Its external client probe uses a
separately provisioned runtime. Exercise a disposable mounted paged volume
through external Windows clients. Preserve controller/dispatcher/backing lifetime,
stop ownership, resource envelopes and the owner's driver-stability condition.
Staged reclamation, scalable persistent coherence, complete Windows semantics
and useful automatic AMD operation remain required parallel product work.

The same-day official API captures for [event creation](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw),
[multiple-object waits](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)
and [handle duplication](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle)
support the event lifetime/access design. Their older publication dates are API
background, not new 2026 research or product qualification.
