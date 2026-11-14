# FlitzFS safety gates and threat model

## Qualified isolated compression runtime diagnostic

[HIP_LZ4_RUNTIME.md](HIP_LZ4_RUNTIME.md) defines a separate opt-in SDK worker and private-module artifact. The old worker/default filesystem remains unchanged. All target images and source-bound module hashes must qualify; malformed requests, unknown coverage and unconfirmed retirement cannot publish data. CPU/refusal tests do not prove GPU execution or authorize an automatic owner trial. One actual synthetic 4-KiB trial passed at 979cd512 with CPU readback, exact retirement, unchanged inputs/runtime and no remaining owned processes; see [the evidence](evidence/hip-lz4-runtime-979cd512.json). This does not authorize an unchanged rerun, mounted/normal routing or relaxed resources. Keep the existing host/driver constraints and historical retirement uncertainty.


## SDK-only compute boundaries

[HIP_LZ4_COMPUTE.md](HIP_LZ4_COMPUTE.md) defines the new typed compute contract. It is compiled only in the optional private SDK fixture and has no normal caller. CPU fake tests do not authorize owner-device execution, module loading or mounting. Retain exact process/supervisor retirement and independent CPU verification; a failed resource release overrides acceptance. Original memory caps and historical unresolved retirement failures remain controlling. No stored bytes or format change.


The owner's primary constraints are **no formatting, no lost data, no system
freezes and no resource exhaustion**. These remain release blockers, not claims
that finite tests can prove immunity to software, hardware or power failures.
Use isolated synthetic fixtures; never rely on this preview for sole-copy data.

## Current permitted effects (0.3.1)

The CLI reads explicitly supplied ordinary files and can create one explicitly
supplied NEW output. OS-exclusive creation prevents replacing an existing path.
The catalog, projection and optional WinFsp callback backend have no write API.
The backend returns explicit write-protected statuses for mutation callbacks.

Default builds and CTest do **not** create a filesystem, start a dispatcher,
install a driver or mount anything. The separate optional hosted-CI probe below
performs actual driver-backed synthetic mounts and forced-helper recovery.
There is no raw-disk access, partitioning, formatting, scheduled task, GPU
execution or encryption. A bounded CPU-only synthetic run preceded the owner's
subsequent prohibition on further host disk activity. No GPU, driver-backed
mount or power-loss experiment ran on that PC. All further disk-based work
must use disposable hosted machines/VMs; RAM-only host tests are not qualified.

Tests create small, uniquely scoped temporary fixtures and remove only their own
listed files, not directory trees. The normal hosted Windows callback lane
stages a checksum-pinned SDK by extracting the MSI and reading its tables in
mode 0; that lane never runs the installer or starts a driver/service. A separate
explicit integration job provisions only its disposable VM as documented below.
Normal CMake configuration downloads and installs nothing.

## Enforced resource limits

| Resource | Bound |
|---|---|
| Pack input; CLI extract/verify logical bytes | 256 MiB default; explicit maximum 4 GiB |
| Chunk | 4 KiB to 1 MiB, power of two; default 128 KiB |
| Stored chunk | At most 2 MiB |
| Single random read | At most 4 MiB |
| `.flfs` reader cache | One decoded chunk, at most 1 MiB |
| `.flfs` compressed scratch | Lazily retained per reader; at most one stored chunk, 2 MiB |
| FLZIP snapshot / logical bytes / index | 34 MiB / 32 MiB / 1 MiB |
| FLZIP decoded block / cache | At most 16 MiB each; one cached block |
| FLZIP composition scratch | At most one extra decoded block, 16 MiB maximum |
| Reader index | At most 1,048,576 entries / 32 MiB serialized index |
| Catalog | At most 16 backing files; 4 MiB aggregate serialized index |
| Plaintext catalog manifest | 8 KiB, 16 records, sibling-only container references |
| Index admission | Checked before index buffer/entry-array allocation |
| Catalog full verification | 32 MiB default; at most 256 MiB, preflighted |
| Native adapter startup verification | Fixed 32 MiB aggregate logical-byte budget |
| Native adapter | At most 257 nodes, 256 open contexts, two active decodes |
| Virtual paths | ASCII only, 240 characters total, 100 per component, depth 16 |
| CPU compression | Levels 1 to 5; one stream, one reusable context and output buffer |
| Output disk-space preflight | Worst-case output plus 1 GiB available reserve |
| CI | Two requested build jobs; sequential CTest, 45 seconds per test, 10 minutes per workflow job |
| GPU resources | Not used |

The serialized-index cap is **not** a full process-RAM ceiling: decoded caches,
entry arrays, SDK/runtime memory and allocator overhead are separate. The active
read limit does not cap all callers of the lower-level catalog. Direct Reader
calls are single-threaded; catalog reads/verification lock each backing file.
No object may be moved or destroyed while its callbacks are running.

CPU pipeline reuse adds no worker threads or GPU operations. A reader can retain
one compressed-input buffer and a decoder context until destruction, in addition
to its existing decoded cache. Sixteen catalog readers can therefore retain up
to 32 MiB of compressed-input scratch plus up to 16 MiB of decoded caches; indices,
codec contexts, caller output and allocator overhead are separate. Codec context
allocation remains a dependency operation, not a new whole-process RAM guarantee.
A failed load invalidates the cache identity before touching its buffer, and a
failed decoder operation discards its context before another attempt.

## Path and handle boundaries

Virtual paths and physical host paths have different policies. Virtual paths
never resolve an OS directory, and reject traversal, controls, streams, reserved
names and all unsupported characters, including characters after a dot.

Physical Windows paths are screened BEFORE OS opening/creation for device and
extended namespaces, UNC paths, drive-relative syntax, stream colons, NUL,
reserved DOS aliases (including superscript COM/LPT digits), invalid characters
and ambiguous component suffixes. Windows-created output handles are checked
as ordinary disk files before writing. Input handles reject final-component
reparse points; POSIX uses no-follow/nonblocking open followed by regular-file
validation and now rejects embedded NUL explicitly as well.

These checks are not a complete sandbox. Parent-directory reparse/symlink
containment, changed drive mappings and hostile concurrent directory mutation
are not fully controlled. Use a trusted, disposable local directory. Do not
advertise arbitrary attacker-controlled paths as safe.

## Integrity, identity and durability limitations

- CRC32 is accidental-corruption detection, **not authentication**. An attacker
  can modify data and recompute it. Confidentiality is not provided.
- Windows readers use read-only sharing, blocking ordinary new write/delete
  opens while the handle is held. This is not a storage snapshot or proof
  against every form of mutation; POSIX backing data can still change.
- Zstandard is trusted dependency code. Frame/output checks bound our records,
  but do not constitute an independent codec audit or whole-process bound.
- The optional adapter verifies all data before exposing callbacks, but has no
  authenticated persistent manifest. Its owner-only descriptor does not retain
  source ACLs. Timestamps are synthetic and allocation sizes are logical sizes
  rounded to 4096, not physical compressed storage use.
- New-file writes can leave partial new files on interruption. The footer and
  flush do not establish transactional filesystem recovery. Preserve originals.
- Free-space checks are preflight checks, not reservations. Concurrent disk
  filling, quotas, driver faults and power loss can still cause errors.
- Passing native callbacks does not qualify mounted I/O, actual authorization,
  memory mapping, kernel cancellation/shutdown, Windows 11, or arbitrary apps.

## Prohibitions for later work

Never format, repartition, alter boot state or write raw disks. Never install a
driver, mount over existing data, change GPU tuning, run stress tests, register
services or schedule persistence automatically on the owner's machine. Keep
backing stores, recovery tools and binaries outside any eventual virtual volume.
Never enable writes on important data before isolated recovery tests and
independent backups. GPU output must match an independent CPU reference.

Current evidence is recorded in [VALIDATION.md](VALIDATION.md). Linux tests and
GitHub-hosted Windows tests are distinct from tests on the owner's PC; none of
the latter have been performed. No production-safety guarantee is made.

## Disposable live integration boundary

