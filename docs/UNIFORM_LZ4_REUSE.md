# Experimental reusable uniform compute scope

This is an implementation candidate following the [retained failed hardware sweep](evidence/uniform-sweep-847423f-failure.json). The abnormal exit's cause remains unproven. No new owner experiment or speed, memory-growth repair, universal-safety or normal-filesystem-routing claim follows before source-specific qualification.

## Bounded lifetime

The explicit sweep now calls `run_hip_uniform_lz4_sweep_in_worker`. Its scope retains one authenticated runtime/kernel binding, verifies device identity/topology/coverage and maximum-quantum geometry once, creates one low-priority nonblocking stream and allocates one peak-sized device slab. It owns two equally bounded CPU workspaces. None survives the finite worker operation.

The single-window and phase-profile entry points retain their existing initialization and retirement boundaries. Common runtime-table/coverage admission and actual device binding are factored without changing the kernel ABI, ordinary frames or persisted codecs.

The caller still admits the complete immutable source, dense worst-case result, one prepared quantum, workspaces, runtime and transport costs. A small slab does not authorize an unbounded host input. Existing process jobs, five-second experiment observation, complete host/resource gates and the 128 MiB worker limit remain unchanged.

## Quantum completion and protected reuse

Each quantum receives fresh host/GPU memory admission against the operation's original baseline. Its active upload is fully initialized in the reusable CPU workspace; the remaining physical slab is filled with a distinct 0xd3 canary. The kernel receives the canonical ACTIVE extent, which may be smaller than its owned backing allocation.

The same stream executes one quantum at a time. Full stream synchronization precedes complete peak-slab readback. All active source/alignment/output guards, candidate sizes, independent CPU bytes and the entire unused tail must pass. A fresh completion-memory check follows. Only then may the next upload replace the workspace. The maximum launch geometry was qualified for the largest admitted quantum; a shorter tail cannot increase it.

After all quanta, the complete combined source/result passes CPU verification and explicit stream/slab retirement must succeed. Module retirement, successful exact child/supervisor exit and independent parent verification remain required before acceptance. Any quantum, memory, guard, allocation or cleanup failure discards the private prefix and grants no retry. Cleanup failure overrides prior candidate success. CPU selection remains independent of driver cleanup in the unchanged asynchronous controller; this explicit diagnostic remains synchronous.

## Qualification requirements

Portable wire tests compare initialization of dirty reused storage against canonical uploads and reject wrong extents while preserving outer guards. Selected-SDK CPU tests require exactly one initialization, stream creation, allocation and retirement across complete and partial sweeps; property/attribute/priority queries must remain bounded. They cover wave selection, a device grid that fits only the quantum, one-byte input, next-quantum resource refusal, second-quantum wait failure, corrupt active output and unused tail, null/misaligned resources and final cleanup failure. No partial candidate may escape.

The old single-window, phase-profile, serial-callback and raw/refusal controls remain. The raw control now exercises both single and reusable APIs without CPU decoding and must touch no HIP API. Both actual wrapper entry points are checked with a null runtime in the supervised child. Fake CPU calls are not hardware execution.

Publishing-source core and full SDK qualification are pending. Any later owner experiment must use audited exact qualified binaries, fresh adapter/runtime/topology/headroom and unchanged strict jobs. Do not repeat source `847423f`, increase its cap to obscure failure, deliberately inject a driver fault or leave a resident helper.

## Research and remaining work

The current primary HIP sources and native transport receipts are retained in [the sweep research evidence](evidence/uniform-sweep-research-2026-11-14.json). They document stream ordering, full synchronization and the distinction between completion and destruction; no new HIP API or installed toolchain is introduced here. The public source is not asserted to match the installed SDK build identifier.

A successful new trial would establish only this bounded implementation on the observed configuration. Root-cause attribution, repeated complete-cost measurements, production cost qualification, automatic Windows routing and minimum-total-size collective learning remain separate work. The full filesystem goal remains active.

## First SDK fixture correction

At `92d326e`, SDK compilation reached the CPU matrix and failed with `Copy slab extent`. The fake allocator admitted the quantum slab while its copy predicate still expected the whole request's slab. The correction records the successful allocation extent, requires both full copies to equal that exact extent and bounds each active kernel layout within it. It adds no size tolerance and changes no production execution. [The failed result](evidence/uniform-reuse-92d326e-fixture-failure.json) is retained; the corrected source requires new qualification.
