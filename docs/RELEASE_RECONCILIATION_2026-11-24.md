# FlitzFS release reconciliation · 24 November 2026

FlitzFS has a writable Windows filesystem and verified AMD work in bounded
development paths. Three observations will do the most to change the release
outlook: the cost of a populated writable attach, settled bytes after
collective compression, and the first repeatable AMD win delivered through
ordinary Windows I/O. Share the workload, artifact manifest and independent
readback across these lanes. Develop the paths needed to obtain those results
before adding another GPU format or worker protocol.

The latest review starts from `af0aba8556364e643b208ce4694b04456963652f`
and its 1,074 tracked paths. Since the previous retirement review, the
product changes concern fresh AMD memory observations and separate ready
dispatch admission. The fixture now has a matched supervisor timing control.
This round also changes which existing protected prefix candidates automatic
compression can consider. Those candidates use the existing prefix format
and CPU decoder.
[Source and protected-size evidence](evidence/windows-protected-prefix-banks-2026-11-24.json) ·
[AMD timing control](evidence/windows-amd-group-supervisor-comparison-2026-11-24.json)

The follow-up reviews the **1,070-path candidate tree**
`68b73e14181242d113e5b2e7ce4346c2b3f89f3d` over `7efa78e`.
It reconciles every changed path after the first review with the current gate
ledger. The populated trace now gives a common operation witness, and
inactive-range continuation settles the prepublished sparse case. Packed
physical settlement and autonomous image selection are the remaining
reclamation priorities.
[Follow-up source, correctness and research receipt](evidence/windows-mounted-range-retirement-2026-11-24.json)

The initial reviewed source is
`c32348b7efe5aaa789965ee669189840f3f6f5b3`, tree
`2de01631e9021df3006b85f18d80710ed52ae473`. This review inventories all
1,065 tracked paths and reconciles the product lanes against the previous
1,034-path review. It checks the 23 intervening commits, live entry points and
published evidence. Historical trials retain their original source and
execution scope. [Review and research receipt](evidence/release-reconciliation-2026-11-24.json)
· [Previous source review](architecture/REVIEW.md)
· [Current diagrams](architecture/CURRENT.md)
· [Target design](architecture/TARGET.md)

## Product and release gates

