# FlitzFS stable release execution plan

Reconciled against `df5be9b5b9d0bc05ac565eb2da8454ac76bed3d2`.
The [repository review](architecture/REVIEW.md) describes current evidence
and the [revised architecture](architecture/TARGET.md) specifies designated
product boundaries. The [implementation plan](IMPLEMENTATION_PLAN.md) owns
the ordered engineering checkpoints. This document defines the release
qualification evidence for each checkpoint. The
[adaptive storage execution plan](architecture/ADAPTIVE_STORAGE_EXECUTION.md)
sets out the implementation order for sealed regions, model selection,
ordinary AMD I/O and provider-specific qualification.

[Current repository overview](REPOSITORY_OVERVIEW.md) ·
[Architecture evidence](evidence/architecture-reconciliation-2026-11-21.json) ·
[Latest real hardware and SDK evidence](evidence/amd-fresh-completion-observation-2026-11-21.json)

## Release assessment

FlitzFS remains in development qualification. The earliest credible
release candidate needs measured improvements in the ordinary Windows
workflow, recovery on each supported backing provider, and strong held-out
compression results. The verified pieces use different executables and
fixtures today. Bring them together before counting a gate as complete.

| Area | Evidence now | Work that still determines release |
| --- | --- | --- |
| Native Windows storage | Checked paged paths, inodes and extents serve writable callbacks. Reusing the verified inode reduced backing bytes by 38% in the 4,096-file handle query fixture. The full existing-volume audit remains on the reopen path. | Bound open, traversal, mutation, reclamation and recovery costs together. Qualify them on large mounted namespaces. |
| Mount and application | Signed WinFsp 2.2 guest testing covers both backings. Two populated file-backed trials recovered all 1,024 original files and a flushed write after helper termination. A fresh writable helper then flushed another write and passed independent CPU recovery. | Qualify interactive Explorer on the current client, the signed installer lifecycle, earlier publication cuts, physical backing recovery and mounted AMD operation. |
| AMD and CPU continuity | A matched live Windows controller trial executed 14 AMD kernel launches and selected 12 advised members. The settled image used 319,488 bytes on the AMD route and 327,680 on CPU. Both passed independent CPU verification of all 36 files. A deadline fixture recovered all 15 files on CPU and re-admitted the same adapter. A running controller also adopted a foreground AMD route and recovered the accelerated write and earlier CPU data. | Measure complete-operation gains on held-out inputs. Qualify mounted foreground compression, driver-reset recovery and automatic route selection across supported adapters and workload classes. |
| Compression | Live selection on the full AIT A–P fixture preserved the open handle and all bytes on independent CPU reopen. The Windows memory controller now prepares and selects the base and tail automatically and retires old memory in bounded steps. An open SDK file context consumed verified Radeon frames after the switch. [Live selection evidence](evidence/live-hybrid-selection-2026-11-22.json) · [Automatic selection evidence](evidence/automatic-memory-sealing-2026-11-23.json) | Implement and qualify persistent placement, held-out total-size gains and mounted read access. |
| Learning and reclamation | Advisory checkpoints, incremental candidate discovery, and staged cold recompression work in checked fixtures. Verified committed totals now make history eligible after reopen. | Prove bounded lookup and restart at production scale, persistent automatic policy changes, and steady-state reuse that reduces occupied space and unnecessary write traffic. |
| Backing media | Memory and Windows file providers measure the actual time of maintenance I/O. A sequential read-ahead fixture verified that a slower observed provider can defer optional reads. File-backed attachment checks the selected volume identity and geometry. Explicit device-loss results end access through the opened backing. | Qualify NVMe, SATA SSD, rotating disks and removable flash with the active Windows cache and removal settings. Verify dense-file reuse on media without sparse ranges. Measure complete read and rewrite costs and verify recovery after interrupted writes and removal. See [backing provider qualification](architecture/ADAPTIVE_STORAGE_EXECUTION.md#4-backing-provider-qualification). |
| Durability and compatibility | Checked roots, mirrored payload recovery, generation-bound references and several interrupted-write models are exercised. | Bind acknowledgement to real backend barriers and close retained-generation repair, mixed-format upgrade and independent rescue gates. |
| Packaging and security | User-mode dependency staging and exact package binding are in place. | Validate the installed and removed product, hostile inputs, IPC access control, update and rollback, dependency evidence and recovery packaging. |

[Read-ahead verification](evidence/sequential-read-ahead-2026-11-22.json) ·
[Media qualification plan](architecture/ADAPTIVE_STORAGE_EXECUTION.md#4-backing-provider-qualification) ·
[Mounted guest receipt](evidence/windows11-stable-winfsp-2.2-2026-11-20.json) ·
[Mounted volume activation](evidence/windows11-mounted-volume-activation-2026-11-21.json) ·
[Populated post-flush recovery](evidence/windows11-mounted-post-flush-recovery-2026-11-21.json) ·
[Helper selection and matched controller runs](evidence/helper-file-selection-2026-11-21.json) ·
[Cold compaction receipt](evidence/screened-cold-compaction-2026-11-20.json) ·
[AMD reference-profile receipt](evidence/amd-installed-reference-profile-2026-11-20.json) ·
[AMD deadline recovery receipt](evidence/amd-recovery-qualification-2026-11-21.json) ·
[Foreground AMD route activation](evidence/windows-amd-foreground-activation-2026-11-21.json) ·
[Indexed inventory receipt](evidence/indexed-reopen-single-pass-audit-2026-11-20.json) ·
[Latest collective packing and AMD receipt](evidence/indexed-cross-window-packing-2026-11-21.json)

### Remaining engineering effort

These ranges carry forward the work-package estimates. The sealed result
narrows the cold-protection cost risk. Incremental region selection, mounted
AMD and media-specific qualification still determine release readiness.

| Work to complete | Packages | Person-weeks | Evidence that narrows the estimate |
| --- | --- | ---: | --- |
| Scalable native I/O and reclamation | E01–E02 | 11–22 | Populated client scale curves and bounded reuse during foreground writes |
| Useful automatic AMD and CPU continuity | E03 | 4–9 | One mounted paired run with successful AMD, CPU-only and rejected-device routes |
| Collective compression and offline learning | E04–E07 | 18–44 | First held-out complete-size frontier with access costs, restart and write accounting |
| Windows interface, compatibility and package | E08–E10 | 11–22 | Two mounted providers on an exact signed client package and one old-volume migration |
| Independent durability and security qualification | E11 | 6–12 | First physical-backend interruption campaign across all publication barriers |
| Comparative qualification and stabilization | E12–E13 | 9–18 | Complete matched results and the exact-candidate soak |

The total is **59–127 person-weeks**, rounded to **60–130** for planning.
Confidence remains low until the mounted scale, physical recovery and
held-out compression evidence is available. Exceptional compression has
a separate research risk with no reliable finish date. The rolling stability
record can begin during development. The exact-candidate soak follows the
last material fix. [Work-package estimates](IMPLEMENTATION_PLAN.md#17-completion-dependency-graph-and-effort)

## Current gate ledger

All twelve gates remain open for a stable candidate. Each row names the next
artifact and the observation needed before the related work can advance.

| Gate and work package | Next implementation or qualification artifact | Required evidence |
| --- | --- | --- |
| S1 · E10–E11 | Generate a candidate manifest from the CMake configurations, private dependency locks, staged toolchain and actual app, worker and SDK binaries. | Source, runtime and package hashes match the tested artifacts. Each applicable test lane identifies the exact candidate or the unchanged inputs it covers. |
| S2 · E01 | Extend the WinFsp populated client trace and add read, write and lock accounting at name lookup, inode read, publication and reopen. Exercise deep and wide layouts at several measured sizes and model larger populations. | Replay agrees with a reference namespace and independent CPU bytes. Backing work and resident state are measured at feasible sizes. Validate projected scaling against a held-out size. |
| S3 · E02 | Use the existing whole-image rebuild as a control for bounded segment evacuation and reusable allocation in the paged store. Persist reader and retained-root ownership across handoffs. | Repeated mounted churn settles at stable or falling occupied bytes. Old readers and checked dependencies remain readable across cancellation and interrupted retirement. |
| S4 · E03 | Qualify the installed AMD route for foreground eligible writes and cold shared-base work on the same mounted app build. Compare with a matched CPU lane and an unavailable-device lane. | Device counters prove useful submitted work. End-to-end cost or settled size improves on qualifying data, while missing or failed devices leave CPU operations and every stored representation readable. |
| S5 · E04–E06 | Freeze the mixed held-out corpus and collect complete-image accounting for independent codecs, shared dictionaries, delta and multi-reference candidates and the existing shared-base control. | Net space gains survive protection, retained generations and reclamation costs. Independent restart units give practical cold random reads. Size and access results include all cohorts and losses. |
| S6 · E07 | Drive the persistent advisory catalog and policy from observed savings, live resource pressure and workload changes in the mounted lifecycle. | Corrupt or absent advice is rebuildable. Foreground bursts preempt the next quantum. Recovery, migration, learning, compaction and scan progress resume independently with measured write cost. |
| S7 · E08 | Extend the signed WinFsp fixture to an interactive Windows 11 26H2 x64 client and the published Explorer and Win32 compatibility matrix. | Both supported backings cover create, attach, metadata, copy, sharing, mapped I/O, detach and fresh-process recovery. Scoped app, service, mount and worker ownership is verified through failure and removal. |
| S8 · E02, E11 | Build from the mounted Write, EOF shrink, rename, mapped write and image-selection cuts. Add concurrent cached and mapped flushes, then qualify the persistent backend on dedicated disposable hardware. | Each cut resolves to an allowed complete generation. Acknowledged bytes and namespaces survive the declared failure domain. A single protected-copy loss repairs with byte-exact independent recovery. |
| S9 · E09 | Add a capability and reader matrix for historical fixtures and mixed old/new representations. Exercise bounded, resumable format migration under regular writes and space pressure. | Supported history opens on CPU. Old writers refuse unsupported mandatory features before mutation. Upgrade interruption, compatible rollback and CPU-only export recover the promised data. |
| S10 · E10–E11 | Stage a signed Windows package and CPU recovery tool with owned installation, repair, removal and rollback. Review the parser, broker, update and dependency trust boundaries. | Every artifact is attributable. The supported install paths work on a clean client. Defensive negative tests and independent review close actionable issues for the exact packaged source. |
| S11 · E12 | Collect repeated matched DiskSpd and file-workflow results on real hardware, with filesystem and compression controls. | Publishable raw results include complete occupied bytes, CPU/GPU utilization, write amplification, foreground tails and independent recovery for every phase. |
| S12 · E13 | Freeze the candidate and run the existing soak and stability programme across the declared configurations. | Resolve material findings and rerun affected gates. Attach the evidence bundle to the owner's release decision. |

## Critical path

Use the [ordered engineering checkpoints](IMPLEMENTATION_PLAN.md#3-execution-order-and-dependencies).
The same source-bound installed Windows path must support native client
semantics, indexed publication, independently readable data and useful
automatic AMD work. The execution evidence below sets each gate's exit.
Independent corpus, process ownership, allocator and failure-model work can
begin before the integrated path closes.

The signed-driver fixture now covers in-flight Write, EOF shrink, rename and
selector changes during reclamation. Qualify installed foreground AMD through
ordinary writes, with eligibility and response ordering observable. Extend
the recovery trace to concurrent cached and mapped flushes, populated-directory
scale and maintenance interference.

The Windows scheduler can service eligible group work after a scan declines
under its CPU budget. Retain the test with a due but deferred scrub as a
mixed-activity control. [Scheduling and scale evidence](evidence/indexed-scale-reopen-2026-11-20.json)

## 1. Candidate and platform ledger

- Maintain one source manifest for each candidate. Include the exact tree,
  dependency locks and resolved versions, compiler and SDK, WinFsp artifacts,
  ROCm package and HIP runtime digests, supported Windows versions, and
  applicable qualification receipts. Keep results bound to their original
  commit IDs and use the migration map when linking them to current history.
- Freeze a supported current Windows 11 x64 client and servicing channel for
  each campaign. Microsoft lists 26H2 in the General Availability Channel
  and identifies 26H1 as a separate new-device release. Test the exact client
  and updates the product promises to support.
  [Windows 11 release information](https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information)
- Track the Windows release, AMD driver, ROCm package and loaded HIP runtime
  as separate inputs. AMD's 10.2.0 support table names Windows 11 25H2 and its
  component table lists HIP 7.17.0. Exercise the current 26H2 client explicitly
  for the product's AMD qualification. Record the package build marker and
  actual runtime query alongside support documents and CPU continuity results.
  [AMD ROCm 10.2.0 release notes](https://rocm.docs.amd.com/en/docs-10.2.0/about/release-notes.html)
- Use the exact WinFsp artifact in correctness, installation and rollback
  checks. The release API identifies WinFsp 2026 as v2.2 and exposes the
  MSI and test archive digests. The download overview still links to an
  older installer, so verify the release inventory when selecting a package.
  [WinFsp 2026 release](https://github.com/winfsp/winfsp/releases/tag/v2.2)
- Resolve the pinned ROCm Core SDK package, compiler, HIP headers, runtime DLL
  and code objects from the admitted Windows artifact. AMD publishes the Core
  SDK and HIP as distinct versioned components. The checked `10.2.0` archive
  carried package build `10.2.0rc2` and HIP `7.17.26434`. Qualify the exact
  installed driver/runtime, supported adapter and worker bundle on each host.
  Recheck the archive provenance and release status before promoting the pin.
  [AMD Core SDK release notes](https://rocm.docs.amd.com/en/latest/about/release-notes.html) ·
  [HIP installation](https://rocm.docs.amd.com/projects/HIP/en/latest/install/install.html) ·
  [Windows archive qualification](evidence/windows-rocm-10.2-qualification-2026-11-20.json)
- Keep routine builds and checks local. Use a new owned fixture, current
  resource observations, silent launches, and confirmed process retirement.
  Place development artifacts in the scoped workspace and keep live owner-host
  test volumes in memory. Use isolated systems for unqualified driver setup,
  abrupt failure testing and physical backing qualification.
- Save the selected gate list, original commands and return codes, the
  exact mounted or injected boundary, readback digests, and any failure or
  deferral. The checkout, package and mounted results must agree before the
  final candidate can pass S1.
- Keep the original tool and fixture failures with their run IDs. Track the
  executed native, reduced-codec, sanitizer, packaging and isolated lanes
  against the candidate that supplied their binaries. A rebuilt downstream
  package needs the applicable package and lifecycle checks. Use
  source-bound receipts to reuse evidence for unchanged components.

## 2. Large indexed volumes and foreground I/O

- Inventory every supported callback through the Windows adapter, native
  session and paged store. Check path resolution, large directory cursors,
  rename replacement, file lifetime after unlink, sparse ranges, EOF updates,
  flush, and delete-on-close. Include access, sharing and error codes.
- Keep root and path verification on cold reads and give the remainder of the
  graph an explicit resumable scrub. For writable resume, establish which
  generation and dependency claims the backend can prove under its leases.
  Preserve the full-audit path until the replacement passes the same
  acknowledgement and recovery matrix. Charge caches to live headroom and test
  their eviction and pinned generations under pressure.
- Replace application-visible full scans and prototype-sized work where they
  affect startup, enumeration, update or close. Use existing indexed cursors
  and generation-safe read views. Split background preparation from bounded
  publication and record the lock time of both.
- Measure volumes at a single-file control and several increasing feasible
  sizes. Choose rungs that expose page-depth and cache transitions on the
  backing being tested. Vary depth, directory width, allocation density and
  memory headroom. Include empty and byte-sized files, mixed real content,
  multi-gigabyte files and sparse files above 4 GiB. Use mixed workflows so
  small files and large streams compete for the same caches.
- Fit operation work from named files, extents, directory shape, tree depth
  and bytes touched. Hold out a larger measured rung or a different tree
  before projecting to 10 million and 30 million files. Separate extrapolated
  work from measured latency and capacity. Report the fit range, residuals
  and assumptions for depth, data distribution, caching and resource limits.
  Record live memory, pages visited, writes, open and enumeration costs, and
  I/O tails. Build the admitted smaller fixtures and label the results
  beyond the largest measured rung as projections.
- Verify a reference model with handle and operation traces after replay.
  S2 passes when ordinary clients complete the declared layouts and resource
  budgets with exact contents and namespace results.

[Indexed volume implementation](INDEXED_NATIVE_VOLUME.md) ·
[Windows controller](WINFSP_OWNED_CONTROLLER.md)

Reusing one checked view per indexed directory page reduced backing reads
from 27,261,323 to 20,183,683 bytes for 385 names read through single-entry
SDK buffers. A wide buffer reduced reads from 965,711 to 726,507 bytes in
the same directory. Both paths returned the expected names, inode IDs,
logical sizes, and termination markers. Add these cases to the large
mounted client trace and retain the mixed-size buffer cases when measuring
read throughput and contention.
[Directory query evidence](evidence/indexed-directory-query-2026-11-21.json)

### Next writable-resume qualification

The current paired start still runs a complete CPU inventory audit on each side
of the image and selector barriers. The healthy path now acquires both leases
before those barriers and issues one flush to each backing interface. On the
matched 16,384-file fixture it reduced backing reads from 197,479,086 to
130,327,966 bytes with zero startup backing writes. The existing five-MiB
WinFsp fixture and the larger native fixture bind separate evidence.
The large-volume gate still requires a mounted ordinary client and a larger
dataset.
[Indexed scale receipt](evidence/indexed-scale-reopen-2026-11-20.json) ·
[Five-MiB fixture](evidence/indexed-reopen-single-pass-audit-2026-11-20.json)

The Windows adapter now checks allocation, basic metadata and security
from the initial audit's reachable inodes. The 4,096-file WinFsp SDK fixture
read 34,756,618 backing bytes per startup, down from 355,798,881. It retained
both barriers and wrote zero backing bytes during reopen. A missing
descriptor on a reachable file still refuses startup before mirror repair.
The independent CPU reader checked the original files and the following
acknowledged callback write. Both complete native audits remain the next
startup scaling target.
[Windows indexed startup evidence](evidence/winfsp-indexed-startup-audit-2026-11-20.json)

The two audits now use the verified inode payloads directly and reuse the
previous extent owner while it remains the same inode. Writable reopen
read 113,630,294 backing bytes on the 16,384-file fixture and 29,995,842
on the 4,096-file WinFsp fixture. Both barriers and zero startup writes
were observed in each run. The separate compressed-object comparison
measures the saving from a single decoded check for byte-identical
encodings. Both stored copies still receive independent verification.
[Storage audit evidence](evidence/indexed-audit-reuse-2026-11-20.json)

The decoded name-page cache reduced median writable reopen from 271 ms
to 192 ms on 16,384 short files. On the 2,640-file CPython 3.15.2
standard-library fixture it moved from 163 ms to 132 ms. The comparison
used both fences and two full native inventories. Its short-file work model
projects about 64 GB of backing reads at 10 million files and 192 GB at
30 million under comparable layout and per-file cost. This identifies
the next target for large-volume startup. Move verified work off the
foreground path under a qualified root and backing proof, then measure
insertion churn, filesystem shape and mature mounted reads separately.
[Exact measurements and projection](evidence/indexed-audit-page-cache-2026-11-22.json)

The healthy retained path now reuses the first decoded audit after both
fences. A fresh object walk reads both encoded replicas and checks them
against the exact checkpoint. Fault fixtures refused newly degraded
raw data, compressed data and name pages at either fence. On the same
16,384-file shape, reopen image reads declined from 104.14 MB to
76.46 MB. The new short-file curve predicts the held-out result within
0.006% and projects 46.7 GB and 140.1 GB at 10 million and 30 million.
The stored walk still reads the entire live object inventory. The next scaling work is
to give foreground reads and writable admission a qualified bounded path
while a resumable check verifies the rest of the selected generation.
[Stored recheck evidence](evidence/indexed-fenced-stored-recheck-2026-11-22.json)

The ordinary helper now retains an existing backing after the initial
indexed and Windows metadata audit. This removes the separate read-only image
inspection. The focused startup control measured 1,644,441 backing bytes
before and 1,304,248 after, with the same three flushes and zero writes.
The test covers rejection of a corrupt selected image and handle retention
after a failed fence or mount. The native verification passes around the image
barrier still determine the scaling curve.
[Current attachment startup evidence](evidence/release-reconciliation-attachment-2026-11-21.json)

1. Extend the separate pre- and post-image-barrier counters by selector,
   object-table pages, name-index pages, inode/lifetime metadata, encoded
   payloads and dependency resolution. Use the same populated
   volume for a clean start, a start requiring verified repair and a read-only
   rescue. Collect call counts, backing bytes, allocations and elapsed
   distributions for both loaded and idle starts.
2. Define the reopened-writer invariant against a concrete selector and image
   identity, checkpoint generation, namespace sequence, decoder dependencies
   and in-flight lifetime state. Identify which proofs are valid under both
   leases, which become invalid at the flush, and which require revisiting
   stored bytes. Preserve the exact result for same-generation forks and
   damage appearing at or after the fence.
3. Evaluate a durable, versioned verification-progress record for bounded
   resumable audits. Bind progress to the exact root and backend admission
   evidence. Treat it as evidence of what was verified at that checkpoint;
   revalidate touched pages, frame digests and dependencies on use. Test
   torn, stale, corrupt and replayed progress plus older volumes that have no
   such record. Keep independent CPU rescue available without advisory state.
4. Qualify any alternative writable admission against the existing full audits
   as an oracle. Cover healthy starts, late and double replica damage,
   invalid native frames with valid storage digests, leaked lifetime state,
   interrupted repair, and writes concurrent with the next scrub quantum.
   Compare the acknowledged namespace and readback after a reopen in each arm.
5. Repeat the retained-data checks, large-population runs and mounted client
   operations on the selected implementation. Record foreground stalls,
   repair amplification, pending queue growth and recovery while another
   reader pins a generation. Keep the conservative admission path for
   any backend or legacy format whose invariants have not been qualified.

## 3. Allocation, reclamation and acknowledged data

Verified object-tree totals now recover a conservative compactable extent
estimate on reopen. The paired-range models at 4 KiB and 64 KiB both
qualified for compaction before the next foreground write. The previous
reader kept its allocation until release, and the settled volume reopened
byte-for-byte on CPU. The next mounted tests should carry this case over
multiple sessions and measure actual allocated bytes and write costs.
[Reopen and reclamation evidence](evidence/reopened-reclamation-2026-11-21.json)

- Trace each commit from record allocation through payload and metadata writes,
  barrier completion and root acknowledgement. Give reference records,
  readers and repair work explicit generation ownership. Preserve the
  previous valid graph across interrupted publication.
- Finish incremental reclamation in the standard volume lifecycle. Select a
  batch from measured reclaimable bytes, reserve its cost first, reconcile
  concurrent foreground commits, and return space only after old readers and
  references release it. Include new dictionaries and representation changes
  in that accounting.
- Charge occupied bytes for mirrors, metadata, free-space structures,
  unreclaimed tails, retained roots and protected decode dependencies.
  Measure rewrite bytes, live/retired space, recovery time and foreground
  interference after repeated create/update/delete cycles.
- Run full-volume, low-headroom, overlapping-reader and interrupted
  reclaim tests. Model failures before and after every handoff. Verify the
  same cases on each actual persistent backend in isolated qualification.
- Verify the backing provider's write and flush semantics. Windows exposes
  file flush as an OS boundary. Real durability depends on the complete
  path through the storage device. Record and test the hardware and failure
  model used for acknowledgements.
  [Microsoft file flush contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
- S3 and S8 pass after the ordinary background path produces verified net
  space recovery, keeps existing acknowledged bytes through the supported
  failures and restores repairable protected copies.

## 4. Useful AMD work and independent CPU continuity

- Keep installed discovery selective and asynchronous. Verify the approved
  helper, module, runtime and hardware identity before the first compute
  attempt. Confirm the Windows adapter/node and current physical, commit
  and video budget for the actual request.
- Amortize fixed worker setup across multiple independently protected
  groups. Collect setup, transfer, kernel, reply, CPU verification and
  publication time. Calculate full-image savings per cohort. Use copy
  search if the reference working set or launch budget cannot be admitted.
  Preserve foreground responsiveness and the existing job ownership.
  In the current host trials, five existing disjoint groups published on
  CPU before each full AMD batch returned. The matched wait controls
  published zero during that interval. Complete size matched and timing
  ranges overlapped. [Overlap evidence](evidence/windows-amd-full-batch-overlap-2026-11-21.json)
- The rebuilt controller's installed reference route submitted 17 AMD kernel
  launches. The matched missing-package arm submitted zero. Both regrouped
  and recovered all 23 files on CPU, with identical 1 MiB compacted images.
  Use this pair as a device-execution and fallback control in the mounted
  qualification. [Current controller evidence](evidence/release-reconciliation-indexed-handle-2026-11-22.json)
- A completed AMD exchange now keeps its group reservation through foreground
  activity until a funded idle step can verify it. The installed-route trial
  published 90 advised members after a CPU read. A selected edit left advice
  usable for the other four groups, and all 15 files passed independent CPU
  readback. The read trial and its matched CPU control each settled at
  2,760,704 bytes. [Retired reply evidence](evidence/amd-retired-replies-2026-11-22.json)
- The installed anchor scheduler starts with complete groups and learns
  its launch allowance from independently verified exchanges. In a
  constrained Windows trial, the worker reached its hard deadline before
  publication and all five groups completed on CPU. The normal installed
  route verified a device-selected reference and recovered all 23 files.
  Compare warm and cold starts on mounted workloads and qualify recovery
  with fresh device evidence after an actual runtime interruption.
  [Measured batch evidence](evidence/amd-measured-batch-sizing-2026-11-21.json)
- The uniform LZ4 parent can group complete independent requests into one
  supervised AMD sweep. A live three-request comparison reduced median AMD
  time from 3.55 s to 1.44 s and kept the encoded bytes identical. A constrained
  trial refused stale memory telemetry at completion. Measure the sustained
  complete-operation benefit against CPU before enrolling this workload.
  [Independent request batching](evidence/amd-uniform-request-batching-2026-11-21.json)
- Foreground GPU preparation now releases the filesystem lock before encoding
  the worker request. The supervisor takes the immutable frame by move and
  retains its route pins. On the matched public-source trace the worker
  accounted for 97% or more of AMD response time. Keep that workload on CPU.
  Measure a finite readiness episode and the actual arrival of independent
  eligible requests before adopting any foreground warm-up policy.
  [Foreground worker evidence](evidence/windows-amd-foreground-worker-boundary-2026-11-22.json)
- The measured foreground route now verifies that the exact write fits the
  live work and host budget before deferring. A controlled AMD run refused a
  partial offer, adopted a funded sweep and recovered its complete bytes.
  [Complete measured-window evidence](evidence/windows-amd-full-measured-window-2026-11-22.json)
- A one-request preparation lease now reaches actual AMD readiness before input
  delivery. On three matched 1 MiB public requests it cut the median response
  after submission from 1.79 s to 545 ms. Charging setup, the prepared median
  was 1.30 s. One unused preparation retired with no reply. Keep this workload
  on its existing CPU route until an end-to-end comparison earns a new decision.
  [Prepared sweep evidence](evidence/amd-prepared-sweep-2026-11-22.json)
- Measure automatic route selection for early ingestion and cold work
  separately. Use the CPU reference for forecast and fallback. Compare real
  public and controlled inputs across supported AMD classes and a CPU-only
  client. Require actual device work and verified storage benefit before
  issuing a qualified route profile.
- Exercise unsupported and missing artifacts, memory and queue pressure,
  malformed or stale advice, cancellation, changed file content, timeout
  and runtime removal. Test device loss and high-risk driver faults on
  isolated systems. Confirm CPU read/write continuity and bounded recovery
  attempts from distinct evidence after retirement.
- S4 passes when the installed application shows useful automatic AMD
  behavior in ordinary operation and a separate CPU reader reopens every
  admitted representation and generation.

[AMD route](AMD_RUNTIME_DISCOVERY.md) ·
[Worker ownership](WORKER_CHANNEL.md) ·
[GPU recovery policy](GPU_RECOVERY.md)

## 5. Strong collective compression and autonomous learning

- Extend the incremental similarity and coherence catalogs across distant
  files and many groups. The paged catalog now retains funded alternate
  nominations and can resume within a matching bucket. One controlled
  13-file workload recovered a live match after the newest nomination became
  stale, saving 116 KiB of combined image, selector and catalog space. The
  lookup count grew from 13 to 27. Test recall, space, write cost and time to
  find useful candidates across larger and denser buckets. Compare sparse
  slot order with size-aware ranking before widening the search. Keep the
  catalog independently rebuildable, and tie scan progress and eviction to
  observed resources. Keep measured costs and failed trials in the advisory area.
  [Displaced nomination evidence](evidence/indexed-coherence-alternates-2026-11-21.json)
- In the 5,121-record catalog trace, reusing the verified root and pruning
  empty key ranges reduced metadata traffic for 7,168 misses from 168 MiB to
  110.6 MiB. All 1,024 positive ranges resolved and older snapshots retained
  their own records across publication. A separate 16,384-fingerprint
  construction trial hit its 64 MiB backing allowance during appends.
  Compare write amplification under sorted and interleaved ingestion,
  compaction debt, total occupied bytes and read traffic on the same input.
  Qualify retained-reader overlap and restart with the managed physical-file
  catalog provider before increasing the scan envelope.
  [Sparse catalog evidence](evidence/indexed-coherence-range-reads-2026-11-21.json)
- Compact index pages and coalesced sibling writes reduced the interleaved
  16,384-fingerprint build from 121.8 MiB to 67.3 MiB. Backing writes fell
  from 122.8 MiB to 68.3 MiB. The compacted catalog uses 520 KiB more than the
  packed 4 KiB control. Miss traffic is lower in bytes and higher in read calls.
  [Compact catalog evidence](evidence/indexed-coherence-compact-pages-2026-11-21.json)
- Under the 64 MiB catalog reservation, the normal maintenance path
  compacted and retained 15,748 primary and 3,937 alternate nominations.
  The selected replacement occupied 8.6 MiB, and the next scan appended
  to it. The old and new backings occupied 73.0 MiB together while the old
  reader was pinned. Exercise repeated compactions and edits with uneven
  real data, using measured persistent-media read and write costs to choose
  between dense packed pages and compact update pages.
  [Reclamation evidence](evidence/indexed-coherence-reclamation-2026-11-21.json)
- The paired advisory journal now selects an identified catalog backing
  after its replacement has been checked. Independent Windows RAM fixtures
  cover restart before the first selector write, after the first barrier,
  after the final barrier and with the resolver returning the wrong complete
  catalog. New qualified managed files supply two sparse reservations.
  The model exercises reopening on both sides of the selector commit,
  waiting through one torn advice slot, deferring retirement under a live
  pin, and reusing a released range with a fresh identity. Qualify repeated
  rotation on populated volumes under native Windows allocation and
  interruption on disposable storage. Measure catalog writes and foreground
  response with the mounted helper active.
  [Catalog selection format](OPTIMIZATION_STATE.md#identified-catalog-selection) ·
  [Windows restart evidence](evidence/identified-coherence-selection-2026-11-21.json) ·
  [Managed range validation](evidence/managed-coherence-catalog-ranges-2026-11-21.json)
- Use training and held-out sets chosen before the evaluation. Test close
  revisions, unrelated files, small text, SVG, tabular and numeric data,
  binaries, media and already compressed files. Include nonsynthetic
  workloads and simple controls that isolate each encoding.
- Compare the total space of candidate encodings, including references,
  record padding, dictionaries, model storage, protected metadata, rebuild
  work and retained copies. Verify lossless decode independently on CPU.
  Measure cold random access and read amplification with the exact selected
  group representation. The interleaved-group audit now reuses verified
  bases within one pass and checks the stored replicas directly. Extend the
  backing-traffic comparisons to groups that exceed the audit cache and
  exercise recovery with pinned readers. Keep the owned session and
  independent snapshot read measurements. On the mixed public corpus, group
  packing lowered occupied bytes and owned-session backing reads, with higher
  backing reads from independent snapshots.
  [Grouped audit evidence](evidence/indexed-grouped-audit-2026-11-20.json)
- Persist advisory observations in the protected internal area. Key them
  by representation, workload signature, format generation and relevant
  device or backend identity. Age stale observations and make poor
  predictions recoverable. Keep every required decoding dependency separate
  from rebuildable learning data.
- Retained similarity lookup recovered one missed group in a controlled
  cross-window workload and saved 24 KiB after compaction. The three other
  workloads held their prior compacted sizes, including two public corpora.
  Keep this ranking for short extents with no fitting in-window partner.
  An earlier version that also reranked the current window enlarged the
  public revision image by 8 KiB. Compare joint group alternatives by
  verified occupied bytes, including the space required by displaced files,
  before applying wider reranking. Measure shortlist recall, maintenance
  reads and writes, and small-read amplification across larger populations.
  Carry useful lookup into the paged advisory catalog and retain the bounded
  in-memory window for active preparation. Expand the grouping envelope
  together with CPU range reads and dependency retirement.
  [Retained similarity comparison](evidence/indexed-retained-packing-similarity-2026-11-22.json)
- Use fresh idle opportunities and bounded work to choose further scans,
  grouping, consolidation and format migration. Learn from net space
  saved, foreground service time, memory and write cost. Stop or shrink
  background work promptly under contention. Measure the system as it learns
  across restart, routine edits and changing workload.
- S5 and S6 pass after held-out complete-image savings, practical access,
  offline learning, durable restart and low interference are demonstrated
  together on stated hardware and workloads.

## 6. Windows experience, compatibility and installation

- Use the existing helper and WinFsp integration as the normal Windows path.
  Exercise creation and clean attachment of memory volumes and controlled
  persistent volumes. Make mount target collision, drive removal, pause,
  unmount, helper shutdown and restart recoverable and visible in the app.
- Run native Win32 callers and Explorer for case and Unicode semantics,
  file attributes and access checks, long paths, replace and sharing,
  metadata enumeration, flush, cancellation and multi-process handle behavior.
  Exercise cached, noncached, overlapped and mapped I/O and record the tested
  combinations. Include notifications, streams, links and reparse behavior in
  the published compatibility contract and applicable qualification.
- Test interrupted installer installation, repair, upgrade, removal and
  rollback with the exact signed dependency chain. Confirm package staging
  resolves tools and runtimes without relying on a development machine.
  Test normal CPU operation without the AMD package and optional GPU
  availability on supported setups.
- Retain independent CPU readers for volumes written by at least the preceding
  three released versions. Until those releases exist, keep the supported
  historical fixtures and protected decoding dependencies. Reject unsupported
  mandatory features before mutation and preserve each codec identifier's
  meaning.
- Verify old and new readers, mixed storage, partial migration,
  interruption/restart, space pressure, and rollback to the supported
  previous generation. Verify bytes and persist new dependencies before
  publishing each migrated unit. Preserve a recovery route to exported bytes.
  Qualify old-software downgrade separately from new-reader compatibility.
- Close S7, S9 and S10 with actual client coverage, clean owned shutdown,
  compatible format and update behavior, and signed installation receipts.

## 7. Security and failure qualification

| Failure domain | Required observation on the affected path |
| --- | --- |
| Process exit and forced stop | Every acknowledged generation reopens exactly. User processes see predictable status. The helper, worker and supervisors retire. |
| Interrupted writes and reclamation | Recovery chooses a complete generation and resumes within the documented failure model. Later reclamation retains references and preserves space accounting. |
| One corrupted protected copy | Independent validation finds surviving data. Repair publishes a verified new copy and records the action. |
| Unrecoverable corruption or missing backend | Return an explicit failure, preserve surviving material, and provide an independent recovery or export route. |
| GPU reset or failed compute | Ignore the incomplete result. Continue with CPU and gate later attempts on authenticated evidence and verified retirement. |
| Hostile metadata, namespace or IPC | Bound parsing, authority and resource use. Verify the package, update and app trust boundaries and close actionable findings. |

Re-run the targeted portable, Windows, raw-codec and sanitizer lanes on the
exact change. Cover the native worker and package interfaces with targeted
negative cases. For final S10, retain current advisory evidence and review
dependencies, installer behavior, ACLs, update signing and the recovery tool.
The connector refused the Dependabot endpoint. An authenticated GitHub REST
read returned an empty first page of open alerts during this reconciliation.
Refresh that read for the exact release candidate and retain the published
advisory and artifact inventories.
[Current dependency receipt](evidence/release-reconciliation-indexed-handle-2026-11-22.json)

## 8. Benchmarks, soak and the release decision

- Use the full 2026 AIT Data Compression Challenge and retain its original
  training and held-out splits. Current measurements cover standalone codecs,
  verified mirrored image size and collective compression on complete CPython
  revisions. Continue with ordinary cold recompression and account for the
  persistent catalog and all maintained copies before claiming a size win.
  The paired Windows cold test saved 557,056 bytes beyond compaction on A–H.
  The read-only screens for I–P and A–P declined a rewrite. Standalone 256 KiB
  and 1 MiB blocks brought small gains on I–P. Measure complete image size,
  allocation and small-range access before choosing a native unit size.
  Evaluate protection overhead against the same isolated-failure recovery
  model before selecting a denser representation.
  [Public workloads and evidence](BENCHMARK_PROTOCOL.md#public-compression-workloads)
- Use Microsoft's DiskSpd 2.3 as the current controlled Windows I/O
  benchmark. Retain the exact binary, XML profile and results, cache mode,
  queue depth, duration, fixture data and platform topology. Run the common
  buffered and unbuffered profiles only where both filesystems can honor
  equivalent semantics. Report provider-specific acceleration such as
  NTFS BypassIO as a separately labeled result.
  [Microsoft DiskSpd](https://github.com/microsoft/diskspd)
- Pair DiskSpd with a repeatable file tree. Time Robocopy from a fixed source
  to a new destination, with fixed thread and retry settings and recorded
  metadata choices. Time Explorer enumeration and small file opens, then
  replay directory creation, renames, updates and deletion in the fixture.
  Use a pinned source checkout and a normal incremental build for a mixed
  read/write workflow. Reopen the volume and verify content and namespace
  after each workload. Measure foreground tails during background packing.
  [Microsoft Robocopy](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/robocopy)
- Compare FlitzFS at ingest, after admitted idle work, and after verified
  reclamation. Use NTFS on the same current Windows 11 build as the primary
  control, with both native defaults and supported NTFS compression.
  Capture the Windows build, NTFS volume information, allocation units, and
  enabled cache and compression settings. Match backing, usable capacity
  and corpus. Include exFAT and ReFS where available, and use offline codec
  controls for the compression frontier.
  [Microsoft NTFS volume queries](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-fsinfo)
- Where a licensed UL PCMark 10 Data Drive fixture fits the available resource
  and protection envelope, retain its standard trace results as another
  workload. UL reports about 15 GB of writes at default settings. Fund the
  published workload before running it and reserve the storage-consistency
  test for separate endurance qualification.
  [UL PCMark 10 storage workloads](https://support.benchmarks.ul.com/support/solutions/articles/44002171443)
- Alternate repeated controls and candidates. Report absolute occupied
  space, foreground read/write throughput and tail latency, peak CPU/GPU/RAM
  use, process retirement, background duty, I/O amplification and wear-related
  write totals. Reopen and check all data in each phase. Treat overlapping
  timings and unqualified storage semantics explicitly.
- Benchmark on qualified real hardware. Capture isolation-guest results as
  correctness evidence. Keep the owner's development volumes in memory.
  Use a separate qualified physical test path for a claim about persistence
  or drive throughput.
- Freeze the exact candidate for the final soak. Cover sustained ordinary
  workloads, maintenance, power and recovery qualification within the
  failure model, and alternate supported host configurations. Inspect
  material failures and rerun affected gates. S11 and S12 require the
  complete retained evidence and the owner's release decision.
- Plan for the existing 30-day candidate soak and 90-day rolling stability
  record across supported configurations. Confirm the meaningful failure
  coverage and material fixes with the evidence from each lane.

## Immediate implementation order

The [five implementation checkpoints](IMPLEMENTATION_PLAN.md#3-execution-order-and-dependencies)
set the order. The following receipts address the release gates and
are reusable only when their source, runtime and configuration inputs still
apply.

### Execution checkpoints

| Step | Implementation and verification | Exit evidence |
| --- | --- | --- |
| 1. Reconcile | Record the exact source and GitHub baseline, independent checkout provenance and declared dependency locks. Retain the original failing controls. Re-run the affected dual-codec, raw, sanitizer, portable notices and interruption lanes. | Verified inventory, linked failures and controls, exact current run outcomes. S1 |
| 2. Install and use one Windows volume | Extend the mounted Win32 results for both backings to a signed installation and interactive Explorer. Carry both through create, reopen, duplicate attach refusal, detach, restart and uninstall on the target Windows client. | Trace from installed helper through interactive Explorer and byte-exact CPU reopen for each provider. S7 |
| 3. Scale the same workflow | Instrument verified root, inode, page and dependency I/O, then choose the largest measured bottleneck. Preserve generation-scoped checks and incremental scrub. Repeat under cold, warm, concurrent and low-headroom conditions using named populated files up to the supported scale. | Access, memory and write curves, stable Windows results, old-reader and latest-writer assertions. S2 |
| 4. Prove reclamation and recovery together | Drive overwrite/delete churn until space is scarce, publish a smaller checked replacement, then release the old reader. Extend to incremental evacuation where whole images repeatedly lose to concurrent writes. Reconcile ownership of live ranges, protected dependencies, readers and pending repairs at every commit. | Returned occupied bytes, bounded write costs and no loss of acknowledged hashes across a backend-specific failure matrix. S3, S8 |
| 5. Use AMD on the same volume | Exercise a qualified installed foreground offer and automatic collective work with current runtime, headroom and workload. Trace pressure, device loss, stale evidence, CPU selection, publication and verified retirement. Match AMD, CPU and refused-device client traces. | End-to-end CPU-readback results and complete-cost observations for each automatically enabled role. S4 |
| 6. Improve compression and learn offline | Freeze tuning and held-out families early. Extend the sparse candidate index and revision reuse. Price exact deduplication, independent compression, dictionaries, reference groups and new methods by complete stored size. Retain the best verified representation and learn from admitted idle results. | Held-out total-size/access curves, bounded discovery at scale, restartable policy, corruption isolation and measured background/wear impact. S5, S6 |
| 7. Freeze formats and installation | Enumerate decoder requirements and maintain real older-reader fixtures. Trial versioned mixed representations and incremental COW migration with CPU export. Exercise package integrity, app/worker ACLs, signed driver/runtime combinations, user-data preservation, update and clean removal. | Supported compatibility matrix, manifest and SBOM, closed security findings, signed installation and rescue receipts. S8–S10 |
| 8. Compare and soak | Freeze the qualified package and comparable Windows backing. Alternate DiskSpd and file/metadata workflows on real hardware, with complete size and recovery accounting. Run the declared soak across supported configurations and requalify affected lanes after material changes. | Complete reproducible results, all gate evidence, stability record and owner decision. S11, S12 |

### Next integrated qualification

The first populated external-client trace passed on a signed WinFsp 2.2
guest. It exercised a 1,024-file directory, mapped and ordinary writes,
sharing, retained-handle rename and fresh-process reopening. The control's
fresh read-only pass produced 1,028 namespace updates. Coalescing recent
automatic access-only timestamps removed that traffic while independent
CPU recovery verified the same payload digest.
[Mounted trace evidence](evidence/windows11-populated-mount-access-time-2026-11-21.json)

The populated fixture now passes post-flush process interruption with the
external client handle open. The independent CPU reader recovered all
original files and the acknowledged write. A fresh writable mount of the
same backing flushed a continuation that also passed independent readback.
Both independent guest runs retired their old mount and helper.
[Post-flush recovery evidence](evidence/windows11-mounted-post-flush-recovery-2026-11-21.json)

The 4,096-file controller fixture now records backing reads across 64 handle
queries and checks identifiers, sizes, attributes and all four timestamps.
The change reuses the same inode for basic metadata and automatic Cleanup.
Use these query results as a regression control for the large mounted trace.
[Handle metadata evidence](evidence/release-reconciliation-indexed-handle-2026-11-22.json)

The populated trace now verifies a flushed overwrite from 131,209 to 66,533
bytes and a second rename before helper termination. The retained writer and
the new pathname resolved to the same inode. A separate CPU reader recovered
the new name, EOF, creation time and bytes along with the 1,024 neighboring
files. A fresh writable mount flushed a 65,709-byte continuation that passed
independent CPU readback. The original create-and-flush case also passed.
The byte-I/O failure model replays all 1,242 write and barrier cuts across
4 KiB and 64 KiB failure units. For each unit it reopens and writes from
representatives of all four complete states.
[Overwrite and rename recovery evidence](evidence/windows11-overwrite-rename-recovery-2026-11-22.json)

Two signed-driver guest trials also cut an in-flight Windows write before the
payload flush, after the first paged root flush, and after the second. All six
client requests were pending at the cut and completed with
`ERROR_OPERATION_ABORTED`. Independent CPU recovery found the previous
complete file before the payload flush and the complete replacement after
either root flush. Each case remounted writable and verified a continuation.
[In-flight write evidence](evidence/windows11-inflight-publication-cuts-2026-11-21.json)

Indexed flush now reads each object-tree root replica once per barrier phase.
The native namespace check reuses those verified pages only within that phase.
Leaf and branch fixtures removed four reads and 16 KiB per flush, including
with automatic compression. The tests exercised a late replica failure,
allocation pressure on both sides of the barrier, and independent CPU readback.
[Indexed flush evidence](evidence/indexed-flush-root-reuse-2026-11-22.json)

Ordinary EOF shrink and rename now pass all three inner image cuts in two
fresh WinFsp 2.2 Windows 11 guests. Each fixture preserves 32 acknowledged
neighbors, an independent witness, the target inode and the expected namespace.
The fresh mounted writer flushes a continuation, which the CPU reader checks
again. The original three in-flight Write controls also pass on the same build.
[Mounted metadata evidence](evidence/windows11-mounted-metadata-publication-cuts-2026-11-22.json)

Two fresh signed guests passed all three outer selector cuts during native
background reclamation. The CPU reader selected epoch 1 before the first
selector write and epoch 2 after either slot flush, and verified both retained
image ranges. Each case remounted and flushed a continuation. A separate sparse
provider test on the same build verified retired-range allocation release and
reuse in both directions after the retention checks completed.
[Mounted selector evidence](evidence/windows11-mounted-selector-publication-cuts-2026-11-22.json)

A mounted client now changes a mapped view in between ordinary acknowledged
cached writes, delete and recreation, and a directory rename. Six process
cuts recovered the expected target bytes, 32 neighbors and a separately
acknowledged mapped witness. Each case recovered under the CPU reader and
flushed a continuation after remount. The interrupted caller was still in
`FlushViewOfFile`. [Mapped publication evidence](evidence/windows11-mounted-mapped-publication-cuts-2026-11-22.json)

1. **Combine pending cached and mapped I/O.**
   Extend the populated trace to simultaneous outstanding cached and mapped
   operations. Add cuts around the file flush that follows a successful view
   flush, and record the two acknowledgements independently. Keep binding each
   result to the client operation, storage geometry and binaries. Run the
   physical failure campaign on dedicated backing.
   [Mapped flushing contract](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-flushviewoffile)
2. **Measure the scaling bottleneck on that trace.** Count calls and bytes
   for selector, object-index and name-index pages, inodes and lifetimes,
   content frames and protected dependencies. Observe entry, image fence,
   selector fence and callback publication separately. Compare handle and
   volume flush, and measure the inode check before the barrier separately.
   Use the single read of each object-tree root replica per phase as a
   regression control. Start with 1,024 and
   16,384 populated files, then test larger populations up to the supported
   scale on adequately resourced clients. Include wide and deep directories,
   cold and warm caches, retained readers and recoverable damage. Compare
   bounded writable admission to the complete audit oracle using
   same-generation substitutions and corruption at either fence. Land the
   alternative only when both arms agree on acknowledged bytes and rejection.
3. **Advance allocation using measured steady-state cost.** Use the current
   paired-range handoff as the recovery and equivalence control. Compare
   incremental evacuation and reusable segment accounting on create, edit,
   delete and regroup churn. Track ownership through current, prior and
   in-flight generations, including shared bases and active readers. Make
   the next reclamation step resumable and reserve its physical write cost
   against live headroom. Reuse a range only after every generation that can
   address it retires. Require a falling or stable occupied-byte curve at
   stable live content, bounded work per quantum and exact acknowledged
   readback across interrupted publication on each supported backend.
4. **Bring both AMD roles into the same Windows client.** Use the worker and
   installed admission path for the existing collective route. Supply the
   running foreground controller with a profile earned from matched complete
   writes. Feed the verified batch path from already ready requests and charge
   startup, host copies, validation and publication before comparing end-to-end
   results against CPU. Use the prepared exchange only for a measured episode
   tied to eligible queued work. Keep serving CPU work while setup runs. Charge
   ready-but-unused episodes, abandon a stale lease on a workload or resource
   change, and verify retirement before reacquisition. Compare time to verified
   completion, host activity, occupied bytes and foreground tails with an
   immediate AMD batch and CPU. Exercise stale budget samples during long batches.
   Alternate AMD-present,
   CPU-only and rejected-device routes on matched data. Record transfer, kernel, validation,
   publication and backlog separately. Start the mounted failure matrix with
   the verified deadline fixture. Confirm the owner takes a fresh group as
   its next recovery opportunity, CPU operations complete during the check,
   and a later ordinary batch completes on the requalified AMD route. Exercise
   a stale reply and device loss on a disposable setup. Verify only fresh
   evidence and confirmed retirement admit reacquisition.
   Cover discrete AMD, supported unified-memory and CPU-only hosts through
   the qualification matrix. Require a reproducible gain in throughput
   or settled bytes on qualifying workloads and a bounded foreground cost.
5. **Build persistent candidate discovery and stronger compression together.**
   Freeze training and held-out corpora before tuning. Measure recall against
   labeled revision families and dissimilar controls. Extend the paged
   band/fingerprint lookup to bounded postings ranked with size and content
   evidence. Use the present one-representative lookup as a recall and
   occupancy control. Bind postings to stable inode/extent generations,
   expire stale postings and verify restart with the advisory area partially
   written or absent. Report bytes and I/O per live file, recall and probes
   at each scale step, including popular buckets and dissimilar controls.
   Evaluate protected deduplication, trained shared dictionaries, delta
   encoding, reversible transforms and a bounded learned candidate against
   the current shared-base representation. Qualify groups of varying
   membership with independent range decoding and preserve measured
   byte-exact access after base loss and repair.
6. **Close the measured resource and durability loops.** Record changes to
   foreground queues, CPU and GPU duty, memory headroom, wear-related bytes,
   saved bytes and stable progress across sessions. Fit advice to the
   device and workload evidence and cap exploration by live admission.
   Exercise a foreground burst during scan, model update, regroup, scrub,
   reclamation and format migration. Verify that every task yields at
   documented boundaries while useful work makes progress when admitted.
   Run the interruption matrix per provider and use separate qualified
   physical hardware for loss-of-power and device-cache behavior.
7. **Qualify installation and gradual migration.** Apply verified package and
   toolchain identity before any setup operation. Use the FlitzZip native build
   and runtime receipt approach for the staged user-mode dependencies. On the
   exact signed FlitzFS package, exercise clean install, interrupted repair,
   rollback, removal and repeated shutdown. Carry supported older volumes
   through a mixed-capability state, interruption, resumed migration and
   CPU-only export. Prove that a down-level writer is refused before it can
   mutate a volume with required capabilities it cannot preserve.
8. **Freeze the measurement and stabilization candidate.** Use DiskSpd with
   matched caching, access patterns and controls. Pair it with real-file
   copy, replace, open, enumerate and recovery checks, and record both
   complete image bytes and occupied bytes at every phase. Treat physical
   throughput and in-memory operation as distinct measurement profiles.
   Preserve independent readback, exact dependency provenance, applicable
   security review, all raw measurements and sustained stability evidence.

Use the [gate definitions](IMPLEMENTATION_PLAN.md#18-exact-release-gates)
and the [current development state](DEVELOPMENT_STATE.json) as the checklist.
Treat newer test receipts as evidence for their exact scope and record remaining
work alongside them.
