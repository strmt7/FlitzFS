# Input: staged SDK and pinned .ci-deps/zstd and .ci-deps/lz4 on a disposable Windows runner.
# Output: native build; no downloads, driver installation or mount operations.
[CmdletBinding()]
param([switch]$BuildMountProbe, [switch]$PortableCpu)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or $env:RUNNER_OS -ne 'Windows') {
    throw 'Native CI builds require a disposable hosted Windows runner.'
}
if ($PortableCpu -and $BuildMountProbe) { throw 'Portable CPU qualification never mounts or loads the SDK.' }
$zstdRuntime = if ($PortableCpu) { 'ON' } else { 'OFF' }
$runtimeFlags = if ($PortableCpu) {
    @('-DCMAKE_POLICY_DEFAULT_CMP0091=NEW', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded')
} else { @() }
foreach ($codec in @('zstd', 'lz4')) {
    $pin = if ($codec -eq 'zstd') { 'f8745da6ff1ad1e7bab384bd1f9d742439278e99' } else { 'c72c7c0ec132ad2ce2fe9fad7b0c728d9faee2fc' }
    $head = git -C ".ci-deps/$codec" rev-parse HEAD
    if ($LASTEXITCODE -or $head -ne $pin) { throw 'Native codec source identity differs' }
    git -C ".ci-deps/$codec" diff --quiet HEAD --
    if ($LASTEXITCODE) { throw 'Native codec source has tracked modifications' }
}
cmake -S .ci-deps/zstd/build/cmake -B .ci-deps/zstd-build -A x64 -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_MULTITHREAD_SUPPORT=OFF "-DZSTD_USE_STATIC_RUNTIME=$zstdRuntime" @runtimeFlags
if ($LASTEXITCODE) { throw 'Zstandard configure failed' }
cmake --build .ci-deps/zstd-build --config Release --parallel 2
if ($LASTEXITCODE) { throw 'Zstandard build failed' }
$libraries = @(Get-ChildItem .ci-deps/zstd-build -Filter zstd_static.lib -Recurse -File)
if ($libraries.Count -ne 1) { throw 'Expected one pinned static Zstandard library' }
$headers = (Resolve-Path .ci-deps/zstd/lib).Path
# LZ4 is a bounded block decoder; no CLI or multithreaded runtime is needed.
cmake -S .ci-deps/lz4/build/cmake -B .ci-deps/lz4-build -A x64 -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF -DLZ4_BUILD_MULTITHREAD=OFF -DLZ4_STATIC_MULTITHREAD=OFF -DLZ4_BUNDLED_MODE=ON @runtimeFlags
if ($LASTEXITCODE) { throw 'LZ4 configure failed' }
cmake --build .ci-deps/lz4-build --config Release --target lz4_static --parallel 2
if ($LASTEXITCODE) { throw 'LZ4 build failed' }
$lz4Libraries = @(Get-ChildItem .ci-deps/lz4-build -Filter lz4.lib -Recurse -File)
if ($lz4Libraries.Count -ne 1) { throw 'Expected one pinned static LZ4 library' }
$lz4Headers = (Resolve-Path .ci-deps/lz4/lib).Path
$probe = if ($BuildMountProbe) { 'ON' } else { 'OFF' }
$native = if ($PortableCpu) { 'OFF' } else { 'ON' }
$portable = if ($PortableCpu) { 'ON' } else { 'OFF' }
$buildDirectory = if ($PortableCpu) { 'build-portable' } else { 'build-native' }
$rootOptions = @()
if (-not $PortableCpu) { $rootOptions += "-DFLITZFS_WINFSP_SDK=$env:FLITZFS_WINFSP_SDK" }
cmake -S . -B $buildDirectory -A x64 "-DFLITZFS_PORTABLE_CPU=$portable" -DFLITZFS_REQUIRE_LZ4=ON "-DFLITZFS_LZ4_INCLUDE=$lz4Headers" "-DFLITZFS_LZ4_LIBRARY=$($lz4Libraries[0].FullName)" -DFLITZFS_REQUIRE_ZSTD=ON "-DFLITZFS_BUILD_WINFSP=$native" "-DFLITZFS_BUILD_MOUNT_PROBE=$probe" "-DFLITZFS_ZSTD_INCLUDE=$headers" "-DFLITZFS_ZSTD_LIBRARY=$($libraries[0].FullName)" @rootOptions
if ($LASTEXITCODE) { throw 'Native configure failed' }
cmake --build $buildDirectory --config Release --parallel 2
if ($LASTEXITCODE) { throw 'Native build failed' }
