# Native writable Windows adapter candidate (experimental)

This candidate combines the native transaction engine and actual WinFsp SDK
callbacks. It is built only with the opt-in Windows SDK target. Its factory
requires a disposable hosted environment and creates a NEW ordinary image with
OS-exclusive creation/writer ownership. It has no CLI, installer, service, live
provider selection or mounted-write executable. Default product paths and the
existing mounted probe remain read-only. No owner-PC operation follows.

## Current primary evidence

The selected WinFsp release remains v2.2B5 (13 October 2026), immutable source
`14d6e0280ae15e2550c44d6dbc476be51649727f`. Its native release API and source
bytes were reviewed on 10 November 2026. This change uses those actual bytes,
rather than historical API assumptions:

- [Kernel disposition](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/fileinfo.c):
  successful cancellation clears the file node's global pending state. An
  aggregate of per-open explicit intents would incorrectly retain deletion.
- [Cleanup](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/file.c):
  an unprocessed initial per-FD delete-on-close option can arm pending state at
  its later Cleanup. Cancelling current state does not erase that option.
- [SDK dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c):
  Open receives the full CreateOptions, including the high disposition byte;
  Overwrite can close a failed prepared open without Cleanup. The final access
  mask can exclude already-checked implicit overwrite/supersede rights.
