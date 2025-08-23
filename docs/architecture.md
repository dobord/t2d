# Architecture Overview

This document outlines the initial high-level architecture of the t2d game server and clients.

## Modules
| Module | Responsibility |
|--------|----------------|
| core | Configuration loading, lifecycle management |
| net | Networking (transport, encoding, sessions) |
| matchmaking | Queue management and match creation |
| game | Physics, tank logic, damage, projectiles, pickups |
| ai | Bot decision making and navigation |
| proto | Protobuf message schemas |
| client | Qt/QML UI, rendering, input, interpolation |

## Authoritative Server Model
Clients send only input commands; the server simulates the world and distributes state snapshots. This prevents most client-side cheating and maintains consistency.

## Tick Loop (Current Prototype)
1. Collect latest input commands (bots synthesize input internally).
2. Step Box2D physics world (fixed dt = 1 / tick_rate).
3. Synchronize tank & projectile transforms back to authoritative state.
4. Process begin-contact events (projectile → tank) to apply damage & generate Damage/Destroyed events.
5. Spawn / cull projectiles; update ammo reload timers and issue KillFeed batch if any events.
6. Emit delta or full snapshot based on configured intervals.

## Concurrency Model
Coroutines (libcoro) scheduled on a single io_scheduler for I/O bound tasks (network polling, matchmaking). Physics tick runs on a controlled loop to avoid race conditions (single-threaded simulation per match instance) initially.

## Scaling
Multiple matches coexist; each match has its own world state and tick coroutine. A central match manager tracks active matches and available player slots.

## Configuration
YAML configuration (see `config/server.yaml`). Core gameplay (movement speed, projectile speed & damage, bot fire interval, reload time) is now data-driven for rapid balancing.

## Heartbeat & Liveness
Clients periodically send a Heartbeat message. The server records `last_heartbeat` per session. A background coroutine (`heartbeat_monitor`) will later enforce timeouts and prune stale sessions (prototype currently only records timestamps).

## Input Handling
Latest `InputCommand` per authenticated session is stored in `Session::input`. Each match tick samples this struct to apply movement (forward/back), hull rotation, and turret rotation. Older input (by `client_tick`) is ignored to maintain monotonic progression.

## Graceful Shutdown
Signals (SIGINT/SIGTERM) set a global atomic shutdown flag. Long-running loops periodically check this flag and exit cooperatively. Future improvements: broadcast shutdown notice to clients; close sockets explicitly; flush telemetry.
