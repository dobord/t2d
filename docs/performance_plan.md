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
| tick_duration_ns_mean | Avg server tick time | Existing histogram | 287409 (S2* run) | < 2 ms (S2) |
| tick_duration_ns_p99 | 99th percentile tick | Existing histogram | – | < 5 ms (S2) |
| snapshot_full_bytes_mean | Mean full snapshot size | Existing counter | 701.09 (S2* run) | < 25 KB (target guess; refine) |
| snapshot_delta_bytes_mean | Mean delta size | Existing counter | 453.69 (S2* run) | < 4 KB (S2) |
| CPU_user_pct | User CPU % process | perf / pidstat | – | < 75% (S2) |
| RSS_peak_MB | Peak resident memory | /proc/PID/statm sampling | – | Stable (< +5% over 30 min) |
| allocations_per_tick | Dynamic allocations (instrumented) | Custom counter (profiling build) | – | Reduce 50% after phase 1 |
| network_tx_bytes_per_sec | Outgoing bytes/sec | Metrics / tcpdump sample | – | N/A (observe) |
| off_cpu_wait_ns_p99 | Scheduler wait (blocking) | Off-CPU profile | – | Minimize (< tick SLA) |

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
