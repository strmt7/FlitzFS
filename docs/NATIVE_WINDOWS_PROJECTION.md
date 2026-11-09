# Native transactional images connected to Windows

Experimental Windows-only read path. The native inode/extent filesystem can now
supply the existing WinFsp callback adapter and supervised read-only session.
This joins the transactional storage engine to actual Windows file I/O; it does
not enable mounted writes, an end-user GUI/installer, driver-fault immunity or
HIP execution. Owner-PC work remains forbidden.

## Admission and ownership

ReadOnlyProjection::open_native_file requires an existing ordinary image and
an explicit nonzero 128-bit expected volume identity. It opens the qualified
read-only file backend, CPU-verifies the object records and native namespace,
checks identity, and retains that source through the adapter/session lifetime.
Generic object stores, unknown identities, malformed graph/codec/integrity and
unsafe virtual paths fail before callbacks or lifecycle-provider exposure.

There is no mutable custom-backend/snapshot injection into this callback path.
Windows ordinary-handle sharing denies concurrent write/delete opens. It is
not a claim that every pre-existing writable mapping is frozen; integrity still
checks every allocated record before a read's private output is copied to the
Windows caller. A mapping-admission test either observes OS refusal or mutates
only its separate owned synthetic image and requires a failed read with zero
transferred bytes and untouched caller-buffer canaries.

Projection operations on the native source share one mutex. This preserves the
core's one-control-thread contract across concurrent dispatcher reads. Legacy
catalog reads retain their existing per-reader locking. Both sources remain
immutable views; no writer, background service, discovery or GPU starts here.
Session teardown still joins the dispatcher before adapter/source destruction.

## Metadata and limits

CatalogEntry gains an optional native inode ID; catalog-only entries retain zero.
The adapter reports persisted IDs rather than rebuilding them from enumeration
order. A stable 32-bit Windows volume serial folds the full native UUID; the
complete UUID remains authoritative and a serial is not authentication.

native_info exposes the UUID, namespace sequence, checkpoint/integrity-health
metadata and full admitted backing logical length for a future helper app.
ObjectStoreSnapshot::source_bytes distinguishes that length, including uncertain
tails, from the root's committed end. Neither is an NTFS allocated-cluster count.
The read-only native volume reports full image length with zero writable free
space, rather than adding logical sparse file sizes. File AllocationSize remains
a logical 4 KiB rounding estimate in this preview; full production allocation/
quota/space reporting remains unfinished. Timestamps are still adapter-time
values, not persisted Windows file times or namespace sequences.

Verification of native sources admits decoded LIVE objects including metadata,
within the existing 16 MiB store envelope; it does not expand sparse holes.
Legacy catalog verification continues to budget its logical contents. The
4 MiB per-read, 256-inode/255-nonzero-extent, 64 MiB history, namespace/name,
256-handle, two-active-read and two-dispatcher limits remain unchanged.
A native sparse file's 1 TiB logical limit is not physical data capacity.

A surviving replicated current checkpoint can be read with degradation explicitly
visible in native_info. This path never repairs, discards tails, selects a silently
older object set or claims that detection is self-repair. Both-root or referenced
payload loss still fails.

## Windows callback behavior

The existing adapter/session consumes this projection without a new mount or
process-launch mechanism. Native path/type/limit/integrity/I/O error categories
are mapped to explicit NTSTATUS results; exceptions never cross the C ABI.
Reads preserve private verification before exposure, short EOF and sparse zeros.
Cleanup still retains open context for later paging I/O; Close retires the token.
All mutation callbacks and write/delete access remain write-protected.

Current ABI/source authority is the selected WinFsp v2.2B5 header at
14d6e0280ae15e2550c44d6dbc476be51649727f:
https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h
The selected source describes Write/Flush and Cleanup/Close separately. Writable
integration must preserve cached/paging I/O, flush ordering, append/constrained
writes and recoverable delete intent; this read path does not satisfy that gate.

## Configured validation

