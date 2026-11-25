# Current FlitzFS architecture

The diagrams trace the implementation reconciled on 25 November. They
cover prepared AMD read delivery, mirrored cold copy across packed banks,
phase visibility during reclaimer steps and bounded rolling lookup.
[Release reconciliation](../RELEASE_RECONCILIATION_2026-11-25.md) ·
[Packed admission and sealing evidence](../evidence/windows-packed-admission-and-sealing-2026-11-25.json) ·
[Revised architecture](TARGET.md)

## 1. Process and build boundaries

Windows applications reach a mounted volume through the WinFsp driver and its
user-mode dispatcher. The experimental helper currently hosts the writable
adapter and its controller in the same process as the window and tray. Its
unmounted preview and the legacy single-stream CPU CLI are separate entry points.

```mermaid
flowchart TD
    App["Explorer / Win32 applications"] --> FSD["WinFsp filesystem driver"]
    subgraph Helper["Experimental FlitzFS volume app process"]
        SDK["WinFsp DLL and dispatcher"]
        UI["Window and tray"] --> Control["VolumeController command owner"]
        Control --> Preview["Unmounted memory preview"]
        Control --> Attachment["WinFspVolumeAttachment"]
        Attachment --> Owner["WinFspVolumeController and session"]
        SDK <--> Adapter["WinFspNativeWritable"]
        Owner --> Adapter
        Adapter --> IO["NativeFileSession and indexed engine"]
        Preview --> IO
        Owner --> Maintenance["Maintenance host and route acquisition"]
        Maintenance --> IO
    end
    FSD <--> SDK
    IO --> Provider["Memory or qualified ordinary volume-file provider"]
    Maintenance -. "qualified request" .-> AMD["Separately supervised optional AMD worker"]
    CLI["CPU CLI"] --> Stream["Legacy single-stream / catalog tools"]
    Inspect["Independent native CPU inspection API"]
    Inspect --> Provider
```

