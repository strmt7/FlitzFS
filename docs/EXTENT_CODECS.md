# Experimental filesystem compression mechanisms

FlitzFS needs native compressed extents with independent CPU recovery.
This module supplies bounded candidate mechanisms for filesystem research.
The final extent format and codec selection must follow filesystem workloads,
durability requirements and measured compression/throughput tradeoffs.

## Retained mechanisms and scope

src/extent_codec.hpp and src/extent_codec.cpp provide internal CPU decoding of
raw/fill, repeated motifs, segmented static/adaptive prefix coding, Huffman,
optional LZ4 dictionary segments, sparse motifs, closed two-stage composition
and byte-plane transforms/contexts. Raw/fill are directly admitted; other encoded
payloads must be smaller than output. Output is positive and at most 16 MiB;
composition scratch is bounded by one output buffer. Callers supply disjoint
private output and must discard it on error.

Internal mechanism IDs preserve existing test/kernel experiments so GPU and
CPU comparisons remain meaningful. They are not a commitment to use any
particular encoding in the native filesystem or to preserve an archive format.
Unknown/missing mechanisms fail explicitly. There is no file, path, archive,
encryption or GPU-runtime I/O in this decoder. It performs structural admission,
not content authentication. Enclosing immutable records must verify integrity.

The allocation-free experimental HIP kernel still handles the bounded subset
raw/fill/pattern/prefix/adaptive-prefix/Huffman. GPU preparation uses the internal
extent decoder as its independent CPU reference. This preserves mechanisms
while removing archive coupling. Offline compilation and CPU semantic tests
do not establish GPU execution, hardware parity or speedup.

## Source provenance and independent checks

Mechanism research used the owner's FlitzZip repository at
4e6a1bb0d144611a09dcbc65276faa676e923882 as a source of compression and GPU
engineering ideas. This is source provenance, not a filesystem format or product
dependency. The CPU implementation is independent; test-only encoders generate
extent payloads directly.
The optional FLITZFS_EXTENT_REFERENCE_SOURCE comparison requires that exact
upstream commit and an unchanged reviewed source blob, extracts only selected
CPU mechanism functions, and links them into a test executable. No upstream
serializer, ZIP application, HIP runtime or upstream build script is linked or
run by the filesystem product. Upstream licensing/provenance is retained.

Tests verify exact bytes, segment boundaries, invalid tables/counts, truncated
payloads, prohibited recursion, closed composition rules, missing dictionary
dependency and allocation bounds. The object store separately tests corruption
against integrity-bound immutable records. Arbitrary mutations may decode into
different valid bytes; a decoder alone cannot promise original-byte integrity.

## Next design work

Choose native compressed extent framing, decoded integrity coverage, adaptive
codec/transform selection, dictionary lifecycle and recovery before connecting
these mechanisms to transactional records. Evaluate write cost, read throughput,
physical allocation, recovery and diverse holdout corpora. Preserve the generic
lossless path for unknown/already-compressed data and CPU fallback for every
committed encoding. GPU faults must not affect acknowledged data.

## Preserved transaction baseline

Before this scope correction, transaction checkpoint
9c3c1b1352b695779808d2c9e88ec1f276321905 passed all four triggered workflows,
including all six jobs in the hosted CPU/native/mount pipeline. Linux
raw/codec/sanitizer lanes passed 21 tests each; Windows minimal passed 24,
native passed 26, and the disposable mounted read/recovery probe passed.
The transaction engine is retained. Archive-specific tests are removed from
current totals; direct mechanism/guard-byte and filesystem-scope checks replace
that unrelated coverage. Match new test totals to the current commit's results.
