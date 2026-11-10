# Storage resume and process interruption reference

The qualified backend resume protocol and its original 360 executed Linux/Windows
interruption cases were established at 601f4a56989638fefc55718bc9b30be1650f1351.
See OBJECT_STORE_FORMAT.md for storage boundaries and NATIVE_FILESYSTEM.md for
the newly connected namespace layer.

Public ordinary-file factories remain new-file-only. Resume of an existing
ordinary image is an internal hosted-owned-fixture exception, with no owner-PC
permission or production mount/CLI write route. Backend ownership, exact
identity/generation verification, stable flush fence, aligned orphan preservation
and surviving-root repair remain required.

The expanded interruption workflow retains raw/automatic object mutations and
root repair and adds combined native directory/data operations. Its case budget
is now 320, with the same per-process, I/O, queue, fixture and total time bounds.
The native-file phase uses acknowledgement generations 2..3; original storage
phases use 1..2. Each case must independently recover and then continue writing.
No physical power-cut or writable mounted qualification follows from process
termination.

The full earlier protocol is retained below for continuity. Its original
256-case bound describes the previous workflow; the current controller admits
at most 320 cases.

# Qualified backend resume and hosted process interruption

Experimental storage-core work, not a writable mounted filesystem. No owner-PC
process, existing user file, raw device, driver or GPU operation is authorized.
Public ordinary-file factories remain NEW-file-only. The new resume operation
takes a custom, externally qualified, exclusively owned StoreByteIo backend.
An internal ordinary-file reopening adapter exists ONLY for hosted owned-fixture
tests and checks the hosted environment before opening anything.

## Writer ownership and explicit recovery lease

StoreByteIo holds a per-instance atomic writer lease. Creation and resume acquire
it before backing inspection. A second writer on the same instance fails without
I/O. Moving a writer transfers ownership. Errors during initialization/resume
release the lease through destruction; a poisoned returned writer keeps it until
its lifetime ends. This prevents bypassing terminal failure by constructing a
second writer while the first still exists.

This is not a global inode lock or a security boundary. Custom backend owners
must exclude other facades/processes and serialize operations. The file adapter
uses Windows share-mode-zero handles and POSIX nonblocking exclusive flock.
POSIX locks are advisory; unrelated noncooperating writers and hostile directory
mutations remain outside the ownership contract. File adapters reject final
links/reparse points, multiple hard links, nonregular files and excess extents.
One control thread remains required; snapshots do not make arbitrary concurrent
mutation safe.

Resume requires a nonzero expected store identity and an exact recovered
generation. It validates the backend contract, every root/manifest reference,
all reachable stored/decoded content and resource headroom before mutation.
Wrong identity/generation, unknown contract, corrupt latest dependencies and
unavailable recovery codecs fail. Integrity is unkeyed, not authentication or
rollback protection against an attacker.

## Preserving uncertain append tails

A failed writer may leave bytes after the last valid root's committed end.
Resume never truncates those bytes or reuses their failure units. Its append
floor is the backend's physical extent rounded UP to 64 KiB, and subsequent
transactions begin there. Uncertain tails and their alignment consume capacity.
The 64 MiB physical-history cap remains; there is no in-place compaction or
implicit deletion. Existing snapshots retain immutable historical references.

A complete root may have been visible before its failed final flush. A clean
resume fences the fully verified selected baseline with a successful flush
before returning a writer. A flush failure returns no writer. The original
failed commit's outcome remains indeterminate until independently inspected.

## Missing-root repair

One valid root allows read-only recovery but must be repaired before granting a
resumed writer. All plan allocations/admission precede repair writes.

- At generation zero, either initial slot may survive. Flush the baseline, copy
  the canonical initial root into the MISSING slot, then flush. Never overwrite
  the sole surviving root. Both generation-zero roots may legitimately agree.
- For a positive generation, preserve the surviving root. Append a new manifest
  with the SAME logical objects at the aligned append floor, flush dependencies,
  publish generation plus one in the peer root, then flush. Repair advances
  metadata generation without changing object contents or object generations.
- Reparse both roots and verify all reachable content before returning a writer.
  Reject exhausted generations or insufficient repair reserve. Failure may leave
  a complete repaired generation; independent recovery must inspect it.

Under the declared flush/failure-unit contract, interruption preserves the
surviving acknowledged baseline or a complete repair. No latest corrupt
dependency is silently rolled back. Arbitrary media damage, dishonest device
flushes, power failures beyond the admitted failure domain and loss of both
roots cannot be repaired by this protocol alone.

