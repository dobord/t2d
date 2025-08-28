// SPDX-License-Identifier: Apache-2.0
# Configuration Reference (Draft)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| max_players_per_match | uint | 16 (local dev often 4) | Players in a single match (local `server.yaml` uses 4 for faster fills) |
| max_parallel_matches | uint | 8 | Concurrent matches upper bound |
| queue_soft_limit | uint | 256 | Soft cap for waiting players |
| fill_timeout_seconds | uint | 180 | Fill with bots after this wait (shorter in test config) |
| tick_rate | uint | 30 | Simulation ticks per second |
| snapshot_interval_ticks | uint | 5 | Interval for delta snapshots (between full) |
| full_snapshot_interval_ticks | uint | 30 | Interval for mandatory full snapshots |
| bot_difficulty | uint | 1 | Bot AI difficulty level (placeholder) |
| bot_fire_interval_ticks | uint | 60 | Bot firing cadence (ticks; clamped lower in test_mode) |
| movement_speed | float | 2.0 | Tank linear speed (units/s) |
| projectile_speed | float | 15.0 | Projectile speed (units/s) (older docs listed 5.0) |
| projectile_damage | uint | 25 | Damage per projectile hit (>=50 in `test_mode`) |
| projectile_density | float | 20.0 | Projectile physics density (higher value for more stable flight vs. earlier 0.01–0.1) |
| fire_cooldown_sec | float | 1.0 | Minimum seconds between player shots (tank cannon) |
| reload_interval_sec | float | 3.0 | Seconds to regenerate 1 ammo |
| hull_density | float | 1.0 | Tank hull body physics density |
| turret_density | float | 0.5 | Tank turret body physics density |
| track_break_hits | uint | 1 | Hits to a side track before it breaks (mobility reduction) |
| turret_disable_front_hits | uint | 2 | Frontal hits required to disable turret motor |
| disable_bot_fire | bool | false | When true bots never fire (overrides bot cadence) |
| test_mode | bool | false | Enables internal test-oriented clamps (faster bots, higher damage) |
| map_width | float | 100 | World width in world units (earlier prototype used 300) |
| map_height | float | 100 | World height in world units (earlier prototype used 200) |
| listen_port | uint | 40000 | TCP port the server listens on |
| heartbeat_timeout_seconds | uint | 30 | Session timeout for heartbeat |
| matchmaker_poll_ms | uint | 200 | Matchmaker queue poll interval |
| log_level | string | info | Logging verbosity (trace|debug|info|warn|error) |
| log_json | bool | false | Emit JSON log lines |
| metrics_port | uint | 9100 | Metrics HTTP endpoint port (0=disabled) |
| auth_mode | string | stub | Authentication backend mode (disabled|stub|oauth future) |
| auth_stub_prefix | string | user_ | Prefix for stub auth user IDs |

Test configuration example: see `config/server_test.yaml` for a faster iteration profile (reduced cooldowns, higher projectile damage, smaller map, `test_mode: true`).

Bot fire controls: `disable_bot_fire: true` in config or runtime flags/env (`--no-bot-fire`, `T2D_NO_BOT_FIRE=1`) force bots to never shoot (reflected in `MatchStart.disable_bot_fire`).

AI targeting: Bots always ignore tanks with `hp == 0` even when `persist_destroyed_tanks: true` keeps corpses in snapshots.

Subsystem Damage: Track and turret impairment thresholds are controlled via `track_break_hits` and `turret_disable_front_hits`. Setting either to 0 disables that impairment type (no accumulation). These flags replicate as booleans per tank (`track_left_broken`, `track_right_broken`, `turret_disabled`). Broken tracks reduce movement effectiveness; disabled turret stops rotation.

Delta Snapshot Contents (current): tanks, new projectiles, removed_tanks, removed_projectiles, crates (changed/new), removed_crates. Ammo boxes (static until picked up) are sent only in full snapshots; when picked up they simply disappear from subsequent full snapshots (delta optimization pending).

Fields may evolve; new keys are ignored by older binaries (forward compatibility); unknown keys are skipped with defaults.
