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
WAIT_BEFORE_PROF_MS=${WAIT_BEFORE_PROF_MS:-500}
FALLBACK_FILTER=${FALLBACK_FILTER:-t2d_server}
TOP_N_SYMBOLS=${TOP_N_SYMBOLS:-10}

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

# Optional stabilization delay before attaching profilers
if [[ ${WAIT_BEFORE_PROF_MS} -gt 0 ]]; then
	SLEEP_SEC=$(awk -v ms=${WAIT_BEFORE_PROF_MS} 'BEGIN{printf("%.3f", ms/1000.0)}')
	echo "[phase0] Waiting ${WAIT_BEFORE_PROF_MS}ms before starting perf attaches"
	sleep ${SLEEP_SEC}
fi

# Preflight: check perf_event_paranoid (needs <=2 typically; <=1 preferable for kernel stacks)
if [[ -f /proc/sys/kernel/perf_event_paranoid ]]; then
	PERF_PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid || echo 999)
	if [[ ${PERF_PARANOID} -gt 2 ]]; then
		echo "[phase0][warn] kernel.perf_event_paranoid=${PERF_PARANOID} > 2; perf profiling likely to fail. Suggested temporary fix (root):" >&2
		echo "  sudo sysctl kernel.perf_event_paranoid=1" >&2
		echo "  sudo sysctl kernel.kptr_restrict=0" >&2
	fi
fi

echo "[phase0] Launching CPU perf attach (${CPU_PROF_DUR}s)"
mkdir -p "${RUN_DIR}/cpu"
PID=${SERVER_PID} DUR=${CPU_PROF_DUR} FALLBACK_FILTER=${FALLBACK_FILTER} OUT_DIR=${RUN_DIR}/cpu ./scripts/profile_cpu.sh >"${RUN_DIR}/cpu/profile.log" 2>&1 &
CPU_PROF_PID=$!

if [[ ${OFFCPU_PROF_DUR} -gt 0 ]]; then
	echo "[phase0] Launching Off-CPU perf attach (${OFFCPU_PROF_DUR}s)"
	mkdir -p "${RUN_DIR}/offcpu"
	PID=${SERVER_PID} DUR=${OFFCPU_PROF_DUR} FALLBACK_FILTER=${FALLBACK_FILTER} OUT_DIR=${RUN_DIR}/offcpu ./scripts/profile_offcpu.sh >"${RUN_DIR}/offcpu/profile.log" 2>&1 &
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
MEAN_FULL_BYTES=$(awk -v b=${FULL_BYTES} -v c=${FULL_COUNT} 'BEGIN{ if(c>0) printf("%.2f", b/c); else print 0 }')
MEAN_DELTA_BYTES=$(awk -v b=${DELTA_BYTES} -v c=${DELTA_COUNT} 'BEGIN{ if(c>0) printf("%.2f", b/c); else print 0 }')

# Extract top CPU symbols if folded stacks exist (inclusive approximation)
TOP_CPU_SECTION=""
FOLDED_FILE="${RUN_DIR}/cpu/out.folded"
if [[ ! -f "${FOLDED_FILE}" && -f "${RUN_DIR}/cpu/out.folded.full" ]]; then
	FOLDED_FILE="${RUN_DIR}/cpu/out.folded.full"
fi

# Build top CPU symbols (inclusive) if folded present
TOP_CPU_SECTION=""
if [[ -f "${FOLDED_FILE}" ]]; then
	awk -F';' -v TOPN=${TOP_N_SYMBOLS} 'function trim(s){gsub(/^ +| +$/,"",s);return s} { line=$0; sub(/^[^ ]+ /,"",line); c=$NF; if(c+0==0){c=$(NF);}; c+=0; n=split($0,parts,";"); for(i=1;i<=n;i++){# extract function token up to first space
			split(parts[i],f," "); fn=trim(f[1]); if(fn!="") samples[fn]+=c; total+=c; } } END { if(total>0){ for(fn in samples) printf "%s %s\n", samples[fn], fn | "sort -nr" } }' "${FOLDED_FILE}" | head -${TOP_N_SYMBOLS} >"${RUN_DIR}/cpu/.top_syms" || true
	if [[ -s "${RUN_DIR}/cpu/.top_syms" ]]; then
		TOTAL_SAMPLES=$(awk '{s+=$1} END{print s}' "${RUN_DIR}/cpu/.top_syms")
		TOP_CPU_SECTION=$({
			echo "- top_cpu_symbols:"
			awk -v t=${TOTAL_SAMPLES:-0} '{pct= (t>0? ($1*100.0)/t:0); printf "  * %s (%.2f%%, samples=%s)\n", $2, pct, $1}' "${RUN_DIR}/cpu/.top_syms"
		})
	fi
