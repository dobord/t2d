// SPDX-License-Identifier: Apache-2.0
#include "server/matchmaking/session_manager.hpp"

#include <coro/default_executor.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/net/tcp/client.hpp>

#include <cassert>
#include <iostream>

int main()
{
    auto scheduler = coro::default_executor::io_executor();
    // Create two dummy connections
    coro::net::tcp::client c1{scheduler};
    coro::net::tcp::client c2{scheduler};
    auto &mgr = t2d::mm::instance();
    auto s1 = mgr.add_connection(std::move(c1));
    auto s2 = mgr.add_connection(std::move(c2));
    mgr.authenticate(s1, "sess_a");
    mgr.authenticate(s2, "sess_b");
    mgr.enqueue(s1);
    mgr.enqueue(s2);
    auto snap = mgr.snapshot_queue();
    assert(snap.size() >= 2);
    mgr.pop_from_queue({s1});
    auto snap2 = mgr.snapshot_queue();
    bool s1_present = false, s2_present = false;
    for (auto &s : snap2) {
        if (s == s1)
            s1_present = true;
        if (s == s2)
            s2_present = true;
    }
    assert(!s1_present && s2_present);
    std::cout << "unit_session_manager OK" << std::endl;
    return 0;
}
