# Windows app, filesystem engine and driver architecture

Research date: 9 November 2026. Windows-only AMD64 product, latest stable
Windows 11. This records current evidence and engineering decisions; it is not
a claim that the app, writable volume or live HIP worker is complete.

## How non-native Windows filesystems fit

A user-mode filesystem can still present a real Windows filesystem volume:
the kernel-facing driver receives Windows I/O and forwards the operations that
need filesystem-specific work to a user-mode engine. WinFsp's current design
documents that boundary and the kernel caching/request queues. This lets FlitzFS
own its namespace, compressed extent format, transactions and recovery without
putting compression, learning or AMD driver calls into its own kernel code.
The existing native WinFsp callback work is therefore a useful foundation.

Source: [current WinFsp design](https://winfsp.dev/doc/WinFsp-Design/).
Native capture at 8c6dd91604260db7ddeb435a7656f1225739fd33: HTTP 200,
markdown SHA-256 155b76c1f41a65d63558341f8ccf135549ef8d5de8eb786e043c6e7726590dc8.

| Approach | Role to evaluate | FlitzFS decision |
|---|---|---|
| WinFsp native API plus its FSD | Windows kernel boundary with a user-mode filesystem engine | Continue the existing foundation, subject to current driver security/client qualification |
| Dokan | Another maintained user-mode filesystem/driver framework | Relevant comparator and design reference; changing frameworks needs actual correctness/performance evidence |
| WinBtrfs-style direct FSD | Implements filesystem behavior in a Windows kernel driver | Useful Windows integration/failure reference; not a reason to move codecs/HIP into the kernel |
| ProjFS | User-mode projection of hierarchical backing data into Windows files | Evaluate its projection semantics rather than assume it supplies the new storage/transaction engine |
| Cloud Files | Placeholder hydration/synchronization; current Microsoft documentation describes cldflt.sys on NTFS | Not the selected native compressed-volume engine; hydration is different from the required durable compressed storage |

Current maintainer references captured through the native reviewed provider:
[Dokan](https://dokan-dev.github.io/),
[WinBtrfs](https://github.com/maharmstone/btrfs),
[ProjFS](https://learn.microsoft.com/en-us/windows/win32/projfs/projected-file-system),
[Cloud Files](https://learn.microsoft.com/en-us/windows/win32/cfapi/build-a-cloud-file-sync-engine).
No vendor throughput graph establishes that one approach is faster for FlitzFS.

Current native API inventory observed WinFsp stable v2.1, Dokan stable
v2.3.1.1000 and WinBtrfs v1.10. The same review found a newer WinFsp v2.2B5
prerelease. Current main source identities were WinFsp
4425d4807aea4626affb151e38c1cc774ccd5e75, Dokan
49ae5ed3628806878408995916d772b60241e083 and WinBtrfs
3fa5cf21dde2913d58d7b74e150625e18bd490bd. Main is a source reference, not an
automatic binary/dependency selection. Stable and prerelease are distinct.

## Required Windows process boundaries

The helper app is the normal Windows control and status interface. Closing or
crashing that UI must not destroy a mounted volume's transaction state. A
separate per-volume engine owns the writable image lease and handles Windows
callbacks. It independently CPU-verifies storage, publishes durable checkpoints,
controls recovery and fences uncertain writes. A narrow authenticated controller
owns lifecycle operations and optional privileged provisioning. No GUI or GPU
result can acknowledge durable data ahead of the storage protocol.

The optional AMD worker is separately supervised and does not own volume handles,
mount control, storage publication or recovery. Foreground CPU selection remains
independent of its teardown. Background learning/compression/relocation is
advisory and uses explicit shared resource budgets, very low priority, fresh
headroom and foreground yielding. Driver failure cannot be "repaired" by a timer
or repeated runtime discovery.

WinFsp documents a service/launcher architecture that separates service instances
from GUI sessions and supports ACL-controlled lifecycle commands. Reuse that
mechanism where its current APIs fit; do not copy the documentation's permissive
Everyone-control example. Define least-privilege control ACLs, bounded commands,
process/image identity and clean cancellation. Product service implementation
and its installer are still pending.

Source: [current WinFsp service architecture](https://winfsp.dev/doc/WinFsp-Service-Architecture/).
Native capture SHA-256 d2fba7930b33e746f93757e33e1aa341406aeb74cf1e929a40acec8671171167.

## Driver dependency decision

The old latest-stable-only audit missed a relevant development channel.
[WinFsp v2.2B5](https://github.com/winfsp/winfsp/releases/tag/v2.2B5),
published 13 October 2026, lists security fixes including CVE-2026-3006 and
CVE-2026-7162, plus cached-write/memory-pressure, concurrent-rename and forced-
termination deadlock fixes. These are upstream release claims, not an independent
vulnerability/reliability attestation. A "latest stable" label or zero repository
Dependabot alerts cannot prove that an independently downloaded driver has no
important upstream fixes.

Select 2.2B5 as an explicit HOSTED DEVELOPMENT CANDIDATE, preserve its prerelease
status and qualify it before promotion. Its source tag resolves to
14d6e0280ae15e2550c44d6dbc476be51649727f. Native release metadata and the
maintainer's published checksum agree on the 2,207,744-byte MSI and SHA-256
293ffb9bcc0b625c85079fe85fa77f8fe98170aba51a02ac9c83d58f6c3b0e2f.
tools/winfsp-sdk-lock.json binds the package/source/channel. Both SDK staging
and disposable-VM installation consume that same lock; neither falls back to
2.1. The dependency audit checks both stable and prerelease inventory and the
actual selected asset/source identity.

The selected source Product.wxs confirms the existing four public SDK component
identities and distinguishes them from similarly named optional fsext libraries.
Extraction remains non-installing, bounded and whole-MSI verified. Only the
separate opt-in disposable mount job may install the candidate, after CPU/native
checks. Capture exact staged payload hashes and test results. No production
package, owner installation, new FlitzFS product number or release is created.

## Windows 11 release and security gates

Use the current official
[Windows release information](https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information)
and [SDK inventory](https://learn.microsoft.com/en-us/windows/apps/windows-sdk/downloads)
to resolve the target. Their native captures succeeded; do not select an OS
release by comparing SDK or kernel build numbers alone. Windows Server CI is
development evidence, not latest Windows 11 client qualification.

The current
[Windows Driver Policy](https://support.microsoft.com/en-us/windows/hardware/drivers/the-windows-driver-policy)
distinguishes audit and enforcement, WHCP-signed drivers and allowed legacy
cross-signed drivers. Driver acceptance must be verified with the current
Windows 11 security configuration, not inferred from a clean short-lived VM.
Do not disable Secure Boot/HVCI, remove Code Integrity policies or enable test
signing to make FlitzFS appear compatible. If a package is blocked, preserve
CPU recovery and diagnose the real signing/policy issue.

A custom kernel FSD remains a possible later architecture only if measured
requirements justify it and the full signing, security, memory, caching, I/O
cancellation, locking and fault qualification can be met. Replacing the kernel
transport does not by itself improve compression.

## Windows correctness and performance gates

Build an explicit capability matrix and exercise actual Windows applications:
Create/Open access and sharing, stable IDs, enumeration/notifications, rename/
replace/delete disposition, attributes/timestamps, security descriptors,
alternate streams/reparse/hard links when advertised, byte locks/oplocks,
cached/noncached/paging I/O, file mappings, flush/write-through and out-of-space.
Reject unsupported operations accurately. Metadata and storage acknowledgements
must retain their crash-recovery obligations through every callback path.

Use upstream Windows filesystem correctness tools and independent byte oracles
as well as Explorer and representative application workloads. WinFsp's current
[testing document](https://winfsp.dev/doc/WinFsp-Testing/) describes compatibility,
stress, lifecycle and Driver Verifier work. Its test success does not qualify a
new FlitzFS engine or its writable callbacks. All tests remain on disposable VMs.

Later compare native NTFS, Windows compression and relevant third-party
filesystems under equivalent workloads, durability, cache state and resource
limits. Record logical bytes, actual physical allocation including metadata/
history, read/write amplification, sustained throughput, CPU/RAM/GPU use and
recovery outcomes. Separate driver transport, compression, transfer and storage
costs. No RAM-only timing can prove physical persistence.

## Evidence limitations and next implementation

The 8c6dd9 research job accepted 11 of 13 requested pages, preserving native
bodies/status/digests. Two URL candidates returned real 404s: the guessed
Microsoft FSD-guide slug and WinFsp-Compatibility slug. The latter was corrected
using the successful current design page's actual NTFS-Compatibility link;
Microsoft's published file-system/driver reference is requested normally.
The selected HIP stream-management page succeeded independently; the earlier
challenged Doxygen URL remains a rejected capture. The failed group/workflow
must not be represented as fully successful.

The next research run prints substantive body sections, not only navigation,
and must verify the corrected sources. Product work must then connect the native
transactional namespace to Windows callbacks, implement the separate helper/
engine lifecycle and test the new driver candidate. Full Windows 11 client,
writable application, driver-fault and AMD hardware qualification remain open.


## Current-source review update

At 2c44ce969e8557f6eaa81027b96718ddc9a6aa76, job 129405052541
successfully captured all TWELVE Windows product/framework/alternative pages
with substantive body review. Both corrected URLs returned HTTP 200. The
unrelated HIP stream-management request then received an access challenge and
kept the combined job red; do not describe that workflow as passing. Normal
Windows architecture research is now scoped to its three complete Windows
groups. The HIP success/failure history remains in HIP_RUNTIME_WORKER.md and
does not authorize bypassing the challenged endpoint.

The release-information body lists Windows 11 26H2 as the current annual GA
release (29 September 2026), with build 26300.9818 dated 28 October in its
captured latest-update column. It separately describes 26H1 as a new-device
platform, not a normal in-place feature update. The SDK page lists
10.0.28000.2957 and the 26100-family update 10.0.26100.9457, both September
2026, ahead of a separate Preview section. These are observed source values,
not assertions that our runner uses these SDKs or that a higher SDK/build
number defines the appropriate Windows client release.

The current-served ProjFS overview reports a 2021 content revision. Cloud Files
reports a 2025 revision. Their successful fetches prove extraction, not fresh
release-specific behavior. Treat their table entries as background categories;
no important implementation or Windows 11 compatibility decision may rely on
those old revisions. Current SDK/source and actual Windows 11 behavior would
need independent verification before selecting either mechanism. The present
WinFsp continuation is bound to current 2026 package/source review and executed
native tests, with its Windows 11 client gate still open.

The current NTFS-Compatibility document confirms that user-mode engines can
implement their own compression/encryption, while WinFsp does not expose the
corresponding NTFS-specific compression/encryption control codes. It also lists
unsupported interfaces such as Change Journal, quotas and defragmentation
controls, hard links, paging-file support and open-by-file-ID. The selected
v2.2B5 header was inspected through the native repository API at its exact
source commit and exposes no hard-link callback. These are real compatibility
constraints: the current driver is a bring-up foundation, not a justification
to redefine required Windows application behavior. If application requirements
cannot be met, qualify a suitable extension or replacement driver boundary.
This does NOT prevent FlitzFS-native extent compression or internal
layout maintenance. It requires honest advertised Windows capabilities and
tests, rather than claiming complete NTFS equivalence. Source:
https://winfsp.dev/doc/NTFS-Compatibility/

The VM-only installer now captures the exact MSI-selected AMD64 driver bytes,
file version, Authenticode status/type and signer identity before installation.
The official MSI checksum binds that source. The record explicitly keeps
windows11_policy_qualified=false: signature metadata and Server installation
cannot establish current Windows 11 enforcement/HVCI compatibility. Only fresh
owned-fixture JSON provenance is uploaded; no driver binary is redistributed.


## Executed candidate checks

At 2c44ce969e8557f6eaa81027b96718ddc9a6aa76, job 129405058165
staged the actual v2.2B5 SDK and passed 27 native CTest entries, including 529
CPU-only worker assertions with handles 88 to 88. Job 129406895390 installed
that same source/asset-bound candidate on a disposable hosted VM and passed real
Zstd reads, EOF, random/mapped I/O, enumeration, write rejection, shutdown,
backing parity, forced live-mount termination, automatic removal, independent
CPU recovery and same-path remount. Observed process peaks were 9,854,976 and
9,846,784 committed bytes under the existing 128 MiB/20% bounds. The crash probe
deliberately timed out and confirmed termination; it was not accepted as a normal
successful process exit.

These are read-only Windows Server integration checks, not writable application,
Windows 11 client or kernel-fault immunity results. Dedicated Windows native/
mount and interruption development jobs now move to the current hosted
Windows-2025-VS2026 toolchain already observed in whole-ROCm qualification.
This removes reliance on the older Windows Server 2022 development image without
claiming that a Server image is a Windows 11 client VM.

## Persistent native metadata continuation

The next writable-adapter prerequisite is source-bound Windows timestamps,
attributes, allocation and security metadata. See
[WINDOWS_NATIVE_METADATA.md](WINDOWS_NATIVE_METADATA.md) for the inspected
selected FSD/user-mode contracts, twelve-page hosted research scope and strict
remaining implementation gates. The existing transaction sequence fields are
not wall-clock timestamps. A configured research campaign is not an accepted
capture or a qualified writable Windows implementation.
