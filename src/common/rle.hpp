// SPDX-License-Identifier: Apache-2.0
// rle.hpp - extremely simple run-length encoder for repetitive byte sequences (prototype level)
#pragma once
#include <string>

namespace t2d::compress {

inline std::string rle_compress(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        size_t run = 1;
        while (i + run < in.size() && in[i + run] == in[i] && run < 255)
            ++run;
        out.push_back(static_cast<char>(run));
        out.push_back(static_cast<char>(c));
        i += run;
    }
    if (out.size() >= in.size())
        return in; // no expansion allowed: fallback to original
    return out;
}

} // namespace t2d::compress
