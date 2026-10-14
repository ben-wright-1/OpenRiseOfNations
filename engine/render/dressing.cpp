#include "render/dressing.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "assets/tga.hpp"
#include "assets/xml.hpp"
#include "render/terrain_mesh.hpp"

namespace ron {

namespace {

struct RangeTemplate {
	int size = 0;  // 1 small, 2 medium, 3 large
	float height = 0;
	std::string tex, main_alpha, ring_alpha;
};

std::vector<RangeTemplate> read_templates(const GameFiles& files) {
	std::vector<RangeTemplate> out;
	for_each_element(parse_xml(files.read_text("Data/effects_graphics.xml")), "MOUNTAINS", [&](const XmlElement& section) {
		if (!out.empty()) return;
		for (const XmlElement& m : section.children) {
			if (m.name != "MOUNTAIN") continue;
			RangeTemplate r;
			const std::string area = to_lower(m.get("area"));
			r.size = area == "sm" || area == "sml" ? 1 : area == "med" ? 2 : area == "lg" ? 3 : 0;
			r.height = m.get_float("height", 0);
			auto file = [&](const char* n) {
				const XmlElement* c = m.child(n);
				return c ? normalize_game_path(c->get("file")) : std::string();
			};
			r.tex = file("TEMPLATE_TEX");
			r.main_alpha = file("MAIN_ALPHA_TEX");
			r.ring_alpha = file("RING_ALPHA_TEX");
			out.push_back(r);
		}
	});
	return out;
}

// MountainRange::init: one vertex per 4x4-pixel sample with non-zero alpha (48 world units per pixel), faces on complete
// cells, the covered wcoords, and the peak.
struct RangeMesh {
	struct V {
		int tx, ty;  // tcoord offset from the stamp's NW corner
		float z;
		float u0, v0, u1, v1;
	};
	std::vector<V> verts;
	std::vector<int> faces;
	std::vector<std::pair<int, int>> wcoords;
	int peak_tx = 0, peak_ty = 0;
};

RangeMesh build_range(const Image& img, float height) {
	RangeMesh r;
	const int W = img.width, H = img.height;
	auto A = [&](int x, int y) { return img.rgba[(size_t(y) * size_t(W) + size_t(x)) * 4 + 3]; };
	auto B = [&](int x, int y) { return img.rgba[(size_t(y) * size_t(W) + size_t(x)) * 4 + 2]; };
	const int ox = (W >> 1) % 16, oy = (H >> 1) % 16;
	int minx = W, miny = H, maxx = 0, maxy = 0;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			if (A(x, y)) minx = std::min(minx, x), miny = std::min(miny, y), maxx = std::max(maxx, x), maxy = std::max(maxy, y);
		}
	}
	const int tw = (W >> 2) + 3, th = (H >> 2) + 3;
	std::vector<int> tl(size_t(tw) * size_t(th), -1);
	float peak_z = 0;
	const float hz = height * 3.0f;
	for (int y = oy; y < H; y += 4) {
		for (int x = ox; x < W; x += 4) {
			if (!A(x, y)) continue;
			tl[size_t(y >> 2) * size_t(tw) + size_t(x >> 2)] = int(r.verts.size());
			const float vx = float((x - (W >> 1)) * 192) * 0.25f, vy = float((y - (H >> 1)) * 192) * 0.25f;
			const float vz = float(B(x, y)) * hz / 255.0f;
			RangeMesh::V v{int(std::lround(vx / 192.0f)), int(std::lround(vy / 192.0f)), vz, float(x) * 0.0625f, float(y) * 0.0625f,
			               float(x - minx) / float(std::max(1, maxx - minx)), float(y - miny) / float(std::max(1, maxy - miny))};
			if (vz > peak_z) {
				peak_z = vz;
				r.peak_tx = int(std::floor(vx / 192.0f));
				r.peak_ty = int(std::floor(vy / 192.0f));
			}
			r.verts.push_back(v);
		}
	}
	const int hw = (W >> 1) - ox, hh = (H >> 1) - oy;
	for (int y = oy; y < H; y += 4) {
		for (int x = ox; x < W; x += 4) {
			auto at = [&](int dx, int dy) { return tl[size_t((y >> 2) + dy) * size_t(tw) + size_t((x >> 2) + dx)]; };
			const int a = at(0, 0), b = at(1, 0), c = at(0, 1), d = at(1, 1);
			if (a < 0 || b < 0 || c < 0 || d < 0) continue;
			r.faces.insert(r.faces.end(), {a, b, c, c, b, d});
			const std::pair<int, int> w{x / 16 - hw / 16, y / 16 - hh / 16};
			if (std::find(r.wcoords.begin(), r.wcoords.end(), w) == r.wcoords.end()) r.wcoords.push_back(w);
		}
	}
	return r;
}

}  // namespace

