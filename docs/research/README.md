# Research and design

[Documentation](../README.md) · [Developer guide](../development/README.md)

Research informs implementation; it is not proof that a feature exists.
Use the [current plan](../DEVELOPMENT_PLAN.md) and
[release assessment](../RELEASE_READINESS.md) for implementation status.

| Question | Design or research |
| --- | --- |
| Foreground and stronger background compression | [Compression strategy](../COMPRESSION_STRATEGY.md), [collective compression](../COLD_COMPRESSION_RESEARCH.md) |
| Workload learning and low-impact maintenance | [Autonomous maintenance](../AUTONOMOUS_MAINTENANCE.md), [optimization state](../OPTIMIZATION_STATE.md) |
| Durability and repair | [Resilient store](../RESILIENT_STORE_DESIGN.md), [persistence model](../PERSISTENCE_MODEL.md) |
| GPU codec selection | [Candidate review](../GPU_CODEC_CANDIDATES.md), [kernel review](../GPU_LZ4_KERNEL_REVIEW.md) |
| Windows architecture and product behavior | [Windows architecture](../WINDOWS_ARCHITECTURE.md), [product plan](../WINDOWS_PRODUCT.md) |

Source captures and qualification records belong with their evidence, not in the
product README. Preserve failed or superseded observations with their dates and
exact scope. Do not present old rendered content as newly published research.
