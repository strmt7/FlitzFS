# Bounded staged paged rebuild (experimental)

Paged storage now supports an explicit candidate alongside a live foreground
writer. Copying and all full-content verification advance one object at a time;
finishing does not hide a volume scan. Native wrappers preserve each of the
three existing namespace layouts and rebind only the new image identity.
This is a maintenance primitive, not an automatic scheduler, active-volume
handoff, persistent restart journal or collective compression algorithm.

## API and ownership

`PagedObjectStoreWriter::prepare_rebuild` creates one owned candidate for that
writer. `Rewrite::advance(current_writer)` performs one bounded opportunity;
`finish(current_writer)` returns the completed new writer. Generic rebuilding
does not rewrite user object bodies. The native wrapper uses a private,
identity-only root replacement through `prepare_paged_recompression`,
`advance_paged_recompression` and `finish_paged_recompression`.

The source writer keeps its existing exclusive lease; the candidate owns a
separate new-target lease. A weak source-owner token and cached checkpoint
identity/generation guard every advance and finish before I/O. A committed
foreground mutation invalidates the candidate, including after private target
publication. Reopening the same source creates a new owner and cannot revive
an old attempt. Moving writer/candidate objects preserves the original owner.
This guard is currently volume-wide: an unrelated committed change also aborts
the candidate. Continuous foreground writes can therefore prevent completion.
Per-group generation checks, reconciled snapshots and persistent resumable work
are future mechanisms; automatic retries must not turn invalidation into repeated
large writes or capacity pressure.
Only one candidate per writer is admitted; duplicate preparation refuses before
backing I/O. Cancellation or terminal failure releases its reservation and
target lease. Partial target bytes remain owned by the provider, never deleted
or selected automatically.

All calls remain serialized on one control thread. The backend must qualify
exclusive, nonaliasing storage and stable successful-write/flush semantics;
per-instance leases do not stop another facade or hostile provider. Native
preparation rejects pending/detached file contexts, an invalid source/backend
or invalid identity. Flat managed-session/head handoff remains separate.

## Work phases

| Phase | Work per advance | Target roots |
| --- | --- | --- |
| Verify source | One complete independently decoded record | Absent |
| Copy | One record encoded/preserved and independently compared | Absent |
| Verify target | One record compared to its source and generation | Absent |
| Publish | Dependency barrier, two root writes/barriers, independent root reparse | Private complete candidate |
| Verify publication | One record selected through the reparsed target tree | Private complete candidate |
| Ready | Finish transfers the verified writer; no backing I/O | Complete candidate |

Phase transitions process no extra record. They check aggregate inventory,
finish bounded tree levels or initialize the fixed 128-KiB root area. Initial
source verification completes before any target write. Each root replacement
must match the independently read source root except its 16-byte identity.
Data/metadata generations remain unchanged; image publication advances by two.
Compaction preserves non-root encoded payloads; recompression uses the supplied
bounded CPU policy. Existing synchronous copy APIs share the record-copy body.

Target extent/contract drift, integrity failures and indeterminate publication
terminally discard the attempt. Newly degraded metadata refuses publication;
root-slot degradation refuses success. After publication, a separate incremental
CPU pass checks every selected target record. A final source-root/contract/extent
check precedes Ready. `finish` uses those results and, for native storage, copies
the already admitted bounded Model with the new identity; it performs no new
backing reads, writes or flushes. API return/exception establishes the outcome;
an absent/moved/finished candidate has no usable progress and reports the
current API's `failed` phase.

The engine retains one unfinished leaf and bounded references per tree level,
not a volume-sized inventory. Object work is bounded by the existing 1-MiB
object cap; tree finishing by the admitted depth of 16. The native frontend
still has its complete 256-node/255-extent Model. No million-file claim follows.
Resource admission remains the provider/controller's responsibility. There is
no thread, service, timer, online model or GPU launch. An OS flush can still
block; this is a bounded-work contract, not a wall-clock or zero-interference
guarantee. A qualified controller must supply fresh headroom/contention evidence,
low scheduling/I/O priority and foreground-yield opportunities.

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
