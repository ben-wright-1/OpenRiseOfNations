// ScenarioData: the scenario-script state of the simulation that the builtins and a few sim functions share
// (docs/script-spec.md 7; ScenarioFuncSet::init @a03c30 / close @a03650 give the defaults; saved with ScenarioData,
// state-walk.md). Presentation-only state (objectives panel, arrows, highlight, camera) is in sim/script_ui.hpp;
// reveal points, the scripted ally masks, diplomacy settings and the building bonus switches live on Game.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ron::sim {

struct ScenarioData {
	// ScenarioData::objectives[8]: the per-player goals of add_goal / complete_goal (ScenarioObjective {message, id,
	// colour, completed}), in ordered_insert order (append).
	struct Goal {
		std::u16string id, message;
		int32_t completed = 0;
	};
	std::array<std::vector<Goal>, 8> goals{};

	// ScenarioData::scenario_groups (NamedObjectArray<ScenarioGroup>, case-insensitive names): owner (leader index),
	// the member unit ids in insertion order, and the group's own find counter.
	struct Group {
		std::u16string name;
		int32_t who = 0;
		std::vector<int32_t> ids;
		int32_t counter = -1;
	};
	std::vector<Group> groups;
	int32_t find_group(const std::u16string& name) const;  // index or -1 (case-insensitive)

	// The objects the last any_object_near* / find_objects call found (LinkList<int>) and their owner.
	std::vector<int32_t> involved_objects;
	int32_t involved_who = -1;

	// Kill / destruction tallies of scripted games (Unit::close / Build::close with semaphore byte1 & 0x10 or CTW):
	// units_killed[who][type - 50] (read and cleared by unit_type_killed / unit_any_killed), builds_destroyed[who][type -
	// 414] (building_type_destroyed).
	std::array<std::array<int16_t, 352>, 8> units_killed{};
	std::array<std::array<int16_t, 129>, 8> builds_destroyed{};
	std::array<int32_t, 8> last_razed{-1, -1, -1, -1, -1, -1, -1, -1};  // Object::take_damage / disband (o)
	uint8_t city_lost = 0;                                         // BitMask: bit p = p lost a city (City::capture)
	std::array<int16_t, 8> city_lost_to{-1, -1, -1, -1, -1, -1, -1, -1};
	std::array<int32_t, 8> pop_cap{-1, -1, -1, -1, -1, -1, -1, -1};          // set_population_cap override
	std::array<int32_t, 8> last_build_built_o{-1, -1, -1, -1, -1, -1, -1, -1};
	std::array<int32_t, 8> last_unit_built_o{-1, -1, -1, -1, -1, -1, -1, -1};  // Build::train
	std::array<std::array<uint8_t, 8>, 8> war_blocked{};  // diplomacy_block_war
	std::array<uint8_t, 8> reinforcements_arrived{};
	std::array<std::array<int32_t, 3>, 8> camera_init{};  // camera_init_x / y / zoom (-1, -1, 5)
	bool objective_dlg_viewed = false;
	int32_t ignore_orders = 0;                                       // unit_ignore_orders
	std::array<std::vector<int32_t>, 8> objects_ignoring_orders{};  // (consulted by the group code: TODO)
	int32_t custom_time_limit = 0;  // set_time_limit (time_limit setting 8 = custom; 0 reads as 300)
	bool buildings_free = false, units_free = false, techs_free = false, plunder = true;
	uint32_t msg_color = 0x000000ffu, objective_color = 0xffffffffu, game_msg_color = 0xffffffffu;
	int32_t game_msg_time = 12000, msg_time = 200;
	std::u16string victory_message, defeat_message;

	ScenarioData() {
		for (auto& c : camera_init) c = {-1, -1, 5};
	}
};

}  // namespace ron::sim
