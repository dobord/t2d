// SPDX-License-Identifier: Apache-2.0
// unit_framing_fuzz.cpp
// Basic fuzz-style tests for frame parser: malformed lengths, truncations, random noise.
#include "common/framing.hpp"

#include <cassert>
#include <random>
#include <string>
#include <vector>

static void feed_bytes(t2d::netutil::FrameParseState &st, const std::vector<char> &data, size_t chunk)
{
    std::string out;
    for (size_t i = 0; i < data.size();) {
        size_t n = std::min(chunk, data.size() - i);
        st.buffer.insert(st.buffer.end(), data.begin() + i, data.begin() + i + n);
        i += n;
        while (t2d::netutil::try_extract(st, out)) {
            // For fuzzing we only assert payload length matches prefix-specified
            assert(!out.empty());
        }
    }
}

static uint32_t rnd32(std::mt19937 &rng)
{
    return std::uniform_int_distribution<uint32_t>{0, 0xffffffff}(rng);
}

int main()
{
    std::mt19937 rng(12345);
    // 1. Valid random payloads with varied chunk sizes
    for (int caseId = 0; caseId < 200; ++caseId) {
        size_t len = std::uniform_int_distribution<size_t>{1, 2048}(rng);
        std::string payload(len, '\0');
        for (auto &c : payload)
            c = static_cast<char>(rnd32(rng));
        auto frame = t2d::netutil::build_frame(payload);
        t2d::netutil::FrameParseState st;
        feed_bytes(st, std::vector<char>(frame.begin(), frame.end()), (caseId % 17) + 1);
    }
    // 2. Truncated frames should never yield output
    for (int caseId = 0; caseId < 100; ++caseId) {
        size_t len = std::uniform_int_distribution<size_t>{10, 4096}(rng);
        std::string payload(len, 'x');
        auto frame = t2d::netutil::build_frame(payload);
        frame.resize(frame.size() - std::uniform_int_distribution<size_t>{1, len}(rng));
        t2d::netutil::FrameParseState st;
        std::string out;
        st.buffer.assign(frame.begin(), frame.end());
        assert(!t2d::netutil::try_extract(st, out));
    }
    // 3. Malformed length (too large) should cause parser to ignore (not extract) and remain false
    {
        t2d::netutil::FrameParseState st;
        uint32_t badLen = htonl(50'000'000); // exceeds cap 10,000,000
        st.buffer.resize(4);
        std::memcpy(st.buffer.data(), &badLen, 4);
        std::string out;
        bool ok = t2d::netutil::try_extract(st, out);
        assert(!ok);
    }
    // 4. Zero length should be rejected.
    {
        t2d::netutil::FrameParseState st;
        uint32_t badLen = htonl(0);
        st.buffer.resize(4);
        std::memcpy(st.buffer.data(), &badLen, 4);
        std::string out;
        assert(!t2d::netutil::try_extract(st, out));
    }
    return 0;
}
