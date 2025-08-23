// SPDX-License-Identifier: Apache-2.0
// Unit test: verify applying a DeltaSnapshot to a prior full StateSnapshot reconstructs
// the expected current state (roundtrip consistency of server delta emission logic model).
#include "game.pb.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <unordered_map>

// Simple client-side apply of delta semantics matching current server implementation:
// - Base full snapshot provides complete tank/projectile sets (alive only)
// - Delta lists changed/new tanks (alive only), a list of removed ids (tanks/projectiles),
//   and currently (prototype) sends ALL active projectiles each delta.
// - Removed entities are applied before upserting changed ones.
// NOTE: This logic lives only in the test until an actual client sync module exists.

static void apply_delta(t2d::StateSnapshot &base, const t2d::DeltaSnapshot &delta)
{
    // Update tick
    base.set_server_tick(delta.server_tick());
    // Remove tanks (if any)
    if (!delta.removed_tanks().empty()) {
        std::vector<uint32_t> removed(delta.removed_tanks().begin(), delta.removed_tanks().end());
        auto *tanks = base.mutable_tanks();
        tanks->erase(
            std::remove_if(
                tanks->begin(),
                tanks->end(),
                [&](const t2d::TankState &ts)
                { return std::find(removed.begin(), removed.end(), ts.entity_id()) != removed.end(); }),
            tanks->end());
    }
    // Remove projectiles
    if (!delta.removed_projectiles().empty()) {
        std::vector<uint32_t> removed(delta.removed_projectiles().begin(), delta.removed_projectiles().end());
        auto *projs = base.mutable_projectiles();
        projs->erase(
            std::remove_if(
                projs->begin(),
                projs->end(),
                [&](const t2d::ProjectileState &ps)
                { return std::find(removed.begin(), removed.end(), ps.projectile_id()) != removed.end(); }),
            projs->end());
    }
    // Upsert changed/new tanks
    if (!delta.tanks().empty()) {
        // Index existing by entity_id
        std::unordered_map<uint32_t, t2d::TankState *> idx;
        for (auto &t : *base.mutable_tanks())
            idx[t.entity_id()] = &t;
        for (auto &incoming : delta.tanks()) {
            auto it = idx.find(incoming.entity_id());
            if (it == idx.end()) {
                auto *nt = base.add_tanks();
                *nt = incoming;
            } else {
                *(it->second) = incoming;
            }
        }
    }
    // Projectiles: server sends all active projectiles each delta (prototype); treat message list as authoritative set
    // after removals
    if (!delta.projectiles().empty()) {
        // Build map existing
        std::unordered_map<uint32_t, t2d::ProjectileState *> idx;
        for (auto &p : *base.mutable_projectiles())
            idx[p.projectile_id()] = &p;
        for (auto &incoming : delta.projectiles()) {
            auto it = idx.find(incoming.projectile_id());
            if (it == idx.end()) {
                auto *np = base.add_projectiles();
                *np = incoming;
            } else {
                *(it->second) = incoming;
            }
        }
    }
}

static bool tanks_equal(const t2d::StateSnapshot &a, const t2d::StateSnapshot &b)
{
    if (a.tanks_size() != b.tanks_size())
        return false;
    // Order not guaranteed after upserts, so compare as sets by id
    std::unordered_map<uint32_t, const t2d::TankState *> ia, ib;
    for (auto &t : a.tanks())
        ia[t.entity_id()] = &t;
    for (auto &t : b.tanks())
        ib[t.entity_id()] = &t;
    if (ia.size() != ib.size())
        return false;
    for (auto &kv : ia) {
        auto it = ib.find(kv.first);
        if (it == ib.end())
            return false;
        const auto &x = *kv.second;
        const auto &y = *it->second;
        if (x.x() != y.x() || x.y() != y.y() || x.hull_angle() != y.hull_angle() || x.turret_angle() != y.turret_angle()
            || x.hp() != y.hp() || x.ammo() != y.ammo())
            return false;
    }
    return true;
}

