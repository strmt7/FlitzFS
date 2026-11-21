# Documentation conventions

[Developer guide](README.md) · [Documentation index](../README.md)

## Audience and ownership

| Location | Purpose |
| --- | --- |
| Root README | Product introduction, current user-visible capabilities, availability, requirements, quick start and links |
| docs/README.md | Task-based documentation entry point |
| docs/users/ | User workflows, feature limits and troubleshooting |
| docs/development/ | Build, test, contribution and authoring procedures |
| docs/architecture/ | Source-bound system diagrams, architecture review and proposed design |
| docs/reference/README.md | Navigation to canonical component contracts |
| docs/research/README.md | Design investigations, separate from implemented behavior |
| docs/validation/README.md | Readiness, performance claims and evidence interpretation |
| Existing technical pages | Their component's canonical requirements and procedures |
| docs/evidence/ | Source-bound receipts and retained failure evidence |
| AGENTS.md | Agent instructions and mandatory domain reading |

## Keep the product README concise

Follow the audience and navigation pattern of FlitzZip: a logo and relevant
badges, product introduction, capabilities, availability, requirements, quick
start, documentation links, contribution links and license.

Do not append commit hashes, CI counts, worker phases, fixture budgets, failure
timelines, internal architecture or per-round progress reports to the README.
Link to developer guides and validation evidence. Update the README only when
the user-visible product information changes.

Describe intended features as intended until implemented and qualified.
Do not copy another project's installation commands, formats, release numbers
or claims. FlitzFS is a filesystem.

## One canonical source

Add each new component contract to the reference catalog and its relevant task
guide. Keep index entries current when documents are renamed.

Keep each requirement or command in one maintained guide. Index pages link to
it instead of repeating its procedure. Preserve stable paths where practical;
update every consumer when moving a document. Historical evidence remains
available without becoming current product guidance.

The reference catalog makes existing component pages discoverable; it does not
reclassify their historical observations as present authority. Do not delete
regression evidence, notices or safety rules while reorganizing navigation.

## Changes and validation

Check local file links, image paths and section anchors in new or changed
navigation. Keep tables and headings readable on GitHub. Separate audience
changes from runtime changes and run checks appropriate to their actual scope.

The structure was informed by the
[FlitzZip documentation index](https://github.com/strmt7/FlitzZip/blob/dc1a5f0582b95bb389d658d9caef6f973e8b94fe/docs/README.md)
and [developer guide](https://github.com/strmt7/FlitzZip/blob/dc1a5f0582b95bb389d658d9caef6f973e8b94fe/docs/development/README.md).
FlitzFS adds a complete technical catalog and a separate readiness entry point,
while retaining its own component contracts and safety requirements.
