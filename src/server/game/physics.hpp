// SPDX-License-Identifier: Apache-2.0
// physics.hpp - minimal Box2D integration for tank movement & projectile bodies (prototype)
#pragma once
#include <box2d/box2d.h>

#include <cstdint>
#include <vector>

namespace t2d::phys {

struct World
{
    b2WorldId id{b2_nullWorldId};
    std::vector<b2BodyId> tank_bodies;
    std::vector<b2BodyId> projectile_bodies;

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
    CAT_TANK = 0x0001,
    CAT_PROJECTILE = 0x0002,
};

inline b2BodyId create_tank(World &w, float x, float y)
{
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.position = {x, y};
    b2BodyId body = b2CreateBody(w.id, &bd);
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = 1.0f;
    sd.material.friction = 0.3f;
    sd.enableContactEvents = true;
    sd.filter.categoryBits = CAT_TANK;
    sd.filter.maskBits = CAT_PROJECTILE | CAT_TANK; // tank vs projectile (and optionally tank vs tank for later)
    b2Polygon box = b2MakeBox(0.5f, 0.5f);
    b2CreatePolygonShape(body, &sd, &box);
    w.tank_bodies.push_back(body);
    return body;
}

inline b2BodyId create_projectile(World &w, float x, float y, float vx, float vy)
{
    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    bd.position = {x, y};
    bd.isBullet = true;
    b2BodyId body = b2CreateBody(w.id, &bd);
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = 0.1f;
    sd.enableContactEvents = true;
    sd.filter.categoryBits = CAT_PROJECTILE;
    sd.filter.maskBits = CAT_TANK; // only collide with tanks
    b2Polygon box = b2MakeBox(0.1f, 0.1f);
    b2CreatePolygonShape(body, &sd, &box);
    b2Vec2 vel{vx, vy};
    b2Body_SetLinearVelocity(body, vel);
    w.projectile_bodies.push_back(body);
    return body;
}

inline void set_body_velocity(b2BodyId id, float vx, float vy)
{
    b2Vec2 vel{vx, vy};
    b2Body_SetLinearVelocity(id, vel);
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
