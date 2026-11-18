# FlitzFS detailed implementation plan

**17 November 2026 — baseline `38a4a48f005c2ea6c9a7f8cbd9d7ee07df044d70`.**

Read the [current repository overview](REPOSITORY_OVERVIEW.md) first. This plan
extends the existing E01–E13 release work breakdown and maps it to the active
[development ledger](DEVELOPMENT_PLAN.json). Proposed types, formats and tests
below are implementation work, not claims that they already exist.

The objective remains the complete Windows 11 x64 filesystem: normal Windows
access, useful automatic AMD GPU work, independent CPU recovery, very strong
collective lossless compression and autonomous low-impact learning/maintenance.
The owner alone decides when to release. This plan creates no release, tag,
package publication or product-version change.

The 18 November checkpoints extend the baseline with indexed head selection,
WinFsp SDK access, and on-demand locking in the shared memory provider.
The owned indexed memory path now measures obsolete records, stages compaction
automatically and releases unpinned backing in bounded quanta. The fixed
reservation fraction and 4 MiB image floor were replaced with measured
headroom and storage geometry. Follow E02 for reclaiming segments under
foreground writes and E03 for useful automatic AMD routing.
[Current state](DEVELOPMENT_STATE.json)

## 1. Decisions that govern implementation

1. **Keep the shared engine and WinFsp direction.** WinFsp supplies the Windows
   bridge; the filesystem engine supplies storage, compression and recovery.
   Explorer and ordinary applications use the mounted interface. The tray owns
   volume management and status. Consider a new kernel driver only after matched
   measurements demonstrate a material limitation in the user-mode architecture.
2. **Replace the native access model, not its constants.** Retain the paged
   object foundation, but replace whole-namespace maps and repeated aggregate
   validation with checked paged inode/name/extent access and bounded caches.
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

Work can begin on independent substeps before a whole package closes. Package
completion dependencies below are not instructions to defer testing until the end.

| Stage | Concrete result | Must be demonstrated before advancing the claim |
| --- | --- | --- |
| A | One assembled Windows volume path | Shared factory + explicit matched native provider + external-client byte verification + joined retirement; read-only exposure first |
| B | A scalable native engine | Paged inode/name/extent operations, bounded caches and populated-file scale tests; not merely object keys |
| C | A smaller live volume | Protected groups, automatic incremental reclamation and complete-size accounting under concurrent edits |
| D | Useful automatic GPU work | Qualified automatic discovery/cost acquisition, real useful work, CPU read/fallback and exact completion |
| E | A persistent autonomous optimizer | Resumable sparse coherence, workload learning, measured background interference and rewrite restraint |
| F | Release qualification | Windows behavior, persistent providers, repair, compatibility, packaging, independent comparisons and soak all satisfy their exits |

The immediate work after this review is:

1. Qualify the pinned local toolchain and required CPU codecs. Keep reproducible
   CPU recovery and compare current Windows caller paths against this build.
2. Resolve the native WinFsp SDK, DLL, driver and client compatibility as an
   explicit identity. Qualify the smallest external-client memory-backed path,
   from verified read-only access through stop and resource retirement.
3. Build the first paged inode/name/extent transaction, checked range reads and
   bounded allocation accounting under the shared session interface. Validate
   the old and new CPU readers using identical logical operation traces.
4. Exercise the existing staged handoff as a bounded transition and recovery path.
   Prototype packed incremental reclamation under continuous foreground commits
   before promoting an automatic reclaimer as the production default.
5. Start the cost and size comparison workstreams independently. Measure useful
   automatic AMD roles and compare stronger collective candidates on a bounded
   real corpus using the same finalized-byte accounting.

Do not spend another sequence of rounds repeating the unchanged 1-MiB GPU trial,
adding disconnected transport probes, or expanding historical documentation in
place of these results.

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
   [Indexed owned path](evidence/indexed-owned-windows-2026-11-18.json) ·
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
5. Use real checked page ranges in WinFsp callbacks, including large directory
   enumeration, allocation and ACL queries. Add long-path and Unicode behavior
   against the selected Windows contract. Replace foreground subtree scans with
   bounded validation. Run SDK, mounted Windows and recovery suites separately.
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
    payload access. Add an explicit incremental scrubber for untouched pages;
    do not achieve speed by dropping integrity checks.
