# Path to a stable FlitzFS release

Assessment: 15 November 2026. Reviewed local source:
`7c601d39c8eb5f04a02bb889840efc06eac68171`. The subsequent
[native backend bridge](NATIVE_PAGED_BACKEND.md) and
[inode-record persistence](INODE_RECORD_NAMESPACE.md) and
[directory name indices](DIRECTORY_NAME_INDEX.md) and
[native paged maintenance](NATIVE_PAGED_MAINTENANCE.md) and
[bounded staged rebuilding](STAGED_PAGED_REBUILD.md) and
[payload mirroring](MIRRORED_PAYLOADS.md) and
[shared-base groups](SHARED_BASE_GROUPS.md) are traced separately; the
paged namespace work and release gates remain open. The full product goal remains active.
Shared-base preparation now yields by decoded extent and reconciles unrelated
foreground commits; its final storage publication is still synchronous. This
does not close E04/E06/E07 or reduce the uncalibrated effort estimate below.
The [managed paged path](MANAGED_PAGED_VOLUMES.md) now integrates typed head
selection, contexts, source-preserving repair/handoff and advisory funding.
The [automatic group controller](AUTOMATIC_GROUP_MAINTENANCE.md) now discovers
and extends aligned groups, reuses content hashes and suppresses repeated
outcomes under supplied/model resource observations. It does not close E04/E07:
live Windows observation/priority/idle scheduling, persistent paged coherence,
wear policy and application factories remain. No remaining-effort reduction is
claimed without measured integration velocity or held-out compression results.
The [Windows helper](WINDOWS_BACKGROUND_MAINTENANCE.md) now integrates live
CPU/memory observations, background resource mode, full-quantum pacing and
automatic groups on managed paged volumes. This advances E07/E08 but does not
close them. The next bridge is its Explorer-facing WinFsp managed factory and
synchronized maintenance/lifecycle, followed by scalable persistent access and
useful ordinary GPU work. The helper editor does not replace Explorer.
This plan authorizes no release, tag, package publication or product version bump.
It is the current forward plan; older dated checkpoints retain their evidence,
not competing instructions to repeat completed experiments.

## Current position

FlitzFS is an experimental Windows filesystem/storage implementation, substantially
short of a stable release. Its strongest implemented foundations are CPU-readable
transactions, private staging, integrity checks, replicated checkpoint metadata,
retained snapshots, explicit compression/rebuild primitives, Windows callback and
application slices, bounded GPU workers and independent CPU result validation.

| Area | Evidence available now | Remaining gap |
| --- | --- | --- |
| Scalable storage | The opt-in paged store has writes, deletion, paginated lookup, resume and streaming new-image rebuild. A million populated object records were tested. | The ordinary native namespace still uses a flat model: 256 nodes, 255 extents and a 64-MiB image bound. Paged objects are not yet million-file Explorer support. |
| Local qualification | [103 selected Windows CPU groups](evidence/automatic-group-maintenance-2026-11-15.json) passed in raw Release and AddressSanitizer, including managed paged recovery and automatic grouping. The million-object run remains a separate earlier experiment. | This is not the full CTest/optional-codec suite, live host-telemetry/background qualification, mounted Windows qualification or physical power-loss evidence. |
| Sharing | Exact extent sharing/private updates, protected shared-base patch groups, and automatic aligned-family discovery/extension with complete-size admission. | The controller uses the bounded native model; no persistent scalable grouping, learned model, useful AMD collective path or representative size/access comparison. |
| GPU | Actual AMD execution and independent current/prior CPU recovery passed controlled service tests. | The last matched case was slower on GPU: CPU median 24.2311 ms, GPU 513.918 ms, identical complete image sizes. Useful automatic ordinary I/O remains absent. |
| Learning/background work | Persistent advice/provenance, automatic grouping, live Windows helper observations/resource background mode, measured complete-quantum pacing and outcome reuse exist. | The helper is unmounted; no complete autonomous learner, persistent scalable coherence, mounted executor, gradual migration or lifetime write-amplification governor. |
| Windows product | Callback/disposable mount evidence and an unmounted paged helper/tray with automatic idle grouping exist. | Connect the paged/maintenance path to the Explorer-facing WinFsp factory and lifecycle; complete client semantics, scalable access, supported providers, packaging, secure updates and broad qualification. |
| Recovery | COW checkpoints, metadata replicas, modeled failures, explicit recovery and source-preserving rebuilding exist. | General payload/dependency repair, scalable reclamation and complete backend fault qualification remain open. Historical retirement failures are not diagnosed by later successful runs. |

