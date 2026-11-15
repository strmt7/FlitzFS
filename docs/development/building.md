# Building and testing

[Developer guide](README.md) · [Safety contract](../SAFETY.md)

Develop, build and run safe tests locally. The owner deleted the blanket
off-host and VM-first restrictions on 15 November 2026. Use disposable VMs for
operations that could destabilize or change the host, and CI for complementary
platform checks. Read the safety/component contracts before optional integrations;
preserve existing data, drivers, system configuration and other applications.

## Ordinary CPU build

The base build needs C/C++ compilers and CMake. Zstandard and LZ4 are optional
unless explicitly required. Configuration installs nothing.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2 --target flitzfs_native_filesystem_tests flitzfs_native_shared_extent_tests
ctest --test-dir build -C Release -R '^bounded_native_(filesystem|shared_extents)$' --output-on-failure --timeout 45 --parallel 1
```

These selected CPU suites use in-memory filesystem models. The full CTest suite
also contains ordinary-file fixtures; run those only with backing that meets
the experiment's requirements. Optional file/SDK fixture registration is
controlled by `FLITZFS_REGISTER_INTEGRATION_FIXTURES`, which local builds may
enable after qualifying their backing/runtime and containment. CI keeps its
previous registration default; hosting flags are not a permission requirement.
Use [the local host guide](../HOST_RAM_TESTING.md) and
`tools/dev/windows_run.py` for resource-bounded Windows process-tree execution.
Discover available tools and size build concurrency from actual headroom;
the example's parallelism is not a host or product assumption.

MSVC C++ targets explicitly enable `/EHsc`; writer leases and rollback require
exception unwinding. Custom `CMAKE_CXX_FLAGS`, including sanitizer flags, must
preserve that behavior. The store fails compilation when unwinding is disabled.
Local AddressSanitizer checks use the installed matching user-mode runtime,
finite process-tree supervision and appropriate in-memory suites. A sanitizer
pass is scoped to the instrumented configuration and executed checks.

GPU and mount probes remain opt-in and are not
ordinary CTest operations. A successful base build does not qualify an optional
application, driver-backed mount or GPU runtime path.

## Optional components

| Component | Read before enabling |
| --- | --- |
| Windows application | [Application contract](../WINDOWS_VOLUME_APP.md) |
| Read-only callbacks | [WinFsp callbacks](../WINFSP_CALLBACKS.md) |
| Writable adapter | [Writable adapter](../NATIVE_WRITABLE_ADAPTER.md) |
| Disposable mount integration | [Read-only probe](../LIVE_MOUNT_PROBE.md), [writable trials](../WRITABLE_MOUNT_TRIAL.md) |
| GPU runtime and worker | [Runtime](../HIP_LZ4_RUNTIME.md), [uniform runtime](../UNIFORM_LZ4_RUNTIME.md) |
| Owned deferred writes | [Ownership and completion](../OWNED_DEFERRED_WRITES.md) |

SDK staging and driver provisioning described for disposable hosted runners
are not automatic local driver-installation instructions. Keep process-tree budgets, fresh
headroom admission and confirmed resource retirement in every authorized live
harness. Safe local builds and their supervised tests need no prior hosted run.

## Verification and provenance

Use the [validation index](../validation/README.md) to select relevant checks.
Record the exact tested source, configuration, commands and failures.
CPU semantics, SDK compilation, actual GPU execution, mounted integration and
power-loss behavior are separate forms of evidence.

Official hosted provisioning verifies its tools and dependencies in isolated
locations. It is development infrastructure, not the final product's dependency
installer. See [host independence](../HOST_INDEPENDENCE.md).
