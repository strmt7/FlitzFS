# Release readiness checkpoint — 14 November 2026

FlitzFS remains an experimental filesystem/storage prototype, substantially short
of a release candidate. The owner alone decides when to release. No release,
tag, package publication or version change is authorized.

This assessment separates implemented mechanisms, executed qualification and
unmet product goals. A passing fixture or large assertion count is not a
completion percentage. There is no defensible release date yet: ordinary
accelerated operation, scalable storage and the central collective-compression
mechanism still need implementation and measurement.

## Demonstrated progress

The latest qualified hardware source is d33efd64d94427f5b6c9f2e269520d4ba5e26bcd.
Its full 13-job core matrix and complete selected ROCm SDK qualification passed.
One explicit, finite synthetic operation GPU-encoded 1,048,576 input bytes into
135,344 payload bytes, stored the accepted GPU representation through the native
storage path, and recovered both current and prior data through the independent
CPU reader. Exact worker/supervisor retirement, zero remaining locked bytes and
restored working-set limits were checked. It was unmounted.
[Source-bound evidence](evidence/uniform-reuse-d33efd6.json)

That exchange took 626,486,100 ns. The complete image occupied 1,114,112 bytes,
including fixed metadata, alignment and retained history. This is **not** a
demonstrated whole-image size or end-to-end speed advantage. The CPU fixture used
a manually constructed alternate representation, not a competitive optimized CPU
compressor. Neither payload difference nor unmatched timings prove superiority.

The previous non-reusing sweep exited with 0xc0000409; CPU fallback and recovery
passed. The successful replacement does not establish that historical failure's
cause. Keep the failed evidence and do not claim a proven memory-cap diagnosis.
[Failure evidence](evidence/uniform-sweep-847423f-failure.json)

The new finite-sweep observation round adds source/adapter/grant-bound timing
of setup, each serialized work phase, slowest kernel/quantum and final retirement.
Its exact-source qualification is retained in the state/validation ledger; no
owner execution of the new instrumented source is claimed in this assessment. It does not automatically produce
qualified costs or enable an ordinary Windows callback route.

## Product gaps and release gates

| Area | Current position | Required exit |
|---|---|---|
| Ordinary Windows GPU I/O | Controlled native advice operation exists; normal callbacks remain CPU-only | Explorer/Win32 writes use admitted GPU work automatically when useful; reads and fallback work with GPU absent; cancellation and shutdown retire exactly |
| GPU benefit | Actual bounded GPU bytes and CPU recovery demonstrated | Repeated controlled comparisons on varied real data show useful complete-operation throughput and/or net stored-size benefit |
| Capacity | 256 objects/nodes, about 15.94 MiB populated native extents, 64 MiB image bound remain in current implementation | Paged indices, allocation and bounded reclamation handle growing file trees/data without whole-tree residency or merely larger constants |
| Extreme collective compression | Research/design objective; no product cross-file learned codec | A lossless cross-file representation produces substantial held-out TOTAL-size gains, with practical individually addressed reads and protected dependencies |
| Resource adaptation | Live admission, resource-sized layouts/planner and terminal recovery exist | Automatic configuration/shape cost acquisition, contention-aware selection and finite low-priority work; no host-specific product defaults |
| Learning and maintenance | Persistent advisory-state and explicit recompression/handoff primitives exist | Workload learning actually drives automatic grouping/codec/layout choices and resumes after restart while promptly yielding to foreground work |
| Durability and repair | Copy-on-write checkpoints, redundancy for selected metadata and process/fault fixtures exist | Explicit acknowledged-write contract, data/dependency redundancy, recovery/repair and fault campaigns cover the promised backend failure model |
| Windows product | Native callbacks, helper/app/tray and disposable-VM writable probes exist | Complete promised Win32 semantics, Windows 11 client qualification, lifecycle, packaging, secure installation/update and supported volume-creation workflows |
| Security and comparisons | Source/dependency inventories and bounded-input checks exist | Independent review, malformed-input/fuzz campaigns, reproducible filesystem comparisons, supply-chain/installer review and exact-build acceptance |

Capacity values above were rechecked in current object/native headers. A large
sparse logical address range is not populated capacity. SDK and mounted hosted
Windows Server fixtures are not qualification on the latest Windows 11 client.
RAM-backed interruption tests do not prove physical power-loss durability.

CPU fallback may encode with a different codec and produce a different size.
It must continue operations and independently decode every admitted GPU-stored
format and protected dictionary/model/base. CPU/GPU compressed-stream equality
is not a goal. Recovery compares decoded source bytes.

## Direction and revised emphasis

The ownership, transaction, containment and independent-reader foundations are
useful. Delivery has nevertheless accumulated many isolated stages while the
main user-visible GPU path and the distinctive compression engine remain absent.
Further assertion counts and documentation alone will not close that gap.

The next work must deliver a complete automatic Windows/GPU/native-write/read/
fallback slice, then a scalable store and a real collective-compression slice.
Acquire only the measurements needed to make those decisions; avoid indefinite
LZ4 tuning or another sequence of disconnected capability probes. Keep the
[implementation plan](DEVELOPMENT_PLAN.md) and its dependency ledger current.

Before a credible release estimate, demonstrate all three: useful ordinary GPU
I/O, bounded scalable populated storage, and a measured cross-file compression
advantage. After those, durability, Windows/security qualification and comparative
testing remain substantial work. Superior compression is an experimental goal,
not an established property of the current code.

No test establishes survival of arbitrary bugs or destruction of every copy.
Specify the covered fault model and retain enough independent data/dependencies
to recover under it; detection alone is not repair.
