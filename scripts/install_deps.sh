#!/usr/bin/env bash
set -euo pipefail
# Install build dependencies for t2d server (Ubuntu/Debian prototype)

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build protobuf-compiler libprotobuf-dev git ca-certificates

echo "[install_deps] Done."
