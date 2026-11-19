# Bounded staged paged rebuild (experimental)

The [mirrored-payload extension](MIRRORED_PAYLOADS.md) retains its protection
during staged copying and independently verifies both new copies. A staged
attempt can reconstruct a lost bank while the source remains CPU-readable.

Paged storage now supports an explicit candidate alongside a live foreground
writer. Copying and full-content verification advance one object at a time.
A separate index phase reads up to one leaf of record headers per opportunity.
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
publication and retirement each start from a fresh CPU capture.

## Work phases

| Phase | Work per advance | Target roots |
| --- | --- | --- |
| Verify source | One complete independently decoded record | Absent |
| Copy | One record encoded or preserved and independently compared | Absent |
| Build index | Up to one leaf of checked candidate headers and the resulting index pages | Absent |
| Verify target | One record compared to its source and generation | Absent |
| Publish | Dependency barrier, two root writes/barriers, independent root reparse | Private complete candidate |
| Verify publication | One record selected through the reparsed target tree | Private complete candidate |
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
Each payload step admits at most one 1-MiB object. Index work follows one
leaf's capacity, and tree finishing follows the admitted depth of 16. The
indexed native path uses the same staged layout with its paged namespace.
Resource admission remains the provider/controller's responsibility. There is
no thread, service, timer, online model or GPU launch. An OS flush can still
block; this is a bounded-work contract, not a wall-clock or zero-interference
guarantee. A qualified controller must supply fresh headroom/contention evidence,
low scheduling/I/O priority and foreground-yield opportunities.

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

## Filtered advisory compaction

The coherence catalog uses the same staged verifier and packed builder with
a filter over byte-bounded posting records. Each read pass visits at most a
quarter of a leaf per opportunity. The planner records the count, lengths
and digest of the selected inventory, then requires the same selection during
the copy. It reserves write credit for a strictly smaller packed target
before writing and caps the target extent at the planned footprint. The
candidate's typed root preserves the catalog identity. The ordinary data
rewrite still uses a new image identity.
[Coherence catalog](OPTIMIZATION_STATE.md#compaction-at-the-append-limit)

## Executed scope

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
