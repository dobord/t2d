#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Phase 0 automation: run load, capture CPU & Off-CPU perf profiles, extract baseline metrics,
# and append baseline row into docs/performance_plan.md if not present.
set -euo pipefail

# Force stable numeric locale (avoid commas in decimals like 0,270)
export LC_NUMERIC=C

# Defaults (can be overridden by env or CLI flags)
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

usage() {
	cat <<EOF
Usage: $0 [--clients N] [--duration SEC] [--port PORT] [--cpu-prof SEC] [--offcpu-prof SEC] [--no-perf]
Environment overrides also respected: CLIENTS, DURATION, PORT, CPU_PROF_DUR, OFFCPU_PROF_DUR.
--no-perf sets both CPU and Off-CPU profiling durations to 0 (metrics only).
EOF
}

while [[ $# -gt 0 ]]; do
	case $1 in
	--clients)
		CLIENTS=$2
		shift 2
		;;
	--duration)
		DURATION=$2
		shift 2
		;;
	--port)
		PORT=$2
		shift 2
		;;
	--cpu-prof)
		CPU_PROF_DUR=$2
		shift 2
		;;
	--offcpu-prof)
		OFFCPU_PROF_DUR=$2
		shift 2
		;;
	--no-perf)
		CPU_PROF_DUR=0
		OFFCPU_PROF_DUR=0
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "Unknown arg: $1" >&2
		usage
		exit 1
		;;
	esac
done

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

###############################################
# Extract metrics from server.log (robust mode)
###############################################
SERVER_LOG=baseline_logs/server.log
AVG_TICK_NS=0 P99_TICK_NS=0 WAIT_P99_NS=0 CPU_USER_PCT=0 RSS_PEAK_BYTES=0 ALLOCS_PER_TICK_MEAN=0 ALLOCS_PER_TICK_P95=0
FULL_BYTES=0 DELTA_BYTES=0 FULL_COUNT=0 DELTA_COUNT=0
if [[ -f ${SERVER_LOG} ]]; then
	# Temporarily disable -e because grep returning 1 (no matches) would abort script
	set +e
	if grep -q '"metric":"runtime_final"' "${SERVER_LOG}" 2>/dev/null; then
		line=$(grep -E '"metric":"runtime_final"' "${SERVER_LOG}" | tail -1)
	else
		line=$(grep -E '"metric":"runtime"' "${SERVER_LOG}" | tail -1)
	fi
	if [[ -n "${line}" ]]; then
		AVG_TICK_NS=$(echo "${line}" | sed -E 's/.*"avg_tick_ns":([0-9]+).*/\1/' || echo 0)
		P99_TICK_NS=$(echo "${line}" | sed -E 's/.*"p99_tick_ns":([0-9]+).*/\1/' || echo 0)
		WAIT_P99_NS=$(echo "${line}" | sed -E 's/.*"wait_p99_ns":([0-9]+).*/\1/' || echo 0)
		CPU_USER_PCT=$(echo "${line}" | sed -E 's/.*"cpu_user_pct":([0-9\.]+).*/\1/' || echo 0)
		RSS_PEAK_BYTES=$(echo "${line}" | sed -E 's/.*"rss_peak_bytes":([0-9]+).*/\1/' || echo 0)
		ALLOCS_PER_TICK_MEAN=$(echo "${line}" | sed -E 's/.*"allocs_per_tick_mean":([0-9\.]+).*/\1/' || echo 0)
		ALLOCS_PER_TICK_P95=$(echo "${line}" | sed -E 's/.*"allocs_per_tick_p95":([0-9]+).*/\1/' || echo 0)
		# Ensure numeric (strip anything non-digit)
		if ! [[ ${ALLOCS_PER_TICK_P95} =~ ^[0-9]+$ ]]; then ALLOCS_PER_TICK_P95=0; fi
	fi
	# snapshot totals (may appear earlier, so separate grep)
	st_line=$(grep -E '"metric":"snapshot_totals"' "${SERVER_LOG}" | tail -1)
	if [[ -n "${st_line}" ]]; then
		FULL_BYTES=$(echo "${st_line}" | sed -E 's/.*"full_bytes":([0-9]+).*/\1/' || echo 0)
		DELTA_BYTES=$(echo "${st_line}" | sed -E 's/.*"delta_bytes":([0-9]+).*/\1/' || echo 0)
		FULL_COUNT=$(echo "${st_line}" | sed -E 's/.*"full_count":([0-9]+).*/\1/' || echo 0)
		DELTA_COUNT=$(echo "${st_line}" | sed -E 's/.*"delta_count":([0-9]+).*/\1/' || echo 0)
	fi
	set -e
