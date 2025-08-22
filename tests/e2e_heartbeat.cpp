#include "server/net/listener.hpp"
#include "server/matchmaking/matchmaker.hpp"
#include "server/matchmaking/session_manager.hpp"
#include "common/framing.hpp"
#include "game.pb.h"
#include <coro/io_scheduler.hpp>
#include <coro/net/tcp/client.hpp>
#include <coro/coro.hpp>
#include <cassert>
#include <iostream>

using namespace std::chrono_literals;

static coro::task<void> flow(std::shared_ptr<coro::io_scheduler> sched, uint16_t port){
	co_await sched->schedule();
	co_await sched->yield_for(50ms);
	coro::net::tcp::client cli{sched, {.address=coro::net::ip_address::from_string("127.0.0.1"), .port=port}};
	auto st = co_await cli.connect(2s); assert(st==coro::net::connect_status::connected);
	// Auth
	t2d::ClientMessage auth; auth.mutable_auth_request()->set_oauth_token("x"); auth.mutable_auth_request()->set_client_version("t"); std::string payload; auth.SerializeToString(&payload); auto f = t2d::netutil::build_frame(payload); std::span<const char> rest(f.data(), f.size()); while(!rest.empty()){ co_await cli.poll(coro::poll_op::write); auto [ss,r]=cli.send(rest); if(ss==coro::net::send_status::ok||ss==coro::net::send_status::would_block) rest=r; else co_return; }
	// Heartbeat
	uint64_t client_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	t2d::ClientMessage hb; hb.mutable_heartbeat()->set_session_id("sess_t"); hb.mutable_heartbeat()->set_time_ms(client_ms); hb.SerializeToString(&payload); f = t2d::netutil::build_frame(payload); rest={f.data(), f.size()}; while(!rest.empty()){ co_await cli.poll(coro::poll_op::write); auto [ss,r]=cli.send(rest); if(ss==coro::net::send_status::ok||ss==coro::net::send_status::would_block) rest=r; else co_return; }
	// read responses
	t2d::netutil::FrameParseState fps; bool gotAuth=false, gotHB=false; auto deadline=std::chrono::steady_clock::now()+3s;
	while(std::chrono::steady_clock::now()<deadline && (!gotAuth || !gotHB)){
		co_await cli.poll(coro::poll_op::read, 100ms);
		std::string tmp(512,'\0'); auto [rs,span]=cli.recv(tmp); if(rs==coro::net::recv_status::would_block) continue; if(rs==coro::net::recv_status::closed) break; if(rs!=coro::net::recv_status::ok) break; fps.buffer.insert(fps.buffer.end(), span.begin(), span.end());
		std::string pl; while(t2d::netutil::try_extract(fps, pl)){ t2d::ServerMessage sm; sm.ParseFromArray(pl.data(), (int)pl.size()); if(sm.has_auth_response()) gotAuth=true; else if(sm.has_heartbeat_resp()){ gotHB=true; assert(sm.heartbeat_resp().client_time_ms()==client_ms); }}
	}
	assert(gotAuth && gotHB);
	std::cout << "e2e_heartbeat OK" << std::endl;
	co_return;
}

int main(){
	auto sched = coro::io_scheduler::make_shared();
	uint16_t port=41020; sched->spawn(t2d::net::run_listener(sched, port)); sched->spawn(t2d::mm::run_matchmaker(sched, t2d::mm::MatchConfig{16,180,30,200}));
	coro::sync_wait(flow(sched, port));
	return 0;
}
