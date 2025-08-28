// SPDX-License-Identifier: Apache-2.0
#include "server/game/match.hpp"

#include "common/logger.hpp"
#include "common/metrics.hpp"
#include "server/game/physics.hpp"
#include "server/game/snapshot_compress.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace {

using ProjectileMap = std::unordered_map<uint32_t, b2BodyId>;

static void process_contacts(
    t2d::phys::World &phys_world, ProjectileMap &projectile_bodies, t2d::game::MatchContext &ctx)
{
    const uint32_t damage_amount = ctx.projectile_damage;
    auto events = b2World_GetContactEvents(phys_world.id);
    if (events.beginCount <= 0)
        return;
    std::vector<uint32_t> to_destroy_projectiles;
    for (int i = 0; i < events.beginCount; ++i) {
        const b2ContactBeginTouchEvent &ev = events.beginEvents[i];
        b2BodyId a = b2Shape_GetBody(ev.shapeIdA);
        b2BodyId b = b2Shape_GetBody(ev.shapeIdB);
        uint32_t proj_id = 0;
        bool a_is_proj = false;
        uint32_t tank_index = UINT32_MAX;
        for (auto &kv : projectile_bodies) {
            if (kv.second.index1 == a.index1) {
                proj_id = kv.first;
                a_is_proj = true;
                break;
            }
            if (kv.second.index1 == b.index1) {
                proj_id = kv.first;
                a_is_proj = false;
                break;
            }
        }
        if (proj_id == 0)
            continue;
        for (size_t ti = 0; ti < phys_world.tank_bodies.size(); ++ti) {
            auto id = phys_world.tank_bodies[ti];
            if ((a_is_proj ? b.index1 : a.index1) == id.index1) {
                tank_index = ti;
                break;
            }
        }
        if (tank_index == UINT32_MAX || tank_index >= ctx.tanks.size())
            continue;
        auto &tank = ctx.tanks[tank_index];
        if (tank.hp == 0)
            continue;
        auto pit =
            std::find_if(ctx.projectiles.begin(), ctx.projectiles.end(), [&](auto &p) { return p.id == proj_id; });
        if (pit == ctx.projectiles.end())
            continue;
        auto &proj = *pit;
        if (tank.entity_id == proj.owner)
            continue;
        // Penetration requirement: use PRE-STEP (pre-collision) projectile velocity.
        // We captured projectile.prev_vx/vy before the physics step; contact events fire after the step.
        // Decision uses normal component of pre-step velocity INTO the impacted target >= 60% of initial speed.
        // See docs/projectile_penetration.md for full rationale and diagnostics fields.
        b2Vec2 n = ev.manifold.normal; // unit normal from shapeA to shapeB
        // Pre-step velocity (stored earlier this tick)
        float vpre_x = proj.prev_vx;
        float vpre_y = proj.prev_vy;
        float vdotn_pre = vpre_x * n.x + vpre_y * n.y;
        float speed_pre = std::sqrt(vpre_x * vpre_x + vpre_y * vpre_y);
        float into_speed_pre = (a_is_proj ? vdotn_pre : -vdotn_pre);
        // Direction toward tank center (diagnostic) using pre-step projectile position if available
        b2BodyId tank_body_dbg = a_is_proj ? b : a;
        b2Vec2 tpos = b2Body_GetPosition(tank_body_dbg);
        b2Vec2 to_tank{tpos.x - proj.prev_x, tpos.y - proj.prev_y};
        float to_len = std::sqrt(to_tank.x * to_tank.x + to_tank.y * to_tank.y);
        float center_into_pre = 0.f;
        if (to_len > 1e-6f) {
            to_tank.x /= to_len;
            to_tank.y /= to_len;
            center_into_pre = vpre_x * to_tank.x + vpre_y * to_tank.y;
        }
        float required = 0.6f * proj.initial_speed; // threshold updated from 0.4 to 0.6
        if (into_speed_pre + 1e-6f < required) {
            // Not enough pre-step normal speed: no penetration
            t2d::log::trace(
                "[proj_penetration] proj={} tank={} into_pre={} center_into_pre={} speed_pre={} required={} initial={} "
                "vdotn_pre={} n=({}, {}) a_is_proj={} result=NO",
                proj.id,
                tank.entity_id,
                into_speed_pre,
                center_into_pre,
                speed_pre,
                required,
                proj.initial_speed,
                vdotn_pre,
                n.x,
                n.y,
                a_is_proj);
            continue;
        }
        t2d::log::trace(
            "[proj_penetration] proj={} tank={} into_pre={} center_into_pre={} speed_pre={} required={} initial={} "
            "vdotn_pre={} n=({}, {}) a_is_proj={} result=YES",
            proj.id,
            tank.entity_id,
            into_speed_pre,
            center_into_pre,
            speed_pre,
            required,
            proj.initial_speed,
            vdotn_pre,
            n.x,
            n.y,
            a_is_proj);
        if (tank.hp > 0) {
            // Determine impact orientation relative to hull forward to attribute subsystem hits.
            // Hull forward vector from body transform.
            b2Transform hull_xf = b2Body_GetTransform(tank.hull);
            b2Vec2 hull_fwd{hull_xf.q.c, hull_xf.q.s};
            // Contact manifold normal n is from shapeA to shapeB. We need an OUTWARD normal from the hull.
            // If the projectile was shapeA (a_is_proj == true) then the hull is shapeB and n points inward.
            // Flip in that case so classification is consistent.
            b2Vec2 n_out = a_is_proj ? b2Vec2{-n.x, -n.y} : n;
            // Relative projectile position (post-step) to hull center to classify side; more stable than contact
            // normal on complex hull shapes.
            b2BodyId proj_body_id = a_is_proj ? a : b;
            b2Vec2 proj_pos = b2Body_GetPosition(proj_body_id);
            b2Vec2 rel{proj_pos.x - hull_xf.p.x, proj_pos.y - hull_xf.p.y};
            b2Vec2 hull_right{hull_fwd.y, -hull_fwd.x}; // must match get_body_frame
            float lateral = rel.x * hull_right.x + rel.y * hull_right.y;
            float forward_off = rel.x * hull_fwd.x + rel.y * hull_fwd.y;
            float dot_forward = n_out.x * hull_fwd.x + n_out.y * hull_fwd.y;
            // Previous heuristic expected normal opposite hull forward (dot < -0.5), but Box2D contact normal here
            // points outward from the hull (aligned with hull forward on frontal impacts). This prevented frontal hits
            // from being counted. Flip logic: treat as frontal when normal aligns with forward strongly.
            bool frontal = dot_forward > 0.5f;
            t2d::log::debug(
                "[damage] impact tank={} dot_forward={} frontal={} n=({}, {}) hull_fwd=({}, {})",
                tank.entity_id,
                dot_forward,
                frontal,
                n_out.x,
                n_out.y,
                hull_fwd.x,
                hull_fwd.y);
            // Side classification: use lateral offset magnitude beyond threshold; ignore near-center hits.
            constexpr float kSideLateralThresh = 0.5f; // half a meter lateral
            bool side = std::fabs(lateral) > kSideLateralThresh && !frontal;
            if (side) {
                bool hit_right_side = lateral > 0.f; // positive lateral => RIGHT
                t2d::log::debug(
                    "[damage] side hit tank={} lateral={} forward_off={} right_side={} n=({}, {}) hull_right=({}, {}) "
                    "rel=({}, {}) left_broken={} right_broken={}",
                    tank.entity_id,
                    lateral,
                    forward_off,
                    hit_right_side,
                    n_out.x,
                    n_out.y,
                    hull_right.x,
                    hull_right.y,
                    rel.x,
                    rel.y,
                    tank.left_track_broken,
                    tank.right_track_broken);
                if (hit_right_side) {
                    if (!tank.right_track_broken) {
                        ++tank.right_track_hits;
                        if (tank.right_track_hits >= ctx.track_break_hits) {
                            tank.right_track_broken = true;
                        }
                    }
                } else { // left side
                    if (!tank.left_track_broken) {
                        ++tank.left_track_hits;
                        if (tank.left_track_hits >= ctx.track_break_hits) {
                            tank.left_track_broken = true;
                        }
                    }
                }
            } else if (frontal) {
                if (!tank.turret_disabled) {
                    ++tank.frontal_turret_hits;
                    t2d::log::debug(
                        "[damage] frontal turret hit tank={} count={}/{}",
                        tank.entity_id,
                        tank.frontal_turret_hits,
                        ctx.turret_disable_front_hits);
                    if (tank.frontal_turret_hits >= ctx.turret_disable_front_hits
                        && b2Joint_IsValid(tank.turret_joint)) {
                        b2RevoluteJoint_EnableMotor(tank.turret_joint, false);
                        b2RevoluteJoint_SetMotorSpeed(tank.turret_joint, 0.f);
                        tank.turret_disabled = true;
                        t2d::log::info(
                            "[damage] turret disabled tank={} frontal_hits={} threshold={}",
                            tank.entity_id,
                            tank.frontal_turret_hits,
                            ctx.turret_disable_front_hits);
                    }
                }
            }
            uint16_t before = tank.hp;
            if (tank.hp <= damage_amount)
                tank.hp = 0;
            else
                tank.hp -= damage_amount;
            t2d::ServerMessage evmsg;
            auto *d = evmsg.mutable_damage();
            d->set_victim_id(tank.entity_id);
            d->set_attacker_id(proj.owner);
            d->set_amount(damage_amount);
            d->set_remaining_hp(tank.hp);
            for (auto &pl : ctx.players)
                t2d::mm::instance().push_message(pl, evmsg);
            if (before > 0 && tank.hp == 0) {
                if (!ctx.persist_destroyed_tanks) {
                    ctx.removed_tanks_since_full.push_back(tank.entity_id);
                    // Destroy physics bodies immediately so they no longer collide / cost simulation time
                    if (b2Body_IsValid(tank.hull)) {
                        t2d::phys::destroy_body(tank.hull);
                        tank.hull = b2_nullBodyId;
                    }
                    if (b2Body_IsValid(tank.turret)) {
                        t2d::phys::destroy_body(tank.turret);
                        tank.turret = b2_nullBodyId;
                    }
                    if (b2Joint_IsValid(tank.turret_joint)) {
                        b2DestroyJoint(tank.turret_joint);
                        tank.turret_joint = b2_nullJointId;
                    }
                } else {
                    // Disable turret motor so corpse stops rotating
                    if (b2Joint_IsValid(tank.turret_joint)) {
                        b2RevoluteJoint_EnableMotor(tank.turret_joint, false);
                        b2RevoluteJoint_SetMotorSpeed(tank.turret_joint, 0.f);
                    }
                }
                ctx.kill_feed_events.emplace_back(tank.entity_id, proj.owner);
                t2d::ServerMessage tdmsg;
                auto *td = tdmsg.mutable_destroyed();
                td->set_victim_id(tank.entity_id);
                td->set_attacker_id(proj.owner);
                for (auto &pl : ctx.players)
                    t2d::mm::instance().push_message(pl, tdmsg);
            }
        }
        auto body_it = projectile_bodies.find(proj_id);
        if (body_it != projectile_bodies.end()) {
            if (b2Body_IsValid(body_it->second)) {
                to_destroy_projectiles.push_back(proj_id);
            } else {
                projectile_bodies.erase(body_it);
            }
        }
        ctx.removed_projectiles_since_full.push_back(proj_id);
        ctx.projectiles.erase(pit);
    }
    for (auto pid : to_destroy_projectiles) {
        auto it = projectile_bodies.find(pid);
        if (it != projectile_bodies.end()) {
            if (b2Body_IsValid(it->second)) {
                t2d::phys::destroy_body(it->second);
            }
            projectile_bodies.erase(it);
        }
    }
}
} // anonymous namespace

