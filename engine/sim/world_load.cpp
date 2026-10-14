#include "sim/world_load.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

#include "sim/placement.hpp"

namespace ron::sim {

const LandGoods kStockLandGoods = {{
	{{3, 0, -1, -1}},    // 0 land: Knowledge, Food
	{{-1, -1, -1, -1}},  // 1 sandy
	{{-1, -1, -1, -1}},  // 2 ocean
	{{-1, -1, -1, -1}},  // 3 coast
	{{1, -1, -1, -1}},   // 4 forest: Timber
	{{4, -1, -1, -1}},   // 5 mountains: Metal
	{{-1, -1, -1, -1}},  // 6 rocks
	{{5, -1, -1, -1}},   // 7 oil: Oil
	{{4, -1, -1, -1}},   // 8 cliffs: Metal
}};

LandGoods land_goods(const Rules& rules) {
	LandGoods out{};
	for (auto& l : out) l.fill(TYPE_NONE);
	for (size_t i = 0; i < out.size() && i < rules.lands.size(); i++) {
		for (size_t k = 0; k < 4 && k < rules.lands[i].size(); k++) out[i][k] = rules.lands[i][k].good;
	}
	return out;
}

// ---------------------------------------------------------------- terrain chunk

void load_terrain(World& w, const Terrain& t) {
	w.init(t.xs, t.ys);
	// load_wcoord_chunk3: file flag -> WData flag. FILE_FLAG_MOUNTAIN (2) is ignored: the mountains chunk rebuilds it.
	static const std::array<std::array<uint16_t, 2>, 7> kMap = {{
		{{0x04, wflag::kForest}},
		{{0x01, wflag::kRock}},
		{{0x08, wflag::kCoast | wflag::kOriginalCoast}},
		{{0x20, wflag::kRiver}},
		{{0x10, wflag::kOil}},
		{{0x40, wflag::kCliff}},
		{{0x80, wflag::kHalfLand}},
	}};
	for (size_t i = 0; i < w.wdata.size(); i++) {
		const WCoordCell& c = t.wcoords[i];
		WData& d = w.wdata[i];
		uint16_t f = 0;
		for (const auto& m : kMap) {
			if (c.flags & m[0]) f |= m[1];
		}
		d.flags = f;
		d.land = int8_t(c.land);
		d.land_sub = c.land_sub;
		if (d.land == land::kSandy && d.land_sub != 0) d.land_sub = 0;  // "Bad WCoord found and fixed"
	}
	if (t.tcoord_mask.size() != size_t(w.tile_size)) throw std::runtime_error("tcoord grid size mismatch");
	// load_tcoord_chunk2
	for (size_t i = 0; i < w.tdata.size(); i++) {
		const uint16_t m = t.tcoord_mask[i];
		uint16_t v = 0;
		if (m & 0x10) v = uint16_t((v & ~tmask::kMedium) | tmask::kWater);
		if (m & 0x20) v |= tmask::kCoastal;
		if (m & 0x02) v |= tmask::kRiverB;
		w.tdata[i] = v;
	}
}

std::vector<PlacedMountain> add_mountains(World& w, const Terrain& t, MountainTemplates& templates, int32_t* outside) {
	std::vector<PlacedMountain> placed;
	int32_t off = 0;
	for (const MountainStamp& s : t.mountains) {
		const MountainRange* r = templates.get(s.range_index);
		if (!r) continue;
		if (r->face_wcoords != int32_t(s.num_coords)) continue;  // load_mountains_chunk: "invalid number of coords"
		for (const Offset& o : r->wcoords) {
			const int32_t x = s.wx + o[0], y = s.wy + o[1];
			if (!w.in_w(x, y)) {
				off++;
				continue;
			}
			WData& d = w.w(x, y);
			d.flags = uint16_t((d.flags & 0xffc3) | (d.flags & 0x404) | wflag::kMountain);
		}
		for (const Offset& o : r->tcoords) {
			const int32_t tx = s.wx * 4 + o[0], ty = s.wy * 4 + o[1];
			if (!w.in_t(tx, ty)) {
				off++;
				continue;
			}
			w.set_blocked_at(tx, ty, true);
			uint16_t& m = w.t(tx, ty);
			m = uint16_t((m & 0xfffe) | tmask::kMountain);
		}
		// Mountains::set_behind_flags @00898050: NW, N, NE of each mountain tcoord, unless mountain.
		for (const Offset& o : r->tcoords) {
			for (int k = 1; k <= 3; k++) {
				const int32_t x = s.wx * 4 + o[0] + move_x[k], y = s.wy * 4 + o[1] + move_y[k];
				if (w.in_t(x, y) && !w.is_mountain_at(x, y)) w.t(x, y) |= tmask::kPermBehind;
			}
		}
		placed.push_back({s.wx, s.wy, s.range_index, r->tcoords, r->wcoords});
	}
	if (outside) *outside = off;
	return placed;
}

// ---------------------------------------------------------------- compute_all_vals

int32_t compute_all_vals(World& w, Random& rng, const LandGoods& lands, const Regions& regions) {
	const Circles& C = circles();
	int32_t draws = 0;
	for (int32_t x = 0; x < w.xs; x++) {
		for (int32_t y = 0; y < w.ys; y++) {
			WData& d = w.w(x, y);
			if (World::is_ocean(d)) {
				d.val = 0;
				d.goods = 0;
				continue;
			}
			int32_t make[6] = {};
			int32_t count = 0, water = 0;
			// Rings 3 and 4. Every in-map cell counts as land; water cells are taken back off (as the original's
			// register juggling does); a GATHERED centre tcoord skips the MAKE goods and the water test.
			for (int32_t c = C.radius[2]; c < C.radius[4]; c++) {
				const int32_t cx = x + C.x[size_t(c)], cy = y + C.y[size_t(c)];
				if (!w.in_w(cx, cy)) continue;
				count++;
				if (w.t(cx * 4 + 2, cy * 4 + 2) & tmask::kGathered) continue;
				const auto& g = lands[size_t(w.gather_land(cx, cy))];
				for (int32_t k = 0; k < 6; k++) {
					if (k == 3) continue;
					for (int32_t j = 0; j < 4; j++) make[k] += g[size_t(j)] == k;
				}
				const WData& n = w.w(cx, cy);
				if ((n.flags & wflag::kHalfLand) || World::is_ocean(n)) {
					water++;
					count--;
				}
			}
			// The 5x5 around: off map -4; gather land forest/mountain -2; half or open water -1.
			for (int k = 0; k < 25; k++) {
				const int32_t nx = x + move_x[k], ny = y + move_y[k];
				if (!w.in_w(nx, ny)) {
					count -= 4;
					continue;
				}
				const WData& n = w.w(nx, ny);
				const int32_t gl = w.gather_land(nx, ny);
				if (gl == land::kForest || gl == land::kMountains) count -= 2;
				if ((n.flags & wflag::kHalfLand) || World::is_ocean(n)) count -= 1;
			}
			int32_t v = count / 4 + rng.get(0, 0xffff) % 10;
			draws++;
			if (count / 5 < water && water < count / 3) v = v * 3 / 2;
			if (count / 3 <= water) v = v / 10;
			if (make[1] > 4) v *= std::min(make[1] / 4, 5) + 1;
			if (make[4] != 0) v = v * 3 / 2;
			const int32_t rs = regions.r[size_t(d.region)].size;
			if (rs < w.size / 128) v = v / 2;
			if (rs < w.size / 64) v = v * 3 / 4;
			if (rs < w.size / 16) v = v * 3 / 4;
			d.val = uint8_t(std::clamp(v, 0, 255));
			d.goods = uint8_t((make[0] > 4 ? 1 : 0) | (make[1] > 4 ? 2 : 0) | (make[2] > 4 ? 4 : 0) | (make[4] > 4 ? 0x10 : 0) | 0x20);
		}
	}
	return draws;
}

// ---------------------------------------------------------------- adjust_for_blocking

void eliminate_dead_mount_spots(World& w, std::vector<WCoord>* filled) {
	// The original's DynamicBitMask is sized xs*xs and indexed y*xs + x (overruns when ys > xs; no stock map has that).
	std::vector<uint8_t> visited(size_t(w.xs) * size_t(std::max(w.xs, w.ys)), 0);
	for (int pass = 0; pass < 2; pass++) {
		for (int32_t y = 0; y < w.ys; y++) {
			for (int32_t x = 0; x < w.xs; x++) {
				if (!(w.w(x, y).flags & wflag::kMountain)) continue;
				for (int k = 0; k <= 8; k++) {
					const int32_t cx = x + move_x[k], cy = y + move_y[k];
					if (!w.in_w(cx, cy)) continue;
					const size_t bit = w.wi(cx, cy);
					if (visited[bit]) continue;
					bool open = false;
					for (int o = 1; o <= 4 && !open; o++) {
						const int32_t nx = cx + orthog_x[o], ny = cy + orthog_y[o];
						open = w.in_w(nx, ny) && !(w.w(nx, ny).flags & (wflag::kForest | wflag::kMountain));
					}
					if (!open) {
						if (filled) filled->push_back({cx, cy});
						for (int i = 0; i < 16; i++) {
							const int32_t tx = traverse_x(cx, i), ty = traverse_y(cy, i);
							if (w.t(tx, ty) & tmask::kBlocked) continue;
							w.set_blocked_at(tx, ty, true);
							w.set_land(cx, cy, w.w(cx, cy).land, -1, wflag::kMountain);
						}
					}
					visited[bit] = 1;
				}
			}
		}
	}
}

void set_edge_flags(World& w, const std::vector<PlacedMountain>& mountains) {
	auto edge = [&](int32_t tx, int32_t ty) {
		if (w.in_t(tx, ty) && w.has_gather_access(tx, ty)) w.t(tx, ty) |= tmask::kGatherEdge;
	};
	for (const PlacedMountain& m : mountains) {  // Mountains::set_edge_flags @0089c1a0
		for (const Offset& o : m.tcoords) edge(m.wx * 4 + o[0], m.wy * 4 + o[1]);
	}
	// Cliffs::set_edge_flags @008a85e0: no stock scenario has cliffs.
	for (int32_t y = 0; y < w.ys; y++) {
		for (int32_t x = 0; x < w.xs; x++) {
			if (!(w.w(x, y).flags & wflag::kForest)) continue;
			for (int i = 0; i < 16; i++) edge(traverse_x(x, i), traverse_y(y, i));
		}
	}
}

void adjust_for_blocking(World& w, Regions& regions, const std::vector<PlacedMountain>& mountains,
                         std::vector<WCoord>* dead_spots) {
	// 1. NEAR_BLOCKING halo around mountain/forest/cliff wcoords; forest wcoords become blocked tree tcoords.
	for (int32_t y = 0; y < w.ys; y++) {
		for (int32_t x = 0; x < w.xs; x++) {
			WData& d = w.w(x, y);
			if (!(d.flags & wflag::kImpassable)) continue;
			for (int k = 0; k <= 8; k++) {
				const int32_t nx = x + move_x[k], ny = y + move_y[k];
				if (w.in_w(nx, ny)) w.w(nx, ny).flags |= wflag::kNearBlocking;
			}
			if (!(d.flags & wflag::kForest)) continue;
			for (int i = 0; i < 16; i++) {
				const int32_t tx = traverse_x(x, i), ty = traverse_y(y, i);
				w.set_blocked_at(tx, ty, true);
				uint16_t& m = w.t(tx, ty);
				if ((m & tmask::kMedium) != tmask::kTree) d.solid--;
				m |= tmask::kTree | tmask::kPermBehind;
			}
		}
	}
	// 2. Regions
	regions.release_recalc(w);
	regions.find_all(w);
	// 3.
	eliminate_dead_mount_spots(w, dead_spots);
	// 4. Map-edge water is BAD_PATH two tcoords deep; forests open a seam on their east and south sides facing land.
	for (int32_t y = 0; y < w.ys; y++) {
		for (int32_t x = 0; x < w.xs; x++) {
			const WData& d = w.w(x, y);
			if ((x == 0 || y == 0 || x == w.xs - 1 || y == w.ys - 1) && !(d.flags & wflag::kHalfLand) &&
			    (d.land == land::kSandy || d.land == land::kOcean)) {
				for (int i = 0; i < 16; i++) {
					const int32_t tx = traverse_x(x, i), ty = traverse_y(y, i);
					if (tx < 2 || ty < 2 || tx > w.tile_xs - 3 || ty > w.tile_ys - 3) w.set_bad_path(tx, ty, true);
				}
			}
			if (!(d.flags & wflag::kForest)) continue;
			auto untree = [&](int32_t tx, int32_t ty) {
				uint16_t& m = w.t(tx, ty);
				if ((m & tmask::kMedium) == tmask::kTree) {
					m &= 0xffcf;
					w.wdata[w.w_of_t(tx, ty)].solid++;
				}
				m &= 0xfff7;
				if ((m & tmask::kLowField) != tmask::kMountain) w.set_blocked_at(tx, ty, false);
			};
			if (x + 1 < w.xs && !(w.w(x + 1, y).flags & (wflag::kForest | wflag::kMountain))) {
				for (int32_t ty = y * 4; ty < y * 4 + 4; ty++) untree(x * 4 + 3, ty);
			}
			if (y + 1 < w.ys && !(w.w(x, y + 1).flags & (wflag::kForest | wflag::kMountain))) {
				for (int32_t tx = x * 4; tx < x * 4 + 4; tx++) untree(tx, y * 4 + 3);
			}
		}
	}
	// 5.
	set_edge_flags(w, mountains);
}

// ---------------------------------------------------------------- objects

namespace {

std::string lower_ascii(std::string s) {
	for (char& c : s) {
		if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
	}
	return s;
}

// Types::get_type_index over [lo, hi): TYPENAME (NAME when empty), case-insensitive, first match wins.
std::map<std::string, int> names_of(const Rules& rules, int lo, int hi) {
	std::map<std::string, int> m;
	for (int i = lo; i < hi && size_t(i) < rules.types.size(); i++) {
		const Type& t = rules.types[size_t(i)];
		const ustr& n = t.type_name.empty() ? t.name : t.type_name;
		if (n.empty()) continue;
		m.emplace(lower_ascii(to_utf8(n)), i);
	}
	return m;
}

}  // namespace

ObjectStats place_objects(World& w, const Scenario& s, const Rules& rules) {
	ObjectStats st;
	const auto goods = names_of(rules, kGoods, kEndGoods), units = names_of(rules, kUnits, kBuilds),
	           builds = names_of(rules, kBuilds, kItem);
	auto unknown = [&](const std::string& n) {
		if (std::find(st.unknown.begin(), st.unknown.end(), n) == st.unknown.end()) st.unknown.push_back(n);
	};
	auto lookup = [](const std::map<std::string, int>& m, const std::string& n) {
		auto it = m.find(lower_ascii(n));
		return it == m.end() ? -1 : it->second;
	};
	for (const ScenarioGood& it : s.items) {
		item_init(w, it.cx, it.cy);
		st.items++;
	}
	for (const ScenarioGood& g : s.goods) {
		const int ti = lookup(goods, g.type);
		if (ti < 0) {
			unknown(g.type);
			continue;
		}
		st.goods++;
		if (ti == kOilGood) continue;
		const Type& gt = rules.types[size_t(ti)];
		st.outside += good_init(w, ti, gt, g.cx, g.cy);
		if (gt.block) st.goods_blocking++;
	}
	for (const ScenarioUnit& u : s.units) {
		if (u.inside >= 0 || (u.masks & 8)) continue;  // carried / garrisoned, or a crew slot that is not created
		const int ti = lookup(units, u.type);
		if (ti < 0) {
			unknown(u.type);
			continue;
		}
		st.units++;
		st.unit_cells += unit_footprint(w, rules.types[size_t(ti)], u.cx, u.cy, true);
	}
	// Buildings first, then cities (the order re/tools/worlddump.mjs uses; Scenario::buildings is in file order, cities
	// first). TODO(world-spec): the original reads chunk 7 (cities) before chunk 5; the grid result is the same unless
	// footprints overlap.
	for (int pass = 0; pass < 2; pass++)
	for (const ScenarioBuilding& b : s.buildings) {
		if (b.city != (pass == 1)) continue;
		const int ti = lookup(builds, b.type);
		if (ti < 0) {
			unknown(b.type);
			continue;
		}
		const Type& bt = rules.types[size_t(ti)];
		const auto c = snap_center(w, bt, b.cx, b.cy);
		if (c[0] != b.cx || c[1] != b.cy) st.resnapped++;
		start_me(w, bt, c[0], c[1]);  // Build::init -> Wall::init -> start_me(1)
		if (b.active) wall_start(w, bt, c[0], c[1], b.who);  // Build::activate -> Wall::activate -> start
		if (b.city) st.cities++;
		else st.builds++;
	}
	return st;
}

// ---------------------------------------------------------------- the load path

void build_world(WorldBuild& out, const GameFiles& files, const Scenario& s, const Rules* rules, uint32_t seed) {
	World& w = out.world;
	load_terrain(w, s.terrain);
	MountainTemplates templates(files);
	out.mountains = add_mountains(w, s.terrain, templates, &out.mountain_outside);
	// ScenarioRead::import ran Regions::clear_all before the terrain chunk: every region is empty here.
	out.regions = Regions();
	Random rng(seed);
	out.rng_draws = compute_all_vals(w, rng, rules ? land_goods(*rules) : kStockLandGoods, out.regions);
	out.regions.clear_all(w);
	out.dead_mount_spots.clear();
	adjust_for_blocking(w, out.regions, out.mountains, &out.dead_mount_spots);
	// ScenarioEditor::init_terrain: game_random := GameInfo.seed, Regions::clear_all + find_all (territory not here).
	out.regions.clear_all(w);
	out.regions.find_all(w);
	out.has_objects = rules != nullptr;
	if (rules) out.objects = place_objects(w, s, *rules);
}

}  // namespace ron::sim
