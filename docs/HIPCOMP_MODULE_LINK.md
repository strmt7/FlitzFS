# Private typed compression module link (experimental)

This separate manual prerequisite connects the checked byte-entry object to the
same SDK's registered host stubs. It is a link-only fixture on disposable hosted
Windows VMs. The module is never loaded, executed, included in a worker bundle,
linked into a product or exported as a binary artifact.

## Interface and source identity

The private 48-byte KernelApi uses the existing 168-byte Layout by value and
typed kernel pointers. It requires exact structure sizes, allocation/chunk/count
limits, table-slot size, guard size, zero reserved data and both entry addresses.
Selection accepts only reported widths 32 and 64. This validates a private type
contract; it does not authenticate a module, discover a device or authorize a
launch. A future caller must first authenticate and retain its selected module,
validate geometry/resources and keep the module alive through retirement.

Addresses are assigned inside the actual HIP translation unit using the SDK's
registered host stubs. No HIP registration ABI or device-symbol lookup is rebuilt.
The separate C++ module publishes one direct getter whose immutable table remains
module-owned. Its construction requires no application heap ownership or exception
crossing. The native link checks do not call that getter or load the DLL.

The four admitted assets are the contract, API header, module source and HIP entry.
Each retains exact canonical Git/SHA-256/size pins and exact-or-whole-CRLF checkout
verification. Their staged files, the original candidate, complete notices and
all existing source budgets remain checked. Renaming the private contract include
does not change its bytes, 168-byte layout or stored/wire representation.

## Explicit opt-in and link admission

All three workflow inputs must be true: codec_candidate, native_sync_candidate
and link_candidate. Missing dependencies reject before SDK provisioning. The
default SDK path and the original/native/bounded compile-only gates stay unlinked.
This is an explicit exception for the separate link fixture; it does not authorize
loading candidate code or running it on the owner machine.

The same verified whole ROCm distribution supplies compiler, headers, device
inputs and import library. The optional CMake target consumes only the qualified
private object and staged module source; the existing worker keeps its original
kernel module and bundle. Both object and source options are required together.

The linked DLL must remain under 32 MiB, have no delayed imports, import only the
actual header-observed SDK runtime and kernel32, and export exactly the direct
flitzfs_candidate_lz4_api getter. The qualifier independently rechecks every
canonical staged input, physical control asset and candidate object digest
after linking. Existing process, memory, free-space, command and worker gates remain.
Only JSON records and complete notices leave the VM; no DLL/object is uploaded.

## CPU and prior qualification

The normal CPU suite exercises exact typed argument forwarding through two dummy
functions. It rejects each changed scalar field, absent entries and unknown widths.
These are CPU stubs, not a GPU emulation or a call into the linked module.
The four-asset staging cases preserve collision/budget/partial-failure coverage.

At 857d5c9 the preceding entry passed all five core configurations, including
122 sanitizer tests. Each validator log reports 144250 checks, 65546 planner
shapes, 12474 field refusals and 176 geometry groups. All 13 integration jobs,
the complete SDK and 20 worker exits passed. Native ELF inspection observed both
entry symbols; the wave64 function folded to refusal on the gfx1100 fixture.
[Evidence](evidence/hipcomp-bounded-entry-857d5c9.json). That is no wave64 hardware,
actual GPU compression or performance proof.

This publishing source requires new CPU/compiler/link/integration execution.
Actual module/runtime use, GPU output with independent CPU decoding, complete
kernel/worker lifecycle qualification, live resource admission and useful total
operation benefit remain separate requirements. D01 and owner-device gates remain.
