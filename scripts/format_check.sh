#!/usr/bin/env bash
set -euo pipefail
# Simple formatting check (non-strict): run clang-format and show diff
CHANGED=$(git diff --name-only --diff-filter=ACMRTUXB HEAD)
FAIL=0
for f in $CHANGED; do
  if [[ $f == *.cpp || $f == *.hpp || $f == *.h || $f == *.cxx || $f == *.cc ]]; then
    cp "$f" "$f.bak"
    clang-format -i "$f"
    if ! diff -q "$f" "$f.bak" >/dev/null; then
      echo "Needs formatting: $f"
      FAIL=1
    fi
    mv "$f.bak" "$f"
  fi
done
if [ $FAIL -eq 1 ]; then
  echo "Formatting issues detected. Run: cmake --build build --target format" >&2
  exit 1
fi
