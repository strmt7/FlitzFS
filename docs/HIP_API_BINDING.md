# SDK-derived runtime bindings

The worker's function pointer types and loader names now come from one private,
SDK-compiled table. Two-stage string expansion retains the selected header's
actual ABI suffix: hipGetDeviceProperties becomes hipGetDevicePropertiesR0600.
The worker obtains the eight-byte Windows LUID and node mask from the matching
hipDeviceProp_t and checks both against the requested DXGI identity before
selecting a device or creating a stream. Missing exports remain a typed
runtime-unavailable failure; they do not trigger another device search.

## Evidence and qualification

On 11 November 2026, read-only PE inspection of one installed signed AMD64
amdhip64_7.dll found 644 named exports and no hipDeviceGetLuid. Its SHA-256 was
7fc6465b18ff337a80c863b6350d02d7576cfb30528f97b982a6e5723bbb9848.
No module was loaded or GPU initialized by that inspection. A declaration in
a newer SDK header therefore cannot establish that an installed runtime
exports the same function. This observation is not a universal driver rule
or permission to replace the owner's driver.

The selected distribution's HIP GITHASH is
7da43026e4f2ef7600433c38c820732f7a205d22. Its
[official header](https://github.com/ROCm/rocm-systems/blob/7da43026e4f2ef7600433c38c820732f7a205d22/projects/hip/include/hip/hip_runtime_api.h)
defines the versioned properties API and Windows LUID/node-mask fields.
The native repository API reported header blob
fc98f112c932aba62506ecf089bb7c85aa61e726 (482797 bytes).
Current FlitzZip device inspection also uses the selected SDK's properties API.
No archive codec, format or product dependency is imported by this change.

The hosted complete-SDK job reads the binding inventory printed by the actual
header probe and statically checks the selected runtime's PE function exports.
A missing, forwarded, nonexecutable or data-only entry cannot satisfy a required
function. The bounded parser validates file ranges, unique RVA mappings, name
lengths, counts and ordinals. Deterministic malformed fixtures run before SDK
provisioning. The receipt binds the inspected runtime hash, bytes, required names
and missing entries; this does not load the runtime.

A separate CPU executable binds fake typed functions and exercises missing
exports, exact identity, identity mismatches, typed property failures and
invalid priority ranges. It checks that no runtime module was loaded.
These are API and refusal tests. Actual AMD execution and speed still require
the separate supervised hardware qualification.

## Child compiler environment

The existing owner reference documents an access violation with inherited
LLVM_PATH and success with it unset using the same installed runtime.
Before bundle checks or loading HIP, the disposable worker clears LLVM_PATH
from its own process environment and confirms that it is absent or empty.
Failure refuses the worker. It does not change the parent, system or user
environment, PATH, driver settings or installed tools.

The hosted admission probe deliberately sets a compiler-path poison value in
its own process, checks a successful qualified child and verifies that the
parent value remains unchanged. Its scoped fixture restores its original
value. The existing six bounded refusal/admission cases remain mandatory.
This change retains the private authenticated bundle; it does not add a
System32 runtime selection route, automatic loading, retries or a host launch.
