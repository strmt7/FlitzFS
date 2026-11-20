# AMD runtime and adapter acquisition

FlitzFS acquires AMD routes from its Windows control plane, outside filesystem
I/O. A package-enabled build verifies an approved AMD manifest, the selected
workers and their compute modules. The running executable resolves the package
relative to its own directory. An approved profile supplies the limits for
automatic shared-base acquisition and execution.

The build accepts `FLITZFS_INSTALLED_AMD_PACKAGE_DIR` and an independently
approved `FLITZFS_INSTALLED_AMD_MANIFEST_SHA256`. The package layout places
`flitzfs_hip_lz4_worker.exe` and its `hip-uniform-lz4` directory in the
`amd` directory beside the executable. The compiled header binds file
digests and the exact HIP ABI basename. For shared-base work it binds
`flitzfs_hip_worker.exe` and `hip/flitzfs_rocm_kernels.dll` under the same
`amd` directory. The package can contain one route or both. Missing or
altered artifacts return before launching a worker. The explicit helper path
API supports diagnostics. [Executable path lookup](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulefilenamew)

### Shared-base package profile

The manifest's `shared_base_profile` binds an approved qualification receipt
to the signed runtime SHA-256 and the adapter's vendor, device, subsystem and
revision identifiers. The approval covers a specific measured member range
and launch budget. Acquisition checks the runtime and adapter fields before
the first HIP identity query. The separately observed LUID and node govern live
allocation and release. [DXGI adapter identifiers](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_adapter_desc1)

| Fields | Qualification |
| --- | --- |
| `qualification_sha256` | Exact `shared-base-qualification.json` receipt included in the verified inventory |
| `runtime_sha256`, `vendor_id`, `device_id`, `subsystem_id`, `revision` | Runtime and hardware used for the approved measurement |
| `minimum_members`, `maximum_members` | Combined non-base extents across groups covered by the measured work range |
| `worker_memory_mib`, `memory_share_divisor`, `worker_cpu_rate`, `worker_timeout_ms` | Measured fixed child allowance and resource limits. Current geometry adds actual staging and transport charges. |
| `maximum_identity_queries`, `duty_ppm` | Input-work and worker pacing limits |
| `search_mode` | Optional approved mode. `copy` is the default. `reference-xor` lets the worker compare shared references and return sparse XOR candidates in the same exchange. |
| `dispatches_per_wait` | Optional qualified copy-search queue limit from 1 to 4. Live transfer geometry can lower it. Reference search uses 1. |
| `maximum_dispatch_members` | Measured dispatch ceiling within the approved batch size. Copy profiles with a ceiling above three bind the version 11 window API. Copy profiles without this field use version 2. Reference profiles require an explicit ceiling. |

Reference profiles bind the base matching entry, the version 8 sparse arena,
version 10 reference pricing and the copy-search entry for the approved
dispatch width. The planner can price multiple references when their entire
working set and additional launches fit. It can still use copy search on batches
that do not support pricing. The native writer
compares the resulting representation with its CPU alternatives and checks
the reconstructed bytes before publishing.

