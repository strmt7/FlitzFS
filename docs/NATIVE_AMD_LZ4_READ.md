# Native AMD LZ4 reads

The optional AMD compute path decodes standard LZ4 records from indexed
FlitzFS writes. It uses the existing record format and CPU decoder. The
device result can flow through `NativeFileSession::read` after the indexed
store verifies the full frame digest. The decoder runs in an isolated Windows worker. The WinFsp volume controller
can now own the worker and deliver its verified result through ordinary
callbacks when supplied an independently qualified read profile. Automatic
installed-package enrollment and mounted performance remain open.

## Input, device execution and recovery

The store's read route provides an encoded frame and its verified image,
object generation, decoded length and BLAKE3 digest. The batch copies each
encoded frame into an aligned private slab with a separate decoded slot.
Before proportional allocation it checks the frame count and slab grant.
A bounded sequence parser checks literal and match lengths, offsets, the
final literal sequence and exact decoded size. Admission of a device record
also requires an available independent CPU LZ4 decoder.

The kernel runs one cooperating wave per frame. A lane checks framing on
device before the wave enters the pinned AMD LZ4 routine. The layout places
the descriptor, input and output banks in disjoint bounds. Each output slot
has a checked tail guard. The same implementation compiled for both
32- and 64-lane targets. The reference decoder stays the pinned CPU LZ4
library.
[Pinned AMD LZ4 implementation](https://github.com/ROCm/hipCOMP-core/blob/03df9f7369211ee15d370817165224c73fdc9a46/src/LZ4Kernels.cuh#L921) ·
[LZ4 block format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md)

The compute path reuses one stream and device allocation across a finite set
of batches, charging all prepared bytes and frames against the job grant.
It checks resource evidence before initialization, during the sequence and
at completion. It downloads the completion descriptors and decoded bank.
The CPU checks status, exact length, digest and output guards for every frame
before accepting the sequence. The versioned worker exchange binds a reply to its request ID, the BLAKE3
digest of the immutable frame and the ordered record keys. The parent checks
length and decoded BLAKE3 for every frame after both the child and supervisor
retire. The ready window matches exact image, object, generation, decoded
length and digest, then moves the bytes out once. The indexed reader can use
its independent CPU decoder while the worker is pending, on a miss or after
refusal. The reader still checks the indexed record before delivery.

## Source-bound qualification

The optional SDK sub-build consumes the private
`FLITZFS_ROCM_NATIVE_LZ4_READ_OBJECT` and the already pinned CPU LZ4 source.
The source is `src/gpu/native_lz4_decode.hip`. Use
`tools/dev/stage_native_lz4_decode.py --source <pinned-checkout> --output <new-directory>`
to copy the recorded AMD code and apply the exact native-sync transformation.
Use the staged entry and its source directory with the reviewed SDK compiler.
The typed exported table binds the launch layout and both wave entry points.
The verifier requires the exact device module digest, signed installed runtime
digest, current GPU identity and a bounded one-process worker job. Keep any initial
hardware qualification to an owned fixture and one admitted sequence.

The native `bounded_native_lz4_decode` test exercises an overlapping copy,
truncation, bad distances, incorrect output lengths, a short literal tail,
grant exhaustion and corrupted completion metadata, data and guards.
Positive result checks ran with the pinned CPU LZ4 build and AddressSanitizer.
The raw-only control verified framing and refusal to start without an
independent CPU decoder.

The live Radeon RX 9070 XT ran in wave32 with the signed installed runtime.
The probe wrote synthetic bytes and a 131,035-byte prefix of public AIT file A
through the native indexed session. Each workload produced two standard LZ4
records, with decoded lengths of 65,560 and 65,523 bytes. Both workloads
consumed actual GPU results in the indexed file reader, then passed exact
CPU fallback and independent verification. The `gfx90a` target was compiled
offline.

| Workload | Device slab with both frames in one batch | Reused slab across two dispatches |
| --- | ---: | ---: |
| Synthetic | 148,192 bytes | 74,096 bytes |
| AIT A prefix | 261,760 bytes | 130,880 bytes |

The cold reusable probe spent about 0.55 seconds loading modules, initializing
the runtime, decoding and retiring resources for each workload. The paired
CPU session reads took about 0.14 ms for synthetic data and 0.19 ms for the
AIT prefix. Keep these component timings scoped to the recorded process-local
trials. Automatic admission needs measurements of a prepared producer under
ordinary mounted reads and writes.
[In-process verification](evidence/native-amd-lz4-read-2026-11-22.json) ·
[AIT 2026 dataset](https://aitdcc.github.io/dataset.html)

The subsequent isolated-worker trials ran two GPU dispatches for each of the
same workloads and delivered both decoded frames through the indexed session.
CPU reads returned exact bytes while the worker was pending and after the
ready frames were consumed. A deliberately incorrect worker module digest
refused before HIP loading. The child retired and both reads stayed exact on
CPU. A separate SDK callback fixture used CPU-generated completions to check
exact ready delivery, fallback on a miss and refusal of a ready result from
before a write. The callback fixture also passed under AddressSanitizer.

| Workload | Cold exchange through retirement | Paired CPU reads |
| --- | ---: | ---: |
| Synthetic | 2.440 s | 0.152 ms |
| AIT A prefix | 2.018 s | 0.248 ms |

These single observations include concurrent build activity. The exchange
covers child setup, device work, transport, verification and retirement.
[Isolated-worker verification](evidence/native-amd-lz4-ipc-2026-11-22.json)

## Shared-base extent read-ahead

Adjacent reads that encounter packed XOR, sparse XOR, or modular delta can
forecast a grouped read window.
The capture opens one healthy mirrored checkpoint and verifies each selected
member, its backreference and the primary record of its base. It retains each
distinct base once and builds each member's tile offsets from the checked
body. The device request carries one copy of every selected base, and each
AMD lane reconstructs eight neighboring bytes. Sparse planes use sorted
exception positions, including inverted planes. Modular delta lanes handle
1-, 2-, 4-, and 8-byte words and can apply a constant bias with no residual body.
The compressed bodies cross the device boundary in their existing formats.

The Windows controller can choose CPU preparation or send the batch through
the existing bounded AMD helper. The installed package's `packed_groups`
profile requires a pinned module with the group dispatch entry. Packed-only
requests retain the original protocol. A request containing sparse XOR or
modular delta uses version 2 and requires the separate `mixed_groups` profile
and version 2 entry. Earlier profiles continue to serve their eligible
requests. The worker
checks the request, adapter and live memory again, then completes the command
and retires before the controller can use the reply.

With a qualified `prepared_group_submit_lead_ms`, the controller can start
one prepared helper after it pins the forecast's checkpoint and releases
the callback lock. The helper initializes HIP and reserves a slab while the
storage reader captures the grouped records. Only the exact forecast can
queue a request, and its bytes and extent count must fit the reservation.
The helper receives the private request after initialization and releases
its resources on completion, cancellation, or an unused forecast. Capture
and submission still yield when foreground activity changes.

Once submitted, the request belongs to one open context, its sequential
read run and the exact image checkpoint. An unrelated read or metadata query
can finish while the worker owns the request. The CPU also serves early
target demand immediately. If that read advances along the same stream
and leaves part of the grouped capture unread, the worker may finish
within its original deadline. Verification checks every reply extent
against its committed digest after confirmed child exit. Publication then
discards the fully served prefix. A seek, full consumption, context
retirement or changed checkpoint supersedes the capture. Demand checks
the protected record bindings again before using a ready extent.
[Interleaved callback evidence](evidence/windows-group-read-stream-binding-2026-11-25.json)

An admitted physical Radeon trial exercised a four-extent prepared mixed
capture. The first callback read one extent on CPU while the request was
pending. After the child retired and all four replies were verified, a
single callback used the three unread device extents. Independent CPU
snapshot readback verified all file bytes. The fixture did no backing writes.
[CPU prefix and remaining grouped extents](evidence/windows-group-read-cpu-prefix-2026-11-25.json)

A qualified prepared worker can now capture a window covering several
callbacks. The controller uses the bytes already read in consecutive grouped
requests from the same handle. The qualified worker geometry, preparation
grant, available host memory and observed time before the next request
bound the capture. A seek starts a new run. Requests that only use
independent records reset the grouped history. If a wider capture declines
device submission, the controller drops the optional work and demand reads
decode on CPU.

When learned worker startup exceeds the next request's lead, the controller
can choose a later range within the demonstrated consecutive grouped
demand. This requires three aligned grouped requests of the same size.
The shorter of the two observed gaps estimates the cadence. The plan moves
in whole requests and reserves at least one complete later request within
the device and host grants. The intervening requests read normally on CPU.
The existing capture and submission checks still bind one immutable worker
exchange to the open handle, checkpoint and selected range.
[Learned startup and later range evidence](evidence/windows-amd-later-group-window-2026-11-25.json) ·
[Microsoft sequential access guidance](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/ns-fileapi-createfile2_extended_parameters)

Adjacent reads from the same grouped stream can supply timing observations
at different request widths. The estimate uses the shorter of two observed
gaps from grouped reads. Ordinary same-width observations remain available
independently. A seek breaks adjacency. The existing resource grant, verified
input, stream revision and checkpoint still govern preparation and delivery.
In the matched SDK fixture, demand for one, three and two extents retained
47 ms of measured lead in the codec-enabled run and qualified a four-extent
forecast. The CPU verified every byte through subsequent three-extent and
one-extent callbacks. The first device attempt deferred at the live GPU
load check. After the device became idle, the installed RX 9070 XT decoded
four mixed frames from the same request pattern. The SDK fixture waited
for the worker, then delivered all four across the three-extent and
one-extent requests. Independent CPU readback matched, late results
were refused, and the worker retired. The worker took 1,203 ms. The
matched ordinary-client service comparison remains open.
[Mixed-width demand and validation](evidence/windows-amd-variable-group-demand-2026-11-25.json) ·
[Installed Radeon mixed-width run](evidence/windows-amd-mixed-width-live-2026-11-25.json)

The live Radeon SDK fixture used one prepared exchange for four mixed
grouped extents and delivered them across two reads. It retained the
result through an unrelated callback, refused a late result, verified
the data independently on CPU and confirmed worker exit. The matched
automatic mounted service comparison remains open.
[Expanded group capture and readback](evidence/windows-expanded-group-read-and-mounted-replay-2026-11-25.json) ·
[AMD performance guidance](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/how-to/performance_guidelines.html)

The qualified startup lead, measured unready exchanges and verified ready
dispatch cost govern preparation.
The controller probes the same helper's readiness without blocking, before
encoding a request and again before queuing it. An unready worker needs
enough lead for its estimated remaining setup and dispatch. An overdue
estimate still requires full lead until the event confirms readiness. Once
ready, the worker needs the qualified dispatch lead or a longer measured
ready-to-publication time. Only a verified result published from a request
submitted to a ready helper updates ready dispatch. Time spent waiting for
a request after readiness gives no startup sample. Early submissions and
cold launches inform the full exchange estimate. A startup offer also
covers the verified dispatch floor.
[Readiness and fallback evidence](evidence/windows-amd-group-read-admission-2026-11-24.json) ·
[Windows zero-time wait](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)

The packed and mixed tables can name the same device function. The worker
validates both tables and corroborates attributes for each distinct decode
function on the admitted device. The current shared decoder needs two
queries. A module with different device functions needs two per function.
[HIP function metadata API](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/doxygen/html/group___execution.html) ·
[Startup admission evidence](evidence/windows-amd-group-startup-admission-2026-11-25.json)

AMD documents stream priority as disabled, so this route relies on
admission, finite reservations, and cancellation to limit competing work.
[AMD HIP stream management](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/doxygen/html/group___stream.html)

The parent verifies every complete extent against its independently captured
decoded digest, including bytes outside the requested slice. On demand,
FlitzFS checks the exact backing checkpoint and both protected record
bindings before allowing a one-use hit. An unavailable, superseded or
invalid result leaves the normal CPU reader in control. The stored format
uses the existing representations. CPU readers continue to interpret the
same committed capabilities and data.

The RX 9070 XT returned verified packed and mixed group frames in an
admitted Windows SDK callback fixture. Two runs of the earlier prepared
mixed helper refused before initialization with stale budget observations.
After moving the snapshot start to the first DXGI memory query, two prepared
mixed runs and a packed regression run delivered two frames each. All
successful runs passed independent CPU readback, rejected late publication
and confirmed worker retirement. The corrected prepared exchanges took
3.8–5.9 seconds after submission in this fixture.
[Live worker and callback receipt](evidence/windows-amd-group-budget-freshness-2026-11-24.json)

On the later current-source run, the prepared mixed decoder returned two
verified Radeon frames after an unrelated callback. Both ready results
matched independent CPU reads. Late demand refused the ready result, and
the worker retired. This SDK result establishes that delivery path on the
installed device. A matched ordinary-client speed advantage remains open.
[Current live Radeon and CPU evidence](evidence/windows-ordinary-reclamation-progress-2026-11-25.json)

Private live grouped traces attributed **4,790–5,311 ms** to the worker's
HIP kernel-attribute checks. Dispatch and wait accounted for **0.103–0.179 ms**.
Each trace returned two verified frames and passed independent CPU readback.
The callback fixture's hard CPU rate was nested under the supervisor's rate,
with the normal per-worker limit still applied. The separate plain LZ4
control used the same runtime and module, with a different workload and job
hierarchy. Its 860–881 ms exchanges identify a timing discrepancy to isolate.
[Live phase evidence](evidence/windows-amd-group-exchange-phases-2026-11-24.json) ·
[Microsoft nested Job Object CPU rates](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information)

For a grouped live timing comparison, add `--timed-exchange` after the
current fixture arguments. It admits an existing supervisor only after
verifying its hard CPU rate of at most 20% and its process, priority and
memory controls. The fixture keeps its own commit, process-count and CPU-time
limits. The worker keeps its existing per-process admission. The CPU-side
check accepted a verified 18% supervisor and rejected a mismatched helper
before dispatch. After earlier GPU pressure cleared, alternating matched
mixed-group runs took 969 and 985 ms under the verified supervisor. The
same artifacts and protected input under the extra 20% fixture rate took
4,984 and 5,391 ms. The HIP kernel attribute check took about 805–823 ms
and 4,740–4,757 ms respectively. Every run consumed two exact frames.
The current prepared packed and mixed callback paths also consumed two
exact frames each with the ordinary worker.
[Matched supervisor and live readback evidence](evidence/windows-amd-group-supervisor-comparison-2026-11-24.json)

Compare CPU, cold AMD and prepared AMD at observed foreground lead times
under matched supervision. Measure
capture, upload, decode, readback, verification, retirement and late work.
Prioritize amortizing startup if it dominates with the matched controls.
Record the interval before submission as well as the interval from
submission to completion. Microsoft's storage timing guidance uses this
distinction to identify queue and execution costs.
[Microsoft storage timing guidance](https://learn.microsoft.com/en-us/windows/pix/timing-captures/directstorage)

Use the measured costs to compare finite prepared workers with cold
dispatch, including contention and release of idle resources. Anchor copies,
aligned patches, prefixed Zstandard, and packed slices remain on the CPU.

PlaneZip's results on model activations motivate retaining per-tile bit-plane
encodings across the link. FlitzFS needs its own complete-cost measurements
for ordinary reads. [PlaneZip](https://arxiv.org/abs/2610.25033)

[Packed read-ahead evidence](evidence/windows-prepared-group-reads-2026-11-24.json) ·
[Mixed decoder evidence](evidence/windows-mixed-group-reads-2026-11-24.json) ·
[Prepared grouped-read evidence](evidence/windows-prepared-group-worker-2026-11-24.json)

The CPU comparison for small adjacent grouped requests now retains one
verified partial tile at the selected checkpoint. Reuse contributes stream
coverage to the forecast. The CPU-cost estimator samples actual decode
work. The current local callback comparison read the monitored member once
across the 4 KiB requests, down from 16 times. Use this CPU baseline
when determining which ordinary read shapes benefit from preparation.
[Matched CPU evidence](evidence/windows-cpu-tile-and-ordinary-settlement-2026-11-25.json)

### Measuring bytes delivered by grouped read-ahead

The normal Windows controller view reports published, delivered and pending
logical bytes for verified CPU and AMD grouped read-ahead. It credits bytes
only when the indexed reader rechecks the committed member and base and
returns the requested slice. An unread frame, a CPU reread and a reused CPU
demand tile contribute no prepared delivery. A 4 KiB callback delivers 4 KiB
even when its group member holds a full 64 KiB frame. The session preserves
the final totals after joining the dispatcher and releasing readiness.

A published group can retain part of an extent whose prefix was already
served by CPU. Unused logical bytes are the published total less delivered
and currently retained bytes. This allows the same trace to identify work
discarded due to a seek, checkpoint change, close or replacement. These
totals measure the WinFsp callback boundary. A mounted comparison also
records the successful bytes and status observed by the Win32 client.
[WinFsp read interface](https://github.com/winfsp/winfsp/blob/ada6de157df4fa99f5a866b6ac7af66918d6d838/inc/winfsp/winfsp.h) ·
[Windows ReadFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile)

The matched segmented fixture counted 131,072 delivered bytes in 32 calls
in both the CPU and host Radeon runs. A later CPU pass left those totals
unchanged. The Radeon child retired before its output entered the read path.
Use the delivered bytes and retained or discarded work together with setup,
transfer, kernel, verification and demand timings when evaluating a warmed
window. Keep software admission independent of HIP stream-priority support.
[Matched Windows qualification](evidence/windows-grouped-read-delivery-2026-11-25.json) ·
[AMD ROCm Core 10.2.0 release notes](https://rocm.docs.amd.com/en/docs-10.2.0/about/release-notes.html)

### Feedback when demand passes a grouped request

A sequential handle can finish the captured range before the device
exchange delivers it. The checked stream revision and unchanged
checkpoint identify this outcome. A seek, close or checkpoint change
still invalidates the request independently. Each terminal outcome
keeps the CPU-served bytes and cancels work owned by that request.

When the supervisor's completion handle is still unsignaled at a
confirmed demand miss, the controller records a lower bound on
exchange time. Ready submissions measure from submission. Unready
submissions measure from startup. The worker's existing lifetime budget
bounds the observation. A completed verified exchange can refresh the
same phase. A signaled supervisor counts as a delivery miss and leaves
the phase estimate intact. A confirmed worker failure still informs recovery
after foreground demand advances. This lets a later read forecast seek a
farther eligible group without learning false delay from invalidated work.
[Bounded Windows checks](evidence/windows-grouped-demand-expiry-2026-11-25.json) ·
[Windows wait semantics](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)

### Deliver completed reads before changing their checkpoint

The controller verifies a retired AMD reply before it starts write
calibration or advances storage maintenance in the current grant. A
successful publication uses that quantum. The next quantum can resume
maintenance with the same debt and ownership state. If the worker
finishes during a maintenance step, the controller can still verify the
reply at the end of that step. Both checks use the existing host grant
and exact stream and checkpoint requirements.
[Controller scheduling and qualification](evidence/windows-ready-read-delivery-priority-2026-11-25.json)

### Later ranges for a cold grouped worker

A qualified packed or mixed group profile can propose a later read
window when its cold lead estimate exceeds the immediate request gap.
The profile and observed contiguous reads bound the range. The worker
checks the same lead again before dispatch. Prepared setup is a
separate capability, with its own qualification.

In the live small-read fixture, the cold Radeon worker decoded two
group members for a later range. The intervening extent stayed readable
on CPU. The callback consumed both members across 32 adjacent 4 KiB
reads and the CPU audit verified the retained file. The independent
controller fixture also consumed a device-decoded frame during a group
duty wait.
[Live device and CPU verification](evidence/windows-indexed-file-info-reuse-2026-11-25.json)

## Prepared native reads

The indexed session can capture one owned encoded LZ4 record for a readable
file handle and an offset. It verifies the selected namespace, object bounds
and stored digest without decoding the payload. The caller sets the encoded
byte grant. Holes, unsupported records, insufficient grants and damaged
primary records skip capture. Ordinary reads still handle mirror recovery.
The ready key binds the image, object, generation, decoded length and digest.
A foreground write can invalidate a capture immediately through that key.

The prepared worker uses the existing one-request sized exchange. The child
verifies the runtime, adapter, memory grant and device module, then creates
its stream and reserves the admitted slab before signalling readiness. It
accepts a single exact-size request for the prepared channel. Each dispatch
still checks live memory and the entire completion. The parent waits for
both the child and supervisor to retire before accepting decoded bytes.
An unused ready worker can discard the lease and release its resources
without dispatching. CPU demand reads proceed while setup is pending.

### Windows host measurements

Two alternating cold and prepared pairs ran for synthetic extents and the
131,035-byte prefix of AIT file A on a Radeon RX 9070 XT. Each run captured
two records and decoded them in two dispatches. These are rounded
process-local timings from the instrumented memory-backed fixture.

| Workload | Cold exchange | Prepared startup | After prepared submission | Frame capture | Paired CPU reads |
| --- | ---: | ---: | ---: | ---: | ---: |
| Synthetic | 657–721 ms | 613–655 ms | 34–36 ms | 36–40 µs | 117–136 µs |
| AIT A prefix | 654–854 ms | 654–791 ms | 37–45 ms | 70–75 µs | 163–297 µs |

Startup includes transport and preparation through the observed ready
signal. After-submission time includes the request, GPU work, verification
and exact retirement. Every successful trial delivered both byte-exact decoded
frames through the native indexed reader. The prepared route reserved 256 KiB
of device slab space for this fixture. A discarded ready worker published
nothing, and a wrong module digest refused before HIP work. Both cases
retained exact CPU readback.

The measured two-frame reads favor CPU. Automatic admission needs the
completion rate before demand, total startup and resident resource costs,
and paired end-to-end timings for larger sequential windows. Batching
transfers and retaining allocations across admitted work can amortize fixed
costs. Evaluate those changes on the same production read path.
[Prepared native read verification](evidence/prepared-amd-lz4-reads-2026-11-22.json) ·
[AMD HIP performance guidance](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/performance_guidelines.html) ·
[AMD asynchronous execution](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/asynchronous.html)

## Reusing a verified ready frame

An adopted WinFsp file context can supply one captured record when the
controller can obtain the callback lock and foreground work has yielded.
The capture keeps the digest of the stored record checked by the paged
index. The parent retains that digest while the worker runs and verifies
the decoded result before publishing it to the reader.

The file read checks the current index and matches the image, object,
generation, length and stored digest. It rehashes the decoded bytes before
returning them and consumes each ready frame once. A normal index lookup
still runs. The matching frame avoids fetching the compressed record again.
A changed record or known backing damage sends the read through normal
record verification and CPU mirror recovery where needed. Reports from an
advisory route without capture provenance still require the stored record check. A write to an
unrelated extent can preserve a matching ready frame.

### Ready read measurements

The indexed session consumed both frames in all eight Radeon RX 9070 XT
trials with exact bytes. The table isolates demand reads after capture,
GPU work, reply verification and worker retirement had completed.

| Workload | Ordinary backing bytes | Ready backing bytes | Ordinary demand time | Ready demand time |
| --- | ---: | ---: | ---: | ---: |
| Synthetic pair | 35,393 | 18,168 | 114–140 µs | 87–90 µs |
| AIT A prefix | 144,341 | 18,168 | 159–207 µs | 91–103 µs |

The SDK callback fixture consumed a CPU-generated ready frame using 9,239
backing bytes, compared with 17,839 bytes for its ordinary read. It also
checked provisional and closed handles and rejected a ready frame from
before a write. The native fixture verified a miss when two independently
created images reused the same identity and object generation with different
stored bytes. A protected copy remained readable on CPU after the fixture
damaged the primary and observed the failure.

These are bytes requested from a memory-backed provider and time spent in
the paired native reads. Windows file providers can also serve reads from
the system file cache. The prepared two-frame GPU exchange still took
34–37 ms after submission, including verification and exact retirement.
Automatic AMD scheduling needs lead-time and full cost measurements for
larger read-ahead windows.
[Ready read verification](evidence/indexed-ready-amd-reads-2026-11-22.json) ·
[Windows file caching](https://learn.microsoft.com/en-us/windows/win32/fileio/file-caching)

## Qualifying sequential read-ahead

The Windows volume controller can opt into a CPU baseline that follows
adjacent successful reads on an adopted handle. The callback derives an
extent-aligned window from the latest request and records the hint without
backing I/O. The idle owner funds a prefix of that window with the live
memory allowance. It pins one selected namespace and paged checkpoint
under the callback lock, then captures and verifies the frames outside
that lock. Sequential reads on the same handle can consume the prepared
frames across more than one callback. Each hit still matches the current
paged index and passes CPU verification.

The controller offers this work after ordinary maintenance yields. It
uses the remaining idle time, current CPU and memory observations, and
recent measured backing I/O duration. A forecast that exceeds the
remaining time is dropped. A new foreground operation interrupts the
capture or a CPU preparation pass. Misses, interrupted preparation and
stale results use the ordinary CPU read. Preparation has no stored output.

The pinned LZ4 callback fixture read the predicted extent with 9,284
backing bytes on demand. The ordinary read used 17,887 bytes. Capture
added 17,887 bytes, so the prepared route used 27,171 bytes in total.
An owned controller fixture served two prepared reads byte-for-byte.
A measured injected stall blocked the next prediction before backing
access, and the demand read still returned the exact bytes on CPU.
A separate AMD worker run verified two indexed frames on the Radeon
RX 9070 XT and retired its child process. These results exercise
different parts of the read path.
[Windows read-ahead verification](evidence/sequential-read-ahead-2026-11-22.json)

The cost estimate is local to the attached backing. When backing cost
blocks a forecast, the controller can learn from an ordinary read within
that extent on the same handle, or a read covering a refused multi-extent
window. The sample includes checked backing access and CPU decoding.
It requires a successful native LZ4 demand
without a pending ready result. Other ranges, failed reads, and ready
results supply no sample. The controller consumes each measurement once.
A cheaper sample lowers the recent estimate gradually, and a slow
sample holds it high. Each new prediction still needs the existing
foreground, backing, CPU and memory admission.

The live Windows fixture served three prepared reads byte-for-byte.
After the injected backing stall, a slow matching CPU read kept the
next prediction deferred. Faster demand eventually restored admission.
The controller made no backing calls to collect evidence while
speculation was refused. It also stayed quiet through an idle sampling
interval with no demand.
[Read-ahead recovery verification](evidence/read-ahead-demand-recovery-2026-11-22.json) ·
[Windows interval measurement](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter)

The capture owns its query cache and decoder. It carries a strong reference
to the actual image until the encoded frame is owned by the caller. The
backing must support concurrent extent and exact-read operations. The
Windows memory backing serializes byte copies and decommits only after
other owners release the image. The Windows volume-file provider uses
positioned reads with a separate completion event for capture. The same
backing pin protects the selected range until an in-flight request retires.
The opt-in route still needs full-path measurements on persistent devices.
[Positioned volume-file I/O](PERSISTENT_VOLUME_FILE.md#positioned-windows-file-io)

A new foreground operation during capture prevents the older result from
being published. With a test capture held at its first backing read, the
WinFsp callbacks read, overwrote and read back the same extent before the
capture was released. A separate memory-provider test verified both the
latest file bytes and the leased older view, then reclaimed its locked
pages on lease release. A Radeon RX 9070 XT worker decoded two leased
frames byte-for-byte. The independently verified CPU decoder served
the same bytes after a module refusal.
[Capture and recovery verification](evidence/read-capture-lease-2026-11-22.json)

A single checked range query captured four adjacent Windows SDK extents
using 43,781 backing bytes. Four independent captures requested 71,852
bytes. Two consecutive demand calls consumed all four results and read
18,714 backing bytes. Including preparation, the route requested 62,495
bytes against 53,138 for the same two ordinary reads. A smaller live
memory allowance funded one frame, with byte-exact ordinary reads for
the remainder. Capture checks the aggregate encoded and decoded grant
before opening each payload and skips payloads that use another codec.

The Radeon RX 9070 XT probe decoded two frames captured from a single
lease. Both matched the ordinary independent CPU read and arrived through
the indexed reader. Refusing the worker module still served both extents
byte-for-byte on CPU. The successful prepared worker took about
45 ms after submission, with setup measured separately at about 1.27 s.
These are scoped callback and worker measurements. Automatic admission
needs full-path results for its attached provider, including preparation,
unused work and foreground interference.
[Batched capture verification](evidence/batched-lz4-capture-2026-11-22.json)

### Reusing the verified metadata

Capture can move its checked object lookup pages and decoded name-index
metadata into the ready window. The foreground query borrows that memory
only for the same backing owner and full committed checkpoint, including
the data generation, checkpoint generation and root reference. The
namespace root and inode checks still run on the active snapshot, as do
typed extent and decoded-byte verification. The memory is advisory. It
holds a weak reference to the backing and leaves the old range free to
retire once the capture lease ends. A different checkpoint, reported
backing damage or pending payload repair discards the retained metadata.
[Copy-on-write tree reference generations](https://btrfs.readthedocs.io/en/stable/dev/dev-btrfs-design.html) ·
[Cache coherency data](https://docs.kernel.org/6.18/filesystems/caching/netfs-api.html)

| Windows SDK fixture | Ordinary backing bytes | Capture backing bytes | Prepared demand bytes | Complete prepared bytes |
| --- | ---: | ---: | ---: | ---: |
| One extent | 17,887 | 17,887 | 863 | 18,750 |
| Four extents served by two calls | 53,138 | 43,781 | 1,728 | 45,509 |

The four-extent window used 14.4% fewer backing bytes end to end. The
single-extent window still used 4.8% more. A write through another handle
forced both original demand calls to use the current checkpoint. A
reported backing fault restored the ordinary backing traffic, and the
disposable file-backed fixture retired its old range with cached metadata
still retained.

The Radeon RX 9070 XT supplied both frames through the same metadata
handoff in the isolated worker fixture. Capture and demand together
requested 27,689 backing bytes, compared with 35,393 for its matched CPU
reads. The worker still spent about 1.39 s preparing and 39.5 ms from
submission through verification and retirement. Read-ahead remains
opt-in pending workload and device qualification.
[Metadata reuse verification](evidence/prepared-read-metadata-2026-11-22.json)

### AMD read-ahead in the volume controller

A qualified read profile binds the exact helper, device module and signed HIP
runtime, adapter identity, dispatch shape and worker allowance. The volume
controller checks and pins the package before it enables the optional lane.
Its idle owner captures the proposed extent range, sizes one dispatch from
the actual frames and obtains fresh Windows device and host memory evidence.
The existing sized worker channel owns the immutable request and enforces the
child's memory, CPU and deadline. This path starts with a complete request
so it can submit immediately. [Windows job lifetime](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects) ·
[AMD HIP batching guidance](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/performance_guidelines.html)

Foreground demand continues through the independent reader. Foreground
activity can supersede the captured epoch. The controller then signals
cancellation and rejects the late result. A returned batch needs confirmed
child and supervisor retirement, current resource admission and a matching
callback epoch. The foreground store verifies its checkpoint, typed extent
and decoded digest before using the frame. The mount owner also checks
worker retirement when it stops. A refused device configuration uses CPU
for the remainder of that attachment. Temporary pressure uses the shared
GPU recovery policy and requires fresh observations.
[Worker ownership](WORKER_CHANNEL.md#resource-sized-transport-experimental) ·
[GPU recovery](GPU_RECOVERY.md)

The live Windows SDK fixture consumed both frames of a Radeon batch
byte-for-byte through ordinary callbacks. A foreground read served exact
data on CPU while the second batch was pending and the owner retired that
batch as superseded. Controller stop retired a third pending batch. A
separate mismatched architecture profile exercised the worker's
compatibility refusal. Normal CPU reads and the CPU read-ahead
baseline continued, and the refused profile made no second budget query or
launch. These checks used fixed admitted test windows.
[Owned Radeon read verification](evidence/owned-amd-read-ahead-2026-11-22.json)

### Selected sealed images

The ordinary indexed capture lease can read through a protected immutable
base and a writable tail. Both component providers must support concurrent
reads. Foreground writes can advance the selected checkpoint while a
capture still owns the previous immutable records. Frame delivery verifies
its exact read key and decoded bytes through the existing CPU path.
[Sealed read ownership](SEALED_INDEXED_IMAGE.md#reads-during-capture-and-tail-writes)

The Windows controller fixture selects a sealed image, retires the
preparation session and reopens it through the checked indexed adapter.
On the Radeon RX 9070 XT, the worker decoded two frames from the sealed
image and SDK callbacks consumed both verified results. Two later flights
were superseded by foreground demand and shutdown. The worker refusal
fixture continued serving the expected bytes on CPU and used CPU
preparation for the next eligible window. Both variants used the same
explicit maintenance admission policy as the mirrored fixture.
[Sealed AMD read verification](evidence/sealed-amd-read-captures-2026-11-23.json)

The owned indexed memory controller also selected a sealed image during
normal maintenance with an adopted file context held open. The same SDK
read callback consumed two verified frames decoded by the RX 9070 XT.
Foreground CPU reads superseded a later flight. After controller retirement,
the selected image reopened for an ordinary write, followed by independent
CPU readback. This fixture used the actual AMD runtime and SDK callbacks
with a controlled lifecycle.
[Automatic selection and Radeon evidence](evidence/automatic-memory-sealing-2026-11-23.json)

### Installed decoder selection

The Windows helper can accept an approved `native_lz4_read_profile` in the
same AMD manifest as compression and shared-base search. The decoder worker
and its module are colocated under `amd/native-lz4-read` next to the installed
application. The package verifier binds their hashes, the expected export,
the exact HIP runtime, the PCI hardware identity, architecture and wave
coverage, and the measured frame, memory and lead-time allowances. The
package must supply a digest-bound qualification record.
[Installed runtime discovery](AMD_RUNTIME_DISCOVERY.md#installed-native-read-role)

The volume owner learns lead time from the shorter of the two most recent
intervals between adjacent, equal-size reads on the same handle. It subtracts
time already spent since the hint and requires the profile's minimum before
requesting device identity. The controller acquires the package on its owned
background thread and lets the current read window use the CPU path. A later
window with enough retained lead and live resources can use the verified GPU.
Dispatch also accounts for the latest completed GPU turnaround from
submission through publication. The closed CPU reader still handles
ordinary demand and independently validates any prepared result.
[AMD batching guidance](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/performance_guidelines.html)

A short interval stays on CPU without querying DXGI or launching the
decoder. After an acquisition refusal, a later eligible workload can
recheck installation, device readiness and budget evidence at a bounded
cadence. Changed evidence can admit a new acquisition. After a decoder
configuration refusal, that attachment keeps serving CPU reads.

The physical Windows callback fixture acquired the installed RX 9070 XT
with one identity query and consumed two verified frames. The supervised
worker exchange took 4.406 seconds, and the time from submission through
publication was 5.438 seconds. A subsequent window with 1.5-second read
intervals used CPU without another budget query or GPU launch. This
qualifies installed routing and the timing-based CPU decision for the
tested fixture. Matched, representative mounted-workload measurements
still determine whether to ship a profile for this class.
[Installed read verification](evidence/installed-native-amd-read-2026-11-22.json)


### Host transfer and startup costs

The worker uploads descriptors and encoded bytes. When the verified HIP
runtime exports `hipMemsetAsync`, it initializes the output and guard
bank on the same stream as the decoder. Runtimes without that export
receive the initialized bank from existing host scratch. Both routes
reserve the full device slab and check all completion descriptors,
output digests and guard bytes before the result can leave the worker.
[AMD HIP memory operations](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/hip_runtime_api/modules/memory_management.html)

The Windows RX 9070 XT probe alternated host and device guard initialization
on the same worker build. Each result contained the same two verified
frames. CPU reads returned exact bytes during the exchange and after
verification. The public sample used the first 131,035 bytes of AIT
DCC 2026 file A.

| Input | Host-fill upload | Device-fill upload | Readback |
|---|---:|---:|---:|
| Synthetic | 148,160 B | 17,024 B | 131,168 B |
| AIT DCC 2026 file A prefix | 257,088 B | 125,952 B | 131,168 B |

Cold exchanges overlapped across the two routes at 0.767 to 0.990 seconds.
Device initialization took 0.599 to 0.771 seconds. The prepared worker
completed in 50.9 ms after submission, following 906.2 ms of setup. CPU
session reads of the matched cold inputs took 170 to 266 µs. Transfer
reduction is proven here. The measurements leave GPU speed benefit for
small reads unqualified.

A separate SDK callback control reran the prior decoder with the same
binaries and the same in-process memory backing. The fixture-only 20%
CPU cap multiplied the verified supervisor cap of 19%. With only the
supervisor cap and the same worker limits, the two runs took 1.000 and
1.016 seconds through publication. With both caps, one run took 5.562
seconds and another deferred at device budget admission. Compare larger
prepared batches against complete CPU cost with matched supervisor and
worker limits. Keep exact CPU validation and confirmed worker exit.
[Windows nested CPU limits](https://learn.microsoft.com/en-us/windows/desktop/api/winnt/ns-winnt-jobobject_cpu_rate_control_information) ·
[Transfer and lifecycle verification](evidence/native-amd-read-lifecycle-2026-11-22.json)

[Windows memory reservation and commit](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc) ·
[Windows page retirement](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree)
[Windows cached read-ahead](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ccschedulereadaheadex) ·
[Linux read-ahead behavior](https://docs.kernel.org/6.18/core-api/mm-api.html#readahead) ·
[AMD HIP transfer and allocation guidance](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/performance_guidelines.html)
