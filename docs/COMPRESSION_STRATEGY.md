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

## Adaptive foreground and collective background policy

Owner clarification, 11 November 2026: automatic selection must depend on the
host and current workload. Discover actual CPU/GPU capabilities, observed
compression/transfer cost, current load, physical/commit/VRAM headroom and
storage characteristics. Re-evaluate after meaningful resource, data or workload
changes. A GPU's presence alone cannot establish a throughput or ratio benefit.

Foreground writes should provide useful lossless compression promptly. A
qualified, available GPU may enable significantly stronger foreground compression
when measured total costs and available resources support it. A powerful idle CPU,
a busy GPU, small writes or difficult-to-compress bytes may favor a different
route. Choose from measured outcomes; avoid fixed assumptions about hardware,
installed tools, a universal initial level or the owner's machine.

After publication, a bounded local learning policy should analyze related files
collectively and select deeper compression, dictionary sharing or layout
improvements when their complete measured payoff justifies the work. This work
must remain very low priority, yield to foreground demand, and keep recoverable
source data until replacement verification and durable handoff are complete.
Its scheduling, batch sizes, model/dictionary memory and compression strength
must adapt within strict resource ceilings.

The objective combines sustained read/write throughput with exceptional storage
reduction. Neither nominal GPU capability nor a sampled ratio establishes that
objective. Validate full physical savings, read/update amplification, maintenance
cost, foreground interference and independent recovery. The final policy is
automatic and workload-driven; existing primitives are not that complete system.

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

## Historical v1 container checkpoint

The v1 CPU container supports raw and independent Zstandard chunks at levels
1 through 5. This checkpoint optimizes workspace reuse and copies while keeping
the same storage output. The internal extent mechanisms are isolated from archive framing, and the HIP
candidate remains offline-compiled preparation. There is no new codec,
ratio improvement, deduplication, writable mounted store or measured GPU speedup
in this checkpoint. Future format work must retain an independent CPU recovery
path and explicitly define migration and transactional recovery.

## Transactional native compression implementation

The object engine now selects raw, implicit zero, verified motifs and optional
bounded Zstandard records before the applicable transactional publication protocol. Replicated current
checkpoints retain three durable barriers; see REPLICATED_CHECKPOINTS.md. Private compressed
candidates must pass CPU round-trip comparison; committed records bind stored
and decoded integrity. Unknown/already-compressed bytes retain a raw route.
This advances automatic native storage compression; it does not establish
superiority over other filesystems or actual GPU acceleration.

OBJECT_STORE_FORMAT.md specifies the policy, allocator ceilings, compatibility
and hosted RAM comparison. The comparison reports complete physical image end
and read/write/verification costs separately. It is an initial controlled
native-engine experiment, not the requested popular-suite/multiple-filesystem
qualification. Broad corpora, dictionaries, redundancy across extents, durable
namespace writes, reclamation and actual AMD runtime measurements remain required.

## Persistent learning and durability requirement

The final policy learns over time from actual workloads and bounded idle
microbenchmarks, retaining parameters and progress in a small internal area.
AUTONOMOUS_MAINTENANCE.md defines its helper/driver update boundary, storage
accounting, crash-safe publication and verified repair requirements. Decode-critical
models/dictionaries remain protected versioned data dependencies. Learning-state
loss must not invalidate otherwise recoverable user data. The complete persistent
learner, autonomous scheduling and repair executor remain unfinished.

The bounded GPU execution/observation checkpoint is documented separately in
GPU_MEMORY_OBSERVATION.md. Its tiny internal byte comparison does not qualify
automatic filesystem GPU routing, compression benefit or the autonomous policy.
