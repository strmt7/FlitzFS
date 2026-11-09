# Agent instructions for autonomous FlitzFS development

## Non-negotiable safety constraints

The owner's highest-priority requirements are: **do not format or repartition drives; do not damage existing data; do not freeze the system; do not exhaust RAM, VRAM, CPU, or disk space.** Treat any unverified claim of safety as a blocker, not an invitation to take a risk. An isolated test is preferred to any risky test on the owner's Windows PC.

- Never run, suggest executing, or programmatically invoke raw-device writes, `diskpart`, `format`, `clean`, `Initialize-Disk`, `Clear-Disk`, partition changes, volume-level reconfiguration, or commands that alter boot/system state.
- Never install, load, or enable a filesystem/kernel driver on the user's PC automatically. Never auto-mount over an existing directory or drive letter. Mounting is a separate release gate and must begin with a disposable read-only fixture.
- Never overwrite, move, modify, truncate, delete, or recursively remove files outside a freshly created, uniquely scoped test fixture. Do not use `remove_all` in tests.
- Preserve source data unmodified. Require creation of a **new** output path and verify it does not already exist at the OS open/create operation, not merely with a prior existence check.
- Keep all builds/tests CPU-only by default. GPU experiments are opt-in and limited to synthetic data with tiny memory and short duration. Never stress-test WDDM or alter GPU clocks/power/voltage/driver settings.
- Do not run background services, scheduled tasks, persistence across reboot, Windows registry edits, or privileged operations on the user machine as part of an automatic task.
- Guard resource use with explicit hard caps, disk-space reserves, bounded queues, timeouts, and CPU fallback. If resource headroom cannot be determined, fail closed.
- Do not declare real-world data safety, full NTFS compatibility, Windows qualification, or HIP speed improvements from tests that did not evaluate those properties.

## Owner-controlled development state

The owner alone decides when to release. Do not add or bump product numbering,
create tags/releases, publish packages or call development commits releases.
Identify progress by implemented behavior, exact commit hashes and executed
checks. Preserve existing wire compatibility and historical evidence.

## Development workflow

1. Inspect current git history and user changes before editing; preserve existing repo content and licensing. Develop and commit directly to `main` only; do not create development branches or PRs. Never force-push.
2. Batch coherent implementation, regression tests and documentation into larger validated checkpoints on `main`; follow the owner's request to push less often and prioritize substantive development/token efficiency; mark all generated artifacts **experimental**. Use official documentation for WinFsp, Microsoft Windows I/O, AMD HIP/ROCm, Zstd and cryptographic dependencies.
3. Extend deterministic byte-for-byte tests and malicious-input rejection before adding new storage features.
4. Run CPU-only unit tests, raw-only build, ASan/UBSan where supported, and CI on both Windows and Linux. Do not substitute Linux success for Windows qualification.
5. Design filesystem changes with explicit rollback/recovery, correct flush durability, no data loss under kill/power failure, and an independent CPU reader.
6. Run any system-level or GPU tests only after the source is audited and the test harness enforces strict resource budgets and kill-switch behavior; never use primary disks or unique user data.
7. Update README, FORMAT, SAFETY, ROADMAP, and validation results with actual measured facts, not projected results.

## Present development boundary

The project has an **OFF-by-default native WinFsp read-only callback backend**
to the CPU `.flfs` container, synchronized catalog and Windows-path projection.
Read `docs/WINFSP_CALLBACKS.md` before touching it. Callback tests use synthetic
files and the user-mode SDK DLL only: no filesystem creation, mount, dispatcher,
driver installation, persistent service or GPU operation. The pinned SDK staging
script may run only on disposable GitHub-hosted Windows runners, never on the
owner's PC. A successful callback test is not a mounted-filesystem qualification.

Reader is single-threaded; catalogs lock each file. Native callbacks additionally
bound handles and concurrent decodes. Destruction/move must not race callbacks.
Encryption, authentication, HIP execution and writable volumes remain unimplemented.
Keep optional behavior opt-in and report actual coverage without upgrading
partial tests into production claims.

## Explicit isolated integration exception

The optional hosted-CI-only mount probe is documented in `docs/LIVE_MOUNT_PROBE.md`.
Normal builds and CTest remain no-mount/no-driver. Only the separate
`windows-mount` job may provision the pinned driver on a disposable GitHub-hosted
Windows VM, after the ordinary CI gates pass. Never transfer that permission to
the owner's PC, a self-hosted runner, arbitrary files or an existing mountpoint.
The probe must keep its 128 MiB/20% CPU/30-second process limits and synthetic-only
input policy. Kernel faults cannot be covered by a no-freeze guarantee.

The bounded plaintext collection manifest and read-only CLI are described in
`docs/CATALOG_MANIFEST.md`. They do not implement encryption, authentication,
transactional writes, or hostile-directory sandboxing. Keep the existing
namespace and index budgets; validate all references before backing access.

FlitzFS is a filesystem. The owner's FlitzZip repository supplies engineering
inspiration and candidate GPU/compression mechanisms only. Do not add archive
readers, import/export compatibility, archive CLI commands, archive mount
routes or archive serialization product dependencies. Retain genuine licensing
and mechanism provenance. Read docs/EXTENT_CODECS.md for the bounded internal
compression decoder and optional test-only mechanism comparisons.

