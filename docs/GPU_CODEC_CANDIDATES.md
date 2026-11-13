# AMD GPU codec candidate evaluation (experimental)

13 November 2026. Native filesystem operations still use the existing CPU route.
The helper at `99d5365` passed all 13 integration jobs, its 728-assertion application
model, hosted GUI workflow and one healthy owner GUI check. Its complete evidence
is retained separately. This evaluation does not change a storage format or select
a new runtime dependency.

## Current sources and candidate

The reviewed isolated Crawl4AI provider fetched AMD's current release notes,
selected 10.1.0 release notes and installation page on 13 November. All three
returned HTTP 200 with substantive extraction and matching retained markdown
digests. Both release-note titles identify ROCm Core SDK 10.1.0. Publisher revision
dates were not verified; these are current-served captures. The installation
page redirected to its default Linux query, so it does not establish a particular
native Windows installation procedure. No host installation follows.

The ROCm distribution remains the toolchain selection authority. Future compiler,
HIP headers and runtime qualification must inspect the actual selected package;
a component's independent version or an upstream project's older tested-version
list cannot silently select the host toolchain.

AMD's [hipCOMP source](https://github.com/ROCm/hipCOMP-core/tree/03df9f7369211ee15d370817165224c73fdc9a46)
is pinned to `03df9f7369211ee15d370817165224c73fdc9a46`, committed 29 October.
Its current README identifies a 2.3.0 early-access implementation with LZ4, Snappy,
Cascaded and RLE encoders and decoders; Zstandard and Deflate support is described
as experimental decoding only. The implementations are described as unoptimized.
The source has not been built, run or qualified for FlitzFS, Windows compatibility
or useful compression gains. The reviewed native releases API returned no release
entries; the pin is a source checkpoint, not a claimed stable package release.

The root MIT notice and the complete CMake file containing the additional
NVIDIA/AMD notices are hash-pinned for retention. Any eventual code reuse still
requires its applicable file notices and source provenance. The candidate is a
filesystem implementation reference, with no archive-software product interface.

## Separate source-bound navigation

The existing isolated semantic workflow adds the closed `hipcomp` query group.
Its default FlitzFS and maintenance scopes remain unchanged. The new group stages
only the pinned public checkout in a separate directory, verifies both the control
repository and selected source Git roots, exact revisions and tracked cleanliness,
then indexes a private text mirror. Upstream code, CMake and tests are never run.

Two actual queries target bounded batched compression and portable framing.
CUDA source/header suffixes are admitted only for this candidate, with C++ parsing;
test/oracle directories are excluded. Existing per-file, aggregate, CPU, memory,
process, time, package and model controls remain. No existing MCP registration,
credentials, owner-host index or shared cache is changed.

Every query and final routing receipt records the selected repository and source
commit plus the independent workflow/control commit. A source/query mismatch
rejects before indexing. The exact source inventory and native response digests
remain required; a downloaded checkout or index marker alone is insufficient.
Relevant hits must be confirmed in the actual pinned source before further work.

The expanded admission suite passed all 22 tests at `905a4d3`. Default routing
completed eight queries over 143 FlitzFS files; upstream routing completed two
queries over 142 pinned candidate files, with zero reported indexing errors.
Repository/source/control identities, inventories, notices and relevant live LZ4
source hashes were admitted. See [executed navigation evidence](evidence/gpu-codec-navigation-905a4d3.json).
The [offline Windows object gate](HIPCOMP_OFFLINE.md) at df51d8c passed all
20 new source/staging tests and native compilation, with the existing SDK/worker
gates preserved. No runtime or CPU-format compatibility is established. The new
[bounded allocation plan](GPU_LZ4_LAYOUT.md) and native object inspection require
exact-source execution.

## Evaluation gates

1. Admit the actual candidate search receipts, source identities and notices.
2. Review kernel bounds, scratch/output calculations, stream/status semantics,
   malformed input, CPU format recovery and every dependency used by the build.
3. Compile an audited minimal candidate using the current selected ROCm package
   in disposable Windows VMs. No driver installation, module loading or GPU
   execution is implied by successful compilation.
4. Verify byte-exact independent CPU decoding, raw fallback, immutable owned input,
   terminal failure and source-preserving publication before any runtime use.
5. Retain the current batch, outstanding-request and process/resource bounds.
   Refuse a candidate whose required scratch/output exceeds admission; changes
   to those bounds need their own replacement contract and qualification.
6. Measure complete foreground/background operations on held-out mixed data,
   including startup, transfers, verification, metadata and recovery replicas.
   A faster kernel or a larger payload-only ratio is insufficient.
7. Qualify any worker amortization and low-priority scheduling separately.
   Stream priority must not be assumed to provide preemption or bounded impact.
   Failed GPU work still selects independent CPU bytes immediately and retains
   supervised ownership until actual retirement.

Durable mapping/authorization, long-lived funded continuation, automatic learning,
collective optimization, useful end-to-end GPU gains, Windows semantics,
installation and physical-backend qualification remain part of the full goal.
