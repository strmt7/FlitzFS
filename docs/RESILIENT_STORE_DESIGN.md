# Resilient compressed filesystem: architecture and qualification plan

Research/owner-direction checkpoint: 9 November 2026. This is a design, not
implemented writable storage, power-loss qualification or measured GPU benefit.
Existing read-only formats remain unchanged.

## Product objective and current host boundary

Combine high sustained read/write throughput with exceptionally strong native,
automatic lossless compression across arbitrary files. Minimum latency is
secondary. FlitzZip is a reference, not a constraint on codecs or algorithms.
Preserve normal filesystem advantages and independent CPU recovery when AMD
hardware or its software stack is unavailable.

All further disk-based development, research tools, builds, tests, benchmarks
and cleanup run only on disposable hosted machines/VMs. No more host processes
or local checkout writes. RAM-only owner-host tests are not yet qualified;
anonymous memory can be paged, and no RAM-disk driver is to be installed.
Power cycling or inducing GPU/driver faults on the owner's PC is forbidden.

No software can guarantee every hardware/firmware/media failure without defined
assumptions and redundant copies. Define acknowledged durable writes separately
from unflushed application memory. GPU availability must never be a prerequisite
for storage recovery.

## Primary-source research and its implications

- [SQLite atomic commit](https://www.sqlite.org/atomiccommit.html) separates
  recovery-log persistence, data persistence and publication while assuming
  reordered writes and meaningful flushes. Inference: test ordered barriers and
  crashes at each publication boundary; a v1 footer is not a transaction log.
- [Btrfs design](https://btrfs.readthedocs.io/en/latest/dev/dev-btrfs-design.html)
  describes COW metadata with ownership/address/generation checks. Inference:
  bind recovery references to generation and identity, beyond a bare length/CRC.
- [OpenZFS copy-on-write](https://openzfs.github.io/openzfs-docs/Basic%20Concepts/Copy-on-write.html)
  distinguishes transaction groups and synchronous intent, and explains COW
  overhead. Inference: account for log/checkpoint/reclamation costs when measuring
  both physical space and equivalently durable throughput.
- [Microsoft FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
  documents buffered writes, file flushing and its small-write costs. Inference:
  review file/name durability explicitly; this does NOT authorize volume/raw
  access or administrative commands.
- [CrashMonkey/Ace abstract](https://www.usenix.org/conference/osdi18/presentation/mohan)
  reports crash-consistency bugs in mature filesystems from small operation
  workloads. Inference: systematically test create/rename/unlink/write/flush,
  beyond terminating a read-only helper. Full paper not reviewed in this round.
- [Btrfs compression](https://btrfs.readthedocs.io/en/latest/Compression.html)
  documents automatic policy, incompressible data and extent/read interactions.
  Inference: compare multiple region sizes and explicit bypass decisions.
- [GogetaFS abstract](https://www.usenix.org/conference/fast25/presentation/pan)
  studies filesystem/deduplication metadata maintenance. Inference: measure index
  and transaction overhead rather than treating deduplication as free. This is
  an abstract review, not full algorithm/performance verification.
- [GPUfs author-institution record](https://cris.technion.ac.il/en/publications/gpufs-integrating-a-file-system-with-gpus-5/)
  describes GPU-oriented file access/cache semantics. GPU filesystem research
  therefore exists; that does not make it a general compressed desktop product.
  Differentiation needs combined measured outcomes, beyond having a GPU.
- [HIP error codes](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/error_codes.html)
  and [error handling](https://rocmdocs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/error_handling.html)
  distinguish incompatible/missing hardware, resources and damaged contexts.
  Inference: clearing the last error or observing fresh VRAM budgets cannot
  establish repaired driver health. See GPU_RECOVERY.md.

Ten substantive sources passed the isolated reviewed Crawl4AI provider's scope
checks before the owner's later host restriction. It used robots checks,
45-second page timeouts, no external LLM and no personal/persistent profile.
Native source captures/digests were retained externally. The AMD CDNA/RDNA study
landing page returned HTTP 202 with one extracted byte; it was rejected and
supports no AMD performance claim. Search snippets and abstracts do not substitute
for complete papers. New research needs an admitted off-host crawler; do not
resume the owner-host provider or bypass it with another page reader.

These are engineering inferences, not implemented or proved durability. A
compact source receipt manifest is retained alongside this document.

## Candidate architecture to implement and falsify

These hypotheses need prototypes and comparisons; they are not an asserted
invention, selected wire format or qualification result.

### CPU-controlled durability

Keep authoritative namespace, transaction order and recovery metadata under
CPU control. A bounded ordinary-file-backed landing log records bytes and
metadata in a CPU-readable form. Immutable records/COW references bind object
identity, logical offset, stored/logical lengths, codec, generation and integrity.
VRAM never contains the sole acknowledged copy.

Group commit can amortize barriers, but durable success follows actual required
persistence, not queue admission. Explicitly define cached-write, write-through,
flush and mapped-I/O acknowledgement. Durable-by-default is a candidate whose
cost must be compared against equally durable configurations.

Preserve old committed state until a new independently checked root is durable.
Specify multiple generation-bound roots and torn-record rejection without
assuming whole-record atomic writes. Their placement/alignment/failure domains
need a platform proof: two records in the same vulnerable write unit are not
independent. Recover complete transactions only, with admitted dictionaries
and codecs and an independent CPU reader.

Provisional publication order, still requiring a proved platform protocol:

1. Admit memory, metadata/output growth and recovery-space reserves.
2. Append new data/metadata without modifying reachable committed objects.
3. Make data, dictionary dependencies and recovery intent durable.
4. Publish the generation/root through a separately validated commit protocol.
5. Complete publication/name persistence before durable acknowledgement.
6. Reclaim only after durable reference updates and reader retirement.

Windows ordinary-file namespace/directory durability needs more source review
and fault injection. Current v1 pack/extract file flush is not that writable
protocol. GC interruption must leave old committed data independently readable.

### Adaptive compression without durability dependency

Use a fast CPU-readable landing representation while a separately admitted
compressor produces stronger immutable candidates. Investigate content-aware
segmentation, larger regions with recoverable subregions, dictionaries,
deduplication, similarity/delta coding and reversible type transforms. Each
needs bounded memory/indexing, CPU decoding and provenance. Preserve unknown,
compressed and encrypted bytes; no lossy coding for general file content.

Keep GPU output private until identity, completion, status and content checks
pass. Initially retain independent CPU byte comparison. Reducing verification
cost requires a separately justified integrity/authentication design.
Recompression/compaction publish references transactionally, never overwrite
live data. Failure leaves the authoritative CPU-readable form intact.

Hypothesis: stronger bulk compression reduces storage traffic enough to offset
transfer, codec, indexing and commit costs. Measure steady state after backlog
stabilizes. Count landing records, dictionaries, unreclaimed objects and pending
work in physical storage. Bound queues/reserves so fast writes cannot hide an
unbounded recompression debt. Include contention and recovery costs.

Preserve existing eight-block/256-KiB HIP ceilings until hardware admission and
isolated evidence justify changes. A fast kernel is not an end-to-end speedup.

### Failure-aware scheduling

CPU completion must not wait for driver teardown. Fence late candidates by
request generation. Supervise retirement independently and forbid replacement
workers until confirmed safe. Keep immutable inputs and independent CPU recovery.

Use configuration identities, fresh adapter budgets, failure history and separate
recovery qualification. No device discovery in callbacks or repeated scan on
every request. Notifications can request bounded off-path qualification; they
are not health proof. The policy exists, but the runtime/coordinator/evidence
producer remain unimplemented. A per-request gate stays terminal even when a
later request is separately permitted.

## Conventional filesystem acceptance ledger

| Behavior | Required evidence / present limit |
|---|---|
| Arbitrary file bytes/types | Reversible transforms and generic fallback |
| Create/overwrite/append/truncate/partial writes | Transactional metadata/content; no mounted writes yet |
| Rename/replace/unlink/delete-on-close | Atomic namespace, open-handle behavior and crash recovery |
| Flush/write-through/mapped I/O | Explicit durability, paging writes and cancellation |
| Sharing/locks/concurrency | Windows applications and contention tests |
| ACLs/ownership/times/stable IDs | Persistence/round trips; current metadata is synthetic |
| Sparse ranges/large files/directories | Bounded growth/recovery; existing small caps remain |
| Links/reparse points/streams/case | Explicit supported contract; reject unsafe/unsupported forms |
| Snapshots/backups/CPU export | Stable generations and independently verified restore |
| Bit rot/media loss | Detection plus redundant recovery; checksum cannot recreate lost bytes |
| Full disk/quotas/interrupted GC | Commit/recovery reserves and bounded orphan handling |
| Missing/overloaded/faulty GPU | Independent CPU result and recovery |

## Staged rounds and comparison requirements

1. Finish CPU/recovery-policy validation; retain unfavorable measurements.
2. Specify transaction format and independent reader before mutation APIs.
3. Build a deterministic RAM persistence model: reorder unflushed epochs, drop
   tails, tear records, fail flushes, exhaust space, interrupt GC and crash every
   publication step. Compare recovered history to acknowledged commits.
4. Reproduce using real file I/O/process termination ONLY in disposable VMs.
   Process termination alone is not physical power-loss proof.
5. Implement isolated runtime/module admission and asynchronous coordination;
   fake faults first, then separately qualified tiny synthetic AMD work.
6. Compare codecs on held-out diverse corpora and steady-state complete-system
   throughput, physical allocation and recovery costs.
7. Qualify durability with controlled persistence/fault injection on disposable
   systems; never force host power/driver failures.
8. Add writable callbacks only after independent recovery passes, then application,
   namespace/concurrency/mapped-I/O matrices on disposable VMs. Release is owner-controlled.

Near product qualification, use several appropriate conventional/compressed
filesystems and established workload tools. Candidate comparison families are
Windows NTFS/built-in compression/ReFS and Linux ext4/Btrfs/OpenZFS, where supported
in disposable environments. Candidate suites include DiskSpd/fio, filebench and
filesystem namespace/durability tests; exact versions, official docs and licenses
still need review. These are planned candidates, not executed comparisons.

Match durability, cache conditions, hardware, corpora, resource caps and metadata
accounting. Alternate lanes and retain distributions/source hashes. Never combine
RAM-backed and physical-storage results into one superiority claim. Owner-host
RAM testing cannot establish real-media behavior; physical tests stay off-host.
