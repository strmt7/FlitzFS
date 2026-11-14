<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="resources/brand/flitzfs-logo-dark.png">
    <img src="resources/brand/flitzfs-logo-typed.png" alt="FlitzFS logo" width="760">
  </picture>
</p>

[![License: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)
[![Windows CI](https://img.shields.io/github/actions/workflow/status/strmt7/FlitzFS/ci.yml?branch=main&label=windows-ci)](https://github.com/strmt7/FlitzFS/actions/workflows/ci.yml)
[![Commit activity](https://img.shields.io/github/commit-activity/m/strmt7/FlitzFS)](https://github.com/strmt7/FlitzFS/commits/main)
[![cocoindex-code](https://img.shields.io/static/v1?label=&message=cocoindex-code&color=555&logo=github&logoColor=white)](https://github.com/cocoindex-io/cocoindex-code)
[![caveman](https://img.shields.io/static/v1?label=&message=caveman&color=555&logo=github&logoColor=white)](https://github.com/JuliusBrussee/caveman)
[![crawl4ai](https://img.shields.io/static/v1?label=&message=crawl4ai&color=555&logo=github&logoColor=white)](https://github.com/unclecode/crawl4ai)

# FlitzFS

An experimental compressed filesystem for **Windows 11 x64**, designed for AMD
GPU acceleration, CPU fallback and automatic background optimization.

FlitzFS aims to combine fast everyday file access with strong lossless
compression, while keeping a familiar file-and-folder view in Windows. The
project includes an experimental Windows helper and taskbar tray.

**Under development.** FlitzFS is not ready for everyday use or valuable data.
Automatic GPU operation, collective background compression and self-learning
remain unfinished. See [project readiness](docs/RELEASE_READINESS.md) for the
current scope and limitations.

## What it does

Experimental builds provide:

- A Windows application for creating a temporary test volume and working with text files.
- Lossless compressed storage, snapshots and independent CPU recovery tools.
- Explicit recompression and experimental AMD GPU compression paths.

The complete Explorer integration and physical-disk management workflows are
still being developed. See [features and limitations](docs/users/features.md).

## Compression and background optimization

The intended workflow uses fast compression when files are written, then
analyzes related files together to find smaller representations in the
background. The priority is minimum total stored size with practical file access.

This collective, self-adjusting behavior is still under development. See the
[feature overview](docs/users/features.md#compression-and-optimization).

## Get FlitzFS

There is currently no public release or supported installer.

The [source repository](https://github.com/strmt7/FlitzFS) is available for
development. Build instructions are in the [developer guide](docs/development/README.md).

## Requirements

FlitzFS targets **Windows 11 x64**. AMD GPU acceleration requires compatible
hardware and runtime support; CPU access must remain available independently
of the GPU.

See [getting started](docs/users/getting-started.md) for the current experimental
application and its limitations.

## Quick start

Start with the [experimental application guide](docs/users/getting-started.md)
for the available create, save, read, optimize and discard workflow.

A supported installation and Explorer workflow will be documented when ready.

## Documentation

Start with the [documentation index](docs/README.md).

| Task | Guide |
| --- | --- |
| Try the experimental application | [Getting started](docs/users/getting-started.md) |
| Understand available and planned features | [Features and limitations](docs/users/features.md) |
| Resolve a problem | [Troubleshooting](docs/users/troubleshooting.md) |
| Check readiness and performance claims | [Readiness and validation](docs/validation/README.md) |

## Development and contributing

Report problems with the exact action, expected result, observed result and a
small reproducible example through [GitHub Issues](https://github.com/strmt7/FlitzFS/issues).

See [CONTRIBUTING.md](CONTRIBUTING.md), the
[developer guide](docs/development/README.md) and [AGENTS.md](AGENTS.md) for
contributor and AI-agent instructions.

## License

FlitzFS is licensed under [AGPL-3.0](LICENSE). Dependencies retain their own
licenses and notices; see the [dependency guide](docs/DEPENDENCY_AUDIT.md).
