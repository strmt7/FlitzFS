# Agent instructions for FlitzFS development

## Current owner direction: local development is primary

On 15 November 2026 the owner explicitly deleted the blanket off-host,
GitHub-only development and mandatory VM-first restrictions. Use the owner's
Windows PC for normal source editing, builds, debugging, research tools,
CPU/GPU testing, GUI work and benchmarks that can safely run there. Do not use
GitHub APIs as a substitute for a local checkout or require hosted CI before
ordinary local development or tests. Use GitHub for source synchronization,
security information and complementary checks; push at meaningful checkpoints.
Use VMs or hosted machines for operations that cannot safely run on this host,
including deliberate kernel/driver/power faults and unqualified driver provisioning.

The owner reaffirmed full host use during this planning round. Remove obsolete
location-only instructions and checks from safe tooling; resource, source,
ownership and backing contracts are the admission criteria. Historical hosted
receipts and legacy fixture names are not permission rules. A genuine OS/API or
fixture-backing requirement must be described as such, never as a general host
ban. Do not require hosted CI before ordinary local work.

This section governs execution location. Older component pages and historical
receipts do not reinstate a blanket owner-PC ban, mandatory CI/VM-first gate,
source-only local-work exception or prohibition on local build/cache/log files.
Preserve their technical format, ownership, integrity and resource contracts.
Do not treat a past successful fixture as qualification of changed behavior.

The owner expects autonomous, token-efficient progress. Inspect actual state,
choose useful implementation work, fix failures at their cause, and reuse valid
unchanged evidence. Do not repeat an unchanged diagnostic or add more isolated
probes in place of product integration. Do not ask for permission already given.

## Data and host protection

- Preserve all existing user files and changes. Never format, repartition,
  initialize or clean drives, write raw devices, change boot/security policy,
  or run destructive drive/volume commands.
- Filesystem test-volume/image backing on this host remains in memory. Source,
  build, dependency, cache, corpus and report files are ordinary development
  artifacts and may use a scoped development directory with verified free space.
  An in-memory model does not prove that Windows performs zero storage I/O.
- The owner's later 15 November instruction permits installing a FlitzFS-related
  driver on this host only after its stability has been established. Preserve
  this conditional permission; older blanket driver-install prohibitions do not
  override it. No current experimental driver is thereby qualified. Before any
  install, bind the exact signed artifact to current Windows 11 client, integrity,
  filesystem/lifecycle, resource, recovery and isolated driver-fault evidence;
  verify installation/rollback and compatibility with existing driver state.
  Do not claim perfection or qualification from a clean build or a short test.
- Never modify, replace or reconfigure the GPU driver, unrelated drivers or GPU
  clocks/power/voltage. Never weaken boot/security policy or mount over an existing
  directory/drive letter. Unqualified installation and destabilizing driver/kernel/
  power tests remain isolated. No current install action is authorized by tests
  that did not establish the owner's driver-stability condition.
- Do not exhaust RAM, VRAM, CPU or storage. Discover actual capabilities, load
  and available physical/commit/device headroom. Size build concurrency and test
  envelopes from that evidence; do not assume one host's specifications or turn
  a diagnostic cap into a permanent product allocation/group limit.
- Use bounded process trees, finite deadlines, below-normal background priority,
  bounded queues and confirmed shutdown. Stop only owned processes/handles.
  Leave no unintended workers, compiler helpers, services, scheduled tasks,
  registry changes or reboot persistence. Never clean unrelated applications.
- Use new uniquely owned fixtures. Creation of output files must be exclusive
  at the OS create operation. Preserve source data and partial failed outputs;
  do not use recursive removal or remove_all in tests.
- Local GPU work has standing owner authorization. Audit the changed path, use
  fresh resource/runtime/adapter evidence, independent CPU comparison, finite
  quanta and confirmed retirement. Do not induce driver faults or stress WDDM.
  Unknown headroom or integrity evidence refuses that work and selects CPU.
