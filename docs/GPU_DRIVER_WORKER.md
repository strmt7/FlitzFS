# Explicit installed-driver HIP worker

This experimental route is explicit. Normal filesystem operations remain CPU-only.
The same supervised worker can use either its authenticated private SDK bundle or
a kernel-only bundle plus the installed driver runtime. There is no driver
installation, service, device enumeration, automatic retry or mounted default.

## Identity and ownership

The driver route requires an explicit ordinal, eight-byte LUID, single-node mask
and expected runtime SHA-256. Both --driver-adapter and --driver-match-adapter add
the digest before the existing worker-channel marker. Malformed arguments and wire
input fail before system-runtime or adapter access.

Kernel-only verification admits exactly the compiled filesystem GPU kernel DLL
with its expected size and hash. The complete private SDK bundle is refused in
this mode. Directory/file pins remain held. The selected Windows system-directory
runtime is verified through VerifiedSystemModule: exact expected bytes, a 32 MiB
file ceiling and cached-only, no-UI trust.

The runtime pin remains alive while its absolute path is loaded with restricted
search flags and through kernel execution and cleanup. The loader checks the
actual module paths after loading both modules. Kernel imports must be limited to
the SDK runtime basename and kernel32.dll. The qualifying artifact therefore
contains no AMD runtime or rocm_kpack.dll. These are user-mode GPU modules; this
route does not install or load a filesystem kernel driver.

The existing child resource owner, matched DXGI/HIP identity, lowest stream
priority, explicit allocation bounds, independent CPU checks and typed failures
remain in force. Both modules close before response publication. The parent
still requires exact child exit before copying or accepting a reply.

## Bounded diagnostics

The diagnostic supports internal metadata and installed-driver modes:

- --describe-amd-adapters collects bounded DXGI identities without HIP calls.

- --verify-driver-runtime performs only metadata verification in its child.
- --run-amd-driver-once uses the existing 12 KiB synthetic decode fixture.
- --run-amd-driver-match-once uses a 4 KiB repeated synthetic source and validates
  the source-bound match reply independently.

All require the exact qualified worker path and explicit expected runtime hash.
Compute modes additionally require an explicit ordinal and LUID. No mode searches
for another device or retries. Successful metadata is not hardware qualification.

The diagnostic bounds its own process and descendants: 128 MiB per process,
256 MiB aggregate, two processes, 20 percent CPU, below-normal priority and a
ten-second per-process CPU-time ceiling. It requires 768 MiB physical and commit
headroom before admission. Its child retains the exact 128 MiB/20 percent/five-
second job, kill-on-close lifecycle and existing termination observation.
Closing the parent's budget handle does not terminate the reporting parent;
the child has its own kill-on-close job. OS calls and kernel faults remain
outside a no-freeze guarantee.

## Qualification and artifact

The hosted SDK job preserves all nine prior metadata/private-worker cases.
It additionally checks correct kernel-only admission, unexpected private files,
a same-size modified kernel, malformed decode/match input, unavailable or
mismatched system runtime and malformed driver digest. Seventeen child exits
must be confirmed. Separate metadata/decode/match diagnostics must refuse the
unqualified runtime with no candidate publication under their self-imposed caps.

The experimental artifact contains only the worker, filesystem GPU kernel,
diagnostic, source-bound manifest and license files. Expanded and compressed
sizes are capped at 8 MiB. It is not a release or permission to execute on the
owner's PC. Exact matching VM receipts, source/import review, current headroom
and the owner-device gate remain necessary before staging and execution.

These VM tests never execute an AMD GPU. Actual hardware results, real-data
compression and speed require separate recorded experiments.

Adapter identity discovery and its DXGI/HIP ordering distinction are described
in ADAPTER_IDENTITY.md. Dedicated capacity alone never admits a compute request.

## Failure-stage diagnostics

The six legacy exit values remain unchanged. A failed worker may additionally
encode one of fifteen bounded stages in bits 8 through 15 of its exit code.
The parent normalizes only a recognized category/stage combination after a
confirmed failed exit from the trusted HIP route. Unknown details retain the
conservative device-fault classification. The reported category preserves the
existing evidence-aware recovery policy; a stage never authorizes retry.
No shared frame, candidate bytes or on-disk layout changes.

The explicit --inspect-adapter-budget LUID diagnostic does not initialize HIP.
It records the existing DXGI collector's status, last API phase, signed HRESULT,
completed snapshot fields and ordinary budget-gate result under the same process
caps. Phases are none=0, factory=1, adapter=2, description=3, local=4,
nonlocal=5 and complete=6. Snapshot fields are valid only on collector success.
The diagnostic uses the same node-mask selector 1 and planned 16,492-byte decode
budget as the tiny fixture; this is metadata inspection, not a GPU speed test.
Compute receipts additionally retain raw exit, normalized category and stage.

## Observed installed-runtime node refusal

The 9c1637a owner experiment admitted the matched DXGI budget and independently
verified the installed runtime's metadata. Its single 12 KiB diagnostic decode
returned adapter_node under the original unsupported_configuration category.
HIP initialization, the properties query and exact LUID comparison had succeeded;
the node check refused before worker device selection, stream creation, explicit
batch allocations or launch. The actual returned node value was not captured.
No candidate output was published. Do not repeat this unchanged configuration,
substitute a guessed mask or classify the refusal as GPU acceleration.

The SDK job now inventories the already staged private runtime closure's normal
and delayed PE imports and bounded license metadata. It reads bytes only and
exports no AMD binaries. This inventory does not establish driver compatibility,
license compliance for redistribution, runtime initialization or device safety.
The current AMD source groups are available through a fixed-choice isolated
research workflow; arbitrary URLs and shell fragments are not accepted.
