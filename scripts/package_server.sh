#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
BUILD_DIR=${BUILD_DIR:-build}
OUT_DIR=${OUT_DIR:-dist}
mkdir -p "$OUT_DIR"
cp "$BUILD_DIR/t2d_server" "$OUT_DIR/" 2>/dev/null || {
	echo "Server binary not found"
	exit 1
}
cp -r config "$OUT_DIR/config"
strip "$OUT_DIR/t2d_server" || true
tar -C "$OUT_DIR" -czf "$OUT_DIR/t2d_server.tar.gz" t2d_server config
sha256sum "$OUT_DIR/t2d_server.tar.gz" >"$OUT_DIR/t2d_server.tar.gz.sha256"
echo "Packaged: $OUT_DIR/t2d_server.tar.gz"
