# Development gates and acceptance criteria

**Product scope: Windows only, latest stable Windows 11, AMD64, with a helper
application and driver-backed filesystem integration.** Older Windows and other
OS products are outside this phase. See WINDOWS_PRODUCT.md.

This is a staged plan, not a claim that the full product is implemented.
Development stays on `main`; research, regressions and actual CI results gate
promotion. No new branches, force pushes or owner-PC operations are needed.

Optimize sustained read/write throughput and physical compression together;
minimum latency is secondary. Future codecs are not restricted to FlitzZip's
algorithms. The broad file-type corpus, compression portfolio and comparative
acceptance criteria are specified in COMPRESSION_STRATEGY.md. No superiority
claim follows from compatibility tests or one synthetic throughput comparison.

| Stage | Status / deliverable | Promotion evidence or remaining gate |
|---|---|---|
| M1: CPU chunk core | Implemented experimental v1 pack/read/verify/new-only extraction | CPU codecs, corruption/bounds tests, Linux/Windows CI; retain original data |
| M1.5: OS handles and catalog | Implemented regular-file reads, exact Zstd frames, bounded virtual tree | No-follow/special-file tests, case/path/collision tests, codec/raw/sanitizer CI |
| M1.6: concurrency and verification | Implemented per-file locks and bounded whole-catalog verification | Deterministic concurrent byte parity and corruption detection |
| M1.7: Windows virtual projection | Implemented root-relative ASCII translation and short reads | Portable tests and Windows CPU CI; no mount |
| M1.8 / release 0.2: native callback foundation | Implemented OFF-by-default WinFsp read-only backend, pre-allocation budgets and physical-path guards | Direct real-SDK Windows callback tests plus Linux/raw/sanitizers; this is NOT mounted qualification |
| M2: native compression mechanisms | Bounded CPU extent mechanisms and independent tests implemented | Native extent framing, adaptive selection, diverse-corpus ratio/throughput and CPU recovery; no archive compatibility goal |
| M3: Windows read-only mount | Native transactional image connected; lifecycle, live native reads and process-crash/remount qualified at named commits | No collisions/recursion; explicit driver provisioning; clean unmount/restart; actual ACL/mapped-I/O/cancellation/app tests; CPU export |
| M4: adaptive CPU/HIP | Bounded six-kind batch, gate, recovery policy and Windows worker controller; complete ROCm 10.1.0 kernel compile/link qualified at named commits | Trusted live runtime, child-specific telemetry, actual Windows 11 AMD hardware parity/faults, H2D/kernel/D2H timing and measured routing remain incomplete |
| M5: authenticated immutable store | Not implemented: AEAD records, authenticated manifest, key handling | Vetted crypto; nonce/clone/rollback analysis; authentication-negative/fuzz tests; independent recovery |
| M6: transactional writes | Bounded native file/object COW engine, replicated current checkpoints with three barriers, integrity, exclusive fixture reopen and source-preserving compacted copy implemented | Production scaling/reclamation, public safe writable ownership, physical persistence and Windows mounted mutations remain incomplete |
| M7: limited writable preview | Not implemented: opt-in disposable mounted folder | M6 plus ACL/locking/mapped I/O/rename/app tests and independent backups; never automatic sole-copy use |

The native callback foundation first used a bounded read-only catalog to test
Windows ABI semantics. The adapter can now open the native transactional
inode/extent image directly with an explicit volume identity. Legacy catalog
fixtures remain regression coverage. This integration still exposes a read-only
snapshot; Windows mounted mutations, production scaling and authenticated
metadata remain separate unfinished gates. See NATIVE_WINDOWS_PROJECTION.md.

## Persistent catalog addition

`CATALOG_MANIFEST.md` specifies the implemented read-only 8 KiB/16-record
collection manifest and CLI operations. This advances persistence but does not
satisfy M5 authenticated metadata or M6 mutable/durable transactions.

## Immediate integration checklist

- Qualify the native-image adapter through direct callbacks and disposable
  mounted reads, forced helper termination and independent CPU reopen/remount.
- Preserve mountpoint reservation, pinned source ownership, bounded supervision
  and dispatcher retirement before source destruction.
- Retain the no-install/no-mount callback lane as a fast regression check.
- Implement the separate Windows helper application and writable engine/session
  boundary with cached I/O, flush, handle lifetime and delete-intent semantics.
