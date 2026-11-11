# Persistent native delete intent and detached inodes

Experimental CPU persistence primitives for the Windows filesystem engine.
They do not implement a Windows handle table, writable callback adapter or
helper application. Those remain required product work. All execution stays
on disposable hosted VMs; no owner-host permission is added.

## State transitions and data ownership

A linked inode normally has a nonzero parent and a visible name. The following
operations use the same native private staging and replicated three-barrier
commit path as file writes:

| Operation | Required state | Durable result |
|---|---|---|
| set_delete_pending(id, true) | Linked non-root inode; directory empty | Name/data retained; pending intent recorded |
| set_delete_pending(id, false) | Linked inode | Intent canceled; name/data retained |
| unlink_inode(id) | Persisted pending intent | Name/parent removed; all extents retained under the same ID |
| write_inode / truncate_inode | Existing regular inode, including detached | Verified atomic mutation under that ID |
| retire_inode(id) | Detached inode AND all caller file contexts retired | Remove live references; retain old physical records/snapshots |

A repeated mark/cancel with the same value and repeated unlink of an already
detached inode are no-ops. A missing/retired inode remains not_found and can
never redirect to a replacement using its former name. Unlink without intent,
retiring a linked inode, canceling an already detached inode and root lifecycle
mutation fail before backing writes. A pending directory cannot receive new
children. Ordinary rename/remove or replacing a pending inode cannot bypass
the lifetime protocol.

The detached node has parent zero, an empty name and an empty stat path. Its
non-root inode ID plus inode_state.unlinked distinguish it from the root.
There is no invented hidden path. Pathname enumeration cannot find it, while
inode I/O and retained snapshots still read its verified bytes. Data changes
through that ID cannot affect a newly created file at its old name.

Retirement is COW reference removal, not physical trimming, secure erasure,
reclamation or destruction of retained snapshot data. The existing history cap
and source-preserving compaction rules still apply. No actual Windows open
context count is inferred by the persistence library.

## Caller responsibilities and Windows integration

The selected current WinFsp v2.2B5 header was fetched again at source commit
14d6e0280ae15e2550c44d6dbc476be51649727f:
https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h

It distinguishes deletion eligibility from Cleanup, forbids deleting during
CanDelete/SetDelete, permits repeated SetDelete changes, and distinguishes
Cleanup from final Close. More I/O can arrive after Cleanup, even for deletion;
Cleanup cannot report failure. These requirements motivate the separate intent,
detach and retirement stages. This mapping is an engineering inference, not
proof of completed Windows semantics.

The future owning engine must retain per-open descriptor identity and counts,
aggregate/cancel disposition requests correctly, admit delete-on-close, reserve
resources before success, handle later paging writes and prove final Close
before retirement. All native transactions remain on one control thread.
WinFsp's coarse operation guard can help serialize the volume; the fine guard
alone permits concurrent I/O across files.

The owning engine must encapsulate the raw writer: unrestricted legacy pathname
remove/replace cannot be mixed with live Windows contexts that depend on
retention. These primitives alone do not enforce that encapsulation. The
driver/helper, Windows metadata/ACL/share/lock/cache/flush behavior and actual
application tests remain unfinished. Do not enable mounted writes from these
unit tests.

## Closed-session recovery

NativeFilesystemSnapshot::inode_state reports pending/detached state and the
ordinary inode metadata. needs_session_recovery reports whether any such state
exists. Public snapshot/open still verify ALL live records, including detached
payloads. Independent CPU inspection remains possible without a GPU or driver.

Ordinary NativeFilesystemWriter::resume rejects unresolved lifetime state
before any native deletion/publication. It does not guess that an old handle
is gone because a name disappeared or enough time elapsed.

resume_closed_session is a separate admission entry point. Its caller MUST
already establish exclusive recovered backend ownership and retirement of ALL
old file contexts, including dispatcher/worker teardown. The API declaration
does not attest that fact. The future helper must supply real lifecycle evidence;
a timeout, caller boolean, PID disappearance alone or fresh telemetry is not
proof.

Under that precondition, admission fully verifies the source, finishes only its
persisted pending/detached deletions in ONE native transaction, verifies ordinary
publication through the object engine, and returns a writer. Namespace sequence
advances once if there was work. Unrelated linked inodes remain unchanged.
An interrupted completion has the complete old or new deletion set; a failed
writer is terminal and must be independently recovered again. A live old CPU
snapshot may retain immutable records, because this operation does not reclaim
their physical storage.

Read-only Windows projection and native compaction reject unresolved states
before exposure/target writes. They cannot silently assume retired contexts,
perform deletion under a read-only source, or present detached storage as normal
namespace data. Native CPU snapshots remain the inspection/recovery route.
There is still no public existing-ordinary-file write factory.

## Internal namespace capability and compatibility

