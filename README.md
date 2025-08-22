# t2d (Tank 2D Multiplayer) – Initial Skeleton

Authoritative 2D multiplayer tank game (server + multi-platform clients). This repository currently contains the early server skeleton, protobuf schemas, and configuration.

## Status
Prototype (pre-MVP). Networking, physics, and AI not yet implemented.

## Build (Server)

### Prerequisites
* CMake >= 3.22
* A C++20 compiler (GCC 12+/Clang 15+ recommended)
* Protobuf library / compiler (protoc) available in PATH
* Git (for FetchContent dependencies)

### Steps
```bash
cmake -S . -B build -DT2D_BUILD_SERVER=ON
cmake --build build -j
./build/t2d_server config/server.yaml
```

The server currently runs an idle loop placeholder.

## Project Layout
```
proto/        Protobuf message definitions
config/       YAML runtime configuration
src/server/   Server source code (skeleton)
docs/         Architecture, protocol, configuration docs
```

## Next Planned Work
1. Implement networking layer (accept connections, message framing).
2. Add matchmaking coroutine and queue management.
3. Introduce per-match simulation loop stub.
4. Integrate Box2D and basic tank movement.

## License
TBD