- Qualify actual AMD runtime parity and recovery before routing filesystem work
  through the GPU. Ordinary CPU operations must not initialize it.
- Keep legacy collection manifests distinct from the native transactional graph
  and future authenticated metadata.

## Performance comparison matrix

Use the same data, cache state and equivalent durability. Measure actual
allocated backing bytes including metadata; logical throughput; 4 KiB random
p50/p95/p99 latency; sequential throughput; CPU/RAM/VRAM; H2D/kernel/D2H time;
GPU busy-vs-idle effects; measured power; write amplification; recovery time.
Compare raw NTFS, CPU FlitzFS, GPU FlitzFS and built-in Windows compression when
applicable. Correctness runs are not performance evidence. Heavy benchmarks,
stress tests and GPU workloads remain disabled by default.

The CPU pipeline now reuses codec contexts and chunk workspaces without changing
v1 storage bytes. Its bounded hosted comparison alternates the pinned baseline
and candidate on identical synthetic data and verifies complete container and
extraction parity. This is an initial throughput regression check, not the
cross-filesystem/compression frontier study. See CPU_PIPELINE.md.

## GPU preparation addition (0.3.2)

The immutable batch planner, independent CPU reference, allocation-free HIP
kernel candidate and result gate are described in HIP_BATCH_PLAN.md. Offline
compilation and CPU semantic tests are evidence only for their named stages;
see HIP_BATCH_VALIDATION.md. No runtime loading, kernel launch, speedup or
filesystem GPU integration is implied. The next gate must provide an explicit
supervised worker, exact adapter/module admission and end-to-end hardware tests
before changing normal CPU-only behavior.

## Executable durability gate

PERSISTENCE_MODEL.md defines a test-only commit-persistence experiment. It
evaluates recovery obligations separately from the bounded object engine;
M6 remains incomplete. Ordinary-file I/O, namespace durability, integrity, independent
recovery and fault-injected writable qualification remain required. The native
research provider now runs in a bounded isolated hosted VM, never on the owner PC.

## Concrete transaction engine gate

OBJECT_STORE_FORMAT.md records the new bounded native object format, admission,
recovery and fault-test contract. It provides a base for compressed immutable
extents and durable namespace transactions. Capacity changes require explicit
decoded/physical/staging budgets, bounded metadata validation, no-write negative
admission and recovery tests. Production scaling and exposed application writes
still require reclaim, writable file reopen, process/OS interruptions and
physical durability qualification. Do not call the object engine a qualified mounted
filesystem or general power-loss guarantee. GPU work remains optional and must
never become a dependency of acknowledged-data recovery.

## Filesystem scope correction

Archive reader/CLI/catalog/mount routes and archive serialization tests are
removed. FlitzZip supplies mechanism inspiration and optional test-only CPU
comparisons. Its archive format/product parity is not a FlitzFS requirement.
EXTENT_CODECS.md replaces the former archive compatibility development track.

## Compression connected to transactions

The object engine admits native zero/motif/Zstandard candidates, independent
CPU round-trip checking and decoded integrity before publication. It preserves
raw-mode bytes and keeps codec failure outside the I/O-poisoned state. This
advances M6 storage primitives; native file/extent namespace, writable reopen,
reclaim and mounted mutation semantics remain required. The hosted comparison
is RAM-backed native-engine evidence, not cross-filesystem qualification.

## Continued persistent storage

Exclusive custom-backend resume, uncertain-tail preservation and missing-root
repair advance the transactional persistence foundation. The new hosted process
interruption gate must verify acknowledged history and successful continuation
on Windows and Linux before public existing-file admission or writable namespace
integration. The file adapter's existing-image mode remains internal, hosted-only
and confined to uniquely owned fixtures. See STORE_RESUME_AND_INTERRUPTION.md.

## Native file semantics connected to transactions

NATIVE_FILESYSTEM.md specifies the implemented directory/inode/extent layer:
atomic metadata/data updates, sparse holes, partial writes, truncate/regrow,
identity-preserving rename/replace, removal and persistent snapshots. It operates
within the current bounded object store; this does not satisfy production
capacity, mounted application semantics or full Windows metadata compatibility.
The hosted interruption gate includes a combined native namespace/data update
and continuation after recovery, alongside the original storage/repair cases.

## Bounded capacity and incremental staging

