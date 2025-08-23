#!/usr/bin/env bash
set -euo pipefail
BUILD_DIR=${BUILD_DIR:-build}
if [ ! -d "$BUILD_DIR" ]; then
  echo "No existing build dir. Run build_full.sh first." >&2
  exit 1
fi
cmake --build "$BUILD_DIR" --parallel
