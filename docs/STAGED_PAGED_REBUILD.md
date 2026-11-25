# Bounded staged paged rebuild (experimental)

The [mirrored-payload extension](MIRRORED_PAYLOADS.md) retains its protection
during staged copying and independently verifies both new copies. A staged
attempt can reconstruct a lost bank while the source remains CPU-readable.

Paged storage now supports an explicit candidate alongside a live foreground
writer. Full-content verification can check several short records from the
same checked leaf in one opportunity. The copy retains its single-record
work unit for ordinary rebuilds. The index phase reads up to one leaf of
record headers per opportunity.
Native wrappers preserve their namespace format and rebind the image identity.
This is a maintenance primitive, not an automatic scheduler, active-volume
handoff, persistent restart journal or collective compression algorithm.

## API and ownership

`PagedObjectStoreWriter::prepare_rebuild` creates one owned candidate for that
writer. `Rewrite::advance(current_writer)` performs one bounded opportunity;
`finish(current_writer)` returns the completed new writer. Generic rebuilding
does not rewrite user object bodies. The native wrapper uses a private,
identity-only root replacement through `prepare_paged_recompression`,
`advance_paged_recompression` and `finish_paged_recompression`.

The source writer keeps its existing exclusive lease and the candidate leases
a separate target. The source-owner token and checkpoint identity guard each
advance and finish. A foreground commit invalidates an exact-checkpoint
candidate. The owned indexed compactor can instead advance from an immutable
source checkpoint and reconcile subsequent commits before selection.
Reopening the source creates a new owner and cannot revive an old attempt.
Moving writer or candidate objects preserves the original owner.
Only one candidate per writer is admitted; duplicate preparation refuses before
backing I/O. Cancellation or terminal failure releases its reservation and
target lease. Partial target bytes remain owned by the provider, never deleted
or selected automatically.

