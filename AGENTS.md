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
2. Work in small, validated commits on `main`; keep each implementation, regression tests and documentation coherent and all generated artifacts **experimental**. Use official documentation for WinFsp, Microsoft Windows I/O, AMD HIP/ROCm, Zstd and cryptographic dependencies.
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

## Compaction and autonomous-maintenance boundary

Read docs/COMPACTED_COPY.md before changing relocation/copy publication. Preserve
a quiescent latest source, distinct NEW destination identity, source bytes,
bounded single-record scratch, CPU verification, two separately flushed roots
and partial-output retention. Native namespaces must explicitly rebind identity.
Do not add in-place deletion, automatic path replacement or background execution
without their separate handoff/reclamation/priority/resource qualification.

Read docs/AUTONOMOUS_MAINTENANCE.md. The owner requires event/evidence-driven
self-learning of contents and workloads, automatic compression/layout and
fragmentation improvement, AMD acceleration with independent CPU recovery, and
verified redundant self-repair without routine intervention. Background CPU/I/O/
GPU work must have very low priority, hard aggregate caps and foreground yielding.
Unknown headroom/unsafe accelerator health means no work. A timer alone is not
a learning/scheduling decision. Advisory model errors must not bypass integrity,
durability, ownership or recovery gates. Do not promise immunity to every future
bug or physical loss of all redundant copies; never silently discard acknowledged
data or call detection alone self-repair.

Do not count a media-fault test as safe repair if it permits loss of any later
acknowledged transaction. Fresh compaction roots have identical contents;
ordinary historical roots can differ. Preserve that distinction in recovery
oracles and qualify replicated current-commit/acknowledgement state before
claiming arbitrary-root/media self-healing.

## Replicated current-checkpoint boundary

Read docs/REPLICATED_CHECKPOINTS.md before changing root format 2, native commit
publication, peer-manifest admission or repair. Preserve legacy raw-mode bytes
and reads; upgraded commits are sticky. Native writable admission uses two
current roots, separate catalog units, three barriers and pre-write metadata
health fencing. Repair may only use the bound SAME object set, must append
beyond uncertain tails, and must retain every acknowledged byte. Both-copy or
payload loss must fail rather than fabricate or silently select older contents.
Checkpoint generation is a publication-group identity, not namespace sequence.
Keep current 1024-case/256-event/3-MiB-combined-fixture hosted caps and existing
process CPU/memory/time bounds. This is metadata replication, not generic data-
replica self-healing or permission for owner-PC execution.

## Dependency freshness and alerts

Read docs/DEPENDENCY_AUDIT.md. The owner requires latest compatible dependency,
package and tool versions and a real Dependabot alert check. Use official native
release/tag identities and immutable reviewed pins; preserve historical benchmark
oracles. Test upgrades off-host. Unknown access, missing pages or a token-scope
failure is NOT a zero-alert result. Capture and disclose it; never print tokens,
change unrelated provider registrations or modify owner-PC tools. Remaining
compiler/CMake/runtime/transitive-provider work must not be labelled complete.

## Host and installation independence

Read docs/HOST_INDEPENDENCE.md. The owner requires host-/installation-agnostic
execution for each supported OS/architecture: no developer-machine package/app
paths, global installation assumptions or dependence on unrelated apps' DLLs.
Missing user-mode dependencies should be prepared automatically in verified,
bounded, versioned application-owned directories with rollback and CPU operation
preserved. Never make optional GPU setup block durable CPU recovery. Protected
OS/driver integration keeps its separate safety/consent/qualification gates.
These product requirements do not authorize owner-PC disk/tool/driver execution.

Hosted build provisioning is isolated development infrastructure, not a product
installer. Keep official immutable artifacts, whole-download verification,
archive/link/output budgets and fresh output ownership. Stage only CPU tools;
LLVM compilation must never initialize/load a GPU runtime/module. Keep older
research-provider locks isolated until their source/compatibility review.

Portable CPU diagnostics are explicitly selected with FLITZFS_PORTABLE_CPU=ON.
Keep sanitizer and immutable historical comparator settings separate. Runtime
claims require the actual PE/ELF audit and relocated, cleaned-environment byte
checks from check_portable_cpu.py on each OS. Static codec sources and Windows
CRT settings must agree. CLI Windows OS paths use UTF-16 argv/explicit UTF-8
conversion; POSIX native filename bytes must remain unchanged. These checks
never install a driver, mount a volume or authorize owner-PC execution.

The explicit Windows WorkerExchangeSession separates CPU fallback from process
teardown. Preserve exact HANDLE observation, single-launch observers, creation-
time job assignment, private stop events, one process/thread admission lease,
continued supervision after abandonment, and immutable post-exit results.
Replacement needs both process and supervisor-thread retirement. Unknown waits
disable admission; never reopen a PID, spin, drop limits or infer driver repair
from time. Normal CLI/callback paths remain CPU-only; this is not a live HIP
worker or permission for owner-PC execution.

The isolated hosted source-routing lane is described in docs/HOSTED_ROUTING.md.
It must emit real successful CocoIndex search receipts bound to current FlitzFS
source/config/model/package identities before NEW broad navigation. A staged
provider or index marker is insufficient. Verify candidate paths in current
source. Never alter the shared MCP registration or run the provider on the owner
PC. Local model/index tooling is a development dependency only, not filesystem
self-learning or a product runtime. AMD deployment decisions require successful
native Crawl4AI captures of the current official distribution rules.

Exact unchanged-extent reuse must preserve nonempty-write metadata sequences,
all three publication barriers and unique inode/block ownership. Resolve
original-record reuse before allocating IDs, independently verify possible
reversions, and keep truncate tails zero. Maintain the 63-buffer cap and bounded
single-extent comparison scratch. A failed reuse read must fence before writes;
do not turn payload elision into weaker durability or cross-inode deduplication.

