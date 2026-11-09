# CPU chunk workspace reuse

Development date: 9 November 2026. Experimental; no release or version change.
Baseline: `2cf06327c6d71ebdfe90aadbed9187f8921a7267`.

## Changed behavior

The packer owns one compressor context and input/output workspace for its entire
operation instead of creating codec state and buffers per chunk. Raw fallback
writes the input buffer directly. Readers lazily own one decoder context and
one compressed-input workspace; raw chunks read directly into the decoded cache,
and compressed chunks decode into that same cache. Verification and extraction
reuse it while continuing to read and check every backing payload.

Zstandard's explicit-context API retains allocation state across independent
frames. This is not a streaming-history or dictionary feature. All existing
frame-size, single-frame, checksum, input/output, exclusive-creation, flush and
disk-reserve guards remain in effect. There are no new threads or GPU calls.
The main objective is reducing CPU overhead, with unchanged compression ratio.

A load invalidates the cache identity BEFORE resizing, reading or decoding its
buffer. Only successful decoding and CRC comparison publish a new identity.
Any decoder exception discards the context, so subsequent reads start with
fresh codec state. Reader move/destruction retain sole ownership. Single-reader
threading rules and per-file catalog locks still apply.

Scratch retention changes memory lifetime: one encoded chunk up to 2 MiB can
remain allocated per reader, in addition to the 1 MiB decoded cache and codec
context. See SAFETY.md for aggregate catalog accounting. Raw-only builds do not
allocate a decoder workspace; empty packs do not allocate an encoder context.

## Regression and measurement contract

Core regressions alternate raw/compressed/short chunks at all five allowed
compression levels, compare reused-context frames to the original one-shot
Zstandard API, read in reverse order, move readers, inject payload corruption,
retry good chunks after rejection, and require corrupt extraction to fail before
creating an output. Existing malicious-input and no-clobber tests remain enabled.

The separate hosted CPU comparison builds the pinned baseline and candidate
with the same compiler/options/Zstandard library. It uses two deterministic
eight-MiB synthetic fixtures and 4/128/1024-KiB chunks. After a warmup pair, six
AB/BA alternating pairs measure pack, verify, four-MiB probe and extraction.
Each pair requires identical complete containers and SHA-256-matching source
and extracted bytes. Raw samples and host context are retained as an artifact.

The script refuses execution outside a disposable GitHub-hosted runner. Parent
address space is capped at 256 MiB; children at 128 MiB, eight CPU seconds,
15 wall seconds and 16 MiB per output file. Total admission is 120 seconds with
at least 1 GiB free-space reserve plus a 128 MiB fixture allowance. It deletes
only individually tracked files inside its uniquely created fixture directory.

Measurements include startup, normal filesystem I/O and flush. They use a warm
shared hosted runner without cache eviction and provide no GPU, power,
cross-filesystem or general workload-performance qualification. A lower timing
is not automatically a statistically established speedup.

## Source evidence and validation state

Source exploration used CocoIndex Code 0.2.37 with a current-root binding and
source digest `25977d889e161dc8e5f9eef692deddc5`; exact candidates were then
confirmed in live `src/container.cpp`. The local search receipt is retained
outside Git. The configured MCP registration for another repository was not
modified, and no other checkout was changed.

The official [Zstandard API manual](https://meta.github.io/zstd/doc/api_manual_latest.html)
documents context reuse, independent one-shot frames and explicit context
operations. Crawl4AI `0.9.5+flitzzip.portable2` extracted it with robots checking,
no persistent browser profile or external LLM and a 45-second timeout. Native
HTTP status was 200; substantive Markdown SHA-256 was
`0c9fb8969be62f9ddb4554d4e220b27a645cfa554416e84fef011ba940bf9533`.
The retained native-capture SHA-256 was
`d8bb759f94a804773b10e60ef0b48df52f3fb6cb2f73ed0a9cf392a901e2210c`.
The older facebook.github.io manual URL returned HTTP 404 and was rejected;
its failure receipt was preserved. No claim relies on its content.

Implementation commit: d6bb574afe21a3b5185c268df2eda9bdbd02f5e1.
All five workflows passed at that commit: [six-job main CI](https://github.com/strmt7/FlitzFS/actions/runs/42593011227),
[CPU comparison](https://github.com/strmt7/FlitzFS/actions/runs/42593011387),
[upstream compatibility](https://github.com/strmt7/FlitzFS/actions/runs/42593011289),
[offline HIP compilation](https://github.com/strmt7/FlitzFS/actions/runs/42593011221),
and [source evidence](https://github.com/strmt7/FlitzFS/actions/runs/42593011255).

The comparison used a four-vCPU AMD EPYC 7763 Linux runner, roughly 16 GiB RAM
and ext4. Complete containers and extracted bytes matched in all 84 warmup/
measured lane executions. Ratios below are baseline median time divided by
candidate median time; below 1 is slower. Six measured samples per lane.

| Synthetic fixture | Chunk KiB | Pack | Verify | Probe | Extract |
|---|---:|---:|---:|---:|---:|
| Text pattern | 4 | 1.170 | 1.251 | 1.159 | 1.231 |
| Text pattern | 128 | 0.958 | 1.277 | 1.071 | 1.292 |
| Text pattern | 1024 | 1.229 | 1.273 | 1.115 | 1.276 |
| Hash noise | 4 | 1.171 | 1.233 | 1.158 | 1.214 |
| Hash noise | 128 | 0.912 | 1.539 | 1.173 | 1.531 |
| Hash noise | 1024 | 1.094 | 1.434 | 1.181 | 1.440 |

Read/extraction medians improved in these cases; writes were mixed, including
slower default-128-KiB pack medians. This is not an overall write-speedup claim
or proof of statistical significance. Compression ratio is unchanged. Raw
samples/context are retained in the comparison run's artifact (14-day retention);
no unfavorable case is omitted. More write-path investigation is required.

A later owner-authorized local raw-only check preceded the subsequent hard
remote-only restriction; see GPU_RECOVERY.md. It was a temporary disk fixture,
not RAM-only. All future disk-based work stays off the owner's PC.