All calls remain serialized on one control thread. The backend must qualify
exclusive, nonaliasing storage and stable successful-write/flush semantics;
per-instance leases do not stop another facade or hostile provider. Native
preparation rejects pending/detached file contexts, an invalid source/backend
or invalid identity. [Managed paged sessions](MANAGED_PAGED_VOLUMES.md) support synchronous handoff.
The owned Windows memory and paired-range paths use the staged copy with a
separate selector switch and reader-safe retirement. The controller can
advance several private preparation steps within one admitted CPU observation.
Each step releases the callback lock afterward and requires current memory
headroom. Candidate creation, private root writes and barriers, selector
publication and the first retirement step each start from a fresh CPU capture.
Subsequent checked inactive-image releases can continue under the same
[controller opportunity](WINFSP_OWNED_CONTROLLER.md#content-events-and-bounded-work).

## Work phases

| Phase | Work per advance | Target roots |
| --- | --- | --- |
| Verify source | A bounded set of independently decoded records from one checked leaf | Absent |
| Copy | One record encoded or preserved and independently compared | Absent |
| Build index | Up to one leaf of checked candidate headers and the resulting index pages | Absent |
| Verify target | A bounded set of records individually compared to their source and generation | Absent |
| Publish | Dependency barrier, two root writes/barriers, independent root reparse | Private complete candidate |
| Verify publication | A bounded set of records selected through the reparsed target tree and individually compared | Private complete candidate |
| Ready | Finish transfers the verified writer; no backing I/O | Complete candidate |

Phase transitions check totals, seal padding, finish bounded tree levels or
initialize the fixed 128-KiB root area. Initial source verification completes
before any target write. The copy builds a rolling BLAKE3 digest of the exact
headers it writes and verifies. Index construction reparses bounded headers
from the private packed payload stream and must reproduce that digest before
publication. The subsequent independent CPU pass checks the data and both
mirrors.

Each root replacement must match the independently read source root except
its 16-byte identity. Data-object generations remain stable and image
publication advances by two. Compaction preserves non-root encoded payloads.
Recompression uses the supplied CPU policy. Synchronous and staged copies
use the same packed layout and checks.

Target extent/contract drift, integrity failures and indeterminate publication
terminally discard the attempt. Newly degraded metadata refuses publication;
root-slot degradation refuses success. After publication, a separate incremental
CPU pass checks every selected target record. A final source-root/contract/extent
check precedes Ready. `finish` uses those results and, for native storage, copies
the already admitted bounded Model with the new identity; it performs no new
backing reads, writes or flushes. API return/exception establishes the outcome;
an absent/moved/finished candidate has no usable progress and reports the
current API's `failed` phase.

The engine retains one unfinished leaf and bounded references per tree level.
A verification step spends up to two commit units on the sum of record
framing, stored size and logical size within a quarter of one leaf. A record
larger than that budget receives its own opportunity, up to the existing
1-MiB object limit. Each record still uses its own replica checks and CPU
comparison. Index work follows one leaf's capacity, and tree finishing
follows the admitted depth of 16. The indexed native path uses the same
staged layout with its paged namespace.
Each verification and copy pass carries a checked tree cursor. Target comparison
uses a bounded source lookup cache for that pass. A pass change resets both,
so verification after publication reads the private image again.

During copy, an independent two-copy readback supplies the decoded bytes for
the comparison. During verification, adjacent frames in one checked leaf and
one bank can share a backing read for each replica. The scanner checks both
banks for equality, validates each record against its own descriptor and digest,
and decodes the records for the individual CPU comparisons. A failed run
triggers the existing per-copy checks. A source survivor can support recovery.
A degraded target fails staging, including when a later reread succeeds. The
per-step foreground yield point and separate data, root and selector checks
remain part of the publication sequence.

The indexed fixture reduced the count from 51 to 33 steps when preserving
encodings and from 51 to 29 when re-encoding. Both 4-KiB and 64-KiB
failure-unit models kept the same maximum measured I/O bytes per step.
The same 1,413 write and barrier cuts ran against the new geometry.
[Rebuild and mounted trace evidence](evidence/windows-ordinary-reclamation-progress-2026-11-25.json)

In the 1,024-record mirrored fixture, copy readback changed from 3,495 backing
reads to 2,330. Each packed-target verification pass changed from 2,370 reads
to 1,020 and checked 1,325,492 bytes. The fragmented-source fixture showed the
same target gains. Its source scan retained the original cost, since the live
source records crossed gaps. The measurements count provider calls in the
Windows-host memory fixture. The two separate faults in a target run were
rejected before and after the private root publication.
[Coalesced rebuild verification evidence](evidence/windows-rebuild-verification-reads-2026-11-25.json)

Resource admission remains the provider/controller's responsibility. There is
no thread, service, timer, online model or GPU launch. An OS flush can still
block; this is a bounded-work contract, not a wall-clock or zero-interference
guarantee. A qualified controller must supply fresh headroom/contention evidence,
low scheduling/I/O priority and foreground-yield opportunities.
[Windows background resource scheduling](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority)

## Foreground reconciliation

The owned indexed compactor fully verifies the packed base image against its
captured source, even when new commits are arriving. Each reconciliation
captures a later immutable source checkpoint. Its sorted tree walk skips
identical references, preserving progress on unaffected subtrees. It compares
changed leaves by object key and detects inserts, replacements and removals
across leaf splits and disappearing branches. The walk uses the roots of
completed foreground commits.

An apply step has one leaf's descriptor budget and admits at most 1 MiB of
decoded payload. The private image preserves current object
generations and codecs. It rebinds only the native root identity, and patches
the private tree with the existing copy-on-write transaction planner. Replay
stops if the private append span can no longer reduce occupied bytes against
the captured source. Root publication uses a fresh observation and verifies
the private checkpoint after its barriers. A second tree walk compares every
changed payload through that checkpoint and confirms each removed key is absent.
Unchanged references still point to individually verified records.

Selection requires the current writer and an exact, verified source
generation. A commit during replay or after Ready triggers another bounded
pass on the same private image. The selector commits the verified image.
Retirement releases space after old readers drop their pins. A failed replay
leaves the selected source readable. Existing readers use the same format.
Staging requires a second image and quiet opportunities. Persistent
candidate recovery and segment reuse remain work for the release.

The initial source pass also checks for a verified capacity-blocked
payload repair for the captured generation. It reconfirms the single-copy
failure and budgets the packed target, including the rebound root, before
writing. A verified replacement may occupy the same space if the selected
generation still carries that observation at handoff. Both target replicas
must pass the normal verification. Foreground commits require exact
current-generation evidence for any non-saving handoff.
[Repair conditions](MIRRORED_PAYLOADS.md#repair-when-the-selected-image-cannot-append)

## Filtered advisory compaction

The coherence catalog uses the same staged verifier and packed builder with
a filter over byte-bounded posting records. Each read pass visits at most a
quarter of a leaf per opportunity. Each filtered pass keeps its current
path and checked leaf across opportunities. Target comparison also keeps
a bounded cache of checked source paths. Both are discarded before another
verification pass. The planner records the count, lengths
and digest of the selected inventory, then requires the same selection during
the copy. On compact-page sources it prices both index page sizes from the
retained inventory. It reserves write credit for the smaller qualifying
packed target before writing and caps the target extent at that footprint. The
candidate's typed root preserves the catalog identity. The ordinary data
rewrite still uses a new image identity.
[Coherence catalog](OPTIMIZATION_STATE.md#compaction-at-the-append-limit)

## Executed scope

The populated indexed foreground-reconciliation fixture made 3,281 backing
read calls after this change, down from 7,990, and read 3,639,264 bytes, down
from 20,644,702. It exercised the same copy and reconciliation steps,
including late foreground edits and byte-exact reopen. The populated packed
and sparse guests still had pending mounted reclamation. Bounded detached
reclamation published the compacted images and retired the old ranges. The
controller can now continue eligible inactive-range releases. In the
populated guest control, sparse retirement settled while the prepublished
image stayed mounted. Packed retirement advanced farther before detach.
The normal path still needs to select and settle a compact image under churn.
[Read accounting and mounted evidence](evidence/windows-staged-reclamation-reads-2026-11-24.json) ·
[Retirement evidence](evidence/windows-mounted-range-retirement-2026-11-24.json)

The dedicated memory suite checks all three layouts under 4-KiB and 64-KiB
failure contracts, both encoding modes, generic empty/1-MiB objects, all phases,
foreground reads, mutation invalidation, owner retirement, cancellation/lease
release, duplicate work refusal, source/target corruption and every observed
target write/barrier cut in three failure modes. Finish must perform zero backing
I/O. The observed step byte counts are synthetic accounting, not timing or
background-interference benchmarks. The 8-MiB test envelope is an assertion for
these fixtures, not a compression-group or host allocation policy.

[Source-bound validation](evidence/staged-paged-rebuild-2026-11-15.json). Optional
codecs, GPU benefit, mounted scheduling, physical power cuts, payload repair,
crash-resumable progress, paged volume selection/reclamation and automatic
learning remain separate qualification work. The target contains a complete
candidate after publication; interruption may retain that image without success
being returned. The unchanged source remains authoritative until a separate
qualified handoff selects a new image.

Current [SQLite incremental backup source](https://github.com/sqlite/sqlite/blob/master/src/backup.c)
was reviewed via its native repository API on 15 November 2026, file blob
`02791037c06e53d7659fa42846e7db1706156518`. Its bounded step and source/destination
ownership checks inform the decomposition. FlitzFS invalidates changed attempts
rather than silently restarting them. No SQLite code or dependency was copied.
