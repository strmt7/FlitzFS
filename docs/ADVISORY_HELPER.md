# Windows helper advisory recovery (experimental)

13 November 2026. Publishing-source VM qualification is required. This connects
the qualified shared-engine rebuild to the existing unmounted Windows helper.
It is not an automatic learner, durable physical mapping or owner-PC test.

## Maintenance, not ordinary file I/O

VolumeModel.maintain_optimization_state runs only as an explicit serialized
maintenance opportunity. Optimize calls it before allocating an image candidate.
Ordinary saves and reads never inspect or update advice.

A healthy owner republishes its identical cached state through the existing
journal check: both actual records are verified, without writes, sequence
advance or allowance consumption. Allocation/preflight failure that leaves
advice healthy propagates without relabelling it as damage.

Unavailable/poisoned advice permits at most one fresh-area attempt. The actual
memory provider allocates it with the unchanged capacity, residency and
physical/commit-headroom checks. There is no timer, hardware search or retry
loop. Newly available capacity can admit a later distinct explicit opportunity.

Four total bindings remain the limit. Exhaustion returns unavailable and defers
image optimization before candidate allocation. It neither fences the data
writer nor disables ordinary file access. This does not reset an exhausted
healthy publication allowance or implement persistent workload-derived credit.

## Actual provider ownership and accounting

The model retains a fixed array of all admitted advice backends, matching the
core's binding count. Stores come exclusively from its private MemoryVolumeDomain;
no arbitrary backing factory, file/device path or runtime fault option is added.
The provider already owns one process-wide domain lease, a bounded store registry
and fresh allocations. Do not infer global physical or cross-process authority.

The core may retain a partial candidate when creation throws. The model records
that admitted mapping on the exception path too. A fixed preflighted array slot,
the core's no-I/O/no-allocation scalar getter and a statically checked nonthrowing
shared-pointer move complete adoption. Earlier stores remain retained by both
the core and the helper, including poisoned leases.

The existing 12-MiB app domain and all process/CPU/time limits are unchanged.
At most four 128-KiB advice reservations are retained, inside that aggregate.
The summary counts all written advice extents and separately reports provider
allocation. It shows cached optimization-state readiness, not continuous integrity
attestation. Advisory failure does not mark a healthy data view as fenced.

Explicit discard retires the session and every advice reference before releasing
the domain. Provider cleanup failure still fails closed. Process termination
loses this temporary provider's contents; no power-loss durability follows.

## Configured qualification

Every old model/controller/GUI check remains. New tests damage only freshly
allocated private test-memory advice: one/both copies, repeated replacement,
four-area exhaustion, unchanged old bytes, real file continuation and a retained
cleaned handle. Filling the provider's own small fixed reservation tests capacity
refusal, unchanged mapping and a later admitted rebuild after release. This is
not global memory exhaustion or kernel/GPU fault injection.

The qualification binary's friend fixture alone can reach its own private
backings. It adds no product command, flag or provider override. The GUI lane
checks the readiness summary through the real native control and retains exact
child exit, handle balance and working-set restoration checks.

The new helper paths require their own executed publishing-source results.
Parent 745d023 passed all 13 core/mounted regression jobs, 687 advisory-model
assertions, 136 Windows native groups, 117 sanitizer groups and the existing app
workflow. These parent results do not qualify this new helper implementation:
[evidence](evidence/advisory-rebuild-745d023.json).

The memory backend has process-lifetime mappings only. Physical reservation,
durable selected-area mapping/reclamation, persistent write credit, helper IPC
authorization, an autonomous low-priority executor and held-out learning/GPU
benefit remain full-product work. The existing explicit worker is not that executor.