The normal core, callback and injected-session tests remain no-mount/no-driver.
The optional live integration probe and its explicit hosted-CI-only installer
are governed by `LIVE_MOUNT_PROBE.md`. Only a fresh disposable hosted Windows VM
may receive the pinned driver. The probe creates synthetic files, pins ordinary
ancestors, rejects an occupied mountpoint and retains originals. No owner-PC
operation is authorized. Job limits are not a driver-memory or kernel sandbox.

Manifest loading is read-only, bounded before allocation and restricted to
lexical sibling filenames. Final link rejection does not authenticate metadata
or contain hostile parent links/hardlinks. The manifest directory is trusted
input. No collection loader silently follows absolute references or accepts
malformed names. See `CATALOG_MANIFEST.md`.

## Bounded FLZIP compatibility boundary

The external archive route uses a verified in-memory byte copy and does not
modify its backing file. All file CRCs are checked before names or ranges are
exposed; CRC is not authentication. Maximum archive/index/block/logical sizes
and the 16-file ceiling remain fixed. A per-snapshot mutex serializes its decoded
cache through catalog callbacks. `.flcatalog` does not implicitly accept FLZIP
paths. Read FLZIP_READONLY.md for stricter-than-upstream compatibility limits.
The hosted real-mount test retains the existing OS process limits and fixture
isolation; it is never automatically run on the owner's PC.

## Native FLZIP CPU decoder expansion

Native entropy/dictionary/sparse/composition frames are decoded only on the CPU.
The same snapshot, logical, block, namespace and verification bounds remain.
Compositions admit no recursion and at most one extra 16 MiB block temporary.
LZ4 decoding uses the bounded library API with exact output extents. No HIP
runtime, GPU experiment or owner-PC access is introduced. This extends the
parser attack surface; deterministic negatives and sanitizers are not a formal
security proof. The compiled reference decoder exists only in optional tests.

## RAM persistence experiment

The new commit-persistence test has no OS file/device, GPU or driver operations.
Its modeled images are at most 40,960 bytes with two pending 128-byte writes.
It enforces 60,000 fault executions and a checked 20-second time budget; CTest
caps it at 30 seconds. These are model bounds, not whole-process RAM guarantees.
Builds/logs/tests run only on disposable hosted VMs. Test-only CRC envelopes
are not authentication, a product format or physical power-loss qualification.

## Raw-object transaction boundary

See OBJECT_STORE_FORMAT.md for the separate experimental transaction core. Its
new-file adapter never overwrites existing outputs; a failed creation may leave
a partial new file. Writes require explicit backend persistence assumptions,
new resource admission and a bounded private transaction. I/O failure makes the
writer terminal; outcome may already have committed, so retry is forbidden.
One surviving root is explicitly degraded. Corrupt latest dependencies fail
rather than silently selecting older contents. BLAKE3 is unkeyed integrity,
not authentication or malicious-tamper protection. There is no writable mount,
existing-store writable reopen or sole-copy safety qualification. File tests
must run on disposable hosted VMs; owner-PC storage remains untouched.

## Filesystem scope

The former archive reader and archive-backed catalog/projection/mount route
have been removed. Historical archive qualification is not current product
coverage or a development requirement. Internal compression mechanisms remain
bounded CPU components with separately tested GPU preparation; they do not
imply an archive product or GPU runtime availability.

## Compressed transaction recovery

Native object records now bind compressed bytes and decoded content separately.
Private CPU round-trip verification precedes backing writes. Failed codecs or
resource admission return an error before publication; actual write/flush errors
still poison the writer and may have an indeterminate durable outcome. No
latest-root corruption is silently rolled back. Custom Zstandard allocators
have 16 MiB encoder / 2 MiB decoder ceilings; transaction/output bounds and file
headroom checks remain mandatory. These limits do not prove total process safety
or hardware power-loss durability. No GPU, owner-PC process or writable mount
is enabled by this compression integration.

## Backend resume and ordinary-file test exception

Custom-backend resume now requires exclusive writer ownership, exact recovery
identity/generation, complete CPU verification and a durable baseline fence.
It preserves uncertain append tails and repairs the missing root before returning
a writer. Failure retains indeterminate-outcome semantics. Read
STORE_RESUME_AND_INTERRUPTION.md. Existing ordinary-file reopening is an internal
hosted-test fixture exception only; public create_file still cannot overwrite or
adopt a path. Process-kill tests do not establish physical power-loss survival.
No owner-PC process, general user-file mutation or mounted write is enabled.

## Native file transaction boundary

The native file layer publishes metadata and changed extents in one core commit.
It admits graph/extent/name bounds and zeroes discarded tails before writes.
Old snapshots retain old records across object-ID reuse. Ordinary file reopening
remains an internal hosted-fixture exception; no mount/CLI write route is added.
See NATIVE_FILESYSTEM.md. Sparse logical size is not physical capacity: only 255
nonzero 64 KiB extents are currently admitted, with no physical reclamation.
Object inventory is capped at 256 and decoded live bytes at 16 MiB. Writer
staging uses at most 63 dirty extents plus bounded metadata; ordinary-file
physical/commit admission is 640 MiB. These are not whole-system reservations.
Staging verifies namespace and touched extents; public open/snapshot still scrub
all reachable data. Untouched-media corruption is detected at full verification
or its next read, not automatically on every unrelated write.
Do not infer physical power-cut, writable Windows application or GPU safety
from these bounded CPU/process tests.

## Compacted-copy and autonomous-maintenance boundary

Compaction copies into an empty NEW volume identity and keeps the source and
partial destinations. It performs no source write/flush, truncation, deletion,
path replacement or automatic background launch. Source ownership is quiescent;
ordinary store readers take cooperating shared OS locks. Root publication and
resource limits are specified in COMPACTED_COPY.md.

Future learning/maintenance must obey AUTONOMOUS_MAINTENANCE.md: very low
CPU/I/O/GPU priority, hard aggregate budgets, fresh headroom, foreground yielding,
bounded debt, isolated cancellation and evidence-aware GPU recovery. No such
executor is enabled yet. Redundant repair must preserve acknowledged history and
stop when a safe reconstruction cannot be verified. No universal immunity to
future bugs/media failure or loss of all copies is claimed.

## Replicated current-checkpoint boundary

Native writes and positive compacted copies now use the replicated metadata
protocol in REPLICATED_CHECKPOINTS.md. A surviving current root/manifest can
recover the same latest acknowledged object set after one root/catalog unit
loss; independent resume restores redundancy before further mutation. A health
preflight prevents overwriting a sole survivor after unnoticed metadata loss.
Legacy generic mode has its older fault envelope. Data payload replicas and
physical/device/GPU/background-maintenance qualification remain unimplemented.
All extra metadata, padding, barriers and history consume the stated budgets.

## Asynchronous worker ownership

The explicit WorkerExchangeSession retains its bounded supervisor, input and
exact process observation after the request owner selects CPU or disappears.
Only supervisor code performs cancellation teardown; ready flags cannot expose
a live helper's bytes. Replacement admission waits for both child and supervisor
retirement. Failed observation disables further transport admission instead of
reopening a PID or repeatedly creating workers. CPU-selected gates remain
terminal. This does not qualify a HIP runtime/driver, protected installer or
no-freeze guarantee, and authorizes no owner-PC execution.

## Exact unchanged-data reuse

Payload reuse requires verified exact bytes at the same inode/block identity.
Possible multi-operation reversions re-read and verify the original before ID
allocation; failed verification fences before publication. No payload sharing,
cross-inode deduplication or reduced flush mode is introduced. Nonempty writes
still commit namespace metadata through three barriers. The additional decoded
comparison scratch is bounded to one 64 KiB extent within the existing staging
admission envelope; it does not authorize owner-host execution or raise caps.

## Integrated worker request policy

The explicit GpuWorkerController does not launch ordinary filesystem work. Its
denied routes expose only prepared CPU bytes, and terminal CPU selection cannot
consume late worker output. It reports a ticket once and holds replacement
admission through exact child AND supervisor retirement; caller-supplied health
cannot override transport ownership. Unknown failure does not prove repaired
driver health. Synthetic CPU-helper fixtures are not HIP qualification. External
helper/configuration authorization, product dependency installation and actual
device/runtime evidence remain required. See GPU_WORKER_CONTROLLER.md.

