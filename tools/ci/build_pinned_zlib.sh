#!/usr/bin/env bash
# Input: exact reviewed zlib checkout. Output: local static library and headers.
# Disposable hosted Linux CI only; no installation or network activity.
set -euo pipefail
[[ "${GITHUB_ACTIONS:-}" == true && "${RUNNER_ENVIRONMENT:-}" == github-hosted && "${RUNNER_OS:-}" == Linux ]]
[[ "$(git -C .ci-deps/zlib rev-parse HEAD)" == 99623ef7097352da7993ba614161fd6a311e046e ]]
git -C .ci-deps/zlib diff --quiet HEAD
cmake -S .ci-deps/zlib -B .ci-deps/zlib-build -DCMAKE_BUILD_TYPE=Release -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_INSTALL=OFF
cmake --build .ci-deps/zlib-build --target zlibstatic --parallel 2
test -f .ci-deps/zlib-build/libz.a
mkdir .ci-deps/zlib-build/flitzfs-headers
cp .ci-deps/zlib/zlib.h .ci-deps/zlib-build/zconf.h .ci-deps/zlib-build/flitzfs-headers/