- [Callback contract](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
  and [memory reference](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/memfs/memfs.cpp):
  basic attributes INVALID_FILE_ATTRIBUTES and times zero mean unchanged.
  Reference no-op Flush and detached slow-I/O patterns are not adopted.

The source-routing receipt for the base commit `7de69ce` used six real hosted
queries, job 129734480112, source SHA-256
`9c23568c77c3538d2c2fe43375c5ad8116a78c75158fa34b6658a363a89b21fa`.
Candidate paths and all changes were verified against native immutable source.
This is a base-source navigation receipt, not an index receipt for new drafts.

## Ownership, callbacks and offlining

One mutex owns every callback interaction with the single-controller engine.
The adapter has 256 bounded monotonic opaque context tokens; no raw pointer
context can rebind after Close. Capacity is checked before creation. An owned
metadata/security lookup admits no new context/grant. The root receives actual
UTC metadata and a protected inheritable descriptor for the process owner in
one transaction. Regular creation sets the source-qualified archive attribute. SDK/FSD access
checks and inherited descriptors remain required;
the adapter does not invent access authority.

Create, Open, Overwrite, private verified Read, synchronous Write, size,
GetFileInfo, real Flush, basic metadata, rename, deletion, security read,
directory enumeration and volume quota are implemented in this candidate.
Queries support provisional contexts. The first trusted data/metadata/Cleanup
callback explicitly adopts a prepared context. This transition is NOT kernel
adoption attestation. A premature active Close without Cleanup fences rather
than silently discarding ownership. The candidate deliberately rejects deletion while any peer Open remains
provisional, even if the FSD may already have adopted that peer. Tests must
check this no-mutation rejection AND the full two-adopted-handle global cancel
oracle. Ordinary deletion with an idle/unconfirmed peer is a remaining product
limitation, not a reason to remove the guard or fabricate adoption.
Actual cancellation/cache/early-paging delivery still requires bounded real-kernel qualification before promotion.

Windows global deletion is an explicit NativeDispositionPolicy; the historical
aggregate policy remains the default. A successful global cancel clears all
current explicit requests/latches only after durable publication, retaining
unprocessed initial per-FD options. Failed writes retain exact runtime ownership.
No stored encoding or historical byte/format/API behavior is changed by policy
selection.

Storage, corruption, unknown or VOID-callback failures fence and monotonically
signal offlining; later callbacks do not retry storage or release contexts.
The controller must observe the signal and stop outside callbacks. No callback
joins/stops a dispatcher. The injected session joins first, removes/deletes only
owned filesystem objects, then captures diagnostics and destroys the writer.
Surviving contexts or fenced diagnostics make stop a failed outcome. Stop's
native ABI can block; a future live helper still needs an external bounded
supervisor and actual process/kernel retirement proof.

## Explicit capacity and durability limits

AllocationSize and volume total/free report the existing native live quota,
not host clusters or per-file compressed size: 255 extent slots, 64 KiB each.
The 4096-byte aligned reservation/cache value is distinct from this shared
extent charge. The 256-inode, 16 MiB aggregate decoded, 63-dirty, 4 MiB transaction
and 64 MiB append-history ceilings are unchanged. This is a small prototype.

The ordinary backing file retains per-transaction projected-growth and memory/
free-space admission, exclusive new-file creation, backend identity checks and
real FlushFileBuffers. Native reservations DO NOT physically preallocate host
storage or guarantee future writes against history exhaustion, allocation
pressure, media failure or other host writers. Thin virtual quota is an
experimental presentation requiring real-kernel free-space/cache qualification;
it must never be sold as a physical-space guarantee.

Every successful engine mutation keeps its three publication barriers and
captured success output. Kernel cached WriteFile acknowledgement is a distinct
contract: future mounted tests must explicitly FlushFileBuffers before asserting
durable file data. CPU barriers and modeled failures cannot prove physical
power-loss durability or immunity to future software/driver bugs.

The profile currently admits ASCII paths, 100-character components, bounded
native path length, at most 1 MiB per write and 4 MiB per read callback.
SetSecurity and volume label changes return explicit unsupported status.
POSIX unlink/rename is disabled; held-target replacement is conservative.
Streams/EAs/reparse, full sharing/locks/cache/suppression, broader names, Windows
11 client/app/installer, reclamation/scale, live AMD, autonomous learning and
payload redundancy/self-repair remain separate unfinished product gates.
The candidate never initializes a GPU.

## Configured qualification (inspect exact executed jobs)

- Pure CPU tests preserve both disposition policies, initial-option cancellation/
  re-arm, explicit/initial latch provenance, nonempty directories and lookup
  ownership. Separate arm/cancel raw/automatic fixed/compact traces inject every
  observed write/flush in all three existing fault modes. Independent CPU
  recovery/continuation must preserve complete old/new data/security/reservation/
  intent and unrelated acknowledged bytes.
- The hosted Windows candidate exercises both compression modes, real SDK
  Write/Read/Cleanup/Close dispatch, direct creation/open/overwrite and other
  callbacks, exact persisted readback after exclusive writer destruction,
  capacity failure before creation, stale tokens, two joined concurrent readers,
  global cancellation, initial deletion and terminal VOID fencing. Handle
  counts compare after identical initialization/thread warmup; they do not
  establish universal heap/kernel/VRAM leak freedom.
- A separate fake-provider lifecycle group covers invalid/missing opt-ins and
  every partial startup failure, callbacks alive during join, rollback order,
  one-shot state, external stop, controller-led offlining after a VOID failure
  with retained failed-context diagnostics, and independent reopening after stop. No real
  filesystem/dispatcher/mount/driver function is invoked.
- All old groups, 45-second per-group limits and mount/process/oracle caps remain.
  Configured tests are not passing evidence until their exact jobs are inspected.


### First hosted build result retained

Commit ce09720 compiled the adapter and pure core, but native test qualification
failed before execution: MSVC rejected value-initialization/assignment of the
SDK's zero-length trailing-array request/response structs, and the test used a
nonexistent NativeFileInfo.basic member. The harness now uses the same explicit
ZeroMemory pattern as the existing SDK tests and the actual snapshot metadata
accessor. No cap, test or oracle was removed. Inspect the follow-up commit's
executed results; this first build is not passing Windows evidence.

The corrected a362338 build compiled and passed 109 of 110 Windows groups;
the paired-delete fixture had attempted deletion before its second Open had
any adoption event. The test now explicitly requires the existing conservative
sharing rejection with unchanged durable/runtime intent, performs real SDK I/O
on both handles, then retains the entire original global-cancellation oracle.
The 11-writer lifecycle/offlining fixture passed. This partial run is retained;
no Windows passing checkpoint is claimed until the complete follow-up run.


The separately gated, OFF-by-default real writable VM trial is now specified in
WRITABLE_MOUNT_TRIAL.md. It adds a per-image cap and preserves stop diagnostics;
it does not promote the candidate into product/owner-PC use or remove adoption
guards. Inspect actual same-commit results before claiming mounted coverage.

## Executed hosted writable-candidate checkpoint

Main `f75f25f45ec1de809b7e1239ff0ac9ee752f7662` (tree `4dc1962f05c561705138abc7f4a956aec9fee5ed`) was developed with native GitHub APIs and executed exclusively on disposable hosted VMs. No local process, owner-PC test/artifact, GPU execution or owner driver change was started. Latest narrow RAM-only/no-driver-change and careful-GPU permissions are committed in AGENTS.md at ce09720. The whole filesystem goal remains active and experimental.

All FOUR workflows triggered by the final test/document correction passed. This is not a claim that eleven workflows ran at this final SHA. Native Windows job 129771006580 passed 110 CTest groups. The new unmounted adapter reported 1,236 checks, 867 combined direct-callback/SDK dispatches and OS handles 151 to 151. Lifecycle job output reported 53 checks and 11 independently reopened retired writers, including every partial initialization and controller-led offlining after a fenced VOID callback. Provider calls were injected only: no writable volume/dispatcher/mount was created. Existing SDK bridge checks remained 474/137 dispatches/28 sealed cases/four retired backing owners/138-to-138 handles. Existing worker handles remained 94 to 94. These are scoped resource checks, not universal heap/driver/VRAM leak proof.

The explicit global-disposition engine reported 90 invariant checks and eight raw/automatic fixed/compact arm/cancel traces: 828 injected failures, all three modes at every observed write/flush. Fixed traces had 57 steps and compact 12. Independent recovery and continuation preserved old/new intent, data, security, reservations and unrelated acknowledged bytes. The old 16 actual SDK data/size/Flush traces retained all 1,299 fault cases.

The first ce09720 native build failed in test code (SDK trailing-array value initialization and a wrong metadata accessor). a362338 compiled and passed 109/110 Windows groups; the paired-delete fixture had not adopted its second prepared open. The final fixture keeps the original global-cancel oracle, additionally requires conservative sharing rejection without durable/intent mutation, then performs real SDK I/O on both handles before cancellation. No guard, old case, oracle or limit was removed. Ordinary deletion with an idle unconfirmed peer remains a real-kernel adoption product gate.

At the final SHA, raw Windows and relocated Windows passed 96 CTest groups; codec/raw Linux, relocated Linux and Linux ASan/UBSan/leak detection passed 92. Sanitizer job 129771005863 covers the shared CPU engine, not instrumented Windows SDK callbacks. Portable job 129771013661 retained supervisor-only before/after accounting, two controlled children, no launch retries, files_created false and unchanged caps.

The actual process-interruption campaigns ran at unchanged-source predecessor a362338: native Linux/Windows 976 cases each (Windows disjoint 488+488), basic metadata 712 (356+356), compact root-3 172 (86+86), with complete independent acknowledged-history continuation. Pinned mechanism/reference and RAM/digest comparisons also passed there; their scopes are unchanged. Source routing job 129767122373 bound that implementation to SHA-256 af3211b07b6512686aa4f2ab4a0add4523b404c7041f5e2c6fdc98d7a66fdcb9 with six actual queries. Final test-only correction did not trigger that workflow; do not relabel its receipt as final-SHA evidence. Native dependency inventory job 129764890668 passed at ce09720 with outdated_pins empty, CMake 4.4.5/LLVM 23.1.5/Python 3.15.2; later corrections changed no pins. Dependabot endpoint access remains unverified after the previously retained connector rejection, not a zero-alert finding.

Final read-only mount job 129774755949 passed normal, deliberately terminated-helper and independent recovery/remount phases, exits 0/1460/0 with actual exit confirmed. Three source-bound space reports reconciled and image parity held. Peaks were 6,164,480 / 6,135,808 / 5,156,864 bytes under unchanged limits. The pinned driver's valid Microsoft hardware-publisher signature was inspected on the disposable VM; windows11_policy_qualified remained false. This is read-only regression evidence, not mounted writable, Windows 11 client, live AMD, physical power-cut or complete-product proof.

Next work must qualify a separately supervised writable VM helper and actual adoption/cancellation/cache/backing/security behavior before any PC experiment. Keep native quota distinct from host preallocation/physical compressed size and plain cached WriteFile acknowledgement distinct from FlushFileBuffers durability. SetSecurity, scale/reclaim, app/installer/client, live AMD, automatic learning and payload replica self-repair remain unfinished.


## Executed first writable VM checkpoint

Commit e5292572eeeb58dcf9a5f51913ef6807dec968ee passed all 11 triggered workflows.
Native Windows job 129786410643 passed 110 CTest groups, reported 1,285 checks/
901 callback-or-SDK dispatches and handles 151 to 151. Lifecycle checks numbered
57 with 12 independently reopened retired writers. Bounded history exhaustion
preserved every acknowledged byte: raw 13 writes/image 4,194,304; automatic
17 writes/image 4,128,768, both within 4 MiB. Existing cases/caps stayed intact.

Real writable VM job 129792618538 created/wrote/flushed/read/closed the 7001-byte
sample, retired every context, observed normal dispatcher stop and mount removal,
then independently verified the native image after confirmed helper exit.
Writer/verifier exits were 0/0, no timeout/cancellation, peaks 4,767,744 /
3,059,712 bytes. Both reports matched namespace 9, generation 18, inode 2 and
image 1,966,080 bytes. Only writer reported observing explicit flush.
Old read-only job 129791168891 retained exits 0/1460/0 and three reconciled
space phases; peaks 6,148,096 /6,184,960 /5,160,960. This is not writable
helper-kill coverage. Linux sanitizers passed 92 groups; Windows raw 96.
Six source-routing queries bound e529257 to source SHA-256
807a4a5b091a0c11b5d36299257eba5993e604978f3059be656a43332b5203cd
(job 129786415468). Dependency inventory and old recovery/reference/comparison
campaigns passed with their original scopes. Dependabot endpoint access remains
unknown, not zero. No owner-PC process/artifact, driver change or GPU run occurred.
The full goal remains active; this small trial is not full-product proof.


The fixed extended lifecycle/termination VM profiles are specified in
WRITABLE_MOUNT_TRIAL.md. Configured cases require actual inspected results;
none changes default product routes, stored bytes or the existing guards.


## Executed extended writable Windows checkpoint

Main 4fe5c4000766436780e1350cefc8ed01f3297e6d, tree b92288bacd5b9a740e6deecc4732994443b83b21, passed all 11 triggered workflows. Development used native GitHub APIs and disposable hosted VMs only; no owner-PC process/artifact/test, driver change or GPU run occurred. The full filesystem goal remains active and experimental.

Windows native job 129993232157 passed 110 CTest groups and compiled the extended helper; the five writable-report negative test groups passed before live gates. The pre-existing seven provisioning-test groups were a separate scope (an intermediate progress count was corrected). Existing callback/cap/lifecycle/fault oracles remained intact; handles were 151->151, worker handles 94->94, cap raw/automatic cases retained 13/17 acknowledged writes and last-byte preservation. Current source routing job 129993233120 completed six real queries bound to source SHA-256 6cdeca8f64fd00c369dcf42647a224b7c2426d1fc4d18ec446e43df31743dc10.

All four actual writable VM profiles passed, including strict JSON/source/phase/PID/type/budget/full-primary-state reconciliation:
- basic job 129999263116: original 7001-byte create/write/explicit-flush/read/close; namespace 9, generation 18, image 1,966,080; writer/verifier exits0/0, confirmed; peaks4,759,552/3,092,480.
- mutations job 129999262395: actual EOF73->17->89, preserved17-byte prefix/zero tail, rename with stable held inode2, unchanged keep file, normal retirement, independent CPU reopen; namespace18/generation36/image3,735,552; exits0/0; peaks4,751,360/3,047,424.
- delete job 129999262391: DeleteFileW while original handle retained readable73-byte data, final target retirement and unchanged37-byte keep inode3; namespace16/generation32/image3,342,336; exits0/0; peaks4,734,976/3,104,768.
- crash job 129999262346: 7001-byte actual mounted WriteFile + FlushFileBuffers and flushed acknowledgement, one file context still open, exact owned helper terminated at15s with exit1460/timed_out true/actual exit confirmed; separately supervised CPU verifier exited0. Both report namespace7/generation14/image1,572,864, exact four times/attributes/reservation8192/security/data digests and resolved healthy checkpoint. Peaks4,775,936/3,092,480. This is ONE post-flush idle-helper termination case, not arbitrary mid-transaction cuts, physical power loss or writable remount/continuation.

Each profile used its own fresh VM/fixture and the unchanged <=4 MiB image +4096 report bytes, 128 MiB/20% CPU/one-process/below-normal bounds. Ordinary/verifier deadlines remained30s; explicitly terminated source15s. No next child launched without actual exit proof. The crash observer does not claim executing a flush or dispatcher stop, and the writer was not described as normally stopped.

Old read-only mounted recovery job129997388256, raw/codec/sanitizer/portable/reference/native process-interruption, dependency inventory and comparison workflows passed at this commit with unchanged old scopes. Do not relabel read-only/helper-kill/core fault/comparison results as general writable power loss, Windows11 client, production throughput or live AMD proof. Dependabot alert access remains unknown after earlier rejected endpoints, not a zero-alert result.

Next requirements include fully qualified cancelled/idle/provisional and overwrite/mapped delivery, writable restart/continuation after verified retirement, scale/reclamation and real app/installer, Windows11 client, live AMD with CPU fallback, adaptive low-priority maintenance and payload-replica repair. One candidate improvement is to inspect an explicit Windows-specific close/peer-debt policy that can correctly retire owned cancelled contexts without fake initial Cleanup or weakened historical no-I/O provisional APIs; this needs current source and bounded real-kernel evidence, not assumption. Also monitor the crash source's atomic offline/dispatcher state while it waits, without new storage I/O, in the next substantive extension.


The explicit hosted resolved-image resume and fixed restart profiles are
specified in WRITABLE_MOUNT_TRIAL.md. Source retirement must be independently
established by the controller; unsupported/unresolved state rejects. Existing
new-only APIs, metadata/bytes and all old cases/caps remain unchanged.
