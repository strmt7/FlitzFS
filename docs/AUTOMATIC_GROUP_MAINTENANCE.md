# Automatic shared-base maintenance

[Reference](reference/README.md) · [Managed paged volumes](MANAGED_PAGED_VOLUMES.md)

## Implemented scope

The experimental `NativeGroupMaintenance` controller discovers related extents
in a managed mirrored paged session and prepares protected
[shared-base groups](SHARED_BASE_GROUPS.md). Callers supply resource observations
and opportunities. The controller uses deterministic CPU discovery and
encoding. A qualified Windows adviser can contribute independently verified
device candidates during preparation.
No thread, timer, online model, driver, mount or service starts implicitly.

An explicit adviser can supply a member grant on an idle opportunity. The
controller stages one decoded member per opportunity. An indexed batch can
collect additional disjoint groups from the current discovery window within the
aggregate member and memory grants. The budget includes each staged base and
any window still held by a consumer.

Each group keeps its own prepared source and revision checks. Advice is matched
by immutable source-window identity. An edit invalidates the affected selection,
and the other groups can still publish through their independent transactions.
While a submitted batch is pending, CPU work can also use independent
candidates from the current ranking after the unsubmitted suffix has drained.
Candidates intersecting the submitted batch stay deferred until it settles.
CPU preparation consumes the staged bytes after an adviser returns or
when advice is unavailable. A complete new group can try the existing
assembled reference and Zstandard prefix methods from the same window
when its live scratch budget can fund them. The verified result with
the smallest complete compacted layout wins. A lost optional grant
releases the alternate candidates and leaves the ordinary group available.
Device and transport admission remain the adviser's responsibility. Volumes
without a qualified adviser use CPU preparation.

