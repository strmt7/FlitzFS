# Release readiness checkpoint — 15 November 2026

FlitzFS remains an experimental filesystem/storage prototype, substantially short
of a release candidate. The owner alone decides when to release. No release,
tag, package publication or version change is authorized.

[The current release execution plan](RELEASE_EXECUTION_PLAN.md) breaks the
remaining work into thirteen concrete packages, about 60–130 person-weeks of
low-confidence planning effort. It includes multi-release compatibility,
offline coherence, genuinely background maintenance and recognized benchmarks.
The research exit and exact release date remain unproved; no completion
percentage follows from code size, assertion counts or a million object keys.

This assessment separates implemented mechanisms, executed qualification and
unmet product goals. A passing fixture or large assertion count is not a
completion percentage. There is no defensible release date yet: ordinary
accelerated operation, scalable storage and the central collective-compression
mechanism still need implementation and measurement.

## Demonstrated progress

The explicit [paged native backend](NATIVE_PAGED_BACKEND.md) connects native
operations and sessions to paged object storage through the shared engine.
Native metadata is still a complete bounded flat Model; scalable names/extents,
native-aware migration/handoff and Windows factory integration remain open.
Conditional owner permission for a qualified FlitzFS-related driver does not
qualify an experimental driver or authorize GPU/unrelated driver changes.

The latest qualified hardware source is d384b6c20cf686520e3a94c156e532307ca0fbf8.
All thirteen core jobs and all 53 prescribed selected-SDK commands passed before
one predeclared CPU/GPU, GPU/CPU, CPU/GPU owner comparison. Each request began
with new identical storage and the same fingerprinted 1,048,576-byte source.
All three GPU requests produced and stored verified candidates. All six requests
passed independent current/prior CPU readback, exact injected response ownership,
joined service threads, confirmed transport retirement and zero final locks.
[Complete source-bound comparison](evidence/owned-service-comparison-d384b6c.json)

Median owned request time was 24.2311 ms for CPU and 513.918 ms for GPU; ranges
were 20.0793–28.1673 ms and 505.7415–713.1048 ms respectively. All six complete
images occupied 1,310,720 bytes, with 135,648 active payload bytes each. The image
exceeded input size. This fixed case showed **no speed or stored-size benefit**.
The request scope includes enqueue/input copying through service join, excluding
volume setup and final independent readback. Three trials per backend, one
synthetic source and cold workers do not establish general filesystem performance
or reusable production costs. Memory peaks are cumulative across the shared job.
The operations were unmounted and used an injected response sink; real GPU
SendResponse/Explorer delivery remains unproven. The postflight found no named
FlitzFS processes, unchanged runtime bytes and GPU status OK; its bounded log
query does not prove absence of every possible driver fault.

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

The explicit CPU comparison mode at 94af72c passed all thirteen core jobs, all
52 prescribed SDK commands and one bounded owner CPU case, with independent
current/prior readback and complete retirement. Its coarse request clock returned
0/16 ms, so those values are not precise speed measurements. Higher-resolution
request timing and source fingerprints subsequently passed at d384b6c and were
used for the comparison above. [CPU-mode evidence](evidence/owned-deferred-cpu-94af72c.json).

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

The local implementation round adds explicit exact shared extents with private
updates, legacy-format rejection and independent CPU reads. The existing native
suite and new sharing suite pass in raw-only Release and MSVC AddressSanitizer
configurations. Two equal 64-KiB files occupy a 393,216-byte freshly compacted
image versus 458,752 bytes for the unshared control. That complete-image saving
is specific to this duplicate fixture; both images exceed the 131,072-byte input.
It does not demonstrate learned/extreme compression or a GPU benefit.
[Local scope and retained failures](evidence/local-native-shared-minimum-2026-11-15.json).

The required scale is millions of files with persistent incremental scanning
and paged metadata/content indices. Groups may contain one, two or thousands of
files, with separately indexed retrieval units and practical measured latency.
The current flat-store limits still need replacement. These requirements keep
the project substantially short of release readiness.

The separate [paged storage API](PAGED_OBJECT_STORE.md) now passes three selected
local raw/AddressSanitizer suites and a one-million-populated-object experiment.
It has real writes, lookup, pagination, deletion and streaming rebuild; opening
that million-object image reads 16 KiB. This closes a storage-foundation step,
not the native namespace/Explorer scale exit. The synthetic 16-byte records
occupy a 434,147,328-byte image, so no compression or filesystem-speed benefit is
inferred. [Exact evidence](evidence/local-paged-store-2026-11-15.json).

| Area | Current position | Required exit |
|---|---|---|
| Ordinary Windows GPU I/O | Three unmounted GPU requests in a matched six-request series passed; real CPU-only mounted responses passed separately; automatic activation remains absent | Explorer/Win32 writes use admitted GPU work automatically when useful; reads and fallback work with GPU absent; cancellation and shutdown retire exactly |
| GPU benefit | Actual bounded GPU bytes and CPU recovery demonstrated | Repeated controlled comparisons on varied real data show useful complete-operation throughput and/or net stored-size benefit |
| Capacity | Opt-in paged store tested with one million objects; native namespace retains 256 nodes/about 15.94 MiB populated extents/64 MiB image limits | Integrate paged inode/name/extent transactions, bounded caches, migration and reader-safe reclamation; qualify populated named files |
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

The measured small-request route must not be enabled merely because a GPU exists.
The next implementation must reduce setup per useful byte through a finite
amortized lifecycle and implement protected cross-file compression with an
independent CPU reader. Advance scalable indices and automatic Windows routing
alongside those mechanisms. Setup amortization alone cannot create a storage
gain when CPU and GPU choose equally sized representations. Acquire only the
measurements needed to choose the next implementation; avoid unchanged reruns,
indefinite LZ4 tuning or disconnected capability probes. Keep the
[implementation plan](DEVELOPMENT_PLAN.md) and its dependency ledger current.

Before a credible release estimate, demonstrate all three: useful ordinary GPU
I/O, bounded scalable populated storage, and a measured cross-file compression
advantage. After those, durability, Windows/security qualification and comparative
testing remain substantial work. Superior compression is an experimental goal,
not an established property of the current code.

No test establishes survival of arbitrary bugs or destruction of every copy.
Specify the covered fault model and retain enough independent data/dependencies
to recover under it; detection alone is not repair.
