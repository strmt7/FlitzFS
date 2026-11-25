# Windows helper background maintenance

[Application](WINDOWS_VOLUME_APP.md) · [Core controller](AUTOMATIC_GROUP_MAINTENANCE.md)

## Shared engine integration

The experimental unmounted helper now creates mirrored managed paged volumes
through `NativeFileSession::create_paged_volume`. Files, protected shared-base
groups, contexts, typed heads, CPU recovery and the reserved advisory area use
the same native engine. `NativeGroupMaintenance` automatically discovers groups
after content changes; callers no longer supply candidate file lists.

This helper is an integration stage, not the final file browser. **Normal Windows
Explorer/Win32 access remains a required product feature.** The existing WinFsp
writable callback layer already owns `NativeFileSession` and now has an explicit
[paged factory and cooperative group entry point](WINFSP_PAGED_VOLUMES.md).
The separate [owned WinFsp controller](WINFSP_OWNED_CONTROLLER.md) now connects
callback activity, this collector/priority implementation and serialized
lifecycle ownership. Current-source mounted qualification remains next work.
The tray controls lifecycle/status; it must not become a separate filesystem or
be required for ordinary file reads/writes. See [the Windows product contract](WINDOWS_PRODUCT.md).

## Allocation and retained ownership

The helper and WinFsp controller composition now use the same
[adaptive backing factory](WINDOWS_MEMORY_VOLUME_FACTORY.md). Backing lifetime,
fresh admission and independent post-join inspection have a common contract.

The admitted capacity comes from live physical and commit headroom. The
factory reserves future image and advisory space and locks actual pages
as the volume uses them. Routine background opportunities check the
working-set lease without walking the entire image. Surviving references
retain the lease through final owner retirement. These volumes have
process-lifetime durability. [Backing contract](WINDOWS_MEMORY_VOLUME_FACTORY.md)

Explicit Optimize now uses verified managed paged recompression/handoff and
suppresses an unchanged namespace sequence. It may compact retained history;
the UI reports completed maintenance rather than promising smaller output for
every explicit operation. Automatic staged compaction/reclamation remains open.

## Trusted opportunities and resource priority

`WindowsMaintenanceHost` reads `GetSystemTimes` when the machine has one active
processor group. It checks group and processor counts on both sides of each
capture and starts a fresh rate window when that topology changes. On machines
with multiple groups or when the native call fails, the host uses the aggregate
language-neutral PDH processor counter.

Two successful samples establish a 1–2 second rate window and require fresh
physical and commit headroom. The native delta subtracts idle time from kernel
plus user time and rejects decreasing, empty or inconsistent counters. PDH
converts its percentage to one million units with upward rounding. The core
separately checks the policy limit on sample duration and the freshness of the
capture before admitting any optional work. When the next paid opportunity
would outlive its CPU reference, pending work on a quiet volume can request
one reference refresh during the existing wait. The controller schedules it
inside the accepted sampling window and lets foreground events interrupt
the wait. The refresh takes no storage step and adds its own elapsed cost
to the pending duty. The normal opportunity still needs current CPU and
memory evidence. A live Windows check compared an expired reference with a
refreshed one at the same paid deadline.
[Reference refresh evidence](evidence/windows-maintenance-reference-refresh-2026-11-25.json)

Group-duty recommendations estimate the time needed to earn enough
credit. A check only credits a freshly measured CPU interval. Pending
duty therefore rechecks at the existing CPU sampling cadence if the
estimate takes longer. The outer elapsed-work deadline still applies.
Each check can defer again for contention, insufficient memory or an
unpaid balance. Unrelated retry deadlines keep their original timing.
[Signed replay and duty-sampling evidence](evidence/windows-packed-settlement-credit-2026-11-25.json)
[Windows system timing](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes) ·
[Processor groups](https://learn.microsoft.com/windows/win32/procthread/processor-groups) ·
[Native sampling and AMD recovery evidence](evidence/windows-native-cpu-sampling-amd-resume-2026-11-20.json)

The existing single owned controller worker runs preparation outside queue
and UI locks. One fresh CPU capture can fund several short preparation steps.
The owner keeps the capture time, refreshes physical and commit headroom for
each step, and stops when another step of the observed cost would exceed 1%
of the sample interval. The core charges its existing idle credit once.
Current-thread Windows resource background mode covers telemetry, residency
and core work and is restored before normal queue handling. A
priority failure disables optional maintenance and releases its private work;
it is not storage authority. CPU file access and volume ownership are retained.
Foreground requests and stop signal the owner's wake event and are checked before
each continuation. Publication requires a fresh CPU capture. A currently
executing synchronous call or publication holds its existing boundary.
[PDH sample intervals](https://learn.microsoft.com/en-us/windows/win32/perfctrs/collecting-performance-data) ·
[Windows resource priorities](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority)

The helper measures complete elapsed time across the admitted steps, including
collection, residency checks and priority overhead. It paces subsequent work at a 1%
elapsed-work target in addition to the core's own accounting. Foreground
commands do not erase that measured pacing debt. Timed waits only pace pending,
content-driven work; once the scan is complete the worker waits for a real
command and stops sampling. This is not a timer that periodically recompresses
unchanged files, nor a measured universal CPU/interference guarantee.

## UI and retirement

At most one unconsumed scalar completion notification is queued. The window
acknowledges it before reading the latest state. Background status has a separate
generation from foreground content results, preserving unsaved editor text and
list selection. No background step runs a full UI directory refresh.

Stop joins the same existing worker. Query handles, private group buffers,
native owners and locked allocations retire before stopped status is exposed.
Tests separately measure native PDH cold initialization and require stable
native-only cycles plus exact handle balance across full application cycles.
No detached thread, service, scheduled task, registry edit or reboot persistence
is installed.

## Evidence and limits

[Exact-source validation](evidence/windows-background-maintenance-2026-11-15.json)
separates live Windows collector/controller grouping and GUI checks from modeled
quiet-observation checks. It retains the initial cold-handle failure and native
initialization diagnosis. CPU byte-exact recovery, foreground access during
preparation, grouped handoff, editor preservation, working-set restoration and
owned retirement are evaluated within the stated fixtures.

The old GPU-probe fixture's fixed 128-MiB allowance was insufficient for ASan.
Application qualification now uses a separate job with at most 1/16 of fresh
physical/commit headroom after its reserve, 20% CPU and a ten-second user-CPU
limit. The GUI child uses the existing adaptive creation-time supervisor within
half that allowance, with a 25-second deadline. No ASan checks or quarantines
were disabled. These are test envelopes; the product provider envelope stays
40 MiB. Both original resource refusals remain in the evidence.

Current official documentation was captured with the admitted isolated Crawl4AI
provider, with URL/status/text/digests retained. Some API pages carry older update
dates; they are current API background references, not newly published research.
The current local Windows SDK and actual API execution provide separate platform
evidence. WinFsp sources describe the driver/user-mode bridge and Explorer shell
integration; this round does not qualify a new mounted path or driver installation.

Scalable paged native access, persistent coherence/learning, lifetime write
amplification, automatic compaction, stronger collective codecs, useful ordinary
AMD execution, full Explorer semantics and physical durability remain unfinished.
No compression record, full AI capability or release readiness is established.
