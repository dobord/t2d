# Configuration Reference (Draft)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| max_players_per_match | uint | 16 | Players in a single match |
| max_parallel_matches | uint | 8 | Concurrent matches upper bound |
| queue_soft_limit | uint | 256 | Soft cap for waiting players |
| fill_timeout_seconds | uint | 180 | Fill with bots after this wait |
| tick_rate | uint | 30 | Simulation ticks per second |
| snapshot_interval_ticks | uint | 5 | Interval for incremental snapshots |
| bot_difficulty | uint | 1 | Bot AI difficulty level |
| listen_port | uint | 40000 | TCP port the server listens on |
| full_snapshot_interval_ticks | uint | 30 | Interval for full snapshots |
| bot_fire_interval_ticks | uint | 60 | Bot firing cadence (ticks) |
| movement_speed | float | 2.0 | Tank linear speed (units/s) |
| projectile_speed | float | 5.0 | Projectile speed (units/s) |
| projectile_damage | uint | 25 | Damage per projectile hit |
| reload_interval_sec | float | 3.0 | Seconds to regenerate 1 ammo |
| heartbeat_timeout_seconds | uint | 30 | Session timeout for heartbeat |
| matchmaker_poll_ms | uint | 200 | Matchmaker queue poll interval |
| log_level | string | info | Logging verbosity |
| log_json | bool | false | Emit JSON log lines |
| metrics_port | uint | 9100 | Metrics HTTP endpoint port (0=disabled) |
| auth_mode | string | stub | Authentication backend mode |
| auth_stub_prefix | string | user_ | Prefix for stub auth user IDs |

Additional fields will be appended as features mature.
