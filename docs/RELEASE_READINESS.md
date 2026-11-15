# Release readiness checkpoint — 15 November 2026

FlitzFS remains an experimental filesystem/storage prototype, substantially short
of a release candidate. The owner alone decides when to release. No release,
tag, package publication or version change is authorized.

This assessment separates implemented mechanisms, executed qualification and
unmet product goals. A passing fixture or large assertion count is not a
completion percentage. There is no defensible release date yet: ordinary
accelerated operation, scalable storage and the central collective-compression
mechanism still need implementation and measurement.

## Demonstrated progress

The latest qualified hardware source is 94aaa2a772e03689044df4c0b7504903faa9d553.
All thirteen core jobs and the complete selected ROCm SDK gate passed. One bounded
synthetic operation GPU-encoded 1,048,576 input bytes into 135,344 candidate bytes
through the owned deferred write service. The stored encoded bytes matched the
captured advice, and independent CPU recovery verified both current and prior
data. The service sent one injected response, joined its control thread, confirmed
transport retirement and released all locked memory.
[Source-bound evidence](evidence/owned-deferred-gpu-94aaa2a.json)

The request scope took 641 ms and complete process execution took 728 ms. The
complete image occupied 1,310,720 bytes, including metadata, alignment and retained
history. It exceeded the input size. This is **not** a demonstrated whole-image
size or end-to-end speed advantage. The operation was unmounted and used an
injected response sink; real GPU SendResponse/Explorer delivery remains unproven.
The hosted CPU fixture's timing is not a matched same-host comparison.

The previous non-reusing sweep exited with 0xc0000409; CPU fallback and recovery
passed. The successful replacement does not establish that historical failure's
cause. Keep the failed evidence and do not claim a proven memory-cap diagnosis.
[Failure evidence](evidence/uniform-sweep-847423f-failure.json)

The new finite-sweep observation round adds source/adapter/grant-bound timing
of setup, each serialized work phase, slowest kernel/quantum and final retirement.
The instrumented source passed its complete isolated gates and one bounded
hardware operation. Setup took 423,295,500 ns of the 467,675,900-ns measured
compute scope; accumulated kernel/full-wait time was 21,927,100 ns. These are one
sample's host wall measurements, not GPU-event timing or a reusable cost bound.
The result prioritizes safe amortization and complete Windows integration over
kernel-only tuning. Automatic qualified costs and ordinary GPU activation remain
unimplemented. That earlier source-11fbf07 profiling remains useful diagnosis,
not a reusable grant or a substitute for the new service-composition evidence.

## This implementation round

The bridge, first qualified at a4ad5166d5063d820cfdd704a600ea20a23f1d93, implements one owned deferred write in the actual SDK callback
path. It owns immutable input and request identity, serializes coordinator access,
orders following callbacks through CPU publication, and joins response ownership
before filesystem destruction. All 13 core jobs, 154 native test groups and the full ROCm SDK gate passed.
The 54 deferred checks include owned input, exact response, held-worker CPU
fallback and concurrent shutdown. The real CPU-only pending-response mount
passed with independent current/prior recovery and complete cleanup.
[Executed source-bound evidence](evidence/owned-deferred-a4ad516.json)

Source 94aaa2a adds driver-independent control composition and optional encoded
advice receipts. Its 154 native groups include 57 deferred and 226 controlled
checks; all thirteen core jobs and all fifty prescribed SDK commands passed.
The new owned-service CPU fixture and one actual unmounted GPU case passed.
CPU-only callback/mounted tests and the injected GPU response are separate scopes.
Automatic helper/cost acquisition, cross-operation batching and ordinary GPU SDK
delivery remain unfinished. The first source 7ef0dd3 failed Windows compilation; its missing include
and SDK variable-tail initialization errors were corrected without suppressions.
[Failure record](evidence/owned-deferred-7ef0dd3-build-failure.json)

This is useful integration progress, but does not close any of the three largest
product exits: ordinary useful automatic GPU I/O, scalable populated storage and measured
collective compression. It does not justify a calendar estimate or percentage.
[Ownership and qualification contract](OWNED_DEFERRED_WRITES.md)

## Product gaps and release gates

| Area | Current position | Required exit |
|---|---|---|
| Ordinary Windows GPU I/O | One unmounted GPU operation through the owned service passed; real CPU-only mounted responses passed separately; automatic activation remains absent | Explorer/Win32 writes use admitted GPU work automatically when useful; reads and fallback work with GPU absent; cancellation and shutdown retire exactly |
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