namespace t2d::game {

coro::task<void> run_match(std::shared_ptr<coro::io_scheduler> scheduler, std::shared_ptr<MatchContext> ctx)
{
    co_await scheduler->schedule();
    t2d::log::info("[match] start id={} players={}", ctx->match_id, ctx->players.size());
    // Physics world (advanced tank physics with hull+turret)
    // Use existing world if already created by matchmaker; else create lazily.
    if (!ctx->physics_world) {
        ctx->physics_world = std::make_unique<t2d::phys::World>(b2Vec2{0.0f, 0.0f});
    }
    auto &phys_world = *ctx->physics_world; // alias
    ProjectileMap projectile_bodies; // projectile id -> body id
    // Initialize physics body list
    phys_world.tank_bodies.clear();
    for (auto &adv : ctx->tanks) {
        phys_world.tank_bodies.push_back(adv.hull);
        // Apply per-match fire cooldown configuration
        adv.fire_cooldown_max = ctx->fire_cooldown_sec;
    }
    // Create static boundary walls (thin rectangles) around map if not already present.
    // Map centered at origin: width extends +/- map_width/2 along X, height +/- map_height/2 along Y.
    const float half_w = ctx->map_width * 0.5f;
    const float half_h = ctx->map_height * 0.5f;
    // Thickness of boundary walls
    const float wall_thickness = 1.0f;
    // Helper to create a static box
    auto create_wall = [&](float cx, float cy, float hx, float hy)
    {
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_staticBody;
        bd.position = {cx, cy};
        b2BodyId body = b2CreateBody(phys_world.id, &bd);
        b2ShapeDef sd = b2DefaultShapeDef();
        sd.density = 0.0f;
        // Treat walls as generic static colliders belonging to tank category but also colliding with crates
        sd.filter.categoryBits = t2d::phys::CAT_BODY;
        sd.filter.maskBits = t2d::phys::CAT_PROJECTILE | t2d::phys::CAT_BODY | t2d::phys::CAT_CRATE;
        sd.enableContactEvents = false; // walls don't need events
        b2Polygon poly = b2MakeBox(hx, hy);
        b2CreatePolygonShape(body, &sd, &poly);
    };
    // Top & bottom
    create_wall(0.f, half_h + wall_thickness * 0.5f, half_w + wall_thickness, wall_thickness * 0.5f);
    create_wall(0.f, -half_h - wall_thickness * 0.5f, half_w + wall_thickness, wall_thickness * 0.5f);
    // Left & right
    create_wall(-half_w - wall_thickness * 0.5f, 0.f, wall_thickness * 0.5f, half_h + wall_thickness);
    create_wall(half_w + wall_thickness * 0.5f, 0.f, wall_thickness * 0.5f, half_h + wall_thickness);
    // Spawn grouped crates (clusters)
    {
        std::mt19937 rng(static_cast<uint32_t>(ctx->match_id.size() * 131u));
        std::uniform_real_distribution<float> ux(-half_w * 0.6f, half_w * 0.6f);
        std::uniform_real_distribution<float> uy(-half_h * 0.6f, half_h * 0.6f);
        const int clusters = 3;
        for (int c = 0; c < clusters; ++c) {
            float cx = ux(rng);
            float cy = uy(rng);
            int count = 4 + (c % 3); // 4..6 crates per cluster
            for (int k = 0; k < count; ++k) {
                float ox = ((k % 3) - 1) * 2.5f + (k * 0.13f);
                float oy = ((k / 3) - 0.5f) * 2.5f;
                auto body = t2d::phys::create_crate(phys_world, cx + ox, cy + oy, 1.2f);
                ctx->crates.push_back({ctx->next_crate_id++, body});
            }
        }
    }
    // Spawn ammo boxes randomly among crate clusters (avoid overlap by sampling near crates)
    {
        std::mt19937 rng(static_cast<uint32_t>(ctx->match_id.size() * 977u));
        std::uniform_real_distribution<float> jitter(-1.5f, 1.5f);
        int targetBoxes = 5;
        for (int i = 0; i < targetBoxes && !ctx->crates.empty(); ++i) {
            auto &cr = ctx->crates[i % ctx->crates.size()];
            b2Vec2 pos = t2d::phys::get_body_position(cr.body);
            float ax = pos.x + jitter(rng);
            float ay = pos.y + jitter(rng);
            auto body = t2d::phys::create_ammo_box(phys_world, ax, ay, 0.9f);
            ctx->ammo_boxes.push_back({ctx->next_ammo_box_id++, body, true, ax, ay});
        }
    }
    using clock = std::chrono::steady_clock;
    // Precise tick interval in nanoseconds to avoid integer millisecond truncation (e.g. 33.333ms at 30Hz).
    auto tick_interval = std::chrono::nanoseconds((1'000'000'000ull + ctx->tick_rate / 2) / ctx->tick_rate);
    auto next = clock::now();
    while (true) {
        auto now = clock::now();
        if (now < next) {
            auto wait_dur = next - now;
            // Record wait duration (off-CPU sleep) as approximation of scheduler idle time.
            t2d::metrics::add_wait_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(wait_dur).count());
            co_await scheduler->yield_for(wait_dur);
            continue;
        }
        auto tick_start = now;
        // Snapshot allocation counter at tick start (profiling builds only)
#if T2D_PROFILING_ENABLED
        uint64_t alloc_before = t2d::metrics::runtime().allocations_total.load(std::memory_order_relaxed);
        uint64_t alloc_bytes_before = t2d::metrics::runtime().allocations_bytes_total.load(std::memory_order_relaxed);
        uint64_t dealloc_before = t2d::metrics::runtime().deallocations_total.load(std::memory_order_relaxed);
        uint64_t log_before = t2d::metrics::runtime().log_lines_total.load(std::memory_order_relaxed);
#endif
        next += tick_interval;
        ctx->server_tick++;
        // Handle disconnects: identify players removed from session manager snapshot
        {
            // Build a set of active session_ids from session manager
            auto active_sessions = t2d::mm::instance().snapshot_all_sessions();
            std::unordered_set<std::string> active_ids;
            active_ids.reserve(active_sessions.size());
            for (auto &sp : active_sessions) {
                if (!sp->session_id.empty())
                    active_ids.insert(sp->session_id);
            }
            for (size_t i = 0; i < ctx->players.size(); ++i) {
                auto &sess = ctx->players[i];
                if (sess->is_bot)
                    continue; // bots persist until match end
                if (!sess->session_id.empty() && active_ids.find(sess->session_id) == active_ids.end()) {
                    // Session disconnected; mark tank dead (if not already) and queue removal if not yet recorded
                    if (i < ctx->tanks.size()) {
                        auto &tank = ctx->tanks[i];
                        if (tank.hp > 0) {
                            tank.hp = 0;
                            if (!ctx->persist_destroyed_tanks) {
                                ctx->removed_tanks_since_full.push_back(tank.entity_id);
                                if (b2Body_IsValid(tank.hull)) {
                                    t2d::phys::destroy_body(tank.hull);
                                    tank.hull = b2_nullBodyId;
                                }
                                if (b2Body_IsValid(tank.turret)) {
                                    t2d::phys::destroy_body(tank.turret);
                                    tank.turret = b2_nullBodyId;
                                }
                                if (b2Joint_IsValid(tank.turret_joint)) {
                                    b2DestroyJoint(tank.turret_joint);
                                    tank.turret_joint = b2_nullJointId;
                                }
                            } else if (b2Joint_IsValid(tank.turret_joint)) {
                                b2RevoluteJoint_EnableMotor(tank.turret_joint, false);
                                b2RevoluteJoint_SetMotorSpeed(tank.turret_joint, 0.f);
                            }
                            ctx->kill_feed_events.emplace_back(tank.entity_id, 0);
                            t2d::ServerMessage tdmsg;
                            auto *td = tdmsg.mutable_destroyed();
                            td->set_victim_id(tank.entity_id);
                            td->set_attacker_id(0); // environment / disconnect
                            for (auto &pl : ctx->players)
                                t2d::mm::instance().push_message(pl, tdmsg);
                        }
                    }
                }
            }
        }
        // Basic input-driven updates (no collision / bounds yet)
        if (ctx->reload_timers.size() != ctx->tanks.size()) {
            ctx->reload_timers.resize(ctx->tanks.size(), 0.f);
        }
        float dt = 1.0f / static_cast<float>(ctx->tick_rate);
        for (size_t i = 0; i < ctx->tanks.size() && i < ctx->players.size(); ++i) {
            auto &adv = ctx->tanks[i];
            if (adv.hp == 0)
                continue; // dead
            auto &sess = ctx->players[i];
            auto input = t2d::mm::instance().get_input_copy(sess);
            // One-shot per tick diagnostic when a human player's input is non-zero (temporary instrumentation)
            if (!sess->is_bot
                && (std::fabs(input.move_dir) > 0.01f || std::fabs(input.turn_dir) > 0.01f
                    || std::fabs(input.turret_turn) > 0.01f || input.fire || input.brake)) {
                t2d::log::trace(
                    "[drive] tick={} eid={} move={} turn={} turret={} fire={} brake={}",
                    ctx->server_tick,
                    adv.entity_id,
                    input.move_dir,
                    input.turn_dir,
                    input.turret_turn,
                    input.fire,
                    input.brake);
            }
            // Basic bot AI: if bot, synthesize movement & periodic fire
            if (sess->is_bot) {
                if (ctx->disable_bot_ai) {
                    // Force idle inputs
                    input.move_dir = 0.f;
                    input.turn_dir = 0.f;
                    input.turret_turn = 0.f;
                    input.fire = false;
                    t2d::mm::Session::InputState upd_idle = input;
                    t2d::mm::instance().set_bot_input(sess, upd_idle);
                } else {
                    // Acquire current tank transform
                    b2Transform myHull = b2Body_GetTransform(adv.hull);
                    b2Transform myTurret = b2Body_GetTransform(adv.turret);
                    float myHullRad = std::atan2(myHull.q.s, myHull.q.c);
                    float myTurretRad = std::atan2(myTurret.q.s, myTurret.q.c);
                    // Bot AI: wandering + target acquisition + LOS-aware firing.
                    // 1. Target selection (cache per tick minimal for prototype)
                    int target_index = -1;
                    float best_score = 1e30f;
                    for (size_t j = 0; j < ctx->tanks.size(); ++j) {
                        if (j == i)
                            continue;
                        const auto &ot = ctx->tanks[j];
                        // Always ignore destroyed tanks even if persist_destroyed_tanks keeps them in world snapshots.
                        if (ot.hp == 0)
                            continue;
                        b2Transform oHull = b2Body_GetTransform(ot.hull);
                        float dx = oHull.p.x - myHull.p.x;
                        float dy = oHull.p.y - myHull.p.y;
                        float d2 = dx * dx + dy * dy;
                        // Prefer real players by reducing effective distance
                        if (!ctx->players[j]->is_bot)
                            d2 *= 0.5f;
                        if (d2 < best_score) {
                            best_score = d2;
                            target_index = (int)j;
                        }
                    }
                    float desired_rad = myHullRad;
                    float last_align_err = 9999.f;
                    // 2. Movement: wander if no target; pursue/strafe if target
                    if (target_index >= 0) {
                        const auto &tt = ctx->tanks[target_index];
                        b2Transform ttHull = b2Body_GetTransform(tt.hull);
                        float dx = ttHull.p.x - myHull.p.x;
                        float dy = ttHull.p.y - myHull.p.y;
                        desired_rad = std::atan2(dy, dx);
                        float base_turn = desired_rad - myHullRad;
                        // Normalize to [-pi, pi] using fmod to avoid potential long while loops
                        {
                            float two_pi = 2.f * (float)M_PI;
                            base_turn = std::fmod(base_turn + (float)M_PI, two_pi);
                            if (base_turn < 0.f)
                                base_turn += two_pi;
                            base_turn -= (float)M_PI;
                        }
                        input.turn_dir = std::clamp(base_turn * 180.f / 120.f / (float)M_PI, -1.f, 1.f);
                        float dist2 = dx * dx + dy * dy;
                        if (dist2 > 900.f) { // far
                            input.move_dir = 1.0f;
                        } else if (dist2 < 100.f) { // too close -> back off slowly
                            input.move_dir = -0.3f;
                        } else {
                            // strafe: alternate slight forward/back using server_tick parity
                            input.move_dir = ((ctx->server_tick / 30) % 2) == 0 ? 0.4f : -0.2f;
                        }
                        // Turret aim independent for faster tracking
                        float tdiff = desired_rad - myTurretRad;
                        // Normalize to [-pi, pi] using fmod (single pass)
                        {
                            float two_pi = 2.f * (float)M_PI;
                            tdiff = std::fmod(tdiff + (float)M_PI, two_pi);
                            if (tdiff < 0.f)
                                tdiff += two_pi;
                            tdiff -= (float)M_PI;
                        }
                        last_align_err = std::fabs(tdiff) * 180.f / (float)M_PI;
                        input.turret_turn = std::clamp(tdiff * 180.f / (60.f * (float)M_PI), -1.f, 1.f);
                    } else {
                        // Wander: slow rotation + occasional forward bursts
                        input.turn_dir = 0.3f;
                        input.move_dir = (ctx->server_tick % 120) < 40 ? 0.5f : 0.0f;
                        input.turret_turn = 0.2f;
                    }
                    // 3. Firing logic: only when turret roughly aligned AND predicted lead not required (simple LOS).
                    if (!ctx->disable_bot_fire) {
                        uint32_t interval = ctx->bot_fire_interval_ticks == 0 ? 1 : ctx->bot_fire_interval_ticks;
                        bool cadence = (ctx->server_tick % interval) == 0;
                        if (cadence && target_index >= 0) {
                            input.fire = (last_align_err < 10.f); // stricter alignment for smarter shots
                        } else {
                            input.fire = false;
                        }
                    } else {
                        input.fire = false;
                    }
                    t2d::mm::Session::InputState upd = input;
                    t2d::mm::instance().set_bot_input(sess, upd);
                }
            }
            // Advanced drive forces
            t2d::phys::TankDriveInput drive{};
            drive.drive_forward = std::clamp(input.move_dir, -1.f, 1.f);
            drive.turn = std::clamp(input.turn_dir, -1.f, 1.f);
            drive.brake = input.brake; // new brake input
            // Mobility degradation: if one track broken, halve forward & turn authority; if both broken, no movement.
            if (adv.left_track_broken && adv.right_track_broken) {
                drive.drive_forward = 0.f;
                drive.turn = 0.f;
            } else if (adv.left_track_broken || adv.right_track_broken) {
                drive.drive_forward *= 0.5f;
                drive.turn *= 0.5f;
            }
            t2d::phys::apply_tracked_drive(drive, adv, dt);

            // Turret aim: always call update_turret_aim (it internally enforces disabled turret state)
            {
                b2Transform xt = b2Body_GetTransform(adv.turret);
                float current = std::atan2(xt.q.s, xt.q.c);
                float desired = current;
                if (std::fabs(input.turret_turn) > 0.0001f) {
                    desired =
                        current + input.turret_turn * t2d::game::turret_turn_speed_deg() * dt * float(M_PI / 180.0);
                }
                t2d::phys::TurretAimInput aim{};
                aim.target_angle_world = desired; // when disabled, update_turret_aim will early return & enforce motor
                                                  // off
                t2d::phys::update_turret_aim(aim, adv);
            }
            if (input.fire && adv.ammo > 0) {
                float forward_offset = 4.4f; // increased to avoid barrel overlap
                auto pid = ctx->next_projectile_id++;
                // Use advanced firing (spawns projectile and applies cooldown/ammo)
                uint32_t fired = t2d::phys::fire_projectile_if_ready(
                    adv, phys_world, ctx->projectile_speed, ctx->projectile_density, forward_offset, pid);
                if (fired) {
                    // Obtain slot from pool
                    bool hit = false, miss = false;
                    uint32_t slot_index;
                    if (!ctx->projectile_free_indices.empty()) {
                        slot_index = ctx->projectile_free_indices.back();
                        ctx->projectile_free_indices.pop_back();
                        hit = true;
                    } else {
                        slot_index = static_cast<uint32_t>(ctx->projectiles_storage.size());
                        ctx->projectiles_storage.push_back({});
                        miss = true; // growth event
                    }
                    b2Transform xt = b2Body_GetTransform(adv.turret);
                    b2Vec2 dir{xt.q.c, xt.q.s};
                    b2Vec2 pos{xt.p.x + dir.x * forward_offset, xt.p.y + dir.y * forward_offset};
                    auto &slot = ctx->projectiles_storage[slot_index];
                    slot.id = fired;
                    slot.x = pos.x;
                    slot.y = pos.y;
                    slot.vx = dir.x * ctx->projectile_speed;
                    slot.vy = dir.y * ctx->projectile_speed;
                    // Initialize pre-step snapshot fields to spawn state (important when reusing pooled slot)
                    slot.prev_x = slot.x;
                    slot.prev_y = slot.y;
                    slot.prev_vx = slot.vx;
                    slot.prev_vy = slot.vy;
                    slot.owner = adv.entity_id;
                    slot.initial_speed = ctx->projectile_speed;
                    slot.age = 0.f;
                    ctx->projectiles.push_back(slot); // copy lightweight POD
                    if (ctx->projectiles.size() > ctx->projectile_pool_hwm)
                        ctx->projectile_pool_hwm = static_cast<uint32_t>(ctx->projectiles.size());
#if T2D_PROFILING_ENABLED
                    t2d::metrics::add_projectile_pool_request(hit, miss);
#endif
                    projectile_bodies.emplace(fired, phys_world.projectile_bodies.back());
                    // Spawn trace log (diagnostic): log both intended muzzle velocity and actual body velocity
                    {
                        b2BodyId pbid = phys_world.projectile_bodies.back();
                        if (b2Body_IsValid(pbid)) {
                            b2Vec2 bv = b2Body_GetLinearVelocity(pbid);
                            float body_speed = std::sqrt(bv.x * bv.x + bv.y * bv.y);
                            t2d::log::trace(
                                "[proj_spawn] proj={} owner={} pos=({}, {}) muzzle_v=({}, {}) body_v=({}, {}) "
                                "body_speed={} initial={} forward_offset={}",
                                fired,
                                adv.entity_id,
                                pos.x,
                                pos.y,
                                slot.vx,
                                slot.vy,
                                bv.x,
                                bv.y,
                                body_speed,
                                slot.initial_speed,
                                forward_offset);
                        }
                    }
                    if (sess->is_bot)
                        t2d::mm::instance().clear_bot_fire(sess);
                }
            }
            // Reload timer update
            auto &rt = ctx->reload_timers[i];
            if (adv.ammo < ctx->max_ammo) {
                rt += dt;
                if (rt >= ctx->reload_interval_sec) {
                    adv.ammo++;
                    rt = 0.f;
                }
            } else {
                rt = 0.f; // full ammo, keep timer reset
            }
        }
        for (auto &adv : ctx->tanks) {
            if (adv.fire_cooldown_cur > 0.f)
                adv.fire_cooldown_cur = std::max(0.f, adv.fire_cooldown_cur - dt);
        }
        // Capture pre-step projectile state (position + velocity) for penetration logic before physics integration
        for (auto &p : ctx->projectiles) {
            auto itb = projectile_bodies.find(p.id);
            if (itb != projectile_bodies.end() && b2Body_IsValid(itb->second)) {
                b2BodyId bid = itb->second;
                b2Vec2 vpre = b2Body_GetLinearVelocity(bid);
                b2Vec2 ppre = b2Body_GetPosition(bid);
                p.prev_x = ppre.x;
                p.prev_y = ppre.y;
                p.prev_vx = vpre.x;
                p.prev_vy = vpre.y;
            } else {
                // Fallback: use stored kinematic values
                p.prev_x = p.x;
                p.prev_y = p.y;
                p.prev_vx = p.vx;
                p.prev_vy = p.vy;
            }
        }
        // Physics step (tanks + projectiles + crates) then process contacts (which will use pre-step projectile data)
        t2d::phys::step(phys_world, dt);
        // Post-first-step velocity trace: log velocity after first physics integration step (age==0 before increment)
        for (auto &p : ctx->projectiles) {
            if (p.age == 0.f) { // just spawned prior to this step
                auto itb = projectile_bodies.find(p.id);
                if (itb != projectile_bodies.end() && b2Body_IsValid(itb->second)) {
                    b2Vec2 vps = b2Body_GetLinearVelocity(itb->second);
                    float sp = std::sqrt(vps.x * vps.x + vps.y * vps.y);
                    t2d::log::trace(
                        "[proj_post_step0] proj={} owner={} v=({}, {}) speed={} initial={}",
                        p.id,
                        p.owner,
                        vps.x,
                        vps.y,
                        sp,
                        p.initial_speed);
                }
            }
        }
        // Handle projectile vs tank impacts (must run before bounds cull destroys bodies)
        process_contacts(phys_world, projectile_bodies, *ctx);
        // Ammo box pickup detection (scan tank vs sensor overlaps) simple O(N*M)
        for (auto &ab : ctx->ammo_boxes) {
            if (!ab.active)
                continue;
            b2Transform tb = b2Body_GetTransform(ab.body);
            for (size_t ti = 0; ti < ctx->tanks.size(); ++ti) {
                auto &adv = ctx->tanks[ti];
                if (adv.hp == 0)
                    continue;
                b2Transform th = b2Body_GetTransform(adv.hull);
                float dx = th.p.x - tb.p.x;
                float dy = th.p.y - tb.p.y;
                if (dx * dx + dy * dy < 4.0f) { // radius pickup (2 units)
                    // Grant ammo
                    if (adv.ammo < ctx->max_ammo) {
                        adv.ammo = std::min<uint16_t>(adv.ammo + 5, (uint16_t)ctx->max_ammo);
                    }
                    ab.active = false;
                    // Convert body to non-interactive
                    if (b2Body_IsValid(ab.body)) {
                        t2d::phys::destroy_body(ab.body);
                        ab.body = b2_nullBodyId;
                    }
                    break;
                }
            }
        }
        // Sync tank state (position + angles) from advanced bodies
        // Angles & positions derived on snapshot build.
        // Sync projectile positions from physics bodies (remove invalid)
        for (auto &p : ctx->projectiles) {
            auto it = projectile_bodies.find(p.id);
            if (it != projectile_bodies.end()) {
                auto pos = t2d::phys::get_body_position(it->second);
                p.x = pos.x;
                p.y = pos.y;
            } else {
                // Fallback: if body missing keep manual update
                p.x += p.vx * dt;
                p.y += p.vy * dt;
            }
            p.age += dt;
        }
        // Simple bounds cull for projectiles (world prototype area +/-100)
        {
            std::vector<size_t> to_remove_bounds;
            for (size_t i = 0; i < ctx->projectiles.size(); ++i) {
                auto &pr = ctx->projectiles[i];
                if (pr.age > ctx->projectile_max_lifetime_sec || std::fabs(pr.x) > 100.f || std::fabs(pr.y) > 100.f) {
                    to_remove_bounds.push_back(i);
                }
            }
            if (!to_remove_bounds.empty()) {
                for (auto it = to_remove_bounds.rbegin(); it != to_remove_bounds.rend(); ++it) {
                    auto pid = ctx->projectiles[*it].id;
                    auto body_it = projectile_bodies.find(pid);
                    if (body_it != projectile_bodies.end()) {
                        t2d::phys::destroy_body(body_it->second);
                        projectile_bodies.erase(body_it);
                    }
                    ctx->removed_projectiles_since_full.push_back(pid);
                    // Return storage slot to freelist (find in storage by id)
                    for (uint32_t si = 0; si < ctx->projectiles_storage.size(); ++si) {
                        if (ctx->projectiles_storage[si].id == pid) {
                            ctx->projectile_free_indices.push_back(si);
                            break;
                        }
                    }
                    ctx->projectiles.erase(ctx->projectiles.begin() + *it);
                }
            }
        }
        // (Contact processing already performed earlier this tick)
        if (ctx->snapshot_interval_ticks > 0 && ctx->server_tick % ctx->snapshot_interval_ticks == 0) {
            bool send_full = (ctx->server_tick - ctx->last_full_snapshot_tick >= ctx->full_snapshot_interval_ticks);
            if (send_full) {
                t2d::ServerMessage sm;
                auto *snap = sm.mutable_snapshot();
                snap->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                // Static map dimensions (unchanged during match) sent with each full snapshot
                snap->set_map_width(ctx->map_width);
                snap->set_map_height(ctx->map_height);
                ctx->last_full_snapshot_tick = static_cast<uint32_t>(ctx->server_tick);
                // Rebuild cache from physics state
                ctx->last_sent_tanks.clear();
                ctx->last_sent_tanks.resize(ctx->tanks.size());
                for (size_t ti = 0; ti < ctx->tanks.size(); ++ti) {
                    auto &adv = ctx->tanks[ti];
                    if (adv.hp == 0 && !ctx->persist_destroyed_tanks)
                        continue; // skip corpses unless persistence enabled
                    auto *ts = snap->add_tanks();
                    ts->set_entity_id(adv.entity_id);
                    auto pos = t2d::phys::get_body_position(adv.hull);
                    b2Transform xh = b2Body_GetTransform(adv.hull);
                    b2Transform xt = b2Body_GetTransform(adv.turret);
                    float hull_rad = std::atan2(xh.q.s, xh.q.c) * 180.f / 3.14159265f;
                    float tur_rad = std::atan2(xt.q.s, xt.q.c) * 180.f / 3.14159265f;
#if T2D_ENABLE_SNAPSHOT_QUANT
                    // Quantize positions & angles into integer buckets stored still as float (prototype keeps proto
                    // schema unchanged)
                    constexpr float POS_SCALE = 100.f; // 1cm
                    constexpr float ANG_SCALE = 10.f; // 0.1 deg
                    ts->set_x(std::round(pos.x * POS_SCALE) / POS_SCALE);
                    ts->set_y(std::round(pos.y * POS_SCALE) / POS_SCALE);
                    ts->set_hull_angle(std::round(hull_rad * ANG_SCALE) / ANG_SCALE);
                    ts->set_turret_angle(std::round(tur_rad * ANG_SCALE) / ANG_SCALE);
#else
                    ts->set_x(pos.x);
                    ts->set_y(pos.y);
                    ts->set_hull_angle(hull_rad);
                    ts->set_turret_angle(tur_rad);
#endif
                    // update cache
                    auto &cache = ctx->last_sent_tanks[ti];
                    cache.entity_id = adv.entity_id;
                    cache.x = pos.x;
                    cache.y = pos.y;
                    cache.hull_angle = hull_rad;
                    cache.turret_angle = tur_rad;
                    cache.hp = adv.hp;
                    cache.ammo = adv.ammo;
                    cache.alive = adv.hp > 0;
                    ts->set_hp(adv.hp);
                    ts->set_ammo(adv.ammo);
                    ts->set_track_left_broken(adv.left_track_broken);
                    ts->set_track_right_broken(adv.right_track_broken);
                    ts->set_turret_disabled(adv.turret_disabled);
                }
                // above loop sets hp/ammo, continue existing code path (skip duplicate at end)
                for (auto &adv_unused_for_scope : ctx->tanks) {
                    (void)adv_unused_for_scope; // no-op; maintain structure after refactor
                }
                // Ammo boxes (active only, once per snapshot)
                for (auto &ab : ctx->ammo_boxes) {
                    if (!ab.active)
                        continue;
                    auto *bx = snap->add_ammo_boxes();
                    bx->set_box_id(ab.id);
                    bx->set_x(ab.x);
                    bx->set_y(ab.y);
                    bx->set_active(true);
                }
                // Crates (position + angle)
                for (auto &cr : ctx->crates) {
                    if (!b2Body_IsValid(cr.body))
                        continue;
                    b2Transform xf = b2Body_GetTransform(cr.body);
                    auto *cs = snap->add_crates();
                    cs->set_crate_id(cr.id);
                    cs->set_x(xf.p.x);
                    cs->set_y(xf.p.y);
                    float ang_deg = std::atan2(xf.q.s, xf.q.c) * 180.f / 3.14159265f;
                    cs->set_angle(ang_deg);
                    // update crate cache
                    bool found = false;
                    for (auto &cc : ctx->last_sent_crates) {
                        if (cc.id == cr.id) {
                            cc.x = xf.p.x;
                            cc.y = xf.p.y;
                            cc.angle = ang_deg;
                            cc.alive = true;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        ctx->last_sent_crates.push_back({cr.id, xf.p.x, xf.p.y, ang_deg, true});
                    }
                }
                for (auto &p : ctx->projectiles) {
                    auto *ps = snap->add_projectiles();
                    ps->set_projectile_id(p.id);
#if T2D_ENABLE_SNAPSHOT_QUANT
                    constexpr float POS_SCALE = 100.f;
                    ps->set_x(std::round(p.x * POS_SCALE) / POS_SCALE);
                    ps->set_y(std::round(p.y * POS_SCALE) / POS_SCALE);
                    ps->set_vx(p.vx); // velocities left unquantized for now
                    ps->set_vy(p.vy);
#else
                    ps->set_x(p.x);
                    ps->set_y(p.y);
                    ps->set_vx(p.vx);
                    ps->set_vy(p.vy);
#endif
                }
                // Approx size: serialize into reusable scratch buffer (size() after serialize provides byte count)
                {
                    bool reused = !ctx->snapshot_scratch.empty();
                    if (!sm.SerializeToString(&ctx->snapshot_scratch)) {
                        // Fallback: should not happen; skip metrics if serialize fails
                    } else {
                        t2d::metrics::add_full(ctx->snapshot_scratch.size());
#if T2D_PROFILING_ENABLED
                        t2d::metrics::add_snapshot_scratch_usage(reused);
#endif
                    }
                }
#if T2D_ENABLE_SNAPSHOT_QUANT
                // Compression placeholder: RLE + optional zlib (only metrics currently recorded by rle_try/zlib_try)
                // Future: send compressed variant conditionally to clients advertising support.
#endif
                for (auto &pl : ctx->players)
                    t2d::mm::instance().push_message(pl, sm);
            } else {
                // delta snapshot
                t2d::ServerMessage sm;
                auto *delta = sm.mutable_delta_snapshot();
                delta->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                delta->set_base_tick(ctx->last_full_snapshot_tick);
                // compare tanks
                if (ctx->last_sent_tanks.size() != ctx->tanks.size())
                    ctx->last_sent_tanks.resize(ctx->tanks.size());
                for (size_t i = 0; i < ctx->tanks.size(); ++i) {
                    auto &adv = ctx->tanks[i];
                    if (adv.hp == 0 && !ctx->persist_destroyed_tanks)
                        continue;
                    if (i >= ctx->last_sent_tanks.size()) {
                        ctx->last_sent_tanks.push_back({adv.entity_id});
                    }
                    auto &prev = ctx->last_sent_tanks[i];
                    if (!prev.alive && adv.hp > 0) {
                        // resurrect case not expected in prototype; treat as changed
                    }
                    auto pos = t2d::phys::get_body_position(adv.hull);
                    b2Transform xh = b2Body_GetTransform(adv.hull);
                    b2Transform xt = b2Body_GetTransform(adv.turret);
                    float hull_deg = std::atan2(xh.q.s, xh.q.c) * 180.f / 3.14159265f;
                    float tur_deg = std::atan2(xt.q.s, xt.q.c) * 180.f / 3.14159265f;
                    bool changed = std::fabs(pos.x - prev.x) > 0.0001f || std::fabs(pos.y - prev.y) > 0.0001f
                        || std::fabs(hull_deg - prev.hull_angle) > 0.01f
                        || std::fabs(tur_deg - prev.turret_angle) > 0.01f || adv.hp != prev.hp || adv.ammo != prev.ammo;
                    if (changed) {
                        auto *ts = delta->add_tanks();
                        ts->set_entity_id(adv.entity_id);
#if T2D_ENABLE_SNAPSHOT_QUANT
                        constexpr float POS_SCALE = 100.f;
                        constexpr float ANG_SCALE = 10.f;
                        ts->set_x(std::round(pos.x * POS_SCALE) / POS_SCALE);
                        ts->set_y(std::round(pos.y * POS_SCALE) / POS_SCALE);
                        ts->set_hull_angle(std::round(hull_deg * ANG_SCALE) / ANG_SCALE);
                        ts->set_turret_angle(std::round(tur_deg * ANG_SCALE) / ANG_SCALE);
#else
                        ts->set_x(pos.x);
                        ts->set_y(pos.y);
                        ts->set_hull_angle(hull_deg);
                        ts->set_turret_angle(tur_deg);
#endif
                        ts->set_hp(adv.hp);
                        ts->set_ammo(adv.ammo);
                        ts->set_track_left_broken(adv.left_track_broken);
                        ts->set_track_right_broken(adv.right_track_broken);
                        ts->set_turret_disabled(adv.turret_disabled);
                        prev.x = pos.x;
                        prev.y = pos.y;
                        prev.hull_angle = hull_deg;
                        prev.turret_angle = tur_deg;
                        prev.hp = adv.hp;
                        prev.ammo = adv.ammo;
                        prev.alive = adv.hp > 0;
                    }
                }
                for (auto id : ctx->removed_tanks_since_full)
                    delta->add_removed_tanks(id);
                // new projectiles since base: naive include all with id > 0 created after base tick (simplify: send all
                // projectiles whose id greater than count at last full snapshot) For prototype, just send all
                // projectiles (client would de-dup by id)
                for (auto &p : ctx->projectiles) {
                    auto *ps = delta->add_projectiles();
                    ps->set_projectile_id(p.id);
#if T2D_ENABLE_SNAPSHOT_QUANT
                    constexpr float POS_SCALE = 100.f;
                    ps->set_x(std::round(p.x * POS_SCALE) / POS_SCALE);
                    ps->set_y(std::round(p.y * POS_SCALE) / POS_SCALE);
                    ps->set_vx(p.vx);
                    ps->set_vy(p.vy);
#else
                    ps->set_x(p.x);
                    ps->set_y(p.y);
                    ps->set_vx(p.vx);
                    ps->set_vy(p.vy);
#endif
                }
                for (auto id : ctx->removed_projectiles_since_full)
                    delta->add_removed_projectiles(id);
                // Crate deltas
                if (ctx->last_sent_crates.size() < ctx->crates.size())
                    ctx->last_sent_crates.resize(ctx->crates.size());
                for (auto &cr : ctx->crates) {
                    if (!b2Body_IsValid(cr.body))
                        continue; // destroyed crates will handled by removed list
                    b2Transform xf = b2Body_GetTransform(cr.body);
                    float ang_deg = std::atan2(xf.q.s, xf.q.c) * 180.f / 3.14159265f;
                    // find cache entry
                    auto it = std::find_if(
                        ctx->last_sent_crates.begin(),
                        ctx->last_sent_crates.end(),
                        [&](auto &cc) { return cc.id == cr.id; });
                    if (it == ctx->last_sent_crates.end()) {
                        // new crate (unexpected after match start, but allow)
                        auto *cs = delta->add_crates();
                        cs->set_crate_id(cr.id);
                        cs->set_x(xf.p.x);
                        cs->set_y(xf.p.y);
                        cs->set_angle(ang_deg);
                        ctx->last_sent_crates.push_back({cr.id, xf.p.x, xf.p.y, ang_deg, true});
                    } else {
                        bool changed = std::fabs(it->x - xf.p.x) > 0.01f || std::fabs(it->y - xf.p.y) > 0.01f
                            || std::fabs(it->angle - ang_deg) > 0.5f; // angle threshold 0.5 deg
                        if (changed) {
                            auto *cs = delta->add_crates();
                            cs->set_crate_id(cr.id);
                            cs->set_x(xf.p.x);
                            cs->set_y(xf.p.y);
                            cs->set_angle(ang_deg);
                            it->x = xf.p.x;
                            it->y = xf.p.y;
                            it->angle = ang_deg;
                            it->alive = true;
                        }
                    }
                }
                for (auto cid : ctx->removed_crates_since_full)
                    delta->add_removed_crates(cid);
                // Deltas for ammo boxes omitted (they are static until picked up; appear only in full snapshots)
                {
                    bool reused = !ctx->snapshot_scratch.empty();
                    if (!sm.SerializeToString(&ctx->snapshot_scratch)) {
                        // skip metrics if failure
                    } else {
                        t2d::metrics::add_delta(ctx->snapshot_scratch.size());
#if T2D_PROFILING_ENABLED
                        t2d::metrics::add_snapshot_scratch_usage(reused);
#endif
                    }
                }
#if T2D_ENABLE_SNAPSHOT_QUANT
                // As above, compression logic lives in snapshot_compress.* (not applied to wire in prototype)
#endif
                for (auto &pl : ctx->players)
                    t2d::mm::instance().push_message(pl, sm);
            }
            if (send_full) {
                // Clear removed lists after full snapshot baseline
                ctx->removed_projectiles_since_full.clear();
                ctx->removed_tanks_since_full.clear();
                ctx->removed_crates_since_full.clear();
            }
        }
        // Emit aggregated KillFeedUpdate if any events occurred this tick
        if (!ctx->kill_feed_events.empty()) {
            t2d::ServerMessage kfmsg;
            auto *kf = kfmsg.mutable_kill_feed();
            for (auto &e : ctx->kill_feed_events) {
                auto *ev = kf->add_events();
                ev->set_victim_id(e.first);
                ev->set_attacker_id(e.second);
            }
            for (auto &pl : ctx->players)
                t2d::mm::instance().push_message(pl, kfmsg);
            ctx->kill_feed_events.clear();
        }
        // Victory condition: only one (or zero) alive tank remains OR time limit reached.
        // Delay evaluation for initial warmup (avoid instant end when match starts with 1 player before bot fill, or
        // transient states).
        if (!ctx->match_over && ctx->server_tick > ctx->tick_rate * 2) { // ~2s grace period
            uint32_t alive_count = 0;
            uint32_t last_alive_id = 0;
            for (auto &t : ctx->tanks) {
                if (t.hp > 0) {
                    ++alive_count;
                    last_alive_id = t.entity_id;
                }
            }
            // Determine fallback timeout (soft end) independent of alive_count branch.
            uint64_t fallback_ticks = ctx->tick_rate * (ctx->disable_bot_fire ? 300ull : 60ull);
            bool timeout_reached = ctx->server_tick > fallback_ticks;
            if (alive_count <= 1 && ctx->initial_player_count > 1) {
                ctx->match_over = true;
                ctx->winner_entity = last_alive_id; // could be 0 if no survivors
            } else if (timeout_reached) {
                ctx->match_over = true; // draw if winner_entity not set
            }
            if (ctx->match_over && !ctx->match_end_sent) {
                t2d::ServerMessage endmsg;
                auto *me = endmsg.mutable_match_end();
                me->set_match_id(ctx->match_id);
                me->set_winner_entity_id(ctx->winner_entity);
                me->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                for (auto &pl : ctx->players)
                    t2d::mm::instance().push_message(pl, endmsg);
                ctx->match_end_sent = true;
                t2d::log::info("[match] over id={} winner_entity={}", ctx->match_id, ctx->winner_entity);
            }
        }
        // Extended max duration if single real player (avoid too-short session): 120s else 10s proto cap
        // Hard cap: ensure eventual termination. If only one real player (initial count 1), long cap.
        // Multi-player (or bots) sessions get larger cap now (60s normally, 300s if bot fire disabled).
        uint64_t hard_cap_ticks = (ctx->initial_player_count <= 1)
            ? (ctx->tick_rate * 120ull)
            : (ctx->tick_rate * (ctx->disable_bot_fire ? 300ull : 60ull));
        if ((ctx->match_over && ctx->match_end_sent) || ctx->server_tick > hard_cap_ticks) {
            if (!ctx->match_end_sent) {
                // Ensure we always emit MatchEnd exactly once before exiting (hard cap emergency path)
                t2d::ServerMessage endmsg;
                auto *me = endmsg.mutable_match_end();
                me->set_match_id(ctx->match_id);
                me->set_winner_entity_id(ctx->winner_entity);
                me->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                for (auto &pl : ctx->players)
                    t2d::mm::instance().push_message(pl, endmsg);
                ctx->match_end_sent = true;
                t2d::log::info("[match] over (hard cap) id={} winner_entity={}", ctx->match_id, ctx->winner_entity);
            }
            t2d::log::info("[match] end id={}", ctx->match_id);
            // Destroy remaining projectile bodies
            for (auto &kv : projectile_bodies) {
                t2d::phys::destroy_body(kv.second);
            }
            projectile_bodies.clear();
            // Adjust metrics on match end
            t2d::metrics::runtime().active_matches.fetch_sub(1, std::memory_order_relaxed);
            // bots_in_match is a gauge reflecting current bots across matches; subtract bots from this match
            size_t bots = 0;
            for (auto &pl : ctx->players)
                if (pl->is_bot)
                    ++bots;
            if (bots > 0)
                t2d::metrics::runtime().bots_in_match.fetch_sub(bots, std::memory_order_relaxed);
            co_return;
        }
        // Record runtime metrics
        t2d::metrics::runtime().projectiles_active.store(ctx->projectiles.size(), std::memory_order_relaxed);
        auto tick_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - tick_start).count();
        t2d::metrics::add_tick_duration(static_cast<uint64_t>(tick_ns));
#if T2D_PROFILING_ENABLED
        uint64_t alloc_after = t2d::metrics::runtime().allocations_total.load(std::memory_order_relaxed);
        uint64_t alloc_bytes_after = t2d::metrics::runtime().allocations_bytes_total.load(std::memory_order_relaxed);
        uint64_t dealloc_after = t2d::metrics::runtime().deallocations_total.load(std::memory_order_relaxed);
        uint64_t log_after = t2d::metrics::runtime().log_lines_total.load(std::memory_order_relaxed);
        if (alloc_after >= alloc_before) {
            auto delta = alloc_after - alloc_before;
            auto &rt = t2d::metrics::runtime();
            rt.allocations_per_tick_accum.fetch_add(delta, std::memory_order_relaxed);
            rt.allocations_per_tick_samples.fetch_add(1, std::memory_order_relaxed);
            if (delta > 0) {
                rt.allocations_ticks_with_alloc.fetch_add(1, std::memory_order_relaxed);
            }
            t2d::metrics::add_allocations_tick(delta);
        }
        if (alloc_bytes_after >= alloc_bytes_before) {
            auto delta_b = alloc_bytes_after - alloc_bytes_before;
            auto &rt = t2d::metrics::runtime();
            rt.allocations_bytes_per_tick_accum.fetch_add(delta_b, std::memory_order_relaxed);
            rt.allocations_bytes_per_tick_samples.fetch_add(1, std::memory_order_relaxed);
        }
        if (dealloc_after >= dealloc_before) {
            auto delta_free = dealloc_after - dealloc_before;
            auto &rt = t2d::metrics::runtime();
            rt.deallocations_per_tick_accum.fetch_add(delta_free, std::memory_order_relaxed);
            rt.deallocations_per_tick_samples.fetch_add(1, std::memory_order_relaxed);
            if (delta_free > 0) {
                rt.deallocations_ticks_with_free.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (log_after >= log_before) {
            auto delta_log = log_after - log_before;
            if (delta_log > 0) {
                auto &rt = t2d::metrics::runtime();
                rt.log_lines_per_tick_accum.fetch_add(delta_log, std::memory_order_relaxed);
                rt.log_lines_per_tick_samples.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Even if zero, still count a sample so mean reflects silent ticks? Decide to count all ticks for
                // consistency with allocation bytes above
                auto &rt = t2d::metrics::runtime();
                rt.log_lines_per_tick_samples.fetch_add(1, std::memory_order_relaxed);
            }
        }
#endif
    }
}

} // namespace t2d::game
