#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# build_android.sh - Local helper to build (and optionally install) the Android app.
#
# Features:
#  - Generates protobuf lite sources via scripts/gen_proto_android.sh (idempotent)
#  - Builds Debug (default) or Release variant using Gradle wrapper
#  - Emits absolute APK path
#  - Optionally installs APK with adb using an absolute path (robust against cwd issues)
#
# Usage:
#   scripts/build_android.sh                # build debug
#   scripts/build_android.sh --release      # build release
#   scripts/build_android.sh --install      # build debug then adb install -r
#   scripts/build_android.sh --release --install
#   scripts/build_android.sh --variant debug # explicit variant
#
# Environment:
#   ANDROID_SDK_ROOT (or ANDROID_HOME) must point to a valid SDK (platforms;android-34 & build-tools;34.0.0 & ndk;29.0.13846066 installed).
#   protoc must be in PATH for proto generation.
#
# Exit codes:
#   0 success, non-zero on failure.

set -euo pipefail

VARIANT="debug"
DO_INSTALL=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --release) VARIANT="release"; shift;;
    --debug) VARIANT="debug"; shift;;
    --variant) VARIANT="${2:-}"; shift 2;;
    --install) DO_INSTALL=1; shift;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "Unknown arg: $1" >&2; exit 1;;
  esac
done

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || true)
if [[ -z $ROOT ]]; then
  echo "Unable to determine repo root (git rev-parse failed)" >&2
  exit 1
fi
cd "$ROOT"

ANDROID_DIR="$ROOT/android"
if [[ ! -d $ANDROID_DIR ]]; then
  echo "Android directory missing: $ANDROID_DIR" >&2
  exit 1
fi

if ! command -v protoc >/dev/null 2>&1; then
  echo "protoc not found in PATH (install protobuf-compiler)" >&2
  exit 1
fi

echo "[android] Generating protobuf lite sources" >&2
scripts/gen_proto_android.sh

WRAPPER="$ANDROID_DIR/gradlew"
if [[ ! -x $WRAPPER ]]; then
  echo "Gradle wrapper not executable; fixing perms" >&2
  chmod +x "$WRAPPER" 2>/dev/null || true
fi

SDK_ROOT=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
if [[ -z $SDK_ROOT ]]; then
  echo "ANDROID_SDK_ROOT / ANDROID_HOME not set" >&2
  exit 1
fi

# Ensure local.properties contains sdk.dir (do not overwrite if user customized)
LP="$ANDROID_DIR/local.properties"
if ! grep -q 'sdk.dir=' "$LP" 2>/dev/null; then
  echo "sdk.dir=$SDK_ROOT" > "$LP"
fi

echo "[android] Building variant=$VARIANT" >&2
pushd "$ANDROID_DIR" >/dev/null
case "$VARIANT" in
  debug)   CMD="assembleDebug";  OUT_SUB=debug;;
  release) CMD="assembleRelease"; OUT_SUB=release;;
  *) echo "Unsupported variant: $VARIANT" >&2; exit 1;;
esac

"$WRAPPER" $CMD --no-daemon --stacktrace
popd >/dev/null

APK_GLOB="$ANDROID_DIR/app/build/outputs/apk/$OUT_SUB/*.apk"
APK_PATH=$(ls $APK_GLOB 2>/dev/null | head -n1 || true)
if [[ -z $APK_PATH ]]; then
  echo "APK not found (looked for $APK_GLOB)" >&2
  exit 1
fi

ABS_APK=$(realpath "$APK_PATH")
echo "APK_ABS_PATH=$ABS_APK" > build/android_apk_path.env 2>/dev/null || true
echo "Built APK: $ABS_APK"

if [[ $DO_INSTALL -eq 1 ]]; then
  if ! command -v adb >/dev/null 2>&1; then
    echo "adb not found; skipping install" >&2
  else
    echo "[android] Installing APK via adb" >&2
    adb install -r "$ABS_APK" || {
      echo "adb install failed" >&2; exit 1; }
    echo "Install complete"
  fi
fi

echo "Done."
