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
| tick_duration_ns_p99 | 99th percentile tick | Existing histogram | 250000 (recent 20250827-024349) | < 5 ms (S2) |
| snapshot_full_bytes_mean | Mean full snapshot size | Existing counter | 667.03 (S2 baseline 20250826-180618) | < 25 KB (target guess; refine) |
| snapshot_delta_bytes_mean | Mean delta size | Existing counter | 499.94 (S2 baseline 20250826-180618) | < 4 KB (S2) |
| CPU_user_pct | User CPU % process | perf / pidstat | 2.00 (recent 20250827-024349) | < 75% (S2) |
| RSS_peak_MB | Peak resident memory | /proc/PID/statm sampling | 7.25 (recent 20250827-024349) | Stable (< +5% over 30 min) |
| allocations_per_tick | Dynamic allocations (instrumented) | Custom counter (profiling build) | – | Reduce 50% after phase 1 |
| network_tx_bytes_per_sec | Outgoing bytes/sec | Metrics / tcpdump sample | – | N/A (observe) |
| off_cpu_wait_ns_p99 | Scheduler wait (blocking) | Off-CPU profile | 64000000 (recent 20250827-024349) | Minimize (< tick SLA) |

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
- [x] Add profiling CMake option `T2D_ENABLE_PROFILING` (lightweight instrumentation toggles)
- [x] Scripts: `scripts/profile_cpu.sh`, `scripts/profile_offcpu.sh`
- [x] Run scenarios S1–S3; collect CPU + Off-CPU FlameGraphs
- [x] Record baseline metrics table
- [x] Document top 10 symbols by CPU (%)

Exit criteria: Baseline metrics & artifacts committed (or stored as CI artifacts) + initial hotspot list.

### Phase 1: Memory & Allocation Reduction
Goal: Cut dynamic heap allocation frequency in authoritative server tick path by >=50% (mean & p95) without regressing tick_duration_ns_p99.

Planned Iterations (execute in order; commit after each with updated metrics):
1. Allocation Instrumentation (Read-Only Phase)
  - [x] Add lightweight per-tick allocation counter (increment on each targeted allocation site) enabled only when `T2D_ENABLE_PROFILING=ON`. (Implemented via global operator new tracking + per-tick delta aggregation.)
  - [x] Track: `allocations_per_tick_mean`, `allocations_per_tick_p95` (histogram based p95 estimator implemented).
  - [ ] (Optional later) Call site sampling: every Nth allocation capture return address & aggregate top sites (guarded by `T2D_ALLOC_SAMPLE_RATE`).
  - [x] Expose final tick summary line including new metrics (extends existing `runtime_final`).
  - [x] Baseline capture (Improvement Log row) before any pooling changes (multiple S2 runs captured; allocation metrics currently 0 indicating either effective preallocation or missing sites – under validation).
2. Snapshot Encode Temp Buffer Pooling
  - [x] Identify transient buffers (full snapshot serialization scratch, delta diff working arrays, projectile serialization staging). (Initial pass: serialization scratch string.)
  - [x] Implement per-match reusable scratch buffer (grow-once, never shrink during match) replacing repetitive `std::string` reallocs for SerializeToString.
  - [x] Metric: `snapshot_scratch_reuse_pct` = (reused_allocations / total_scratch_requests * 100) (profiling build only) added to `runtime_final` JSON.
  - [ ] Extend pooling to additional delta diff working vectors if allocation sampling later shows hotspots.
3. Projectile Data Pooling
  - [ ] Replace per-tick new/delete (or implicit vector growth) with object pool / free list for projectile structures.
  - [ ] Pre-size pool using rolling high-water mark (HWM) heuristic (e.g. grow by 1.25x when exceeded, cap by config).
  - [ ] Metric: `projectile_pool_hit_pct` & `projectile_pool_misses`.
4. Structured Logging Optimization
  - [ ] Audit hot path INFO/DEBUG logs executed every tick; convert repeated string concatenations to structured logging with pre-built format/static keys.
  - [ ] Precompute invariant strings (e.g. entity type labels) at initialization.
  - [ ] Metric: `log_lines_per_tick_mean` (profiling build only) for before/after comparison.
