# hipCOMP LZ4 offline object gate (experimental)

This source prepares optional compilation after the source-bound review in
[GPU_CODEC_CANDIDATES.md](GPU_CODEC_CANDIDATES.md). All 20 CPU source/staging admission cases passed on disposable Windows at
`df51d8c5ba80430453c805619438f431d5677f3b`; native candidate compilation also passed with the verified ROCm 10.1.0 SDK.
The new native object-inspection gate remains pending. No filesystem or worker invokes the
candidate, and no data encoding changes.

## Exact source and private compiler input

The source asset pins ROCm/hipCOMP-core commit
`03df9f7369211ee15d370817165224c73fdc9a46` and the actual 142-file semantic
inventory digest. The helper verifies the checkout Git root, revision and tracked
cleanliness. Git object replacement is disabled. Each source blob's length and
SHA-256 must match the admitted manifest before it is copied.

Source and public-header paths are checked for traversal, Windows device names,
case aliases and include-view collisions. Source files are at most 1 MiB, the
inventory has at most 512 entries and all private input copies, the selected HIP
entry and complete notices must fit 16 MiB. The real admitted inventory provides
136 source/include mappings. The private fixture preserves canonical public
headers and also supplies the include view required by the existing compiler
helper. The original checkout is never patched.

The selected `src/lowlevel/LZ4CompressionKernels.cu` bytes are copied unchanged
to a private `.hip` entry. This makes the compilation language explicit without
editing upstream code. Every destination is NEW and opened exclusively.
Partial failure retains only the new private fixture for diagnosis.

## Compile scope

The existing complete ROCm SDK qualifier still verifies and stages the selected
hash-locked distribution, checks its actual compiler/header identities and
executes its original filesystem-kernel/worker gates. It additionally runs the
new CPU source-admission tests.

Only the explicit workflow `codec_candidate` option supplies a candidate checkout.
The helper then uses the existing pinned compiler mechanism to emit one object
for synthetic target `gfx1100`. This target is a compilation fixture, not an
assumption about an owner's device or a support claim for other architectures.
The subprocess tree retains the existing 2-GiB command budget and bounded output;
the candidate wrapper has a 1,200-second limit and the compiler command a
900-second limit. Original SDK, source and resource checks remain required.

The candidate's CMake/build scripts are never executed. The generated object and
dependency data stay in the disposable VM, have explicit 16-MiB/1-MiB acceptance
caps, and are neither loaded nor linked into a product. Retained artifacts contain
source/SDK identities, command outcomes, output digests and full notices. Existing
own-binary diagnostic packaging does not acquire the candidate object.

## Tests and remaining gates

The new tests validate the actual manifest and private header mapping, closed
identity/schema, source counts, path/alias rejection, byte/digest/aggregate limits
and notices. Private fixture cases cover exclusive creation, input-budget refusal
before writes and retained partial copies. Real Git cases check exact roots and
revisions, dirty source, canonical blob bytes despite working-tree line endings,
rejected substitution and protection against Git replacement refs.

These are CPU source/staging tests. Passing them is not device execution, a CPU
emulation of the codec or proof of its output format. Successful object compilation
also cannot establish safe runtime behavior, CPU decoder interoperability,
compression benefit, low-priority scheduling or driver-fault recovery.

Before runtime integration, complete the relevant kernel/helper review, checked
scratch/output admission, independent CPU decoding and byte comparison, immutable
owned input and the replacement lifecycle/failure contract. Any actual device
experiment requires its own exact-source disposable-VM gates, source audit and
tiny synthetic host budget. No unchanged owner GUI/GPU repetition is needed.

## Bounded allocation preparation

[GPU_LZ4_LAYOUT.md](GPU_LZ4_LAYOUT.md) describes the new host-only allocation
planner and its pending exact-source CPU tests. It preserves the complete
256-KiB explicit device-buffer ceiling, including scratch, tables and guards.
No worker uses it yet.

## Executed compilation checkpoint

At df51d8c, the default SDK run 43418598396 and explicit candidate run
43419213104 completed successfully. The candidate object is 217,673 bytes with
SHA-256 44505b2dde92e35efac37f5fedb69a5c299384ab606283b6eb048bfe9ec33925;
165 private input records account for 1,577,371 bytes before the HIP alias and
complete notices. All 20 source/staging cases passed in both runs, and each run
preserved 20 confirmed worker exits and the original kernel/CPU refusal checks.
The compiler reported its existing unused static-runtime argument warning; no
warning was suppressed. Source-bound details and scope are in
[offline evidence](evidence/gpu-lz4-offline-df51d8c.json).

The next gate adds same-SDK LLVM header and embedded-target inspection. It checks
AMD64 host-object identity and the requested gfx1100 target as data, with new
30-second bounded commands. The object and dependency files remain unexported.
These new inspection results and allocation-planner tests are pending; successful
compilation is not represented as device execution or codec interoperability.

## Preserved object-reader failure and correction

At 0879cce, the optional run 43420782513 compiled the candidate and verified its
COFF-x86-64/IMAGE_FILE_MACHINE_AMD64 header, then failed because the SDK's
advertised llvm-objdump --offload-fatbin option printed usage and exited 2.
The full child report is retained; no metadata target was accepted.

Inspection of the exact compiler source d6f6cb691863e9258034ab48c1dfce21689d5606
confirmed that the option table advertises this flag while the driver never
selects it as an action. The corrected command uses llvm-readobj --offloading.
Its reviewed ObjDumper path prints bundle ID/URI metadata and does not extract
code-object files. New closed-output tests reject usage, absent/foreign/duplicate
targets, malformed URIs and oversized output. The corrected gate requires new
execution; this is not a rerun of unchanged source. See
[allocation and failure evidence](evidence/gpu-lz4-layout-0879cce.json).

## Captured empty-host placeholder schema

At 86bc830, all 25 source/metadata unit cases passed and the native read-only
command successfully reported the gfx1100 bundle. Admission then correctly
failed closed because its schema omitted the SDK's zero-byte AMD64 host
placeholder. Both the native metadata and rejection are retained in
[CPU and reader evidence](evidence/gpu-lz4-cpu-86bc830.json).

The next parser requires exactly one empty host-x86_64-pc-windows-msvc- entry and
one positive hip AMD gfx1100 image. Every URI must name the exact compiler object;
offset/size must stay within its actual byte length. Nonempty/foreign/duplicate
host entries and invalid device extents remain rejected. The suite now contains
28 source/metadata cases; these changes require new execution. Object/dependency
digests are retained before inspection and must remain unchanged afterward.
Reader warnings or truncated metadata fail admission. No candidate is loaded.

[GPU_LZ4_KERNEL_REVIEW.md](GPU_LZ4_KERNEL_REVIEW.md) records the bounded writer
review and the pending current-AMD synchronization/scheduling captures. Kernel
source and existing runtime paths remain unchanged.

## Completed exact object inspection

At 1d20f40, the optional run 43425233273 passed all 28 source/metadata cases,
compiled the pinned entry, validated AMD64 headers and exactly the empty host
placeholder plus positive gfx1100 image, and verified unchanged output digests.
The candidate object is 217,673 bytes with SHA-256
80e9664ff57c3c1815d4cfa53528e5a3a68ecb5d845d43fcee543caddd22d20c.
All original SDK/worker gates remained, including 20 confirmed worker exits.
See [executed evidence](evidence/gpu-lz4-object-1d20f40.json). No object was loaded,
exported or linked into the product, and no actual GPU output was tested.
