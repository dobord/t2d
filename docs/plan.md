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
- [x] Qt/QML desktop client baseline (entity & projectile lists, canvas rendering, interpolation, input controls)

## 2. Current State Snapshot
- Language / Tooling: C++20, libcoro, protobuf, yaml-cpp, Box2D, Qt6 (optional), CMake.
- Server Features: Auth stub, matchmaking, matches with bots & AI, movement, projectiles, heartbeats, delta/full snapshots, damage & kill feed, basic victory logic.
- Protocol: Auth, Queue, MatchStart, StateSnapshot (full), DeltaSnapshot, Heartbeat/Response, DamageEvent, TankDestroyed, KillFeedUpdate, MatchEnd.
- Reliability: Streaming frame parser, outbound batching, heartbeat timeout pruning, deterministic tick loop.
- Observability: Structured logging + metrics counters (snapshot sizes, tick runtime, bots etc.) + Prometheus endpoint.
- Testing: Broad unit + e2e coverage (delta snapshots, heartbeat, framing, bot fill, damage, replay delta basics).
- CI: Multi-job workflow (build/test, coverage, ASan/UBSan, TSAN, dependency verify) with artifact uploads (server binary & package, desktop prototype, Qt client if built), ccache, code coverage lcov artifact, version stamping.
- Client: Prototype desktop client (headless) plus Qt/QML UI (canvas rendering of tanks/projectiles with interpolation, basic input state binding). Other platforms pending.

## 3. Remaining Scope Toward Full CI (Server + Multi‑Platform Clients)
The following epics are required to reach fully automated builds (server + Android + Desktop + WebAssembly clients) with distributable artifacts and quality gates.

### 3.1 Gameplay & Server Core Hardening (Pre‑Artifact Stability)
- [x] Damage & hit detection (projectile ↔ tank overlap) with DamageEvent / TankDestroyed / KillFeedUpdate (implemented + batching)
- [x] Ammo & reload rules; match end / victory condition (basic timer reload + winner detection implemented)
- [x] Graceful disconnect broadcast + entity removal deltas
- [ ] Snapshot compression / size optimization (quantization, thresholding, optional zstd)
- [ ] Configurable snapshot & delta intervals (expose existing constants to config fully)
- [x] Box2D physics integration (server authoritative movement + projectile collision via contact events)
- [x] Map items: ammo boxes spawning logic & pickup (basic grant + disappearance)
- [x] Movable crates (physics bodies, full snapshot serialization)
- [x] Crate delta snapshots (position/angle thresholding)
- [x] Map dimensions in snapshots (client boundary rendering)

### 3.2 Client Build Enablement (Multi‑Platform)
- [x] Desktop client scaffold (Linux prototype networking loop)
- [x] Qt/QML desktop UI baseline (entity/projectile list models, canvas rendering, temporal interpolation, basic input controls)
- [ ] Android client target (Gradle + CMake integration, JNI bridge, QML UI)
- [ ] WebAssembly build (Emscripten + Qt WASM module) packaged in Docker image
- [ ] Shared client-side reconciliation & prediction (beyond current simple interpolation)
- [ ] Battlefield rendering enhancements (sprites, orientation, map tiles / crates)
- [ ] Input mapping (touch, keyboard/mouse, gamepad future)
- [ ] Deterministic replay harness (record & playback)

### 3.3 CI Pipeline Enhancements
- [x] Linux build for server & clients (prototype + Qt UI)
- [ ] Android build job (NDK + Gradle) producing APK artifact
	- [x] Initial debug APK build job (arm64-v8a, native hello stub)
- [ ] WebAssembly client build job (Docker + wasm bundle artifact)
- [x] Separate build stages logically split across jobs (build/test, coverage, sanitizers, deps)
	- [x] CCache integration
	- [ ] Artifact upload completion (APK, WASM bundle, debugging symbols) (PARTIAL: server binaries/package + desktop & Qt client uploaded)
	- [x] Code coverage (lcov) reporting + summary
	- [x] Static analysis (clang-tidy) gating (job now fails CI on findings)
	- [x] Sanitizers matrix (ASan/UBSan + TSAN)
	- [x] Version stamping from git metadata
	- [x] Release workflow (tag push) + changelog (initial release.yml added; changelog generation TBD)
	- [ ] Signed release artifacts (optional)
	- [x] Dependency pin & verification

### 3.4 Observability & Operational Readiness
- [x] Structured logging (JSON lines) + log level from config
- [x] Metrics: TPS, queue depth, active matches, snapshot_full_bytes / snapshot_delta_bytes, bots_in_match
- [x] Prometheus exposition or lightweight HTTP metrics endpoint
- [ ] Crash safety (signal mini‑dump or stack trace)
- [ ] In‑match debug overlay (client) toggle (later)

