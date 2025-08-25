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
#  PORT (default 40000)                          flag: -p|--port <num>
#  BUILD_DIR (default ./build)                   flag: -d|--build-dir <dir>
#  BUILD_TYPE (default Debug)                    flag: -t|--build-type <type>, -r|--release (sets Release)
#  CMAKE_ARGS (extra cmake args)                 flag: --cmake-args "<args>" (can repeat)
#  LOOP=0 disables restart loop                  flag: --once (sets LOOP=0) or --loop <0|1>
#  NO_BUILD=1 skip build                         flag: --no-build
#  VERBOSE=1 extra logging                       flag: -v|--verbose
#  LOG_LEVEL (DEBUG|INFO|WARN|ERROR)             flag: --log-level <lvl>
#  QML_LOG_LEVEL (debug|info|warn|error)         flag: --qml-log-level <lvl>
#  NO_BOT_FIRE=1 disable bot firing              flag: --no-bot-fire
#  NO_BOT_AI=1 disable all bot AI (movement/aim/fire) flag: --no-bot-ai
#  -h|--help prints this help
#
# Examples:
#   ./scripts/run_dev_loop.sh
#   VERBOSE=1 LOOP=0 ./scripts/run_dev_loop.sh
#   BUILD_DIR=build-debug CMAKE_ARGS="-DT2D_BUILD_QT_CLIENT=ON -DT2D_ENABLE_TSAN=ON" ./scripts/run_dev_loop.sh
set -euo pipefail

PORT="${PORT:-40000}"
BUILD_DIR="${BUILD_DIR:-build}"
CMAKE_ARGS=${CMAKE_ARGS:-}
BUILD_TYPE="${BUILD_TYPE:-Debug}"
LOOP="${LOOP:-1}"
NO_BUILD="${NO_BUILD:-0}"
VERBOSE="${VERBOSE:-0}"
LOG_LEVEL="${LOG_LEVEL:-}"
QML_LOG_LEVEL="${QML_LOG_LEVEL:-}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="${ROOT_DIR}/${BUILD_DIR}/t2d_server"
CLIENT_BIN="${ROOT_DIR}/${BUILD_DIR}/t2d_qt_client"

APP_ID="run_dev"
_ts(){ date '+%Y-%m-%d %H:%M:%S'; }
_level_value(){
  case "$1" in
    DEBUG) echo 10;;
    INFO)  echo 20;;
    WARN)  echo 30;;
    ERROR) echo 40;;
    *) echo 20;;
  esac
}
_threshold=20 # provisional; updated after flag parsing
_should_log(){
  local sev=$1; local sv=$(_level_value "$sev");
  if [[ $sv -ge $_threshold ]]; then return 0; else return 1; fi
}
log(){ if _should_log INFO; then echo  "[$(_ts)] [$APP_ID] [I] $*"; fi; return 0; }
log_warn(){ if _should_log WARN; then echo "[$(_ts)] [$APP_ID] [W] $*"; fi; return 0; }
log_err(){ if _should_log ERROR; then echo "[$(_ts)] [$APP_ID] [E] $*" >&2; fi; return 0; }
log_debug(){ if _should_log DEBUG; then echo "[$(_ts)] [$APP_ID] [D] $*"; fi; return 0; }

# Parse flags (override env defaults)
print_help(){ sed -n '1,/^set -euo pipefail/p' "$0" | sed 's/^# \{0,1\}//' | grep -E '^(run_dev_loop|PORT|BUILD_DIR|BUILD_TYPE|CMAKE_ARGS|LOOP=|NO_BUILD|VERBOSE|LOG_LEVEL|QML_LOG_LEVEL|NO_BOT_FIRE|NO_BOT_AI|-p|Usage:| -r| -d| -t| --cmake-args| --no-build| --once| --loop| --log-level| --qml-log-level| --no-bot-fire| --no-bot-ai| -v)'; echo; echo "Example: $0 -d build-debug -p 40100 -r --no-bot-fire --no-bot-ai --cmake-args '-DT2D_ENABLE_SANITIZERS=ON'"; }