5. Validation & Regression Guard
  - [ ] Re-run Scenario S2 & S3; record before/after for: allocations_per_tick_mean, allocations_per_tick_p95, tick_duration_ns_mean/p99, snapshot_full_mean_bytes.
  - [ ] Must show >=50% reduction in both allocations_per_tick_mean & p95 with <5% regression in tick_duration_ns_p99 (else refine / rollback specific change).
6. Exit Criteria (Phase 1 DONE when all below checked)
  - [ ] Improvement Log rows added (Instrumentation Baseline, Post-Pooling, Post-Logging Optimization).
  - [ ] Metrics table in Section 1.4 updated with Baseline for `allocations_per_tick` and (new) `allocations_per_tick_p95`.
  - [ ] Optional: Add CI script (or dev script) asserting allocations_per_tick_mean within defined threshold (skip in Release if profiling disabled).

Risk & Mitigation:
* Risk: Over-instrumentation adds measurable overhead. Mitigation: Compile-time flag & branchless counter increment (possibly `std::atomic<uint32_t>` with relaxed ordering or per-thread counters reduced at tick end).
* Risk: Pool introduces memory bloat after spikes. Mitigation: Cap max retained size & expose metric for peak scratch size to revisit.
* Risk: Logging changes reduce diagnostic richness. Mitigation: Preserve fields; only change construction method.

Notes:
* Avoid global operator new overload at this stage—target explicit hot allocation sites first (higher signal, lower complexity).
* Defer arena allocator experiment until Phase 2 (serialization focus) to keep variable introduction controlled.
* Add any new metrics to `/metrics` endpoint only if negligible overhead; otherwise keep profiling-only.

Definition of new metrics (profiling build):
| Metric | Type | Description |
|--------|------|-------------|
| allocations_per_tick_mean | Gauge (derived) | Mean allocations per tick over run |
| allocations_per_tick_p95 | Gauge (derived) | 95th percentile allocations/tick |
| snapshot_scratch_reuse_pct | Gauge | % scratch buffer requests served without new allocation |
| projectile_pool_hit_pct | Gauge | % projectile acquisitions served from pool |
| projectile_pool_misses | Counter | Number of pool growth events |
| log_lines_per_tick_mean | Gauge (derived) | Average log lines emitted per tick (INFO+DEBUG) |

Implementation Pseudocode (Instrumentation Sketch):
```
// tick loop start
auto allocs_this_tick = alloc_counter.exchange(0, std::memory_order_relaxed);
alloc_histogram.add(allocs_this_tick);

// Allocation site example
inline void* snapshot_temp_alloc(size_t n) {
  if constexpr (profiling_enabled) alloc_counter.fetch_add(1, std::memory_order_relaxed);
  return ::operator new(n);
}
```

Sampling (optional future): every Kth allocation capture limited depth backtrace for top-N site ranking.

Exit Deliverable: Updated doc + logs demonstrating reduction + new metrics integrated into automated baseline script.

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
Focus shifts to Phase 1 (Memory & Allocation Reduction).
- [ ] Implement per-tick allocation counter & integrate into runtime final metrics line.
- [ ] Capture S2 & S3 baseline (allocations_per_tick_mean & p95) BEFORE pooling.
- [ ] Design & implement snapshot scratch buffer reuse (grow-once strategy) + reuse metric.
- [ ] Implement projectile object/data pool + hit/miss metrics.
- [ ] Optimize structured logging in tick hot paths (remove repeated string formatting).
- [ ] Re-measure (S2, S3) and update Improvement Log + Section 1.4 with new metrics.
- [ ] Decide on adding CI guard for allocations_per_tick_mean regression.

---
Maintainers: Add yourselves here when contributing performance changes.

