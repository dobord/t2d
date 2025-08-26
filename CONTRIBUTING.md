# Contributing to t2d

Thank you for your interest in improving t2d.

All contributions (code, docs, CI) must follow the rules below to keep quality, determinism and reproducibility.

## Quick Start Checklist
- Fork & clone (or create a feature branch if you have push rights)
- Update / init submodules: `git submodule update --init --recursive`
- Configure build (see `README.md` for options)
- Run full test suite locally (`ctest --output-on-failure`)
- Run formatting & SPDX checks: `scripts/format_check.sh --apply` then `scripts/spdx_check.sh`
- Update `DEPENDENCIES.md` if submodule SHAs changed
- Keep commit subject <= 50 chars, present tense ("Add X", not "Added X")
- Include rationale in body when non‑trivial (wrap at ~72 cols)
- Make sure new code paths are covered by unit or e2e tests where practical

## Code Style & Formatting
- C++20 only (no GNU extensions unless already present). Do not downgrade language standard.
- Every first‑party C/C++ file starts with: `// SPDX-License-Identifier: Apache-2.0`
- Do NOT edit files under `third_party/`.
- Run `scripts/format_check.sh --apply` before committing; CI will fail on style drift.
- CMake files are formatted via `cmake-format` target (see CI). Avoid manual reflow that diverges.

## Coroutines (libcoro) Rules
(See `.github/copilot-instructions.md` for authoritative wording; summary here for contributors.)
- Each coroutine must begin with a scheduling/yield operation (e.g. `co_await scheduler->schedule();`).
- Prefer free / static functions over capturing lambdas for coroutine tasks.
- Reuse the existing scheduler; do not construct new schedulers inside coroutines.
- Handle `would_block` as retry (after `poll()`), return early on fatal statuses.

## Networking Patterns
- TCP send loops must `poll(write)` and loop on partial sends until buffer drained.
- UDP receive: `poll(read)` then `recvfrom`; ignore `would_block` and continue.
- Convert IPv4 from frames via string helper to avoid endianness pitfalls.

## Tests
- Unit tests: place in `tests/` with `unit_*.cpp` naming for small scoped logic.
- End-to-end tests: `e2e_*.cpp` naming; prefer deterministic timing (avoid sleeps; rely on ticks/config overrides).
- Add at least one test per new protocol message or state machine branch.
- Avoid flakiness: prefer polling with timeouts over fixed sleeps.

## Adding Protocol Messages
1. Edit `proto/game.proto`.
2. Rebuild (CMake regenerates `game.pb.*`).
3. Add handling in server components and (if needed) client prototype.
4. Extend tests (encode/decode + integration).

## Configuration
- Extend `config/server.yaml` only with documented, validated keys. Unknown keys are ignored, but still document new keys in `docs/config.md` + README table.

## Dependencies / Submodules
- External libs reside under `third_party/` as submodules.
- After updating a submodule revision, adjust the SHA in `DEPENDENCIES.md` table.
- Avoid adding large dependencies unless justified (footprint, maintenance, license).

## Commit Messages
Format:
```
Short imperative subject (<=50 chars)

Optional detailed body explaining rationale / design / trade-offs.
Wrap lines at ~72 characters.
```
Examples:
```
Add delta snapshot removal list metrics
Refactor matchmaking to isolate queue policy
Fix TCP framing partial read handling
```

## Pull Requests
Include:
- Summary of change + motivation
- Any protocol / config / schema changes (highlight clearly)
- Test evidence (new tests, coverage impact, manual steps)
- Rollback plan if risky

## Security / Robustness
- Validate all network input lengths and enum ranges.
- No blocking syscalls without prior non-blocking readiness (`poll`).
- Early return on parse or framing errors; log at appropriate level (avoid log spam in tight loops).

## Logging
- Use existing structured logger (respect config `log_json` & level).
- Avoid excessive per-tick logs; aggregate metrics instead.

## Metrics
- Add new counters/gauges responsibly; name with clear, stable prefixes (e.g. `t2d_snapshot_...`).
- Update `README.md` metrics section if adding externally visible series.

