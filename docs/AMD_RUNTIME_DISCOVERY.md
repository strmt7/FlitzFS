# AMD runtime and adapter acquisition

FlitzFS has a Windows x64 control-plane API for acquiring a verified AMD
runtime and device identity. Its input is a trusted package's helper path,
SHA-256 and compiled HIP runtime basename, together with a process budget.
The acquisition caller runs outside ordinary filesystem I/O.

1. Check the request and open the helper through pinned ordinary local
   directories. Verify the helper's exact bytes before opening the runtime.
2. Open the named runtime from the operating system's System32 directory.
   Hold the file open against replacement, check cached Authenticode trust,
   and take its SHA-256. The parent performs no HIP load.
3. Find one hardware AMD adapter in DXGI and corroborate one D3D node.
   Require fresh Windows video-memory and host-memory headroom. Multiple
   AMD adapter candidates currently produce an explicit refusal.
4. Ask the bounded worker for the HIP ordinal's LUID and node mask. Compare
   its actual LUID with DXGI. A zero HIP node mask is admissible when D3D
   independently proves one node. A conflicting mask refuses the route.
5. Require confirmed worker exit and fresh headroom before returning a route.
   Query at most the caller's bounded ordinal count under one exchange
   deadline. After an abnormal exchange, return without another probe.

The resulting key binds the helper and runtime bytes, LUID, node and ordinal
for this handoff. Keep the pins alive through the intended handoff and release
them afterward. A durable performance profile needs its own driver, OS,
method and workload identity. The application currently uses CPU for
ordinary writes. Automatic GPU work requires qualified performance evidence
and integration with the recovery policy.

The optional `FLITZFS_BUILD_HIP_ROUTE_PROBE` target provides a file-only
fingerprint and a separately admitted metadata fixture. The Windows native
test lane builds the target and exercises eight pre-discovery refusals without
requiring AMD hardware. The metadata worker initializes the HIP runtime for
identity lookup. It loads no FlitzFS kernel.

AMD documents HIP 7.16.0 as the component shipped with ROCm Core 10.1.0.
The observed Windows runtime DLL had file version 10.0.3683.0. Package,
component and installed-file versions are distinct. [AMD release notes](https://rocm.docs.amd.com/en/docs-10.1.0/about/release-notes.html#hip-7-16-0)

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
