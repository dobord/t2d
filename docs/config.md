# Configuration Reference (Draft)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| max_players_per_match | uint | 16 | Players in a single match |
| max_parallel_matches | uint | 8 | Concurrent matches upper bound |
| queue_soft_limit | uint | 256 | Soft cap for waiting players |
| fill_timeout_seconds | uint | 180 | Fill with bots after this wait |
| tick_rate | uint | 30 | Simulation ticks per second |
| snapshot_interval_ticks | uint | 3 | Interval for incremental snapshots |
| full_snapshot_interval_ticks | uint | 30 | Interval for full snapshots |
| bot_difficulty | uint | 1 | Bot AI difficulty level |
| listen_port | uint | 40000 | TCP port the server listens on |

Additional fields will be appended as features mature.
