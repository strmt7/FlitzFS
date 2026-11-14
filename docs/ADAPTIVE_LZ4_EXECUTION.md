# Resource-adaptive compression execution — experimental

Owner direction, 14 November 2026: the old prototype allocation ceiling must not
govern full development or product sizing. Never assume every host has the same
RAM, GPU, installed software, available resources or workload. This supersedes
the earlier tiny ceiling as a general development policy; it does not authorize
driver changes, owner mounts, resource exhaustion or unqualified host execution.

## Scalable layout

gpu_lz4_uniform.hpp introduces a separate private version-2 kernel layout.
Its byte offsets and allocation extent are 64-bit. The block count follows input
length rather than an eight-descriptor array. A uniform block width allows
constant-work per-block region calculation instead of scanning every other
descriptor. Checked addition/multiplication, dense nonoverlapping arenas, output
guards and canonical field validation remain mandatory.

The selected compressor's 64-KiB block constraint is algorithmic, not the size of
a batch, operation, filesystem or GPU allocation. Larger work consists of many
blocks. The launch must additionally respect the detected device's actual grid,
wave, function and memory capabilities; symbol presence is not that evidence.
The private typed API is separate from the legacy module API. Old wire/storage
bytes and old compatibility tests remain intact.

## Automatic memory window

plan_gpu_lz4_window selects the largest input window admitted by CURRENT
local/nonlocal GPU budgets, physical availability and process-constrained commit
capacity, after the existing independently enforced reserves. It charges the
actual implementation's fixed runtime bound, resident input/slab/output copies
and per-block metadata. A separately qualified per-block kernel-time estimate,
validated execution quantum and detected grid capacity also constrain work.
Slower measured execution, tighter foreground quanta and smaller actual hardware
grid limits automatically reduce a window. These estimates are not universal
wall-time or kernel-fault guarantees. Cost evidence is configuration- and recovery-generation
bound. Unknown, stale, changed or unavailable evidence selects CPU.

There is no fixed KiB/MiB allocation ceiling in this planner. A large operation
returns a bounded window plus its unconsumed remainder for streaming. Available
capacity is not an instruction to allocate it: small demand stays small.

The work-share divisor controls interference as a fraction of remaining
resources and therefore scales with observed capacity. It must be at least 16;
background work should use a smaller share and yield promptly to foreground
activity. This is a conservative independently enforced safety policy, not a
claim of a learned optimum. The helper must derive/tune policy from actual
CPU/GPU contention, foreground demand, observed costs and persistent workload
learning. One owner's measurements must never become universal defaults.
A mutable learned policy cannot override integrity, resource or ownership gates.

The planner is allocation-free and uses at most 32 binary-search trials.
Its output is not a memory reservation or permission to launch. The allocating
worker must repeat fresh resource admission and original-baseline transition
checks, preserve independent CPU comparison and confirm exact retirement.

## Implementation/qualification state

The new portable tests cover capacities from 4 to 64 GiB, different GPU budgets,
changing physical/commit/GPU pressure, variable and partial blocks, work shares,
large streaming remainders, arithmetic extremes, configuration/recovery mismatch
and every canonical layout word. These are synthetic resource observations;
large reported windows allocate no corresponding payload in those tests.

The isolated SDK build compiles the registered version-2 kernel bodies for the
reviewed target set and links a SEPARATE private module. Its source/header blobs,
native images, imports and exact getter export must be verified. It is not
loaded, placed in the legacy worker bundle, exported as an installable component
or called by ordinary filesystem operations.

The legacy 256-KiB/eight-block path remains a compatibility/control fixture.
It must not be presented as the new path's resource policy. The new planner and
module require exact-source qualification; no adaptive GPU execution is claimed
at initial publication.

## Required full integration

1. Qualify the portable host/device layout and resource-plan invariants and
   current-SDK native objects without owner execution.
2. Add a resource-adaptive streaming worker transport, with exact negotiated
   frame capacities, immutable source, create-only protected session objects,
   one outstanding operation and bounded host/device resident accounting.
3. Execute uniform batches through the authenticated typed module, validate
   all output sizes/guards and independent CPU bytes, and repeat resource
   admission at allocation/completion. No fixed prototype ceiling may constrain
   full-development work. Retain finite work quanta and a kill switch.
4. Connect the asynchronous controller and qualified Windows advice bridge.
   CPU selection must not wait for GPU/driver cleanup; late replies remain
   terminal and retries require evidence plus exact retirement.
5. Automatically learn/tune workload size, compression and scheduling from
   measured complete costs and live CPU/GPU contention. Verify actual hardware
   and foreground impact after matching isolated qualification, then advance
   collective deep compression and scaling.

The selected fast-stage block width does not constrain future collective cold
compression groups, protected shared models/dictionaries or alternative codecs.

This is an active implementation path toward the full filesystem. A planner,
compiled kernel, green CPU test or larger allowable allocation does not prove
the requested autonomous resource governor, useful GPU acceleration, extreme
compression, universal data safety or release readiness.
