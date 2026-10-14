// The rules data the renderer needs to name graphics: unit and building TYPENAME -> GRAPH, the age techs, and the
// nation (tribe) list with each nation's art styles. docs/scenario-objects-spec.md sections 6 and 7.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "assets/files.hpp"

namespace ron {

struct TribeStyle {
	int build_style = 0;   // BUILD_CONTINENT: 0 DEFAULT, 1 NEUROPE, 2 ASIAN, 3 KOREAN, 4 AMERICAN, 5 ARAB, ...
	int backup_style = 0;  // BACKUP_BUILD_CONTINENT
	int unit_style = 0;    // UNIT_CONTINENT: 0 DEFAULT, 1 ARAB, 2 AMERICAN, 3 ASIAN, 4 NA, 5 INDIA
};

struct Rules {
	std::map<std::string, std::string> unit_graph;   // TYPENAME -> GRAPH (first match wins)
	std::map<std::string, std::string> build_graph;  // TYPENAME -> GRAPH
	std::set<std::string> age_techs;                 // "Classical Age" .. "Information Age"
	std::vector<std::string> tribe_files;            // the 24 nation files, game-relative paths

	// The player's age (0 Ancient .. 7 Information): the number of age techs it knows.
	int age(const std::vector<std::string>& techs) const;
};

Rules load_rules(const GameFiles& files);

// Conquest campaigns replace the tribe list with conquest/CTW_<Campaign>_Map_01.xml's. Returns the override for a
// scenario under conquest/<Campaign>/, or an empty list.
std::vector<std::string> conquest_tribe_files(const GameFiles& files, const std::string& scenario_path);

TribeStyle load_tribe_style(const GameFiles& files, const std::string& tribe_file);

const char* build_style_name(int style);  // "DEFAULT", "NEUROPE", ...
const char* unit_style_name(int style);   // "DEFAULT", "ARAB", ...

}  // namespace ron
