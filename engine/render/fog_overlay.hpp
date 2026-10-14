// The terrain fog-of-war overlay (docs/fog-spec.md section 9.2): a mesh draped over the terrain on the 192-unit vertex
// grid whose u coordinate indexes the tileset's FOGTEX ramp (art/fow_fade.tga: black, alpha 255 -> 0). Per vertex, u is
// fog_tcoord_lookup[s] with s the sum of the fog levels (0 unexplored .. 3 visible) of the four fog cells around a corner
// vertex, twice the two cells beside an edge vertex, or four times the cell at a cell centre (TerrainVis::
// fill_fog_texcoords @8b19f0): a bilinear sample of level / 3 with texel centres at the 384-unit fog-cell centres.
#pragma once

#include <cstdint>
#include <vector>

#include "assets/scenario.hpp"
#include "render/vertex.hpp"

namespace ron {

struct FogOverlay {
	int fxs = 0, fys = 0;  // fog cells (2 per wcoord on each axis)
	int vxs = 0, vys = 0;  // vertices (4 per wcoord + 1)
	std::vector<ColorVertex> vertices;
	std::vector<uint32_t> indices;
};

// Geometry over the whole map; heights clamped to sea level so the overlay also covers water surfaces. All u start at 1
// (visible: no fog).
void build_fog_overlay(const Terrain& terrain, FogOverlay& out);
// Refreshes every vertex's u from `levels` (fxs * fys bytes, row-major, values 0..3). Returns true if any u changed.
bool update_fog_overlay(FogOverlay& fog, const uint8_t* levels);

}  // namespace ron
