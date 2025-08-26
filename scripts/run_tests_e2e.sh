#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
BUILD_DIR=${BUILD_DIR:-build}
CFG_ARG="${1:-}"
for t in t2d_e2e_match_start t2d_e2e_input_move t2d_e2e_heartbeat t2d_e2e_bot_fill t2d_e2e_bot_projectile t2d_e2e_delta_snapshots t2d_e2e_damage_event t2d_e2e_damage_multi t2d_e2e_kill_feed; do
	if [ -x "$BUILD_DIR/$t" ]; then
		echo "[run_e2e] $t ${CFG_ARG:+(cfg=$CFG_ARG)}"
		if [ -n "$CFG_ARG" ]; then
			"$BUILD_DIR/$t" "$CFG_ARG"
		else
			"$BUILD_DIR/$t"
		fi
	fi
done
