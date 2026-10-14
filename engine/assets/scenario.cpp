#include "assets/scenario.hpp"

#include <zlib.h>

#include <algorithm>

namespace ron {

namespace {

std::vector<Chunk> children(const Bytes& b, const Chunk& c) { return read_chunks(b, c.start, c.end); }

const Chunk* find(const std::vector<Chunk>& list, uint16_t id) {
	for (const Chunk& c : list) {
		if (c.id == id) return &c;
	}
	return nullptr;
}

// Windows wchar_t (UTF-16) up to the first NUL, narrowed (the game's names are ASCII).
std::string wide_string(const Bytes& b, size_t p, size_t end) {
	std::string s;
	for (; p + 1 < end; p += 2) {
		const uint16_t c = read_u16(b, p);
		if (!c) break;
		s += c < 0x80 ? char(c) : '?';
	}
	return s;
}

// A {u32 count, ...} header chunk followed by its data chunk.
const Chunk& need(const std::vector<Chunk>& list, uint16_t id, const char* what) {
	const Chunk* c = find(list, id);
	if (!c) throw FormatError(std::string("scenario terrain: missing ") + what);
	return *c;
}

void load_terrain(const Bytes& b, const Chunk& tc, Terrain& t) {
	const auto kids = children(b, tc);
	for (const Chunk& c : kids) {
		switch (c.id) {
			case 81:  // tileset name
				t.tileset = wide_string(b, c.start, c.end);
				break;
			case 88: {  // wcoord3: 15 {count, xs, ys}, 16 count x {u16 flags, u8 land, u8 land_sub}
				const auto k = children(b, c);
				const Chunk& h = need(k, 15, "wcoord header");
				const uint32_t count = read_u32(b, h.start);
				t.xs = int(read_u32(b, h.start + 4));
				t.ys = int(read_u32(b, h.start + 8));
				if (t.xs <= 0 || t.ys <= 0 || t.xs > 1024 || t.ys > 1024 || count != uint32_t(t.xs * t.ys)) {
					throw FormatError("scenario terrain: bad wcoord dimensions");
				}
				const Chunk& d = need(k, 16, "wcoord data");
				if (d.end - d.start < size_t(count) * 4) throw FormatError("scenario terrain: short wcoord data");
				t.wcoords.resize(count);
				for (uint32_t i = 0; i < count; i++) {
					const size_t p = d.start + size_t(i) * 4;
					t.wcoords[i] = {read_u16(b, p), b[p + 2], b[p + 3]};
				}
				break;
			}
			case 68: {  // tcoord2: 17 {count, txs, tys}, 18 count x u16 mask
				const auto k = children(b, c);
				const uint32_t count = read_u32(b, need(k, 17, "tcoord header").start);
				const Chunk& d = need(k, 18, "tcoord data");
				if (d.end - d.start < size_t(count) * 2) throw FormatError("scenario terrain: short tcoord data");
				t.tcoord_mask.resize(count);
				for (uint32_t i = 0; i < count; i++) t.tcoord_mask[i] = read_u16(b, d.start + size_t(i) * 2);
				break;
			}
			case 89: {  // heights
				const size_t n = (c.end - c.start) / 4;
				t.heights.resize(n);
				for (size_t i = 0; i < n; i++) t.heights[i] = read_f32(b, c.start + i * 4);
				break;
			}
			case 99: {  // wcoord flags: 100 {count}, 101 count x u16
				const auto k = children(b, c);
				const uint32_t count = read_u32(b, need(k, 100, "coord flag header").start);
				const Chunk& d = need(k, 101, "coord flags");
				if (d.end - d.start < size_t(count) * 2) throw FormatError("scenario terrain: short coord flags");
				t.coord_flags.resize(count);
				for (uint32_t i = 0; i < count; i++) t.coord_flags[i] = read_u16(b, d.start + size_t(i) * 2);
				break;
			}
			case 69: {  // mountains: 70 {n}, 71 n x {u8 wx, u8 wy, u16 pad, i32 range, u8 num_coords, u8 pad[3]}
				const auto k = children(b, c);
				const uint32_t n = read_u32(b, need(k, 70, "mountain header").start);
				const Chunk& d = need(k, 71, "mountains");
				if (d.end - d.start < size_t(n) * 12) throw FormatError("scenario terrain: short mountains");
				for (uint32_t i = 0; i < n; i++) {
					const size_t p = d.start + size_t(i) * 12;
					t.mountains.push_back({b[p], b[p + 1], int32_t(read_u32(b, p + 4)), b[p + 8]});
				}
				break;
			}
			case 92: {  // rivers: 93 {n}, then per river 94 {num_nodes} + num_nodes x 95 {i32 wx, i32 wy}
				for (const Chunk& k : children(b, c)) {
					if (k.id == 94) t.rivers.emplace_back();
					if (k.id == 95 && !t.rivers.empty()) {
						t.rivers.back().push_back({int32_t(read_u32(b, k.start)), int32_t(read_u32(b, k.start + 4))});
					}
				}
				break;
			}
			case 73: {  // doobers: 74 {n}, 75 n x {i32 x, i32 y, u8 kind, u8 pad[3]}
				const auto k = children(b, c);
				const uint32_t n = read_u32(b, need(k, 74, "doober header").start);
				const Chunk& d = need(k, 75, "doobers");
				if (d.end - d.start < size_t(n) * 12) throw FormatError("scenario terrain: short doobers");
				for (uint32_t i = 0; i < n; i++) {
					const size_t p = d.start + size_t(i) * 12;
					t.doobers.push_back({int32_t(read_u32(b, p)), int32_t(read_u32(b, p + 4)), b[p + 8]});
				}
				break;
			}
			default: break;
		}
	}
	if (t.wcoords.empty()) throw FormatError("scenario terrain: no wcoord chunk");
	if (t.heights.size() != size_t(t.vxs()) * size_t(t.vys())) throw FormatError("scenario terrain: height grid size mismatch");
	if (!t.tcoord_mask.empty() && t.tcoord_mask.size() != size_t(t.xs) * 4 * size_t(t.ys) * 4) {
		throw FormatError("scenario terrain: tcoord grid size mismatch");
	}
}

}  // namespace

float Terrain::sample_height(float x, float y) const {
	const float gx = std::clamp(x / kTCoordSize, 0.0f, float(vxs() - 1) - 1e-4f);
	const float gy = std::clamp(y / kTCoordSize, 0.0f, float(vys() - 1) - 1e-4f);
	const int tx = int(gx), ty = int(gy);
	const float fx = gx - float(tx), fy = gy - float(ty);
	const float h00 = height(tx, ty), h10 = height(tx + 1, ty), h01 = height(tx, ty + 1), h11 = height(tx + 1, ty + 1);
	if (fx + fy <= 1.0f) return h00 + fx * (h10 - h00) + fy * (h01 - h00);
	return h11 + (1.0f - fx) * (h01 - h11) + (1.0f - fy) * (h10 - h11);
}

namespace {

int32_t i32(const Bytes& b, size_t p) { return int32_t(read_u32(b, p)); }

// A counted list: a header child {i32 count} and a data child of `count` fixed-size records.
template <typename F>
void counted_list(const Bytes& b, const Chunk& c, uint16_t header_id, uint16_t data_id, size_t record, F&& f) {
	const auto k = children(b, c);
	const Chunk* h = find(k, header_id);
	const Chunk* d = find(k, data_id);
	if (!h || !d) return;
	const int32_t n = i32(b, h->start);
	if (n < 0 || d->end - d->start < size_t(n) * record) throw FormatError("scenario: short counted list");
	for (int32_t i = 0; i < n; i++) f(d->start + size_t(i) * record);
}

// A container whose first child is {i32 count} and second child holds the records (ids vary by version).
template <typename F>
void counted_pair(const Bytes& b, const Chunk& c, size_t record, F&& f) {
	const auto k = children(b, c);
	if (k.size() < 2) return;
	const int32_t n = i32(b, k[0].start);
	if (n < 0 || k[1].end - k[1].start < size_t(n) * record) throw FormatError("scenario: short counted list");
	for (int32_t i = 0; i < n; i++) f(k[1].start + size_t(i) * record, k[1].id);
}

void load_leaders(const Bytes& b, const Chunk& c, Scenario& s) {
	for (const Chunk& pc : children(b, c)) {
		if (pc.id != 36) continue;
		const auto k = children(b, pc);
		const Chunk* rec = find(k, 107);
		if (!rec) rec = find(k, 87);
		if (!rec) continue;
		ScenarioPlayer p;
		p.who = i32(b, rec->start);
		p.tribe = i32(b, rec->start + 4);
		p.name = wide_string(b, rec->start + 0x14, rec->start + 0x14 + 200);
		p.color = b.at(rec->start + 0xE4);
		p.leader_flags = read_u32(b, rec->start + 8);
		p.is_human = i32(b, rec->start + 0xDC);
		p.gov = i32(b, rec->start + 0xC);
		p.handicap = i32(b, rec->start + 0xE0);
		if (rec->id == 107 && rec->end - rec->start >= 0xEC) p.difficulty = i32(b, rec->start + 0xE8);
		for (const Chunk& ch : k) {
			if (ch.id == 38 || ch.id == 76 || ch.id == 41 || ch.id == 44) p.child_order.push_back(ch.id);
			if (ch.id == 38 || ch.id == 76) {
				// wchar[256] records (86 / 78), or the legacy char[256] (40); the reader stops at the first name that
				// does not resolve (Game::load).
				auto& out = ch.id == 38 ? p.techs : p.types;
				const auto kk = children(b, ch);
				if (kk.size() < 2) continue;
				const int32_t n = i32(b, kk[0].start);
				const bool narrow = kk[1].id == 40;
				const size_t rs = narrow ? 256 : 512;
				if (n < 0 || kk[1].end - kk[1].start < size_t(n) * rs) throw FormatError("scenario: short tech list");
				for (int32_t i = 0; i < n; i++) {
					const size_t at = kk[1].start + size_t(i) * rs;
					if (narrow) {
						std::string t;
						for (size_t q = at; q < at + rs && b.at(q); q++) t += char(b.at(q));
						out.push_back(t);
					} else {
						out.push_back(wide_string(b, at, at + rs));
					}
				}
			} else if (ch.id == 41) {
				p.has_diplomacy = true;
				counted_pair(b, ch, 12, [&](size_t at, uint16_t) { p.diplomacy.push_back({i32(b, at), i32(b, at + 4), i32(b, at + 8)}); });
			} else if (ch.id == 44) {
				counted_pair(b, ch, 8, [&](size_t at, uint16_t) { p.stockpile.push_back({i32(b, at), i32(b, at + 4)}); });
			}
		}
		s.players.push_back(std::move(p));
	}
}

// Chunk 85 (or the legacy 32): {33 {i32 count, i32 first_in_progress}, 35 count x wchar[256] (legacy char[256])}.
void load_queue(const Bytes& b, const Chunk& qc, ScenarioBuilding& g) {
	const auto k = children(b, qc);
	if (k.size() < 2) return;
	const int32_t n = i32(b, k[0].start);
	g.first_in_progress = i32(b, k[0].start + 4);
	const bool narrow = qc.id == 32;
	const size_t rs = narrow ? 256 : 512;
	if (n < 0 || k[1].end - k[1].start < size_t(n) * rs) throw FormatError("scenario: short build queue");
	for (int32_t i = 0; i < n; i++) {
		const size_t at = k[1].start + size_t(i) * rs;
		if (narrow) {
			std::string t;
			for (size_t q = at; q < at + rs && b.at(q); q++) t += char(b.at(q));
			g.queue.push_back(t);
		} else {
			g.queue.push_back(wide_string(b, at, at + rs));
		}
	}
}

void load_objects(const Bytes& b, const Chunk& c, Scenario& s, int32_t& seq) {
	for (const Chunk& oc : children(b, c)) {
		const auto k = children(b, oc);
		if (oc.id == 25) {  // unit -> 83, UnitPropertiesChunk4 (0x244)
			const Chunk* r = find(k, 83);
			if (!r || r->end - r->start < 0x244) continue;
			ScenarioUnit u;
			u.who = i32(b, r->start);
			u.o = i32(b, r->start + 4);
			u.angle = read_u32(b, r->start + 0x10);
			u.masks = read_u32(b, r->start + 0x1C);
			u.type = wide_string(b, r->start + 0x26, r->start + 0x226);
			u.cx = i32(b, r->start + 0x228);
			u.cy = i32(b, r->start + 0x22C);
			u.inside = i32(b, r->start + 0x234);
			u.inside_who = i32(b, r->start + 0x238);
			u.o_up = i32(b, r->start + 8);
			u.o_down = i32(b, r->start + 0xC);
			u.dest_angle = read_u32(b, r->start + 0x14);
			u.tolerance = i32(b, r->start + 0x18);
			u.mana_burn = int16_t(b.at(r->start + 0x20) | (b.at(r->start + 0x21) << 8));
			u.form = int8_t(b.at(r->start + 0x22));
			u.stance = int8_t(b.at(r->start + 0x23));
			u.damage = i32(b, r->start + 0x230);
			u.seq = seq++;
			s.units.push_back(std::move(u));
		} else if (oc.id == 26) {  // building -> 84, BuildPropertiesChunk2 (0x224)
			const Chunk* r = find(k, 84);
			if (!r || r->end - r->start < 0x224) continue;
			ScenarioBuilding g;
			g.who = i32(b, r->start);
			g.o = i32(b, r->start + 4);
			g.cx = i32(b, r->start + 0x14);
			g.cy = i32(b, r->start + 0x18);
			g.type = wide_string(b, r->start + 0x1C, r->start + 0x21C);
			g.active = i32(b, r->start + 0x21C) != 0;
			g.damage = i32(b, r->start + 8);
			g.gather_x = i32(b, r->start + 0xC);
			g.gather_y = i32(b, r->start + 0x10);
			g.ever_seen = b.at(r->start + 0x220);
			if (const Chunk* q = find(k, 85)) load_queue(b, *q, g);
			else if (const Chunk* q2 = find(k, 32)) load_queue(b, *q2, g);
			g.seq = seq++;
			s.buildings.push_back(std::move(g));
		}
	}
}

void load_cities(const Bytes& b, const Chunk& c, Scenario& s, int32_t& seq) {
	for (const Chunk& cc : children(b, c)) {
		if (cc.id != 27) continue;
		const auto k = children(b, cc);
		const Chunk* r = find(k, 30);  // CityPropertiesChunk (0x2f0)
		if (!r || r->end - r->start < 0x2F0) continue;
		ScenarioBuilding g;
		g.city = true;
		g.who = i32(b, r->start);
		g.o = i32(b, r->start + 4);
		g.cx = i32(b, r->start + 0x14);
		g.cy = i32(b, r->start + 0x18);
		g.city_name = wide_string(b, r->start + 0x24, r->start + 0xEC);
		g.type = wide_string(b, r->start + 0xEC, r->start + 0x2EC);
		g.active = i32(b, r->start + 0x2EC) != 0;
		g.damage = i32(b, r->start + 8);
		g.gather_x = i32(b, r->start + 0xC);
		g.gather_y = i32(b, r->start + 0x10);
		g.city_flags = read_u32(b, r->start + 0x1C);
		g.founder = i32(b, r->start + 0x20);
		if (const Chunk* q = find(k, 85)) load_queue(b, *q, g);
		g.seq = seq++;
		s.buildings.push_back(std::move(g));
	}
}

void load_goods(const Bytes& b, const Chunk& c, uint16_t header_id, uint16_t data_id, std::vector<ScenarioGood>& out) {
	counted_list(b, c, header_id, data_id, 0x208, [&](size_t at) {
		out.push_back({wide_string(b, at, at + 512), i32(b, at + 0x200), i32(b, at + 0x204)});
	});
}

}  // namespace

Bytes gunzip(const Bytes& in) {
	z_stream z{};
	if (inflateInit2(&z, 15 + 32) != Z_OK) throw FormatError("inflateInit failed");  // 32: auto-detect gzip/zlib
	Bytes out;
	out.resize(in.size() * 4 + 4096);
	z.next_in = const_cast<Bytef*>(in.data());
	z.avail_in = uInt(in.size());
	int rc = Z_OK;
	while (rc != Z_STREAM_END) {
		if (z.total_out >= out.size()) out.resize(out.size() * 2);
		z.next_out = out.data() + z.total_out;
		z.avail_out = uInt(out.size() - z.total_out);
		rc = inflate(&z, Z_NO_FLUSH);
		if (rc != Z_OK && rc != Z_STREAM_END) {
			inflateEnd(&z);
			throw FormatError("corrupt compressed data");
		}
	}
	out.resize(z.total_out);
	inflateEnd(&z);
	return out;
}

Scenario parse_scenario(const Bytes& file) {
	const Bytes b = file.size() > 2 && file[0] == 0x1f && file[1] == 0x8b ? gunzip(file) : file;
	const auto root = read_chunks(b);
	if (root.size() != 1 || root[0].id != 0) throw FormatError("not a scenario chunk file");
	const auto top = children(b, root[0]);
	if (top.empty()) throw FormatError("empty scenario");
	Scenario s;
	const Chunk& h = top[0];
	s.header_version = h.id;
	s.name = wide_string(b, h.start + 8, std::min(h.end, h.start + 8 + 200));
	s.camera_x.fill(-1);
	s.camera_y.fill(-1);
	s.camera_zoom.fill(5);
	if (h.end - h.start >= 0x39 * 4) s.world_seed = read_u32(b, h.start + 0x38 * 4);
	if ((h.id == 0x6c || h.id == 0x52 || h.id == 0x66 || h.id == 0x67) && h.end - h.start >= 0x118) {
		s.has_settings = true;
		s.team_style = int32_t(read_u32(b, h.start + 0xd0));
		s.game_rules = int32_t(read_u32(b, h.start + 0xd4));
		s.start_tech = int32_t(read_u32(b, h.start + 0xec));
		s.end_tech = int32_t(read_u32(b, h.start + 0xf0));
		s.elimination = int32_t(read_u32(b, h.start + 0xf8));
		s.victory = int32_t(read_u32(b, h.start + 0xfc));
		s.score_goal = int32_t(read_u32(b, h.start + 0x100));
		s.time_limit = int32_t(read_u32(b, h.start + 0x104));
		s.wonderwin = int32_t(read_u32(b, h.start + 0x108));
		s.popwin = int32_t(read_u32(b, h.start + 0x10c));
		s.econwin = int32_t(read_u32(b, h.start + 0x110));
		s.rush_rules = int32_t(read_u32(b, h.start + 0x114));
		s.scenario_flags = read_u32(b, h.start + 0xe4);
		s.map_style = int32_t(read_u32(b, h.start + 0xd8));
		if (h.id == 0x6c && h.end - h.start >= 0x180) {
			s.pop_limit = int32_t(read_u32(b, h.start + 0x17c));
			s.ai_sea_map = int32_t(read_u32(b, h.start + 0x178));
		}
		if (h.id == 0x67 && h.end - h.start >= 0x12c) {
			s.pop_limit = int32_t(read_u32(b, h.start + 0x128));
			s.ai_sea_map = int32_t(read_u32(b, h.start + 0x124));
		}
	}
	if (h.id == 0x6c && h.end - h.start >= 0x5e * 4) {
		s.reveal_map = int32_t(read_u32(b, h.start + 0xf4));
		for (size_t i = 0; i < 8; i++) {
			s.camera_x[i] = int32_t(read_u32(b, h.start + (0x46 + i) * 4));
			s.camera_y[i] = int32_t(read_u32(b, h.start + (0x4e + i) * 4));
			s.camera_zoom[i] = int32_t(read_u32(b, h.start + (0x56 + i) * 4));
		}
	} else if ((h.id == 0x52 || h.id == 0x66 || h.id == 0x67) && h.end - h.start >= 0x124) {
		// Older headers store one camera (x, y, zoom at 0x118) that the loader copies to every player.
		s.camera_x.fill(int32_t(read_u32(b, h.start + 0x118)));
		s.camera_y.fill(int32_t(read_u32(b, h.start + 0x11C)));
		s.camera_zoom.fill(int32_t(read_u32(b, h.start + 0x120)));
	}
	const Chunk* tc = find(top, 2);
	if (!tc) throw FormatError("scenario has no terrain chunk");
	load_terrain(b, *tc, s.terrain);
	if (const Chunk* c = find(top, 11)) load_leaders(b, *c, s);
	// Objects and cities get their creation order (seq) from the top-level chunk order (ScenarioRead::import reads the
	// chunks in file order: 7 cities before 5 objects in every stock file).
	int32_t seq = 0;
	for (const Chunk& c : top) {
		if (c.id == 5) load_objects(b, c, s, seq);
		else if (c.id == 7) load_cities(b, c, s, seq);
	}
	if (const Chunk* c = find(top, 4)) load_goods(b, *c, 23, 24, s.goods);
	if (const Chunk* c = find(top, 3)) load_goods(b, *c, 21, 22, s.items);
	return s;
}

}  // namespace ron
