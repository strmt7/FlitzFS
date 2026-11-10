# Bounded RAM-only host qualification (experimental)

The owner explicitly includes their Windows11 PC in testing. Filesystem image
backing on that PC MUST remain RAM-only, never a system/HDD/SSD/NVMe image, until
the owner changes this instruction. Tiny qualified GPU work is authorized, but
no driver installation/reinstallation/change, clocks/power/boot changes, stress
or induced WDDM faults follow. No questions, elevation popups, scheduled services
or unattended workers are needed for this workflow.

## Source corpus and controlled staging

Real files complement synthetic input. The owner permits a modest temporary
corpus on the verified HDD, preserving every existing file. Keep corpus plus
small experimental tool/report artifacts below256 MiB initially, with per-file,
stream, network-time and free-space limits. The initial corpus download cap is
8 MiB, not a target to fill. Files are CREATE_NEW inside unique owned fixtures;
retain URL/commit, actual bytes, digest and errors. Never execute corpus content.
Only remove verified owned files after testing; never recurse into other data.

These source/tool/report files are NOT filesystem backing. All authoritative
live/stable model images and volume-head bytes in the probe reside in locked
RAM. No image container, mount, driver or GPU context is created by this probe.
Its optional source/report paths are explicit absolute local paths; UNC, ADS,
relative paths, final reparse points, multiple hard links and oversized input
are rejected. Source handles are read-only with no write/delete sharing.

## Qualified probe boundary

FLITZFS_BUILD_RAM_HOST_PROBE is OFF by default and requires a portable Windows
build, bounded Zstandard and an exact source commit. It reuses the already
qualified creation-time Windows Job supervisor, not a new process launcher.
The child must independently verify:

- 128 MiB process AND job committed-memory caps; one process, no breakaway.
- Below-normal priority and20% hard CPU cap;15-second supervision deadline.
- At least2 GiB fresh available physical AND commit reserve.
- Maximum40 MiB aggregate locked allocations; actual image buffers peak32.25 MiB.
- A process-local working-set allowance, restored after all buffers are freed.
- VirtualLock success and QueryWorkingSetEx Valid/Locked checks before and after
  use. Lock/residency/privilege failure means immediate rejection; no pageable
  allocation, file backing, privilege elevation or repeated retries as fallback.
- Byte-exact readback and independently selected compacted-image contents.
- Zero locked allocations afterward, successful unlock/free/working-set restore,
  balanced covered HANDLE counts, and confirmed child process exit.

The live/stable buffers form a VOLATILE RAM persistence model. Stable means the
last modeled flush while the process lives; power loss/process termination can
destroy RAM. This is NOT physical power-loss qualification. Locking these image
buffers does not assert that Windows itself, source input or every temporary
codec allocation performs zero physical I/O.

The child writes at most2048 bytes of numeric qualification/error report through
CREATE_NEW. The parent reads it only after actual process exit; failed/timed-out
children authorize no result and are not retried. No shell or arbitrary helper
executable is launched. The exact same binary supervises its one child.

## Measurements and interpretation

Raw and automatic CPU policies process identical inputs in separate synthetic
and real-data groups. Source files are bounded to six files/256 KiB each. Timed
core initialization/write, validated reads and compaction exclude source-network
transfer and WinFsp/kernel I/O. Report full source/compacted extents, active stored
payload and original input bytes separately. Retained source plus candidate,
head, allocator slack and stable RAM copies are additional resource costs.

These single-run phase timings are diagnostics, NOT comparative filesystem
benchmarks or a GPU speedup. Compaction preserves existing payload encodings. The separate explicit
post-compression phase uses recompress() and verifies the raw-to-automatic
candidate; it is not an idle scheduler or learning system. See RECOMPRESSION.md. Formal comparisons still need
controlled alternating repetitions, live load/cache/durability accounting,
appropriate established tools and several comparator filesystems.

## Build/provenance gate and actual host state

The isolated Windows workflow builds the static-runtime executable without the
WinFsp SDK or GPU dependencies, audits its actual PE imports, executes the exact
bounded probe with real repository files and synthetic data, and promotes a
small artifact only after complete matching source/residency/cleanup evidence.
Preserve dependency licenses and SHA256 provenance. Before owner-PC execution,
verify the exact archive/executable identity, limits, available headroom and
known-input/report fixtures. Do not substitute an unqualified local build.

A read-only host check identified a stopped WinFsp2025/2.1.25156 driver, with no
RAM volume present. It has NOT been loaded, changed or replaced. That is not the
already qualified hosted2026 SDK/driver path. A real RAM-backed mounted host
experiment requires its own exact driver/helper/backing qualification first;
the CPU RAM probe must never be called a mounted-filesystem test.

Microsoft memory API references were captured by the reviewed isolated
Crawl4AI provider on10 November2026: all four native HTTP200 captures passed
scope/content checks, with receipt
70f77c2bcc26e0c88d97dd36e8ae36ec1defde5a4ae8d3249fd116ebd41b53bb
(job130115897265). The provider used robots checks,45-second page limits, no
external LLM/profile/bypass. Publisher dates/hashes are retained in
HOST_RAM_SOURCES.json; current serving is not a recent-revision claim. Runtime
qualification, not a documentation assertion, must establish actual page locks.

The first exact VM-qualified owner-PC CPU run is recorded in
HOST_RAM_VALIDATION.json. Later probe changes need their own VM qualification;
that earlier result never qualifies a changed binary or a mounted/GPU path.

## Managed-session probe update

The next probe version uses public managed-session creation, per-file writes,
live-handle reads across both compaction and post-compression, independent head
selection, real Flush and actual Cleanup/Close. This changes phase scope and
transaction history from the earlier batched primitive-only probes; do not
compare their diagnostic timings/image extents as a controlled speedup. The
resource caps and VM-first qualification requirement are unchanged. See
VOLUME_SESSION.md; mounted maintenance and automatic scheduling remain absent.
