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
the file leases. The current installed-package binding covers the foreground
LZ4 worker. Acquisition of a packaged shared-base profile still requires
qualification.

The owner stages a verified group window under the same CPU and memory
admission used for group preparation. It receives each completed window once.
It checks device headroom, sizes the complete immutable request and launches
the existing coalesced worker outside the callback lock. CPU telemetry
continues while storage preparation waits for the optional reply. The owner
waits on the exact supervisor handle alongside the stop and activity events.
It holds the session that owns the handle for the duration of the wait.
[Windows wait contract](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)

A foreground callback can wake the owner at entry while a GPU request is
outstanding. Activity or lost group headroom discards the optional result and
signals cancellation. CPU work resumes on admitted opportunities while the
supervisor retains the worker and its resources until retirement. A foreground
yield carries no device-fault conclusion. Late results cannot reenter group
preparation. Runtime and integrity failures retain their recovery
classification, and transient pressure retries require fresh qualified
observations and exact retirement. Elapsed worker time separately paces later
GPU launches. Reply verification runs during a fresh idle opportunity, and
publication goes through the same CPU comparison and packed-size checks.
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
`stop_and_join` additionally confirms the controller thread has exited. Destruction
also joins, never detaches. A native provider stop can block inside WinFsp, so
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
