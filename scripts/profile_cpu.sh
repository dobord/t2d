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

STABILIZE_MS=${STABILIZE_MS:-300}

if [[ -n "${CMD}" ]]; then
	echo "[profile] Launching target command under perf..."
	${PERF_BIN} record -F "${FREQ}" -g --output "${OUT_DIR}/perf.data" -- ${CMD} &
	PERF_PID=$!
	sleep "${DUR}" || true
	kill -INT ${PERF_PID} 2>/dev/null || true
else
	echo "[profile] Attaching to PID ${PID}..."
	# Small retry loop to avoid race where process not fully visible to perf yet.
	for i in {1..10}; do
		if [[ -d /proc/${PID} ]]; then break; fi
		sleep 0.05
		[[ $i -eq 10 ]] && echo "[profile] PID ${PID} not found in /proc after retries" >&2
	done
	# Extra stabilization sleep to let loader finish mapping (optional)
	sleep $(awk -v ms=${STABILIZE_MS} 'BEGIN{printf("%.3f", ms/1000.0)}')
	ATTACH_OK=1
	if ! ${PERF_BIN} record -F "${FREQ}" -g -p "${PID}" --output "${OUT_DIR}/perf.data" -- sleep "${DUR}"; then
		status=$?
		ATTACH_OK=0
		if [[ -f /proc/sys/kernel/perf_event_paranoid ]]; then
			PEV=$(cat /proc/sys/kernel/perf_event_paranoid || echo '?')
			echo "[profile] perf record attach failed (exit=${status}). perf_event_paranoid=${PEV}" >&2
		fi
		# Fallback: system-wide capture to still produce a flamegraph; user can filter later.
		echo "[profile] Falling back to system-wide capture (-a)." >&2
		if ! ${PERF_BIN} record -F "${FREQ}" -g -a --output "${OUT_DIR}/perf.data" -- sleep "${DUR}"; then
			echo "[profile] System-wide perf capture also failed (exit=$?)." >&2
			exit ${status}
		fi
		FALLBACK_FILTER=${FALLBACK_FILTER:-t2d_server}
	fi
fi

echo "[profile] Generating folded stacks..."
if command -v stackcollapse-perf.pl >/dev/null 2>&1 && command -v flamegraph.pl >/dev/null 2>&1; then
	# Explicitly point perf script at the recorded data file; otherwise it looks in CWD.
	${PERF_BIN} script -i "${OUT_DIR}/perf.data" >"${OUT_DIR}/perf.script"
	stackcollapse-perf.pl "${OUT_DIR}/perf.script" >"${OUT_DIR}/out.folded.full"
	if [[ ${ATTACH_OK:-1} -eq 0 && -n "${FALLBACK_FILTER:-}" ]]; then
		grep -F "${FALLBACK_FILTER}" "${OUT_DIR}/out.folded.full" >"${OUT_DIR}/out.folded" || true
		if [[ ! -s "${OUT_DIR}/out.folded" ]]; then
			cp "${OUT_DIR}/out.folded.full" "${OUT_DIR}/out.folded"
		fi
	else
		cp "${OUT_DIR}/out.folded.full" "${OUT_DIR}/out.folded"
	fi
	flamegraph.pl "${OUT_DIR}/out.folded" >"${OUT_DIR}/cpu_flame.svg" || echo "[profile] flamegraph generation failed" >&2
	echo "FlameGraph: ${OUT_DIR}/cpu_flame.svg"
	[[ ${ATTACH_OK:-1} -eq 0 ]] && echo "[profile] NOTE: System-wide fallback used; file filtered by '${FALLBACK_FILTER:-<none>}'" >&2
else
	echo "FlameGraph tools not found; install from https://github.com/brendangregg/FlameGraph" >&2
fi

echo "Done. Raw data: ${OUT_DIR}/perf.data"
