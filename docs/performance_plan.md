# SPDX-License-Identifier: Apache-2.0

# Performance Improvement Plan

All documentation is maintained in English (repository style policy). This document tracks the systematic discovery, measurement, remediation, and regression prevention of server & client performance issues. It will evolve iteratively: each cycle adds (a) captured measurements, (b) hypotheses, (c) chosen interventions, and (d) validated outcomes.

---
## 1. Analysis (Current Baseline & Goals)
Status: INITIAL (no committed profiling artifacts yet)

### 1.1 Scope
Focus (phase 1): Authoritative server runtime (tick loop, networking, snapshot generation, delta encoding, entity & projectile simulation, logging, metrics). Secondary: Qt client frame latency & interpolation jitter once server baselines are stable.

### 1.2 Key Workloads (Representative Scenarios)
| Scenario | Description | Target Scale v1 | Notes |
|----------|-------------|-----------------|-------|
| S1 Match (Bots Light) | 2 human, 4 bots, standard map | 6 players | Functional smoke; quick profiling warm-up |
| S2 Match (Bots Medium) | 2 human, 20 bots | 22 players | AI + projectile stress |
| S3 Match (Bots Heavy) | 0 human, 64 bots | 64 players | Upper bound CPU stress (AI, collisions, snapshot size) |
| S4 Burst Projectiles | 16 players each firing rapidly | 16 players, projectile burst | Stress serialization & allocation |
| S5 Long-Run Stability | Continuous S2 for 30 min | 22 players | Memory growth, fragmentation, leak detection |

### 1.3 Initial Hypotheses / Potential Hotspots
| Area | Hypothesis | Risk | Evidence (TBD) |
|------|------------|------|----------------|
| Snapshot (full) serialization | Excessive copy / string formatting | High bandwidth & CPU spikes | To be profiled |
| Delta snapshot diffing | Per-entity map/set lookups | CPU in large matches | To be profiled |
| Projectile updates | O(N projectiles) per tick linear scan | Tick time variance | To be profiled |
| Logging (INFO in hot path) | I/O or string formatting overhead | Jitter under burst | To be profiled |
| Memory allocations (entity temp buffers) | Frequent heap alloc/free | Cache misses, allocator contention | To be profiled |
| Bot AI decision loop | Runs every tick for all bots | Scales poorly with bot count | To be profiled |
| Physics (Box2D step) | Step cost grows with dynamic bodies | Tick > SLA if body count high | To be profiled |

### 1.4 Baseline Metrics (To Collect Before Any Optimizations)
| Metric | Description | Collection Method | Baseline (TBD) | Target |
|--------|-------------|-------------------|----------------|--------|
| tick_duration_ns_mean | Avg server tick time | Existing histogram | 287453 (S2 baseline 20250826-180618) | < 2 ms (S2) |
| tick_duration_ns_p99 | 99th percentile tick | Existing histogram | 500000 (recent 20250826-185117) | < 5 ms (S2) |
| snapshot_full_bytes_mean | Mean full snapshot size | Existing counter | 667.03 (S2 baseline 20250826-180618) | < 25 KB (target guess; refine) |
| snapshot_delta_bytes_mean | Mean delta size | Existing counter | 499.94 (S2 baseline 20250826-180618) | < 4 KB (S2) |
| CPU_user_pct | User CPU % process | perf / pidstat | 0.43 (recent 20250826-185117) | < 75% (S2) |
| RSS_peak_MB | Peak resident memory | /proc/PID/statm sampling | 7,36 (recent 20250826-185117) | Stable (< +5% over 30 min) |
| allocations_per_tick | Dynamic allocations (instrumented) | Custom counter (profiling build) | – | Reduce 50% after phase 1 |
| network_tx_bytes_per_sec | Outgoing bytes/sec | Metrics / tcpdump sample | – | N/A (observe) |
| off_cpu_wait_ns_p99 | Scheduler wait (blocking) | Off-CPU profile | 64000000 (recent 20250826-185117) | Minimize (< tick SLA) |

### 1.5 Performance SLA (Provisional)
- Normal gameplay (S2): p99 tick < 5 ms; mean tick < 2 ms.
- Heavy scenario (S3): p99 tick < 12 ms; mean tick < 6 ms.
- Long-run memory growth < 5% over 30 minutes (no unbounded leaks).
- Full snapshot not more frequent than configured interval; size spike smoothing (no double spikes).