With automatic Zstandard storage, a scan can also pack short payloads
missed by similarity search into one protected decode unit. The scanner
uses already sampled lengths and respects the remaining preparation
budget. It keeps AMD reservations disjoint from each CPU pack and retains
the last scan window until its AMD batch settles. A size comparison
declines packs whose complete layout fails to save space.
[Short-file packing](INDEXED_NATIVE_VOLUME.md#packed-slices)

The mixed AMD adviser tests sparse XOR and modular delta for each staged
member in one admitted dispatch. A batch can supply both kinds of advice.
The writer independently decodes each proposed member and compares the
prospective compacted size before publishing the protected group.
[Mixed-codec validation](evidence/amd-mixed-codec-selection-2026-11-19.json)

The Windows diagnostic used the same discovery controller and coalesced AMD
worker to publish five-file groups for synthetic and public-derived inputs.
Its AMD records matched the CPU reference. All files matched byte for byte
after reopening the compacted images. The matched synthetic input occupied
385,024 bytes with accepted GPU advice and 737,280 bytes after CPU fallback.
A live shared-memory budget refusal and a deliberately rejected runtime both
completed with independently verified CPU output. The two successful AMD
trials each peaked below 102 MiB of host process-tree commitment.
[Source-bound discovery and fallback evidence](evidence/amd-discovered-anchor-2026-11-18.json)

One controller owns a weak reservation on the session. Invoke it only on the
session's serialized control context. Session moves preserve that context;
retirement, poisoning, cancellation and image replacement invalidate private
work. Weak ownership does not make destruction concurrent with callbacks safe.

## Discovery and reuse

Each scan advances over one cached native extent at a time. A changed extent is
CPU-decoded and checked before its full digest and eight positional 8-KiB band
digests are recorded. Indexed volumes also record up to four offset-independent
features for matching content after insertions and removals. The sparse inverted
index maps matching features to candidates. Larger buckets are considered first,
with deterministically ordered, memory-admitted member windows.

For a new masked-XOR group with at least three sampled members, discovery
retains the bit patterns from eight complete 256-byte tiles per extent.
The selector compares the estimated dense and sparse body sizes. It
prices each changed plane as a bitmap, a set-position list or a
clear-position list and includes the tile masks. It uses the smaller
estimated body for each peer.

The selector compares every candidate to a bounded set of representative
samples. It then checks the preliminary winner, the dense-plane candidate
and the first member against every retained sample. Sorting equal planes
for the dense candidate takes `O(n log n)` work. The sparse comparisons
take `O(n)` work at the current sampling depth. Retained per-extent data
counts against the maintenance memory budget. Existing protected anchors
and previously retained representatives keep their priority. The earlier
candidates remain eligible if an estimated reference declines publication.
[Sparse reference validation](evidence/sparse-reference-cost-2026-11-19.json)

For a new masked-XOR group, the CPU path can refine its first sampled
reference after staging the entire group within the current memory grant.
It prices one pair of extents per admitted maintenance opportunity. The
reference set includes the sampled choice and evenly spaced alternatives
in stable member order. Its size grows logarithmically with the group.
Pricing compares the complete dense and sparse XOR frame sizes, including
mask and header bytes, without allocating candidate frames. Each member
is then encoded and independently checked by the writer.

The same preparation accepts a verified AMD proposal when it is available.
A declined alternate leaves the sampled reference eligible for its own
transaction. On successful publication, maintenance records the logical
extent that carries the selected reference so a later group window can
reuse its immutable base.
[CPU reference pricing evidence](evidence/cpu-full-reference-pricing-2026-11-19.json)

Band matches are advice only. The existing shared-base encoder independently
checks content and accepts a publication only when its complete prospective
compacted upper size is smaller than the old lower size. This includes shared
anchors, both payload copies, indexes, padding and roots. The indexed writer
can store source-copy instructions for shifted matches and independently
decodes each new member before publication.
[Indexed anchor-copy format](INDEXED_NATIVE_VOLUME.md#anchor-copy-members)

Physical revisions protect selected work against edits. Separate volatile
content revisions allow feature reuse across representation-only rewrites.
Changed content is resampled; unchanged content survives metadata changes and
regrouping. Existing members can be merged with newly related files. Bounded
outcome digests suppress identical successful or declined groups, including the
new physical revisions of a just-published group. An unchanged completed scan
performs no backing I/O. The indexed path can restore a cursor and its bounded
sparse representatives from the [funded advisory area](OPTIMIZATION_STATE.md#indexed-discovery-checkpoints).
An exact completed checkpoint skips resampling. An older incomplete checkpoint
continues to the end before revisiting the prefix. Same-image checkpoints retain
representatives across unrelated transactions. Each reuse checks the live
extent coordinate, object and physical revision. Group preparation uses the
current representation and independently verifies the selected data. Changed
images rebuild discovery from their current index.

## Resource and foreground admission

Observations must be valid, fresh, monotonically sequenced and internally
consistent. Invalid telemetry, inadequate physical/commit headroom, foreground
work, CPU contention, competing maintenance or a failed source defer or stop
work before backing I/O. Invalid telemetry and memory pressure release optional
buffers and indexes. Resource totals are supplied by the host; no machine's
installed RAM is a product assumption.

The default policy reserves 1/16 of reported physical and commit totals, then
admits at most 1/64 of their smaller remaining headroom. The conservative
scratch estimate includes sixteen 64-KiB extents plus eight extents per group
member and a 64-KiB base index for indexed volumes. Expanded indexed history
uses a separate charge within that grant for retained features, bucket
references and checkpoint copies. The controller may admit fewer representatives
as headroom falls. Fewer than two admitted members defers work. Current native limits still bound
the index and group size; these are prototype limits, not intended product caps.
These estimates and observations are not an OS memory reservation.

Fresh idle observation intervals earn work credit, defaulting to 1% elapsed
work share. Actual steady-clock duration is charged, including overruns; a
measured cost estimate determines subsequent deferral and the suggested delay.
An owned caller can send another preparation opportunity from the same CPU
capture with a new opportunity sequence and refreshed memory. The unchanged
capture time earns zero additional credit. Group publication, advisory
checkpoint writes and coherence reclamation require a fresh CPU capture.
Elapsed duration is not process CPU time. Default CPU admission requires at
most 12.5% busy ticks within the supplied observation window. Time only paces
pending work; it never overrides resource or foreground evidence. Each call
performs at most one controller step. Namespace validation can still read the
whole bounded native model, and final publication is synchronous.

The caller must provide trusted real host observations, low OS scheduling/I/O
priority and foreground serialization. The [Windows helper integration](WINDOWS_BACKGROUND_MAINTENANCE.md)
now supplies those observations and opportunities through its owned worker.
Generic core tests still use modeled telemetry. A live small-volume case cannot
establish universal background interference or a hard maximum foreground stall.

## Writes, accounting and failures

Accepted file writes earn advisory checkpoint credit. Each checkpoint consumes
4 MiB of measured durable input for a 128 KiB pair of records. The resource
admission and measured duty budget also cover that publication. A group
publication appends protected history, and actual space reclamation still
needs separately admitted compaction. `minimum_compacted_bytes_saved` sums each
accepted publication's conservative size difference; repeated regrouping means
it is not current image savings or a compression ratio.
`retained_history_bytes_added` reports the corresponding physical growth.
No lifetime wear budget, automatic compaction/handoff or physical drive-wear
qualification is implemented by this controller.

Selected foreground changes invalidate prepared work and trigger reconciliation
against current content. Actual storage/integrity failure fences the source and
stops automatic retries. The independent CPU reader remains the recovery path;
this is not a claim of immunity to all bugs, hardware faults or power loss.

## Validation and next integration

[Source-bound evidence](evidence/automatic-group-maintenance-2026-11-15.json)
records synthetic in-memory Windows CPU checks in raw and AddressSanitizer
builds. Tests cover automatic eight-member discovery, extension to nine members,
unchanged hash reuse, negative outcomes, unrelated data, observation refusals,
ownership and memory pressure, a selected foreground edit during preparation,
and 492 injected publication failures across 4-KiB/64-KiB modeled failure units.
Independent CPU readback checks every original file after each injected case.
This is correctness evidence, not a recognized benchmark or real-data size record.

The Windows helper now integrates observations and low-priority opportunities.
The owned indexed Windows controller uses the reserved advice area for
checkpoints. Grow the persistent sparse postings beyond the current window to
support useful long-range matching, and qualify mounted operation. Measure
write amplification and reader-side cost on varied workloads.
Compare stronger collective representations on held-out heterogeneous data,
including useful actual AMD work and independent CPU decoding. These remain
required product work.
