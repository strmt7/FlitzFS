# Background compression research and implementation plan

13 November 2026. Experimental hypotheses, not an implemented codec or a measured
compression claim. This extends A02/A03/A04 in the full product plan. Useful actual
GPU acceleration with independent CPU recovery remains mandatory.

## Optimization priority

The owner's priority is minimum TOTAL stored bytes. Latency may increase and
retrieval may slow moderately if that buys substantial reduction; avoid extreme
latency or dramatic sustained-speed degradation. Do not reinterpret this as
minimum latency or require the deepest codec to be faster than conventional
codecs. Initial ingestion still needs a practical fast path.

Treat access cost as a practical acceptance constraint, not a coequal objective
that automatically defeats a smaller representation. Establish it from measured
cold/warm whole-file and interactive/random operations and workload behavior,
without inventing a universal threshold or locking in tiny compression groups.
Evaluate larger groups, deeper shared models and more GPU work when their net
size wins justify the retrieval cost. Account for all models, indexes, checkpoints,
protected dependencies and redundancy; an apparently small payload is not enough.
Keep the measured size/access frontier and adverse cases visible.

Safety bounds on experiment allocations and process work are not permanent
limits on filesystem compression-group size. Stream or tile computation within
those caps; any necessary experiment-cap change still needs audited resource
admission and isolated qualification. Never weaken data integrity, CPU recovery
or host safety to improve compression.

## Product requirement

Fast ingestion is the first stage. The write codec must not constrain a later,
much more expensive, GPU-supported representation for cold data. That later
stage may combine existing mechanisms or introduce an experimentally validated
new algorithm. Files must remain byte-exact and individually accessible with a
measured, manageable retrieval penalty; minimum total storage takes priority. A successful LZ4 kernel does not satisfy
this background compression requirement.

Use content and observed workloads to find opportunities across files and
generations. Do not assume extensions imply content or apply one model to every
file. Background work must be automatic, persist useful learning, remain very low
priority, and promptly yield to foreground activity under fresh resource evidence.

## Transparent cross-file grouping

The user sees ordinary files and folders in Windows Explorer. Internal compression
groups must not become archives, require extraction, rename/rearrange folders,
or expose optimization artifacts as user files. File identity, names, permissions,
timestamps, logical lengths, sparse regions and Windows handle behavior belong
to the normal namespace independently of physical compression placement.

Group by measured content similarity and access relationships, potentially at
chunk rather than whole-file granularity. A logical file-offset map resolves to
independently indexed compressed data and protected shared dependencies. Reading,
editing, moving or deleting one file must preserve all other files that share a
dictionary, base, model or physical extent. Use transactional generation changes,
stable logical identities and protected dependency ownership; no unbounded delta
chains or whole-volume solid decoding. Test random small reads, concurrent
updates, deletion of base-file names and recovery during regrouping.

## Research questions and candidates

These are competing hypotheses to investigate, not selected architecture:
- GPU similarity search, clustering, shared dictionaries and bounded delta bases
  may remove redundancy that independent per-file compression misses.
- A locally learned byte predictor with exact residual/entropy coding may improve
  hard residuals. Compare compact models against large models after charging all
  model storage, loading, inference, checkpoint and retrieval costs.
- Reversible content-specific transforms may expose coherence in structured
  binaries, numeric arrays, source trees or document families. The format's
  original byte representation, padding, exceptional values and metadata must
  reconstruct exactly; semantic similarity alone is insufficient.
- A new composition could combine a shared content representation with restartable
  local predictors/residuals and GPU-parallel decode. Establish prior art before
  calling it novel, and measure it against the individual components.

Recent discovery candidates are L3 (ACM 2026), GPU lossless invocation/dependency
work (November 2026), diffusion-LM lossless compression (August 2026), fixed-capacity
causal GPU models (November revision), invariant bit packing (2026) and
cross-backup base selection (October 2026). These are discovery leads only.
Search snippets and generated paper summaries do not qualify their claims.
The reviewed isolated crawler now has a closed six-page scope; each page needs
native capture, digest, substantive scope review and failure retention.
A preprint's results remain author claims until reproduced in relevant conditions.
Numeric-column, ML-tensor or repeated-backup results do not prove general
filesystem behavior. No vendor GPU implementation implies AMD compatibility.

## Access and decoder contract

Select compression groups and restart/checkpoint spacing by measured whole-file
and small random reads, cache-cold/warm latency, amplification and total size.
A small read may decode a larger shared unit when the size gain justifies it;
it must not require decoding the entire volume or an unbounded chain of files. Bound dependency fanout, chain depth, decoded scratch, work and
cancellation points. Charge indexes and restart metadata to compression results.

Learned policy state is rebuildable advice. A model, dictionary or base required
to decode stored bytes is protected immutable versioned data, independently
recoverable and reference-counted with its extents. Do not replace it with the
latest mutable model or require a network service. Keep inference/coding
deterministic across the supported CPU and AMD implementations. A floating-point
probability mismatch cannot be waved away by calling a method lossless.

Every new persistent encoding needs a documented versioned decoder contract,
bounded hostile-input rejection, an independent CPU reader and GPU/CPU byte
parity. GPU disappearance must preserve access through CPU recovery without
rediscovery loops; stale GPU results never replace selected CPU bytes.

## Resource and replacement contract

Retain existing hard experiment caps and source-bound process supervision.
Reported low stream priority alone is not evidence of low interference. Measure
foreground impact, GPU/CPU use, memory and cancellation responsiveness, and stop
or decline work when fresh headroom is absent. No idle worker, recurring host task,
unbounded training or model download is authorized by this research plan.

Recompression must use a new generation with rollback: verify complete decoded
equality and all required decoder dependencies before publication. Retain the old
generation until the defined durable publication/reader-retirement conditions
pass. Test interruption at every metadata/model/payload publication phase;
never turn an optimization into a loss of the only readable representation.
Process-interruption and memory fixtures do not establish physical power-loss
survival.

## Experiments and decision gates

1. Finish the bounded actual-GPU file-operation slice so the GPU path is real.
   In parallel, read the closed current primary-source scope and identify
   reproducible algorithms, licenses, code and limitations.
2. Define a small licensed mixed corpus with source-family-separated training and
   held-out evaluation. Include text/code, structured/numeric binaries, documents,
   multimedia, already compressed and incompressible controls, and modified file
   families. Start on disposable runners with strict corpus/resource caps.
3. Compare existing strong compression, dictionary and cross-file/delta baselines
   before a learned or novel candidate. Use the same lossless input and account
   for payload, metadata, indexes, models, redundancy and decoder dependencies.
4. Measure actual HIP GPU execution and independent CPU decoding separately.
   Record complete encode/read time, cold model/context startup, transfers,
   validation, peak resources, random-read latency, background interference and
   exact retirement. Repeat matched comparisons and report dispersion.
5. Prefer the smallest held-out total representation that meets practical access
   and speed constraints. A slower cold codec may win on size; both encode/decode
   speed need not exceed conventional codecs.
   Publish negative results and dataset-specific limits. Do not claim extreme
   universal gains, a new algorithm or full product completion from one corpus.
6. Integrate the winning representation with transactional replacement, resource
   governance and persistent learning. Qualify recovery and corruption handling
   before automatic unattended promotion.

The original write and recovery requirements remain intact. Research here must
produce an implemented, measured cold-data path; a research document is not its
completion.