- Run only host-safe tests. A memory fixture, process kill or callback test does
  not prove physical power-loss durability, complete Windows compatibility,
  arbitrary-failure survival, universal compression gains or no-freeze safety.

## Mandatory efficiency and research tools

Apply the installed caveman skill for every task. Keep internal context bounded;
use normal clear public prose and preserve exact safety/technical meaning.

Before broad/conceptual repository navigation, use the installed cocoindex-code
skill and a current source-bound semantic search. Confirm candidates in live
source and retain an actual search receipt. Exact filenames, symbols, counts
and small known scopes use rg directly. Bind a local provider/cache explicitly;
never overwrite another repository's MCP registration, credentials or index.
If unavailable/stale, diagnose and restore it before dependent broad navigation.

For web-page reading, apply crawl4ai-research with a reviewed isolated provider,
robots checks, finite timeouts, no external LLM/profile/stealth/challenge bypass,
and substantive native URL/status/text/version/digest receipts. Search engines
may discover URLs; snippets are not source evidence. Exact API/package/source
byte captures use native transports. Local research is authorized. Preserve
failures and restore an unavailable provider before dependent research.

Use recent reputable primary sources and current official documentation for
important API, dependency, algorithm or feature decisions. Verify current facts;
do not substitute model recollection, a success flag or an undated page for
source review. Pin/audit dependencies and support safe private installation of
missing user-mode tools where appropriate. Dependency setup never changes GPU or
unrelated drivers; qualified FlitzFS driver deployment follows the condition above.

## Local source and Git workflow

1. Inspect Git history/status and preserve existing changes before editing.
   Prefer an appropriate existing checkout; use a separate local checkout if
   needed to protect another dirty working tree. Develop on main only. No
   development branches, PRs, force-pushes or destructive resets of user work.
2. Edit, review diffs, build and test locally. Validate the actual changed source;
   bind reports to commit plus dirty-diff/source digests when not yet committed.
   Use small validated local commits and less frequent normal pushes.
3. Keep builds/tests CPU-only by default; invoke authorized GPU work explicitly.
   Run relevant deterministic byte-exact and malformed-input tests, raw-only
   controls and appropriate sanitizer/Windows checks. Use hosted/VM checks where
   the required OS or unsafe fault scenario is unavailable locally. Do not claim
   a platform pass from a different platform or a subset as a full suite pass.
4. Review lifecycle/resource cleanup and retain failures as well as successes.
   Repeat checks only after a change or unresolved failure justifies them.
5. Do not add/bump product numbering, tags/releases, publish packages or call a
   development commit a release. The owner alone decides when to release.

## Product requirements and storage invariants

FlitzFS is a Windows 11 AMD64 filesystem with helper/tray and driver integration,
not ZIP software. FlitzZip provides mechanisms/inspiration, not a format or
codec constraint. Keep discovery and installation host-agnostic on the supported
OS/architecture. The product must support physical and memory-backed volumes;
the host experiment's memory backing is not a product feature limitation.

Minimum TOTAL stored bytes is the primary compression goal, including models,
dictionaries, indexes, redundancy, temporary work and retained history. Keep
practical read/write and random/cold access; minimum latency is secondary. Use
fast ingestion and stronger collective background compression. Protect immutable
versioned bases/models/dictionaries; bound dependency depth, decode scratch and
amplification. Shared content must survive mutation/deletion of any filename.
Novel/learned codecs need evidence, exact lossless decoding and fair comparisons.

Owner clarification, 15 November 2026: design storage and automatic group
discovery for thousands/millions of files and growth with available capacity.
Use paged indexes, bounded caches, incremental similarity search and persistent
scan progress; avoid whole-namespace residency and all-pairs comparisons.
Compression groups may contain one, two or thousands of files. Choose membership
and size from net stored cost and practical observed access latency. Separate
group membership from indexed decode/restart units so a small read has bounded
work. Current flat-store/64-KiB diagnostic limits are not product group limits.

