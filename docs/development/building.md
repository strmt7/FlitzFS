# Building and testing

[Developer guide](README.md) · [Safety contract](../SAFETY.md)

Use a disposable development VM for builds and testing. Read the safety and
component contracts before enabling optional integrations. The owner's existing
data, drivers, system configuration and running applications must remain protected.

## Ordinary CPU build

The base build needs C/C++ compilers and CMake. Zstandard and LZ4 are optional
unless explicitly required. Configuration installs nothing.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --timeout 45 --parallel 1
```

These are ordinary CPU checks. GPU and mount probes remain opt-in and are not
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
are not owner-PC installation instructions. Keep process-tree budgets, fresh
headroom admission and confirmed resource retirement in every authorized live
harness. Do not run a generic executable outside its qualified supervisor.

## Verification and provenance

Use the [validation index](../validation/README.md) to select relevant checks.
Record the exact tested source, configuration, commands and failures.
CPU semantics, SDK compilation, actual GPU execution, mounted integration and
power-loss behavior are separate forms of evidence.

Official hosted provisioning verifies its tools and dependencies in isolated
locations. It is development infrastructure, not the final product's dependency
installer. See [host independence](../HOST_INDEPENDENCE.md).
