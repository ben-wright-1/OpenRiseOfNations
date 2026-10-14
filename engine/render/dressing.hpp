// Static terrain dressing: mountain overlays and snowcaps, and land doobers (bush/rock billboards).
// docs/terrain-dressing-spec.md; reference implementation re/tools/dressing.mjs. Rivers are not built yet.
#pragma once

#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/graphics_txt.hpp"
#include "assets/scenario.hpp"
#include "assets/tileset.hpp"
#include "render/vertex.hpp"

namespace ron {

// One mountain range template's placed stamps (all stamps of a range share its alpha textures). UV0 tiles the tileset's
// mountain textures once per wcoord; UV1 maps the range's MAIN/RING alpha over the template footprint.
struct MountainRangeMesh {
	std::string main_alpha, ring_alpha;
	std::vector<ColorVertex2> vertices;
	std::vector<uint32_t> indices;
};

struct DooberBatch {
	std::string texture;
	std::vector<ColorVertex> vertices;
	std::vector<uint32_t> indices;
};

struct Dressing {
	std::vector<MountainRangeMesh> mountains;  // drawn twice: ALPHATEX + RING, then BASETEX + MAIN ("Mountain Blend")
	std::vector<ColorVertex2> snowcaps;        // "Weathering Underlay" patches with the tileset's snowcap texture
	std::vector<uint32_t> snowcap_indices;
	std::vector<DooberBatch> doobers;          // "Doober Blend" billboards
};

Dressing build_dressing(const GameFiles& files, const GraphicsTxt& gfx, const Terrain& t, const Tileset& ts);

}  // namespace ron