else
	echo "[phase0][warn] server.log not found; metrics default to zero" >&2
fi

MEAN_TICK_MS=$(awk -v ns=${AVG_TICK_NS} 'BEGIN{ printf("%.3f", ns/1000000.0) }')
P99_TICK_MS=$(awk -v ns=${P99_TICK_NS:-0} 'BEGIN{ if(ns>0) printf("%.3f", ns/1000000.0); else printf("0.000"); }')
WAIT_P99_MS=$(awk -v ns=${WAIT_P99_NS:-0} 'BEGIN{ if(ns>0) printf("%.3f", ns/1000000.0); else printf("0.000"); }')
RSS_PEAK_MB=$(awk -v b=${RSS_PEAK_BYTES:-0} 'BEGIN{ printf("%.2f", b/1024/1024); }')
MEAN_FULL_BYTES=$(awk -v b=${FULL_BYTES} -v c=${FULL_COUNT} 'BEGIN{ if(c>0) printf("%.2f", b/c); else print 0 }')
MEAN_DELTA_BYTES=$(awk -v b=${DELTA_BYTES} -v c=${DELTA_COUNT} 'BEGIN{ if(c>0) printf("%.2f", b/c); else print 0 }')

# Extract top CPU symbols if folded stacks exist (inclusive approximation)
TOP_CPU_SECTION=""
FOLDED_FILE="${RUN_DIR}/cpu/out.folded"
if [[ ! -f "${FOLDED_FILE}" && -f "${RUN_DIR}/cpu/out.folded.full" ]]; then
	FOLDED_FILE="${RUN_DIR}/cpu/out.folded.full"
fi

