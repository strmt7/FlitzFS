# Continue FlitzFS in Codex Cloud

Select `strmt7/FlitzFS` on `main` in the cloud environment. Read
[AGENTS.md](../../AGENTS.md) and record `git rev-parse HEAD` and
`git status --short --branch`. The checkout carries the development
state, default test inputs and published evidence.
[Codex Cloud setup](https://help.openai.com/en/articles/20001545-using-codex-cloud)

## Choose the next change

Start from the [state ledger](../DEVELOPMENT_STATE.json),
[release assessment](../RELEASE_RECONCILIATION_2026-11-26.md) and
[prioritized research](../research/release-blockers-2026-11-26.md).
The [implementation plan](../IMPLEMENTATION_PLAN.md) links the work
packages. Good first cloud tasks are proving incremental ownership
checks against full indexed recovery and comparing compression
candidates by protected bytes and exact CPU readback.

Use the agent workflow in AGENTS.md. Provision the required tools
in task-owned locations during environment setup when network access
is needed. The [semantic runner](../../tools/dev/semantic_search.py)
accepts explicit provider and model paths. The
[research workflow](../../.github/workflows/hosted-research.yml)
records the reviewed source and dependency setup. Its runner commands
apply to its disposable environment.

## Run the portable baseline

From the repository root on Linux, use CMake 3.22 or newer, a C and
C++20 toolchain, and a build tool. Size parallelism from available
resources. These checks use generated data and memory-backed fixtures.

```sh
cmake -S . -B build-cloud-raw -DCMAKE_BUILD_TYPE=Release \
  -DFLITZFS_WITH_ZSTD=OFF -DFLITZFS_WITH_LZ4=OFF
cmake --build build-cloud-raw --parallel 2 \
  --target flitzfs_native_filesystem_tests flitzfs_native_shared_extent_tests
ctest --test-dir build-cloud-raw --no-tests=error --output-on-failure \
  --timeout 60 --parallel 1 -R '^bounded_native_(filesystem|shared_extents)$'
```

For compression work, stage clean pinned Zstandard and LZ4 checkouts
at `.ci-deps/zstd` and `.ci-deps/lz4` during cloud setup. The
[portable CPU workflow](../../.github/workflows/portable-cpu.yml) gives
the repository URLs and commits. The
[codec builder](../../tools/ci/build_linux_codecs.py) verifies both pins
and prepares a fresh CMake cache and build receipt.

```sh
python3 tools/ci/build_linux_codecs.py
cmake -C .ci-deps/linux-codecs/codec-cache.cmake -S . -B build-cloud-codecs \
  -DCMAKE_BUILD_TYPE=Release -DFLITZFS_REQUIRE_ZSTD=ON -DFLITZFS_REQUIRE_LZ4=ON
cmake --build build-cloud-codecs --parallel 2 --target flitzfs_native_group_maintenance_tests
ctest --test-dir build-cloud-codecs --no-tests=error --output-on-failure \
  --timeout 60 --parallel 1 -R '^bounded_native_indexed_(packed_short_extents|pack_distinct_short_extents|cross_window_pack)$'
```

## Carry the evidence forward

Record the revision, build options, dependencies and actual results.
Recreate historical public fixtures from their receipts where possible.
For unavailable fixtures, use fresh matched inputs and record the
difference. Follow the [validation guide](../validation/README.md) for
Windows SDK, Explorer, driver-backed mount and AMD qualification.
Reserve Windows performance claims for the
[matched Windows benchmarks](../BENCHMARK_PROTOCOL.md).

Publish verified changes to `main` through the official connector as
described in AGENTS.md. If that connector is unavailable in the selected
environment, retain the patch and test evidence for authorized
publication. The owner controls release.
