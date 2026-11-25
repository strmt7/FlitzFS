# FlitzFS detailed implementation plan

**Reconciled on 25 November 2026.** The next integrated milestones are the
shared Win32 settlement trace, automatic AMD service on identical client
demand, the indexed attach differential and held-out settled compression.
The E01–E13 acceptance and dependency contracts below define the release exits.
[Current assessment](RELEASE_RECONCILIATION_2026-11-25.md) ·
[Ordered implementation](RELEASE_RECONCILIATION_2026-11-25.md#ordered-implementation-and-decision-points) ·
[Source and review evidence](evidence/windows-indexed-cold-batched-copy-2026-11-25.json)

The [current architecture](architecture/CURRENT.md) shows the implementation.
The [source review](architecture/REVIEW.md) tests the assumptions and the
[revised architecture](architecture/TARGET.md) gives proposed product boundaries.
This plan maps those boundaries to the existing E01–E13 work breakdown and
[development ledger](DEVELOPMENT_PLAN.json).

The objective remains a complete Windows 11 x64 filesystem with ordinary
Explorer and Win32 access, useful automatic AMD work, independent CPU recovery,
very strong collective lossless compression and low-impact autonomous
maintenance. The owner makes the release decision.

The populated signed-driver fixture supports post-flush recovery and a fresh
writable mount. Independent hardware and SDK paths cover finite AMD batching,
controlled deferred integration and live indexed readback. Mounted inner
publication cuts cover Write, EOF shrink and rename. The CPU reader also
verified both image ranges through mounted selector cuts during reclamation.
Mapped-write recovery passed with cached neighbors and directory churn.
Packed relocation between allocation groups has a verified recovery model
and an ordinary NTFS guest-file trial. The helper creates packed backing
and reopens both supported formats. The mounted two-group fixture now
automatically selects a new image and crosses the trailing physical allocation
boundary. It preserves a held reader and passes independent CPU reopen and a
continued write. Repeated reuse and concurrent cached/mapped flush recovery
have now passed bounded packed and sparse guest traces. Extend that client
to populated churn and use the same trace to attribute attach, publication
and allocator costs. Admit AMD through the ordinary helper when measured
complete-path cost and consumed device output support the offer.
[Integrated sequence](RELEASE_EXECUTION_PLAN.md#immediate-implementation-order)
Keep independent CPU bytes, full occupied-size accounting and safe ownership
for each path. [Release gate ledger](RELEASE_EXECUTION_PLAN.md#current-gate-ledger)
· [Current state](DEVELOPMENT_STATE.json)

## 1. Decisions that govern implementation

1. **Keep the shared engine and WinFsp direction.** WinFsp supplies the Windows
   bridge; the filesystem engine supplies storage, compression and recovery.
   Explorer and ordinary applications use the mounted interface. The tray owns
   volume management and status. Consider a new kernel driver only after matched
   measurements demonstrate a material limitation in the user-mode architecture.
2. **Finish bounded indexed access.** Keep the checked inode, name and extent
   paths in the mounted engine. Move healthy reopen and remaining aggregate work
   to generation-bound admission after proving recovery equivalence.
3. **Measure total occupied bytes.** Include metadata, required bases/models,
   redundancy, padding and retained history. Report temporary/peak work and
   provider reservations separately. Do not accept a smaller payload counter
   as proof that a volume is smaller.
4. **Use fast ingestion and stronger later representations.** CPU fallback may
   encode differently and compress less. Every admitted representation remains
   independently CPU-readable. A GPU need not emit the CPU encoder's bitstream.
5. **Make grouping scalable and access-aware.** Use sparse content features and
   bounded candidate queries, not a dense file-by-file matrix. Logical groups
   may span one file, two files or thousands; representation and decode-work
   budgets govern access cost, not an arbitrary permanent member-count ceiling.
6. **Separate optional intelligence from required decoding state.** Correlation,
   tuning history and predictions may be discarded/rebuilt. Required bases,
   dictionaries, probability tables and model versions are immutable protected
   data referenced by the published representation.
7. **Make maintenance genuinely background.** Use actual workload/resource
   observations, OS priorities, bounded work, foreground preemption between
   quanta, persistent continuation and a write budget. Timers may pace already
   justified work; they must not cause repeated recompression of unchanged data.
8. **Keep GPU failure out of data authority.** Preserve owned immutable input,
   one completion decision, CPU selection before optional teardown, independent
   result validation and confirmed retirement before GPU reuse. Do not repeat
   discovery on every read/write or retry an integrity failure on a timeout.
9. **Preserve source and acknowledged bytes through change.** Publish new COW
   data and dependencies before switching roots. Retire old storage after reader,
   context, recovery and worker pins clear. Keep the current append-only
   validators with their format. Give reusable allocations durable incarnation
   identities and explicit capability negotiation.
10. **Treat claims as gates.** A callback test is not a mount; a mount is not full
    Windows compatibility; a RAM fault model is not physical power-loss evidence;
    a GPU result is not a benefit; a similarity score is not compression.

These decisions are compatible with new algorithms. They prevent an algorithm
experiment from bypassing the mechanisms that make its bytes usable safely.

## 2. Requirements and proof map

| Requirement | Required proof | Main packages |
| --- | --- | --- |
| Windows 11 x64, Explorer and ordinary applications | Current-source external-client mounted operations and a published semantics matrix | E08, E10, E13 |
| Automatic useful AMD GPU operation | Real GPU execution in ordinary I/O or collective work, matched complete-cost results and automatic eligibility | E03, E06, E12 |
| CPU fallback and independent reads | GPU absent/failing/disabled cases recover every admitted format and dependency without HIP | E03, E06, E09, E11 |
| Maximum practical lossless reduction | Held-out complete-size comparisons against strong matched baselines, with access/compute costs | E05, E06, E12 |
| Fast ingestion followed by stronger optimization | Separate ingestion, normal-idle and settled-state measurements; all stages read back exactly | E03, E07, E12 |
| Millions of files and variable-size groups | Populated named-file scale ladder, bounded resident state and sparse candidate-query cost | E01, E04, E06 |
| Offline self-learning with reserved internal state | Automatic decisions and continuation survive restart; corrupt advice does not block data reads | E04, E07, E09, E11 |
| Low background interference, including GPU work | CPU/GPU/memory/I/O and foreground-tail-latency traces under representative foreground load | E02, E03, E07, E12 |
| Host/installation independence | Capability discovery, changed-host tests, private dependency setup and relocation tests | E03, E07, E10 |
| Smart unavailable-device recovery | Evidence-keyed state transitions, bounded attempts and terminal unsafe states, not timer-only retry | E03, E11 |
| Power loss and incomplete writes | Acknowledgement oracle plus backend-specific interruption/reorder/tear campaigns | E02, E09, E11 |
| Repair without silent loss | Surviving-copy/dependency repair is verified; unrecoverable cases report failure instead of fabricated bytes or rollback | E02, E11 |
| Several-version backward readability and gradual updates | Frozen reader fixtures, mixed-generation mounts and interrupted COW migrations | E09, E13 |
| Drive-wear consideration | Physical writes, reclaimed bytes and rewrite amplification are measured and governed | E02, E07, E12 |
| Conventional filesystem advantages | Large files, directories, names, metadata, ACLs, sharing, caching/mapping and lifecycle tests | E01, E08 |
| Latest compatible dependencies and safe setup | Current primary-source inventory, reproducible manifests, advisory review and setup/rollback tests | E10, E11 |
| Objective recognized benchmarks | DiskSpd/WinFsp and appropriate codec controls, matched corpora/settings and published distributions | E12 |
| No host data damage or GPU-driver changes | Owned memory fixtures, no raw-device actions, no unrelated driver/security changes and contained retirement | All |
| No stale processes, services or scheduled work | Exact owned process/handle/thread cleanup, lifecycle tests and no unintended persistence | E03, E08, E10, E11 |
| No online model dependence in the finished filesystem | Disconnected operation and packaged CPU decoders/required parameters | E04, E06, E07, E10 |
| End-user documentation and helper | Product-oriented README/workflows; developer detail stays in technical guides | E08, E10, E13 |
| Owner-controlled release | Complete evidence audit followed by the owner's separate release decision | E13 |

### Reliability is defined by a failure model

The aim is no silent corruption or loss of acknowledged data within the verified
failure model. Specify which Win32 effects are acknowledged by the filesystem,
which can be held by the Windows cache or a mapped view, and what successful
flush and write-through operations commit. Run recovery against the independently
observed contract. A successful buffered API return alone cannot establish
device persistence. No implementation can promise survival of every future bug or
destruction of every copy. A temporary RAM volume cannot survive loss of its
power/process solely because it has checksums. Those limitations must not be
hidden behind the term self-repair.

| Failure | Intended response and qualification |
| --- | --- |
| GPU missing, busy, reset, bad result or incompatible runtime | Continue on CPU; reject unverified candidates; quarantine the failing configuration and require new evidence before reuse |
| Interrupted write within a qualified failure unit | Recover a valid committed state and every previously acknowledged byte; never infer flush guarantees from a format field |
| One damaged protected copy | Independently recover/check surviving data, repair through new publication and retain fault evidence |
| Damaged advisory state | Ignore/rebuild optional policy; do not reinterpret it as a decoding dependency or lose user-data access |
| Damaged required base/dictionary/model | Recover a protected copy or verified backup; refuse wrong bytes if none exists |
| Full or low-memory/full-volume condition | Refuse/defer new work before admission; preserve existing data and recoverable state |
| Host/process power loss for RAM backing | Contents are temporary; distinguish this capability from a qualified persistent provider |
| Kernel/driver fault campaign | Use an isolated expendable environment; never deliberately induce it on the owner's PC |

All owner-host filesystem experiment backing remains in memory under current
instructions. Source/build/tool/corpus/report files use scoped ordinary development
storage. Driver setup authorization is conditional on the established stability
gate; it does not authorize GPU-driver changes or destructive device operations.

## 3. Execution order and dependencies

The [revised architecture](architecture/TARGET.md) sets the target ownership,
format and resource contracts. The [release execution plan](RELEASE_EXECUTION_PLAN.md#next-integrated-qualification)
defines the first integrated trace and its receipt. The ordered checkpoints
below model package dependencies. E01–E13 work-package numbers and S1–S12
release gates provide the references.

| Checkpoint | Primary work packages | Required result before promotion |
| --- | --- | --- |
| 1. One instrumented Windows path | E01, E02, E03, E08, E11 | The helper creates and reopens packed backing. A populated trace ties application completion to commit, flush, cancellation and CPU recovery. Installed AMD, CPU-only and refused-AMD lanes exercise the same workload. |
| 2. Scale and reclaim under writes | E01, E02, E11 | Named namespace and restart curves, generation-bound validation, owned segments and a proven reclamation path preserve prior readers and acknowledged data. |
| 3. Reduce complete size and learn | E04–E07, E03 | Sparse candidate selection and protected encodings lower held-out total bytes with measured random access, resource and write costs. Learning survives restart and yields under contention. |
| 4. Qualify lifecycle and compatibility | E08–E11 | Independent volume ownership, supported backings, versioned CPU rescue, gradual migration and failure recovery pass their declared bounds. |
| 5. Compare and stabilize | E12–E13 | Recognized paired benchmarks, independent byte verification and soak evidence bind to the exact candidate before the owner decides. |

### Checkpoint 1. Establish the client and storage trace

- Retain the opened backing identity and physical extent through
  `WinFspVolumeFileSelection`. Start with the passing mounted boundary
  crossing and run additional overwrite/delete/regrow cycles through full
  settlement while readers pin earlier data. Record wait reasons, logical
  ownership, physical EOF and allocated bytes after each generation. Keep
  the detached native and sparse references and verify a fresh reopen and
  continued flushed write.
  [Relocation evidence](evidence/dense-cross-group-2026-11-23.json) ·
  [Mounted retirement and next steps](RELEASE_EXECUTION_PLAN.md#first-implementation-slice)
- Generate one exact manifest for source, toolchain, helper, WinFsp DLL and
  driver, AMD modules and runtime, data and backing. Record client-observed
  status separately from WinFsp response attempts. Add operation identity,
  affected range, indexed generation, lock/verification time, each publication
  and barrier, fallback reason and retirement to the trace.
- Reuse signed-driver [inner publication cuts](evidence/windows11-mounted-metadata-publication-cuts-2026-11-22.json)
  for Write, EOF shrink and rename and [outer selector cuts](evidence/windows11-mounted-selector-publication-cuts-2026-11-22.json)
  during reclamation. Keep independent exact readback of both retained image
  ranges, a verified next write and retired-range ownership checks. Reuse the
  [mapped cuts and directory churn](evidence/windows11-mounted-mapped-publication-cuts-2026-11-22.json)
  for a trace with concurrent cached writes and both view and file flush
  observations. Carry the same client and artifact ledger through each case.
- Wire qualified foreground offers from the installed owner to an ordinary
  deferred callback. Separate role-specific package inputs and verify the
  app can run without the optional worker. Reuse the existing background
  anchor and controlled finite uniform work as reference routes. Current
  matched writes spend most of the AMD response inside the cold worker. Test
  a finite readiness episode funded by real eligible backlog, with exact
  retirement and CPU service before readiness. Preserve the complete CPU
  path through stale observations, lost runtime, pressure and cancellation.
  Collect aligned multi-extent and partial write shapes from ordinary Win32
  operations before selecting batches. Compare complete usable time and
  persisted byte cost against the CPU path. Bind certificates to the runtime,
  adapter, module, shape and allocation state that produced them. Treat the
  supported client and the vendor's listed Windows build as separate evidence.
  [Foreground worker evidence](evidence/windows-amd-foreground-worker-boundary-2026-11-22.json)
- Run matching client workloads using the supported Windows paths. Include
  append, overlapping writes, Flush, truncate, rename, Cleanup and Close.
  Compare alternate order across CPU and eligible AMD trials on the live
  hardware. Exercise high-risk failures and mounted correctness in disposable
  environments. Time transfers, verification, storage and completion as well
  as the device operation. Defer wider concurrency until ordering and
  contention evidence justify it.

### Checkpoint 2. Make allocation, lookup and recovery grow with the workload

- Measure 1,024, 16,384, 100,000 and 1,000,000 named-file checkpoints where
  qualified capacity permits, with wide and deep directories, sparse files,
  pinned readers, writes and restart. Report work by component and peak
  resident bytes. Record a reproducible fixture or a specific resource
  limitation before making a claim for that tier.
- Split reopen costs into selector, tree read, full inventory, durable fence
  and handle exposure. Prototype generation-bound bounded admission alongside
  full CPU validation. Compare successful visibility and damaged-state
  refusal/repair. Specify off-path scrub coverage and time to detection.
  Keep the [checked post-fence read batches](evidence/indexed-fenced-read-runs-2026-11-25.json)
  as the current full-scan control. Record checked bytes and read calls by
  phase and compare packed and fragmented layouts under the same client.
  Log the first discrepancy between the proposed admission decision and the
  full audit with the exact selector, generation and touched range.
- Add segment liveness and durable retirement data while the append-layout
  reader remains available. Prove alternate/old readers, cancellation and
  changed-version costs using staged image handoff as the control. Test a
  segment evacuation without reuse first. Compare exact occupied and written bytes.
- Introduce reuse only under a separately negotiated capability with durable
  incarnation identities. Test old-reader access, outline ownership changes
  at every barrier, and replay allocation/repair cuts on both sides of reuse.
  Add sustained device-range churn and low-space failure after the ownership
  oracle passes.

### Checkpoint 3. Establish the complete-size compression frontier

- Freeze mixed real, similar-version, small-file, already-compressed and
  adversarially skewed input cohorts. Keep tuning and held-out sets separate.
  Calibrate the current grouping/index cost and independent LZ4/Zstd, exact
  sharing and grouped compressor controls. Measure output and occupied
  storage, with protection, dictionaries and retained objects included.
- Extend existing banded/projected postings to a versioned sparse candidate
  interface. Compare bounded content-defined and shifted features with a
  small exhaustive oracle. Measure recall, rescan work, catalog growth and
  persistent progress through churn. Revalidate every selected content and
  allocation version before publishing.
- Prototype independent frames with direct references, dictionaries, packed
  small members and standalone fallback. Select by complete settled bytes
  under observed cold-access and write-cost bounds. Add feasible fixed-point
  learned models as another candidate family. Any required model or table
  must travel through the same CPU-decodable durability checks as user bytes.
- Keep advisory results and policy independent of decoding. Learn which
  candidate, codec and AMD route fit each measured resource/workload regime.
  Record writes, canceled work, unexplored candidates and foreground
  interference. Exercise simultaneous compression, scrub, migration and
  reclaimer pressure across multiple volumes before enabling default policy.

### Checkpoint 4. Qualify an owned Windows product and supported formats

- Separate the foreground-started engine and tray using the same current
  WinFsp attachment and storage contracts. Prove reconnect, duplicate owner
  rejection, mount identity, stop, process exit, retained volume ownership and
  exact cleanup before choosing a supported deployment lifecycle.
- Qualify the actual signed driver and SDK on the general-availability
  Windows 11 26H2 x64 channel. Capture the servicing build. Carry mapped,
  cached, noncached and ordinary client operations through both supported
  backing workflows and the real recovery paths.
- Freeze historical CPU fixtures and required codec features. Test mixed
  layouts and bounded incremental upgrade with independent readback before
  and after publication. Include damaged advice, unavailable GPU, failed
  allocations, both-copy loss, compatible export and uninstall/rollback.
- Run the backend fault campaign against the declared stable-write model and
  qualified disposable persistent storage. Keep publication and independent
  restore receipts. Qualify a safe product installer and standalone CPU
  reader/export/package configuration for the same declared formats.

### Checkpoint 5. Measure and stabilize the exact candidate

- Use DiskSpd plus real Windows namespace and file workflows. Record a
  comparable backing/cache policy and full workload parameters. Pair Windows
  baselines with published codec and applicable filesystem controls, and
  preserve exclusions, variance and resource refusals.
- Report ingestion, cold and warm read/write, background settling, full
  occupied image size, access amplification, write amplification and host
  impact. Comparisons use the real host and exact CPU readback for each
  result. Treat each validated cohort and backing distinctly.
- Run the installed-client, migration, recovery and resource soak on the
  frozen artifact. Requalify any changed inputs to the relevant gates.
  Preserve raw results, remaining known limitations and the evidence for
  every S1–S12 gate.

Preparatory work can proceed in parallel within the dependency graph.
Integrated claims depend on the corresponding complete paths. This sequence
reuses the existing mounted and AMD controls instead of rebuilding the first
indexed or read-only prototypes.

### Next implementation tickets

Use one frozen application trace and select one production change per
decision. The expanded [decision table](RELEASE_RECONCILIATION_2026-11-25.md#decisions-to-carry-into-the-package-plan)
defines when to continue with admission and pacing, locality, AMD scheduling
or a new representation.

- **Mounted settlement.** Record admission, copy phase, verification,
  selector publication, retained references, provider operations and
  allocation in order. Reuse the independent client and CPU oracle.
  The latest signed replay published and reduced provider allocation to
  6,160,384 bytes while mounted. Detached inspection required no reclamation
  and verified the selected image. One maintenance failure and the missing
  quiescence observation keep E02 open. Attribute that failure to its exact
  reclaimer operation, exercise its recovery boundary, and repeat for clean
  idle settlement. Then measure real-host phase cost and admitted credit
  under foreground activity and quiet periods.
  [Mounted publication and failure evidence](evidence/windows-maintenance-reference-refresh-2026-11-25.json)
- **Useful device delivery.** Feed observed open stream ranges and
  generations to the existing owner. For read, write and collective work,
  compare CPU, cold GPU and a ready lease using identical observations.
  Count useful bytes delivered or committed, discarded bytes and retirement.
  Quantify resource cost across quiet and active gaps. Enroll profitable
  shapes on the actual runtime, driver and device tuple.
- **Compression and search.** Freeze versions and family splits before
  tuning the shortlist. Measure incremental gains from existing aligned,
  projected and rolling matches separately. For a small held-out subset,
  enumerate admissible direct-reference candidates and price them with the
  existing protection and placement rules. Use the gap between exhaustive
  and selected savings to choose a better search or a new representation.
  Compare total settled provider bytes at the same protection level.
- **Writable reopen.** Extend current verified reads and CPU audit with
  a read-only comparison of authority summary, dependency references and
  allocation ownership. Trace differences by checkpoint and touched
  pages. Compare old images, interrupted publications, and one damaged
  copy. Hold one feasible size tier out from estimation and report the
  resulting error before extrapolating to larger namespaces.
- **Product exits.** Bind package and CPU rescue to the exact candidate.
  Use interrupted upgrade and backend-specific recovery fixtures before
  representative performance claims. Measure DiskSpd and named-file
  workloads on installed hardware with verified Windows-native compressed
  and plain NTFS controls. Stabilize on the evidence-supported Windows
  and backing matrix before the owner's release decision.

Use the reconciliation's current sequence to decide which product change
earns implementation. The next test should reuse the populated operation
witness, separate ownership from provider allocation and explain where the
ordinary controller stops. Then use one paired client trace to price automatic
AMD service and a protected cohort oracle to identify compression gains.
Plan the writable-attach differential independently with its complete-audit
reference and a held-out file-count tier.
[Current priorities and decisions](RELEASE_RECONCILIATION_2026-11-25.md#ordered-implementation-and-decision-points) ·
[Next bounded work by package](RELEASE_RECONCILIATION_2026-11-25.md#package-handoffs)

#### E03 and E08. Qualify ordinary AMD foreground I/O

1. **Acquisition and retirement.** Use the one owner in
   [WinFsp controller](../src/windows/winfsp_controller.cpp). Extend its
   installed-role acquisition to uniform writes. Use cancellation through
   [installed discovery](../src/windows/hip_route_discovery.cpp) and the
   existing worker supervisor. Distinguish lack of a winning workload,
   transport contention, pressure, cancellation, configuration change,
   untrusted results and unconfirmed retirement. Reuse the existing qualified
   foreground offer. Confirm CPU requests complete during discovery and while
   cancellation or worker retirement is in progress. Use a fake transport
   and missing-package control before testing exact hardware.
2. **Observe useful demand.** Publish a bounded observation from
   [ordinary writes](../src/windows/winfsp_native.cpp) through the controller.
   Capture alignment, request size, write mode, stream cadence, foreground
   compression options, backing isolation unit and observed CPU write cost.
   Add workload signals for compressibility and locality using measured
   sampling overhead. Keep ownership of captured bytes explicit in
   calibration; release buffers on veto, stale demand or shutdown. Count
   aligned and rejected requests, along with sampling cost. Validate zero
   worker launch on normal CPU-only, busy and unsupported shapes.
3. **Calibrate with a complete operation.** Keep the controller's verified
   CPU and prepared Radeon replays bound to one immutable group and the
   approved package. Expand from the first completed pair to repeated
   independently decoded native
   transactions over identical immutable bytes and equal protection, initial
   volume shape and exact foreground policy. Rotate first-use order. Record
   launch/setup/transfer/wait/readback/verification/retirement/publication,
   as well as total elapsed and occupied bytes. Keep complete-cost sampling
   separate from launch timing for funding. Preserve partial, fallback and
   failed trials. The [existing policy](../src/windows/native_amd_write_policy.cpp)
   is the control for eligibility. Require a positive matched result and
   an independently funded complete window before offering a route.
   [Paired owner evidence](evidence/windows-owner-amd-write-pairs-2026-11-23.json)
4. **Consume and invalidate.** Bind a measured offer to package/driver/runtime,
   adapter, role, backing and compatible demand. Validate live headroom and
   every candidate before authoritative publication. Expire or replace an offer
   on material workload, runtime, pressure or topology change. Keep terminal
   per-request fallback and exactly one accepted publication/response.
   Replay refused fetch, malformed reply, cancellation, two competing offers,
   fault before acknowledgement, unavailable accelerator and clean reacquisition.
   Freeze a Windows client manifest and report accepted device extents,
   reads delivered from independently verified device decode, and collective
   outputs that change the selected image. Keep CPU recovery independent of
   runtime installation, GPU encoder choice and persisted learner state.
5. **Use measured losses.** If foreground remains CPU-preferred, improve the
   cost stage that dominates on the paired hardware. Extend the prepared
   stream from one exchange to a finite funded burst with backpressure, fresh
   admission for each group and a stop condition driven by demand and
   contention. Check exact group ownership, CPU verification before byte
   use, and retirement on arena exhaustion, foreground demand or stop.
   Compare useful extents per exchange, setup amortization, immutable slab
   reuse and verified staged prefetch. Use larger opportunities only when
   fresh quotas fund the full operation and its retirement. The read lane evaluates whole
   delivered latency and useful prefetch rate. The collective lane evaluates
   verified net bytes saved and foreground interference. Do not infer a win
   from kernel throughput or from a stored extent with higher complete cost.
   A diagnostic losing shape remains a regression control. E03 exits only
   with useful automatic mounted work for advertised roles and CPU continuity.
   The experimental version 6 transport now supports admission of separately
   sealed writes, including a new write captured after the device is ready.
   Compare the approved arrived-write route against complete CPU work and
   fresh GPU workers under paired real workloads. Record the first readiness
   gap, later request latency, cancellation, memory and power while waiting,
   and byte-exact CPU recovery. Qualify additional request classes for the
   installed package using that evidence.
   [Current preparation baseline and warm-session criteria](AMD_RUNTIME_DISCOVERY.md#keeping-the-hip-worker-ready-during-active-work)

#### E01, E02 and E11. Replace the global startup bottleneck safely

1. **Use one independent trace.** Extend the existing mounted mixed-flush
   case in [Windows application tests](../tests/windows_volume_app_tests.cpp).
   Record client request ordering and successful barriers. Run paired packed
   and sparse fixtures with fixed initialized data, mixed small/large files,
   held readers and cache flags. Sweep short-file populations through feasible
   geometrically spaced tiers. Reserve at least one feasible tier and one
   different corpus for validation. Capture calls, bytes, cache traffic and
   writes at the selector, image, catalog, inode and payload edges. Continue
   from independent CPU recovery after helper exit. Export machine-readable
   per-operation results with labels for native, mounted and physical evidence.
2. **Observe before using faster admission.** Define the exact accepted root
   pair, selector generation, backing identity and geometry, active lease,
   allocator incarnation and dirty range coverage. Put the candidate attach
   decision beside the complete CPU checks in
   [native session](../src/native_session.cpp) and
   [indexed inventory verification](../src/native_indexed_volume.cpp). The
   decision runs in comparison mode while every attachment still uses full
   authority checks. The current complete scrub observation remains advisory
   and local to its exact checkpoint. It cannot stand in for signed-off
   namespace/allocator coverage or for verification after a new lease. The
   current complete audit groups mirrored reads on both sides of the fences.
   Use that implementation as the comparison baseline. Attribute the remaining
   read calls to namespace resolution, object verification and shared bases
   before choosing which metadata path to change.
   [Decoded reopen evidence](evidence/indexed-decoded-reopen-runs-2026-11-25.json)
3. **Check touched dependencies and reconcile.** Prove page and payload
   verification before exposure, including acquired reader pins and both
   protection paths for newly written data. Exercise dirty shutdown, bad peer
   and bad root, generation or selector replacement, changed backing, old
   format, missing progress, concurrent mutation and damage outside the first
   requested path. Compare selected generation, reads, error handling,
   bytes and invocation counts with the reference. Any difference with
   unresolved ownership or durability keeps full admission. Fit separate
   predictions for lookup page growth, attach inventory reads, coherence
   candidates, peak memory and reuse under churn. Check the predictions against
   held-out measured tiers. Report prediction intervals and the assumed file
   size, directory fan-out, data locality and hardware envelope when projecting
   to millions or tens of millions of files. Keep observed ranges visible
   beside projections.
4. **Reclaim those same ranges.** Add reusable allocation incarnations behind
   explicit format capability. Record extent, current owner, last reader and
   rollback pins and retire sequence in the same COW authority as new
   references. Model crashes before and after each range state change.
   Extend the [native backing](../src/native_volume_file.cpp) and
   [paged store](../src/paged_store.cpp) tests for near-full churn,
   pinned generations, truncation, cancellation and partial provider
   release. Verify allocation conservation and monotonic settled size
   for a fixed live set after rebuilding and reopening. Keep the checked
   rebuild as the oracle/provider fallback.

#### E04 through E07. Establish the complete-volume compression frontier

Start with C1 and C2 in the
[settled-size implementation sequence](RELEASE_RECONCILIATION_2026-11-24.md#first-settled-size-implementation-sequence).
The matched level-3 and level-9 controls already show distinct results for
CPython revisions and AIT. Measure their interaction with grouping on
held-out files under the same writable protection and geometry. Price
protection changes in their own paired runs. Use complete savings and
rewrite/read costs to choose between independent cold re-encoding and
deeper similarity work for each data family.

1. **Build the accounting and recall oracles.** Freeze content manifests,
   revision groups and access schedules before tuning. Keep AIT A–H and I–P
   as distinct named partitions and keep an additional corpus/revision
   holdout independent of AIT pilot work. Count canonical object and
   dependency bytes, namespaces, indices, models, mirrors, retention and
   provider allocation. Report peak temporary bytes and write amplification
   separately from settled size. Verify every file and a held-out set of
   random ranges with the independent CPU reader.
2. **Build scalable candidate discovery.** Extend the
   [indexed coherence catalog](../src/indexed_coherence.cpp) from current
   versioned postings. Reconcile by immutable extent/content ID and revision,
   share exact equality only after confirming bytes, and use sparse bands
   and bounded range lookups for similarity. Measure exact-oracle recall
   on small cohorts and held-out savings on large cohorts. Page group
   membership and resumable cursors, retire stale postings incrementally
   and throttle frequent-key fan-out. Let one, a few or thousands of files
   share a grouping if attractive. Use independent restart frames and
   versioned dependencies to limit the work required for one file range.
3. **Test representations before adding disk methods.** Compare current
   packing, base/residual and strong per-frame controls with content-defined
   matches, dictionary generations, structure-aware residuals and optional
   specialized stages. Vary frame size, group size and dependency sharing
   independently. Use the [current group codecs](../src/native_group_codec.cpp)
   and fully charged protected roots as controls. Select the best complete
   settled size among candidates that pass the declared range access and
   resource envelope. Preserve incompressible, encrypted and already
   compressed input as realistic controls. A learned codec must earn its
   protected model and compute cost and provide a versioned deterministic
   CPU reader before it can become a stored method.
4. **Promote the tested format and offline policy.** Introduce only candidates
   that pass size and access checks into the versioned CPU decoder registry.
   Commit protected dependencies and self-contained frame references with
   checked source revisions and reader lifetimes. Off-path preparation
   must surrender publication rights when a selected source changes.
   Learn which experiments to try and when to stop, using a small
   rebuildable local learner and a deterministic baseline. Persist counters,
   sampled features, policy version and progress in bounded advice objects.
   Evaluate workload drift, restart, advice corruption, false positives,
   low resources and two-volume contention. Report foreground overhead,
   saved bytes, time to settle and actual bytes written.
5. **Fund all autonomous maintenance together.** Run compression, scrubbing,
   reclamation and migration through the same host/device admission and
   per-volume resource accounting. Use live physical/commit/VRAM budgets,
   CPU/GPU utilization, backing pressure, measured work cost and
   foreground changes to admit bounded quanta. Share quotas across volumes;
   protect readers, verify cancellation and track recipe-specific IO cost.
   On HDD compare seek penalties and on flash record host writes and avoid
   speculative rewrite churn. Observe actual pressure and capability by
   backing rather than inferring policy from a drive label. Produce a
   quiescence and resource-retirement report for each mounted profile.

#### E08 through E13. Qualify and stabilize the thing users would install

1. **Windows and provider contracts.** Exercise Explorer, common Win32 file
   editors, directory enumeration, sharing, mapped and cached data, durable
   flush, long/Unicode names, case matching and all advertised metadata.
   Explicitly decide which additional Windows semantics become supported.
   Apply the same contract to memory and existing-volume image backings.
   Stage and verify missing user-mode dependencies with isolated scope,
   transaction logs and rollback. Keep the approved kernel/display driver
   state. Qualify the tray's create/mount/detach, recovery presentation,
   offline install, relocation, upgrade and uninstall with signed artifacts
   on the supported Windows client. Reuse FlitzZip's installed discovery
   and dependency staging patterns where the filesystem's acknowledgement
   and ownership contracts permit. Use byte and resource tests for each
   additional provider before adding it to the support matrix.
2. **Compatibility and restore.** Freeze historical readable fixtures and
   their CPU decoder/dependency manifests. Test the proposed current
   plus three preceding releases once those versions exist. Include
   mixed-version roots, optional codec absence, unknown mandatory
   features, staged upgrade under an older reader, interrupted selection
   and recoverable rollback. Pin the data under upgrade until the
   authority is verified. Ship an independent CPU inspection/export/repair
   path with no accelerator dependency. Recover from surviving copies or a
   separately verified restore source. Maintain an explicit failure model
   for cases where every valid copy is lost.
3. **Failure qualification and exact-candidate proof.** With the same
   acknowledgement oracle, test data/metadata barriers, out-of-space and
   device I/O errors, torn selectors, missing dependencies and faulty
   retirement in isolated environments. Collect physical durability
   evidence on disposable independent backings before a persistent
   durability claim. Run benchmark, lifecycle and long soak work on the
   exact supported candidate. Use Microsoft DiskSpd, WinFsp metadata and
   correctness tools, and the public workloads specified in the
   [benchmark protocol](BENCHMARK_PROTOCOL.md). Compare plain NTFS and
   built-in NTFS compression; verify every compressed control file's state.
   Keep guest results for correctness and collect comparison timings on
   the real host. Record every open gate, unsupported path and material
   measurement uncertainty in the owner decision packet.

## 4. E01 — Scalable native inode, name and extent storage

**Reuse:** `PagedObjectStore*`, typed heads, native mutation/session contracts,
inode/name serialization experience, snapshots and malformed-input tests.
**Replace:** whole `Model` residency/copying, aggregate 64-KiB namespace assembly,
data IDs restricted to the flat object's 256-ID space and whole-namespace checks
on unrelated reads. **Planning effort:** 6–12 person-weeks; confidence low.

The normal Windows session path lookup now obtains allocation, basic metadata
and security from a single verified namespace read. The matched flat fixture
read 985 backing bytes, down from 3,940. The paged fixtures matched one verified
inode query at both tested failure units. Step 11 still includes replacing the
remaining whole-namespace pass with bounded page lookup. [Lookup evidence](evidence/native-path-lookup-2026-11-17.json)

Current indexed reads now verify the persisted directory against the checked
name map in place. Pagination emits the requested range directly, and native
session revalidation compares each inode record individually. The standard
WinFsp lifecycle test uses sealed managed memory and verifies the committed
bytes through callbacks. [Directory verification evidence](evidence/native-directory-verification-2026-11-17.json)

Live inode-record queries now verify the bound root and selected path, inode
and extent records. The cache ends with each query. The indexed 132-inode
fixture read 17,655 backing bytes for a name lookup. Complete namespace
validation read 2,262,944 bytes. Full mutation and recovery checks remain in
place. The writable directory callback now consumes checked name and metadata
pages sized to the Windows output buffer. The 39-child fixture verified stable
identity and exclusive continuation through the real SDK packing helper. The
new namespace can use this session interface for demand-loaded name and inode
records. Existing mounted layout limits still apply.
[Scoped read evidence](evidence/native-scoped-read-2026-11-17.json) ·
[Directory page evidence](evidence/native-directory-pages-2026-11-17.json)

Paged publication writes each changed inode frame directly. Compaction
and grouped writes generate the namespace root independently. Paged reads
decode one inode frame at a time and reuse one inventory for validation.
The legacy contiguous layout shares the canonical inode codec. The existing
format caps the namespace at 256 inodes. The next layout needs separate
capability negotiation, independently checked inode, directory and extent
lookup pages, and an explicit migration from existing images.
The Btrfs developer documentation provides a current reference for grouping
metadata by stable inode IDs and indexing directory entries separately.
[Serializer evidence](evidence/native-inode-serialization-2026-11-17.json), [Btrfs trees](https://btrfs.readthedocs.io/en/latest/dev/dev-btrees.html)

The object-backed [ordered name index](NATIVE_PAGED_BACKEND.md#ordered-name-pages-for-the-scalable-namespace)
implements checked name lookup, keyset enumeration and atomic edits for E01.
The Windows fixture exercised 1,511 name bindings, and 63 interrupted renames
retained complete old or new checkpoints. The compacted fixture occupied
655,360 bytes after rebuilding from 6,004,736 bytes. These measurements give the
typed-page comparison a baseline for footprint and read amplification. The
new namespace still needs its own volume capability and normal session access
before it can replace the existing limits.
[Name-index evidence](evidence/native-name-index-2026-11-17.json)

The [indexed native snapshot format](INDEXED_NATIVE_VOLUME.md) now connects
the ordered index and sparse extents to the existing snapshot API. The
1,024-file fixture checked every named nonempty file and exercised high
logical offsets. Explicit legacy export verifies the destination before
publishing its root and leaves the source selected. The first scale rung
now has native reader evidence.

The native path writer now uses that layout for creation, sparse writes,
truncation, rename, deletion and metadata updates. Direct initialization and
verified resume use the same on-demand index. The 1,024-file write fixture
verified byte readback and a retained checkpoint. A 231-case simulated storage
interruption sweep reopened complete old or new checkpoints. The test build
also runs the in-memory WinFsp worker cases on ordinary local SDK builds.
[Indexed writer evidence](evidence/native-indexed-writer-2026-11-18.json)

### Next steps for the writable Windows path

1. Carry the indexed path through Explorer and helper lifecycle. The owned
   memory provider now supports indexed creation, startup rollback,
   independent CPU inspection, closed-session sealing and writable remount of
   the verified retained image. It accepts an explicit provider and plan.
   The experimental tray now invokes that owner for attach, detach and
   reattach. Verify real Explorer operations and the signed Windows 11
   driver/runtime pair before enabling ordinary provisioning. Measure the
   full CPU verification cost on large retained volumes. Keep
   earlier writable images readable and test their actual reopening.
   Indexed WinFsp startup now validates each reachable inode's Windows
   metadata during the initial audit and uses that same result to retain
   the selected backing. Profile the two remaining storage verification
   passes on larger mounted fixtures. Account for selectors, object and
   directory pages, inode records, payloads and dependencies separately.
   Bind any replacement proof to the exact selector, checkpoint, namespace
   sequence and permitted backend mutation model. Preserve refusal for
   corrupted payload peers and same-generation replacement at either barrier,
   and retain an independent CPU rescue path.
   [Indexed owned path](evidence/indexed-owned-windows-2026-11-18.json) ·
   [Windows startup evidence](evidence/winfsp-indexed-startup-audit-2026-11-20.json) ·
   [Helper attachment audit](evidence/release-reconciliation-attachment-2026-11-21.json) ·
   [Memory backing evidence](evidence/demand-locked-memory-2026-11-18.json) ·
   [Tray attachment](WINDOWS_VOLUME_APP.md#explorer-attachment)
2. Qualify mounted delete intent, detached I/O, final Close and restart through
   an isolated external Windows client. The indexed SDK fixture now proves
   preservation of detached data and closed-session recovery with its
   synthetic owner. Measure behavior with multiple handles and mappings,
   replacement of open destinations and actual FSD-granted access.
   [Indexed lifetime evidence](evidence/native-indexed-lifetimes-2026-11-18.json) ·
   [Windows disposition rules](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/4c26c8cb-5a8f-4339-a372-b315c4974131) ·
   [WinFsp interface contract](https://winfsp.dev/doc/WinFsp-API-winfsp.h/)
3. Extend indexed retirement to live truncation and physical reuse. The owned
   low-priority controller now consumes closed-file retirement in bounded
   transactions admitted by fresh CPU and memory observations. Add durable
   visibility boundaries for shrinking a live file without scanning its whole
   tail on the foreground path. Verify shrink-then-grow, write-after-truncate,
   queue fairness and survival of earlier roots. Track retired mappings and
   physically reusable bytes.
4. Drive staged indexed copies from the low-priority owner and admit qualified
   physical reuse. Capability-3 head selection now binds the indexed capacity,
   independent native sequence and durable checkpoint floor. Managed compaction
   preserves pinned roots and open file identities in the tested fixture.
   Measure copying and final selection interference at scale. Preserve
   collective dependencies through an explicit versioned migration with
   verified rollback and matched complete-size comparisons.
5. Qualify paged enumeration, allocation, ACL queries and the Unicode path
   profile in real Windows clients. Tracked directories now validate moves
   from transactional summaries. The WinFsp owner backfills older directories
   in idle pages and retains completed summaries as checkpoints. Measure
   backfill writes, mount interference and restart cost on wide legacy
   directories. Carry the migration through the mounted Windows client tests.
   [Automatic backfill](INDEXED_NATIVE_VOLUME.md#automatic-backfill)
6. Connect the controlled indexed deferred write path to normal mounted
   workload selection. The indexed writer checks candidate content against
   the staged extent and publishes the inode and block in the verified LZ4
   frame. The independent CPU reader can reopen it. Measure end-to-end
   costs and occupied bytes on matched inputs, including setup, transfer,
   verification and retained storage. Cover foreground contention, device
   loss and low-priority collective dispatch before choosing automatic
   selection rules. [Indexed callback and codec evidence](evidence/indexed-lz4-deferred-2026-11-18.json)

### Implementation steps

1. Specify native catalog roots for inode lookup, `(parent,name)` lookup,
   per-file extent maps, allocation state and dependency references. Establish
   64-bit identity/range arithmetic and reserved ID domains before adding bytes.
2. Design immutable checked tree pages with explicit counts, lengths, ordering,
   generation and digest bindings. Validate framing before allocation or child
   traversal. Give names ordered keys by parent and normalized name and extents
   ordered keys by inode and logical range. Compare typed slotted pages with
   an object-backed index in a bounded trace before freezing the layout. Document
   capability negotiation and rejection of unknown formats.
3. Introduce a paged native access interface behind the existing session API.
   Keep legacy readers explicit. Do not maintain a second user-visible filesystem
   or turn a paged backend into another fully materialized map.
4. Implement point inode lookup and path traversal through bounded checked pages.
   Cache by immutable root/page identity and generation. Bound negative results
   to their snapshot and account for page reads, decoded metadata and allocation
   at each lookup.
5. Implement ordered name lookup and pagination without loading an entire
   directory. Define cursor snapshot identity, mutation behavior and retirement.
6. Implement per-file extent range lookup, holes, zero ranges and reservation
   accounting. Large logical sizes must not require a proportional in-memory map.
7. Build a small typed transaction overlay for only touched pages/inodes.
   Atomically publish name/inode/extent/free-space/dependency-root changes.
8. Preserve create/open/overwrite/delete-pending/cleanup/close behavior and
   stable file IDs. Define ID reuse only after every relevant stale reference
   can no longer resolve to a different live object.
9. Implement rename/move with bounded ancestor validation and atomic replacement.
   Reject cycles and preserve open-handle visibility without subtree rebuilding.
   Verify source and destination names and parents against the same committed root.
10. Store and validate cached counters for usage/status. Prepare fallible result
    data before publication; do not add post-acknowledgement I/O/allocation.
11. Replace per-read global validation with checked lookup paths and verified
    payload access. The Windows owner checks untouched payload replicas
    incrementally on populated indexed attachments. Persist the scan cursor
    and qualify recovery from damaged index pages.
12. Add a streaming legacy-to-new-layout migration into a new target and verify
    data, namespace and required opaque metadata before COW selection.

### Acceptance and failure checks

- Measure named files and mixed directory layouts with the
  [native scale protocol](RELEASE_EXECUTION_PLAN.md#2-large-indexed-volumes-and-foreground-io).
  Include a wide directory and populated reads beyond cache capacity. Record
  the backing and actual workload for every measured rung. Test the forecast
  against a held-out rung and declare which quantities are extrapolated.
- Include small/large/skewed directories, long names, large populated and sparse
  files, overwrites, rename/delete/truncate and restart. Record object counts
  separately from file counts.
- Bound resident metadata/caches independently of total files. Show lookup,
  creation and enumeration costs rather than only successful final reads.
- Inject failures across multi-index publication; verify the independent
  acknowledged namespace/data oracle and no allocation/reference leaks.
- Demonstrate that changing one file does not require reading or rewriting every
  other inode. No larger constants or sparse-only test closes E01.

**Output:** format/access contract, implementation, migration reader, scale and
fault evidence. **Existing ledger:** S01. E02/E04/E06 require this storage model
for production-scale completion, but their bounded design/experiments can start now.

## 5. E02 — Allocation, dependency ownership and automatic reclamation

The callback model admits consecutive confirmed packed EOF truncations
within a measured idle window. Each step checks the selector, allocator and
provider contract. The staged rebuild now packs adjacent unchanged mirrored
records into bounded writes with immediate replica and CPU verification.
Choose between scheduling, placement and local range reuse after a normal
mounted run accounts for each phase and actual provider allocation.
[Implementation sequence](RELEASE_RECONCILIATION_2026-11-25.md#1-close-ordinary-mounted-settlement--e02-e08-e11) ·
[Copy checks](evidence/windows-batched-staged-copy-2026-11-25.json) ·
[Packed continuation checks](evidence/windows-packed-tail-continuation-2026-11-25.json)

**Reuse:** mirrored payloads, COW records, protected anchors, staged rebuild,
context-preserving head handoff and source-preserving recovery.
**Available:** bounded indexed handoff, automatic staged compaction and
exact sealed selection on owned Windows memory and modeled paired ranges.
The paired selector describes each candidate's range geometry and preserves
CPU reopen of prior mirrored layouts. Private preparation uses work credit within a
fresh CPU sample. Publication and retirement each require a fresh observation.
Indexed compaction can preserve its private work across foreground commits by
reconciling immutable tree checkpoints. A verified live repair can use that
handoff to restore payload redundancy when append admission fails.
The packed ordinary-file provider evacuates live tail units within and
across groups. A mirrored relocation intent binds interrupted directory
updates. Both the native NTFS fixture and the signed mounted helper reached
a group boundary with an earlier reader and a continued write after CPU
recovery. The mounted test now verifies release independent of optional
compression admission. Its first boundary crossing leaves full settlement
to be measured.
[Cross-group evidence](evidence/dense-cross-group-2026-11-23.json) ·
[Mounted retirement](evidence/windows-mounted-packed-retirement-2026-11-23.json)
**Missing:** pageable recovery at scale, durable progress across longer
maintenance jobs and persistent-provider qualification.
**Planning effort:** 5–10 person-weeks; confidence low.

### Implementation steps

1. Define allocated, live, reclaimable, pinned, reserved and temporary bytes.
   Make every compression/status/benchmark consumer use the same accounting.
2. Persist free-space, object placement and dependency ownership changes with
   the namespace update. Assign reused storage an allocation incarnation and
   validate range, identity, generation and tree level on dereference. Existing
   append-order pages keep their original checks and format capability.
3. Pin roots and backends for readers, contexts, workers and recovery anchors.
   Establish the oldest reachable allocation incarnation before freeing or
   discarding a unit. Make pin registration atomic with acquiring its root,
   including independent CPU readers and stale queued work.
4. Specify reserve behavior for foreground writes, metadata/repair and candidate
   staging. Full-volume failure must not require deleting existing user files.
5. Connect staged rebuild and verified handoff for bounded, quiescent migration
   or repair. Preflight peak space and use the existing exact-generation guard.
   The memory path now keeps one private candidate, charges abandoned image
   writes and validates both anchors at selection. The paired sparse file
   provider publishes a smaller verified image, protects the previous range
   while snapshots pin it and releases allocation after confirmed selection.
   Recovery identifies out-of-selection ranges from the head's epoch.
   Preserve private staging across foreground changes by reconciling
   checkpoint differences. Persist restart progress and measure interference
   during mounted Windows 11 activity. Record logical retirement, backing
   relocation and physical release as separate events with their admissions,
   reserved bytes and remaining work. Verify repeated overwrite, deletion
   and regrowth at equal live size against the detached and sparse controls.
   [Persistent range evidence](evidence/windows-sparse-volume-reclamation-2026-11-19.json)
6. Implement steady-state segment evacuation with bounded units and a persistent
   liveness index or equivalent reverse mapping. Select a victim using its live
   bytes, pins and expected write cost. Copy verified live objects, recheck each
   object's incarnation, publish the new location and recycle after old
   references retire. Unrelated writes must leave the scan's progress intact.
7. Independently verify CPU data and all referenced dependencies, flush the
   candidate, publish the selected head in the correct order, then retire old
   storage only after pins clear. Preserve the source on every ambiguous failure.
8. Recover interrupted moves and migrations from validated allocation,
   dependency and progress roots. Exercise crashes before the copy, after copy
   but before publication, after publication while a reader pins the old copy,
   and during retirement or reuse. Count orphaned output against staging space.
9. Extend the memory provider's observed-obsolete-byte trigger to durable
   segment liveness. Keep measured gain and resource admission in the decision.
   Repeated unchanged opportunities must neither append records nor allocate
   another candidate. Charge failed attempts and retained readers against
   capacity and write-amplification budgets.
10. Measure padding, metadata write amplification and protection cost for tiny,
    packed and bulk writes. Compare time- and size-bounded batching, packed banks
    and smaller backend-qualified failure units. Evaluate mirroring and parity
    against the same fault model and independent CPU repair requirements. Preserve
    acknowledgement and isolation semantics through batching.
11. Add layout/defragmentation selection based on access locality, medium traits
    and net benefit. Flash does not need a periodic HDD-style full rewrite.
12. Measure source reads, bytes written, temporary high water, retired bytes and
    rewrite amplification. Keep the counters stable across sessions. Attribute
    required metadata and protection writes to their logical operation, and
    count rebuildable similarity checkpoints separately. Verify independent CPU
    recovery across every new ordering boundary.

### Acceptance and failure checks

- Repeated write, optimize and delete cycles reach a bounded steady footprint.
  Occupied bytes fall after useful grouping and safe retirement. Test a mix with
  continuous unrelated commits, a long-lived reader and scarce staging space.
  Compare write counts and total occupied bytes for one tiny update, a batch of
  tiny updates and a large sequential write.
- Keep live readers and open file contexts across grouping and handoff. Verify
  reads before/after selected and unrelated edits and deletion of base filenames.
- Interrupt every relevant publication phase, including head/advice/dependency
  transitions; independently recover every prior acknowledged byte.
- Pressure free space and memory. No candidate may consume emergency reserves
  or recycle an uncertain free. Stop leaves no stale worker or lease.
- Corrupt each protected copy independently. Repair into new verified state;
  failure of all available copies is detected, not hidden by invented data.

Current storage research separates reclaimed bytes, data movement and write
amplification. FlitzFS uses this distinction to qualify a smaller selected
image and a later unpinned release independently. Segment selection should
measure the surviving bytes it must copy and use liveness rather than time
alone. [FAST '26 discard and compaction](https://www.usenix.org/conference/fast26/presentation/bian) ·
[FAST '26 data placement](https://www.usenix.org/conference/fast26/presentation/kim-jeeyun) ·
[Tidehunter preprint](https://arxiv.org/abs/2602.01873)

**Output:** allocator and pin protocol, automatic segment reclamation,
bounded staged handoff, recovery and write amplification evidence. **Dependencies:** E01 for scalable completion.
**Existing ledger:** S01, R01. This closes a core size claim, not just a tool feature.

## 6. E03 — Useful automatic AMD execution and CPU continuity

**Reuse:** HIP identity/module verification, DXGI budget/topology collection,
bounded workers, recovery state machine, CPU validation, uniform LZ4 controller,
native deferred responses and retirement accounting.
**Planning effort:** 4–9 person-weeks, with hardware/research variance.

The installed-package checkpoint verifies relocatable worker and module
acquisition and matches the actual HIP LUID against eligible DXGI candidates.
Three alternating 512 KiB AMD/CPU trials produced the same occupied image
size. CPU completed in 1.21–1.60 ms and the one-shot AMD route in
703–1,331 ms. The earlier phase fixture measured 1.19 s of initialization
in a 1.37 s worker exchange, with 2.57 ms of kernel wait. These results keep
the tested foreground request class on CPU. Finite reuse subsequently
processed 1 MiB per worker under a 128 KiB launch limit. The public source
sample still favored CPU for both time and occupied bytes. Use the finite
worker to explore cross-file similarity and stronger representations while
keeping foreground selection tied to workload measurements.
[Runtime acquisition](AMD_RUNTIME_DISCOVERY.md) · [Finite exchange evidence](evidence/finite-amd-exchange-2026-11-18.json)

The worker now reuses a scoped D3D12 device during the zero-node-mask setup
checks and retires it before HIP stream and slab creation. Three matched
live pairs with the same kernel module reduced median 1 MiB exchange time
from 1.45 s to 1.26 s. Setup timings varied. Use whole-exchange costs when
sizing finite cross-file work. Measure complete occupied bytes, foreground
interference and retirement, including cohorts where the stronger encoding
does not pay for its setup.
[Topology lifetime and matched worker evidence](evidence/amd-topology-reuse-2026-11-18.json)

The wide packed-XOR setup profile separates stream creation from slab
allocation. On the physical Windows host, median stream creation took
1.213 seconds for 24 members and 1.215 seconds for 101 members. Median slab
allocation took 24 and 23 microseconds, respectively. The six measured
exchanges completed with verified GPU candidates, matched CPU costs and
confirmed process retirement. The complete AMD exchange still exceeded CPU
encoding and verification in both cohorts. These are host call timings for
one adapter. Prioritize more valuable collective search per admitted stream
and compare complete savings and elapsed cost before automatic selection.
[Split setup measurements](evidence/amd-anchor-stream-setup-2026-11-19.json)

Indexed native writes now use one CPU comparison of the standalone GPU
compression candidate before prefixing authoritative extent metadata. Mirrored
readback still precedes publication. The physical-host acceptance profile
measured a 7.0% median paired improvement. Whole-write comparisons still
favored CPU for both 1 MiB inputs. Keep this class on CPU and evaluate finite
AMD work on groups where savings amortize worker initialization. Include
stale-budget outcomes in qualification, even when CPU recovery succeeds.
[Acceptance and paired write evidence](evidence/native-lz4-advice-acceptance-2026-11-19.json)

The installed native LZ4 read role acquires the approved decoder from the
volume controller on a cadence-eligible demand. In Windows SDK callbacks it
returned two hardware-decoded frames. The earlier 5.438-second turnaround
included the fixture's additional CPU cap. A separate control held the
binaries fixed across the two cap settings. Its supervisor-only trials
completed at 1.000 and 1.016 seconds. Cold worker traces place most of the
remaining cost in HIP device initialization. Device-side guard fills avoid 131,136 upload bytes per
two-frame batch. These samples leave a speed advantage unqualified.
[Installed native read evidence](evidence/installed-native-amd-read-2026-11-22.json) ·
[Host transfer and lifecycle evidence](evidence/native-amd-read-lifecycle-2026-11-22.json)

Compare larger, finite prepared windows against CPU on the same checkpoint
with the same supervisor limits. Include initialization, device fills,
launches, readback, independent verification and confirmed retirement in the
cost. Measure delivered bytes, abandoned candidates and foreground tail
latency before admitting a production profile.

### Implementation steps

1. Build a trusted configuration identity from OS/architecture, adapter LUID/node,
   driver/runtime/module/compiler identities and decoder/method versions.
   Preserve the ROCm Core/package versus HIP component version distinction.
   Capture the actual runtime queried by the worker and bind profiles to the
   exact signed package and device. AMD's published ROCm Core 10.2.0 support
   table lists Windows 11 25H2 and the component table lists HIP 7.17.0.
   Qualify support on the target Windows 11 servicing channel separately.
   [AMD ROCm 10.2.0 release notes](https://rocm.docs.amd.com/en/docs-10.2.0/about/release-notes.html)
2. Discover compatible user-mode runtimes safely from supported installations,
   private packages and explicit configuration. Pin opened artifacts, validate
   provenance and avoid ambiguous DLL/PATH search. Do not replace the GPU driver.
3. Implement one automatic acquisition attempt per justified configuration
   generation. Distinguish absent runtime, unsupported hardware, busy resources,
   stale telemetry, integrity failure and unconfirmed retirement.
4. Acquire bounded calibration from actual complete work: startup, transfers,
   kernels, readback, CPU verification, encoding/storage and response completion.
   Key profiles by method, shape and configuration; do not extrapolate one
   1-MiB result into every workload.
5. Use ordinary workload observations plus small admitted experiments to decide
   whether foreground GPU work helps. Background compression may justify a GPU
   through stronger reduction even when minimum latency is not improved.
6. Connect qualified discovery and cost profiles to the owned production
   provider. The explicit SDK adapter now accepts deferred indexed writes and
   forwards independently verified candidates into the existing CPU-readable
   frame. The configured native controller now collects live start and
   post-verification budget evidence outside the filesystem lock. It retains
   qualified route pins and coalesces acquisition demand. Installed package
   verification and alternating matched decision logic are implemented. The
   indexed Windows owner now acquires a relocatable approved shared-base route
   on eligible background demand. A reference profile can price shared bases
   and encode sparse differences in the same exchange. Copy search remains
   available when the reference plan cannot be admitted. Produce package
   profiles from representative measurements, connect approved packages to
   the mounted launcher and verify routing on hosts with multiple AMD devices.
   Evaluate foreground completion and automatic cold compression independently.
   The latest selected-SDK and real hardware trial exercised finite uniform
   batches plus native indexed writeback. Keep a separate ordinary mounted
   qualification and test independent optional-role links.
   [Latest AMD trial](evidence/amd-fresh-completion-observation-2026-11-21.json)
   For the first mounted comparison, keep request trace, allocation policy,
   cache preparation and byte verification fixed across admitted AMD,
   CPU-only and refused-AMD trials.
   Treat the observed foreground CPU choice as specific to the tested request class.
   [Foreground route evidence](evidence/installed-amd-routing-2026-11-18.json) ·
   [Installed shared-base evidence](evidence/installed-amd-shared-base-2026-11-18.json) ·
   [Installed reference-profile evidence](evidence/amd-installed-reference-profile-2026-11-20.json)
7. Amortize setup over finite admitted work windows. The native indexed
   path can now cover several independently bounded launches in one worker
   exchange and charges the complete host footprint. CPU work can publish
   independent groups from the current discovery window while the entire
   staged batch runs on AMD. An approved uniform write profile can also
   allocate a private exchange for separately admitted, same-sized closed
   writes. Both the new and legacy package modes recovered the original bytes
   through the controller fixture.
   [Separate-request admission evidence](evidence/windows-controller-separate-request-admission-2026-11-24.json)
   The version 6 transport can finish a nonempty supplied prefix and verify
   the reply count after exact retirement. Its Radeon fixture finished two
   requests with room reserved for a third.
   [Finished-burst evidence](evidence/windows-finishable-worker-bursts-2026-11-24.json)
   The native replay now supplies a write committed after the worker
   reaches readiness. It verifies the exact supplied prefix in a private
   native image and keeps the originals available through CPU.
   [Arriving native write evidence](evidence/windows-native-arriving-write-burst-2026-11-24.json)
   The controller now waits at that boundary when an approved profile and
   observed write intervals support reuse. It checks resource headroom before
   capture and supply, finishes an empty wait and cancels on shutdown. The
   Radeon callback fixtures exercised both outcomes and independent CPU recovery.
   [Controller arrival evidence](evidence/windows-controller-arriving-write-bursts-2026-11-24.json)
   Mixed-size continuations now use the largest prepared request's transport
   and device allocation. Each accepted file retains its own frame geometry.
   Public FlitzFS source and oversized controls passed through the Radeon
   controller fixture.
   [Mixed-size controller evidence](evidence/windows-mixed-size-resident-worker-2026-11-24.json)
   Measure setup savings and retention costs with varied real arrival
   patterns and larger compression jobs. Completed comparisons now apply
   to one initial write size and storage policy. Larger writes can use a
   sealed exchange when a second request cannot fit. The shared pacing and
   fresh source requirements apply across sizes. A single Radeon controller
   verified both 64 KiB and 1 MiB public-source inputs after the shared cooldown
   and recovered all acknowledged files independently.
   [Geometry calibration evidence](evidence/windows-amd-write-geometry-calibration-2026-11-24.json)
   Exercise host pressure, foreground interruption and device changes
   during the wait before promoting any route.
   A verified full-group copy measurement can now fund a mixed prefix with
   one setup charge and a full measured launch allowance for each group.
   The Radeon scope verified four groups from one mixed exchange and the
   complete-cost estimate admitted two. Both lanes produced identical
   protected image sizes and verified independent readback.
   [Mixed collective setup evidence](evidence/windows-mixed-collective-setup-2026-11-24.json)
   Collective arena version 12 adds dense XOR as a third device choice.
   The indexed publication fixture verified selected bodies and recovered
   through the CPU when a body reconstructed different bytes. Compare both
   arena versions and CPU on the same native cohorts. Measure reply bytes,
   selected representations, complete image size, and total maintenance cost.
   One paired Radeon trial now verifies the third encoding through native
   publication and CPU recovery. The revised worker supplied 100 selected
   encodings and the older worker supplied 80. Both images occupied the same
   number of bytes. Ordinary CPU grouped reads reuse the verified base digest and
   apply dense planes one word at a time.
   [Device and CPU read evidence](evidence/windows-collective-dense-readback-2026-11-24.json)

   Packed XOR, sparse XOR, and modular delta read-ahead capture both protected records under one mirrored
   checkpoint. The verifier binds results to the current image, backing,
   record generations and decoded digest. One request uploads each distinct
   base once. WinFsp demand uses prepared bytes only while both record
   bindings remain current. CPU reads continue on pressure, a changed binding
   or an unavailable result.
   [Grouped read-ahead design and verification](NATIVE_AMD_LZ4_READ.md#shared-base-extent-read-ahead)

   A separately qualified prepared worker can overlap part of initialization
   with protected capture. The controller reserves one finite exchange and
   checks both startup and submission lead times. A missed forecast releases
   the unused reservation through the same retirement boundary.

   A submitted request now tracks progress on its own open context and
   image checkpoint. Include interleaved data reads and metadata queries
   in the matched mounted workload, along with target demand arriving before
   readiness and mutations arriving before publication. Count useful
   prepared bytes separately from verified but unconsumed replies.
   [Interleaved callback evidence](evidence/windows-group-read-stream-binding-2026-11-25.json)

   Run the device qualification for this exact module and worker. Then compare
   cold and prepared workers using the same captured groups and natural read
   lead times. Record transfer, verification, retirement, cancellation and
   forecast hit rate before approving automatic use. Qualify v1 and v2 with
   their exact worker packages. Use the finished comparisons to select which
   further shared-base methods warrant acceleration.
   Recent GPU bit-packing and tiled-decode work motivates coalesced work and
   bounded materialization. Qualify the benefit with the ordinary file read
   workload where results return to host memory.
   [AMD device layout](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/understand/hardware_implementation.html) ·
   [Invariant bit packing](https://arxiv.org/abs/2605.30728) ·
   [Tiled GPU decompression](https://arxiv.org/abs/2610.32695)

   Extend finite exchanges to independently admitted collective cohorts.
   Bind each job to a source revision, verify decoded bytes before publication,
   and retire the helper when admitted work drains or foreground demand rises.
   Compare complete occupied
   bytes and foreground interference with a matched CPU baseline, including
   unsuccessful or cancelled batches. For reference selection, compare
   bounded candidate retrieval and actual delta cost. Include held-out data
   that lacks a natural base. Use protected, independently readable references
   and record search cost as part of the complete comparison.
   [Finite exchange evidence](evidence/finite-amd-exchange-2026-11-18.json) ·
   [Full-batch CPU overlap evidence](evidence/windows-amd-full-batch-overlap-2026-11-21.json) ·
   [Sonic similarity study](https://www.mdpi.com/2073-431X/15/9/607) ·
   [ZipLLM delta study](https://www.usenix.org/conference/nsdi26/presentation/wang-zirui)
8. Preserve immutable owned input and independent CPU selection. CPU completion
   must not wait solely for optional GPU teardown; late GPU completion must not
   publish a second result or touch a newer file version.
9. The installed shared-base owner now rechecks discovery after a changed
   native file identity, adapter topology or released transport on new group
   demand. CPU work continues during the attempt. The compute policy retains
   quarantine history across replacement. Add independent same-device
   recovery qualification and broader multi-adapter fault evidence before
   claiming the full evidence-keyed recovery ladder.
   [Recovery policy and native inputs](GPU_RECOVERY.md#installed-windows-routes)
10. Qualify data reads with GPU/runtime missing, inaccessible or disabled. GPU
    encoder choice may differ from CPU choice; decoder availability is mandatory.
11. Integrate the selected collective backend from E06, not only fast ingestion.
    Measure whether features, dictionary/model work or encoding is the most
    valuable GPU task on each workload class.
12. Add bounded phase/cost diagnostics and regression cases for old abnormal
    exits/retirement failures. Never infer a historical cause from a later pass.

### Acceptance and failure checks

- Ordinary external-client writes or automatic collective work demonstrably
  execute useful AMD work without manual test-only routing. CPU fallback and
  independent reads work with no GPU dependency loaded by the recovery reader.
- Repeated alternating matched comparisons show a complete-operation throughput
  and/or total-size advantage in stated useful conditions, including overhead.
- Verify bad outputs, stale/wrong-adapter telemetry, low memory, cancellation,
  timeout, worker exit and unavailable runtime. Unsafe driver faults remain isolated.
- No repeated discovery storm, duplicate response, uncertain resource reuse or
  silently changed data. Report conditions selecting CPU as such.

**Output:** automatic runtime/cost acquisition, ordinary routing, useful measured
backend and failure evidence. **Existing ledger:** G02, G03, A02, D01.

## 7. E04 — Persistent sparse coherence and candidate discovery

**Reuse:** content/physical revisions, aligned and offset-independent features, outcome keys,
internal advisory ownership and the emerging paged engine.
**Planning effort:** 3–7 person-weeks; confidence low.

The indexed controller now streams allocated extent coordinates into bounded
fingerprint windows. It retains a limited set of earlier representatives and
extends reusable immutable anchors. Rolling samples retain similarity signals
after an insertion shifts matching content. Selected generations are checked during
cooperative preparation. Funded checkpoints can now retain up to 373
representatives in the existing advisory pair. In the distant-match fixture,
the restarted owner found a match after sampling one new extent. It later
reused 125 verified feature bindings and resampled the edited extent. The
paged coherence catalog now gives an indexed session a point lookup beyond
that checkpoint window. The managed scanner now populates a provisioned
catalog automatically. Its journal settles the full append and root write
allowance before catalog mutation. The owned Windows memory path provisions
the catalog on the first useful funded batch, retains it on a joined remount
and reclaims discarded backing in bounded steps. A full catalog now prunes
stale postings in a funded staged copy and releases the unselected pages.
The scan window, advisory checkpoint and paged catalog retain masked XOR
signatures for candidates with changes in every aligned band. The restarted
scanner found a match after one new sample across 126 retained representatives.
The paged path found another match across 374 unrelated extents after its
representative left checkpoint history. That group saved at least 96 KiB in
the compacted user image. The catalog occupied 412 KiB and the controller
performed 1,879 point lookups. Measure total storage cost and lookup yield
on mixed workloads, adapt signature retention to observed value, and
qualify persistent catalog compaction.
[Restart validation](evidence/indexed-representative-retention-2026-11-19.json)
[Paged coherence catalog](OPTIMIZATION_STATE.md#paged-coherence-catalog) ·
[Funding validation](evidence/funded-paged-coherence-2026-11-19.json) ·
[Windows ownership validation](evidence/windows-catalog-provisioning-2026-11-19.json) ·
[Catalog compaction validation](evidence/windows-catalog-compaction-2026-11-19.json) ·
[Masked signature validation](evidence/projected-xor-discovery-2026-11-19.json) ·
[Paged masked discovery](evidence/projected-coherence-catalog-2026-11-19.json)

Schema 3 of the optional catalog adds modular word signatures. An indexed
restart fixture recovered a reference eight unrelated extents away with
room for two decoded candidates. The complete compacted footprint,
including head, journal and selected catalog, fell from 2,019,328 to
1,896,448 bytes. The masked-only control occupied 2,015,232 bytes. A stale
binding could nominate a candidate and was rejected before group
preparation. With a smaller write grant the managed scanner published
the earlier signature families in the same reservation.
[Numeric discovery evidence](evidence/modular-coherence-catalog-2026-11-19.json)

The existing rolling sketches now have one input-funded persisted key per
eligible representative. On the bounded restart fixture, a 67-byte shift
after twelve unrelated extents reduced the complete settled footprint
from 2,543,616 to 2,420,736 bytes. The fixture charges the image, selector,
optimization area and catalog. It also verifies refusal of a stale
nomination. The scanner now qualifies secondary rolling matches even when
the shifted extent prefers a different key. The controlled CPython-derived
case and the synthetic case each lowered complete occupied bytes by
122,880. The candidate uses the same 159,744-byte catalog. The long-range
projected control used 4,879 lookups, up from 3,754. Compare profitable
recall and query cost on additional revision trees before spending more
catalog space.
[Persistent shifted discovery evidence](evidence/indexed-shifted-coherence-2026-11-25.json) ·
[Secondary rolling lookup evidence](evidence/release-reconciliation-rolling-lookups-2026-11-25.json)

Reusing checked pages in the same catalog view preserves complete candidate
selection while reducing repeated search work. The compact 16,384-fingerprint
control used 151,924 page reads for missing keys, down from 222,798. On the
374-extent projected fixture, catalog reads fell from 18,822 to 14,322 with
the same published group and measured protected savings.
Before changing the index or admitting more queries, measure cost and
profitable recall on held-out revision histories.
[Catalog page reuse evidence](evidence/indexed-coherence-lookup-pages-2026-11-25.json)

Indexed discovery can start with the extents in a covered history of
successful foreground writes. In a 1,034-file fixture, checking the changed
extent and refreshing the representative took two visits. The unmatched
and history-overflow cases exercised the ordinary scan. On the physical
Windows host, an SDK callback fixture attached three edited extents to
an existing group in one verified AMD batch.
[Incremental discovery evidence](evidence/indexed-change-discovery-2026-11-20.json)

RubikFS provides evidence for evaluating similarity and access hotness
together. MedFS shows why selective delta maintenance should account for write
traffic as well as the stored representation. FlitzFS uses its own live
generation checks and packed-size proof when selecting a candidate.
[RubikFS](https://www.usenix.org/conference/fast26/presentation/huang) ·
[MedFS](https://www.usenix.org/conference/fast25/presentation/wu)

### Implementation steps

1. Define stable content identities and feature versions independent of path,
   metadata timestamps and physical representation. Rename/ACL updates must not
   force resampling unchanged bytes.
2. Extend the internal paged advisory roots for scan continuation, file/chunk
   sketches, candidate postings and outcome history. Store accepted decode
   membership with the protected indexed data. Keep both outside the normal
   folder namespace.
3. Use file-level sketches to narrow searches, then chunk/region features where
   worthwhile. Evaluate content-defined and shift-tolerant features alongside
   the current aligned baseline; do not index every byte indiscriminately.
4. Implement bounded sparse candidate queries using appropriate locality/hash
   indexes and small approximate-neighbor sets. Use the current primary,
   projected and alternate postings as controls for size-filtered additional
   candidates. Retain an expansion only when measured recall and cost justify
   it. Record the candidates
   examined, feature bytes written, query reads, recalled relevant pairs
   and settled-byte benefit. Bind the persistent index to the recovered
   image and resume scans from a verified checkpoint when the hints are
   stale or incomplete.
5. Version features and postings by content generation. Reject stale selections
   at preparation/publication and invalidate only affected content.
6. Persist scan watermarks and fairness so constant foreground churn cannot
   indefinitely restart the same prefix. Deletion/rename and large directories
   must not lose or duplicate work.
7. Persist useful negative outcomes and confidence with configuration/method
   versions. Revisit only when content, model, method or measured evidence changes.
8. Charge index/feature/history bytes to the optimization objective. Choose
   sampling/index density by predicted benefit and actual resource budget.
9. Treat advisory corruption as rebuildable. Mandatory representation references
   remain authoritative in the native dependency graph, not this catalog.
10. Add event sources for content changes, new method availability, idle/resource
    opportunities and host migration. Timed pacing alone is not a reason to rescan.
11. Stream candidate groups into E05/E06 preparation without materializing a
    whole corpus or all pairwise scores in RAM/VRAM.

### Acceptance and failure checks

- Show candidate recall, precision, bytes sampled, query work and metadata size
  on diverse held-out data, including shifted content and unrelated files.
- Restart mid-scan and mid-index publication; resume useful progress and retain
  correct file bytes even when the catalog is unavailable.
- Exercise the E01 scale ladder with bounded resident state. Report candidate
  counts and comparison work; no O(N²) fallback is hidden behind a small fixture.
- Moving the filesystem to another host invalidates unsuitable performance advice
  without losing content identities or decoding ability.

**Output:** persistent feature/query format and resumable scanner. **Dependencies:**
E01 for scale. **Existing ledger:** S01, A04, L01.

## 8. E05 — Stronger collective-compression research and selection

This package is a controlled search for a better representation, not a promise
that adding a neural network creates a record. **Planning effort:** 6–16
person-weeks, with an explicitly open research tail.

### Candidate families

| Family | What to evaluate | Main risk/control |
| --- | --- | --- |
| Current shared-base patches | Baseline for aligned near-duplicates | Weak on shifts/reordering; count anchor and protection overhead |
| Content-defined sharing + multi-reference delta | Shift-tolerant common regions, edited versions and related files | Reference/index cost and transitive read amplification |
| Shared dictionary/entropy coding | Similar text, logs, source and structured records across files | Dictionary/training cost and small-file overhead |
| Reversible structure/bit-plane transforms | Typed binary/numeric families, XOR/residual structure, repeated fields | Original bytes, including headers/padding/ordering, must be reconstructed exactly |
| Bounded typed transform/program search | Learn compact reversible operator sequences for related data, with GPU candidate evaluation where useful | Charge the program/model; use a closed validated operator set, no arbitrary stored native/JIT code |
| Bounded learned predictor + entropy coding | Offline/online local statistical adaptation and reusable models | Deterministic CPU/GPU probabilities, model cost and random-read latency |
| Carefully selected format-specific recompression | Already-compressed formats where original encoding can be restored | Parsing/security and side information can exceed the gain; no lossy transcoding |

### Implementation and experiment steps

1. Freeze a corpus manifest, source/license/digest, split policy and all accounting
   before comparing candidates. Use heterogeneous real data and synthetic controls.
2. Keep related versions/families in the intended grouping workload, but prevent
   test-result leakage into model/parameter training. Include an unseen-family split.
3. Establish strong CPU controls: current codec versions at sensible fast and
   high-compression settings, long-window/shared/solid controls where appropriate,
   and the present protected group baseline.
4. Count complete representation bytes, all required models/dictionaries,
   indexes, checksums/protection and access structures. Also measure temporary
   memory/space and encoding work; do not hide learned weights outside the score.
5. Implement independent CPU decoding before admitting any candidate. Use
   bounded typed stages and validated references, not arbitrary media-supplied code.
6. For neural/statistical models, specify deterministic integer/quantized
   probability semantics or another exact reproducibility method. Floating-point
   agreement observed on one GPU is not a portable decoding specification.
7. Compare candidate grouping at region/chunk/file levels and variable group
   sizes. Determine random-read cost from the representation, not just member count.
8. Evaluate GPU feature/encoding/model-training benefit only with required CPU
   verification and transfer costs included. Preserve a functional CPU encoder
   fallback; it need not match the GPU's size or method.
9. Test small/cold/random reads, extraction of one member, editing/removing any
   member, regrouping and a missing GPU. Avoid an opaque solid group that makes
   unrelated files necessary for every read.
10. Include incompressible and already-compressed/encrypted controls. A correct
    classifier chooses a nonexpanding representation rather than promising gains
    where no exploitable redundancy exists.
11. Maintain a size/access/compute Pareto frontier. Minimum stored bytes is primary;
    practical latency and no dramatic slowdown are acceptance constraints, not
    excuses to default permanently to the weakest codec.
12. Select a candidate only after held-out benefits survive complete accounting
    and fault/access tests. Preserve rejected candidates and negative results.
    If none wins, change the research approach; do not relabel the baseline a record.

### Research implications from current sources

RubikFS's 2026 similarity/hotness grouping provides a relevant read-only comparison.
ZipLLM and Invariant Bit Packing motivate specialized family/bit-structure
candidates; they are not evidence of arbitrary-file or AMD gains. The November
invocation-bottleneck preprint motivates an explicit model-call/decode design.
For a bounded DirectStorage Zstd evaluation, use unchanged, independently
decodable FlitzFS frames and charge initialization, submission, transfer,
readback and CPU verification. DirectStorage uses CPU decompression for
system-memory destinations. A GPU comparison therefore needs a device
buffer, verified GPU execution and complete readback into the callback's
address space. Microsoft introduced Zstd in the 1.4 preview series. The
24 November package inventory lists 1.3.0 as the latest stable version and
1.4.0-preview2-2606.904 as the latest preview. Add it to the normal reader
only after verifying complete operation cost and independent CPU recovery.
[Microsoft Zstd preview](https://devblogs.microsoft.com/directx/directstorage-1-4-release-adds-support-for-zstandard/) ·
[Microsoft developer guidance](https://github.com/microsoft/DirectStorage/blob/main/Docs/DeveloperGuidance.md) ·
[NuGet package inventory](https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.directstorage/index.json)

**Output:** reproducible candidate report, independent decoder and selected
representation/access specification. **Existing ledger:** A04, G03, Q01.
Record-setting ambitions remain open until comparable evidence exists.

## 9. E06 — Protected collective representation in ordinary storage

**Reuse:** immutable anchor objects, content tokens, CPU digest verification,
staged selection and COW publication. **Planning effort:** 5–12 person-weeks.

Indexed volumes now publish the protected shared-base codec with ordered
member backreferences. Current members can be detached independently, and
an anchor can grow through several bounded appends. Indexed readback,
compaction, modeled interruption recovery and overwrite through the AMD
SDK callback have executed. Keep comparison against complete compacted
size and measure reads under normal Windows workloads.
[Indexed group format and evidence](INDEXED_NATIVE_VOLUME.md#shared-base-extents)

The indexed scanner now tries short standalone payloads after its
similarity and catalog candidates. It prices a protected 64 KiB pack
against the complete compacted image. Two public sets saved 8 KiB each.
In separate mixed AMD runs the CPU pack reduced the image alongside
verified GPU advice. Extend the comparison to larger short-file sets and
measure cold random reads and maintenance writes before increasing the
scope of each packing search.
[Direct packing comparison](evidence/indexed-direct-short-packing-2026-11-20.json)

Indexed capability 4 supports anchor-copy members alongside aligned members.
A controlled Zstandard-enabled source fixture compacted from 592 KiB to 192 KiB
with byte-exact CPU readback, and the format promotion passed 528 modeled
interruptions per lane. Compare CPU and AMD matching on many related regions
and on mixed corpora. Count index bytes, transfer costs and CPU readback when
selecting a method for the ordinary optimizer.
[Shifted group validation](evidence/shifted-indexed-groups-2026-11-18.json)

Indexed capability 5 adds independently readable packed XOR planes for dense,
low-bit differences against an immutable base. In a matched nine-file
low-bit fixture, automatic grouping reduced the protected compacted image
from 1,176 KiB with the previous optimizer to 400 KiB. Discovery used one
unchanged band per extent. A later physical-host trial used batched AMD advice
for all eight proposed members across two groups. CPU and AMD both compacted
to 768 KiB with byte-exact CPU reopen. CPU settled in 49.7 seconds and AMD
in 55.7 seconds, including idle scheduling and worker retirement. Keep CPU
preferred for the observed workload. A separate nine-file fixture changed
two middle bit planes throughout each extent. Masked discovery reduced the
protected compacted image from 1,304 KiB to 544 KiB with exact CPU readback.
The Windows owner now keeps the recovery ticket through the per-member byte
comparisons. A mismatch quarantines the configuration and leaves CPU grouping
available. The verifier constructs and hashes a stored frame only for a body
that beats the competing size. The wider worker exchange now carries a packed
body up to a full extent slot. In one physical two-plane pair, the 16 KiB
layout declined all eight bodies and the 64 KiB layout supplied all eight.
Both compacted to 896 KiB and reopened exactly on CPU. Completion took
55.5 seconds for the narrow exchange with CPU fallback and 59.0 seconds for
the wider exchange.

The full-extent AMD worker now obtains its dispatch width from the work grant
and live memory admission. It checks the detected device grid before launching.
The new kernel entry preserves the earlier private entries and the CPU-readable
on-volume format. Profiles bind the request, reply layout, adapter and geometry,
and compare the supervised exchange with independent CPU encoding on matching
related and mixed cohorts. Persistent learning should use complete verified
cost, including startup and retirement, when deciding whether to use a GPU.
Reuse a bounded worker lifetime to amortize setup and measure the result across
device classes before enabling automatic routing.
[Packed XOR format](INDEXED_NATIVE_VOLUME.md#packed-xor-members) ·
[CPU size evidence](evidence/indexed-packed-xor-2026-11-19.json) ·
[Physical AMD evidence](evidence/amd-packed-xor-batch-2026-11-19.json) ·
[Packed XOR semantic verification](evidence/amd-packed-xor-verdicts-2026-11-19.json) ·
[Wider paired trial](evidence/amd-wide-packed-xor-2026-11-19.json) ·
[Dispatch and phase measurements](evidence/amd-adaptive-packed-xor-2026-11-19.json)

New masked-XOR groups use sampled bit widths to choose a shared base before
either CPU or AMD encoding. The nine-extent first-reference control compacted
to 557,056 bytes and the sampled choice to 442,368 bytes, with exact CPU
readback. A separate input exercised refusal of a poorly estimated base and
successful publication using the original candidate. Across three alternating
physical-host pairs per cohort, both routes reduced the complete compacted
image from 13,975,552 to 2,678,784 bytes. AMD supplied 100 independently
verified members per trial. Median complete times were 0.245 seconds on CPU
and 2.103 seconds with AMD for synthetic data, and 0.243 and 2.044 seconds for
controlled public-derived data. Median worker intervals were 1.633 and
1.573 seconds, respectively. Keep this workload on CPU
and compare deeper reference searches after qualifying a bounded AMD worker
lifetime that can amortize setup.
[Sampled reference selection and matched evidence](evidence/sampled-xor-reference-2026-11-19.json)

The reference scorer now ranks using sampled complete tile planes. Every
nonmatching plane adds the same payload cost. The previous bit-width score
overweighted high planes and preferred the first member in a controlled
nine-extent cohort. Matching sampled planes reduced the verified compacted
image from 999,424 to 876,544 bytes on that input. An unsuitable sampled
reference still falls back to the original candidate while the group
remains active. Three alternating physical-host pairs per cohort reached
7,823,360 compacted bytes from 13,975,552 bytes with either route and exact
CPU reopen. AMD advice survived final size competition for 29 synthetic
or 19 public-derived members per run. CPU completed both controlled cohorts
sooner. An admitted stream can do more useful work by evaluating competing
representations for the same verified group.
[Tile reference validation](evidence/packed-xor-tile-reference-2026-11-19.json)

Indexed capability 7 adds a protected sparse XOR member. It chooses a bitmap
or a short set or clear position list for each active tile plane. Two
fixed-reference 15-file cohorts compacted to 1,753,088 bytes using the new
encoder, compared with 4,759,552 bytes using the prior engine. Both reopened
byte-for-byte. Each raw, WinFsp SDK and AddressSanitizer lane recovered from
528 modeled publication interruptions. Six live AMD runs each supplied 100
published members through the verified dense reply and CPU conversion.
Both routes reached the same complete size. CPU was faster in these runs.
[Sparse XOR format and evidence](evidence/indexed-sparse-xor-2026-11-19.json)

The direct AMD sparse window now returns actual sparse bodies. Its reply
occupied 423,204 bytes for the original cohort and 653,604 bytes when all
eight planes differed. The dense route sent 6,554,004 bytes in both cases.
It supplied 100 members in the first cohort and empty slots in the second.
Direct sparse encoding supplied 100 independently verified members each
time. Every route produced the same protected compacted size within each
cohort. CPU had the lowest median complete time in both.
[Direct AMD sparse and CPU fallback evidence](evidence/amd-direct-sparse-xor-2026-11-19.json)

The sparse device arena reduces the readback of 60 extents from 3,932,160
bytes to 392,196 on the related input and 242,052 on the mixed input. Twelve
physical-host exchanges matched the independently verified CPU candidate
counts and sizes. Median readback wall times declined from 2.79 to 1.82 ms
and from 2.72 to 2.31 ms, respectively. Median HIP stream creation remained
above 1.2 seconds for both entries and inputs. CPU completed these
comparisons sooner. The normal controller used the arena in two compacted
image tests and independently reopened all files. The next qualification
needs more useful search per stream and complete-volume comparisons with
fresh pressure, cancellation and retirement evidence.
[Sparse device readback evidence](evidence/amd-sparse-device-readback-2026-11-19.json)

The mixed AMD dispatch compares sparse XOR and modular delta after a
single upload for each admitted window. In 24 matched physical-host
runs, the mixed worker supplied 100 used candidates per batch, including
60 sparse and 40 modular members. The two single-codec workers supplied
60 and 40 used candidates respectively. The mixed reply occupied
500,884 bytes, compared with 833,695 to 1,423,691 for sparse-only and
3,887,888 to 3,888,956 for modular-only. All routes produced a
1,900,544-byte compacted image from 13,975,552 bytes and reopened all 15
files byte-for-byte. Median complete time was 2.45 to 2.53 seconds for
mixed AMD and about 0.26 seconds for CPU. Automatic GPU preference needs
whole-workload evidence with enough alternative reference and codec
search to justify startup and verification costs.
[Mixed-codec measurements](evidence/amd-mixed-codec-selection-2026-11-19.json)

Masked-XOR discovery now prices sparse lists, inverse lists and dense bitmaps
in its reference sample. On each matched 15-file cohort, this reduced complete
compacted size from 3,186,688 to 2,826,240 bytes with either CPU or AMD
encoding. The previous selector and the new selector each ran three times
per cohort and backend, and all 24 trials reopened every file on CPU.
The new AMD replies carried 960,804 bytes, down from 1,142,564. AMD still
supplied 100 used members per trial. CPU remained fastest on the tested
inputs. Larger profiles should compare competing references with full-image
costs and include naturally related data beyond the controlled cohorts.
[Sparse-aware reference measurements](evidence/sparse-reference-cost-2026-11-19.json)

When a complete new group fits the admitted device allocation and launch
budget, the AMD worker can compare several references across every tile. It
encodes the selected reference in the same stream. In 18 matched host trials
on two controlled 15-file cohorts, the full-tile route changed the reference
for all five groups and reduced both compacted images from 3,121,152 to
2,768,896 bytes. All routes reopened every file byte-for-byte on CPU. Median
complete times were 0.23–0.24 seconds for CPU, 2.11–2.12 seconds for AMD with
the sampled reference and 2.57–2.58 seconds for AMD with full-tile pricing.
CPU remains the default.
[Full-tile reference measurements](evidence/amd-full-reference-pricing-2026-11-19.json)

Indexed CPU maintenance now prices complete admitted masked-XOR groups one
extent pair at a time. The physical-host comparison used three rotations
of sampled CPU, full-tile CPU and full-tile AMD on each of the two cohorts.
Both full-tile routes produced the same 2,768,896-byte compacted size.
Full-tile CPU medians were 226–228 ms and full-tile AMD medians were
2.56–2.59 seconds. CPU considered five references per group and AMD
considered two under its launch grant. All 18 trials read back every file
on CPU. Five published references changed per full-tile trial, and the
controller now retains the actual selected reference for later reuse.
[CPU full-tile measurements](evidence/cpu-full-reference-pricing-2026-11-19.json)

The public revision fixture uses the latest five distinct blobs of each of
eight pinned paths, split into source and documentation cohorts. The
Zstandard-backed images decreased from 606,208 to 491,520 bytes for
source and from 843,776 to 540,672 bytes for documentation. They contained
exactly the original files. The original AMD copy search completed one
verified exchange for each corpus and codec profile. The writer selected
zero suggestions from those exchanges.

Indexed grouped reads now check the expected anchor generation during the
payload lookup and reuse one verified base within the request. Validation
of a member body runs as it is decoded. In three rotating runs per route,
the Zstandard source cohort reduced whole-file backing reads from 1,240,279
to 1,199,319 bytes and sampled 4 KiB backing reads from 3,333,720 to 3,251,800.
On the documentation cohort those figures dropped from 1,492,358 to
1,410,438 and from 3,681,097 to 3,558,217 bytes. Image sizes stayed fixed
and independent CPU reads matched every file. A separate six-extent
fixture verified one primary base read per request and recovery from a
modeled damaged primary on a later read. The two deferred WinFsp sanitizer
cases passed after host headroom recovered. The app fixtures now receive
the supervisor's budget through CTest and apply a private commit limit
within the live host reserve.
[Public revision and read qualification](evidence/public-revision-reads-2026-11-19.json)

AMD copy suggestions now extend against the original bytes before the
writer prices the candidate. Every supplied range is checked even if a
different span already covers it. In 36 alternating physical-host tests,
the writer selected AMD spans for 16 raw source, four Zstandard source
and four raw documentation members in each respective run. The raw
source image shrank from 868,352 to 860,160 bytes. The other three images
kept their measured sizes. Independent CPU reads matched every file.
CPU cold complete medians were 36–52 ms and refined AMD medians were
1.89–1.96 s, including discovery and verified compaction. Keep CPU
preferred at these sizes. Next, measure how larger verified exchanges
amortize cold worker startup, profile transfer and search separately,
and assess foreground interference before admitting the added work
automatically. Mounted throughput remains a separate qualification.
[Matched reader and AMD hint qualification](evidence/verified-device-copy-refinement-2026-11-19.json)

The installed Windows controller can use a qualified copy-search queue when
the admitted slab contains a full wait group. The parent verifies the bound
phase record and the copy ranges after the worker retires. In eight pairs
per public cohort, the preliminary two-dispatch queue produced the same
selected members and compacted sizes as serial waits. Median complete time
was 1.91 seconds serial and 2.16 seconds queued for raw source, and about
2.16 seconds on both routes for Zstandard documentation. Both paired
intervals included zero. The final controller stays serial when the slab
cannot fill the qualified depth. Qualify larger complete workloads and
measure foreground interference before expanding package opt-in.
The live installed callback fixture verified four dispatches per wait,
published ten advised members, and read all eleven files on CPU. A
foreground callback cancelled another queued exchange and CPU completed
its group. The short public source group retained serial waits and its
860,160-byte image.
[Windows queue qualification](evidence/native-anchor-wait-qualification-2026-11-19.json)

The Windows owners now admit several short preparation steps while one
genuine CPU observation is fresh. The core credits that observation once.
Each continuation resamples memory, checks foreground and stop, and defers
storage publication until a fresh CPU capture. The outer owner charges its
complete measured work against the existing background pacing. Qualify
foreground callback tails and complete maintenance throughput across larger
mixed workloads. The final matched 11-file installed AMD callback fixture
completed in 30.2 seconds, compared with 62.2 seconds on the prior controller.
Both published one group and read back every file on CPU. The final build
ran 32 additional preparation steps within fresh observations and preserved
CPU completion when a foreground callback cancelled the AMD exchange.
The desktop helper used the same scheduling mechanism in its live test.
[Windows maintenance timing and validation](evidence/windows-maintenance-opportunities-2026-11-19.json)

A foreground callback now releases unresolved AMD work and preserves a
settled verified reply. In the installed-package fixture, the previous
controller published one advised member after a foreground read burst.
The updated controller published all ten from its verified batch. Another
trial edited a selected extent after verification. CPU preparation rebuilt
the group and independent readback matched the new bytes. Reads after
submission also cancelled an outstanding worker and CPU finished the group.
The recorded 4 KiB timings cover the direct callbacks in those short bursts.
Measure sustained mixed access and mounted latency separately.
[Foreground advice and callback evidence](evidence/amd-verified-foreground-2026-11-19.json)

The indexed owner now fills a finite worker exchange across several scan
windows under live host and exact channel admission. In the related-data
fixture, one real AMD exchange supplied 310 verified candidates for five
independent group transactions. Compacted CPU readback verified every file.
When a later candidate partly overlaps a staged group, the owner settles the
batch and revisits the remaining extents in that window. A measured mixed-data
regression fell from 23,035,904 to 8,650,752 compacted bytes after the fix,
matching CPU. Three alternating runs per route and cohort produced equal
compacted sizes and exact CPU readback on the physical Windows host. Median
complete time was 1.48 seconds on CPU and 3.30 seconds with AMD assistance for
the related cohort, and 1.37 and 3.21 seconds for mixed data. These timings
include discovery through verified compaction and use modeled idle opportunities.
Keep CPU preferred for these profiles and qualify other complete workloads and
device classes before automatic routing.

A physical append-space refusal now releases the uncommitted batch and pauses
group preparation for that image generation. The verified reclaimer can
consider the measured shortfall alongside estimated reclaimable bytes. The
near-capacity memory fixture kept the source unchanged at refusal, verified a
compacted replacement, resumed compression and read all 45 files byte-exactly.
[Multiwindow maintenance evidence](evidence/amd-multiwindow-maintenance-2026-11-19.json)
[Matched timing and capacity evidence](evidence/amd-maintenance-parity-2026-11-19.json)

The owner now continues collecting independent groups in a window with
deferred overlaps. It retains the candidate order for intersecting extents
and revisits them when the batch settles. The overlap regression collected
three groups in place of the previous single group and preserved compacted
size and CPU readback. Three before/after pairs on the public-derived workload
both had a 3.28-second median with AMD assistance. The matched CPU median was
1.32 seconds and the compacted size was 8,650,752 bytes in every run. The
next GPU selection evidence needs work that yields complete-image savings or
amortizes stream creation over more useful candidates.
[Overlap scheduling evidence](evidence/indexed-overlap-batch-2026-11-19.json)

Indexed capability 6 adds a modular predictor for 1-, 2-, 4- and 8-byte
words. The scanner uses bias-invariant samples to nominate related extents.
The writer selects the smallest admitted member representation, verifies it
on CPU and publishes it with the protected base. Two nine-file controlled
public-derived cases with Zstandard level 6 fell from 768 to 184 KiB and
from 912 to 200 KiB in complete compacted size. The AMD worker now scores
and packs the same format, returning compact replies to the CPU verifier.
Six matched pairs on one physical Windows host used 310 verified AMD
members per batch and all four word widths. Both routes compacted from
41,623,552 to 4,096,000 bytes and reopened every file exactly.
Median complete times were 0.98 seconds on CPU and 3.16 seconds with AMD for
synthetic inputs, and 1.00 and 3.17 seconds for the public-derived inputs.
The wider AMD search scores up to five distinct biases for each word width.
Across six matched sets on two controlled inputs, it published 100 advised
members per trial and reached 2,768,896 bytes. The previous AMD worker used
13,836,288 bytes. The fast CPU route used 13,828,096 and 13,787,136 bytes.
Median complete times for the wider AMD search were 2.25 and 2.28 seconds.
The fast CPU medians were 0.28 and 0.38 seconds. The 310-member single-offset
control kept its 4,096,000-byte result.

The normal CPU encoder now evaluates the same proposals. Across six matched
pairs, CPU and AMD both produced 2,768,896-byte compacted images and verified
all source bytes. The median complete costs were 0.49 seconds for CPU on
both cohorts and 2.17 and 2.57 seconds for AMD. Total CPU time across each
test's owned process tree, including preparation, had medians of 0.69
seconds for CPU and 1.33 and 1.30 seconds for AMD. The separate 310-member
case retained the same 4,096,000-byte image with both routes. Use measured
CPU cost for these profiles. Qualify larger and natural numeric cohorts and
measure persistent signature yield, stored cost and foreground interference.
[Modular-delta format](INDEXED_NATIVE_VOLUME.md#modular-delta-members) ·
[Size and recovery evidence](evidence/indexed-modular-delta-2026-11-19.json) ·
[Matched AMD evidence](evidence/amd-modular-delta-2026-11-19.json) ·
[Wider predictor evidence](evidence/amd-sampled-bias-2026-11-19.json) ·
[Equivalent search comparison](evidence/cpu-amd-modular-parity-2026-11-19.json)

The first collective AMD trial reused the authenticated uniform LZ4 worker.
Copies from tiles that contain the same base produced a 600 KiB complete
image on the seeded fragmented fixture, compared with 752 KiB for the same
tiled search on CPU with Zstandard available. Zstandard alone reached 416 KiB
on the source-derived fixture and the group was declined. Full AMD costs were
1.79–2.20 seconds in those trials.
[Collective comparison](evidence/amd-anchor-collective-2026-11-18.json)

The direct AMD matcher now builds one base index per request and returns packed
copy suggestions. In paired three-member trials it cut input transfer by two
thirds and reduced the seeded whole image from 520 KiB with tiled AMD search
to 360 KiB. CPU search produced the same complete image in the direct mode and
remained faster.
[Direct shared-base search](evidence/direct-amd-anchor-2026-11-18.json)

The sized worker reused one base index across 65 search launches for 193
members. It retained the 816 KiB device allocation and matched the CPU exactly.
The 1.01 second exchange still exceeded the 144 ms CPU search and verification
time. The profiled 193-member run spent 330 ms in stream and slab setup and
438 ms in host-to-device calls. Launch and stream wait took 8.2 ms. Fresh-worker
timings also vary across repeated runs. The pinned asynchronous candidate
hit the nonlocal-memory gate during the seven-member smoke test, while the
synchronous control verified that workload. Reusing one authenticated runtime,
stream and slab across three independent bases cut the median GPU exchange
from 1.72 to 0.75 seconds at 144 member extents. The CPU reference took
0.10 seconds. Coalescing pageable transfers cut the number of transfer pairs
from 426 to six at 1,272 extents. The shared AMD exchange took 1.24–1.38
seconds, compared with 1.79–2.35 seconds using one transfer per dispatch.
CPU matching and verification took 0.90 seconds. Moving private snapshots
through the worker cut median peak process-tree commitment from 725 MiB to
302 MiB at 1,272 extents. Two admitted 1,824-extent trials approached or
beat their CPU reference. Measure crossover across mixed content and
cold launches. Use the complete cost and current contention to qualify
ordinary routing with prompt yielding and independent CPU progress.
[Sized shared-base sweep](evidence/amd-anchor-sweep-2026-11-18.json)
[Phase measurements](evidence/amd-anchor-phases-2026-11-18.json)
[Pinned transfer trial](evidence/amd-anchor-pinned-trial-2026-11-18.json)
[Independent-group batch comparison](evidence/amd-anchor-batch-2026-11-18.json)
[Coalesced transfer comparison](evidence/amd-anchor-transfers-2026-11-18.json)
[Shared request snapshot comparison](evidence/amd-anchor-snapshot-2026-11-18.json)

A partially admitted AMD batch now leaves its unsubmitted groups eligible
for CPU maintenance during the exchange. Three paired public-code trials
each processed two groups on AMD and three on CPU. In the overlapping
schedule the CPU completed all three before AMD reply verification. Both
schedules reopened all 20 files exactly and occupied 280 KiB. Times moved
in both directions across pairs. Evaluate larger admitted groups and
foreground contention before assigning a throughput benefit.
[Split scheduling evidence](evidence/windows-amd-cpu-overlap-2026-11-20.json)

The producer now appends each admitted group directly to the final snapshot
and provides verified views from that construction. Release and
AddressSanitizer fixtures compared the views with independent wire parsing.
Three alternating 1,824-extent pairs reduced median preparation from
57.37 to 35.22 milliseconds. Live related and mixed public-derived
workloads through 2,304 extents matched the CPU replies, with overlapping
or worse complete AMD exchange times. Coalesced phase profiles measured
roughly 430 milliseconds for dispatches and stream waits at 1,824
extents. An experimental queue cut the median of that phase by 8.4%
across four matched pairs, with CPU still faster in the complete
comparison. Use these observations to prioritize bounded reuse of setup
across admitted work and further reduce search cost before enabling
automatic selection.
[Streaming construction and corpus evidence](evidence/amd-anchor-streamed-prep-2026-11-18.json) ·
[Mixed-content and SDK evidence](evidence/amd-anchor-cohorts-2026-11-18.json) ·
[Wait and admission evidence](evidence/amd-anchor-queue-phases-2026-11-18.json)

Batch reply verification now reads directly from the wire and reuses the
digest bound to each private request. Verified replies stayed byte-identical
and median host verification time fell by 30–35% at 864 member extents.
Complete AMD exchange still exceeded CPU cost on those cohorts. Further
routing qualification should measure complete costs under matched conditions.
[Reply verification evidence](evidence/amd-anchor-reply-verification-2026-11-18.json)

Indexed maintenance can combine disjoint groups from the same discovery window
within a qualified aggregate grant. It stages one member per admitted
opportunity and charges each base and retained source window. The Windows
owner submits the complete batch outside the filesystem lock. Each group
matches its reply by immutable identity and checks live revisions before
separate publication. Foreground activity or lost headroom returns work to CPU
preparation. The relocatable package supplies the qualified runtime, device and
work range. Broader routing qualification still needs end-to-end measurements
of compacted size, read cost and total background cost.
[Group maintenance interface](AUTOMATIC_GROUP_MAINTENANCE.md) ·
[Live discovery and fallback evidence](evidence/amd-discovered-anchor-2026-11-18.json) ·
[Owned Windows controller](WINFSP_OWNED_CONTROLLER.md#amd-shared-base-search) ·
[Packaged batch evidence](evidence/amd-owned-group-batch-2026-11-18.json)

Packed compaction now bases total-size comparison on checked totals and
changed lookup paths. This removes the full metadata scan from each indexed
proposal. Actual compaction still needs separate copying, indexing and CPU
verification opportunities. [Packed rebuild accounting](PAGED_OBJECT_STORE.md#packed-rebuild-accounting)

Indexed preparation now stages and independently verifies one extent at a time.
The writer invalidates changed selections at transaction commit. Finalization
checks live extent generations and the packed-size proof while preserving
unrelated edits. The current Btrfs comparison contract illustrates the
importance of proving source bytes before sharing and excluding concurrent
changes at publication. [Indexed preparation evidence](evidence/cooperative-indexed-shared-base-2026-11-18.json) ·
[Btrfs deduplication](https://btrfs.readthedocs.io/en/latest/Deduplication.html#safety-of-block-comparison)

The background controller now feeds bounded indexed candidates through this
path. Persist the candidate state in E04. For each
candidate method, charge bounded windows, retained state and worker scratch
before admission, and cache statistics by immutable content and model
parameters. FlitzZip's current pipeline demonstrates these techniques and
preserves a terminal HIP statistics failure through parent cleanup. FlitzFS
already has an owned worker and independent CPU verification. Extend that path when a
collective prototype shows a useful result in matched CPU and AMD trials
with full occupied-space accounting.
[FlitzZip source review](evidence/cooperative-indexed-shared-base-2026-11-18.json)

### Implementation steps

1. Turn the E05 selection into a closed method specification: framing, feature
   bits, lengths, integer ranges, seek index, dependency references and CPU decoder.
   Do not overwrite or reuse existing method/capability IDs.
2. Store immutable group manifests separately from user filenames. A base file's
   rename/deletion must never remove another member's required data.
3. Represent member ranges with individually addressable chunks/segments and
   verified dictionaries/models. Groups can span many files without requiring
   an entire group to be decoded for a small member read.
4. Validate dependency graphs for cycles, absent generations, wrong digests,
   unsupported methods and declared-versus-actual decode cost. Bound transitive
   work and scratch; flatten/restructure expensive chains rather than letting
   dependency depth grow indefinitely.
5. Implement the CPU range reader and malformed-input rejection. Bundle every
   required decoder with recovery tooling; optional encoders are a separate choice.
6. Add the GPU encoder and useful GPU read/transform path where qualified.
   Validate candidates independently before they receive storage authority.
7. Stage selection, data transformation and verification in bounded opportunities.
   Pin source generations and reject changed selected content before publication.
8. Atomically publish member mappings and dependency ownership. Prepare all
   fallible metadata/results before acknowledgement and preserve unrelated edits.
9. Implement edits, truncation, deletion, clone/hard-link interactions and
   regrouping without corrupting unchanged members. Writes can detach affected
   chunks into the fast representation; later maintenance may regroup them.
10. Integrate E02 reclamation so useful results reduce occupied bytes. Preserve
    rollback/recovery and account for old snapshots/pins that intentionally retain data.
11. Publish independently decoded exact bytes through normal Windows reads,
    including with runtime/GPU absent. Never make mutable learning state necessary.
12. Add mixed-method/mixed-generation fixtures and expose only meaningful user
    status. Internal models/manifests must not appear as normal folder contents.
13. Extend the indexed grouped audit measurements across working sets below,
    at and beyond one audit's base-cache budget. Alternate file order and
    group membership, verify both replicas, and record backing bytes, decode
    work and peak scratch. Include recovery with a pinned older checkpoint,
    a missing replica, coexisting generations and groups changed during
    preparation. Audit state expires with the checked inventory.
14. Use the same held-out images to measure owned-session and independent
    snapshot reads. Record backing bytes per requested byte for small and
    sequential reads alongside occupied bytes after verified reclamation.
    Advance layouts that preserve the savings across practical access
    patterns. Evaluate narrower independently seekable slices or better
    grouping when repeated base fetches make ordinary reads too expensive.
    [Grouped audit and live corpus evidence](evidence/indexed-grouped-audit-2026-11-20.json)

    - The packer uses size and similarity to find a retained partner when
      the current window has none. Extend evaluation to paged postings with
      mixed payload sizes, larger revision families and changing content.
      Compare object order, size-filtered lookup and similarity ranking with
      the same inputs. Record candidate recall, CPU, resident memory and
      backing traffic. Price groups and displaced files together before
      changing the selection of already fitting in-window groups.
      [Retained similarity evidence](evidence/indexed-retained-packing-similarity-2026-11-22.json)
    - Report group costs and one-member read work. Account for the base,
      reverse references, padding, protection, pending reclamation and any
      pinned older copies. Choose trial sizes from these observations.
      Exercise false predictions, write pressure, changes during preparation
      and restored advisory state. Bind trial outcomes to the exact content
      and method before using them to avoid repeated work.
    - Prototype larger immutable manifests over independently readable
      segments. Qualify the CPU range path and its dependency work first.
      Compare shared dictionaries, reference bodies and simple standalone
      compression against the same complete layouts. Interrupt publication
      and retirement with pinned readers, stale members and loss of one
      copy. Require observed end-to-end GPU benefit and immediate CPU
      continuity during failures.

### Acceptance and failure checks

- Real related groups of 1, 2, many and eventually thousands of files work within
  the qualified scale/access envelope. A one-file group can choose a standalone method.
- A random small read, one-member mutation and base-filename deletion preserve
  all acknowledged data; no unbounded whole-group extraction is concealed.
- Fail every relevant publication/dependency phase and recover on CPU. Reject
  malformed lengths, cycles, unknown methods and decompression bombs before allocation.
- Demonstrate held-out settled TOTAL-size gains, with live/temporary/protected
  dependency costs, after automatic reclamation. No payload-only success closes E06.

**Dependencies:** E01/E04 for production scale, E05 for method selection and E02
for a live-size claim. **Existing ledger:** A04, R01, G02, Q01.

## 10. E07 — Autonomous learning, resource control and background policy

**Reuse:** Windows PDH/memory observations, thread background mode, foreground
events, elapsed-work pacing, content revisions and bounded advisory journals.
**Planning effort:** 4–9 person-weeks; confidence low.

Indexed compaction now retains cold encoding coverage in the funded advice
records. Coverage binds to the selected image and encoder policy, and
replayed foreground writes remain eligible. This records completed work
for step 9. Workload learning, maintenance arbitration, lifetime rewrite
accounting and mounted interference measurement remain open.

Append-mostly volumes can now earn an optional cold copy through measured
sampling. The helper funds the private copy from a durable settlement
before it begins writing. This covers the refactoring of cold admission
in step 5 and one write class in step 10. Larger-volume write-amplification
measurements and adaptation of the sample policy remain qualification work.

### Implementation steps

1. Define separate budgets for foreground ingestion, optional background work,
   recovery/repair and calibration. Assign reservations across open volumes
   using current capabilities, foreground impact and live available resources.
2. Measure own CPU time, GPU work/transfer time, physical/commit/VRAM headroom,
   I/O bandwidth and observed foreground interference. Keep wall-time pacing,
   but do not mistake it for a measured whole-machine CPU share.
3. Replace a permanent fixed busy threshold with a conservative learned
   interference policy and hysteresis. Consider CPU strength/processor groups,
   outstanding foreground work and resource pressure; never infer idle from a
   stale or unavailable collector.
4. Build event-driven opportunities for content/policy changes, new evidence,
   power/resume and idle availability. Wait without counter sampling when
   nothing useful is pending. Bound any health probe and suppress unchanged failure.
5. Choose among feature scanning, candidate preparation, model/dictionary work,
   recompression, compaction, scrub and migration by expected net benefit and
   urgency. Maintenance must not starve correctness-critical recovery.
6. Persist learning as versioned, bounded sufficient statistics and decision
   history. Start with an interpretable statistical/contextual selection policy;
   use more complex local ML only where held-out results justify its cost.
7. Learn from actual workload classes, file regions, access temperature and
   outcomes. Do not treat file extension, one sample or one host's result as truth.
8. Schedule small finite exploration benchmarks only from admitted spare budget.
   Charge their reads, writes, model state and GPU/CPU cost; avoid repeatedly
   benchmarking a decision whose relevant inputs have not changed.
9. Persist resumable work state and negative outcomes without making them decode
   authority. Torn learning writes revert to a valid policy or rebuild/default
   behavior; user data remains independently readable.
10. Add lifetime rewrite accounting and benefit thresholds. Credit accepted input
    and actual reclaimed/saved bytes appropriately; do not allow a restart to
    reset wear history or an old qualification limit to disable maintenance forever.
11. Classify failure responses: resource deferral releases appropriate private
    work; stale content replans; unavailable telemetry waits for new evidence;
    integrity failures quarantine the method/configuration. No broad catch should
    silently turn every transient condition into permanent manual intervention.
12. Measure and bound each quantum and final publication. Stop/foreground events
    preempt before the next quantum. Redesign an oversized synchronous phase;
    labeling its thread low priority does not make a long held lock harmless.
13. Expose meaningful automatic status: healthy, optimizing, deferred, repairing
    or action required. Keep model internals and resource-budget details out of
    normal user flows; diagnostics remain available separately.

### Acceptance and failure checks

- In disconnected operation, changing workloads lead to useful automatic policy
  changes, and restarting retains valid progress/advice without another full scan.
- Under CPU/GPU/memory/I/O contention, foreground bytes and operations remain
  correct; traces show the actual deferral/yield behavior and interference.
- Corrupt/lose advice and change host/runtime. Reads still work; policy rebuilds
  without an unavailable online model or repeated device-search loop.
- Repeated unchanged opportunities cause no needless data writes. Workload
  exploration and migration consume the same measured background/write budgets.
- Test sustained foreground activity, prolonged idle, stop/resume, failed
  observations and a recovered provider. Do not require a busy host to become idle
  merely to obtain a green test; record which live branches actually executed.

**Dependencies:** E02/E04/E06 for the complete optimizer; E03 for GPU evidence.
**Existing ledger:** A01, A03, L01, A04. The current 32-profile/16-benchmark area
is an advisory primitive, not the production coherence database or a learner.

## 11. E08 — Windows semantics, Explorer and owned volume lifecycle

The sealed memory path implements the user-mode assembly and external-client
fixture. The exact WinFsp v2.2B5 pair passed sealed reads, writable mounts and
the helper's attach, detach and reattach workflow in Windows 11 Sandbox.
Supported install and rollback, driver retirement and interactive Explorer
qualification remain open. [Sealed memory path](WINFSP_READONLY_MEMORY.md) ·
[Windows 11 mounted evidence](evidence/windows11-sandbox-memory-mount-2026-11-18.json)

The signed stable 2.2 pair also passed the same helper's memory and
persistent workflows in an offline guest. The persistent path handled
explicit launch, external Win32 writes, detach, exclusive-create refusal
and verified reopen in a fresh process.
[Mounted activation evidence](evidence/windows11-mounted-volume-activation-2026-11-21.json)

**Reuse:** actual native SDK callbacks, Windows-global disposition, ACL/time
validation, deferred responses, owned controller, shared memory factory and tray.
**Planning effort:** 5–10 person-weeks; confidence low.

### Implementation steps

1. Select and verify the user-mode/runtime/driver combination from exact source,
   signatures and supported ABI evidence. Extend the signed v2.2 guest baseline
   to the selected Windows client and exact deployment package. An older or
   different installed driver needs explicit compatibility evidence.
2. Add an explicit native provider implementation and reproducible assembly of
   volume backing, engine, dispatcher, resource controller and supervision.
   Constructor defaults must not silently select privileged setup.
3. Qualify a disposable native read-only view first. Read-only means rejecting
   mutation, including cleanup/metadata side effects, not only hiding a write button.
4. Use an external client process to enumerate/read/verify the mounted fixture,
   then stop and independently inspect retained data. A same-process callback
   invocation does not satisfy this gate.
5. Qualify a new writable memory fixture through external clients: create/open,
   read/write/append/overwrite, flush, rename, delete, metadata, security and directories.
   Preserve each partial failure and exact retirement evidence.
6. The owned writable controller now uses explicit stop, dispatcher events
   and session fencing for its lifetime. Test and worker supervisors enforce
   their own deadlines. Qualify the new lifetime through mounted Windows 11
   clients, including a sustained idle hold, resume of I/O, orderly dismount
   and independent recovery after stop.
   [Controller contract](WINFSP_OWNED_CONTROLLER.md)
7. Qualify the versioned indexed name profile with Explorer and Win32 clients
   on Windows 11. Cover case aliases, displayed spelling, both Unicode forms
   of a visibly identical name, long-path-aware applications and reserved-name
   rejection. Verify current and legacy images after dismount and reopen.
   [Indexed name profiles](INDEXED_NATIVE_VOLUME.md#name-profiles)
8. Complete share modes, delete-pending/rename/replace, allocation/EOF/sparse
   behavior, file IDs, directory cursor semantics and error/status mapping.
   Add hard links, streams, reparse points or other promised features only with
   correct storage/lifetime semantics; explicitly report unsupported features.
9. Qualify cached/noncached and paging/mapped I/O, flush/cleanup ordering, process
   exit, cancellation and concurrent readers/writers. Test actual Windows clients
   and applicable WinFsp/Windows suites, not only direct table calls.
10. Introduce immutable pinned read views and bounded concurrency where safe.
    Keep a clear writer linearization point; do not trade away correctness for
    multiple callback threads or allow destruction to race active callbacks.
11. Make the helper manage create/mount/unmount/status and existing volumes using
    the same engine. Explorer supplies the normal folder view. Avoid an internal
    whole-volume file list or text editor as the product's mandatory access path.
12. Support safe unused-letter/new-directory selection and atomic collision
    refusal. Never mount over an existing directory/drive or infer ownership from
    a path string alone. Pin the objects used for cleanup.
13. Design process ownership deliberately: GUI/tray failure, volume-host failure,
    GPU-helper failure and backing-owner failure are different cases. Decide
    where temporary backing survives and who performs recovery/unmount. The GPU
    worker must not be the sole owner of acknowledged filesystem data.
14. Make stop asynchronous to the GUI and explicit about pending file users,
    unconfirmed teardown and retained temporary data. Do not destroy unsafe
    owners to meet a UI timeout or announce retirement before it occurs.
15. Add current Windows 11 client qualification and a published feature matrix.
    Record exact OS/build/security/cache settings. Windows Server CI or an older
    client pass is not a substitute for the target client.

### Minimum external-client matrix

| Area | Cases that must be covered |
| --- | --- |
| Data | Empty/small/large files, unaligned and partial writes, EOF extension, holes, append, truncate and low-space partial failure |
| Namespace | Nested/large directories, Unicode/case collisions, long paths, rename/move/replace, delete-pending and enumeration during changes |
| Handles | Share/access combinations, repeated cleanup/close, stale tokens, open-after-delete, process termination and cancellation |
| Metadata/security | Times/attributes/size/allocation, ACL inheritance/query/change, denied access and unsupported operations |
| Caching | Cached/noncached reads/writes, mappings, cleanup/flush ordering, concurrent views and consistency across publication |
| Lifecycle | Failed preflight/create/start/mount, unexpected dispatcher stop, explicit stop, GUI/tray exit and retained-backing recovery |

**Acceptance:** WinFsp `winfsp-tests`/`winfstest` and Windows FSX/applicable
Windows tests run against the actual mounted product configuration, with exclusions
listed by the promised feature contract. No kernel/driver fault induction on the
owner host. **Existing ledger:** W01, D01; scalable completion also requires E01.

## 12. E09 — Multi-release readers and gradual format migration

**Reuse:** existing flat/paged capabilities, typed heads, compatible resume,
immutable representations and COW handoff. **Planning effort:** 3–6 person-weeks.

### Implementation steps

1. Inventory every currently readable root, namespace, record and codec capability.
   Freeze exact valid/corrupt/truncated fixtures with source/provenance and CPU results.
2. Specify the backward-read support window before a first release. A proposed
   baseline is current software reading volumes from at least the three preceding
   releases. Retain required decoding support while live supported data depends on
   it. Publish exact reader, writer and migration compatibility separately.
3. Maintain a method/capability registry with no ID reuse, closed composition,
   required CPU decoders and clear unknown-feature behavior. Product release
   numbers remain owner-controlled; format design is not an automatic release bump.
4. Distinguish backward readability from forward readability and downgrade.
   An older program cannot be promised arbitrary new-method support; prevent it
   from writing a volume whose required features it does not understand.
5. Allow mixed old/new representations while a migration runs. Resolve each
   object by its stored method/version and protected dependencies, not mutable policy.
6. Persist bounded migration continuation and selection state. Migrate cold data
   gradually in COW transactions and reuse E07 foreground/write budgets.
7. Verify decoded equality, metadata/security and selected root/dependency state
   before publishing. Keep the old generation pinned until rollback is no longer needed.
8. Define explicit rollback boundaries, especially after a new writer begins
   creating data an old reader cannot decode. Never advertise a reversible update
   that would require silently dropping newly acknowledged data.
9. Separate rebuildable learner schema updates from mandatory data/decoder
   updates. Invalid advice must not make an otherwise supported old volume unreadable.
10. Run frozen-reader and interrupted-update matrices across supported versions,
    including GPU absent and partial/corrupt reserved-area writes.

**Acceptance:** every supported reader/volume pair has an explicit result; upgrade
interruption preserves acknowledged bytes and CPU recovery; gradual migration
does not force a full rewrite on mount or unbounded background load.
**Dependencies:** E01/E02/E06 for new scalable/collective formats.
**Existing ledger:** U01, L01, R01.

## 13. E10 — Safe dependency setup, packaging and supported volume workflows

**Reuse:** static CPU profile, manifests/notices, read-only SDK extraction,
source-pinned tools, helper/tray and module verification.
**Planning effort:** 3–6 person-weeks; confidence low.

### Implementation steps

1. Define supported Windows 11 x64 packages and required/optional components.
   Bundle CPU decoders for every writable format; GPU absence must not trigger
   a missing-decoder download just to read existing files.
2. Discover installed capabilities/paths safely and support explicit configuration.
   Avoid development-checkout paths, drive letters, a fixed RAM/GPU model or a
   presumed globally installed 7-Zip, Python, CMake or ROCm in product behavior.
3. Provision missing user-mode tools/runtime pieces into a private verified
   location where needed. Verify exact archives, selection, signatures/provenance,
   disk reserves and atomic installation; retain rollback and partial-failure evidence.
   Reuse the same verified artifact discovery, exact executable identity and
   side-by-side runtime checks where their contracts apply to FlitzFS.
   [FlitzZip build receipts](https://github.com/strmt7/FlitzZip/blob/11396b5123505a4a1959173e1db2d0858c10dd6d/tools/native_build_receipt.py) ·
   [FlitzZip runtime packaging](https://github.com/strmt7/FlitzZip/blob/11396b5123505a4a1959173e1db2d0858c10dd6d/tools/package_runtime.ps1)
4. Separate driver setup from ordinary private packages. Never replace/reconfigure
   the GPU driver. A needed filesystem-driver install must satisfy the owner's
   stability and artifact/client/rollback qualification, without assuming a
   signature or a clean build proves perfection.
5. Define helper/volume-host/broker privilege and authenticated local IPC. Use
   least privilege, bounded frames/queues and explicit ownership; no online model
   service or globally writable control endpoint is required.
6. Implement supported creation/open/mount/unmount/recovery workflows for temporary
   memory and persistent providers. The [indexed volume-file provider](PERSISTENT_VOLUME_FILE.md)
   now has exclusive creation, checked reopen, independent CPU inspection and
   a WinFsp callback entry point. The helper's attachment worker now owns a
   qualified file through mount failure, detach, reopen and exit. User-facing
   selection is exercised in the mounted guest. Qualify the installed
   Explorer lifecycle, available-space accounting and temporary ownership
   under helper and system interruption. A direct block-volume provider needs
   its own design and tests.
7. Build any direct-volume provisioning/format support only under explicit product
   design and isolated disposable-device tests. Development never formats or writes
   raw owner devices, and this plan is not permission to do so.
8. Make installation/update/uninstall preserve user volumes, backups and profiles
   that are not installer-owned disposable files. Exercise partial install, failed
   upgrade, rollback, relocation, path spaces and normal-user launch.
9. Produce a component manifest/SBOM, licenses/notices, exact source/build binding,
   trusted update metadata and anti-rollback/compatibility checks appropriate to
   the supported update policy. Distinguish a local digest from vendor provenance.
10. Keep user documentation concise: what works, creation/mount choices, temporary
    versus persistent data, recovery and meaningful status. Put algorithms,
    budgets, traces and development instructions in linked technical guides.
11. Verify no unintended service, task, autostart entry, registry residue, process
    or data deletion after installation/uninstall and development tests. Necessary
    product persistence must be explicitly designed/owned, not left by a test.

**Acceptance:** clean-machine and update/rollback qualification on the supported
client; normal-user operation; required independent CPU recovery present;
compatible missing packages handled safely without assumed host layout.
**Existing ledger:** W01, U01, Q01. Do not publish a package as part of this plan.

## 14. E11 — Durability, repair, and independent failure qualification

This starts with each implementation change and continues through final release
qualification. It is not a final pass of extra assertions after design is frozen.
**Planning effort:** 6–12 person-weeks, excluding unavailable hardware campaigns.

### Implementation steps

1. Specify acknowledgement and flush semantics for each provider and API mode.
   Separate process-lifetime ordering from host-power persistence and document
   write-failure domains. Record the difference between native response attempt
   and client confirmation, including mapped writes. Validate real backend
   guarantees externally.
2. Maintain an independent oracle for acknowledged file bytes, names, metadata
   and required dependencies. Its expected state must not be reconstructed by
   the same production writer/selection code being tested.
3. Enumerate publication transitions for data, namespace, free space, heads,
   models/dictionaries, advice, repair and migration. Associate each with the
   failure model and required old/new/acknowledged state.
4. Test torn/partial writes, failed flushes, reorderings permitted by the model,
   stale identities, damaged peers, missing records and unexpected process exit.
   Keep source fixtures and ambiguous results; do not retry until a cause/state
   change justifies a different experiment.
5. Extend automated verified scrub and repair. A verified live indexed
   fallback or idle scrub schedules append-only repair from a surviving
   replica. A verified alternate-image switch can restore redundancy when
   appending is blocked and a target fits. Closed indexed reopen restores
   verified replicas before writable admission. The Windows providers
   can select a verified alternate when startup cannot append. The idle
   scrub can resume a persisted verified prefix of the same image.
   Healthy writable reopen reuses its checked image writer through the
   selector and image barriers. Further scale work must replace routine
   whole-image passes with checked index paths and durable evidence while
   preserving fault detection. Persist repair intent, add index
   reconstruction, and qualify persistence under physical failure. Preserve the acknowledged namespace and retain evidence when
   intact data cannot be reconstructed.
6. Preserve independent CPU export/recovery tooling and useful diagnostics when
   normal mounting or the GPU path is unavailable. Backup/redundancy needs must
   match the claimed failure domains; same-memory copies do not survive power loss.
7. Fuzz parsers, codecs, references, names, worker frames and update manifests
   under appropriate sanitizers. Include allocation/size overflows, cycles,
   decompression amplification and worst-case CPU work before allocation/execution.
8. Assess hostile media, untrusted filenames, DLL/package replacement, local IPC,
   privilege boundaries and cross-user grouping side channels. Decide explicit
   authentication, encryption and key recovery requirements. Define sharing
   domains before deduplication or learning can cross security boundaries. Exercise
   offline export and recovery with an independent tool, including access control,
   missing keys and supported backup/restore. Checksums detect accidental damage.
9. Verify canonical byte comparison/digest and lifetime checks around every GPU
   result. Exercise CPU selection and completion races without inducing unsafe
   owner-host kernel/driver faults.
10. Run current compatible dependency/advisory checks and bind all selected source
    and runtime identities. A zero Dependabot queue is not the security assessment.
11. Maintain configuration-specific qualification requirements, including sanitizer
    process overhead and parent/child aggregate budgets. Refuse inadequate
    resources honestly; do not weaken sanitizers, fabricate idle telemetry or
    change a production worker safety policy to make a GUI fixture pass.
12. Resolve the historical unexplained retirement/abnormal-exit cases with captured
    root cause and a targeted regression, or keep their release gate open.
13. Run persistent-backend and real driver/power-fault campaigns only in isolated
    qualified environments. Record what an emulator cannot establish about actual
    hardware/firmware and do not substitute RAM results for physical persistence.
14. Repeat full relevant current-source codec-enabled/raw, Windows SDK/client and
    supported sanitizer/analysis configurations. Every reported result identifies
    source, configuration, backing, exact binaries, scope and retirement.

### Required failure matrix dimensions

| Dimension | Required variation |
| --- | --- |
| Publication | Before data, during data, before/after flush, between metadata peers, root switch and retirement |
| Representation | Raw, CPU codecs, shared/learned methods, old/current versions and mixed dependencies |
| Operation | Create/write/append/overwrite, truncate/delete/rename, grouping, compaction, repair and migration |
| Runtime | No GPU, qualified GPU, rejected result, busy resources, missing runtime, cancellation and child exit |
| Reserved state | Missing/degraded/corrupt/torn advisory state and required dependency state treated separately |
| Ownership | Active readers/contexts, deferred response, stale generation, stop and failed retirement |
| Provider | Temporary RAM model, qualified persistent file and qualified direct-volume model where supported |

The complete Cartesian product is not asserted by a small fixture. Use exhaustive
bounded transition tests, randomized/stateful campaigns and risk-based combinations;
publish the uncovered cells. **Existing ledger:** D01, R01, Q01, G01/G02.

## 15. E12 — Recognized, unbiased size and performance qualification

**Reuse:** the [benchmark protocol](BENCHMARK_PROTOCOL.md), source manifests,
exact readback and resource/process containment. **Planning effort:** 3–6 person-weeks.

### Implementation steps

1. Recheck current tool versions and pin source/binaries. Use Microsoft DiskSpd
   and the repeatable Windows file-tree workflows in the
   [release benchmark protocol](RELEASE_EXECUTION_PLAN.md#8-benchmarks-soak-and-the-release-decision).
   Use WinFsp tests/FSX for semantics and lzbench or strong codec controls for
   codec work.
2. Keep custom tests for properties those tools do not supply: group discovery,
   total bytes/dependencies, CPU/GPU failure, migration and acknowledged-write recovery.
3. Build exact real-corpus manifests with source/license/digests, training/held-out
   separation and controlled family/version relationships. Add random and already
   compressed controls; do not select only nearly identical synthetic data.
4. Cover text/source/logs, structured data, binaries, media, documents/containers,
   numeric/model families and mixed unrelated data. Bound downloaded/local storage
   and do not execute untrusted corpus binaries.
5. Use NTFS on the current Windows 11 build as the primary control, both with
   defaults and with supported compression. Include supported ReFS and other
   filesystem controls on qualified comparable backings. Preserve the operating
   system, volume, capacity, corpus and workload configuration in each receipt.
6. Use an in-memory WinFsp reference where useful to separate bridge overhead
   from FlitzFS costs. Keep real filesystem, image-size and archive/codec-only
   comparisons in explicitly different result panels.
7. Compare equivalent protection, snapshots, cache/durability semantics, resource
   envelopes and corpus bytes. Also show native defaults separately. Charge
   learned models, indexes, fixed metadata, redundancy, retained history and padding.
8. Measure ingestion, a declared normal background opportunity budget and settled
   state. Do not stop when a favorite method happens to have its best result.
9. Measure sequential/random small/large reads/writes, mixed workloads, metadata
   and large-directory work, warm/cold access, CPU-only/GPU paths and background
   interference. Report latency distributions and amplification, not just throughput.
10. Use repeated alternating/randomized order, adequate duration, source-matched
    builds, equal security settings and complete raw results. No owner-host global
    cache purge, antivirus change, driver tuning or clock/power modification.
11. Report RAM-based measurements as RAM-based. They expose algorithm/CPU/GPU
    overhead and do not establish SSD/HDD latency, endurance or power-loss behavior.
12. State which measured workload/constraints a claimed record covers. Compare
    against current applicable strong controls, and distinguish a compression
    leaderboard result from a writable filesystem with practical random access.

### Result bundle

For every comparison retain source/build/tool/runtime identities, corpus manifest,
settings/order/seeds, allocated/live/temporary bytes, ingest and access timings,
CPU/GPU/transfer/resource costs, background writes/interference, exact readback,
all failures and cleanup. Publish distributions/confidence and inconclusive cases.

**Acceptance:** the primary total-size advantage and useful performance survive
held-out matched comparisons and E11 correctness gates. No kernel-only timing,
active-payload figure or one small request constitutes this result.
**Dependencies:** integrated E03/E06/E07/E08. **Existing ledger:** Q01, G03, A04.

## 16. E13 — Stabilization and the owner's release decision

**Planning effort:** 6–12 person-weeks; real soak/hardware coverage is not removed
by faster code generation.

### Implementation steps

1. Freeze a candidate's source, dependency/runtime set, decoder registry, feature
   contract and qualification plan. No product numbering or publication is implied.
2. Run repeated representative use on the supported Windows client and different
   CPU/GPU/RAM configurations, including CPU-only and changed-host recovery.
3. Exercise sustained create/edit/delete workloads, large namespaces, idle
   optimization, near-full conditions, user/process exit and interrupted maintenance.
   Keep the planned 30-day exact-candidate soak and 90-day rolling stability
   evidence across supported configurations. Resolve defects found during soak.
4. Track regressions and leaks over repeated cycles; compare resource trends and
   acknowledgement/recovery state, not just one clean process exit.
5. Require resolution of known critical/high-impact data, security, lifecycle and
   compatibility failures. A later clean run does not erase an unexplained failure.
6. Repeat exact-candidate recovery, migration, installer/update/uninstall and
   independent benchmark gates. Retest affected scopes after every stabilization fix.
7. Publish the supported limits, failure model, unsupported cases and exact
   evidence. Do not claim universal no-loss behavior or superiority over every
   possible input/method when the evidence only covers particular conditions.
8. Present the complete concrete candidate and remaining risks to the owner.
   Only the owner authorizes release, tags, packages or product-version changes.

**Exit:** all requirements mapped in section 2 and release gates below have
authoritative evidence for the exact candidate, with no required work left open.
The active full-product goal is not complete merely because a development round is.

## 17. Completion dependency graph and effort

These are package **completion** dependencies. Independent design, regression
work, bounded research and early integration steps should proceed sooner.

| Package | Completion dependencies | Planning effort, person-weeks |
| --- | --- | ---: |
| E01 scalable native storage | Existing paged foundation | 6–12 |
| E02 allocation/reclamation | E01 | 5–10 |
| E03 useful automatic GPU path | Existing protected service; E08 integration substeps | 4–9 |
| E04 persistent coherence | E01 | 3–7 |
| E05 candidate research | Corpus/accounting/decoder controls | 6–16 |
| E06 collective representation | E01, E04, E05; E02 for settled-size proof | 5–12 |
| E07 autonomous optimizer | E02, E03, E04, E06 | 4–9 |
| E08 Windows product | E01 for promised scale; native-provider qualification | 5–10 |
| E09 compatibility/migration | E01, E02, E06 | 3–6 |
| E10 supported package/workflows | E08, E09 | 3–6 |
| E11 independent failure qualification | Runs throughout; final relevant implementations | 6–12 |
| E12 comparative qualification | E03, E06, E07, E08 and applicable E11 correctness gates | 3–6 |
| E13 stabilization | E02, E03, E06–E12 | 6–12 |

Total: **59–127 person-weeks**, conventionally rounded to **60–130**, plus an
unbounded research risk if the desired compression frontier is not reached.
This is a low-confidence work estimate, not a deadline, a completion percentage
or an assertion about this agent's coding speed. Do not convert it mechanically
into calendar time or lower it because another fixture passed. Re-estimate after:

- A bounded scalable native engine with populated-file measurements.
- The first held-out collective comparison including access and protection costs.
- The installed Windows path with lifecycle and interference evidence.
- The first real persistent-backend interruption and repair campaign.

Those results determine whether the remaining work is primarily implementation,
an architectural correction or an unresolved compression research problem.

## 18. Exact release gates

The [release execution plan](RELEASE_EXECUTION_PLAN.md) maps these gates to
the current evidence, the order of implementation and the next checkpoints.

| Gate | Required result |
| --- | --- |
| S1 | Reproducible exact source/runtime/dependency inventory and appropriate full current-source validation |
| S2 | Actual scalable native populated files/directories and bounded resident state, not only paged object keys |
| S3 | Automatic verified reclamation reduces occupied bytes while preserving readers, dependencies and acknowledged data |
| S4 | Useful real AMD work in an automatic ordinary/collective product path; independent CPU fallback/readability |
| S5 | Strong held-out collective TOTAL-size results and practical access against current applicable controls |
| S6 | Persistent offline learning/continuation, corrupt-advice isolation and measured low background interference/write restraint |
| S7 | Promised Windows/Explorer/application behavior and clean owned lifecycle on the target Windows 11 client |
| S8 | Backend-specific durability, repair and independent acknowledgement/failure qualification |
| S9 | Supported backward-reader window, mixed methods and interrupted gradual upgrade/rollback behavior |
| S10 | Security/dependency/IPC/package/update/uninstall qualification and required CPU recovery tooling |
| S11 | Recognized reproducible comparisons with complete raw results, costs and limitations |
| S12 | Representative exact-candidate soak, resolved material failures and the owner's separate release approval |

A missing gate stays missing. A default-disabled unsupported feature does not
close a requirement that calls for automatic operation. No release is created by
this plan or by reaching an intermediate vertical slice.

## 19. Efficient execution rules for the active goal

- Keep the full goal intact. Select the next highest-value incomplete product
  slice from this plan; do not redefine completion around a safer fixture.
- Inspect current local state, preserve the owner's checkout and work on `main`.
  Build and verify locally. Publish meaningful checkpoints through the official
  GitHub connector procedure, binding the expected parent and exact tree.
- Use current source-bound semantic navigation and recent reputable primary
  research for consequential choices. Reuse a valid unchanged index/evidence;
  reuse the verified result when an unchanged check covers the same input.
  Choose the next experiment to resolve a release gate or an observed high
  product cost. Stop adding variants when the existing comparison can decide
  the next implementation.
- Freeze actual inputs before final qualification. On a failure, retain it,
  diagnose the mechanism, change only justified code and rebind affected evidence.
  Do not edit source during a successful final run and keep its old attribution.
- Scope tests by changed dependencies. Native, raw, codec-enabled, sanitizer,
  SDK, mounted, physical and GPU results remain separate. Test counts and tool
  success flags do not imply unexercised behavior.
- Make qualification resource-aware and finite. Record legitimate busy-host
  deferral and unexecuted publication coverage; never fabricate quiet samples,
  weaken sanitizer settings or leave an owned process after observation stops.
- Keep current status concise and link immutable evidence. New component contracts
  hold requirements once; historical checkpoints stay historical. The root README
  stays end-user oriented, with no commit/testing diary.
- Missing safe user-mode tools should be provisioned privately and automatically
  when needed. Unsafe/unqualified deployment or hardware-fault work does not block
  independent storage, codec, CPU recovery, planning or application work.
- Update the execution ledger with implemented behavior, exact source, executed
  checks and the next concrete action. Keep the harness goal active until the
  complete release requirement audit is actually satisfied or the owner pauses it.

## 20. Limits on universal claims

A lossless compressor cannot shorten every possible input. There are `2^n`
bit strings of length `n`, but only `2^n - 1` bit strings shorter than `n`; an
injective decoder cannot map every length-`n` input to a shorter unique output.
Cross-file context can reveal redundancy in a collection, but it does not remove
this counting limit. Thus the research target is exceptional measured reduction
on real workloads, including cross-file structure, with honest nonexpanding
fallback on unsuitable inputs. No online model or hidden dictionary may be
omitted from required reconstruction/accounting.

Likewise, checksums and self-repair do not recreate information after every
protected copy/dependency is destroyed, nor can a test prove the absence of all
future bugs. The ambitious engineering target is strong verified failure
coverage, automatic safe repair where information survives, independent recovery
and no silent corruption or unjustified success claim. These limits are not a
reason to drop the GPU, compression, Windows or autonomy requirements.
