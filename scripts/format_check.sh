#!/usr/bin/env bash
set -euo pipefail
# Strict formatting check: format all first-party sources into a temp copy and compare.
ROOT=$(git rev-parse --show-toplevel)
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
STATUS=0
FILES=$(git -C "$ROOT" ls-files '*.cpp' '*.hpp' '*.h' '*.cc' '*.cxx' | grep -v '^third_party/' || true)
for rel in $FILES; do
  f="$ROOT/$rel"
  mkdir -p "$TMPDIR/$(dirname "$rel")"
  cp "$f" "$TMPDIR/$rel"
  clang-format -i "$TMPDIR/$rel"
  if ! diff -u "$f" "$TMPDIR/$rel" >/dev/null; then
    echo "Needs formatting: $rel"
    STATUS=1
  fi
done
if [ $STATUS -ne 0 ]; then
  echo "Formatting issues detected. Run: cmake --build build --target format" >&2
  exit 1
fi
echo "Formatting OK"