The native object envelope now admits 256 live records under an independent
16 MiB decoded live-data ceiling. The file layer admits 256 inodes and 255
nonzero 64 KiB extents, with a 63-dirty-extent transaction allowance and a proven
64 KiB metadata ceiling. Final-budget admission precedes codec work; overlap
validation sorts bounded ranges. Writer staging checks namespace and touched
records instead of rereading the full volume on each write. Public snapshot,
open and resume still fully verify reachable data. NATIVE_FILESYSTEM.md defines
the immutable-backend assumption and delayed detection of untouched corruption.
These are developmental scaling steps; multi-gigabyte allocation, reclamation,
writable mounted semantics and hardware qualification remain outstanding.

## Compacted copy and autonomous maintenance direction

COMPACTED_COPY.md supplies a source-preserving native relocation primitive:
latest-snapshot/lease checks, a new volume identity, CPU-verified copied records,
two complete separately flushed roots, interruption recovery and continuation.
It excludes obsolete history from the destination; source storage is retained.
Automatic durable handoff, reader retirement and actual reclamation remain.

AUTONOMOUS_MAINTENANCE.md records the required workload-driven learning,
automatic compression/layout/fragmentation improvement, low-priority bounded
CPU/GPU execution and verified redundant self-repair. Learning state is advisory
and cannot authorize unsafe publication or replace independent CPU recovery.
The executor/model/replica-repair mechanisms are not implemented by copying.

## Current acknowledged-metadata replication

The native file layer now uses two current roots and isolated replicated
manifests, with pre-write health fencing and independent repair. This closes
the previously identified latest-root-loss metadata gap for upgraded checkpoints;
REPLICATED_CHECKPOINTS.md specifies the exact fault envelope and costs. It does
not close file-payload redundancy, physical persistence, automatic handoff/
reclamation, resource-priority/learning or AMD-runtime/application gates.

- Asynchronous Windows transport now separates CPU-result selection from helper
  cancellation and exact process/thread retirement. Its single-slot lifetime
  and abandonment checks run as CPU-only hosted fixtures. Production recovery-
  policy/runtime identity integration, automatic private AMD component setup,
  actual HIP dispatch and hardware fault/benefit qualification remain required.

## Redundant payload writes

The native file layer elides exact unchanged bytes and ordered updates that
restore the same inode/block contents. It preserves metadata publication,
replicated barriers, source-read verification, independent recovery and all
existing limits. NATIVE_FILESYSTEM.md specifies the bounded comparison cost and
fault/regression checks. This reduces one source of encoding/write amplification;
it does not establish a throughput advantage over other filesystems or implement
background learning, writable mounts, production capacity or GPU dispatch.

## Explicit worker requests connected to recovery policy

GpuWorkerController now joins policy admission, asynchronous transport, immutable
gate validation, terminal CPU fallback and exact retirement. The integration
bounds one pending helper, permits subsequent CPU work during retirement, and
retains configuration quarantine/hysteresis across requests. Windows CPU-helper
tests exercise the integrated lifecycle and keep the strict handle baseline.
This advances supervised request orchestration; real HIP dispatch, trusted
package/discovery evidence, measured routing, automatic installer and actual
filesystem GPU operation remain separate incomplete gates.


## Windows app and driver product gates

The helper application must provide volume and mount lifecycle, readable
health/recovery/space/compression state and automatic bounded maintenance without
routine manual tuning. It must report current driver/runtime readiness and preserve
CPU access when acceleration is unavailable. A CLI or correctness harness is not
completion of this application requirement.

The filesystem engine must expose Windows read/write/rename/delete, file and
directory handles, sharing/delete disposition, byte locks, oplocks/caching,
flush/write-through, file mappings, security descriptors, attributes/timestamps,
directory enumeration/notifications and error/cancellation semantics through the
selected driver interface. Unsupported operations must return accurate Windows
status and never fabricate success. The exact supported capability matrix must
come from implementation and application tests.

The production driver must satisfy the current Windows 11 signing and security
policy, clean installation/update/rollback, stop/unmount/restart and bounded
request cancellation. Do not infer kernel/driver stability from a user-mode Job
Object or a clean Windows Server runner. Actual latest Windows 11 VMs with normal
security settings, Driver Verifier/HLK-appropriate checks, Explorer/application
workloads and recovery/fault campaigns remain separate qualification gates.
No automatic owner-PC installation, raw-device write or security-policy change
is permitted.