Manual CPU selection or expiry must not discard later known worker faults.
The integrated controller retains the old verified reference and generation
until late-result classification, independently of any newer CPU request.
Outcome reporting happens once after evidence is available; CPU bytes remain
terminal throughout. The proportional gate/current-batch/late-candidate subtotal
is statically bounded below 2 MiB; transport and allocator overhead are separate.


Native inode I/O delegates to the existing CPU transactional writer and does not
enable a mount, GPU, service or existing-file write factory. A removed inode
fails; it never redirects to a reused path. The accepted count is returned only
after successful publication. The same poison/recovery and hosted-only testing
rules apply. See NATIVE_INODE_IO.md.


Pending deletion and detached inode retention require explicit namespace
capability FLTXNS02 with replicated current checkpoints. Ordinary native
resume, read-only projection and compaction reject unresolved state.
Closed-session completion and detached retirement require independently
established context retirement by the owning caller; no timer or declared
boolean proves it. All old snapshots and unrelated committed data must remain
intact. These CPU primitives do not authorize writable mounts or owner-PC tests.
See NATIVE_FILE_LIFETIME.md.


The native context owner provides only its own descriptor-count evidence.
It cannot attest that old external/kernel contexts have retired. It retains
cleaned contexts, keeps records on failed retirement and fences protocol/
integrity failure rather than retrying or guessing deletion. Read optimization
does not bypass namespace/requested-record verification or full public snapshot
admission. See NATIVE_FILE_SESSION.md. No mounted-write/owner-host permission
or proof of universal failure immunity is added.


Native Flush performs a real backend barrier without changing publication or
releasing contexts. It validates current metadata and namespace and fences
failed/unverifiable completion. It does not scrub every payload, repair media,
infer retired external contexts or qualify physical hardware persistence.
Existing source preservation, backend assumptions and off-host-only execution
remain. See NATIVE_FLUSH.md.

### Windows basic metadata

FLTXNS03 tuples are explicit persisted values with strict presence/attribute/
nonnegative-time validation, not Windows request sentinels or guessed clocks.
Data and tuple changes share a private stage and the same three publication
barriers. Attribute-write authority is separate from data-write authority;
failed updates retain contexts and fence. Existing limits, legacy encoding,
full public verification and delayed untouched-media detection remain.
Read-only SDK projection uses present fields exactly while independently denying
writes. Current synthetic host-oracle/mount checks stay off the owner's PC;
they do not qualify Windows 11 clients, writable caching, physical power cuts,
automatic timestamp policy, security/allocation or arbitrary-failure immunity.

The separately scoped FLTXNS03 process campaign preserves every existing legacy
case, binds all six phase traces and decoded state closures, requires confirmed
writer termination, independently reopens expected old/new tuple/data and
commits/reopens a combined continuation. Strict schema-3 merges reject omitted,
mixed, relabelled or duplicate evidence under unchanged resource limits.
These are hosted ordinary-file process interruptions, not physical power cuts,
owner-PC permission, mounted writes or arbitrary-failure immunity.

The Windows portability harness now explicitly requires private-job accounting
to show only its supervisor before a new child launch and after retirement.
Its existing active-process, memory, CPU, priority and time limits remain.
A bounded wait observes actual retirement; elapsed time alone never authorizes
a launch. Query/quota failures remain failures, with no process retry or quota
increase. A hosted real contention regression and SDK ABI checks must pass
before claiming the revised harness qualified. This is development tooling,
not a new filesystem driver/GPU policy or owner-PC execution.

## Automatic Cleanup bridge

Requested metadata and unlink publish together; failed operations retain
contexts and acknowledged state. The Windows noexcept bridge fences on
unsupported allocation/flags, clock failure or any engine exception.
The vendor VOID Cleanup dispatcher still returns success, so a future writable
adapter must offline through its supervisor outside the callback. This bridge
is not wired to the read-only callback table; no writable route, driver load,
resource-cap increase or owner-PC operation is added.
See [the contract and remaining gates](NATIVE_CLEANUP_METADATA.md).

## Atomic data and automatic metadata

Stable-inode write/truncate may include trusted requested archive/time effects
in the SAME normal replicated commit. No accepted count escapes before durable
success. Admission never invents legacy tuples; invalid clocks reject before
backing I/O, and zero accepted writes apply no effects. Failure preserves owning
contexts and the established terminal rules. NATIVE_MUTATION_METADATA.md defines
the contract and configured fault oracles. No writable callback, owner operation,
allocation support, GPU activation or resource-limit increase follows.

## Commit-bound mutation output

Native write/truncate results now carry bounded facts captured before durable
publication. Forming their success result performs no post-commit backing
query or pathname allocation; scalar assignment/return moves cannot throw.
The active-denial fixture rejects all backing operations after the last barrier
and must still receive the complete success result. NATIVE_MUTATION_RESULTS.md
defines coverage and remaining Windows allocation/security/FileInfo gates.
No encoding, resource-cap, writable-mount or owner-PC permission changes.

## Windows space observation gate

WINDOWS_SPACE_REPORTING.md adds bounded read-only API queries and create-only
8 KiB reports on disposable hosted VMs. Required source/phase/file closure
and presentation facts must reconcile; optional compression API errors are
preserved rather than converted to supported features or zero physical bytes.
The explicit aggregate fixture-length bound includes all retained source/
baseline images, inputs and reports. Actual backing allocation is reported
separately. Process/CPU/time/free-space/driver/owner-host limits are unchanged.

## Opt-in catalog density

COMPACT_CHECKPOINT_METADATA.md adds smaller complete manifest spans only under
an explicit qualified backend failure contract. It never guesses power-loss
geometry from sectors, shrinks root separation, reuses acknowledged units or
removes a durability barrier. New-output opt-in, strict wire admission,
incompatible-contract fencing and source-preserving copy are mandatory.
Default/mounted layouts and all resource limits remain unchanged.
Root-3 real interruption/client/physical-device/owner-host qualification is open.


## Persisted security boundary

NATIVE_SECURITY.md adds bounded opaque descriptor persistence in the SAME
replicated transaction as file/basic updates. CPU framing is not an ACL evaluator;
Windows validates present descriptors before read-only callback exposure.
Data/basic grants never imply full descriptor replacement. Keep all aggregate
metadata/resource caps and legacy absence, preserve NULL versus empty DACLs, and
require real OS/FSD authorization/inheritance/privilege qualification before
writable use. Existing mount callbacks stay read-only. New file checks remain
behind hosted admission; no owner-PC operation or product installation follows.


## Runtime CPU digest acceleration

CPU_DIGEST_ACCELERATION.md keeps the same verified BLAKE3 digest and all
integrity/durability checks. Official runtime CPU/OS dispatch selects separately
compiled implementations; portable fallback remains, with no global ISA flag,
TBB or GPU launch. Test-only feature overrides are separately renamed and cannot
reach the production cache. Keep source provenance, guarded/known-vector parity,
old fault budgets and exact-commit hosted evidence. No owner-PC execution or
physical/client/full-filesystem safety claim follows from this component.


## Atomic native creation ownership

NativeFileSession::create_open reserves bounded context/token capacity before
durable creation and installs it only after one combined name/basic/security/
optional-delete-intent publication. The prepared result/slot uses statically
nonthrowing operations after success; there is no post-commit open or backing
query. Admission errors publish nothing; publication failures expose no handle,
retain existing contexts and terminal/indeterminate fencing. See
NATIVE_CREATE_OPEN.md for the configured denial and modeled fault matrix.

Creation consumes explicit already-authorized grants and canonical metadata.
It does not evaluate parent access, inherit/validate Windows ACL semantics,
reserve allocation, construct complete FileInfo or enable a writable callback.
Old wire bytes, limits, fault campaigns and owner-host restrictions remain.


## Atomic overwrite and implicit driver access checks

The explicit native overwrite event preserves existing creation/security/
identity while resetting EOF and requested attrs/times under the same THREE
barriers. It captures result facts before publication and retains contexts on
failure; history and old snapshots remain. See NATIVE_OVERWRITE.md.

Separate trusted overwrite/supersede authorization is required because the
selected FSD may remove implicit rights from final GrantedAccess after checking
them. Do not infer it from data/delete/basic/security grants or call it an ACL
evaluator. The selected DLL can call Close without Cleanup after failure:
writable integration needs qualified provisional-context retirement/offlining,
not invented Cleanup or dropping poisoned ownership. No writable routing or
allocation reservation is introduced. Owner-PC restrictions remain unchanged.


