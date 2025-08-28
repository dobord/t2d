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
# Auto-configure build dir to produce compile_commands.json if missing
if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
	if [[ -d "$BUILD_DIR" ]]; then
		echo "[clang-tidy] compile_commands.json missing in $BUILD_DIR – attempting to reconfigure with export enabled" >&2
	else
		echo "[clang-tidy] build dir $BUILD_DIR not found – creating" >&2
		mkdir -p "$BUILD_DIR"
	fi
	cmake -S . -B "$BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ${CMAKE_ARGS:-} >/dev/null 2>&1 || true
fi
if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
	echo "compile_commands.json still not found in $BUILD_DIR (ensure project config succeeds)" >&2
	exit 1
fi

# Collect first-party source files (exclude third_party and generated proto headers, but include generated pb.cc for analysis if present)
mapfile -t TRACKED < <(git ls-files '*.cpp' '*.cc' '*.cxx' '*.hpp' '*.h')
mapfile -t UNTRACKED < <(git ls-files --others --exclude-standard | grep -E '\.(cpp|cc|cxx|hpp|h)$' || true)
FILES=()
for f in "${TRACKED[@]}" "${UNTRACKED[@]}"; do
	[[ -z "$f" ]] && continue
	[[ $f == third_party/* ]] && continue
	[[ $f == build/* ]] && continue
	[[ $f == build-qt/* ]] && continue
	[[ $f == build-tests/* ]] && continue
	FILES+=("$f")
done
if [[ ${#FILES[@]} -eq 0 ]]; then
	echo "No source files found for analysis" >&2
	exit 0
fi

FAIL=0
if command -v run-clang-tidy >/dev/null 2>&1; then
	echo "[clang-tidy] Using run-clang-tidy helper" >&2
	# run-clang-tidy handles parallelism; filter file list to unique
	printf '%s\n' "${FILES[@]}" | sort -u | xargs -r run-clang-tidy -quiet -p "$BUILD_DIR" || FAIL=1
else
	for f in "${FILES[@]}"; do
		echo "[clang-tidy] $f" >&2
		clang-tidy "$f" --quiet -p "$BUILD_DIR" --export-fixes /tmp/clang-tidy-fixes.yaml -- -std=c++20 || FAIL=1
	done
fi

if [[ $FAIL -ne 0 ]]; then
	echo "clang-tidy reported issues" >&2
	exit 1
fi
echo "clang-tidy clean"
