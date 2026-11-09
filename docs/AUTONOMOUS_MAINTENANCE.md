# Workload-driven adaptation and verified repair

Owner requirements added during native-file scaling work. This is the required
product architecture and qualification ledger, not an implemented self-learning
service, autonomous maintenance executor or universal data-safety claim.

## Learning must follow evidence

The filesystem should learn from observed data characteristics, file/folder
workloads, compression outcomes, read amplification, update frequency, storage
layout and CPU/GPU transfer/compute cost. A filename extension is a hint, not a
codec contract. Selection must retain a lossless arbitrary-byte route.

Learning should improve future decisions and identify older extents worth
reconsidering. Candidate work includes stronger compression, dictionary sharing,
redundancy reduction, locality-aware placement and fragmentation repair.
Triggers should follow meaningful workload/content/model/resource changes,
confidence and estimated payoff, rather than a timer being the decision.
Safety deadlines may bound execution; elapsed time alone is not new evidence.

The model must be local, bounded and advisory. No external LLM, cloud upload or
personal-content service is needed. A model error must never change decoded
bytes, authorize unsafe I/O or bypass recovery validation. Persisted learning
state needs its own crash-safe encoding and should be rebuildable from observed
outcomes. Loss/corruption of advisory state must not prevent CPU data recovery.

Use bounded per-volume/context summaries, explicit decay/change detection,
confidence-aware exploration and held-out workloads. Count sampling/training,
dictionary/index memory, recompression debt and write amplification. Preserve
unfavorable results. Do not call a fixed heuristic or scheduled task a complete
self-learning filesystem.

## Background work must remain subordinate

No routine manual intervention should be necessary in the final product.
Before that can be enabled, the executor must prove all of these properties:

- Very low CPU and I/O scheduling priority, one bounded worker and small
  interruptible work units. Priority alone does not cap resource use.
- Fresh, trustworthy physical/commit/VRAM headroom and foreground contention
  evidence before admission and between batches; fail closed when unknown.
- Hard memory, queue, compute, transfer, I/O and cumulative-work ceilings;
  independently supervised cancellation and confirmed worker retirement.
- Immediate yielding to foreground application demand, memory pressure,
  overloaded storage/GPU or degraded accelerator health. Never let maintenance
  starve application writes, reads, recovery or durable-commit reserves.
- AMD acceleration for qualified beneficial tasks, using supported low-priority
  execution and measured end-to-end cost. Unsupported priority/preemption or
  unsafe GPU state means CPU work or no maintenance, not an aggressive launch.
- No repeated adapter scans or timeout-only recovery. Integrate the existing
  evidence-aware GPU policy; callbacks must not wait for driver teardown.

Foreground durability must not depend on maintenance finishing. Learning and
optimization must not create an unbounded backlog or consume the last recovery
space. Prove behavior under interactive workloads and contention, not merely
an idle synthetic benchmark. No owner-PC experiment is authorized by this plan.

## Repair must be verified and preserve acknowledged history

Immutable records, identity/generation binding, separate stored/decoded
integrity, durable publication barriers and independent CPU recovery are core
invariants. New representations remain private until independently verified;
publish references atomically before reader retirement and reclamation.

Repair requires verified redundant content in suitable failure domains.
Checksums detect damage but cannot reconstruct a destroyed sole copy. Preserve
damaged evidence and surviving roots, reject ambiguous candidates and never
silently roll back acknowledged data, fabricate bytes or accept failed integrity
as a successful CPU fallback. A GPU failure must never endanger a durable CPU
copy. Stop unsafe mutation when a safe repair cannot be proven.

No implementation can establish immunity to every future bug, dishonest flush,
common-mode failure or loss of all copies. State the backend/device assumptions,
redundancy/failure domains and independent-copy requirements. Test the claimed
fault envelope; do not turn a green process test into an unrestricted guarantee.

## Current implementation and ordered dependencies

The native file layer provides bounded transactional semantics and incremental
write staging with full public snapshot/open verification. Qualified custom
resume repairs a missing recovery root while preserving its survivor. Compacted
copies now provide a new-volume copy primitive with source preservation; see
COMPACTED_COPY.md for exact protocol and limits.

These do not yet implement persisted workload learning, automatic strategy
selection, live GPU execution, low-priority background scheduling, data-replica
self-healing, in-place GC or automatic durable handoff. The next mechanisms need:

1. Durable destination handoff, reader retirement and safe reclamation;
   resumable/cancellable work without reusing uncertain failure units.
2. Resource/priority admission and an isolated executor with tests that measure
   foreground interference and prove bounded debt under overload.
3. Bounded persistent workload observations and confidence-aware strategy choice,
   independent correctness gates and held-out workload evaluation.
4. Qualified AMD runtime/cancellation and CPU fallback integrated with the same
   policy, with contention/driver failures first simulated off-host.
5. Redundant data/metadata repair and fault-domain qualification, preserving
   acknowledged bytes through interrupted repair and reclamation.

Full writable application semantics, scalable metadata/allocation, broad native
compression comparisons and physical power-loss qualification remain required.
