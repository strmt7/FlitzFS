# FlitzFS safety gates and threat model

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
