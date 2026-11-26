# FlitzFS release reconciliation · 26 November 2026

FlitzFS has a writable Windows filesystem engine, a WinFsp helper and a CPU
recovery reader. It has consumed real Radeon results in controlled Windows
callbacks and passed separately signed guest mounts. A stable release still
requires automatic AMD service on the ordinary mounted path, scalable
writable reopen, supported-media recovery and matched Windows comparisons.
Use this review for implementation order. The E01–E13 plan retains the
detailed package contracts and the release execution plan retains the gates.
[Implementation packages](IMPLEMENTATION_PLAN.md#3-execution-order-and-dependencies) ·
[Release gates](RELEASE_EXECUTION_PLAN.md#current-gate-ledger)

The current review starts from the 1,160 tracked paths at
`7cd54c3a21136fff956950fa098dcc260f3430b3` and the mixed-group packing
change frozen in source tree `baa05298743776f3e110acece5c97b1605dcb3fb`.
It carries forward the subsystem source review and rechecks the fifteen
changed implementation and test paths since `51458d2`, covering the
deferred Windows write owner, AMD selection, budget sampling and burst
coordination. The status table and release gates reconcile the accumulated
evidence with the proposed design.
[Current evidence](evidence/windows-packed-shared-subsets-2026-11-26.json) ·
[Source review](architecture/REVIEW.md)

The follow-up AMD checkpoint made priority optional in the Windows workers.
The installed Radeon delivered two verified read extents through callbacks.
After fresh headroom became available, a separate write fixture stored
sixteen verified extents and independently reopened the exact bytes.
Withdrawing a later device grant selected CPU and preserved the exact files.
The matched mounted comparison remains the next AMD release decision.
[Stream capability qualification](HIP_PRIORITY_CAPABILITY.md#26-november-windows-qualification) ·
[Installed write and CPU continuation](evidence/windows-private-seal-credit-2026-11-26.json)

The latest live write owner waits for worker exit, activity and actual
deadlines. A failed source-free preparation releases the slot before its
original deadline. A prepared request still requires independent CPU
validation and a later resource observation before publication. Callback
tests cover readiness, acceptance, pressure, setup refusal and shutdown.
Mounted AMD benefit still needs the external client comparison.
[Owned write scheduling](OWNED_DEFERRED_WRITES.md#event-driven-write-service)

## Repository and release evidence

| Subsystem | Implemented and observed | Remaining release proof |
| --- | --- | --- |
| Windows and volume ownership | The helper and WinFsp callbacks use exclusive memory and file-backed volume ownership. Signed guest clients covered populated operations, cached and mapped flush interruption, reclaim and writable reopen. | Qualify the ordinary signed helper and driver together on current Windows 11. Verify Explorer behavior, lifecycle, sharing and the advertised file semantics. |
| Indexed store and scale | Copy-on-write names, inodes, extent maps and payload ownership are queryable in pages. Read callbacks reuse checked metadata. A 32-callback control read 54,672 backing bytes after the change, down from 341,008. Writable reopen verifies the full inventory, including Windows admission checks. | Put a durable bound on normal attach I/O without skipping namespace, allocation or dependency invariants. Measure named-file growth and memory on a held-out scale tier. |
| Space reuse and recovery | Protected append, paired selectors, staged compaction, reader pins, alternate peer recovery and independently readable sealed parity are implemented. Priced packing and the locked-workspace seal both run through the volume owner. | Qualify continuous settlement under churn and pressure, all relevant publication cuts and each advertised storage medium. Prove the bounds on delay from pinned readers and incomplete work. |
| Compression | Foreground CPU codecs, verified device candidates, protected small-file packing and shared bases exist. Assembled-prefix subset refinement reduced one protected mixed group from 752 to 560 KiB. Packing retained short extents saved another allocation pair in a separate matched fixture. The latest CPython control reached the same occupied bytes across the two binaries. | Move useful pages and segments into their smallest verified form. Compare codecs and groups at equal protection and charge model, dependency, repair and allocation costs. |
| AMD | HIP workers can use a nonblocking stream when stream priority is unavailable. Radeon callbacks delivered verified read and write bytes. The foreground write service wakes on worker exit, adapts burst depth to headroom and can revisit a larger grant. The latest public-source write fixture favored CPU on occupied size, 240 versus 264 KiB. | Establish winning workloads on the ordinary mount, by role. Bind retained preparation to measured demand and account for discovery, readiness, validation, committed size, misses and idle residency. Target predictable grouped reads and collective search before more small uniform-write tuning. |
| Autonomous policy | Persistent sparse candidates, revision guards, input-funded maintenance and live contention/resource gates already exist. | Test drift, restart, fairness, idle recovery and write cost with ordinary workloads. Qualify the full chain from offline advice to independently verified net savings. |
| Evolution and deployment | CPU readers, required features, format checks and pinned setup exist. Format and provider evidence is distributed across historical fixtures. | Freeze the supported compatibility window, run interrupted gradual migration, and qualify private dependency setup, the exact signed installer and offline recovery. |
| Measurement and stabilization | A Win32 operation witness, DiskSpd and named public inputs support repeatable comparisons. | Execute matched current NTFS and FlitzFS runs on live hardware, then close the declared fault, compatibility and soak matrix on one source-bound candidate. |

[Current source flows](architecture/CURRENT.md) ·
[Win32 operation witness](evidence/windows-populated-client-trace-2026-11-24.json) ·
[Read and device evidence](evidence/windows-foreground-query-reuse-2026-11-25.json) ·
[Shared-base comparison](evidence/windows-subset-reference-refinement-2026-11-26.json)

### What changes the next implementation decision

The packed-slice comparison now survives an unrelated extent in a complete
staged group. One matched 4 KiB protection fixture retained 24 short files
and left the unrelated extent's object and revision unchanged. The complete
compacted image moved from **304 to 296 KiB**, or **432 to 424 KiB**
including the selector.
The prior 16-member diagnostic tied at 296 KiB. The CPython control's best
settled arm occupied **440 KiB including its selector** in both binaries.
Pinned, current and independently copied images returned exact CPU bytes.
The result supports an additional representation on an existing verified
path. The largest compression investment still depends on measured
coverage and closed-loop results on real workloads.
[Subset packing and CPython control](evidence/windows-packed-shared-subsets-2026-11-26.json)

The latest Radeon write fixture funded eight prepared requests and
accepted two in the observed opportunity. Three stored extents contributed
192 KiB of independently CPU-verified input. Engine activity deferred the
next pass, which still completed through CPU. The image size gap and
headroom results give two reasons to prioritize grouped reads and
collective search as the next physical AMD comparisons.
This round's collective preflight deferred under roughly 99% existing
device-engine load. The Windows suite, guarded sanitizer run and raw
controls all passed.
[Callback data and occupied size](evidence/windows-amd-write-grant-growth-2026-11-26.json)

### Protected placement evidence

The model held the same file bytes, 6 MiB slot, failure unit and available
sealing credit across arms. The previous planner declined the transition
with either scratch offer. The new planner declined the file-only arm and
reached **4.25 MiB** with private memory, a **1.75 MiB reduction (29.2%)**.
It retained the old snapshot and retired the scratch and inactive allocation.
The Windows SDK fixture with locked memory sealed in the same slot size,
wrote through a held callback handle and independently reopened the CPU
bytes. That callback fixture has its own allocation accounting and
workload. Neither fixture measures physical crash durability or throughput.

The scheduler and commit admission now compare the two placement charges.
Private scratch receives a process-owned memory allowance. Parity and the
writable tail must still fit the checked inactive range and the selected
plan must earn backing writes. The recorded case had enough credit only
for the memory plan. A refused scratch lock preserved the selected image
and the unspent credit. The next decision needs per-phase costs on a named
workload, including reclaim and the complete outer volume.
[Private seal credit evidence](evidence/windows-private-seal-credit-2026-11-26.json) ·
[Volume-file sealing contract](PERSISTENT_VOLUME_FILE.md#sealed-paired-images)

```mermaid
flowchart LR
    S["Selected checked image and held readers"] --> F["Funded seal opportunity"]
    F --> M{"Fresh private workspace grant"}
    M -->|fits and credits pass| L["Fold to locked scratch"]
    M -->|refused or too small| W["Existing file workspace"]
    L --> B["Protected base in inactive range"]
    W --> B
    B --> V["Independent CPU verification"]
    V --> T["Retire scratch and bind writable tail"]
    T --> C["Fence source and flush candidate"]
    C --> P["Flush both selector peers"]
    P --> R["Retire old range after pins release"]
```

## Research that changes decisions

- **Cluster by affinity and access together.** FAST 2026 RubikFS reported
  better compression and fewer unnecessary reads on its read-only
  workloads by clustering similar chunks and grouping by access
  temperature. Use that result to test fragment-level candidate recall and
  read amplification together. FlitzFS needs bounded online updates and
  immutable published restart units. Start with the existing sparse
  catalog and a small offline oracle for the alternative orderings.
  [FAST 2026 RubikFS research](https://www.usenix.org/conference/fast26/presentation/huang)
- **Charge the full placement.** EROFS's project comparison explicitly
  identifies fragment packing and deduplication in the compressed images.
  Compare those mechanisms separately in FlitzFS by holding protection
  constant and counting metadata, shared dependencies and allocation
  rounding. The 16-member tie in this round illustrates why a smaller
  encoded representation alone gives an incomplete answer.
  [EROFS deduplication and fragment comparison](https://erofs.docs.kernel.org/en/latest/comparison/dedupe.html)
- **Keep a current public control and a named-file holdout.** The public
  2026 AITDCC release provides checksums and separate A–H training and
  I–P testing sets. Use those files for byte-exact codec comparisons.
  The named Windows trees independently test directory structure,
  metadata, random reads, edits and collective coverage. Freeze the
  family-level split and bytes before reranking an encoder.
  [AITDCC 2026 official dataset](https://aitdcc.github.io/dataset.html)
- **Support the runtime that is actually present.** AMD published ROCm Core
  SDK 10.2.0 on 18 November with HIP 7.17.0. The release notes disable priority
  streams, although the APIs remain described. The FlitzFS worker now
  negotiates priority separately from basic nonblocking stream support.
  The official Core SDK table names Windows 11 25H2 and Microsoft lists
  26H2 as the newer general update. Treat the local 26H2 CPU/SDK pass and
  documented AMD runtime support as different evidence. The locked Core
  archive recorded an internal `10.2.0rc2` label. Verify package, API,
  runtime DLL, driver and adapter independently before making a support
  claim or changing the pinned package.
  [AMD release notes](https://rocm.docs.amd.com/en/docs-10.2.0/about/release-notes.html#hip-7-17-0) ·
  [AMD stream APIs](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/reference/hip_runtime_api/modules/stream_management.html) ·
  [AMD compatibility matrix](https://rocm.docs.amd.com/en/latest/compatibility/compatibility-matrix.html) ·
  [Windows release health](https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information) ·
  [Observed SDK package](../tools/rocm-sdk-lock.json)
- **Separate first-use and service costs.** AMD documents lazy runtime
  initialization and explicit initialization within a process. Retain the
  initialized stream and buffers only while observed demand funds the
  worker. Park on an event without issuing keepalive kernels. Windows
  supports waiting on both an event and an exact thread handle. Its coarse
  tick is generally 10–16 ms, so record GPU completion and telemetry timing
  with a high-resolution clock before redesigning the freshness boundary.
  Retain a budget sampler only while its exact adapter and factory are
  current. Collect fresh local, non-local and host observations for each
  paid boundary.
  [AMD HIP initialization](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/how-to/hip_runtime_api/initialization.html) ·
  [Windows multiple-object wait](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects) ·
  [Windows tick resolution](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-gettickcount64) ·
  [DXGI topology change](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgifactory1-iscurrent)
- **Price redundancy before increasing codec complexity.** An earlier AITDCC
  I–P run used 35,635,200 mirrored image bytes and 22,523,904 sealed image
  bytes for the same 25,153,011 logical bytes. Range reads were slower in
  that experiment and its accounting excluded the outer volume selector.
  The latest subset route settled at 35,160,064 bytes with the selector.
  These are separate controls. Measure the current candidate, including the
  selector and retained/dependent data, before quoting a comparative gain.
  Windows Storage Spaces uses mirrored writes and a colder parity tier.
  Ceph and FAST ’26 work on repair emphasize pricing reconstruction I/O.
  FlitzFS keeps its local single-failure-unit repair model and measures it.
  [Sealed corpus evidence](evidence/sealed-indexed-images-2026-11-22.json) ·
  [Storage Spaces](https://learn.microsoft.com/en-us/windows-server/storage/storage-spaces/plan-volumes) ·
  [Ceph erasure coding](https://docs.ceph.com/en/latest/rados/operations/erasure-code/) ·
  [FAST ’26 LESS](https://www.usenix.org/conference/fast26/presentation/cheng)
- **Separate logical grouping from read units.** FAST ’26 RubikFS reports
  benefits from clustering similar chunks and grouping by access
  temperature on read-only images. Its study justifies measuring clustering
  and read amplification. FlitzFS needs reusable immutable dependencies,
  a bounded CPU restart unit and transaction-scoped membership changes for
  writable volumes. FAST ’26 CoFS similarly uses a packed lookup structure
  for a fixed read-only tree. Keep the paged writable index for FlitzFS and
  measure attach work independently of client lookups.
  [RubikFS](https://www.usenix.org/conference/fast26/presentation/huang) ·
  [CoFS](https://www.usenix.org/conference/fast26/presentation/wang-li)
- **Try pipelined candidate discovery and price all model bytes.** FAST '26
  ParaSync overlaps content-defined chunking, comparison and reconstruction
  in a synchronization pipeline. Test its stage-separation principle
  locally with checked source generations, bounded queues and no required
  network access. The November Tufazip report includes its model weights
  and decoder in the final size for enwik9. A model that improved payload
  size further lost after charging its added weight bytes. Use a small
  offline ranker to prioritize candidates first. A new on-volume
  predictor also has to repay its protected state and CPU restart cost.
  [ParaSync](https://www.usenix.org/conference/fast26/presentation/zhang-zhihao-parasync) ·
  [Tufazip author's report](https://tufalabs.ai/research/tufazip/)
- **Pin tools and qualify the exact configuration.** WinFsp publishes stable
  v2.2 and the repo already pins its MSI. Zstandard 1.6.0 is the current
  qualified source dependency. DiskSpd 2.3 changed default thread
  placement and buffer separation. Freeze explicit affinity, cache mode,
  buffer settings and verified bytes in comparisons. Windows exposes its
  built-in NTFS compression as per-file and per-directory state. A compressed
  fixture directory sets the default for new files. Verify the resulting
  state on every timed file.
  [WinFsp 2.2](https://github.com/winfsp/winfsp/releases/tag/v2.2) ·
  [Zstandard 1.6.0](https://github.com/meta/zstd/releases/tag/v1.6.0) ·
  [DiskSpd 2.3](https://github.com/microsoft/diskspd/releases/tag/v2.3) ·
  [Native NTFS compression](https://learn.microsoft.com/en-us/windows/win32/fileio/compression-state)

The research receipt records two initial extractions that failed their
content checks. Subsequent captures confirmed the AMD asynchronous/stream
references and the native compression state API. The accepted set holds
18 substantive primary pages. The local FlitzZip reference at
`11396b5123505a4a1959173e1db2d0858c10dd6d` supplies a pinned host-buffer
allocation precedent. Use it only where end-to-end staging pays its own
cost. FlitzFS keeps its independent filesystem format and CPU decoder.
[Source and validation receipt](evidence/windows-independent-seal-workspace-2026-11-26.json) ·
[FlitzZip host-buffer allocation](https://github.com/strmt7/FlitzZip/blob/11396b5123505a4a1959173e1db2d0858c10dd6d/src/gpu/hip_device.cpp#L373) ·
[AMD asynchronous execution](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/how-to/hip_runtime_api/asynchronous.html)

The follow-up verified fourteen primary pages for the write wait, compression
research, host benchmark and Windows durability contracts. One NTFS API
render lacked the required content. The accepted compression-state and
wire-format pages restored the native-control evidence.
[Follow-up research receipt](evidence/windows-deferred-write-events-2026-11-26.json)

## Target control and storage design

```mermaid
flowchart TB
    W["Explorer and Win32"] --> O["WinFsp and exclusive volume owner"]
    O --> C["Authoritative CPU transaction and read path"]
    C --> A["Checked roots, page indexes and decoder registry"]
    A --> H["Hot writable protected extents"]
    A --> B["Immutable protected compression groups"]
    O -. "Measured offer" .-> G["Bounded device worker"]
    G -. "Verified candidate only" .-> C
    C -. "Usage and revisions" .-> P["Rebuildable offline policy and sparse candidates"]
    P --> Q["Yielding work with complete-size and write budgets"]
    Q --> R["Staged image or segment with independent CPU readback"]
    R --> C
```

The volume owner remains the single authority for acknowledgements and
publication. Host observations govern admission, worker lifetime and
background quanta. They cannot authorize stale results, failed barriers
or retired backing access. Recovery selects a consistent checked root
chain. Decoder dependencies live in protected authoritative records.
Learning state can lose its progress and rebuild from a checked snapshot.
[Target architecture](architecture/TARGET.md) ·
[Recovery model](RESILIENT_STORE_DESIGN.md)

## Detailed implementation order

### Make ordinary AMD execution portable and useful

**E03, E08 and S4.** Treat optional priority, verified device output and
event-driven foreground waits as implemented. Retained preparation still
has a high measured first-use cost. In the latest eight-window native
fixture, readiness took 3.179 s of the 3.258 s exchange. The earlier
CPython 64 KiB trial favored CPU for both service time and occupied bytes,
including when preparation was reported separately. The production
controller's arrival trial replays already-committed writes in private.
Its result is a measurement. An independently qualified offer can be
applied to the live callback path, where the exact request still has to
qualify.

1. Bind the populated client witness to the exact signed helper, runtime,
   modules, adapter, CPU decoder and owned backing. First collect a cold
   CPU run and a CPU replay under concurrent but refused device demand.
   Then time cold, prepared, missed and consumed device work on the same
   geometry. Attribute discovery, runtime initialization, ready idle
   residency, input staging, kernel, transfer, supervisor retirement,
   CPU comparison, completion observation and commit. Record the cost of
   abandoned work and the share of device results actually delivered.
2. Prioritize grouped reads with predictable lead and large or recurring
   write cohorts. Feed only a bounded eligible range into the retained
   worker. The installed write trial now funds the largest complete
   burst within the observed grant. It records funded and completed
   request counts separately and can revisit a geometry when fresh
   headroom funds a larger burst. The current suppression watermark
   records funded count for an input-size slot under the selected storage
   policy. Keep that protection against repeated launches. Drive the next
   eligibility decision from completed and consumed counts, changed
   arrival cadence, input class, runtime identity and measured readiness.
   A grant of eight and consumption of two gives evidence about the
   opportunity that occurred. Record the cause of unused reservations
   before allowing this size to recalibrate with unchanged headroom.
   Retire promptly on stalls, pressure, detach or incompatible I/O.
   Background admission on a
   runtime without stream priority requires fresh idle evidence and
   finite dispatch. A refused or failed route leaves CPU I/O available
   without waiting for GPU cleanup. Retry only with relevant changed
   evidence and confirmed retirement.
   [Funded burst and CPU verification](evidence/windows-adaptive-amd-write-bursts-2026-11-26.json)
   · [Grant growth and engine deferral](evidence/windows-amd-write-grant-growth-2026-11-26.json)
3. The foreground writer now binds the fresh completion observation to the
   exact request and records counter order after CPU validation. The SDK
   verifies acceptance within a coarse tick and CPU fallback on ambiguous
   ordering. Measure actual collection and handoff latency on the installed
   path. If those costs materially affect service, reuse the live DXGI
   sampler on one owner while LUID, node, topology and pinned route remain
   current. Pair against the current event-driven path and preserve
   refusal on old evidence, missing headroom or device changes.
   A controlled Radeon callback accepted both fresh budget observations
   and stored 16 device-compressed extents. Independent CPU readback
   verified 1 MiB. The cold callback took 4.006 s. Prioritize amortizing
   worker setup across useful work while recording the smaller stages
   for attribution.
   [Completion observation](OWNED_DEFERRED_WRITES.md#ordering-the-completion-observation) ·
   [Live Radeon and CPU readback](evidence/windows-ordered-write-budget-2026-11-26.json)
4. For foreground compression, price a prepared process and compare
   occupied bytes and end-to-end client time against the existing CPU
   writer. If the device wins only over a batch, use a funded finite burst
   with independent retirement and CPU verification for collective
   recompression. Begin by using the existing native format and shared
   reference selection. Check source freshness again before applying the
   batch. Prioritize savings over tuning an already-losing small shape.
5. Run the winning role on the ordinary supported mount with matched
   baseline and refusal runs. Require current enough evidence to cover
   runtime and workload drift. Bind learned eligibility to device role
   and workload. Prove exact readback, one response, retained prior
   state and clean retirement after refusal, cancellation or loss.
   Measure full p95/p99 and actual protected size against the declared
   access envelope before making a role automatic. Use isolated driver
   failure campaigns to qualify real device loss.

[Current worker gate](../src/windows/hip_api.hpp) ·
[Recovery policy](GPU_RECOVERY.md) ·
[Mounted workload protocol](BENCHMARK_PROTOCOL.md#populated-windows-operation-trace)

### Measure settled protection and remove redundant staging

**E02, E06, E11 and S3, S5, S8.** Start with the existing engine and its
actual refusal reasons. Replay a matched mixed workload of small files,
large unrelated files and repeated updates. Test both observed backing
geometries, a held snapshot, near-full allocation, a denied memory offer
and a fresh CPU reopen. Attribute backing writes, flushes, released
allocation and logical versus occupied capacity to each phase. Preserve
the client's acknowledgement log and exact files.

1. Reuse the current two-placement seal projection and confirm both
   admission paths on the same named workload. Record private scratch
   peak, backing capacity, writes and selector publication separately.
   Exercise memory offers that fit, are refused, and release after
   cancellation. Preserve the funded file path and confirm credit
   refunds only when no backing writes occurred. Compare both outcomes
   with the original protected image and repeat with a pinned reader.
2. Instrument folded build, parity output, verification, tail binding,
   select and retirement before choosing a larger refactor. If scratch
   limits material held-out savings, prototype emitting complete stripes
   to the owned inactive range in logical order. Separate root/index
   construction and any rewrite/read-after-write dependencies from the
   append pass. Retain only bounded verified state or use the existing
   workspace for cases that need random access. Reuse the current stripe
   layout and CPU reader if byte-for-byte format comparison permits it.
3. Independently decode and verify the prepared image, then bind and
   flush the tail, recheck source revision, and publish the selector
   peers. Keep the prior root available until both copies and reader
   lifetimes permit release. Cut at each barrier and range release.
   Require exact surviving payloads, metadata and accounting. If the
   streaming candidate fails that proof, retain the verified workspace
   path. Consider segment-local compaction only after the phase trace
   assigns sustained write amplification to complete image copies.

[Paired layout](PERSISTENT_VOLUME_FILE.md#sealed-paired-images) ·
[Reclaim and repair exits](IMPLEMENTATION_PLAN.md#5-e02--allocation-dependency-ownership-and-automatic-reclamation)

### Bound writable attach and discovery as the namespace grows

**E01, E04, E07 and S2, S6.** The current inventory verifies checked name
pages, every inode and extent, Windows admission and reference ownership.
Keep it as the baseline for old or ambiguous images.

1. Specify which checked fields are sufficient for a normal writable
   attach. Bind the volume identity, root generation, required features,
   namespace/extent roots, allocation and dependency ownership summaries
   and recoverable transaction state in the same publication. Update the
   affected checks on create, rename, unlink, mutation, dependency handoff
   and migration. Derive counters from verified deltas. Compare against a
   reconstructed inventory in development.
2. Verify the touched page or extent on access and bound the outstanding
   repair or verification debt. Admit allocation and mutation only from
   checked state. A missing, damaged, unsupported or conflicting proof
   selects the full recovery path before admitting writes or reclaim.
   Carry foreground repair through the same ownership and CPU recovery
   rules. Rate-limit background auditing using resource and workload
   evidence.
3. Replay the same named-file trace at geometric tiers from one file to
   the largest tier local headroom safely permits. Measure attach with
   cold and retained caches, directory enumeration, random open/read,
   mutations, restart, bytes read and peak memory. Regress against page
   counts, file sizes and live/shadow allocation. Separate fixed startup,
   checked I/O requests, bytes read and live records visited in the
   cost model. Validate the model on a larger held-out tier and on a
   different file-size distribution. Report errors and projections for
   ten million and higher independently of measured runs. Treat full
   enumeration and explicit repair as workload-proportional operations.
4. Persist a bounded scan cursor per volume generation. Refresh changed
   content by revision and preserve sparse shortlist keys under churn.
   Keep the reserved logical area paged and rebuildable. Measure sampled
   discovery against exhaustive ranking on small frozen subsets, then
   use a budgeted shortlist for larger populations.

[Indexed audit](../src/native_indexed_volume.cpp) ·
[Scalable namespace and discovery contracts](IMPLEMENTATION_PLAN.md#4-e01--scalable-native-inode-name-and-extent-storage)

### Improve complete compression with offline, adaptive selection

**E04–E07 and S5–S6.** The first comparison is strong independent Zstandard
and the existing staged shared-base route with the same protection. Price
a candidate on actual settled occupied bytes. Include live payloads,
reference models, indexes, padding, parity, the selector and retained
generations. Record temporary peak space and extra writes separately.
A representation enters production only with exact independent CPU
readback at normal and partial read boundaries.

Minimize settled protected bytes across the declared workload. Reserve
separate envelopes for admissible range-read latency, peak temporary
space, foreground interference and backing wear. Estimate each cohort's
unrealized savings on its measured byte share. This determines whether
the next implementation changes metadata packing, reference discovery,
parity placement or the codec.

1. Freeze mixed named corpora and family splits before selecting settings.
   Keep the published AITDCC I–P and CPython results as repeatable controls.
   Freeze a fresh blind revision-family cohort for any new tuning. Use
   shifted, sparse, already compressed and unrelated inputs alongside
   highly redundant inputs. Keep exact-source copies, attribution and
   input and restored output digests.
2. Compare the existing fixed windows to rolling anchors and
   content-defined chunks under an equal scan budget. Price shared bases,
   dictionary/prefix, reference patches and strong independent encodings
   with their full retained cost. Begin with candidate reranking and
   result reuse. Preserve the local paging and revision invalidation
   behavior already present. Persist per-content-revision sketches and
   capped postings or candidate neighborhoods in protected advisory
   pages. Update touched entries from committed revisions. Compare
   candidate recall and retained bytes against an exhaustive oracle on
   small subsets before extending the scan to large populations.
3. Let groups grow across many files when the measured savings justify
   it. Use indexed restart units with bounded depth and scratch, and
   measure bytes fetched and decoded per returned range. Route GPU work
   to high-volume matching, real compression and verified decoding where
   measured. Keep decode semantics independent of adapter or model
   availability. A CPU encoder can choose a different valid layout.
4. Use an offline compact ranker only after a simple measured policy
   supplies a reference. Train on held-out benefit prediction from
   sparse similarity, access temperature, allocation shape and actual
   compression history. Compare saved bytes per unit of background work,
   false admission, p99 foreground interference and writes per net byte
   saved before enabling online adaptation. Continue with the simpler
   rule when a model earns no gain. An experimental lossless codec must
   separately improve the verified occupied-size frontier including its
   model overhead and honest CPU decoding cost.
5. Run maintenance in bounded resumable steps with below-normal
   scheduling, fresh RAM/VRAM/CPU and I/O headroom, and idle-budget
   accounting. Measure with multiple volumes, workload drift, battery
   or graphics pressure and user writes arriving mid-step. Derive write
   cost and reuse policy from backing capabilities and measured latency.
   Retain wear and recovery reserves on persistent media. Measure
   seek-heavy work on HDDs and written bytes on SSD and removable flash.
   Derive admission from the provider contract and observed behavior,
   including when the bus identity is ambiguous.

[Collective compression design](IMPLEMENTATION_PLAN.md#9-e06--protected-collective-representation-in-ordinary-storage) ·
[Official AITDCC dataset](https://aitdcc.github.io/dataset.html)

### Qualify Windows lifecycle, evolution and backend recovery

**E08–E11 and S1, S7–S10.** Reuse signed WinFsp and the current helper lifecycle.
The tray should expose the owned memory and persistent volume workflows,
show which capabilities the selected backing supports and report recoverable
failures. Determine media capabilities by probing the backing. Qualify
the advertised NVMe, SATA SSD, HDD and flash combinations before exposing
them in setup.
Use the ordinary Explorer and an independent Win32 client for absent,
already-in-use and distinct owned mount targets. Cover open sharing,
rename/replace/delete, sparse extension and truncation, enumeration,
Win32 naming, timestamps and attributes, applicable access checks,
memory maps, cache modes, explicit flush, repeated clean detach and
exhausted-resource behavior. Document the exact supported semantics.

1. Freeze the current source, compiler, pinned dependencies, exact driver,
   helper, CPU decoder and optional AMD worker identities before the
   candidate run. Exercise clean setup, offline CPU operation, upgrade,
   refusal, rollback and removal on declared Windows builds. Stage missing
   supported user-mode dependencies privately. Record the AMD runtime
   compatibility evidence and retain CPU access if accelerator setup
   is unavailable. Confirm teardown of every owned process, handle and
   allocation.
2. Freeze golden images and independent readers for each promised format
   generation. Maintain tested CPU reads throughout the declared
   multi-release window. Migrate through bounded copy-on-write work with
   persisted progress and an intact recovery anchor, including models and
   any new codec features. Check old-reader behavior, unknown required
   features, stale writers, interruption before and after each selector
   change and return to service.
3. Define a durability matrix per supported backing. Specify the safe
   failure unit, which successful operations and flushes are durable, the
   peer or parity reconstruction envelope and observable corruption
   outcomes. Inject incomplete writes and lost or damaged units at the
   existing barriers, with independently retained acknowledged operations
   and expected file hashes. Run destructive, physical and driver-fault
   qualification on disposable isolated systems. Establish recovery of
   acknowledged file bytes, metadata, names and allocation before writes
   resume. Device destruction and losses beyond surviving redundancy need
   an independent copy. Advisory-state loss can be recovered by rescan.

[Windows system boundaries](WINDOWS_ARCHITECTURE.md) ·
[Compatibility package](IMPLEMENTATION_PLAN.md#12-e09--multi-release-readers-and-gradual-format-migration) ·
[Durability package](IMPLEMENTATION_PLAN.md#14-e11--durability-repair-and-independent-failure-qualification)

### Run matched real-host comparisons and freeze the candidate

**E12–E13 and S11–S12.** First replay the ordinary Windows witness with
fixed test files and independently verified outcomes. Predeclare which
workloads require a gain and report neutral and negative cohorts.
Run DiskSpd 2.3 as the recognized throughput and access-latency tool.
Use the named-tree operation trace for Windows semantics, cold/steady
maintenance and storage allocation. AITDCC A–H and I–P are separate
controls. Named real files and at least one fresh protected cohort
exercise grouped behavior across files and directories.

- On the same current Windows build and equivalent disposable backing,
  compare plain NTFS, Windows-native NTFS compression, FlitzFS CPU and
  automatic AMD at ingest and after settlement. Use native
  `FSCTL_SET_COMPRESSION` with `COMPRESSION_FORMAT_DEFAULT` in the new
  compressed fixture directory and verify each file's state with
  `FSCTL_GET_COMPRESSION`. Bind the NTFS geometry and OS build. Record
  allocated bytes per file and the whole-volume allocation delta. The
  latter includes metadata, dependencies and protection. Expose any
  difference between NTFS and FlitzFS protection in the comparison.
  Run common cache and client flush scenarios before any provider-specific
  benchmarks. Use a dedicated memory-backed fixture for the host speed
  comparisons and separate provider failure proof for durability.
- Keep public workload hashes, benchmark parameters and raw results.
  Alternate trial order and report dispersion, p50/p95/p99 access, durable
  write and read throughput, peak and settled bytes, decode amplification,
  CPU/GPU use, backing writes, interference and verified byte counts.
  Report a result as inconclusive when run variation exceeds the effect.
  Keep affinity, data and cache modes fixed within a pair. Use repeated
  pairs within observed host headroom. Classify guest runs as correctness
  evidence.
- Reuse a successful narrow proof while inputs remain identical. Run the
  full comparative grid once the candidate can change the release
  decision. Close each advertised tuple on the frozen artifacts and
  complete rolling and final soak with zero unresolved data-integrity or
  lifecycle failures. The owner reviews the result and decides on release.

[Objective benchmark protocol](BENCHMARK_PROTOCOL.md) ·
[Exact release gates](IMPLEMENTATION_PLAN.md#18-exact-release-gates)

### Decide the next batch from three integrated results

| Priority and decision | Next deliverable | Acceptance and follow-through |
| --- | --- | --- |
| 1. AMD work consumed by an ordinary Windows client | Add one trace record per observed demand and admitted finite opportunity to the populated Win32 witness. Join demand, worker readiness, CPU verification, stored representation and receipt on the same operation. Drive the first comparison with grouped sequential reads and protected collective analysis. Keep current small writes as the CPU control. | Alternate matched lanes and record completed and delivered device bytes, CPU continuity, occupied size, p99, idle residency and retirement. Use an already-qualified installed tuple for host numbers. Use signed guests for correctness and fault experiments. Enroll profiles that improve the measured objective after setup and verification. |
| 2. Highest complete-size gain on named data | Add a report over the existing indexed maintenance and reencoding witness. For each cohort report eligible and nominated byte share, verified retained members, rejection reason, selected representation, protected bytes after retire, peak and bytes rewritten. Compare the current group path to a strong independent Zstd pass, then replay a small fragment oracle on the high-cost cohorts. | Compare identical bytes, namespace and protection. Publish a held-out size/access frontier with neutral cohorts included. If the oracle finds savings missed by lookup, change the sparse nominations. If lookup recalls the profitable pairs and allocation absorbs their gains, work on packed protected records or parity. Introduce a model or a new codec only after an existing baseline measures the remaining opportunity. |
| 3. Bounded writable attach and provider recovery | Grow a named churned tree over feasible orders of magnitude. Record allocated pages, live and dirty records, attach I/O and peak private memory. Draft a checkpoint proof with checked incremental summaries and a bounded continuation of changed pages. Exercise each incomplete transaction through the independent CPU audit and current checked reopen first. | Fit against feasible observations and validate on a reserved higher tier before projecting tens of millions of files. Resume or fall back to the full audit on absent, stale or damaged proofs. Confirm each promised flush and recovery condition on the supported backing qualification matrix before changing writable defaults. |

The same client log should support all three decisions. Run a focused test
for a changed invariant and reuse source-bound unaffected evidence. Require
the larger cross-product of tuples at candidate freeze, after these results
can inform what the product will support. Provider durability, Windows
package ownership and historical CPU compatibility remain parallel release
dependencies. The final NTFS, compression and soak grid runs on the same
signed artifacts and explicitly declared compatibility window.

## Remaining effort and repricing

The existing package estimate remains **59–127 person-weeks** at low
confidence, with an open research tail for stronger collective compression.
The new evidence removes a staging constraint, a runtime compatibility
failure and fixed foreground completion polling. All twelve formal gates
remain open for a stable candidate. Reprice after the ordinary automatic AMD
comparison, a named-file attach curve, a complete protected-size frontier
and the first backed interruption campaign. The project remains in
development qualification and the owner makes the release decision.
[Package effort and critical path](RELEASE_EXECUTION_PLAN.md#remaining-engineering-effort)
