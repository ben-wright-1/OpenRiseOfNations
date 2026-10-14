// Visual trees on forest wcoords (ObjectsOut::sort_trees, ObjectsData::place_tree): five per forest tile at
// deterministic, position-seeded offsets, with models and textures from graphics.txt. docs/terrain-spec.md section C.
#pragma once

#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/graphics_txt.hpp"
#include "assets/scenario.hpp"
#include "assets/tileset.hpp"

namespace ron {

struct TreeInstance {
	float x = 0, y = 0, z = 0;  // world units; z on the ground
	int model = 0;              // index into TreeSet::models
};

// The texture slots of a tree model's sub-meshes, in graphics.txt order.
enum TreeSlot { kTrunk, kBottom0, kBottom1, kMiddle0, kMiddle1, kMiddle2, kTop0, kShadow, kTreeSlots };

struct TreeSet {
	std::vector<std::string> models;  // relative .bh3 paths
	std::string textures[kTreeSlots];  // relative .tga paths ("" if missing)
	float scale = 3.0f;               // treescale x 3; models are drawn at (s, -s, s)
};

// Reads <TILESET>_TREE_MODELS0 / _TREE_TEXTURES0 (falling back to DEFAULT_) for the tileset.
TreeSet load_tree_set(const GameFiles& files, const GraphicsTxt& gfx, const Tileset& ts);

// Texture slot of a tree model node ("Trunk", "Bottom_1_A", "Middle_2_c", "Top_1_A"; the final letter picks the
// variant). -1 for nodes that carry no geometry of their own.
int tree_slot_for_node(const std::string& node_name);

std::vector<TreeInstance> place_trees(const Terrain& t, size_t model_count);

}  // namespace ron
