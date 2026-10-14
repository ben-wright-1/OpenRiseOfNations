// Resolves a scenario's objects to drawable graphics: who stands where, facing which way, drawn with which graphic
// and team colour. The recipe of docs/scenario-objects-spec.md section 9.
#pragma once

#include <string>
#include <vector>

#include "assets/buildings.hpp"
#include "assets/catalog.hpp"
#include "assets/files.hpp"
#include "assets/rules.hpp"
#include "assets/scenario.hpp"

namespace ron {

struct PlacedObject {
	enum Kind { Unit, Building, Good, Farm } kind = Unit;  // Farm: drawn by render/farms (wheat boxes or an animal pen)
	std::string graphic;  // a unit_graphics.xml or building_graphics.xml name; for goods the GOOD name; "FARM" for farms
	float x = 0, y = 0;   // world units
	float angle_deg = 0;  // units: the draw angle (facing + 180)
	int color = 0;        // team colour slot
	int who = 0, slot = -1;  // owner and object slot (buildings, farms and scenario units; -1 for turrets)
	bool city = false;       // a city centre (farms belong to the nearest city of their owner)
};

struct Battlefield {
	std::vector<PlacedObject> objects;
	std::vector<std::string> unresolved;  // "type (graphic tried)" for objects without a graphic
};

// `unit_names` is the unit graphics catalog (for variant lookups); `scenario_path` picks a conquest tribe override.
Battlefield resolve_battlefield(const GameFiles& files, const Scenario& sc, const std::string& scenario_path, const Rules& rules,
                                const BuildCatalog& builds, const std::vector<UnitGraphic>& unit_names);

}  // namespace ron