12. Add a streaming legacy-to-new-layout migration into a new target and verify
    data, namespace and required opaque metadata before COW selection.

### Acceptance and failure checks

- Exercise 1,000, 16,000, 100,000 and at least 1,000,000 **named nonempty files**,
  a 100,000-entry directory and populated-file reads beyond cache capacity.
  Counts/sizes adapt to safe test headroom; an unexecuted larger rung stays open.
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

**Reuse:** mirrored payloads, COW records, protected anchors, staged rebuild,
context-preserving head handoff and source-preserving recovery.
**Available:** bounded indexed handoff and automatic staged compaction on the
owned Windows memory provider, using measured obsolete history and CPU
verification. **Missing:** resumable segment evacuation under foreground
commits, durable liveness/progress, and persistent-provider qualification.
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
   writes and validates both anchors at selection. Persist candidate identity
   and continuation where a provider can durably reopen it. Measure foreground
   interference and verify selection during mounted Windows 11 activity.
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
    rewrite amplification. Feed them to E07 rather than resetting a wear counter
    on every new maintenance session.

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

### Implementation steps

1. Build a trusted configuration identity from OS/architecture, adapter LUID/node,
   driver/runtime/module/compiler identities and decoder/method versions.
   Preserve the ROCm Core/package versus HIP component version distinction.
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
   on eligible background demand. Produce package profiles from representative
   measurements, connect packaging to the mounted launcher and verify routing
   on hosts with multiple AMD devices. Treat the observed foreground CPU
   choice as specific to the tested request class.
   [Foreground route evidence](evidence/installed-amd-routing-2026-11-18.json) ·
   [Installed shared-base evidence](evidence/installed-amd-shared-base-2026-11-18.json)
