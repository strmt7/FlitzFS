# FlitzFS stable release execution plan

Reviewed on 21 November 2026 against the source at `76e9e17f0954998ee05b16d4f05c8aed9ea3d233`
and the collective packing work validated in this round. The [implementation plan](IMPLEMENTATION_PLAN.md)
defines the full product scope and work packages. This plan gives the order
of execution, evidence required at each handoff, and the remaining release
work.
[Previous assessment and work-package estimates](https://github.com/strmt7/FlitzFS/blob/912e3e0d01f37b41c5435f0d6e50b4b5cb947060/docs/RELEASE_EXECUTION_PLAN.md)

## Release assessment

FlitzFS remains in development qualification. The work left is a multi-month
engineering programme. The existing estimate of **60–130 person-weeks** is a
planning envelope with low confidence. The compression research has a
separate, open-ended tail. Update the estimate after a large mounted run,
an isolated persistent-backend recovery campaign, and a held-out compression
comparison. The current benefits apply to the sampled images and workloads.
[Review and source evidence](evidence/release-execution-review-2026-11-20.json)

| Area | Evidence now | Work that still determines release |
| --- | --- | --- |
| Native Windows storage | Indexed paths, inodes and extents use checked paged storage. CPU readback and modeled interruption results cover several native paths. | Complete namespace and data I/O at scale. Bound open, traversal, mutation, reclamation and recovery costs together. |
| Mount and application | The WinFsp 2.2 guest fixture exercised mounted external Win32 clients, extended paths, Unicode and persistent identity. The application and injected attachment checks build locally. | Qualify the signed installer, interactive Explorer, system restart and ordinary workloads on the supported Windows 11 client. |
| AMD and CPU continuity | The qualified test package discovers AMD automatically, prices a reference and produces sparse bodies in one supervised exchange. The writer publishes independently verified advice. The identical CPU-only control reopens all files exactly. | Measure useful complete-operation gains on held-out inputs. Qualify route selection and recovery across supported adapters and workload classes. |
| Compression | Shared bases, sparse XOR, prefixed Zstandard and short-file grouping can reduce complete occupied images. On the mixed-length fixture, looking ahead to a fitting extent reduced the settled image from 827,392 to 786,432 bytes. | Establish repeatable total-size gains against strong, applicable controls, including shared metadata and retained versions. Qualify access costs for the same volume. |
| Learning and reclamation | Advisory checkpoints, incremental candidate discovery, and staged cold recompression make progress in checked fixtures. | Prove bounded lookup and restart at production scale, persistent automatic policy changes, and steady-state reuse that reduces occupied space and unnecessary write traffic. |
| Durability and compatibility | Checked roots, mirrored payload recovery, generation-bound references and several interrupted-write models are exercised. | Bind acknowledgement to real backend barriers and close retained-generation repair, mixed-format upgrade and independent rescue gates. |
| Packaging and security | User-mode dependency staging and exact package binding are in place. | Validate the installed and removed product, hostile inputs, IPC access control, update and rollback, dependency evidence and recovery packaging. |

[Mounted guest receipt](evidence/windows11-stable-winfsp-2.2-2026-11-20.json) ·
[Cold compaction receipt](evidence/screened-cold-compaction-2026-11-20.json) ·
[AMD reference-profile receipt](evidence/amd-installed-reference-profile-2026-11-20.json) ·
[Indexed inventory receipt](evidence/indexed-reopen-single-pass-audit-2026-11-20.json) ·
[Collective packing and current AMD receipt](evidence/indexed-uneven-short-packing-2026-11-21.json)

## Critical path

1. Freeze the exact candidate and supported Windows contract for each
   checkpoint. Bind source, dependencies, data fixtures, GPU runtime, SDK,
   application binary and test configuration.
2. Make one current indexed volume complete a real Windows operation sequence.
   Use that same volume for interference, interrupted recovery and
   compaction checks. Use the signed driver in isolated qualification first.
3. Close scalable namespace and allocation work, then exercise large mounted
   volumes. Make the allocator, handle lifetime and dependency transitions
   part of the same qualification campaign.
4. Move from isolated AMD advice and compression trials to automatic use with
   a measurable benefit. Keep ordinary CPU operations and independent readback
   available during every failure case.
5. Qualify persistence, repair, migration, packaging and lifecycle against the
   exact sources selected for the release candidate. Complete comparative
   runs and the final soak on that candidate. The owner makes the release
   decision.

GPU, compression research and the Windows correctness matrix can advance while
the allocator and recovery work proceed. Mounted performance depends on both
a qualified driver path and the scalable storage path. A changed source,
runtime or dependency invalidates only the evidence whose inputs it changes.
The evidence matrix records that dependency explicitly.

The Windows scheduler now checks pending compression after an optional scan
declines under its CPU budget. The injected Windows test admits a beneficial
group within its own budget while the due scrub stays deferred. Retain this
case in the mixed-activity matrix with both controls and exact CPU readback.
[Scheduling and scale evidence](evidence/indexed-scale-reopen-2026-11-20.json)

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
- Use the exact WinFsp artifact in correctness, installation and rollback
  checks. The release API identifies WinFsp 2026 as v2.2 and exposes the
  MSI and test archive digests. The download overview still links to an
  older installer, so verify the release inventory when selecting a package.
  [WinFsp 2026 release](https://github.com/winfsp/winfsp/releases/tag/v2.2)
- Keep routine builds and checks local. Use a new owned fixture, current
  resource observations, silent launches, and confirmed process retirement.
  Place development artifacts in the scoped workspace and keep live owner-host
  test volumes in memory. Use isolated systems for unqualified driver setup,
  abrupt failure testing and physical backing qualification.
- Save the selected gate list, original commands and return codes, the
  exact mounted or injected boundary, readback digests, and any failure or
  deferral. The checkout, package and mounted results must agree before the
  final candidate can pass S1.

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
- Qualify 1,000, 16,000, 100,000 and at least one million named files, then
  extend the largest supported configurations as backing and headroom allow.
  Exercise deep and wide directories, unrelated updates, and several
  memory envelopes.
  Record bytes resident, paths/page lookups, writes, open and enumeration work,
  and random and sequential I/O tails against the same reference contents.
- Verify a reference model with handle and operation traces after replay.
  S2 passes when ordinary clients complete the declared layouts and resource
  budgets with exact contents and namespace results.

[Indexed volume implementation](INDEXED_NATIVE_VOLUME.md) ·
[Windows controller](WINFSP_OWNED_CONTROLLER.md)

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
  files and many groups. Keep catalog entries bounded and independently
  rebuildable, with wavefront progress and eviction tied to resource
  observations. Use sparse paged lookup with resumable candidate retrieval.
  Index stable content and region generations so that a filename change
  preserves the relationship. Track candidate recall across large buckets,
  and keep measured costs and failed trials in the advisory area.
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
- Follow the fitting-extent change with trials across multiple discovery
  windows. For each size and similarity cohort, record candidate recall,
  attempts and refusals, settled bytes saved, temporary bytes written, and
  reads needed for small ranges. Compare current ordering, size-aware
  candidate lookup and measured similarity ranking on the same images. Keep
  candidate lookup paged and bounded. Qualify a larger grouping envelope only
  once its range reader and dependency retirement pass the same checks.
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
The connector refused the Dependabot URL. An authenticated paginated REST
read returned zero open alerts on 21 November. Refresh that read for the exact
release candidate and retain the published advisory and artifact inventories.
[Current dependency receipt](evidence/indexed-uneven-short-packing-2026-11-21.json)

## 8. Benchmarks, soak and the release decision

- Use Microsoft's DiskSpd 2.3 as the current controlled Windows I/O
  benchmark. Record the exact binary, arguments, caching mode, access
  pattern, queue depth, data size, test duration and runtime configuration.
  Pair it with the Windows semantics harness for namespace, durability and
  identity behavior, and with separately timed real-file workflows.
  [Microsoft DiskSpd](https://github.com/microsoft/diskspd)
- Compare FlitzFS at ingest, after admitted idle work, and after verified
  reclamation. Match the usable backing and test data across NTFS baseline,
  supported NTFS compression, exFAT, and ReFS where the host and volume setup
  provide it. Add relevant external compression controls for the space
  frontier and record which have comparable random access.
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

| Checkpoint | Changes to finish | Evidence needed before advancing |
| --- | --- | --- |
| A | Carry the installed reference route into a verified automatic Windows workload. Instrument the foreground and recovery path on the same populated image. | Matched readback, occupied bytes, route selection, startup and worker retirement. |
| B | Advance indexed foreground semantics and incremental reclamation together. Replace any measured namespace or whole-image bottleneck with a generation-safe bounded operation. | Real client operation trace, large-population resource curves, pinned-reader and failure-cut recovery. |
| C | Run held-out compression on active and cold files. Connect the protected learning policy to measured route and representation choices. | Complete image savings, read amplification, background interference, write cost and restart continuation. |
| D | Qualify backend durability, gradual upgrades, system integration and package cleanup. | Independent CPU rescue, exact backend and installed-driver receipts, Windows compatibility and security review. |
| E | Run the recognized matched benchmarks and exact-candidate final soak. | An auditable pass for S1–S12 and the owner's decision. |

### Next integrated qualification

1. Prepare one disposable indexed volume and a deterministic external Win32
   operation trace. Use the same populated image for create, overwrite,
   handle-sharing tests, rename, flush, unmount, and reopening. Record returned
   status, generation, expected bytes and observed bytes at each flush. Begin
   at a population supported by the existing mounted harness and repeat as
   indexed allocation and lookup scale. Keep the separate CPU reader and an
   ordinary client reader in the test.
2. Break resume and enumeration costs down by object-index pages, name-index
   pages, inode and lifetime records, and payload and dependency reads. Mark
   the proof boundary at each barrier. Use cold, warm, concurrent-reader, and
   verified-repair runs with the same image. Advance the bounded writable-resume
   design in section 2 only when amended-generation and post-barrier damage
   checks agree with the full-audit control.
3. Run create, edit, delete and cold regroup cycles while a reader pins an
   earlier generation. Report live, protected and reclaimable ranges and each
   operation's actual bytes written. Release the reader and confirm that the
   new reader sees the latest acknowledged bytes and that the returned ranges
   are reusable. Repeat with an interrupted publication and one damaged copy.
   Once the model passes, repeat under the actual persistent backing provider
   in isolation to establish its acknowledgement and repair contract.
4. Feed the same mounted request trace with qualified AMD, CPU only and AMD
   withdrawal after submission. Add real revision clusters with variable
   lengths and dissimilar controls. Record verified work, transfer and kernel
   duration, publication decisions, independent CPU readback, and the approved
   process-tree retirement after each batch. Recheck active handles, committed
   memory and worker ownership across repeated cycles after warm-up. Keep
   process exit and long-lived resource use as separate measurements.
5. Freeze a held-out file set before tuning. Compare ingest, idle recompression
   and reclaimed size for each volume, including protected copies and retained
   generations. Use the same files to measure random ranges, ordinary open and
   directory enumeration, CPU and GPU duty, and writes attributable to
   maintenance. Run the Windows I/O matrix on qualified live hardware and
   collect comparable controls under the same storage and cache conditions.
6. Freeze the signed package for compatibility, update, restore and removal
   testing. Carry an older created volume through interrupted migration, a
   fresh mount by the upgraded reader, CPU-only export, and supported rollback.
   Record source and installer identity, owned process and driver state before
   and after testing, and exact filesystem readback. Repeat affected lanes
   after changes and track passive stability on the exact final candidate.

Use the [gate definitions](IMPLEMENTATION_PLAN.md#18-exact-release-gates)
and the [current development state](DEVELOPMENT_STATE.json) as the checklist.
Treat newer test receipts as evidence for their exact scope and record remaining
work alongside them.