Read docs/GPU_WORKER_CONTROLLER.md before changing integrated worker requests.
Keep policy history across requests, expected-backend/configuration trust,
independent CPU comparison, one-time outcomes and exact child/thread retirement.
A denied/current CPU request must not wait for an older helper; external health
must never override actual transport ownership. CPU selection itself performs
no cancellation, OS query or driver call. Do not classify a timeout as repaired
hardware or count CPU-helper tests as live HIP/installer/filesystem integration.

## Current-source rule for important changes

The owner explicitly requires recent reputable sources rather than model-training
memory or assumptions for feature and other important changes. Verify current
primary documentation, actual source/artifact identity and executed behavior;
retain dates, native receipts and limitations. Search snippets are discovery only.

ROCm is the installable SDK distribution and determines its bundled HIP/compiler/
library versions. Do not select HIP independently from a legacy repository's
latest-release pointer. Consult the CURRENT FlitzZip SDK lock, bootstrap, runtime
loader and compile/resource mechanisms before reimplementing GPU infrastructure.
Use source-bound evidence, preserve whole-distribution identity and adapt only
filesystem requirements; never add archive product formats or commands.


## Windows-only product target (owner clarification, 9 November 2026)

FlitzFS is a non-native Windows filesystem ONLY for the current product scope.
Target the latest stable supported Windows 11 release for the selected AMD64
architecture, with a Windows helper application and qualified filesystem-driver
integration. Do not add Linux/macOS product support, old-Windows compatibility
layers, installers or release matrices. Verify the current Windows release,
SDK/WDK and driver acceptance rules from recent authoritative sources before
selecting or changing the target; Insider/preview is not implied by "latest".

Existing non-Windows CPU/sanitizer jobs are internal analysis of shared algorithms,
not a Linux product or supported-platform claim. Preserve useful corruption and
recovery tests; stop developing non-Windows product packaging/features. Windows
Server runner results are development checks, not proof of latest Windows 11
client qualification.

Research existing Windows non-native filesystems, their real kernel/user-mode
boundaries, Windows file semantics, driver distribution/signing and safe service/
app lifecycle before committing to new driver architecture. Prefer verified
mechanism reuse over an unnecessary new kernel implementation. Keep compression,
learning and HIP faults outside the kernel-facing critical path. Product driver
qualification must preserve current Windows security settings; never remove Code
Integrity policies, disable Secure Boot/HVCI or enable test signing on the owner PC.
All driver/app/mount/fault tests remain off-host. The Windows app/driver product
requirements do not authorize any host installation, privilege, persistence or
testing exception.


## Native Windows image projection

Read docs/NATIVE_WINDOWS_PROJECTION.md before changing this bridge. An explicit
Windows-only read-only ordinary-file factory now connects verified native
transactional images to the existing WinFsp adapter/session. Require a nonzero
expected UUID, retain the source handle through dispatcher join, serialize native
reader access and preserve full CPU admission/per-read integrity before byte
exposure. Never accept a mutable custom backend into this callback route or
mistake deny-write sharing for freezing pre-existing writable mappings.

Keep persistent inode IDs and bounded sparse reads. Distinguish backing logical
length, committed end, live decoded bytes, logical file size and allocated
clusters; do not fabricate physical compression/space or persisted timestamps.
The real mounted fixture now uses the native image, <4 MiB aggregate fixture,
a >4 GiB sparse logical file, and independent native recovery/remount after a
confirmed helper kill. The 1 TiB sparse case remains a no-driver projection test.
Keep all existing child/queue/image limits, source bytes and malformed/identity/
degradation/alias tests. No mounted-write, app/installer, live GPU or owner-PC
permission is added.


## Native inode mutation boundary

Read docs/NATIVE_INODE_IO.md before changing stable-inode read/write/truncate,
append or constrained semantics. Preserve volume-scoped IDs, no redirection of
removed/replaced inodes, full supplied-input admission, private verified reads,
single-control-thread EOF selection and the existing replicated commit path.
No accepted count may escape before durable success. Empty/clipped writes do
not publish; nonempty identical writes retain all three barriers and metadata.
Keep result moves after publication nonthrowing, old snapshots and zeroed tails.
This is not Windows open/unlinked-inode lifetime, persistent delete intent or
mounted-write authorization. No owner-PC exception or capacity increase follows.


## Persistent native file lifetime

Read docs/NATIVE_FILE_LIFETIME.md before changing delete intent/cancellation,
detachment, retirement, closed-session admission or FLTXNS02. Preserve legacy
FLTXNS01 bytes/support, sticky capability, strict flags/magic/graph checks,
replicated current checkpoints and the SAME three-barrier commit path.
Pending/detached inodes and data count against all existing aggregate limits.
Keep inode data addressable after unlink and never invent a surrogate path.

Ordinary resume, read-only projection and native compaction must reject
unresolved states. resume_closed_session and retire_inode require the owning
caller's independent proof that old file contexts have retired; the declaration
is not driver/handle attestation. No mounted write, existing-file writer factory,
open-context proof, owner-PC operation or Windows app completeness follows.
The future engine must encapsulate raw pathname mutations, own per-open counts/
dispositions and retain contexts through paging I/O until Close.


## Owning native file session

Read docs/NATIVE_FILE_SESSION.md before modifying context ownership, aggregated
dispositions, latches or verified writer queries. Preserve exclusive writer
encapsulation, weak owner identity/monotonic token scopes, the 256-entry fixed
table, single control thread and no live-owner move assignment. Cleanup must
retain contexts for paging I/O; cancellation cannot erase another request or
latched intent; only final owned Close can retire detached data. Failed commits
must not release/advance contexts or retry I/O. Clear records only after all
fallible work; retain no-throw post-commit updates.

