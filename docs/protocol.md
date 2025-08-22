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