The pinned manifest authenticates the approval and its exact artifacts.
Qualification must establish useful size or throughput benefit for the
recorded workload and exact CPU readback. After configuring the approved
package, the optional component stages its verified workers, modules and
receipts plus listed licenses at a new application directory.
[CMake install component](https://cmake.org/cmake/help/v4.5/command/install.html)

```text
cmake --install <build> --config Release --prefix <application-directory> --component FlitzFSAMD
```

The indexed Windows owner starts a low-priority acquisition when live telemetry
admits a group candidate. A later candidate can trigger another attempt after
a relevant package or topology change, or the confirmed release of an
unavailable worker transport. CPU work continues during acquisition. Stop cancels
and joins the scoped attempt. The verified route starts compute only when an
owned group batch meets the approved aggregate work range.
[Windows recovery evidence policy](GPU_RECOVERY.md#installed-windows-routes)
[Owned controller](WINFSP_OWNED_CONTROLLER.md#amd-shared-base-search)

The relocated SDK fixture made one installed metadata query and then used
AMD advice for four extents. Separate missing- and altered-module fixtures
published through CPU without a metadata or compute launch. The runtime
digest and adapter mismatch checks refused before runtime initialization.
The qualification profile and artifacts are bound to the tested hardware and
workload. [Installed shared-base evidence](evidence/installed-amd-shared-base-2026-11-18.json)

The relocated reference-profile fixture made one metadata query, verified
one device reference search and published advice for 18 members. The
package and CPU-only controls reopened all 23 files exactly and each
produced a 1 MiB compacted image. The mixed public input used copy
search when reference pricing could not be admitted. It kept the
319,488-byte image from the copy control. The Lucide pairs each occupied
196,608 bytes. [Reference and fallback evidence](evidence/amd-installed-reference-profile-2026-11-20.json)

1. Validate the request and pin ordinary local directories. For an
   installed package, verify and pin both the worker and the compute module
   before opening the runtime.
2. Open the named runtime from the operating system's System32 directory.
   Hold the file open against replacement, check cached Authenticode trust,
   and take its SHA-256. The parent performs no HIP load.
3. Enumerate hardware AMD adapters within the acquisition deadline.
   Use dedicated memory capacity to order the initial topology and budget
   checks. Use current Windows resource availability to admit the probe.
4. Ask the worker for each bounded HIP ordinal's LUID and node mask until
   an eligible physical adapter matches. A zero HIP node mask is admissible
   when D3D independently proves one node on that exact adapter. Skip a
   conflicting mask or an adapter that fails the topology check.
5. Require confirmed worker exit and fresh headroom before returning a route.
   Query at most the caller's bounded ordinal count under one exchange
   deadline. After an abnormal exchange, return without another probe.

The route key binds the worker, compute module and runtime digests to
the adapter LUID, node and HIP ordinal. Its sealed token retains the file
pins for the controller. A workload recommendation also needs matched
write observations and independently qualified launch costs. Each CPU/AMD
pair uses identical input bytes and independently verifies the stored
result. Different pairs may use different inputs. Admission uses storage
options and complete aligned request size, so ordinary writes need no
benchmark content hash.

The performance decision keeps CPU when GPU adds cost without an observed
benefit. The storage engine still verifies and owns all publication.
Persistent representative calibration and integration with the ordinary
mount launcher remain release work.
[AMD HIP runtime device identification](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/hip_runtime_api/modules/initialization_and_version.html)

The optional `FLITZFS_BUILD_HIP_ROUTE_PROBE` target provides a file-only
fingerprint and a separately admitted metadata fixture. The Windows native
test lane builds the target and exercises eight pre-discovery refusals without
requiring AMD hardware. The metadata worker initializes the HIP runtime for
identity lookup. It loads no FlitzFS kernel.

AMD documents HIP 7.16.0 as the component shipped with ROCm Core 10.1.0.
The observed Windows runtime DLL had file version 10.0.3683.0. AMD's public Windows SDK guide identifies its package as HIP SDK for Windows
7.2.0. Use the installed runtime's exact identity for qualification, with the
Core and Windows release notes for supported combinations.
[AMD Core release notes](https://rocm.docs.amd.com/en/docs-10.1.0/about/release-notes.html#hip-7-16-0) ·
[AMD Windows SDK release notes](https://rocm.docs.amd.com/projects/install-on-windows/en/latest/about/releasenotes.html)

## Matched installed-package writes — 18 November 2026

A relocated test executable discovered its sibling package without worker
path or digest arguments. One bounded identity query acquired the installed
adapter. The absent-package and changed-module fixtures returned before
device discovery. The hardware run matched one adapter. A system with
multiple AMD devices still needs coverage.

| Pair order | CPU callback | AMD callback | Occupied image on each path |
| --- | ---: | ---: | ---: |
| CPU, AMD | 1.214 ms | 1,331.102 ms | 1,835,008 bytes |
| AMD, CPU | 1.601 ms | 956.330 ms | 1,835,008 bytes |
| CPU, AMD | 1.265 ms | 703.106 ms | 1,835,008 bytes |

Each 512 KiB write used the indexed memory image and the fast foreground
codec policy. Timing ended when the synchronous call returned or the
deferred response reached the sink. Each GPU write verified eight candidate
extents and sixteen mirrored records. Independent CPU reopening verified
both writes in every trial, and the scoped workers retired. The policy
selected CPU for this workload. Broader corpora, longer work windows and
mounted operation need separate measurements.
[Package, build and execution evidence](evidence/installed-amd-routing-2026-11-18.json)

## Indexed write on 18 November 2026

The controlled indexed WinFsp SDK callback used one fresh AMD adapter
identity acquisition, one 128 KiB HIP request and one write response.
The two verified LZ4 bodies totaled 16,918 encoded bytes. All four mirrored
records contained the exact worker bodies with the indexed prefixes.
Independent CPU reopen recovered the source and the retained prior snapshot.
The selected image occupied 1,310,720 bytes. Callback completion took
1,017.5 ms in this one sample, which had no matched CPU timing. The parent
kept HIP unloaded and all owned processes retired.
[Current-source evidence](evidence/indexed-lz4-deferred-2026-11-18.json)

## Controller-owned budget observations

A configured Windows volume can pass qualified accelerator options to its
WinFsp controller. They bind the adapter and node, the initial acquired
observation, and the helper and runtime pins retained by the service. The
configuration still requires a qualified workload and cost envelope.

An eligible write with stale resources commits through the synchronous CPU
path and posts one coalesced budget request. The controller queries DXGI
outside the filesystem lock. A later eligible request can use the fresh
sample. Small writes, idle volumes and reads create no GPU sampling demand.
After verified worker output, the service asks for exactly one sample newer
than its independent CPU comparison. The controller binds that sample to
the same demand, adapter and node before publication can select GPU advice.
An unavailable or wrong adapter disables that binding and selects the owned
CPU input. Later recovery requires newly qualified evidence and ownership.

The configured indexed SDK hardware fixture produced a normal CPU write and
then one GPU-assisted write with two controller budget queries and exact
independent CPU reopen. The 512 KiB sweep used one bounded worker with two
extents per quantum. Eight accepted extents matched sixteen mirrored records. It used a pinned helper, an explicit
fixture cost envelope and injected lifecycle/response functions. Product
package discovery and measured automatic workload qualification remain open.
[Controller and hardware evidence](evidence/native-gpu-budget-controller-2026-11-18.json)

## Host qualification on 17 November 2026

The installed RX 9070 XT and signed runtime produced a matching identity in
one worker exchange. A separate pinned earlier build compressed a 128 KiB
fixture on that adapter. It published the independently CPU-verified
candidate, recovered byte-exact data and preserved the prior snapshot. The
CPU fixture also passed. Both images occupied 983,040 bytes. The active GPU
payload occupied 17,037 bytes versus 17,045 for the CPU fixture. The 8-byte
payload difference left the occupied image size unchanged in this experiment.

| Measurement | Time |
| --- | ---: |
| Complete worker exchange | 1,374.42 ms |
| Worker trace | 1,318.26 ms |
| Device initialization | 1,194.22 ms |
| Kernel wait | 2.57 ms |

The single small fixture isolates startup as the largest measured phase.
The earlier matched 1 MiB series also favored CPU on elapsed time and produced
identical occupied sizes. AMD documents implicit runtime initialization in HIP
APIs. The next experiment will test amortization in a bounded owned work
window. It will measure repeated complete operations, separate cold and warm
work, and retire the worker when its admitted queue ends. [Observed phases and provenance](evidence/amd-route-2026-11-17.json), [earlier matched series](evidence/owned-service-comparison-d384b6c.json), [AMD initialization API](https://rocm.docs.amd.com/projects/HIP/en/docs-10.1.0/reference/hip_runtime_api/modules/initialization_and_version.html)