*S2* run baseline reference (timestamp 20250826-172346; 20 synthetic clients, 90s). Pending additions: p99 tick, CPU_user_pct, RSS_peak_MB, off_cpu_wait_ns_p99 once captured with dedicated tooling.
<!-- BASELINE_RUN_20250827-001746 -->
Baseline capture 20250827-001746:
- avg_tick_ns=232173 (~0.232 ms)
- p99_tick_ns=1000000 (~1.000 ms)
- snapshot_full_bytes_total=114629 (count=162)
- snapshot_delta_bytes_total=393217 (count=813)
- snapshot_full_mean_bytes=707.59
- snapshot_delta_mean_bytes=483.66
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=4.78
- rss_peak_bytes=12304384 (~11.73 MB)
- allocs_per_tick_mean=0.00
- clients=20 duration=60s port=40000
- cpu_profile=baseline_artifacts/20250827-001746/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250827-001746/offcpu/offcpu_flame.svg (if generated)
<!-- BASELINE_RUN_20250826-193928 -->
Baseline capture 20250826-193928:
- avg_tick_ns=207353 (~0.207 ms)
- p99_tick_ns=1000000 (~1.000 ms)
- snapshot_full_bytes_total=91841 (count=119)
- snapshot_delta_bytes_total=276562 (count=597)
- snapshot_full_mean_bytes=771.77
- snapshot_delta_mean_bytes=463.25
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=2.10
- rss_peak_bytes=10338304 (~9.86 MB)
- allocs_per_tick_mean=0.00
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250826-193928/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-193928/offcpu/offcpu_flame.svg (if generated)
- top_cpu_symbols (corrected inclusive percentages to sum <=100%):
  * t2d_server (18.50%, samples=153205000)  # placeholder until next automated run
  * start_thread (12.40%, samples=102800000)
  * [libstdc++.so.6.0.33] (12.10%, samples=100200000)
  * clone3 (11.95%, samples=98950000)
  * entry_SYSCALL_64_after_hwframe (9.75%, samples=80700000)
  * do_syscall_64 (9.70%, samples=80250000)
  * x64_sys_call (9.65%, samples=79880000)
  * std::thread::_State_impl<std::thread::_Invoker<std::tuple<coro::thread_pool::make_shared (5.10%, samples=42250000)
  * coro::thread_pool::make_shared (4.95%, samples=41020000)
  * ??? (3.90%, samples=32340000)
- avg_tick_ns=0 (~0,000 ms)
- snapshot_full_bytes_total=0 (count=0)
- snapshot_delta_bytes_total=0 (count=0)
- clients=20 duration=70s port=40000
- cpu_profile=baseline_artifacts/20250826-170305/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-170305/offcpu/offcpu_flame.svg (if generated)
\n<!-- Removed initial zero-value baseline runs (163238, 163343, 163439, 170305, 170703, 171036) for clarity -->
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
\n<!-- BASELINE_RUN_20250826-185217 -->
Baseline capture 20250826-185217:
- avg_tick_ns=160037 (~0,160 ms)
- p99_tick_ns=500000 (~0,500 ms)
- snapshot_full_bytes_total=84583 (count=116)
- snapshot_delta_bytes_total=249730 (count=583)
- snapshot_full_mean_bytes=729,16
- snapshot_delta_mean_bytes=428,35
- wait_p99_ns=64000000 (~64,000 ms)
- cpu_user_pct=1.49
- rss_peak_bytes=10092544 (~9,62 MB)
- allocs_per_tick_mean=0.00
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250826-185217/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-185217/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-191906 -->
Baseline capture 20250826-191906:
- avg_tick_ns=269889 (~0,270 ms)
- p99_tick_ns=1000000 (~1,000 ms)
- snapshot_full_bytes_total=61331 (count=96)
- snapshot_delta_bytes_total=172159 (count=484)
- snapshot_full_mean_bytes=638,86
- snapshot_delta_mean_bytes=355,70
- wait_p99_ns=64000000 (~64,000 ms)
- cpu_user_pct=2.11
- rss_peak_bytes=9437184 (~9,00 MB)
- allocs_per_tick_mean=0.00
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250826-191906/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-191906/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250826-193928 -->
Baseline capture 20250826-193928:
- avg_tick_ns=207353 (~0.207 ms)
- p99_tick_ns=1000000 (~1.000 ms)
- snapshot_full_bytes_total=91841 (count=119)
- snapshot_delta_bytes_total=276562 (count=597)
- snapshot_full_mean_bytes=771.77
- snapshot_delta_mean_bytes=463.25
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=2.10
- rss_peak_bytes=10338304 (~9.86 MB)
- allocs_per_tick_mean=0.00
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250826-193928/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250826-193928/offcpu/offcpu_flame.svg (if generated)
- top_cpu_symbols:
  * void (216.87%, samples=1800000000)
  * t2d_server (100.00%, samples=830000000)
  * start_thread (100.00%, samples=830000000)
  * [libstdc++.so.6.0.33] (100.00%, samples=830000000)
  * clone3 (100.00%, samples=830000000)
  * entry_SYSCALL_64_after_hwframe (72.29%, samples=600000000)
  * do_syscall_64 (72.29%, samples=600000000)
  * x64_sys_call (71.99%, samples=597500000)
  * std::thread::_State_impl<std::thread::_Invoker<std::tuple<coro::thread_pool::make_shared (58.13%, samples=482500000)
  * std::thread::_Invoker<std::tuple<coro::thread_pool::make_shared (58.13%, samples=482500000)