TOP_CPU_SECTION=""
if [[ -f "${FOLDED_FILE}" ]]; then
	# Each line: stack;func;... count  -> inclusive add 'count' to every frame in stack.
	awk 'NF>1 {cnt=$NF; if(cnt+0==0) next; stack=$0; sub(/ [0-9]+$/,"",stack); n=split(stack,parts,";"); for(i=1;i<=n;i++){gsub(/^ +| +$/,"",parts[i]); if(parts[i]!="") inc[parts[i]]+=cnt} total+=cnt} END{for(f in inc) printf "%d\t%s\n", inc[f], f | "sort -nr"; if(total==0) total=1; print total > "/dev/stderr"}' "${FOLDED_FILE}" 2>"${RUN_DIR}/cpu/.total_samples" | head -${TOP_N_SYMBOLS} >"${RUN_DIR}/cpu/.top_syms" || true
	if [[ -s "${RUN_DIR}/cpu/.top_syms" && -s "${RUN_DIR}/cpu/.total_samples" ]]; then
		TOTAL_SAMPLES=$(cat "${RUN_DIR}/cpu/.total_samples" | tail -n1)
		TOP_CPU_SECTION=$({
			echo "- top_cpu_symbols:"
			awk -v t=${TOTAL_SAMPLES:-1} '{pct=($1*100.0)/t; printf "  * %s (%.2f%%, samples=%s)\n", $2, pct, $1}' "${RUN_DIR}/cpu/.top_syms"
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
	awk 'NF>1 {cnt=$NF; if(cnt+0==0) next; stack=$0; sub(/ [0-9]+$/,"",stack); n=split(stack,parts,";"); for(i=1;i<=n;i++){gsub(/^ +| +$/,"",parts[i]); if(parts[i]!="") inc[parts[i]]+=cnt} total+=cnt} END{for(f in inc) printf "%d\t%s\n", inc[f], f | "sort -nr"; if(total==0) total=1; print total > "/dev/stderr"}' "${OFF_FOLDED_FILE}" 2>"${RUN_DIR}/offcpu/.total_samples" | head -${TOP_N_SYMBOLS} >"${RUN_DIR}/offcpu/.top_syms" || true
	if [[ -s "${RUN_DIR}/offcpu/.top_syms" && -s "${RUN_DIR}/offcpu/.total_samples" ]]; then
		TOTAL_SAMPLES_OFF=$(cat "${RUN_DIR}/offcpu/.total_samples" | tail -n1)
		TOP_OFFCPU_SECTION=$({
			echo "- top_offcpu_symbols:"
			awk -v t=${TOTAL_SAMPLES_OFF:-1} '{pct=($1*100.0)/t; printf "  * %s (%.2f%%, samples=%s)\n", $2, pct, $1}' "${RUN_DIR}/offcpu/.top_syms"
		})
	fi
fi
if [[ -f "${FOLDED_FILE}" ]]; then
	# Build inclusive sample counts per function (robust to mawk); avoid reusing array name as loop var.
	TOP_TMP=$(awk -F';' '{
		n=split($0, parts, ";");
		split(parts[n], lastfields, " ");
		cnt=lastfields[length(lastfields)];
		if(cnt=="" || cnt ~ /[^0-9]/){cnt=0}; cnt+=0;
		for(i=1;i<=n;i++){
			split(parts[i], toks, " "); fn=toks[1]; if(fn!="") samples[fn]+=cnt;
		}
		total+=cnt;
	} END { for (fnm in samples) printf "%s %s\n", samples[fnm], fnm > "/dev/stderr"; print total > "/dev/stdout" }' "${FOLDED_FILE}" 2>"${RUN_DIR}/cpu/.symcounts") || true
	TOTAL_SAMPLES=$(echo "${TOP_TMP}" | tail -n1)
	if [[ -f "${RUN_DIR}/cpu/.symcounts" && ${TOTAL_SAMPLES:-0} -gt 0 ]]; then
		TOP_CPU_SECTION=$({
			echo "- top_cpu_symbols:"
			set +o pipefail
			sort -nr "${RUN_DIR}/cpu/.symcounts" | head -10 | awk -v t=${TOTAL_SAMPLES} '{pct=($1*100.0)/t; printf "  * %s (%.2f%%, samples=%s)\n", $2, pct, $1}' || true
			set -o pipefail
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
		echo "- p99_tick_ns=${P99_TICK_NS} (~${P99_TICK_MS} ms)"
		echo "- snapshot_full_bytes_total=${FULL_BYTES} (count=${FULL_COUNT})"
		echo "- snapshot_delta_bytes_total=${DELTA_BYTES} (count=${DELTA_COUNT})"
		echo "- snapshot_full_mean_bytes=${MEAN_FULL_BYTES}"
		echo "- snapshot_delta_mean_bytes=${MEAN_DELTA_BYTES}"
		echo "- wait_p99_ns=${WAIT_P99_NS} (~${WAIT_P99_MS} ms)"
		echo "- cpu_user_pct=${CPU_USER_PCT}"
		echo "- rss_peak_bytes=${RSS_PEAK_BYTES} (~${RSS_PEAK_MB} MB)"
		echo "- allocs_per_tick_mean=${ALLOCS_PER_TICK_MEAN}"
		echo "- allocs_per_tick_p95=${ALLOCS_PER_TICK_P95}"
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
P99 tick (ns): ${P99_TICK_NS}
P99 tick (ms): ${P99_TICK_MS}
Full snapshot bytes total: ${FULL_BYTES} (count ${FULL_COUNT})
Delta snapshot bytes total: ${DELTA_BYTES} (count ${DELTA_COUNT})
Mean full snapshot bytes: ${MEAN_FULL_BYTES}
Mean delta snapshot bytes: ${MEAN_DELTA_BYTES}
Wait p99 (ns): ${WAIT_P99_NS}
Wait p99 (ms): ${WAIT_P99_MS}
CPU user pct: ${CPU_USER_PCT}
RSS peak bytes: ${RSS_PEAK_BYTES} (~${RSS_PEAK_MB} MB)
Allocs per tick mean: ${ALLOCS_PER_TICK_MEAN}
Allocs per tick p95: ${ALLOCS_PER_TICK_P95}
EOF

# Update baseline table cells for collected metrics (only overwrite when we have non-zero values where applicable).
TMP_PLAN=$(mktemp)
awk -v p99="${P99_TICK_NS}" \
	-v ts="${TIMESTAMP}" \
	-v cpu="${CPU_USER_PCT}" \
	-v rssmb="${RSS_PEAK_MB}" \
	-v allocm="${ALLOCS_PER_TICK_MEAN}" \
	-v waitp="${WAIT_P99_NS}" 'BEGIN{FS=OFS="|"} {
	# Helper: trim leading/trailing spaces for reliable numeric test (awk auto-converts on compare)
	if($2 ~ /tick_duration_ns_p99/ && p99+0 > 0){
		$5 = " " p99 " (recent " ts ") ";
	} else if($2 ~ /CPU_user_pct/ && cpu+0 > 0){
		$5 = " " cpu " (recent " ts ") ";
	} else if($2 ~ /RSS_peak_MB/ && rssmb+0 > 0){
		$5 = " " rssmb " (recent " ts ") ";
	} else if($2 ~ /allocations_per_tick/ && allocm+0 > 0){
		$5 = " " allocm " (recent " ts ") ";
	} else if($2 ~ /off_cpu_wait_ns_p99/ && waitp+0 > 0){
		$5 = " " waitp " (recent " ts ") ";
	}
	print $0;
}' "${PLAN_FILE}" >"${TMP_PLAN}" && mv "${TMP_PLAN}" "${PLAN_FILE}" || true

exit 0
