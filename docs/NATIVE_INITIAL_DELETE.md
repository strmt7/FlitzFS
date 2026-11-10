# Initial delete-on-close and general prepared opens (experimental)

The Windows engine now models initial FILE_DELETE_ON_CLOSE independently from
explicit SetDelete and final GrantedAccess. It becomes effective at real
Cleanup, preserving canceled Open/Create and nonempty directory contents.
No writable callback table, volume or helper app is enabled by this component.

## Current primary evidence

The official native release API was rechecked on 10 November 2026: newest
development candidate remains WinFsp v2.2B5, published 13 October 2026, explicitly
a prerelease. Selected immutable source:
14d6e0280ae15e2550c44d6dbc476be51649727f.

- [Kernel initial flag and pending/deletion calculation](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/file.c)
- [Create completion and cancellation](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/create.c)
- [Cleanup delivery and actual request fields](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/cleanup.c)
- [Disposition cancellation and callback-elision policy](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/fileinfo.c)
- [Actual user-mode Cleanup/Close dispatcher](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c)
- [Reference preservation of nonempty directories](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/memfs/memfs.cpp)
- [Current create-related initial-option fixture](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/winfsp-tests/create-test.c)

The kernel stores DeleteOnClose in FileDesc at Create completion. It first
sets node delete-pending from that flag in FspFileNodeCleanup, then requests
actual deletion on the last HANDLE Cleanup (or separately qualified POSIX
semantics). Cleaned but not closed paging contexts still count as open contexts;
the engine must not infer Delete solely from its own context count.

FspFsvolSetDispositionInformationFlags can clear current pending state.
It does not clear a still-open descriptor's initial option, which can re-arm
at that descriptor's later Cleanup. FILE_DISPOSITION_ON_CLOSE is explicitly
unsupported by the selected FSD; it is not this initial Create option.

PostDispositionWhenNecessaryOnly can elide the user-mode SetDelete callback
for regular files. The future writable adapter must disable or separately
qualify that policy, alongside PostCleanupWhenModifiedOnly and stopped-queue
retirement. The complete shared-disposition/kernel policy still requires live
conformance; engine tests do not prove an unconfigured callback route correct.

The guessed delete-test.c path returned native HTTP 404. The complete pinned
tree had no such file; create-test.c was located and read instead. Missing
content supports no behavior claim. Source references above are actual native
captures, not search snippets or model-memory assumptions.

## General preparation without early deletion

NativeInitialDeleteAccess is a separate already-checked initial-option grant.
The adapter must bind it to successful FSD authorization and actual request
options; a raw option or ordinary DELETE bit alone is insufficient. The flag
does not grant explicit SetDelete, metadata mutation or data access.

prepare_open adds a general opened phase to the existing provisional protocol.
It permits metadata queries, I/O-free healthy cancellation, and explicit trusted
adoption without requiring an overwrite. prepare_overwrite has an overload
carrying the same initial grant; overwrite remains atomic and must finish
before adoption. Root initial deletion is rejected.

create_prepared publishes a NEW inode/basic/security tuple, returning an opened
context with the initial option in memory only. Healthy cancellation preserves
the created name, matching the reference's independent Close behavior. A
reported Open failure can therefore leave a complete unacknowledged creation;
it must not trigger speculative deletion. Original acknowledged unrelated
contents remain intact.

The historical NativeSessionCreateOptions::delete_on_close option and
create_open API retain their already-qualified EXPLICIT durable initial-intent
behavior and bytes. They are NOT the direct mapping of FILE_DELETE_ON_CLOSE.
The new NativePreparedCreateOptions makes that distinction explicit without
changing old public layouts, cases or wire encodings.

Adoption remains a trusted adapter declaration, not kernel acknowledgement.
Open/creation/overwrite/adoption never persists this new initial option.
A canceled or interrupted request before actual Cleanup cannot cause later
closed-session recovery to delete an existing file from the option alone.

