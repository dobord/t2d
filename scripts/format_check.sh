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
# Ensure project style file is available inside temp tree so clang-format
# does not fall back to its built-in default (which previously caused every
# file to be reported as needing formatting). Copy rather than symlink to
# keep the temp dir self-contained.
if [[ -f "$ROOT/.clang-format" ]]; then
  cp "$ROOT/.clang-format" "$TMPDIR/.clang-format"
fi
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
echo "C/C++ formatting OK"

# Shell script formatting check (shfmt diff mode)
if command -v shfmt >/dev/null 2>&1; then
  SH_FILES=$(git -C "$ROOT" ls-files '*.sh' | grep -v '^third_party/' || true)
  if [ -n "$SH_FILES" ]; then
    SH_STATUS=0
    for shf in $SH_FILES; do
      # shfmt does not have a built-in dry-run diff across many versions; emulate via temp copy
      cp "$ROOT/$shf" "$TMPDIR/$shf.shfmt" || continue
      shfmt "$TMPDIR/$shf.shfmt" > "$TMPDIR/$shf.out" 2>/dev/null || continue
      if ! diff -u "$ROOT/$shf" "$TMPDIR/$shf.out" >/dev/null; then
        echo "Needs formatting (shfmt): $shf"
        SH_STATUS=1
        if [[ $APPLY -eq 1 ]]; then
          shfmt -w "$ROOT/$shf" || true
        fi
      fi
    done
    if [ $SH_STATUS -ne 0 ]; then
      if [[ $APPLY -eq 1 ]]; then
        echo "Applied shfmt to offending shell scripts. Please stage & commit." >&2
      else
        echo "Shell script formatting issues detected. Install shfmt and run with --apply to fix." >&2
        STATUS=1
      fi
    else
      echo "Shell script formatting OK"
    fi
  fi
fi

# Protobuf formatting check (buf format -d) if buf present
if command -v buf >/dev/null 2>&1; then
  if [ -f "$ROOT/buf.yaml" ] || [ -f "$ROOT/buf.yml" ]; then
    if ! buf format -d > "$TMPDIR/buf.diff" 2>/dev/null; then
      echo "Warning: buf format encountered an error (skipping check)" >&2
    else
      if [ -s "$TMPDIR/buf.diff" ]; then
        echo "Protobuf formatting issues detected (buf)."
        if [[ $APPLY -eq 1 ]]; then
          buf format -w || true
          echo "Applied buf formatting. Please stage & commit." >&2
        else
          echo "Run: buf format -w  (or scripts/format_check.sh --apply)" >&2
        fi
        STATUS=1
      else
        echo "Protobuf formatting OK"
      fi
    fi
  fi
fi

if [ $STATUS -eq 0 ]; then
  echo "All formatting OK"
else
  exit 1
fi
