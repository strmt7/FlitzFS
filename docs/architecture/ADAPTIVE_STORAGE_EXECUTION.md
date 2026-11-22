# Adaptive compression and storage execution

This work extends the [target design](TARGET.md) and the
[release gates](../RELEASE_EXECUTION_PLAN.md). Optimize the complete protected
footprint first. Measure access time, resource use and storage writes as
constraints on each size improvement. The ordinary Windows read and write
paths, CPU recovery and existing user-visible file semantics carry across every
representation.

## 1. Price protection and selection before compressing more

The [sealed image and writable tail](../SEALED_INDEXED_IMAGE.md#a-sealed-base-with-a-writable-tail)
can now expose an ordinary indexed address space through the native volume
selector. The base folds checked identical mirror units and protects them
with parity. New writes keep the existing mirrored copy-on-write path. The
complete AIT corpus used 36.5% fewer active bytes, including the selector in
both layouts. The byte savings held through the same small write. Direct
folding stages one copy of each paired unit. It cut complete AIT staging
coexistence from 137,506,816 to 111,828,992 bytes, including the prior source.
[Folded staging verification](../evidence/folded-hybrid-workspace-2026-11-22.json)

The native session can now prepare the folded workspace, protected base
and writable tail incrementally. Each advance yields after its bounded
work, including individual ordered flushes. The full AIT fixture preserved
the staged footprint and exact CPU readback. Ownership and admission are
still explicit at the caller.
[Cooperative preparation](../SEALED_INDEXED_IMAGE.md#cooperative-preparation) ·
[Verification](../evidence/cooperative-hybrid-preparation-2026-11-22.json)

Extend discovery and storage selection in this order.

1. Use bounded staged preparation within the existing maintenance governor.
   Let live resource evidence fund the source, candidate, dependencies and
   workspace together. Measure foreground interference, read amplification
   and the actual footprint before enrolling an ordinary storage provider.
2. Bind base and tail placement to the indexed volume allocator and resolver.
   Verify the frozen generation and exact content at selection, and preserve
   the source on stale work, cancellation or unavailable backing. Compare
   the candidate's settled footprint with the current selected layout and
   charge the temporary and pinned bytes during the handoff.
3. Commit region references and reclamation ownership with the namespace
   publication. Replay every write/flush cut through the independent CPU
   reader. Verify that rollback and snapshots still reach their original
   content before reclaiming either physical extent.
4. Measure the footprint after real reclamation and the common small-read
   trace. Improve descriptor and range locality before enrolling sealed regions
   for hot reads. Keep the detached CPU restore path available for old
   supported representations.

## 2. Learn which collective work is worth doing

Use the existing sparse paged coherence index as a starting point. Learn at
chunk or fragment granularity. Keep entries keyed to content and generation,
with band postings, compact cluster representatives and a bounded set of
ranked neighbors. Sample common buckets with retained cursors so a large
family does not starve other candidates. Validate proposal recall against an
exhaustive match on small controlled data. Then measure postings visited,
bytes read and memory across geometric namespace sizes and a skewed, held-out
larger size. An all-pairs similarity matrix would require quadratic space and
update work. [FAST 2026 RubikFS](https://www.usenix.org/conference/fast26/presentation/huang)

Trial exact duplicate references, independent compression, shared dictionaries,
direct reference residuals and independently decodable short-file frames.
Feature-test shifted and content-defined fragments before replacing current
extent boundaries. A group can span thousands of files. Its read plan follows
a frame index and a bounded set of exact references, with limits chosen from
observed decode work and the resource grant. Use verified content identity and
direct readback as the authority for sharing.

Start with a small quantized decision tree or integer ranker trained to predict
**net protected bytes saved** and wasted trial work. Use already available
sample statistics, similarity, existing compression, access profile and current
resource measurements. Save model size, scores and the selection policy in
versioned advisory state. Bootstrap on AIT A–H and grouped revision families
from CPython. Reserve I–P and unrelated lineages for evaluation. Compare against
a frozen hand-written policy with equal trial budgets, including missed savings
and discovery effort. Corpus coverage beyond either split remains a separate
measurement.
[AIT 2026 dataset](https://aitdcc.github.io/dataset.html) ·
[Lossless codec selection research preprint](https://arxiv.org/abs/2509.25219)

Automatic indexed discovery now starts from the actual encoded payload bytes
of the current members. Four fixed-point estimates track the recent observed
yield of positional, shifted, masked and modular discovery. The controller
uses the observations when it next orders buckets. Prior representatives can
supply an existing reference. Each proposed replacement still has to pass the
independent CPU reconstruction and protected compacted-size checks. The
estimates live with the controller and start fresh on attachment.

Two alternating comparisons per workload on Windows produced identical
settled image sizes for all AIT 2026 splits and the paired CPython revisions.
For the revisions, the new ordering wrote 8,192 fewer bytes during
maintenance and ended at the same 819,200-byte compacted image. The AIT splits
made no group writes in either policy. Exact readback passed for both the
original and selected image in every trial.
[Collective candidate comparison](../evidence/collective-yield-ranking-2026-11-22.json)

Continue fitting and evaluating locally using admitted idle-work outcomes.
Introduce model changes through the same selection test. Persist only checked
advisory state that can be rebuilt from live data. Compare complete committed
bytes and independently verify the CPU output for every selected candidate. A learned model that changes the
stored coding is a separate experiment. Trial fixed-point context prediction
of transformed residuals with bounded restart frames and immutable, versioned
tables. Include model bytes, dependency protection, cold CPU decoding and
AMD scheduling in the price. A new mandatory decoder needs format
capability checks, historical fixtures and CPU rescue coverage.
[StateSMix learned lossless compression preprint](https://arxiv.org/abs/2605.02904) ·
[Tufa Labs measured model and payload costs on enwik9](https://tufalabs.ai/research/tufazip/)

## 3. Put AMD on ordinary reads and writes

FlitzZip's independent rANS segments, separately validated model and batch
descriptors, reusable workspace and stream-local cleanup inform the worker
implementation. FlitzFS defines its own restart framing, access granularity
and durable metadata contract. Bind the ROCm SDK distribution, actual HIP
runtime and module identities independently, along with the adapter and role.
[FlitzZip device decode workspace](https://github.com/strmt7/FlitzZip/blob/11396b5123505a4a1959173e1db2d0858c10dd6d/src/gpu/hip_rans.cpp#L79-L133) ·
[FlitzZip owned batch route](https://github.com/strmt7/FlitzZip/blob/11396b5123505a4a1959173e1db2d0858c10dd6d/src/gpu/hip_codec.cpp#L945-L972) ·
[AMD ROCm 10.2 release notes](https://rocm.docs.amd.com/en/docs-10.2.0/about/release-notes.html)

- **Foreground write.** Integrate the finite installed worker into ordinary
  write and flush paths for eligible windows. Match against fast CPU encoding,
  charging transfers, framing, CPU verification and durable commit. Run with
  competing I/O and with the GPU unavailable. The successful durable choice
  can use a different CPU and GPU encoding.
- **Cold size.** Run GPU candidate search, transforms or encoding in admitted
  quanta. Confirm the same logical bytes and better settled size than the
  current selected representation before publishing. Record when accelerated
  work increases the search yield but leaves the same chosen codec.
- **Foreground read.** Route frames already supported by the worker through the
  normal indexed range reader. Verify exact bounds, identity and a CPU-computed
  digest of reconstructed bytes before returning them. Initially compare all
  results against the independent CPU decoder. Qualify the digest-based
  read path separately so successful device work can eventually avoid duplicate
  CPU decoding. Track time to data and memory movement for isolated small reads
  and multi-frame requests.

The indexed session read path can now consume a ready native LZ4 result. It
keys that result to the verified image, object generation, decoded length
and decoded digest, and the CPU checks the complete result before file-range
projection. A missing or rejected result decodes the same checked record on
CPU. The decoder hook runs on the existing serialized read path and returns
without waiting. The mounted AMD producer and its matched measurements are
still open. The pinned LZ4 Release and AddressSanitizer runs each checked
one accepted ready result and three rejected replies, with exact CPU
fallback on the same stored records.
[Windows verification](../evidence/pinned-provider-indexed-lz4-read-2026-11-22.json)

The private [native AMD LZ4 decoder](../NATIVE_AMD_LZ4_READ.md) now passes
byte-exact synthetic and public-data reads on the development adapter. The
older GPU decoder supports different block kinds and a 65,536-byte block limit. A full indexed extent includes a 24-byte header
in addition to its 65,536 data bytes. Give native LZ4 decode its own
resource-sized batch contract. Validate total lengths, sequence boundaries,
copy distances, final literals and alignment before input reaches the device.
Qualify both wave widths where available, with the same stored frames decoded
through the independent CPU library. Measure the complete read path before
automatic admission. The isolated child transport now returns two real AMD
frames through indexed session reads, and the SDK callback can consume an exact
ready result once. The independent CPU path served reads while the child was
pending and after a module refusal. The next step is to qualify a prepared
producer and anticipatory reads. Measure complete lifetime cost and short-read
hit rate before changing the ordinary mount's default route.
[Isolated-worker verification](../evidence/native-amd-lz4-ipc-2026-11-22.json)
[Pinned AMD LZ4 decoder](https://github.com/ROCm/hipCOMP-core/blob/03df9f7369211ee15d370817165224c73fdc9a46/src/LZ4Kernels.cuh#L921) ·
[LZ4 block format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md)

The prepared native LZ4 worker now separates setup from submission and
exact retirement. An indexed file handle can supply an owned encoded frame
without decompressing its payload. On Radeon RX 9070 XT, the two-frame
prepared path took 34–45 ms from submission through verification and
retirement. Paired CPU reads took 117–297 µs. Use these measurements to keep
small demand reads on CPU and price the full cost of a larger speculative
window before scheduling it automatically.
[Prepared native read qualification](../NATIVE_AMD_LZ4_READ.md#prepared-native-reads)

The indexed ready path can now bind a captured frame to the stored digest
in the current paged index. A hit skips fetching its compressed record and
checks the decoded bytes before delivery. The public AIT A two-frame fixture
reduced backing reads on demand from 144,341 to 18,168 bytes with an already
verified AMD result. The scheduler still needs to account for capture,
preparation, execution and the chance that the data goes unused.
[Indexed ready read qualification](../NATIVE_AMD_LZ4_READ.md#reusing-a-verified-ready-frame)

An opt-in controller baseline now derives a one-extent forecast from
ordinary sequential reads. It admits the forecast only after other
maintenance yields and the observed backing costs fit the idle window.
The CPU baseline measures preparation and consumption separately. Wider
batches and automatic AMD enrollment depend on full-path results and
bounded foreground interference.
[Sequential read-ahead qualification](../NATIVE_AMD_LZ4_READ.md#qualifying-sequential-read-ahead)

Use [GPU recovery](../GPU_RECOVERY.md) for immediate CPU continuity and
evidence-driven retry admission. A declined, failed or late reply never
authorizes stored data or changes the selected version. Bind measurements to
running adapter and workload roles. Enroll a path after matched live runs prove
useful compression or read work through the same production operation.

## 4. Backing provider qualification

Collect provider identity, lifetime, geometry and access capabilities before
admitting work. Windows exposes logical sector size, physical atomicity and
preferred alignment separately. Keep transport, seek penalty, queueing,
TRIM, write-cache behavior and removal as separate advisory values. A USB
bridge can report incomplete characteristics and a SATA bus can carry
either solid-state or rotating media. Sparse range reclamation depends on
the outer filesystem's supported operations. Admission to write still
depends on the provider's qualified recovery contract. Preserve unknown
fields and use measured I/O on the selected provider to tune maintenance.
[Windows storage property IDs](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ne-winioctl-storage_property_id) ·
[Windows file storage geometry](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_storage_info) ·
[Windows write-cache queries](https://learn.microsoft.com/en-us/windows-hardware/drivers/storage/querying-for-the-write-cache-property) ·
[Windows sparse file operations](https://learn.microsoft.com/en-us/windows/win32/fileio/sparse-file-operations)

The Windows file provider now admits qualified local fixed and removable
volumes. It requires the opened parent and backing file to resolve to the
same volume GUID. Free-space checks and allocation geometry use that GUID
so a changed drive letter cannot supply evidence for the old volume. The
space reserve is one sixteenth of caller-visible capacity, rounded up and
capped at 1 GiB. Existing geometry and explicit flush requirements still
govern writes. Sparse range reclamation uses the qualified NTFS path. The
read-only Windows probe confirmed the GUID and DOS path for the development
drive root and its test directory. The next probe obtained storage properties
through a metadata-only handle to that same volume. It observed SATA transport,
a seek penalty, and TRIM disabled. The ordinary directory handle refused the
three storage property queries. The probe read no backing bytes and made no
write attempts. Removable media and physical persistence still require device
tests.
[Read-only storage probe](../evidence/windows-backing-media-2026-11-22.json)
[Windows final paths by handle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew) ·
[Caller-visible free space](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespaceexw) ·
[Windows drive types](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdrivetypew)

| Provider | Signals and tailored decisions | Qualification |
| --- | --- | --- |
| RAM backing | Identify the provider when creating and reopening. Resize work against physical and commit headroom. Use memory bandwidth observations to limit copies and caches. Contents follow the backing's lifetime. | Pressure, cancellation, initiated detach and exact readback while the backing survives. |
| NVMe | Measure single-request and queued latency, sustained throughput and flush cost. Use queue capacity only when it improves the foreground path. Coalesce low-priority writes and yield when request tails rise. | Check steady-state allocation, supported reclaim and recovery at commit barriers on dedicated devices. |
| SATA SSD | Learn queue and flush bounds for the actual device. Limit write bursts during sustained slowdown. Charge attempted and completed backing writes, including reclamation overhead. | Test sustained mixed I/O, a nearly full volume, repeated rewrites and interruption under the qualified cache contract. |
| HDD | Use reported seek penalty and random versus sequential observations. Group nearby reads and writes without changing publication order. Favor sequential passes in idle windows. | Compare metadata and small-file access with sequential media work. Verify both retained and newly selected regions after interruption. |
| USB storage and memory cards | Keep removal, transport and seek behavior independent. Shorten writes and in-flight work when measured stalls warrant it. Stop on removal and verify fresh identity and committed roots before reopening. | Use disposable media to qualify removal and reinsertion at commit boundaries with the active Windows removal policy. Verify integrity and space recovery for each supported outer filesystem. |
| Unclassified backing | Start from the qualified provider contract and conservative concurrency. Change one dimension at a time when ordinary I/O supplies evidence. | Bind supported modes and performance statements to the actual provider and workload. |

An externally created RAM disk needs its own volatile lifetime contract. A
backing file and successful flush can establish the file provider's barriers
within that lifetime. Reopening after power loss requires persistent storage.
Windows removable write-caching behavior follows the active removal policy.
FlitzFS should observe behavior through the attached provider and keep its
own commit barriers and detach sequence on every device.
[Microsoft Windows removal policies](https://learn.microsoft.com/en-us/windows/client-management/client-tools/change-default-removal-policy-external-storage-media) ·
[Microsoft NVMe storage queries](https://learn.microsoft.com/en-us/windows/win32/fileio/working-with-nvme-devices) ·
[Microsoft file flush contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)

The mounted controller now measures reads, writes, barriers and selected
control operations on the backing during each maintenance step. The memory
provider and the qualified Windows file provider supply observations from
their actual calls. Each capture belongs to the current maintenance thread.
The counters distinguish bytes requested from exact calls that completed.
These are logical provider operations. Device-level write amplification
requires independent measurements.

The controller keeps a separate recent backing-cost estimate for group
preparation, compaction and payload scrubbing. It uses the estimate along
with the last step's elapsed time to decide whether another step fits the
current idle window. New observations gradually lower a slow estimate.
A fresh window can still admit one step under the existing live resource
and foreground checks. The controller charges any overrun to its existing
elapsed-work pacing. This changes admission between synchronous steps.
[Backing-aware maintenance verification](../evidence/backing-maintenance-admission-2026-11-22.json) ·
[Windows background resource scheduling](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority) ·
[Windows monotonic timing](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps)

Continue the backing-dependent work in this order.

1. Bind advisory properties to the opened volume identity. Probe only the
   selected backing in managed setup, with finite ownership and deadlines.
   Use explicit provider identity for process memory. Keep file-system-level
   range reclamation, device TRIM and qualified flush behavior independent.
   Reacquire advisory properties on a fresh attach. Label missing or
   ambiguous evidence as unknown.
2. Qualify the measured admission policy with real device latency and a
   separate foreground trace. Select the next bounded quantum from observed
   providers, current resource grants and contention. Keep the existing
   foreground yield checks. Cluster safe reads on seek-sensitive media and
   coalesce qualified sequential writes when the recovery contract allows it.
3. Charge backing write attempts, retained generations and temporary space to
   each optimization. Keep recovered space and estimated device wear distinct
   from application bytes. Size reduction remains the goal. Repeatedly rewriting
   marginal candidates consumes the budget without a corresponding gain.
   Give integrity repair precedence and resume optional work from checked cursors.
4. Qualify dense volume files separately from NTFS sparse files. The current
   Windows helper asks for sparse range reclamation. A removable volume without
   that capability needs an independently qualified dense path with safe reuse
   or compaction before routine maintenance can reclaim its bytes. Keep
   the same versioned CPU reader and verify detach, removal and reinsertion
   before resuming work on a persistent provider.
5. Qualify each persistent provider's barriers, interrupted writes and removal
   on disposable media. A volatile provider preserves files while its owner and
   backing survive. Measure matched foreground and background workload traces on
   real devices when authorized. Keep reported transport, outer filesystem,
   allocation, recovery scope and observed performance in each result.

## 5. Finish the autonomous Windows lifecycle

Bind installer, WinFsp integration, tray/helper, optional AMD worker and CPU
rescue tool to verified package versions. Have the helper drive discovery,
attach/detach, integrity checks, progress, repair, compaction and gradual
migration. Distinguish a user's initial volume/backing selection from routine
maintenance, which should run automatically. Persist only checked policy and
work state. Stop owned mount and worker resources on removal or shutdown.
Verify install, update, rollback and clean removal on the exact candidate.
Use a CPU reader with every supported mandatory format revision and keep older
representations readable while migration proceeds.

Use the [benchmark protocol](../BENCHMARK_PROTOCOL.md) for paired mounted
FlitzFS, plain NTFS and native-compressed NTFS. Measure complete allocated
bytes, foreground tails, readback, CPU/GPU usage and background/storage debit
for each media class. Extrapolate scaling only from measured ranges with an
independent validation point. Retain confidence intervals, losses and setup
details in the evidence. A stable release decision requires the integrated
results and remains with the owner.
