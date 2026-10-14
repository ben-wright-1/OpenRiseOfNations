// graphics.txt in the game root: "#SECTION" headers followed by comma-separated lines (tree models and textures per
// tileset, land doobers, terrain and water constants).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "assets/files.hpp"

namespace ron {

class GraphicsTxt {
public:
	explicit GraphicsTxt(const std::string& text);
	// Lines of a section (case-insensitive name, without '#'), trimmed; blank and ';' comment lines dropped. Null if absent.
	const std::vector<std::string>* section(const std::string& name) const;

private:
	std::map<std::string, std::vector<std::string>> sections_;  // lowercase name -> lines
};

GraphicsTxt load_graphics_txt(const GameFiles& files);

// Splits "a, b, c" on commas and trims each field.
std::vector<std::string> split_fields(const std::string& line);

}  // namespace ron
