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

## Tick Loop (Planned)
1. Collect input commands for current tick.
2. Step physics world (fixed timestep = 1 / tick_rate).
3. Resolve collisions and damage.
4. Spawn / expire projectiles and ammo boxes.
5. Record events (damage, kills) and queue for broadcast.
6. Every N ticks send incremental snapshot; every M ticks send full snapshot.

## Concurrency Model
Coroutines (libcoro) scheduled on a single io_scheduler for I/O bound tasks (network polling, matchmaking). Physics tick runs on a controlled loop to avoid race conditions (single-threaded simulation per match instance) initially.

## Scaling
Multiple matches coexist; each match has its own world state and tick coroutine. A central match manager tracks active matches and available player slots.

## Configuration
YAML configuration (see `config/server.yaml`). Networking now uses `listen_port` for the TCP listener.

## Heartbeat & Liveness
Clients periodically send a Heartbeat message. The server records `last_heartbeat` per session. A background coroutine (`heartbeat_monitor`) will later enforce timeouts and prune stale sessions (prototype currently only records timestamps).

## Input Handling
Latest `InputCommand` per authenticated session is stored in `Session::input`. Each match tick samples this struct to apply movement (forward/back), hull rotation, and turret rotation. Older input (by `client_tick`) is ignored to maintain monotonic progression.

## Graceful Shutdown
Signals (SIGINT/SIGTERM) set a global atomic shutdown flag. Long-running loops periodically check this flag and exit cooperatively. Future improvements: broadcast shutdown notice to clients; close sockets explicitly; flush telemetry.
