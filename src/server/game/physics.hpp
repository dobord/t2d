// physics.hpp - minimal Box2D integration for tank movement & projectile bodies (prototype)
#pragma once
#include <box2d/box2d.h>

#include <cstdint>
#include <vector>

namespace t2d::phys {

struct World
{
    b2World world;
    std::vector<b2Body *> tank_bodies;
    std::vector<b2Body *> projectile_bodies;

    explicit World(const b2Vec2 &gravity) : world(gravity) {}
};

inline b2Body *create_tank(World &w, float x, float y)
{
    b2BodyDef def;
    def.type = b2_dynamicBody;
    def.position = {x, y};
    auto body = w.world.CreateBody(&def);
    b2Polygon box = b2MakeBox(0.5f, 0.5f);
    b2ShapeDef sd;
    sd.density = 1.0f;
    sd.friction = 0.3f;
    sd.restitution = 0.f;
    body->CreateFixture(&sd, &box);
    w.tank_bodies.push_back(body);
    return body;
}

inline b2Body *create_projectile(World &w, float x, float y, float vx, float vy)
{
    b2BodyDef def;
    def.type = b2_dynamicBody;
    def.position = {x, y};
    def.bullet = true;
    auto body = w.world.CreateBody(&def);
    b2Polygon box = b2MakeBox(0.1f, 0.1f);
    b2ShapeDef sd;
    sd.density = 0.1f;
    sd.restitution = 0.f;
    sd.friction = 0.f;
    body->CreateFixture(&sd, &box);
    body->SetLinearVelocity({vx, vy});
    w.projectile_bodies.push_back(body);
    return body;
}

inline void step(World &w, float dt)
{
    w.world.Step(dt, 4, 2);
}

} // namespace t2d::phys
