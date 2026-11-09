# Dependency and alert qualification

Owner requirement: keep dependencies, packages and tools on latest compatible
stable versions and check Dependabot alerts. All auditing/build/tool execution
runs on disposable hosted VMs; nothing is installed or run on the owner's PC.

Official native GitHub release/tag API captures on 9 November 2026 confirmed
Zstandard v1.5.7 (f8745da6ff1ad1e7bab384bd1f9d742439278e99), LZ4 v1.11.0
(c72c7c0ec132ad2ce2fe9fad7b0c728d9faee2fc), BLAKE3 1.8.7
(f3149ec5bb5449af877ba20377a11008ff499fa2) and WinFsp v2.1 as latest stable.
Windows/interruption source pins match those identities. Linux distribution
codec packages and toolchains are not thereby proved latest upstream.

The ten existing workflows now pin official checkout v7.0.1, setup-python
v7.1.0 and upload-artifact v7.0.2 to exact release commits, using Node 24.
Historical comparator/reference commits remain immutable evidence, not runtime
dependency update candidates. Do not advance a comparison baseline silently.

The dependency-audit workflow captures native release/tag identities, installed
tool versions and paginated open Dependabot alerts. It retains response bytes,
URL, HTTP status, timestamps and SHA-256 digests. Access failure, schema failure
or pagination exhaustion must not be reported as zero alerts. The connector
itself rejects the Dependabot endpoint; the hosted job separately attempts the
native API with the repository's minimal read-only workflow token. If that token
cannot read alerts, an alert-enabled read connection is still required.
No repository security setting or user credential is modified by this audit.

This checkpoint does NOT claim every tool/transitive package is upgraded:
official latest CMake v4.4.5 and LLVM 23.1.5 are newer than current build tools.
The reviewed isolated research provider still uses its pinned Python 3.13 and
locked dependencies, requiring source/lock compatibility review before upgrade.
Linux native-codec jobs that discover distribution packages need latest-source
lanes. Compiler/runtime, CMake and provider migrations must run CPU/raw/codec,
sanitizer, Windows and offline HIP checks before being called compatible.
Full dependency inventory and transitive tooling qualification remain open;
do not turn an upstream tag match into a clean vulnerability claim.

The shared CocoIndex registration belongs to another repository and must not be
used or altered. This audit reads exact known build/workflow/dependency scopes;
restore an isolated off-host provider before new broad/fuzzy repository searches.
