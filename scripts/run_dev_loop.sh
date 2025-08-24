#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# run_dev_loop.sh - Build and (re)run t2d server + Qt client in a dev loop.
#
# Features:
#  - Kills previous instances (if any) before starting.
#  - Ensures CMake configured with Qt client enabled.
#  - Rebuilds incrementally (uses existing build dir or creates ./build).
#  - Waits for server TCP port before launching Qt client.
#  - Auto-restart loop: if either process exits unexpectedly, both are restarted.
#  - Ctrl-C cleanly stops both and exits.
#
# Environment / flags:
#  PORT (default 40000)
#  BUILD_DIR (default ./build)
#  CMAKE_ARGS (extra args, e.g. "-DT2D_ENABLE_SANITIZERS=ON")
#  LOOP=0 disables restart loop (single run)
#  NO_BUILD=1 skip build step (just run existing binaries)
#  VERBOSE=1 extra logging
#
# Examples:
#   ./scripts/run_dev_loop.sh
#   VERBOSE=1 LOOP=0 ./scripts/run_dev_loop.sh
#   BUILD_DIR=build-debug CMAKE_ARGS="-DT2D_BUILD_QT_CLIENT=ON -DT2D_ENABLE_TSAN=ON" ./scripts/run_dev_loop.sh
set -euo pipefail

PORT="${PORT:-40000}"
BUILD_DIR="${BUILD_DIR:-build}"
CMAKE_ARGS=${CMAKE_ARGS:-}
LOOP="${LOOP:-1}"
NO_BUILD="${NO_BUILD:-0}"
VERBOSE="${VERBOSE:-0}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="${ROOT_DIR}/${BUILD_DIR}/t2d_server"
CLIENT_BIN="${ROOT_DIR}/${BUILD_DIR}/t2d_qt_client"

log(){ echo "[run_dev] $*"; }
[[ "$VERBOSE" == 1 ]] && set -x

ensure_build(){
  if [[ "$NO_BUILD" == 1 ]]; then
    log "Skipping build (NO_BUILD=1)"
    return
  fi
  if [[ ! -d "$ROOT_DIR/$BUILD_DIR" || ! -f "$ROOT_DIR/$BUILD_DIR/CMakeCache.txt" ]]; then
    log "Configuring CMake build (dir=$BUILD_DIR)"
    mkdir -p "$ROOT_DIR/$BUILD_DIR"
    cmake -S "$ROOT_DIR" -B "$ROOT_DIR/$BUILD_DIR" -DT2D_BUILD_SERVER=ON -DT2D_BUILD_QT_CLIENT=ON ${CMAKE_ARGS}
  else
    # Ensure Qt client/server enabled; reconfigure if missing
    if [[ ! -f "$SERVER_BIN" || ! -f "$CLIENT_BIN" ]]; then
      log "Re-configuring to ensure required targets are enabled"
      cmake -S "$ROOT_DIR" -B "$ROOT_DIR/$BUILD_DIR" -DT2D_BUILD_SERVER=ON -DT2D_BUILD_QT_CLIENT=ON ${CMAKE_ARGS}
    fi
  fi
  log "Building targets"
  cmake --build "$ROOT_DIR/$BUILD_DIR" --target t2d_server t2d_qt_client -j $(nproc || echo 4)
}

kill_existing(){
  for p in t2d_server t2d_qt_client; do
    if pgrep -f "$p" >/dev/null 2>&1; then
      log "Killing existing $p"
      pkill -f "$p" || true
    fi
  done
}

wait_port(){
  local timeout=20
  local waited=0
  while ! bash -c "</dev/tcp/127.0.0.1/${PORT}" 2>/dev/null; do
    sleep 0.25
    waited=$((waited+1))
    if (( waited*25 >= timeout*1000 )); then
      log "Timeout waiting for port ${PORT}"; return 1
    fi
  done
  log "Server port ${PORT} is up"
}

run_once(){
  ensure_build
  kill_existing
  log "Starting server"
  T2D_LOG_APP_ID="srv" "${SERVER_BIN}" &
  SERVER_PID=$!
  log "Server PID=${SERVER_PID}"
  wait_port || { log "Server failed to open port"; kill ${SERVER_PID} || true; return 1; }
  log "Starting Qt client"
  T2D_LOG_APP_ID="qt" "${CLIENT_BIN}" &
  CLIENT_PID=$!
  log "Client PID=${CLIENT_PID}"
  wait -n ${SERVER_PID} ${CLIENT_PID}
  local exited=$?
  log "Process exit detected (code=${exited})"
  kill ${SERVER_PID} ${CLIENT_PID} 2>/dev/null || true
  wait ${SERVER_PID} 2>/dev/null || true
  wait ${CLIENT_PID} 2>/dev/null || true
  return 0
}

cleanup(){
  log "Cleaning up..."
  kill_existing
  exit 0
}
trap cleanup INT TERM

if [[ "$LOOP" == 0 ]]; then
  run_once
  log "Single run completed"
  exit 0
fi

log "Entering auto-restart loop (Ctrl-C to exit)"
while true; do
  run_once || log "Run encountered an error"
  log "Restarting in 1s..."
  sleep 1
done
