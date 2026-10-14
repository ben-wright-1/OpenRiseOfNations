// Vertex layouts shared by the engine's mesh builders and the GPU backends.
#pragma once

#include <cstdint>

namespace ron {

// Position, normal, UV and an RGBA8 diffuse colour (terrain: baked lighting and alpha). 36 bytes, tightly packed.
struct ColorVertex {
	float px, py, pz;
	float nx, ny, nz;
	float u, v;
	uint8_t r, g, b, a;
};
static_assert(sizeof(ColorVertex) == 36, "ColorVertex layout is shared with GPU shaders");

// ColorVertex plus a second UV set (terrain transitions: UV0 the ground texture, UV1 the mask). 44 bytes.
struct ColorVertex2 {
	ColorVertex base;
	float u1, v1;
};
static_assert(sizeof(ColorVertex2) == 44, "ColorVertex2 layout is shared with GPU shaders");

}  // namespace ron
