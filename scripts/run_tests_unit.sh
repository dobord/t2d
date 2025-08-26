#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
BUILD_DIR=${BUILD_DIR:-build}
ctest -T Test -R "t2d_unit_" --output-on-failure -C ${CONFIG:-Release} -j $(nproc) || true
# Direct run fallback if ctest labels not used
for t in t2d_unit_session_manager t2d_unit_framing t2d_unit_heartbeat_timeout; do
	if [ -x "$BUILD_DIR/$t" ]; then
		"$BUILD_DIR/$t"
	fi
done
