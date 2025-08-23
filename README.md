# t2d (Tank 2D Multiplayer)

Authoritative 2D multiplayer tank game (server + (future) multi‑platform clients). Current focus: server core, deterministic match loop, networking, matchmaking, bots, snapshots & metrics.

## Status
Prototype (core gameplay & networking implemented). Physics (Box2D integration), advanced clients, compression & security still pending.

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
* Deterministic match loop (tick rate configurable) with bots (periodic firing) & naive movement
* Damage + projectile collision (naive circle overlap) & kill feed batching
* Delta & full snapshots (intervals configurable) with removal lists and size metrics
* Heartbeat timeout pruning + disconnect handling (entity removal & destroy events)
* Auth provider abstraction (stub strategy selectable via config)
* Metrics counters (Prometheus endpoint `/metrics`), snapshot size accumulation, runtime tick duration accumulation
* Structured logging (plain or JSON) with level from config
* Formatting enforcement (clang-format) + dependency manifest check in CI
* Code coverage job (lcov artifact)

## Current Limitations / TODO (High-Level)
* No snapshot compression yet (planned optional zlib / quantization)
* No Box2D physics (movement and collisions are simplified)
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

## License
TBD (pending; intend to choose a permissive OSI license before first tagged release)