Range/query optimization must compare verified namespace bytes with the last
admitted model and verify requested records privately. Public snapshot/open/
resume still fully verify. Do not equate in-memory counts with proof of old
external/kernel context retirement or enable mounted writes from CPU tests.
The actual Windows helper must establish dispatcher/process retirement before
closed-session recovery; FILE_DELETE_ON_CLOSE, ACL/sharing/locks/flush/app and
Windows 11 client qualification remain open. No owner-PC execution is added.


## Real native Flush

Read docs/NATIVE_FLUSH.md before changing checkpoint/native/session Flush.
Preserve exact acknowledged metadata/extent checks, one actual backend barrier,
native pre/post namespace verification and no publication/repair/trim. Failed or
unverifiable barriers terminally fence; preflight allocation pressure before
barrier entry may leave a healthy writer usable. Never acknowledge Flush through
a no-op, retry a poisoned writer, release contexts or treat it as payload scrubbing,
driver retirement, honest hardware persistence or physical power-cut proof.
All mutation barriers, encodings, resource caps and owner-host prohibitions stay.

## Persistent Windows basic tuples

Read docs/NATIVE_BASIC_METADATA.md before modifying FLTXNS03, tuple presence,
canonical attributes/times or atomic data+metadata publication. Preserve exact
legacy FLTXNS01/02 bytes, sticky capability across lifecycle transitions,
replicated roots, 88-byte format-3 inode headers, strict absent-field zeros,
the combined sixteen-operation cap and the existing aggregate budgets.
Do not persist Windows request sentinels, treat logical sequences as UTC,
invent original timestamps or OR synthetic attributes into present tuples.
Generic operations preserve tuples; the future Windows adapter must supply
authorized automatic updates in the SAME transaction. Separate data/attribute
write grants and context retention through failure. Security/allocation policy,
writable callbacks, helper app, client qualification and live GPU remain open.

## Metadata writer interruption evidence

The basic-store-interruption workflow uses a separately scoped schema-3
FLTXNS03 campaign; preserve all legacy schema-2/976 cases. Bind all six complete
raw/automatic phase traces, decoded live-state closure digests, actual
termination, exact cut/acknowledgement identity and independently reopened
combined continuation. Reject mixed/omitted/relabelled/duplicate evidence.
Keep existing per-process, event, queue, image, case and partition bounds.
A verified process kill is not physical power loss or client qualification.

## Private portability qualification process accounting

Preserve the unchanged two-process/512-MiB-per-process/768-MiB-job/20-percent
Windows qualification limits. Before launching another diagnostic child and
after completion, require actual private-job accounting to show the supervisor
alone. Unknown membership, changed limits or unretired children fail closed;
never raise limits or retry CreateProcess to hide WinError 1816. Retain native
SDK ABI assertions, real controlled-contention coverage, before/after telemetry
and historical failed runs. No owner-host permission follows.

## Automatic native Cleanup metadata

Read docs/NATIVE_CLEANUP_METADATA.md before changing requested effects or the
Windows VOID-ABI bridge. Preserve one atomic metadata+optional-unlink commit,
creation/unrequested fields, absence, strict clocks, first successful duplicate
stamps, authorized delete intent and no-throw post-commit latches/context state.
Implicit trusted FSD updates are not explicit WRITE_ATTRIBUTES grants or ACL
authorization. Keep contexts through paging until Close. Every bridge failure
must fence without I/O/retry/release; unsupported allocation/unknown flags fail
closed. A future supervisor must offline outside Cleanup; never join/stop a
dispatcher within its own callback. Keep the existing read-only table unwired.
Actual per-open -1/-2 suppression, complete Windows policy, allocation/security,
writable adapter/app and Windows 11 client/live GPU qualification remain open.

## Atomic data and requested automatic metadata

Read docs/NATIVE_MUTATION_METADATA.md before changing the metadata-aware inode/
session write or truncate APIs. Reuse the same Stage and replicated commit,
preserve result layouts/nonthrowing success, current-EOF append, full supplied
input admission, constrained counts, zeroed tails and snapshots. Zero accepted
writes never apply effects. Meaningful effects require original present tuples;
invalid clocks fail before I/O. The historical Cleanup type remains intact via
the NativeAutomaticMetadata alias. Implicit trusted effects require data-write
ownership, not explicit attribute grants; do not turn that into ACL bypass.
Keep detached late paging and exact context state on failure, no retries,
no capacity increase and no writable callback/owner-PC permission. Actual
Windows event/suppression/FileInfo/allocation/security/helper gates remain open.

## Commit-bound native mutation results

Read docs/NATIVE_MUTATION_RESULTS.md before changing captured inode facts,
result APIs or post-publication output. Preserve original public layouts and
methods, validated private-stage capture, no-throw bounded scalar copies/moves,
verified no-op sequences/absence and no post-commit allocation/backing/path
query. A failed call returns no result and retains contexts/fencing.
Keep active storage denial after the third barrier and every existing direct
result fault point/mode. Do not fabricate Windows allocation/security/times
or claim this partial output is complete FileInfo. Writable routing/helper,
latest Windows 11 client, live AMD and owner-host qualification remain open.

## Windows space/allocation reporting gate

