<!-- SPDX-License-Identifier: Apache-2.0 -->
# Projectile Penetration Logic

Status: active
Owner: server gameplay / physics
Last Updated: 2025-08-28

## Overview

This document specifies the server-side projectile vs. tank penetration rule implemented in `server/game/match.cpp` (`process_contacts`). The goal is to produce stable, intention‑aligned hit outcomes by evaluating projectile kinematics *before* any collision response impulses alter velocity.

## Motivation for Change

Previous implementation used the **post-step (post-impulse)** projectile velocity to decide penetration. When the physics solver applied restitution or friction impulses, the projectile's normal component could drop sharply, yielding false negatives (projectile should have penetrated based on approach speed but appeared too slow afterward). This produced inconsistent gameplay, especially at shallow impact angles.

## Core Rule (Current)

Let:
- `v_pre` be the projectile linear velocity captured **before** the physics step for the tick in which the contact begins (`prev_vx`, `prev_vy`).
- `n` be the contact manifold normal (unit) directed from shapeA -> shapeB provided by Box2D.
- `a_is_proj` indicate whether contact body A is the projectile.
- `initial_speed` be the speed assigned at spawn (stored per projectile).

Define the normal (into-target) component using pre-step velocity:

```
vdotn_pre = dot(v_pre, n)
into_speed_pre =  a_is_proj ?  vdotn_pre : -vdotn_pre   // always positive when moving into target
```

Penetration condition:

```
into_speed_pre >= REQUIRED
REQUIRED = PENETRATION_FACTOR * initial_speed
PENETRATION_FACTOR = 0.60   // updated from 0.40
```

If condition is satisfied: damage is applied and projectile is consumed (destroyed). Otherwise the projectile is also consumed (no ricochet behavior implemented yet) but **no** damage is applied (logged as `result=NO`).

## Data Capture Timing

Each tick, before invoking the physics integration (`t2d::phys::step`), the server iterates all active projectile bodies and snapshots:

```
prev_x, prev_y, prev_vx, prev_vy
```

These values are immutable for the ensuing contact processing of that tick and represent the pre-collision kinematics.

## Diagnostic Logging

Two categories of trace logs support validation:

1. Spawn:
```
[proj_spawn] proj=<id> owner=<eid> pos=(x, y) muzzle_v=(vx, vy) body_v=(vx_body, vy_body) body_speed=S initial=I forward_offset=F
```
2. First post-step (age == 0 after first integration):
```
[proj_post_step0] proj=<id> owner=<eid> v=(vx, vy) speed=S initial=I
```
3. Penetration decision (one per projectile-tank contact begin):
```
[proj_penetration] proj=<pid> tank=<tid> into_pre=IP center_into_pre=C speed_pre=SP required=R initial=I vdotn_pre=DP n=(nx, ny) a_is_proj=B result=YES|NO
```

Field meanings:
- `into_pre`: Normal component magnitude of pre-step velocity into the target (primary decision metric).
- `center_into_pre`: Dot of pre-step velocity with normalized vector from pre-step projectile position to tank center (helps analyze glancing vs. center-line hits).
- `speed_pre`: Scalar speed before collision impulses.
- `vdotn_pre`: Signed dot product with the contact normal (before orientation flip via `a_is_proj`).
- `required`: Threshold (0.60 * initial).
- `a_is_proj`: Contact ordering diagnostic.

## Edge Cases & Notes

| Scenario | Handling |
|----------|----------|
| Projectile spawned and collides within same tick | Pre-step snapshot equals spawn kinematics (initialized on spawn). |
| Missing physics body (stale / invalid) | Falls back to stored kinematic fields; still consistent. |
| Very small approach speed (< 1e-6) | Fails threshold; logged normally. |
| Multiple simultaneous contacts same tick | Each contact evaluated independently; first successful damage application removes projectile after loop, others become moot. |
| Friendly fire (projectile owner == tank entity) | Skipped before decision (no penetration check performed). |

## Rationale for 60% Factor

Raising from 0.40 to 0.60 tightens penetration so only sufficiently direct, energetic hits succeed, reducing incidental penetrations from shallow grazes while preserving consistent outcomes for intended shots. Empirically, pre-step vs. post-step discrepancy averaged 25–45% reduction in normal component on impact; using pre-step plus a higher factor balances reliability and selectivity.

## Future Extensions

Planned or potential follow-ups (not yet implemented):
- Make `PENETRATION_FACTOR` configurable via server config / proto message.
- Introduce ricochet behavior for failed penetrations instead of silent consume.
- Armor model: per-tank or per-surface modifiers applied to `REQUIRED`.
- Angle-based bonus: reward closer alignment of velocity with center vector (`center_into_pre`).

## Summary

Penetration now assesses the *approach* velocity before physics impulses, eliminating false negatives caused by post-collision damping. Threshold increased to 60% of spawn speed for better gameplay feel and predictability.
