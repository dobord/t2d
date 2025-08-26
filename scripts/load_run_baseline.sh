#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Simple baseline load generator: launches server and spawns multiple test clients to join matches.
# Usage: ./scripts/load_run_baseline.sh [--clients 20] [--duration 60] [--port 40000]
set -euo pipefail

CLIENTS=12
DURATION=60
PORT=40000
CFG=config/server.yaml
SERVER_BIN=./t2d_server
CLIENT_BIN=./t2d_test_client
BUILD_DIR=${BUILD_DIR:-build-prof}
LOG_DIR=baseline_logs
EXTRA_SERVER_ARGS=""

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
	--config)
		CFG=$2
		shift 2
		;;
	--server-bin)
		SERVER_BIN=$2
		shift 2
		;;
	--client-bin)
		CLIENT_BIN=$2
		shift 2
		;;
	--)
		shift
		break
		;;
	*)
		echo "Unknown arg: $1"
		exit 1
		;;
	esac
done

mkdir -p "${LOG_DIR}" "${BUILD_DIR}" || true

# Resolve absolute config path early (from project root)
ABS_CFG_PATH=$(realpath "${CFG}")
if [[ ! -f "${ABS_CFG_PATH}" ]]; then
	echo "[load] Config not found: ${CFG}" >&2
	exit 1
fi

if [[ ! -x ${BUILD_DIR}/t2d_server || ! -x ${BUILD_DIR}/t2d_test_client ]]; then
	echo "[build] Building profiling binaries (server + test client)..." >&2
	# Configure if needed (idempotent)
	if [[ ! -f ${BUILD_DIR}/CMakeCache.txt ]]; then
		cmake -S . -B "${BUILD_DIR}" -DT2D_ENABLE_PROFILING=ON -DT2D_BUILD_TESTS=OFF -DT2D_BUILD_CLIENT=ON -DT2D_BUILD_QT_CLIENT=OFF >/dev/null
	fi
	cmake --build "${BUILD_DIR}" -j $(nproc) --target t2d_server t2d_test_client >/dev/null || {
		echo "[build] Build failed" >&2
		exit 1
	}
fi

# Safety: verify test client exists before spawning
if [[ ! -x ${BUILD_DIR}/t2d_test_client ]]; then
	echo "[load] Missing t2d_test_client after build; aborting" >&2
	exit 1
fi

pushd "${BUILD_DIR}" >/dev/null

# Launch server
LOG_SERVER="../${LOG_DIR}/server.log"
"${SERVER_BIN}" "${ABS_CFG_PATH}" >"${LOG_SERVER}" 2>&1 &
SERVER_PID=$!
echo ${SERVER_PID} >../${LOG_DIR}/server.pid
sleep 1

echo "[load] Spawning ${CLIENTS} clients..."
for i in $(seq 1 ${CLIENTS}); do
	"${CLIENT_BIN}" ${PORT} >"../${LOG_DIR}/client_${i}.log" 2>&1 &
	echo $! >>../${LOG_DIR}/clients.pid
	# small stagger to avoid thundering herd connect
	sleep 0.05
done

START_TS=$(date +%s)
END_TS=$((START_TS + DURATION))

echo "[load] Running for ${DURATION}s..."
while [[ $(date +%s) -lt ${END_TS} ]]; do
	sleep 1
	if ! kill -0 ${SERVER_PID} 2>/dev/null; then
		echo "[load] Server exited early." >&2
		break
	fi
done

echo "[load] Shutting down..."
kill -INT ${SERVER_PID} 2>/dev/null || true
sleep 2
# cleanup clients
if [[ -f ../${LOG_DIR}/clients.pid ]]; then
	while read -r CPID; do
		kill -INT "$CPID" 2>/dev/null || true
	done <../${LOG_DIR}/clients.pid
fi

popd >/dev/null

echo "[load] Logs in ${LOG_DIR}"
