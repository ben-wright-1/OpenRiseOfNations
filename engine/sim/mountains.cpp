#include "sim/mountains.hpp"

#include <algorithm>
#include <stdexcept>

#include "assets/chunk.hpp"
#include "assets/xml.hpp"

namespace ron::sim {

MountainRange build_mountain_range(const Image& img) {
	MountainRange r;
	const int32_t W = img.width, H = img.height;
	auto opaque = [&](int32_t x, int32_t y) {
		return x >= 0 && y >= 0 && x < W && y < H && img.rgba[(size_t(y) * size_t(W) + size_t(x)) * 4 + 3] != 0;
	};
	// The template centre sits on the stamp's wcoord corner; samples start at the centre's offset within a wcoord.
	const int32_t ox = (W >> 1) % 16, oy = (H >> 1) % 16;
	const int32_t hw = (W >> 1) - ox, hh = (H >> 1) - oy;
	std::vector<Offset> face_w;
	for (int32_t y = oy; y < H; y += 4) {
		for (int32_t x = ox; x < W; x += 4) {
			if (!(opaque(x, y) && opaque(x + 4, y) && opaque(x, y + 4) && opaque(x + 4, y + 4))) continue;
			r.tcoords.push_back({x / 4 - hw / 4, y / 4 - hh / 4});
			const Offset w{x / 16 - hw / 16, y / 16 - hh / 16};
			if (std::find(face_w.begin(), face_w.end(), w) == face_w.end()) face_w.push_back(w);
		}
	}
	r.face_wcoords = int32_t(face_w.size());
	for (int32_t y0 = oy; y0 + 16 < H; y0 += 16) {
		for (int32_t x0 = ox; x0 + 16 < W; x0 += 16) {
			bool all = true;
			int32_t n = 0;
			for (int32_t i = 0; i <= 16; i += 4) {
				for (int32_t j = 0; j <= 16; j += 4) {
					if (opaque(x0 + i, y0 + j)) n++;
					else all = false;
				}
			}
			if (all || n > 15) r.wcoords.push_back({x0 / 16 - hw / 16, y0 / 16 - hh / 16});
		}
	}
	return r;
}

MountainTemplates::MountainTemplates(const GameFiles& files) : files_(files) {
	bool done = false;
	for_each_element(parse_xml(files.read_text("Data/effects_graphics.xml")), "MOUNTAINS", [&](const XmlElement& section) {
		if (done) return;
		done = true;
		for (const XmlElement& m : section.children) {
			if (m.name != "MOUNTAIN") continue;
			const XmlElement* c = m.child("TEMPLATE_TEX");
			textures_.push_back(c ? normalize_game_path(c->get("file")) : std::string());
		}
	});
}

const MountainRange* MountainTemplates::get(int32_t index) {
	if (index < 0 || size_t(index) >= textures_.size()) return nullptr;
	auto it = built_.find(index);
	if (it == built_.end()) {
		const std::string path = files_.resolve(textures_[size_t(index)]);
		if (path.empty()) throw std::runtime_error("mountain template texture not found: " + textures_[size_t(index)]);
		it = built_.emplace(index, build_mountain_range(parse_tga(read_file(path)))).first;
	}
	return &it->second;
}

}  // namespace ron::sim
