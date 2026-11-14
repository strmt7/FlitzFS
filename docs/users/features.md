# Features and limitations

[Documentation](../README.md) · [Getting started](getting-started.md)

## Available experimental components

- A native Windows application and taskbar tray for a temporary test volume.
- A filesystem engine with files, directories, compressed storage and snapshots.
- Independent CPU readers and explicit recompression tools.
- Experimental AMD GPU compression, with independently checked CPU recovery.

An engine capability or isolated GPU test does not imply that the complete
Windows application exposes it automatically. The current application remains
an experimental text-file workflow.

## Compression and optimization

FlitzFS is intended to compress files during writing, then analyze related data
across files and improve compression and layout in the background. The priority
is the complete stored size, including shared information needed to read files,
subject to practical retrieval speed.

Automatic grouping, deeper collective compression, persistent workload learning
and low-impact unattended maintenance remain under development. CPU and GPU
encoders may produce different sizes; stored data must remain readable on CPU
when GPU support is unavailable.

## Planned Windows experience

The complete product is intended to provide a normal Explorer file-and-folder
view, with a helper and tray for managing physical and virtual volumes.
Production volume creation, installation, updates and complete Windows behavior
remain unfinished.

## Reliability and performance

FlitzFS has experimental integrity, snapshot and recovery mechanisms. They do
not establish universal protection against bugs, lost power or destroyed copies.
No comparative speed or compression-superiority claim is established.

See [readiness and validation](../validation/README.md) for the current limits
and supporting evidence.
