// Basic unit test for framing parser.
#include "common/framing.hpp"
#include <cassert>
#include <string>
#include <iostream>

int main(){
	using namespace t2d::netutil;
	std::string p1 = "hello";
	std::string p2 = std::string(100, 'x');
	auto f1 = build_frame(p1);
	auto f2 = build_frame(p2);
	FrameParseState st;
	// feed first half of combined frames
	std::string all = f1 + f2;
	size_t half = all.size()/2;
	st.buffer.insert(st.buffer.end(), all.data(), all.data()+half);
	std::string out;
	bool got = try_extract(st, out);
	// May or may not have full first frame depending on split; ensure correctness
	if(half >= f1.size()) {
		assert(got);
		assert(out == p1);
	} else {
		assert(!got);
	}
	// feed rest
	st.buffer.insert(st.buffer.end(), all.data()+half, all.data()+all.size());
	if(!got) {
		bool got_now = try_extract(st, out);
		assert(got_now && out == p1);
	}
	std::string out2;
	bool got2 = try_extract(st, out2);
	assert(got2 && out2 == p2);
	std::cout << "unit_framing OK" << std::endl;
	return 0;
}
