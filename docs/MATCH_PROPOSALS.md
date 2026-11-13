# Bounded compression match proposals

Staged recompression can expose a verified next object of at most 64 KiB
to an external match finder. It returns owned bytes and the object ID.
Submission occurs on a later serialized call. A changed source generation
invalidates the candidate before copying. The ordinary copy path remains
available when the optional sequence API or eligible input is absent.

Each proposal names an absolute match position, backward distance and length.
There are at most 4096 proposals. The encoder checks increasing nonoverlapping
output ranges, available history, length of at least four bytes, input bounds
and every matched byte. Validation work is bounded by the admitted input length.
Proposals, GPU metadata and accelerator status never enter the on-disk format.

The encoder first computes its ordinary CPU result. A link probe admits the
optional Zstandard sequence API, and execution additionally requires the
linked library identity to equal the compile-time version. The candidate
uses the same allocation caps, single-threaded settings, frame size/checksum
and independent CPU decoder as ordinary compression. Library errors, malformed
proposals and larger candidates retain the ordinary result. The accepted
candidate must decode byte-for-byte and fit the existing record rule.

This qualification route performs both ordinary and proposed compression.
Its purpose is to establish result quality and failure isolation before a
worker can supply matches. It provides no GPU execution or throughput result.
The private hint flag is diagnostic memory state. Record codec IDs, roots,
payload framing and historical reader behavior are unchanged.

Tests cover valid backward matches, rejected geometry, wrong match contents,
overlapping output, proposal-count caps, raw-only fallback and the public
managed-session copy/publication path. An accepted sequence-frame result must
be exercised when the linked API is available. Every selected frame is read
with the independent CPU decoder.

