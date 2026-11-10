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