The million-object experiment used 16,000,000 bytes of tiny raw payloads in a
434,147,328-byte image. It proves a useful index-scaling step, not excellent
compression or filesystem throughput. Reopening read 16 KiB of metadata; the
native name/extent layer has not received that scaling benefit yet.
[Current storage evidence](evidence/local-paged-store-2026-11-15.json),
[hardware comparison](evidence/owned-service-comparison-d384b6c.json),
[shared extents](SHARED_EXTENTS.md).

The critical direction correction is integration: develop one shared engine for
Windows files, memory-backed volumes and eventual physical backends. Avoid a
second reduced-feature demo, more disconnected capability probes, larger flat
constants or more unchanged LZ4 trials as substitutes for the requested product.

## Scope and priority contract

1. **Minimum complete stored size is primary.** Count payloads, shared models,
   dictionaries/bases, indexes, restart points, protection, retained history and
   migration space. Latency and drive wear receive practical consideration;
   they are not equal-weight goals that routinely reject substantial size gains.
2. **Files remain ordinary Windows files.** A group may contain one, two or
   thousands of files. Independently indexed decode units separate group/model
   scope from the amount a small read must recover. Names and permissions do
   not depend on physical grouping.
3. **All intelligence is local.** Final operation needs no online model, content
   upload or remote inference. Local statistics, measured trials and potentially
   locally trained models guide policy; exact decoding never depends on advice.
4. **Everything adapts to the host and workload.** Detect capabilities and fresh
   physical/commit/VRAM, CPU/GPU and storage contention. Learn complete costs and
   access patterns. No owner's hardware, installed paths or experimental cap is
   a universal product default or compression-group ceiling.
5. **Maintenance is actually background.** Correlation, training, regrouping,
   compression, defragmentation, repair and migration use bounded resumable work,
   very low scheduling/I/O priority and fresh admission; promptly yield to user
   activity. An OS priority flag or fixed timer alone is insufficient.
6. **CPU recovery is independent.** GPU and CPU encoders may differ. Every stored
   representation and required immutable dependency needs an available CPU
   decoder. GPU failure selects CPU without driver waits; new GPU attempts need
   qualified recovery evidence and confirmed retirement.
7. **Existing data and host stability remain protected.** The host is the default
   development/research/build/test/GUI/GPU/benchmark machine. No blanket hosted-CI
   or VM-first requirement applies. Keep experiment backing in memory and own
   resource/process lifetimes. Unqualified driver deployment and destabilizing
   tests use isolation. The owner's later 15 November permission allows a
   FlitzFS-related host driver install after exact-artifact stability and rollback
   qualification; GPU and unrelated drivers remain untouched. No current install
   is qualified. Do not format/repartition or risk existing user storage.

## Concrete work and estimated remaining effort

These are low-confidence engineering estimates, not measured agent velocity or
a release date. A person-week means roughly one experienced engineer working
full time, including design, implementation and relevant review/tests. Work
packages overlap the existing harness IDs; they must not double-count those IDs
as additional finished work. Research rows budget an investigation, not a promise
that a record-beating algorithm will be discovered within that time.

