#!/usr/bin/env bash
set -euo pipefail
BUILD_TYPE=${BUILD_TYPE:-Release}
BUILD_DIR=${BUILD_DIR:-build}
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DT2D_BUILD_TESTS=ON -DT2D_BUILD_CLIENT=ON "$@"
cmake --build "$BUILD_DIR" --parallel
