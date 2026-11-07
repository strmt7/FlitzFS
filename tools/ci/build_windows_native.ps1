# Input: staged SDK and pinned .ci-deps/zstd and .ci-deps/zlib on a disposable Windows runner.
# Output: native build; no downloads, driver installation or mount operations.
[CmdletBinding()]
param([switch]$BuildMountProbe)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or $env:RUNNER_OS -ne 'Windows') {
    throw 'Native CI builds require a disposable hosted Windows runner.'
}
cmake -S .ci-deps/zstd/build/cmake -B .ci-deps/zstd-build -A x64 -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_MULTITHREAD_SUPPORT=OFF
if ($LASTEXITCODE) { throw 'Zstandard configure failed' }
cmake --build .ci-deps/zstd-build --config Release --parallel 2
if ($LASTEXITCODE) { throw 'Zstandard build failed' }
$libraries = @(Get-ChildItem .ci-deps/zstd-build -Filter zstd_static.lib -Recurse -File)
if ($libraries.Count -ne 1) { throw 'Expected one pinned static Zstandard library' }
$headers = (Resolve-Path .ci-deps/zstd/lib).Path
# zlib is separately pinned by checkout; build only its static target.
cmake -S .ci-deps/zlib -B .ci-deps/zlib-build -A x64 -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_INSTALL=OFF
if ($LASTEXITCODE) { throw 'zlib configure failed' }
cmake --build .ci-deps/zlib-build --config Release --target zlibstatic --parallel 2
if ($LASTEXITCODE) { throw 'zlib build failed' }
$zlibLibraries = @(Get-ChildItem .ci-deps/zlib-build -Filter libzs.lib -Recurse -File)
if ($zlibLibraries.Count -ne 1) { throw 'Expected one pinned static zlib library' }
$zlibHeaders = Join-Path (Resolve-Path .ci-deps/zlib-build).Path 'flitzfs-headers'
if (Test-Path -LiteralPath $zlibHeaders) { throw 'Header staging directory already exists' }
New-Item -ItemType Directory -Path $zlibHeaders | Out-Null
Copy-Item -LiteralPath .ci-deps/zlib/zlib.h -Destination $zlibHeaders
Copy-Item -LiteralPath .ci-deps/zlib-build/zconf.h -Destination $zlibHeaders
$probe = if ($BuildMountProbe) { 'ON' } else { 'OFF' }
cmake -S . -B build-native -A x64 -DFLITZFS_REQUIRE_ZLIB=ON "-DZLIB_INCLUDE_DIR=$zlibHeaders" "-DZLIB_LIBRARY_RELEASE=$($zlibLibraries[0].FullName)" -DFLITZFS_REQUIRE_ZSTD=ON -DFLITZFS_BUILD_WINFSP=ON "-DFLITZFS_BUILD_MOUNT_PROBE=$probe" "-DFLITZFS_WINFSP_SDK=$env:FLITZFS_WINFSP_SDK" "-DFLITZFS_ZSTD_INCLUDE=$headers" "-DFLITZFS_ZSTD_LIBRARY=$($libraries[0].FullName)"
if ($LASTEXITCODE) { throw 'Native configure failed' }
cmake --build build-native --config Release --parallel 2
if ($LASTEXITCODE) { throw 'Native build failed' }
