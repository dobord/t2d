#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if ! command -v perf >/dev/null 2>&1; then
	echo "perf not found (install linux-tools)." >&2
	exit 1
fi

DUR=${DUR:-30}
OUT_DIR=${OUT_DIR:-profiles/offcpu/$(date +%Y%m%d-%H%M%S)}
PID=${PID:-}
mkdir -p "${OUT_DIR}"

if [[ -z "${PID}" ]]; then
	echo "Usage: PID=<pid> [DUR=30 OUT_DIR=...] $0" >&2
	exit 2
fi

META_FILE="${OUT_DIR}/README.txt"
GIT_SHA=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
cat >"${META_FILE}" <<EOF
Off-CPU profile capture
Git SHA: ${GIT_SHA}
Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)
Duration: ${DUR}
PID: ${PID}
Event: sched:sched_switch
EOF

echo "[offcpu] Capturing off-CPU stacks from PID ${PID}..."
perf record -e sched:sched_switch -g -p "${PID}" --output "${OUT_DIR}/perf.data" -- sleep "${DUR}"

if command -v stackcollapse-perf.pl >/dev/null 2>&1 && command -v flamegraph.pl >/dev/null 2>&1; then
	perf script >"${OUT_DIR}/perf.script"
	stackcollapse-perf.pl --kernel "${OUT_DIR}/perf.script" >"${OUT_DIR}/out.folded"
	flamegraph.pl --title "Off-CPU" "${OUT_DIR}/out.folded" >"${OUT_DIR}/offcpu_flame.svg"
	echo "FlameGraph: ${OUT_DIR}/offcpu_flame.svg"
else
	echo "FlameGraph tools not found; skipping SVG generation." >&2
fi

echo "Done. Raw data: ${OUT_DIR}/perf.data"
