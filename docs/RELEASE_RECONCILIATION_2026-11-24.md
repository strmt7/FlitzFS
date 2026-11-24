# FlitzFS release reconciliation · 24 November 2026

FlitzFS has a writable Windows filesystem and verified AMD work in bounded
development paths. Three observations will do the most to change the release
outlook: the cost of a populated writable attach, settled bytes after
collective compression, and the first repeatable AMD win delivered through
ordinary Windows I/O. Share the workload, artifact manifest and independent
readback across these lanes. Develop the paths needed to obtain those results
before adding another GPU format or worker protocol.

The reviewed source is
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
| S3 · Allocation and reuse | Packed relocation preserved a held reader and crossed a physical allocation boundary in a 60-revision guest trace. Another finite guest run verified repeated packed and sparse settlement. | Prove stable or decreasing settled allocation under sustained mounted churn, pressure, pinned old generations and repair on each supported backing. [Packed retirement](evidence/windows-mounted-packed-retirement-2026-11-23.json) · [Repeated reclamation](evidence/windows-mounted-repeated-reclamation-2026-11-23.json) |
| S4 · AMD and independent CPU continuity | The controller measures CPU write costs and can run paired replay, including separate incoming sizes in a funded prepared worker. Controlled device-produced native extents recovered on CPU. Three paired 1 MiB writes selected CPU. Plain LZ4 read-ahead has verified real device frames. Packed XOR and mixed sparse XOR/modular read-ahead now have prepared capture and CPU/SDK results. Both grouped-decode live preflights refused admission and made zero launches. | In isolated exact-artifact tests, prove device readback for both grouped formats. Then prove automatic ordinary delivery and a benefit on at least one supported workload per advertised AMD read, write and collective role. Preserve correct and immediate CPU selection during contention, faults, cancellation and unsupported runtime. [Paired writes](evidence/windows-owner-amd-write-pairs-2026-11-23.json) · [LZ4 reads](NATIVE_AMD_LZ4_READ.md) · [Grouped preparation](evidence/windows-prepared-group-worker-2026-11-24.json) |
| S5 · Complete compression | The indexed format has shared bases, packed placements and grouped residuals. The AIT A–P layout trial went from 51,486,720 to 32,440,320 settled bytes with identical encodings and protection. The newer dense Radeon encoder selected 100 rather than 80 outputs and cut reply bytes by 5,120. Both dense arms occupied 1,982,464 bytes after compaction. | Compare marginal settled bytes against strong independent and collective controls on held-out file families, with all dependencies, protection, restart costs and retained readers charged. [Layout trial](evidence/dense-cross-group-2026-11-23.json) · [Dense pair](evidence/windows-collective-dense-readback-2026-11-24.json) |
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

## Prioritized implementation

### 1. Extend one mounted trace and one complete accounting record

**Start here.** Extend the existing WinFsp application test with an external
Win32 operation witness and a versioned, reproducible manifest. Reuse the
current modeled and mounted publication hooks. Drive create/open, overlapped
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

Run the exact staged packed and mixed decoders first in the finite
SDK/native verification path. If admitted, compare the same protected inputs
with GPU disabled, a cold worker and the current one-use preparation. Count
CPU-produced foreground bytes before GPU readiness, accepted GPU bytes,
forecast coverage, later use of cached results and missed or cancelled
opportunities. A successful frame that a caller never uses has no delivered
read benefit. Compare with the faster grouped CPU reader now in the tree.

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
   change, pressure or stop. Waiting time bounds resource ownership and
   cannot by itself qualify another attempt. Configuration changes,
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
of completed files. [Windows native option](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/compact)

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
new prepared paths improves the experiments without resolving their critical
unknowns. Re-estimate once a populated mounted trace establishes an attach
curve, an automatically delivered AMD shape wins end to end, complete
held-out occupancy is known, and a physical-backend interruption campaign
reconciles the independent witness. Significant parts of the work can proceed
concurrently after the shared trace and contracts exist. Signed platform
coverage and the exact-candidate soak independently affect elapsed time.
[Effort by package](IMPLEMENTATION_PLAN.md#17-completion-dependency-graph-and-effort)
· [Release gate ledger](RELEASE_EXECUTION_PLAN.md#current-gate-ledger)
