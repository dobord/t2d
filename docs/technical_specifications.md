# t2d – Networked 2D Tank Battle Game (Technical Specifications)

> NOTE: All repository documentation must be in English (language policy).

## 1. Target Platforms
### Server
- Linux (Ubuntu 24.x) runtime packaged via Docker (Alpine Linux 3.20.6 base image).

### Clients
1. Android (Qt + NDK + QML)
2. Desktop: Linux (Ubuntu 22.04+) (other desktop platforms deferred)
3. WebAssembly (Qt for WASM) built inside Docker (Alpine 3.20.6 base)

## 2. Core Technology Stack
| Component | Technology | Purpose |
|-----------|-----------|---------|
| Language | C++20 | Core server & native client code |
| UI | Qt 6.8.3 + QML | Cross‑platform UI & rendering |
| Build | CMake | Multi‑platform build configuration |
| Protocol | Google Protocol Buffers | Network application protocol |
| Concurrency | libcoro | C++20 coroutine IO & scheduling |
| Config | yaml-cpp | Server configuration parsing |
| Auth | OAuth 2.0 | Player & admin authentication / authorization |
| Physics | Box2D | Authoritative physics (movement, turret rotation, projectiles, collisions) |

### 2.1 Dependency Versions & Sources
| Dependency | Version / Tag | URL | Notes / Rationale |
|------------|---------------|-----|-------------------|
| C++ Standard | C++20 | (compiler provided) | Required for coroutines & modern features |
| CMake | >= 3.22 | https://cmake.org | Matches project minimum in CMakeLists.txt |
| Qt | 6.8.3 | https://www.qt.io | Planned for cross‑platform UI (desktop, Android, WASM) |
| Protobuf | 3.21.12 | https://github.com/protocolbuffers/protobuf | Matches currently detected build version |
| libcoro | 1d472a8 (submodule) | https://github.com/dobord/libcoro | Async IO + coroutines (pinned via git submodule) |
| yaml-cpp | 2f86d13 (submodule) | https://github.com/jbeder/yaml-cpp | Configuration parsing (pinned via git submodule) |
| Box2D | af12713 (submodule) | https://github.com/erincatto/box2d | Physics engine (pinned via git submodule) |
| OpenSSL (optional future) | 3.x | https://www.openssl.org | TLS / secure transport (if enabled) |
| zstd (optional future) | 1.5.x | https://github.com/facebook/zstd | Snapshot compression (future optimization) |
| GoogleTest (tests) | 1.14.x (optional) | https://github.com/google/googletest | If expanded beyond ad‑hoc asserts |
| Emscripten (WASM) | latest SDK (pin) | https://emscripten.org | WebAssembly client toolchain |
| Android NDK | 29.0.13846066 | https://developer.android.com/ndk | Version from .env (pin exact for reproducibility) |
| Android Build Tools | 34.0.0 | https://developer.android.com/studio/releases/build-tools | Aligns with .env; ensure CI image has it installed |
| Gradle | 8.x | https://gradle.org | Android build orchestration |
| Docker | 27.x+ | https://www.docker.com | Container packaging (server & WASM) |
| docker-compose | v2.x | https://docs.docker.com/compose | Multi‑service deployment |

Pinning Policy:
- Core C/C++ third-party libraries are now vendored as git submodules with explicit commit SHAs (see table). Update cadence: on-demand (feature need or security fix) with a single PR updating SHA + changelog note.
- Before first public release: generate a `DEPENDENCIES.md` manifest including license summaries & all SHAs (placeholder task pending).
- Security updates: monthly review cycle; critical CVEs patched immediately.

License Compatibility:
- All listed libraries are permissive (MIT / BSD / Apache2) — maintain a NOTICE file before release.


## 3. Gameplay Overview
1. Player registers / authenticates (OAuth token).
2. Clicks "Random Battle".
3. Enters matchmaking waiting screen. Target match size = 16 (configurable).
4. If not full by timeout (default 180s) remaining slots filled with AI bots.
5. Each player controls a tank with limited ammo; ammo replenished by driving over randomly spawned ammo crates.
6. Players aim turret and fire rounds at enemy tanks to reduce HP.
7. Starting HP = 100 (configurable). At 0 HP the tank is destroyed (client explosion effect) and eliminated.
8. Win condition: last surviving tank (future: teams / additional modes).
9. In‑match HUD: kills, score, HP, ammo (extended stats later).

### Current / Planned Entities
- Tank (player / bot)
- Projectile
- Ammo box (static pickup; grants ammo; sent in full snapshots only; disappears when picked up)
- Crate (movable obstacle; full + delta snapshot coverage)
- (Future) Destructible objects / respawning pickups / terrain hazards

### Future Extensions
- Armor angle based damage modifiers
- Splash damage shells
- Team battles, spectator mode, persistent progression