| Work | Concrete deliverable | Existing packages | Person-weeks |
| --- | --- | --- | ---: |
| E01 | Connect paged inode/name/extent indexes to the existing native/session APIs, atomic multi-index publication, stable directory cursors and old-format migration. Qualify actual named files with data. | S01 | 6–12 |
| E02 | Persistent allocation/reclamation, reader pins, bounded recovery traversal, protected shared dependencies and source-preserving compaction/handoff. | S01, R01 | 5–10 |
| E03 | Automatic trusted helper/runtime discovery, finite useful AMD work amortization, keyed cost acquisition, actual Windows completion and CPU fallback. | G02, G03, A02 | 4–9 |
| E04 | Persistent paged coherence catalog with versioned content features, incremental scanning, stale-generation rejection and resumable candidate queries. | S01, A04 | 3–7 |
| E05 | Compare fine-grained similarity, shared dictionaries, multi-reference delta and local learned predictor/residual candidates on held-out data; choose by complete size/access results. | A04 | 6–16 |
| E06 | Implement the selected collective representation, independent deterministic CPU decoder, AMD acceleration, indexed random access, mutation/deletion and protected dependency lifecycle. | A04, R01 | 5–12 |
| E07 | Event-driven offline learning/controller, fresh host adaptation, cancellation/yield, persistent progress, negative-result suppression and measured write amplification. | A01, A03, L01 | 4–9 |
| E08 | Complete the promised Windows semantics and use the same engine through Explorer/Win32, helper and tray; resolve completion/cancellation/retirement behavior. | W01, D01 | 5–10 |
| E09 | Multi-release reader contract, frozen compatibility fixtures, mixed representations, resumable COW migration and explicit rollback boundaries. | U01 | 3–6 |
| E10 | Supported volume lifecycle, least-privilege installer/helper, private dependency setup, signed/verified software updates, relocation and uninstall. | W01, U01 | 3–6 |
| E11 | Independent crash/recovery oracle, corruption and malformed-input fuzzing, resource/lifecycle campaigns, current dependency/security review and repair validation. | D01, R01, Q01 | 6–12 |
| E12 | Reproducible recognized-tool filesystem and codec comparisons, held-out real-data evaluation, CPU/GPU and maintenance-interference results. | Q01 | 3–6 |
| E13 | Representative long-running use, failure triage, regressions, stabilization and repeat qualification of the exact candidate. | Q01 | 6–12 |

Total planning envelope: **59–127 person-weeks**, approximately 60–130. For one
experienced engineer this is roughly 14–30 months of full-time work. A small
experienced team with useful parallelism might reach the stable-release gate in
roughly **6–15+ calendar months**, conditional on the compression research and
integration succeeding. Autonomous implementation can shorten coding work; it
does not remove review, hardware coverage, fault testing or soak time. This is
not a promise about this agent's delivery rate.

An integrated development alpha might become assessable in roughly 3–5 months
under that small-team assumption. It would still not satisfy the full release
goal. Superior compression is an open research exit: there is no defensible
calendar bound for beating every existing approach, and failure to beat the
selected baselines must remain visible rather than being relabeled success.
Re-estimate after E01 and the first held-out E05 comparison; those remove the
largest current engineering and algorithm uncertainties.

## Dependency order and acceptance gates

**Gate 1 — shared scalable engine.** Complete E01 first, while defining the E04
catalog and E06 dependency manifest. Route existing native APIs to paged data
without keeping the whole namespace or extent inventory in RAM. Preserve basic
metadata, security, stable IDs, sparse bytes, exact sharing and open-file
lifetime. Avoid O(number of files) path lookup and O(N²) candidate discovery.
Test large, skewed directories, many populated files, a large populated file,
sparse ranges, rename/delete/truncate, restart and bounded resident state.
Use a qualification ladder of 1,000, 16,000, 100,000 and at least 1,000,000 named
files with nonzero contents on an adequately resourced test environment, including
a 100,000-entry directory and populated-file access beyond the resident cache.
These are qualification targets, not product ceilings or allocations every host
must support. Admission scales each experiment to actual available resources;
large-address sparse/model tests are labeled separately from populated data.
Publish exact object/file counts separately. Support explicit legacy migration;
raising 256 or 64-MiB constants is not this gate.

