#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

if ! command -v clang-tidy >/dev/null 2>&1; then
	echo "clang-tidy not found in PATH" >&2
	exit 1
fi

BUILD_DIR=${1:-build}
if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
	echo "compile_commands.json not found in $BUILD_DIR (configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON or use Ninja generator)" >&2
	exit 1
fi

# Collect first-party source files (exclude third_party and generated proto headers, but include generated pb.cc for analysis if present)
mapfile -t FILES < <(git ls-files '*.cpp' '*.cc' '*.cxx' '*.hpp' '*.h' | grep -v '^third_party/' | grep -v '^build/' || true)
if [[ ${#FILES[@]} -eq 0 ]]; then
	echo "No source files found for analysis" >&2
	exit 0
fi

FAIL=0
for f in "${FILES[@]}"; do
	echo "[clang-tidy] $f" >&2
	clang-tidy "$f" --quiet --export-fixes /tmp/clang-tidy-fixes.yaml -- -std=c++20 || FAIL=1
done

if [[ $FAIL -ne 0 ]]; then
	echo "clang-tidy reported issues" >&2
	exit 1
fi
echo "clang-tidy clean"