## Provisional overwrite Close

Only the explicit provisional protocol can retire a healthy context without
Cleanup or storage I/O. Preparation owns no persistent intent and excludes peer
deletion; cancellation never undoes successful overwrite or later acknowledged
writes. Old active contexts keep real Cleanup requirements. Poisoned/fenced
Close retains ownership and never retries storage.

The new VOID Close bridge fences and reports its outcome; the actual adapter
must signal offlining outside the callback. No dispatcher stop/join, timeout-
based retirement or writable route is added. Initial delete-on-close overwrite
and real kernel adoption remain unqualified. NATIVE_PROVISIONAL_OPEN.md separates
the actual user-mode SDK checks from missing driver/client/power/AMD proof.


## Initial delete is not speculative Open deletion

Driver-facing initial delete authority remains volatile until actual Cleanup;
general prepared Open/Create and overwrite cancellation cannot persist that
option or remove a named inode. Eligible Cleanup captures effective pending/
unlink facts before the same publication; all latch/cleaned updates follow
success without another storage query. Nonempty directories retain children
and do not acquire an unsafe durable delete intent.

Runtime-only directory pending guards live admission and is distinct from
independent disk snapshots. Initial cancellation preserves existing explicit
request/latch provenance. Historical create_open's durable-intent option is
not the direct FILE_DELETE_ON_CLOSE mapping. See NATIVE_INITIAL_DELETE.md for
configured failure coverage, actual source/delivery flags and remaining real
kernel/client/writable/AMD qualification. Owner-PC restrictions stay unchanged.

### Native reservations are not physical-space guarantees

NATIVE_ALLOCATION.md adds an opt-in format-5 per-inode live-extent reservation, capped by the unchanged shared 255-extent/16-MiB decoded budget. Pending/detached ownership retains capacity; overcommit rejects before writes. Allocation shrink and EOF/tail changes publish together through the same three barriers. Compression does not bypass reservation charge. These units do not preallocate host clusters or guarantee future append-history/RAM/disk admission. Keep Windows allocation callbacks unwired until actual backing, FileInfo, cache and helper policy are qualified. New modeled faults remain distinct from old process-kill campaigns and physical power loss. Owner-PC execution remains forbidden.

### Atomic allocation events and resource hygiene

NATIVE_ALLOCATED_EVENTS.md keeps native reservation/EOF/basic/security changes in one Stage/three barriers. The original Cleanup bridge still rejects allocation; the new explicit bridge is unwired, rejects impossible delete+trim before clock/I/O, and fences untracked regular trim or engine failure without retry/release. No successful physical-space or writable Windows allocation claim follows.

Throughout development, all agents must leave no orphaned processes, unintended background/scheduled services/tasks, leaked memory/VRAM/handles/threads/mappings or needless resource use. Own and bound every worker, confirm exit/join, release handles/buffers, and test errors/cancellation/repeated use. Unknown retirement fails closed; an observation timeout is not process-exit proof. VM disposal contains effects but does not prove product cleanup. The new SDK fixture checks repeated backing-owner release and OS handle counts; those checks alone do not establish universal leak freedom.

The latest owner instruction narrowly permits later PC testing on verified, exclusively test-owned RAM serving as the virtual filesystem disk. Never use a system drive or real-storage test volume, change/install/reinstall drivers, alter security/boot settings or create persistence. Keep builds/downloads/cache/logs/temp/images off system storage. Verify RAM backing and ample headroom; anonymous pageable memory is not proof of zero physical-storage activity. Preserve hosted-first qualification, strict caps/low priority, synthetic data and tested stop/cleanup. No unfinished writable/GPU/kernel/failure experiment moves to the PC under this permission. Current development has not accessed or tested the PC.

### Windows data bridge output and fencing

NATIVE_WINDOWS_IO.md requires complete output prerequisites before data mutation and returns count/FileInfo only from captured durable facts. Missing original metadata or reservation cannot be silently synthesized. Logical failures return zero output; backing/terminal/unknown failures retain ownership, fence and require offlining outside callbacks, with no I/O retry. Flush retains its actual barrier AND post-barrier verification. No writable callback table, process/service/task, driver/PC experiment or physical compressed-size claim is added. Apply the existing lifecycle/resource and narrow qualified RAM-only/no-driver-change host rules.


### Hosted unmounted writable candidate

The separate NATIVE_WRITABLE_ADAPTER.md candidate owns a new exclusive image,
serializes callbacks, bounds opaque contexts and signals failures for offlining
outside callbacks. Its session joins before destroying callback/writer resources;
unretired contexts/fencing make stop unsuccessful. Tests use only direct SDK
callbacks and injected lifecycle providers, never a real volume/dispatcher/mount.
The old read-only probe and all process/resource/oracle limits are unchanged.
First-operation adoption remains a trusted transition needing real-kernel proof.
Native allocation is thin live quota, not physical preallocation or guaranteed
future write admission; default product routes stay read-only.

Latest careful GPU permission preserves qualified RAM-backed synthetic PC
storage, ample verified headroom, strict low-priority caps, CPU verification/
recovery, actual worker retirement and no driver/security/clock/power changes.
No local artifacts, system-drive activity or unfinished runtime experiment is
authorized by that permission. Current work remains on disposable hosted VMs.
Finite tests cannot establish immunity to every future bug or loss of all copies.
\n
### Separate disposable writable VM trial

WRITABLE_MOUNT_TRIAL.md is a narrowly scoped exception for a new gated hosted
job, after all ordinary and old read-only mounted checks. It uses fresh pinned
owned paths, no arbitrary inputs, a 4 MiB image plus <=4096 report bytes, and
sequential 128-MiB/20%-CPU/one-process/below-normal/30-second children with actual
exit proof. The capped backend retains all ordinary admission/barrier checks.
Only create/write/explicit-Flush/readback/close is attempted initially; old
adoption guards remain. Preserve stop reasons and fail on unretired contexts or
missing notification; never delete stale junctions or raise old limits. This
adds no owner-PC driver/GPU/real-storage permission or product safety claim.


### Separate bounded writable lifecycle/termination profiles

The writable VM matrix adds fixed EOF/rename, held-open deletion and one
post-Flush helper kill. Each runs in its own fresh VM/fixture with the SAME
4 MiB image +4096 report-byte, 128 MiB/20%-CPU/one-process/below-normal bounds.
The ordinary children keep 30 seconds; the explicitly killed writer has 15.
No next child starts before actual exit confirmation. The crash child publishes
and flushes a bounded acknowledgement only after actual mounted FlushFileBuffers
and native byte verification, then performs no more application I/O while
holding its file. Its independently supervised verifier starts no dispatcher
and must reject unresolved session state or any changed acknowledged checkpoint.
No mutable existing-image recovery factory, owner-PC action, mapped/power/device
fault, live AMD or broad crash-coverage claim follows. Old basic/read-only and
all fault/oracle/resource cases remain. See WRITABLE_MOUNT_TRIAL.md.


### Hosted resolved-image restart/continuation

Only the fixed hosted controller may invoke the explicit resumed adapter after
actual successful/terminated source process exit, old mount disappearance and
exact independent acknowledgement comparison. The API declaration is NOT
retirement proof. Exclusive regular single-link existing-file admission, exact
identity/generation, full Windows metadata/profile checks and all payload
verification precede writable admission/root repair. Unresolved session state
and unsupported metadata reject without writes. No production existing-file
CLI/factory or owner-PC action follows. Source, resumer and independent final
CPU verifier run sequentially under unchanged child budgets. All retained
snapshots must end before acquiring a replacement lease. See WRITABLE_MOUNT_TRIAL.md.


The opt-in hosted writable adapter now has a bounded SetSecurity candidate using
WinFsp's merge helper and original per-part granted-access checks. SDK/CPU tests
are configured; inspect exact-commit CI before claiming a pass. No owner-PC, mounted authorization, GPU or physical-power qualification follows. Keep all existing host/resource/driver gates.
See [NATIVE_SECURITY.md](NATIVE_SECURITY.md) for its contract and limits.


