// Unit graphics catalog: Data/unit_graphics.xml (model, texture, named animations per unit graphic)
// joined with Data/anim_graphics.xml (animation name -> .bha file).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "assets/files.hpp"

namespace ron {

struct UnitAnim {
	std::string action;  // e.g. CHAR_IDLE1
	std::string file;    // resolved path relative to the game dir, or empty if the name is unknown
};

// <EXISTEVENT>: while the unit plays `action` and start_ms <= t <= end_ms, the AMMO piece named `type` is drawn
// rigidly attached to attach slot `node` (-1: the unit's model origin). See docs/attachments-spec.md.
struct ExistEvent {
	std::string action;  // e.g. CHAR_ATTACK2
	uint16_t start_ms = 0;
	uint16_t end_ms = 0xFFFF;
	std::string type;    // AMMO name
	int node = -1;
};

struct UnitGraphic {
	std::string name;     // e.g. ADVMACHINEGUN-DEFAULT-AGE0
	std::string model;    // relative path, e.g. art/minigun_gunner.bh3
	std::string texture;  // relative path, may be empty
	float scale = 1.0f;
	// Interface icon (the portrait / command-card button): atlas id (TEX_UNITS_n), cell, explicit texel origin (-1 = from
	// the cell: 62 * col, 40 * row), docs/ui-spec.md section 6.4.
	std::string tex_id;
	int tex_col = 0, tex_row = 0, tex_x = -1, tex_y = -1;
	std::vector<UnitAnim> anims;
	std::vector<ExistEvent> exist_events;
};

// Data/anim_graphics.xml: animation name -> relative .bha path.
std::map<std::string, std::string> load_anim_names(const GameFiles& files);

std::vector<UnitGraphic> load_unit_catalog(const GameFiles& files);

}  // namespace ron