Comprehensive current primary-source research compares the existing WinFsp
foundation against Dokan, direct Windows FSDs, ProjFS and Cloud Files before an
architecture decision is promoted. The low-level driver transport and the
filesystem's compression/durability design are separate decisions; benchmark
evidence must justify any later custom-driver replacement.


## Native storage connected to Windows

The explicit native read-only image projection now connects the transactional
inode/extent engine to the existing Windows callback/session and live hosted
probe. It binds UUID and persisted IDs, serializes native reads, preserves
sparse bounds and reports distinct image/checkpoint/live-data metadata for the
future helper. Independent native CPU recovery/remount replaces the legacy
catalog in this probe. App/installer, writable ordinary-image ownership,
Windows mutation/cache/ACL/time/handle semantics, scale/reclamation, latest
Windows 11 client and live AMD qualification remain required.


Stable-inode engine I/O now supplies snapshot stat/list/read and durable
write/truncate with current-EOF append and constrained accepted counts.
NATIVE_INODE_IO.md tracks the exact contract and configured failure tests.
The next writable integration still needs open/unlinked-inode lifetime,
persistent delete-intent cancellation, Windows metadata/sharing and flush
semantics before callbacks can replace their current write-protected behavior.


Persistent native lifetime primitives now separate delete intent/cancellation,
retained detachment and explicit retirement. NATIVE_FILE_LIFETIME.md defines
the opt-in namespace capability, closed-session recovery contract and fault
checks. The owning Windows handle table/counts/dispositions and actual context
retirement proof are still required before writable callbacks. A library
precondition is not a substitute for that lifecycle evidence.


The native context owner now encapsulates its writer, tracks bounded per-open
tokens/dispositions, retains cleaned contexts and gates final retirement.
NATIVE_FILE_SESSION.md tracks verified range-query traffic, late-I/O/failure
cases and remaining external context/Windows ABI/security/cache/app gates.
This is the engine layer for writable integration, not completion of that
integration or the full Windows product.


The engine now has an explicit real shared-image Flush with metadata/namespace
pre/post checks and terminal failure fencing (NATIVE_FLUSH.md). This advances
the writable synchronization boundary. Actual Windows callback FileInfo/cache/
mapped/write-through/cancellation behavior and physical persistence remain
separate gates, not conclusions from CPU barrier tests.

### Persisted basic Windows tuples

NATIVE_BASIC_METADATA.md specifies canonical optional attrs/times, sticky
FLTXNS03, atomic combined namespace/data/basic publication, owning-context
attribute grants and read-only SDK/mount exposure. Keep transaction counters
separate from UTC, current hard limits and legacy formats. The new configured
fault/capacity/malformed/API tests must pass at the exact commit before
qualification. Next required work remains automatic per-open Windows timestamp
policy, secure descriptor persistence/ACL behavior, allocation/reclamation,
Create/Overwrite/write-cache/cancellation integration and the helper app.

### Metadata-specific real writer interruptions

The new basic-store-interruption Windows workflow configures separately
bounded upgrade/update/compound writer kill coverage with closure-digest and
strict union oracles; legacy cases remain intact. Inspect exact native results
before closing this test gate. Automatic FSD timestamp/attribute events,
secure descriptor and allocation persistence, writable callbacks/helper,
Windows 11 client, live AMD, scaling/reclaim/repair and learning remain required.

### Automatic Cleanup engine and VOID-ABI bridge

Requested regular-file archive/access/write/change effects can now combine
with unlink in one durable commit. Owning contexts retain first successful
duplicate stamps, latched intent and late paging lifetime. The selected SDK
user-mode bridge samples one clock and fences failure across its VOID callback;
configured native/SDK tests require executed receipts before qualification.

Complete per-open timestamp suppression/restoration, Create/Overwrite/write
metadata policy, persistent security and allocation, supervisor offlining,
writable callbacks/helper and latest Windows 11 client remain required.
No writable mount, live HIP, scale/reclaim, redundant payload repair or automatic
learning claim follows. See NATIVE_CLEANUP_METADATA.md.

### Atomic owning-session data/basic effects

The stable-inode write/truncate engine and owning session now accept trusted
requested archive/time effects in one durable transaction, preserving accepted
counts, clipped/empty writes, original tuple absence, detached late paging and
old snapshots. NATIVE_MUTATION_METADATA.md defines the configured fault/grant/
identity checks; exact executed CI remains required.

Next writable integration still needs complete per-open time policy, truthful
FileInfo/allocation, persistent security/ACLs, Create/Overwrite/cache/cancellation,
supervisor offlining and the helper app. Capacity/reclaim, verified redundant
payload repair, autonomous learning and measured live AMD benefit remain open.