The hosted writable candidate now rejects all owner/group-bearing security
updates atomically before merge/adoption/storage access because the selected
WinFsp request does not carry the current requester's token. Original handle
rights and equal SIDs do not supply that authority. DACL/SACL behavior and stored
formats remain unchanged. Full owner/group support remains required through a
separately qualified authenticated request path; it is not complete. See
NATIVE_SECURITY.md and WINDOWS_OWNERSHIP_SOURCES.json for current-source
evidence, regression requirements and driver/host boundaries.


The experimental custom-backend native volume head adds a fixed 128-KiB,
replicated authority for selecting a verified compacted image while retaining
the source and a stable external volume identity. Its publication/recovery,
CPU comparison, minimum-checkpoint binding and fault-test contract are in
VOLUME_HEAD.md. Existing encodings/routes/caps remain unchanged. The CPU model tests passed at
dd388157456d0ae6ed9735c0c16a5e83826b37b7; see VOLUME_HEAD_VALIDATION.md.
This does not qualify mounted head integration. No file-provider, mounted handoff,
source deletion, automatic reclaim/worker, GPU or owner-PC operation follows.


The owner explicitly includes their PC in carefully bounded RAM-only testing.
The optional locked-RAM CPU probe and immutable real-file corpus policy are in
HOST_RAM_TESTING.md. It requires VM-qualified exact provenance, page locking,
hard process budgets and confirmed cleanup, with no WinFsp/driver/GPU invocation.
Small source/tool/report staging on the verified HDD is separately capped; it
never supplies filesystem image backing. Configuration is not a host execution
or mounted/GPU/idle-recompression qualification. Existing formats/routes stay
unchanged. Dependabot OPEN-alert access was successfully verified (0 on the
complete10-November snapshot); see DEPENDENCY_AUDIT.md for scope.

The explicit CPU post-compression primitive converts a preserved raw/fast
source into a separately verified NEW image. It preserves native state and
object generations, streams bounded objects and compares decoded source/target
bytes before replicated publication. See RECOMPRESSION.md. Automatic
idle learning/scheduling and live GPU execution remain unimplemented. The first
six-file owner-PC unmounted RAM CPU result is in HOST_RAM_VALIDATION.json;
its complete image remained larger than the tiny corpus despite compressed
payload reduction. New re-encoding tests require their own exact-commit pass.

Managed native volume sessions now connect fixed-head image handoff to the
existing context-owning I/O layer. Explicit compaction/recompression preserves
live resolved handles, access grants, Cleanup state and stable volume identity.
A failed transition retains both images and the context table for one verified
CPU recovery attempt. See VOLUME_SESSION.md. This remains custom-backend
engine work, with new exact-commit tests pending; no automatic maintenance,
mounted/GPU integration, physical reclamation or power-loss claim follows.

Incremental recompression accepts one qualified new target per managed session.
The caller serializes every step with foreground I/O. During copying, the source
writer remains active and the candidate has no roots. A newer committed source
invalidates the candidate before another step or publication. Cancellation
releases its lease without deleting or changing storage. A candidate error
before handoff leaves the source selected. The existing fenced recovery path
still governs failure after actual writer retirement.

A hosted controller can offer an idle copy or a separate final publication
opportunity. The existing callback mutex is acquired without waiting and
active or waiting foreground callbacks defer the offer. The caller still owns
priority, lifecycle and fresh backing admission. A foreground arrival after
admission can wait for the current operation. Full final verification remains
synchronous. No scheduled task or production background service is enabled.

External compression hints are limited to 64 KiB input and 4096 matches, with exact byte and range checks. Optional sequence compression retains the ordinary CPU result on a candidate error. No hint becomes persistent authority. See MATCH_PROPOSALS.md.

The new match producer admits one owned input up to 64 KiB. Its input, 16-bit predecessor table and output occupy at most 229,376 explicit device bytes. Each lane has at most eight candidates and 1024 comparisons. Index traversal must remain strictly backward. CPU result admission checks every returned lane and exact source bytes, including lanes skipped by the later greedy parser. The kernel is compiled offline and has no default runtime launch route.

Match worker input and replies use separate 64-byte headers, explicit field serialization and an exact source digest. Request/reply maxima are 65,600/32,832 bytes within the existing generic transport ceilings. The child reconstructs its index and retains the existing job, adapter telemetry, priority, bundle and cleanup gates. The parent validates every match against its own input. GPU status is a trusted-helper declaration and does not attest hardware execution. Test-only CPU publication may proceed while the same exact helper remains supervised and alive.

The match controller owns at most one pending source and one later CPU source. A CPU-selected request cannot be replaced by a late worker reply. No subsequent GPU attempt can bypass exact process/supervisor retirement or configuration quarantine. CPU selection clears only private proposals and performs no OS query, cancellation or wait. Explicit cancellation and retirement remain separately owned operations.

HIP dynamic symbols must match the selected SDK's actual versioned API. Exact properties LUID/node checks precede device selection. The worker clears only its inherited LLVM_PATH before any runtime load and refuses on failure; it does not edit the parent or installed environment. Static export inspection and fake API tests load no GPU code. Keep the existing private-bundle, process, CPU/reference and retirement gates. See HIP_API_BINDING.md.

Installed-runtime metadata must use the discovered system directory, strict basename/expected hash, direct ancestor and read-only file pins, a 32 MiB file cap and cached-only no-UI trust. Nonzero VERIFY or CLOSE status refuses. Metadata mode never loads its subject or initializes HIP, and runs under the existing exact worker job. Valid metadata does not authorize a GPU or owner mount. See SYSTEM_RUNTIME_METADATA.md.

The explicit installed-driver route must retain exact runtime hash/trust pins, strict own-kernel inventory, restricted loading and confirmed cleanup/exit. Its diagnostic enforces 128 MiB per process, 256 MiB aggregate, two processes and 20 percent CPU; the worker retains its five-second bound. No driver change, device search or retry is added. Its artifact contains no AMD runtime and cannot authorize a host launch by itself. See GPU_DRIVER_WORKER.md.

Adapter discovery is metadata-only, explicit and capped at64 DXGI entries under the existing diagnostic process budget. Reject incomplete enumeration and invalid/duplicate selected LUIDs. Do not reinterpret DXGI ordinal as HIP ordinal or dedicated capacity as current memory headroom; worker identity and fresh-budget checks remain mandatory. See ADAPTER_IDENTITY.md.

Worker diagnostics may encode a known failure stage in the process exit code.
Only the six original categories and fifteen defined stages are recognized;
unknown codes remain device faults. Staging cannot authorize candidate bytes,
retry, repair or a weaker recovery category. Confirmed exit and a trusted HIP
worker remain prerequisites for category interpretation. Failure responses stay
empty. The explicit adapter-budget diagnostic performs DXGI metadata queries
under the existing process caps; it never loads HIP or reserves GPU memory.

Compression profile probes are advisory and own at most 64 KiB of sample bytes.
They use the existing 16 MiB encoder and 2 MiB verifier allocator ceilings, not
a new process-memory guarantee. No probe starts a worker or performs I/O. Source
identity/range/bytes must match before advice is returned; failed probes stay
terminal and retain CPU input. Callers still need qualified resource/quietness
and the existing complete-candidate verification/publication gates. See
COMPRESSION_PROBES.md. No owner-host execution is authorized by compilation.

## Explicit Windows physical-node metadata

The SDK-only topology collector in GPU_NODE_PROOF.md may create a D3D12 device
for the exact matched adapter under the existing process budgets and external
watchdog. It requests no queues, GPU resources or commands. Device creation can
enter driver code; no universal no-freeze guarantee is possible. Only a healthy
same-LUID AMD device with exactly one physical node may resolve an omitted zero
HIP node mask for requested node zero. It cannot repair a driver, authorize
retries or bypass CPU verification, residency, retirement or RAM-only rules.
The normal filesystem path never invokes this collector.

The 96cb919 owner node query passed; the subsequent tiny decode refused with
CPU reference preserved and confirmed worker exit. No GPU correctness/speed
qualification follows. Later memory-predicate tagging preserves every admission
rule and legacy recovery category. Current memory-accounting research and new
exact-commit checks precede any policy revision or further owner attempt.

