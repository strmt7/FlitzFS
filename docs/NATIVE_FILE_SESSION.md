# Owning native file-context session

Experimental CPU ownership layer for the Windows filesystem engine.
No driver, dispatcher, mount, helper process, thread or GPU starts here.
This is implemented engine work toward the writable Windows product; actual
writable callbacks, helper application and Windows client/hardware qualification
remain required.

## Ownership and handle identity

NativeFileSession consumes a NativeFilesystemWriter, fully verifies admission,
and refuses unresolved prior-session lifetime state. It exposes no raw writer
accessor. Pathname removal and replacement therefore cannot bypass the owning
layer while its contexts depend on retained data.

A fixed 256-entry table represents open instances (future WinFsp file-descriptor
contexts), not individual Win32 handles. NativeFileHandle is an opaque value
alias for one such instance. Copying it does not create another open or pin.
A successful Close invalidates every alias of that instance. Actual
DuplicateHandle/FILE_OBJECT reference accounting remains the FSD's job.

Each token carries a private weak owner identity and a never-reused descriptor
serial. Closed, default, foreign and destroyed-owner values reject before
backing I/O. Moving the owner preserves identity; move assignment is deleted
so an existing owner cannot be silently replaced. Destruction performs no
native mutation or automatic deletion. The future helper must stop/join its
dispatcher and retire actual external contexts before destroying/replacing an
owner or selecting resume_closed_session.

The fixed table, latch array, counters and writer/owner fields fit in 16 KiB
by a compile-time check. Caller-held token values and external WinFsp context
allocations are separate and must also be bounded by the eventual adapter.
There is no unbounded context map, queue, retry loop, timer or thread.

## Metadata and range I/O

The writer now provides inspect_path, inspect_inode, inspect_directory and
read_inode for its single-control-thread current committed view. A private
checked_namespace verifies the backing namespace record and compares its full
bytes with the previously admitted last-acknowledged model before use.
It exposes no unchecked snapshot.

A range read fully verifies every touched allocated record privately before
returning bytes, preserves sparse zeros and short EOF, and keeps the 4 MiB
read cap. It does not reread unrelated payloads. A namespace mismatch or touched
backing-integrity failure fences the writer/session before subsequent I/O.
Ordinary missing/path/type/size admission errors do not poison healthy state.

All public snapshot/open/resume paths still fully verify every live object,
including detached payloads. Untouched media corruption may remain undetected
until touched or scrubbed, as already documented for incremental staging.
Metadata inspection is not a fresh whole-volume root-health/hardware scrub.
Mutation publication still performs the existing checkpoint-health preflight
and replicated three-barrier protocol.

The session uses these verified calls instead of constructing a full public
snapshot on every callback. Its tests count backing reads for a two-MiB file,
bound metadata/open and a 128-byte cross-extent read, and separately require
full payload verification by a public snapshot. These are synthetic traffic
counts, not timing, storage throughput or GPU performance benchmarks.

## Permissions and namespace operations

Each context records read/write/delete permissions already authorized by its
future OS adapter. Read/list, write/truncate and disposition/rename operations
check those permissions before backing I/O. These fields are not an ACL
implementation or security decision for an unauthenticated application.
The eventual Windows adapter must perform real access checks and preserve
Windows sharing/locking semantics.

create_entry and rename are trusted engine operations. Rename follows the
source inode, so other contexts survive name changes and pathname reuse.
Replacement is rejected while any target context remains, including a cleaned
context that may receive paging I/O. No raw remove API escapes the owner.
The FSD's additional directory-rename/share rules and actual mapped-target
behavior still require Windows tests; the guard here is deliberately
conservative pending that qualification, not a completed compatibility claim.

FILE_DELETE_ON_CLOSE admission, create/overwrite security descriptors, byte
locks, allocation semantics, persistent Windows timestamps/ACLs, cache/flush
integration and cancellation remain unfinished adapter/product work. Do not
claim them from the native context tests.

## Aggregated delete intent and Cleanup

set_delete records a request per open instance and persists the OR of all
requests plus already latched requests for that inode. Canceling one request
cannot erase another's intent. A new open of a pending name is rejected.
A failed intent commit does not advance the in-memory request state.

Cleanup is distinct from Close. A requesting instance cleaned without deletion
latches its already durable request before it can close, so another context
cannot cancel that departed owner's request. A later trusted FSD deletion flag
requires known persisted authorization and atomically detaches the inode.
All contexts retain their IDs and data access after detachment and Cleanup.