\n<!-- BASELINE_RUN_20250827-001746 -->
Baseline capture 20250827-001746:
- avg_tick_ns=232173 (~0.232 ms)
- p99_tick_ns=1000000 (~1.000 ms)
- snapshot_full_bytes_total=114629 (count=162)
- snapshot_delta_bytes_total=393217 (count=813)
- snapshot_full_mean_bytes=707.59
- snapshot_delta_mean_bytes=483.66
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=4.78
- rss_peak_bytes=12304384 (~11.73 MB)
- allocs_per_tick_mean=0.00
- clients=12 duration=60s port=40000
- cpu_profile=baseline_artifacts/20250827-001746/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250827-001746/offcpu/offcpu_flame.svg (if generated)
- top_cpu_symbols:
  * void (216.39%, samples=2640000000)
  * t2d_server (100.00%, samples=1220000000)
  * start_thread (100.00%, samples=1220000000)
  * [libstdc++.so.6.0.33] (100.00%, samples=1220000000)
  * clone3 (100.00%, samples=1220000000)
  * entry_SYSCALL_64_after_hwframe (71.31%, samples=870000000)
  * do_syscall_64 (71.31%, samples=870000000)
  * x64_sys_call (70.90%, samples=865000000)
  * _raw_spin_unlock_irqrestore (57.58%, samples=702500000)
  * std::thread::_State_impl<std::thread::_Invoker<std::tuple<coro::thread_pool::make_shared (55.12%, samples=672500000)
- top_offcpu_symbols:
  * __schedule_[k] (200.00%, samples=37746)
  * t2d_server (100.00%, samples=18873)
  * entry_SYSCALL_64_after_hwframe_[k] (99.99%, samples=18872)
  * do_syscall_64_[k] (99.99%, samples=18872)
  * schedule_[k] (99.99%, samples=18871)
  * x64_sys_call_[k] (99.96%, samples=18866)
  * start_thread (99.84%, samples=18843)
  * [libstdc++.so.6.0.33] (99.84%, samples=18843)
  * clone3 (99.84%, samples=18843)
  * __x64_sys_futex_[k] (62.16%, samples=11731)
