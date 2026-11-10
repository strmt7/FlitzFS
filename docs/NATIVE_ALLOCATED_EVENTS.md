# Atomic allocation events (experimental)

This Windows-only engine component combines native live-extent reservations
with Create, Overwrite and trusted Cleanup. It does not preallocate host disk
space or enable a writable callback table, driver/helper product or PC test.

## Current primary evidence

The current native release inventory still selects WinFsp v2.2B5, published
13 October 2026, immutable source 14d6e0280ae15e2550c44d6dbc476be51649727f.
The release API and these selected native source bytes were reviewed again:

- [Actual Create/Overwrite/Cleanup dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c)
  forwards allocation in Create/Overwrite and trusted Cleanup flags. Cleanup
  is VOID and the dispatcher returns success even when the engine must fence.
- [Kernel Cleanup policy](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/file.c)
  requests allocation trim only for the last HANDLE Cleanup, with
  TruncateOnClose and without delete-pending. User-mode context counts alone
  must not synthesize this event.
- [Cleanup request delivery](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/cleanup.c)
  carries that decision plus metadata effects; stopped-queue delivery remains
  best effort. No engine can stamp or trim a callback never delivered.
- [Reference implementation](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/memfs/memfs.cpp)
  applies the requested allocation during creation/overwrite and trims to
  aligned EOF at Cleanup. Its RAM allocation is not this store's physical model.

NATIVE_ALLOCATION.md retains the separate native-capacity versus physical/
Windows reporting boundary. Returning reserved native slots as measured host
clusters or compressed physical allocation would be incorrect.

## One transaction for each event

create_inode_storage_result accepts an optional aligned regular-file reservation
alongside canonical basic metadata and an opaque framed descriptor. All publish
in the SAME private Stage and three barriers. A present zero enables tracking;
absence preserves legacy behavior. Directories require absence. Invalid
alignment/capacity/kind rejects before backing I/O. Context capacity and token
admission precede creation; create_prepared_storage installs only after success.

The prepared creation keeps initial delete-on-close in memory only. Canceled
Close neither queries nor mutates storage, rolls back a creation, releases its
reservation or speculatively deletes it. A complete unacknowledged creation may
persist after a failing final barrier; recovery must preserve unrelated
acknowledged bytes and admit a complete old or new namespace.

overwrite_inode_storage_result combines the existing regular-file truncate/
attribute/time event with optional requested reservation replacement. Creation
time, descriptor and stable identity remain exact. Absence retains the existing
reservation; a present aligned zero releases it. Old overwrite APIs keep their
historical behavior. The separately checked overwrite/supersede grant remains
required even when final ordinary WRITE_DATA or DELETE is absent. Provisional
adoption/cancellation and poisoned ownership rules remain unchanged.

NativeCleanupEvent is separate from the historical metadata type/layout.
cleanup_event combines requested metadata, optional tracked-reservation trim
to aligned current EOF, and the existing optional unlink policy in one Stage.
Trim rejects an untracked regular file rather than inventing an allocation
contract; directories have no reservation and trimming them is a no-op.
Creation time, unrequested fields, all bytes and nonempty children stay intact.

Duplicate successful Cleanup must match delete, metadata request bits AND trim.
A later sampled clock cannot replace the first successful stamp. Conflicting
duplicates reject. Latches/cleaned/trim state change only after durable success;
all capacity is preflighted and these updates cannot allocate or throw.
Late paging ownership remains until Close; later writes can grow a reservation
again. Final context retirement and physical/history reclamation stay distinct.

## Captured storage output

NativeInodeStorageMutationCommit returns bounded verified inode facts, optional
reservation, accepted offset/count and transaction for write/truncate.
Create/overwrite return NativeInodeAllocationCommit, and prepared creation adds
its owned handle through NativeSessionStorageCreateCommit. Facts are captured
before publication and returned with nonthrowing scalar/optional/move operations.
No post-commit path/backing/clock query forms them. Original result layouts and
APIs remain; their wrappers discard only the added facts without new I/O.

The existing 16-KiB session implementation, 256-context table, 64-KiB metadata,
255 live/reserved extent, decoded/dirty/transaction/history and all process/time
bounds stay. A reservation optional costs at most 16 bytes per inode/model;
the new storage facts are at most 96 bytes. No format, codec or product version
changes beyond the already implemented opt-in FLTXNS05 capability.

## Explicit unwired SDK bridge

dispatch_native_cleanup keeps rejecting allocation flags, preserving old callers
and tests. The separate dispatch_native_allocated_cleanup admits trim only for
an explicitly selected tracked engine; unsupported flags or impossible combined
Delete+SetAllocationSize fence before clock/I/O. Unknown/untracked regular trim
or engine failure fences without retry/release. Directory trim needs no field.
The new bridge remains UNWIRED from the read-only filesystem callback table.

A future supervisor must offline outside the callback; never stop/join the
dispatcher inside its own VOID Cleanup. This bridge alone is not authorization
to acknowledge Windows allocation or enable a writable route before the actual
backing/FileInfo/cache/driver/helper/client contract is qualified.

## Configured verification

Five core groups cover creation, overwrite/supersede, trimming, capture, old
snapshots, cancellation, grants, admission and capacity. Four fault groups cover
all observed writes/flushes for create, merge, supersede and trim, under
raw/automatic and fixed/compact layouts and all three existing failure modes.
Recovery requires complete old/new data/EOF/basic/security/reservation state
and independent CPU continuation. Existing tests and 45-second limits remain.

Twenty-four sealed cases cover Create, both overwrite grants, Cleanup trim and
write/truncate capture across both modes/layouts. Every backend operation is
denied after the final barrier. Return/latch updates must finish without I/O;
a later query proves denial and fencing without ownership release.

The new hosted-only SDK group uses actual FspFileSystemOpCleanup/Close exports.
Two cohorts cover raw/automatic trim, trim-only clock avoidance, duplicate first
stamp, nonempty directory retention, untracked/failed-engine fencing and the
source-derived impossible flag combination. It checks backing ownership expires
after every complete fixture destruction and OS handle counts match after
repeated use. This is evidence for those resources/cases, not universal
heap/VRAM/driver leak freedom. No process, service, scheduled task, volume or
driver is created by this SDK test.

Configured tests are not executed results. All model failures, old writer-kill
campaigns, physical power loss and actual Windows 11 client/kernel lifecycle
qualification remain distinct. Full FileInfo/physical allocation, writable
helper/driver integration, scalable reclaim/repair, live AMD acceleration and
bounded adaptive maintenance remain required.