## Snapshot & Compression Roadmap (Contrib Guidance)
Near-term acceptable contributions:
- Implement optional zlib compression behind a config flag (keep default off).
- Quantization experiments must be isolated and compile-time or config gated.

## Performance
- Benchmark heavy loops (physics, snapshot build) before micro-optimizing.
- Prefer clear code; document any non-obvious optimizations.

### Profiling Tooling (perf + FlameGraph)
To capture CPU/Off-CPU flame graphs consistently:

1. Install perf (Linux tools package) and FlameGraph scripts (if not already present):
	- FlameGraph repository: https://github.com/brendangregg/FlameGraph
	- After cloning, place `flamegraph.pl` and `stackcollapse-perf.pl` in your `PATH` (e.g. `/usr/local/bin`).
2. Ensure frame pointers are enabled: build with `-fno-omit-frame-pointer` (the profiling CMake option `T2D_ENABLE_PROFILING` already sets this).
3. Relax kernel restrictions (required to attach perf to non-root processes):
	```bash
	sudo sysctl kernel.perf_event_paranoid=1
	sudo sysctl kernel.kptr_restrict=0
	# Verify
	cat /proc/sys/kernel/perf_event_paranoid
	cat /proc/sys/kernel/kptr_restrict
	```
	For a one-off session you can also write the values directly into the files under `/proc/sys/kernel/`.
4. Run capture scripts (examples):
	- CPU: `PID=<server_pid> DUR=30 OUT_DIR=profiles/cpu/$(date +%Y%m%d-%H%M%S) ./scripts/profile_cpu.sh`
	- Off-CPU: `PID=<server_pid> DUR=30 OUT_DIR=profiles/offcpu/$(date +%Y%m%d-%H%M%S) ./scripts/profile_offcpu.sh`
	- Automated baseline: `CLIENTS=20 DURATION=90 CPU_PROF_DUR=40 OFFCPU_PROF_DUR=40 ./scripts/phase0_baseline_capture.sh`
5. Artifacts:
	- Raw data: `perf.data`
	- Expanded stacks: `perf.script`
	- Folded stacks: `out.folded` (+ `out.folded.full` if fallback filtering used)
	- SVG flame graph: `cpu_flame.svg` / `offcpu_flame.svg`
6. Fallback behavior: if direct PID attach fails (transient process visibility races), scripts fall back to a system-wide capture (`-a`) then filter by binary name (`FALLBACK_FILTER`, default `t2d_server`). Adjust if multiple server processes are active.
7. Do not commit large `perf.data` files. Commit only derived artifacts (SVG) when they are needed for code review context; prefer attaching them to issues or storing as CI artifacts.

Additional runtime metrics (profiling builds `T2D_ENABLE_PROFILING`):
- `t2d_p99_tick_ns` / `t2d_tick_duration_ns_*` histogram buckets (existing)
- `t2d_wait_p99_ns` (scheduler wait / sleep between ticks)
- `t2d_cpu_user_pct` (process user+sys CPU % sampled via /proc)
- `t2d_rss_peak_bytes` (peak RSS observed)
- `t2d_allocs_per_tick_mean` (mean dynamic allocations per tick via global operator new hook)

Security note: Lowering `perf_event_paranoid` affects system-wide observability. Revert if necessary after profiling (`sudo sysctl kernel.perf_event_paranoid=4`).

## Issue Triage Labels (Proposed)
- `bug`, `enhancement`, `protocol`, `performance`, `docs`, `build`, `good-first-issue`.

## Licensing
- All contributions are accepted under Apache 2.0 (see `LICENSE`).
- Ensure new files carry the proper SPDX header.

## Release Packaging
- The server packaging script lives in `scripts/package_server.sh`. Modify with care; keep reproducible and deterministic.

## Questions
Open a GitHub issue (choose an appropriate template). Provide logs (sanitized), config snippet, and reproduction steps.

---
Thank you for helping improve t2d!