The current upstream Zstandard release was checked through its native release
API and remains v1.5.7. Its pinned header documents full-input sequence
compression, explicit sequence validation and recalculated repeat offsets.
The build checks the actual library symbols before enabling this optional path.
[Zstandard 1.5.7 API source](https://github.com/meta/zstd/blob/f8745da6ff1ad1e7bab384bd1f9d742439278e99/lib/zstd.h)

The current owner reference was inspected at
50bae7610275a8f68d1fb89f11f7e0e238187aba. Its dictionary matcher provides bounded
distance/length proposals with explicit effort accounting. That informs the
future device producer. No archive codec, archive framing or ZIP operation is
linked into this filesystem path.
[Reference match interface](https://github.com/strmt7/FlitzZip/blob/50bae7610275a8f68d1fb89f11f7e0e238187aba/src/gpu/dictionary_matcher.hpp)

## Bounded AMD producer

PreparedGpuMatchSearch owns at most 64 KiB of input and builds exact four-byte
prefix/position keys. Sorting those keys identifies each prefix's nearest
earlier occurrence. Sixteen-bit links suffice because the final bytes cannot
start a complete prefix. The temporary CPU key array stays below 512 KiB.
The owned input and links remain valid when the caller changes its buffer.

The device kernel searches one anchor every 16 bytes. Each lane examines at
most eight predecessors and compares at most 1024 bytes. Strictly decreasing
links bound even malformed chains. Match length is additionally capped at
8192 bytes. Input, links and output total at most 229,376 explicit device
bytes. Runtime, driver and module overhead require the existing separate
budget admission when transport is implemented.

Every output lane is checked on the CPU before greedy parsing selects
nonoverlapping ranges. Invalid effort, geometry or byte matches reject the
whole result. The staged encoder retains its independent sequence checks,
ordinary CPU size comparison and decoded-byte verification. Empty or weak
matches can therefore retain the ordinary compression result.

The complete SDK builds flitzfs_match_v1 and exports its separate typed
flitzfs_rocm_match_api_v1 table. The earlier decoder table keeps its own
contract. This compilation does not load the runtime. Worker transport,
hardware qualification and performance measurement remain required.

The algorithm adapts the owner's exact-prefix predecessor indexing and
bounded match search. On 11 November the current reference at
2c3c399df2a7b22c0240b5d9e1ac1f4437451045 retained the same matcher blob
4e202fe37476445f315d27301d674fc3fcc96c48 as the reviewed source at
50bae7610275a8f68d1fb89f11f7e0e238187aba. The filesystem producer adds its own
buffer, anchor, comparison and CPU admission bounds. The repositories share
the GNU AGPL license. No archive-format dependency is introduced.

## Supervised match transport

An explicit --match-adapter worker command accepts the qualified ordinal,
adapter LUID, node mask and protected WorkerChannel token. Normal operations
select their existing CPU routes. Match routing uses its own magic and a
64-byte header with request ID, source size, lane count, execution declaration,
zero reserved field and a digest of the complete owned input.

The request contains only source bytes. The trusted child rebuilds the sorted
predecessor index independently. It authenticates the private runtime/module
bundle and uses the same real Job Object, matched DXGI telemetry, low-priority
HIP stream and checked allocation/copy/launch APIs as the decoder worker.
Explicit device buffers retain the 229,376-byte maximum.

The child initializes every result lane to an invalid sentinel. It validates
the returned bytes and compares all match fields against CPU semantics for
this initial runtime qualification. It checks residency and retires buffers,
stream and module handles before publication. The existing process supervisor
must confirm actual child exit before copying a reply.

The parent checks ID, source digest, shape, backend and every suggested match
against its own immutable bytes. Only then can proposals enter the existing
staged encoder, whose source-generation and independent-decoding checks still
apply. A late or unusable result can retain ordinary CPU recompression.
There is no automatic retry or device search in this route. The long-lived
match routing/recovery controller still needs integration with GPU_RECOVERY.md.

The CPU wire tests use both execution declarations solely to exercise framing.
The Windows child fixture explicitly declares CPU semantics. It demonstrates
native publication before a held child's actual exit, followed by cancellation,
confirmed retirement and closed channel objects. Full SDK tests compile the
real HIP dispatch and exercise refusal before runtime loading. Actual AMD
execution and measured benefit remain unverified.

The explicit match route now uses the long-lived [match recovery controller](GPU_MATCH_CONTROLLER.md). It preserves the shared evidence/quarantine policy across requests and retains pending source data until exact retirement. Caller-selected CPU work stays terminal while late result evidence is classified. Normal filesystem operations still require explicit controller integration and workload/hardware qualification.

## Actual producer and native selection feedback

The tiny source-bound 04d884b GPU producer trial accepted four proposals for a
4-KiB synthetic source and matched CPU semantics. See
[evidence/gpu-admission-04d884b.json](evidence/gpu-admission-04d884b.json).
No native image operation ran in that trial.

The new [recompression feedback API](RECOMPRESSION_FEEDBACK.md) records whether
the native encoder actually selects a suggestion encoding, together with its
ordinary CPU baseline payload size and exact source image/generation/object.
Supplying, forwarding and selecting hints are distinct observations. Equal-size
selection is possible; payload-only counts do not establish whole-image savings
or speed. This new API still needs matching implementation qualification.

## Input-only parent preparation

prepare_request() owns source bytes without building an unused parent CPU index.
Existing prepare() and trusted child decode still build the exact old index.
The former has no CPU predecessor accessor: requesting it throws explicitly.
Both forms declare the same planned device footprint and encode byte-identical
requests. Parent proposal validation uses the independently retained input bytes.
Explicit hardware and native-composition callers use the input-only path; CPU
semantic callers retain prepare(). See GPU_PHASE_PROFILE.md for qualification
and separate wall-time diagnostics. No codec, kernel, stored or match-frame change.

Match scratch now uses the checked gpu_match_device_layout offsets: input at
zero, 2-byte-aligned predecessor links and 8-byte-aligned results in one owned
device allocation. One initialized host upload includes explicit little-endian
links, zero padding and invalid output sentinels. Padding counts toward the
same 229,376-byte maximum; CPU scratch adds at most that amount. Only the base
pointer is released, and the kernel receives the original three pointers.
The complete original child index/search/byte verification remains required.
This is pending changed-source qualification and does not establish GPU benefit.
