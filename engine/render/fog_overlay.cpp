#include "render/fog_overlay.hpp"

#include <algorithm>
#include <cstring>

namespace ron {

namespace {

// fog_tcoord_lookup @af2df4 (13 floats, by bit pattern): about s / 12, except entry 1 = 1/24.
const uint32_t kLookupBits[13] = {0x00000000, 0x3d2a9931, 0x3e2a9931, 0x3e7fe5ca, 0x3eaa9931, 0x3ed53f7d, 0x3effe5ca,
                                  0x3f15460b, 0x3f2a9931, 0x3f3fec57, 0x3f553f7d, 0x3f6a92a3, 0x3f800000};

float lookup(int s) {
	float f;
	std::memcpy(&f, &kLookupBits[std::clamp(s, 0, 12)], 4);
	return f;
}

}  // namespace

void build_fog_overlay(const Terrain& terrain, FogOverlay& out) {
	out.fxs = terrain.xs * 2;
	out.fys = terrain.ys * 2;
	out.vxs = terrain.vxs();
	out.vys = terrain.vys();
	out.vertices.clear();
	out.indices.clear();
	out.vertices.reserve(size_t(out.vxs) * size_t(out.vys));
	for (int j = 0; j < out.vys; j++) {
		for (int i = 0; i < out.vxs; i++) {
			// A few units above the ground so it covers the ground underlays (farm fields, snowcaps) drawn just above it.
			const float z = std::max(0.0f, terrain.height(i, j)) + 6.0f;
			out.vertices.push_back({float(i) * 192.0f, float(j) * 192.0f, z, 0, 0, 1, 1.0f, 0.5f, 255, 255, 255, 255});
		}
	}
	out.indices.reserve(size_t(out.vxs - 1) * size_t(out.vys - 1) * 6);
	for (int j = 0; j + 1 < out.vys; j++) {
		for (int i = 0; i + 1 < out.vxs; i++) {
			const uint32_t a = uint32_t(j * out.vxs + i), b = a + 1, c = a + uint32_t(out.vxs), d = c + 1;
			out.indices.insert(out.indices.end(), {a, b, c, c, b, d});
		}
	}
}

bool update_fog_overlay(FogOverlay& fog, const uint8_t* levels) {
	// Cells outside the grid replicate the edge cells (WorldMapFog::set_fog_lookup's border).
	auto L = [&](int fx, int fy) {
		fx = std::clamp(fx, 0, fog.fxs - 1);
		fy = std::clamp(fy, 0, fog.fys - 1);
		return int(levels[size_t(fy) * size_t(fog.fxs) + size_t(fx)]);
	};
	bool changed = false;
	for (int j = 0; j < fog.vys; j++) {
		for (int i = 0; i < fog.vxs; i++) {
			// Vertex (i, j) sits at 192-unit steps: odd indices are fog-cell centres, even ones cell edges.
			int s;
			if ((i & 1) && (j & 1)) s = 4 * L(i >> 1, j >> 1);
			else if (i & 1) s = 2 * (L(i >> 1, (j >> 1) - 1) + L(i >> 1, j >> 1));
			else if (j & 1) s = 2 * (L((i >> 1) - 1, j >> 1) + L(i >> 1, j >> 1));
			else s = L((i >> 1) - 1, (j >> 1) - 1) + L(i >> 1, (j >> 1) - 1) + L((i >> 1) - 1, j >> 1) + L(i >> 1, j >> 1);
			ColorVertex& v = fog.vertices[size_t(j) * size_t(fog.vxs) + size_t(i)];
			const float u = lookup(s);
			if (v.u != u) v.u = u, changed = true;
		}
	}
	return changed;
}

}  // namespace ron
