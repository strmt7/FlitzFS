# Disposable writable Windows mount trial (experimental)

This is the first deliberately small real writable integration trial, not a
product mount command. It runs ONLY in the separate windows-write-mount job on
a freshly provisioned disposable GitHub-hosted Windows VM, after same-commit
CPU/raw/sanitizer/native checks AND the old read-only mount job pass. Default
builds and CTest never mount. No owner-PC permission, driver change, existing
image writer, writable crash-recovery claim or product qualification follows.

## Source and protocol review

The native release API was rechecked on 10 November 2026: selected WinFsp v2.2B5
(13 October 2026), source 14d6e0280ae15e2550c44d6dbc476be51649727f.
[Current SDK operation context](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
is TLS access to the current request/response.
[Actual dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fs.c)
calls LeaveOperation after the SDK operation returns but BEFORE the next kernel
transaction. Therefore that hook does not prove kernel adoption. The native
source was captured through GitHub APIs; a guessed dispatcher.c path returned
404, then exact-symbol native search found fs.c and it was verified at the
selected immutable pin. No page reader/search snippet supplied source evidence.

The existing provisional-peer guard stays. This first trial excludes deletion,
cancellation/overwrite races, mapped writes and forced helper termination.
Those remain required writable qualification work; read-only kill/recovery
results do not cover them. Unverified callbacks still fence and signal external
offlining, never join inside callbacks or retry storage.

## Strict ownership and limits

FLITZFS_BUILD_WRITE_PROBE is OFF by default and never registers a CTest.
The executable accepts only its fixed hosted modes and a verifier source PID,
not a data/image/mount path. It requires GITHUB_ACTIONS=true,
RUNNER_ENVIRONMENT=github-hosted, RUNNER_OS=Windows and BOTH
FLITZFS_ALLOW_DISPOSABLE_MOUNT=1 and FLITZFS_ALLOW_DISPOSABLE_WRITE=1.

The parent launches one writer child, confirms actual successful exit, then
launches one independent CPU verifier child through run_bounded_process.
Each child has the unchanged 128 MiB process/job, one-process, 20% CPU,
below-normal priority and 30-second limits with kill-on-job-close.
The child rechecks those actual OS limits before creating/reading its fixture.
This is user-mode containment, not a guarantee against kernel hangs.
No GPU, service/scheduled task, arbitrary child, retry or detached worker is used.

The runner-temp ancestors and exclusively created flitzfs-write-probe-PID
directory are pinned without delete sharing; regular directory/reparse/type
checks and 24-pin/path limits remain. There is one NEW ordinary native.fltx,
one 7001-byte synthetic mounted file, and two <=2048-byte reports. The NEW-image
factory has an additional aligned history cap: 4 MiB for this trial, checked
before every projected append and actual read/write range. Aggregate fixture
bytes are <=4 MiB +4096 report bytes. This independent writable fixture does
not raise/change the old read-only campaign's 4 MiB combined limit or any
existing process/store/oracle cap. Files are retained for VM disposal; no
recursive deletion or forced stale-junction removal occurs.

The capped wrapper owns the SAME exclusive new-file backend and lease. It
preserves its memory/free-space/regular-single-link identity admission, explicit
64 KiB failure-unit contract and actual flush barriers. The historical
three-argument hosted adapter factory keeps its API/default 64 MiB bound;
the new explicit overload admits 4..64 MiB in 64 KiB units. Invalid bounds
reject before file creation. Native reservations remain thin live quota, not
host preallocation or guaranteed future history capacity. A bounded raw/auto
unit campaign must exhaust history, return no accepted output, retain fenced
ownership and independently recover the last acknowledged byte without
exceeding the cap.

## Actual trial and evidence

The helper supplies the explicit native provider, starts two dispatcher threads,
mounts only its fresh private directory and verifies the target is writable
FlitzFS before creating anything through it. It creates sample.bin with Win32
CREATE_NEW, performs one cached WriteFile, then explicitly FlushFileBuffers.
It checks EOF/aligned cache reservation, persisted inode identity and actual
creation metadata; reads back through the same handle, closes it, and reads
again through a new handle. Plain cached WriteFile acknowledgement alone is
never the persistence oracle.

All application handles close before bounded context-drain observation.
Failure/offline/unexpected stop or remaining contexts fails the trial.
The controller stops/joins, removes/deletes only owned filesystem state, records
the actual dispatcher notification, releases the writer, and independently opens
and fully verifies the native image. Startup rollback now preserves its first
stop reason and diagnostics instead of dropping them with the adapter.
Missing stop notification after attempted dispatch is a failed outcome.

After confirmed helper process exit, the separately bounded verifier checks
mount absence and independently opens/verifies the image, namespace/data,
security descriptor, reservation and replicated healthy checkpoint. It starts
no dispatcher/driver/mount. Reports bind phase, source commit/PID, observer PID,
exact byte count/inode/sequence/generation and image budget. Only the writer
reports observing an explicit flush; the verifier reports its CPU observation.
The parent reads bounded regular single-link non-reparse reports and emits
the native facts after actual successful exits. Review exact executed jobs;
configured steps are not passing evidence.

Latest Windows 11 client/app/installer, broader names/semantics, SetSecurity,
deletion/adoption/cache/kill recovery, realistic scale/reclamation, physical
power-loss/media/payload-replica repair, live AMD and adaptive low-priority
maintenance remain unfinished. This trial cannot prove full-product safety
or superiority over other filesystems.

## Extended fixed profiles (configured, inspect executed evidence)

The original basic mode and every assertion remain. The matrix now runs four
separate fresh disposable Windows VMs: basic, mutations, delete, crash.
Artifact names include the profile to prevent overwrite/collision. Each fixture
still has one <=4 MiB image and two <=2048-byte reports, never an aggregate-cap
increase within a VM or a shared writable image across workers.

- mutations creates an acknowledged target and an unrelated keep file, shrinks
  target EOF to 17, regrows to 89, explicitly flushes and verifies the preserved
  17-byte prefix/zero tail, renames with the original inode still open and
  checks stable inode identity/readback. Independent CPU reopen verifies both
  final files and absence of the old name.
- delete creates the same two files, calls DeleteFileW while the original target
  handle stays open, verifies original bytes through that handle, then closes
  and drains all contexts. Independent CPU reopen requires only the unchanged
  keep file and no removed target/unlinked session work.
- crash creates/writes the original 7001-byte sample, actually FlushFileBuffers,
  checks mounted and native bytes, and flushes an ordinary bounded acknowledgement
  report. An immutable native view is dropped before waiting; the original
  application handle remains open. No more application I/O occurs afterward.
  The private supervisor must terminate that exact process at 15 seconds and
  confirm exit 1460 before launching the 30-second CPU verifier. No dispatcher,
  mutable recovery or remount is used by the verifier. Mount disappearance and
  a healthy, resolved current checkpoint are required.

Extended reports use schema 2: source/observer PID, exact source commit, profile/
phase, inode/file count/bytes, namespace/generation/image cap, context phase,
all four persisted times, attributes/reservation and existing BLAKE3 record
digests of primary data/security and unrelated kept data. Only an actual writer
reports observing explicit flush; a CPU verifier never claims that operation.
The crash acknowledgement may retain its one live file context, while every
normal post-stop and independent observer reports zero owned contexts.

The standard-library Python verifier rejects missing/extra/duplicate keys,
wrong schema/profile/phase/source/process, bool-as-integer confusion, budget/
alignment/retirement errors and any changed exact acknowledged data, primary
metadata/security/reservation, namespace sequence/generation or image extent.
It checks bounded regular single-link non-reparse report/image files in the
one process-bound fixture. Its in-memory negative tests run before live gates;
a valid JSON document alone is not recovery evidence. The C++ parent separately
requires actual process-object exit before each subsequent child. PID inequality
is not a replacement for that proof.

Selected current WinFsp source fsop.c at
14d6e0280ae15e2550c44d6dbc476be51649727f was re-read through native GitHub APIs:
classes 19/20 dispatch SetFileSize, 13/64 dispatch SetDelete after readonly
precheck, and 10/65 dispatch Rename after its native access check. No callback
guard/adoption/authorization or stored filesystem encoding is relaxed.
These fixed cases are NOT arbitrary writable fault-cut, power-loss, mapped I/O,
cancelled-open/overwrite, latest Windows 11 client, live AMD or product proof.

## Hosted writable restart/continuation (configured)

The matrix adds restart and crash-restart without removing the first four
profiles. The fixed parent first confirms source exit using its actual managed
process object: normal0 or deliberately terminated1460. Only then may it launch
the bounded resumer. That caller-side proof is mandatory; the hosted factory
name/argument is not independent old-context attestation. No PID is reopened.

The resumer verifies the old mount is absent and compares the entire immutable
source acknowledgement byte-for-byte with a newly fully verified native view,
including exact checkpoint, primary metadata, descriptor and data. It drops that
read-only handle before reopening. resume_retired_hosted is an explicit internal
HOSTED experiment, not production public ordinary-file resume. The backend
acquires its existing OS-exclusive writer lease and regular/single-link/file
checks. Wrong identity/generation, unsupported Windows tuple/security/allocation,
missing original volume time, excessive history and unresolved session state
reject BEFORE resume may repair a root. No fake old Cleanup or default descriptor
is introduced. Valid admitted resume retains old append-floor/root-repair/codec/
headroom/barrier rules and original volume identity/creation/permissions.

A new private remount reads the saved7001 bytes, creates continued.bin with
3073 deterministic bytes, explicitly flushes and reads it, closes every app
handle and normally stops/joins/releases the second writer. A separately
supervised final CPU observer starts only after actual resumer exit, verifies
both files/inodes2+3 and the complete final checkpoint, and checks both old and
new mount directories are absent. No arbitrary existing path/mount is accepted.

The source report's original bytes remain immutable. An exclusively opened,
regular single-link non-reparse receipt appends ONE bounded JSONL continuation
record after rechecking that original prefix. Source+append remain <=2048 bytes;
the final independent report remains <=2048 bytes. There is no third report and
no image/aggregate/process/CPU cap increase. The receipt binds the exact source
acknowledgement digest and canonical final state digest to source/resumer IDs,
commit, namespace/generation and extent. Final CPU observation carries the
actual resumer PID supplied by the parent, which observed that process exit.
Typed reconciliation rejects missing/mixed/changed/relabelled records, rollback,
non-appending continuation or mismatched independently observed state. Original
data/security/creation/attributes/reservation and write/change times must remain;
the explicit new read may advance access time. Producer-state byte comparison
is exact BEFORE that new operation.

Unit coverage must reject active writer exclusion, wrong identity/stale
generation, unsupported native Windows metadata and unresolved pending intent
without changing backing bytes; valid raw/automatic restart must append and
retain source bytes/inode/volume creation. This resolved-only admission does
not qualify generic pending/deleted-session repair or production cold startup.

Acknowledged-source waits now inspect only atomic offline/dispatcher state,
with no new storage/GPU work, so unexpected source dispatcher failure before
the controlled termination is a failing outcome.


### First restart execution retained

At 4552d21 the four old mounted profiles, all110 native CTest groups, six
report-validation groups and raw/automatic restart unit cases passed. Both
new source/resumer/final-verifier sequences also returned the required actual
exits (restart0/0/0, crash-restart1460/0/0) and exact continuation-state digests.
The final Python extent check failed because reconciliation returned the
producer's pre-continuation extent/generation rather than the independently
verified final state. The follow-up fixes that result selection and adds
explicit regression assertions for all three returned final fields. No byte,
metadata, process, source or cap check is removed or raised. Inspect the
follow-up's full executed checks before claiming a passing restart checkpoint.


## Verified resolved-image restart checkpoint

Executed qualification at 04bdef47de8e3dbdb45728c775b19fd45eb78750: all nine triggered workflows succeeded. All six writable profiles reconciled complete with source-bound reports and confirmed child exits. Normal restart advanced to generation 30, namespace 15, image 3145728 bytes; controlled-termination restart advanced to generation 28, namespace 14, image 2949120 bytes. Both retained 7001 original bytes and verified 3073 continued bytes through an independent CPU reader. Metadata attempt one timed out configuring Zstd before filesystem execution; unchanged-limit failed-job retry succeeded on a new runner. Preserve failed attempt. No owner-host or GPU execution; no physical power-loss evidence. Fold into next substantive batch.

## Locked RAM handoff profile

The hosted `ram-handoff` profile uses the existing writable probe supervisor
with its 128 MiB job memory limit, 20 percent CPU cap and 30-second deadline.
The source, candidate and volume head use locked RAM. A temporary undersized
candidate exercises CPU recovery while two Win32 handles remain open.

The successful handoff checks the mounted volume serial, file ID, retained
handle reads, a continuation write and Flush. After both handles close and the
dispatcher stops, an independent CPU reader verifies the selected image and
the pinned source. The fixture verifier requires mount removal, released
locks and a single bounded report in the fixture directory.

The adapter uses the same mutex for maintenance and callbacks. The test invokes
maintenance explicitly while the mount is running. The locked RAM images are
volatile. The scope is a disposable Windows VM with the pinned WinFsp driver.