## Test layers

The RAM suite exercises ownership/moves/poison retention, stale lease rejection,
unaligned orphan preservation, both initial survivor slots, positive-generation
repair and a retry after EVERY observed repair write/flush failure. Prefix
persistence, whole-unit damage and post-flush errors are injected. Rejected
resume attempts must neither flush nor change bytes.

Hosted ordinary-file tests cover valid continuation, independent CPU reopen,
old snapshots, OS writer exclusion, link/missing/non-store rejection and
no-clobber creation. The existing-file adapter has no public product factory or
CLI/mount route. It is available only through the internal hosted test interface.

The separate Native store process-interruption recovery workflow builds a
source-pinned CPU probe on disposable Linux and Windows VMs. The Python owner
creates an exclusive temporary parent and nonce marker. The child accepts only
that parent/token and the fixed image.fltx leaf. An unrelated sentinel must stay
unchanged. Cleanup removes individually recorded fixture names, nonrecursively,
only after confirmed child exit.

The child admits process limits BEFORE corpus or file work. Windows uses a
768 MiB memory/one-process/20-percent-CPU Job Object; this quota leaves room for
the ordinary-file adapter's conservative physical/commit headroom checks.
POSIX uses 512 MiB address space, 15 seconds CPU, no core dump and 2 MiB maximum
file size. Each child is wall-bounded to 15 seconds by the controller, which also
bounds line bytes, queues, case count (256) and total execution (180 seconds).

For raw and automatic compression, the controller first enumerates a complete
mutation or root-repair trace. It then repeats each trace, forcibly terminating
the writer at every numbered boundary, including after acknowledged success.
A new process verifies an independent input/acknowledgement oracle, performs
qualified resume, commits a continuation object, and independently reopens it.
All original objects and the sentinel must retain their expected bytes.

The hook boundaries surround complete StoreByteIo calls. They do not interrupt
a kernel write halfway through a sector. RAM tearing tests cover separate
abstract partial-write cases. SIGKILL/TerminateProcess leave the OS and hardware
running, so these runs do NOT establish physical power-cut durability, Windows
creation-name durability, writable mount semantics or real GPU failure behavior.
Inspect the exact commit's workflow/results; do not infer a pass before execution.

## Next filesystem gates

The custom-backend resume is a persistence primitive for a native file/extent
namespace. Public existing-file write admission, persisted inode/extent semantics,
capacity/reclamation, writable application/mapped I/O, full physical persistence
qualification and actual AMD runtime behavior remain required before a product.

The file adapter refreshes size from its opened OS handle, including after
failed/partial writes. Resume rechecks extent, metadata references and decoded
content after its baseline flush; a changed fence fails before repair writes.
This rejects unstable backend state instead of appending from a cached extent.

After interruption, snapshot physical_end names the COMMITTED end. The backing
may be larger because of orphaned writes; backend extent is authoritative for
remaining physical capacity. A later commit incorporates that tail into its
new committed end. Resume does not pretend orphan bytes are free space.

## Compacted-copy extension

COMPACTED_COPY.md adds latest-snapshot copying into an empty, distinct volume
identity. The source is held quiescent and never modified. Ordinary store input
handles now acquire cooperating shared locks; exclusive OS writers cannot
bypass those readers. POSIX exclusion remains advisory.

The current hosted controller additionally covers native compaction with source
generation 3 and destination publication generations 4..5. A source BLAKE3
digest is captured at its acknowledgement and passed to a new recovery process.
Every cut must preserve it, validate any published destination, or retry into a
separate new path/identity, then commit a continuation and reopen independently.
No partial file is truncated/reused. The case ceiling is now 640; the existing
128-event, 256-byte-line, eight-item-queue, 15-second-child/180-second-total and
process CPU/memory limits remain. The combined image files are capped at 2 MiB.
Historical 320/256-case limits above describe earlier controllers. None of
these tests establishes physical power-loss or autonomous maintenance safety.

## Replicated current-checkpoint qualification extension

REPLICATED_CHECKPOINTS.md supersedes single-publication repair for root format 2.
A degraded current root/catalog is restored by appending two isolated manifests,
then separately publishing/flushing two roots with the same verified objects.
Uncertain tails remain reserved. Legacy root-1 resume keeps its old protocol.