The ordinary format remains FLTXNS01 with format field 1 and zero flags.
Ordinary path/inode I/O that never needs lifecycle state retains those bytes.
The first actual intent/retention transition selects FLTXNS02 and format field 2.
The selection is sticky even after all flags are cleared or retired.

All existing header sizes, inode sizes, extent encodings and limits remain.
The only new inode field is the previously reserved u32 at inode offset 20:

| Bit | Meaning |
|---:|---|
| 0 / value 1 | Linked inode has persistent delete intent |
| 1 / value 2 | Detached inode; no parent/name, payloads remain live |

Unknown bits, both bits together, root flags, flags under format 1, a magic/
format mismatch, named/parented detached nodes and linked children of pending/
detached directories are rejected. All remaining reserved bytes stay zero.
Format 2 requires replicated current checkpoints; a digest-valid downgrade to
historical single-root publication is rejected.

Namespace-format capability is distinct from object root format 2 and is not a
product version/release. Object records/codecs/root publication are unchanged.
Older namespace-v1-only readers reject FLTXNS02 explicitly; use the matching
CPU recovery library. The new reader retains v1 support. Historical
NativeFileInfo, NativeFileMutation and NativeFsCommit layouts remain unchanged.

## Bounds and configured qualification

Detached/pending nodes and their extents count against the SAME 256-inode,
255-nonzero-extent and 16 MiB decoded-live ceilings. The 64 KiB namespace limit,
46,128-byte worst-case encoding proof, 63 dirty buffers, 1 MiB per-write,
4 MiB transaction and 64 MiB physical-history limits remain. The capability
uses existing reserved bytes and adds no per-node stored allocation.

Closed-session completion visits at most 256 nodes, stages metadata and erases
at most 255 payload references plus the one namespace put. It has no data
payload copy, unbounded queue, scan of sparse holes, timer or background worker.
Every actual lifecycle change retains all three persistence barriers.
Advisory learning/GPU work cannot change any of these safety decisions.

The dedicated raw/automatic-codec suite configures tests for intent/no-op/
cancellation, directory eligibility, rename/remove bypass rejection, detached
I/O through pathname reuse, zeroed shrink/regrow, retained snapshots, explicit
closed-session admission, unrelated-data preservation and continued CPU writes.
Digest-valid malformed flags/graphs/capabilities and publication downgrade are
negative cases.

For mark, cancel, detach, detached append, retirement and closed-session finish,
the suite observes every write/flush and injects every boundary under each of
the three existing persistence-failure modes. Recovery must match the complete
old or new state and retain unrelated acknowledged bytes before continuing.
Windows projection tests create separate NEW pending/detached images, verify
them with the CPU reader, and require read-only admission rejection without
altering either source. Existing native, 976-case hosted process, codec,
sanitizer, relocated CPU and mounted read/recovery checks remain required.

These are configured checks until exact-commit hosted results are inspected.
They are not physical power-cut, full Windows handle lifetime, writable app,
latest Windows 11 client or live AMD acceleration qualification.


## Bounded campaign correction

At 40ea0774a5484d3b18f4cca947b0093862104cd2, native Windows and CPU lanes
passed, but ASan/UBSan stopped the combined lifetime test at its 45-second
process limit. No sanitizer error was reported before the timeout; that is
not a complete sanitizer qualification. The gated mount job was skipped.

The same cases now run as four explicit, sequential CTest processes:
invariants/metadata/capacity, mark+cancel faults, detach+write faults, and
retire+closed-session faults. Every fault group still covers raw and automatic
mode, every observed I/O boundary and all three failure modes. Each process
retains the 45-second limit; the explicit suite ceiling is four times 45 seconds,
inside the existing job budget. No production or child-process limit changes.

Each completed phase/mode prints its actual I/O-step and fault-case counts,
requiring exactly three cases per step. The twelve distinct phase/mode receipts
plus the invariants completion must be inspected in the exact-commit CI logs.
There is no skip, timeout widening, dropped data case or unsanitized substitute.


## Owning context layer addition

NATIVE_FILE_SESSION.md now specifies an exclusive native context owner,
per-open delete aggregation/latching, retained post-Cleanup I/O and last-owned-
Close retirement. It does not attest old external/kernel context retirement
or enable mounted writes. Core closed-session admission still requires the
future helper's independent dispatcher/process retirement evidence.

The passing two-phase sanitizer group at a30af8a3ad0d36f506ca302f0e2b60817a45f5ff
reached 43.91 seconds under its 45-second cap. The next configuration gives
each of the SIX phases its own sequential process, retaining raw/automatic
modes and all 2,064 cases. With invariants there are seven processes, each
still limited to 45 seconds; explicit suite ceiling 315 seconds. No production,
child-process or per-test limit widens and no covered case is omitted.
