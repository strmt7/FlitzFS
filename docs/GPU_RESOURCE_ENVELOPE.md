# Experimental GPU resource-envelope admission

Research baseline: a8fa5798c52848ad706fa0883a0844e0fbb2e127, 11 November 2026.
This implementation candidate is G01 in DEVELOPMENT_PLAN.json. Qualification
must match the publishing commit. It does not enable GPU work in normal file
operations, implement an AI learner, prove speed improvement or qualify owner
mounts. No owner GPU test of this candidate has run.

## Why the strict rule changes

The retained bc6d18d6 tiny synthetic observation completed GPU work, matched
12,288 CPU bytes and confirmed cleanup, but rejected the result solely because
aggregate nonlocal usage grew. A zero-growth requirement cannot distinguish
ordinary runtime/shared allocations from harmful resource pressure. Removing
the check without an alternative bound would also be wrong.

The worker, decoder result gate and match controller now share one pure
allocation-free envelope and one original-baseline transition check. It uses
live budgets and headroom, never this owner's device names or measured usage
as universal defaults. Existing 8-block/64-KiB/256-KiB, process-tree, single-slot,
identity, snapshot-age, deadline, CPU comparison and confirmed-retirement gates
remain. The 256 MiB reserve floor remains an experimental safety constraint,
not an assumption about installed capacity.

## Observations and arithmetic

The allocating worker's DXGI collector additionally calls GlobalMemoryStatusEx.
All observations share the existing start timestamp, so collection time counts
against freshness. Unknown/failed/inconsistent telemetry fails closed.

Required fields include both process memory-segment budgets/usages, dedicated
capacity, actual physical total/availability and current-process commit
limit/availability. Zero totals/budgets and impossible host availability are invalid. A valid OS
budget shrinking below current segment usage is resource pressure, as is zero
available capacity; neither proves a failed GPU context. ullAvailPageFile is process-constrained
commit capacity, not global free RAM. None of these snapshots reserves memory.

The candidate retains these reserves:

| Resource | Reserve |
| --- | --- |
| Local process GPU budget | max(256 MiB, local budget / 16) |
| Physical RAM | max(256 MiB, physical total / 16) |
| Process-constrained commit capacity | max(256 MiB, commit limit / 16) |
| Nonlocal process GPU budget | nonlocal budget / 16 |

After subtracting reserves and explicit device bytes with checked subtraction,
shared growth is bounded by the minimum of remaining nonlocal budget / 64,
physical availability / 256 and commit availability / 256. These are conservative
experimental interference controls. They are neither a learned policy nor a
claim that these fractions are optimal for every workload. Existing independent
process and allocation ceilings remain in force.

Completion re-evaluates current headroom and uses the smaller original/current
allowance. Its baseline is the original pre-initialization/admission observation,
including at the worker's initialized and completed phases. An intermediate
observation never grants another allowance. Counter decreases cannot underflow.
All malformed, stale, identity-changing, over-budget or over-envelope cases
select CPU; partial output is never accepted. A changed dedicated capacity under
the same request is an identity/configuration refusal.

Worker pressure, stale telemetry and over-envelope growth use the existing
temporary_pressure category and existing evidence-aware hysteresis. Invalid
identity/telemetry remains a conservative device fault. CPU result gates also
quarantine invalid identity/telemetry through their common outcome mapping.
No new automatic retry, timer-only rediscovery or driver cleanup is introduced.
All private diagnostic stage numbers 1..31 remain unchanged.

## Wire and storage compatibility

No on-disk storage format, decoder or kernel task ABI changes.
The historical FLHMEM01 diagnostic remains exactly 256 bytes and deliberately
does not serialize the added host fields. Decoding that observation leaves those
fields zero, so it cannot become sufficient admission evidence. It remains
observation-only metadata and never a GPU data reply. An explicit budget-only
probe prints the new host fields and computed allowance; it initializes no HIP.

## Required qualification

- Portable oracle cases for independently calculated reserves and exact growth
  boundaries, doubled capacity, falling physical/commit capacity, missing fields,
  overflows, original-baseline accumulation and changed identity.
- Worker phase/category tests and exact diagnostic-wire golden bytes; the
  historical observation must fail the new complete admission predicate.
- Windows CPU-semantic match/controller/transport tests with bounded accepted
  growth and over-envelope rejection, terminal CPU fallback and exact retirement.
- Raw-only, codec, sanitizers, Windows native and current complete ROCm SDK lanes.
- Only after exact-source qualification and harness audit: one tiny synthetic
  actual-runtime trial, then separate native-compression integration and speed
  measurements. Do not repeat the old rejected experiment unchanged.

At initial publication these new tests are **not yet executed**. Existing
research workflow 43005349149 passed all five current-source captures; that is
research evidence, not implementation qualification. D01's intermittent hosted
termination failure remains open and is not repaired by this policy.

## Current official source evidence

The isolated Crawl4AI capture is
[evidence/gpu-resource-research-a8fa579.json](evidence/gpu-resource-research-a8fa579.json).
It retains provider/version, robots checks, exact URLs, status, digests and
publisher dates. The API pages are currently served contracts with older
rendered revisions; they are not described as newly published research.

- [GlobalMemoryStatusEx](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-globalmemorystatusex)
- [MEMORYSTATUSEX](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex)
- [DXGI memory information](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info)

Related contracts: HIP_BATCH_PLAN.md, GPU_RECOVERY.md, WORKER_CHANNEL.md,
GPU_MEMORY_OBSERVATION.md and GPU_MATCH_CONTROLLER.md.

## First exact-source qualification checkpoint

Implementation 04d884be80dbd96024bbdb6cf0d79b5056cb0da4 passed 113 CTest groups
in each Linux raw-only and codec lane (jobs 130476762346 and 130476762479, run
43006974090). Windows, sanitizers and SDK qualification remain pending at this
checkpoint; no owner GPU execution follows from these CPU results.

Historical regression run 43006973709 failed during compilation, because it
compiled the updated current fixture against the f3ab1c5 snapshot type, which
does not contain the newer nonlocal-budget/host fields. This was a fixture ABI
mismatch, not the required old behavior failure. The harness now pins the prior
test fixture to a8fa5798c52848ad706fa0883a0844e0fbb2e127 for the historical
library. It still requires exactly the original failure message and exit 1.
The corrected/current build uses today's full recovery suite, which retains
the same streak-interruption regression and the new resource contract.
No product check or expected historical failure is weakened. Matching corrected
workflow evidence is still required.
