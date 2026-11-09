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
