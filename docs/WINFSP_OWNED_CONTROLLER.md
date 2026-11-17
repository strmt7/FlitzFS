# Owned WinFsp background controller

[Paged callback engine](WINFSP_PAGED_VOLUMES.md) · [Windows observations](WINDOWS_BACKGROUND_MAINTENANCE.md)

## Implemented ownership

`WinFspVolumeController` owns one thread and a `WinFspNativeSession`. Construction
requires an explicit adapter, provider operation table and valid mount plan;
there is no default native provider or automatic driver selection. That thread
alone starts the session, supplies background opportunities and stops it. The
existing experimental plan lifetime is enforced, including while idle.

The controller uses the same writable callbacks, managed paged engine, protected
shared-base encoding and independent CPU reader. A new compression format or
alternative filesystem is not introduced. The tray/application may observe a
small synchronized view and request stop; it does not execute storage work.
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
foreground count. Dispatcher termination also signals. The owner checks the
cached namespace sequence before collecting telemetry. Read-only activity on a
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
Timed waits pace pending work or enforce the explicit lifecycle deadline; they
do not periodically recompress unchanged content. The synchronous quantum and
final commit cannot be preempted once entered. This is not a measured universal
CPU-share or foreground-latency guarantee.

## Retirement and qualification

Stop interrupts waits, then the same owner joins deferred work and the provider
dispatcher, removes only its owned mount, deletes its filesystem object and
retires the adapter. Retirement status is exposed after those owners retire;
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
