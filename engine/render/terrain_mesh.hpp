// Builds drawable terrain geometry from a scenario's terrain and its tileset (docs/terrain-spec.md section B):
// the land mesh (one 5x5-vertex patch per wcoord, grouped into one index list per texture) with baked lighting,
// and the sea-level water sheet. Transitions, mountains overlays, rivers and trees are not built yet.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assets/scenario.hpp"
#include "assets/tileset.hpp"
#include "render/transitions.hpp"
#include "render/vertex.hpp"

namespace ron {

struct TerrainBucket {
	std::string texture;            // game-relative texture path
	std::vector<uint32_t> indices;  // into TerrainMesh::land
};

// Transition overlays sharing one ground texture and one mask (docs/terrain-spec.md B2a T7/T8).
struct TransitionBucket {
	std::string texture;  // the overlay (next-lower) ground texture
	std::string mask;     // terrain art/transitions/<code>.tga
	bool ocean = false;   // overlays on deep ocean (sand + coral, drawn without depth test)
	std::vector<ColorVertex2> vertices;
	std::vector<uint32_t> indices;
};

struct TerrainMesh {
	std::vector<ColorVertex> land;
	std::vector<TerrainBucket> buckets;
	std::vector<TransitionBucket> transitions;
	std::vector<ColorVertex> water;  // sea water at z = 0 (docs/water-spec.md section 1)
	std::vector<uint32_t> water_indices;
	std::vector<ColorVertex2> floor;  // the opaque ocean-floor sheet at z = -101 (section 2.3); UV0/UV1 mirrored layers
	std::vector<uint32_t> floor_indices;
	std::string floor_texture;        // the tileset's OCEAN texture
	float width = 0, height = 0;  // world units: xs * 768, ys * 768
};

// Per-vertex terrain normal at height-grid vertex (tx, ty) (TerrainOut::calculate_norms).
void terrain_normal(const Terrain& t, int tx, int ty, float n[3]);

// Terrain light: one directional light travelling along (-1, 0, -1)/sqrt(2) (Game::init_light).
constexpr float kTerrainLightDir[3] = {-0.70710678f, 0.0f, -0.70710678f};

// `masks` may be null to skip transitions; `seed` is the scenario's world seed.
TerrainMesh build_terrain_mesh(const Terrain& t, const Tileset& ts, const std::vector<TransitionMask>* masks = nullptr,
                               uint32_t seed = 0);

}  // namespace ron