### Commit-bound Windows mutation facts

The native/owning-session result path now captures inode, size, sequence,
optional tuple and lifetime facts before publication, returning them with
durable accepted counts without further backing I/O. Original APIs/layouts
remain regression paths. NATIVE_MUTATION_RESULTS.md defines configured sealed-
backend and direct result fault checks; actual exact-commit CI remains required.

Next: real allocation/security and complete Windows FileInfo translation,
qualified per-open metadata policy, Create/Overwrite/cache/cancellation,
writable callback/supervisor/helper lifecycle and Windows 11 client admission.
Live AMD, production scaling/reclaim, redundant payload repair, automatic
learning and unbiased common-filesystem benchmarks remain open.

### Truthful Windows allocation/compression reporting

The source-bound read-only mount oracle now observes standard allocation/EOF,
volume identity/geometry/flags, compression query outcomes and separate backing
image allocation across normal/crash/recovery. WINDOWS_SPACE_REPORTING.md
defines strict closure and remaining interpretation gates. Configured reporting
must pass and be inspected at the exact commit before conclusions.

Use actual results to choose persisted allocation/security and complete
Windows FileInfo/write callbacks; the selected FSD lacks native compression-
information support. Do not equate rounded logical length, sparse holes,
stored payload, committed image/history and physical host allocation.
Windows 11 client, writable helper/driver lifecycle, live AMD, scaling/reclaim,
redundant payload repair, learning and representative benchmarks remain open.

### Reduce whole-image checkpoint waste

An opt-in compact replicated catalog capability now uses persisted, explicitly
qualified failure-unit geometry while keeping 64 KiB-separated roots, complete
independent manifests and three barriers. COMPACT_CHECKPOINT_METADATA.md
defines strict compatibility, modeled failures, hosted new-file checks and
native byte profiles. Inspect executed results before claiming measured gains.
Default and mounted layouts remain fixed. Root-3 real writer termination passed all 172 cuts at 1b81473;
keep broader adoption explicit; physical power/device/client,
reclaim/scaling, Windows reporting/writable app and live AMD remain open.


### Persistent Windows descriptor foundation

NATIVE_SECURITY.md adds atomic bounded descriptor persistence, owning descriptor
grants and read-only persisted security callbacks. Configured CPU/Windows SDK
groups require exact-commit executed evidence. Next complete Windows per-part
authorization/inheritance/privilege policy, NS04 real termination, actual mounted
ACL checks and truthful allocation/FileInfo before writable helper integration.
Keep Windows 11 client/live AMD, scaling/reclaim, redundant payload repair,
autonomous learning and representative filesystem benchmarks open.


### Accelerate the independent CPU integrity path

CPU_DIGEST_ACCELERATION.md adds verified upstream runtime CPU dispatch with
portable fallback and unchanged stored digests. Inspect actual Windows/parity/
sanitizer/static results and same-source capped RAM measurements before claiming
a benefit. Continue toward writable Windows allocation/security/FileInfo/helper,
scalable indices/capacity, NS04 process/mounted ACL conformance, live AMD and
bounded automatic learning/redundant payload repair.


### Atomically create the inode and its owned context

NATIVE_CREATE_OPEN.md defines a new NEW-name creation path that admits context
capacity before publication and captures complete basic/security/lifetime facts
before the same three-barrier commit. Optional delete-on-close intent is atomic;
successful result/slot installation needs no post-commit query or allocation.
Configured denial, lifetime, admission and every-step modeled failure groups
need exact-commit hosted inspection. Existing create APIs/bytes remain.

Continue to real Windows allocation/full FileInfo, ACL/Create option/overwrite
mapping, caches/mappings/concurrency and driver/helper/client integration.
This component does not enable writable callbacks or qualify actual Create/
NS04 process kills, live AMD, production scale, repair or automatic learning.


### Atomic existing-inode overwrite and checked operation grants

NATIVE_OVERWRITE.md adds EOF reset, attribute merge/replacement, archive and
one supplied access/write/change timestamp in the same replicated event.
Original creation/security/identity and owned peer contexts remain. Separate
already-checked overwrite/supersede grants account for the actual FSD's
implicit-access removal; ordinary final GrantedAccess bits cannot substitute.