GPU acceleration is required development work. CPU fallback provides continued
operation and independent reads of every admitted stored representation. CPU
and GPU encoders/streams/sizes may differ. Verify decoded source bytes, not
compressed-stream equality. A GPU presence alone does not establish benefit.
Keep immediate CPU selection free of driver calls/waits, terminal per-request
failure, evidence-aware recovery and confirmed retirement before a new worker.

Authoritative namespace, acknowledged writes and decoder dependencies require
CPU-readable recovery. Retain existing COW publication, exact identity/generation
bindings, independent stored/decoded integrity, barriers, terminal I/O fencing
and rollback. Checksums detect damage but do not recreate lost bytes. Define the
failure model and redundancy before claiming repair or power-loss resilience.

Persistent workload learning/optimization is advisory, rebuildable and separate
from essential decoder state. Make maintenance automatically responsive to
workload/content and fresh contention, very low priority, incremental, bounded
and promptly yielding. Mutable learned preferences never override safety gates.

Cooperative shared-base work uses cached per-data-object revisions and immutable
source ownership. Every native data publication/removal must update its guards;
test selected-content invalidation before I/O and unrelated-write reconciliation.
Preparation steps are not an automatic scheduler or a nonblocking commit.

The owner's coherence-catalog clarification requires a persistent sparse,
paged similarity/candidate index in a protected internal logical area, not a
dense all-pairs file matrix or another host partition. Final learning/inference
and all normal operation must work offline without an online model. Minimum
complete stored size remains primary; practical latency and drive wear receive
secondary consideration. Background correlation, compression, defragmentation,
repair and format migration must measure interference and write amplification,
reuse unchanged work and defer/yield under contention. Adapt to each host and
volume's actual data distribution; do not present fixed heuristics as full AI.

Multi-release compatibility and gradual automatic migration are required product
work. Preserve supported older readers/fixtures and immutable CPU decoding
dependencies. Version format semantics explicitly; never reuse identifiers.
Migrate bounded units with verified COW publication and recoverable progress,
not destructive whole-volume upgrades. The release plan specifies the proposed
minimum support window; it is not an implemented updater or a release grant.

## Component reading and current boundaries

Read the relevant canonical contracts before changing their implementation.
[Reference index](docs/reference/README.md) and its [catalog](docs/reference/catalog.md)
cover narrower contracts. In particular:

- Storage/format/publication: docs/OBJECT_STORE_FORMAT.md,
  docs/RESILIENT_STORE_DESIGN.md, docs/REPLICATED_CHECKPOINTS.md,
  docs/PERSISTENCE_MODEL.md, docs/STORE_RESUME_AND_INTERRUPTION.md.
- Namespace/session/maintenance: docs/NATIVE_FILESYSTEM.md,
  docs/INODE_RECORD_NAMESPACE.md, docs/DIRECTORY_NAME_INDEX.md,
  docs/NATIVE_PAGED_MAINTENANCE.md, docs/MANAGED_PAGED_VOLUMES.md, docs/STAGED_PAGED_REBUILD.md, docs/MIRRORED_PAYLOADS.md, docs/SHARED_BASE_GROUPS.md,
  docs/NATIVE_FILE_LIFETIME.md, docs/VOLUME_SESSION.md, docs/COMPACTED_COPY.md,
  docs/RECOMPRESSION.md, docs/OPTIMIZATION_STATE.md, docs/COLD_COMPRESSION_RESEARCH.md.
- Windows callbacks and pending ownership: docs/WINFSP_CALLBACKS.md,
  docs/NATIVE_WRITABLE_ADAPTER.md, docs/OWNED_DEFERRED_WRITES.md,
  docs/NATIVE_WINDOWS_LZ4_ADVICE.md, docs/MOUNT_PROCESS_ISOLATION.md.
