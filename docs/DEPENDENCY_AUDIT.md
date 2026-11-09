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

This checkpoint does NOT claim every tool/transitive package is upgraded:
official latest CMake v4.4.5 and LLVM 23.1.5 are newer than current build tools.
The reviewed isolated research provider still uses pinned Python 3.13 and locked
dependencies, requiring source/lock compatibility review before upgrade.
Linux codec jobs that discover distribution packages need latest-source lanes.
Compiler/runtime, CMake and provider migrations must run CPU/raw/codec,
sanitizer, Windows and offline HIP checks before being called compatible.
Full dependency inventory and transitive tooling qualification remain open.

The shared CocoIndex registration belongs to another repository and must not be
used or altered. This audit reads exact known build/workflow/dependency scopes;
restore an isolated off-host provider before new broad/fuzzy repository searches.