7. Amortize setup over finite admitted work windows. The native indexed
   path can now cover several independently bounded launches in one worker
   exchange and charges the complete host footprint. Extend finite exchanges
   to collective work across files. Bind each job to a source revision,
   verify every decoded byte before publication, and retire the helper when
   admitted work drains or foreground demand rises. Compare complete occupied
   bytes and foreground interference with a matched CPU baseline, including
   unsuccessful or cancelled batches. For reference selection, compare
   bounded candidate retrieval and actual delta cost. Include held-out data
   that lacks a natural base. Use protected, independently readable references
   and record search cost as part of the complete comparison.
   [Finite exchange evidence](evidence/finite-amd-exchange-2026-11-18.json) ·
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
cooperative preparation. Funded advisory checkpoints now preserve scan
continuation and the bounded representative window. In the 72-extent restart
fixture the second owner sampled 10 extents and extended the same base. The
next step is to extend the persistent sparse postings for long-range discovery
and charge their occupied bytes and write traffic to the compression benefit.
[Restart validation](evidence/persistent-indexed-discovery-2026-11-18.json)
[Indexed discovery](INDEXED_NATIVE_VOLUME.md#automatic-candidate-discovery)

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
2. Create protected internal paged roots for scan continuation, file/chunk
   sketches, candidate postings, accepted group membership and outcome history.
   Keep them out of the user-visible folder namespace.
3. Use file-level sketches to narrow searches, then chunk/region features where
   worthwhile. Evaluate content-defined and shift-tolerant features alongside
   the current aligned baseline; do not index every byte indiscriminately.
4. Implement bounded sparse candidate queries using appropriate locality/hash
   indexes and small approximate-neighbor sets. A dense million-by-million
   32-bit matrix would cost roughly 4 TB before other state; it is not the design.
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
Microsoft's 2026 DirectStorage Zstd work is worth a bounded evaluation where its
destination/readback model fits; lossy conditioning is excluded. See the cited
primary sources in the [overview](REPOSITORY_OVERVIEW.md#what-current-research-changes).

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

Indexed capability 4 supports anchor-copy members alongside aligned members.
A controlled Zstandard-enabled source fixture compacted from 592 KiB to 192 KiB
with byte-exact CPU readback, and the format promotion passed 528 modeled
interruptions per lane. Compare CPU and AMD matching on many related regions
and on mixed corpora. Count index bytes, transfer costs and CPU readback when
selecting a method for the ordinary optimizer.
[Shifted group validation](evidence/shifted-indexed-groups-2026-11-18.json)

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

### Implementation steps

1. Define separate budgets for foreground ingestion, optional background work,
   recovery/repair and calibration. Derive them from current capabilities and
   available resources, not one development machine's capacities.
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

The [sealed memory path](WINFSP_READONLY_MEMORY.md) implements the user-mode
assembly for steps 1–3 and the executable external-client fixture for step 4.
The staged stable SDK matches the installed user-mode DLL. Evidence of a real
read-only mount still requires the disposable driver-backed run.

**Reuse:** actual native SDK callbacks, Windows-global disposition, ACL/time
validation, deferred responses, owned controller, shared memory factory and tray.
**Planning effort:** 5–10 person-weeks; confidence low.

### Implementation steps

1. Select and verify the user-mode/runtime/driver combination from exact source,
   signatures and supported ABI evidence. The installed v2.1 driver and staged
   v2.2B5 SDK are not proved interchangeable merely because both are WinFsp.
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
7. Specify Windows name/case semantics, UTF-16/UTF-8 representation, long paths,
   reserved names, case preservation and normalization policy. Implement them in
   native indexes and adapters with collision/security tests.
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
4. Separate driver setup from ordinary private packages. Never replace/reconfigure
   the GPU driver. A needed filesystem-driver install must satisfy the owner's
   stability and artifact/client/rollback qualification, without assuming a
   signature or a clean build proves perfection.
5. Define helper/volume-host/broker privilege and authenticated local IPC. Use
   least privilege, bounded frames/queues and explicit ownership; no online model
   service or globally writable control endpoint is required.
6. Implement supported creation/open/mount/unmount/recovery workflows for temporary
   memory and persistent providers. Persistent file-backed images are distinct
   from a direct block-volume provider; neither may be claimed without its own tests.
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

## 14. E11 — Durability, repair, security and independent failure qualification

This starts with each implementation change and continues through final release
qualification. It is not a final pass of extra assertions after design is frozen.
**Planning effort:** 6–12 person-weeks, excluding unavailable hardware campaigns.

### Implementation steps

1. Specify acknowledgement and flush semantics for each provider and API mode.
   Separate process-lifetime ordering from host-power persistence and document
   write-failure domains. Validate real backend guarantees externally.
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
5. Implement automated verified scrub/repair and clear containment of uncorrectable
   damage. Recovery must not silently choose an older acknowledged namespace,
   manufacture file bytes or delete material needed for later recovery.
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
   for file I/O, appropriate native Windows fio workloads where supported,
   WinFsp tests/FSX for semantics and lzbench or strong codec controls for codec work.
2. Keep custom tests for properties those tools do not supply: group discovery,
   total bytes/dependencies, CPU/GPU failure, migration and acknowledged-write recovery.
3. Build exact real-corpus manifests with source/license/digests, training/held-out
   separation and controlled family/version relationships. Add random and already
   compressed controls; do not select only nearly identical synthetic data.
4. Cover text/source/logs, structured data, binaries, media, documents/containers,
   numeric/model families and mixed unrelated data. Bound downloaded/local storage
   and do not execute untrusted corpus binaries.
5. Compare NTFS uncompressed/compressed and supported ReFS configurations, plus
   suitable other filesystem/image controls in isolated environments. Do not
   format owner storage or claim a cross-OS throughput comparison is matched.
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
| E11 independent failure/security qualification | Runs throughout; final relevant implementations | 6–12 |
| E12 comparative qualification | E03, E06, E07, E08 and applicable E11 correctness gates | 3–6 |
| E13 stabilization | E02, E03, E06–E12 | 6–12 |

Total: **59–127 person-weeks**, conventionally rounded to **60–130**, plus an
unbounded research risk if the desired compression frontier is not reached.
This is a low-confidence work estimate, not a deadline, a completion percentage
or an assertion about this agent's coding speed. Do not convert it mechanically
into calendar time or lower it because another fixture passed. Re-estimate after:

- a bounded scalable native engine with populated-file measurements;
- the first held-out collective comparison including access/protection costs;
- the real assembled Windows path with lifecycle and interference evidence.

Those results determine whether the remaining work is primarily implementation,
an architectural correction or an unresolved compression research problem.

## 18. Exact release gates

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
  Commit validated milestones locally and push meaningful checkpoints; do not use
  GitHub as the working folder or require ordinary hosted CI before local work.
- Use current source-bound semantic navigation and recent reputable primary
  research for consequential choices. Reuse a valid unchanged index/evidence;
  no duplicate large rebuild or unchanged GPU trial to fill a round.
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
