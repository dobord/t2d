# Protocol (Draft v0)

Transport independent (current implementation: reliable ordered TCP with length‑prefixed protobuf frames). Future evaluation: UDP or hybrid for latency sensitive state updates.

## Message Containers
`ClientMessage` and `ServerMessage` (protobuf) wrap payload variants via `oneof`, enabling forward‑compatible additions without breaking older clients (unknown fields ignored).

## Authentication
Client sends `AuthRequest` (placeholder OAuth token). Server responds with `AuthResponse` containing a `session_id` if accepted. `auth_mode` config controls validation strategy (`stub` currently).

## Queueing
`QueueJoinRequest` puts a session into the matchmaking queue. Server emits periodic `QueueStatusUpdate` messages (queue position, players in queue, needed for match, countdown, projected bot fill) until a match forms or fill timeout triggers bot insertion.

## Match Start
`MatchStart` provides match id, authoritative tick rate, and RNG seed (used for deterministic spawn distribution: tanks, crate clusters, ammo boxes).

## Inputs
`InputCommand` carries player intent: `move_dir`, `turn_dir`, `turret_turn`, `fire`, `brake` plus a monotonically increasing `client_tick`. Server always uses only the latest input per session; stale/out‑of‑order commands are ignored to maintain determinism.

## State Distribution
`StateSnapshot` (full) contains authoritative state:
* Tanks: id, x, y, hull_angle, turret_angle, hp, ammo
* Projectiles: id, x, y, vx, vy
* Ammo boxes: id, x, y, active (static pickups; inactive boxes omitted)
* Crates: id, x, y, angle (movable obstacles)
* Map dimensions: width, height (world bounds for clients)

Clients interpolate tank & projectile motion between full snapshots using delta snapshots. The initial full snapshot after `MatchStart` uses `server_tick=0` baseline.

## Delta Snapshots
`DeltaSnapshot` transmits only changes since the last full snapshot (`base_tick`):
* server_tick, base_tick
* tanks (changed/new)
* projectiles (new spawns only; per‑tick motion predicted client‑side)
* removed_tanks, removed_projectiles
* crates (changed/new if moved >0.01 units or rotated >0.5°)
* removed_crates (future crate destruction)

Ammo boxes presently appear only in full snapshots; pickup removal is inferred by absence (delta optimization pending).

## Events
Combat/lifecycle messages:
* `DamageEvent` (victim, attacker, amount, remaining_hp)
* `TankDestroyed` (victim, attacker)
* `KillFeedUpdate` (batched destruction events per tick)
Future: explicit pickup event, match phase transitions.

## Heartbeat
`Heartbeat` / `HeartbeatResponse` pair provides RTT estimation and liveness; sessions exceeding `heartbeat_timeout_seconds` are pruned.

## Baseline Snapshot
Issued immediately after all players spawn (`server_tick=0`) establishing delta baseline and conveying static map dimensions.

## Versioning
Additive modifications (new fields/messages) are backward compatible (proto3). Recent additions: `CrateState` (full + delta), `AmmoBoxState`, map dimensions in full snapshots, crate delta fields (`crates`, `removed_crates`). Firing cadence is configurable via server config (`fire_cooldown_sec`) without protocol change. Any removal or semantic change requires bumping a protocol version constant (planned) and coordinated deploy.

## Future Extensions
* Ammo box delta updates (active -> inactive) instead of full snapshot reliance
* Snapshot compression (quantization + optional zlib/zstd) with negotiation
* Projectile motion deltas if prediction drift proves significant
* Partial reliability / UDP channel for high‑frequency ephemeral data
* Replay & checksum messages for debugging desyncs

Maintain this document in sync with `proto/game.proto` for each change set.

# Protocol (Draft v0)

Transport independent (assumes reliable ordered stream e.g. TCP for MVP). Future: evaluate UDP or hybrid for real-time state.

## Message Containers
`ClientMessage` and `ServerMessage` (protobuf) wrap specific payload variants via `oneof`.

## Authentication
Client sends `AuthRequest` with OAuth token. Server validates (stub in MVP) and replies `AuthResponse` with a session ID.

## Queueing
`QueueJoinRequest` enters matchmaking. Server periodically emits `QueueStatusUpdate` until a match starts or timeout triggers bot fill.

## Match Start
`MatchStart` provides tick rate and RNG seed.

## Inputs
`InputCommand` batched at client tick resolution (client may send several per network frame). Server reconciles and simulates.

## State Distribution
`StateSnapshot` contains authoritative positions and status of entities. Clients interpolate between snapshots.

## Events
`DamageEvent`, `TankDestroyed`, and `KillFeedUpdate` convey combat outcomes.

## Heartbeat
`Heartbeat` used for latency measurement and liveness. Server replies with `HeartbeatResponse` echoing client timestamp and including server time and delta.

## Baseline Snapshot
Immediately after `MatchStart` the server emits a baseline `StateSnapshot` with `server_tick=0` so clients can initialize state deterministically before incremental updates.

## Versioning
Protocol version increments on breaking changes; future field additions must be backward compatible.
