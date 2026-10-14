#include "render/terrain_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace ron {

namespace {

constexpr int kSub = 4;  // tcoords per wcoord edge

const TilesetTexture* texture_for(int land, int sub, const Tileset& ts) {
	auto pick = [](const std::vector<TilesetTexture>& list, size_t i) -> const TilesetTexture* {
		return list.empty() ? nullptr : &list[std::min(i, list.size() - 1)];
	};
	switch (land) {
		case 0: return pick(ts.baseland, size_t(sub));
		case 2: return pick(ts.ocean, 0);
		default: return pick(ts.coastal, 0);  // 1 shelf water, 3 coast
	}
}

// UV0 of patch vertex (i, j) of wcoord (wx, wy) into `tex`: supertiles use subtile (wx%3, wy%3) of 170 px.
void ground_uv(const TilesetTexture& tex, int wx, int wy, int i, int j, float& u, float& v) {
	if (tex.supertile) {
		u = (float(wx % 3) + float(i) / kSub) * 170.0f / 512.0f;
		v = (float(wy % 3) + float(j) / kSub) * 170.0f / 512.0f;
	} else {
		u = float(wx * kSub + i) / kSub;
		v = float(wy * kSub + j) / kSub;
	}
}

// Per-vertex normal over the whole height grid: the sum of the unnormalised normals (-dz/dx, -dz/dy, 192)
// of the adjacent cells (TerrainOut::calculate_norms), approximated with central differences.
void grid_normal(const Terrain& t, int x, int y, float n[3]) {
	const int vx = t.vxs(), vy = t.vys();
	const float hl = t.height(std::max(x - 1, 0), y), hr = t.height(std::min(x + 1, vx - 1), y);
	const float hd = t.height(x, std::max(y - 1, 0)), hu = t.height(x, std::min(y + 1, vy - 1));
	const float sx = float(std::min(x + 1, vx - 1) - std::max(x - 1, 0));
	const float sy = float(std::min(y + 1, vy - 1) - std::max(y - 1, 0));
	n[0] = -(hr - hl) / std::max(sx, 1.0f);
	n[1] = -(hu - hd) / std::max(sy, 1.0f);
	n[2] = kTCoordSize;
	const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	n[0] /= len, n[1] /= len, n[2] /= len;
}

uint8_t to_byte(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// graphics.txt #NEW_TERRAIN_CONST / #NEW_TERRAIN_WATER values the water passes use.
constexpr float kCoastDepth = -101.0f, kLandHeight = 10.0f;
constexpr float kMinClipLevel = -25.0f;  // "OceanWaterMaxHeight" default
constexpr int kWaterSolid = 245;
constexpr uint8_t kFloorGrey = 96;   // 0.6 x lerp(OceanFloorTop 255, OceanFloorBot 64, t) at mid-screen
constexpr uint8_t kUnderwaterGrey = 195;  // min(255, floor tint + trans_bright 99)

// Land vertex alpha (TerrainOut::light_land_vert): 255 down to z = -52, 0 at z <= -89.
uint8_t land_alpha(float z) {
	if (z >= 0) return 255;
	const float D = std::fabs(kCoastDepth + kLandHeight + 2 + 52);  // 37
	const float d = std::max(0.0f, -52.0f - z);
	return uint8_t(int((1.0f - std::min(d, D) / D) * 255.0f));
}

// The baked land colour of a vertex: lighting, then underwater fade and floor tint (modify_ocean_floor).
void land_color(const Tileset& ts, const float n[3], float z, bool ocean_floor, uint8_t rgba[4]) {
	const float ndl = std::max(0.0f, n[0] * -kTerrainLightDir[0] + n[1] * -kTerrainLightDir[1] + n[2] * -kTerrainLightDir[2]);
	for (int c = 0; c < 3; c++) rgba[c] = to_byte(ts.ambient[c] + ts.diffuse[c] * ndl);
	rgba[3] = land_alpha(z);
	if (ocean_floor && rgba[3] < 221) rgba[0] = rgba[1] = rgba[2] = kUnderwaterGrey;
}

// get_water_fade: raw water alpha from the ground height (river sub-tiles get a quarter).
int water_fade(float h, bool river) {
	int a = 0;
	if (h <= -55.0f) a = kWaterSolid;
	else if (h <= kMinClipLevel) a = int(float(kWaterSolid) * (h - kMinClipLevel) / -55.0f);
	if (a != 0 && river) a >>= 2;
	return a;
}

}  // namespace

void terrain_normal(const Terrain& t, int tx, int ty, float n[3]) { grid_normal(t, tx, ty, n); }

TerrainMesh build_terrain_mesh(const Terrain& t, const Tileset& ts, const std::vector<TransitionMask>* masks, uint32_t seed) {
	TerrainMesh m;
	m.width = float(t.xs) * kWCoordSize;
	m.height = float(t.ys) * kWCoordSize;

	// Wcoords under the ocean floor sheet: shelf, deep ocean and half water (build_ocean_floor).
	auto floor_cell = [&](int wx, int wy) {
		if (!t.in_bounds(wx, wy)) return true;
		const WCoordCell& c = t.cell(wx, wy);
		return c.land == 1 || c.land == 2 || (c.flags & wfile::kHalfWater);
	};

	// Baked lighting (TerrainOut::light_vert): rgb = min(1, ambient + diffuse * max(0, N . -dir)).
	std::vector<float> normals(size_t(t.vxs()) * size_t(t.vys()) * 3);
	for (int y = 0; y < t.vys(); y++) {
		for (int x = 0; x < t.vxs(); x++) grid_normal(t, x, y, &normals[(size_t(y) * size_t(t.vxs()) + size_t(x)) * 3]);
	}

	std::map<std::string, size_t> bucket_of;
	for (int wy = 0; wy < t.ys; wy++) {
		for (int wx = 0; wx < t.xs; wx++) {
			const TilesetTexture* tex = texture_for(t.cell(wx, wy).land, t.cell(wx, wy).land_sub, ts);
			if (!tex || tex->file.empty()) continue;
			auto [it, added] = bucket_of.emplace(tex->file, m.buckets.size());
			if (added) m.buckets.push_back({tex->file, {}});
			TerrainBucket& bucket = m.buckets[it->second];

			const uint32_t base = uint32_t(m.land.size());
			for (int j = 0; j <= kSub; j++) {
				for (int i = 0; i <= kSub; i++) {
					const int tx = wx * kSub + i, ty = wy * kSub + j;
					const float* n = &normals[(size_t(ty) * size_t(t.vxs()) + size_t(tx)) * 3];
					ColorVertex v{};
					v.px = float(tx) * kTCoordSize;
					v.py = float(ty) * kTCoordSize;
					v.pz = t.height(tx, ty);
					v.nx = n[0], v.ny = n[1], v.nz = n[2];
					ground_uv(*tex, wx, wy, i, j, v.u, v.v);
					uint8_t rgba[4];
					land_color(ts, n, v.pz, floor_cell(wx, wy), rgba);
					v.r = rgba[0], v.g = rgba[1], v.b = rgba[2], v.a = rgba[3];
					m.land.push_back(v);
				}
			}
			for (int j = 0; j < kSub; j++) {
				for (int i = 0; i < kSub; i++) {
					const uint32_t a = base + uint32_t(j * (kSub + 1) + i), b = a + 1, c = a + kSub + 1, d = c + 1;
					// Split along the (x+1,y)-(x,y+1) diagonal, as TerrainOut::find_data_z interpolates.
					bucket.indices.insert(bucket.indices.end(), {a, b, c, c, b, d});
				}
			}
		}
	}

	// Transitions: one overlay patch per listed wcoord, over the same 5x5 vertices as the base pass.
	if (masks && !masks->empty()) {
		TransitionInput in;
		in.terrain = &t;
		in.num_tiles[0] = int(ts.baseland.size());
		in.num_tiles[1] = int(ts.coastal.size());
		in.num_tiles[2] = int(ts.ocean.size());
		in.coast_base = ts.coast_base;
		in.seed = seed;
		in.masks = masks;
		std::map<std::string, size_t> bucket_of_pair;
		for (const TransitionPatch& p : build_transitions(in)) {
			if (p.mask < 0) continue;  // the game draws nothing either
			const TilesetTexture* tex = texture_for(p.over_land, p.over_sub, ts);
			if (!tex || tex->file.empty()) continue;
			const std::string& mask = (*masks)[size_t(p.mask)].file;
			const bool ocean = p.over_land == 1;  // the coastal sand overlay: ocean wcoords
			auto [it, added] = bucket_of_pair.emplace(tex->file + "|" + mask, m.transitions.size());
			if (added) m.transitions.push_back({tex->file, mask, ocean, {}, {}});
			TransitionBucket& b = m.transitions[it->second];
			// The land patch of this wcoord was emitted in row-major wcoord order (skipping untextured wcoords),
			// so rebuild its vertices rather than index into `land`.
			const uint32_t base = uint32_t(b.vertices.size());
			for (int j = 0; j <= kSub; j++) {
				for (int i = 0; i <= kSub; i++) {
					const int tx = p.x * kSub + i, ty = p.y * kSub + j;
					const float* n = &normals[(size_t(ty) * size_t(t.vxs()) + size_t(tx)) * 3];
					ColorVertex2 v{};
					v.base.px = float(tx) * kTCoordSize;
					v.base.py = float(ty) * kTCoordSize;
					v.base.pz = t.height(tx, ty);
					v.base.nx = n[0], v.base.ny = n[1], v.base.nz = n[2];
					ground_uv(*tex, p.x, p.y, i, j, v.base.u, v.base.v);
					transition_mask_uv(float(i) / kSub, float(j) / kSub, p.orient, p.quad, v.u1, v.v1);
					uint8_t rgba[4];
					land_color(ts, n, v.base.pz, floor_cell(p.x, p.y), rgba);
					v.base.r = rgba[0], v.base.g = rgba[1], v.base.b = rgba[2], v.base.a = rgba[3];
					b.vertices.push_back(v);
				}
			}
			for (int j = 0; j < kSub; j++) {
				for (int i = 0; i < kSub; i++) {
					const uint32_t a = base + uint32_t(j * (kSub + 1) + i), bb = a + 1, c = a + kSub + 1, d = c + 1;
					b.indices.insert(b.indices.end(), {a, bb, c, c, bb, d});
				}
			}
		}
	}

	// Ocean floor (build_ocean_floor): an opaque sheet at z = -101 over every shelf, deep and half-water wcoord;
	// UV0 = (g + 10) / 8 and a mirrored UV1 = (size - g + 10) / 8.
	if (const TilesetTexture* ocean = texture_for(2, 0, ts)) {
		m.floor_texture = ocean->file;
		const int lx = t.xs + 2;  // lattice over gx in -1 .. xs
		std::vector<int> index(size_t(lx) * size_t(t.ys + 2), -1);
		auto vertex = [&](int gx, int gy) -> uint32_t {
			int& slot = index[size_t(gy + 1) * size_t(lx) + size_t(gx + 1)];
			if (slot < 0) {
				slot = int(m.floor.size());
				ColorVertex2 v{};
				v.base.px = float(gx) * kWCoordSize;
				v.base.py = float(gy) * kWCoordSize;
				v.base.pz = kCoastDepth + kLandHeight - 10.0f;
				v.base.nz = 1;
				v.base.u = float(gx + 10) / 8.0f;
				v.base.v = float(gy + 10) / 8.0f;
				v.u1 = float(t.xs - gx + 10) / 8.0f;
				v.v1 = float(t.ys - gy + 10) / 8.0f;
				v.base.r = v.base.g = v.base.b = kFloorGrey;
				v.base.a = 153;
				m.floor.push_back(v);
			}
			return uint32_t(slot);
		};
		// The original also covers a one-wcoord ring past the x = 0 and y = 0 edges; in game that strip does not show
		// (it reads as a stray water border around the map), so only in-map wcoords get the floor.
		for (int wy = 0; wy < t.ys; wy++) {
			for (int wx = 0; wx < t.xs; wx++) {
				if (!floor_cell(wx, wy)) continue;
				const uint32_t a = vertex(wx, wy), b = vertex(wx + 1, wy), c = vertex(wx, wy + 1), d = vertex(wx + 1, wy + 1);
				m.floor_indices.insert(m.floor_indices.end(), {a, b, c, c, b, d});
			}
		}
	}

	// Sea water (generate_water / modify_water): a half-wcoord lattice over the wcoords that contain water, raw alpha
	// from the heights, one in-place smoothing pass, then a 13-vertex fan patch per water wcoord. docs/water-spec.md 1.
	auto coord_flags = [&](int wx, int wy) -> uint16_t {
		const size_t i = size_t(wy) * size_t(t.xs) + size_t(wx);
		return i < t.coord_flags.size() ? t.coord_flags[i] : 0;
	};
	auto contains_water = [&](int wx, int wy) {
		if (!t.in_bounds(wx, wy)) return false;
		const WCoordCell& c = t.cell(wx, wy);
		if ((c.flags & wfile::kCoast) || c.land == 2 || (c.flags & wfile::kHalfWater)) return true;
		const uint16_t f = coord_flags(wx, wy);
		if (f & 0x9) return true;  // WATER | OCEAN
		if (f & 0x10) return true;  // RIVER
		if (f & 0x4) {  // COASTAL, with any height vertex below sea level
			for (int j = 0; j <= kSub; j++) {
				for (int i = 0; i <= kSub; i++) {
					if (t.height(wx * kSub + i, wy * kSub + j) < 0) return true;
				}
			}
		}
		return false;
	};
	const int W = 2 * t.xs + 1, H = 2 * t.ys + 1;
	std::vector<uint8_t> has(size_t(W) * size_t(H), 0);
	std::vector<int> alpha(size_t(W) * size_t(H), 0);
	for (int py = 0; py < H; py++) {
		for (int px = 0; px < W; px++) {
			const int wx = px / 2, wy = py / 2;
			bool exists;
			if (py % 2 == 0) {  // a wcoord edge row
				exists = px % 2 == 0 ? contains_water(wx - 1, wy - 1) || contains_water(wx, wy - 1) || contains_water(wx - 1, wy) || contains_water(wx, wy)
				                     : contains_water(wx, wy - 1) || contains_water(wx, wy);
			} else {  // a wcoord middle row
				exists = px % 2 == 0 ? contains_water(wx - 1, wy) || contains_water(wx, wy) : contains_water(wx, wy);
			}
			if (!exists) continue;
			const size_t k = size_t(py) * size_t(W) + size_t(px);
			has[k] = 1;
			// Sample (2 px, 4 (py / 2)): odd rows also read the wcoord's top edge (a quirk of the original).
			const int tx = std::min(2 * px, t.vxs() - 1), ty = std::min(4 * (py / 2), t.vys() - 1);
			const size_t ti = size_t(std::min(ty, t.ys * kSub - 1)) * size_t(t.xs * kSub) + size_t(std::min(tx, t.xs * kSub - 1));
			const bool river = ti < t.tcoord_mask.size() && (t.tcoord_mask[ti] & tfile::kRiver);
			alpha[k] = water_fade(t.height(tx, ty), river);
		}
	}
	// Smoothing: raster order, in place; each vertex becomes min(own, mean of own and existing neighbours), squared.
	const long n = long(W) * long(H), bound = long(W) * long(W);  // the original bounds-checks against (2xs+1)^2
	for (long k = 0; k < n; k++) {
		if (!has[size_t(k)]) continue;
		const float own = float(alpha[size_t(k)]) / 255.0f;
		float sum = own;
		int count = 1;
		for (long d : {-1L, 1L, -long(W), long(W), -long(W) - 1, -long(W) + 1, long(W) - 1, long(W) + 1}) {
			const long q = k + d;  // k +- 1 wraps across rows, as in the original
			if (q < 0 || q >= bound || q >= n || !has[size_t(q)]) continue;
			sum += float(alpha[size_t(q)]) / 255.0f;
			count++;
		}
		const float a = std::min(own, sum / float(count));
		alpha[size_t(k)] = int(a * a * 255.0f);
	}
	auto water_vertex = [&](int px, int py) {
		ColorVertex v{};
		v.px = float(px) * 384.0f;
		v.py = float(py) * 384.0f;
		v.nz = 1;
		v.u = v.px / kWCoordSize;
		v.v = v.py / kWCoordSize;
		// RGB: WaterTop -> WaterBot down the screen; approximated along the map diagonal (the screen gradient needs
		// the camera).
		const float g = std::clamp((v.px + v.py) / (m.width + m.height), 0.0f, 1.0f);
		v.r = 0;
		v.g = uint8_t(std::lround(159 + (111 - 159) * g));
		v.b = uint8_t(std::lround(255 + (218 - 255) * g));
		v.a = uint8_t(alpha[size_t(py) * size_t(W) + size_t(px)]);
		return v;
	};
	static const uint32_t kFan[48] = {0, 9, 3, 0, 1, 9, 1, 4, 9, 4, 3, 9, 1, 10, 4, 1, 2, 10, 2, 5, 10, 5, 4, 10,
	                                  3, 11, 6, 3, 4, 11, 4, 7, 11, 7, 6, 11, 4, 5, 12, 5, 8, 12, 8, 7, 12, 7, 4, 12};
	for (int wy = 0; wy < t.ys; wy++) {
		for (int wx = 0; wx < t.xs; wx++) {
			if (!contains_water(wx, wy)) continue;
			const uint32_t base = uint32_t(m.water.size());
			ColorVertex grid[9];
			for (int j = 0; j < 3; j++) {
				for (int i = 0; i < 3; i++) grid[j * 3 + i] = water_vertex(2 * wx + i, 2 * wy + j);
			}
			m.water.insert(m.water.end(), grid, grid + 9);
			static const int kQuads[4][4] = {{0, 1, 3, 4}, {1, 2, 4, 5}, {3, 4, 6, 7}, {4, 5, 7, 8}};
			for (const auto& q : kQuads) {  // the quad centres: the mean of the four corners
				ColorVertex c = grid[q[0]];
				c.px = (grid[q[0]].px + grid[q[3]].px) / 2;
				c.py = (grid[q[0]].py + grid[q[3]].py) / 2;
				c.u = (grid[q[0]].u + grid[q[3]].u) / 2;
				c.v = (grid[q[0]].v + grid[q[3]].v) / 2;
				c.r = uint8_t((grid[q[0]].r + grid[q[1]].r + grid[q[2]].r + grid[q[3]].r) / 4);
				c.g = uint8_t((grid[q[0]].g + grid[q[1]].g + grid[q[2]].g + grid[q[3]].g) / 4);
				c.b = uint8_t((grid[q[0]].b + grid[q[1]].b + grid[q[2]].b + grid[q[3]].b) / 4);
				c.a = uint8_t((grid[q[0]].a + grid[q[1]].a + grid[q[2]].a + grid[q[3]].a) / 4);
				m.water.push_back(c);
			}
			for (uint32_t i : kFan) m.water_indices.push_back(base + i);
		}
	}
	return m;
}

}  // namespace ron