The explicit SDK memory-observation mode in GPU_MEMORY_OBSERVATION.md can
report a typed compute refusal as metadata after successful module retirement
and confirmed process exit. A zero diagnostic exit is transport completion, not
compute admission. It never returns candidate data, retries automatically or
weakens any memory/residency rule. Default operations remain CPU-only.

## Live resource-envelope qualification boundary

GPU_RESOURCE_ENVELOPE.md replaces the experimental zero-growth predicates with
one original-baseline, headroom-derived policy. Incomplete allocating-process
physical/commit or segment telemetry fails closed. Valid budgets shrinking
below usage are pressure, not proof of a broken device. The shared-growth bound
is re-evaluated against current headroom; CPU comparison, small batches, terminal
fallback and confirmed retirement remain mandatory. The implementation needs
exact-source hosted CPU/Windows/SDK qualification before any new owner GPU
trial. It changes no owner mounting, driver, resource-cap or RAM-only boundary.
The fixed historical diagnostic wire omits host fields and cannot authorize
admission. This is not an AI governor or protection against all driver faults.

## Accepted tiny GPU diagnostics and provisional feedback

The 04d884b raw/fill and 4-KiB match experiments returned accepted CPU-verified
results with confirmed exit. Postflight found no own probe/worker and unchanged
healthy driver/runtime observations. These are explicitly bounded synthetic
experiments, not owner mounts, native GPU compression speed, a physical
power-loss test or protection against every future driver fault. Their evidence
is docs/evidence/gpu-admission-04d884b.json.

The new recompression-feedback getter is fixed-size advisory state only.
Candidate copies are not durable publications; missing/stale/failed/cancelled
feedback must not authorize learning credit, reclamation or data exposure.
Queries perform no backing I/O or GPU operation. Existing retained-source,
independent CPU verification and handoff barriers remain the authority.

## Native locked-RAM worker composition gate

The optional probe in NATIVE_GPU_RAM_PROBE.md uses only the shared locked-RAM
backend for source, target and head; ordinary outputs are new bounded numeric
reports. The old one-process CPU RAM probe remains unchanged. Shared two-process
limits and SDK factoring passed at 480573e; GPU source inputs remained unchanged
through the native probe at 645a1d1. CPU-helper/probe binaries are separately
hashed and qualified at 645a1d1.

The explicit synthetic owner trial accepted GPU proposals and verified native
readback/source preservation, locked-page retirement, equal handles and actual
child/supervisor exit. Postflight found no owned workers and unchanged healthy
driver/runtime observations. See evidence/native-gpu-ram-645a1d1.json.
No normal filesystem GPU route, mount, driver installation, stress test,
persistent worker or physical-power-loss qualification follows. No unchanged
GPU retry is needed; the trial found no size/speed benefit.

The prior D01 mounted retirement failures remain open despite the current
13-job pass. Owner mounts remain prohibited. Future batching/resident-worker
changes require a replacement security/lifecycle/resource contract and isolated
fault qualification before any owner-device test.

## Candidate crash-source actor isolation

The source/client test controller in MOUNT_PROCESS_ISOLATION.md is CPU-only
infrastructure plus a separate OFF-by-default hosted mount path. Normal CTest
never creates a filesystem. Unknown source or supervisor retirement is terminal
and cannot authorize the next helper. The client file remains open until actual
host/supervisor exit; source byte and metadata proofs are retained.

The additional caller job and wall watchdog bound previously unbounded parent
work; they do not raise the original child caps or image/report budgets.
A test-only source guard uses the exact pinned SDK's permitted Create/Open
origin-PID query, then delegates admitted requests to the unchanged native
authorization/adoption logic. It is not a production security policy or proof
that all driver failures can be contained. Owner mounts/driver changes remain
prohibited and no new owner test is authorized.


The 8122bf5 initial matrix confirmed both external-client crash profiles under
the unchanged source/exit caps; the source-bound evidence is
evidence/mount-source-isolation-8122bf5.json. These results do not establish the
historical failure cause. New CPU post-ack cancellation/abandonment and early-exit
tests preserve exact retirement and handle accounting; they need their own
source-bound qualification before any result claim.


## Advisory optimization area

OPTIMIZATION_STATE.md defines a CPU-only custom-backend 128-KiB reservation.
Its state is rebuildable advice, never namespace or decode authority. Missing,
unsupported, foreign or damaged state returns no policy; it does not change
user-image readback. Creation uses only a new empty backend, updates retain
a disjoint acknowledged copy, and repair writes the missing peer first.
Invalid or both-lost areas are never reset automatically. Per-instance leases,
explicit publication allowances and terminal fencing do not qualify global
provider nonaliasing or lifetime wear. No ordinary-file factory, automatic
helper, scheduler, mount, GPU launch or owner-device test is added.


## Volume-owned advisory updates

The optional session binding shares the existing serialized control owner and
retains the attempted backend after failure. Known active/head/candidate aliases
reject, and both bound and failed areas are excluded from maintenance targets.
Advisory corruption, allocation refusal or write/readback failure supplies no
usable policy and must not fence user-data I/O or change file-context ownership.
An external qualified provider still owns global reservation/mapping, capacity
accounting and authorization. The scalar getter is diagnostic only.

## Native application ownership and volatile memory

The opt-in native application uses a private bounded provider and a serialized
CPU engine owner. At f4928c2 the exact audited binaries passed isolated Windows
and owner-host provider/model/native-control checks. Three owner model cycles
balanced 179 to 179 handles, restored working-set settings and passed cleanup;
the GUI workflow verified UTF-8 bytes, explicit discard and clean process exit.
Postflight found no remaining FlitzFS processes. WinFsp stayed Stopped/Manual.
[Exact limits, hashes, evidence and exclusions](evidence/volume-app-f4928c2.json).

All stores remain locked under a process-wide lease until their final owners
retire. The 64-store registry and explicit residency checks cover active stores;
unconfirmed frees retain accounting and failed cleanup blocks another domain.
The app's 12-MiB aggregate and provider's 40-MiB maximum are experimental limits.
Flush preserves process-lifetime bytes without a whole-image copy; it makes no
physical persistence or process-crash survival promise.

Close retains a temporary volume. Explicit Discard retires it. NEW-only Save
cannot overwrite an existing file, although a failed later write may leave its
new empty name. These development checks do not qualify full Windows semantics,
visual/accessibility behavior, an owner mount, physical media, power loss or
FlitzFS HIP execution. No driver installation, settings change, service or
scheduled task was performed. Preserve the existing separate gates.

## Independent GPU cost preparation

Input-only request preparation removes only unused parent CPU indexing. Device
admission still reserves the complete old footprint and the child independently
builds its index and verifies every match field. CPU recovery is unchanged.
The opt-in phase diagnostic preserves existing process/device/time bounds and
publishes only a complete, bound profile after cleanup; malformed/unavailable
profiles cannot be used as data or hardware-recovery evidence. No new owner
execution is qualified by this source change. See GPU_PHASE_PROFILE.md.

The coalesced match upload owns only one base device pointer; kernel subregion
pointers never acquire independent ownership. Layout and returned-address checks
bound aligned input/index/output regions before upload/launch. The maximum
explicit allocation remains 229,376 bytes, with one equally bounded temporary
CPU upload vector. No CPU verification, telemetry or release-before-publication
check is removed. Source99's single actual GPU profile is evidence only for its
older three-allocation path. The new path must pass exact-source isolated gates
before any owner run; neither observation establishes no-freeze guarantees,
useful compression benefit, power-loss safety or full-product qualification.

The next D01 fixture change moves all mounted client calls in basic, mutations
and delete to the supervised external controller. Its filesystem child holds no
mounted client handle. Normal client handles close before ordered stop, and the
exact source/supervisor lease must retire before CPU verification. Crash profiles
retain their open external handle through source retirement. All old limits and
oracles remain. Restart/resumer, RAM-handoff and read-only actor conversion are
unfinished, and the source-specific guard is not production protection.
Qualification of this publishing source is pending; no owner operation is enabled.

Qualified b6a9b4e evidence, including one bounded actual GPU profile and postflight,
is retained in evidence/gpu-slab-b6a9b4e.json. It records zero remaining FlitzFS
processes and an unchanged installed runtime/driver after that experiment. These
observations do not establish general no-freeze/data-loss guarantees or a GPU
performance improvement.

