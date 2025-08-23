# Project Plan (Authoritative Tank Game)

All documentation in this repository is maintained in English (see repository style policy). This document summarizes what has been completed so far and outlines the roadmap to fully automated server & client builds (artifacts) in the GitHub CI pipeline.

## 1. Completed Milestones (Checklist)
- [x] Architectural planning (authoritative server, matchmaking, coroutines, protobuf, YAML config)
- [x] Initial scaffold (CMake, proto, config loader, listener, session manager)
- [x] TCP framing & streaming parser + tests
- [x] Matchmaking queue & session lifecycle
- [x] Match loop baseline (movement, periodic full snapshots, baseline tick=0)
- [x] Input handling (authoritative state updates)
- [x] Heartbeat & monitoring (HeartbeatResponse + stale pruning)
- [x] Outbound batching of server messages
- [x] Logger (env‑filtered levels)
- [x] Bot fill after timeout (queue auto completion)
- [x] Basic Bot AI (wander + periodic firing, projectiles)
- [x] Delta snapshots (periodic deltas, full every N ticks)
- [x] Extended unit & e2e test suite
- [x] CI pipeline (Linux build + tests)
- [x] Vendored third-party libs via git submodules (yaml-cpp, libcoro, box2d) with pinned SHAs
- [x] Structured logging + metrics counters (JSON mode, config-driven level, Prometheus /metrics, runtime + snapshot size metrics)
- [x] Auth strategy abstraction (pluggable provider stub)

## 2. Current State Snapshot
- Language / Tooling: C++20, libcoro, protobuf, yaml-cpp, CMake.
- Server Features: Auth stub, matchmaking, matches with bots & AI, movement, projectiles, heartbeats, delta/full snapshots.
- Protocol: Auth, Queue, MatchStart, (Full) StateSnapshot, DeltaSnapshot, Heartbeat/Response, Damage & events placeholders.
- Reliability: Streaming frame parser, batching, heartbeat timeout.
- Observability: Basic logger (stdout). No metrics/tracing yet.
- Testing: Broad unit + e2e coverage for core behaviors.
- CI: Single workflow building & testing (Linux). No artifact upload; client build disabled.

## 3. Remaining Scope Toward Full CI (Server + Multi‑Platform Clients)
The following epics are required to reach fully automated builds (server + Android + Desktop + WebAssembly clients) with distributable artifacts and quality gates.

### 3.1 Gameplay & Server Core Hardening (Pre‑Artifact Stability)
- [ ] Damage & hit detection (projectile ↔ tank overlap) with DamageEvent / TankDestroyed / KillFeedUpdate
- [ ] Ammo & reload rules; match end / victory condition
- [ ] Graceful disconnect broadcast + entity removal deltas
- [ ] Snapshot compression / size optimization (quantization, thresholding, optional zstd)
- [ ] Configurable snapshot & delta intervals (expose existing constants to config fully)
- [ ] Box2D physics integration (server authoritative movement, turret rotation, projectile collision)
- [ ] Map items: ammo crates spawning logic & pickup events

### 3.2 Client Build Enablement (Multi‑Platform)
- [ ] Desktop client scaffold (Qt 6 + QML minimal scene, Linux & Windows)
- [ ] Android client target (Gradle + CMake integration, JNI bridge, QML UI)
- [ ] WebAssembly build (Emscripten + Qt WASM module) packaged in Docker Alpine base image
- [ ] Shared network layer (reuse test client framing) + interpolation & reconciliation loop
- [ ] Basic battlefield rendering (tanks, projectiles, simple map tiles / crates)
- [ ] Input mapping (touch, keyboard/mouse, gamepad future)
- [ ] Deterministic replay harness (optional early metrics mode)

### 3.3 CI Pipeline Enhancements
- [ ] Matrix builds (Linux / Windows / macOS) for server & desktop client
- [ ] Android build job (NDK + Gradle) producing APK artifact
- [ ] WebAssembly client build job (Docker image + wasm bundle artifact)
- [ ] Separate build stages: server | clients | tests
- [ ] CCache / build cache integration
- [ ] Artifact upload (server tar/zip + symbols; desktop binaries; APK; wasm bundle)
- [ ] Code coverage (lcov / llvm-cov) reporting (advisory)
- [ ] Static analysis / sanitizers nightly (ASan, UBSan, TSAN matrix)
- [ ] Version stamping from git tag into `T2D_VERSION`
- [ ] Release workflow (tag push) publishing all platform artifacts + changelog
- [ ] Signed release artifacts (optional GPG) later
 - [ ] Dependency pin & verification job (compare vendored versions vs docs & .env)

### 3.4 Observability & Operational Readiness
- [ ] Structured logging (JSON lines) + log level from config
- [ ] Metrics: TPS, queue depth, active matches, snapshot_full_bytes / snapshot_delta_bytes, bots_in_match
- [ ] Prometheus exposition or lightweight HTTP metrics endpoint
- [ ] Crash safety (signal mini‑dump or stack trace)
- [ ] In‑match debug overlay (client) toggle (later)

