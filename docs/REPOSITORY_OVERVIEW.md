# FlitzFS repository overview

Reviewed on **17 November 2026** at
[`38a4a48f005c2ea6c9a7f8cbd9d7ee07df044d70`](https://github.com/strmt7/FlitzFS/commit/38a4a48f005c2ea6c9a7f8cbd9d7ee07df044d70).
This is a repository-wide architectural and delivery review, not an exhaustive
security audit or a new full-suite, mounted-filesystem or GPU qualification.
The [detailed implementation plan](IMPLEMENTATION_PLAN.md) is the forward plan.
[Review receipts](evidence/repository-review-2026-11-17.json) bind inventories,
semantic searches, inspected sources, current research and API captures.
Later checkpoints are recorded under recent executed evidence and in the
[development state ledger](DEVELOPMENT_STATE.json).

## Assessment

FlitzFS has a substantial experimental storage foundation and several working
Windows integration components. It is **not close to a stable release candidate**.
The main problem is no longer a lack of safety mechanisms or diagnostic fixtures.
It is the gap between those mechanisms and a complete, scalable, automatically
optimized filesystem that ordinary Windows applications can use.

Keep the shared CPU-readable engine, copy-on-write publication, protected
decoding dependencies, WinFsp integration and isolated GPU execution. Replace
the assumptions that make current production paths depend on small volumes,
append ordering, full-image copies and a supplied GPU routing decision. The
next milestones must exercise complete application operations and account for
all occupied bytes.

There is no evidence yet that FlitzFS beats modern filesystems or the strongest
applicable compression methods on representative held-out data. That remains a
research and engineering objective. The full owner goal remains active; no
release, tag, publication or product-version change is authorized by this review.

## What is in the repository

The clean reviewed checkout contains 697 tracked files, about 9.65 MB in total.
The `src/` and `include/` semantic inventory contains 152 code files, about
1.13 MB. These are inventory measurements, not measures of completion.

| Area | Inventory | Responsibility |
| --- | ---: | --- |
| `src/` | 117 files | Storage, native filesystem, codecs, Windows adapters, controllers and workers |
| `include/` | 36 files | Public core, session, storage, resource and recovery interfaces |
| `tests/` | 138 files | Model, malformed-input, failure, SDK, process, application and qualification fixtures |
| `tools/` | 57 files | Dependency staging, provenance, resource containment and qualification |
| `docs/` | 297 files | Contracts, plans, historical results and source-bound evidence |
| `.github/` | 19 files | Complementary build, analysis and experimental workflows |
| `third_party/` | 18 files | Pinned BLAKE3 sources, licensing and provenance |

The current raw/ASan SDK build configurations each register **153 CTest entries**.
Only inventory enumeration was performed for that count; it is not a statement
that all 153 were executed in this review. Recent executed checks are described
separately below.

## Current architecture

```mermaid
flowchart TD
  GUI[Experimental helper and tray] --> VM[VolumeModel]
  WC[WinFspMemoryVolume composition] --> CT[Owned WinFsp controller]
  CT --> SDK[Writable WinFsp callbacks]
  VM --> S[NativeFileSession]
  SDK --> S
  S --> N[Native filesystem writer and snapshots]
  N --> P[Paged or legacy flat object storage]
  S --> H[Typed volume head]
  S --> A[Reserved advisory state]
  VM --> B[Shared adaptive locked-memory factory]
  WC --> B
  S --> G[CPU shared-base group maintenance]
  D[Explicit deferred GPU write service] -. admitted candidates .-> S
  R[Independent CPU reader] --> H
  H --> P
```

The indexed WinFsp composition owns its memory backing and one controller
through creation, callback service, retirement, compaction and CPU inspection.
Local SDK tests exercise lifecycle and resource decisions. An earlier linked
provider checkpoint also passed external mounted I/O on Windows Server 2025.
GPU-enabled indexed writes have used the AMD device in a controlled callback
fixture. Automatic dispatch and mounted performance remain open.

The diagnostic CLI operates on experimental image/container formats. It is not
the final user-facing mount, creation or management interface. Historical ZIP
compatibility work was retired; it is not a product requirement or a proposed
filesystem format.

## Architecture decisions after reconsideration

| Boundary | Decision for the successor | Reason and first proof |
| --- | --- | --- |
| Windows namespace | Keep WinFsp and one engine behind Explorer, Win32 and the helper. Prototype the supported installed driver/runtime binding before considering a custom driver. | The SDK callbacks already cover substantial behavior. Measure the bridge with a memory reference and qualify the actual mounted lifecycle before attributing latency to the driver. |
| Authority and formats | Keep CPU-readable immutable roots and current readers. Introduce a separately negotiated capability for new indexes and reusable allocation rather than reinterpret old references. | The current child and payload ordering checks rely on append offsets. New readers need explicit generation, lifetime and range proofs before recycling them. |
| Metadata | Replace whole-model authority with ordered directory and extent pages, stable inode identities and a small per-transaction overlay. | An object index keyed by integers alone does not supply ordered parent/name and inode/range access. Prove bounded point and range costs with real populated files. |
| Space and repair | Use verified full-image rebuild for bounded migration, recovery and initial handoff. Design incremental segment evacuation for steady operation. | A full copy requires another image and any current foreground commit invalidates it. Sustained low-rate writes must still allow useful reclamation. |
| Data protection | Retain independent verification and recovery, compare record packing and redundancy layouts under equivalent failure assumptions. | With a 64 KiB failure unit, even a single small mirrored payload and its metadata can allocate at least 256 KiB before other costs. Compaction alone cannot turn that transaction geometry into high small-file efficiency. |
| Foreground and GPU | Keep CPU completion independent. Use measured automatic dispatch and compare accelerator roles at complete-operation cost. | Kernel throughput alone cannot overcome transfers, setup, verification and publication. Some GPU work may earn its budget through a smaller settled volume. |
| Strong compression | Keep shared-base coding as a control. Compete with bounded multi-reference, dictionary, reversible transform and learned candidates across content families. | The winning method can depend on data, protection cost and retrieval work. A versioned CPU decoder and held-out complete-size results decide adoption. |
| Learning and scheduling | Separate advisory models/coherence from required decoding objects. Replace prototype thresholds with measured yielding, budgeted exploration and persistent sparse progress. | The volume stays readable after advice loss. Background compute and writes need demonstrated useful savings and measured foreground impact. |
| Physical storage and upgrades | Keep the operation contract shared. Qualify each provider and explicit format migration, including interrupted upgrade and rollback. | A RAM volume and an append-only file model have different failure domains. Recovery must refer to a specified acknowledged state. |
| Tooling, tests and docs | Keep reproducible private dependencies, meaningful independent oracles and immutable historical evidence. Consolidate current decisions in this review, the plan and its index. | Many old checkpoints still describe earlier scopes. They inform regressions and research without imposing obsolete host restrictions or closing current product gates. |

The protected metadata path is worth preserving as a design pattern. Different
storage geometry, shorter publication paths or less redundancy require separate
capacity and failure evidence before replacing it.

## Implemented behavior and practical limits

| Component | Implemented and supported by evidence | Material remaining gap |
| --- | --- | --- |
| Object storage | Flat and paged stores, checked COW transactions, replicated metadata, streaming rebuild and bounded indexed selector publication | Durable segment allocation, reclamation under concurrent writes and backend qualification |
| Native namespace | Indexed path and handle transactions, typed head selection, mounted Server 2025 fixture, scheduled retirement and staged memory compaction | Windows 11 client semantics, large live truncation and automatic indexed collective work |
| Populated capacity | Million-object paged inventory and 1,024 named nonempty indexed files. The indexed writer bypasses the older fixed namespace and extent model. | Larger populated external-client runs, root/open recovery budgets and scalable collective indexing |
| Compression | Optional CPU Zstd/LZ4, exact sharing and independently readable indexed shared-base literal-patch members | Stronger shifted/multi-reference/type-aware/learned collective representations and representative complete-size results |
| Background work | Aligned-family discovery in the legacy engine, live Windows admission and bounded automatic indexed memory reclamation | Scalable persistent features, restartable learning, workload-driven codec selection and a lifetime write governor |
| CPU recovery | Independent parsers/decoders, typed-head validation, surviving-copy reads and source-preserving repair/rebuild primitives | Full backend failure qualification, repair after wider dependency damage and production recovery orchestration |
| GPU | Bounded AMD execution through the indexed WinFsp SDK callback, exact mirrored output and independent CPU reopen | Automatic selection and measurable benefit in mounted workloads, plus stronger collective GPU work |
| Windows interface | Writable native SDK callbacks, owner ACL/time initialization, contexts, cleanup/close, metadata/security and directory operations | Current-source mounted product qualification, complete Unicode/path/Win32 behavior, caching/mapping/concurrency and deployment |
| Application | Native helper/tray, create/save/read/optimize/discard, owned worker and GUI qualification | Explorer-facing mounted lifecycle, complete volume-management workflows and a supported installer |
| Memory composition | Live adaptive admission beyond the old ceiling, on-demand page locking, retained backing through stop and explicit-join CPU inspection | Larger populated volumes, multiple-volume aggregate policy and broker/process-failure behavior |
| Indexed volume files | Exclusive create and verified reopen, persistent head and advice, CPU inspection, WinFsp callbacks and verified paired-range reclamation | User-facing selection, incremental reclamation during concurrent edits and actual media durability qualification |
| Learning state | Budgeted scan checkpoints, a paged coherence catalog and stale-posting compaction in owned indexed memory volumes | Persistent provider, large-scale validation and workload outcomes for a complete autonomous learner |
| Compatibility | Several historical native encodings and explicit capabilities remain readable | A published support window, mixed-version migration program, rollback boundaries and exact released-reader matrix |

### The important namespace bottleneck

The Windows session's legacy writer keeps all nodes, names and per-file extents
in `Model`. Its scoped inode-record queries verify the touched path and records.
Mutation staging still copies the model and compares namespace records against
the backing. The indexed writer stages affected inodes and ranges against the
paged object tree and publishes names, extents and counters together.

A 17-byte replacement in the indexed 1,024-file fixture read 202,372 backing
bytes. Full scrub read 291,765,432. The multipage format removes whole-namespace
work from that mutation and leaves considerable overhead in the nested indexes.
The indexed head and lifetimes now reach the owned Windows provider.
The controller consumes closed-file retirement in bounded transactions.
The next effort is physical reuse, bounded live truncation and lower
metadata read/write amplification with independent verification.
[Indexed writer evidence](evidence/native-indexed-writer-2026-11-18.json)

Authoritative source: [native limits and interfaces](../include/flitzfs/native_filesystem.hpp),
[`Model`, namespace decode/matching and staging](../src/native_filesystem.cpp),
[paged object API](../include/flitzfs/paged_store.hpp).

### The important compression bottleneck

The current group format stores an immutable 64-KiB base. Indexed members can
use literal patches, anchor copies, packed XOR planes or modular deltas. CPU
decoding verifies both the base and reconstructed member digests. Arbitrary
reordering and learned grouping and transforms remain open.

Discovery uses digests and band features. The indexed scanner extends aligned
features with shifted samples and restores verified feature bindings from
budgeted checkpoints. Its sparse catalog supplies point nominations outside the
current window. Full catalogs can drop stale postings in a verified compacted
copy. Complete retained outcome policy remains open. The current representation
has one-level base dependencies.
More complex representations require explicit dependency, access-cost and
recovery designs.

Publishing a group also appends history. The maintenance counters distinguish
hypothetical compacted savings from retained history added. A smaller live
representation can therefore coexist with a larger current image. If a group
cannot fit, the owner releases its staged batch and considers measured append
shortfall when scheduling a verified rebuild. A foreground commit still
invalidates the staged rebuild even if it touched unrelated files. Develop a
reclaimer that makes incremental progress across concurrent edits. Evaluate
compression using **total occupied bytes after safe reclamation**.

The current mirrored paged format pairs 64 KiB units on the memory provider.
For a commit adding one small nonempty payload, it seals a paired payload bank
and allocates at least one paired metadata bank. This is a lower bound of
256 KiB of new layout space, excluding any alignment gap. The reclaimer must
address packing and short-transaction amplification as well as stale records.
A smaller failure unit requires backend evidence, especially on persistent
storage. Child/page ordering also forbids simply substituting arbitrary free
offsets into the existing format.

Source: [group encoder/decoder](../src/native_group_codec.cpp),
[automatic discovery](../src/native_maintenance.cpp),
[shared-base contract](SHARED_BASE_GROUPS.md),
[staged rebuild](STAGED_PAGED_REBUILD.md).

### The important GPU bottleneck

The explicit native deferred service already supports CPU selection and a single
response while separately tracking later worker retirement. That mechanism
should be reused, not rebuilt. Normal product factories do not configure it
automatically, and its health/cost/binding inputs still need product acquisition.

The last matched six-request observation at `d384b6c` used 1 MiB per trial:
CPU median 24.2311 ms, GPU median 513.918 ms, and identical 1,310,720-byte images.
It demonstrated actual GPU work and CPU recovery; it demonstrated no speed or
complete-size advantage in that case. It was unmounted and is not a benchmark
of today's paged factory, a different corpus or every AMD GPU.

GPU work remains required. The next objective is a useful automatic route and
more valuable collective work, with complete-operation cost and size accounting.
CPU fallback may encode differently and produce a different size; every admitted
GPU-produced representation still needs an independent CPU decoder.

Source: [deferred service](../src/windows/native_deferred_write.cpp),
[controlled native writes](../src/windows/native_uniform_write.cpp),
[matched observation](evidence/owned-service-comparison-d384b6c.json).

## Main risks and inefficiencies

These priorities describe engineering and release risk, not newly proven
security vulnerabilities.

| Priority | Finding | Consequence and required response |
| --- | --- | --- |
| Critical | Native namespace/extents remain globally bounded and resident | Ordinary populated files and large directories hit prototype limits. Replace the access model rather than enlarging constants. |
| Critical | No automatic reclamation closes the compression transaction | Compacted savings do not ensure smaller occupied storage. Qualify bounded handoff and implement segment-level reclamation that survives unrelated foreground commits. |
| High | Current append-order format and failure-unit padding limit useful space | Reusing extents changes the proof of record identity and graph validity. Measure occupied bytes and writes by record class and introduce explicit reusable-allocation semantics. |
| Critical | Ordinary GPU use and benefit remain absent | Explicit requests are not the requested product. Add automatic qualified discovery/calibration/routing and a beneficial collective backend. |
| Critical | Physical durability is not qualified | `StoreIoContract` describes assumptions; RAM ordering and modeled faults do not attest to a drive's power-loss behavior. |
| High | Global callback serialization and synchronous publication | Foreground requests can wait for an in-flight quantum/commit. Add immutable read views, bounded concurrency and measured interference. |
| High | Full namespace validation and full-image opening/scrubbing costs | Startup/read/maintenance work will not scale by carrying today's checks unchanged. Replace them with checked tree paths and scheduled scrubbing, without removing integrity validation. |
| High | Current Windows name profile is ASCII and short | Many normal Windows workloads are unsupported. Implement a specified Unicode/case/path model and test ordinary clients. |
| High | Incomplete persistent learning | Indexed checkpoints retain features and scan progress. A persistent-media catalog, scale qualification and learned strategy across workloads remain open. |
| High | Quiet-work policy remains an initial fixed policy | A 12.5% busy threshold and 1% elapsed pacing are mechanisms, not proof of optimal host adaptation or low interference. |
| High | Driver/runtime qualification and historical retirement faults remain open | A valid signature or later clean fake-lifecycle test does not explain earlier failures or qualify a new native combination. |
| High | Security claims exceed current mechanisms if read too broadly | Checksums are not authentication; owner ACLs are not volume encryption. No full hostile-media/parser/update assessment is established here. |
| Medium | Partial codec-enabled qualification | The pinned Windows codec lane covers the paged store and SDK callbacks. Complete current-source application, mounted and recovery-package coverage remain open. |
| Medium | Process-lifetime RAM ownership | Stop retention now works; last-owner/process loss still loses temporary contents. Broker and persistent-provider policies must be explicit. |
| Medium | Evidence/documentation fan-out | Current and historical descriptions drift across large ledgers. Use one current requirement/exit map and immutable evidence references. |
| Medium | Many automatic hosted workflows remain | Local qualification is primary. Reduce redundant heavy runs and reserve remote/isolation resources for a stated coverage need. |

The callback surface does not currently expose a complete implementation of all
Win32 filesystem features: for example, its volume-label setter rejects the
operation, POSIX unlink/rename support is disabled, and stream/reparse behavior
must not be inferred from the callback table. Features must be implemented or
explicitly reported unsupported, with a published compatibility contract.

Source: [WinFsp callback table and parameters](../src/windows/winfsp_native.cpp),
[session release ordering](../src/windows/winfsp_session.cpp),
[controller](../src/windows/winfsp_controller.cpp),
[backend contract](../include/flitzfs/object_store.hpp).

## Fresh dependency and alert check

Authenticated GitHub REST capture on 17 November returned **HTTP 200 and zero
open Dependabot alerts**, with complete cursor pagination. The initial review
request incorrectly supplied `page` and received HTTP 400; it was corrected.
This is a current successful API check, not an inference from the old 403 or a
claim that every C++/runtime dependency is vulnerability-free.

| Dependency/tool | Repository selection | Fresh official result / qualification limit |
| --- | --- | --- |
| Zstandard | v1.5.7 pin | Still the latest stable release returned by the API |
| LZ4 | v1.11.1 pin | Latest stable, published 10 November 2026 |
| BLAKE3 | 1.8.7 pin | Latest stable returned by the API |
| WinFsp | v2.2B5 development SDK | Latest available prerelease; stable endpoint still returns v2.1. This does not qualify an installed driver. |
| ROCm Core SDK | 10.1.0 package pin | Current AMD Core documentation is 10.1.0; its HIP component is 7.16.0. Do not substitute the legacy Windows HIP SDK 7.2 documentation or equate package/component version numbers. |
| CMake | Private staging recipe 4.5.0 | Latest stable API result is 4.5.0. It was checksum-verified and used in the new Windows codec lane. The earlier raw/ASan factory results used the Visual Studio bundle. |
| GitHub Actions | checkout 7.0.1 / setup-python 7.1.0 / upload-artifact 7.0.2 | Selected version labels match fresh stable release metadata |
| CocoIndex Code | 0.2.43 isolated tooling | Selected version label matches the current stable API result; this is agent tooling, not a product dependency |
| DiskSpd | Not newly installed/run | Current official release API returns v2.3; benchmark planning only |

The ROCm lock records an observed `10.1.0rc3` package identity and an archive
digest measured locally from AMD HTTPS, not an independently published AMD
checksum. Preserve that distinction in release supply-chain qualification.
Do not install or replace a GPU driver to resolve it.

Client compatibility also needs an explicit distinction. The inspected host
reported Windows 11 Pro build 26300. Microsoft's current page lists Windows 11
26H2 build 26300.9974, while the captured ROCm 10.1 compatibility matrix lists
Windows 11 25H2. A build-family observation is not a full servicing/driver
qualification. Test the actual target combination and keep vendor-documented
support separate from a locally exercised profile; do not change the OS or GPU
driver to make a probe fit a document.
[Windows release information](https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information),
[ROCm compatibility matrix](https://rocm.docs.amd.com/en/docs-10.1.0/compatibility/compatibility-matrix.html).

[AMD Core release notes](https://rocm.docs.amd.com/en/docs-10.1.0/about/release-notes.html),
[current lock](../tools/rocm-sdk-lock.json),
[dependency policy/history](DEPENDENCY_AUDIT.md).

## Recent executed evidence

On 19 November, six paired multiwindow comparisons on the physical Windows
host produced equal compacted sizes and byte-exact CPU readback in both routes.
The related cohort retained 27.4% of its input image size and the mixed cohort
retained 20.8%. CPU finished sooner in every pair when including discovery,
publication, verified compaction and readback. A near-full model separately
verified that background compression releases its staged input, waits for a
changed image and resumes after verified reclamation.
[Matched maintenance and capacity evidence](evidence/amd-maintenance-parity-2026-11-19.json)

The indexed background controller now discovers related extents through checked
ordered lookups. One 129-member group grew through five publications while
foreground writes continued. A live Windows run used the SDK callbacks and
actual resource observations to publish and independently reopen a group.
Durable scan progress, broader similarity methods and useful collective
AMD throughput remain open.
[Automatic indexed discovery evidence](evidence/automatic-indexed-discovery-2026-11-18.json)

Indexed shared-base preparation now yields before each member and follows live
extent identity at publication. A 64-member fixture completed in 65 steps with
foreground edits interleaved and verified. The 528 modeled indexed publication
interruptions reopened complete checkpoints. Persistent discovery and useful
collective AMD execution remain open.
[Cooperative indexed grouping](evidence/cooperative-indexed-shared-base-2026-11-18.json)

Packed compaction now separates payload and index space. In a 1,926-object
mirrored fixture, three changed records needed 40 KiB of sizing metadata
reads, down from 328 KiB. Packing reduced complete compacted size by 15.5%
at 4 KiB isolation and 86.8% at 64 KiB. The indexed grouping path also
reopened and checked all 1,032 populated files around an eight-member
publication. Actual Windows resource scheduling completed staged compaction
and retired with balanced handles.
[Packed compaction evidence](evidence/packed-rebuild-accounting-2026-11-18.json)

The writable controller now stays available for the owner-controlled
lifetime. Probe time budgets belong to the external test supervisor.
The controller wakes on a stop, dispatcher termination, deferred write
retirement or a storage fence. A 31-second injected SDK idle test passed
with no maintenance or GPU budget queries and balanced handles after stop.
Current mounted Windows 11 lifetime qualification remains open.
[Controller evidence](evidence/owned-writable-lifetime-2026-11-18.json)

Indexed volumes now publish protected cross-file shared bases. Eight related
synthetic files occupied 76.2% less after compaction in the 4 KiB model,
and 63.6% less in the 64 KiB model. All 591 modeled indexed interruptions
recovered a complete checkpoint. Six paired SDK overwrites on the CPU and
AMD device preserved the other group members, with exact CPU reopen and
equal occupied bytes. The current foreground comparison selects CPU.
Scalable automatic candidate selection and preparation remain open.
[Indexed shared-base evidence](evidence/indexed-shared-base-2026-11-18.json)

The Windows controller collects GPU memory observations on demand. A
relocated executable acquired the installed AMD package, and three 512 KiB
AMD writes each verified eight candidate extents and sixteen mirrored
records. Independent CPU readback passed. All comparisons occupied
1,835,008 bytes per path. CPU completed in 1.21–1.60 ms and AMD in
703–1,331 ms, so this profile selects CPU for that request class. Useful
automatic GPU work, multi-AMD qualification and mounted response delivery
remain release work.
[Installed AMD evidence](evidence/installed-amd-routing-2026-11-18.json)

Indexed memory compaction now uses the staged CPU verification to select a
new image without re-reading its full contents at the switch. The current
provider uses the actual Windows page size and reserves virtual image capacity
from available headroom. In a controlled small-write fixture the image fell
from 3.75 MiB to 360 KiB with identical readback. An old CPU snapshot kept its
image pinned until release. [Compaction and sizing evidence](evidence/indexed-background-compaction-2026-11-18.json)

The owned indexed provider passed local Windows lifecycle and CPU recovery
checks with injected WinFsp operations. The real host scheduler retired
65 mappings in two transactions. The SDK phase fixture retired 129 in
three transactions and verified five resource refusals without backing I/O.
The linked provider passed the external indexed write/flush/rename/delete
fixture on disposable Windows Server 2025. It reopened the retained volume
on the CPU after unmount. [Owned indexed evidence](evidence/indexed-owned-windows-2026-11-18.json)

The controlled indexed WinFsp callback sent 128 KiB to the actual AMD device.
Both mirrored copies of each of the two stored extents matched verified
worker output. CPU reopen recovered the bytes with HIP unloaded from the
reader. The callback took 1.02 s in this single trial. Representative speed,
storage benefit and mounted behavior remain open.
[Indexed AMD evidence](evidence/indexed-lz4-deferred-2026-11-18.json)

Managed indexed selection and the SDK passed 22 selected Windows executions
across raw, codec and AddressSanitizer configurations. The compaction fixture
reduced occupied bytes from 385,024 to 147,456 and selected a complete old
or new image through 12 modeled interruptions. SDK checks covered sparse
writes, detach/reopen, independent reads and append-tail space reporting.
[Indexed managed evidence](evidence/indexed-managed-winfsp-2026-11-18.json)

The common memory provider uses headroom instead of a 40 MiB capacity ceiling.
On the Release run it admitted 91,881,472 bytes of domain capacity, locked zero
bytes for empty stores and locked 20,480 bytes for the growth fixture.
Routine status and background maintenance no longer scan all committed pages.
[Memory provider evidence](evidence/demand-locked-memory-2026-11-18.json)

At the reviewed commit, [the factory checkpoint](evidence/memory-volume-factory-2026-11-17.json)
binds 18 successful bounded local runs: raw and ASan builds, memory composition,
controller, SDK callback/fault suites, helper model/advice, live opportunities,
GUI workflow, and a separate app-only build with WinFsp disabled.

Each configuration passed 4,290 SDK callback checks and 492 modeled publication
failures. The raw memory-composition window published a live two-file group;
the final ASan window did not claim publication. Both final helper live tests
recorded real resource deferral. Modeled quiet-provider grouping and byte-exact
foreground reads were checked separately. Native RNG initialization was
distinguished from exact factory handle balance. Earlier failures are retained.

The new Windows codec lane used private CMake 4.5.0 and source-pinned static
Zstandard and LZ4. Nine paged storage tests, three codec tests, five SDK tests
and the helper model passed. The former helper maintenance fixture assumed that
three already compact files would benefit from grouping and failed its publication
assertion with Zstandard enabled. The initial build attempt also exposed MSVC
D8040 from Zstandard's separate default /MP fan-out. The bounded build passed
with the pinned upstream option disabled. [Review receipts](evidence/repository-review-2026-11-17.json).

The follow-up removed duplicate zero writes over complete metadata pages.
The focused mirrored 4 KiB-unit commit wrote 24 KiB instead of 32 KiB for the
same 16 KiB growth and two required root updates. The candidate passed 24
selected CTest invocations and three helper maintenance cases across codec,
raw and AddressSanitizer configurations. In the codec lane the helper declined
the already compact family without writes and grouped a separate profitable
family with at least 128 KiB of modeled compacted savings. The live helper runs
recorded resource deferral. [Follow-up evidence](evidence/paged-write-accounting-2026-11-17.json).

On 19 November, AMD full-tile reference pricing produced 11.3% smaller
protected compacted images than sampled reference selection in two
controlled cohorts. Eighteen matched runs reopened all files on CPU.
The next 18 trials exercised full-tile CPU pricing, which matched AMD's
compacted size and completed sooner on both cohorts. CPU now performs this
work in bounded steps during normal indexed maintenance. A separate set of
40 public file revisions measured grouping savings on top of Zstandard
and quantified backing reads. The grouped reader reduced backing reads
on both cohorts across rotating CPU and AMD runs. Extending verified AMD
copy hints enabled the writer to select spans in three of four public
revision profiles and cut the raw source image by another 8 KiB.
Independent CPU reads matched every file in all 36 matched trials.
CPU completed sooner on these small cold workloads. Sustained device
throughput and foreground interference measurements remain open.
[Full-tile reference evidence](evidence/amd-full-reference-pricing-2026-11-19.json)
[CPU reference pricing evidence](evidence/cpu-full-reference-pricing-2026-11-19.json)
[Public revision reads](evidence/public-revision-reads-2026-11-19.json)
[Matched reader and AMD hint qualification](evidence/verified-device-copy-refinement-2026-11-19.json)

The installed Windows controller verified a four-dispatch copy search and
published ten advised members in an eleven-file group. A separate live
foreground cancellation completed on CPU. Short transfers use serial waits
when they cannot fill the qualified queue. The small public revision pairs
provide timing and compacted-size controls for that decision.
[Windows queue qualification](evidence/native-anchor-wait-qualification-2026-11-19.json)

The owned Windows controller and desktop helper now reuse a fresh CPU
observation for short preparation steps with refreshed memory. The final
installed AMD callback fixture completed in 30.2 seconds compared with
62.2 seconds on the previous controller. Fresh observations still govern
publication, and an independent CPU reader verified every file.
[Windows preparation evidence](evidence/windows-maintenance-opportunities-2026-11-19.json)

Verified AMD copy advice now survives foreground reads while its staged
source remains eligible. On the Windows callback fixture, the same read
burst left one advised member on the previous controller and ten on the
updated controller. A selected edit invalidated the prepared group and
CPU rebuilt it with exact readback of the edited file.
[Foreground advice evidence](evidence/amd-verified-foreground-2026-11-19.json)

The evidence covers specific CPU paths, modeled failures, SDK callbacks and
helper behavior. Full current-source CTest, an actual mounted product, useful
ordinary GPU operation and physical power-loss qualification remain open.
Earlier million-object and hardware/mount results keep their source and fixture scope.

## What current research changes

Research is recent and primary, with native extraction receipts. Preprints and
abstract-only captures remain labeled as such; reported results are not FlitzFS
measurements.

- **Similar-data grouping is established research.** FAST 2026 RubikFS groups
  and reorders chunks using similarity and hotness. It is a read-only design,
  so FlitzFS still has substantial writable, recovery and GPU questions to solve.
  Grouping alone cannot establish uniqueness. [RubikFS](https://www.usenix.org/conference/fast26/presentation/huang).
- **Structured families deserve explicit candidates.** NSDI 2026 ZipLLM studies
  model-family redundancy and combines deduplication with lossless delta coding.
  Its specialized workload is not evidence of gains on arbitrary files.
  [Primary publication](https://www.usenix.org/conference/nsdi26/presentation/wang-zirui).
- **GPU-friendly representations can be valuable beyond LZ4.** EuroSys 2026
  Invariant Bit Packing studies structured ML data and transfer cost. Treat it
  as a candidate mechanism, not a generic-filesystem or AMD performance result.
  [Extended primary paper](https://arxiv.org/html/2605.30728v1).
- **Learned compression needs a decode-cost design.** The November 2026
  invocation-bottleneck preprint examines the cost of model calls and
  reproducibility of probability computation. Investigate bounded deterministic
  predictors rather than assuming a large model yields usable file reads.
  [Preprint](https://arxiv.org/html/2611.05871v1).
- **Search can choose reversible transforms.** The August 2026 Brevis preprint
  investigates typed program synthesis for lossless tensor compression. A bounded
  search over safe reversible operators is a candidate for FlitzFS research;
  arbitrary executable programs from stored media are not.
  [Preprint](https://arxiv.org/html/2608.02162v1).
- **Windows already offers relevant GPU decompression research/software.**
  Microsoft's DirectStorage 1.4 work includes CPU/GPU Zstd and asset-conditioning
  techniques. Some conditioning is lossy and must be excluded. Destination,
  readback, workload and CPU-recovery requirements need evaluation before use
  in a general filesystem. [Microsoft](https://devblogs.microsoft.com/directx/directstorage-1-4-release-adds-support-for-zstandard/).
- **Record comparisons need complete rules.** The maintained large-text benchmark
  is useful for specialized compression comparisons, not filesystem semantics.
  Model/decoder costs and access constraints must be disclosed; a text result
  cannot stand in for heterogeneous storage. [Benchmark](https://mattmahoney.net/dc/text.html).

## Release distance and course correction

There is no defensible completion percentage. A million object keys, a tray
application, a GPU request and thousands of checks do not compose into a
million-file, accelerated, durable product automatically.

The detailed plan retains a low-confidence engineering envelope of roughly
**60–130 person-weeks**, with an open research tail for the compression frontier.
That is a work estimate, not a promised calendar for an autonomous agent.
It must be recalibrated after the scalable native engine, real mounted path and
first held-out collective comparison. The small recent integration slices do
not justify calling a release imminent or silently lowering the exit criteria.

The immediate direction is to connect and qualify the real Windows path and
design the next storage capability around both paged native metadata and
incremental safe allocation. A bounded staged handoff and occupied-byte
instrumentation supply early evidence. GPU cost acquisition and stronger
collective-compression experiments proceed alongside that work. A setup or qualification problem in one track must not
freeze independent progress in the others. The [implementation plan](IMPLEMENTATION_PLAN.md)
specifies concrete steps, dependencies, acceptance evidence and stop conditions.
