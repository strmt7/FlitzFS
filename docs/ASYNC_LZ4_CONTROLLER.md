# Asynchronous compression controller — experimental

The compression controller owns immutable CPU source bytes independently of its
one supervised worker. It reuses the existing worker session, protected local
transport, creation-time process budgets, request identity generator and
evidence-aware recovery policy. It adds no service, resident worker, driver,
discovery loop, normal filesystem caller or owner execution permission.

An explicit begin requires consent, separately qualified useful workload,
qualified helper/configuration identity, fresh compatible resource evidence,
complete device-slab accounting and an independent CPU verifier. A missing
decoder refuses before any worker starts. Overlapping waiting requests do not
consume caller input. Result deadlines retain the existing 1–100 ms range;
that is an experiment boundary, not a claim that cold GPU startup can satisfy it.

CPU selection is terminal and immediately exposes the immutable CPU source; it
does not wait, query a process, signal cancellation or call a runtime. Selected
worker candidates cannot overwrite it later. Cancellation is a separate signal
and exact child/supervisor retirement remains on the bounded transport.

A new CPU-only request may proceed while the old worker retires. The controller
retains at most that old batch and one new CPU batch. No replacement GPU request
is allowed until exact quiescence. Late immutable replies are classified once,
independently decoded, and cannot change selected bytes or forget a corrupt
configuration. Runtime absence blocks that configuration without repeated
search. Driver faults require distinct successful recovery evidence, not elapsed
time. Temporary pressure uses the existing distinct-healthy-evidence hysteresis.
No local model or heuristic can override integrity, memory or ownership gates.

Accepted candidates are independent LZ4 blocks in request order. They may be
non-saving; acceptance proves exact source recovery, not useful compression or
publication. The native storage encoder must still compare them with the exact
merged extent and apply its own nonexpansion, ownership and commit protocol.
Source and candidate spans expire at the next nonoverlapping begin. The
controller must outlive all such spans and remain on one serialized control
thread.

The tests use the same trusted test binary and CPU-only child modes.
They cover missing-decoder refusal, complete-slab admission, disabled/unqualified
work, moved-from input, immutable source ownership, overlapping begins, terminal
selection, corrupt/late replies, unavailable helper, live-child CPU selection,
cancellation, deadlines, resource transitions, exact retirement and qualified
recovery. A fake HIP execution tag exercises policy only and proves no GPU work.

The source-bound controller qualified at f08e6b8: all 13 core jobs and full SDK
qualification passed. The retained evidence is
[evidence/asynchronous-lz4-f08e6b8.json](evidence/asynchronous-lz4-f08e6b8.json).
These are CPU-only controller tests, not actual GPU execution. It does not implement ordinary
Windows write routing, lifecycle amortization or measured GPU benefit. D01 remains
open and a resident worker still needs a separately reviewed lifecycle contract.
