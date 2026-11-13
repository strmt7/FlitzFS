# hipCOMP LZ4 offline object gate (experimental)

This source prepares optional compilation after the source-bound review in
[GPU_CODEC_CANDIDATES.md](GPU_CODEC_CANDIDATES.md). Its own admission tests and
Windows compiler execution remain pending. No filesystem or worker invokes the
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
