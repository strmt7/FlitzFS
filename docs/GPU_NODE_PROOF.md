# Independent Windows GPU node proof

Experimental, 11 November 2026. Normal filesystem and CLI operations remain
CPU-only. This collector is excluded from ordinary builds and linked only into
the explicitly supervised SDK worker and hardware diagnostic.

## Observed failure and correction

At 77931001b8a1238fd29f9e41081d2025ab657f23 the owner's properties-only worker
successfully initialized the installed HIP runtime and observed LUID
c573010000000000, matching DXGI exactly. The raw node mask was zero, while the
existing memory-budget selector was one. The diagnostic exited successfully in
151 ms, with 9,412,608 bytes child peak commit and 10,784,768 bytes aggregate
peak commit. It requested no FlitzFS kernel load, batch allocation or launch.
The installed runtime SHA-256 was
7fc6465b18ff337a80c863b6350d02d7576cfb30528f97b982a6e5723bbb9848.
This establishes a metadata omission, not a broken GPU, driver qualification
or successful FlitzFS GPU execution.

Compute admission accepts the ordinary exact nonzero single-bit mask match.
Only a raw HIP mask of zero paired with requested mask one may request an
independent native proof. HIP LUID equality is checked first. The proof must
identify that exact AMD hardware adapter, create a healthy D3D12 device for
that explicit adapter, and establish exactly one physical node. Node zero is
then unambiguous. No other mismatch, multi-node count, missing proof, wrong LUID,
invalid selector or failed native call is resolved. No adapter ordinal is
converted to another API's ordinal.

The collector preflights the existing matched DXGI reserve, restricts loading
to the discovered absolute System32 d3d12.dll, validates its actual module
path, uses the SDK-typed D3D12CreateDevice pointer, and supplies a nonnull
matched adapter. It verifies GetAdapterLuid, GetNodeCount and
GetDeviceRemovedReason. It releases its device reference and loader handle
before issuing a successful proof and restores its thread error mode.
No command queue, resource, copy, command list or dispatch is requested.
Device creation can enter driver code; process supervision cannot guarantee
recovery from a kernel/driver fault.

The worker retains its existing 128 MiB / 20 percent CPU / five-second limits,
fresh before/after HIP memory and residency admission, low-priority stream,
tiny bounded allocations, independent CPU comparison, cleanup and successful
confirmed exit before reply acceptance. Native topology failure produces the
existing terminal node refusal and CPU selection; it does not search devices,
retry a runtime or establish that a failing driver has recovered. Runtime,
worker/policy and OS evidence must stay bound in any future recovery identity.
No topology proof is persisted or reused across workers.

## Explicit diagnostics and VM-first qualification

--inspect-adapter-topology LUID performs this OS-only metadata query under the
parent resource job and outer watchdog. It never initializes HIP. Its report
includes native status/phase, node count, hardware flag, exact LUID match and
whether the complete single-node proof passed.

--inspect-warp-topology is restricted to disposable GitHub-hosted runners. It
selects the WARP software adapter explicitly and exercises the real Windows
device creation, identity, health and retirement path without queues or kernels.
Software results always fail the AMD single-node consent predicate. This is
native OS-path coverage, not AMD driver or GPU execution evidence.

Portable tests reject missing, incomplete, non-AMD, wrong-identity,
multi-node and failed proof records. SDK fake-API tests verify omission-only
resolution and no proof call or selection for forbidden mismatches. Complete-SDK
qualification additionally requires the real WARP path and unknown-LUID
rejection. Owner execution requires matching successful CI and exact own-binary
hashes; no AMD runtime redistribution or driver changes are authorized.

## Primary-source evidence

Current-served Microsoft documentation was captured by the isolated reviewed
Crawl4AI provider (0.9.5+flitzzip.portable2), with robots checks and bounded
timeouts, in run 42970860829 / job 130358754847. All three pages returned HTTP 200
with substantive native extraction. Documentation revision dates are older
than this observation; they are not represented as newly published research.

- [GetNodeCount](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getnodecount):
  physical nodes associated with the device; extracted Markdown SHA-256
  9c7f0c559577f91f531690c1411ec3c5a24f47a71b13e1fc9e9e81d5cfc74e9c.
- [D3D12CreateDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice):
  explicit adapter and actual device output, LUID identity and removed-device
  failure; Markdown SHA-256
  6116f6e0363d7d1ac652eaa16582c8dbadad467a7eb5d406e847405c2c8e4946.
- [Multi-adapter systems](https://learn.microsoft.com/en-us/windows/win32/direct3d12/multi-engine):
  linked physical nodes, node zero and bit-one mask; Markdown SHA-256
  a32c1ae15c4e86891fd06d82fe866fe2dc0e1ace1e7217c5f7a37ea66f1e6ffe.

Artifact 13833506726 binds the native receipts and text; ZIP SHA-256
f45e70721078e010bb6f0ce312d4c3aab74753572d03479253c517fe785879ed.
Recent typed corroboration is Microsoft's DirectX-Headers commit
61c7718954ec73dbdd43c723240fe1a0452a3724 (9 November 2026).
[include/directx/d3d12.idl](https://github.com/microsoft/DirectX-Headers/blob/61c7718954ec73dbdd43c723240fe1a0452a3724/include/directx/d3d12.idl)
was captured through the native repository API: blob
67c1a0c0480bfafeb69e2ba4ff3fbf1dafed0abf, 351778 bytes, SHA-256
466c2a0fef5692a6a62bd92cc96f0202f1eba67ffcafb6461f53f26ce3094153.
It retains the typed methods used here. No performance or universal stability
claim follows from documentation or fake API tests.
