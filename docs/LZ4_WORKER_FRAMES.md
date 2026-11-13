# Bounded LZ4 compression-candidate frames (experimental)

These CPU library primitives prepare and validate candidate messages for a future
supervised compressor. They add no GPU caller, module load, automatic filesystem
route or stored codec. The existing WorkerExchangeSession supplies process and
thread ownership; this format creates no launcher, queue, retry or background job.

## Owned input and resource admission

PreparedLz4WorkerBatch owns one to eight nonempty chunks, each at most 64 KiB.
Before copying, it admits both the source extents and the COMPLETE existing
GpuLz4DeviceLayout. All device tables, input, worst-case output, guards, alignment
and scratch still fit 256 KiB together. A batch inside the logical-input cap can
therefore be refused by the stricter complete-device cap.

A canonical count/size directory and the owned plaintext are hashed with the
existing two-span BLAKE3 record-digest helper. Chunk boundaries are part of the
digest. This is unkeyed integrity, not authentication. Caller changes to the
original chunks cannot change the prepared input. Explicit swap-based moves
leave the old object empty; borrowed views expire on replacement, move or
destruction. One control thread must retain immutable spans throughout each call.

The preparation owns at most 256 KiB of plaintext plus fixed arrays. Candidate
verification reuses one private scratch buffer no larger than 64 KiB, then copies
accepted compressed chunks within the bounded total. These are per-call logical
limits, not an application memory reservation or permission to keep an unbounded
number of batches. A future controller must retain the existing headroom,
one-outstanding-request, transport, child and supervisor budgets.

## Serialized representation

Every field is explicit little endian. No pointer, native Layout, path, grant or
CPU decoding workspace crosses the channel. The fixed header is 64 bytes:

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | FLFSLRQ or FLFSLRP followed by byte 1 |
| 8 | 8 | Nonzero request identity |
| 16 | 4 | Chunk count, 1 through 8 |
| 20 | 4 | Total logical source bytes |
| 24 | 4 | Reserved, zero |
| 28 | 4 | Request zero; reply expected CPU-semantics or HIP declaration |
| 32 | 32 | Digest of canonical count/sizes and source bytes |

A request body contains count again, its four-byte chunk sizes, and densely
concatenated source. Both counts, every size, the exact aggregate, complete
device layout, frame length and digest must agree before ownership is created.
The owned copy is independently rebound to the same source digest afterwards.

A reply directory has one logical-size/stored-size pair per chunk, followed by
dense compressed payloads. Parent count, source size, request ID, backend and
digest must match exactly. Every logical size equals its retained source; every
stored size is positive and inside its planned output capacity. Truncation,
trailing bytes, overflow and reserved/unknown fields reject before decoding.

The request maximum is 262244 bytes and the reply maximum is 262272 bytes,
within the existing generic mapping caps. No mapping or transport limit grows.
The distinct magic cannot pass the existing decode or match frame acceptors.

## Independent verification and publication

The producer validates every candidate with the existing optional CPU LZ4
decompress-safe wrapper before serializing a success reply. The parent repeats
that verification against its OWN immutable source before copying any candidate.
Every decoded length and byte must match. All chunks succeed together; there is
no partial successful reply or implicit raw substitution.

Missing CPU LZ4 verification raises an explicit unavailable error. A future caller
must preflight gpu_lz4_cpu_verification_available before launching work and keep
CPU filesystem operation available. Malformed framing/invalid candidates reject
separately with invalid_argument. A valid encoding need not be a unique canonical
byte representation: exact recovered plaintext is the acceptance condition.

Backend tags are declarations by the trusted helper, not GPU attestation.
Successful parsing cannot authorize a launch or storage publication. The caller
still requires authenticated helper/module ownership, fresh resource/result
gates, correct generation and actual successful child AND supervisor retirement.
Late/failed helpers cannot expose bytes through the generic transport. Normal
I/O remains CPU-only; candidate bytes are not a new persistent filesystem codec.

## Qualification scope

The new portable tests cover mixed and maximum chunks, eight-chunk batches,
actual pinned CPU LZ4 output, independent literal fixtures, missing decoding,
source/directory binding, owned copies, moves, every small-frame truncation,
header/directory corruption, payload mismatch and cross-kind rejection.
They never initialize HIP or execute a GPU.

The Windows fixture runs only its own trusted test binary under the unchanged
job/channel limits. It covers real CPU candidate roundtrip, decoder-unavailable
refusal, malformed input, corrupt output and failure after publication. Existing
exact retirement, closed mappings and ZERO handle-growth checks remain intact;
no allowance, thread count, process count or deadline is increased.

The prior private typed-module link passed at c86559b, including all 13 core jobs,
122 sanitizer tests, five 144524-check validator configurations, the separate SDK
link and 20 confirmed worker exits.
[Evidence](evidence/hipcomp-module-link-c86559b.json).
This publishing source needs its own CPU, sanitizer, Windows transport and
integration execution. No actual compression worker/runtime or GPU output is
qualified. D01, useful performance, filesystem routing and full product exits
remain open.

## Executed source-bound qualification

At 959606a all five core configurations and all 13 ordinary integration jobs
passed. Codec-enabled Linux, Windows native and ASan/UBSan logs each report
621 framing checks; both raw-only configurations report 593. The trusted Windows
CPU child exchanged three chunks totaling 1074 source bytes. The complete
existing-plus-new exchange suite reports 1209 checks enabled and 1197 disabled,
with 79 initial and final handles. All five admitted LastTest.log reports contain
zero failures. [Evidence](evidence/lz4-worker-frames-959606a.json).

The next [checked slab layer](GPU_LZ4_SLAB.md) constructs bounded upload bytes
and independently verifies complete synthetic readback. These are CPU checks,
not module loading, an actual HIP compression worker, GPU output or useful gain.
