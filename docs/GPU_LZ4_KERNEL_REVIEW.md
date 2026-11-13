# Pinned LZ4 kernel review and pending current-AMD captures

This is a source review of ROCm/hipCOMP-core at
03df9f7369211ee15d370817165224c73fdc9a46, not GPU execution or a safety certificate.
The admitted upstream query receipts and matching source hashes are in
[evidence](evidence/gpu-codec-navigation-905a4d3.json). The source remains an
experimental candidate; no filesystem operation or worker uses it.

## Reviewed bounded portions

The selected launcher uses one warp per chunk and the hash workspace calculated
from the largest chunk. Its interface does not accept output capacity. Only the
byte-oriented UCHAR path is under consideration; typed integer modes are not
qualified by the byte-block CPU verifier.

The reviewed output helpers in src/LZ4Kernels.cuh write a token, literal-length
extensions, exact literal bytes, a positive two-byte backward offset and optional
match-length extensions. copyLiterals and writeLSIC use explicit index bounds.
The compression loop advances by a positive valid-lane count when no match is
selected; its tail branch emits the remaining literals. Match-length work is
bounded by the admitted source length. These observations do not establish warp
coordination correctness, every arithmetic invariant or total driver latency.

The [complete allocation plan](GPU_LZ4_LAYOUT.md) admits tables, inputs, worst-case
outputs, guards and scratch together before any future runtime allocation. The
[independent CPU verifier](GPU_LZ4_CPU_VERIFY.md) separately checks exact decoded
length and source-byte equality in disjoint bounded scratch. Its passing
CPU-generated fixtures do not prove compatibility with actual GPU output.

## Unfinished synchronization and scheduling review

The AMD branch currently makes SYNCWARP1/SYNCWARP empty while using shuffle,
ballot and cooperating hash-table access. The NVIDIA branch uses explicit sync
intrinsics. This is an unresolved review question, not a confirmed AMD race or a
reason to patch vendor source from memory. The current selected SDK's documented
warp execution, memory visibility and synchronization semantics must be read
before changing or qualifying this path. Stream priority also needs current
documented limits before it can support background-impact claims.

The new closed rocm-warp research group captures three official HIP pages at the
selected ROCm 10.1.0 documentation scope: language extensions, programming model
and stream management. Its exact URLs, markers and failure/retired-scope checks
are code-reviewed; native Crawl4AI extraction and content review remain pending.
Search results located URLs only and supply no semantic evidence here.

The existing reviewed provider, robots checks, no personal profile/external LLM,
45-second page deadline, 3-GiB memory/2-CPU/256-task limits and private capture
directory remain unchanged. All three successful substantive native captures
are required. Publisher revision dates must be reported as unknown unless
actually present in captured metadata.

No candidate kernel is changed by this review. Before runtime use, finish source
and synchronization review, typed ABI and launch geometry checks, immutable owned
inputs, independent CPU validation of actual GPU outputs, exact worker retirement,
fresh resource admission and measured complete-operation benefit. Output guards
cannot prevent an invalid kernel write, and user-mode process isolation cannot
guarantee freedom from kernel/driver faults.
