# Experimental reusable uniform compute scope

This is an implementation candidate following the [retained failed hardware sweep](evidence/uniform-sweep-847423f-failure.json). The abnormal exit's cause remains unproven. No new owner experiment or speed, memory-growth repair, universal-safety or normal-filesystem-routing claim follows before source-specific qualification.

## Bounded lifetime

The explicit sweep now calls `run_hip_uniform_lz4_sweep_in_worker`. Its scope retains one authenticated runtime/kernel binding, verifies device identity/topology/coverage and maximum-quantum geometry once, creates one low-priority nonblocking stream and allocates one peak-sized device slab. It owns two equally bounded CPU workspaces. None survives the finite worker operation.

The single-window and phase-profile entry points retain their existing initialization and retirement boundaries. Common runtime-table/coverage admission and actual device binding are factored without changing the kernel ABI, ordinary frames or persisted codecs.

The caller still admits the complete immutable source, dense worst-case result, one prepared quantum, workspaces, runtime and transport costs. A small slab does not authorize an unbounded host input. Existing process jobs, five-second experiment observation, complete host/resource gates and the 128 MiB worker limit remain unchanged.

The owned native write path now uses separate whole-request and per-launch
sizing. A write may use several serial launches through one bounded worker
when the complete source and reply fit the host budget. The original
single-launch path keeps its existing capacity and behavior.
[Adaptive admission](ADAPTIVE_LZ4_EXECUTION.md#automatic-memory-window)

## Quantum completion and protected reuse

The Windows worker retains one verified DXGI adapter binding for the attempt.
It samples local and nonlocal video memory and host physical and commit memory
at each checkpoint. It checks that the factory still describes the current
topology before and after sampling. Query or topology failure retires the
binding and refuses the attempt. The normal recovery policy decides whether a
later request can start a new attempt.
[DXGI memory query contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo)

Each quantum receives fresh host/GPU memory admission against the operation's original baseline. Its active upload is fully initialized in the reusable CPU workspace; the remaining physical slab is filled with a distinct 0xd3 canary. The kernel receives the canonical ACTIVE extent, which may be smaller than its owned backing allocation.

The same stream executes one quantum at a time. Full stream synchronization precedes complete peak-slab readback. All active source/alignment/output guards, candidate sizes, independent CPU bytes and the entire unused tail must pass. A fresh completion-memory check follows. Only then may the next upload replace the workspace. The maximum launch geometry was qualified for the largest admitted quantum; a shorter tail cannot increase it.

After all quanta, the complete combined source/result passes CPU verification and explicit stream/slab retirement must succeed. Module retirement, successful exact child/supervisor exit and independent parent verification remain required before acceptance. Any quantum, memory, guard, allocation or cleanup failure discards the private prefix and grants no retry. Cleanup failure overrides prior candidate success. CPU selection remains independent of driver cleanup in the unchanged asynchronous controller; this explicit diagnostic remains synchronous.

## Qualification requirements

Portable wire tests compare initialization of dirty reused storage against canonical uploads and reject wrong extents while preserving outer guards. Selected-SDK CPU tests require exactly one initialization, stream creation, allocation and retirement across complete and partial sweeps; property/attribute/priority queries must remain bounded. They cover wave selection, a device grid that fits only the quantum, one-byte input, next-quantum resource refusal, second-quantum wait failure, corrupt active output and unused tail, null/misaligned resources and final cleanup failure. No partial candidate may escape.

The old single-window, phase-profile, serial-callback and raw/refusal controls remain. The raw control now exercises both single and reusable APIs without CPU decoding and must touch no HIP API. Both actual wrapper entry points are checked with a null runtime in the supervised child. Fake CPU calls are not hardware execution.

Publishing-source core and full SDK qualification passed at `d33efd6`; the executed scope is recorded below. Any later owner experiment must use audited exact qualified binaries, fresh adapter/runtime/topology/headroom and unchanged strict jobs. Do not repeat source `847423f`, increase its cap to obscure failure, deliberately inject a driver fault or leave a resident helper.

## Research and remaining work

The current primary HIP sources and native transport receipts are retained in [the sweep research evidence](evidence/uniform-sweep-research-2026-11-14.json). They document stream ordering, full synchronization and the distinction between completion and destruction; no new HIP API or installed toolchain is introduced here. The public source is not asserted to match the installed SDK build identifier.

A successful new trial would establish only this bounded implementation on the observed configuration. Root-cause attribution, repeated complete-cost measurements, production cost qualification, automatic Windows routing and minimum-total-size collective learning remain separate work. The full filesystem goal remains active.

## First SDK fixture correction

At `92d326e`, SDK compilation reached the CPU matrix and failed with `Copy slab extent`. The fake allocator admitted the quantum slab while its copy predicate still expected the whole request's slab. The correction records the successful allocation extent, requires both full copies to equal that exact extent and bounds each active kernel layout within it. It adds no size tolerance and changes no production execution. [The failed result](evidence/uniform-reuse-92d326e-fixture-failure.json) is retained; the corrected source requires new qualification.

## Finite Windows write comparison — 18 November 2026

The native indexed test admitted 1 MiB per worker while limiting each
launch to 128 KiB of input and a 328,288-byte peak device slab. Each AMD
trial verified all 16 extents, recovered the image independently on CPU
and confirmed worker retirement. The synthetic trials also verified all
32 mirrored records per write against the GPU payload.

| Input | CPU callback | AMD callback | CPU occupied image | AMD occupied image |
| --- | ---: | ---: | ---: | ---: |
| Synthetic repeated extents | 2.12–2.60 ms | 1,051–1,411 ms | 2,097,152 bytes | 2,097,152 bytes |
| Public FlitzFS source text | 4.12–4.70 ms | 1,564–1,695 ms | 3,145,728 bytes | 3,276,800 bytes |

Each row contains three alternating CPU/AMD pairs. The public sample
contains bytes from 128 tracked files. The comparison measures the
complete callback and resulting image in an injected WinFsp fixture.
Both request classes select CPU. Larger admitted GPU batches improve
the amount of work per initialization. The measured LZ4 route still
adds latency and provides no size savings on these inputs.
[Source-bound comparison](evidence/finite-amd-exchange-2026-11-18.json)

## Executed qualification and bounded hardware result

Source `d33efd64d94427f5b6c9f2e269520d4ba5e26bcd` passed all 13 core jobs (run 43613526754), 152 native Windows groups, both Linux 131-test configurations, Windows raw and sanitizers. Full SDK run 43613527401 passed 3,074 uniform-compute checks across 79 scenarios (9 successes, 70 refusals), both missing-decoder controls and 20 confirmed admission exits. CPU tests enforce one setup/stream/allocation/retirement and cover partial final quanta, memory refusal, active/unused-tail corruption and cleanup failure.

One separately admitted owner trial then completed the 1,048,576-byte synthetic native operation as eight two-chunk quanta. The GPU returned 135,344 encoded bytes, stored exactly and independently recovered on CPU together with prior acknowledged data. Explicit device bytes stayed at 328,288. Worker peak commit was 92,737,536 bytes (88.44 MiB) under the unchanged 128 MiB cap; aggregate job peak was 104,452,096 bytes. All 4,194,304 locked bytes returned to zero, working-set limits restored, exact worker/supervisor retirement completed and no FlitzFS process remained. Runtime digest and driver version stayed unchanged; the GPU reported healthy. The scoped recorded-event check found no matching warnings/errors and cannot prove absence of a driver fault or reset.

Exchange wall time was 626,486,100 ns. This mode has no phase profile. Both CPU/GPU complete fixture images occupied 1,114,112 bytes; payload differences against the manually constructed CPU advice are not an optimized CPU-compressor comparison. This single aligned hardware case establishes no comparative speed, whole-image gain, larger-group cost bound or physical-power-loss qualification. The original `847423f` abnormal exit remains retained and causally unproven.

[Exact source, artifacts, raw results and cleanup evidence](evidence/uniform-reuse-d33efd6.json) is retained. Do not repeat the unchanged trial. Ordinary callbacks remain CPU-only. Repeated configuration/recovery keyed complete-cost measurements, varied real data, automatic resource policy, controlled Windows routing and minimum-total-size collective compression remain required.
