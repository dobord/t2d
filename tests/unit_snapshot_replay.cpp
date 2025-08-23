// SPDX-License-Identifier: Apache-2.0
// unit_snapshot_replay.cpp
// Validates reconstruction of state by applying a sequence of full + delta snapshots
// and comparing with authoritative state extracted from final full snapshot.
#include "game.pb.h"

#include <cassert>
#include <cmath>
#include <random>
#include <string>
#include <vector>

// Simple in-memory model of tanks to apply deltas.
struct TankSimple
{
    uint32_t id;
    float x{0};
    float y{0};
    float hull{0};
    float turret{0};
    uint32_t hp{100};
    uint32_t ammo{10};
    bool alive{true};
};

static void apply_full(const t2d::StateSnapshot &snap, std::vector<TankSimple> &out)
{
    out.clear();
    out.reserve(snap.tanks_size());
    for (auto &ts : snap.tanks()) {
        TankSimple t{
            ts.entity_id(), ts.x(), ts.y(), ts.hull_angle(), ts.turret_angle(), ts.hp(), ts.ammo(), ts.hp() > 0};
        out.push_back(t);
    }
}

static void apply_delta(const t2d::DeltaSnapshot &delta, std::vector<TankSimple> &base)
{
    // removals
    for (auto id : delta.removed_tanks()) {
        // mark dead; we'll physically remove later to emulate server full snapshot semantics
        for (auto &t : base)
            if (t.id == id) {
                t.alive = false;
                break;
            }
    }
    // changed/new tanks
    for (auto &ts : delta.tanks()) {
        bool found = false;
        for (auto &t : base)
            if (t.id == ts.entity_id()) {
                t.x = ts.x();
                t.y = ts.y();
                t.hull = ts.hull_angle();
                t.turret = ts.turret_angle();
                t.hp = ts.hp();
                t.ammo = ts.ammo();
                t.alive = ts.hp() > 0;
                found = true;
                break;
            }
        if (!found) {
            TankSimple t{
                ts.entity_id(), ts.x(), ts.y(), ts.hull_angle(), ts.turret_angle(), ts.hp(), ts.ammo(), ts.hp() > 0};
            base.push_back(t);
        }
    }
}

static bool equal_states(std::vector<TankSimple> a, std::vector<TankSimple> b)
{
    auto prune = [](std::vector<TankSimple> &v)
    {
        v.erase(std::remove_if(v.begin(), v.end(), [](auto &t) { return !t.alive; }), v.end());
    };
    prune(a);
    prune(b);
    auto sortf = [](const TankSimple &x, const TankSimple &y)
    {
        return x.id < y.id;
    };
    std::sort(a.begin(), a.end(), sortf);
    std::sort(b.begin(), b.end(), sortf);
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto &x = a[i], &y = b[i];
        if (x.id != y.id || std::fabs(x.x - y.x) > 1e-5f || std::fabs(x.y - y.y) > 1e-5f
            || std::fabs(x.hull - y.hull) > 1e-3f || std::fabs(x.turret - y.turret) > 1e-3f || x.hp != y.hp
            || x.ammo != y.ammo || x.alive != y.alive)
            return false;
    }
    return true;
}

int main()
{
    // Generate a sequence: full (tick 10) -> deltas (ticks 15,20)-> full (tick 30). We'll reconstruct end full.
    t2d::StateSnapshot full1;
    full1.set_server_tick(10);
    for (uint32_t i = 1; i <= 3; ++i) {
        auto *t = full1.add_tanks();
        t->set_entity_id(i);
        t->set_x(i * 1.0f);
        t->set_y(i * 2.0f);
        t->set_hull_angle(0);
        t->set_turret_angle(0);
        t->set_hp(100);
        t->set_ammo(10);
    }
    t2d::DeltaSnapshot d15;
    d15.set_server_tick(15);
    d15.set_base_tick(10);
    // Move tank 2 slightly, damage tank 3
    {
        auto *t = d15.add_tanks();
        t->set_entity_id(2);
        t->set_x(2.5f);
        t->set_y(4.1f);
        t->set_hull_angle(0);
        t->set_turret_angle(5);
        t->set_hp(100);
        t->set_ammo(10);
    }
    {
        auto *t = d15.add_tanks();
        t->set_entity_id(3);
        t->set_x(3.0f);
        t->set_y(6.0f);
        t->set_hull_angle(0);
        t->set_turret_angle(0);
        t->set_hp(50);
        t->set_ammo(8);
    }
    t2d::DeltaSnapshot d20;
    d20.set_server_tick(20);
    d20.set_base_tick(10);
    // Tank 3 destroyed, tank 1 moved
    {
        auto *t = d20.add_tanks();
        t->set_entity_id(1);
        t->set_x(1.2f);
        t->set_y(2.4f);
        t->set_hull_angle(10);
        t->set_turret_angle(2);
        t->set_hp(100);
        t->set_ammo(9);
    }
    d20.add_removed_tanks(3);
    t2d::StateSnapshot full2;
    full2.set_server_tick(30);
    // Final authoritative state after applying previous deltas
    {
        auto *t = full2.add_tanks();
        t->set_entity_id(1);
        t->set_x(1.2f);
        t->set_y(2.4f);
        t->set_hull_angle(10);
        t->set_turret_angle(2);
        t->set_hp(100);
        t->set_ammo(9);
    }
    {
        auto *t = full2.add_tanks();
        t->set_entity_id(2);
        t->set_x(2.5f);
        t->set_y(4.1f);
        t->set_hull_angle(0);
        t->set_turret_angle(5);
        t->set_hp(100);
        t->set_ammo(10);
    }
    // Tank 3 removed

    std::vector<TankSimple> recon;
    apply_full(full1, recon);
    apply_delta(d15, recon);
    apply_delta(d20, recon);

    std::vector<TankSimple> finalFull;
    apply_full(full2, finalFull);
    assert(equal_states(recon, finalFull));
    return 0;
}
