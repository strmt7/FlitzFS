# Source and architecture review

**Reviewed tree [df5be9b5b9d0](https://github.com/strmt7/FlitzFS/commit/df5be9b5b9d0bc05ac565eb2da8454ac76bed3d2).** Current behavior is shown in the [architecture diagrams](CURRENT.md).
The [revised architecture](TARGET.md) and [implementation plan](../IMPLEMENTATION_PLAN.md)
describe the work prompted by this review.

## Scope and evidence

The inventory read and hashed every tracked blob in the selected tree. It
visited 102,268 first-party source, test, build and tooling lines across 414
files and retained 7,290 lines matching review criteria for ownership,
resources, platforms, recovery, compatibility and explicit assumptions. It
also parsed 323 first-party JSON files, 55 Python files and four PowerShell
files. A stricter duplicate-key pass across all 325 tracked JSON files
found two different values under the development state's release-plan key.
The two pointers now have distinct names. The work-package index now includes
all 156 numbered steps, including two collective audit steps already in the
written plan. The other tracked paths include
documentation, historical receipts, product assets and vendored sources.

Manual review followed the normal entry points and ownership boundaries
through the app, WinFsp, native session, indexed storage, backing providers,
compressed extents, maintenance, GPU workers and qualification tooling.
The inventory counts describe automated coverage. Manual verification traced
the critical paths and checked the assumptions listed below. The evidence
record binds both to the inspected tree.
[Source and research receipt](../evidence/architecture-reconciliation-2026-11-21.json)

## Findings by subsystem

| Area | Implementation and assumption checked | Effect and next proof |
| --- | --- | --- |
| Windows and Explorer | The helper owns the volume controller in-process. The signed WinFsp path has mounted guest evidence, including live client I/O and post-flush recovery. File controls are an optional qualified NTFS-image build. | Separate the volume engine and tray, and exercise UI restart, ownership handoff, detach, close and cleanup. Capture the interactive Windows client, driver, DLL and backing together. [Attachment](../../src/windows/winfsp_volume_attachment.cpp#L55) · [post-flush trial](../evidence/windows11-mounted-post-flush-recovery-2026-11-21.json) |
| Operation ordering | Native callbacks take a control lock. The optional deferred path owns a single source buffer and request hint. An incoming callback can force CPU completion. A response attempt and the client's observation are separate. | Trace direct, cached, mapped and concurrent I/O through the actual callback order. Measure how often traffic forces CPU, and separate validation, lock, flush and response costs before widening concurrency. [Callback boundary](../../src/windows/winfsp_native.cpp#L418) · [deferred service](../../src/windows/native_deferred_write.cpp#L44) |
| Namespace scale | Paged keys, demand reads and bounded continuation replaced the flat model on the current mounted path. Healthy writable reopen uses one checked audit, but that audit still visits the complete inventory. A million-object test and a 16,384-file native reopen test prove different properties. | Measure populated mounted paths and dirty/clean startup under the same counts, shape and backing. Use a generation-bound admission path only after the full CPU verifier proves the equivalent visibility and failure contract. [Retained reopen](../../src/native_session.cpp#L642) · [scale results](../evidence/indexed-scale-reopen-2026-11-20.json) |
| Allocation and retention | Paged metadata requires children and payloads to precede parent offsets. Staged paired-range rebuild and retirement can reconcile unrelated writes, but need alternate capacity and scan work. | Give reusable extents durable allocation incarnations and prove reachability across active roots, retained recovery and reader pins. Keep existing append-layout checks for old images. [Reference checks](../../src/paged_store.cpp#L255) · [file reclaimer](../../src/native_volume_file.cpp#L227) |
| Durability and repair | A mirrored transaction reads back new records, flushes data and publishes two individually flushed root slots. The outer selector has its own handoff. The surviving peer is checked before exposure. File backing validates access and geometry and calls the OS flush. | Exercise each barrier against a reference application trace and verify ordinary Flush, response loss, shrink, rename and image switch. Model complete backend loss with an independent restore source. Publish distinct conclusions for model, guest and physical evidence. [Paged commit](../../src/paged_store.cpp#L1947) · [selector](../../src/volume_head.cpp#L233) · [backing](../../src/store_file.cpp#L109) |
| Compression representations | The indexed decoder implements exact sharing, independently checked protected base members and packed slices. It has several residual or prefix formats. Optional codec availability affects declared reader capability. | Keep these as useful controls. Choose a representation using complete occupied/settled bytes and observed read/rewrite costs. Run every candidate through the independent CPU reader, including required codec and dependency availability. [Indexed decode](../../src/native_indexed_volume.cpp#L213) · [format methods](../../src/native_group_codec.hpp#L14) · [indexed format](../INDEXED_NATIVE_VOLUME.md) |
| Similarity and learning | The catalog already supports an exact-band primary, versioned projected keys and a bounded alternate lookup. Entries carry candidate identity and revision. Recall, space cost and stability under million-file churn still need measurements. | Use existing roles and formats as baselines for sparse paged multireference selection. Compare sampled, position-shifted and content-defined methods on small exhaustive oracles and held-out large cohorts. Handle stale postings and frequent keys using measured query budgets. [Catalog API](../../include/flitzfs/indexed_coherence.hpp#L26) · [candidate selection](../../src/native_maintenance.cpp#L530) |
| Host governance | A per-volume controller schedules bounded work using live observations, OS priority and a 1% elapsed-work pacing policy. Scrub, group work and image preparation can block or defer at different points. Advisory credit has a separate store. | Measure combined pressure across volumes, deadlines, cancellation and physical writes. Share reservations across CPU, RAM/commit and GPU, then tune work from workload changes and observed foreground impact. [Controller pacing](../../src/windows/winfsp_controller.cpp#L205) · [maintenance advancement](../../src/native_maintenance.cpp#L1132) |
| AMD useful work | Installed acquisition is connected to background anchor work. The running native controller's foreground route offer has SDK test callers. The independent controlled bridge wrote 16 AMD-advised extents and matched 32 mirrors. The real batch check executed three matched groups. | Connect the ordinary write offer using workload and device evidence. Exercise installed foreground I/O, measured fallback and subsequent useful work on one client and candidate manifest. Keep independently readable CPU bytes. [Controller entry](../../src/windows/winfsp_controller.cpp#L443) · [latest hardware receipt](../evidence/amd-fresh-completion-observation-2026-11-21.json) |
| AMD build ownership | The first uniform rebuild in the latest campaign failed to link with an older companion object. The successful build matched all eight live companion inputs. ROCm Core 10.2.0, the HIP 7.17.26434 headers and the amdhip64_7.dll ABI are three recorded identities. | Create role-specific link targets or explicit shared companion dependencies. Qualify actual staged SDK, worker, runtime and module together, including relocation and missing optional dependencies. [Recorded link refusal and recovery](../evidence/amd-fresh-completion-observation-2026-11-21.json) · [AMD component notes](https://rocm.docs.amd.com/en/develop/about/release-notes.html) |
| Compatibility and tooling | Prototype stream/catalog tools, flat and indexed images, SDK mocks, optional role builds and guest smoke tests use different product boundaries. The current MSVC sanitizer path stages and checks the configured runtime before launch. | Keep old readers and fixtures, add a versioned standalone native rescue executable, and tie each test to the exact artifact and capability it establishes. Compare installed behavior before considering a product gate closed. [Build options](../../CMakeLists.txt#L34) · [diagnostic CLI](../../src/cli.cpp#L29) · [native CPU inspection](../../src/native_volume_file.cpp#L471) · [sanitizer staging](../../cmake/WindowsAsanRuntime.cmake) |

## Primary research and architectural consequences

- **Windows client and driver.** Microsoft lists the general-availability
  Windows 11 26H2 family separately from 26H1 for new devices. The captured
  November 26H2 entries include regular servicing build 26300.9974 and
  optional 26300.10075. Qualify the general-availability x64 client and
  record the actual servicing build. WinFsp 2.2 was published on November 19.
  Its architecture supports the existing user-mode engine direction, and its
  service model gives a documented lifecycle alternative to investigate for
  independent volume ownership. [Windows release information](https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information)
  · [WinFsp 2.2](https://github.com/winfsp/winfsp/releases/tag/v2.2)
  · [WinFsp design](https://winfsp.dev/doc/WinFsp-Design/)
  · [WinFsp service architecture](https://winfsp.dev/doc/WinFsp-Service-Architecture/)
- **Commit and crash recovery.** F2FSJ orders data flushing ahead of committed
  metadata and handles out-of-place identities. WOFS packages file operation
  metadata for persistent memory. Together they motivate an explicit operation
  record, measured publication boundaries and a tested owner for each durable
  generation. The FlitzFS WinFsp/NTFS path still requires its own mapping from
  application success to the provider's barriers. Windows distinguishes mapped
  view flushing from a full file flush. [F2FSJ, OSDI 2025](https://www.usenix.org/conference/osdi25/presentation/cui)
  · [WOFS, OSDI 2025](https://www.usenix.org/conference/osdi25/presentation/pan)
  · [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
  · [FlushViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-flushviewoffile)
- **Integrity and repair.** The 2026 FS-PI work measures integrity coverage
  through multiple storage layers. ReFS documents repair with available
  alternate copies. These motivate separate checks at codec output, record,
  allocation owner and backing boundaries, plus a precise distinction between
  verified peer repair and restoring data after full backing loss. [FS-PI, FAST 2026](https://www.usenix.org/conference/fast26/presentation/gupta)
  · [Microsoft ReFS overview](https://learn.microsoft.com/en-us/windows-server/storage/refs/refs-overview)
- **Collective compression.** RubikFS validates similarity and hotness sorting
  for read-only image creation. Its paper also measures the cost of sorting
  and discusses read amplification. EROFS demonstrates independently fetched
  physical clusters. Btrfs documents the metadata cost of smaller compressed
  extents. These support sparse nominations and independent restart blocks for
  mutable FlitzFS, with complete-size costs counted for finer indexing. Zstd
  offers an established dictionary and frame control against which to test
  new region-level references or learned models. [RubikFS, FAST 2026](https://www.usenix.org/conference/fast26/presentation/huang)
  · [EROFS design](https://erofs.docs.kernel.org/en/latest/design.html)
  · [Btrfs compression](https://btrfs.readthedocs.io/en/latest/Compression.html)
  · [Zstandard training](https://github.com/facebook/zstd/blob/dev/programs/README.md)
  · [Zstandard frame format](https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md)
- **Hardware funding and comparisons.** The selected AMD SDK, runtime and node
  are acquired independently of the CPU engine. DXGI documents a live process
  budgeting interface, and AMD documents limitations on stream priority.
  Bound actual GPU work and its retirement. DiskSpd 2.3 changes affinity and
  buffer defaults that can affect comparisons. Lock tool version, cache,
  placement and command lines, and extend I/O tests with the same client and
  corpus activities for each filesystem. [DXGI process budget](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo)
  · [AMD HIP stream management](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/reference/hip_runtime_api/modules/stream_management.html)
  · [Microsoft DiskSpd 2.3](https://github.com/microsoft/diskspd/releases/tag/v2.3)
  · [DiskSpd configuration](https://github.com/microsoft/diskspd/wiki/Customizing-tests)

Reviewed the related FlitzZip checkout's AMD-only backend convention and
maintainer-side Zstd patch mechanism. FlitzFS already has private staging,
portable recovery and protected native record formats. Reuse verified build,
dependency and runtime mechanisms where they fit. Compare codecs on FlitzFS's
own mutable, range-readable and independently recoverable storage contract.

## Release assessment

Four open tracks dominate the remaining uncertainty:

1. The ordinary installed Windows path needs integrated byte, operation-order
   and lifecycle proof, including automatic AMD work and a CPU-only lane.
2. Space and startup need populated-volume curves. Reuse must preserve retained
   readers and every acknowledged state while reducing repeated full-image
   work.
3. An admissible compression winner needs a held-out complete-size advantage
   under measured range-read, host-interference and write-cost bounds.
4. Recovery, compatibility, installation and measured baseline results must
   bind to the same candidate. The first physical interruption campaign, mixed
   format migration and sustained installed-client soak still matter.

The current work-package planning range remains 60–130 person-weeks, with
low confidence before the integrated evidence. The compression frontier
remains an independent research risk. [Release effort and gates](../RELEASE_EXECUTION_PLAN.md#remaining-engineering-effort)