| Gate | Current implementation and evidence | Remaining acceptance evidence |
| --- | --- | --- |
| S1 · Exact artifacts | The Windows builds can bind approved WinFsp and AMD package artifacts. Group-worker callback, codec-free, ASan and ROCm link checks identify their exact source and binaries. | Freeze a single package/driver/runtime, CPU decoder registry, OS update, backing and test manifest. Verify the same artifacts in clean install, recovery and benchmark workflows. [Worker build and test receipt](evidence/windows-prepared-group-worker-2026-11-24.json) |
| S2 · Native scale | Paged inode, directory and extent authority exists. At 16,384 short files, coalescing lowered checked backing calls from 350,772 to 186,089 while checking the same 76,462,421 bytes. The 24 November indexed changes stream more of the checked namespace work. | Separate mount cost from background validation under an exact authority contract. Fit and cross-check separate scale curves for attach, lookup, listing, working set and mutation. [Read coalescing](evidence/indexed-record-read-coalescing-2026-11-23.json) · [Indexed validator](../src/native_indexed_volume.cpp) |
| S3 · Allocation and reuse | The 1,027-file trace recovered and accepted further writes. Scoped rebuild reconciliation cut backing reads from 7,990 to 3,281. After preselecting a compact image, sparse backing settled while mounted and packed retirement left fewer detached steps. | Select and settle a compact image of the same population through the ordinary mounted owner. Attribute packed physical release and prove a stable occupied size across sustained churn, retained readers and repair. [Rebuild](evidence/windows-staged-reclamation-reads-2026-11-24.json) · [Populated retirement](evidence/windows-mounted-range-retirement-2026-11-24.json) |
| S4 · AMD and independent CPU continuity | Controlled device writes recovered on CPU. Three paired 1 MiB writes selected CPU. The SDK callback fixture consumed verified plain LZ4, packed XOR and mixed sparse XOR/modular frames. Matched mixed exchanges took 969–985 ms under the verified supervisor and 4,984–5,391 ms with the extra fixture CPU cap. The current prepared packed and mixed paths both returned exact frames. | Demonstrate an end-to-end benefit with automatic ordinary delivery for each advertised AMD role. Preserve immediate CPU service during contention, faults, cancellation and unsupported runtime. [Paired writes](evidence/windows-owner-amd-write-pairs-2026-11-23.json) · [Matched read exchanges](evidence/windows-amd-group-supervisor-comparison-2026-11-24.json) |
| S5 · Complete compression | Automatic prefixes released a 131,072-byte mirrored allocation pair in a targeted fixture. The new trial had no incremental effect on the two public cohorts. Existing grouping on complete CPython revisions saved 573,440 bytes including the unchanged selector. On AIT A–P, grouping saved zero bytes under writable mirroring. A separate conversion to sealed parity reduced the image from 51,486,720 to 32,440,320 bytes with the same encoded objects. Both dense CPU/AMD arms occupied 1,982,464 bytes after compaction. | Run strong independent and collective controls on held-out families with identical protection in each codec pair, then qualify placement and failure tolerance in a separate comparison. [Protected-size pair](evidence/windows-protected-prefix-banks-2026-11-24.json) · [Sealed images](evidence/sealed-indexed-images-2026-11-22.json) · [Dense pair](evidence/windows-collective-dense-readback-2026-11-24.json) |
| S6 · Autonomous maintenance | The sparse paged catalog, workload samples, yield ranking, bounded continuations, resource admission and checked checkpoint advice exist. The dense CPU read changes cut median full-path time by about 14% in four paired private native-session runs on one fixture. | Measure adaptation profit and low interference on ordinary reactive mixed workloads, establish restart and stale-advice behavior, and coordinate across volumes and media. Treat the CPU timing as one useful control for future grouped AMD trials. [Optimizer source](../src/native_maintenance.cpp) · [Dense CPU readback](evidence/windows-collective-dense-readback-2026-11-24.json) |
| S7 · Windows experience | The helper and WinFsp controller attach a memory volume or a supported file-backed packed/sparse volume. Guest workloads exercise actual callbacks, a held reader, clean reattach and additional writes. | Run Explorer and independent Win32 applications through the signed current Windows 11 x64 installation, including tray lifecycle, file semantics, recovery and clean teardown. [Volume factory](../src/windows/volume_app_winfsp.cpp) · [Mounted Explorer fixture](evidence/windows-packed-explorer-2026-11-23.json) |
| S8 · Durable data and repair | The inner store flushes data before redundant root publication. Mounted guest interruption cases cover inner publications, outer selector changes and mixed mapped/cached operations. CPU readback and protected-peer repair cover specific fault cases. | Define and qualify backend-specific acknowledged-operation and repair boundaries with independent witnesses, persistent media and real interruption. Recovery must explain missing copies and refuse an unsupported claim to the newest data. [Root publication](../src/paged_store.cpp) · [Mapped interruption](evidence/windows11-mounted-mapped-publication-cuts-2026-11-22.json) · [Mixed flush](evidence/windows-mounted-mixed-flush-recovery-2026-11-23.json) |
| S9 · Format evolution | The native CPU reader, explicit format/codec checks and v1/v2 grouped read protocols provide pieces of a compatibility boundary. Worker protocol versions are independent of the on-volume reader contract. | Freeze a multi-release format support matrix and golden fixtures. Test read-old/write-current, mixed methods, old-writer refusal and recoverable incremental update. [Format contract](OBJECT_STORE_FORMAT.md) · [Mixed groups](evidence/windows-mixed-group-reads-2026-11-24.json) |
| S10 · Install and recover | Build, dependency staging and signed WinFsp guest fixtures exist. Windows SDK and helper tests can validate ownership without modifying installed graphics drivers. | Qualify the supported signed workflow, offline recovery, optional scoped dependency setup, missing optional components, upgrades and uninstall. Cybersecurity review remains deferred at the owner's direction and has an independent release status. [Windows architecture](WINDOWS_ARCHITECTURE.md) · [Release packaging package E10](IMPLEMENTATION_PLAN.md#13-e10--safe-dependency-setup-packaging-and-supported-volume-workflows) |
| S11 · Comparisons | AITDCC and Windows performance tooling are staged, with native-session, codec and occupied-image pilots. The Windows-native-compressed NTFS procedure is specified. | Execute the matched plain NTFS, built-in compressed NTFS and FlitzFS CPU/AMD comparison with real-host timings, exact byte witnesses and separately labeled complete space accounting. [Benchmark protocol](BENCHMARK_PROTOCOL.md) |
| S12 · Stabilization | Current receipts identify bounded development checks and their gaps. | Resolve material regressions, finish the full exact-candidate matrix and complete the rolling and frozen-candidate soak. Deliver the evidence and limitations for the owner's decision. [E13](IMPLEMENTATION_PLAN.md#16-e13--stabilization-and-the-owners-release-decision) |

## Research and resulting design decisions