### 1.6 Measurement Integrity Guidelines
- Use dedicated profiling build: `-O2 -g -fno-omit-frame-pointer` (no sanitizers) for timing; separate runs with sanitizers only for correctness.
- Warm-up: discard first 5 seconds (JIT/allocator warm). (No JIT here, but initial allocation bursts.)
- Repeat each measurement at least 3 runs; report mean + stdev.
- Profiling overhead kept < 5% (sampling frequency 400 Hz unless hotspot requires finer granularity).

---
## 2. Profiling Strategy (Toolbox)
| Goal | Tool(s) | Notes |
|------|---------|-------|
| CPU hotspots | `perf record -F 400 -g -p <pid>` + FlameGraph | Save SVG under `profiles/cpu/DATE/` |
| Off-CPU waits | `perf record -e sched:sched_switch -g -p <pid>` + OffCPU FlameGraph | Identify blocking gaps |
| Lock contention | `perf lock record -p <pid>` / `perf lock report` | Only if mutex hotspots suspected |
| Memory allocations | `heaptrack`, `pprof` (tcmalloc), custom counters | Allocation site ranking |
| Microbench (serialization) | Google Benchmark (future) | Deterministic input set |
| Long-run leak / growth | `valgrind massif` (short), RSS sampling script | Massif only on smaller scenario |
| Network throughput | `nstat`, `ss`, internal counters | Burst vs sustained |
| Physics cost | Scoped timers around Box2D step | Attribute % of tick |

Outcome artifacts: store under `profiles/<category>/<YYYYMMDD-HHMM>/` with README per run (scenario, build hash, command line, commit SHA).

---
## 3. Metrics & Targets (Expandable)
Initial targets defined in 1.4. Future columns to add once baseline captured: Baseline, Post-Phase1, Post-Phase2.

---
## 4. Risk Register (Performance-Specific)
| Risk | Impact | Mitigation | Owner | Status |
|------|--------|-----------|-------|--------|
| Snapshot serialization spike | Tick SLA breach | Incremental buffers, preallocation | TBD | OPEN |
| Bot AI O(N) with large N | Unscalable CPU | Variable decision frequency, LOD AI | TBD | OPEN |
| Excessive allocations | Cache misses | Pool / arena allocators | TBD | OPEN |
| Logging I/O bursts | Jitter | Async ring buffer logger | TBD | OPEN |
| Physics broadphase cost | Tick spike | Spatial partition tuning | TBD | OPEN |

---
## 5. Action Backlog (Phased Roadmap)
### Phase 0: Baseline Capture (NO CODE CHANGES for optimization)
- [ ] Add profiling CMake option `T2D_ENABLE_PROFILING` (lightweight instrumentation toggles)
- [ ] Scripts: `scripts/profile_cpu.sh`, `scripts/profile_offcpu.sh`
- [ ] Run scenarios S1–S3; collect CPU + Off-CPU FlameGraphs
- [ ] Record baseline metrics table
- [ ] Document top 10 symbols by CPU (%)

Exit criteria: Baseline metrics & artifacts committed (or stored as CI artifacts) + initial hotspot list.

### Phase 1: Memory & Allocation Reduction
- [ ] Instrument allocation counts per tick (profiling build)
- [ ] Pool frequently recreated buffers (snapshot encode temp, projectile arrays)
- [ ] Reduce string formatting in hot path (structured logging fields preformatted)
- [ ] Re-measure allocations_per_tick (target 50% reduction)

### Phase 2: Snapshot Serialization Optimization
- [ ] Profile serialization inclusive time
- [ ] Introduce arena / contiguous builder to avoid per-entity small allocs
- [ ] Optional compression experiment (zstd) behind config flag
- [ ] Evaluate delta field thresholding (skip near-zero movement)

### Phase 3: Tick Loop Jitter & Scheduling
- [ ] Add watchdog for tick > SLA logging with backtrace
- [ ] Off-CPU analysis to identify sleeping / blocking
- [ ] Optimize lock granularity (if contention found)

### Phase 4: AI Scaling
- [ ] Adaptive decision frequency based on bot count
- [ ] Batch path / target selection

### Phase 5: Physics Cost
- [ ] Time Box2D step proportion; tune iteration counts
- [ ] Spatial partition or pruning (only active bodies)

(Phases beyond 5 will be appended as new bottlenecks appear.)