Configured exact-commit invariant/sealed/fault groups need hosted inspection.
The selected DLL closes a failed overwrite open directly without Cleanup;
provisional-open retirement/supervisor handling must be qualified together
with allocation/full FileInfo before writable callback integration.
Windows 11 client/app, live AMD, scale/reclaim, repair and learning remain open.


### Provisional overwrite ownership and actual SDK Close

NATIVE_PROVISIONAL_OPEN.md introduces prepared, overwritten and explicitly
adopted contexts, I/O-free healthy cancellation and retained poisoned ownership.
Peer deletion is excluded while preparation is unresolved. A VOID Close bridge
reports a supervisor-visible fenced outcome; configured SDK cases invoke the
actual selected Overwrite/Close/Cleanup dispatcher without a filesystem object.

Inspect exact-commit native/sanitizer/SDK/fault results before qualification.
Next: atomic initial overwrite delete intent and general Create adoption/failure,
allocation/full FileInfo and actual writable kernel/helper lifecycle. Normal
Cleanup delivery policy and stopped-queue retirement need source-bound tests.
Windows 11 client, live AMD, scale/reclaim, repair and learning remain required.


### Initial delete-on-close and general Open/Create preparation

NATIVE_INITIAL_DELETE.md adds separate prechecked initial-option authority,
general opened-phase preparation, prepared creation, Cleanup-bound atomic
intent/metadata/unlink and safe retention of nonempty directories. Fixed latch
provenance distinguishes explicit, initial and volatile authority, including
cancellation/re-arm without removing independent explicit requests.
Existing APIs/layouts/wire bytes and limits remain.

Configured engine, sealed, interruption and actual SDK groups need inspected
exact-commit execution. The future adapter must qualify callback-elision flags,
live kernel shared disposition/adoption and full allocation/FileInfo before
writable binding. Windows 11 client/app, live AMD, scalable indexing/reclaim,
redundant repair, learning and conventional-filesystem comparisons remain open.

### Native allocation capacity prerequisite

Implement explicit persisted native live-extent reservations, mixed reserved/legacy accounting, growth, atomic shrink and bounded captured results (NATIVE_ALLOCATION.md). Qualify the new matrix before passing this component. Still required: actual backing preallocation/history headroom, complete Windows FileInfo/free-space policy, atomic Create/Overwrite allocation and trusted Cleanup trim, writable callback/helper integration and Windows 11 client/live AMD qualification.

### Allocation event integration

NATIVE_ALLOCATED_EVENTS.md combines native reservations with Create/Overwrite/Cleanup and captures reservation facts for data results. Qualify the new matrix/SDK lifecycle cases before passing this component. Physical backing admission, complete Windows FileInfo/cache/space policy, actual writable helper/driver routing, Windows 11 client and live AMD qualification remain required. Apply the latest AGENTS.md RAM-only PC permission and resource-hygiene requirements; it does not make unfinished driver/GPU/fault experiments suitable for the PC.

### Windows FileInfo and synchronous data boundary

NATIVE_WINDOWS_IO.md adds strict complete FileInfo translation and owned Write/SetFileSize/query/Flush bridges with actual SDK/fault coverage gates. Qualify executed results before passing this component. The explicit native-reserved cache presentation is not physical preallocation or compressed-size reporting. Actual writable callback/helper routing still needs backing/free-space/cache policy, kernel adoption/shared disposition/security, latest Windows 11 client, scaling and AMD qualification.


## Hosted unmounted writable integration candidate

NATIVE_WRITABLE_ADAPTER.md combines actual callbacks, explicit Windows-global
deletion policy and an injected join-before-release controller. Configured
qualification adds old/new fault closure, bounded callback/concurrency/handle
checks and all partial startup rollback paths. Inspect exact executed jobs;
configuration is not passing evidence. The old read-only table/mount probe is
unchanged. Next gates are a supervised writable VM helper, kernel cancellation/
adoption/cache/Flush behavior, backing/free-space and ACL/client qualification;
then RAM-only owner tests only if every prerequisite is verified without driver
changes. SetSecurity, scale/reclaim, live AMD, adaptive maintenance and payload
replica repair remain unfinished. No superiority or full-product claim follows.


## First supervised writable VM trial

