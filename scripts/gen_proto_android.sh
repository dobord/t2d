#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
PROTO_DIR="$ROOT/proto"
OUT_CPP_DIR="$ROOT/android/proto/cpp"
OUT_JAVA_DIR="$ROOT/android/app/src/main/java"

if ! command -v protoc >/dev/null 2>&1; then
	echo "protoc not found in PATH" >&2
	exit 1
fi

mkdir -p "$OUT_CPP_DIR"

# Generate C++ lite runtime sources (do not alter server build which uses full runtime)
protoc --cpp_out=lite:"$OUT_CPP_DIR" -I "$PROTO_DIR" "$PROTO_DIR/game.proto"

# Generate Java classes using lite runtime (smaller, suitable for Android)
protoc --java_out=lite:"$OUT_JAVA_DIR" -I "$PROTO_DIR" "$PROTO_DIR/game.proto"

echo "Generated proto (lite) into $OUT_CPP_DIR and Java into $OUT_JAVA_DIR"
