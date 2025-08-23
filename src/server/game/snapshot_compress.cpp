#include "common/metrics.hpp"
#include "common/rle.hpp"

#include <string>

// Attempt RLE compression and track compressed size metrics; return possibly compressed string or original.
std::string rle_try(const std::string &in)
{
    auto out = t2d::compress::rle_compress(in);
    if (&out != &in && out.size() < in.size()) {
        // Heuristic: decide full vs delta based on a tiny marker inside payload (snapshot vs delta_snapshot field tags)
        bool is_delta = in.find("\x52\x08") != std::string::npos; // crude placeholder, not robust
        if (is_delta)
            t2d::metrics::add_delta_compressed(out.size());
        else
            t2d::metrics::add_full_compressed(out.size());
        return out;
    }
    return in;
}
