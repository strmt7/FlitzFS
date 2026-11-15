# Readiness and validation

[Documentation](../README.md) · [Developer guide](../development/README.md)

## Current product status

FlitzFS is experimental and substantially short of a release candidate.
Start with [release readiness](../RELEASE_READINESS.md) for implemented behavior,
major gaps and claim boundaries. No release date or completion percentage is
established.

## What evidence means

CPU tests, SDK compilation, real GPU execution, Windows mounted integration and
physical power-loss tests cover different properties. A successful isolated
case does not establish full Windows compatibility, automatic GPU operation,
comparative performance or universal data safety.

Performance must account for complete stored bytes and whole-operation costs.
Synthetic payload results are not filesystem benchmarks. Current evidence does
not establish speed or size superiority over other filesystems.

## Developer evidence

| Question | Source |
| --- | --- |
| What was executed? | [Validation records](../VALIDATION.md) |
| Which source is currently qualified? | [Current development state](../DEVELOPMENT_STATE.json) |
| What remains? | [Release readiness](../RELEASE_READINESS.md), [implementation plan](../DEVELOPMENT_PLAN.md) |
| Where are immutable receipts? | [Evidence directory](../evidence/) |
| What qualifies automatic group discovery? | [Controller contract](../AUTOMATIC_GROUP_MAINTENANCE.md), [source-bound CPU/model evidence](../evidence/automatic-group-maintenance-2026-11-15.json) |
| What does the native store measurement cover? | [Native measurements](../NATIVE_STORE_MEASUREMENTS.md) |
| What failure model is promised? | [Persistence model](../PERSISTENCE_MODEL.md), [resilient store design](../RESILIENT_STORE_DESIGN.md) |

Retain historical failures and source identities. Earlier green cases do not
explain an old failure or qualify later changed code.
