#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# build_android.sh - Local helper to build (and optionally install) the Android app.
#
# Features:
#  - Generates protobuf lite sources via scripts/gen_proto_android.sh (idempotent)
#  - Optionally builds OpenSSL (static, minimal) for specified Android ABIs to support libcoro TLS
#  - Builds Debug (default) or Release variant using Gradle wrapper
#  - Emits absolute APK path
#  - Optionally installs APK with adb using an absolute path (robust against cwd issues)
#
# Usage:
#   scripts/build_android.sh                               # build debug
#   scripts/build_android.sh --release                     # build release
#   scripts/build_android.sh -r                            # build release (short alias)
#   scripts/build_android.sh --install                     # build debug then adb install -r
#   scripts/build_android.sh --release --install
#   scripts/build_android.sh --variant debug               # explicit variant
#   scripts/build_android.sh --with-openssl                # also build OpenSSL for default ABI(s)
#   scripts/build_android.sh --with-openssl --abis arm64-v8a;x86_64
#   scripts/build_android.sh --no-openssl                  # skip OpenSSL even if enabled by default later
#
# Environment:
#   ANDROID_SDK_ROOT (or ANDROID_HOME) must point to a valid SDK (platforms;android-34 & build-tools;34.0.0 & ndk;29.0.13846066 installed).
#   protoc must be in PATH for proto generation.
#   curl + perl required if using --with-openssl (for OpenSSL build system & download).
#
# Exit codes:
#   0 success, non-zero on failure.

set -euo pipefail

VARIANT="debug"
DO_INSTALL=0
WITH_OPENSSL=0
OPENSSL_VERSION=${OPENSSL_VERSION:-"3.5.0"}
MIN_API_LEVEL=${MIN_API_LEVEL:-24}
ABIS=(arm64-v8a) # default matches gradle abiFilters; override via --abis

# Simple log helpers
info(){ echo "[android] $*" >&2; }
err(){ echo "[android][err] $*" >&2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
  -r|--release) VARIANT="release"; shift;;
    --debug) VARIANT="debug"; shift;;
    --variant) VARIANT="${2:-}"; shift 2;;
  --install) DO_INSTALL=1; shift;;
  --with-openssl) WITH_OPENSSL=1; shift;;
  --no-openssl) WITH_OPENSSL=0; shift;;
  --abis) IFS=';' read -r -a ABIS <<<"${2:-}"; shift 2;;
  --openssl-version) OPENSSL_VERSION="${2:-}"; shift 2;;
  --api) MIN_API_LEVEL="${2:-}"; shift 2;;
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

build_openssl_for_abi() {
  local ABI="$1"
  local NDK_PATH="$2"
  local OPENSSL_SRC_DIR="$3/src"
  local OPENSSL_BUILD_BASE="$3/build"
  local OPENSSL_INSTALL_BASE="$4"
  mkdir -p "$OPENSSL_SRC_DIR" "$OPENSSL_BUILD_BASE" "$OPENSSL_INSTALL_BASE"
  local TARBALL="openssl-${OPENSSL_VERSION}.tar.gz"
  local TARBALL_PATH="$OPENSSL_SRC_DIR/../$TARBALL"
  if [[ ! -f $TARBALL_PATH ]]; then
    echo "[openssl] Download $OPENSSL_VERSION" >&2
    local URLS=(
      "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
      "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
      "https://ftp.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
    )
    for u in "${URLS[@]}"; do
      if curl -fsSL "$u" -o "$TARBALL_PATH.tmp"; then mv "$TARBALL_PATH.tmp" "$TARBALL_PATH"; break; fi
    done
    [[ -f $TARBALL_PATH ]] || { echo "[openssl] download failed" >&2; return 1; }
  fi
  local SRC_DIR="$OPENSSL_SRC_DIR/openssl-${OPENSSL_VERSION}"
  if [[ ! -d $SRC_DIR ]]; then
    tar -xf "$TARBALL_PATH" -C "$OPENSSL_SRC_DIR"
  fi
  local TOOLCHAIN="$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64"
  [[ -d $TOOLCHAIN ]] || { echo "[openssl] toolchain missing: $TOOLCHAIN" >&2; return 1; }
  local TARGET CONF_PREFIX CC_TRIPLE
  case "$ABI" in
    arm64-v8a) TARGET="android-arm64"; CC_TRIPLE="aarch64-linux-android";;
    armeabi-v7a) TARGET="android-arm"; CC_TRIPLE="armv7a-linux-androideabi";;
    x86) TARGET="android-x86"; CC_TRIPLE="i686-linux-android";;
    x86_64) TARGET="android-x86_64"; CC_TRIPLE="x86_64-linux-android";;
    *) echo "[openssl] unsupported ABI $ABI" >&2; return 1;;
  esac
  local BUILD_DIR="$OPENSSL_BUILD_BASE/$ABI"
  local INSTALL_DIR="$OPENSSL_INSTALL_BASE/$ABI"
  rm -rf "$BUILD_DIR"; mkdir -p "$BUILD_DIR" "$INSTALL_DIR"
  cp -r "$SRC_DIR" "$BUILD_DIR/openssl"; pushd "$BUILD_DIR/openssl" >/dev/null
  export AR="$TOOLCHAIN/bin/llvm-ar"
  export CC="$TOOLCHAIN/bin/${CC_TRIPLE}${MIN_API_LEVEL}-clang"
  export CXX="$TOOLCHAIN/bin/${CC_TRIPLE}${MIN_API_LEVEL}-clang++"
  export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
  export STRIP="$TOOLCHAIN/bin/llvm-strip"
  export ANDROID_NDK_ROOT="$NDK_PATH"
  echo "[openssl] Configure $ABI ($TARGET)" >&2
  perl ./Configure "$TARGET" -D__ANDROID_API__=$MIN_API_LEVEL --prefix="$INSTALL_DIR" --openssldir="$INSTALL_DIR" no-shared no-tests no-apps no-docs -static
  make -j"$(nproc)" build_libs
  make install_sw || make install_dev || true
  popd >/dev/null
  if [[ -f "$INSTALL_DIR/lib/libssl.a" && -f "$INSTALL_DIR/lib/libcrypto.a" ]]; then
    echo "[openssl] built $ABI -> $INSTALL_DIR" >&2
  else
    echo "[openssl] missing libs for $ABI" >&2; return 1
  fi
}