The new hosted Windows projection fixture builds a NEW native image, commits
directory/data creation, a deleted inode and a rename, then closes its writer.
It checks real persisted inode IDs, reopened identity, sparse tails up to the
core's 1 TiB logical bound without a driver, concurrent verified reads, denied
ordinary writer/delete handles, moved ownership, wrong UUIDs, generic-store
rejection, orphan-tail accounting, recoverable metadata degradation and full
corruption rejection. It compares original source bytes afterwards.

Real SDK callback tests add native images, sparse metadata, UUID-derived serial,
persisted inode enumeration, Cleanup lifetime, error statuses, mutation denial
and private-buffer corruption fencing. Injected lifecycle tests repeat every
rollback case with the native source and prove that wrong identities make no
provider calls.

The hosted live probe now mounts the actual native transactional image, not the
legacy collection manifest. It retains collision protection and tests committed
rename/delete visibility, compressed reads across 64 KiB extents, mapping,
persistent IDs, enumeration, concurrent readers, write denial and source parity.
A sparse file slightly larger than 4 GiB tests 64-bit mounted offsets without a
terabyte-sized kernel cache map. The source image, immutable crash baseline and
original data stay below 4 MiB total. No large file is materialized.

Forced helper termination still requires a verified-live readiness marker and
confirmed exact-child exit. A separate CPU native reader must recover UUID,
sequence, inodes and bytes, compare the entire backing image and collision
sentinel, then remount and check mapped/sparse reads. Existing 128 MiB/20% child,
30-second normal/recovery and 15-second forced-stop bounds are retained.
Kernel resources are outside user-mode Job Object containment.

These are configured tests until exact-commit hosted results are inspected.
Windows Server qualification is distinct from latest Windows 11 client,
writable application, physical power-cut and AMD hardware qualification.


## Executed native-image integration evidence

Commit 3e61929e1be5c71711be8bc2fc73ea0c3f177177 passed all eight
triggered workflows on 9 November 2026. The native Windows job
129431185740 passed all 28 CTest entries, including native projection,
actual-SDK callbacks and injected lifecycle rollback. The raw Windows
job passed 26 entries; Linux CPU, ASan/UBSan, relocated CPU binaries,
reference mechanisms, dependency inventory and six source-bound semantic
queries also passed their separate checks.

The live native-image job 129432925521 in run 42696941121 returned:
normal mount exit 0, confirmed; forced helper stop ERROR_TIMEOUT (1460),
confirmed; independent recovery/remount exit 0, confirmed. Peak committed
process bytes were 6,201,344, 6,119,424 and 5,230,592 respectively.
These are observed process peaks under the unchanged 128 MiB/20% limits,
not total kernel/GPU resource measurements or performance benchmarks.
The selected source-bound WinFsp v2.2B5 driver retained its valid Microsoft
hardware-compatibility signature and driver SHA-256
0a91fc3429fd5adf8b9c65976ac0c4ae924702be2cfbed856d90d2a7d99b0390.
The provenance still explicitly reports windows11_policy_qualified=false.

The independent Windows process-interruption campaign at that same commit
completed both disjoint 488-case partitions in 64.742 and 78.859 seconds.
Its merger verified the complete 976-case union and recovered every
acknowledged byte before continuing. This is modeled/process interruption
evidence; it does not simulate every storage-device power-loss behavior.

Logs:
- https://github.com/strmt7/FlitzFS/actions/runs/42696941121
- https://github.com/strmt7/FlitzFS/actions/runs/42696941201

Qualification applies to that exact implementation and those environments.
It does not complete mounted writes, the helper application, Windows 11 client,
live AMD acceleration, production capacity or physical power-cut qualification.


## Unresolved file-lifetime admission

The later native lifetime capability is specified in NATIVE_FILE_LIFETIME.md.
The read-only factory rejects pending/deferred inode deletion after full CPU
admission and UUID checks, before any callback/session exposure. It performs
no source mutation or inference about old open contexts. Independent native
CPU inspection remains possible. Separate newly owned Windows fixtures require
this rejection for valid pending and detached images and compare source bytes.
