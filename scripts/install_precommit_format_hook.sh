#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Install a git pre-commit hook that auto-formats ONLY first-party sources.
# It does NOT invoke generic 'format' CMake targets (which may touch third_party in dependencies).
# Strategy:
#  1. Detect staged C/C++ files excluding third_party/**.
#  2. Run clang-format -i directly on those files.
#  3. Optionally run cmake-format only on root CMakeLists.txt.
#  4. Re-stage changed files. In strict mode (env T2D_FORMAT_BLOCK=true or git config t2d.formatBlock=true), abort.
#
# Usage:
#   scripts/install_precommit_format_hook.sh
#
# Requirements:
#   - CMake configured (will auto-configure into ./build if missing)
#   - clang-format available for C++ sources
#   - (optional) cmake-format for CMakeLists.txt formatting
set -euo pipefail

if ! git rev-parse --show-toplevel >/dev/null 2>&1; then
  echo "Error: not inside a git repository." >&2
  exit 1
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
HOOK_DIR="$REPO_ROOT/.git/hooks"
HOOK_PATH="$HOOK_DIR/pre-commit"
BUILD_DIR="$REPO_ROOT/build"

mkdir -p "$HOOK_DIR"

cat >"$HOOK_PATH" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR="$ROOT/build"

have_any=0
command -v clang-format >/dev/null 2>&1 && have_any=1
command -v cmake-format >/dev/null 2>&1 && have_any=1 || true

if [ ! -d "$BUILD_DIR" ]; then
  cmake -S "$ROOT" -B "$BUILD_DIR" >/dev/null
fi

run_target() {
  local tgt="$1"
  if cmake --build "$BUILD_DIR" --target "$tgt" >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

STAGED_ALL=$(git diff --cached --name-only --diff-filter=ACMR || true)

# C/C++ sources
mapfile -t STAGED_CXX < <(echo "$STAGED_ALL" | grep -E '\\.(c|cc|cxx|cpp|h|hpp)$' || true)
FILTERED=()
for f in "${STAGED_CXX[@]}"; do
  case "$f" in
    third_party/*) continue;;
    *) FILTERED+=("$f");;
  esac
done

# QML sources
mapfile -t STAGED_QML < <(echo "$STAGED_ALL" | grep -E '\\.qml$' || true)
QML_FILTERED=()
for f in "${STAGED_QML[@]}"; do
  case "$f" in
    third_party/*) continue;;
    *) QML_FILTERED+=("$f");;
  esac
done

# Also include modified (but not yet staged) tracked QML files so they are auto-formatted & staged.
while read -r _q; do
  [ -z "$_q" ] && continue
  case "$_q" in
    third_party/*) continue;;
  esac
  # ensure tracked (skip untracked brand-new files; user must git add them first)
  if git ls-files --error-unmatch "$_q" >/dev/null 2>&1; then
    if ! printf '%s\n' "${QML_FILTERED[@]}" | grep -Fxq "$_q"; then
      QML_FILTERED+=("$_q")
    fi
  fi
done < <(git diff --name-only --diff-filter=ACMRT | grep -E '\\.qml$' || true)

# Auto-add SPDX header to new first-party C/C++ files missing it.
for f in "${FILTERED[@]}" "${QML_FILTERED[@]}"; do
  # Only operate on files that exist in the working tree
  [ -f "$f" ] || continue
  if ! grep -E -q 'SPDX-License-Identifier:\s*Apache-2.0' "$f"; then
    # Determine comment prefix (all current first-party sources use //; adjust if ever adding pure C headers with /* */ style)
    # Preserve shebang if present
    tmp="$f.tmp.spdx";
    if head -1 "$f" | grep -q '^#!'; then
      { head -1 "$f"; echo "// SPDX-License-Identifier: Apache-2.0"; tail -n +2 "$f"; } > "$tmp"
    else
      { echo "// SPDX-License-Identifier: Apache-2.0"; cat "$f"; } > "$tmp"
    fi
    mv "$tmp" "$f"
  fi
done