### 3.5 Quality & Tooling
- [ ] Clang-format enforcement (fail CI on diff)
- [ ] Clang-Tidy curated checks
- [ ] Performance microbench (serialization, framing)
- [ ] Fuzzing: frame parser (libFuzzer) & delta reassembly
- [ ] Replay validator comparing reconstructed vs authoritative state
- [ ] Security lint (basic secret scan & dependency audit)

### 3.6 Security & Authentication
- [ ] OAuth integration (token verification service adapter)
- [ ] Registration & email verification (stub; server config placeholders)
- [ ] Admin auth channel / maintenance API (later)

### 3.7 Build & Deployment Scripts (from Technical Specifications)
- [ ] Bash script: install dependencies (server)
- [ ] Bash script: full rebuild (clean)
- [ ] Bash script: partial incremental build (reuse unchanged libs)
- [ ] Bash script: run unit tests
- [ ] Bash script: run e2e tests
- [ ] Bash script: package & Docker image build (server)
- [ ] Bash script: WebAssembly client Docker build & push
- [ ] Bash script: release tagging helper (creates & pushes git tag)
- [ ] Dockerfile: production server (Alpine base)
- [ ] docker-compose.yml for server deployment (with config volume & env)
- [ ] Deployment script: remote host provisioning (install deps, configure OAuth, docker-compose up)
- [ ] Deployment script: zero‑downtime update path
 - [ ] Script: verify Android toolchain versions match `.env` (NDK 29.0.13846066, Build Tools 34.0.0)
 - [ ] Script: dependency audit / license report generation

### 3.8 Physics (Box2D Integration)
- [ ] Integrate Box2D as dependency (server)
- [ ] Replace naive movement with physics body simulation
- [ ] Tank hull vs projectile collision shapes
- [ ] Ray / hull collision for firing line (optional early approximation)
- [ ] Ammo crate pickups via AABB contact listener

## 4. Incremental Roadmap (Proposed Order)
1. [x] Damage & collision events (with tests) // basic projectile->tank damage + events implemented
2. [x] Entity removal / disconnect deltas (projectile + tank destruction removals implemented; disconnect pending)
3. [x] Snapshot interval config + size metrics (intervals configurable; size counters implemented)
4. [x] Structured logging + metrics counters (JSON + Prometheus endpoint + graceful shutdown snapshot stats)
5. [x] OAuth auth strategy abstraction (stub provider; real external validation later)
6. [ ] Desktop client scaffold + CI build
7. [ ] Android & WASM build jobs
8. [ ] Dependency pin/verification (NDK, Build Tools, periodic submodule SHA audit job)
9. [ ] Artifact uploads (all targets)
10. [ ] Release workflow + version stamping
11. [ ] Box2D physics integration phase 1 (movement + collision)
12. [ ] Coverage & sanitizer matrix
13. [ ] Fuzzing & replay validation

## 5. Risk & Mitigation
| Risk | Impact | Mitigation |
|------|--------|------------|
| Growing protocol complexity w/o versioning policy | Client/server mismatch errors | Introduce explicit `protocol_version` constant & bump checklist early. |
| Delta snapshot correctness drift | Visual or positional desync | Add replay test comparing reconstructed state vs authoritative at intervals. |
| Bot AI performance at scale | Server tick lag | Profile once >100 bots, add throttling or simplified AI path. |
| Large full snapshots | Bandwidth spikes | Periodic full snapshot interval tuning + optional compression (zstd) later. |
| Lack of structured logs | Harder debugging in CI artifacts | Implement JSON log sink early (before scaling). |

## 6. Definition of Done for "Full CI Builds"
A. GitHub Actions workflow producing:
- Server binary artifact (Linux at minimum) per PR + tag.
- Client binary artifact (headless or GUI) per PR + tag.
B. Test matrix (all unit & e2e) green across target OS list.
C. Formatting + (optional) static analysis jobs passing.
D. Version string embedded & traceable to commit/tag.
E. Release workflow publishes artifacts on tag push.

## 7. Immediate Next Actions (Actionable Backlog)
- [ ] Projectile ↔ tank collision + DamageEvent tests hardening (edge cases, multi-hit)
- [ ] TankDestroyed & entity removal on disconnect (currently destruction covered)
- [ ] Desktop client minimal (network connect + log baseline & delta)
- [ ] CI matrix + artifact upload (server + desktop)
- [ ] Bash build scripts (install, full/partial build, tests, package)
- [ ] Add dependency pin list manifest (DEPENDENCIES.md) & CI check vs docs (.env sync)

## 8. Tracking & Metrics (Planned)
Metric prototypes (initial):
- [ ] tick_duration_ms (histogram)
- [ ] snapshot_full_bytes
- [ ] snapshot_delta_bytes
- [ ] queue_depth
- [ ] active_matches
- [ ] bots_in_match
- [ ] projectiles_active
- [ ] auth_failures_total

## 9. Out of Scope (Later Phases)
- Advanced physics & terrain detail (beyond Box2D core integration)
- Skill/ELO matchmaking system
- Persistence / player progression
- Spectator / observer mode
- Anti-cheat systems (heuristics, server authoritative already partially mitigates)
- In‑game economy / cosmetics

---
Maintained: Update after each merged epic (append new row to Completed Milestones and adjust roadmap).
