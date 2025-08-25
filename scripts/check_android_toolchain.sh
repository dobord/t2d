#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

if [[ -f .env ]]; then
  # shellcheck disable=SC1091
  source .env
fi

EXPECTED_NDK=${ANDROID_NDK_VERSION:-}
EXPECTED_BUILD_TOOLS=${ANDROID_BUILD_TOOLS_VERSION:-}

if [[ -z ${EXPECTED_NDK} || -z ${EXPECTED_BUILD_TOOLS} ]]; then
  echo "Missing expected versions (ANDROID_NDK_VERSION / ANDROID_BUILD_TOOLS_VERSION) in environment or .env" >&2
  exit 1
fi

SDK_ROOT=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
if [[ -z $SDK_ROOT ]]; then
  echo "ANDROID_SDK_ROOT / ANDROID_HOME not set" >&2
  exit 1
fi

NDK_DIR="$SDK_ROOT/ndk/$EXPECTED_NDK"
BT_DIR="$SDK_ROOT/build-tools/$EXPECTED_BUILD_TOOLS"

FAIL=0
if [[ ! -d $NDK_DIR ]]; then
  echo "NDK directory missing: $NDK_DIR" >&2
  FAIL=1
else
  echo "Found NDK: $NDK_DIR"
fi

if [[ ! -d $BT_DIR ]]; then
  echo "Build Tools directory missing: $BT_DIR" >&2
  FAIL=1
else
  echo "Found Build Tools: $BT_DIR"
fi

PLATFORM_DIR="$SDK_ROOT/platforms/android-34"
if [[ ! -d $PLATFORM_DIR ]]; then
  echo "Platform android-34 missing: $PLATFORM_DIR" >&2
  FAIL=1
else
  echo "Found Platform: $PLATFORM_DIR"
fi

if [[ $FAIL -ne 0 ]]; then
  echo "Android toolchain verification FAILED" >&2
  exit 1
fi

echo "Android toolchain verification OK"