## GPU preparation boundary

Read `docs/HIP_BATCH_PLAN.md` before changing the GPU batch, device kernel or
DXGI collector. The kernel is compiled offline only, never loaded automatically.
The default CLI and filesystem paths remain CPU-only. CPU semantic tests of the
shared device body must not be described as GPU execution. Keep 8-block/64-KiB/
256-KiB batch bounds, independent CPU comparison, fail-closed memory telemetry,
immutable owned input, one outstanding request and terminal failure semantics.
No actual-runtime integration or owner-device experiment is qualified yet.

## Worker transport boundary

Read `docs/WORKER_CHANNEL.md` before changing worker frames or process exchange.
Normal file operations still never launch a worker. The Windows fixture runs
only the same trusted test binary and CPU semantics. Do not call those results
GPU execution. Keep bounded memory-only frames, one exchange at a time, protected
local-session objects, successful confirmed child exit before reply copying,
independent reference comparison, and no automatic runtime retries.

## Owner's current host boundary (9 November 2026)

This later instruction supersedes earlier permission for local CPU testing.
All further disk-based development, builds, tests, research-tool execution,
benchmarks and cleanup run ONLY on disposable hosted machines/VMs, never the
owner's PC. Use remote repository APIs for edits/commits; leave the existing
local checkout and temporary artifacts untouched. Do not launch local processes
or install a RAM disk/driver to work around this boundary.

RAM-only host experiments remain unqualified and must not start automatically.
Even anonymous memory may be paged; do not label a memory-only process as proof
of zero storage activity. Driver/mount/power-loss/GPU-fault tests stay off-host.
Use conventional benchmark suites and several relevant comparator filesystems
near product qualification, with equivalent durability/resource/workload settings.
Owner-host RAM measurements cannot establish physical-storage durability/speed.

Across requests, the owner requires evidence-aware GPU recovery rather than
repeated device searches or timeout-only retry. Read docs/GPU_RECOVERY.md.
A per-request poisoned gate remains terminal; any later candidate uses a new
gate and qualified evidence with supervisor-confirmed worker quiescence.
Read docs/RESILIENT_STORE_DESIGN.md before adding writable storage. GPU faults
must not affect the durable CPU recovery path or acknowledged committed bytes.
The live GPU runtime/coordinator and writable mounted store remain absent. The bounded compressed-object transaction core is described in docs/OBJECT_STORE_FORMAT.md; it is not routed into CLI/mount paths. Preserve its new-file-only public ordinary-file factories, sealed segments,
terminal I/O poisoning, explicit backend assumptions and hosted-only file tests.
Qualified custom-backend resume and the internal owned-fixture reopening
exception are specified in docs/STORE_RESUME_AND_INTERRUPTION.md.

The existing shared CocoIndex MCP registration belongs to another repository.
Do not use it as a FlitzFS receipt or change its registration. Prior searches
bound FlitzFS through the reviewed adapter and verified source receipts. Restore
an off-host reviewed provider before NEW broad searches; exact known scopes
remain appropriate for direct source/API reads. The prior Crawl4AI captures
remain evidence. The reviewed provider is now admitted in hosted-research.yml
with native capture/digest review in durability-evidence.yml. Keep new page
research off-host with the same isolation/robots/timeout/receipt requirements;
never run the host crawler or substitute search snippets.

## Native transactional compression boundary

Read docs/OBJECT_STORE_FORMAT.md before modifying store codecs or records.
Preserve historical raw-mode bytes, separate stored/decoded integrity, complete
private CPU round-trip verification before writes, and hard library allocation
ceilings. Never silently bypass unavailable recovery codecs or roll back a
corrupt latest dependency. Timed RAM comparisons are not disk/mounted-filesystem
benchmarks; retain unfavorable results and count metadata/padding/history.

## Owned-image resume qualification exception

Only the internal hosted test adapter may reopen existing ordinary images for
writes, exclusively inside freshly owned VM fixtures. It must validate the
hosted environment, OS writer exclusion, regular single-link file identity,
resource bounds and the core identity/generation lease. No production file
resume factory, CLI/mount write route or owner-PC permission follows from this
exception. Retain backend lease lifetime through poison, aligned orphan
preservation, surviving-root repair and independent acknowledged-history tests.

## Native file layer boundary

Read docs/NATIVE_FILESYSTEM.md before changing the namespace/inode/extent layer.
Preserve atomic namespace+data publication, stable inode IDs, old snapshots,
zeroed truncate tails, full graph/reference admission and terminal I/O fencing.
Do not silently increase the 256-inode/255-nonzero-extent live limits, the
63-dirty-extent staging bound, or the independent 16 MiB decoded object budget.
Any further capacity change needs explicit aggregate-memory/metadata reasoning
and positive/negative/fault tests. Do not introduce archive routes or expose
mounted writes. Private incremental staging must still verify metadata and every
touched record; preserve full before-exposure verification on public snapshots,
open and resume, and document delayed detection of untouched-media corruption. Keep logical sparse size separate from
physical allocation and history. The extended hosted process probe covers native
file transactions but remains distinct from physical power-loss qualification.
