# Isolated writable source and external client (experimental)

11 November 2026. D01 candidate; executed qualification is required. This
document does not declare the intermittent retirement defect resolved.

## Evidence and hypothesis

The earlier source at d1e66f6 reached its explicit-flush acknowledgement but
did not confirm child exit after the unchanged 15-second operation deadline
and two-second observation. The underlying kernel state was not captured.
The 343acaa matrix later passed; that does not explain the previous failures.

The old crash/crash-restart source also held the Win32 client file handle
inside its filesystem-host process until forced termination.
[Upstream issue 702](https://github.com/winfsp/winfsp/issues/702), created
29 October2026, describes a self-namespace-I/O termination hazard. It is a user
report without maintainer confirmation, not proof of our failure's cause.

The selected current SDK is v2.2B5, source
14d6e0280ae15e2550c44d6dbc476be51649727f.
Its [originating-process API](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
is FspFileSystemOperationProcessId, valid ONLY during Create/Open or an eligible
Rename. The exact pinned header blob is
0bfa8a25d8d33e893c52df49bdfc9bafafcbbb5b, read through the native GitHub API.
No process identity query is added to arbitrary callbacks.

## Ownership protocol

The existing controller becomes the external application client. One separately
supervised child hosts the source filesystem. There is no additional process,
service, driver, arbitrary executable, mount path or image argument.

1. The controller creates three unpredictable, protected local-session manual
   events: ready, written and acknowledgement. Objects grant only the current
   user SID; inheritance is disabled and an existing name is rejected.
2. The host opens the role-specific event rights and starts only its own fresh
   disposable mount. Setup must leave zero native file contexts.
3. Test-only Create/Open guards are armed. An actual self CreateFileW attempt
   must fail ACCESS_DENIED, without acquiring a native context or changing
   generation/extent/namespace. Original native authorization/adoption checks
   still handle admitted requests.
4. The host signals ready. The external controller opens the mounted namespace,
   creates/writes/FlushFileBuffers/reads sample.bin and keeps that handle OPEN.
   The host's pinned SDK callback must observe that controller's real PID;
   a successful original Flush callback must also be observed.
5. The controller signals written. The host independently verifies the native
   image, confirms one external context and publishes its bounded acknowledgement,
   then signals acknowledgement and performs no further data I/O.
6. The original process supervisor terminates the host at15 seconds and retains
   its exact process object until confirmed exit. The external client handle
   remains open through host and supervisor retirement, then closes.
7. Only confirmed retirement authorizes the existing resumer/CPU verifier.
   The retained source process lease prevents PID reuse during reconciliation.

Ready/written/acknowledgement phases each have five-second diagnostic bounds.
These do not replace or widen the host operation/termination deadlines.
Unknown retirement is terminal: no replacement source, no next verifier and
no successful qualification. One bounded test-only quarantine retains unknown
ownership until the failed controller exits; it is never an owner-device route.

The guard wraps the actual pinned SDK callback table only for the killed source.
It is not a replacement production authorization mechanism. Normal ordered-stop
profiles remain unchanged. General Windows helper self-I/O policy still requires
production design and client qualification.

## CPU-only prerequisite and resource containment

supervised_source_fixture.hpp schedules the existing run_bounded_process;
it does not duplicate its process creation, argument quoting, creation-time job
assignment, memory/CPU caps, kill-on-close or exit-code logic. It keeps the exact
observer and a separately confirmed supervisor-thread lease. One source slot
cannot be reused until both retire. Unknown observation never reopens a PID.

Normal Windows CTest uses only its own trusted CPU helper for real event-role,
collision, identity, cancellation, timeout, abandonment, failed-launch and
repeated handle-retirement cases. It never creates a filesystem or dispatcher.
These checks must pass before the existing isolated mount jobs can start.

The controller reuses the already qualified CPU-only diagnostic budget:
128 MiB/process,256 MiB aggregate, two active processes,20% CPU, below-normal,
ten seconds per-process user CPU time and fail-closed physical/commit headroom.
Despite its historical gpu_probe_budget.hpp name, this utility performs no GPU
operation. Each source/resumer/verifier keeps its original128-MiB one-process
20%-CPU kill-on-close child job. Source deadline remains15 seconds; ordinary/
resumer/verifier deadlines remain30 seconds; child exit observation remains2.

The hosted PowerShell wrapper adds a100-second TOTAL controller wall bound,
plus2-second termination confirmation, disposing its process handle and
checking bounded output (16 KiB stdout/4 KiB stderr). This encloses the existing
three child phases; it does not make any child deadline longer. A kernel fault
still cannot be converted into a no-freeze guarantee by user-mode limits.

Images remain <=4 MiB, source+continuation report <=2048 bytes and independent
report <=2048 bytes. No third report, recursive deletion, in-place repair,
source overwrite, GPU work, owner mount or owner-driver operation is added.
All fixtures remain on disposable GitHub-hosted Windows machines.

## Evidence compatibility

The historical schema2 evidence stays readable. The new crash source variant
adds exactly source_protocol="external-client", client_process,
host_self_io_rejected and client_flush_callback_observed. The acknowledgement
observes the last two facts; the independent verifier must report them false,
while binding the same original client PID and exact source acknowledgement.

The new live crash campaign REQUIRES that variant. Missing, mixed, malformed,
self/observer/resumer-aliased actors and invented observer claims reject.
Old reports cannot silently satisfy the new live actor-isolation gate.
Native image encodings and existing worker frames are unchanged.

The source acknowledgement still binds every old byte, metadata, security,
reservation and checkpoint property. Restart still compares its complete
original acknowledgement before acquiring the exclusive writer lease.
Continuation/final state digests include the new protocol fields. A passing
JSON record alone is not a mounted recovery result or an explanation of the
historical failure.

## Qualification required

Review CPU/raw/native/sanitizer checks, exact SDK build and both affected
mounted profiles, including real source guard rejection and independent
recovery. Preserve failures and test before/after causal evidence rather than
declaring D01 fixed from a green run. No owner experiment is authorized.

## Executed checkpoint at 8122bf5

The initial full 13-job matrix passed at
8122bf51f6ac3cf43512f8a2dc37368afa0772c5. Windows native CTest passed 131 checks,
Windows raw passed 117, and both Linux configurations and ASan/UBSan passed.
Both affected mounted profiles observed the real external client, rejected
the host's self-open and confirmed source termination with exit 1460 under the
unchanged bounds. Independent CPU recovery retained the acknowledged state.
The crash-restart profile also resumed writes under a separate process.

[Exact reports and run/actor/retirement evidence](evidence/mount-source-isolation-8122bf5.json)
retain raw integer text and distinguish the initial matrix from repetitions.
These observations do not reconstruct the older kernel state or establish
that issue 702 caused those failures. D01 remains open.

The next CPU-only fixture revision additionally covers cancellation and
destructor abandonment after acknowledgement, plus an early real child exit
before ready. It requires the exact nonzero exit 37 and confirmed retirement;
neither ready nor acknowledgement may be invented. Warm each real path, then
repeat protocol, failed launch, post-ack cancellation, post-ack abandonment and
early exit eight times with exact handle equality. No tolerance, mount, driver,
GPU operation, larger deadline or new product route is added. These new checks
require their own publishing-commit qualification.

## Ordered external clients for basic, mutation and deletion profiles

The next D01 change moves the actual Win32 client workload for basic, mutations
and delete into the existing supervising controller. The filesystem child now
owns only its backend, adapter and dispatcher. These profiles reuse the existing
protected ready/written events and the exact Child supervisor; there is no new
transport, process, service or worker loop.

The child first proves rejection of its own Create through the same SDK callback
guard as the crash source, without a context or native generation/extent/namespace
change. It then signals ready. The controller independently checks the mounted
identity, performs every original write/read/metadata/EOF/rename/delete operation
and closes its client handles before signaling written. The child requires the
actual originating controller PID and successful original Flush callback, drains
contexts and performs the original ordered dispatcher/writer shutdown.

The controller requires confirmed child AND supervisor retirement before an
independent CPU verifier can start. Its source lease remains alive through that
verification. The existing normal 30-second child operation, five-second protocol
phases, two-second exit observations, 128-MiB process/256-MiB aggregate and 20%-CPU
limits are unchanged. The crash source keeps its original 15-second deadline and
open external handle through forced retirement.

Basic schema1 and extended schema2 reports retain their historical fields.
The existing external-client variant now also covers basic, mutations and delete:
writer self-rejection/flush observations are true, independent verifier observations
are false, and both bind the same distinct client PID. The current live campaigns
require that variant; legacy self-client records remain readable only as historical
evidence. Missing/mixed actors, aliased PIDs and invented verifier observations
reject. Report counts/2048-byte limits and all stored filesystem bytes are unchanged.

This publishing source requires its own CPU/Windows/sanitizer and affected mounted
qualification. It does not resolve D01's historical kernel state. Ordered restart
source/resumer, RAM-handoff and read-only mounted workloads still need actor
separation before adapter-wide self-origin rejection can be enabled. The current
guard remains fixture-specific; no production guard or owner mount is claimed.
Do not bypass a future guard using a synthetic origin provider to keep a live
self-client fixture passing. No unchanged GPU experiment is needed for these
test-only lifecycle changes.

## Restart, resumer and read-only actor conversion

Source 6dac1f109f7e97965f26fc1cca5a72fab6375cac passed all thirteen core jobs.
Its basic, mutation and deletion records bind distinct host/client/verifier PIDs,
actual source self-open rejection, original Flush observation, zero contexts
and successful exact source/verifier retirement. Raw report text is retained in
evidence/ordered-actors-6dac1f1.json, preserving full-width integer text.

The next change moves the ordered restart source AND both normal/crash writable
resumers to the same external-controller protocol. The controller performs every
original read/write/Flush operation, then closes its handles before signaling
written. A resumer first compares the complete original acknowledgement, acquires
the original exclusive backend lease and mounts; its self-open probe must preserve
the nonempty old namespace as well as generation/extent/context state. The actual
controller PID and successful original Flush are required before ordered stop.
The retained source and resumer process leases survive through independent CPU
verification. Restart now requires the external-client report variant in the
live campaign; historical self-client records remain readable.

The read-only normal, kill and recovery/remount workloads also execute in their
external controller. They retain sequential/random/mapped/concurrent reads,
32-bit-crossing sparse tails, inode/basic identity, enumeration/write rejection,
all three exact space reports, unchanged backing checks and independent recovery.
The host performs only backend/adapter/dispatcher work and scoped non-mounted
fixture validation. The controller holds the exact retired process leases through
the campaign, never reopens a PID and records distinct actor/retirement observations.
These read-only records do not yet attest to an SDK request-origin guard.

The read-only controller gains the same explicit 128-MiB process/256-MiB aggregate,
two-process,20%-CPU,below-normal,ten-user-CPU-second and fail-closed headroom budget.
Its existing 30/15/30-second child deadlines and two-second exit observations remain.
The hosted wrapper adds a100-second total outer deadline plus2-second termination
confirmation. Bounded stdout is32 KiB to preserve three existing <=8192-byte space
reports plus fixed diagnostics; stderr stays4 KiB. This bounds a previously
unbounded controller output stream; no report or child limit is enlarged.

All new actor paths require their own publishing-source qualification. No current
green source explains the historical kernel state, establishes full Windows 11
client behavior or enables an owner mount. RAM handoff still needs its bounded
multi-round external-client protocol before adapter-wide self-origin protection.
No live self-client may be hidden behind a synthetic origin query.

The reviewed isolated Crawl4AI research lane now offers a process suite: existing
wait/thread/close references, current Microsoft Create/Set/Reset/OpenEvent and
event-rights pages, then process/job termination references. It retains its locked
provider, robots checks, native receipts and VM limits. The source-group default
tracks this lifecycle work; every prior explicit AMD/app group remains available.
Read and admit substantive exact-source captures before designing the new RAM
multi-round protocol; a successful installation or queued crawl is not evidence.

## Finite external RAM-handoff client

Source9dd6a98384f3c315522d1ae840afd43ac862bb0b passed all thirteen core jobs.
Its restart/crash-restart source and writable resumer both retired before
independent CPU verification. The read-only normal/kill/remount campaign observed
distinct controller/host PIDs and confirmed source/supervisor retirement. See
evidence/extended-actors-9dd6a98.json for raw reports and exact job/artifact identities.

The next RAM-handoff implementation moves its actual Win32 file handles into
the existing external controller too. The filesystem child owns only the bounded
locked-memory backends, adapter and dispatcher. A separate test-only finite
conversation uses six action events and one acknowledgement event, all initially
nonsignaled auto-reset objects. It transports no paths, HANDLEs or payload bytes.
Each action has exactly one outstanding request and one acknowledgement; repeated
signals are never treated as a counter. No PulseEvent, discovery/retry loop,
persistent worker or new process is introduced.

The ordered actions are initial write/two-handle read, post-capacity-recovery read,
foreground rewrite invalidating a stale candidate, one read per bounded copy step,
post-publication identity/read, and final append/read/handle close. The independent
state machines admit1..256 copy rounds, advance only after the exact pending
acknowledgement and become terminal on foreign/order/overlap/cap/role/wait failure.
All six non-payload commands must occur in order; at most261 requests are possible.
A completed conversation cannot be reused. Child exit is prioritized over a
simultaneously pending request; all waited handles retain exact owner lifetimes.

The protocol reuses protected current-user Local-session names, BCrypt nonces,
no inheritance and role-required SYNCHRONIZE/EVENT_MODIFY_STATE handles. Existing
names are rejected without changing their state. These constraints contain a
trusted test binary, not an adversarial same-user process. Per-phase waits remain
five seconds; the source retains its original30-second/128-MiB/20%-CPU job and
the controller its128-MiB process/256-MiB/two-process aggregate budget.

Every old mounted handoff oracle remains: capacity failure recovery, stale
candidate rejection, low-level copy progress, retained file identity and two
contexts, reduced payload, repeat suppression, tail append, independent selected
and pinned CPU bytes, root metadata, source/target/head integrity, dispatcher
retirement, working-set restoration and zero remaining locked bytes. Normal
client handles close before final acknowledgement. The same fixed synthetic
UUID's volume serial is checked; no host specification is assumed.

RAM schema1 observations retain a historical-reader path. The current live gate
requires distinct host/client actors, actual self-open rejection and original
Flush observation, in addition to every old resource/byte/progress field. Partial,
aliased, uncompleted, foreign and invented observations reject. The report cap
remains2048 bytes. CPU-only tests cover pure sequence transitions plus actual
trusted helper commands, wrong-phase peers, early exit, cancellation, collisions,
roles and repeated exact handle equality. They perform no mount, driver or GPU work.

The eleven Microsoft captures admitted at9dd6a98 are retained by exact artifact,
provider, URL/status and native/markdown digest in evidence/process-events-9dd6a98.json.
Current event semantics require one outstanding action and lifetime-safe waits;
termination requests remain separate from exit confirmation. Those captures were
fetched now, not presented as newly published2026 API features.

The publishing source still requires its own CPU/raw/sanitizer/Windows and actual
RAM-handoff qualification. Adapter-wide self-origin protection is the next product
boundary, not already implemented by this fixture. The historical D01 kernel cause,
Windows11 client qualification and full-product goals remain open. No owner mount/
driver operation or unchanged GPU rerun is authorized.

## Product guard replaces fixture authority

Sourcefa9fe90 qualified the final external RAM client across all thirteen jobs.
The next source moves own-Create/Open admission into both product adapters.
Writable source wrappers now delegate their own origin and audit the product's
denial/diagnostic; they cannot intercept the request first. Read-only normal/
kill/remount sources also require real own-open denial. Every mounted actor table
supplies the actual SDK query, with no live fake-origin escape. Direct CPU-only
fixtures and missing/throwing provider tests remain separate synthetic evidence.
Read WINFSP_ORIGIN_GUARD.md. Qualification is pending; the historical cause and
the broader kernel/backing/lifecycle failure model remain open.
