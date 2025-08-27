// SPDX-License-Identifier: Apache-2.0
// physics.hpp - Tank physics (hull + turret) and projectile integration
#pragma once
#include <box2d/box2d.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace t2d::phys {

struct World
{
    b2WorldId id{b2_nullWorldId};
    std::vector<b2BodyId> tank_bodies; // hull bodies
    std::vector<b2BodyId> projectile_bodies; // projectile bodies
    std::vector<b2BodyId> crate_bodies; // movable obstacle crates
    std::vector<b2BodyId> ammo_box_bodies; // small ammo pickup boxes

    explicit World(const b2Vec2 &gravity)
    {
        b2WorldDef def = b2DefaultWorldDef();
        def.gravity = gravity;
        id = b2CreateWorld(&def);
    }
};

// Simple category bits for collision filtering (prototype)
enum Category : uint32_t
{
    CAT_BODY = 0x0001,
    CAT_HEAD = 0x0002,
    CAT_PROJECTILE = 0x0004,
    CAT_CRATE = 0x0008,
    CAT_AMMO_BOX = 0x0010
};

struct TankWithTurret
{
    b2BodyId hull{b2_nullBodyId};
    b2BodyId turret{b2_nullBodyId};
    b2JointId turret_joint{b2_nullJointId};
    uint32_t entity_id{0};
    uint16_t hp{100};
    uint16_t ammo{20};
    float fire_cooldown_max{0.25f}; // configured per match from fire_cooldown_sec
    float fire_cooldown_cur{0.0f};
};

struct TankDriveInput
{
    float drive_forward{0.f};
    float turn{0.f};
    bool brake{false};
};

struct TurretAimInput
{
    std::optional<float> target_angle_world; // radians
};

struct BodyFrame
{
    b2Vec2 forward;
    b2Vec2 right;
};

// Retrieve local forward/right vectors for body frame
BodyFrame get_body_frame(b2BodyId body);
// Create tank hull + turret bodies and revolute joint
TankWithTurret create_tank_with_turret(
    World &world, float x, float y, uint32_t entity_id, float hull_density = 1.0f, float turret_density = 0.5f);
void apply_tracked_drive(const TankDriveInput &in, TankWithTurret &tank, float step_dt);
void update_turret_aim(const TurretAimInput &aim, TankWithTurret &tank);
uint32_t fire_projectile_if_ready(
    TankWithTurret &tank, World &world, float speed, float density, float forward_offset, uint32_t next_projectile_id);

inline b2BodyId create_projectile(World &w, float x, float y, float vx, float vy, float density)
{
    // Legacy visual bullet: 60x20 px while legacy tank ~640 px height.
    // Physics tank hull height ~= 4.8 world units (from -2.4 .. +2.4). Pixel->world scale ≈ 4.8 / 640 = 0.0075.
    // Bullet world size (60 * 0.0075, 20 * 0.0075) ≈ (0.45, 0.15). Half extents: (0.225, 0.075).
    // Use that to align physical hit box with legacy proportions.
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.position = {x, y};
    bd.isBullet = true;
    b2BodyId body = b2CreateBody(w.id, &bd);
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = density; // configurable projectile density
    sd.enableContactEvents = true;
    sd.filter.categoryBits = CAT_PROJECTILE;
    sd.filter.maskBits = CAT_BODY | CAT_CRATE; // collide with tanks and crates (walls share CAT_BODY)
    b2Polygon box = b2MakeBox(0.225f, 0.075f); // width 0.45, height 0.15
    b2CreatePolygonShape(body, &sd, &box);
    b2Vec2 vel{vx, vy};
    b2Body_SetLinearVelocity(body, vel);
    w.projectile_bodies.push_back(body);
    return body;
}

inline b2BodyId create_crate(World &w, float x, float y, float halfExtent)
{
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.position = {x, y};
    bd.angularDamping = 2.0f;
    b2BodyId body = b2CreateBody(w.id, &bd);
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = 0.5f;
    // Surface material properties (Box2D v3 API)
    sd.material.friction = 0.8f;
    sd.material.restitution = 0.1f;
    sd.filter.categoryBits = CAT_CRATE;
    // Crates collide with tanks (includes walls categorized as CAT_BODY), other crates, and projectiles
    sd.filter.maskBits = CAT_BODY | CAT_PROJECTILE | CAT_CRATE;
    sd.enableContactEvents = false;
    b2Polygon box = b2MakeBox(halfExtent, halfExtent);
    b2CreatePolygonShape(body, &sd, &box);
    w.crate_bodies.push_back(body);
    return body;
}

inline b2BodyId create_ammo_box(World &w, float x, float y, float radius)
{
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_staticBody; // static pickup; could switch to kinematic if we want slight motion
    bd.position = {x, y};
    b2BodyId body = b2CreateBody(w.id, &bd);
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = 0.0f;
    sd.isSensor = true; // sensor so we manually handle pickup
    sd.filter.categoryBits = CAT_AMMO_BOX;
    sd.filter.maskBits = CAT_BODY; // only tanks trigger
    sd.enableContactEvents = true; // we will scan contacts to detect pickup
    b2Circle circle{{0.0f, 0.0f}, radius};
    b2CreateCircleShape(body, &sd, &circle);
    w.ammo_box_bodies.push_back(body);
    return body;
}

inline b2Vec2 get_body_position(b2BodyId id)
{
    return b2Body_GetPosition(id);
}

inline void step(World &w, float dt)
{
    b2World_Step(w.id, dt, 4);
}

inline void destroy_body(b2BodyId id)
{
    if (b2Body_IsValid(id)) {
        b2DestroyBody(id);
    }
}

} // namespace t2d::phys
