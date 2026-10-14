// Terrain transitions: soft, alpha-masked overlay patches between ground textures (docs/terrain-spec.md B2a).
// A 1:1 port of the reference re/tools/transitions.mjs, which mirrors TerrainOut::mark_nodes / mark_trans_flags /
// check_for_tendrils / create_transition_renderables and Transitions::find_map, including the original's quirks.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/scenario.hpp"

namespace ron {

struct TransitionMask {
	std::string file;              // relative path of terrain art/transitions/<12-bit code>.tga
	uint32_t code = 0;             // the file's code
	std::array<uint32_t, 8> codes{};  // the code in each of the 8 orientations
};

// The mask library in the game's (case-insensitive name) order, without 111011111111.
std::vector<TransitionMask> load_transition_masks(const GameFiles& files);

struct TransitionPatch {
	int x = 0, y = 0;                // wcoord
	int over_land = 0, over_sub = 0;  // the overlay texture: the next-lower texture in the chain
	int mask = -1;                   // index into the mask library; -1 if none fits (the game then draws nothing)
	int orient = 0;                  // 0..7
	int quad = 0;                    // 64^2 quadrant of the 128^2 mask
};

struct TransitionInput {
	const Terrain* terrain = nullptr;
	int num_tiles[3] = {4, 1, 1};  // texture counts of BASELAND, COASTAL, OCEAN
	int coast_base = 0;
	uint32_t seed = 0;
	const std::vector<TransitionMask>* masks = nullptr;
};

// At most one patch per wcoord.
std::vector<TransitionPatch> build_transitions(const TransitionInput& in);

// Mask UV for a patch-local (u, v) in [0,1]^2: orientation transform, then x 0.5 + quadrant offset.
void transition_mask_uv(float u, float v, int orient, int quad, float& mu, float& mv);

}  // namespace ron
