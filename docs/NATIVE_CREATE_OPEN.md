# Atomic native creation and open ownership (experimental)

The Windows engine can now create a NEW file or directory and return its owned
open context in one operation. This fills the creation/ownership gap before
writable callback integration. No writable callback, dispatcher, mount, driver
installation, app lifecycle or owner-PC operation is enabled.

## Current primary contract

On 10 November 2026 the official native release API still listed WinFsp v2.2B5,
published 13 October 2026, as the newest candidate; it remains a prerelease.
The selected immutable source is 14d6e0280ae15e2550c44d6dbc476be51649727f.

- [Create callback and granted access](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
- [Actual create/security/result dispatcher](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c)
- [FileInfo output ABI](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/fsctl.h)
- [Actual allocation and basic-information handling](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/fileinfo.c)

The selected Create dispatcher performs create access checking and constructs
the inherited self-relative descriptor before calling the user-mode Create
callback. On success it copies context and FileInfo into the response. The
native API documents GrantedAccess as already checked. Inference: the engine
must admit its context before durable creation and finish its own return without
a fallible post-publication open/query. It must not reinvent the driver's access
checks or claim those checks execute in this pure CPU component.

The current basic-information source also normalizes attributes and translates
-1 time requests before the callback. Future Windows time policy must follow
the actual FSD contract; adding a competing guessed per-open interpretation is
not part of this change. Allocation, full FileInfo and OS client qualification
remain separate requirements.

## One staged durable creation

NativeFilesystemWriter::create_inode_result requires a new valid name, an
explicit canonical basic tuple and a bounded framed full security descriptor.
It returns NativeInodeCreateCommit: the existing transaction result and bounded
NativeInodeCommitInfo. The returned structure owns no pathname or descriptor.

Creation, parent sequence, basic tuple, descriptor and optional persistent
delete intent share the SAME private Stage, validation and three-barrier
replicated publication. The new-inode selector is checked and its facts copied
before publication. There is no second delete-intent transaction or post-commit
lookup. A new empty regular file has logical size zero and no payload records;
a directory is empty. No storage reservation or physical compression count is
invented. This is strictly NEW-name creation; existing names are rejected rather
than opened, overwritten or truncated.

This path uses existing FLTXNS04 encoding and limits. It changes no format,
default checkpoint geometry, historic create_entry behavior, capacity, codec,
stored digest or acknowledgement barrier. Legacy calls preserve their existing
bytes and semantics. Basic fields are explicit caller values, never inferred
from transaction counters or a hidden clock. CPU framing is not Windows
descriptor validity, inheritance, privilege or ACL authorization.

## Context ownership before and after publication

NativeFileSession::create_open admits a free slot in the existing 256-entry
table and a monotonic token before storage work. Cleaned but not closed contexts
still consume slots. It prepares the weak owner identity privately. The session
still requires one control thread; no other request can consume that local
reservation between admission and publication.

NativeSessionCreateOptions holds already-authorized data/delete, explicit basic,
and descriptor read/full-replacement grants. Delete-on-close additionally
requires the delete grant. Parent creation authority, inherited security,
read-only policy and Windows open semantics remain the FSD/adapter's job.

Only after successful durable creation does the method install the fixed slot
and return the committed facts plus NativeFileHandle. The remaining assignments,
weak-owner operations, transaction moves and result return are statically
nonthrowing. No open(), snapshot(), inspect(), path lookup, storage access or
new allocation forms the success result. The context retains normal rename,
paging-after-Cleanup, aggregated disposition, Close, stale-token and terminal
fencing semantics.

Delete-on-close sets the new context's requested disposition only after the
same transaction durably records its intent. Cancellation uses the existing
owned disposition mechanism. Creation does not unlink immediately. Cleanup
detaches and final owned Close retires the inode. Independent recovery may
finish pending creation only after the caller proves old contexts have retired;
the hosted model's destroyed session is not proof about a real kernel helper.

A failed call exposes no result/handle and installs no new context. Existing
contexts remain owned. Admission errors leave a healthy session usable; failed
publication retains terminal/indeterminate storage fencing. The last barrier
can fail after persisting complete new state: recovery must then recognize
that complete state rather than fabricate success or roll back acknowledged
unrelated data.

## Configured checks and resource limits

Five bounded CTest groups add actual production-path coverage:

- Sixteen raw/automatic, fixed/compact, file/directory, normal/delete-on-close
  cases deny EVERY backend operation after the third barrier. Creation must
  still return its complete facts and owned context. A later handle query must
  hit the active denial, fence, and retain the context. Independently reopened
  bytes must match the result.
- Context exhaustion (including Cleanup retention), invalid grants/paths/
  tuples/descriptors, case-insensitive collisions, missing parents and the
  unchanged 64 KiB namespace limit must publish nothing and leak no context.
- Created contexts exercise data/basic/security grants, write, stable rename,
  child directories, cancellation, repeated lifetimes, detached late writes,
  stale aliases and inode nonreuse.
- Four separately bounded fault groups cover file/directory and optional
  delete-on-close. Each raw/automatic and fixed/compact trace injects all three
  existing failure models at EVERY observed write/flush step. Independent CPU
  recovery requires old or complete new metadata, preserves unrelated
  acknowledged bytes, and continues through a fresh owned creation.
- Closed-session recovery separately refuses ordinary resume with pending
  creation, then removes only the pending inode after explicit context
  retirement while preserving unrelated bytes.

All old cases/oracles and the existing 45-second per-test ceiling remain.
These modeled RAM failures are not real writer termination, Windows callback/
mounted writes, physical power loss or Windows 11 client evidence. Existing
976/712/172 process campaigns retain their original scope; none is relabelled
as new Create or NS04 termination coverage. Exact-commit execution is required
before configured checks can be called passed.

## Remaining Windows product work

Truthful allocation/full FileInfo, Create options/overwrite/disposition mapping,
Windows descriptor admission and ACL/inheritance/privilege semantics, sharing,
caches/mappings/cancellation, concurrent dispatcher ownership and supervised
helper lifecycle still need actual integration and client tests. Current
descriptor transport limits remain independent from the native wire limit.

Production-scale indexing/capacity/reclamation, live AMD benefits and fault
recovery, redundant payload repair, bounded low-priority adaptive learning,
installation/update/rollback and representative filesystem comparisons remain
required. The product target remains latest stable Windows 11 AMD64 only.
Non-Windows analysis is not a supported product.

## Executed hosted checkpoint

At 9a251dd2e9b27b12ebae9bca25d8bb82c2a60eee, Windows native job
129654857662 passed all 76 CTest entries, raw Windows job 129654857647 and
relocated static Windows job 129654854097 each passed 72, and sanitizer job
129654857378 passed 68. Each inspected creation matrix emitted 325 invariant
checks, 16 sealed cases and all 1,656 fault cases across sixteen complete
raw/automatic, fixed/compact, file/directory and delete-intent traces.

The sealed cases returned an owned context and committed facts with zero
post-barrier backend accesses, then confirmed active storage denial through
that handle without releasing its context. Modeled faults preserved unrelated
acknowledged bytes and independently resumed creation/data access. New
sanitizer fault groups took 6.94-7.21 seconds under the unchanged 45-second
ceiling. Windows worker handles remained 94 before and after testing.

Existing Windows legacy/basic/root-3 jobs 129654856542 / 129654856246 /
129654854099 completed their original 976 / 712 / 172 termination cases and
strict disjoint unions. These campaigns do not become actual Create or NS04
writer-kill evidence. Their two partition times were 64.425/54.811,
48.941/34.082 and 6.600/6.485 seconds respectively.

The checks above do not enable writable callbacks or qualify Windows 11
client, real Create/NS04 termination, physical power cuts, live AMD, production
capacity, payload repair or autonomous learning. No owner-PC operation occurred.

Read-only mounted job 129657411513 also passed normal operation, confirmed
helper termination and independent recovery/remount with exits 0 / 1460 / 0.
All three space reports reconciled; peaks were 6,201,344 / 6,189,056 / 5,169,152
bytes. The unchanged default-layout, absent-security fixture preserved source
bytes; this is a regression check, not new writable Create or NS04 ACL coverage.
All eleven triggered workflows succeeded at the exact commit.


The later driver-facing create_prepared API separates volatile initial
FILE_DELETE_ON_CLOSE from this historical create_open durable-intent option.
It prepares general adoption/cancellation without persisting initial deletion
before actual Cleanup. See NATIVE_INITIAL_DELETE.md; do not map raw driver
options to the historical durable-intent field.
