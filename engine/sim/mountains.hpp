// Mountain range templates as the simulation grid sees them (MountainRange::init @008998b0, docs/world-spec.md
// section 3.2): the template's mountain tcoords and mountain wcoords, read from its TEMPLATE_TEX alpha. The render
// mesh of the same template is built separately (render/dressing).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/tga.hpp"

namespace ron::sim {

using Offset = std::array<int32_t, 2>;

struct MountainRange {
	// One per complete cell of the 4-pixel vertex lattice (all four corner samples opaque), offset from the stamp's
	// NW tcoord, in face order (rows, then columns).
	std::vector<Offset> tcoords;
	// 16x16-pixel blocks whose 5x5 corner samples are all opaque or at least 16 are, offset from the stamp's wcoord.
	std::vector<Offset> wcoords;
	// The face-wcoord count (distinct wcoords of the complete cells): a chunk-69 stamp's num_coords must equal it.
	int32_t face_wcoords = 0;
};

MountainRange build_mountain_range(const Image& template_tex);

// The <MOUNTAINS> templates of Data/effects_graphics.xml in file order (= the range index stored in chunk 69),
// with each range built on first use.
class MountainTemplates {
public:
	explicit MountainTemplates(const GameFiles& files);
	size_t count() const { return textures_.size(); }
	// Null for an invalid range index. Throws when the template texture is missing.
	const MountainRange* get(int32_t index);

private:
	const GameFiles& files_;
	std::vector<std::string> textures_;  // TEMPLATE_TEX per template
	std::map<int32_t, MountainRange> built_;
};

}  // namespace ron::sim
