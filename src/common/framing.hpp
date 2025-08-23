// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <arpa/inet.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace t2d::netutil {

inline std::string build_frame(const std::string &payload)
{
    uint32_t len = static_cast<uint32_t>(payload.size());
    uint32_t net = htonl(len);
    std::string frame;
    frame.resize(4 + payload.size());
    std::memcpy(frame.data(), &net, 4);
    std::memcpy(frame.data() + 4, payload.data(), payload.size());
    return frame;
}

struct FrameParseState
{
    std::vector<char> buffer; // accumulated bytes
    uint32_t expected_len{0};
    bool have_len{false};
};

// Try extract one frame; returns true if a complete payload extracted into out.
inline bool try_extract(FrameParseState &st, std::string &out)
{
    if (!st.have_len) {
        if (st.buffer.size() >= 4) {
            uint32_t net;
            std::memcpy(&net, st.buffer.data(), 4);
            st.expected_len = ntohl(net);
            st.have_len = true;
            if (st.expected_len == 0 || st.expected_len > 10'000'000)
                return false; // invalid
        } else
            return false;
    }
    if (st.have_len && st.buffer.size() >= 4 + st.expected_len) {
        out.assign(st.buffer.data() + 4, st.expected_len);
        // erase consumed
        st.buffer.erase(st.buffer.begin(), st.buffer.begin() + 4 + st.expected_len);
        st.have_len = false;
        st.expected_len = 0;
        return true;
    }
    return false;
}

} // namespace t2d::netutil
