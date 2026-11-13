# Offline native HIP synchronization API probe (experimental)

The selected distribution's actual source provides native wave synchronization.
ROCm/rocm-systems at 7da43026e4f2ef7600433c38c820732f7a205d22 defines __syncwarp
with wavefront release/acquire fences and a wave barrier when invocable.
Its masked overload checks participating lanes and requires a 64-bit integral
mask. A 32-bit source mask must be explicitly widened. The header enables these
builtins unless HIP_DISABLE_WARP_SYNC_BUILTINS is defined.
[Exact source identities and review](evidence/hip-warp-source-ab37c4a.json).

The pinned hipCOMP fallback instead uses shared values inside partial-lane
warpMatchAny calls, with empty AMD synchronization macros. A block-wide barrier
inside those branches could deadlock. Do not patch the vendor kernel from API
names or a documentation-page search. The existing byte-identical offline
candidate gate and every runtime path remain unchanged.

## New compile-only gate

tests/rocm_compile/warp_sync_probe.hip is a separate own-source asset. The SDK
qualifier verifies its canonical Git blob, scoped cleanliness, bounded physical
bytes and exact SHA256 before SDK bootstrap. Only exact or whole CRLF checkout
forms are admitted; the fresh compiler input uses canonical bytes. The probe
rejects disabled builtins, uses explicit 64-bit masks, and demonstrates typed
32/64-bit source masks and partial-wave calls. It has no loader or launch caller.

After the existing hash-locked whole-SDK bootstrap, the helper binds the actual
SDK header to the reviewed source's exact canonical digest. File/ancestor/type/
single-link/size checks reject escaped or reparse inputs. The current compiler
mechanism emits one synthetic gfx1100 object under the original 2-GiB/900-second
process-tree budget. This target is not an owner-hardware assumption.

The dependency reader binds the FULL selected SDK header path, including the
actual helper's Make escapes for spaces, hashes, dollars and continuations.
Another installed header or a similarly named file cannot satisfy that check.
Object/dependency acceptance caps stay 16 MiB/1 MiB. Verified free storage must retain
4 GiB, with 20 MiB additional precompile headroom. Native reader metadata comes only
from the same selected SDK, with 30-second commands and unchanged output digests.

The existing strict parser requires exactly an empty AMD64 host placeholder plus
one positive gfx1100 bundle within the actual object extents. Reader warnings or
truncation reject. The probe object is never loaded, linked into a product or
exported; only identities, bounded command/metadata outcomes and digests remain.
Every original SDK/filesystem-kernel/worker qualification gate, historical/current
priority oracle and independent CPU refusal remains required.

## Meaning and remaining work

Nine CPU cases cover the real source asset, exact/CRLF identities, tampering and
bounds, regular/foreign/missing/reparse inputs, ancestor limits and full dependency
binding with platform escaping. They run only in the explicit hosted SDK lane,
with no HIP load, driver installation, mount or GPU execution.

This publishing source requires actual CPU and SDK execution. Even a successful
probe proves only API/type availability and offline object identity. It does not
prove emitted fence instructions, full kernel synchronization, output format,
safe runtime execution, preemption, background interference or useful compression.
Kernel changes need a separately audited composition, independent actual-output
CPU verification, lifecycle/fallback/resource gates and measured complete-operation
benefit. Current GPU/owner gates and the unresolved historical D01 cause remain.
