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

Hosted build, correctness, sanitizer and performance results are pending at
initial publication. No product build, filesystem test or GPU workload was run
on the owner's PC. Successful hosted results must be recorded against their
exact implementation commit before any validation or throughput claim.