Read docs/WINDOWS_SPACE_REPORTING.md before changing Windows AllocationSize,
compression/volume claims or the live reporting oracle. Preserve distinctions
between logical/sparse length, decoded live objects, stored payload, committed/
full image bytes, host image allocation and per-file reservation. The selected
FSD rejects FileCompressionInformation; do not infer Win32/Explorer behavior or
advertise unsupported compression from internal codecs alone.
Keep all three source-bound normal/crash/recovery reports, strict record/phase/
type/identity closure, explicit optional errors and aggregate fixture budgets.
Reports are owned hosted fixtures only, never owner-PC diagnostics. The mount-
only target must retain full same-commit native/CPU prerequisites and limits.
Writable allocation/security/FileInfo, Windows 11 client and live AMD remain open.

## Compact checkpoint metadata

Read docs/COMPACT_CHECKPOINT_METADATA.md before changing root format 3,
manifest spans or checkpoint options. Preserve default legacy bytes and
public result layouts, the separate 64 KiB root zones, explicit persisted
failure-unit admission, complete disjoint catalog spans and THREE barriers.
Never infer power-loss units from sectors/OS/device names or overwrite an
acknowledged unit. Resume inherits the stored unit; incompatible/unknown
contracts fence before writes. New-identity copy may reselect geometry only
for its distinct NEW target; source bytes and native identity rules remain.
Keep full catalogs/malformed roots/unit conflicts, all modeled fault modes,
native fixed+compact cases and actual hosted new-file checks. Do not relabel
fixed process campaigns as root-3 coverage. Root-3 termination passed its separate 172-cut gate at 1b81473. Physical
power/device, Windows 11/writable/live AMD and owner-host qualification stay open.

## Compact-layout process evidence

The compact-store-interruption workflow adds separately identified schema-4
root-3/4096 writer termination. Keep fixed schema-2/976 and schema-3/712
campaigns intact. Require actual selected-root digest/generation/format/unit
inspection before acknowledgements and independent recovery/continuation,
complete decoded closures, every observed cut and three barriers, confirmed
child/thread retirement and strict disjoint merge. Preserve all existing
case/event/queue/fixture/process/time caps. Configured gates need exact-commit
executed evidence; they are not physical power-loss/client/live-AMD proof.
Default/mounted layouts and the owner-host prohibition remain unchanged.


## Persistent security descriptor boundary

Read docs/NATIVE_SECURITY.md before changing FLTXNS04 or security APIs. Preserve
opaque self-relative bytes and absent/NULL/empty ACL distinctions, bounded
framing versus actual Windows validation, sticky capability, legacy bytes and
all sixteen combined operations/64-KiB namespace/other resource ceilings.
Descriptor read/full replacement grants are already-authorized distinct rights,
not WRITE_DAC shortcuts or an ACL evaluator. Keep one atomic file/basic/security
Stage, three barriers, snapshots, lifecycle/CPU recovery and no post-commit
fallible result construction. Present descriptors must reach read-only security
callbacks rather than silently using the legacy default. The SDK/file group is
hosted-gated; no writable callbacks, owner-PC operation, live AMD or full ACL/
inheritance/privilege/client qualification follows. Keep old process campaigns
scoped honestly until a separate NS04 termination campaign executes.


## Runtime CPU digest boundary

Read docs/CPU_DIGEST_ACCELERATION.md before changing BLAKE3 dispatch/builds.
Preserve exact upstream source/blob provenance, all digest/stored bytes, scalar
fallback, CPU/OS feature gating and separate ISA translation units. No global
/arch or -march=native, TBB, GPU dependency or forced production feature cache.
Test-only scalar/masked symbols stay separately prefixed and unlinked from
product targets; masks may only REMOVE detected host features. Preserve known
vectors/guards/concurrent use, every old fault point and all resource/time caps.
Same-source capped hash/RAM measurements are not disk/mounted/GPU benchmarks.
Keep partial failures and timing uncertainty, immutable historical comparators,
Windows-only product scope and the owner-host prohibition.


## Atomic native create/open boundary

Read docs/NATIVE_CREATE_OPEN.md before changing create_inode_result/create_open.
Preserve NEW-name-only behavior, explicit canonical tuple/framed descriptor,
one private Stage and THREE barriers for optional initial delete intent.
Preflight context/token capacity and delete grant before storage creation;
install only after success, with statically nonthrowing result/slot operations
and no post-commit allocation/backing/path query. Retain contexts on failure,
Cleanup/paging/Close/stale-owner lifetime, 256-entry and all metadata/resource caps.
The FSD/adapter must supply already-authorized creation/grants/inheritance and
actual Windows descriptor admission; do not invent ACL/allocation/FileInfo.
Keep every legacy case and configured raw/automatic fixed/compact creation fault.
No writable callback, actual Create/NS04 termination, Windows 11 client, live
AMD or owner-PC qualification follows from this pure CPU ownership component.


## Atomic native overwrite boundary

Read docs/NATIVE_OVERWRITE.md before changing overwrite_inode_result or its
session grant path. Preserve live regular/present-basic admission, exact
creation/security/inode/name, zeroed regrowth and ONE Stage/THREE barriers even
for repeated empty same-clock events. Capture results before publication; no
post-commit allocating/backing/path query or context changes on failure.
Separate already-checked overwrite/supersede grants are required: actual
WinFsp may REMOVE implicit WRITE_DATA/DELETE from final GrantedAccess, and
ordinary data/delete/basic/security rights must not imply this operation.
Keep all old API/layout/format/resource/case boundaries. The actual DLL can
Close a failed overwrite open WITHOUT Cleanup; qualify provisional retirement
and offlining before writable wiring, never fake Cleanup/drop poisoned contexts.
No allocation/full FileInfo, writable callback/app/client/live AMD or owner-PC
permission follows. Keep new modeled faults distinct from old process probes.


## Provisional overwrite and VOID Close