WRITABLE_MOUNT_TRIAL.md configures actual Win32 create/write/explicit Flush/
readback and orderly mounted-helper retirement, followed by a separately bounded
CPU verifier after confirmed exit. It uses a <=4 MiB backing cap and preserves
all old read-only/resource/adoption oracles. Inspect executed jobs before marking
this trial passed. Next extend real-kernel adoption/cancellation/delete/overwrite,
cache/mapped I/O, helper-kill/acknowledged-history and recovery coverage, then
client/app/backing/scale/GPU/learning/repair requirements; none is complete from
a small successful write or an unmounted test. Owner-PC testing stays deferred.


## Extended writable lifecycle and acknowledgement recovery

The fixed writable VM matrix now configures EOF shrink/regrowth with zero-tail
checking, stable-inode rename, deletion while an owned handle retains readable
data, and independent preservation of an unrelated file. A separate profile
kills a live helper after an explicit mounted Flush and records exact CPU
recovery after actual exit. Strict metadata/data/security/reservation/checkpoint
reconciliation is required; inspect exact jobs before marking these profiles
passed. Preserve all old guards and limits. Cancellation/overwrite/mapped I/O,
more fault cuts and re-entry/recovery, Windows 11 client/app, scale/reclamation,
live GPU, learning and payload self-repair remain unfinished.


## Persistent writable restart experiment

The fixed restart and crash-restart profiles now configure actual source exit,
exclusive resolved-image resume, a fresh writable remount preserving original
bytes/identity, new flushed continuation data and a separately supervised final
CPU observer after resumer exit. Exact pre-resume acknowledgement comparison and
append-only handoff/state-digest reconciliation are required. Inspect executed
jobs before promoting this checkpoint. General unresolved-state recovery,
production startup/app/installer, arbitrary failure cuts, cancellation/mapped
semantics, history reclamation/scale and live AMD/learning/repair remain open.


The opt-in hosted writable adapter now has a bounded SetSecurity candidate using
WinFsp's merge helper and original per-part granted-access checks. SDK/CPU tests
are configured; inspect exact-commit CI before claiming a pass. Live caller-authorization/privilege, inheritance propagation, client/product, scale/reclaim and GPU/learning/repair qualification remain unfinished.
See [NATIVE_SECURITY.md](NATIVE_SECURITY.md) for its contract and limits.


The hosted writable candidate now rejects all owner/group-bearing security
updates atomically before merge/adoption/storage access because the selected
WinFsp request does not carry the current requester's token. Original handle
rights and equal SIDs do not supply that authority. DACL/SACL behavior and stored
formats remain unchanged. Full owner/group support remains required through a
separately qualified authenticated request path; it is not complete. See
NATIVE_SECURITY.md and WINDOWS_OWNERSHIP_SOURCES.json for current-source
evidence, regression requirements and driver/host boundaries.


The experimental custom-backend native volume head adds a fixed 128-KiB,
replicated authority for selecting a verified compacted image while retaining
the source and a stable external volume identity. Its publication/recovery,
CPU comparison, minimum-checkpoint binding and fault-test contract are in
VOLUME_HEAD.md. Existing encodings/routes/caps remain unchanged. The CPU model tests passed at
dd388157456d0ae6ed9735c0c16a5e83826b37b7; see VOLUME_HEAD_VALIDATION.md.
This does not qualify mounted head integration. No file-provider, mounted handoff,
source deletion, automatic reclaim/worker, GPU or owner-PC operation follows.


The owner explicitly includes their PC in carefully bounded RAM-only testing.
The optional locked-RAM CPU probe and immutable real-file corpus policy are in
HOST_RAM_TESTING.md. It requires VM-qualified exact provenance, page locking,
hard process budgets and confirmed cleanup, with no WinFsp/driver/GPU invocation.
Small source/tool/report staging on the verified HDD is separately capped; it
never supplies filesystem image backing. Configuration is not a host execution
or mounted/GPU/idle-recompression qualification. Existing formats/routes stay
unchanged. Dependabot OPEN-alert access was successfully verified (0 on the
complete10-November snapshot); see DEPENDENCY_AUDIT.md for scope.

The explicit CPU post-compression primitive converts a preserved raw/fast
source into a separately verified NEW image. It preserves native state and
object generations, streams bounded objects and compares decoded source/target
bytes before replicated publication. See RECOMPRESSION.md. Automatic
idle learning/scheduling and live GPU execution remain unimplemented. The first
six-file owner-PC unmounted RAM CPU result is in HOST_RAM_VALIDATION.json;
its complete image remained larger than the tiny corpus despite compressed
payload reduction. New re-encoding tests require their own exact-commit pass.

