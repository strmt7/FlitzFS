# Explicit native-wave LZ4 offline variant (experimental)

This is an opt-in compiler composition, not a filesystem codec or runtime route.
The original pinned checkout, byte-identical candidate gate, normal CPU operations,
worker transport and every existing source/resource/SDK gate remain intact.

## Closed change and source basis

The only admitted upstream header is src/LZ4Kernels.cuh at hipCOMP-core
03df9f7369211ee15d370817165224c73fdc9a46:34435 canonical bytes, SHA256
11db6cb48273de0d105c3131795ab958f0c99760a47e6d26093b9d897490a112.
Unknown bytes, alternate newlines, BOMs and ambiguous/missing anchors reject.

The private transform replaces only the empty AMD wave synchronization macros,
using native __syncwarp and an explicit unsigned-long-long mask conversion.
It refuses a disabled/non-AMD builtin context and never adds a block barrier.
The selected SDK's exact header/API/type availability was qualified at af2f714;
its physical-input stability and original SDK/worker gates passed at 2fdd01c.
These are availability/object facts, not proof of every memory-order property.

The second closed change rejects a future hash offset before convertIdx and
rejects a non-backward converted position before data access. With assertions
disabled, the original unsigned conversion of offset 2 at position 1 yields
4294901762; its unsigned distance 65535 can pass the old distance-only check.
This is an invalid/corrupted-table counterexample that violates old preconditions,
not a demonstrated reachable GPU race or the cause of an observed owner failure.
The new checks preserve valid admitted backward references while making those
invalid positions a no-match result. Actual source functions require CPU proof.

The source review must retain this compression geometry: one hardware wave per
chunk, correct reported/compiled wave width, independent per-chunk scratch and
uniform outer progress. Partial-match participants must all execute with the
same valid nonzero mask. Native wave calls are appropriate to those branches;
block barriers inside partial-lane calls are not. Actual composition, emitted
behavior, codec output and runtime containment remain separate gates.

## Private materialization and bounds

Only after original canonical staging, inside its freshly created private
fixture, the helper verifies the single canonical header record and bytes.
It creates a NEW retained canonical copy and checks it before removing that
private include copy and exclusively creating the transformed file. It never
edits the upstream checkout, overwrites an existing destination or touches a
user file. Collision/budget failures preserve the original private header.
A later failure retains the canonical backup and partial fixture for VM disposal.

The complete 16MiB source limit accounts for the retained header, original alias,
complete notices and 68KiB of bounded oracle-source allowance. Existing 1MiB source,
16MiB object/1MiB dependency, 2GiB command, 900-second compiler, 1200-second wrapper
and 30-second reader limits remain. Native mode additionally requires a 4GiB
free-space reserve with 64MiB staging margin. No GPU object is loaded or linked. Only the bounded CPU oracle executable runs;
neither binary is linked into a product or exported.

## CPU index oracle and qualification

Seven CPU cases exercise the exact pinned transform, altered/unknown source,
missing/duplicate anchors, pre-read guard placement, unchanged block-barrier
count, canonical retention, manifest digests and collision/budget refusal.
They run with the explicit pinned upstream checkout, never an arbitrary header.

A separate bounded MSVC CPU executable extracts only the exact reviewed unsigned
conversion and patched hash-validation functions, retaining their full license
prefix. Its test-only NDEBUG reveals original release arithmetic without
dereferencing an invalid old pointer; this does not disable kernel assertions.
The real patched function must reject future/current/null entries before a
read hook and preserve actual valid reads over 65523 admitted positions. Expected
results are 65523 old future admissions, 131046 valid reads and196569 invalid
rejections. These are pending CPU index observations, not GPU synchronization,
full codec emulation or GPU/CPU interoperability.

The manual SDK workflow requires BOTH codec_candidate and native_sync_candidate.
It first retains the original candidate qualification and exact native API probe,
then executes the seven cases, actual CPU oracle and separate native-wave
candidate compilation/strict object inspection. Every old SDK/worker test remains.
A failed or unavailable prerequisite terminates qualification; no fallback removes
a guard. Both flags default false, and normal builds/file operations never invoke it.

This publishing source requires its own executed qualification. No mounted or
owner GPU experiment follows from compilation. Independent verification of actual
GPU output, full kernel review, lifecycle/quarantine, fresh resource admission and
measured complete-operation benefit remain required. D01 remains open.

The transformed include, retained canonical copy, entry alias and complete notices
must match their recorded physical bytes before and after compilation. Native
mode also requires the dependency output to name the actual selected SDK header.