**Gate 2 — one protected collective representation.** E04/E05 must lead to a
real cross-file encoding, not just similarity scores. Start with a verifiable
baseline/dictionary or multi-reference composition, then investigate stronger
local learned residuals where worthwhile. Retain multiple candidates and the
size/access frontier; do not hardcode one codec or tiny group size. Verify
byte-exact recovery, random small reads, editing any group member, deletion of
base-file names, regrouping interruption and all charged dependencies. GPU
encoding may differ from CPU encoding; neither may make CPU reading impossible.

**Gate 3 — useful ordinary Windows/GPU path.** E03/E08 connect trusted resource
and helper discovery to the actual callback/completion path. Charge cold setup,
transfers, verification, publication and retirement. Handle missing/incompatible
GPU, driver/API failure, cancellation, late replies and changing load with
immediate CPU publication and no retry storm. Compare on varied real data;
the old slower fixed case is not a reason either to enable it automatically or
to avoid required GPU development. D01 must have a verified resolution or a
demonstrably corrected lifecycle mechanism covering its failure, not another
unchanged successful trial.

**Gate 4 — autonomous, quiet optimization.** E07 drives E04/E06/E09 from actual
content/workload/resource events. All scan, query, training, encoding, migration
and cleanup steps have bounded memory/work, a persisted continuation and prompt
yield. Test sustained foreground use and changing contention, not only idle
throughput. Measure foreground slowdown, tail latency, RAM/VRAM, CPU/GPU time,
read/write bytes and cancellation/retirement. Calibrate interference limits on
the supported matrix and shrink/defer work when exceeded; no fixed host specs.
Reuse unchanged fingerprints, coalesce journal updates, suppress unchanged
negative trials and avoid rewriting identical representations. Drive wear is
considered through measured write amplification and scheduling, while allowing
expensive background work when meaningful size reduction justifies it.

**Gate 5 — durability, repair and evolution.** E02/E09/E11 define exactly which
writes are acknowledged and which backend flush/failure assumptions apply.
Exercise actual publication and dependency lifecycles with an independent
last-acknowledged-byte oracle: torn/short/reordered writes, interruption at every
barrier, metadata loss, corrupt payload/model/base, interrupted reclamation,
repair and migration, and repeated recovery. Protection/erasure coding must
cover the promised repair domain and count its overhead. Checksums alone cannot
recreate destroyed payloads; no finite campaign proves survival of arbitrary
bugs or loss of every copy. Qualify physical-provider behavior separately before
claiming physical power-loss resilience; memory/VM tests alone do not prove it.

**Gate 6 — Windows and package completeness.** E08/E10 cover the published
capability matrix: Unicode/case rules and long paths; stable IDs; ACLs and
inheritance; sharing/deletion/locks; notifications; timestamps/attributes;
sparse files; streams/links/reparse behavior; cached, noncached, overlapped and
mapped I/O; flush/cancellation and shutdown. No silent success for unsupported
operations. Exercise ordinary applications, restart/update/uninstall and both
backend lifecycles on the current stable Windows 11 AMD64 client. Windows Server
and CPU-only callbacks are complementary evidence, not substitutes. Existing
driver installations are preserved; no owner-host automatic driver replacement.

**Gate 7 — independent performance and release confidence.** E11–E13 apply the
[benchmark protocol](BENCHMARK_PROTOCOL.md), independent source/format review,
fuzz regressions and representative soak. Plan at least 30 days of candidate
soak and a 90-day rolling stability record across supported configurations;
meaningful fixes restart affected lanes rather than erasing failures. Require
no unexplained acknowledged-byte loss, unsafe repair, stranded worker/resource
growth, reproducible severe Windows incompatibility or unresolved critical/high
release-scope security issue. Counts and elapsed time alone do not satisfy these
exits. The owner alone decides whether to release after reviewing the evidence.

## Coherence catalog and offline intelligence

