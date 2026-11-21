# Current FlitzFS architecture

**Source baseline [df5be9b5b9d0](https://github.com/strmt7/FlitzFS/commit/df5be9b5b9d0bc05ac565eb2da8454ac76bed3d2).** These diagrams describe paths in that tree. The [repository review](REVIEW.md)
records the assumptions and evidence for them. The [revised architecture](TARGET.md)
describes proposed changes.

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
backing is an ordinary local NTFS image under a qualified contract.
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
The healthy indexed writable path can reuse its just-audited read state while
taking the image and head fences. A damaged or closed-session state has separate
repair and retirement paths. Resolver, image and selector identity are required
before a writer is returned.

```mermaid
flowchart TD
    Open["Open from CPU"] --> Head["Validate outer selector pair and choose checked epoch"]
    Head --> Image["Resolve exact image and checkpoint / namespace floors"]
    Image --> Audit["Audit selected records, namespace and ownership"]
    Audit -->|Read-only| Reader["Return verified CPU view"]
    Audit -->|Healthy writable reopen| Fence["Acquire image and selector ownership then flush and confirm matching state"]
    Audit -->|Admitted damaged or retired state| Repair["Check surviving protection and old lifetime records"]
    Audit -->|No verified authority| Refusal["Refuse writable exposure"]
    Repair --> Candidate["Repair selected state or prepare verified replacement image"]
    Candidate --> Publish["Publish paired selector when image changes"]
    Publish --> Fence
    Fence --> Ready["Construct authorized writable session"]
```

Normal remount cost still includes full indexed validation. Reader and image
pins constrain reclaimable storage. Losing both trustworthy copies or losing
the backing medium is outside the demonstrated mirror-repair scope.
[Selector reopen and audit](../../src/volume_head.cpp#L158)
· [single-pass indexed audit path](../../src/native_session.cpp#L642)
· [retained reopen evidence](../evidence/indexed-retained-reopen-2026-11-20.json)
· [mounted post-flush restart](../evidence/windows11-mounted-post-flush-recovery-2026-11-21.json)

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
    Decide --> Reclaim["Incremental staging of alternate image or region retirement"]
    Plan --> Search["CPU matching or qualified AMD advice"]
    Search --> Compare["Check tokens, bytes and complete compacted cost"]
    Compare -->|Smaller and current| Publish["Publish protected group by normal transaction"]
    Compare -->|Stale or no net improvement| Yield
    Repair --> Yield
    Reclaim --> Commit["Verify target and publish selector before old range retirement"]
    Commit --> Yield
    Publish --> Yield
```

Existing grouping includes exact sharing, shared-base members and packed slices.
The current append layout reclaims normal churn by staged image replacement or
provider-supported tail/region release. It can reconcile unrelated commits
under bounds, but still depends on alternate capacity and scans for this work.
[Windows scheduling and pacing](../../src/windows/winfsp_controller.cpp#L205)
· [native group advancement](../../src/native_maintenance.cpp#L1132)
· [persistent postings](../../src/indexed_coherence.cpp#L258)
· [file reclaimer](../../src/native_volume_file.cpp#L227)
· [staged rebuild](../STAGED_PAGED_REBUILD.md)

## 9. AMD work and immediate CPU continuity

The installed background anchor acquisition path can start from an eligible
native maintenance candidate. The separately qualified foreground controller and
finite uniform sweep demonstrate additional real GPU work. Both follow an
independent CPU validation boundary.

```mermaid
flowchart LR
    B["Background anchor opportunity"] --> Discover["Match package, runtime, adapter LUID and node"]
    F["Qualified foreground offer / native integration fixture"] --> Discover
    Discover --> Admit["Fresh host and device budgets, active identity and useful-work evidence"]
    Admit -->|Admitted| Worker["Bounded process / session channel / finite kernel work"]
    Admit -->|Refused| CPU["Continue on independent CPU input"]
    Worker --> Validate["Check job, byte output and live completion evidence"]
    Validate -->|Candidate fits| Publish["Native owner revalidates before publication"]
    Validate -->|Any refusal| CPU
    Worker --> End["Confirm process and channel retirement"]
    End --> Health["Evidence-driven future eligibility or quarantine"]
```

The most recent uniform worker change allows one qualifying replacement sample
after compute if its first sample has aged out. The same allocation baseline and
headroom checks apply. An invalid or still-stale result is discarded without an
additional launch. Current ROCm product, HIP API and loaded runtime ABI identities
are separately recorded. [Runtime discovery](../../src/windows/hip_route_discovery.cpp#L129)
· [GPU health policy](../../src/gpu_recovery.cpp#L61)
· [freshness checks](../../src/windows/hip_worker_budget.hpp)
· [selected-SDK and physical AMD results](../evidence/amd-fresh-completion-observation-2026-11-21.json)

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

The source tree preserves evidence for CPU, SDK, live AMD and mounted guest runs.
The reviewed round includes 203 Windows tests, focused raw and sanitizer checks,
selected-SDK fault cases, and three matched real AMD batches. Those checks cover
the scopes recorded in their receipts. The [release gate ledger](../RELEASE_EXECUTION_PLAN.md#current-gate-ledger)
specifies the remaining products of Windows, storage and benchmark qualification.
[Validation guide](../validation/README.md)
· [sanitizer preflight](../../cmake/WindowsAsanRuntime.cmake)
· [latest AMD campaign](../evidence/amd-fresh-completion-observation-2026-11-21.json)
