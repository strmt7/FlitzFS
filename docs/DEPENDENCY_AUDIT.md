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

## Native dependency and Action pins

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
