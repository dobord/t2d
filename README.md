# t2d (Tank 2D Multiplayer)

Authoritative 2D multiplayer tank game (server + (future) multi‑platform clients). Current focus: server core, deterministic match loop, networking, matchmaking, bots, snapshots & metrics.

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
| projectile_speed | 5.0 | Projectile speed (units/s) |
| projectile_damage | 25 | Damage per projectile contact |
| reload_interval_sec | 3.0 | Seconds to regenerate one ammo (up to max_ammo) |
| max_players_per_match | 16 | Match size (bot fill after timeout) |
| fill_timeout_seconds | 180 | Seconds before auto bot fill |
| heartbeat_timeout_seconds | 30 | Disconnect if no heartbeat within window |
| matchmaker_poll_ms | 200 | Matchmaker queue poll interval |
| log_level | info | Logging verbosity (env override) |
| log_json | false | Enable structured JSON logs |
| metrics_port | 9100 | Prometheus metrics endpoint (0 disables) |
| auth_mode | stub | Authentication provider mode |

Adjusting these allows rapid iteration on pacing (e.g., shorten `bot_fire_interval_ticks` to accelerate tests or increase `movement_speed` to test balance). Unknown keys are ignored; absent keys fall back to compiled defaults.

## License
Apache License 2.0. See `LICENSE` for full text.

All source files should begin with a short SPDX header, e.g.:
```cpp
// SPDX-License-Identifier: Apache-2.0
```

Copyright (c) 2025 dobord and contributors.
