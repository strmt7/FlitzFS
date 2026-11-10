# Atomic existing-inode overwrite (experimental)

The Windows engine now has an explicit overwrite event: truncate an existing
regular file to zero and publish its requested attributes and timestamps in
the SAME durable transaction. It preserves inode/name, creation time and the
existing security descriptor. This is engine integration work; allocation,
complete FileInfo and writable callbacks/helper remain unfinished.

## Current primary sources and an important authorization distinction

On 10 November 2026 the official native release API again confirmed WinFsp
v2.2B5, published 13 October 2026, as the newest candidate. Its prerelease
status remains explicit. The selected source is immutable commit
14d6e0280ae15e2550c44d6dbc476be51649727f.

- [Overwrite callback contract](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
- [Actual access checking and Open/Overwrite dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c)
- [Current reference overwrite policy](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/memfs/memfs.cpp)
- [Allocation and basic-information handling](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/fileinfo.c)

FspFileSystemOverwriteCheck checks implicit FILE_WRITE_DATA for overwrite or
DELETE for supersede. It may then REMOVE those implicit rights from the returned
GrantedAccess when the application did not request keeping them. Therefore
requiring the final data-write/delete bits would incorrectly reject an already
authorized overwrite. Conversely, an ordinary write/delete/WRITE_ATTRIBUTES
grant alone does not identify an authorized overwrite request.

The new NativeOverwriteAccess records separate already-checked overwrite and
supersede operation grants. The six-argument session open overload accepts
them; all historical open overloads and new creation contexts default to neither
grant. The future adapter must derive them from the verified successful driver
request/disposition route. This pure CPU component performs no ACL or privilege
check and cannot attest that a caller actually traversed that route.

The selected reference implementation truncates EOF, merges or replaces
attributes plus archive, updates access/write/change from one clock, and retains
creation time. That is the explicit event policy implemented here, not a claim
of every NTFS/Windows application behavior. No reference allocation/EA/stream
code or platform access-check algorithm is copied into the engine.

## One staged data and metadata event

NativeOverwriteMetadata supplies one explicit nonnegative 100-nanosecond UTC
value, canonical request attributes and replacement/merge selection. Zero is a
real supplied UTC value, never a sentinel. Windows NORMAL and request sentinels
must be normalized at the actual OS boundary; unknown/unsupported attributes,
DIRECTORY and negative time bit patterns reject before backing I/O.

overwrite_inode_result resolves the stable live regular inode and requires
present original basic metadata. It never invents original times for an
unstamped legacy inode. Pending/detached inodes and directories are rejected.
Within the existing private Stage it truncates EOF to zero, removes live extent
references, and updates:

- Attributes: request OR existing OR ARCHIVE for merge; request OR ARCHIVE for
  replacement.
- Last access, last write and change times: the one supplied UTC value.
- Creation time and the entire optional security descriptor: preserved exactly.
- Inode identity, parent and name: preserved; modified sequence records the event.

An explicit overwrite publishes all THREE replicated barriers even for an
already empty file with identical fields and the same sampled clock. It does
not reduce the event to a size-only no-op. Old snapshots retain old bytes;
regrowth through any still-owned handle exposes zeros. Payload history is not
silently reclaimed or overwritten.

The existing NativeInodeMutationCommit returns zero accepted count/offset and
captured stable facts from the validated Stage. Those facts are copied before
publication and returned afterward using the existing nonthrowing result path.
No post-commit path, allocation, storage or snapshot query forms the result.
All historical write/truncate/create APIs, result layouts, formats, codecs and
resource ceilings remain unchanged.

## Owned session and failure behavior

NativeFileSession::overwrite_result requires the matching independent operation
grant. Data/basic/security grants do not imply it, and it does not imply those
grants for later ordinary operations. A supersede-style replacement requires
the separate supersede grant; merge requires overwrite authority. Invalid,
closed, foreign or cleaned contexts reject before mutation. Pending/unlinked
state also rejects. The operation follows the stable inode after a rename,
never a new same-name inode.

Success and failure retain all owned contexts, requested dispositions, Cleanup
state and latches. Publication failure returns no result and fences through the
existing terminal/indeterminate rules. Independent CPU recovery accepts either
the previous state or the complete new EOF/tuple; unrelated acknowledged bytes
must remain intact. An unacknowledged complete persisted overwrite is possible
when a final flush reports failure; recovery must not fabricate success or
silently mix its metadata with old payload references.

## Required adapter lifecycle follow-through

The actual selected DLL handles overwrite in two stages: Open then Overwrite.
If Overwrite fails, FspFileSystemOpOverwrite calls Close DIRECTLY, without a
Cleanup callback. The existing session Close requires Cleanup and intentionally
retains contexts on failed publication. A future writable adapter must therefore
qualify provisional-open/error retirement and supervisor offlining before
wiring these calls. Faking Cleanup, dropping a poisoned context, or treating
Close as proof that all kernel/helper contexts retired would be incorrect.

The new operation does not implement AllocationSize, OverwriteEx EAs, streams,
full FileInfo, complete Create disposition mapping, sharing/caching/mappings,
cancellation or the helper lifecycle. No unsupported allocation reservation is
acknowledged and no callback table or mounted-write route is enabled. This
preserves the full Windows product requirements rather than presenting a
partial core result as a complete Windows operation.

## Configured regression and failure matrix

Three bounded CTest groups exercise actual production methods:

- Raw/automatic and fixed/compact operation, both attribute modes, absent/present
  descriptors, preserved creation/security/identity, still-open peer handles,
  stable rename, zero UTC and repeated already-empty publication.
- Read-only final access with a separately checked overwrite grant, denial of
  ordinary data/basic mutation and the wrong overwrite mode; ordinary broad
  data/delete/basic/security grants without overwrite authority remain denied.
- Invalid clocks/attributes/types/IDs, legacy tuple absence, cleaned/stale
  contexts, pending/unlinked state and zeroed post-overwrite regrowth.
- Eight sealed cases block EVERY backend operation after the third barrier.
  The result must return complete facts with contexts retained; a later query
  proves the denial is active and fences without retry.
- Separate merge/replacement groups inject all three existing failure models
  at EVERY observed write/flush point in each raw/automatic, fixed/compact
  profile. Independent recovery checks old-or-new closure, preserved unrelated
  bytes and resumed CPU continuation.

All old tests, process campaigns and the 45-second per-group ceiling remain.
Configured groups need inspected exact-commit execution before passing claims.
Modeled RAM failures and old termination campaigns are not new overwrite/NS04
writer-kill proof, physical power cuts, writable mounting or Windows 11 client
qualification. Owner-PC execution remains forbidden.

Production capacity/indexing/reclaim, live AMD/CPU recovery integration,
redundant payload repair, bounded automatic learning and representative
filesystem benchmarks remain required parts of the active objective.

## Initial exact-commit execution

At 6574049cb0e7534eba83b5560cacb7b6741c38e6, native Windows job
129661803043 passed 79 CTest entries, raw Windows job 129661803137 passed 75,
and sanitizer job 129661802988 passed 71. Each inspected overwrite matrix
completed 412 invariant checks, eight sealed cases and all 828 fault cases
across eight raw/automatic, fixed/compact and attribute-mode traces.
Worker handles remained 94 before and after testing.

This initial commit used LZ4 1.11.0. Its separate dependency inventory job
129661802483 correctly failed after the new 1.11.1 release. That is retained
as a real current-release-policy failure, not relabelled as a fully green
checkpoint. DEPENDENCY_AUDIT.md records the reviewed immutable upgrade and
the requirement to inspect new exact-commit compatibility executions.