---
## 6. Improvement Log (Append Per Completed Item)
Template Row Format:
```
| YYYY-MM-DD | Phase | Change | Before (metric) | After (metric) | % Delta | Scenario | Notes |
```
Initial Table:
| Date | Phase | Change | Before | After | % Delta | Scenario | Notes |
|------|-------|--------|--------|-------|---------|----------|-------|

---
## 7. Operating Procedures (Quick Guides)
### 7.1 Capture CPU FlameGraph (Running Server PID)
```
perf record -F 400 -g -p <PID> -- sleep 30
perf script | stackcollapse-perf.pl > out.folded
flamegraph.pl out.folded > cpu_flame.svg
```
Store under `profiles/cpu/<timestamp>/` with a README containing:
- Commit SHA
- Build flags
- Scenario (S1/S2/S3 etc.)
- Command line
- Notes (anomalies)

### 7.2 Off-CPU Profile
```
perf record -e sched:sched_switch -g -p <PID> -- sleep 30
perf script | stackcollapse-perf.pl --kernel | flamegraph.pl --title "Off-CPU" > offcpu_flame.svg
```
Focus: large off-CPU stacks inside lock waits, poll, futex.

### 7.3 Lock Contention Sample
```
perf lock record -p <PID> -- sleep 10
perf lock report
```
Investigate locks with highest acquired wait time.

### 7.4 Memory Allocation (heaptrack)
```
heaptrack ./t2d_server <args>
heaptrack_print heaptrack.<PID>.zst > heaptrack.txt
```
Rank by inclusive cost.

### 7.5 Metrics Snapshot Script (Planned)
A future script will query `/metrics` endpoint 1 Hz, writing `metrics_<timestamp>.log` for correlation with perf samples.

---
## 8. Definition of Done (Performance Initiative)
A phase is considered DONE when:
1. Baseline & post-change metrics recorded in Section 6.
2. Regression guard (CI script or benchmark) updated.
3. Documentation of trade-offs / configs added (if new flags introduced).
4. No degradation (>5%) in unrelated key metrics (tick p99, memory, snapshot size) unless justified.

Global initiative reaches initial completion when Phases 0–3 are DONE and p99 tick SLA met for S2 & S3.

---
## 9. Governance / Update Policy
- Update this file in the same PR as performance changes (append Improvement Log row + adjust backlog).
- If a hypothesis disproved, move it to a Retired Hypotheses list (to be added when first occurs) rather than deleting (knowledge retention).

---
## 10. Next Immediate Actions
(Reflects entry state — to be updated after first baseline commit.)
- [ ] Implement Phase 0 tasks (profiling option + scripts) and capture first baseline.

---
Maintainers: Add yourselves here when contributing performance changes.