## 4. Server Architecture
- Authoritative simulation tick loop (fixed rate)
- TCP + length‑prefixed protobuf frames (prototype)
- Baseline + periodic full + delta state snapshots
- Matchmaking queue + bot fill after timeout
- Heartbeat & disconnection pruning
- Outbound batching per connection

## 5. Physics (Planned Box2D)
- Dynamic body per tank (linear & angular constraints)
- Turret logical rotation (physics optional)
- Projectiles: raycast or fast body to avoid tunneling
- Ammo crates: static bodies with trigger pickup

### 5.1 Tank & Projectile Geometry (Prototype Spec)
The prototype uses a simplified descriptive shape for gameplay and rendering:

Tank (all units in world meters):
1. Hull: axis‑aligned rectangle 3.0 (width) x 6.0 (length). Length axis defines the forward direction (hull_angle = 0 faces +X).
2. Tracks: two rectangles running along the hull length, each 6.0 x 0.3, positioned flush against the long sides. Their centers are offset ±1.35 on the width axis ( (3.0 / 2) - (0.3 / 2) ). Tracks are currently a visual detail only (no separate collision fixtures in prototype).
3. Turret: circle radius 1.3 centered at the hull center (same origin as hull).
4. Barrel: rectangle 4.0 (length) x 0.1 (thickness) rigidly attached to the turret, extending forward (along +X after hull and turret rotations) from the turret center. Local barrel origin starts at turret center (i.e. barrel spans [0, 4.0] in its local +X).

Projectile:
- Rectangle 0.3 (length) x 0.1 (thickness); collision shape approximated by a Box2D box with half extents (0.15, 0.05). (Previously a 0.2x0.2 square.)

Collision Representation (current prototype):
- Tank: single Box2D box fixture with half extents (1.5, 3.0) matching the hull (rotation not yet dynamically updated — orientation handled logically for movement and firing).
- Projectile: single Box2D box with half extents (0.15, 0.05).

Spawn Offsets:
- Projectiles spawn at (tank_center + forward * 4.2) to appear just beyond the barrel tip (hull half length 3.0 + barrel length 4.0, slight extra to avoid immediate self‑collision) and reduce early contact artifacts.

Future Improvements:
- Add a separate (smaller) circular collision shape for the turret for more accurate hits.
- Dynamic rotation of the Box2D tank body to align with hull_angle (currently deferred for simplicity).
- Barrel raycast firing instead of spawning a physical projectile body for latency‑sensitive hit registration.

## 6. Networking & Protocol
Implemented messages: AuthRequest/Response, QueueJoin/Status, MatchStart, StateSnapshot, DeltaSnapshot, Heartbeat/Response, DamageEvent, TankDestroyed, KillFeedUpdate, MatchEnd.
Planned additions: PickupEvent, DisconnectNotice (explicit), future replay / debug control messages.

## 7. Authentication & Security
- OAuth token validation (pluggable module – not implemented yet, stub in place)
- Future: rate limiting, intrusion logging
- Authoritative server rejects inconsistent client state (only inputs accepted)

## 8. Bots & AI
- Queue fill bots for under‑populated matches
- Current AI: basic wandering + periodic fire
- Roadmap: target selection, pathfinding, difficulty tiers

## 9. Build & Packaging Scripts (Planned)
For each component (server, desktop, Android, WASM):
1. install_deps.sh – install build/runtime deps
2. build_full.sh – clean full rebuild
3. build_incremental.sh – incremental build (reuse cached libs)
4. test_unit.sh – run unit tests
5. test_e2e.sh – run end‑to‑end tests
6. package.sh – produce distributable artifact (tar/zip, APK, wasm bundle)
7. docker_build.sh – build & push Docker image (server / wasm)
8. release_tag.sh – create & push git tag to trigger CI release

## 10. Server Deployment Automation
Deployment script responsibilities:
1. Install server dependency packages
2. Configure OAuth (client IDs/secrets, email verification integration placeholder)
3. Deploy via docker-compose.yml (start or update stack)
4. Support upgrade: pull new image, start, health check, switch traffic, retire old
5. (Future) Rollback using previous tagged image

## 11. Observability (Planned)
- Structured JSON logging (level, timestamp, match_id, session_id)
- Metrics: tick duration, active matches, queue depth, snapshot sizes, bot count
- Optional Prometheus endpoint / sidecar exporter

## 12. Quality & Reliability
- clang-format enforcement
- clang-tidy advisory → gating
- Sanitizer builds (ASan/UBSan/TSan) nightly
- Fuzz harness for frame & delta parsers

## 13. Release Workflow
1. PR merges → snapshot artifacts (nightly) (future)
2. Git tag (vX.Y.Z) → multi‑platform build + artifact publish
3. Changelog generation (conventional commits – future)

## 14. Outstanding Work (High Level)
- Physics (Box2D) integration
- Damage model & events
- OAuth real validation
- Multi‑platform client scaffolds (desktop / Android / WASM)
- Deployment & build scripting

---
This document will be updated as core gameplay, platform targets, and infrastructure evolve.
