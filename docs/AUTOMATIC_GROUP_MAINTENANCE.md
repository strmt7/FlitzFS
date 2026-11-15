# Automatic shared-base maintenance

[Reference](reference/README.md) · [Managed paged volumes](MANAGED_PAGED_VOLUMES.md)

## Implemented scope

The experimental `NativeGroupMaintenance` controller discovers related extents
in a managed mirrored paged session and prepares protected
[shared-base groups](SHARED_BASE_GROUPS.md). Callers supply resource observations
and opportunities, rather than file lists. The implementation is a deterministic
CPU baseline for collective optimization, not a completed AI learner or GPU path.
No thread, timer, online model, driver, mount or service starts implicitly.

One controller owns a weak reservation on the session. Invoke it only on the
session's serialized control context. Session moves preserve that context;
retirement, poisoning, cancellation and image replacement invalidate private
work. Weak ownership does not make destruction concurrent with callbacks safe.

## Discovery and reuse

Each scan advances over one cached native extent at a time. A changed extent is
CPU-decoded and checked before its full digest and eight positional 8-KiB band
digests are recorded. A sparse inverted index maps matching bands to candidates;
there is no dense all-pairs similarity matrix. Larger buckets are considered
first, with deterministically ordered, memory-admitted member windows.

Band matches are advice only. The existing shared-base encoder independently
checks content and accepts a publication only when its complete prospective
compacted upper size is smaller than the old lower size. This includes shared
anchors, both payload copies, indexes, padding and roots. Hash collisions cannot
authorize incorrect decoding. Aligned bands miss shifted or weakly related
content; this baseline is not an optimal clustering or compression algorithm.

Physical revisions protect selected work against edits. Separate volatile
content revisions allow feature reuse across representation-only rewrites.
Changed content is resampled; unchanged content survives metadata changes and
regrouping. Existing members can be merged with newly related files. Bounded
outcome digests suppress identical successful or declined groups, including the
new physical revisions of a just-published group. An unchanged completed scan
performs no backing I/O. Advice, cursors and outcomes are currently rebuilt on
reopen or resource-driven cancellation, not persisted.

## Resource and foreground admission

Observations must be valid, fresh, monotonically sequenced and internally
consistent. Invalid telemetry, inadequate physical/commit headroom, foreground
work, CPU contention, competing maintenance or a failed source defer or stop
work before backing I/O. Invalid telemetry and memory pressure release optional
buffers and indexes. Resource totals are supplied by the host; no machine's
installed RAM is a product assumption.

The default policy reserves 1/16 of reported physical and commit totals, then
admits at most 1/64 of their smaller remaining headroom for the conservative
scratch estimate: sixteen 64-KiB extents plus eight extents per group member.
Fewer than two admitted members defers work. Current native limits still bound
the index and group size; these are prototype limits, not intended product caps.
These estimates and observations are not an OS memory reservation.

Fresh idle observation intervals earn work credit, defaulting to 1% elapsed
work share. Actual steady-clock duration is charged, including overruns; a
measured cost estimate determines subsequent deferral and the suggested delay.
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

Ordinary accepted input funding is unchanged. Grouping cannot mint journal
credit. A publication appends protected history; actual space reclamation still
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
Connect that path to the synchronized Explorer-facing adapter. Replace whole-model residency with paged
inode/name/extent access and persistent sparse features/restart cursors. Add a
measured data-work/write-amplification policy and reader-safe staged reclamation.
Compare stronger collective representations on held-out heterogeneous data,
including useful actual AMD work and independent CPU decoding. These remain
required product work.
