// SPDX-License-Identifier: Apache-2.0
#include "server/game/physics.hpp"

#include <algorithm>
#include <cmath>

namespace t2d::phys {

static b2Vec2 rot_to_vec(const b2Rot &r)
{
    return {r.c, r.s};
}

BodyFrame get_body_frame(b2BodyId body)
{
    b2Transform xf = b2Body_GetTransform(body);
    b2Vec2 fwd = rot_to_vec(xf.q);
    b2Vec2 right{fwd.y, -fwd.x};
    return {fwd, right};
}

TankWithTurret create_tank_with_turret(
    World &world, float x, float y, uint32_t entity_id, float hull_density, float turret_density)
{
    TankWithTurret t{};
    t.entity_id = entity_id;
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.position = {x, y};
    bd.linearDamping = 0.5f;
    bd.angularDamping = 0.8f;
    t.hull = b2CreateBody(world.id, &bd);
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = hull_density;
    sd.filter.categoryBits = CAT_BODY;
    // Tanks should collide with other tanks, projectiles, and crates
    sd.filter.maskBits = CAT_BODY | CAT_PROJECTILE | CAT_CRATE;
    sd.enableContactEvents = true;
    b2Polygon hull_box = b2MakeBox(2.79f, 2.12f);
    b2CreatePolygonShape(t.hull, &sd, &hull_box);
    b2Vec2 rear_pts[4] = {{-3.2f, -2.4f}, {-3.2f, -1.0f}, {+3.2f, -1.0f}, {+3.2f, -2.4f}};
    b2Hull rear_hull = b2ComputeHull(rear_pts, 4);
    b2Polygon rear_poly = b2MakePolygon(&rear_hull, 0.0f);
    b2CreatePolygonShape(t.hull, &sd, &rear_poly);
    b2Vec2 front_pts[4] = {{-3.2f, +2.4f}, {+3.2f, +2.4f}, {+3.2f, +1.0f}, {-3.2f, +1.0f}};
    b2Hull front_hull = b2ComputeHull(front_pts, 4);
    b2Polygon front_poly = b2MakePolygon(&front_hull, 0.0f);
    b2CreatePolygonShape(t.hull, &sd, &front_poly);
    b2BodyDef td = b2DefaultBodyDef();
    td.type = b2_dynamicBody;
    td.position = {x, y};
    td.linearDamping = 0.5f;
    td.angularDamping = 0.8f;
    t.turret = b2CreateBody(world.id, &td);
    b2ShapeDef tsd = b2DefaultShapeDef();
    tsd.density = turret_density;
    tsd.filter.categoryBits = CAT_HEAD;
    // Turret same collision set
    tsd.filter.maskBits = CAT_HEAD | CAT_PROJECTILE | CAT_CRATE;
    tsd.enableContactEvents = true;
    b2Polygon turret_box = b2MakeBox(1.25f, 1.0f);
    b2CreatePolygonShape(t.turret, &tsd, &turret_box);
    b2Vec2 barrel_pts[4] = {
        {+3.2f + 0.8f, -0.15f}, {+3.2f + 0.8f, +0.15f}, {+3.2f - 2.40f, +0.15f}, {+3.2f - 2.40f, -0.15f}};
    b2Hull barrel_hull = b2ComputeHull(barrel_pts, 4);
    b2Polygon barrel_poly = b2MakePolygon(&barrel_hull, 0.0f);
    b2CreatePolygonShape(t.turret, &tsd, &barrel_poly);
    b2RevoluteJointDef rjd = b2DefaultRevoluteJointDef();
    rjd.bodyIdA = t.hull;
    rjd.bodyIdB = t.turret;
    rjd.localAnchorA = {0.f, 0.f};
    rjd.localAnchorB = {0.f, 0.f};
    rjd.enableMotor = true;
    rjd.maxMotorTorque = 50.f;
    rjd.motorSpeed = 0.f;
    t.turret_joint = b2CreateRevoluteJoint(world.id, &rjd);
    // Runtime debug (logger lives outside phys; minimal dependency): mass summary
    float hull_mass = b2Body_GetMass(t.hull);
    float turret_mass = b2Body_GetMass(t.turret);
    (void)hull_mass;
    (void)turret_mass;
    // (Logging skipped here to avoid logger include; matchmaker can log after creation if needed.)
    return t;
}

void apply_tracked_drive(const TankDriveInput &in, TankWithTurret &tank, float step_dt)
{
    (void)step_dt;
    if (!b2Body_IsValid(tank.hull))
        return;
    constexpr float g = 9.8f;
    constexpr float k_side = 0.9f;
    constexpr float k_drive = 0.7f;
    constexpr float k_neutral = 0.2f;
    constexpr float track_offset = 2.4f;
    b2Vec2 v_lin = b2Body_GetLinearVelocity(tank.hull);
    float v = std::sqrt(v_lin.x * v_lin.x + v_lin.y * v_lin.y);
    BodyFrame frame = get_body_frame(tank.hull);
    float mass = b2Body_GetMass(tank.hull);
    float base_drive_force = mass * g; // used for propulsion & braking
    float mg = mass * g; // used for drag, lateral resistance & rotational damping
    bool is_brake = in.brake;
    bool is_drive = std::fabs(in.drive_forward) > 0.0001f && !is_brake;
    bool is_turn = std::fabs(in.turn) > 0.0001f && !is_brake;
    float dy = std::clamp(in.drive_forward, -1.f, 1.f);
    float dx = std::clamp(in.turn, -1.f, 1.f);
    float e1 = 0.f, e2 = 0.f, b1 = 0.f, b2 = 0.f;
    if (!is_brake) {
        if (dy >= 0) {
            e1 = std::clamp(dy + dx, 0.f, 1.f);
            e2 = std::clamp(dy - dx, 0.f, 1.f);
            b1 = std::max(0.f, -(dy + dx));
            b2 = std::max(0.f, -(dy - dx));
        } else {
            e1 = std::clamp(dy + dx, -1.f, 0.f);
            e2 = std::clamp(dy - dx, -1.f, 0.f);
            b1 = std::max(0.f, (dy + dx));
            b2 = std::max(0.f, (dy - dx));
        }
    }
    b2Transform xf = b2Body_GetTransform(tank.hull);
    b2Vec2 p_center = xf.p;
    b2Vec2 p1{p_center.x - frame.right.x * track_offset, p_center.y - frame.right.y * track_offset};
    b2Vec2 p2{p_center.x + frame.right.x * track_offset, p_center.y + frame.right.y * track_offset};
    auto apply_force_at = [&](b2Vec2 force, b2Vec2 point)
    {
        b2Body_ApplyForce(tank.hull, force, point, true);
    };
    b2Vec2 fwd_force1{
        frame.forward.x * e1 * base_drive_force * k_drive, frame.forward.y * e1 * base_drive_force * k_drive};
    b2Vec2 fwd_force2{
        frame.forward.x * e2 * base_drive_force * k_drive, frame.forward.y * e2 * base_drive_force * k_drive};
    apply_force_at(fwd_force1, p1);
    apply_force_at(fwd_force2, p2);
    if (b1 > 0.f || b2 > 0.f) {
        b2Vec2 brake_dir{-frame.forward.x, -frame.forward.y};
        apply_force_at(
            {brake_dir.x * b1 * base_drive_force * k_drive, brake_dir.y * b1 * base_drive_force * k_drive}, p1);
        apply_force_at(
            {brake_dir.x * b2 * base_drive_force * k_drive, brake_dir.y * b2 * base_drive_force * k_drive}, p2);
    }
    if (!is_drive && v > 0.01f) {
        float proj = (v_lin.x * frame.forward.x + v_lin.y * frame.forward.y) / v;
        b2Vec2 drag{
            -frame.forward.x * proj * mg * (is_brake ? k_drive : k_neutral),
            -frame.forward.y * proj * mg * (is_brake ? k_drive : k_neutral)};
        b2Body_ApplyForceToCenter(tank.hull, drag, true);
    }
    float lateral = (v_lin.x * frame.right.x + v_lin.y * frame.right.y);
    if (std::fabs(lateral) > 0.01f) {
        float s = (v > 0.0f) ? lateral / v : 0.0f;
        b2Vec2 side{-frame.right.x * s * mg * k_side, -frame.right.y * s * mg * k_side};
        b2Body_ApplyForceToCenter(tank.hull, side, true);
    }
    float av = b2Body_GetAngularVelocity(tank.hull);
    if (!is_turn && std::fabs(av) > 0.01f) {
        float s = (av > 0.f) ? 1.f : -1.f;
        float k = is_brake ? 0.5f * (k_drive + k_neutral) : k_neutral;
        float torque = -s * mg * k * track_offset;
        b2Body_ApplyTorque(tank.hull, torque, true);
    }
}

void update_turret_aim(const TurretAimInput &aim, TankWithTurret &tank)
{
    if (!aim.target_angle_world || !b2Joint_IsValid(tank.turret_joint))
        return;
    float target = *aim.target_angle_world;
    b2Transform t_tur = b2Body_GetTransform(tank.turret);
    float turret_angle = std::atan2(t_tur.q.s, t_tur.q.c);
    float diff = target - turret_angle;
    // Normalize to [-pi, pi] using fmod to avoid potential long loops (though rare here).
    const float two_pi = 2.f * (float)M_PI;
    diff = std::fmod(diff + (float)M_PI, two_pi);
    if (diff < 0.f)
        diff += two_pi;
    diff -= (float)M_PI;
    float abs_diff = std::fabs(diff);
    float speed = 0.f;
    const float fast_threshold = 5.f * float(M_PI / 180.0);
    const float precise_threshold = 0.01f * float(M_PI / 180.0);
    if (abs_diff > fast_threshold)
        speed = (diff > 0 ? 1.f : -1.f) * 90.f * float(M_PI / 180.0);
    else if (abs_diff > precise_threshold)
        speed = (diff > 0 ? 1.f : -1.f) * 20.f * float(M_PI / 180.0) * (abs_diff / fast_threshold);
    else
        speed = 0.f;
    b2RevoluteJoint_SetMotorSpeed(tank.turret_joint, speed);
}

uint32_t fire_projectile_if_ready(
    TankWithTurret &tank, World &world, float speed, float density, float forward_offset, uint32_t next_projectile_id)
{
    if (tank.fire_cooldown_cur > 0.f || tank.ammo == 0)
        return 0;
    BodyFrame tf = get_body_frame(tank.turret);
    b2Transform xt = b2Body_GetTransform(tank.turret);
    b2Vec2 muzzle{xt.p.x + tf.forward.x * forward_offset, xt.p.y + tf.forward.y * forward_offset};
    uint32_t pid = next_projectile_id;
    create_projectile(world, muzzle.x, muzzle.y, tf.forward.x * speed, tf.forward.y * speed, density);
    tank.fire_cooldown_cur = tank.fire_cooldown_max;
    tank.ammo--;
    return pid;
}

} // namespace t2d::phys
