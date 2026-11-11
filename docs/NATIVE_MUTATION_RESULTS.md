# Commit-bound native mutation results (experimental)

The Windows-only write engine now returns verified inode facts together with
its durable transaction. It does not need another path/snapshot/metadata read
after publication to construct those facts. No writable callback or mount is
enabled.

## Current primary contract

The selected 2026 WinFsp v2.2B5 source was read again through native repository
API captures at 14d6e0280ae15e2550c44d6dbc476be51649727f:

- [Write/SetFileSize API](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
- [Actual success dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c)
- [FileInfo ABI](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/fsctl.h)

The actual Write dispatcher copies the accepted count and FileInfo after a
successful synchronous callback. The selected ABI asserts FileInfo is 72 bytes.
Its fields include allocation size separately from logical file size, four
times, attributes, inode identity, reparse/link/EA information. A future callback
must initialize the complete structure and return truthful qualified values.
The native result here supplies only facts the current engine actually knows;
it does not fabricate allocation reservations, compressed physical size,
security, missing times or unsupported Windows features.

## Captured before publication, returned after success

NativeInodeCommitInfo contains stable inode ID, logical byte size, modified
namespace sequence, the optional stored basic tuple, directory/delete-pending/
unlinked state. It contains no string, owning buffer or pathname. Its size is
statically bounded to 80 bytes and its copy assignment cannot throw.

NativeInodeMutationCommit combines those facts with the existing transaction,
actual selected offset and accepted write count. Truncate returns zero offset/
count. The new write_inode_result/truncate_inode_result methods use the SAME
private staging, validation and replicated publication as existing operations.

After structure validation, the core copies the facts from that private staged
inode. It publishes the transaction through the existing three barriers,
moves the acknowledged model, then copies the bounded facts into its private
return object. That copy and the return moves are statically nonthrowing.
No new allocation, backing query, path construction or clock call occurs
after successful publication to form the result.

A failure returns no result. A verified no-op returns the current acknowledged
sequence and actual unchanged inode facts, not the staging attempt's incremented
sequence. Empty/clipped writes preserve tuple absence and do not stamp. Same-EOF
truncate can apply explicitly requested effects; fully unchanged values do not
publish. All existing admission, append, constrained count, zeroed-tail,
integrity, original-tuple and terminal failure rules remain.

The original NativeFileInfo, NativeInodeWriteCommit, NativeFsCommit and existing
public method signatures/layouts are unchanged. Older write/truncate methods
reuse the new path and move their original result fields without post-commit
allocation. No format, metadata header, capacity or codec changes.

## Owned-context result methods

NativeFileSession::write_result/truncate_result select the owned context and
require data-write access. Trusted automatic effects remain separate from
explicit WRITE_ATTRIBUTES. Results follow the stable inode after rename and
detachment, including cleaned contexts still valid for paging until Close.
They do not release contexts or change per-open delete/latch state.

Returned facts are immutable values for that committed operation. A later
write, Close, retirement or backing-query failure cannot alter them. They are
not an authenticated external handle, current live query, actual driver-context
retirement proof, persistent ACL or complete Windows FileInfo object.

## Runtime denial after the final barrier

The new CPU test backend forwards real bounded RAM operations until the third
publication flush succeeds, then rejects every backing extent/read/write/
admission/flush operation. The noexcept contract declaration is unchanged.
Successful result construction must finish without hitting that denial.

Seven cases per raw/automatic mode cover overwrite, append, constrained write,
truncate, same-EOF requested metadata, identical data and 1 TiB sparse EOF.
Each checks accepted counts, exact stored times/attributes and detached identity,
retained context state, and parity with an independent reopened backend.
A subsequent explicit query is deliberately rejected to prove the denial is
active. That later failure must fence without changing returned facts or
releasing either context; further operations must not retry storage.
Sparse EOF does not allocate its logical length; fixture size remains <4 MiB.

Separate cases cover verified no-ops, legacy absence, invalid clocks and denied/
stale owned contexts. The existing 1,074 append/constrained/truncate fault cases
now call the result API directly and require its successful facts to match the
same committed data/basic/lifetime state. No fault points or modes are removed.
Other masks/ownership cases still use the original API for regression coverage.

Configured tests need exact-commit executed evidence before passing claims.
The denial test proves no post-barrier backend I/O in its covered success paths;
the nonthrowing scalar/move assertions and source structure support the separate
no-allocation return contract. It is not arbitrary-allocation fault injection,
physical power-cut proof or Windows 11 client qualification.

## Remaining writable integration

Real allocation/security, per-open timestamp suppression/policy, complete
FileInfo translation, Create/Overwrite/cache/cancellation, writable callback
routing and supervisor/helper lifecycle remain unfinished. A result with absent
basic metadata must never cause invented original times. A future adapter must
admit all necessary policy/output prerequisites before mutation and retain
failure fencing, not acknowledge a partially populated FileInfo.

All existing process/image/queue/time/memory ceilings and read-only mounted
behavior stay unchanged. No owner-PC execution, driver installation, live AMD
runtime or writable mounting follows from this component. Production capacity/
reclamation, redundant payload repair, automatic low-priority learning and
measured filesystem/GPU benefits remain required.

## Executed hosted checkpoint

At f8cba2874b7d423ad0c044c55d9382330bf7f910 Windows native job
129563658582 passed all 55 entries and raw Windows job 129563658777 passed
all 52. Both emitted all 14 storage-denial cases and 149 result checks.
Each covered success returned complete facts with zero post-barrier backing
I/O, independently reopened parity and terminal behavior for a later query
against the deliberately unavailable backend.

All 1,074 existing raw/automatic append/constrained/truncate fault cases now
exercise the result API directly, with no cuts/modes removed. Their traces
remained 61/59/59 I/O steps per phase, with 183/177/177 faults per mode.
The 264 mutation invariants and older APIs remain regression coverage.
Actual Windows basic API and all nine SDK Cleanup cases passed, with worker
handle totals unchanged at 94 to 94.

Metadata writer job 129563658666 completed 712 real termination cases in two
356-case partitions (39.556/39.463 seconds). Legacy job 129563653928 completed
976 cases in two 488-case partitions (72.006/70.825 seconds). Those existing
campaigns are not silently relabelled as real process interruption of every
new result method. Modeled faults, process kills and physical power cuts
remain distinct.

Relocated native Windows job 129563654541 passed all 52 CTest entries.
No owner-PC operation, live AMD runtime or writable mounting occurred.

Sanitizer job 129563658900 passed all 48 entries, including 14 denial cases,
149 result checks and all 1,074 direct mutation-result fault cases.
Read-only mount job 129566308788 passed actual reads/mapping/enumeration,
write rejection, source parity, confirmed helper termination and independent
CPU recovery/remount. Normal/recovery exits were zero; deliberate kill returned
1460 with exit confirmed. Peaks were 6,176,768 / 6,176,768 / 5,189,632 bytes.
All nine triggered workflows succeeded. These hosted Server results do not
qualify Windows 11 client, writable mounting, live AMD or owner-PC execution.

## Windows space-reporting prerequisite

[WINDOWS_SPACE_REPORTING.md](WINDOWS_SPACE_REPORTING.md) records the selected
FSD compression-information gap and the source-bound live observation gate.
The captured mutation facts remain valid; completing Windows FileInfo still
requires truthful allocation/security policy. Do not derive physical per-file
compression from rounded logical length or silently fill an unsupported field.