- GPU/runtime/worker boundaries: docs/HIP_BATCH_PLAN.md, docs/GPU_RECOVERY.md,
  docs/WORKER_CHANNEL.md, docs/HIP_LZ4_RUNTIME.md, docs/HIP_LZ4_COMPUTE.md,
  docs/ASYNC_LZ4_CONTROLLER.md, docs/ADAPTIVE_LZ4_EXECUTION.md,
  docs/UNIFORM_LZ4_REUSE.md, docs/UNIFORM_LZ4_SWEEP_PROFILE.md.
- Encoders/readers: docs/NATIVE_FAST_INGESTION.md, docs/NATIVE_CODEC_PLAN.md,
  docs/FLZIP_READONLY.md. Compatibility readers are bounded CPU subsets, never
  grounds to initialize HIP or claim full upstream interoperability.

Normal callbacks/tests do not install/load a driver or create a mount. Native
pending writes own copied input and opaque request identity; use one control
thread/coordinator, exactly one response, ordered CPU publication and joined
completion ownership before destruction. Never replay a committed write after
response delivery fails. Owned immutable inputs and exact worker exit/transport
quiescence remain mandatory. CPU semantic/device-body tests are not GPU execution.

Current GPU diagnostic behavior must not be promoted to ordinary automatic
routing without implementation and measured benefit. Preserve old readers/wire
compatibility or add an explicit verified extension; never silently reinterpret
old bytes. Keep compile-only/reference oracles test-only and exact-source pinned.
Do not edit upstream/other repositories or claim unexecuted runtime behavior.

## Documentation and completion claims

Before changing automatic group discovery, content revision stamps or resource
opportunities, read docs/AUTOMATIC_GROUP_MAINTENANCE.md. Ordinary data mutations
must update physical and content revisions; representation-only shared-base
rewrites update physical revisions alone. Preserve stale-selection rejection,
independent CPU verification, weak session lifetime and one serialized control
context. Supplied/model observations do not prove real host telemetry, low OS
priority or bounded foreground interference. The core controller alone supplies
neither Windows scheduling nor a normal GPU route; the helper integration below
provides separately qualified Windows opportunities.

Read docs/WINDOWS_BACKGROUND_MAINTENANCE.md before changing the helper's volume
factory, PDH observation window, background thread scope or idle loop. Preserve
fresh all-processor observations, measured full-quantum pacing, foreground/stop
preemption before the next quantum, one unconsumed scalar notification and the
separate foreground content generation that protects unsaved editor text.
The helper now selects managed mirrored paged storage; Explorer/WinFsp factory
integration remains required. Background scheduling is not GPU qualification or
permission to install a driver. Preserve exact resource retirement checks;
separate native PDH cold initialization from leaks in product-owned lifecycles.

Read docs/development/documentation.md before changing documentation. Follow
FlitzZip's end-user README/navigation logic, adapted to FlitzFS. README contains
product information and links, never commit/test histories, internal budgets,
worker details or per-round updates. Preserve existing paths, notices and failure
evidence. Component contracts, current plans and source-bound receipts belong
under docs/, with links from the relevant index.

Update FORMAT, SAFETY, ROADMAP and validation records when actual behavior
changes. Keep a concise current checkpoint, separate from historical results.
Report what changed, why, executed checks, resource cleanup and material limits.

Use docs/RELEASE_EXECUTION_PLAN.md as the current release work/effort/gate plan
and docs/BENCHMARK_PROTOCOL.md for objective comparisons. Prefer recognized
DiskSpd/fio/fsbench and correctness/codec suites where suitable; custom tests
cover only explicit gaps. Preserve all trial results and measure complete
stored bytes, CPU/GPU setup, access costs and background interference. Historical
example timings, synthetic zeros or selected best runs do not establish records.
Do not claim release readiness, exceptional compression, GPU speedup or data-loss
immunity without evidence covering that specific claim. Keep the full goal active
until its required product work is actually complete; do not release autonomously.
