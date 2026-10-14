// Effects catalog: the <AMMO> entries of Data/effects_graphics.xml. Besides flying ammunition these are the
// props that units carry (EXISTEVENT type=) and the animated add-ons of buildings (EXISTEVENT subject=).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "assets/files.hpp"

namespace ron {

struct AmmoPiece {
	std::string name;
	std::string model;        // relative .bh3 path
	std::string texture;      // relative .tga path, may be empty
	std::string renderstate;  // e.g. "Ammo Blend", "Ammo Team Blend", "Fire Blend", "Smoke Blend"
	std::string anim;         // relative .bha path (resolved through anim_graphics.xml), or empty
};

struct AmmoCatalog {
	std::vector<AmmoPiece> pieces;  // file order
	// Lookup as the game does it: case-insensitive, first entry in file order wins. Null if absent.
	const AmmoPiece* find(const std::string& name) const;

	std::map<std::string, size_t> by_lower;
};

// `tag` selects the element (AMMO by default; FLAG reads the flag pieces: GarrisonFlag, WaypointFlag, ScenarioFlag,
// RallyFlag, which have the same attributes).
AmmoCatalog load_ammo_catalog(const GameFiles& files, const std::map<std::string, std::string>& anim_names,
                              const char* tag = "AMMO");

}  // namespace ron
