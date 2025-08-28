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

// Projectile / object creation (moved out-of-line to reduce header churn)
b2BodyId create_projectile(World &w, float x, float y, float vx, float vy, float density);
b2BodyId create_crate(World &w, float x, float y, float halfExtent);
b2BodyId create_ammo_box(World &w, float x, float y, float radius);
b2Vec2 get_body_position(b2BodyId id);
void step(World &w, float dt);
void destroy_body(b2BodyId id);

} // namespace t2d::phys