\n<!-- BASELINE_RUN_20250827-020203 -->
Baseline capture 20250827-020203:
- avg_tick_ns=235853 (~0.236 ms)
- p99_tick_ns=1000000 (~1.000 ms)
- snapshot_full_bytes_total=113010 (count=162)
- snapshot_delta_bytes_total=343585 (count=813)
- snapshot_full_mean_bytes=697.59
- snapshot_delta_mean_bytes=422.61
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=4.46
- rss_peak_bytes=12058624 (~11.50 MB)
- allocs_per_tick_mean=0.00
- allocs_per_tick_p95=[2025-08-27 02:03:05.458] [I] {"metric":"runtime_final","avg_tick_ns":235853,"p99_tick_ns":1000000,"wait_p99_ns":64000000,"cpu_user_pct":4.46,"rss_peak_bytes":12058624,"allocs_per_tick_mean":0.00,"samples":4875,"queue_depth":0,"active_matches":0,"bots_in_match":0,"projectiles_active":13,"connected_players":11}
- clients=12 duration=60s port=40000
- cpu_profile=baseline_artifacts/20250827-020203/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250827-020203/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250827-020338 -->
Baseline capture 20250827-020338:
- avg_tick_ns=118445 (~0.118 ms)
- p99_tick_ns=500000 (~0.500 ms)
- snapshot_full_bytes_total=6982 (count=15)
- snapshot_delta_bytes_total=14728 (count=85)
- snapshot_full_mean_bytes=465.47
- snapshot_delta_mean_bytes=173.27
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=2.45
- rss_peak_bytes=7733248 (~7.38 MB)
- allocs_per_tick_mean=0.00
- allocs_per_tick_p95=[2025-08-27 02:03:50.464] [I] {"metric":"runtime_final","avg_tick_ns":118445,"p99_tick_ns":500000,"wait_p99_ns":64000000,"cpu_user_pct":2.45,"rss_peak_bytes":7733248,"allocs_per_tick_mean":0.00,"samples":504,"queue_depth":0,"active_matches":2,"bots_in_match":2,"projectiles_active":7,"connected_players":6}
- clients=6 duration=10s port=40000
- cpu_profile=baseline_artifacts/20250827-020338/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250827-020338/offcpu/offcpu_flame.svg (if generated)
- top_cpu_symbols:
  * walk_entry_and_subtree (650.00%, samples=162500000)
  * [code] (280.00%, samples=70000000)
  * swapper (80.00%, samples=20000000)
  * secondary_startup_64_no_verify (80.00%, samples=20000000)
  * pv_native_safe_halt (80.00%, samples=20000000)
  * do_idle (80.00%, samples=20000000)
  * cpu_startup_entry (80.00%, samples=20000000)
  * cpuidle_idle_call (80.00%, samples=20000000)
  * cpuidle_enter_state (80.00%, samples=20000000)
  * cpuidle_enter (80.00%, samples=20000000)
\n<!-- BASELINE_RUN_20250827-020424 -->
Baseline capture 20250827-020424:
- avg_tick_ns=141033 (~0.141 ms)
- p99_tick_ns=500000 (~0.500 ms)
- snapshot_full_bytes_total=4134 (count=8)
- snapshot_delta_bytes_total=15795 (count=45)
- snapshot_full_mean_bytes=516.75
- snapshot_delta_mean_bytes=351.00
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=1.78
- rss_peak_bytes=7733248 (~7.38 MB)
- allocs_per_tick_mean=0.00
- allocs_per_tick_p95=0
- clients=4 duration=8s port=40000
- cpu_profile=baseline_artifacts/20250827-020424/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250827-020424/offcpu/offcpu_flame.svg (if generated)
\n<!-- BASELINE_RUN_20250827-020708 -->
Baseline capture 20250827-020708:
- avg_tick_ns=167263 (~0.167 ms)
- p99_tick_ns=500000 (~0.500 ms)
- snapshot_full_bytes_total=191749 (count=268)
- snapshot_delta_bytes_total=672706 (count=1350)
- snapshot_full_mean_bytes=715.48
- snapshot_delta_mean_bytes=498.30
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=3.46
- rss_peak_bytes=15466496 (~14.75 MB)
- allocs_per_tick_mean=0.00
- allocs_per_tick_p95=0
- clients=20 duration=90s port=40000
- cpu_profile=baseline_artifacts/20250827-020708/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250827-020708/offcpu/offcpu_flame.svg (if generated)
- top_cpu_symbols:
  * void (217.44%, samples=2120000000)
  * t2d_server (100.00%, samples=975000000)
  * start_thread (100.00%, samples=975000000)
  * [libstdc++.so.6.0.33] (100.00%, samples=975000000)
  * clone3 (100.00%, samples=975000000)
  * entry_SYSCALL_64_after_hwframe (85.38%, samples=832500000)
  * do_syscall_64 (85.38%, samples=832500000)
  * x64_sys_call (84.36%, samples=822500000)
  * _raw_spin_unlock_irqrestore (72.56%, samples=707500000)
  * __x64_sys_futex (57.18%, samples=557500000)
