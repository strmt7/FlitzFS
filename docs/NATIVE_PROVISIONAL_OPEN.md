# Provisional overwrite opens and actual Close dispatch (experimental)

The Windows engine now separates preparation, successful overwrite and trusted
normal use. This implements an ownership boundary required by the selected
driver's two-stage Open/Overwrite protocol. It does not enable writable mounts,
complete FileInfo/allocation, an app or initial delete-on-close overwrite.

## Current primary source evidence

The official native release API on 10 November 2026 still selects WinFsp
v2.2B5 (13 October 2026) as the newest development candidate, explicitly a
prerelease. Exact selected source: 14d6e0280ae15e2550c44d6dbc476be51649727f.

- [Actual kernel create/overwrite and canceled-open Close sequence](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/create.c)
- [Actual DLL Overwrite failure and Close/Cleanup dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c)
- [Kernel Cleanup delivery policy](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/cleanup.c)
- [Kernel close counters and share-access removal](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/file.c)
- [Public callback ABI](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)

The kernel can post Close after Open if overwrite is rejected before delivery
or canceled while pending. When a delivered Overwrite callback fails, the DLL
itself calls Close directly and preserves the original overwrite error.
Neither path invents a user-mode Cleanup callback. The kernel's internal
HandleCleanup argument only adjusts its own counts; it is not delivery of
the user-mode Cleanup operation.

Best-effort requests may never arrive after the queue stops. The helper must
establish actual dispatcher/context retirement before recovery, rather than
infer it from time. PostCleanupWhenModifiedOnly can also suppress normal
Cleanup delivery; the future writable adapter must configure or separately
qualify that policy before relying on active-context Cleanup requirements.

## Bounded in-memory protocol

The new prepare_overwrite method borrows an existing live regular inode. It
requires exactly one separately prechecked overwrite/supersede operation grant,
using the source-verified implicit-access distinction in NATIVE_OVERWRITE.md.
Preparation admits the same fixed context/token limits and performs verified
namespace admission. It publishes no data, delete intent or metadata.

Each new context moves through three private, nonpersistent phases:

1. Prepared: metadata queries can form Open output. Ordinary data, mutation,
   directory, disposition, Cleanup and per-file Flush operations reject before
   backing I/O. Only its matching prepared overwrite can advance it.
2. Overwritten: the ordinary atomic overwrite succeeded and returned captured
   facts. The phase assignment and result return require no post-publication
   query or allocation. Ordinary operations remain blocked; a second prepared
   overwrite is rejected.
3. Active: adopt_prepared explicitly declares trusted normal use. This bounded
   transition performs no I/O. Existing ordinary access and Cleanup/Close rules
   then apply.

Adoption is an adapter declaration, not evidence that a real kernel accepted
an Open. There is no invented acknowledgement callback or timer. The future
adapter must bind the transition to source-qualified real request ordering.
Current tests model that trusted boundary; they do not prove kernel adoption.

Existing open/create methods still produce active contexts and retain all old
semantics. Their public layouts, NativeSessionStats, tokens and wire bytes are
unchanged. The table and other implementation state must still fit 16 KiB.
No new thread, device lookup, worker, queue, clock, allocation or storage reserve
is introduced by phase transitions.

## Healthy cancellation without fabricated Cleanup

Close on a healthy prepared or overwritten context releases ONLY that context.
It performs no backing extent/read/write/admission/flush call, publishes no
deletion and does not undo an overwrite or any later acknowledged peer write.
Aliases become stale and a later open receives a distinct token.

This is possible because preparation publishes no delete intent and because
peer delete requests and deleting Cleanup are excluded while any context on
the inode remains provisional. Replacement already rejects owned target
contexts. A provisional caller cannot request disposition or unlink. Closing
an ordinary peer still counts the provisional owner, preventing premature
retirement. Cached unexpected deletion/phase/lifetime state fences instead of
clearing a context.

After adoption, Close still requires real Cleanup. A missing active Cleanup
does not become synthetic success. Storage corruption, failed publication,
explicit fencing or unavailable backing discovered elsewhere causes Close to
retain ALL remaining contexts and perform no retry. Healthy cached state is
not a fresh media-integrity scrub; cancellation itself acknowledges no data.

## VOID Close bridge

dispatch_native_close catches every engine exception, fences and returns a
bounded outcome. A future actual VOID callback must record that outcome and
signal supervisor offlining outside the callback. This helper never joins or
stops a dispatcher, releases a poisoned context, queries devices or retries I/O.