# Run clang-format directly on filtered staged files (if available)
if command -v clang-format >/dev/null 2>&1 && [ ${#FILTERED[@]} -gt 0 ]; then
  clang-format -i "${FILTERED[@]}" || true
fi

# Discover qmlformat (prefer PATH, else from CMakeCache)
QMLFORMAT_BIN=""
if command -v qmlformat >/dev/null 2>&1; then
  QMLFORMAT_BIN="$(command -v qmlformat)"
elif [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  QMLFORMAT_BIN="$(grep -E '^QMLFORMAT_BIN:FILEPATH=' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2 || true)"
fi
# If still not found, inspect qt_local.cmake for custom Qt prefixes (gcc_64 style) and look in their bin dirs
if [ -z "$QMLFORMAT_BIN" ] && [ -f "$ROOT/qt_local.cmake" ]; then
  while read -r qt_prefix; do
    cand="$qt_prefix/bin/qmlformat"
    if [ -x "$cand" ]; then
      QMLFORMAT_BIN="$cand"
      echo "[pre-commit] qmlformat discovered via qt_local.cmake: $QMLFORMAT_BIN" >&2
      break
    fi
  done < <(grep -Eo '/[^" ]+/gcc_64' "$ROOT/qt_local.cmake" | sort -u)
fi
if [ -n "$QMLFORMAT_BIN" ] && [ -x "$QMLFORMAT_BIN" ]; then
  have_any=1
fi

# If QML files present but qmlformat still not found, attempt a quick CMake reconfigure (once)
if [ ${#QML_FILTERED[@]} -gt 0 ] && [ -z "$QMLFORMAT_BIN" ]; then
  if [ -d "$BUILD_DIR" ]; then
    cmake -S "$ROOT" -B "$BUILD_DIR" >/dev/null 2>&1 || true
    if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
      QMLFORMAT_BIN="$(grep -E '^QMLFORMAT_BIN:FILEPATH=' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2 || true)"
    fi
    if [ -z "$QMLFORMAT_BIN" ] && [ -f "$ROOT/qt_local.cmake" ]; then
      while read -r qt_prefix; do
        cand="$qt_prefix/bin/qmlformat"
        if [ -x "$cand" ]; then
          QMLFORMAT_BIN="$cand"; break
        fi
      done < <(grep -Eo '/[^" ]+/gcc_64' "$ROOT/qt_local.cmake" | sort -u)
    fi
  fi
fi

# Optional strict blocking if qmlformat required
if [ ${#QML_FILTERED[@]} -gt 0 ] && [ -z "$QMLFORMAT_BIN" ] && [ "${T2D_QML_FORMAT_REQUIRED:-0}" = 1 ]; then
  echo "[pre-commit] ERROR: qmlformat not found but QML files changed. Install Qt tool or set qt_local.cmake." >&2
  exit 1
fi

if [ -n "$QMLFORMAT_BIN" ] && [ -x "$QMLFORMAT_BIN" ] && [ ${#QML_FILTERED[@]} -gt 0 ]; then
  # Filter out empty files (qmlformat errors on empty)
  QML_TO_FORMAT=()
  for qf in "${QML_FILTERED[@]}"; do
    [ -s "$qf" ] && QML_TO_FORMAT+=("$qf") || echo "[pre-commit] Skipping empty QML: $qf" >&2
  done
  if [ ${#QML_TO_FORMAT[@]} -gt 0 ]; then
  echo "[pre-commit] qmlformat formatting ${#QML_TO_FORMAT[@]} file(s)" >&2
  "$QMLFORMAT_BIN" --inplace "${QML_TO_FORMAT[@]}" || echo "[pre-commit] WARNING: qmlformat failed" >&2
  fi
fi

# Optionally format root CMakeLists.txt (first-party) if cmake-format exists
if command -v cmake-format >/dev/null 2>&1; then
  if git ls-files --error-unmatch CMakeLists.txt >/dev/null 2>&1; then
    cmake-format -i CMakeLists.txt || true
  fi
fi

# If truly no formatting tools available, exit early (after all detection attempts)
if [ $have_any -eq 0 ]; then
  exit 0
fi

# Re-stage any changes produced (only our filtered set + root CMakeLists)
CHANGED=0
FILES_TO_CHECK=("${FILTERED[@]}" "${QML_FILTERED[@]}")
if ! git diff --quiet -- "${FILES_TO_CHECK[@]}" CMakeLists.txt 2>/dev/null; then
  [ ${#FILTERED[@]} -gt 0 ] && git add "${FILTERED[@]}" 2>/dev/null || true
  [ ${#QML_FILTERED[@]} -gt 0 ] && git add "${QML_FILTERED[@]}" 2>/dev/null || true
  if [ -f CMakeLists.txt ]; then git add CMakeLists.txt 2>/dev/null || true; fi
  CHANGED=1
fi

# Respect optional strict blocking mode (env var or git config)
STRICT=${T2D_FORMAT_BLOCK:-$(git config --bool --get t2d.formatBlock 2>/dev/null || echo false)}
if [ "$STRICT" = "true" ] && [ "$CHANGED" = 1 ]; then
  echo "[pre-commit] Formatting changes applied (strict mode). Review with 'git diff --cached' then recommit." >&2
  exit 1
fi

if [ "$CHANGED" = 1 ]; then
  echo "[pre-commit] Formatting applied and staged (non-strict mode, commit continues)." >&2
fi

exit 0
EOF

chmod +x "$HOOK_PATH"

echo "Installed pre-commit formatting hook at $HOOK_PATH"