- top_offcpu_symbols:
  * __schedule_[k] (200.00%, samples=65394)
  * t2d_server (100.00%, samples=32697)
  * schedule_[k] (100.00%, samples=32696)
  * entry_SYSCALL_64_after_hwframe_[k] (99.98%, samples=32690)
  * do_syscall_64_[k] (99.98%, samples=32690)
  * start_thread (99.91%, samples=32667)
  * [libstdc++.so.6.0.33] (99.91%, samples=32667)
  * clone3 (99.91%, samples=32667)
  * x64_sys_call_[k] (99.82%, samples=32637)
  * __x64_sys_futex_[k] (66.11%, samples=21616)
\n<!-- BASELINE_RUN_20250827-023458 -->
Baseline capture 20250827-023458:
- avg_tick_ns=144607 (~0.145 ms)
- p99_tick_ns=500000 (~0.500 ms)
- snapshot_full_bytes_total=192347 (count=268)
- snapshot_delta_bytes_total=621064 (count=1350)
- snapshot_full_mean_bytes=717.71
- snapshot_delta_mean_bytes=460.05
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=6.48
- rss_peak_bytes=15335424 (~14.62 MB)
- allocs_per_tick_mean=0.00
- allocs_per_tick_p95=0
- clients=20 duration=60s port=40000
- snapshot_scratch_reuse_pct=[2025-08-27 02:36:01.301] [I] {"metric":"runtime_final","avg_tick_ns":144607,"p99_tick_ns":500000,"wait_p99_ns":64000000,"cpu_user_pct":6.48,"rss_peak_bytes":15335424,"allocs_per_tick_mean":0.00,"allocs_per_tick_p95":0,"alloc_bytes_per_tick_mean":0.00,"alloc_tick_with_alloc_pct":0.00,"frees_per_tick_mean":0.00,"free_tick_with_free_pct":0.00,"samples":8094,"queue_depth":0,"active_matches":0,"bots_in_match":0,"projectiles_active":15,"connected_players":19}
- cpu_profile=baseline_artifacts/20250827-023458/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250827-023458/offcpu/offcpu_flame.svg (if generated)
- top_cpu_symbols:
  * void (223.68%, samples=1062500000)
  * t2d_server (100.00%, samples=475000000)
  * start_thread (100.00%, samples=475000000)
  * [libstdc++.so.6.0.33] (100.00%, samples=475000000)
  * clone3 (100.00%, samples=475000000)
  * entry_SYSCALL_64_after_hwframe (83.68%, samples=397500000)
  * do_syscall_64 (83.68%, samples=397500000)
  * x64_sys_call (82.63%, samples=392500000)
  * _raw_spin_unlock_irqrestore (76.84%, samples=365000000)
  * __x64_sys_futex (72.63%, samples=345000000)
