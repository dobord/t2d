#!/usr/bin/env bash
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

# Silence if no formatting tools present
if ! command -v clang-format >/dev/null 2>&1; then
  # Nothing to do
  exit 0
fi

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
mapfile -t STAGED < <(echo "$STAGED_ALL" | grep -E '\.(c|cc|cxx|cpp|h|hpp)$' || true)
FILTERED=()
for f in "${STAGED[@]}"; do
  case "$f" in
    third_party/*) continue;;
    *) FILTERED+=("$f");;
  esac
done

# Run clang-format directly on filtered staged files
if [ ${#FILTERED[@]} -gt 0 ]; then
  clang-format -i "${FILTERED[@]}" || true
fi

# Optionally format root CMakeLists.txt (first-party) if cmake-format exists
if command -v cmake-format >/dev/null 2>&1; then
  if git ls-files --error-unmatch CMakeLists.txt >/dev/null 2>&1; then
    cmake-format -i CMakeLists.txt || true
  fi
fi

# Re-stage any changes produced (only our filtered set + root CMakeLists)
CHANGED=0
if ! git diff --quiet -- "${FILTERED[@]}" CMakeLists.txt 2>/dev/null; then
  git add "${FILTERED[@]}" 2>/dev/null || true
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
