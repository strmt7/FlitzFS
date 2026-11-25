# Qualified Windows process/backing profile. Same source/codecs, two digest policies.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'Digest qualification uses native Windows process APIs'
}
$pin='916e8e70b33317d469a41ee837e47317807815c8'
$head=git -C .ci-deps/zstd rev-parse HEAD
if ($LASTEXITCODE -or $head -ne $pin) { throw 'Zstandard source identity' }
git -C .ci-deps/zstd diff --quiet HEAD --
if ($LASTEXITCODE) { throw 'Zstandard tracked modifications' }
foreach ($path in @('.ci-deps/zstd-digest-build','build-digest-auto','build-digest-scalar')) {
    if (Test-Path -LiteralPath $path) { throw 'Digest builds require fresh owned output paths' }
}
# Keep the pinned Zstandard project within the outer build process budget.
cmake -S .ci-deps/zstd/build/cmake -B .ci-deps/zstd-digest-build -A x64 -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_MULTITHREAD_SUPPORT=OFF -DZSTD_LEGACY_SUPPORT=ON -DZSTD_LEGACY_LEVEL=5 -DACTIVATE_MULTITHREADED_COMPILATION=OFF
if ($LASTEXITCODE) { throw 'Digest codec configure' }
cmake --build .ci-deps/zstd-digest-build --config Release --parallel 2 -- /nr:false
if ($LASTEXITCODE) { throw 'Digest codec build' }
$libraries=@(Get-ChildItem .ci-deps/zstd-digest-build -Filter zstd_static.lib -Recurse -File)
if ($libraries.Count -ne 1) { throw 'Expected one private Zstandard library' }
$headers=(Resolve-Path .ci-deps/zstd/lib).Path
foreach ($policy in @('auto','scalar')) {
    $simd=if ($policy -eq 'auto') { 'ON' } else { 'OFF' }
    cmake -S . -B "build-digest-$policy" -A x64 "-DFLITZFS_CPU_SIMD=$simd" -DFLITZFS_WITH_LZ4=OFF -DFLITZFS_REQUIRE_ZSTD=ON "-DFLITZFS_ZSTD_INCLUDE=$headers" "-DFLITZFS_ZSTD_LIBRARY=$($libraries[0].FullName)" -DFLITZFS_BUILD_STORE_COMPARISON=ON -DFLITZFS_BOUNDED_DIGEST_COMPARISON=ON
    if ($LASTEXITCODE) { throw 'Digest policy configure' }
    cmake --build "build-digest-$policy" --config Release --target flitzfs_digest_tests flitzfs_store_comparison --parallel 2 -- /nr:false
    if ($LASTEXITCODE) { throw 'Digest policy build' }
    ctest --test-dir "build-digest-$policy" -C Release -R '^bounded_runtime_digest_parity$' --output-on-failure --timeout 45 --parallel 1
    if ($LASTEXITCODE) { throw 'Digest policy byte/guard qualification' }
}
