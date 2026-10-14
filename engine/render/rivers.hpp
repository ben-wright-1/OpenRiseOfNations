// Rivers from a scenario's stored polylines (terrain chunk 92): the textured trough under the water and a faint water
// quad per section. docs/terrain-dressing-spec.md section 2; reference re/tools/dressing.mjs (buildRiver).
#pragma once

#include <vector>

#include "assets/scenario.hpp"
#include "render/vertex.hpp"

namespace ron {

enum class RiverTexture { Normal, Delta, HalfDelta };  // RIVERTEX_NORMAL / _DELTA / _HALFDELTA

struct RiverMesh {
	// Trough geometry grouped by texture (sections at and past the river mouth use the delta textures).
	std::vector<ColorVertex> trough[3];
	std::vector<uint32_t> trough_indices[3];
	// Water quads at z = 0: UV0 world/768 (the newwater frame), UV1 the river's bank-to-bank/flow coordinates.
	std::vector<ColorVertex2> water;
	std::vector<uint32_t> water_indices;
};

RiverMesh build_rivers(const Terrain& t);

}  // namespace ron
