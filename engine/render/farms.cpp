#include "render/farms.hpp"

#include <algorithm>
#include <cmath>

#include "assets/xml.hpp"

namespace ron {

namespace {

// Random::get (riseofnations.exe): a 32-bit LCG, scaled by the low 16 bits. Same generator as the game, own seed.
int random_get(uint32_t& seed, int lo, int hi) {
	seed = seed * 0x19660Du + 0x3C6EF35Fu;
	return lo + int((int64_t(seed & 0xFFFF) * int64_t(hi - lo)) >> 16);
}

// build_box @008d6140: one wheat cell, 192 units square (half extent 96), height 1 (scaled per cell). Vertex 0-3 the
// top, 4-7 the +y side, 6-9 the +x side (sharing the corner column 6/7). z 1 = top of the wheat, 0 = the ground.
struct BoxVertex {
	float x, y, z;
	int col, row;  // which terrain corner the vertex stands on: corner_z[x + col][y + row]
	bool wind;     // the top vertices sway (wheat_use_wind)
};
constexpr BoxVertex kBox[10] = {
	{-96, -96, 1, 0, 0, true}, {96, -96, 1, 1, 0, true}, {-96, 96, 1, 0, 1, true}, {96, 96, 1, 1, 1, true},
	{-96, 96, 1, 0, 1, true},  {-96, 96, 0, 0, 1, false}, {96, 96, 1, 1, 1, true}, {96, 96, 0, 1, 1, false},
	{96, -96, 1, 1, 0, true},  {96, -96, 0, 1, 0, false},
};
constexpr uint32_t kBoxFaces[18] = {0, 1, 2, 2, 1, 3, 4, 6, 5, 5, 6, 7, 6, 8, 7, 7, 8, 9};
// wheat_box_texs[0] and [1], picked by (x + y) & 1 (a checkerboard). The texture's top half holds two 64x64 top-view
// tiles, the bottom half a 128x64 side view that wraps around the box corner (mirrored in set 1).
constexpr float kBoxUV[2][10][2] = {
	{{0.5f, 0}, {0.5f, 0.5f}, {0, 0}, {0, 0.5f}, {0, 0.5f}, {0, 1}, {0.5f, 0.5f}, {0.5f, 1}, {1, 0.5f}, {1, 1}},
	{{1, 0}, {1, 0.5f}, {0.5f, 0}, {0.5f, 0.5f}, {1, 0.5f}, {1, 1}, {0.5f, 0.5f}, {0.5f, 1}, {0, 0.5f}, {0, 1}},
};

// Scene::render: wind_vec = (4, 4, 3) x 1.4925374e-5, then rotated about z by wind_angle (an oscillation within +-45
// degrees that is not decoded; taken as 0 here). Its xy is all FarmsOut::draw uses.
constexpr float kWindX = 4.0f * 1.4925374e-5f, kWindY = 4.0f * 1.4925374e-5f;

// LeaderData::get_radius: CITY_CENTER_RADIUS (20 tiles) + (level - 1) x CITY_CENTER_POP_RADIUS (4), at most 64 tiles.
// (The Indian tribe bonus is not applied.)
float city_radius(int level) { return float(std::min(20 + (std::max(level, 1) - 1) * 4, 64)) * kTCoordSize; }
constexpr float kNearestFarm = 1152.0f;          // FarmsData::get_nearest_farm_type search radius (0x480)

}  // namespace

FarmParams load_farm_params(const GameFiles& files) {
	FarmParams p;
	for_each_element(parse_xml(files.read_text("Data/building_graphics.xml")), "FARM", [&](const XmlElement& e) {
		static const char* keys[4] = {"stalk_red", "stalk_green", "stalk_blue", "stalk_alpha"};
		for (int i = 0; i < 4; i++) p.stalk[i] = uint8_t(e.get_int(keys[i], -1));  // missing: -1, i.e. 0xFF
		p.min_height = e.get_float("min_height", p.min_height / 3) * 3.0f;
		p.max_height = e.get_float("max_height", p.max_height / 3) * 3.0f;
	});
	return p;
}

std::vector<Farm> place_farms(const Terrain& t, const std::vector<FarmSite>& sites, const std::vector<CitySite>& cities) {
	std::vector<Farm> farms;
	std::vector<int> farm_city;
	for (const FarmSite& s : sites) {
		Farm f;
		f.cx = s.cx, f.cy = s.cy, f.who = s.who;
		f.tx0 = int(std::floor(s.cx / kTCoordSize)) - 2;
		f.ty0 = int(std::floor(s.cy / kTCoordSize)) - 2;
		for (int x = 0; x <= 4; x++) {
			for (int y = 0; y <= 4; y++) {
				const int tx = std::clamp(f.tx0 + x, 0, t.vxs() - 1), ty = std::clamp(f.ty0 + y, 0, t.vys() - 1);
				f.corner_z[x][y] = t.height(tx, ty);
			}
		}
		// The farm's city (Build::find_city -> ObjectsData::find_city_at): the nearest city of the same owner whose radius
		// reaches the farm.
		int city = -1;
		float best = 0;
		for (size_t c = 0; c < cities.size(); c++) {
			if (cities[c].who != s.who) continue;
			const float dx = cities[c].cx - s.cx, dy = cities[c].cy - s.cy, d = std::sqrt(dx * dx + dy * dy);
			if (d <= city_radius(cities[c].level) && (city < 0 || d < best)) best = d, city = int(c);
		}
		// Farms::add: a farm without a city is an animal pen when its object slot is odd. In a city, the farm is wheat once
		// the city has an animal pen; otherwise the fifth farm is a pen, else it copies the nearest farm within 1152 units,
		// else the fourth farm is a pen, else a random 1-in-4 (here: wheat).
		if (city < 0) {
			f.type = (s.slot & 1) ? FarmType::Animal : FarmType::Wheat;
		} else {
			int others = 0, wheat = 0;
			for (size_t i = 0; i < farms.size(); i++) {
				if (farm_city[i] != city) continue;
				others++;
				wheat += farms[i].type == FarmType::Wheat;
			}
			if (others != wheat) {
				f.type = FarmType::Wheat;
			} else if (others == 4) {
				f.type = FarmType::Animal;
			} else {
				const Farm* nearest = nullptr;
				float nd = kNearestFarm * kNearestFarm;
				for (const Farm& o : farms) {
					const float dx = o.cx - s.cx, dy = o.cy - s.cy, d = dx * dx + dy * dy;
					if (d <= nd) nd = d, nearest = &o;
				}
				if (nearest) f.type = nearest->type;
				else f.type = others == 3 ? FarmType::Animal : FarmType::Wheat;
			}
		}
		farms.push_back(f);
		farm_city.push_back(city);
	}
	return farms;
}

void farms_inc_time(std::vector<Farm>& farms, uint32_t& seed) {
	for (Farm& f : farms) {
		if (f.type == FarmType::Animal || f.build < 1.0f) continue;  // only finished wheat farms grow
		int bare = 0;
		for (int y = 0; y < 4; y++) {
			for (int x = 0; x < 4; x++) {
				float& p = f.percent[x][y];
				uint8_t& s = f.status[x][y];
				if (s == kWheatGrowing) p += 0.005f;
				if (s == kWheatSnipped) p -= 0.01f;
				if (p <= 0.0f) {
					s = kWheatBare, p = 0.0f;
					bare++;
				} else if (p >= 1.0f) {
					s = kWheatRipe, p = 1.0f;
				}
			}
		}
		// Chance per mille that one bare cell starts growing: 20 with 12+ bare cells, (bare - 4) x 20 / 8 with 5..11.
		int chance = 0;
		if (bare >= 12) chance = 20;
		else if (bare >= 5) chance = (bare - 4) * 20 / 8;
		if (chance < 1) continue;
		if (random_get(seed, 0, 0xFFFF) % 1000 >= chance) continue;
		int pick = bare <= 1 ? 0 : random_get(seed, 0, 0xFFFF) % bare;
		for (int y = 0; y < 4 && pick >= 0; y++) {
			for (int x = 0; x < 4 && pick >= 0; x++) {
				if (f.percent[x][y] != 0.0f) continue;
				if (pick-- == 0) f.status[x][y] = kWheatGrowing;
			}
		}
	}
}

void build_wheat(const std::vector<Farm>& farms, const FarmParams& p, uint32_t frame, WheatMesh& out) {
	out.vertices.clear();
	out.indices.clear();
	const uint8_t sr = p.stalk[0], sg = p.stalk[1], sb = p.stalk[2], sa = p.stalk[3];
	// Farms in painter's order (the render-object tree, sorted by x + y like the other objects).
	std::vector<const Farm*> order;
	for (const Farm& f : farms) {
		if (f.type != FarmType::Animal) order.push_back(&f);
	}
	std::stable_sort(order.begin(), order.end(), [](const Farm* a, const Farm* b) { return a->cx + a->cy < b->cx + b->cy; });
	for (const Farm* fp : order) {
		const Farm& f = *fp;
		// Back to front: rows along +y outside, cells along +x inside (the camera looks toward -x, -y).
		for (int y = 0; y < 4; y++) {
			for (int x = 0; x < 4; x++) {
				const float g = f.percent[x][y];
				const float height = p.min_height + (p.max_height - p.min_height) * g;
				// Colour: young wheat is the stalk tint, fading in from stalk_alpha (scaled by construction progress) to
				// opaque; then the tint fades to white, so ripe wheat shows the texture unmodified.
				uint8_t c[4] = {sr, sg, sb, sa};
				if (g > 0.25f) {
					if (g <= 0.75f) {
						const float k = (g - 0.25f) * 2.0f;
						for (int i = 0; i < 3; i++) c[i] = uint8_t(uint32_t(float(255 - p.stalk[i]) * k + float(p.stalk[i])));
						c[3] = 255;
					} else {
						c[0] = c[1] = c[2] = c[3] = 255;
					}
				}
				if (g < 0.25f) c[3] = uint8_t(int(float(255 - sa) * g * 4.0f + float(sa) * f.build));
				// Wind: a triangle wave over 64 frames, phase-shifted per cell, scaled by growth.
				const uint32_t phase = (frame + uint32_t(5 * y + 17 * x + f.tx0 + f.ty0)) & 63;
				const float tri = float(phase < 32 ? phase : 64 - phase);
				const float sway = (tri * 0.0625f - 1.0f) * 25000.0f * g;
				const float cx = float(f.tx0 + x) * kTCoordSize + 96.0f, cy = float(f.ty0 + y) * kTCoordSize + 96.0f;
				const int uv = (x + y) & 1;
				const uint32_t base = uint32_t(out.vertices.size());
				for (int i = 0; i < 10; i++) {
					const BoxVertex& b = kBox[i];
					ColorVertex v{};
					v.px = cx + b.x + (b.wind ? kWindX * sway : 0.0f);
					v.py = cy + b.y + (b.wind ? kWindY * sway : 0.0f);
					v.pz = b.z * height + f.corner_z[x + b.col][y + b.row];
					v.nz = 1;
					v.u = kBoxUV[uv][i][0], v.v = kBoxUV[uv][i][1];
					v.r = c[0], v.g = c[1], v.b = c[2], v.a = c[3];
					out.vertices.push_back(v);
				}
				for (uint32_t k : kBoxFaces) out.indices.push_back(base + k);
			}
		}
	}
}

void build_farm_underlays(const Terrain& t, const std::vector<Farm>& farms, FarmType type, UnderlayMesh& out) {
	out.vertices.clear();
	out.indices.clear();
	for (const Farm& f : farms) {
		if (f.type != type) continue;
		const uint32_t base = uint32_t(out.vertices.size());
		for (int j = 0; j <= 4; j++) {
			for (int i = 0; i <= 4; i++) {
				const int tx = std::clamp(f.tx0 + i, 0, t.vxs() - 1), ty = std::clamp(f.ty0 + j, 0, t.vys() - 1);
				ColorVertex2 v{};
				v.base.px = float(f.tx0 + i) * kTCoordSize;
				v.base.py = float(f.ty0 + j) * kTCoordSize;
				v.base.pz = t.height(tx, ty);
				v.base.nz = 1;
				v.base.u = v.u1 = float(i) / 4.0f;
				v.base.v = v.v1 = float(j) / 4.0f;
				v.base.r = v.base.g = v.base.b = v.base.a = 255;
				out.vertices.push_back(v);
			}
		}
		for (int j = 0; j < 4; j++) {
			for (int i = 0; i < 4; i++) {
				const uint32_t a = base + uint32_t(j * 5 + i), b = a + 1, c = a + 5, d = c + 1;
				out.indices.insert(out.indices.end(), {a, b, c, c, b, d});
			}
		}
	}
}

std::vector<FarmAnimal> farm_animals(const Farm& f) {
	std::vector<FarmAnimal> out;
	if (f.type != FarmType::Animal || f.build < 1.0f) return out;
	uint32_t seed = uint32_t(int32_t(f.cx)) * 73856093u ^ uint32_t(int32_t(f.cy)) * 19349663u;
	for (int i = 0; i < 5; i++) {
		FarmAnimal a;
		a.type = (random_get(seed, 0, 0xFFFF) & 1) == 0 ? "FARMCHICKEN" : "FARMPIG";
		a.y = f.cy - 192.0f + float(random_get(seed, 0, 0xFFFF) % 384);
		a.x = f.cx - 192.0f + float(random_get(seed, 0, 0xFFFF) % 384);
		a.angle_deg = float(random_get(seed, 0, 360));
		a.variant = random_get(seed, 0, 2);
		out.push_back(a);
	}
	return out;
}

}  // namespace ron
