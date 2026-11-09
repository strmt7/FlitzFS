# Compression and throughput objectives

Owner direction, 9 November 2026: prioritize the combination of sustained read
and write speed and extreme physical filesize reduction, across diverse files.
Absolute lowest latency is secondary. FlitzZip provides engineering inspiration and candidate mechanisms; archive
compatibility is outside scope and does not constrain codec selection.
Beating current and experimental filesystems is the target, not a present claim.

## Product criteria

The filesystem must preserve arbitrary file bytes, including unknown formats.
Optimization must be optional for unsupported data types, with lossless CPU
recovery independent of AMD GPU availability. Preserve source data and verified
fallback when an accelerator is unavailable. A routing policy must distinguish
an unavailable accelerator from failed integrity or authentication; corrupted
storage must never be silently accepted as a successful fallback.

Measure physical allocation including metadata, dictionaries, duplicate indexes,
padding, retained snapshots and any recovery log. Report read and write
throughput with the same flush/durability semantics. A gain in one metric must
show its effect on the others, resource use and recovery. Latency still needs
bounded behavior for ordinary applications, even when it is not the main target.

## Candidates to investigate

These are research candidates, not implemented or approved production features:

- Content-aware selection between fast raw storage and different lossless codecs
  and compression settings, based on bounded sampling and measured payoff.
- Larger compression regions with independently recoverable subregions, balancing
  cross-file redundancy against read amplification and write/recovery costs.
- Shared trained dictionaries with explicit identity, lifecycle and recovery.
- Exact duplicate blocks, content-defined chunks and zero/sparse extents, with
  collision-safe verification, bounded indexing and crash-safe references.
- Reversible transforms for structured numeric and tabular data, executable
  code and repeated byte planes. Always retain a generic arbitrary-byte route.
- Bounded AMD HIP batching for stages that actually benefit from parallelism,
  accounting for upload, dispatch, download, GPU contention and CPU fallback.
- Background recompression only after transactional publication, resource
  admission, cancellation and independent recovery are implemented and tested.

Already-compressed or encrypted files must remain fully supported. A candidate
that expands them should lose to raw storage after all metadata is included.
Deduplication and compression results must be reported separately so a corpus
with repeated files cannot disguise a weak codec result.

## Evidence before selection

Use reproducible, openly redistributable corpora containing text/source,
documents, structured numeric data, binaries, raw media, compressed media,
archives, high-entropy bytes, zero/sparse data and duplicate/modified versions.
Keep training and evaluation data separate for dictionaries and adaptive policy.
Add holdout file types and mixed directory trees. Record corpus identities,
parameters, dependency commits and exact product commit.

Compare against relevant native compression and representative compressed and
deduplicating current/research filesystems on equivalent hardware and durability.
Record which comparator configurations were actually available and tested;
absence is not evidence of superiority. Alternate lanes, retain distributions
and raw measurements, verify every decoded byte, and report inconclusive noise.
Heavy comparisons belong on separately qualified disposable machines, never
automatically on the owner's PC. No GPU benchmark is authorized by a CPU result.

## Current implementation boundary

The v1 CPU container supports raw and independent Zstandard chunks at levels
1 through 5. This checkpoint optimizes workspace reuse and copies while keeping
the same storage output. The internal extent mechanisms are isolated from archive framing, and the HIP
candidate remains offline-compiled preparation. There is no new codec,
ratio improvement, deduplication, writable mounted store or measured GPU speedup
in this checkpoint. Future format work must retain an independent CPU recovery
path and explicitly define migration and transactional recovery.
