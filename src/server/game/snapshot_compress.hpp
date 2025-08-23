// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

// Attempt RLE compression and record metrics if smaller.
std::string rle_try(const std::string &in);

// Attempt zlib compression if available (T2D_HAS_ZLIB) returning compressed string if smaller; otherwise returns input.
std::string zlib_try(const std::string &in);
