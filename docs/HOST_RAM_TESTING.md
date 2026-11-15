# Local development and host testing (experimental)

The owner's 15 November 2026 instruction makes their Windows PC the primary
development and testing system. The blanket off-host, GitHub-only development
and mandatory VM-first restrictions are deleted. Edit, build, debug, run safe
CPU/GPU and GUI tests, and research locally. Use CI/VMs as complementary tools
for unavailable platforms or work that cannot safely run here. Existing user
changes must be preserved; use a separate local checkout when appropriate.

Filesystem experiment backing remains in memory. Normal source, build, tool,
dependency, cache and report files may use a scoped development directory with
fresh storage headroom. The owner's later 15 November direction conditionally
permits installing a stable, qualified FlitzFS-related driver; see AGENTS.md for
the artifact-bound qualification and rollback gate. No current experimental
driver is qualified by that permission. Never change the GPU or unrelated
drivers, alter clocks/power/boot/security settings, induce driver/kernel faults,
overwrite user data or leave stale processes, services or tasks. Host
authorization does not authorize releases.

The standard-library-only tools/dev/windows_run.py supports local Windows builds
and tests with creation-time Job Object assignment, aggregate process/memory/CPU
limits, finite deadlines, fresh physical/commit/storage reserves, exclusive logs
and confirmed process-tree retirement. Its memory allowance scales with current
headroom. Remaining owned tool helpers are stopped and reported at command end.
It is resource containment, not protection against arbitrary kernel faults.
Select appropriate tests: in-memory unit models do not create a mounted volume
or establish that Windows performs no paging/storage activity. Local builds do
not require a prior hosted qualification run.

## Source corpus and controlled staging

Real files complement synthetic input. The owner permits a modest temporary
corpus on the verified HDD, preserving every existing file. Keep corpus plus
small experimental tool/report artifacts under an explicit measured storage
budget, with per-file, stream, network-time and free-space limits. Normal build
and dependency caches have their own resource-adaptive development envelopes.
The initial corpus download cap is
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

The optional isolated Windows workflow builds the static-runtime executable without the
WinFsp SDK or GPU dependencies, audits its actual PE imports, executes the exact
bounded probe with real repository files and synthetic data, and promotes a
small artifact only after complete matching source/residency/cleanup evidence.
Preserve dependency licenses and SHA256 provenance. Before execution, verify the
actual source/binary identity, limits, available headroom and known-input/report
fixtures. Safe local builds/tests are authorized directly; hosted artifact
promotion is a separate provenance path, not a prerequisite for development.

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
HOST_RAM_VALIDATION.json. That earlier result never qualifies a changed binary
or a mounted/GPU path. Validate changed behavior locally where safe; use an
isolated VM for tests that could alter or destabilize the host.

## Managed-session probe update

The next probe version uses public managed-session creation, per-file writes,
live-handle reads across both compaction and post-compression, independent head
selection, real Flush and actual Cleanup/Close. This changes phase scope and
transaction history from the earlier batched primitive-only probes; do not
compare their diagnostic timings/image extents as a controlled speedup. The
probe-specific resource caps remain unchanged; VM-first is no longer a blanket
host-development requirement. See
VOLUME_SESSION.md; mounted maintenance and automatic scheduling remain absent.
