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

## Contributing
See `CONTRIBUTING.md` for guidelines (coding style, coroutine rules, tests, commit / PR format, dependency manifest updates). Issue and feature request templates are available via GitHub (bug reports & enhancements). Pull requests should include rationale, test evidence, and note any protocol or configuration key changes.