Read docs/NATIVE_PROVISIONAL_OPEN.md before changing prepare_overwrite,
overwrite_prepared_result, adopt_prepared, phase admission or native_close.
Preserve exactly one checked grant, existing regular/live-only preparation,
no persistent initial intent and all phase/peer-deletion guards. Healthy
provisional Close may clear ONLY its context with no backing call, fabricated
Cleanup, rollback or deletion; active Close keeps Cleanup requirements.
Poisoned/fenced ownership is retained without retry. Result/phase return after
publication is statically nonthrowing; fixed table/16-KiB implementation and
all old caps/cases stay. The VOID bridge must report offlining outside callbacks,
never join/stop there. Kernel best-effort/normal Cleanup delivery and real
adoption need separate qualification; a declaration is not kernel attestation.
Do not silently map initial FILE_DELETE_ON_CLOSE, allocation or general Create
to this protocol. Actual SDK tests create no volume/dispatcher/mount/driver.
No owner-PC, writable app/client, physical-power or live AMD proof follows.


## Initial delete option and general preparation

Read docs/NATIVE_INITIAL_DELETE.md before changing prepare_open/create_prepared,
NativeInitialDeleteAccess, cleanup_initial_delete_result or latch provenance.
Do NOT persist the FSD initial option at Open/Create/overwrite/adoption.
Real Cleanup arms eligible intent + requested metadata + optional unlink in
ONE Stage/THREE barriers; captured facts update latches only afterward with no
post-commit query/allocation. Preserve nonempty directories/children without
unsafe durable pending; runtime overlays must guard opens/destinations.
Keep explicit/initial/volatile provenance and source-qualified cancel/re-arm
semantics; cancel must not erase independent historical explicit requests.
Old public layouts, durable-intent APIs/bytes, caps/cases and 16-KiB owner limit
remain. Kernel callback-elision/delivery/shared-disposition/adoption policy
needs real qualification before writable binding; never infer it from these
SDK fixtures or a trusted declaration. No owner-PC/app/client/power/live AMD
permission or completion follows.


## Native live-extent reservations

Read docs/NATIVE_ALLOCATION.md before changing FLTXNS05 or reservation APIs.
Preserve exact old bytes/support, sticky capability, strict absent/present-zero
fields, regular-only/aligned/EOF admission and the SAME shared 255-extent/
64-KiB metadata/decoded/dirty/input/history ceilings. Charge tracked reservations
and untracked extents together, including detached contexts; never double-charge
or let compression bypass capacity. Shrink must truncate/zero atomically in
ONE Stage/THREE barriers; generic tracked growth reserves aligned capacity.
No post-commit backing/path/clock/allocation for captured result return. Failure
retains owner/context/fencing. Keep all modeled points/modes and old tests.
These units are native live slots, NOT actual host clusters, physical backing
preallocation or complete Windows AllocationSize/space guarantees. Actual
backing/headroom, atomic Create/Overwrite/cleanup trimming, FileInfo/cache,
writable driver/helper, Windows 11 client and live AMD gates remain open.
Never route successful Windows allocation callbacks from this component alone.
No owner-PC exception or capacity increase follows.

## Process, service and resource hygiene (owner clarification)

Throughout ALL development, every agent must prevent stale/orphaned processes,
unintended background or scheduled services/tasks, leaked RAM/VRAM/handles/
threads/mappings, and avoidable resource use. This applies to normal completion,
errors, cancellation, timeouts and failed initialization; a timed-out observation
is NOT proof that a process stopped.

- Keep every launched process/worker/dispatcher owned by an explicit bounded
  supervisor/job and lifecycle. Confirm actual exit/join and close owned handles,
  pipes, mappings and buffers before releasing its ownership or starting a
  replacement. Unknown membership/retirement fails closed; never hide it with
  retries, higher limits, detached launches or unconditional new workers.
- Do not create automatic background services, scheduled tasks or other
  persistence as development conveniences. A product helper must have an
  intentional, documented lifetime and reliable stop/crash/uninstall behavior;
  it must not survive as an unintended development artifact.
- Use strict aggregate CPU/RAM/VRAM/I/O/process/queue budgets and low background
  priority. Quiescence must release resources and stop needless work. Leak and
  lifecycle checks must cover repeated use, failures and cancellation, not just
  a successful single run. Retain before/after accounting and confirmed child/
  thread retirement evidence; do not claim leak freedom from code intent.
- Dispose of test-owned services/drivers/fixtures only within their expressly
  authorized disposable VM scope, with explicit ownership and bounded teardown.
  VM destruction is containment, not evidence that product cleanup works.
- Never inspect, kill or reconfigure unrelated owner/host processes or services.
  Owner-PC access remains subject to the later RAM-only clarification below.
  Development and lifecycle validation stay off-host until their prerequisites
  pass; authorization for a RAM fixture never permits unrelated host cleanup.

## Owner-PC RAM-only clarification (latest owner instruction)

The owner now authorizes limited PC testing ONLY with part of RAM serving as
the filesystem's virtual test disk, never a system drive or real-storage test
volume. This supersedes the earlier blanket no-PC rule ONLY for that narrow,
qualified RAM-backed test. It does not authorize driver installation,
reinstallation, replacement, update, configuration/security changes, or system
instability. All other off-host development restrictions remain.

- Continue building, researching, fault-testing and qualifying new driver/helper
  behavior on disposable VMs first. Do not transfer an unfinished or unqualified
  writable/GPU/failure experiment onto the PC just because RAM is authorized.
- Before a later PC test, establish that the exact test backing is genuinely
  RAM-backed, exclusively test-owned, bounded and not a system/real-storage
  volume. Do not infer this from a drive letter, path, anonymous allocation or
  a name. Anonymous memory can page; RAM-only process data is not proof of zero
  physical-storage activity. If backing or required headroom cannot be verified,
  do not start the dependent test.
