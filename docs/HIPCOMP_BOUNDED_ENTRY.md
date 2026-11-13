# Bounded byte-compression entry (experimental, offline only)

This is the next G02 compiler prerequisite. It adds no runtime export, worker
command, stored codec or normal filesystem caller. Both existing original and
native-wave candidate objects remain separately qualified before this entry.

## Source and contract

The only upstream composition is hipCOMP-core at
03df9f7369211ee15d370817165224c73fdc9a46. GitHub's native commit API again reported
that same upstream HEAD on 13 November 2026, with commit date 29 October 2026.
Its exact canonical header and closed native-sync/index transform remain required.
The package-bound HIP source/API/header qualifications are unchanged.

The new own HIP entry and contract header are fixed assets admitted by canonical
Git blob, SHA-256, length and exact-or-whole-CRLF physical representation. They are
copied exclusively into a NEW private compiler fixture. Complete source, original
entry alias, retained canonical header, notices and oracle allowance remain under
16 MiB. Existing upstream files and the byte-identical original entry remain intact.

The 168-byte trivially copyable contract is private compiler metadata, not a
persisted format, IPC frame, public ABI or pointer serialization. It mirrors the
already qualified host planner by explicit field conversion in CPU tests. It keeps
one to eight nonempty chunks, 64 KiB per chunk, and the stricter 256 KiB limit on
ALL explicit device storage together. A large input batch may therefore be refused.
The four eight-byte tables, eight-byte alignment, worst-case output, 32-byte
output guards and per-chunk uint16 hash workspace remain accounted.

## Before the first device-memory access

The shared allocation-free validator checks all counts, sizes, canonical offsets,
exact output capacities, scratch shape, exact total allocation and zero unused
chunk rows. Guarded subtraction precedes range arithmetic. Regions must match the
dense nonoverlapping host layout; no device address is accepted from a pointer table.

Each explicit wave32/wave64 entry requires the selected width to equal the actual
device-reported warpSize and block width. Block y/z and grid y/z must be one;
grid x must equal the admitted chunk count. Width mismatch is a refusal, not a
guess that a 64-thread block necessarily executes as one hardware wave.
The entry does not assert a fixed workgroup-size attribute that could let the
compiler assume the very geometry being checked. A future host must still
independently validate every launch before submission.

All admission conditions are uniform within a block and occur before slab access.
Null/misaligned bases and foreign block coordinates return without accessing memory.
After admission, one hardware-wave block handles one chunk, derives only checked
owned offsets, initializes its own output-length slot and calls only the pinned
byte-oriented compressStream. Typed integer codec modes are not admitted.
The function remains bounded by the source chunk; native wave synchronization
and index guards remain part of the private composition.

These checks do not prove that a future caller actually allocated the supplied
slab extent. A future trusted host must independently own/verify that allocation,
immutable source, module and request identity. Output guards detect corruption
after execution; they cannot prevent an invalid write. Driver/kernel faults are
not contained by a user-mode timeout guarantee.

## Executed and pending evidence

At 09c4fca the preceding native-wave composition passed seven CPU source cases,
the compiled index oracle (131046 valid reads and 196569 invalid-entry rejections),
both offline objects, exact header/dependency checks and 20 confirmed worker exits.
All 13 ordinary jobs passed, including 121 reconciled sanitizer cases.
[Source-bound evidence](evidence/hipcomp-native-sync-09c4fca.json).
Those observations do not qualify this new entry.

The new CPU test compares admission with every single-chunk planner size and
bounded multi-chunk cases, changes every canonical field/bit, rejects arithmetic
extremes and exercises mismatched widths, incomplete/excessive grids and dimensions.
It executes the exact shared validator, never device compression.

Twelve Python cases check real control asset identities, exclusive staging,
complete budget, alias/collision refusal, retained partial writes and physical
stability. Synthetic object fixtures test exact selected-byte extraction,
out-of-range/absent/duplicate images, ELF identity, native reader warnings,
truncation, missing/ambiguous symbols and preservation of an existing destination.
Their stubbed readers are parser tests, never actual AMDGPU-object evidence.

The explicit native_sync_candidate SDK mode now also compiles this separate
entry in its own fixture after the unchanged original/native candidates. It keeps
the 2-GiB command budget, 900-second compiler and 1200-second wrapper, 16-MiB object,
1-MiB dependency and 30-second reader limits. The 4-GiB reserve and 64-MiB staging
margin remain. The compiler must name the actual own entry/contract, transformed
header and selected SDK synchronization header in its dependency output.

After strict COFF/bundle inspection, the qualifier extracts only the selected
bounded device extent into a NEW private file. The same SDK's native reader must
report actual AMDGPU ELF64 identity and exactly one symbol for each explicit
entry. Every reader warning/truncation or changed input/output digest rejects.
The object, extracted image and CPU oracle executable are never exported, loaded
or linked into a product. Retained artifacts contain records and complete notices.

This publishing source still requires its own CPU/compiler/integration execution.
Actual GPU output, CPU decoder interoperability, complete kernel audit, worker
lifecycle, fresh resource admission, foreground interference and useful total
operation benefit remain unqualified. D01 stays open; no owner execution follows.

The entry's CPU/offline gate qualified at 857d5c9; see
[evidence](evidence/hipcomp-bounded-entry-857d5c9.json).
A separate [typed-module link fixture](HIPCOMP_MODULE_LINK.md) is the next
explicit opt-in. The original/native/bounded compile-only gates remain unlinked;
link-only qualification never loads or exports the private module.
