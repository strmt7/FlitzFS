# Native AMD LZ4 reads

The optional AMD compute path decodes standard LZ4 records from indexed
FlitzFS writes. It uses the existing record format and CPU decoder. The
device result can flow through `NativeFileSession::read` after the indexed
store verifies the full frame digest. The private qualification probe runs the decoder in an isolated Windows
worker. The WinFsp SDK callback can consume the same type of ready result.
Installed helper scheduling and mounted performance remain open.

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
Automatic scheduling therefore still needs lead-time and full cost
measurements for larger read-ahead windows.
[Ready read verification](evidence/indexed-ready-amd-reads-2026-11-22.json) ·
[Windows file caching](https://learn.microsoft.com/en-us/windows/win32/fileio/file-caching)