### 3.5 Quality & Tooling
 - [x] Clang-format enforcement (format check job fails CI on diff)
 - [x] Clang-Tidy curated checks (advisory CI job initial; gating & rule tuning pending)
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
- [x] Integrate Box2D as dependency (server)
- [x] Replace naive movement with physics body simulation
- [x] Tank hull vs projectile collision shapes + contact events
- [ ] Ray / hull collision for firing line (optional early approximation)
- [x] Ammo box pickups (proximity radius)
- [x] Crate movable obstacles (cluster spawn)
- [ ] Crate destruction logic (health / removal events)

## 4. Incremental Roadmap (Proposed Order)
1. [x] Damage & collision events (with tests) // projectile->tank damage via Box2D contacts
2. [x] Entity removal / disconnect deltas (projectile + tank destruction removals implemented; disconnect pending)
3. [x] Snapshot interval config + size metrics (intervals configurable; size counters implemented)
4. [x] Structured logging + metrics counters (JSON + Prometheus endpoint + graceful shutdown snapshot stats)
5. [x] OAuth auth strategy abstraction (stub provider; real external validation later)
6. [x] Desktop client scaffold + CI build (Linux-only; prototype non-UI target `t2d_desktop_client`)
7. [x] Desktop client (Linux Qt 6.8.3 UI QML) – basic UI layer (lists, canvas rendering, interpolation, input)
8. [x] Crate & ammo box visualization (canvas render + list models)
9. [x] Crate delta snapshot integration (client applyDelta)
10. [ ] Android & WASM build jobs
11. [x] Dependency pin/verification (NDK, Build Tools, periodic submodule SHA audit job) (verification job present; NDK/Build Tools check still pending)
12. [ ] Artifact uploads (all targets) (PARTIAL: server + desktop & Qt client artifacts done)
13. [ ] Release workflow + version stamping (version stamping core in place, release workflow pending)
14. [x] Box2D physics integration phase 1 (movement + collision + filtering)
15. [x] Sanitizer matrix (coverage DONE; ASan+UBSan + TSAN in CI) (static analysis separate)
16. [ ] Fuzzing & replay validation

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
- [ ] Snapshot compression (quantization flag exists; implement zlib/zstd and thresholding)
- [ ] Configurable snapshot & delta intervals fully exposed to config file
- [x] Map items: ammo boxes (spawn + pickup)
- [x] Crate physics obstacles + delta updates
- [ ] Crate destruction (removal events to populate removed_crates)
- [ ] Android build job (NDK + minimal JNI stub) producing APK artifact
- [ ] WASM client build job (Emscripten) producing bundle artifact
- [ ] Artifact uploads completion (APK, WASM bundle, symbol files)
	- [x] Debug APK artifact upload (initial)
- [x] Release workflow on tag push (publish artifacts + basic autogenerated notes)
- [x] clang-tidy CI job (non-blocking → gating)
- [ ] Performance microbench harness (serialization & framing) + baseline capture
- [ ] Fuzzing target for frame parser (libFuzzer) in nightly CI
- [ ] Replay validator test (reconstruct vs authoritative) & deterministic harness
- [ ] Security lint (secret scan + license report script)
- [ ] Script: Android toolchain version verification vs manifest
- [ ] Script: dependency audit / license report generation
- [ ] Crash handling: backtrace on signals (disabled under sanitizers)
- [ ] Client prediction & reconciliation (beyond linear interpolation)
- [ ] OAuth real token validation adapter (pluggable external service)
 - [x] Script: Android toolchain version verification vs manifest (added CI job android-toolchain)

## 8. Tracking & Metrics (Planned)
Metric prototypes (initial):
- [x] tick_duration_ns (histogram)  <!-- implemented as t2d_tick_duration_ns_* -->
- [x] snapshot_full_bytes
- [x] snapshot_delta_bytes
- [x] queue_depth
- [x] active_matches
- [x] bots_in_match
- [x] projectiles_active
- [x] auth_failures_total
- [ ] client_interpolation_alpha (gauge for drift diagnostics)

## 9. Out of Scope (Later Phases)
- Advanced physics & terrain detail (beyond Box2D core integration)
- Skill/ELO matchmaking system
- Persistence / player progression
- Spectator / observer mode
- Anti-cheat systems (heuristics, server authoritative already partially mitigates)
- In‑game economy / cosmetics

---
Maintained: Update after each merged epic (append new row to Completed Milestones and adjust roadmap).
