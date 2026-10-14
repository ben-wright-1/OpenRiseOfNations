// .BHa keyframed node animation (AnimObj::load_hier, HierObj::set_to_time in riseofnations.exe).
// The node tree mirrors the BH3 hierarchy and is matched to it by structure (child/sibling order).
#pragma once

#include <vector>

#include "assets/chunk.hpp"
#include "assets/mathx.hpp"

namespace ron {

struct AnimKey {
	uint32_t time_ms = 0;  // cumulative
	Xform xform;           // local transform, applied after the node's rest transform
};

struct AnimNode {
	std::vector<AnimKey> keys;
	std::vector<AnimNode> children;
};

struct Animation {
	std::vector<AnimNode> roots;
	uint32_t duration_ms = 0;
};

Animation parse_bha(const Bytes& bytes);

// Local transform of one node at `ms` (loops over `duration_ms`). Returns false if the node has no keys.
bool sample(const std::vector<AnimKey>& keys, uint32_t ms, uint32_t duration_ms, Xform& out);

}  // namespace ron
