# FlitzFS stable release execution plan

Reviewed on 24 November 2026 against the 1,065 tracked files at
`c32348b7efe5aaa789965ee669189840f3f6f5b3` and tree `2de01631e9021df3006b85f18d80710ed52ae473`.
The [current reconciliation](RELEASE_RECONCILIATION_2026-11-24.md) records
the evidence, design decisions and prioritized checkpoints. It includes a
populated retirement follow-up over `7efa78e`.
[Source and research receipt](evidence/release-reconciliation-2026-11-24.json) ·
[Follow-up receipt](evidence/windows-mounted-range-retirement-2026-11-24.json)
Use the [repository overview](REPOSITORY_OVERVIEW.md) for the product snapshot,
the [current architecture](architecture/CURRENT.md) and [target architecture](architecture/TARGET.md)
for system flows, and the [detailed implementation plan](IMPLEMENTATION_PLAN.md)
for the E01–E13 work packages. The [previous plan](https://github.com/strmt7/FlitzFS/blob/1afc654b9ee44952dce82a99a74e474cb5b03fb9/docs/RELEASE_EXECUTION_PLAN.md)
preserves the earlier order and assessment.

## Release assessment

**Development qualification.** The code provides a writable WinFsp filesystem
with a native indexed store, independent CPU recovery and protected compression
dependencies. The latest evidence reduces the cost of checked open and ongoing
scrub, and confirms small mounted recovery sequences. The next release risks
are ordinary AMD admission, the amount of work startup does at scale, the
value of stronger collective compression, and recovery on persistent media.

| Subsystem | Demonstrated | Work that still determines release |
| --- | --- | --- |
| Writable Windows path | Bounded signed WinFsp 2.2 guest runs mounted packed and sparse images, handled overlapping cached and mapped flushes, and recovered acknowledged edits after helper interruption. A fresh helper accepted more writes. | Extend one independent Win32/Explorer trace across populated churn and a current Windows 11 x64 installation, including sharing, cache behavior, packaging and stop/restart. [Mounted flush recovery](evidence/windows-mounted-mixed-flush-recovery-2026-11-23.json) |
| Indexed storage | At 16,384 short files, merging checked reads reduced backing calls from 350,772 to 186,089 with the same 76,462,421 checked bytes and both fences. An independently checked 1,024-file mount recovered and accepted a further write. | Make healthy writable admission grow with the validated decision surface. Retain the full CPU oracle for ambiguous state, old images and recovery. [Record-read evidence](evidence/indexed-record-read-coalescing-2026-11-23.json) |
| Background repair and space reuse | A same-checkpoint scrub compared 677,412,864 and 20,111,360 backing read bytes over 16,384 objects. The live controller repaired a damaged peer before the first file read. Small mounted packed and sparse overwrite/delete/regrow cycles returned to 2 MiB and 1.75 MiB respectively. | Verify off-path corruption, crash/repair interleavings, near-full reuse, retained readers and sustained plateau under a realistic live set. [Scrub checkpoint](evidence/indexed-scrub-checkpoint-2026-11-23.json) · [Repeated settlement](evidence/windows-mounted-repeated-reclamation-2026-11-23.json) |
| AMD execution | A controlled Radeon write committed 16 device extents with 32 checked protected copies. All three paired 1 MiB public-file write trials selected CPU. A reopened SDK path verified two device-read frames. Six separate transfer trials had near-equal warmed times across modes. | Connect qualified observations to the ordinary controller. Win separately on compression, decompression and collective search where matched full-path measurements justify it, then show consumed output and CPU continuity through the same supported mount. [Native writes](evidence/windows-amd-native-file-policy-2026-11-23.json) · [Reopened reads](evidence/amd-reopened-paired-sealed-read-2026-11-23.json) · [Transfer comparison](evidence/windows-amd-stream-copy-2026-11-23.json) |
| Compression and learning | Existing grouping saved 573,440 bytes across 24 complete CPython revision files under writable mirroring, with the selector included. On AIT A–P, the mirror used 51,486,720 image bytes with or without grouping. Converting the same encoded AIT objects to sealed parity occupied 32,440,320 image bytes. | Compare existing grouping and a strong independent CPU rebuild at matched protection, then test a fragment shortlist against an exact oracle. Charge dependencies, temporary space, realistic access and writes. [Protected-size results](evidence/windows-protected-prefix-banks-2026-11-24.json) · [Sealed placement](evidence/sealed-indexed-images-2026-11-22.json) |
| Durability and evolution | Both root and selector publication have modeled and mounted interruption cases. The independent CPU reader and replica checks have verified recovery in those cases. | Establish per-backing flush behavior, physical fault boundaries, repair from surviving sources, historical reader fixtures and resumable migration. [Mounted publication cuts](evidence/windows11-mounted-mapped-publication-cuts-2026-11-22.json) |
| Comparative evidence | Recognized benchmark tooling and official corpus inputs have been staged. Measured pilots identify individual storage and accelerator costs. | Compare the same installed candidate with plain and Windows-native-compressed NTFS on the real host, using verified writes/reads, occupation and access distributions. [Benchmark protocol](BENCHMARK_PROTOCOL.md) |

### Remaining engineering effort

| Work | Packages | Person-weeks | First evidence that can narrow the estimate |
| --- | --- | ---: | --- |
| Scalable storage and reclamation | E01–E02 | 11–22 | Approved reopen invariant, held-out scale curve and sustained mounted reuse |
| Ordinary automatic AMD operation | E03 | 4–9 | One installed and mounted comparison with accepted device output and CPU continuity |
| Collective compression and offline adaptation | E04–E07 | 18–44 | First held-out complete-size, access and write-cost frontier |
| Windows experience, compatibility and package | E08–E10 | 11–22 | Signed package exercises two providers and a historical upgrade |
| Durability and independent qualification | E11 | 6–12 | First disposable physical-backend interruption campaign |
| Comparative evidence and stabilization | E12–E13 | 9–18 | Repeatable matched comparisons and exact-candidate soak |

**59–127 person-weeks**, approximately **60–130**, remain as a low-confidence
work estimate. The recent work reduced local I/O costs. It leaves the package
ranges intact because it does not change startup's inventory dependence,
ordinary AMD selection or persistent-media qualification. The ranges include
corrective work and verification. Hardware coverage, signed artifact
availability and exact-candidate soak also affect elapsed time. The compression
research has an additional open-ended component until a credible held-out
result sets the direction. Re-estimate where the table identifies new evidence.
[Package dependency and effort table](IMPLEMENTATION_PLAN.md#17-completion-dependency-graph-and-effort)

## Current gate ledger

All twelve gates are open for a stable candidate. Existing results satisfy
particular test cases within a gate. A gate closes when its exit evidence
covers the exact candidate and advertised support matrix.

| Gate | Packages | Exit evidence |
| --- | --- | --- |
| S1 · Candidate identity | E10–E11 | Frozen source, build inputs, signed binaries, package/runtime digests, decoder registry, qualified hardware/OS/backing tuples and reproducible test inventory. |
| S2 · Native scale | E01 | Indexed namespace and writable attach match the reference operation log and CPU readback. Mounted growth curves identify the costs of live pages, touched extents and churn. Held-out feasible tiers support reported projections. |
| S3 · Space reuse | E02 | Repeated mounted churn settles at stable or decreasing occupied bytes for a constant live set and protection policy. Allocator space and pinned generations reconcile after pressure, cancellation and interruption. |
| S4 · AMD and CPU continuity | E03 | Automatic useful AMD work is consumed and verified through mounted read, write and collective paths on declared tuples. Device loss and refusal preserve correct CPU I/O and CPU readability of every stored method. |
| S5 · Complete compression | E04–E06 | Settled protected bytes, including every required dependency, improve held-out results. Independently decoded restart units meet the declared access envelope with truncation and corruption tests. |
| S6 · Adaptive maintenance | E07 | Mounted maintenance measures actual savings and interference, yields promptly and resumes bounded work after restart. Advice loss, workload drift and medium changes preserve the recovery contract. |
| S7 · Windows semantics | E08 | Exact signed artifacts pass an interactive Windows 11 client and Win32 file-operation matrix for every supported backing and advertised feature. |
| S8 · Durability and repair | E02, E11 | Client-confirmed durable operations survive each cut inside the qualified failure model. Verified repair and independent CPU restore reconcile bytes, metadata and namespace. |
| S9 · Compatibility | E09 | Declared historical CPU reads, unsupported-writer refusal, mixed-format access and interrupted upgrade/rollback have reproducible results from frozen fixtures. |
| S10 · Packaging and final review | E10–E11 | Signed installer and recovery package pass clean/offline install, update, refusal and removal. The separate cybersecurity qualification remains deferred by the owner. |
| S11 · Comparison | E12 | Complete alternating live-hardware results include verified plain and Windows-native-compressed NTFS, FlitzFS CPU/AMD and settled runs, and published evidence of size, speed and interference. |
| S12 · Stabilization | E13 | Freeze all candidate inputs, resolve material failures, complete the declared rolling and exact-candidate soak and deliver an owner decision packet. |

## Critical path

1. **Start from the ordinary application.** Use one external client trace for
   startup, a populated mixed operation sequence, foreground AMD, allocation
   and successful flush acknowledgements. The helper, driver and backing
   manifests travel with that trace. Keep the reference native CPU reader in
   a different process. E01, E02, E03 and E08 share this harness.
2. **Prove the writable authority and reuse rules.** Advance E01's checked
   mount and E02's allocator together where their invariants intersect.
   Isolate the acceleration change from any format change. Keep prospective
   incremental decisions in comparison mode until they agree with full
   verification across healthy, interrupted and older images.
3. **Find useful GPU and compression regions in parallel.** E03 starts with
   costs on the ordinary write and read paths and can qualify background
   compute while foreground shapes are still on CPU. E04 and E05 price
   candidate selection and encoding on disposable images before committing
   to a new representation. E06 owns the independent reader and format
   contract. E07 can tune behavior after those costs and invariants are
   observable.
4. **Qualify the delivered product.** E08–E11 carry the same path through a
   signed Windows 11 installation, clean restart, physical and memory
   backings, older fixtures, independent rescue and interruptible upgrades.
   Prepare package and failure fixtures during earlier work. The applicable
   durability evidence must precede exact-candidate performance claims.
5. **Freeze the candidate and make the results reproducible.** E12 runs
   matched real-host comparisons and documents where FlitzFS wins or loses.
   E13 closes outstanding integration failures and repeats affected evidence
   after any artifact change. The final packet goes to the owner.

Open gates stay visible in the ledger. The team records any failed criterion
with the observed scope, first diverging operation and the work package that
owns the correction.

## 1. Candidate and platform ledger

- Record the commit/tree, normalized source digests for any development delta,
  compiler and SDK inputs, codec and runtime component versions, exact
  installer/driver/app/worker hashes, signer and API capability manifests.
  Qualify at the actual artifact boundary. Reuse evidence for unchanged
  inputs only when the measured path and environment still apply.
- Bind actual Windows edition, architecture, build and servicing revision.
  Microsoft's current Windows 11 26H2 table lists 26300.9974 for the November
  B update and 26300.10075 for the November D update. Qualify the supported
  generally available x64 servicing build rather than inferring full client
  coverage from mounted guest build 26100. Record the edition, channel and
  actual client revision on every run. [Windows release information](https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information)
- Bind the exact signed WinFsp SDK, user DLL and driver independently.
  WinFsp 2026 v2.2 is the checked upstream release. Run the selected package
  in a disposable client before any conditional owner installation.
  [WinFsp releases](https://github.com/winfsp/winfsp/releases/tag/v2.2)
- Track ROCm Core SDK distribution, HIP headers, runtime ABI and installed
  driver as separate identities. Use version-specific AMD release notes and
  observed device capabilities for any support claim. Recent AMD notes cover
  the 10.2.0 distribution and its HIP 7.17.0 component. They also report
  that priority streams are disabled in that HIP release. Use host scheduling,
  finite device quanta and foreground feedback to govern background work.
  A successful live run covers the recorded tuple, including the actual DLL
  ABI and digest. Preserve CPU service when no AMD tuple qualifies.
  [ROCm Core SDK 10.2.0 release notes](https://rocm.docs.amd.com/en/docs-10.2.0/about/release-notes.html)
- For a candidate change, invalidate evidence that shares any changed input
  or altered contract. Write the required recheck into the gate record before
  considering the candidate frozen.

## 2. Large indexed volumes and foreground I/O

1. Add request/byte counters by selector, root and allocator page, name and
   inode page, checked reference, stored payload and decoded frame. Attribute
   exclusive owner time separately from wait, page I/O, decode and flush
   time. Record peak working set plus resident queues and cache usage.
2. Drive the same ordinary Win32 operations through clean attach, dirty
   attach, read-only rescue, healthy and unhealthy peers. At minimum cover
   many tiny files, few large sparse and dense files, deep nesting, a hot
   wide directory, extension/truncation, and cross-directory rename.
3. Specify writable admission as an invariant over the selected volume and
   both leases, checkpoint generation, protected page roots, allocator and
   lifetime summaries, mandatory codec capabilities, and both backing
   barriers. A change detected at a barrier invalidates the checked view.
   Retain the existing full audit as the differential oracle and rescue
   path for a format or backend whose fast admission is unqualified.
4. Prototype a versioned, protected generation-bound verification summary.
   Check touched pages, extents and dependencies on use. Continue the
   resumable audit in the background. Keep unverified ranges out of
   allocations, reuse and repair. Test delayed damage, stale/replayed or
   missing summaries, conflicting roots, torn publication, old volumes,
   repair during a foreground write and loss of both peers.
5. Freeze an operation stream before scale tests. Start with the existing
   16,384-file tiers and increase sizes subject to measured headroom. Vary
   population, fan-out, extents per file, churn, pinned generations and
   group count independently. Fit observed page/byte work and resident
   memory, validate against withheld feasible sizes and show confidence
   bounds for projections into tens of millions. Record the single-file
   case and transitions where indexes or cache regimes change.
6. Promote the bounded path after its writable results agree with the
   full oracle under the failure matrix and it eliminates the whole-image
   foreground data walk on healthy attach. Retain separate startup and
   full-background-scrub costs in the result.

The bounded background replica pass now reports a checkpoint observation
after covering every paged object in one exact generation and rechecking
its roots. Saved advisory progress can resume the scan. An attach authority
still needs the native namespace, allocator and provider proofs in this
section. [Bounded replica verification](evidence/indexed-scrub-checkpoint-2026-11-23.json)

Read the [indexed namespace contract](INODE_RECORD_NAMESPACE.md), the
[on-disk store contract](OBJECT_STORE_FORMAT.md), and the
[fenced recheck receipt](evidence/indexed-fenced-stored-recheck-2026-11-22.json).

## 3. Allocation, reclamation and acknowledged data

1. Extend the repeated packed/sparse fixture to a sustained attached client
   with independently checked populated directories, small-file churn, larger
   extents, concurrent handles, mapped writes and a reader pin. Preserve
   per-operation logical bytes, live protected bytes, retired ranges, both
   allocation anchors and actual occupied backing allocation at each stage.
2. Separate retirement, relocation, selector handoff, anchor advancement and
   physical tail release in the trace. Compare the same live data and
   protection after each settle, including backing writes, peak staging and
   free-reserve requirements. Reproduce anomalous growth in the smallest case
   that retains the mounted trigger and same references.
3. Qualify such reclaim only when the selected metadata and all pinned
   generations protect their still-readable ranges. Reconcile every candidate
   extent with current references. Use the existing provider-specific allocator
   and bounded owner steps. Require source-generation revalidation and both
   durable selection barriers before reuse of retired ranges.
4. Make pressure behavior explicit. Reserve enough space for foreground
   publication and repair, stop optional work under narrow headroom, hold old
   generations only while required and report prolonged pins to the helper.
   Yield between quanta. Repeat after a failed flush, failed tail release,
   cancellation, helper interruption and reopen.
5. Test with both the prior generation and the new state through CPU recovery.
   Require a stable allocation plateau for repeated equal-live-size churn,
   no dangling dependency, no unreconciled reservation and a subsequent
   acknowledged write after recovery. Record whether a plateau is imposed by
   quantum rollover, alignment or legitimate deferred retention.

[Backing and media contracts](architecture/ADAPTIVE_STORAGE_EXECUTION.md#4-backing-provider-qualification) ·
[Retirement evidence](evidence/windows-mounted-repeated-reclamation-2026-11-23.json)

## 4. Useful AMD work and independent CPU continuity

1. Keep the CPU path selectable for every stored method and adequate for cold
   reads, maintenance rescue and export. Device-produced compressed bytes may
   use a different encoding or size. The acceptance oracle checks decoded
   bytes and protected dependencies independently before durable selection.
2. Record all costs per role and shape. A write pays for worker launch and
   discovery if needed, inputs copied and sent, setup and allocations, uploads,
   kernel bounds and actual intervals, readback, CPU verification, fallback and
   publication. A cold read pays for fetching compressed data, routing,
   transfers, decode, verification and delivery. A collective job includes
   search, verification, protected metadata, post-commit bytes and host impact.
3. Evaluate real shapes independently of first-use ordering. The latest
   synthetic six-exchange comparison had a much slower first separated
   request; the two warmed pairs differed by under 1%. Retain stream-bound
   copy as a controlled experiment. Calibrate qualifying cost from repeated
   observations separated by first-use state. Use the previous public timeline
   and the new failure stages to investigate long later windows and failures. If transfer cost remains minor, prioritize useful work per
   dispatch and reusable preparation under the existing finite owner.
4. Bind the measured route to exact helper/runtime/module, adapter identity,
   role, transfer mode, input class, full exchange bound and resource margins.
   Decide conservatively with fresh CPU load, physical and commit headroom,
   device segment budgets and foreground/backing pressure. Prefer relevant
   observations generated by the mounted workload. Admit shape probes only
   at a useful idle opportunity and reuse current evidence.
5. Connect the qualified foreground offer to the ordinary helper owner via
   `offer_qualified_foreground_route`. Bound the newly-owned request to
   measured service capacity. Publish once after validation, handle cancellation
   and response loss distinctly, and drive GPU-off, qualifying, busy, driver
   refusal and removal paths from the same external Win32 client. Record
   accepted device extents and read frames, retired resources and CPU-written
   bytes by reason. Apply the same cost/identity discipline to cold reads
   and the existing collective route.
6. Quarantine a failing route using classified evidence. Keep the original
   immutable input until CPU completion and advance the client immediately
   according to the request state. Never wait for another discovery to serve
   CPU. Retry on material adapter/runtime/resource or workload evidence when
   the previous owner has retired. Treat time as a pacing input. Verify the
   absence of duplicate commits, search loops, stale workers and stranded
   leases across unplug, sleep/resume, runtime refusal and device reset in the
   appropriate qualification environment.
7. Require useful consumed work in the candidate, in each advertised role.
   Compare matched repetitions against the best qualifying CPU policy.
   Promote measured regions where throughput or complete stored cost wins
   with acceptable access tails. Expand the device and driver matrix after
   the first ordinary mount passes.

[Worker recovery](GPU_RECOVERY.md) · [Deadline and time attribution](UNIFORM_LZ4_SWEEP_PROFILE.md) ·
[Qualified write entry point](../src/windows/winfsp_controller.cpp)

## 5. Strong collective compression and autonomous learning

1. Freeze the size baseline by protection profile and corpus split. Record
   logical bytes, independent codec payload, actual occupied volume bytes,
   occupied peak and total writes. Count each replica, required decoder
   dependency, dictionary/model, page index, reference, selector, retained
   generation and allocation gap. Measure native placement savings separately
   from encoded payload savings.
2. Use the existing persistent paged catalog and stable extent/content
   revisions. Compare content-defined fingerprints and current projection
   bands with exact sharing and locality. Maintain sparse versioned postings
   with bounded candidate and cache counts per quantum, independent of total
   file count or logical group size. Persist continuation and invalidate stale
   postings. Measure recall against exhaustive search on small sets and
   pressure on large and skewed workloads.
3. Test independent Zstd/LZ4 and raw storage, shared dictionaries, exact
   deduplication, current protected shared bases, and direct residuals against
   one or more independent references. Try small independently decoded
   co-packed frames for short members. Use reversible transforms only where
   their complete cost earns admission. A group may span one file or thousands.
   Give each range a small indexed restart unit and bound fetched/decoded
   bytes and direct dependencies. Protect referenced bases until the last
   live or pinned generation releases them.
4. Use fresh held-out families as well as held-out file versions. Record net
   repaid writes, time to equilibrium, cold p95/p99, decode amplification,
   throughput and regressions by content class. Measure partial reads,
   appends, overwrites and deletes of one member without rewriting its full
   group. Compare stronger encoders and applicable specialized controls on
   cohorts they target. For example, ZipLLM motivates a checkpoint-specific
   adapter study. Its results cover its model-storage workload.
   [ZipLLM, NSDI 2026](https://www.usenix.org/conference/nsdi26/presentation/wang-zirui)
5. Compare a calibrated hand-tuned policy against a compact offline ranker
   and adaptive selection on chronological observations. Use predicted net
   full-volume size first, subject to a tested range-read envelope and
   measured resource and rewrite budgets. Record actually verified savings,
   current resource consumption, read heat, redundancy, age and drift.
   Cold start should work from conservative observed defaults. Schedule small
   comparable trials during genuine headroom and preserve the previous policy
   for rollback. Version and persist samples and progress in the protected
   logical advisory area. Rebuild on loss or version mismatch.
6. Consider a learned entropy codec once simpler candidates are measured.
   Trial small deterministic CPU-decodable models, independent resets and
   trained dictionaries before any format assignment. Charge model storage,
   read amplification, decode time, CPU requirements and update frequency.
   Keep any decoding dependency immutable and protected in committed storage.
   The learner's advisory state and the selected representation's decoding
   dependencies have different lifetimes and recovery requirements.
7. Use a shared per-host budget across mounts for scan, compaction, scrub,
   migration, learning and device work. Use OS CPU/I/O priority and the
   qualified GPU priority, short quanta and cancellation boundaries. Derive
   CPU, memory, commit, GPU, queue and write reservations from current
   capabilities and contention. Give explicit credit for measured space gain
   and debit actual backing traffic. Use the appropriate reservations for RAM,
   SSD/TRIM and HDD seek limits, and removable-media write/flush behavior.
   Verify prompt yield and full ownership retirement.
8. If a candidate fails CPU parity, useful net savings, access cost or
   interruption tests, keep its evidence and leave persisted formats
   undisturbed. Promote the best measured frontier. A score from a single
   benchmark or file class only supports a claim for that workload.

[Collective representation design](SHARED_BASE_GROUPS.md) ·
[Optimization state](OPTIMIZATION_STATE.md) ·
[Adaptive architecture and provider policy](architecture/ADAPTIVE_STORAGE_EXECUTION.md)

## 6. Windows experience, compatibility and installation

- Complete the helper lifecycle as tested product behavior. Trace creation and
  reopen of a volatile volume and a persistent image on a selected supported
  medium, mount target validation, Explorer exposure, normal/elevated client
  access, pause/detach, cancellation, application exit, resume and removal.
  Make CPU versus AMD service, repair progress and capacity visible. Keep
  normal storage decisions automatic. A direct-device provider has a distinct
  ownership and qualification gate from the current image-backed providers.
- Exercise Win32 and Explorer create/open, rename/delete/sharing, enumeration
  and case behavior, cache modes, mapped I/O and flushes, small/large files,
  cancellation and simultaneous handles. Test the intended ACL/basic
  metadata/attribute contract, allocation/sparse ranges, alternate streams and
  reparse behavior where advertised. Record clear error semantics for features
  outside the declared contract. Use real clients, including copy and editors.
- Consolidate distribution around the existing signed WinFsp and helper
  architecture. Discover installed capabilities at runtime. Pin, verify and
  stage compatible missing user-mode dependencies privately with an offline
  path. Verify clean install, existing/newer dependencies, interruption,
  rollback, and removal of only the package's own processes and registration.
  Define conditional driver deployment evidence by exact signed binary.
  Verify CPU operation without the HIP SDK or a compatible GPU.
- Freeze compatible CPU-reader goldens for every supported volume format.
  The proposed support contract is current software reading at least three
  preceding release lines once those releases exist. Document the precise
  reader/writer/capability matrix and keep method IDs permanent. Gate unknown
  required capabilities before mutating a volume.
- Migrate bounded units in low-priority COW operations with resumable progress.
  CPU-decode and independently compare data and metadata before selection.
  Preserve read service for a mix of versions. Use durable fence tests around
  migration publication and resume after a crash or near-full pause.
  Verify compatible rollback while possible and CPU export/recovery from
  advanced formats before declaring the boundary. Test old readers against
  each supported image and unsupported-writer refusal before an old program
  can change it.

[Windows helper](WINDOWS_VOLUME_APP.md) · [Compatibility and migration package](IMPLEMENTATION_PLAN.md#12-e09--multi-release-readers-and-gradual-format-migration) ·
[Package lifecycle](IMPLEMENTATION_PLAN.md#13-e10--safe-dependency-setup-packaging-and-supported-volume-workflows)

## 7. Durability and failure qualification

The required durability contract is defined per provider. A success returned
by a buffered write, a successful end-to-end durability flush, and a published
but undelivered response have different protocol meanings. Record them
separately. [Persistence model](PERSISTENCE_MODEL.md)

1. Give every external test operation a witness ID and log client-observed
   completion independently of the mount owner. Check exact permitted
   generations before and after data write/readback, metadata publication,
   individual root fences, selector publication, allocator release and
   repair publication. Carry overlapping cached and mapped edits, directory
   operations, mixed file sizes and near-full pressure through those cuts.
2. Exercise the deterministic storage model first, then guest process and
   driver-lifecycle cuts, then media faults in a disposable dedicated lab.
   Test flush errors, loss of a peer copy, corrupted metadata and dependencies,
   failed resize, device removal, lost completion and interrupted repair.
   Verify each survivor by independent CPU reader and continued writable
   operation when repair is justified.
3. Capture real persistence evidence by provider, controller, cache/removal
   policy and OS. Validate volatile and persistent providers against their
   intended lifetimes. Cover sudden interruption on eligible NVMe/SSD, HDD and
   removable media before claiming those combinations. Preserve existing host
   data and boot/device state throughout development and qualification.
4. If a peer survives, validate authority and decoding dependencies before
   repair. After loss beyond the declared redundancy domain, fail closed and
   use the independently verified recovery/export workflow. Same-device
   redundancy covers the specified localized failures. Complete device loss
   requires an independent copy. Test loss of the advisory area separately.
5. Preserve Windows process containment, noninteractive launches, matching ASan
   runtime preflight and confirmed worker/mount retirement. Keep raw-only and
   codec-enabled tests, fault-model verifies, mounted traces and media evidence
   distinct. Record all unexpected outcomes and requalify affected gates after
   a fix. The owner deferred the separate cybersecurity workstream.

[Replicated checkpoints](REPLICATED_CHECKPOINTS.md) ·
[Resume and interruption](STORE_RESUME_AND_INTERRUPTION.md)

## 8. Benchmarks, soak and the release decision

1. Pin the benchmark command, executable and corpus. Use Microsoft DiskSpd 2.3
   with file targets and compatible caching/flush options across comparators.
   Pair sequential/random and mixed I/O with the same replayable real file
   tree for copy, enumeration, edit, rename, reopen and recovery. Use the
   official AITDCC 2026 train and test splits and pinned CPython revisions for
   repeatable public workloads. Add independently sourced cohorts for
   incompressible media, application data, tiny files, sparse files and
   versioned trees. Keep standalone lzbench and challenge scores separate from
   matched protected-volume occupancy. [DiskSpd 2.3](https://github.com/microsoft/diskspd/releases/tag/v2.3) ·
   [AITDCC 2026 dataset](https://aitdcc.github.io/dataset.html)
2. Use plain NTFS and Windows' **built-in NTFS file and folder compression**
   as the required baselines. Prepare equivalent disposable targets on the
   same Windows/storage configuration with the same allocation-unit size
   and inspect support. On a fresh empty test directory, use
   `FSCTL_SET_COMPRESSION` with `COMPRESSION_FORMAT_DEFAULT`, or ordinary
   `compact /C`. Verify inherited state on each completed file using
   `FSCTL_GET_COMPRESSION` and require LZNT1 for the compressed arm and
   `COMPRESSION_FORMAT_NONE` for plain. Flush before checking and reverify
   after moves/writes. Exclude CompactOS, executable compression flags,
   archive codecs and alternate NTFS drivers from the NTFS comparator.
   [Microsoft native compression API](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_compression) ·
   [Microsoft query API](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_compression) ·
   [Microsoft compact command](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/compact)
3. Compare FlitzFS CPU-only, automatic AMD, settled collective maintenance,
   and foreground I/O during maintenance with both NTFS arms. Include
   supported ReFS or an appropriate WinFsp control as separately scoped
   evidence of feature or framework cost. Retain an explicit unavailable
   comparator when the required target cannot be safely staged.
4. Alternate treatments with a declared cache state and repeated trials on
   the real host. Report per-phase throughput and latency distributions,
   complete occupied bytes, per-file physical allocation, peak intermediate
   storage, write amplification, CPU/GPU usage, queue pressure and recovery
   checks. Record first use, warm runs, loader costs, refused GPU jobs and
   failed or interrupted samples. Guest results establish correctness.
5. Preregister the access, foreground impact and size criteria on candidate
   workloads before comparative runs. Require material end-to-end savings
   over native-compressed NTFS on the declared representative cohorts with
   all protection charged. Compare against applicable stronger methods and
   independent CPU code paths. Publish honest trade-offs and uncertainty.
   Losing cohorts, unavailable modes or excessive latency remain visible.
6. Freeze the eventual candidate and run the existing 90-day rolling and
   30-day exact-candidate stability windows. Resolve known material data,
   lifecycle, installation and performance failures. Retest changed routes
   and repeat the soak coverage affected by material fixes. Give the owner
   hashes, gate status, verified scope, supported failure model and remaining
   limitations. Only the owner authorizes release.

[Full benchmark protocol](BENCHMARK_PROTOCOL.md) ·
[Stabilization package](IMPLEMENTATION_PLAN.md#16-e13--stabilization-and-the-owners-release-decision)

## Immediate implementation order

| Start order | Work package | First reviewable output | Next dependency |
| --- | --- | --- | --- |
| 1 | Shared E01/E02/E03/E08/E11/E12 integration trace | External Win32 witness, exact artifacts, populated packed/sparse trace, independent CPU readback and a complete occupancy record. | Common comparison and fault harness |
| 2 | E01/E02/E11 checked attach and settled ownership | Full-audit differential with attach phase counters, preserved flush witness and allocator settlement under churn. | Generation-bound prototype and held-out scale curve |
| 2 | E03/E08 ordinary AMD acquisition and CPU continuity | Exact packed and mixed decode trial where admitted, paired complete-cost CPU/AMD timings, and consumed ordinary I/O on a shape that wins. | Measured offer and route outcomes by role |
| 2 | E04/E05/E06 complete-size oracle and research | Replayable pinned cohorts, bounded-query recall, independently decoded restart frames and validation split by source family. | A candidate with demonstrable protected savings and usable range reads |
| 3 | E06/E07/E09 collective storage, policy and upgrade | CPU-first reader, protected dependency publication, bounded migration and a measured offline policy. | Windows client, recovery and comparative qualification |
| 3 | E08–E11 installed Windows experience | Signed artifact passes current Windows 11 installation, interaction and removal, backing lifecycle, compatibility and scoped physical-recovery campaigns. | Freeze executable, driver, format and supported platform inputs |
| 4 | E12/E13 comparisons and stabilization | Matched real-host reports, reliability and soak evidence, and an owner decision packet for the twelve gates. | Owner approval |

The three measurement lanes share the first trace and can progress on
independent fixtures. Favor the largest measured cost or complete storage
saving in each lane before expanding format or worker variants.
[Next implementation steps](RELEASE_RECONCILIATION_2026-11-24.md#prioritized-implementation)

### Next integrated qualification

The populated packed and sparse runs now share a 6,293-record Win32 transcript
with 1,027 live files and independent CPU readback across three helper
lifecycles. The same workload exercises rename replacement, enumeration,
cached and mapped flushes, held reads and delete/regrow. Backing allocation
grew despite a fixed live set, and retirement remained pending. Recovery
and settlement have separate verdicts.
[Trace evidence](evidence/windows-populated-client-trace-2026-11-24.json)
· [Workload and transcript contract](BENCHMARK_PROTOCOL.md#populated-windows-operation-trace)

The staged rebuild now makes 59% fewer backing reads on the controlled
reconciliation fixture. The populated runs also compare mounted observation
with detached reclamation on the resulting images. Both detached owners
published compact images and verified all bytes. Both mounted windows ended
with eligible retirement pending.
[Staged reclamation evidence](evidence/windows-staged-reclamation-reads-2026-11-24.json)

The controller now continues checked inactive-image release within the same
idle window. Paired signed guest runs used the same operation transcript.
The prepublished sparse case retired the old range while mounted. Packed
occupied allocation stayed flat during the mounted observations and
remaining detached steps fell from 377 to 323. The next control must cover
copy, durable selection and packed physical settlement beginning with the
ordinary mounted owner, including a maintenance-withheld arm and independent
write acknowledgements. Measure complete foreground and background cost
on the real host with qualified backing and exact artifacts.
[Retirement evidence](evidence/windows-mounted-range-retirement-2026-11-24.json) ·
[Prioritized follow-up](RELEASE_RECONCILIATION_2026-11-24.md#immediate-work-after-the-retirement-batch)

### First implementation slice

1. **Completed.** Installed uniform write discovery
   accepts cancellation through the existing worker transport. The Windows SDK
   checks cover a pre-cancelled request and the existing CPU ownership path.
   All routes recheck cancellation after final budget sampling.
   [Validation receipt](evidence/release-plan-refresh-2026-11-23.json)
2. **Implemented for synchronous CPU service.** The ordinary controller
   receives complete bounded windows for successful aligned write sizes and
   their foreground policy and backing geometry. The window includes each
   completion time and native service cost. Qualification can use the
   timestamps to estimate active request cadence and check freshness.
   [Write demand and Radeon evidence](evidence/windows-ordinary-amd-write-demand-2026-11-23.json)
3. **Owner-controlled paired replay implemented.** The owner captures
   generation-bound completed writes from distinct files. An admitted trial
   sends the exact same capture through private mirrored CPU and Radeon
   images. Foreground activity cancels the paired opportunity, and valid
   results require independent readback and retired scratch.
   [Grouped capture evidence](evidence/windows-ordinary-amd-capture-groups-2026-11-23.json) ·
   Two scoped paired runs used verified Radeon output, and the foreground
   cancellation controls recovered the ordinary writes independently. CPU was
   faster for the tested 1 MiB shape.
   [Paired owner evidence](evidence/windows-owner-amd-write-pairs-2026-11-23.json)
   Balance order in follow-up trials across multiple funded group sizes and
   held-out workload classes. Separate cold and prepared read paths. Bind
   offers to observed geometry, verified output and confirmed retirement.
   [Grouped Radeon evidence](evidence/windows-amd-grouped-write-replay-2026-11-23.json) ·
   [Benchmark protocol](BENCHMARK_PROTOCOL.md#cold-and-prepared-cpu-and-radeon-trials)
4. Measure useful output with a real mounted request after a positive route
   qualification. Record why other requests use CPU and whether the helper
   resumes with a fresh opportunity after an interruption. A losing shape stays
   on CPU and feeds the batch/codec work in E03, E05 and E06.
5. In the same development sequence, exercise writable admission and scrub
   against the full audit on a new scale tier. The full mirrored-object audit
   now accounts for native name pages during its object walk. Paired fixtures
   at 1,024 and 16,384 files reduced backing reads.
   [Indexed inventory evidence](evidence/indexed-name-page-inventory-2026-11-23.json)
   Verify legacy and damaged images, repairs between observation and attach,
   and interrupted publication. Advance an attach optimization only after the
   differential agrees.

[Detailed implementation tickets](IMPLEMENTATION_PLAN.md#next-implementation-tickets) ·
[Current architecture diagrams](architecture/CURRENT.md) ·
[Physical-host and guest evidence boundaries](BENCHMARK_PROTOCOL.md)
