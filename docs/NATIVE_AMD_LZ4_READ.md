# Native AMD LZ4 reads

The optional AMD compute path decodes standard LZ4 records from indexed
FlitzFS writes. It uses the existing record format and CPU decoder. The
device result can flow through `NativeFileSession::read` after the indexed
store verifies the full frame digest. The current executable is a private
qualification probe. Installed helper scheduling and mounted performance
remain open.

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
before accepting the sequence. The verified results leave the worker after
confirmed resource retirement. An unavailable or failed device leaves the
indexed reader's independent CPU path available.

## Source-bound qualification

The optional SDK sub-build consumes the private
`FLITZFS_ROCM_NATIVE_LZ4_READ_OBJECT` and the already pinned CPU LZ4 source.
The source is `src/gpu/native_lz4_decode.hip`. Use
`tools/dev/stage_native_lz4_decode.py --source <pinned-checkout> --output <new-directory>`
to copy the recorded AMD code and apply the exact native-sync transformation.
Use the staged entry and its source directory with the reviewed SDK compiler.
The typed exported table binds the launch layout and both wave entry points.
The verifier requires the exact device module digest, signed installed runtime
digest, current GPU identity and a bounded one-process job. Keep any initial
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
[Verification receipt](evidence/native-amd-lz4-read-2026-11-22.json) ·
[AIT 2026 dataset](https://aitdcc.github.io/dataset.html)