- **Group as broadly as useful and read in independently indexed units.**
  RubikFS shows that chunk similarity and access hotness can inform layout in
  a read-only filesystem. Use that observation to rank candidates, then
  revalidate versions at publication and test write churn and reclamation in
  FlitzFS. A separate 2026 CDC study separates base boundary selection from
  window and size normalization. Compare an aligned baseline, shifted
  matching and one normalized CDC configuration at equivalent average chunk
  size. Measure both discovery cost and accepted protected bytes.
  [FAST ’26 RubikFS](https://www.usenix.org/conference/fast26/presentation/huang)
  · [October 2026 CDC preprint](https://arxiv.org/abs/2610.13704)
- **Use learned ranking first and demand strong evidence for learned
  decoding.** ZipLLM finds useful sparse, family-specific relationships in
  model tensors. A November lossless-compression preprint distinguishes
  removable GPU execution costs from sequential model prediction per token.
  Use type and entropy features to choose an encoding trial. Limit trained
  encode/decode experiments to bounded restart frames with an exact,
  portable CPU decoder and charge the required model bytes. The reported
  source results are specific to their workloads.
  [NSDI ’26 ZipLLM](https://www.usenix.org/conference/nsdi26/presentation/wang-zirui)
  · [November 2026 lossless-compression preprint](https://arxiv.org/abs/2611.05871)
- **Keep HIP ready while a verified workload can pay for it.** AMD's HIP
  documentation marks stream priority disabled. DXGI memory telemetry describes
  process usage and assigned budget. Use live adapter/driver identity, engine
  pressure, available memory, active demand and recent useful completions as
  separate inputs. Close a warm window when they no longer support the next
  exchange. In FlitzZip, the production small-file path batches several
  independent inputs in one HIP submission and verifies exact offsets and
  fallback. Reuse that submission and validation pattern where it fits.
  FlitzFS adds mutable extent ownership, authoritative publication and
  independently readable shared dependencies.
  [HIP 10.2 stream management](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/doxygen/html/group___stream.html)
  · [Microsoft DXGI budget contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo)
  · [FlitzZip batching](https://github.com/strmt7/FlitzZip/blob/11396b5123505a4a1959173e1db2d0858c10dd6d/docs/small-file-gpu-batching.md)
- **Verify the complete durability chain.** Microsoft documents the Win32
  flush contract at the file handle. The ATC study of Open CAS demonstrates
  failures in combinations of a filesystem and a persistent cache. Use the
  study as a reason to test the whole selected backend and its interruption
  model. Keep client acknowledgements in an independent witness and reopen
  using a separate CPU reader after each cut.
  [Windows flush API](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
  · [ATC ’25 cache-layer study](https://www.usenix.org/conference/atc25/presentation/duan-shaohua)
- **Freeze the comparison tools and the platform.** Microsoft lists Windows
  11 26H2 as build 26300 in the General Availability Channel. Capture the
  exact serviced revision and channel in every future qualification. Stage
  WinFsp 2.2 with its release identity. Use its named semantic suites for
  correctness. DiskSpd 2.3 changes affinity and buffer layout defaults, and
  offers NTFS-specific bypass options. Freeze a shared file I/O profile and
  query the actual compressed mode with the Windows filesystem control.
  [Windows release health](https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information)
  · [WinFsp 2.2](https://github.com/winfsp/winfsp/releases/tag/v2.2)
  · [WinFsp testing](https://github.com/winfsp/winfsp/blob/master/doc/WinFsp-Testing.asciidoc)
  · [DiskSpd 2.3](https://github.com/microsoft/diskspd/releases/tag/v2.3)
  · [Built-in NTFS compression](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_compression)
- **Keep a recognized corpus and a versioned filesystem workload.** AITDCC
  2026 publishes original bytes and hashes for 16 files, with a distinct
  A–H training split and I–P test split. Preserve the splits and the required
  attribution. Use the AIT files to check content diversity and codec
  baselines. Earlier FlitzFS trials already used A–P. Reserve a new validation
  set split by entire source families, using independently pinned file versions
  and Windows file operations for collective placement, lookup and rewriting.
  [AITDCC 2026 rules and paper reference](https://aitdcc.github.io/)
  · [Canonical corpus and reuse terms](https://aitdcc.github.io/dataset.html)

### Recent research applied to the next experiments

- **Package and runtime identity govern the AMD offer.** AMD publishes Core
  SDK 10.1.0 with a Windows tarball path and documents the HIP runtime as a
  component of the SDK. AMD lists HIP 7.17.0 in ROCm Core SDK 10.2.0. A
  Windows route still needs the exact supported package, resident runtime,
  headers, device code, adapter and existing driver verified together.
  AMD's HIP reference describes stream priorities as disabled. Qualify idle
  interference through observed scheduling, short submissions and yielding
  between them. A finite prepared window can hide setup when live demand and
  resource measurements fund it.
  [ROCm Core SDK components](https://rocm.docs.amd.com/en/docs-10.2.0/components/core.html) ·
  [HIP stream management](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/reference/hip_runtime_api/modules/stream_management.html)
- **Test search methods by the protected space they save.** Sonic studies
  low-similarity chunks that faster detectors can miss. Add a bounded,
  shift-sensitive feature arm beside existing sketch bands, exact sharing and
  the small exhaustive reference oracle. ZipLLM finds useful
  family-specific and tensor-level similarity among model versions. Use
  pinned weights as a held-out cohort for reversible grouping, and require
  identical file bytes, including container headers and padding. Any offline
  learned ranker must earn its catalog space and search time on held-out
  families. [Sonic](https://www.mdpi.com/2073-431X/15/9/607) ·
  [ZipLLM](https://www.usenix.org/conference/nsdi26/presentation/wang-zirui)
- **Measure a protected allocation boundary before broader training.** The
  pinned Zstandard prefix API references bytes for one frame. The optimizer
  can reuse its checked base and unchanged decoder to try a small reduction
  at an allocation boundary. The 2026 XLL paper coordinates reuse and
  logging in distributed storage. Test generation-bound reference and
  allocation updates together under FlitzFS's local failure model.
  [Zstandard 1.5.7 headers](https://github.com/facebook/zstd/blob/v1.5.7/lib/zstd.h) ·
  [XLL](https://www.usenix.org/conference/nsdi26/presentation/shawger)
- **Keep recognized workloads and size accounting complementary.**
  DiskSpd covers file I/O rate and latency under common profiles.
  SPECstorage Solution 2020 publishes distinct software-build, AI image,
  genomics, video-acquisition and EDA workloads. Use a licensed workload
  result only when the run meets SPEC's rules, and keep per-workload results.
  Use pinned real versioned files for compression savings. For NTFS, apply
  the ordinary native directory setting, query file compression after writing,
  and pair exact content with actual occupied size.
  [Microsoft DiskSpd](https://microsoft.github.io/VirtualClient/docs/workloads/diskspd/) ·
  [SPECstorage](https://www.spec.org/storage2020/) ·
  [Windows NTFS compression state](https://learn.microsoft.com/en-us/windows/win32/fileio/compression-state) ·
  [Microsoft compact](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/compact)

## Prioritized implementation

### Immediate work after the retirement batch

The earlier and revised observers share 6,293 Win32 records, three helper
lifecycles and exact independent CPU readback of 1,027 files. Detached
preparation copied and verified the candidate in 8,489 steps before each
mounted observation. Sparse release advanced from 64 MiB in the earlier
window to complete old-range retirement in the revised window. Packed
backing stayed at its starting allocation in both windows. It had 323
detached retirement steps left instead of 377. Both packed runs settled to
6,160,384 bytes, and both sparse runs to 5,898,240 bytes. These are guest
correctness and allocation observations. Performance comparisons use the
real host. [Paired observations](evidence/windows-mounted-range-retirement-2026-11-24.json)

| Order | Implementation | Exit evidence and next decision |
| --- | --- | --- |
| 1 · E02 | Extend the ordinary controller observer from copy through physical settlement, without preselecting the image. Record elapsed and backing work per phase, durable generation and pins, encoded live bytes, provider slots, group high-water mark, safe tail bytes and actual allocation. Use the same transcript with cold work withheld as a correctness control. | Attribute remaining occupied bytes to image preparation, selector admission, reader retention or packed placement. Keep paging and release budgets while changing the measured dominant phase. |
| 2 · E02 and E11 | For the private copy path, measure verification, copy, index construction and published verification separately. Try coalesced ordered-record reads and bounded page batches with per-item generation checks. For packed backing, compare packing away from the boundary with bounded live-slot relocation using both allocation anchors. | Exact CPU verification and unchanged acknowledged bytes across each cut. Improve full mounted settlement, with stable or decreasing occupied bytes on repeated churn and favorable provider bytes written per byte freed. Retain staged rebuild for unresolved ownership. |
| 3 · E03 | Use the matched CPU rate for CPU, cold AMD and prepared AMD on identical protected groups. Vary useful batch size and real lead while retaining independent service. Separately compare complete write and collective work. Record preparation, transfer, compute, validation, delivery and retirement, including CPU bytes served during preparation. | Select positive complete-cost regions separately for reads, ingestion and deep compression. Keep immediate CPU service on refusal, failure, contention or supersession and prove readiness recovery before another device offer. |
| 4 · E01, E04–E07 | Use the same client witness for generation-bound attach in shadow mode and for the [settled-size implementation sequence](#first-settled-size-implementation-sequence). Charge mirrors, dictionaries, catalog, root history, temporary reservations and provider allocation. Compare paged search against exact small-set recall and validate held-out count tiers and file families. | A measured scaling curve and a Pareto frontier of settled bytes, access and rewrite costs determine the next storage format and offline learner. Advance state or dependency depth after repeatable complete-volume evidence. |
| 5 · E08–E13 | Carry the resulting reader and writer into exact signed Windows 11 packaging, Explorer/client compatibility, historical format access, operator-free migration and backing recovery. Preserve the independent CPU reader and acknowledge only qualified barriers. Run controlled plain/native NTFS and FlitzFS comparisons on installed Windows with the frozen artifacts. | Each advertised feature and backing has signed-artifact, correctness, failure, interference and comparison evidence. Close S1–S12 before sending the release decision to the owner. |

The storage, AMD and compression lanes may share the transcript and proceed
independently. A test must answer a release-gate question or distinguish
the next implementation choice. Use operation and backing counters for fast
iteration. Reserve the broad fault matrix and comparative benchmark runs
for material changes and frozen candidates.

For packed allocation work, the design keeps four distinct transition
records. **Prepared** binds source generation and verified target records.
**Selected** records both durable selector peers. **Reclaimable** binds the
inactive range to its current allocation incarnation and empty pin set.
**Released** records the provider's physical extent and both allocation
anchors. A crash or ambiguous result returns to independent CPU validation
at the last durable authority. Tag candidate pages and delayed work with their
generation, and compare again before installing an optimized action. Refuse a
release with an unresolved owner. [Target reclamation design](architecture/TARGET.md#3-make-reopen-and-reclamation-scale-incrementally) ·
[Packed provider](../src/dense_volume_backing.cpp) ·
[Range owner](../src/native_volume_file.cpp)

### 1. Extend one mounted trace and one complete accounting record

**Shared harness.** The external Win32 witness and versioned transcript are
implemented for the populated guest. Extend the same workflow to matched
maintenance, fault, scaling and acceleration arms. Reuse the current modeled
and mounted publication hooks. Drive create/open, overlapped
and cached writes, mapping, explicit barriers, rename-overwrite, held readers,
delete/regrow, listing, range read and final independent reopen. Record the
client-observed return and barrier, driver/helper/package identities, selector
and inner generation, checked bytes and calls, route reason, pin count, file
tree digest, and all occupancy and write totals. Preserve failure rows and
resumed/cancelled work.

1. Run matched packed and sparse correctness arms in the qualified disposable
   guest, with normal maintenance and with it withheld for the control.
   Establish a populated baseline before changing write authority, and use
   the same input when comparing AMD, compression and scale work.
2. Demonstrate that each logical operation and each flush has a unique client
   record. Check affected files, neighboring data, namespace and metadata
   with the independent CPU reader after stopping the owner. Assert zero
   remaining child handles/processes and release of fixture ownership.
3. Connect the same client and record format to the live memory-backed
   workflow once the artifact and backing are qualified. Keep algorithm
   timings on the real host. Launch exact work only after live headroom and
   ownership are observed, and retain refused runs as admission evidence.

**Exit.** Source, installed artifacts, operation sequence, whole-image bytes
and CPU readback can be compared between versions without test-internal
shortcuts. The record is the shared input for E01, E02, E03, E08, E11 and E12.
[Current mounted client](../tests/windows_volume_app_tests.cpp)
· [Integrated trace](RELEASE_EXECUTION_PLAN.md#next-integrated-qualification)

### 2. Make startup and allocation cost scale with work

In E01/E02, the first prototype computes an **advisory attach decision in
shadow mode** beside the existing inventory validator. Bind it to the checked
dual roots, exact selector/lease, image identity, allocator incarnation and
coverage of touched pages and dependency records. Add counters at the checked
page/name, extent, payload, selector and allocator boundaries. A reused digest
or scrub result can prioritize work. The winning root, ownership and
allocation proof still come from the current authoritative path.

1. Quantify full attach inventory cost separately from point lookup, first
   listing, resumed listing and long-directory churn. Use geometric file
   counts funded by discovered headroom. Request distributions cover one
   file, many short files, fan-out extremes, sparse large files and held
   versions. Fit curves to measured live pages, checked bytes and touched
   extents, then validate on an excluded tier and a different layout. Report
   assumptions, prediction intervals and extrapolation distance for tens of
   millions of files.
2. Compare shadow decisions through both valid and damaged peers, unknown
   allocator state, interrupted barriers, older images, changed backing and
   off-path corruption. Find the first diverging decision before enabling a
   fast writable mode. The fast mode requires authoritative evidence of
   ranges it can modify and of files it will expose. Ambiguous ownership,
   old format or unresolved overlap retain the full audit or recovery mode.
3. Track each retired allocation and every remaining pin through checkpoint
   selection, repair, publication acknowledgement and reuse. Run repeated
   overwrite/delete/regrow at a constant live set, including near-full cases
   and held readers. Require flat or decreasing settled occupancy for the
   same live data and protection policy over the verified span.

**Decision.** Advance the checked attach only where it changes the observed
dominant cost and the recovery differential agrees. Design the allocator
handoff with the same generation witness. [E01](IMPLEMENTATION_PLAN.md#4-e01--scalable-native-inode-name-and-extent-storage)
· [E02](IMPLEMENTATION_PLAN.md#5-e02--allocation-dependency-ownership-and-automatic-reclamation)

### 3. Find which AMD work actually wins

The matched mixed fixture attributes most of the old timing to HIP kernel
attribute queries under the extra CPU cap. With only the verified
supervisor, those queries still took about 805–823 ms. GPU dispatch and
synchronization took less than 0.2 ms in these four tiny inputs. The next
end-to-end comparison uses GPU disabled, cold launch and qualified preparation
under the same cap, with useful bytes and lead drawn from the real workload.
[Matched exchange and phase data](evidence/windows-amd-group-supervisor-comparison-2026-11-24.json) Count
CPU-produced foreground bytes before GPU readiness, accepted GPU bytes,
forecast coverage, later use of cached results and missed or cancelled
opportunities. A successful frame that a caller never uses has no delivered
read benefit. Compare with the faster grouped CPU reader now in the tree.

The prepared path now corroborates attributes by distinct function pointer.
Both tables in the current device module share one decoder. The lead
estimator uses unready exchanges for setup and excludes any time spent
waiting after readiness. The SDK tests verify the query count and a
ready-wait regression. Two live preflights observed 99% GPU utilization
and deferred the comparison before launching a worker. Use the matched
supervisor for the next before-and-after comparison.
[Startup admission evidence](evidence/windows-amd-group-startup-admission-2026-11-25.json)

1. Carry the same receipt from enqueue and capture through reserve/setup,
   transfer, kernel, wait, independent verification, child retirement, and
   native delivery or publication. Report complete wall latency, useful
   throughput, CPU occupation, resource deltas and separate stage costs.
   Alternating paired order and a held-out workload distinguish wins from
   idle-state drift and warmup effects.
2. Train a route preference by role, codec, stable workload features,
   backing cost and current resource/identity tuple, with age and coverage
   recorded. If the interval for complete cost straddles the CPU cost, keep
   that shape on CPU. If the accelerator produces materially smaller
   settleable output within the observed interference budget, fund that
   collective shape even when a CPU microbenchmark has lower latency.
3. Use the existing admission, finite bursts and stop signal. Condition
   preparation on a predicted lead with a useful next request and fund its
   complete retirement. Cancel stale work on foreground arrival, workload
   change, pressure or stop. An initialized context and slab can stay ready
   for the admitted next request or a finite batch with useful predicted
   demand. Retire them when demand or headroom expires. Waiting time bounds
   resource ownership. Configuration changes,
   renewed resource headroom or successful qualified work inform
   reacquisition. In-flight CPU operations retain independent service.
4. Install the first positive, reproducible offer in the ordinary controller
   with validation of artifact, adapter and workload identity. Cover the
   missing-runtime and busy-device controls, successful delivery, device
   fault, demand supersession, late reply and subsequent authorized offer.
   Measure one useful read, write and collective workload independently.

**Decision.** Invest first in the costly boundary shown by paired evidence.
A sustained losing shape goes to the CPU catalog. If setup and retirement
dominate, qualify naturally larger, idle collective batches and sequential
read windows before adding another kernel. A different encoding policy for
CPU writes is valid when every published method retains portable independent
CPU readback. [E03](IMPLEMENTATION_PLAN.md#6-e03--useful-automatic-amd-execution-and-cpu-continuity)
· [Warm worker criteria](AMD_RUNTIME_DISCOVERY.md#keeping-the-hip-worker-ready-during-active-work)

### 4. Compete on verified protected bytes, then learn from the result

The next compression change should be selected by a detached replay using
the real indexed allocator. Use current same-size packed/grouped output as
one control, independent high-ratio Zstd and exact sharing as controls, and a
shift-tolerant fragment candidate with directly referenced immutable bases
as the first challenger. Measure the indexed **delta in settled occupancy**
for the same reachable data and reader pins. Include frame headers, base and
dictionary bytes, both protected copies, fixed and incremental metadata,
holes, padding, unreclaimable old bytes and optimizer records. Record peak
temporary storage and writes separately.

1. Build the next candidate cursor on existing paged coherence postings.
   Include the stable content sketch, dataset generation and extent version.
   Bound hot-bucket sampling and candidate residency using the live budget
   and retain continuation to prevent starvation. Compare candidate recall
   and selected marginal bytes with exhaustive candidates on a small
   oracle, then repeat with skew, version churn, dissimilar data and
   previously compressed files. A sparse per-volume index can represent
   millions of extents without a full pairwise matrix.
2. For each candidate cohort, choose bounded restart frames independently
   from the logical group size. Compare ordinary independent frames,
   shared bases and local residual/dictionaries on held-out source
   families. Try a deterministic reversible type transform if the
   matching baseline misses that family. Decode random byte ranges with
   independent CPU code and measure useful GPU work when the device can
   decode a funded batch. Count reference fan-out, bytes decoded per
   requested byte, p95/p99 latency and peak scratch.
3. Rank feasible candidates by complete settled bytes first. Guard
   interactive decode latency and interference with declared empirical
   envelopes derived from the client trace. Gate background write cost
   against the lower bound on net reclaimable savings, considering medium,
   demand, wear exposure and space pressure. A candidate that merely
   shrinks a worker reply can remain a benchmark fixture.
4. Publish useful candidates only after a new generation check of exact
   members, protected dependencies, placement and reader pins. Use current
   COW transaction and selector ownership, verify reconstructed bytes and
   leave old ranges readable until their pins and durable references clear.
   On cancellation or pressure, retain bounded resumable planning advice.
5. Persist small local policy measurements by content class, workload shape,
   representation and medium in a versioned, reconstructible advisory area.
   Compare a simple contextual ranker with the existing yield heuristic
   and a deterministic control. Evaluate incremental gain and false
   promotion before making it active and roll back its choices on drift.
   Profile a small learned byte predictor only after those controls. A
   promoted decoder requires immutable versioned model bytes, fixed
   semantics and complete CPU recovery with external networking absent.

**Decision.** Integrate a new format only for repeatable held-out settled
savings in the declared access and rewrite envelope. A learning experiment
must pay its own observation, model and metadata cost. [E04–E07](IMPLEMENTATION_PLAN.md#7-e04--persistent-sparse-coherence-and-candidate-discovery)
· [Proposed architecture](architecture/TARGET.md#4-optimize-complete-size-with-bounded-random-access)

#### First settled-size implementation sequence

The matched arms use the stronger encoder already available during a
staged indexed copy. Grouping before that copy saved 73,728 settled bytes
on CPython revisions and 106,496 on the held-out curl revisions compared
with the reverse order. It also cut cumulative stage writes by 327,680 and
311,296 bytes respectively. AIT A–P had no useful grouping in either arm.
Its stronger copy wrote 50,339,840 bytes to save 1,155,072. These samples
favor group-first work for observed revision families and combining cold
encoding with an already-funded reclaim copy on weak-gain cohorts. The
volume scheduler still uses live resources and generation checks. Continue
with the shift-tolerant candidate comparison and mounted interference.
[Matched order and write evidence](evidence/windows-collective-cold-order-2026-11-24.json)

| Step | Production boundary and deliverable | Exit evidence |
| --- | --- | --- |
| C1 · Lock inputs and cost axes | Use the indexed rebuild and AIT fixture loader. Freeze whole-file manifests, allocation unit, protection, initial write policy, cache condition, durable generations and reader pins. Record payload, headers, padding, selector, catalog, live and retired backing, and peak temporary bytes. | Compaction and independent CPU reopen agree with the lossless witness. Report writable and sealed-parity sizes in separate series. Reconcile projected image size against occupied backing. |
| C2 · Measure strong independent compression | Carry the measured level-3 and level-9 rebuilds, group-only and both staged-copy orders into mixed binaries, already-compressed media and ordinary client access. Measure complete bytes, backing writes and reads, cold random ranges and foreground delay. Capture live eligibility separately from the size arms. | Use the cohort-specific savings and rewrite cost to decide whether each independent or combined representation belongs in normal maintenance. Test resumable per-object re-encoding where the gain cannot justify a full copy. |
| C3 · Find missed candidates cheaply | Keep exact identity before approximate matching. Add a shift-tolerant fragment challenger beside the current offset sketches. Page sparse postings by sketch lane and band, with immutable extent versions and resumable bucket positions. Rank a bounded shortlist by estimated protected savings and send the survivors to the allocator for exact pricing. | Measure candidate recall and marginal savings against exhaustive pricing on small families. Fit page reads, comparisons and rewrite bytes across independent count and size tiers. Validate on a held-out tier and report projections to tens of millions with workload assumptions and uncertainty. |
| C4 · Refine restartable representations | Stage independent high Zstandard, same-base prefix and shifted copy/residual candidates. Compare different bases as whole proposals. Mix member encodings only when they bind the same reference bytes and pass the complete layout check. Give large families independently addressable frames and a bounded direct dependency set per frame. | Choose the smallest settled feasible image after lossless decode, recoverable references, foreground random access and background interference checks. Attribute any reduction to the contributing representation and placement. |
| C5 · Publish and retire through the same owner | Bind the proposal to checked image and extent generations. Reprice after competing writes, retain new dependencies before publishing descriptors, and use the current checked copy-on-write root. Queue old ranges after durable references and reader pins clear. Keep ranking feedback reconstructible. | Verify the flush witness, both namespace generations, peer repair and occupied-size plateau through cuts at payload, barrier, root, selector and retirement. Read each recovered case with the independent CPU reader. |
| C6 · Enable earned offline learning | Use a bounded contextual ranker over marginal occupancy, shortlist recall, hotness and observed decode, rewrite and GPU costs first. Fit a shadow challenger on volume-local samples. If its held-out error justifies another model, evaluate a small deterministic predictor and account for its persistent bytes. | Promote on unseen families after charging metadata, observation I/O and missed work. The portable CPU reader and offline recovery remain usable for supported reader versions with the model tables persisted alongside their formats. |

Complete the held-out C1 and C2 comparisons before expanding C3 or C4. Use the generation-bound
attach and mounted retirement measurements to decide which private outcomes
can move into ordinary maintenance without adding foreground work. Share
the manifests and counters across AMD, size and provider trials.
[Detailed E04–E07 plan](IMPLEMENTATION_PLAN.md#7-e04--persistent-sparse-coherence-and-candidate-discovery) ·
[Size and publication design](architecture/ADAPTIVE_STORAGE_EXECUTION.md#settled-size-qualification-and-candidate-publication)

### 5. Qualify the volume lifecycle, durability and format evolution together

1. Freeze the supported Windows 11 x64 servicing target, signing/input
   identity, backing geometry and transport. Qualify the helper's current
   memory and supported file-backed flows through the client first. Add
   direct persistent volume provisioning only with a proven ownership,
   rollback and backend path. Discover machine, runtime, budget and backing
   capabilities at runtime. Stage compatible optional dependencies
   transactionally and keep CPU attach and rescue available offline.
2. Establish E11's independent acknowledgement fence for each successful
   flush. Cut before and after data writes, data flush, each root/selector
   write, each flush response and retirement. Repeat with a torn write,
   missing mirror, disk full, stale cursor, WinFsp helper interruption,
   competing reader and a repair in flight. Distinguish bounded model
   injection, guest correctness and isolated physical persistence tests.
   Qualify actual persistent backings and device caches in a disposable
   environment before making power-loss claims.
3. Freeze reader feature identifiers and mixed-format fixtures before
   delivering an update. Deploy the CPU reader first and prove valid
   reopen of older roots and methods. Stage incremental relocations within
   the same transaction/repair contract. Validate interrupted upgrade,
   repeat and rollback, and explicit refusal before writer open when a
   required feature is unknown. Preserve a recoverable old generation
   and defer its reclaim until both the new reader and retention criteria pass.
4. Exercise WinFsp semantic suites, Windows FSX and independent Explorer
   actions with the exact signed package. Verify the tray's create/attach,
   errors, stop and detach with open handles, restart and cleanup with
   multiple mounts. Verify install, update, removal and CPU rescue under
   clean, absent-dependency and conflicting-version states.

**Exit.** Independent repeatable coverage for the advertised Windows, backing
and compatibility matrix. Document outcomes for a lost device, destroyed
replicas or a backend that fails its flush contract. [E08–E11](IMPLEMENTATION_PLAN.md#11-e08--windows-semantics-explorer-and-owned-volume-lifecycle)

### 6. Freeze the data and validate the product

Use the current [benchmark protocol](BENCHMARK_PROTOCOL.md) and the shared
client to confirm matching cache, allocation geometry and durability modes.
Run DiskSpd 2.3 file-target throughput/latency profiles and Windows
file-tree/FSX controls within their respective purposes. Use the same
independently verified file contents for plain NTFS, Windows-native
compressed NTFS and FlitzFS CPU/AMD, including at ingest, after the same
background opportunity and after verified settlement. For the native
baseline, set compression on a disposable directory with ordinary
`compact /C` or the default filesystem control and read back the exact state
of completed files. The directory setting supplies the default for new
files. Record files where native compression is unsupported and retain
them in the corpus. Compare per-file space and complete volume allocation
under the same backing geometry.
[Windows native compression control](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_compression) ·
[Windows file allocation measurement](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getcompressedfilesizew) ·
[DiskSpd latency output](https://github.com/microsoft/diskspd/wiki/Command-line-and-parameters/f65c8d89150f012803d3c24f31d2958edf61cff7)

Collect matched repeated trials, per-shape distributions and real host
pressure, record workload and order, report refusals and saturation, and
show complete volume allocation separately from summed file allocation.
Report advantages only for measured workload and protection conditions.
Start the rolling evidence as soon as the installed lifecycle can support it.
Run the exact-candidate soak after material changes, close failures in the
affected lanes, and send the owner the exact release gate record. [E12–E13](IMPLEMENTATION_PLAN.md#15-e12--recognized-unbiased-size-and-performance-qualification)

## Release distance

All twelve release gates remain open. The package estimate remains
**59–127 person-weeks** of engineering and qualification, with additional
compression research if held-out data fails to show the intended advantage.
This estimate carries forward the prior package breakdown. Evidence from the
new prepared paths isolates one dominant cost, and the protected-size pair
identifies which cohorts favor grouping. Re-estimate once a populated
mounted trace establishes an attach
curve, an automatically delivered AMD shape wins end to end, complete
held-out occupancy is known, and a physical-backend interruption campaign
reconciles the independent witness. Significant parts of the work can proceed
concurrently after the shared trace and contracts exist. Signed platform
coverage and the exact-candidate soak independently affect elapsed time.
[Effort by package](IMPLEMENTATION_PLAN.md#17-completion-dependency-graph-and-effort)
· [Release gate ledger](RELEASE_EXECUTION_PLAN.md#current-gate-ledger)