## Atomic Cleanup and directory preservation

cleanup_initial_delete_result uses the SAME private Stage and replicated
publication. It applies only requested automatic metadata and evaluates the
current stable inode. For a regular file or empty directory it arms existing
pending state and optionally unlinks in that ONE transaction. Captured bounded
facts return after success without a backing/path/allocation query.

If other handles remain and Delete is false, the session retains the new
pending authority in its bounded latch table. Closing the initial descriptor
does not release that authority. Last trusted Cleanup can unlink; final owned
Close still waits for all retained paging contexts before retirement.
A subsequent confirmed closed-session recovery can finish this DURABLE
eligible intent. This is the engine's explicit recovery contract, not a claim
about physical power persistence or every other filesystem.

A nonempty directory is retained, as in the selected reference. No unsafe
durable pending flag is created for a directory whose children must survive.
Requested times may still publish. The session records a runtime-only initial
pending latch and reports it through owned info; independent disk snapshots
retain the actual nonpending directory/children and need no destructive
closed-session recovery.

That volatile latch rejects new opens, child creation and destination renames.
Previously owned child contexts can still complete authorized deletion.
If the directory becomes empty, last Cleanup can atomically unlink it. If it
remains nonempty, last Cleanup preserves it and clears the runtime latch.
No child is discarded to satisfy delete-on-close.

These distinctions preserve old snapshots and CPU recovery. A runtime pending
overlay is not an on-disk flag; a disk snapshot must not be substituted for the
owning session's live admission policy. No header/capability/codec/digest change
is introduced.

## Cancellation provenance and nonthrowing ownership

The fixed latch table now records explicit, initial and volatile provenance in
one bounded byte per slot. Explicit cancel removes current INITIAL pending
authority only after successful backing publication/no-op verification.
It leaves still-unprocessed per-open initial options intact, and preserves
independent explicit requests/latches under the historical owning-session
aggregation contract. That historical aggregation is not a claim of complete
global FSD/NTFS shared-disposition conformance.

All latch capacity is reserved before Cleanup publication. Captured result
facts determine durable versus volatile state; nonthrowing bounded assignments
update latches and cleaned state only afterward. Failed publication preserves
contexts/latches and terminal fencing. The VOID Cleanup bridge still returns
a supervisor-visible outcome; Close never drops poisoned ownership or retries.

The table, provenance bytes, counters and writer still must fit 16 KiB.
The 256-context/node, metadata/extent, history and every existing resource/time
ceiling remain. No worker, timer, background task or GPU initializes.

## Configured verification

Five CPU groups cover general/prepared Create/overwrite cancellation, initial
versus explicit grants, single/multiple handles, directory preservation and
later emptiness, cancellation before/after effective pending, mixed latch
provenance, duplicate Cleanup stamps, blocked destinations and runtime-crash
recovery.

Eight sealed cases deny EVERY backend operation after the third barrier while
eligible intent or nonempty-directory metadata commits. Latch/cleaned updates
must finish without another query. Separate arm, unlink, retain and final-dir
groups inject all three old failure models at EVERY observed write/flush step
under raw/automatic and fixed/compact layouts, then independently recover and
continue with CPU code. All old groups/oracles remain.

The hosted SDK group uses actual FspFileSystemOpCleanup/Close and the production
VOID bridges for 22 raw/automatic cases: eligible single/pair/empty-directory,
nonempty direct/pair preservation, canceled Open/Create, failed and unauthorized
Cleanup, and initial overwrite/supersede. These calls test user-mode ABI and
engine behavior; they do NOT execute real kernel FILE_DELETE_ON_CLOSE requests.

Configured checks require exact-commit executed inspection. Real writable
Windows 11 client/kernel cancellation, shared disposition, policy flags,
allocation/full FileInfo, driver/helper offlining, live AMD, scale/reclaim,
payload repair and low-priority learning remain required. No owner-PC
operation is authorized.