- Use only an already available compatible driver/runtime without changing it.
  Never install a RAM-disk/filesystem/GPU driver, reconfigure a driver, alter
  Secure Boot/HVCI/test-signing/boot policy, or use system-drive files as a
  workaround. Absence of a qualified safe route means keep that test off-host.
- Use synthetic data only, small bounded RAM capacity, substantial verified
  headroom, below-normal priority, strict aggregate process/CPU/memory/queue
  limits and a tested stop/cleanup path. No raw devices, stress tests, deliberate
  kernel/GPU faults, forced power loss, background persistence or scheduled
  services/tasks. Confirm retirement and release all test-owned resources.
- Do not write build/download/cache/temp/log/image/benchmark artifacts to a
  system drive under this permission. Keep development on hosted machines.
  Do not inspect/stop unrelated applications or change any owner data.
- Never promise that RAM alone guarantees no OS/driver instability. Preserve
  explicit qualification evidence and stop on failed safety prerequisites.


## Atomic native allocation events

Read docs/NATIVE_ALLOCATED_EVENTS.md before changing storage-aware Create,
Overwrite, data-result capture or cleanup_event. Preserve original APIs/result
layouts, FLTXNS05/old bytes, SAME Stage/THREE barriers and every capacity/time
limit. Preflight context capacity and grants; install/adopt/latch/clean only
through nonthrowing post-success updates. Keep initial delete volatile until
actual Cleanup and healthy canceled Close free of I/O/rollback/deletion.
Explicit native reservations are NOT physical preallocation or complete Windows
FileInfo. Trim requires tracked regular files, directories remain untracked,
and duplicate Cleanup must match trim as well as old requests/first stamp.
The original bridge still rejects allocation. The explicit allocated bridge
remains unwired; impossible Delete+SetAllocationSize rejects before clock/I/O,
and every engine failure fences without retry/release. Qualify real supervisor,
backing/cache/FileInfo/writable routing before mounted use. Keep sealed results,
all actual fault steps/modes, SDK repeated backing-release/handle evidence and
existing lifecycle tests; none proves universal leak freedom, physical power,
Windows 11 client or live AMD. Apply the latest narrow RAM-only owner permission
without weakening these implementation qualification gates.


## Native Windows FileInfo/data boundary

Read docs/NATIVE_WINDOWS_IO.md before changing strict FileInfo translation or
synchronous data bridges. Preserve all 72 ABI bytes/offsets, exact present
basic tuples/inode identity, explicit native-reserved-cache policy and zero
unsupported reparse/HardLinks/EA fields. Never invent missing original times,
attributes/reservations or label native cache quota as compressed/host physical
allocation. Preflight active data-write ownership and output prerequisites
BEFORE mutation; captured success output/count needs no backing/path/clock/
heap action afterward. Unexpected captured-fact failure fences, zeroes output
and acknowledges no bytes. Flush MUST keep its real barrier/post-verification;
returning prepared file facts is not permission to skip either check.
Keep no-throw native status boundaries, logical-error health and terminal no-I/O
retry/context retention; supervisor offlining stays outside callbacks. No
implicit provisional adoption or grant escalation. The bridge remains unwired
until backing/cache/free-space, helper/lifetime/adoption/security/shared-
disposition policy is qualified. Keep actual SDK dispatch, sealed outputs,
all observed fault steps/modes, independent continuation, repeated ownership/
handle evidence and every old limit/oracle. These do not prove mounted/client,
physical-power, live AMD or universal leak safety. Apply latest RAM-only host
permission without driver changes or premature PC testing.



## Owner GPU-test permission (latest owner instruction)

The owner explicitly authorizes careful GPU use, subject to the existing
RAM-only PC test-storage, no-driver-change and resource-hygiene constraints.
This is not permission to risk system instability/freezes or skip qualification.

- Qualify the exact runtime/worker/device-code path off-host first and preserve
  independent CPU decoding/verification and durable CPU recovery. Do not run
  an unfinished GPU integration on the PC merely because permission exists.
- Begin with tiny bounded synthetic jobs on verified healthy compatible hardware,
  conservative aggregate CPU/RAM/VRAM/queue/time limits and ample measured
  headroom. Unknown/unsafe health or headroom means CPU selection, no probing
  loops or GPU work. No stress/soak run, clock/power/voltage changes, driver
  install/reinstall/update/reconfiguration, WDDM reset/TDR or fault injection.
- Keep GPU execution isolated from kernel/storage publication and acknowledged
  data. On any unverifiable/error outcome, select the independent CPU path,
  fence the failed worker/gate, require actual quiescence before replacements
  and use evidence-driven requalification rather than timeout-only retries.
- Keep launch/output/storage artifacts within the separately authorized RAM
  fixture; no system-drive download/cache/temp/log/test files or persistence.
  Release mappings/buffers/handles/VRAM and confirm worker exit; never leave
  background processes or services. Test cleanup/failure paths off-host.
- Do not promise that user-mode isolation or RAM eliminates driver/kernel risk.
  Record actual evidence and limits; CPU selection itself must launch nothing.


## Hosted unmounted writable candidate qualification

Read docs/NATIVE_WRITABLE_ADAPTER.md before changing the candidate callback
table, native writable session or Windows-global disposition policy. This is
an explicit UNMOUNTED test/integration candidate inside the opt-in SDK target,
not promotion of the default product, old read-only table or mount probe.
Its only factory remains hosted-only, NEW ordinary-file-only and exclusive;
never select a live provider, create a writable volume/mount or run it on the
owner PC from these component results. Real backing/cache/adoption/ACL/shared-
disposition/kernel/client gates above still control mounted promotion.

