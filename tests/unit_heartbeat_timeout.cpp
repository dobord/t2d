#include "server/matchmaking/session_manager.hpp"
#include <coro/net/tcp/client.hpp>
#include <coro/io_scheduler.hpp>
#include <cassert>
#include <iostream>

int main(){
	auto sched = coro::io_scheduler::make_shared();
	// Create dummy tcp client with invalid socket (libcoro requires scheduler) – for logic only
	coro::net::tcp::client dummy{sched};
	auto s = t2d::mm::instance().add_connection(std::move(dummy));
	t2d::mm::instance().authenticate(s, "sess_test");
	// Simulate stale heartbeat by manually rewinding timestamp
	{
		// Direct access to adjust (test - acceptable since we cannot set via API)
		s->last_heartbeat -= std::chrono::hours(1);
	}
	auto all = t2d::mm::instance().snapshot_all_sessions();
	bool found=false; for(auto &x: all) if(x->session_id=="sess_test") found=true; assert(found);
	// Invoke disconnect_session as monitor would
	t2d::mm::instance().disconnect_session(s);
	all = t2d::mm::instance().snapshot_all_sessions();
	for(auto &x: all) { assert(x->session_id != "sess_test"); }
	std::cout << "unit_heartbeat_timeout OK" << std::endl;
	return 0;
}
