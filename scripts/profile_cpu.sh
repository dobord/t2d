#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if ! command -v perf >/dev/null 2>&1; then
	echo "perf not found (install linux-tools)." >&2
	exit 1
fi

DUR=${DUR:-30}
FREQ=${FREQ:-400}
OUT_DIR=${OUT_DIR:-profiles/cpu/$(date +%Y%m%d-%H%M%S)}
PID=${PID:-}
CMD=${*:-}
mkdir -p "${OUT_DIR}"

if [[ -z "${PID}" && -z "${CMD}" ]]; then
	echo "Usage: PID=<pid> [DUR=30 FREQ=400 OUT_DIR=...] $0  # profile existing process" >&2
	echo "   or: DUR=30 FREQ=400 OUT_DIR=... $0 ./t2d_server --cfg server.yaml" >&2
	exit 2
fi

META_FILE="${OUT_DIR}/README.txt"
GIT_SHA=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
cat >"${META_FILE}" <<EOF
CPU profile capture
Git SHA: ${GIT_SHA}
Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)
Duration: ${DUR}
Frequency: ${FREQ}
Command: ${CMD}
PID: ${PID}
EOF

if [[ -n "${CMD}" ]]; then
	echo "[profile] Launching target command under perf..."
	perf record -F "${FREQ}" -g --output "${OUT_DIR}/perf.data" -- ${CMD} &
	PERF_PID=$!
	sleep "${DUR}" || true
	kill -INT ${PERF_PID} 2>/dev/null || true
else
	echo "[profile] Attaching to PID ${PID}..."
	perf record -F "${FREQ}" -g -p "${PID}" --output "${OUT_DIR}/perf.data" -- sleep "${DUR}"
fi

echo "[profile] Generating folded stacks..."
if command -v stackcollapse-perf.pl >/dev/null 2>&1 && command -v flamegraph.pl >/dev/null 2>&1; then
	perf script >"${OUT_DIR}/perf.script"
	stackcollapse-perf.pl "${OUT_DIR}/perf.script" >"${OUT_DIR}/out.folded"
	flamegraph.pl "${OUT_DIR}/out.folded" >"${OUT_DIR}/cpu_flame.svg"
	echo "FlameGraph: ${OUT_DIR}/cpu_flame.svg"
else
	echo "FlameGraph tools not found; install from https://github.com/brendangregg/FlameGraph" >&2
fi

echo "Done. Raw data: ${OUT_DIR}/perf.data"