fi

# Build top Off-CPU symbols if available
TOP_OFFCPU_SECTION=""
OFF_FOLDED_FILE="${RUN_DIR}/offcpu/out.folded"
if [[ ! -f "${OFF_FOLDED_FILE}" && -f "${RUN_DIR}/offcpu/out.folded.full" ]]; then
	OFF_FOLDED_FILE="${RUN_DIR}/offcpu/out.folded.full"
fi
if [[ -f "${OFF_FOLDED_FILE}" ]]; then
	awk -F';' -v TOPN=${TOP_N_SYMBOLS} 'function trim(s){gsub(/^ +| +$/,"",s);return s} { line=$0; sub(/^[^ ]+ /,"",line); c=$NF; if(c+0==0){c=$(NF);}; c+=0; n=split($0,parts,";"); for(i=1;i<=n;i++){ split(parts[i],f," "); fn=trim(f[1]); if(fn!="") samples[fn]+=c; total+=c; } } END { if(total>0){ for(fn in samples) printf "%s %s\n", samples[fn], fn | "sort -nr" } }' "${OFF_FOLDED_FILE}" | head -${TOP_N_SYMBOLS} >"${RUN_DIR}/offcpu/.top_syms" || true
	if [[ -s "${RUN_DIR}/offcpu/.top_syms" ]]; then
		TOTAL_SAMPLES_OFF=$(awk '{s+=$1} END{print s}' "${RUN_DIR}/offcpu/.top_syms")
		TOP_OFFCPU_SECTION=$({
			echo "- top_offcpu_symbols:"
			awk -v t=${TOTAL_SAMPLES_OFF:-0} '{pct=(t>0? ($1*100.0)/t:0); printf "  * %s (%.2f%%, samples=%s)\n", $2, pct, $1}' "${RUN_DIR}/offcpu/.top_syms"
		})
	fi
fi
if [[ -f "${FOLDED_FILE}" ]]; then
	# Build inclusive sample counts per function
	TOP_TMP=$(awk -F';' '{n=split($0, parts, ";"); split(parts[n], lf, " "); cnt=lf[length(lf)]; if(cnt=="") cnt=0; cnt+=0; for(i=1;i<=n;i++){ split(parts[i], f, " "); fn=f[1]; samples[fn]+=cnt;} total+=cnt;} END {for (f in samples) printf "%s %s\n", samples[f], f > "/dev/stderr"; print total > "/dev/stdout"}' "${FOLDED_FILE}" 2>"${RUN_DIR}/cpu/.symcounts") || true
	TOTAL_SAMPLES=$(echo "${TOP_TMP}" | tail -n1)
	if [[ -f "${RUN_DIR}/cpu/.symcounts" && ${TOTAL_SAMPLES:-0} -gt 0 ]]; then
		TOP_CPU_SECTION=$({
			echo "- top_cpu_symbols:"
			sort -nr "${RUN_DIR}/cpu/.symcounts" | head -10 | awk -v t=${TOTAL_SAMPLES} '{pct=($1*100.0)/t; printf "  * %s (%.2f%%, samples=%s)\n", $2, pct, $1}'
		})
	fi
fi

# Append baseline metrics section note if not already appended (idempotent marker)
MARKER="<!-- BASELINE_RUN_${TIMESTAMP} -->"
if ! grep -q "${MARKER}" "${PLAN_FILE}"; then
	{
		echo "\n${MARKER}"
		echo "Baseline capture ${TIMESTAMP}:"
		echo "- avg_tick_ns=${AVG_TICK_NS} (~${MEAN_TICK_MS} ms)"
		echo "- snapshot_full_bytes_total=${FULL_BYTES} (count=${FULL_COUNT})"
		echo "- snapshot_delta_bytes_total=${DELTA_BYTES} (count=${DELTA_COUNT})"
		echo "- snapshot_full_mean_bytes=${MEAN_FULL_BYTES}"
		echo "- snapshot_delta_mean_bytes=${MEAN_DELTA_BYTES}"
		echo "- clients=${CLIENTS} duration=${DURATION}s port=${PORT}"
		echo "- cpu_profile=${RUN_DIR}/cpu/cpu_flame.svg (if generated)"
		echo "- offcpu_profile=${RUN_DIR}/offcpu/offcpu_flame.svg (if generated)"
		if [[ -n "${TOP_CPU_SECTION}" ]]; then echo "${TOP_CPU_SECTION}"; fi
		if [[ -n "${TOP_OFFCPU_SECTION}" ]]; then echo "${TOP_OFFCPU_SECTION}"; fi
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
Mean full snapshot bytes: ${MEAN_FULL_BYTES}
Mean delta snapshot bytes: ${MEAN_DELTA_BYTES}
EOF