The next actor conversion removes ordered restart/resumer and read-only mounted
client calls from their filesystem-host processes. Exact source/resumer leases
remain held through CPU verification. Normal external client handles close before
ordered stop; the original open-handle crash coverage remains. The read-only root
controller gains the already established process-tree CPU/memory/headroom budget
and an outer100-second hosted deadline, without extending child lifetimes.
Its publishing-source qualification is pending. Kernel faults still cannot be
converted into no-freeze guarantees by user-mode bounds. RAM-handoff actor
conversion, adapter-wide self-origin protection, Windows 11 client and owner
mount qualification remain separate gates; no owner operation is enabled.

The next RAM-handoff fixture keeps actual mounted file handles in its external
controller. Only a finite, one-command-at-a-time, protected seven-event conversation
coordinates host-side memory copy/publication with those client operations.
Foreign/order/overlap/role/cap/wait failures are terminal; signals are not counted
as queued work. Exact process/supervisor and waited-handle ownership remain required.
No process, byte-report, lock, memory, CPU or source lifetime limit is enlarged.

The original capacity/stale/publication/identity/pinned CPU/metadata/flush/working-set
and zero-lock cleanup oracles remain. New pure and real CPU-helper protocol tests
do not establish mounted correctness. Publishing-source RAM qualification and
adapter-wide self-origin protection remain separate gates. Current research
receipts document auto-reset and asynchronous termination semantics, not general
kernel-fault recovery or no-freeze guarantees. No owner operation is enabled.

The next product origin guard rejects the host's new Create/Open context before
native preparation or slot allocation and rejects a missing/throwing provider.
Live tables supply the real SDK query, while non-mounted CPU fixtures inject
explicit synthetic origins. Original rights/storage/byte checks remain on all
admitted foreign/kernel requests. Source wrappers now audit product rejection
instead of intercepting their own actor first. Read docs/WINFSP_ORIGIN_GUARD.md
for supported callback scope, immutable binding, retained diagnostic and limits.
Publishing-source qualification is pending; duplicated/inherited handles, indirect
backing cycles and arbitrary driver/kernel bugs are not solved by this one rule.
No owner or GPU operation is enabled.

## Bounded failed-advice rebuilding

[ADVISORY_REBUILD.md](ADVISORY_REBUILD.md) specifies a separate control transition
for unavailable/poisoned advice only. Original initial-binding calls stay one-
attempt. Retain every failed area/lease, reject all owned area aliases as image
targets and account at most four 128-KiB reservations. Never reset healthy advice,
reuse partial bytes, infer durable mapping, loop automatically or fence user data
because advice failed. New tests require their own publishing-source gates;
parent 053ade8 passed all 13 jobs. No owner execution or lifetime wear claim follows.

## Helper advisory recovery

[ADVISORY_HELPER.md](ADVISORY_HELPER.md) keeps advice audits/rebuilds in the
serialized maintenance path. No normal save/read consults advice. Retain all
admitted backends, including failed creation, within the existing 12-MiB app
domain/four-area ceiling. Exhaustion defers optional work without fencing data.
Fault tests touch only new private VM memory; no production injection command,
backing override, host operation, timer or persistence is introduced. New-source
qualification is required; physical mapping and persistent credit remain open.

Source 9977674 passed its full VM matrix and a healthy owner GUI workflow;
[evidence](evidence/advisory-helper-9977674.json). The next allocator fixture
refuses one specific allocation in the VM test executable only, after the core
admitted a new area. It creates no global memory pressure and is not an owner
fault experiment. Its publishing-source qualification remains required.

## Persistent write provenance

The optional [workload counter](WORKLOAD_ACCOUNTING.md) grants no optimization
authority. Only actual accepted write input changes it in the same replicated
namespace transaction; rejected batches and maintenance cannot mint workload.
Enabling starts at zero, repeated enable cannot reset it, and saturation cannot
wrap or block ordinary writes. Independently recovered data and count must
belong to the same selected transaction. Preserve explicit capability admission,
source-copy invariance and all existing bounds. Tests model persistence failures
in disposable VM memory, not owner faults or physical power-loss survival.

## Managed/helper workload consumer

[WORKLOAD_HELPER.md](WORKLOAD_HELPER.md) confines activation to explicit healthy
managed control without pending/staged maintenance and NEW helper creation
before exposure. No diagnostic allocation/I/O follows successful publication.
Moved/fenced owners expose no usable counter. Preserve live contexts, legacy
admission and all existing provider/process limits. Qualification of the new
integration is required before owner execution; no automatic credit, mount,
driver action or GPU operation is enabled.

## Input-funded advisory control

[OPTIMIZATION_BUDGET.md](OPTIMIZATION_BUDGET.md) requires verified native input,
monotonic persisted settlement, charged creation/repair and fresh-input forfeiture
after loss. Only actual NEW head/image creation can fund one initial provision;
ordinary resume cannot reset it. Keep failure isolated from data, all retained
owners/aliases and existing publication/binding/resource limits. The new controls
need their own full VM qualification and launch no worker or automatic optimizer.
No global physical-wear, anti-rollback or arbitrary-failure claim follows.


## Funded helper pre-allocation control

The funded journal at `5db3ae7` passed all 13 hosted core/integration jobs,
including 42 injected journal failures per core configuration. The next helper
consumer requires its own qualification. Failed advice establishes a verified
loss baseline before replacement allocation; unchanged insufficient input defers
without another candidate or native snapshot. Positive preflight grants no write
authority, and actual rebuild independently verifies input and admission again.

The helper retains its single-publication allowance, four-area and 12-MiB limits,
no ordinary-save/read advice work, source/context preservation and exception-safe
mapping adoption. Test funding uses actual accepted synthetic writes. The four
allocator cuts select the expected real admitted binding and measured heap request
so a preflight failure cannot be misreported as a journal fault. All owner fault,
driver, mount and GPU gates remain unchanged. This is explicit metadata maintenance,
not an automatic image-work budget or physical durability qualification.


## GPU candidate source-index boundary

The funded helper at `99d5365` passed its full 13-job matrix and a healthy owner
GUI check with confirmed process exit and unchanged driver inventory. The next
candidate navigation change modifies no runtime path. It stages only a fixed
public source revision in a disposable hosted VM, verifies independent control
and source bindings, retains hash-pinned notices and indexes a private text mirror.
Upstream build scripts and code are never executed during indexing. Existing CPU,
memory, process, duration, package/model and private-cache controls remain.
Actual source-specific receipts and live confirmation are prerequisites for later
kernel analysis; compilation, CPU format recovery and GPU execution have separate
gates. No owner installation or repeated unchanged host/GPU test follows.


## Optional codec object compilation

The new [offline gate](HIPCOMP_OFFLINE.md) accepts only the admitted pinned vendor
source and verified complete SDK on disposable hosted Windows VMs. Source size,
digest, path/case aliases, notices and NEW output checks precede compilation;
Git replacements are disabled and canonical source blobs are copied into a private
fixture. Candidate CMake is never executed. The generated object remains unloaded,
unlinked and unexported. Existing 2-GiB command and SDK/resource gates remain, with
explicit wrapper/compiler deadlines and output acceptance caps. No owner driver,
GPU, mount or repeated unchanged GUI action is enabled. Actual tests/compiler
results and separate runtime/CPU recovery/performance gates remain required.


## Complete candidate allocation accounting

The new [LZ4 layout](GPU_LZ4_LAYOUT.md) has fixed stack metadata and admits all
explicit buffers within the existing 256-KiB ceiling before source copying or
runtime use. It includes worst-case output, scratch, four tables, alignment and
32-byte output guards. Guards diagnose corruption; they do not prevent it.
The same verified SDK's LLVM object readers run only on disposable VMs with
30-second/2-GiB command limits, and retain identities and untruncated format/target
evidence. They never load or execute the candidate. These new gates require their
own actual qualification; no owner installation, runtime or GPU action follows.


## Private independent candidate decoding

