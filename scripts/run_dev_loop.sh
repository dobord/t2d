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
#  NO_BOT_FIRE=1 disable bot firing (passes --no-bot-fire to server)  # convenience wrapper for config/env
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

APP_ID="run_dev"
_ts(){ date '+%Y-%m-%d %H:%M:%S'; }
log(){ echo "[$(_ts)] [$APP_ID] [I] $*"; }
log_warn(){ echo "[$(_ts)] [$APP_ID] [W] $*"; }
log_err(){ echo "[$(_ts)] [$APP_ID] [E] $*" >&2; }
[[ "$VERBOSE" == 1 ]] && set -x

# Run code formatting targets before building (mandatory auto-format step)
run_format(){
  # Requires a configured build directory. Attempt aggregate target first.
  if [[ ! -d "$ROOT_DIR/$BUILD_DIR" ]]; then
    return 0
  fi
  log "Running code format targets"
  # Prefer aggregated format_all (it is constructed only from first-party format targets: t2d_format, format_cmake, format_qml)
  # This still avoids third_party because t2d_format already filters it out.
  if [[ "$VERBOSE" == 1 ]]; then
    if cmake --build "$ROOT_DIR/$BUILD_DIR" --target format_all -j $(nproc || echo 4); then
      log "Formatting completed (target=format_all)"; return 0; fi
    if cmake --build "$ROOT_DIR/$BUILD_DIR" --target t2d_format -j $(nproc || echo 4); then
      log "Formatting completed (target=t2d_format)"; return 0; fi
    if cmake --build "$ROOT_DIR/$BUILD_DIR" --target format -j $(nproc || echo 4); then
      log "Formatting completed (target=format)"; return 0; fi
  else
    if cmake --build "$ROOT_DIR/$BUILD_DIR" --target format_all -j $(nproc || echo 4) >/dev/null 2>&1; then
      log "Formatting completed (target=format_all)"; return 0; fi
    if cmake --build "$ROOT_DIR/$BUILD_DIR" --target t2d_format -j $(nproc || echo 4) >/dev/null 2>&1; then
      log "Formatting completed (target=t2d_format)"; return 0; fi
    if cmake --build "$ROOT_DIR/$BUILD_DIR" --target format -j $(nproc || echo 4) >/dev/null 2>&1; then
      log "Formatting completed (target=format)"; return 0; fi
  fi
  log_warn "No formatting target found (format_all/format/t2d_format). Skipping auto-format."
}

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
    # Reconfigure if qt_local.cmake is newer than cache (new Qt path or changed) to pick up qmlformat
    if [[ -f "$ROOT_DIR/qt_local.cmake" && -f "$ROOT_DIR/$BUILD_DIR/CMakeCache.txt" ]]; then
      if [[ "$ROOT_DIR/qt_local.cmake" -nt "$ROOT_DIR/$BUILD_DIR/CMakeCache.txt" ]]; then
        log "qt_local.cmake updated; reconfiguring to refresh Qt tool discovery"
        cmake -S "$ROOT_DIR" -B "$ROOT_DIR/$BUILD_DIR" -DT2D_BUILD_SERVER=ON -DT2D_BUILD_QT_CLIENT=ON ${CMAKE_ARGS}
      else
        # If qmlformat was previously NOTFOUND but now exists in one of prefix bin dirs, reconfigure.
        if grep -q 'QMLFORMAT_BIN:FILEPATH=.*-NOTFOUND' "$ROOT_DIR/$BUILD_DIR/CMakeCache.txt"; then
          # Quick heuristic: look for any qmlformat under prefixes in qt_local.cmake or CMAKE_PREFIX_PATH
          if grep -Eo '/[^" ]+/gcc_64' "$ROOT_DIR/qt_local.cmake" 2>/dev/null | while read -r p; do [[ -x "$p/bin/qmlformat" ]] && echo hit && break; done | grep -q hit; then
            log "qmlformat now present under qt_local prefix; reconfiguring to enable format_qml"
            cmake -S "$ROOT_DIR" -B "$ROOT_DIR/$BUILD_DIR" -DT2D_BUILD_SERVER=ON -DT2D_BUILD_QT_CLIENT=ON ${CMAKE_ARGS}
          fi
        fi
      fi
    fi
  fi
  log "Building targets"
  # Mandatory auto-format before compiling
  run_format || log_warn "Auto-format step encountered issues"
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
  local server_args=()
  if [[ "${NO_BOT_FIRE:-0}" == 1 ]]; then
    server_args+=("--no-bot-fire")
    log "Bot firing disabled via NO_BOT_FIRE=1"
  fi
  T2D_LOG_APP_ID="srv" "${SERVER_BIN}" "${server_args[@]}" &
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
