// SPDX-License-Identifier: Apache-2.0
## Protocol (Draft v0)

Transport independent (current implementation: reliable ordered TCP with length‑prefixed protobuf frames). A hybrid (reliable + UDP) path may be introduced later for high‑frequency ephemeral updates; the message schema is designed to remain additive‑extensible.

Keep this document in sync with `proto/game.proto` on every change. Remove stale sections instead of appending duplicates (this file previously contained an outdated duplicate block which has been consolidated).

### 1. Message Containers
`ClientMessage` and `ServerMessage` wrap payload variants via a `oneof`. Unknown fields are ignored (proto3), enabling forward compatible field additions without breaking older clients. New message types should be appended to the `oneof` with the next available tag number; never reuse or repurpose existing tags.

### 2. Authentication
Client sends `AuthRequest { oauth_token, client_version }`.
Server responds with `AuthResponse { success, session_id, reason }`.
`auth_mode` (config) drives validation strategy (`stub` today). `client_version` is reserved for coordinated upgrades (policy TBD).

### 3. Matchmaking Queue
`QueueJoinRequest` places the session into the queue. Server emits periodic `QueueStatusUpdate` until match formation or bot fill triggers start.

`QueueStatusUpdate` fields:
| Field | Type | Description |
|-------|------|-------------|
| position | uint32 | 1-based position of this session in queue |
| players_in_queue | uint32 | Total waiting sessions (including this) |
| needed_for_match | uint32 | Target player count for next match (config `max_players_per_match`) |
| timeout_seconds_left | uint32 | Legacy countdown until bot fill (superseded by `lobby_countdown`; kept for backward compatibility) |
| lobby_countdown | uint32 | Remaining seconds until lobby auto-start (0 if not yet scheduled) |
| projected_bot_fill | uint32 | Number of bots that would be inserted if countdown expired now |
| lobby_state | uint32 | Enumerated lobby phase: 0=queued, 1=forming (match selected / waiting start), 2=spawning, 3=unknown/reserved |

### 4. Match Start
`MatchStart` launches the authoritative simulation. Fields:
| Field | Type | Description |
|-------|------|-------------|
| match_id | string | Unique identifier (opaque) |
| tick_rate | uint32 | Authoritative simulation ticks per second |
| seed | uint32 | RNG seed for deterministic spawn/layout generation |
| initial_player_count | uint32 | Number of human players present at formation (excludes bots) |
| disable_bot_fire | bool | Echo of server config influencing expected match duration / pacing |
| my_entity_id | uint32 | Authoritative tank entity id for THIS client (replaces earlier client-side heuristic) |

### 5. Player Input
`InputCommand` conveys *intent* only; the server simulates results.
Fields: `session_id`, `client_tick` (monotonic per client), analog-ish axes (`move_dir`, `turn_dir`, `turret_turn` in -1..1), discrete flags (`fire`, `brake`).
Server keeps only the latest command per session (ignoring older or duplicate `client_tick` values) to bound per-tick processing.

### 6. State Distribution
Two snapshot forms:
1. `StateSnapshot` (full): complete tank, projectile, ammo box (active only), crate state + map dimensions.
2. `DeltaSnapshot`: changes since a base full snapshot (`base_tick`).

Clients interpolate tank/projectile motion between authoritative updates. Projectiles currently only emit a creation delta; linear motion is predicted until removal or impact.

### 7. Delta Snapshot Semantics
`DeltaSnapshot` includes:
* `server_tick`, `base_tick`
* `tanks` (changed/new since base)
* `projectiles` (new only)
* `removed_tanks`, `removed_projectiles`
* `crates` (changed/new when exceeding movement/rotation thresholds)
* `removed_crates` (future destruction/removal events)

Ammo boxes are presently full-snapshot only; disappearance (pickup) inferred by absence. Planned: delta toggle for active->inactive to reduce full snapshot reliance.

### 8. Combat & Lifecycle Events
* `DamageEvent` – per hit (victim, attacker, amount, remaining_hp)
* `TankDestroyed` – single destruction (victim, attacker or 0 for environment)
* `KillFeedUpdate` – batched destruction events for the tick (optimization over multiple `TankDestroyed`)
* `MatchEnd` – emitted exactly ONCE per match (guarantee: server ensures single dispatch even across internal coroutines). Contains `winner_entity_id` (0 draw/timeout) and `server_tick` of termination.

### 9. Heartbeat
`Heartbeat` / `HeartbeatResponse` pair provides RTT estimation and liveness. Server tracks `last_heartbeat` and prunes sessions exceeding `heartbeat_timeout_seconds` (config). Response echoes `client_time_ms` and includes `server_time_ms` plus computed `delta_ms`.

### 10. Client Entity Identity
`my_entity_id` in `MatchStart` eliminates earlier client heuristics for determining the controlled tank (previously inferred by spawn order). Clients MUST discard any local inference logic and rely solely on this field for ownership binding.

### 11. Match Lifecycle Guarantees
* Exactly one `MatchStart` then zero or more snapshots (baseline full snapshot uses `server_tick=0`).
* Optional interleaving of events (`DamageEvent`, `KillFeedUpdate`, etc.).
* Exactly one terminal `MatchEnd` – clients should treat any additional as protocol violation (log & ignore).
* After `MatchEnd` no further state or combat events for that `match_id` are valid (future: explicit teardown / lobby transition message).

### 12. Ordering & Reliability
All frames traverse a reliable ordered stream. Application-level ordering rules:
* Snapshots / deltas are processed in `server_tick` order; discard any delta with unknown `base_tick` (until full snapshot arrives) or regressive `server_tick`.
* Events referencing removed entities may arrive after the removal delta/full snapshot (client should handle gracefully).

### 13. Versioning Policy
Current proto embeds a comment placeholder for a protocol version constant (TBD numeric field). Until formalized:
* Additive, backward compatible changes (new optional fields, new messages, new enum values) are permitted without bump.
* Semantic changes (repurposing meaning, removal of fields, altering required ordering) require introducing an explicit version field in both `AuthRequest` & `AuthResponse` and gating logic on negotiated minimum.
* Never recycle a removed field number; mark it reserved in the `.proto` if retired.

### 14. Planned Extensions
| Area | Change |
|------|--------|
| Ammo boxes | Delta updates for pickup state |
| Compression | Optional zstd / zlib snapshot compression + negotiation flag |
| Transport | UDP channel for high-frequency ephemeral (projectile position, maybe prediction corrections) |
| Replay | Deterministic replay & checksum frames for desync detection |
| Disconnect | Explicit disconnect reason event (currently implicit via removal) |

### 15. Field Addition Checklist
1. Add field to `.proto` with next free tag.
2. Update server serialization & client deserialization.
3. Update this document (section & any tables).
4. Add tests (unit or e2e) asserting presence / semantics.
5. Consider impact on existing logs (ensure trace/debug instrumentation if helpful).

---
Historical duplicate content removed on consolidation commit. Refer to repository history for prior drafts if needed.