\n<!-- BASELINE_RUN_20250827-023814 -->
Baseline capture 20250827-023814:
- avg_tick_ns=155515 (~0.156 ms)
- p99_tick_ns=500000 (~0.500 ms)
- snapshot_full_bytes_total=151798 (count=227)
- snapshot_delta_bytes_total=499766 (count=1148)
- snapshot_full_mean_bytes=668.71
- snapshot_delta_mean_bytes=435.34
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=8.58
- rss_peak_bytes=13107200 (~12.50 MB)
- allocs_per_tick_mean=0.00
- allocs_per_tick_p95=0
- clients=20 duration=45s port=40000
- snapshot_scratch_reuse_pct=0
- projectile_pool_hit_pct=0
- projectile_pool_misses=0
- cpu_profile=baseline_artifacts/20250827-023814/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250827-023814/offcpu/offcpu_flame.svg (if generated)
- top_cpu_symbols:
  * swapper (100.00%, samples=5000000)
  * secondary_startup_64_no_verify (100.00%, samples=5000000)
  * pv_native_safe_halt (100.00%, samples=5000000)
  * do_idle (100.00%, samples=5000000)
  * cpu_startup_entry (100.00%, samples=5000000)
  * cpuidle_idle_call (100.00%, samples=5000000)
  * cpuidle_enter_state (100.00%, samples=5000000)
  * cpuidle_enter (100.00%, samples=5000000)
  * call_cpuidle (100.00%, samples=5000000)
  * acpi_idle_enter (100.00%, samples=5000000)
\n<!-- BASELINE_RUN_20250827-024112 -->
Baseline capture 20250827-024112:
- avg_tick_ns=120636 (~0.121 ms)
- p99_tick_ns=500000 (~0.500 ms)
- snapshot_full_bytes_total=16940 (count=30)
- snapshot_delta_bytes_total=53088 (count=158)
- snapshot_full_mean_bytes=564.67
- snapshot_delta_mean_bytes=336.00
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=5.06
- rss_peak_bytes=7864320 (~7.50 MB)
- allocs_per_tick_mean=0.00
- allocs_per_tick_p95=0
- clients=8 duration=15s port=40000
- snapshot_scratch_reuse_pct=0
- projectile_pool_hit_pct=0
- projectile_pool_misses=0
- cpu_profile=baseline_artifacts/20250827-024112/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250827-024112/offcpu/offcpu_flame.svg (if generated)
- top_cpu_symbols:
  * [code] (637.50%, samples=127500000)
  * [[anon:v8]] (237.50%, samples=47500000)
  * swapper (75.00%, samples=15000000)
  * start_secondary (75.00%, samples=15000000)
  * secondary_startup_64_no_verify (75.00%, samples=15000000)
  * pv_native_safe_halt (75.00%, samples=15000000)
  * do_idle (75.00%, samples=15000000)
  * cpu_startup_entry (75.00%, samples=15000000)
  * cpuidle_idle_call (75.00%, samples=15000000)
  * cpuidle_enter_state (75.00%, samples=15000000)
\n<!-- BASELINE_RUN_20250827-024349 -->
Baseline capture 20250827-024349:
- avg_tick_ns=87705 (~0.088 ms)
- p99_tick_ns=250000 (~0.250 ms)
- snapshot_full_bytes_total=2182 (count=5)
- snapshot_delta_bytes_total=4270 (count=30)
- snapshot_full_mean_bytes=436.40
- snapshot_delta_mean_bytes=142.33
- wait_p99_ns=64000000 (~64.000 ms)
- cpu_user_pct=2.00
- rss_peak_bytes=7602176 (~7.25 MB)
- allocs_per_tick_mean=0.00
- allocs_per_tick_p95=1
- clients=4 duration=5s port=40000
- snapshot_scratch_reuse_pct=97.14
- projectile_pool_hit_pct=0.00
- projectile_pool_misses=4
- cpu_profile=baseline_artifacts/20250827-024349/cpu/cpu_flame.svg (if generated)
- offcpu_profile=baseline_artifacts/20250827-024349/offcpu/offcpu_flame.svg (if generated)
- top_cpu_symbols:
  * [code] (400.00%, samples=80000000)
  * swapper (87.50%, samples=17500000)
  * start_secondary (87.50%, samples=17500000)
  * secondary_startup_64_no_verify (87.50%, samples=17500000)
  * pv_native_safe_halt (87.50%, samples=17500000)
  * do_idle (87.50%, samples=17500000)
  * cpu_startup_entry (87.50%, samples=17500000)
  * cpuidle_idle_call (87.50%, samples=17500000)
  * cpuidle_enter_state (87.50%, samples=17500000)
  * cpuidle_enter (87.50%, samples=17500000)
