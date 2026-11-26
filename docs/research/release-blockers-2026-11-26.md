# FlitzFS release blockers and implementation priorities

Reviewed on 26 November 2026 against source `30be66335ad34667eb81d3b695806037f53c88da`.
The release depends on a normal mounted Windows client consuming useful
AMD work, independently recoverable commits on each declared backing
provider, and a matched measurement of complete occupied size. The
[implementation plan](../IMPLEMENTATION_PLAN.md) owns the work packages.

## 1. Accelerate ordinary Windows I/O

The installed callback evidence shows verified Radeon read results reaching
later requests. The earlier prepared write experiment shows how much process
and runtime setup can dominate compression even when the kernel succeeds.
Start with the existing mounted Win32 client. Track demand, queueing, image
generation, adapter identity, capture, setup, transfers, compute, verification,
delivery and retirement using host-clock timestamps. Measure CPU, cold AMD
and prepared AMD on identical request streams. Count bytes actually used by
the application. A kernel-only result cannot establish throughput or tail latency.
[Read delivery evidence](../evidence/windows-later-lz4-reads-2026-11-26.json) ·
[Prepared write measurements](../evidence/windows-prepared-write-retry-2026-11-26.json)

Keep speculative work outside the callback's critical path. A WinFsp
report reproduced a same-file open waiting on an in-flight noncached
read, including when the read returned pending. Retest same-file and
different-file opens on the exact signed build, both with populated
file caches and with cold ranges. The Windows test should exercise
normal CPU service during an outstanding request and verify that the
existing generation check prevents using a reply after a write.
[WinFsp issue #676](https://github.com/winfsp/winfsp/issues/676)

Measure how setup amortizes across the existing bounded multi-dispatch
exchange before trying pinned staging, graph replay or a longer prepared
lease. Keep a lease only while fresh demand, measured turnaround and
resource headroom justify it. Recheck pressure before submission and
learn from completed work that arrived after the request. AMD's ROCm
10.2.0 notes list HIP 7.17.0 and disable priority streams. An upstream
Windows report describes a stalled fence after a copy on a specific
earlier device and runtime combination. This supports finite jobs,
immediate CPU service and confirmed worker retirement before retry.
HIP errors, changing adapter or driver identity, and recovered resources
should each be observed separately from the passage of time.
[AMD release notes](https://rocm.docs.amd.com/en/docs-10.2.0/about/release-notes.html#hip-7-17-0) ·
[AMD runtime issue #12535](https://github.com/ROCm/rocm-systems/issues/12535) ·
[NSDI 2026 delayed-hit analysis](https://www.usenix.org/conference/nsdi26/presentation/keren)

In the latest four-frame attempt the initial GPU load was low. The
application later returned `engine_deferred` before any HIP identity query.
Two subsequent checks found over 97% existing GPU load and retired
before the fixture ran. Distinguish expired, missing, invalid and busy
engine samples at the point of the decision. Compare the sample taken
before package verification with current controller evidence immediately
before launch. A rate counter requires a valid interval and status on
each reported instance.
[Four-frame attempt records](../evidence/windows-amd-engine-observations-2026-11-26.json) ·
[Microsoft rate counter collection](https://learn.microsoft.com/en-us/windows/win32/perfctrs/collecting-performance-data) ·
[Microsoft counter status contract](https://learn.microsoft.com/en-us/windows/win32/perfctrs/checking-pdh-interface-return-values)

## 2. Qualify acknowledgement, repair and the installed lifecycle

Use the same client to exercise cached writes under memory pressure,
mapped flush, rename, handle closure and teardown while reads and
notifications are outstanding. WinFsp's maintainers fixed distinct
cached-write and shutdown deadlocks. Their failure patterns belong in
the guest regression matrix for the exact kernel/user-mode pair.
Compare the last acknowledged namespace and byte hashes after every
publication, flush, reclaim, repair and migration cut. Run independent
CPU recovery from copied surviving ranges after each cut.
[WinFsp stable v2.2](https://github.com/winfsp/winfsp/releases/tag/v2.2) ·
[Cached-write diagnosis](https://github.com/winfsp/winfsp/pull/677) ·
[Shutdown diagnosis](https://github.com/winfsp/winfsp/issues/682)

Determine the backing file's effective failure unit separately from
device geometry and preferred I/O alignment. Microsoft documents cases
where Windows reports different physical and filesystem-effective sizes
for the same device. NVMe power-fail atomicity research is relevant to
the physical provider's contract. Verify the contract through the
actual Windows stack and cache barriers on disposable media during
release qualification. FS-PI reinforces the need to check a complete
data and metadata path. Compare repair from each independently protected
copy and descriptor region. Report lower-level write amplification
separately from FlitzFS writes when provider telemetry supports it.
[Windows NVMe sector reporting](https://learn.microsoft.com/en-us/troubleshoot/sql/database-engine/database-file-operations/troubleshoot-os-4kb-disk-sector-size) ·
[FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers) ·
[FAST 2025 AWUPF](https://www.usenix.org/conference/fast25/presentation/jeon) ·
[FAST 2026 FS-PI](https://www.usenix.org/conference/fast26/presentation/gupta) ·
[FAST 2026 WARP](https://www.usenix.org/conference/fast26/presentation/song)

## 3. Make writable attachment scale with changed state

Preserve the independent full inventory for recovery and use it as the
reference for a generation-bound ownership summary. Update the summary
with the extent, inode, name, reference and allocator changes it describes.
In comparison mode, require agreement across interrupted transactions,
dirty history, repair, reuse, relocation and upgrades. Measure both
routes on geometric growth tiers with ordered and permuted names,
nested and flat directories, short files and large sparse files. Fit
attach work to live and changed pages, then test the estimated slope on
the largest held-out tier before projecting to tens of millions of files.
Measure backing requests, allocated memory, startup time and namespace
tail latency separately. Epoch-based F2FS work provides another example
of bounding recovery work with versioned state. FlitzFS must validate
its own copy-on-write roots and reverse dependencies.
[Indexed design](../INDEXED_NATIVE_VOLUME.md) ·
[Current audit measurements](../evidence/windows-packed-audit-locality-2026-11-26.json) ·
[OSDI 2025 epoch journaling](https://www.usenix.org/conference/osdi25/presentation/cui)

## 4. Reduce settled occupied bytes within a finite idle budget

Use sparse similarity postings to nominate revision-bound candidates
in the rebuildable advisory area. A family can cover many files across
independently readable extents. Keep hotness and decode amplification
in the ranking. A complete pairwise matrix for ten million objects
would contain about 50 trillion entries. With sparse postings,
persistent cost follows retained features and candidate work follows
the funded probes. Measure both under skewed common fingerprints.
Use bounded windows and sample actual encode cost to choose where to
spend work. RubikFS demonstrates joint similarity and
access locality in read-only images. SkyFS studies windowed reordering
in a distributed storage system. DOGI motivates comparing the online
selector with a stronger oracle on small, held-out subsets.
[FAST 2026 RubikFS](https://www.usenix.org/conference/fast26/presentation/huang) ·
[November 2026 SkyFS preprint](https://arxiv.org/abs/2611.22847) ·
[FAST 2026 DOGI](https://www.usenix.org/conference/fast26/presentation/kim-jeeyun)

Label each candidate by measured occupied bytes after protected
publication, reclamation and settlement. Include reference metadata,
codec dependencies, retained history, parity, small-read amplification
and extra written bytes. On small frozen cohorts, price the feasible
groups to estimate how much attainable saving the online search misses.
Compare that recall under fixed work and rewrite budgets before extending
the index across more windows. Test shared dictionaries, bounded delta chains
and reversible data conditioning on their intended data families.
The ZipLLM authors measured structured redundancy across related model
versions. This supports testing byte-plane and delta methods on such
families with ordinary unrelated and already-compressed controls.
[NSDI 2026 ZipLLM](https://www.usenix.org/conference/nsdi26/presentation/wang-zirui) ·
[Zstandard frame specification](https://github.com/meta/zstd/blob/dev/doc/zstd_compression_format.md) ·
[AITDCC 2026 public files](https://aitdcc.github.io/dataset.html)

For denser GPU-readable data, compare an independently framed Zstandard
route before designing new decoder state. Microsoft's DirectStorage 1.4
preview supplies a GPU shader reference for small frames and demonstrates
reversible conditioning for specific texture formats. AMD describes its
optimization work on that decoder. Measure the existing reference, CPU
Zstandard and a scoped HIP mapping with the same FlitzFS frame boundaries,
host transfers and verification. One DirectStorage preview report describes
incorrect GDeflate bytes under load. Include this general failure mode in
the device test by requiring CPU-verified bytes at each delivery.
[Microsoft shader and conditioning preview](https://devblogs.microsoft.com/directx/directstorage-1-4-release-adds-support-for-zstandard/) ·
[AMD optimization announcement](https://gpuopen.com/learn/amd-microsoft-gdc-2026/) ·
[DirectStorage readback report](https://github.com/microsoft/DirectStorage/issues/175)

Learn from exact gains and costs first with a small local ranker. Keep it
advisory and compare its recall, rewrite cost and idle duty against the
current scoring under the same work budget. Evaluate llama.cpp as an
optional experiment if its measured benefit pays for model bytes and
inference. The November compression preprint distinguishes removable
GPU pipeline overhead from sequential decoder work. Nacrith combines a
transformer with lighter online predictors and arithmetic coding.
A decoder experiment needs bounded restart intervals, stable bit-exact
CPU and GPU predictions, and a measured random-read path before it can
become a format capability.
[November 2026 invocation bottleneck preprint](https://arxiv.org/abs/2611.05871) ·
[Nacrith preprint](https://arxiv.org/abs/2602.19626)

## 5. Make installation portable and evidence reproducible

Discover the OS, installed driver, device architecture and runtime
capabilities. Bind the exact private files used by each worker to a
package manifest. AMD documents a portable Windows distribution for
the ROCm 10.2.0 SDK. Its component and job execution versions must be
verified independently. AMD's developer threads also describe differing
APU support and install outcomes. Test a clean Windows installation,
compatible and incompatible pre-existing components, a missing AMD
runtime, an interrupted private provision, restart and uninstall.
Use the same byte-exact client before and after the operation.
[ROCm Windows distribution](https://rocm.docs.amd.com/en/docs-10.2.0/install/rocm.html) ·
[AMD Developer Community](https://devcommunity.amd.com/t/rocm-10-2-0-advancing-performance-stability-and-developer-experience/1497) ·
[AMD Windows packaging discussion](https://github.com/ROCm/TheRock/issues/8538)

Pair sanitizer builds with the selected compiler's matching DLL and
the existing preflight. Microsoft documents a separate dynamic ASan
runtime for both MSVC CRT linkage modes. Run tests under the existing
noninteractive supervisor and confirm retirement of the launched tree.
[Microsoft ASan linker and runtime contract](https://learn.microsoft.com/en-us/cpp/sanitizers/asan-building?view=msvc-170)

## 6. Measure on the matched Windows client

Use DiskSpd 2.3 XML profiles with the same completion method, caching,
affinity, queue depth and data across targets. Use Win32 client traces
for mapped, directory and durability behavior. The WinFsp performance
guide describes distinct cached, noncached and mapped workloads and
helps interpret where a request reaches user mode. For the Windows
baseline, compare plain NTFS and native NTFS file compression using the
file compression attribute and `FSCTL_GET_COMPRESSION` on the test
copies. Track complete allocation after setup, writes and settlement,
alongside p50/p99, throughput, foreground interference, host resource
use and independent readback. AITDCC complements the recognized file
I/O workload with recent lossless coding controls.
[DiskSpd 2.3 XML profiles](https://github.com/microsoft/diskspd/wiki/XML-profiles) ·
[WinFsp performance methodology](https://github.com/winfsp/winfsp/blob/v2.2B5/doc/WinFsp-Performance-Testing.asciidoc) ·
[Native Windows compression](https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/optimize/compactos) ·
[FlitzFS benchmark protocol](../BENCHMARK_PROTOCOL.md)