Preserve the historical aggregate deletion policy by default. Explicit Windows
global cancellation clears current node-wide requests/latches only after
durable success; unprocessed initial per-FD options remain to re-arm at Cleanup.
Keep bounded trusted pre-open lookups, no grant escalation and no stored-byte
change. Serialize all engine calls and exception fencing under the same lock.
Keep 256 monotonic opaque tokens, preflight capacity, no-throw post-commit
installation/output and exact context retention on failure.

The candidate's explicit first-operation adoption is a trusted adapter
transition, not proof of kernel adoption or best-effort Cleanup delivery.
Unexpected active Close fences. No callback may join/stop the dispatcher;
the controller must observe monotonic failure and stop outside callbacks.
Join before mount removal/filesystem deletion/writer destruction, retain failed
outcomes, confirm actual process retirement in future live tests, and treat
remaining contexts/fencing as unsuccessful stop. Keep every old case/cap.

Native AllocationSize/volume totals are explicit thin live quota/cache facts,
not host preallocation, compressed physical bytes or guaranteed future backing.
Do not weaken per-transaction memory/free-space admission or three barriers.
SetSecurity/volume-label mutation and broader Windows semantics remain explicit
unfinished gates. Plain cached WriteFile return is not power-durable data;
real mounted qualification must distinguish it from FlushFileBuffers.

## Explicit disposable writable integration trial

The owner requested full off-host development/testing. The separate
windows-write-mount job may now perform ONLY the fixed synthetic trial in
docs/WRITABLE_MOUNT_TRIAL.md after same-commit CPU/raw/sanitizer/native AND
read-only mounted gates pass. This narrowly supersedes the earlier unmounted
candidate prohibition only for that VM experiment, not product promotion,
arbitrary mounts, the old probe, a self-hosted runner or the owner's PC.
Pinned driver provisioning remains restricted to that fresh disposable VM;
never transfer it to the owner's machine or change the selected driver policy.

Preserve all old cases/caps/oracles and provisional-peer/adoption guards.
One bounded writer and then one independently bounded CPU verifier, each
128 MiB/20% CPU/one process/below-normal/30 seconds; actual exit is required
before the next child. One new image <=4 MiB plus two <=2048-byte reports,
pinned fresh ancestors, synthetic 7001-byte data, no arbitrary paths, no GPU,
no deletion/cancellation/mapped-write/kill experiments in this first trial.
Enforce the image cap before every append/range WITHOUT bypassing the ordinary
backend's identity/memory/free-space/barrier contract. Native quota is not
physical backing. Only actual FlushFileBuffers success is its data durability
oracle; the kernel's cached WriteFile return alone is insufficient.

Stop/join outside callbacks before owned mount/filesystem/writer destruction.
Keep first normal/unexpected stop diagnostics through startup rollback, fail
on missing notification or retained contexts, verify mount disappearance and
independent CPU bytes/checkpoint after confirmed process exit. Never forcibly
delete a stale junction, raise limits, skip failing cases or label the trial
physical-power, Windows 11 client, writable crash recovery or live AMD proof.
If it fails, retain exact evidence and fix off-host; never move it onto the PC.

## Fixed writable lifecycle/acknowledged-termination extension

The latest owner says continue all tasks. Extend ONLY the disposable writable
trial with the separate basic/mutations/delete/crash VM profiles documented in
WRITABLE_MOUNT_TRIAL.md, after all same-commit gates. This narrowly supersedes
the first trial's no-deletion/no-kill restriction ONLY for those fixed synthetic
cases on fresh VMs, not arbitrary crash cuts, mounted recovery writes, mapped
I/O, driver changes on the PC, live GPU or product promotion.

Keep each VM's <=4 MiB image plus two <=2048-byte reports and unchanged
128 MiB/20%-CPU/one-process/below-normal bounds. Normal/verifier children keep
30 seconds; explicitly terminated writer gets 15. Retain every old basic/
read-only/cap/fault/oracle assertion. Require actual successful/terminated
process-object exit before the next launch; never infer it from a PID or time.
The source helper must explicitly FlushFileBuffers and publish/flush its exact
acknowledgement before waiting with its file open; independent CPU recovery must
preserve exact acknowledged data, primary metadata/security/reservation and
checkpoint identity and reject unresolved state. No source view may escape its
owning lifetime or remain across a normal stop.

Run strict source/profile/phase/type/budget/closure reconciliation and negative
tests. Preserve provisional-peer, cleanup/Close, authorization and no-I/O retry
guards; diagnose and fix real Windows failures instead of raising caps, skipping
or relabelling evidence. A helper termination is not physical power loss or full
writable recovery. Apply current RAM-only/no-driver-change and hosted-first PC
rules. Continue filesystem-only scope; do not resurrect removed archive routes.

## Token-efficient continuation (latest owner direction)

The owner asks for maximum token efficiency WITHOUT reducing code quality,
intelligence, safety or required validation. Start with docs/DEVELOPMENT_STATE.json
and verify its qualified commit against current main/CI; it is a navigation
ledger, never authority over source, this file or user instructions. Update it
with executed results in the next substantive implementation commit.

Reuse verified same-commit source/receipts within a task; fetch only changed or
uncertain exact scopes. Before new broad navigation retain current CocoIndex
receipts and live-source confirmation. Native API captures and hosted Crawl4AI
page receipts retain their existing rules. Never alter another task's provider.
Batch independent reads; request bounded excerpts and extract failures/actual
evidence from logs instead of dumping whole files. Retain native handles/digests.
Make one bounded evidence-backed design decision, implement it and test it;
avoid repeated speculative replanning, repeated unchanged status and duplicate
work. Keep required progress updates, every safety condition/negation, exact
symbols/units/uncertainties and all relevant tests. No quality/research/validation
shortcuts, silent model capability reduction, fake results or new delegation.

