# Developer guide

Start with [CONTRIBUTING.md](../../CONTRIBUTING.md), then read the guide for the
affected component. [End-user documentation](../README.md) is separate.

| Work | Canonical guide |
| --- | --- |
| Set up and build | [Building and testing](building.md), [host independence](../HOST_INDEPENDENCE.md) |
| Choose checks and interpret results | [Validation](../validation/README.md), [validation details](../VALIDATION.md) |
| Understand architecture | [Current diagrams](../architecture/CURRENT.md), [source review](../architecture/REVIEW.md), [revised architecture](../architecture/TARGET.md) |
| Plan implementation | [Current repository overview](../REPOSITORY_OVERVIEW.md), [detailed implementation plan](../IMPLEMENTATION_PLAN.md), [execution ledger](../DEVELOPMENT_PLAN.json), [current state](../DEVELOPMENT_STATE.json) |
| Change the storage format or recovery | [Storage references](../reference/README.md#storage-and-recovery) |
| Compact or re-encode a paged native image | [Paged maintenance](../NATIVE_PAGED_MAINTENANCE.md), [bounded staging](../STAGED_PAGED_REBUILD.md) |
| Protect payload copies and recover a damaged bank | [Mirrored paged payloads](../MIRRORED_PAYLOADS.md) |
| Develop protected collective representations | [Shared-base groups](../SHARED_BASE_GROUPS.md) |
| Integrate paged sessions, handoff and advice | [Managed paged volumes](../MANAGED_PAGED_VOLUMES.md) |
| Develop automatic collective discovery and admission | [Automatic group maintenance](../AUTOMATIC_GROUP_MAINTENANCE.md) |
| Connect Windows resource observations and idle work | [Windows background maintenance](../WINDOWS_BACKGROUND_MAINTENANCE.md) |
| Connect paged storage to writable WinFsp callbacks | [Paged callback integration](../WINFSP_PAGED_VOLUMES.md) |
| Own WinFsp events, background work and shutdown | [Owned controller](../WINFSP_OWNED_CONTROLLER.md) |
| Create shared helper/WinFsp memory backing | [Adaptive factory and retained ownership](../WINDOWS_MEMORY_VOLUME_FACTORY.md) |
| Scale object storage and enumeration | [Paged object store](../PAGED_OBJECT_STORE.md), [native backend integration](../NATIVE_PAGED_BACKEND.md), [inode records](../INODE_RECORD_NAMESPACE.md), [directory indices](../DIRECTORY_NAME_INDEX.md) |
| Change Windows file operations | [Windows references](../reference/README.md#windows-integration) |
| Change GPU compression or fallback | [GPU references](../reference/README.md#gpu-compression-and-worker-lifecycle) |
| Change compression or maintenance | [Compression references](../reference/README.md#compression-and-learning), [research](../research/README.md) |
| Change the application | [Application contract](../WINDOWS_VOLUME_APP.md), [shared backends](../WINDOWS_VOLUME_BACKENDS.md) |
| Review dependencies | [Dependency audit](../DEPENDENCY_AUDIT.md) |
| Write or reorganize documentation | [Documentation conventions](documentation.md) |

The [complete reference catalog](../reference/README.md#complete-component-catalog)
keeps existing technical documents discoverable without moving their paths.

AI agents follow [AGENTS.md](../../AGENTS.md). Its safety and domain rules remain
controlling. Keep commands and requirements in their canonical guide and link
to them from navigation pages.
