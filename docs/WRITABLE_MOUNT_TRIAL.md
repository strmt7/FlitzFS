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
