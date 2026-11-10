# Native automatic Cleanup metadata (experimental)

This component advances the Windows-only write engine. It does not enable
writable callbacks, mount a volume or create a driver/service. The existing
WinFsp callback table remains read-only.

## Current primary source and callback contract

The selected WinFsp v2.2B5 source is pinned to
14d6e0280ae15e2550c44d6dbc476be51649727f (2026). Native repository API
captures of the header, FSD Cleanup implementation, user-mode dispatcher and
memfs implementation were rechecked for this change:

- [ABI and flags](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
- [FSD request construction](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/cleanup.c)
- [VOID callback dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c)
- [Reference metadata effects](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/memfs/memfs.cpp)

The actual dispatcher calls Cleanup and returns STATUS_SUCCESS. Its VOID
callback cannot transmit an engine error. It forwards Delete (0x01),
SetAllocationSize (0x02), SetArchiveBit (0x10), SetLastAccessTime (0x20),
SetLastWriteTime (0x40) and SetChangeTime (0x80). FSD per-descriptor state
determines the requested automatic effects. Cleanup is best effort during
shutdown; the engine cannot promise a stamp for a callback never delivered.

These sources justify the explicit requested-effects bridge. They do not
qualify all Windows timestamp policy. In particular, actual Windows per-open
SetFileTime -1/-2 suppression/restoration, cached/mapped writes and delivery
during cancellation/shutdown require an off-host writable conformance fixture.
No new kernel implementation or Windows security rollback follows.

## One durable operation

NativeCleanupMetadata carries requested archive/access/write/change booleans
and one nonnegative 100-nanosecond UTC value. Time must fit INT64_MAX. If no time
is requested, utc_time must be zero. Zero is an actual requested time, never a
preserve sentinel. Pure invalid clocks reject before backing I/O.

NativeFilesystemWriter::cleanup_inode selects the stable inode, copies its
existing optional tuple, applies only requested times and ORs ARCHIVE only
for a regular file. Creation and all other fields remain unchanged. Directory
archive requests are ignored, matching the selected reference behavior.
A meaningful effect requires a present tuple; missing original times/attributes
are not fabricated. Directory archive alone does not upgrade a legacy image.

Requested metadata and optional unlink share one private Stage and the normal
replicated three-barrier transaction. No name/data/basic intermediate state is
published. Identical final values without unlink do not publish. Existing
namespace encodings, legacy bytes, sticky FLTXNS03, resource ceilings, data
integrity, retained snapshots and terminal I/O behavior are unchanged.

## Owning contexts and duplicate delivery

The owning session exposes an overload of cleanup(handle, delete, effects).
The original overload supplies no effects. The session verifies ownership and
authorized persisted delete intent, reserves any latch before publication,
then performs the combined durable operation. Only after success does it
change cleaned/latch state. These updates allocate nothing and cannot throw.

A duplicate with the same delete/request booleans is idempotent even if a
later valid event time is supplied: the first successful stamp is retained.
Conflicting duplicate actions reject. Invalid clocks reject first. Contexts
remain owned through late paging reads/writes/truncates until Close; ordinary
late data I/O preserves the tuple. Final Close retains its existing exclusive
retirement rules. Earlier snapshots remain valid.

Implicit trusted FSD effects do not require the separate explicit
WRITE_ATTRIBUTES grant. This is not an ACL evaluator: the future adapter must
authenticate contexts and supply actual trusted FSD flags. Explicit user
SetBasicInfo continues to require its own metadata-write grant.

## Windows user-mode failure bridge

src/windows/native_cleanup.cpp maps the selected SDK flags into the session.
It samples GetSystemTimeAsFileTime exactly once when times are requested;
archive-only work needs no clock. Tests can supply a bounded noexcept clock.
Unknown flags and SetAllocationSize fence immediately before any clock or I/O.
Allocation is deliberately unsupported until real allocation accounting exists.

The noexcept bridge catches every engine exception and fences the owning
session without releasing contexts, retrying I/O, retiring data or allowing
subsequent operations. fence() itself performs no I/O. NativeCleanupOutcome
distinguishes unsupported requests, clock rejection and engine failure.
A future writable adapter MUST send this outcome to its supervisor and
offline the volume outside the callback. It must never stop/join its own
dispatcher inside Cleanup or mistake the vendor dispatcher's success for
durable success. That supervisor/offline integration remains unfinished.

## Bounded qualification

Four sequential CPU CTest groups cover raw and automatic compression:
all sixteen request masks, untouched/creation fields, zero/invalid clocks,
legacy absence, duplicates, cross-context deletion/latches, late paging and
every observed write/barrier fault in retain, latch and unlink phases.
Independent CPU recovery checks complete old/new metadata and lifetime state,
acknowledged unrelated bytes, retained snapshots and combined continuation.

The Windows-only SDK test calls the actual FspFileSystemOpCleanup export with
stack request/response/interface structures and real per-open context mapping.
It creates no filesystem object, volume, mount, dispatcher or driver.
Nine cases cover one-clock effects without attribute grant, archive-only work,
atomic delete, unsupported allocation/flags, missing/invalid clock, backend
failure, terminal retry prevention and the real Windows clock.

Each test process keeps the existing 45-second limit; no data, namespace,
queue, memory or mount limit is increased. Configured tests require exact-commit
executed receipts before they count as passing evidence. Windows Server VM
results are not latest Windows 11 client qualification. Physical power loss,
full Windows policy/security/allocation, writable mount/app, live AMD benefit,
scaling/reclamation, redundant payload repair and automatic learning remain
separate required work. Nothing runs on the owner's PC.

## Executed hosted checkpoint

The engine checkpoint 63c9caa1cb65616f6ee98ddd8e095a36ca0ceefc passed 209
Cleanup invariants and 1,026 modeled Cleanup write/barrier failures on Windows
and under sanitizers. Every raw/automatic retain/latch/unlink trace contained
57 I/O steps, with 171 failures per trace. Its actual Windows writer campaigns
retained all 712 metadata and 976 legacy termination cases. This commit's
overall CPU run failed while compiling the new SDK test: MSVC C2466 rejects
aggregate initialization of the vendor flexible Buffer[] structs.

Commit 7dfd92ddda38980b2971c96c62c80aea8d97de2e changes only that test's
fixed-header initialization, with trivial-copyability assertions. Windows
native job 129548757829 passed all 50 entries, including all nine actual SDK
Cleanup cases and the existing Windows basic API oracle. Worker handle totals
were 94 before and after. Sanitizer job 129548758105 passed all 43 entries,
retaining 209 Cleanup invariants, 1,026 Cleanup faults, 400 basic invariants,
1,038 basic faults, 2,064 lifetime faults, 1,026 context faults and 271 Flush
checks. The process campaign sources remain byte-identical to 63c9caa.

Windows metadata job 129546823785 executed two 356-case partitions in
46.646/47.127 seconds. Legacy job 129546820382 executed two 488-case partitions
in 68.743/68.805 seconds. Their results remain bound to the executed 63c9caa
commit; source parity is not a claim that untriggered jobs reran at 7dfd92d.
No owner-PC operation, writable mount or live GPU was performed.

The corrected checkpoint's read-only mount job 129550973558 passed actual
native image reads/mapping/enumeration/write rejection, persisted inode/basic
metadata, source parity, confirmed helper termination and independent CPU
recovery/remount. Normal/recovery exit codes were zero; intentional kill
reported 1460 with confirmed exit. Peaks were 6,197,248 / 6,201,344 /
5,226,496 bytes. All three triggered workflows at 7dfd92d succeeded.
The selected candidate's driver signature remained valid Microsoft WHCP;
its provenance explicitly reports windows11_policy_qualified=false. These
Server results add no client, writable-mount, live GPU or owner-PC permission.