Store a **sparse, paged similarity index**, not a dense all-file matrix, in a
protected internal logical area of FlitzFS. This does not require another host
partition. Associate features with stable content identities/generations and
regions, not filename extensions alone. Combine cheap exact/fine-grained
fingerprints with bounded candidate retrieval; later test learned local
embeddings or multiple reference spaces where their complete benefit warrants
the cost. Whole-file and region relationships can coexist. Persist cursors,
feature/model versions, confidence, measured savings/access costs and failed
trials. Large/hot candidate buckets need resumable traversal rather than a
permanent small cap that silently excludes most files.

The index proposes candidates; it never proves a compression ratio or authorizes
publication. Trials must check recovered source bytes and complete size. Its
loss or old advisory schema can trigger incremental rebuilding without blocking
ordinary reads. Decoder-critical dictionaries/models/bases belong in protected
immutable referenced storage, not the rebuildable advisory catalog.

Two reviewed 2026 primary papers inform the investigation. Sonic studies
fine-grained resemblance and approximate candidate selection, including misses
from fixed super-feature combinations. DCLC studies context-aware local learned
representations, locality caching and multi-reference delta encoding. These are
backup/cloud research results, not reproduced FlitzFS gains or proof of AMD
benefit. An attempted recent CDC HTML paper returned 404 and is excluded.
[Sonic](https://www.mdpi.com/2073-431X/15/9/607),
[DCLC](https://link.springer.com/article/10.1007/s44443-026-01042-5).

## Compatibility and gradual automatic migration

U01 is a required release work package. The policy target is that each release
reads volumes from **at least the preceding three released software versions**,
with longer format support where practical. Until such releases exist, preserve
the supported historical format fixtures and independent CPU readers. This is a
forward engineering contract, not a claim that an updater already exists.

Never reuse a codec/format identifier with different semantics. Persist feature
and minimum-reader requirements, keep frozen old images/decoders in regression
testing, and reject unknown mandatory capabilities before mutation. Keep any
decoder/dependency needed by a live volume or snapshot even when its software
release ages beyond the minimum window. Test raw/optional-codec builds and GPU-
absent recovery; an old data stream must not depend on a newly fetched model.

Installing software and migrating stored data are different operations. The
new software first reads old data correctly. Its background controller can then
migrate independently addressable units into a new version, compare exact CPU
bytes, persist dependencies and atomically publish references. Mixed old/new
units remain readable. Preserve the previous recoverable state until publication
and reader retirement; checkpoint progress and resume after interruption. Apply
the same contention/write-amplification controls as compression. Avoid whole-
volume rewrites just because the application version changed.

Backward compatibility means a newer reader accepts supported older data.
Downgrading old software onto newly written representations is a separate
compatibility question. Publish and test the rollback boundary rather than
promising that old software understands new codecs. Software updates require
verified packages, atomic replacement and recovery of interrupted installation;
filesystem operation remains independent of network access and online models.

## Immediate executable sequence

1. Finish native paged inode/name/extent integration behind the existing engine
   interfaces; add directory pagination and protect shared references. Establish
   authoritative tests on real named-file operations before claiming scale.
2. Implement the reserved, versioned coherence catalog and persistent scan cursor
   alongside protected group/decode-unit manifests. Include bounded cancellation,
   write-grant refusal before publication and no-op update suppression.
3. Produce the first complete cross-file representation and independent CPU
   reader, then couple meaningful finite AMD work to actual Windows operations.
   Compare candidates on held-out real files before committing to one algorithm.
4. Complete adaptive automatic maintenance, scalable reclamation and gradual
   migration; resolve lifetime faults and finish the Windows capability matrix.
5. Run the full correctness/security/fault matrix and recognized benchmarks;
   stabilize and soak the exact candidate. Prepare an owner-reviewable report,
   never an automatic release.

The compact dependency/effort ledger is in [DEVELOPMENT_PLAN.json](DEVELOPMENT_PLAN.json).
Source-bound review receipts are in
[release-plan evidence](evidence/release-plan-review-2026-11-15.json).