Managed native volume sessions now connect fixed-head image handoff to the
existing context-owning I/O layer. Explicit compaction/recompression preserves
live resolved handles, access grants, Cleanup state and stable volume identity.
A failed transition retains both images and the context table for one verified
CPU recovery attempt. See VOLUME_SESSION.md. This remains custom-backend
engine work, with new exact-commit tests pending; no automatic maintenance,
mounted/GPU integration, physical reclamation or power-loss claim follows.

Incremental recompression now gives a managed session one-record work units and
generation-checked candidate cancellation. Next, qualify event-driven admission
under foreground contention and bound final verification/publication latency.
The measured host corpus result used explicit CPU recompression in locked RAM.

The hosted WinFsp maintenance trial now consumes explicit quiet opportunities,
rejects inadequate measured savings and avoids duplicate work on an
acknowledged generation. Remaining work includes trusted event delivery,
bounded final verification latency and workload-based codec selection with
qualified AMD acceleration and immediate CPU fallback.

The staged store encoder can consume bounded external match proposals and retain the smaller independently verified frame. Next, connect a source-bound AMD producer through the existing supervised worker and measure end-to-end costs and ratio on real workloads.

A bounded AMD match producer is compiled with the complete pinned SDK. It searches eight predecessors with at most 1024 byte comparisons per lane. Next, qualify transport and supervisor integration, then compare measured device and end-to-end costs on public data.

The explicit HIP worker can dispatch the compiled match entry through bounded source-bound frames. CPU fixtures now check channel-to-store publication and fallback before helper retirement. Next, connect the long-lived evidence-aware routing policy and measure qualified device behavior on real files.

The match controller now preserves recovery history across requests. It supports immediate CPU selection, late-result classification and configuration quarantine without a retry loop. Next, qualify the workload controller and actual AMD execution, including measured compression gains and CPU/GPU costs on public data.

The supervised HIP path must pass SDK-derived export inventory, typed properties/LUID tests and child compiler-environment isolation before a separate tiny hardware experiment. These CPU/PE checks do not establish GPU execution or speed; the authenticated private-runtime selection is unchanged.

Installed-driver preparation now has a shared metadata verifier and explicit no-load worker mode. Complete its matching SDK qualification before adding the separate kernel-only bundle and actual installed-runtime compute route. Preserve the current source-bound recovery and process-retirement gates.

The installed-driver route now binds the expected System32 runtime hash to a kernel-only GPU module. Qualify its complete refusal matrix and self-bounded diagnostics before the first metadata-only owner check and separately qualified tiny compute experiment. Repeated real-data CPU/GPU measurements follow hardware correctness, not compile success.

Qualify and run bounded adapter metadata before the first explicit hardware probe. The metadata helper does not initialize HIP, choose a HIP ordinal, reserve VRAM or enable default GPU routing. Hardware correctness and controlled real-data measurements remain subsequent gates.

The first tiny installed-driver owner experiment on 37aaed8 refused with
unsupported_configuration and confirmed child exit. Do not repeat that unchanged
configuration or proceed to match experiments. First qualify stage-preserving
failure diagnostics and inspect the matched DXGI budget without HIP. Resolve the
observed refusal before the next separately qualified compute attempt. No GPU
speed, real-data GPU result or production safety conclusion follows from refusal.

An incremental native profile probe now provides bounded, source-bound
measurements before a maintenance policy is chosen. Qualify its CPU/Zstandard,
raw-only, Windows and sanitizer tests, then use these observations in a bounded
learner and event-driven resource controller. Do not infer whole-volume gains
from samples or weaken the existing unchanged-image suppression and handoff
gates. Automatic workload learning and GPU execution remain separate work.

Resolve the observed installed-HIP zero node mask with the independent Windows
physical-node proof in GPU_NODE_PROOF.md. Require exact-commit portable/fake-API
and real hosted WARP metadata checks, then one bounded owner OS metadata query
before one tiny GPU decode. After byte-exact GPU correctness, qualify native
match production and controlled end-to-end compression benefit; avoid unchanged
refusal loops and never infer speed or production readiness from a tiny fixture.

The exact OS node proof now passes on the owner adapter. Diagnose the retained
later device_fault / none decode failure using specific shared memory-predicate
stages and current Microsoft memory-accounting sources. Do not weaken freshness,
reserve or residency rules based on a guessed cause; no accepted GPU result or
speed improvement is established yet.
