#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Phase 0 automation: run load, capture CPU & Off-CPU perf profiles, extract baseline metrics,
# and append baseline row into docs/performance_plan.md if not present.
set -euo pipefail

DURATION=${DURATION:-60}
CLIENTS=${CLIENTS:-12}
PORT=${PORT:-40000}
CPU_PROF_DUR=${CPU_PROF_DUR:-30}
OFFCPU_PROF_DUR=${OFFCPU_PROF_DUR:-30}
BUILD_DIR=${BUILD_DIR:-build-prof}
OUT_ROOT=baseline_artifacts
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
RUN_DIR=${OUT_ROOT}/${TIMESTAMP}
PLAN_FILE=docs/performance_plan.md

mkdir -p "${RUN_DIR}" || true

echo "[phase0] Starting load (clients=${CLIENTS} dur=${DURATION}s) in background"
LOAD_LOG=${RUN_DIR}/phase0_load.log
./scripts/load_run_baseline.sh --clients ${CLIENTS} --duration ${DURATION} --port ${PORT} >"${LOAD_LOG}" 2>&1 &
LOAD_PID=$!

# Wait until server.pid appears
echo "[phase0] Waiting for server.pid..."
for i in {1..20}; do
	if [[ -f baseline_logs/server.pid ]]; then break; fi
	sleep 0.5
done
if [[ ! -f baseline_logs/server.pid ]]; then
	echo "[phase0] server.pid not found; abort" >&2
	kill -INT ${LOAD_PID} 2>/dev/null || true
	exit 1
fi
SERVER_PID=$(cat baseline_logs/server.pid)
echo "[phase0] Detected server PID=${SERVER_PID}"

echo "[phase0] Launching CPU perf attach (${CPU_PROF_DUR}s)"
PID=${SERVER_PID} DUR=${CPU_PROF_DUR} OUT_DIR=${RUN_DIR}/cpu ./scripts/profile_cpu.sh >/dev/null 2>&1 &
CPU_PROF_PID=$!

if [[ ${OFFCPU_PROF_DUR} -gt 0 ]]; then
	echo "[phase0] Launching Off-CPU perf attach (${OFFCPU_PROF_DUR}s)"
	PID=${SERVER_PID} DUR=${OFFCPU_PROF_DUR} OUT_DIR=${RUN_DIR}/offcpu ./scripts/profile_offcpu.sh >/dev/null 2>&1 &
	OFFCPU_PROF_PID=$!
else
	OFFCPU_PROF_PID=""
fi

# Wait for load to finish
wait ${LOAD_PID} || true
echo "[phase0] Load script finished"

# Wait profiling tasks (they auto-timeout)
wait ${CPU_PROF_PID} 2>/dev/null || true
if [[ -n "${OFFCPU_PROF_PID}" ]]; then wait ${OFFCPU_PROF_PID} 2>/dev/null || true; fi

# Extract metrics from server.log
SERVER_LOG=baseline_logs/server.log
# Prefer final runtime flush if present, else periodic runtime sample
if grep -q '"metric":"runtime_final"' ${SERVER_LOG}; then
	AVG_TICK_NS=$(grep -E '"metric":"runtime_final"' ${SERVER_LOG} | tail -1 | sed -E 's/.*"avg_tick_ns":([0-9]+).*/\1/' || echo 0)
else
	AVG_TICK_NS=$(grep -E '"metric":"runtime"' ${SERVER_LOG} | tail -1 | sed -E 's/.*"avg_tick_ns":([0-9]+).*/\1/' || echo 0)
fi
FULL_BYTES=$(grep -E '"metric":"snapshot_totals"' ${SERVER_LOG} | tail -1 | sed -E 's/.*"full_bytes":([0-9]+).*/\1/' || echo 0)
DELTA_BYTES=$(grep -E '"metric":"snapshot_totals"' ${SERVER_LOG} | tail -1 | sed -E 's/.*"delta_bytes":([0-9]+).*/\1/' || echo 0)
FULL_COUNT=$(grep -E '"metric":"snapshot_totals"' ${SERVER_LOG} | tail -1 | sed -E 's/.*"full_count":([0-9]+).*/\1/' || echo 0)
DELTA_COUNT=$(grep -E '"metric":"snapshot_totals"' ${SERVER_LOG} | tail -1 | sed -E 's/.*"delta_count":([0-9]+).*/\1/' || echo 0)

MEAN_TICK_MS=$(awk -v ns=${AVG_TICK_NS} 'BEGIN{ printf("%.3f", ns/1000000.0) }')

# Append baseline metrics section note if not already appended (idempotent marker)
MARKER="<!-- BASELINE_RUN_${TIMESTAMP} -->"
if ! grep -q "${MARKER}" "${PLAN_FILE}"; then
	{
		echo "\n${MARKER}"
		echo "Baseline capture ${TIMESTAMP}:"
		echo "- avg_tick_ns=${AVG_TICK_NS} (~${MEAN_TICK_MS} ms)"
		echo "- snapshot_full_bytes_total=${FULL_BYTES} (count=${FULL_COUNT})"
		echo "- snapshot_delta_bytes_total=${DELTA_BYTES} (count=${DELTA_COUNT})"
		echo "- clients=${CLIENTS} duration=${DURATION}s port=${PORT}"
		echo "- cpu_profile=${RUN_DIR}/cpu/cpu_flame.svg (if generated)"
		echo "- offcpu_profile=${RUN_DIR}/offcpu/offcpu_flame.svg (if generated)"
	} >>"${PLAN_FILE}"
fi

# Copy logs & plan snapshot
cp baseline_logs/server.log "${RUN_DIR}/server.log" || true
cp baseline_logs/clients.pid "${RUN_DIR}/clients.pid" 2>/dev/null || true
cp ${PLAN_FILE} "${RUN_DIR}/performance_plan_snapshot.md" || true

cat <<EOF
[phase0] Done.
Artifacts in: ${RUN_DIR}
Avg tick (ns): ${AVG_TICK_NS}
Avg tick (ms): ${MEAN_TICK_MS}
Full snapshot bytes total: ${FULL_BYTES} (count ${FULL_COUNT})
Delta snapshot bytes total: ${DELTA_BYTES} (count ${DELTA_COUNT})
EOF
