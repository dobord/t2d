#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# build_and_run_tests.sh - Configure, build and run all tests in ./build-tests
# Usage examples:
#   ./scripts/build_and_run_tests.sh              # default Debug
#   BUILD_TYPE=Release ./scripts/build_and_run_tests.sh -DT2D_ENABLE_TSAN=ON
# Environment variables:
#   BUILD_TYPE (default Debug)
#   BUILD_DIR  (default build-tests)
set -euo pipefail
BUILD_TYPE=${BUILD_TYPE:-Debug}
BUILD_DIR=${BUILD_DIR:-build-tests}
# Always enable tests; skip Qt client to speed up unless explicitly requested via EXTRA_CMAKE_ARGS
EXTRA_CMAKE_ARGS=${EXTRA_CMAKE_ARGS:-}
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DT2D_BUILD_TESTS=ON -DT2D_BUILD_CLIENT=OFF -DT2D_BUILD_QT_CLIENT=OFF ${EXTRA_CMAKE_ARGS}
cmake --build "$BUILD_DIR" --parallel --target t2d_tests || cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure --parallel $(nproc || echo 2)