PENDING_CMAKE_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--port) PORT="$2"; shift 2;;
    -d|--build-dir) BUILD_DIR="$2"; shift 2;;
    -t|--build-type) BUILD_TYPE="$2"; shift 2;;
    -r|--release) BUILD_TYPE=Release; shift;;
    --cmake-args) PENDING_CMAKE_ARGS+=("$2"); shift 2;;
    --loop) LOOP="$2"; shift 2;;
    --once) LOOP=0; shift;;
    --no-build) NO_BUILD=1; shift;;
    -v|--verbose) VERBOSE=1; shift;;
    --log-level) LOG_LEVEL="$2"; shift 2;;
    --qml-log-level) QML_LOG_LEVEL="$2"; shift 2;;
    --no-bot-fire) NO_BOT_FIRE=1; shift;;
  --no-bot-ai) NO_BOT_AI=1; shift;;
    -h|--help) print_help; exit 0;;
    --) shift; break;;
    *) log_warn "Unknown argument: $1"; shift;;
  esac
done
if ((${#PENDING_CMAKE_ARGS[@]})); then
  # Append with space separation preserving ordering
  for a in "${PENDING_CMAKE_ARGS[@]}"; do
    if [[ -z "$CMAKE_ARGS" ]]; then CMAKE_ARGS="$a"; else CMAKE_ARGS+=" $a"; fi
  done
fi
# Finalize LOG_LEVEL now that VERBOSE/log-level flags parsed; export lowecase for server before it starts
if [[ -z "$LOG_LEVEL" ]]; then
  if [[ "$VERBOSE" == 1 ]]; then LOG_LEVEL=DEBUG; else LOG_LEVEL=INFO; fi
fi
export T2D_LOG_LEVEL="${LOG_LEVEL,,}"
_threshold=$(_level_value "$LOG_LEVEL")
[[ "$VERBOSE" == 1 ]] && set -x && log_debug "Shell trace enabled (VERBOSE=1)"
log_debug "Effective flags: PORT=$PORT BUILD_DIR=$BUILD_DIR BUILD_TYPE=$BUILD_TYPE LOOP=$LOOP NO_BUILD=$NO_BUILD VERBOSE=$VERBOSE LOG_LEVEL=$LOG_LEVEL QML_LOG_LEVEL=$QML_LOG_LEVEL NO_BOT_FIRE=${NO_BOT_FIRE:-0} NO_BOT_AI=${NO_BOT_AI:-0} CMAKE_ARGS='$CMAKE_ARGS'"

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
    cmake -S "$ROOT_DIR" -B "$ROOT_DIR/$BUILD_DIR" -DT2D_BUILD_SERVER=ON -DT2D_BUILD_QT_CLIENT=ON -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" ${CMAKE_ARGS}
  else
    # Ensure Qt client/server enabled; reconfigure if missing
    if [[ ! -f "$SERVER_BIN" || ! -f "$CLIENT_BIN" ]]; then
      log "Re-configuring to ensure required targets are enabled"
      cmake -S "$ROOT_DIR" -B "$ROOT_DIR/$BUILD_DIR" -DT2D_BUILD_SERVER=ON -DT2D_BUILD_QT_CLIENT=ON -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" ${CMAKE_ARGS}
    fi
    # Reconfigure if build type mismatch
    if grep -q '^CMAKE_BUILD_TYPE:' "$ROOT_DIR/$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
      CUR_BT=$(grep '^CMAKE_BUILD_TYPE:' "$ROOT_DIR/$BUILD_DIR/CMakeCache.txt" | cut -d= -f2)
      if [[ "$CUR_BT" != "$BUILD_TYPE" ]]; then
        log "Switching build type: $CUR_BT -> $BUILD_TYPE (re-configuring)"
        cmake -S "$ROOT_DIR" -B "$ROOT_DIR/$BUILD_DIR" -DT2D_BUILD_SERVER=ON -DT2D_BUILD_QT_CLIENT=ON -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" ${CMAKE_ARGS}
      fi
    fi
    # Reconfigure if qt_local.cmake is newer than cache (new Qt path or changed) to pick up qmlformat
    if [[ -f "$ROOT_DIR/qt_local.cmake" && -f "$ROOT_DIR/$BUILD_DIR/CMakeCache.txt" ]]; then
      if [[ "$ROOT_DIR/qt_local.cmake" -nt "$ROOT_DIR/$BUILD_DIR/CMakeCache.txt" ]]; then
        log "qt_local.cmake updated; reconfiguring to refresh Qt tool discovery"
        cmake -S "$ROOT_DIR" -B "$ROOT_DIR/$BUILD_DIR" -DT2D_BUILD_SERVER=ON -DT2D_BUILD_QT_CLIENT=ON -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" ${CMAKE_ARGS}
      else
        # If qmlformat was previously NOTFOUND but now exists in one of prefix bin dirs, reconfigure.
        if grep -q 'QMLFORMAT_BIN:FILEPATH=.*-NOTFOUND' "$ROOT_DIR/$BUILD_DIR/CMakeCache.txt"; then
          # Quick heuristic: look for any qmlformat under prefixes in qt_local.cmake or CMAKE_PREFIX_PATH
          if grep -Eo '/[^" ]+/gcc_64' "$ROOT_DIR/qt_local.cmake" 2>/dev/null | while read -r p; do [[ -x "$p/bin/qmlformat" ]] && echo hit && break; done | grep -q hit; then
            log "qmlformat now present under qt_local prefix; reconfiguring to enable format_qml"
            cmake -S "$ROOT_DIR" -B "$ROOT_DIR/$BUILD_DIR" -DT2D_BUILD_SERVER=ON -DT2D_BUILD_QT_CLIENT=ON -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" ${CMAKE_ARGS}
          fi
        fi
      fi
    fi
  fi
  log "Building targets"
  # Mandatory auto-format before compiling
  run_format || log_warn "Auto-format step encountered issues"
  # Run build with explicit failure handling to emit clear error message instead of silent set -e exit
  if ! cmake --build "$ROOT_DIR/$BUILD_DIR" --target t2d_server t2d_qt_client -j $(nproc || echo 4); then
    log_err "Build failed for targets: t2d_server t2d_qt_client (see errors above). Aborting."
    exit 1
  fi
  # Verify expected binaries exist/executable
  if [[ ! -x "$SERVER_BIN" || ! -x "$CLIENT_BIN" ]]; then
    log_err "Build finished but expected binaries missing: $([[ ! -x $SERVER_BIN ]] && echo t2d_server ) $([[ ! -x $CLIENT_BIN ]] && echo t2d_qt_client )"
    exit 1
  fi
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
  # T2D_LOG_LEVEL already exported earlier; just log if verbose
  log_debug "T2D_LOG_LEVEL=${T2D_LOG_LEVEL} (propagated)"
  if [[ "${NO_BOT_FIRE:-0}" == 1 ]]; then
    server_args+=("--no-bot-fire")
    log "Bot firing disabled via NO_BOT_FIRE=1"
  fi
  if [[ "${NO_BOT_AI:-0}" == 1 ]]; then
    server_args+=("--no-bot-ai")
    log "Bot AI disabled via NO_BOT_AI=1"
  fi
  T2D_LOG_APP_ID="srv" "${SERVER_BIN}" "${server_args[@]}" &
  SERVER_PID=$!
  log "Server PID=${SERVER_PID}"
  wait_port || { log "Server failed to open port"; kill ${SERVER_PID} || true; return 1; }
  log "Starting Qt client"
  local client_args=()
  # Determine QML log level argument (case-insensitive). QML side parser handles lowercase.
  if [[ -n "$QML_LOG_LEVEL" ]]; then
    client_args+=("--qml-log-level=${QML_LOG_LEVEL,,}")
  elif [[ -n "$LOG_LEVEL" ]]; then
    client_args+=("--qml-log-level=${LOG_LEVEL,,}")
  fi
  log_debug "Qt client args: ${client_args[*]:-(none)}"
  T2D_LOG_APP_ID="qt" "${CLIENT_BIN}" "${client_args[@]}" &
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
