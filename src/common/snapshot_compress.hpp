// snapshot_compress.hpp
// Optional lightweight quantization helpers for snapshot coordinates & angles.
#pragma once

#include <cmath>
#include <cstdint>

namespace t2d::compress {

struct QuantConfig
{
    float pos_scale = 100.0f; // 1/100 meter resolution
    float angle_scale = 10.0f; // 0.1 degree resolution
};

// Quantize a float to uint16 (clamped) given scale.
inline uint16_t qpos(float v, float scale)
{
    int x = static_cast<int>(std::lround(v * scale));
    if (x < 0)
        x = 0;
    if (x > 65535)
        x = 65535;
    return static_cast<uint16_t>(x);
}

inline float deqpos(uint16_t q, float scale)
{
    return static_cast<float>(q) / scale;
}

inline uint16_t qangle(float deg, float scale)
{
    // Normalize angle into [0,360) using fmod (avoids iterative looping for large magnitudes)
    deg = std::fmod(deg, 360.0f);
    if (deg < 0.f)
        deg += 360.f;
    int x = static_cast<int>(std::lround(deg * scale));
    if (x < 0)
        x = 0;
    if (x > 65535)
        x = 65535;
    return static_cast<uint16_t>(x);
}

inline float deqangle(uint16_t q, float scale)
{
    return static_cast<float>(q) / scale;
}

} // namespace t2d::compress
