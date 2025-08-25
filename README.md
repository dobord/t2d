# t2d (Tank 2D Multiplayer)

![CI](https://github.com/dobord/t2d/actions/workflows/ci.yml/badge.svg)
![Coverage](https://github.com/dobord/t2d/actions/workflows/coverage.yml/badge.svg)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![box2d v3.1.1](https://img.shields.io/badge/box2d-v3.1.1-forestgreen)
![yaml--cpp 0.8.0](https://img.shields.io/badge/yaml--cpp-0.8.0-orange)
![libcoro fix/skip_linking_pthread_on_android](https://img.shields.io/badge/libcoro-fix__skip__linking__pthread__on__android-purple)

Authoritative 2D multiplayer tank game (server + (future) clients). Current focus: server core, deterministic match loop, networking, matchmaking, bots, snapshots & metrics. Desktop client prototype currently builds only on Linux (other platforms dropped for now to reduce CI surface).

## Status
Prototype (core gameplay & networking implemented). Box2D physics (movement + projectile→tank contacts) integrated; advanced clients, compression & extended security still pending.

## Build (Server)

### Prerequisites
* CMake >= 3.22
* A C++20 compiler (GCC 12+/Clang 15+ recommended)
* Protobuf library / compiler (protoc) available in PATH
* Git (for submodule dependencies)
* Run once after clone (or when submodule SHAs update):
```bash
git submodule update --init --recursive
```

### Steps
```bash
cmake -S . -B build -DT2D_BUILD_SERVER=ON
cmake --build build -j
./build/t2d_server config/server.yaml
```

Optional flags:
* `-DT2D_ENABLE_SANITIZERS=ON` (ASan/UBSan where supported)
* `-DT2D_ENABLE_COVERAGE=ON` (Debug builds; GCC/Clang)

Third-party tests/examples/tools are force-disabled (yaml-cpp, libcoro, box2d, c-ares) for lean builds.

## Qt Desktop Client (Experimental UI)
Qt/QML client (Linux only) with tank & projectile list models, 2D canvas rendering and temporal interpolation between server snapshots.

Prerequisites: Qt 6 (>=6.5; tested with 6.8.3), components Quick/Qml/Gui/Core in PATH (qmake/qt-config not required if CMake package config files installed).

Build:
```bash
cmake -S . -B build-qt -DT2D_BUILD_QT_CLIENT=ON -DT2D_BUILD_SERVER=OFF
cmake --build build-qt -j
./build-qt/t2d_qt_client  # connect to localhost:40000 (ensure server running)
```

Run server separately in another terminal:
```bash
cmake -S . -B build -DT2D_BUILD_SERVER=ON
cmake --build build -j
./build/t2d_server config/server.yaml
```

Notes:
* QML module URI: `T2DClient`; main file embedded via `qt_add_qml_module` when available.
* Current UI: tank & projectile lists (id, pos, hp, ammo), canvas rendering (interpolated positions), basic input controls (move/turn/turret/fire) bound to network loop.
* Falls back gracefully if Qt not found (warning & target skipped).

## Features Implemented (Snapshot)
* Protobuf protocol: Auth, Queue, MatchStart, StateSnapshot, DeltaSnapshot, DamageEvent, KillFeedUpdate, TankDestroyed, MatchEnd, Heartbeat/Response
* TCP framing + streaming parser (incremental, handles partial frames)
* Matchmaking queue with bot fill after timeout & single-player allowance
* Deterministic match loop (tick rate configurable) with bots (configurable firing interval) & physics‑based movement (Box2D)
* Damage + projectile collision via Box2D contact events & kill feed batching
* Delta & full snapshots (intervals configurable) with removal lists and size metrics
* Heartbeat timeout pruning + disconnect handling (entity removal & destroy events)
* Auth provider abstraction (stub strategy selectable via config)
* Metrics counters (Prometheus endpoint `/metrics`), snapshot size accumulation, runtime tick duration accumulation
* Structured logging (plain or JSON) with level from config
* Formatting enforcement (clang-format) + dependency manifest check in CI
* Code coverage job (lcov artifact)

## Current Limitations / TODO (High-Level)
* No snapshot compression yet (planned optional zlib / quantization)
* Physics lacks advanced features (no terrain, raycast firing line, crate pickups yet)
* No real authentication (OAuth adapter pending)
* Desktop / Android / WASM clients not yet implemented (prototype test client only)
* No persistence, matchmaking skill/ELO, or replay/validation harness

## Roadmap
High-level, non-binding plan (subject to change). Earlier items are higher priority. Contribution proposals should reference which item they target or justify new items.

### Short Term (Prototype Hardening)
- [x] Implement optional snapshot compression (zlib; config flag disabled by default)
- [x] Add basic auth adapter abstraction implementation (token verification stub -> pluggable OAuth)
 - [x] Improve matchmaking: lobby countdown + dynamic bot fill pacing
- Deterministic snapshot diff validation test (encode → apply → state equivalence)
- Metrics: histogram for tick duration, gauge for connected players per match
- Add zlib dependency integration (guarded) & compression size metrics
- Strengthen TCP framing fuzz/unit tests (malformed length, truncation)
- Basic disconnect / reconnect (session resume within short timeout)

### Mid Term (Feature Expansion)
- Desktop client improvements (rendering, input smoothing)
- Android client prototype using shared protocol
- Web/WASM experimental client (Emscripten build pipeline)
- Projectile / physics enhancements (raycast firing line, obstacles/terrain tiles)
- Power-ups / pickups prototype (ammo crate, health pack)
- Persistent player identifiers + simple stats (matches played, kills)
- Replay / record raw snapshots for offline validation
- Matchmaking ELO / skill rating draft (config toggle)
- Optional TLS for client connections (behind build flag)

### Long Term (Scalability & Polish)
- Multi-match shard orchestration (process pool or cluster controller)
- State compression research (quantization, dictionary / delta chain compaction)
- Cheat resistance: server-side movement reconciliation + sanity heuristics
- Observability: tracing spans (open telemetry) around tick phases
- Replay playback & determinism verification pipeline in CI
- Load testing harness (synthetic clients; performance dashboards)
- Advanced authentication (OAuth provider integration, refresh tokens)
- Spectator mode (read-only snapshot stream)
- In-game chat (moderation + rate limiting)

### Exploratory / Nice-to-Have
- AI bot strategy improvements (path planning, target prioritization)
- Map editor & multiple map rotation
- Procedural map generation experiment
- Cross-play session handoff (desktop ↔ mobile) with state sync

If you plan to work on a roadmap item, open an issue first to avoid duplication and discuss scope.

## Project Layout
```
proto/        Protobuf message definitions
config/       YAML runtime configuration
src/server/   Server source code (skeleton)
docs/         Architecture, protocol, configuration docs
```

## Development / Testing
Run all tests:
```bash
cmake -S . -B build -DT2D_BUILD_TESTS=ON -DT2D_BUILD_CLIENT=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
cd build && ctest --output-on-failure
```

Formatting check (fails CI if diff):
```bash
scripts/format_check.sh            # verify
scripts/format_check.sh --apply    # auto-fix
```

### Formatting & Tooling Overview
First‑party sources (C/C++, QML, CMake) are auto‑formatted via a layered setup:

| Target / Script | Purpose | Notes |
|-----------------|---------|-------|
| `t2d_format` | clang-format on C/C++ (excludes `third_party/`) | Always safe to run |
| `format_qml` | Runs `qmlformat` on QML files | Created only if Qt's `qmlformat` located |
| `format_cmake` (if present) | Formats CMakeLists / *.cmake | Requires `cmake-format` |
| `format_all` | Aggregates the above found targets | Only present when at least one underlying target exists |
| `scripts/install_precommit_format_hook.sh` | Installs git pre-commit that formats staged & modified tracked C/C++/QML + injects SPDX if missing | Honors `T2D_FORMAT_BLOCK` / `t2d.formatBlock` and `T2D_QML_FORMAT_REQUIRED` |

Pre-commit hook discovery of `qmlformat` order:
1. PATH lookup
2. CMake cache variable `QMLFORMAT_BIN`
3. `qt_local.cmake` custom prefix (e.g. `/opt/Qt/6.8.3/gcc_64`)
4. On QML change without tool: a light CMake reconfigure is attempted

Environment flags affecting the hook:
| Variable | Effect |
|----------|--------|
| `T2D_PRECOMMIT_DEBUG=1` | Enables bash `set -x` tracing inside hook |
| `T2D_QML_FORMAT_REQUIRED=1` | Fail commit if QML changed but `qmlformat` not found |
| `T2D_FORMAT_BLOCK=1` or git config `t2d.formatBlock=true` | Abort commit after formatting (user must re-stage) |

### Dev Loop Script (`scripts/run_dev_loop.sh`)
Automated local iteration helper (server + Qt client auto-restart).

Environment variables / CLI flags (all can be set as env vars or via flags shown):

| Var / Flag | Default | Values | Effect |
|------------|---------|--------|--------|
| `PORT` / `-p <num>` | 40000 | int | Server listen & client connect port |
| `BUILD_DIR` / `-d <dir>` | build | path | Build directory containing artifacts |
| `BUILD_TYPE` / `-t <type>` / `-r` | Debug | Debug/Release/RelWithDebInfo/etc. | CMake build type ( `-r` forces Release ) |
| `CMAKE_ARGS` / `--cmake-args "..."` | (empty) | string | Extra CMake configure args (repeat flag to append) |
| `LOOP` / `--once` / `--loop <0|1>` | 1 | 0/1 | Auto-restart loop (0 = single run) |
| `NO_BUILD` / `--no-build` | 0 | 0/1 | Skip build step (assumes binaries present) |
| `VERBOSE` / `-v` | 0 | 0/1 | Enables bash trace + sets default LOG_LEVEL=DEBUG |
| `LOG_LEVEL` / `--log-level <lvl>` | INFO | TRACE/DEBUG/INFO/WARN/ERROR | Script log filtering + seeds `T2D_LOG_LEVEL` (lowercased) if that env unset |
| `QML_LOG_LEVEL` / `--qml-log-level <lvl>` | (inherits LOG_LEVEL) | trace/debug/info/warn/error | Passed to Qt client `--qml-log-level` |
| `NO_BOT_FIRE` / `--no-bot-fire` | 0 | 0/1 | Disables bot firing (server flag) |
| `NO_BOT_AI` / `--no-bot-ai` | 0 | 0/1 | Disables all bot movement/aim/fire (server flag) |

Script log levels (LOG_LEVEL) are independent from C++ runtime levels but propagate: the script exports `T2D_LOG_LEVEL` (in lowercase) for server & client if not already set externally. TRACE is the most verbose.

Generated/consumed runtime environment for server/client inside the loop:

| Exported | Purpose |
|----------|---------|
| `T2D_LOG_LEVEL` | Minimum C++ logger level (trace→error) |
| `T2D_LOG_APP_ID` | Set per process (`srv` / `qt`) for log prefixes |

Timestamps in script logs include milliseconds aligning with C++ logger output.

Behavior:
* Runs formatting targets before each build (`format_all` → fallback chain).
* Reconfigures CMake when `qt_local.cmake` timestamp is newer or a newly found `qmlformat` becomes available.
* Provides explicit build failure message instead of silent exit.

### Qt Local Path Override (`qt_local.cmake`)
Place a `qt_local.cmake` at repo root to append custom Qt install prefixes to `CMAKE_PREFIX_PATH` before `find_package(Qt6 ...)` runs. Example snippet:
```cmake
# qt_local.cmake (not committed normally)
set(CMAKE_PREFIX_PATH "/opt/Qt/6.8.3/gcc_64" ${CMAKE_PREFIX_PATH})
```
The pre-commit hook and CMake tool discovery reuse these paths when searching for `qmlformat` and related Qt helpers.

### Logging (C++ & QML)
The C++ logger (see `src/common/logger.hpp`) supports:
* Levels: trace / debug / info / warn / error via `T2D_LOG_LEVEL` env (trace is the most verbose)
* Millisecond timestamp precision in plain and JSON formats
* JSON mode when `T2D_LOG_JSON` is set
* Optional per-process `T2D_LOG_APP_ID` tag (e.g. `srv`, `qt`)

Qt/QML side provides parallel log helpers with level filtering. Level precedence:
1. Command line `--qml-log-level=<level>` passed to `t2d_qt_client`
2. Fallback: `--log-level=<level>` (if provided) or inherited environment via C++ injection

Runtime examples:
```bash
./build/t2d_server config/server.yaml &
T2D_LOG_LEVEL=debug ./build/t2d_qt_client --qml-log-level=warn
# Full tracing (very verbose):
T2D_LOG_LEVEL=trace ./build/t2d_qt_client --qml-log-level=trace
```

Network input debug spam can be silenced by elevating QML log level above DEBUG.

### Command Line Logging Flags
* `--log-level=<level>`: parsed by C++ client (unless `T2D_LOG_LEVEL` already set) to set backend logger level.
* `--qml-log-level=<level>`: QML UI logging filter; if absent, reuse `--log-level`.

### Environment Variables Summary (Runtime)
| Env Var | Component | Purpose |
|---------|-----------|---------|
| `T2D_LOG_LEVEL` | server / client | Minimum C++ log level (trace|debug|info|warn|error) |
| `T2D_LOG_JSON` | server / client | Enable JSON log output |
| `T2D_LOG_APP_ID` | both | Short tag in log prefix |
| `T2D_QML_FORMAT_REQUIRED` | git hook | Enforce presence of `qmlformat` when QML changes |
| `T2D_PRECOMMIT_DEBUG` | git hook | Verbose trace of formatting hook |
| `T2D_FORMAT_BLOCK` | git hook | Abort commit after formatting changes |
| `QML_LOG_LEVEL` | dev loop | Explicit QML log level forwarded to client |
| `LOG_LEVEL` | dev loop | Script verbosity and default QML level fallback |

More details in `docs/tooling.md`.

Coverage (locally):
```bash
cmake -S . -B build-cov -DT2D_BUILD_TESTS=ON -DT2D_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build-cov -j
cd build-cov && ctest --output-on-failure
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' 'third_party/*' --output-file coverage.info
genhtml coverage.info -o cov_html
```

## Metrics (Prometheus Style)
Enable by setting `metrics_port` in `config/server.yaml`. Exposes counters for snapshot bytes, counts, runtime tick durations, queue depth, active matches, bots, projectiles, auth failures, etc.

## Gameplay & Tuning Configuration
Gameplay and server behavior are data‑driven via `config/server.yaml`. Key parameters (defaults shown):

| Key | Default | Purpose |
|-----|---------|---------|
| tick_rate | 30 | Simulation ticks per second |
| snapshot_interval_ticks | 5 | Interval between delta snapshots |
| full_snapshot_interval_ticks | 30 | Interval between mandatory full snapshots |
| bot_fire_interval_ticks | 60 | Bot firing cadence (ticks; 60 @30Hz ≈ 2s) |
| movement_speed | 2.0 | Tank linear speed (units/s) |
| projectile_speed | 15.0 | Projectile speed (units/s) |
| projectile_damage | 25 | Damage per projectile contact |
| reload_interval_sec | 3.0 | Seconds to regenerate one ammo (up to max_ammo) |
| fire_cooldown_sec | 1.0 | Seconds between player shots (tank cannon) |
| max_players_per_match | 16 | Match size (bot fill after timeout) |
| fill_timeout_seconds | 180 | Seconds before auto bot fill |
| heartbeat_timeout_seconds | 30 | Disconnect if no heartbeat within window |
| matchmaker_poll_ms | 200 | Matchmaker queue poll interval |
| log_level | info | Logging verbosity (env override) |
| log_json | false | Enable structured JSON logs |
| metrics_port | 9100 | Prometheus metrics endpoint (0 disables) |
| auth_mode | stub | Authentication provider mode |

Adjusting these allows rapid iteration on pacing (e.g., shorten `bot_fire_interval_ticks` to accelerate tests or increase `movement_speed` to test balance). Unknown keys are ignored; absent keys fall back to compiled defaults.

### Test Configuration
For automated or rapid local tests a separate `config/server_test.yaml` is provided with faster regen, lower cooldowns and `test_mode: true` (enables internal clamps like minimum bot fire interval and elevated projectile damage for concise matches). Launch with:
```
./build/t2d_server config/server_test.yaml
```

## License
Apache License 2.0. See `LICENSE` for full text.

All source files should begin with a short SPDX header, e.g.:
```cpp
// SPDX-License-Identifier: Apache-2.0
```

Copyright (c) 2025 dobord and contributors.

## Contributing
See `CONTRIBUTING.md` for guidelines (coding style, coroutine rules, tests, commit / PR format, dependency manifest updates). Issue and feature request templates are available via GitHub (bug reports & enhancements). Pull requests should include rationale, test evidence, and note any protocol or configuration key changes.