*S2* run baseline reference (timestamp 20250826-172346; 20 synthetic clients, 90s). Pending additions: p99 tick, CPU_user_pct, RSS_peak_MB, off_cpu_wait_ns_p99 once captured with dedicated tooling.
\n<!-- BASELINE_RUN_20250826-163238 -->
Baseline capture 20250826-163238:
- avg_tick_ns=0 (~0,000 ms)
- snapshot_full_bytes_total=0 (count=0)
- snapshot_delta_bytes_total=0 (count=0)
- clients=6 duration=20s port=40000
- cpu_profile=baseline_artifacts/20250826-163238/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-163238/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-163343 -->
Baseline capture 20250826-163343:
- avg_tick_ns=0 (~0,000 ms)
- snapshot_full_bytes_total=0 (count=0)
- snapshot_delta_bytes_total=0 (count=0)
- clients=6 duration=25s port=40000
- cpu_profile=baseline_artifacts/20250826-163343/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-163343/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-163439 -->
Baseline capture 20250826-163439:
- avg_tick_ns=0 (~0,000 ms)
- snapshot_full_bytes_total=0 (count=0)
- snapshot_delta_bytes_total=0 (count=0)
- clients=6 duration=15s port=40000
- cpu_profile=baseline_artifacts/20250826-163439/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-163439/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-170305 -->
Baseline capture 20250826-170305:
- avg_tick_ns=0 (~0,000 ms)
- snapshot_full_bytes_total=0 (count=0)
- snapshot_delta_bytes_total=0 (count=0)
- clients=20 duration=70s port=40000
- cpu_profile=baseline_artifacts/20250826-170305/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-170305/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-170703 -->
Baseline capture 20250826-170703:
- avg_tick_ns=0 (~0,000 ms)
- snapshot_full_bytes_total=0 (count=0)
- snapshot_delta_bytes_total=0 (count=0)
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250826-170703/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-170703/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-171036 -->
Baseline capture 20250826-171036:
- avg_tick_ns=0 (~0,000 ms)
- snapshot_full_bytes_total=4671 (count=10)
- snapshot_delta_bytes_total=10707 (count=55)
- clients=4 duration=10s port=40000
- cpu_profile=baseline_artifacts/20250826-171036/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-171036/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-171109 -->
Baseline capture 20250826-171109:
- avg_tick_ns=73683 (~0,074 ms)
- snapshot_full_bytes_total=3340 (count=8)
- snapshot_delta_bytes_total=515 (count=45)
- clients=4 duration=8s port=40000
- cpu_profile=baseline_artifacts/20250826-171109/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-171109/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-171221 -->
Baseline capture 20250826-171221:
- avg_tick_ns=93461 (~0,093 ms)
- snapshot_full_bytes_total=69674 (count=167)
- snapshot_delta_bytes_total=7525 (count=843)
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250826-171221/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-171221/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-171707 -->
Baseline capture 20250826-171707:
- avg_tick_ns=152601 (~0,153 ms)
- snapshot_full_bytes_total=13242 (count=25)
- snapshot_delta_bytes_total=40459 (count=134)
- clients=8 duration=15s port=40000
- cpu_profile=baseline_artifacts/20250826-171707/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-171707/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-172003 -->
Baseline capture 20250826-172003:
- avg_tick_ns=288591 (~0,289 ms)
- snapshot_full_bytes_total=192328 (count=268)
- snapshot_delta_bytes_total=709234 (count=1350)
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250826-172003/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-172003/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-172346 -->
Baseline capture 20250826-172346:
- avg_tick_ns=287409 (~0,287 ms)
- snapshot_full_bytes_total=187891 (count=268)
- snapshot_delta_bytes_total=612481 (count=1350)
- snapshot_full_mean_bytes=701,09
- snapshot_delta_mean_bytes=453,69
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250826-172346/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-172346/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-174241 -->
Baseline capture 20250826-174241:
- avg_tick_ns=311717 (~0,312 ms)
- snapshot_full_bytes_total=192470 (count=268)
- snapshot_delta_bytes_total=672825 (count=1350)
- snapshot_full_mean_bytes=718,17
- snapshot_delta_mean_bytes=498,39
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250826-174241/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-174241/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-174641 -->
Baseline capture 20250826-174641:
- avg_tick_ns=296626 (~0,297 ms)
- snapshot_full_bytes_total=119023 (count=162)
- snapshot_delta_bytes_total=414493 (count=813)
- snapshot_full_mean_bytes=734,71
- snapshot_delta_mean_bytes=509,83
- clients=12 duration=60s port=40000
- cpu_profile=baseline_artifacts/20250826-174641/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-174641/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-174800 -->
Baseline capture 20250826-174800:
- avg_tick_ns=196427 (~0,196 ms)
- snapshot_full_bytes_total=106247 (count=162)
- snapshot_delta_bytes_total=304827 (count=813)
- snapshot_full_mean_bytes=655,85
- snapshot_delta_mean_bytes=374,94
- clients=12 duration=60s port=40000
- cpu_profile=baseline_artifacts/20250826-174800/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-174800/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-175052 -->
Baseline capture 20250826-175052:
- avg_tick_ns=232702 (~0,233 ms)
- snapshot_full_bytes_total=45874 (count=67)
- snapshot_delta_bytes_total=173868 (count=336)
- snapshot_full_mean_bytes=684,69
- snapshot_delta_mean_bytes=517,46
- clients=12 duration=35s port=40000
- cpu_profile=baseline_artifacts/20250826-175052/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-175052/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-175420 -->
Baseline capture 20250826-175420:
- avg_tick_ns=175585 (~0,176 ms)
- snapshot_full_bytes_total=48649 (count=77)
- snapshot_delta_bytes_total=156992 (count=387)
- snapshot_full_mean_bytes=631,81
- snapshot_delta_mean_bytes=405,66
- clients=20 duration=40s port=40000
- cpu_profile=baseline_artifacts/20250826-175420/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-175420/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-175656 -->
Baseline capture 20250826-175656:
- avg_tick_ns=174801 (~0,175 ms)
- snapshot_full_bytes_total=30236 (count=47)
- snapshot_delta_bytes_total=111477 (count=235)
- snapshot_full_mean_bytes=643,32
- snapshot_delta_mean_bytes=474,37
- clients=8 duration=25s port=40000
- cpu_profile=baseline_artifacts/20250826-175656/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-175656/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-175909 -->
Baseline capture 20250826-175909:
- avg_tick_ns=132937 (~0,133 ms)
- snapshot_full_bytes_total=19983 (count=36)
- snapshot_delta_bytes_total=57740 (count=185)
- snapshot_full_mean_bytes=555,08
- snapshot_delta_mean_bytes=312,11
- clients=8 duration=20s port=40000
- cpu_profile=baseline_artifacts/20250826-175909/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-175909/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-180130 -->
Baseline capture 20250826-180130:
- avg_tick_ns=150134 (~0,150 ms)
- snapshot_full_bytes_total=27627 (count=46)
- snapshot_delta_bytes_total=81191 (count=235)
- snapshot_full_mean_bytes=600,59
- snapshot_delta_mean_bytes=345,49
- clients=10 duration=25s port=40000
- cpu_profile=baseline_artifacts/20250826-180130/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-180130/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-180351 -->
Baseline capture 20250826-180351:
- avg_tick_ns=137619 (~0,138 ms)
- snapshot_full_bytes_total=18088 (count=32)
- snapshot_delta_bytes_total=55840 (count=165)
- snapshot_full_mean_bytes=565,25
- snapshot_delta_mean_bytes=338,42
- clients=8 duration=18s port=40000
- cpu_profile=baseline_artifacts/20250826-180351/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-180351/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-180439 -->
Baseline capture 20250826-180439:
- avg_tick_ns=297725 (~0,298 ms)
- snapshot_full_bytes_total=74910 (count=119)
- snapshot_delta_bytes_total=236122 (count=597)
- snapshot_full_mean_bytes=629,50
- snapshot_delta_mean_bytes=395,51
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250826-180439/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-180439/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-180618 -->
Baseline capture 20250826-180618:
- avg_tick_ns=287453 (~0,287 ms)
- snapshot_full_bytes_total=79376 (count=119)
- snapshot_delta_bytes_total=298465 (count=597)
- snapshot_full_mean_bytes=667,03
- snapshot_delta_mean_bytes=499,94
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250826-180618/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-180618/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-181746 -->
Baseline capture 20250826-181746:
- avg_tick_ns=104897 (~0,105 ms)
- p99_tick_ns=500000 (~0,500 ms)
- snapshot_full_bytes_total=6982 (count=15)
- snapshot_delta_bytes_total=14749 (count=85)
- snapshot_full_mean_bytes=465,47
- snapshot_delta_mean_bytes=173,52
- clients=6 duration=10s port=40000
- cpu_profile=baseline_artifacts/20250826-181746/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-181746/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-182356 -->
Baseline capture 20250826-182356:
- avg_tick_ns=126857 (~0,127 ms)
- p99_tick_ns=1000000 (~1,000 ms)
- snapshot_full_bytes_total=417 (count=1)
- snapshot_delta_bytes_total=735 (count=5)
- snapshot_full_mean_bytes=417,00
- snapshot_delta_mean_bytes=147,00
- wait_p99_ns=64000000 (~64,000 ms)
- cpu_user_pct=[2025-08-26 18:24:03.962] [I] {"metric":"runtime_final","avg_tick_ns":126857,"p99_tick_ns":1000000,"wait_p99_ns":64000000,"cpu_user_pct":{:.2f},"rss_peak_bytes":0.333217,"allocs_per_tick_mean":{:.2f},"samples":7192576,"queue_depth":0.000000,"active_matches":31,"bots_in_match":0,"projectiles_active":1,"connected_players":2} 0 2
- rss_peak_bytes=0 (~0,00 MB)
- allocs_per_tick_mean=[2025-08-26 18:24:03.962] [I] {"metric":"runtime_final","avg_tick_ns":126857,"p99_tick_ns":1000000,"wait_p99_ns":64000000,"cpu_user_pct":{:.2f},"rss_peak_bytes":0.333217,"allocs_per_tick_mean":{:.2f},"samples":7192576,"queue_depth":0.000000,"active_matches":31,"bots_in_match":0,"projectiles_active":1,"connected_players":2} 0 2
- clients=2 duration=5s port=40000
- cpu_profile=baseline_artifacts/20250826-182356/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-182356/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-182712 -->
Baseline capture 20250826-182712:
- avg_tick_ns=97620 (~0,098 ms)
- p99_tick_ns=250000 (~0,250 ms)
- snapshot_full_bytes_total=417 (count=1)
- snapshot_delta_bytes_total=735 (count=5)
- snapshot_full_mean_bytes=417,00
- snapshot_delta_mean_bytes=147,00
- wait_p99_ns=64000000 (~64,000 ms)
- cpu_user_pct=0.33
- rss_peak_bytes=7454720 (~7,11 MB)
- allocs_per_tick_mean=0.00
- clients=2 duration=5s port=40000
- cpu_profile=baseline_artifacts/20250826-182712/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-182712/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-183018 -->
Baseline capture 20250826-183018:
- avg_tick_ns=0 (~0,000 ms)
- p99_tick_ns=0 (~0.000 ms)
- snapshot_full_bytes_total=0 (count=0)
- snapshot_delta_bytes_total=0 (count=0)
- snapshot_full_mean_bytes=0
- snapshot_delta_mean_bytes=0
- wait_p99_ns=0 (~0.000 ms)
- cpu_user_pct=0.00
- rss_peak_bytes=7192576 (~6,86 MB)
- allocs_per_tick_mean=0.00
- clients=1 duration=3s port=40000
- cpu_profile=baseline_artifacts/20250826-183018/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-183018/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-183048 -->
Baseline capture 20250826-183048:
- avg_tick_ns=0 (~0,000 ms)
- p99_tick_ns=0 (~0.000 ms)
- snapshot_full_bytes_total=0 (count=0)
- snapshot_delta_bytes_total=0 (count=0)
- snapshot_full_mean_bytes=0
- snapshot_delta_mean_bytes=0
- wait_p99_ns=0 (~0.000 ms)
- cpu_user_pct=0.00
- rss_peak_bytes=7208960 (~6,88 MB)
- allocs_per_tick_mean=0.00
- clients=1 duration=2s port=40000
- cpu_profile=baseline_artifacts/20250826-183048/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-183048/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-183237 -->
Baseline capture 20250826-183237:
- avg_tick_ns=0 (~0,000 ms)
- p99_tick_ns=0 (~0.000 ms)
- snapshot_full_bytes_total=0 (count=0)
- snapshot_delta_bytes_total=0 (count=0)
- snapshot_full_mean_bytes=0
- snapshot_delta_mean_bytes=0
- wait_p99_ns=0 (~0.000 ms)
- cpu_user_pct=0.00
- rss_peak_bytes=7192576 (~6,86 MB)
- allocs_per_tick_mean=0.00
- clients=1 duration=2s port=40000
- cpu_profile=baseline_artifacts/20250826-183237/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-183237/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-183305 -->
Baseline capture 20250826-183305:
- avg_tick_ns=179503 (~0,180 ms)
- p99_tick_ns=2000000 (~2,000 ms)
- snapshot_full_bytes_total=834 (count=2)
- snapshot_delta_bytes_total=1325 (count=10)
- snapshot_full_mean_bytes=417,00
- snapshot_delta_mean_bytes=132,50
- wait_p99_ns=64000000 (~64,000 ms)
- cpu_user_pct=0.57
- rss_peak_bytes=7585792 (~7,23 MB)
- allocs_per_tick_mean=0.00
- clients=2 duration=6s port=40000
- cpu_profile=baseline_artifacts/20250826-183305/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-183305/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-185117 -->
Baseline capture 20250826-185117:
- avg_tick_ns=100311 (~0,100 ms)
- p99_tick_ns=500000 (~0,500 ms)
- snapshot_full_bytes_total=834 (count=2)
- snapshot_delta_bytes_total=1325 (count=10)
- snapshot_full_mean_bytes=417,00
- snapshot_delta_mean_bytes=132,50
- wait_p99_ns=64000000 (~64,000 ms)
- cpu_user_pct=0.43
- rss_peak_bytes=7716864 (~7,36 MB)
- allocs_per_tick_mean=0.00
- clients=2 duration=6s port=40000
- cpu_profile=baseline_artifacts/20250826-185117/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-185117/offcpu/offcpu_flame.svg (if generated)
