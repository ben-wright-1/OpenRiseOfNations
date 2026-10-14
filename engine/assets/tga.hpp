// Truevision TGA decoding for the formats the game ships (type 2 raw / type 10 RLE, 24 or 32 bpp).
#pragma once

#include <cstdint>
#include <vector>

#include "assets/chunk.hpp"

namespace ron {

struct Image {
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;  // top-left origin, 4 bytes per pixel
};

Image parse_tga(const Bytes& bytes);

}  // namespace ron