The controller additionally force-stops the replicated catalog-repair phase.
Native file baseline/update publication generations are now 4/6, compaction
source/destination 6/8, and checkpoint repair 2/4; logical namespace sequences
remain 2/3 and 3 respectively. Intermediate first-root generations may recover
the same complete candidate. Current bounds are 1024 cases, 256 events and
3 MiB combined images, retaining 256-byte lines/eight queued events,
15-second-child/180-second-total and existing process caps. Earlier limits and
executed counts above are historical evidence, not current-count predictions.

## Reverted extent in the process-interruption fixture

The native file phase now includes stable.bin, a separate synthetic file. The
same atomic update zeroes its contents and restores them, together with the
existing cross-extent data/truncation/rename/replacement operations. It must
retain the original immutable payload generation while advancing its modified
namespace sequence. Fixture identity is found by exact verified padded contents,
not an assumed object ID, host path or installed tool.

Every existing kill point recovers the complete old or new namespace and checks
the stable file's exact bytes, size and sequence. Independent recovery and later
continuation must retain the baseline payload generation. Compacted-copy
fixtures include this file but correctly create fresh destination records.
Resource limits remain unchanged: at most 1024 cases, 256 events and 3 MiB
combined fixture images, with existing per-process/time limits. The controller
rejects overflow; no cases may be silently omitted or budgets raised to pass.
Only disposable hosted VMs execute this test; ordinary owner-PC files and mounted
writes remain outside its authorization and qualification.


## Bounded Windows campaign partitions and failure evidence

At fddd1b7108d8a6e1b4d9359475749c27b22ffe85, Windows run 42686079473
(job 129393153713) failed at approximately 180 seconds in the recovery reader.
The old reader called either its individual 15-second deadline OR the shared
180-second campaign deadline "probe process deadline", and the old controller
produced no partial report on failure. No data-corruption result was reported,
but this was NOT a passing recovery campaign. Linux passed separately; it cannot
qualify the Windows failure.

The controller now names the expired budget and operation/policy/phase/PID,
records the active cut and completed cases on failure, and uploads incomplete
reports with a nonzero workflow result. It never counts partial coverage as
success. The individual process deadline, 768 MiB/20% Windows child ceiling,
eight-event queue, 256-event/256-byte bounds and 3 MiB aggregate image fixture
remain unchanged.

Windows now runs TWO sequential, deterministically disjoint campaigns from the
same build, each under the existing 180-second campaign bound. Every baseline
trace is admitted before selecting even/odd global fault ordinals. Each campaign
retains the 1024 aggregate planned-case cap; no sampled case or fault point is
dropped. The full run has an explicit maximum 360-second campaign allowance
instead of 180, with unchanged peak resource caps and the existing 20-minute
job ceiling. This deliberate VM-only allowance accommodates the complete Windows
fault matrix without relaxing a stuck child's 15-second limit. It is unrelated
to GPU retry/recovery policy or permission for host work.

A schema-2 merge must prove identical commit/platform/trace/limits, both distinct
partitions, exact ordinal-to-fault identity, nonzero forced exits, preserved
acknowledged history and continued commits, with a complete nonoverlapping union.
Missing, relabelled, incomplete, differently configured or duplicate coverage is
rejected. Only that merged report can represent the complete Windows campaign.
Failure reports are retained without being promoted to recovery evidence.
The new no-I/O report tests cover the union oracle and its negative cases.


The split campaign at 8c6dd91604260db7ddeb435a7656f1225739fd33 passed
on Windows: 488 cases in 54.436 seconds and 488 in 70.321 seconds, with the strict
merge proving all 976 original fault points. The independent analysis VM passed
all 976 as well. These are observed runs, not guaranteed timing budgets.
The next dedicated Windows campaign uses the current hosted VS2026/Server2025
image; actual latest Windows 11 client and physical power-loss tests remain open.

## Separate FLTXNS03 writer interruption campaign

See NATIVE_BASIC_METADATA.md. A separate hosted Windows basic campaign keeps
every legacy 976-case scenario intact while exercising capability upgrade,
tuple replacement and compound data/namespace/basic transactions. Schema 3
binds fresh complete phase traces to decoded live-state closure digests and
checks every cut, complete old/new recovery, acknowledged tuple/data retention
and a separately reopened combined continuation. Keep current process/queue/
fixture/case limits and two 180-second partitions; no physical power-loss,
Windows 11 client, owner-PC or mounted-write qualification follows.