Duplicate identical Cleanup is idempotent. Conflicting duplicate flags fail.
Cleanup deletion without persisted owned intent fences the session without
deleting data. Close without Cleanup fails before changes. A final Close
that still has pending intent but never received required deletion Cleanup
fences rather than guessing an unlink from elapsed time or context count.

Existing contexts may perform verified reads/writes/truncation after Cleanup,
including on detached data. Only the last actual owned Close calls
retire_inode. If retirement fails, its context remains recorded and subsequent
I/O is fenced. All fallible work precedes clearing a context; post-success
table operations do not allocate or throw. Retained CPU snapshots keep their
immutable records after logical retirement.

The selected current WinFsp v2.2B5 source contract was freshly compared at:
https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h
It defines per-node versus per-open contexts, Cleanup/Close, later paging I/O,
SetDelete cancellation, flush ordering and operation guarding. This layer
models those persistence/lifetime needs; it is not actual FSD integration.

## Failure and retirement evidence

stats reports bounded in-memory context, cleaned/request/latch counts and a
fence flag without I/O. It is diagnostic, not an independent driver/retirement
attestation or authoritative report of an indeterminate failed commit.

Core I/O/integrity failure retains terminal writer semantics. Table changes
that acknowledge intent, Cleanup or retirement happen only after durable
success. After an indeterminate failure, the helper must quiesce the old
driver/session, independently inspect/recover and create a new owner.
The new owner cannot import unexplained old pending/detached state.

The context table proves counts for contexts it created and still owns.
It does NOT prove that a crashed external process/dispatcher or old kernel
file object is gone. resume_closed_session retains its separate qualified
caller precondition. A timer, fresh telemetry or token from another owner
cannot discharge that obligation.

## Configured tests and bounds

The dedicated session suite covers multi-open rename/path reuse, protected
replacement, permission and kind rejection, stale/foreign/default tokens,
move/destruction, all 256 slots, disposition cancellation and latching,
authorized Cleanup through a different read-only context, late paging I/O,
last-Close retirement, protocol fencing and refusal of old-session state.

It separately corrupts requested data and cached namespace backing, requires
private before-exposure failure and zero retries after fencing, and distinguishes
untouched-record detection from full-snapshot verification.

After acknowledged detachment, a later acknowledged orphan append AND unrelated
overwrite are tested against loss of either current root or either manifest
unit. Recovery must retain both exact latest contents before qualified
closed-session completion, preserve the unrelated data, and continue CPU writes.
This adds no payload replication or whole-device-loss guarantee.

For intent, Cleanup unlink and final Close retirement, fixed tests fault every
observed write/flush under all three existing failure modes, raw and automatic
compression. They require the context/request/Cleanup counts to remain at the
last acknowledged state on failure, complete old/new persistent state, retained
bytes, unrelated data and independent continued CPU writes. Each phase/mode
prints its actual completed case counts for reconciliation.

All context tests run in explicit sequential groups with the existing
45-second per-process limit. Separately, native lifetime tests now use one
phase per process after a passing combined sanitizer group took 43.91 seconds
of its 45-second budget. They retain both modes and all 2,064 cases. The
explicit ceilings are seven lifetime processes (315 seconds) plus four
session processes (180 seconds), within the unchanged hosted job budget.
Actual total work is unchanged by splitting; no fault points are dropped.

Native namespace, object, dirty-buffer, input/read, decoded-live and physical
history caps remain. All builds/tests stay on disposable hosted VMs. Configured
checks are not passing results until exact-commit CI and complete receipts are
inspected. Mounted writes, actual Windows handle/ACL/share/cache behavior,
latest Windows 11 client, physical power cuts and live AMD qualification
remain unfinished.


## Path admission before ownership copies

The context layer reuses canonical_catalog_path, whose current source checks
the 240-byte cap before allocating. Open/create/rename validate the virtual
path before backing lookup and before copying caller views into mutation
strings. Root is allowed only for open. Existing unsafe-name/type rules and
stored format/bounds remain unchanged. Regression cases include the byte
boundary, traversal, reserved names and empty mutation paths, requiring no
backing I/O or healthy-owner fencing. Directory enumeration also has a
positive owned-context identity check.


## Real synchronized Flush addition

The session now exposes file-context and volume forms of a real shared-image
barrier, with exact checkpoint metadata and pre/post namespace validation.
Contexts, requests, latches and detached data remain unchanged. Cleaned contexts
stay valid until Close; stale contexts reject before I/O. NATIVE_FLUSH.md defines
fencing and coverage limits. Actual Windows cached/mapped/write-through/cancel
behavior and the writable callback adapter remain unfinished.