The new [CPU verifier](GPU_LZ4_CPU_VERIFY.md) checks bounded candidate data against
immutable source bytes using the existing optional LZ4 dependency. It rejects
wrong extents and full/partial scratch aliasing before writes. Failed decoding
may leave private scratch partial; source and compressed input are never output
buffers. No worker, GPU, filesystem write or publication is invoked. Valid span
ownership/immutability remains a caller contract and is not supplied by a byte
comparison. Raw-only builds report unavailable. Exact-source malicious-input,
canary, codec/raw and sanitizer qualification remains required.

The optional offline inspection failure at 0879cce is preserved. The replacement
same-SDK llvm-readobj --offloading implementation was reviewed through its native
source path: it reports bundle metadata without extracting files. Its corrected
command and closed-output regressions require new-source VM execution. No
unchanged owner GPU/GUI test or driver operation follows.


## Exact object metadata and unresolved warp semantics

The read-only metadata command at 86bc830 returned an empty AMD64 host placeholder
and a positive gfx1100 image; the prior schema rejected the placeholder and
accepted no candidate. The next parser binds both URIs/extents to the actual
object, requires exactly one of each, rejects nonempty/foreign/duplicate hosts,
reader warnings and truncated output, and checks unchanged output digests.
New-source execution remains required.

The [kernel review](GPU_LZ4_KERNEL_REVIEW.md) does not classify the AMD no-op warp
sync macros as either safe or a proven race. Three current selected-distribution
AMD pages must first be captured through the existing isolated provider and
reviewed substantively. All process/resource/robots/profile restrictions remain;
no kernel edit, actual device execution or owner test follows from adding URLs.


The strict offline object gate passed at 1d20f40 with all 28 admission cases,
exact file-bound host/device metadata and unchanged output hashes. The native
research group separately failed an absent literal marker despite valid HTTP-200
content. Its next scope check requires the observed closed URI, selected version
and substantive markers while preserving native failures. It must pass before
current-AMD semantic conclusions; no source/kernel/owner operation follows from
an admission correction. Bounded formatting retains the initial observation
truncation and emits digests/scope diagnostics instead of full native HTML.


## Bounded optional-capability evidence

The [priority guard](HIP_PRIORITY_CAPABILITY.md) adds one bounded corroborating
query for the same selected device. It performs no rescan, timer retry, stream
creation, batch allocation or kernel launch. Unreported/partial/inconsistent
capability refuses as unsupported configuration; explicit stable invalid ranges
and actual API errors retain fault/pressure/unsupported classifications. CPU
selection, configuration suppression, process limits and exact retirement remain.

A test-only byte-pinned historical header and the current header run the same
focused fake API case. The SDK gate requires the actual old failure category and
one query, then the corrected unsupported category and two queries; neither
probe may load HIP. Native import auditing, all original SDK/worker checks and
no owner execution remain. The new source is unqualified until these runs pass.
Current source review confirms the selected runtime normally writes both outputs;
no owner-device occurrence of incomplete output is asserted.


## Historical retirement reconstruction

The manual retirement-reference workflow is isolated from product targets and
normal tests. Read [RETIREMENT_REFERENCE.md](RETIREMENT_REFERENCE.md) for exact
old-source pins, one trial per disposable VM, phase receipts and original limits.
No owner driver/mount/GPU operation or stored-format change follows. Unknown
retirement remains terminal; a green reconstruction cannot explain the old kernel
state by itself. Publishing-source execution is still required.

At497066c all 13 ordinary jobs and8 Linux source tests passed. Both historical
comparison variants failed preparation before driver installation or trial.
The correction binds actual directory identities and independently reconciles raw
phase/acknowledgement/result/CPU records. Windows/Linux source tests and actual
controlled trials require new-source qualification; D01 remains unresolved.


At400f74c Linux passed16 source cases and Windows passed7 record cases before
rejecting fixture bytes; no manual driver/trial was run. Canonical Git capture
with separately verified exact/CRLF checkout identity is the next18-case gate.
The source fixture failure is not a historical retirement result or GPU evidence.


At ab37c4a, all18 source cases passed on both OSes, all 13 core jobs passed, and
both historical held/closed-handle trials retired under original limits with
exact7001-byte CPU recovery. Their one-shot outcome remains inconclusive for D01.
[Raw source-bound evidence](evidence/retirement-reference-ab37c4a.json).
The separate [native synchronization API probe](HIP_WARP_SYNC_PROBE.md) now needs
its nine-case CPU and exact SDK compiler qualification. Vendor kernels, runtime
routes and stored encodings remain unchanged; useful GPU gain is unproven.


The native synchronization API probe passed its9 CPU cases, actual selected header,
compiler/dependency/object checks and original SDK/20-worker gates ataf2f714
([evidence](evidence/hip-warp-offline-af2f714.json)). Exact physical-input stability
and complete automatic trigger coverage are the next narrow qualification;
no vendor kernel, runtime or GPU-output behavior changes.


At 2fdd01c, all 13 ordinary jobs and the SDK/worker gates passed, including
physical-input stability of the native synchronization probe
([evidence](evidence/hip-warp-stable-2fdd01c.json)).
The separate [native-wave candidate](HIPCOMP_NATIVE_SYNC.md) is experimental,
OFF by default and awaiting its own hosted qualification: seven composition
cases, a compiled CPU index oracle and strict offline object inspection.
No GPU object is loaded, stored codec added or runtime path enabled. D01 remains open.


The native-wave composition passed at 09c4fca: seven source cases, the actual
compiled index oracle, both offline objects and all SDK/20-worker checks.
All 13 ordinary jobs passed; all 121 sanitizer results were reconciled.
[Evidence](evidence/hipcomp-native-sync-09c4fca.json).
The next [bounded byte entry](HIPCOMP_BOUNDED_ENTRY.md) retains the complete
256-KiB allocation cap and checks actual wave geometry before slab access.
Its new CPU/source/image/compiler gates are pending. No runtime or stored codec
is enabled; actual GPU output, useful gain and D01 remain unqualified.


The bounded entry qualified at 857d5c9: all five validator configurations,
all 13 ordinary jobs, 12 source/image cases, exact native AMDGPU symbols and the
complete SDK/20-worker checks passed
([evidence](evidence/hipcomp-bounded-entry-857d5c9.json)).
The next [private typed-module fixture](HIPCOMP_MODULE_LINK.md) adds CPU argument
forwarding and separately opted-in linking. It remains unqualified until its own
execution; no module loading, binary artifact, worker integration or GPU gain follows.


The private typed-module link qualified at c86559b, including all 13 ordinary
jobs, 122 sanitizer tests, five 144524-check validator configurations and 20
confirmed worker exits ([evidence](evidence/hipcomp-module-link-c86559b.json)).
The [LZ4 candidate frames](LZ4_WORKER_FRAMES.md) qualified at 959606a in all five
core configurations and all 13 ordinary integration jobs
([evidence](evidence/lz4-worker-frames-959606a.json)). Enabled builds reported
621 framing checks; disabled builds reported 593. Windows CPU exchanges retained
79 initial/final handles. The [checked slab layer](GPU_LZ4_SLAB.md) then passed
all five core configurations and all 13 jobs at 38229d1: 968 checks with the
decoder and 939 without it ([evidence](evidence/gpu-lz4-slab-38229d1.json)).
The live owner-host model and GUI passed, with exact retirement and complete
staging cleanup ([evidence](evidence/owner-gui-38229d1.json)).
The next [static SDK-worker CPU decoder](CPU_LZ4_WORKER.md) is explicitly opt-in.
New staging, decoder/protocol and both worker-capability configurations require
publishing-source execution. No actual GPU compressor or normal-I/O route follows.
D01 remains open.

The optional static CPU LZ4 worker qualified at 94ec882: all five core
configurations, all 13 ordinary integration jobs, ten staging cases per
configuration, the same decoder's 806/621/968 verifier/frame/slab checks and both
success/unavailable worker retirements. All original 20 admission cases remained.
[Evidence](evidence/cpu-lz4-worker-94ec882.json).
The separate source-snapshot job failed the obsolete 512-file preflight and is
retained as a failure; SOURCE_SNAPSHOT.md defines the corrected combined
file/directory accounting within the unchanged archive-byte/768-entry limits.
New snapshot execution remains pending. No actual GPU compressor is enabled.

