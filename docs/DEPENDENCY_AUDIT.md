# Dependency and alert qualification

Owner requirement: keep dependencies, packages and tools on latest compatible
stable versions and check Dependabot alerts. Builds/tool execution stay on
disposable hosted VMs; nothing is installed on the owner's PC.

## Verified Dependabot result

The owner's authenticated GitHub Security page was inspected directly on
9 November 2026 at https://github.com/strmt7/FlitzFS/security/dependabot.
It showed the default is:open filter, Open (0), Closed (0), and:
There are no open Dependabot alerts for this repository.
Dependency files had been checked five minutes before that inspection.
This is a real UI check, not an inference from an API failure or dependency pin.

The connector rejected the alerts endpoint, and the first hosted workflow token
attempt at f2d749c0e53ad4866de9983ac05fc2229c7e2daa returned HTTP 403.
That limited workflow-token access, not the owner's signed-in UI access.
No extra token or repository secret is required for the completed UI check.
The release-inventory workflow does not claim to query Dependabot. Future alert
checks can use the authenticated Security UI; preserve any service-access
failure rather than claiming it means zero alerts.

## Original 9 November native dependency and Action audit

Official native GitHub release/tag API captures confirmed Zstandard v1.5.7
(f8745da6ff1ad1e7bab384bd1f9d742439278e99), LZ4 v1.11.0
(c72c7c0ec132ad2ce2fe9fad7b0c728d9faee2fc), BLAKE3 1.8.7
(f3149ec5bb5449af877ba20377a11008ff499fa2) and WinFsp v2.1 as latest stable.
Windows/interruption source pins match those identities. Linux distribution
codec packages and toolchains are not thereby proved latest upstream.

The ten existing workflows now pin official checkout v7.0.1, setup-python
v7.1.0 and upload-artifact v7.0.2 to exact release commits, using Node 24.
Historical comparator/reference commits remain immutable evidence, not runtime
dependency update candidates. Do not advance a comparison baseline silently.

The release-inventory workflow captures release/tag identities, installed tool
versions, native response bytes, URL, HTTP status, timestamp and SHA-256 receipts.
Its check covers the explicit pins; it is not a comprehensive vulnerability scan.

## Remaining freshness work

The current hosted toolchain now stages verified CMake 4.4.5, Linux LLVM
23.1.5 and Python 3.15.2. Linux native codec lanes build exact latest reviewed
Zstandard/LZ4 source pins; sanitizer lanes instrument those libraries too.
The corrected release audit passed at e356a4a7b5e35fae836d3380e140b00a9d7e80df.
Portable CPU relocation, raw/codec, sanitizer, Windows callback and offline HIP
checks passed at that commit; mounted qualification is recorded separately.

The reviewed isolated research provider still uses pinned Python 3.13 and a
95-package explicit lock; its current wrapper admits Python 3.13..3.14 only.
Changing that guard requires source/lock compatibility qualification, not a
suppression. Full transitive tooling and broader Windows compiler inventory
remain open. No owner-PC tool is installed or changed.

The shared CocoIndex registration belongs to another repository and must not be
used or altered. This audit reads exact known build/workflow/dependency scopes;
restore an isolated off-host provider before new broad/fuzzy repository searches.

## Current build-tool/source-codec upgrade

The hosted provisioning implementation and limits are in HOST_INDEPENDENCE.md.
Build qualification now stages official CMake 4.4.5, Linux LLVM 23.1.5 and stable
Python 3.15 in private job locations. Default Linux C/C++ tools come from the
verified LLVM directory; Windows retains its explicitly qualified MSVC target.
Linux codec-enabled/reference/comparison jobs use the exact latest source pins,
including sanitizer-instrumented dependencies in the sanitizer lane.
Official release inventory checks archive version/size/SHA-256 metadata against
the provisioning pins and prints actual installed tool versions.

Inspect executed exact-commit results before calling the migration compatible.
Research-provider Python/locked packages and broader end-user runtime portability
remain open qualification work; old reference commits are intentional immutable
comparison evidence. This upgrade performs no owner-PC installation.

## Isolated semantic-tool qualification