## Fixed hosted resolved-image restart exception

The separate restart/crash-restart VM profiles in WRITABLE_MOUNT_TRIAL.md may
use the explicit resume_retired_hosted adapter ONLY after their fixed trusted
controller independently confirms actual old source process exit and the
resumer verifies mount absence and exact source acknowledgement. This narrowly
extends the hosted owned-fixture reopening exception; it is not production
existing-file startup, owner-PC access or permission to infer retirement from
a boolean, declaration, PID or delay.

Keep OS-exclusive regular single-link existing-file admission, exact UUID/
generation, complete Windows profile/payload checks BEFORE possible root
repair, and rejection of all unresolved old-session state. Preserve original
volume creation/identity/permissions; never manufacture old Cleanup or metadata.
No source view may survive replacement writer admission. Resume/append and
final CPU observation are separately bounded, sequential children with actual
exit proof before each next launch. Retain the 4 MiB image/two2048-byte report
budgets; append a bounded receipt without replacing the source acknowledgement.
No arbitrary paths, mutable general recovery, raises/skips/false promotion.

## Hosted security-update boundary

Read docs/NATIVE_SECURITY.md before changing SetSecurity. Capture original
Create/Open grants; require WRITE_DAC, WRITE_OWNER and ACCESS_SYSTEM_SECURITY
for their distinct requested parts. Never map data/basic grants to replacement
or expose the session's trusted merge-input query as public descriptor read.
Use the selected WinFsp merge helper and matching deallocator; validate both
bounded self-relative input/output before replicated publication. No post-commit
read/allocation, retry, stored-format change, cap increase or automatic mount.
Keep every old fault/cap/handle/retirement test. SDK dispatch does not prove
kernel access/privilege enforcement, inheritance propagation or client safety.

## Request-bound ownership qualification

The selected SetSecurity frame has NO requester token. Read the latest section
of docs/NATIVE_SECURITY.md and its native/crawl receipts before changing this
policy. Reject whole OWNER/GROUP-bearing requests before adoption/merge/I/O,
including identical SIDs and mixed ACL updates. WRITE_OWNER alone grants no
session replacement access. Do not treat one helper rejection as universal
protection or enable SEF_AVOID_OWNER_CHECK/SEF_AVOID_PRIVILEGE_CHECK. Production
ownership remains required but needs authenticated live requester authority,
negotiated protocol/capability and proven token cleanup; no opener/service-token/
PID/boolean substitute. No driver permission on the PC follows. Retain all old
ACL/cap/fault/retirement cases and native exact outcome reporting.

## Volume-head authority boundary

Read docs/VOLUME_HEAD.md before changing volume selection. Keep its fixed two
64-KiB slots, independently flushed self-contained selectors and exact volume/
epoch/image/checkpoint binding. Compare canonical namespace (only image UUID
rebound) and every live payload; never scan sparse logical size or trust a
caller attestation. Require resolved, quiescent latest source/target and healthy
replicated target metadata. Fence/revalidate the target before head publication;
repair the nonselected peer FIRST. Preserve source and partial candidates.
Terminal errors retain the head lease and expose no cached authority/retry I/O.
The provider must prove global ownership/nonaliasing/durable unique ID mapping;
per-instance leases are not authentication or proof against other facades.
No path factory, source deletion, name replacement, automatic worker, mounted
handoff, driver/PC/GPU permission or physical-power claim follows. Those remain
separate full-product requirements; retain every historical case/cap/oracle.

## Latest owner host-testing instructions (10 November 2026)

The PC IS in testing scope. Earlier text excluding all owner-PC testing is
superseded. The filesystem under test MUST be RAM-backed, never an HDD/SSD/NVMe
image/system volume, until the owner changes this rule. Carefully bounded CPU
and GPU tests are authorized after exact-path audit/qualification and live
headroom checks; no driver installation/reinstallation/change, clocks/power/
boot changes, risky stress/fault injection, popups or unattended persistence.
Use only a small part of RAM/VRAM, hard child process-tree memory/CPU/time caps,
low priority, terminal failures and actual exit/cleanup proof. No stale services,
workers, tasks, mappings/locked pages/handles or needless long runs. Unknown
backing/headroom/driver qualification means no dependent launch, not disk fallback.

Real-file corpora are authorized as well as synthetic input. The owner permits
modest TEMPORARY data on the verified HDD, preserving all existing user files.
Start below256 MiB combined and use per-file/HTTP/time/drive-reserve caps; never
hundreds of GiB or near exhaustion. Keep new outputs inside unique owned fixtures,
CREATE_NEW, record URL/commit/size/hash/failures and clean only verified owned
files. These source downloads are NOT permission for physical-drive filesystem
backing. Keep benchmark claims separate for CPU RAM core, mounted I/O, GPU and
idle recompression; configuration/plans are not implementation or speed evidence.
The owner requests fully autonomous work without questions/popups; continue
independent work when a prerequisite is missing rather than relaxing safety.

Small qualification executables and bounded numeric reports may accompany the
permitted temporary HDD source corpus within the SAME256-MiB staging cap; they
are not filesystem backing. Use the explicit owned HDD fixture, never silently
stage to system TEMP. Keep image/head data in verified locked RAM. Audit native
artifact/PE/source identity and execute the exact VM-qualified bounded binary
before claiming a PC pass. Do not load the detected stopped2025 WinFsp driver
as though it were the qualified2026 path. Read docs/HOST_RAM_TESTING.md.
