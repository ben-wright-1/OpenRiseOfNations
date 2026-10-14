#include "render/forest.hpp"

#include <cctype>
#include <algorithm>
#include <cstdint>

namespace ron {

namespace {

// Random::get (riseofnations.exe): a 32-bit LCG, scaled by the low 16 bits.
struct GameRandom {
	uint32_t seed;
	int get(int lo, int hi) {
		seed = seed * 0x19660Du + 0x3C6EF35Fu;
		return lo + int((int64_t(seed & 0xFFFF) * int64_t(hi - lo)) >> 16);
	}
};

std::string resolve_in(const GameFiles& files, const std::string& basedir, const std::string& name) {
	const std::string n = normalize_game_path(name);
	for (const std::string& dir : {basedir, std::string("terrain art/"), std::string("art/"), std::string()}) {
		if (!files.resolve(dir + n).empty()) return dir + n;
	}
	return std::string();
}

const std::vector<std::string>* tileset_section(const GraphicsTxt& gfx, const std::string& tileset, const std::string& suffix) {
	if (const auto* s = gfx.section(tileset + suffix)) return s;
	return gfx.section("DEFAULT" + suffix);
}

}  // namespace

TreeSet load_tree_set(const GameFiles& files, const GraphicsTxt& gfx, const Tileset& ts) {
	TreeSet set;
	set.scale = ts.treescale * 3.0f;
	if (const auto* models = tileset_section(gfx, ts.name, "_TREE_MODELS0")) {
		for (const std::string& line : *models) {
			const std::string path = resolve_in(files, ts.basedir, split_fields(line)[0]);
			if (!path.empty()) set.models.push_back(path);
		}
	}
	if (const auto* textures = tileset_section(gfx, ts.name, "_TREE_TEXTURES0")) {
		for (size_t i = 0; i < textures->size() && i < kTreeSlots; i++) {
			const auto fields = split_fields((*textures)[i]);
			if (fields.size() >= 2) set.textures[i] = resolve_in(files, ts.basedir, fields[1]);
		}
	}
	return set;
}

int tree_slot_for_node(const std::string& node_name) {
	const std::string n = to_lower(node_name);
	if (n.empty()) return -1;
	const int variant = std::isalpha(static_cast<unsigned char>(n.back())) ? n.back() - 'a' : 0;
	auto clamp = [&](int first, int count) { return first + std::min(std::max(variant, 0), count - 1); };
	if (n.rfind("trunk", 0) == 0) return kTrunk;
	if (n.rfind("bottom", 0) == 0) return clamp(kBottom0, 2);
	if (n.rfind("middle", 0) == 0) return clamp(kMiddle0, 3);
	if (n.rfind("top", 0) == 0) return kTop0;
	return -1;
}

std::vector<TreeInstance> place_trees(const Terrain& t, size_t model_count) {
	std::vector<TreeInstance> out;
	if (model_count == 0) return out;
	auto forest = [&](int wx, int wy) { return t.in_bounds(wx, wy) && (t.cell(wx, wy).flags & wfile::kForest); };
	for (int wy = 0; wy < t.ys; wy++) {
		for (int wx = 0; wx < t.xs; wx++) {
			if (!forest(wx, wy)) continue;
			const float cx = float(wx) * kWCoordSize + kWCoordSize / 2, cy = float(wy) * kWCoordSize + kWCoordSize / 2;
			GameRandom rng{uint32_t(int32_t(cx * 7.0f + cy))};
			// Reach toward each neighbour: further into adjoining forest.
			const int W = forest(wx - 1, wy) ? 144 : 96, E = forest(wx + 1, wy) ? 144 : 96;
			const int N = forest(wx, wy - 1) ? 144 : 96, S = forest(wx, wy + 1) ? 144 : 96;
			for (int k = 0; k < 5; k++) {
				int x = int(cx), y = int(cy);
				switch (k) {
					case 0:
						x += -W + rng.get(-W, 0);
						y += -N + rng.get(-N, 0);
						[[fallthrough]];  // the original has no break here: tree 0 also takes tree 1's step
					case 1:
						x += -W + rng.get(-W, 0);
						y += S + rng.get(0, S);
						break;
					case 2:
						x += rng.get(-W, E);
						y += rng.get(-N, S);
						break;
					case 3:
						x += E + rng.get(0, E);
						y += -N + rng.get(-N, 0);
						break;
					default:
						x += E + rng.get(0, E);
						y += S + rng.get(0, S);
						break;
				}
				TreeInstance tree;
				tree.x = float(x);
				tree.y = float(y);
				tree.z = t.sample_height(tree.x, tree.y);
				tree.model = int(unsigned(x + y) % model_count);
				out.push_back(tree);
			}
		}
	}
	return out;
}

}  // namespace ron