maybe_build_openssl() {
  (( WITH_OPENSSL )) || return 0
  command -v curl >/dev/null 2>&1 || { echo "curl required for --with-openssl" >&2; exit 1; }
  command -v perl >/dev/null 2>&1 || { echo "perl required for --with-openssl" >&2; exit 1; }
  local NDK_PATH
  if [[ -n ${ANDROID_NDK_HOME:-} ]]; then NDK_PATH="$ANDROID_NDK_HOME"; elif [[ -n ${ANDROID_NDK_ROOT:-} ]]; then NDK_PATH="$ANDROID_NDK_ROOT"; else
    echo "ANDROID_NDK_HOME/ROOT not set (required for OpenSSL build)" >&2; exit 1; fi
  local BASE_OUT="$ROOT/android/external/openssl"
  echo "[openssl] Building OpenSSL ${OPENSSL_VERSION} for ABIs: ${ABIS[*]}" >&2
  for abi in "${ABIS[@]}"; do
    build_openssl_for_abi "$abi" "$NDK_PATH" "$ROOT/.cache/openssl" "$BASE_OUT" || exit 1
  done
  echo "[openssl] Done. Set OPENSSL_ROOT_DIR=$BASE_OUT/<ABI> when integrating TLS." >&2
}

maybe_build_openssl

WRAPPER="$ANDROID_DIR/gradlew"
if [[ ! -x $WRAPPER ]]; then
  echo "Gradle wrapper not executable; fixing perms" >&2
  chmod +x "$WRAPPER" 2>/dev/null || true
fi

SDK_ROOT=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
if [[ -z $SDK_ROOT ]]; then
  # Try common locations
  if [[ -d /opt/android-sdk ]]; then
    SDK_ROOT="/opt/android-sdk"; export ANDROID_SDK_ROOT="$SDK_ROOT"; info "Guessed ANDROID_SDK_ROOT=$SDK_ROOT";
  elif [[ -d "$HOME/Android/Sdk" ]]; then
    SDK_ROOT="$HOME/Android/Sdk"; export ANDROID_SDK_ROOT="$SDK_ROOT"; info "Guessed ANDROID_SDK_ROOT=$SDK_ROOT";
  fi
fi
if [[ -z $SDK_ROOT ]]; then
  err "ANDROID_SDK_ROOT / ANDROID_HOME not set (install SDK or export path)"; exit 1; fi

# NDK discovery (adds if missing)
if [[ -z "${ANDROID_NDK_HOME:-}" && -z "${ANDROID_NDK_ROOT:-}" ]]; then
    if [[ -d "$SDK_ROOT/ndk" ]]; then
        export ANDROID_NDK_HOME="$SDK_ROOT/ndk/$(ls "$SDK_ROOT/ndk" | sort -V | tail -1)"
        info "Detected ANDROID_NDK_HOME=$ANDROID_NDK_HOME"
    elif [[ -d /opt/android-sdk/ndk ]]; then
        export ANDROID_NDK_HOME="/opt/android-sdk/ndk/$(ls /opt/android-sdk/ndk | sort -V | tail -1)"
        info "Detected ANDROID_NDK_HOME=$ANDROID_NDK_HOME"
    else
        err "Android NDK not found; set ANDROID_NDK_HOME"; # continue, may still build Java-only
    fi
fi
NDK_PATH="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
[[ -n $NDK_PATH ]] && info "Using NDK: $NDK_PATH"

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
