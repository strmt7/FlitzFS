# Experimental native LZ4 storage composition

This separate SDK diagnostic checks whether one independently verified worker
payload can become a real native-file extent. It is not an ordinary filesystem
worker route, mounted volume, speed benchmark, release or physical power-loss test.

The earlier compression trial at 979cd512 verified actual GPU output but did not
store it in the native filesystem. This new composition uses the codec-4 storage
foundation and native inode advisory hook. It preserves independent CPU recovery.

## Qualification before execution

The explicit SDK runtime fixture builds the probe with the same source, statically
linked CPU LZ4 and native filesystem. CI first retains all existing metadata,
refusal, compute fault and raw-reader controls. Its hosted-only
`--native-cpu-fixture` then exercises both accepted independent block bytes and
empty advice selecting CPU encoding. Both cases require exact public native CPU
readback, preservation of a previously acknowledged snapshot, verified locked
pages, zero remaining locked bytes and restored working-set limits.

The new composition remains unqualified until that exact-source workflow passes.
CPU success does not establish GPU execution. Owner execution additionally needs
the matching full core matrix, reviewed source and artifacts, exact artifact/file
digests, fresh current adapter/runtime identity, physical/commit headroom and fresh
GPU memory admission. Never repeat an unchanged hardware trial.

## Explicit hardware mode

`flitzfs_lz4_runtime_probe.exe --gpu-native-once TRUSTED_WORKER ORDINAL LUID NODE RUNTIME_SHA256`
requires an absolute trusted same-package compression worker, one physical node
and the exact installed SystemDirectory runtime digest. The caller must verify
the containing directories and all package files first. It never stages or
installs an AMD runtime or driver.

The synthetic source is one 64-KiB extent with an 8-KiB seeded prefix repeated
eight times. Before GPU work, the probe acknowledges a separate four-byte sentinel
and retains its readable snapshot. The source and candidate occupy ordinary bounded
process memory; authoritative live/checkpoint image buffers are locked private RAM,
2 MiB each. There is no image path, mount, disk backend or pageable-image fallback.

One supervised compression exchange uses the existing 128-MiB worker, 256-MiB
aggregate process job, 20-percent CPU cap and complete 256-KiB explicit device
allocation ceiling. The native mode uses a five-second worker observation limit;
the existing older admission mode retains its separate 15-second limit.
The outer invocation must retain its existing 15-second wall bound and two-second
termination observation. Unknown child/supervisor retirement is terminal and
forbids replacement work.

A confirmed ordinary worker refusal selects CPU encoding. An accepted result
passes the worker-wire CPU comparison and the native object encoder's independent
comparison against the exact final extent. The public native CPU reader must
recover the current file and the prior sentinel. Only afterward may test-only
record inspection establish that the stored codec-4 payload equals the supplied
worker bytes. Stored-size accounting reports active payload and complete image
extent separately.

The report records verification/selection, source/payload/image bytes, complete
exchange time, device allocation, worker status/retirement, memory peaks and locked
memory cleanup. Status 0 requires verified GPU bytes actually stored. CPU recovery
without that result returns 2. Exceptions remain failed diagnostics.

This synchronous one-shot diagnostic does not demonstrate immediate asynchronous
CPU fallback, useful end-to-end GPU speed, background learning or production
Windows helper integration. Startup amortization and the normal automatic path
remain development work. A volatile RAM checkpoint does not survive host power loss.

Related: HIP_LZ4_RUNTIME.md, NATIVE_FAST_INGESTION.md, WORKER_CHANNEL.md,
GPU_RESOURCE_ENVELOPE.md and HOST_RAM_TESTING.md.