The existing read-only callback table is unchanged. The new helper is an
independently tested component for later writable binding, not a complete
writable table, driver installation or helper lifecycle.

## Configured engine and actual SDK tests

Three CPU CTest groups cover:

- Both attribute modes under raw/automatic and fixed/compact layouts; metadata
  queries before adoption; denied ordinary operations before/after overwrite;
  mode/time/phase checks and explicit activation.
- Healthy cancellation before overwrite and after success, including a later
  acknowledged peer write that cancellation must preserve.
- Peer deletion exclusion and later release, stable rename after adoption,
  stale/foreign tokens, 256-context saturation and bulk I/O-free cancellation,
  unstamped legacy failure and unchanged named contents.
- Eight sealed backend cases: all backend calls are denied after the third
  publication barrier. Result/phase return, cancellation or adoption must finish
  without I/O. A later explicit query proves denial, fencing and retained
  ownership.
- Every observed write/flush failure in all three existing fault models for
  each mode/layout and attribute policy, followed by independent CPU recovery
  and a newly prepared/adopted continuation. Older fault groups remain intact.

The hosted-only Windows SDK group invokes the ACTUAL selected DLL functions
FspFileSystemOpOverwrite, FspFileSystemOpClose and FspFileSystemOpCleanup. Its
32 raw/automatic and attribute-mode cases check canceled Open, bad clock,
unstamped inode, unsupported test allocation, first-write failure, successful
pre-adoption cancellation, adoption with real Cleanup, and rejected active
Close without Cleanup.

In failed Overwrite cases the DLL must call Close exactly once, call Cleanup
zero times and preserve the original status. Healthy failures retire without
storage I/O; publication failures retain the fenced context and produce a
supervisor-visible outcome. Success output uses a synthetic zero-allocation/
zero-EOF fixture to verify ABI copies only; it is NOT a production FileInfo or
allocation implementation. No filesystem object, dispatcher, mount or driver
is created in this SDK group.

All old tests, process campaigns and 45-second test ceilings remain. Configured
checks require exact-commit executed results before passing claims. The user-mode
DLL test does not execute kernel cancellation, prove Windows 11 client behavior,
qualify physical power loss or establish a no-freeze guarantee.

## Remaining integration

The new protocol is existing-inode overwrite preparation only. Initial
FILE_DELETE_ON_CLOSE/other dispositions must not be silently mapped to it:
atomic initial intent and correct trusted grant/lifetime semantics remain
required. Existing new-file creation has its separate intent behavior.
Allocation/full FileInfo, general Create failure/adoption, actual kernel
concurrency/cancellation, cache/mapping policy and helper offlining still need
integration and qualification before writable routing.

The active objective still includes production capacity/indexing/reclaim,
live AMD acceleration with independent CPU recovery, payload redundancy/repair,
bounded low-priority adaptive learning, installation/update/rollback, latest
Windows 11 client and representative filesystem benchmarks. No owner-PC
operation is authorized.

## Inspected hosted engine and SDK checkpoint

At 51e014fb990af3e020305735b29021f524519770, native Windows job
129678108850 passed all 83 CTest entries. It emitted 468 new invariant checks,
eight sealed backend cases and all 828 new modeled failures across eight
complete mode/layout/attribute traces. The actual user-mode SDK group
completed all 32 cases and 97 assertions. Worker handles remained 94/94.

Raw Windows job 129678109208 passed 78 entries. These are exact-commit engine
and user-mode SDK executions. They do not qualify actual kernel cancellation/
adoption, writable mounting, initial delete-on-close overwrite, allocation/
full FileInfo, Windows 11 client, physical power loss or live AMD.

Sanitizer job 129678109184 passed all 74 entries at the same source, retaining
468 checks, eight sealed cases and every one of the 828 new faults.
Relocated static Windows job 129678113613 passed 78 entries. No old case or
test/resource ceiling changed. Read-only mounted job 129680967465 also passed normal operation, confirmed
helper kill and independent recovery/remount. All three exact-commit space
reports reconciled. All eleven triggered workflows succeeded. The mounted
fixture remains the unchanged default-layout read-only regression; it is not
actual provisional kernel cancellation/adoption, writable, NS04 ACL, Windows
11 client, physical power-loss or AMD evidence. No owner-PC operation occurred.


General prepared Open/Create and separately authorized initial delete-on-close
now extend this existing-overwrite protocol in NATIVE_INITIAL_DELETE.md.
Its configured tests require new exact-commit inspection; this does not close
the real writable kernel/adoption/allocation/FileInfo/client gates.
