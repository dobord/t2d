#!/usr/bin/env bash
set -euo pipefail

APPLY=0
if [[ ${1:-} == "--apply" ]]; then
  APPLY=1
fi

# Strict formatting check: compare clang-format output with repo files.
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
    if [[ $APPLY -eq 1 ]]; then
      clang-format -i "$f"
    fi
  fi
done
if [ $STATUS -ne 0 ]; then
  # Always prefer our project-only target; never suggest external 'format' that may touch third_party
  if [[ $APPLY -eq 1 ]]; then
    echo "Applied clang-format to offending files (third_party excluded). Please stage & commit." >&2
  else
    echo "Formatting issues detected. Run: cmake --build build --target t2d_format  (or scripts/format_check.sh --apply)" >&2
    exit 1
  fi
fi
echo "Formatting OK"
