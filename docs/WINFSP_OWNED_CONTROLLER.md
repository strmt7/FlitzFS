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
completed unchanged volume does not sample or scan. A deferred service's pending
publication or outstanding response remains pending even before its content
sequence changes, preventing a delayed CPU commit from being missed.

While changed work is pending, the existing local English PDH collector and
fresh physical/commit observation feed cooperative group opportunities. Windows
current-thread background resource mode is entered outside view locks and ended
before publishing status or stopping. The adapter still tries the callback lock
and refuses queued/active foreground calls and deferred publication/retirement.
Collector, priority, optional-controller and event failures disable optional work;
they never enable a GPU path or reinterpret unknown headroom as idle capacity.

Complete quantum elapsed time, including wrapper/collector overhead, is charged
against a 1% elapsed-work pacing target. Activity cannot erase the next due time.
Timed waits pace pending maintenance and GPU budget observations. The
synchronous maintenance quantum yields after its transaction. Indexed final Close records
retirement and signals the controller. The controller checks the cached queue
state and requires new, recent CPU and memory observations for each transaction.
A reaper transaction removes at most 64 mappings. The controller checks
foreground activity, deferred response ownership and worker retirement before
entering the transaction. The owned memory controller can then advance one justified compaction
opportunity, including bounded work to release an unpinned old image. A quiet
volume with no pending retirement or compaction sleeps until an event.
[Indexed checks](evidence/indexed-owned-windows-2026-11-18.json) ·
[Memory reclamation](WINDOWS_MEMORY_VOLUME_FACTORY.md#automatic-indexed-compaction)

## AMD shared-base search

A configured indexed controller can supply a qualified shared-base worker
route and workload profile. The route binds the exact worker, anchor module,
system runtime, adapter and node. The worker supervisor owns a reference to
the file leases. A package-enabled indexed controller starts its route
acquisition once live observations admit a real group candidate. The qualified
work range governs the combined member count at compute admission. One scoped
control task checks the installed runtime and device while CPU work continues.
A later admitted group can trigger a fresh attempt after a relevant input
changes or a previously busy transport retires. Stop cancels acquisition
and joins the task.
[Installed AMD package](AMD_RUNTIME_DISCOVERY.md#shared-base-package-profile)

The owner can stage several independent group windows within the same CPU
and memory admission used for group preparation. The qualified member range
applies to their total work. The owner checks device headroom, sizes the
complete immutable request and starts one coalesced worker outside the
callback lock. The worker reuses its runtime, stream and device allocation
across groups. CPU telemetry continues while storage preparation awaits the
optional reply. The owner holds the session that owns the exact supervisor
handle while waiting alongside the stop and activity events.
[Windows wait contract](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)

An indexed batch can span several scan windows. The aggregate preparation
budget follows current physical and commit headroom and the qualified worker
grant. Each group still passes through its own bounded transaction.
Collection ends when the grant fills, the next group would exceed the
retained extent budget, the scan ends, or the admitted number of further
extent visits has been spent. No GPU worker waits during collection. The
saved scan cursor pauses while tentative groups remain in memory so a
restart can reconsider that work.

When a partly covered candidate needs additional extents, the owner settles
the already staged batch before leaving that scan window. The remaining
extents stay eligible for CPU grouping or a later funded AMD batch. This
preserves compression opportunities when several families share features.

A foreground callback can wake the owner at entry while a GPU request is
outstanding. Activity or lost group headroom discards the optional result and
signals cancellation. CPU work resumes on admitted opportunities while the
supervisor retains the worker and its resources until retirement. A foreground
yield carries no device-fault conclusion. Late results cannot reenter group
preparation. Runtime and integrity failures retain their recovery
classification, and transient pressure retries require fresh qualified
observations and exact retirement. Elapsed worker time separately paces later
GPU launches. Reply verification runs during a fresh idle opportunity. Every
group consumes its own verified reply and publishes through the same revision,
CPU byte comparison and packed-size checks.
[Windows thread priorities](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority) ·
[Worker ownership](WORKER_CHANNEL.md#asynchronous-sessions-and-independent-cpu-completion)

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
