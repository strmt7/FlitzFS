# Component reference

[Documentation](../README.md) · [Developer guide](../development/README.md)

These are engineering contracts. Implementation status is tracked in
[release readiness](../RELEASE_READINESS.md) and the
[current state ledger](../DEVELOPMENT_STATE.json), not inferred from a document title.

## Storage and recovery

- [Native filesystem](../NATIVE_FILESYSTEM.md), [object format](../OBJECT_STORE_FORMAT.md), [container format](../FORMAT-v1.md)
- [Persistence model](../PERSISTENCE_MODEL.md), [replicated checkpoints](../REPLICATED_CHECKPOINTS.md), [volume head](../VOLUME_HEAD.md)
- [Managed sessions](../VOLUME_SESSION.md), [compaction](../COMPACTED_COPY.md), [recompression](../RECOMPRESSION.md)
- [Resilient store design](../RESILIENT_STORE_DESIGN.md)
- [Exact shared extents and private updates](../SHARED_EXTENTS.md)
- [Release execution and compatibility plan](../RELEASE_EXECUTION_PLAN.md), [benchmark protocol](../BENCHMARK_PROTOCOL.md)
- [Paged object storage and streaming rebuild](../PAGED_OBJECT_STORE.md)
- [Native operations on paged storage](../NATIVE_PAGED_BACKEND.md)
- [Incremental inode-record namespace](../INODE_RECORD_NAMESPACE.md)
- [Ordered directory names and snapshot cursors](../DIRECTORY_NAME_INDEX.md)
- [Native paged compaction and recompression](../NATIVE_PAGED_MAINTENANCE.md)
- [Managed paged volumes, typed heads and advisory provenance](../MANAGED_PAGED_VOLUMES.md)
- [Bounded staged paged rebuild](../STAGED_PAGED_REBUILD.md)
- [Mirrored payloads and surviving-copy CPU recovery](../MIRRORED_PAYLOADS.md)
- [Protected shared-base cross-file groups](../SHARED_BASE_GROUPS.md)

## Windows integration

- [Windows architecture](../WINDOWS_ARCHITECTURE.md), [product contract](../WINDOWS_PRODUCT.md)
- [Native I/O](../NATIVE_WINDOWS_IO.md), [writable adapter](../NATIVE_WRITABLE_ADAPTER.md), [callback boundary](../WINFSP_CALLBACKS.md)
- [File sessions](../NATIVE_FILE_SESSION.md), [flush](../NATIVE_FLUSH.md), [security](../NATIVE_SECURITY.md), [space reporting](../WINDOWS_SPACE_REPORTING.md)
- [Session lifecycle](../SESSION_LIFECYCLE.md), [origin guard](../WINFSP_ORIGIN_GUARD.md), [process isolation](../MOUNT_PROCESS_ISOLATION.md)
- [Owned deferred writes](../OWNED_DEFERRED_WRITES.md)

## GPU compression and worker lifecycle

- [Uniform runtime](../UNIFORM_LZ4_RUNTIME.md), [controller](../UNIFORM_LZ4_CONTROLLER.md), [native write bridge](../NATIVE_UNIFORM_WRITE.md)
- [Reusable work scope](../UNIFORM_LZ4_REUSE.md), [finite sweep](../UNIFORM_LZ4_SWEEP.md), [observations](../UNIFORM_LZ4_SWEEP_PROFILE.md)
- [Resources](../GPU_RESOURCE_ENVELOPE.md), [adaptive execution](../ADAPTIVE_LZ4_EXECUTION.md), [recovery](../GPU_RECOVERY.md)
- [Worker channel](../WORKER_CHANNEL.md), [driver worker](../GPU_DRIVER_WORKER.md), [runtime binding](../HIP_API_BINDING.md)
- [Independent CPU verification](../GPU_LZ4_CPU_VERIFY.md)

## Compression and learning

- [Compression strategy](../COMPRESSION_STRATEGY.md), [cold collective compression](../COLD_COMPRESSION_RESEARCH.md)
- [Autonomous maintenance](../AUTONOMOUS_MAINTENANCE.md), [optimization state](../OPTIMIZATION_STATE.md), [funding budget](../OPTIMIZATION_BUDGET.md)
- [Workload accounting](../WORKLOAD_ACCOUNTING.md), [helper integration](../WORKLOAD_HELPER.md), [advisory recovery](../ADVISORY_REBUILD.md)
- [CPU pipeline](../CPU_PIPELINE.md), [extent codecs](../EXTENT_CODECS.md), [fast ingestion](../NATIVE_FAST_INGESTION.md)

## Application and development infrastructure

- [Windows application](../WINDOWS_VOLUME_APP.md), [shared volume backends](../WINDOWS_VOLUME_BACKENDS.md)
- [Host independence](../HOST_INDEPENDENCE.md), [host testing](../HOST_RAM_TESTING.md)
- [Source snapshot](../SOURCE_SNAPSHOT.md), [isolated routing](../HOSTED_ROUTING.md), [dependency audit](../DEPENDENCY_AUDIT.md)
- [Safety contract](../SAFETY.md), [validation index](../validation/README.md)

## Complete component catalog

The [complete catalog](catalog.md) links every pre-existing technical Markdown
document under docs/, including narrower component and qualification contracts.
Paths are preserved. Historical pages and evidence remain tied to their stated
sources; their presence does not grant new execution permissions.