Dressing build_dressing(const GameFiles& files, const GraphicsTxt& gfx, const Terrain& t, const Tileset& ts) {
	Dressing out;

	// Mountains: stamps (chunk 69) of range templates; vertices are the terrain's own grid vertices at stored heights
	// (adjust_for_mountains(1)), one mesh per range.
	const std::vector<RangeTemplate> templates = read_templates(files);
	std::map<int, RangeMesh> ranges;
	std::map<int, size_t> mesh_of_range;
	for (const MountainStamp& s : t.mountains) {
		if (s.range_index < 0 || size_t(s.range_index) >= templates.size()) continue;
		const RangeTemplate& tpl = templates[size_t(s.range_index)];
		auto it = ranges.find(s.range_index);
		if (it == ranges.end()) {
			const std::string path = files.resolve(tpl.tex);
			if (path.empty()) continue;
			it = ranges.emplace(s.range_index, build_range(parse_tga(read_file(path)), tpl.height)).first;
		}
		const RangeMesh& r = it->second;
		auto [mit, added] = mesh_of_range.emplace(s.range_index, out.mountains.size());
		if (added) out.mountains.push_back({tpl.main_alpha, tpl.ring_alpha, {}, {}});
		MountainRangeMesh& mesh = out.mountains[mit->second];

		const int stx = s.wx * 4, sty = s.wy * 4;
		std::map<std::pair<int, int>, int> by_tcoord;
		for (size_t i = 0; i < r.verts.size(); i++) by_tcoord[{r.verts[i].tx, r.verts[i].ty}] = int(i);
		std::vector<int> remap(r.verts.size(), -1);
		const uint32_t base = uint32_t(mesh.vertices.size());
		for (const auto& [mx, my] : r.wcoords) {
			const int wx = s.wx + mx, wy = s.wy + my;
			for (int gy = 0; gy <= 4; gy++) {
				for (int gx = 0; gx <= 4; gx++) {
					const int tx = wx * 4 + gx, ty = wy * 4 + gy;
					if (tx < 0 || ty < 0 || tx >= t.vxs() || ty >= t.vys()) continue;
					auto f = by_tcoord.find({tx - stx, ty - sty});
					if (f == by_tcoord.end() || remap[size_t(f->second)] >= 0) continue;
					const RangeMesh::V& tv = r.verts[size_t(f->second)];
					remap[size_t(f->second)] = int(mesh.vertices.size() - base);
					ColorVertex2 v{};
					v.base.px = float(tx) * kTCoordSize;
					v.base.py = float(ty) * kTCoordSize;
					v.base.pz = t.height(tx, ty);
					float n[3];
					terrain_normal(t, tx, ty, n);
					v.base.nx = n[0], v.base.ny = n[1], v.base.nz = n[2];
					v.base.u = tv.u0, v.base.v = tv.v0;
					v.u1 = tv.u1, v.v1 = tv.v1;
					v.base.r = v.base.g = v.base.b = v.base.a = 255;
					mesh.vertices.push_back(v);
				}
			}
		}
		for (size_t i = 0; i + 2 < r.faces.size(); i += 3) {
			const int a = remap[size_t(r.faces[i])], b = remap[size_t(r.faces[i + 1])], c = remap[size_t(r.faces[i + 2])];
			if (a < 0 || b < 0 || c < 0) continue;
			mesh.indices.insert(mesh.indices.end(), {base + uint32_t(a), base + uint32_t(b), base + uint32_t(c)});
		}

		// Snowcap: medium and large ranges, a 6x6 (or 8x8 when realigned) patch of terrain vertices around the peak.
		const int ptx = stx + r.peak_tx, pty = sty + r.peak_ty;
		const bool on_peak = std::any_of(r.wcoords.begin(), r.wcoords.end(), [&](const std::pair<int, int>& w) {
			return s.wx + w.first == (ptx >> 2) && s.wy + w.second == (pty >> 2);
		});
		if (ts.snowcaps && !ts.snowcap.empty() && on_peak && (tpl.size == 2 || tpl.size == 3)) {
			const int size = 6, lim = t.xs * 4 - size;  // the game clamps both axes against the map width
			int x0 = std::clamp(((ptx >> 2) * 4 + (ptx & 3) - 2) - (size >> 1) + 2, 0, lim);
			int y0 = std::clamp(((pty >> 2) * 4 + (pty & 3) - 2) - (size >> 1) + 2, 0, lim);
			int w = size, h = size;
			if (x0 & 1) x0--, w++, w += w & 1;
			if (y0 & 1) y0--, h++, h += h & 1;
			const uint32_t sb = uint32_t(out.snowcaps.size());
			for (int j = 0; j <= h; j++) {
				for (int i = 0; i <= w; i++) {
					const int tx = std::min(x0 + i, t.vxs() - 1), ty = std::min(y0 + j, t.vys() - 1);
					ColorVertex2 v{};
					v.base.px = float(tx) * kTCoordSize;
					v.base.py = float(ty) * kTCoordSize;
					v.base.pz = t.height(tx, ty);
					v.base.nz = 1;
					v.base.u = float(i) / float(w), v.base.v = float(j) / float(h);
					v.u1 = float(i) * 0.5f / float(w), v.v1 = float(j) * 0.5f / float(h);  // quadrant 0 of the alpha atlas
					v.base.r = v.base.g = v.base.b = 255;
					v.base.a = 128;
					out.snowcaps.push_back(v);
				}
			}
			for (int j = 0; j < h; j++) {
				for (int i = 0; i < w; i++) {
					const uint32_t a = sb + uint32_t(j * (w + 1) + i), b = a + 1, c = a + uint32_t(w) + 1, d = c + 1;
					out.snowcap_indices.insert(out.snowcap_indices.end(), {a, b, c, c, b, d});
				}
			}
		}
	}

	// Land doobers: bush (kind 0) and rock (kind 1) billboards from "#<TILESET>_LAND DOOBERS" (or "#LAND DOOBERS").
	const auto* names = gfx.section("LAND DOOBERS");
	const auto* rows = gfx.section(ts.name + "_LAND DOOBERS");
	if (!rows) rows = names;
	if (names && rows) {
		std::map<std::string, size_t> batch_of;
		std::map<std::string, std::pair<int, int>> tex_size;
		for (const Doober& d : t.doobers) {
			const std::string want = d.kind == 0 ? "bush" : d.kind == 1 ? "rock" : "";
			size_t idx = names->size();
			for (size_t i = 0; i < names->size(); i++) {
				if (to_lower(split_fields((*names)[i])[0]) == want) {
					idx = i;
					break;
				}
			}
			if (idx >= rows->size()) continue;
			const std::vector<std::string> f = split_fields((*rows)[idx]);
			if (f.size() < 6) continue;
			const std::string file = normalize_game_path(f[1]);
			const std::string abs = files.resolve(file);
			if (abs.empty()) continue;
			auto ts_it = tex_size.find(file);
			if (ts_it == tex_size.end()) {
				const Image img = parse_tga(read_file(abs));
				ts_it = tex_size.emplace(file, std::make_pair(img.width, img.height)).first;
			}
			const int tw = ts_it->second.first, th = ts_it->second.second;
			const int nrows = std::max(1, std::atoi(f[2].c_str())), ncols = std::max(1, std::atoi(f[3].c_str()));
			const float centering = float(std::atof(f[5].c_str()));
			const bool random = f.size() > 6 && !f[6].empty();
			const float x = float(d.x) + centering * 192.0f, y = float(d.y) + centering * 192.0f;
			const int row = random ? int(y * 13.117f + x * 27.53f) : 0;
			const int col = random ? int(std::fabs(x * 13.932f - y * 17.57f)) : 0;
			const int cell_w = tw / nrows, cell_h = th / ncols;  // rows divide the width, as the exe has it
			const int rr = ((row % nrows) + nrows) % nrows, cc = ((col % ncols) + ncols) % ncols;
			const float hw = float(cell_w) * 0.75f * std::sqrt(2.0f), hgt = float(cell_h) * 2.0f * std::sqrt(3.0f);
			const float z = float(int(t.sample_height(float(d.x), float(d.y))));
			const float u0 = float(cell_w * cc) / float(tw), u1 = float(cell_w * cc + cell_w) / float(tw);
			const float v0 = float(cell_h * rr) / float(th), v1 = float(cell_h * rr + cell_h) / float(th);
			auto [it, added] = batch_of.emplace(file, out.doobers.size());
			if (added) out.doobers.push_back({file, {}, {}});
			DooberBatch& b = out.doobers[it->second];
			const uint32_t base = uint32_t(b.vertices.size());
			b.vertices.push_back({x - hw, y + hw, z + hgt, 0, 0, 1, u0, v0, 255, 255, 255, 255});
			b.vertices.push_back({x + hw, y - hw, z + hgt, 0, 0, 1, u1, v0, 255, 255, 255, 255});
			b.vertices.push_back({x - hw, y + hw, z, 0, 0, 1, u0, v1, 255, 255, 255, 255});
			b.vertices.push_back({x + hw, y - hw, z, 0, 0, 1, u1, v1, 255, 255, 255, 255});
			b.indices.insert(b.indices.end(), {base, base + 1, base + 2, base + 2, base + 1, base + 3});
		}
	}
	return out;
}

}  // namespace ron
