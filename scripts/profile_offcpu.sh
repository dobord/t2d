#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

# Auto-escalate to sudo for perf if not root (can disable with NO_SUDO=1)
PERF_BIN=${PERF_BIN:-perf}
if [[ ${NO_SUDO:-0} -ne 1 && $EUID -ne 0 ]]; then
	if command -v sudo >/dev/null 2>&1; then
		PERF_BIN="sudo perf"
	fi
fi

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
STABILIZE_MS=${STABILIZE_MS:-300}
sleep $(awk -v ms=${STABILIZE_MS} 'BEGIN{printf("%.3f", ms/1000.0)}')
for i in {1..10}; do
	if [[ -d /proc/${PID} ]]; then break; fi
	sleep 0.05
done
OFF_ATTACH_OK=1
if ! ${PERF_BIN} record -e sched:sched_switch -g -p "${PID}" --output "${OUT_DIR}/perf.data" -- sleep "${DUR}"; then
	status=$?
	OFF_ATTACH_OK=0
	if [[ -f /proc/sys/kernel/perf_event_paranoid ]]; then
		PEV=$(cat /proc/sys/kernel/perf_event_paranoid || echo '?')
		echo "[offcpu] perf record attach failed (exit=${status}). perf_event_paranoid=${PEV}" >&2
	fi
	echo "[offcpu] Falling back to system-wide off-CPU capture (-a)." >&2
	if ! ${PERF_BIN} record -e sched:sched_switch -g -a --output "${OUT_DIR}/perf.data" -- sleep "${DUR}"; then
		echo "[offcpu] System-wide fallback also failed (exit=$?)." >&2
		exit ${status}
	fi
fi

if command -v stackcollapse-perf.pl >/dev/null 2>&1 && command -v flamegraph.pl >/dev/null 2>&1; then
	# Use the specific perf.data file path to avoid relying on CWD.
	${PERF_BIN} script -i "${OUT_DIR}/perf.data" >"${OUT_DIR}/perf.script"
	stackcollapse-perf.pl --kernel "${OUT_DIR}/perf.script" >"${OUT_DIR}/out.folded.full"
	if [[ ${OFF_ATTACH_OK:-1} -eq 0 ]]; then
		FALLBACK_FILTER=${FALLBACK_FILTER:-t2d_server}
		grep -F "${FALLBACK_FILTER}" "${OUT_DIR}/out.folded.full" >"${OUT_DIR}/out.folded" || true
		if [[ ! -s "${OUT_DIR}/out.folded" ]]; then
			cp "${OUT_DIR}/out.folded.full" "${OUT_DIR}/out.folded"
		fi
	else
		cp "${OUT_DIR}/out.folded.full" "${OUT_DIR}/out.folded"
	fi
	flamegraph.pl --title "Off-CPU" "${OUT_DIR}/out.folded" >"${OUT_DIR}/offcpu_flame.svg" || echo "[offcpu] flamegraph generation failed" >&2
	echo "FlameGraph: ${OUT_DIR}/offcpu_flame.svg"
	[[ ${OFF_ATTACH_OK:-1} -eq 0 ]] && echo "[offcpu] NOTE: System-wide fallback used; file filtered by '${FALLBACK_FILTER:-<none>}'" >&2
else
	echo "FlameGraph tools not found; skipping SVG generation." >&2
fi

echo "Done. Raw data: ${OUT_DIR}/perf.data"