static bool projectiles_equal(const t2d::StateSnapshot &a, const t2d::StateSnapshot &b)
{
    if (a.projectiles_size() != b.projectiles_size())
        return false;
    std::unordered_map<uint32_t, const t2d::ProjectileState *> ia, ib;
    for (auto &p : a.projectiles())
        ia[p.projectile_id()] = &p;
    for (auto &p : b.projectiles())
        ib[p.projectile_id()] = &p;
    if (ia.size() != ib.size())
        return false;
    for (auto &kv : ia) {
        auto it = ib.find(kv.first);
        if (it == ib.end())
            return false;
        const auto &x = *kv.second;
        const auto &y = *it->second;
        if (x.x() != y.x() || x.y() != y.y() || x.vx() != y.vx() || x.vy() != y.vy())
            return false;
    }
    return true;
}

int main()
{
    // Build base full snapshot
    t2d::StateSnapshot base;
    base.set_server_tick(100);
    {
        auto *t1 = base.add_tanks();
        t1->set_entity_id(1);
        t1->set_x(0);
        t1->set_y(0);
        t1->set_hull_angle(0);
        t1->set_turret_angle(0);
        t1->set_hp(100);
        t1->set_ammo(10);
        auto *t2 = base.add_tanks();
        t2->set_entity_id(2);
        t2->set_x(5);
        t2->set_y(5);
        t2->set_hull_angle(10);
        t2->set_turret_angle(15);
        t2->set_hp(80);
        t2->set_ammo(7);
        auto *p1 = base.add_projectiles();
        p1->set_projectile_id(11);
        p1->set_x(1);
        p1->set_y(1);
        p1->set_vx(2);
        p1->set_vy(0);
    }

    // Simulate server state changes:
    // - Tank 1 moved & ammo decreased
    // - Tank 2 unchanged
    // - Projectile 11 removed
    // - New projectile 12 added
    // Expected final state snapshot (server authoritative)
    t2d::StateSnapshot expected;
    expected.set_server_tick(105);
    {
        auto *t1 = expected.add_tanks();
        t1->set_entity_id(1);
        t1->set_x(0.5f);
        t1->set_y(0.25f);
        t1->set_hull_angle(5);
        t1->set_turret_angle(2);
        t1->set_hp(100);
        t1->set_ammo(9);
        auto *t2 = expected.add_tanks();
        t2->set_entity_id(2);
        t2->set_x(5);
        t2->set_y(5);
        t2->set_hull_angle(10);
        t2->set_turret_angle(15);
        t2->set_hp(80);
        t2->set_ammo(7);
        auto *p2 = expected.add_projectiles();
        p2->set_projectile_id(12);
        p2->set_x(2);
        p2->set_y(2);
        p2->set_vx(2);
        p2->set_vy(1);
    }

    // Build delta representing these changes
    t2d::DeltaSnapshot delta;
    delta.set_server_tick(105);
    delta.set_base_tick(100);
    // Changed tank 1
    {
        auto *t1 = delta.add_tanks();
        t1->set_entity_id(1);
        t1->set_x(0.5f);
        t1->set_y(0.25f);
        t1->set_hull_angle(5);
        t1->set_turret_angle(2);
        t1->set_hp(100);
        t1->set_ammo(9);
    }
    // Removed projectile 11
    delta.add_removed_projectiles(11);
    // New active projectile list (server currently sends all active projectiles; here just id 12)
    {
        auto *p2 = delta.add_projectiles();
        p2->set_projectile_id(12);
        p2->set_x(2);
        p2->set_y(2);
        p2->set_vx(2);
        p2->set_vy(1);
    }

    // Apply delta to a copy of base
    t2d::StateSnapshot applied = base; // copy base full snapshot
    apply_delta(applied, delta);

    // Validate
    assert(applied.server_tick() == expected.server_tick());
    assert(tanks_equal(applied, expected));
    assert(projectiles_equal(applied, expected));

    std::cout << "unit_snapshot_delta OK" << std::endl;
    return 0;
}