CocoIndex Code 0.2.43 is now qualified off-host with real source-bound searches;
HOSTED_ROUTING.md records the binary-only private CPU environment and source/
model/package receipts. Its released commit and official wheel URL, size and
SHA-256 join the native latest-release audit, using the actual provisioning
constants. The semantic lane uses qualified Python 3.14 because its complete
binary-only package graph was unavailable on 3.15; normal build tooling remains
3.15. This is a development provider, not a filesystem runtime or learning model.
The separate Crawl4AI provider's older locked graph remains an explicit review
item, so this does not claim universal transitive freshness.


## ROCm and HIP selection correction

The installable ROCm distribution controls its included HIP/compiler/library
versions. The legacy HIP repository's latest-release pointer was insufficient
current evidence and must not determine this toolchain independently.
The selected whole-package lock matches current FlitzZip reference source
113ca5f30655f0d373c17d1448e35ce1b69f2819: ROCm 10.1.0, observed HIP
7.16.26385-7da43026e4, observed AMD Clang 24.0.0git. Its download checksum is an
observed official-HTTPS digest, not an independently published AMD checksum.
Fresh official release/install captures and actual package/compiler checks
remain required; a repository lock alone does not prove "latest compatible".


## Driver stable/prerelease inventory correction, 9 November 2026

A native API review of the complete current WinFsp release list found v2.2B5
(13 October 2026) beyond releases/latest's v2.1. Its official notes list multiple
security/deadlock fixes. Select it as an explicit disposable-VM DEVELOPMENT
candidate, not a stable product release or owner installation. Its verified
source tag, official asset digest/length and prerelease status are centralized
in tools/winfsp-sdk-lock.json and consumed by both staging and installation.
The audit must inspect the full bounded release inventory for this dependency;
a stable-only green result is insufficient. See WINDOWS_ARCHITECTURE.md.
The authenticated zero-Dependabot observation covers repository alerts only,
not a proof that every external SDK/driver has no upstream security fixes.


## LZ4 1.11.1 follow-through, 10 November 2026

The current native release inventory failed at 6574049cb0e7534eba83b5560cacb7b6741c38e6
after LZ4 published v1.11.1 at 05:49:08 UTC. The strict current-release gate
correctly rejected the old pin; it was not a filesystem test failure.
Job 129661802483 retained native responses and audit rows in artifact
13739905382, SHA256
3cb422eecb834ab629728912fff9ed317dc3033e8295b3cfb14eef96a8ac814b.
The console now identifies each outdated repository/tag/commit directly.

The official release/tag API resolves v1.11.1 to immutable commit
aa4647fad575d01a874aaaadcdbe8e6a40e072f5. Current Windows/Linux codec
provisioning, SDK/portable qualification and mechanism-comparison dependency
checkouts now select that source. The audit declares the same identity.
No owner-PC tool/package is installed or modified.

- [Official release and fix scope](https://github.com/lz4/lz4/releases/tag/v1.11.1)
- [Actual source delta](https://github.com/lz4/lz4/compare/c72c7c0ec132ad2ce2fe9fad7b0c728d9faee2fc...aa4647fad575d01a874aaaadcdbe8e6a40e072f5)
- [Fixed HC reset implementation](https://github.com/lz4/lz4/blob/aa4647fad575d01a874aaaadcdbe8e6a40e072f5/lib/lz4hc.c)

The primary fix initializes the deprecated LZ4_resetStreamHC state at its
declared size rather than trusting dirty memory's extension marker; upstream
reported out-of-bounds writes at HC levels >=10 under that usage. Exact native
repository code search found no FlitzFS call to this symbol. Do not claim an
executed FlitzFS exploit, generic security immunity or an AMD64 speed gain from
upstream's separate ppc64 measurements. Source/build fixes are reviewed; actual
compatibility still requires exact-commit Windows, portable, sanitizer and
decoder-reference results.

Historical codec/benchmark results and immutable FlitzFS/FlitzZip comparator
source commits remain unchanged. Zstandard stays at the same reviewed identity.
The CPU and native-store timing configurations disable LZ4; their shared
provisioning changes do not replace those timed codecs. The extent differential
oracle now uses the same new LZ4 library for both current and pinned reference
mechanisms, explicitly changing that dependency while retaining reference
source/cases. No archived result is relabelled as measured with 1.11.1.

This release audit remains distinct from Dependabot: the 9 November authenticated
zero-alert observation is historical, and a failed API never establishes zero
current alerts. Research-provider transitive freshness, broader Windows tools,
end-user installation/rollback and live AMD runtime remain separate gates.
