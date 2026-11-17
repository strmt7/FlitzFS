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

The helper queries physical and commit headroom before creation. Within the
memory provider's existing 40-MiB qualification envelope, it admits at most
1/256 of the smaller available headroom after reserving the larger of 768 MiB
and 1/8 of detected physical memory. Capacity is rounded down to 64 KiB and
creation refuses below 9 MiB. These are current experimental provider limits,
not final product sizing or assumptions about installed RAM.

The admitted domain funds two equally sized image reservations, one 128-KiB
head and all four possible 128-KiB advisory bindings. Image capacity is derived
from that domain, not a fixed 4-MiB image. Existing lock/residency checks, actual
allocation accounting, ownership through surviving references and working-set
lease restoration remain enforced. No file/device backing or driver operation
is introduced. Temporary contents still do not survive process exit or power loss.

Explicit Optimize now uses verified managed paged recompression/handoff and
suppresses an unchanged namespace sequence. It may compact retained history;
the UI reports completed maintenance rather than promising smaller output for
every explicit operation. Automatic staged compaction/reclamation remains open.

## Trusted opportunities and resource priority

`WindowsMaintenanceHost` owns one local built-in PDH query for
`Processor Information(_Total)\% Processor Time`, added with the language-neutral
API. This avoids silently treating `GetSystemTimes`' primary-group result as
whole-machine load on systems with more than 64 processors. No remote counter,
registry repair, sampling thread or external process is created.

Two samples at least one second apart establish a rate. Only a successful
counter status, finite 0–100% value, actual 1–2 second monotonic window and fresh
physical/commit observation can reach the core. Busy ratios are normalized to
one million units and rounded upward. The core now explicitly bounds its CPU
observation window by policy, default 2,000 ms, separately from the default
250-ms capture freshness. Late windows re-prime rather than reusing stale rates.
An unavailable collector disables optional background execution without repeated
discovery. File access stays on the CPU engine; no host counter/driver is changed.

The existing single owned controller worker performs one opportunity outside
queue/UI locks. Current-thread Windows resource background mode is entered for
telemetry/residency/core work and restored before normal queue handling. A
priority failure disables optional maintenance and releases its private work;
it is not storage authority. CPU file access and volume ownership are retained.
Foreground requests and stop wake the condition variable and are checked before
the next quantum. A currently executing synchronous call/publication cannot be
preempted by this controller, so no hard foreground-stall bound is claimed.

The helper measures complete quantum elapsed time, including collection,
residency checks and priority overhead. It paces subsequent work at a 1%
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