The portable core and CLI can build independently of the Windows app, the WinFsp
SDK and the ROCm worker. Explorer attachment, persistent file controls and
installed AMD acquisition have separate build inputs. The read-only catalog
callback adapter also remains a separate path. [Build options](../../CMakeLists.txt#L34)
· [app targets](../../CMakeLists.txt#L1289) · [Windows backings](../WINDOWS_VOLUME_BACKENDS.md)

## 2. Attach, stop and keep ownership

The file path uses an exclusive operating-system handle and an indexed audit
before a writable reopen. The memory path keeps its backing while detached so
the same volume can be attached again. An unconfirmed stop retains ownership and
blocks another attach.

```mermaid
stateDiagram-v2
    [*] --> Empty
    Empty --> Starting: Attach with qualified runtime and fresh mount location
    Starting --> Attached: Preflight, create, bind, start dispatcher, set mount point
    Starting --> Detached: Failure with retained backing and confirmed retirement
    Starting --> Blocked: Retirement or mount removal uncertain
    Attached --> Stopping: Detach or fault
    Stopping --> Detached: Deferred work drained, dispatcher stopped, mount absent
    Stopping --> Blocked: Retirement or mount removal uncertain
    Detached --> Starting: Reopen retained backing and start a new session
    Detached --> Empty: Explicit close releases backing
```

Fresh adapter creation can fail before any backing is retained. A user-mode
runtime DLL is checked beside the application, and the mount lifecycle holds
the adapter until after dispatcher stop and deletion. The persistent file
backing in the Explorer helper is a local NTFS file under the qualified
contract. New files use packed placement when capacity covers both virtual
image ranges, the reserved catalog and the packed directories. The exclusive
open selects the effective contract for existing packed or sparse files.
[Runtime and backing selection](../../src/windows/volume_app_winfsp.cpp#L67)
· [attachment and retirement](../../src/windows/winfsp_volume_attachment.cpp#L55)
· [dispatcher lifecycle](../../src/windows/winfsp_session.cpp#L101)
· [file-selection evidence](../evidence/helper-file-selection-2026-11-21.json)

## 3. Windows foreground write, with or without a GPU candidate

All native callbacks enter through the adapter's foreground accounting and
control lock. The current deferred service accepts at most one pending native
write. A conflicting entry can force CPU completion while the optional
worker is retiring. The independent storage commit determines status, transferred
bytes and file information in either route.
The SDK drives the deferred-write activation used in this diagram. The
ordinary helper has automatic installed acquisition entry points for
eligible read-ahead and collective work.
[Route activation](../../src/windows/winfsp_controller.cpp) ·
[SDK activation tests](../../tests/test_native_deferred_write_sdk.cpp)

```mermaid
sequenceDiagram
    participant W as WinFsp dispatcher
    participant A as Writable callback adapter
    participant D as Optional deferred-write owner
    participant G as Bounded AMD worker
    participant S as NativeFileSession and indexed writer
    participant P as PagedObjectStoreWriter
    W->>A: Write with context, range and flags
    A->>A: Account foreground, enter control, drain pending if needed
    alt Eligible, offered route and admitted request
        A->>D: Validate handle, copy immutable source, save request hint
        A-->>W: STATUS_PENDING
        D->>G: Optional candidate computation
        alt Admitted fresh result
            G-->>D: Framed LZ4 candidates
            D->>D: Independent CPU decode and byte check
        else Declined, stale, failed, canceled or foreground drains
            D->>D: Select CPU and request worker cancellation
        end
        D->>S: Dispatch native write with usable candidates or CPU input
        S->>P: Publish affected records and roots
        P-->>S: Committed result or failure
        D-->>W: Attempt one matched native response
        D->>G: Confirm retirement before reuse
    else Ordinary synchronous dispatch
        A->>S: Resolve context and write with CPU path
        S->>P: Publish affected records and roots
        P-->>S: Committed result or failure
        A-->>W: Status, byte count and file information
    end
```

An explicit Flush goes through the same callback boundary and invokes the native
file/volume flush. Its effect on Windows-cached and mapped writes is a client
qualification question. The controller implements a foreground route offer.
At this baseline its callers are SDK tests. The separate live indexed write
fixture exercises real AMD through the native write bridge.
[Callback entry and drain](../../src/windows/winfsp_native.cpp#L418)
· [read, write and flush callbacks](../../src/windows/winfsp_native.cpp#L581)
· [deferred completion and response](../../src/windows/native_deferred_write.cpp#L157)
· [foreground route offer](../../src/windows/winfsp_controller.cpp#L443)
· [hardware write evidence](../evidence/amd-fresh-completion-observation-2026-11-21.json)

### Overlapping client flushes

The external Win32 client ran a cached write and a mapped write on separate
files. It flushed the mapped view and then called `FlushFileBuffers` on that
file. Both file-handle flush calls overlapped in the client. The mount owner's
serialized publication protocol still governs which generation becomes visible.

```mermaid
flowchart TD
    Cached["Cached client edit"] --> CachedFlush["Flush cached file handle"]
    Mapped["Mapped client edit"] --> View["Flush mapped view"]
    View --> MappedFlush["Flush mapped file handle"]
    CachedFlush --> Owner["WinFsp and serialized native publication"]
    MappedFlush --> Owner
    Owner --> Ack["Client records both successful file flushes"]
    Ack --> Cut["Terminate the owned helper"]
    Cut --> Verify["Independent CPU reader checks both edits and untouched bytes"]
    Verify --> Resume["Fresh writable mount and allocation settle"]
```

The packed and sparse guest traces each exercised three cycles. The process
cut after both acknowledgements occurred in cycle two. The durable-media gate
uses a separate failure campaign. [Mounted flush receipt](../evidence/windows-mounted-mixed-flush-recovery-2026-11-23.json)

## 4. Durable authority and advisory storage

The volume selector chooses one image identity and carries a minimum checkpoint
and namespace sequence. The paged image root selects its own object tree. Inode,
name, extent and lifetime records share that tree through typed key domains.
The image must hold the bytes required for decoding. Optimization state and
coherence postings can be rebuilt from the selected image.

```mermaid
flowchart LR
    V["Paired outer volume selector (volume, image, epoch, placement and floor)"] --> Image["Selected indexed paged image"]
    V -. "staged publication can select" .-> Alternate["Alternate image when provider supports it"]
    Image --> R["Paired image checkpoint slots"]
    R --> Tree["Checked paged object tree and mirrored metadata references"]
    Tree --> Names["Directory-name and ordinal records"]
    Tree --> Inodes["Inode, attributes and lifetime records"]
    Tree --> Extents["Sparse extent mappings"]
    Extents --> Bank["Mirrored authoritative payload records"]
    Extents --> Base["Protected immutable bases / packed objects"]
    Advisory["Reserved optimization state (governance and progress)"] -. "may pin catalog identity" .-> Coh["Independent coherence catalog"]
    Coh -. "nominates candidates" .-> Extents
```

The memory provider reserves address space and commits pages on demand under a
shared resource domain. A qualified file provider can hold paired image and
catalog regions with independent lifetime pins. A linear file placement is also
readable under its narrower contract. [Memory allocation](../../src/windows/memory_volume.cpp#L79)
· [file regions](../../src/native_volume_file.cpp#L376)
· [selector binding](../../src/volume_head.cpp#L106)
· [typed records](../../src/native_indexed_volume.cpp#L22)
· [coherence backing](../../include/flitzfs/indexed_coherence.hpp#L92)

## 5. Indexed transaction and failure boundaries

Path updates layer an indexed transaction over the paged writer. The writer
stages append records and changed tree paths, validates newly written payload
replicas and publishes a root pair. The store updates its logical state only
after publication. This sequence illustrates a modifying, mirrored commit.

```mermaid
sequenceDiagram
    participant N as IndexedVolumeTransaction
    participant P as PagedObjectStoreWriter
    participant I as Qualified StoreByteIo
    N->>P: Checked mutations and optional name edits
    P->>P: Seal allocation and changed tree paths
    P->>I: Admit range and reserve write allowance if applicable
    P->>I: Append payload copies, padding and metadata pages
    P->>I: Read back new mirrored payloads
    P->>I: Flush newly reachable objects
    P->>I: Write first new checkpoint slot and flush
    P->>I: Write peer checkpoint slot and flush
    alt All publication calls succeeded
        P->>P: Advance in-memory generation and append floor
        P-->>N: Return committed transaction
    else Write, readback or publication failed after mutation
        P->>P: Poison writer
        P-->>N: Return failure and require verified recovery
    end
```

The outer selector changes during image handoff, with its own publication steps.
The same logical application operation can have more than one observable step
when Windows supplies a sequence of callbacks. Generation choice and recovered
bytes matter independently of whether a client already received success.
[Indexed commit](../../src/native_indexed_volume.cpp#L690)
· [physical commit](../../src/paged_store.cpp#L1947)
· [checkpoint slots and flush sequence](../../src/paged_store.cpp#L507)
· [outer selector publication](../../src/volume_head.cpp#L233)

## 6. CPU range reads and recoverable damage

The writable adapter resolves an open context to a native file handle. The
indexed view looks up the requested extents and reconstructs only the requested
ranges. A group member can use a protected base or a packed object. The CPU path
validates identities, lengths and digests before returning bytes.

```mermaid
flowchart TD
    Read["WinFsp Read / independent CPU reader"] --> Token["Validate context or open checked snapshot"]
    Token --> Inode["Read selected inode and touched extent records"]
    Inode --> Kind{"Extent representation"}
    Kind -->|Hole| Z["Return logical zero bytes"]
    Kind -->|Standalone| Decode["Checked CPU raw / LZ4 / Zstd decode"]
    Kind -->|Group member| Member["Check base generation and bound member range"]
    Member --> DecodeGroup["CPU patch / copy / XOR / delta / prefix / packed slice"]
    Decode --> Check["Validate expected logical bytes and output range"]
    DecodeGroup --> Check
    Z --> Output["Return caller's available byte range"]
    Check --> Output
    Decode -. "unreadable primary" .-> Peer["Try independently valid mirror"]
    Member -. "unreadable primary" .-> Peer
    Peer -->|Valid| Check
    Peer -->|Both copies invalid| Err["Report error / preserve evidence"]
```

Query and decoded dependency caches, plus range limits, reduce ordinary read
work. Full audits and scrub cover different ranges. A healthy peer is an input to recovery or
repair, with replacement subject to ownership and publication checks.
[Callback range read](../../src/windows/winfsp_native.cpp#L581)
· [indexed extent decoding](../../src/native_indexed_volume.cpp#L213)
· [group methods](../../src/native_group_codec.hpp#L14)
· [paged peer reads](../../src/paged_store.cpp#L380)
· [mirrored payload behavior](../MIRRORED_PAYLOADS.md)

## 7. Reopen, repair and image switch

Read-only inspection and writable resume first establish the selected image.
The healthy indexed path takes both leases and fences. The exact checkpoint
binds the earlier decoded audit to the stored replicas verified after the
fences. Damaged protection and unresolved lifetimes take the checked recovery
path. Resolver, image and selector identity establish authority for writes.

```mermaid
flowchart TD
    Open["Open from CPU"] --> Head["Validate outer selector pair and choose checked epoch"]
    Head --> Image["Resolve exact image and checkpoint / namespace floors"]
    Image --> Audit["Audit selected records, namespace and ownership"]
    Audit -->|Read-only| Reader["Return verified CPU view"]
    Audit -->|Healthy writable reopen| Fence["Acquire both leases and flush image then selector"]
    Fence --> Exact["Reopen and compare exact checkpoint and selector"]
    Exact --> Mirrors["Read and verify both stored replicas and the object tree"]
    Mirrors --> Ready["Construct authorized writable session"]
    Mirrors -->|Unverified protection| Refusal["Refuse writable exposure"]
    Exact -->|Changed checkpoint| Refusal
    Audit -->|Admitted damaged or retired state| Repair["Check surviving protection and old lifetime records"]
    Audit -->|No verified authority| Refusal
    Repair --> Candidate["Repair selected state or prepare verified replacement image"]
    Candidate --> Publish["Fence and verify recovery authority and publish selector when needed"]
    Publish --> Ready
```

Normal indexed remount requires a full native audit and a stored mirror pass.
Both audits can read adjacent whole frames from one checked leaf and bank.
Replica spans must match byte for byte, and each frame passes its descriptor
and stored digest checks. The decoded pass then verifies each object on CPU.
The selected image and selector remain bound through the fences. Reader and
image pins constrain reclaimable storage. Repair requires a verified survivor.
[Decoded audit and stored replica reads](../evidence/indexed-decoded-reopen-runs-2026-11-25.json)
· [Single-frame reads](../evidence/indexed-record-read-coalescing-2026-11-23.json)
[Selector reopen and audit](../../src/volume_head.cpp#L158)
· [single-pass indexed audit path](../../src/native_session.cpp#L642)
· [stored mirror recheck evidence](../evidence/indexed-fenced-stored-recheck-2026-11-22.json)
· [mounted post-flush restart](../evidence/windows11-mounted-post-flush-recovery-2026-11-21.json)

### Background payload verification

The active Windows controller performs small replica checks during admitted
maintenance. It closes an observation only after verifying one unchanged
checkpoint from the beginning. Restored advisory positions can resume work.
Damage, repair or a change in selected state requires fresh complete coverage.

```mermaid
flowchart TD
    Work["Controller admits low-priority scrub work"] --> Start["Capture exact active checkpoint and cursor"]
    Start --> Batch["Read a funded record batch and verify both replicas"]
    Batch -->|Valid and more remain| Yield["Yield or continue while foreground and budgets permit"]
    Yield --> Batch
    Batch -->|Valid complete unchanged checkpoint| Observation["Record current in-process verification observation"]
    Batch -->|Bad peer| Repair["Queue repair under ordinary COW ownership"]
    Batch -->|State changed or checks failed| Reset["Restart verification or report damage"]
    Resume["Restore advisory cursor"] --> Batch
    Resume -. "needs a fresh complete pass" .-> Start
    Observation -. "telemetry only" .-> Full["Writable admission independently verifies its authority"]
```

[Current scrub evidence](../evidence/indexed-scrub-checkpoint-2026-11-23.json) ·
[Scrub contract](../MIRRORED_PAYLOADS.md) · [Owned controller](../WINFSP_OWNED_CONTROLLER.md)

### Packed backing relocation

The ordinary packed reclaimer may relocate live units into an earlier
allocation group. The anchor intent records the checked transition of both
directories. Readers continue to use the old mapping until both directory
pairs and the cleared anchor pair have settled. Subsequent physical writes
use the backing provider's latest admitted extent. Optional sealing reserves
and prices the whole aligned workspace that contains the fitted image.
[Packed admission and cold-sealing evidence](../evidence/windows-packed-admission-and-sealing-2026-11-25.json) ·
[Corrected signed replay and scheduling evidence](../evidence/windows-maintenance-retry-delay-2026-11-25.json)

```mermaid
sequenceDiagram
    participant Owner as Dense backing owner
    participant Data as Source and destination units
    participant Head as Mirrored outer anchors
    participant Maps as Both allocation directories
    participant Read as Mapping readers
    Owner->>Data: Copy to earlier hole, flush and verify bytes
    Owner->>Head: Commit intent with source, destination and directory digests
    Owner->>Maps: Commit destination pair, then source pair
    Owner->>Head: Commit cleared intent pair
    Owner->>Read: Drain old physical reads and switch logical mapping
    Owner->>Data: Release empty suffix on a later admitted step
```

Reopen with a pending intent validates both directories and the destination
bytes. A verified original can supply read-only recovery and repair of a
damaged destination. The same evidence binds version-1 access and refusal
when neither copy verifies.
[Format and recovery](../PERSISTENT_VOLUME_FILE.md#moving-live-data-across-allocation-groups) ·
[Cross-group test evidence](../evidence/dense-cross-group-2026-11-23.json)

## 8. Background learning, grouping and reclamation

The Windows controller uses live resource samples and foreground activity to fund
an opportunity, plus elapsed-work pacing and host priority. The native scheduler
can advance a continuation or choose among independent kinds of work. Persistent
catalog entries include banded and projected keys and bounded alternatives.
Generation checks, CPU decoding and physical-size admission determine which
nominations change protected data.

```mermaid
flowchart TD
    Wake["Foreground changes / wait or due maintenance"] --> Budget["Refresh host observations for idle, memory, CPU and write allowance"]
    Budget -->|Declined| Yield["Yield / keep a bounded continuation"]
    Budget -->|Admitted| Decide["Controller and native maintenance choose a quantum"]
    Decide --> Scan["Advance content cursor and sample extents"]
    Scan --> Postings["Read or update sparse coherence postings"]
    Postings --> Plan["Nominate bases and candidate extents"]
    Decide --> Repair["Scrub, repair and directory backfill"]
    Decide --> Reclaim["Stage alternate image or retire old logical range"]
    Decide --> Tail["Select eligible packed backing release"]
    Decide --> Screen["Cold codec screening if the optional work gate permits"]
    Plan --> Search["CPU matching or qualified AMD advice"]
    Search --> Compare["Check tokens, bytes and complete compacted cost"]
    Compare -->|Smaller and current| Publish["Publish protected group by normal transaction"]
    Compare -->|Stale or no net improvement| Yield
    Repair --> Yield
    Reclaim --> Commit["Verify target and publish selector before old range retirement"]
    Tail --> Allocator["Recheck selector and ownership then release a bounded suffix"]
    Allocator --> Yield
    Screen --> Yield
    Commit --> Yield
    Publish --> Yield
```

Existing grouping includes exact sharing, shared-base members and packed slices.
The sparse catalog can nominate a prior extent after it leaves the
resident window. An extent first checks the preceding key families,
then its preferred and remaining distinct rolling keys against the same
catalog snapshot. The existing live extent, exact-byte and complete-size
checks govern candidate publication.
[Secondary rolling lookup](../evidence/release-reconciliation-rolling-lookups-2026-11-25.json)
The current append layout reclaims normal churn by staged image replacement or
provider-supported tail/region release. It can reconcile unrelated commits
under bounds. Rebuild still requires alternate capacity and full passes.
Adjacent mirrored records share preserved copy and independent
verification work within one checked leaf. The mapper splits each
replica's stream at physical bank boundaries. Both target audits retain
record-level CPU comparisons before the new image can be selected.
[Staged copy evidence](../evidence/windows-bank-spanning-rebuild-2026-11-25.json) ·
[Independent verification evidence](../evidence/windows-bank-spanning-verification-2026-11-25.json)
[Windows scheduling and pacing](../../src/windows/winfsp_controller.cpp#L205)
· [native group advancement](../../src/native_maintenance.cpp#L1132)
· [persistent postings](../../src/indexed_coherence.cpp#L258)
· [shifted discovery](../evidence/indexed-shifted-coherence-2026-11-25.json)
· [file reclaimer](../../src/native_volume_file.cpp#L227)
· [staged rebuild](../STAGED_PAGED_REBUILD.md)

### Mounted reclaim admission and the native control

```mermaid
flowchart TD
    App["Helper-owned WinFsp dispatcher"] --> Event["Callback activity and retained-reader events"]
    Event --> Scheduler["Controller refreshes host resources and checks pacing"]
    Scheduler --> Slot{"Indexed maintenance slot admitted?"}
    Slot -->|Deferred| Event
    Slot -->|Admitted| Native["Native maintenance evaluates repairs, groups and reclamation"]
    Native --> Phase{"What can safely advance?"}
    Phase -->|Logical reclaim| Advance["Stage, select, or retire a range"]
    Advance -.-> Observe["Observe candidate phase at step entry"]
    Advance --> Copy["Private batches of preserved and short cold frames"]
    Copy --> VerifyCandidate["Check both replicas and compare CPU bytes"]
    Phase -->|Packed move or recovery| Move["Use fresh observation and verified allocation intent"]
    Phase -->|Settled packed suffix| Tail["Recheck selectors and shorten a bounded suffix"]
    Phase -->|Cold work permitted| Screen["Screen and fund cold image work"]
    Phase -->|Deferred| Idle["Another eligible task or wait for evidence"]
    Advance --> Ownership["Recheck both durable selectors and reader pins"]
    Ownership --> Chunk["Retire one checked range and barrier"]
    Chunk --> More{"Same idle window, current memory and more unpinned work?"}
    More -->|Yes| Ownership
    More -->|Wait or finished| Event
    Detach["Detached fixture with independent native writer"] --> Direct["Explicit native admission and step"]
    Direct --> Advance
    Move --> Event
    Tail --> Follow{"Same idle window and more settled suffix?"}
    Follow -->|Admitted| Tail
    Follow -->|Foreground, deferral or finished| Event
    Tail --> Verify["Independent CPU reopen and physical accounting"]
```

The earlier mounted trace completed inactive-range retirement, then withheld
eligible physical tail work behind the optional cold-work gate. File-backed
maintenance now admits eligible backing release through the ordinary idle
policy. The signed WinFsp guest crossed the packed group boundary and verified
both version-2 anchors, the held reader, independent CPU reopen and a continued
write. On the populated trace, the observer begins with a separately selected
compact image. Sparse backing completed old-range retirement while attached.
Packed backing progressed without reducing provider allocation in the
observation. All four observations verified the selected image and retired the
guest. The packed controller now has a separate continuation for confirmed
EOF shortening. Every step revalidates both selectors and ownership. The same
idle window can fund another shorten after the previous extent is checked.
Foreground activity, expiry or any transition away from confirmed shortening
ends that continuation. The observer counts its admitted steps separately.
It now also records each persistent-reclaimer outcome. The controller's
idle observation indicates that no work is eligible at that decision.
Each admitted reclaimer step can also report the phase of an existing
paged candidate at entry, its candidate identity, and its object cursor.
Backing calls, completed bytes and flushes accumulate against that phase
in a fixed-size array. During a long debt wait with pending work, the
controller can refresh the CPU reference once under background priority.
The refresh is charged to the existing deadline. Reclamation still needs
a valid window and current headroom at the paid step. Optional read-ahead
follows the reclaimer measurement scope and is included in the overall totals.
[Windows scheduling and live reference check](../WINDOWS_BACKGROUND_MAINTENANCE.md#trusted-opportunities-and-resource-priority)
Cold-eligible short frames share the mirrored
payload batch within a separate cold-input budget. The writer checks both
replicas and independent CPU bytes before index construction continues.
The fixture checks remaining work independently after detach.
[Matched cold rewrite](../evidence/windows-indexed-cold-batched-copy-2026-11-25.json)
[Ordinary mount observation](../evidence/windows-ordinary-reclamation-progress-2026-11-25.json)
[Populated retirement](../evidence/windows-mounted-range-retirement-2026-11-24.json) ·
[Packed continuation](../evidence/windows-packed-tail-continuation-2026-11-25.json) ·
[Next implementation slice](../RELEASE_RECONCILIATION_2026-11-25.md#ordered-implementation-and-decision-points)

## 9. AMD work and immediate CPU continuity

Package-enabled controllers can acquire installed AMD anchor and read-ahead
routes from eligible opportunities. The ordinary owner records bounded service-cost windows on eligible CPU
writes when a uniform package is configured. That observation records the
admitted geometry and policy for later calibration. The foreground write
interface accepts an independently qualified offer in SDK fixtures, and a
uniform acquisition can be cancelled through its supervised exchange.
[CPU write measurement](../OWNED_DEFERRED_WRITES.md#cpu-service-cost-from-ordinary-writes)

```mermaid
flowchart LR
    B["Ordinary installed read-ahead or anchor opportunity"] --> Discover["Match package, runtime, adapter LUID and node"]
    F["SDK-controlled measured foreground offer"] --> Discover
    Discover --> Admit["Check fresh budgets and supported work"]
    Admit -->|Admitted| Worker["Bounded process with owned immutable input"]
    Admit -->|Refused| CPU["Select independent CPU service"]
    Worker -->|Deadline or cancellation| CPU
    Worker --> End["Confirm process and transport retirement"]
    End -->|Successful exchange| Validate["Check job, bytes and live completion evidence on CPU"]
    Validate -->|Accepted| Publish["Native owner revalidates before delivery or publication"]
    Validate -->|Rejected| CPU
    End -->|Unconfirmed retirement| Quarantine["Fence later accelerator work"]
    Quarantine --> CPU
```

The most recent uniform worker change allows one qualifying replacement sample
after compute if its first sample has aged out. The same allocation baseline and
headroom checks apply. An invalid or still-stale result is discarded without an
additional launch. Current ROCm product, HIP API and loaded runtime ABI identities
are separately recorded. [Runtime discovery](../../src/windows/hip_route_discovery.cpp#L129)
· [GPU health policy](../../src/gpu_recovery.cpp#L61)
· [freshness checks](../../src/windows/hip_worker_budget.hpp)
· [selected-SDK and physical AMD results](../evidence/amd-fresh-completion-observation-2026-11-21.json)

The sweep worker can identify slab, transfer, launch, wait, verification and
retirement failures separately. Its explicit stream-copy comparison keeps
the same independent CPU verification and retirement boundary. The
ordinary helper still needs a measured foreground route for the admitted
write shape. [Stream copy and stage evidence](../evidence/windows-amd-stream-copy-2026-11-23.json)

### Prepared grouped read-ahead

An approved nonzero submission-lead profile lets the controller reserve a
single packed or mixed group before pulling the protected payload. It keeps
the forecast's exact snapshot and range identity. The checked capture can
queue a shorter immutable request inside the reserved maximum while the child
prepares HIP. A foreground operation interrupts capture or submission.
An already submitted request can outlast unrelated read-only callbacks
while its predicted stream and captured checkpoint are current. The native
read path remains independently decodable on CPU.

```mermaid
sequenceDiagram
    participant C as WinFsp controller
    participant S as Native protected snapshot
    participant W as Supervised HIP worker
    participant R as Foreground reader
    C->>S: Ask for forecast and protected identity
    opt Funded prepared group with valid submission lead
        C->>W: Reserve one bounded request and begin setup
        par Prepare device context and arena
            W->>W: Bind device and check live budget
        and Capture immutable group bytes
            C->>S: Recheck snapshot and capture range
            S-->>C: Verified fragments and references
            C->>W: Queue exact bounded payload
        end
    end
    opt Unrelated read while submitted work is active
        R->>S: Complete the unrelated read on CPU
    end
    alt Target demand arrives first or its checkpoint changes
        R->>S: Read requested range with independent CPU decoder
        C->>W: Cancel owned preparation if active
    else Exact worker reply and confirmed retirement
        W-->>C: Exact reply and terminal exit
        C->>S: Independently compare bytes and recheck capture identity
        C-->>R: Make accepted range available to read-ahead
    end
```

Windows transport and CPU-backed callback tests verify capture, refusal and
cancellation. Later admitted physical-host SDK trials consumed the exact
device-produced packed and mixed frames and verified each through the CPU
reader. The route uses separate models for a full startup and for dispatch
after confirmed readiness. Ordinary mounted read acceleration still needs a
matched CPU comparison with delivered bytes under real demand.
[Prepared worker evidence](../evidence/windows-prepared-group-worker-2026-11-24.json) ·
[Interleaved callback evidence](../evidence/windows-group-read-stream-binding-2026-11-25.json) ·
[Live grouped evidence](../evidence/windows-amd-group-supervisor-comparison-2026-11-24.json) ·
[Native LZ4 device evidence](../NATIVE_AMD_LZ4_READ.md)

## 10. Verification and remaining integration

```mermaid
flowchart TD
    Source["Pinned source and private dependencies"] --> CPU["Windows unit / SDK builds and raw-only CPU control"]
    Source --> ASan["Selected-toolset sanitizer runtime staging and preflight"]
    Source --> AMD["Selected AMD package / mock faults / real host batches"]
    Source --> Guest["Signed WinFsp external client / crash-cut guest qualification"]
    CPU --> Evidence["Source, artifact, backing and scope-linked evidence"]
    ASan --> Evidence
    AMD --> Evidence
    Guest --> Evidence
    Evidence --> Open["Open gates for client scale, GPU, persistent recovery and comparison"]
```

The scoped regression and signed mounted guest both verified the new retirement
scheduling. The validation receipt records the Windows, raw and sanitizer
suites and binds their terminal status to the changed source. Earlier installed
AMD read and fallback runs have separate receipts. The
[release gate ledger](../RELEASE_EXECUTION_PLAN.md#current-gate-ledger)
specifies the remaining Windows, storage and benchmark qualification.
[Validation guide](../validation/README.md)
· [sanitizer preflight](../../cmake/WindowsAsanRuntime.cmake)
· [installed AMD read evidence](../evidence/amd-reopened-paired-sealed-read-2026-11-23.json)
