// PathFinder (pathfinder.obj; docs/movement-spec.md sections 6-8): the single global A* used at three levels — W
// (0x300, wcoords), T (0xc0, tcoords) and U (0x30, ucoords around units) — with its cost function, validity
// predicates, the find_upath straightening pass and time-sliced U-level searches stored in the unit.
// Reference model: re/tools/astar-ref.mjs.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "sim/objects.hpp"
#include "sim/world.hpp"

namespace ron::sim {

class Game;

// The pathing snapshot of collision blocks (PathFinder blocklist, Tree<CollBlock*, int> keyed wy * xs + wx): a search
// sees the blocks as they were when it first touched them. `present == false` records a null block.
struct SnapBlock {
	bool present = false;
	CollBlock block;
};
using BlockSnapshot = std::map<int32_t, SnapBlock>;

// The search trees (open list, open refs, closed list, valid cache, block snapshot) and their nodes. Defined in
// pathfinder.cpp; the same type is the pathfinder's own lists and a unit's saved (time-sliced) search
// (UnitData +0x104..+0x114, Unit::openlist).
struct SavedSearch;

// PathFinderData (+0x40..+0xc4 of the global `pathfinder`).
struct PathFinder {
	int32_t pathing_unit_who = -1, pathing_unit_o = -1;  // pathing_unit +0x54
	int32_t sx = 0, sy = 0;                              // the unit's TCoord
	int32_t anti_unit = 0;
	int32_t offx = 0, offy = 0;
	int32_t army = 0, iroquois = 0, worker = 0, no_danger = 0;
	int32_t limit = 0, saving = 0;
	int32_t avoid_land = 0, avoid_sea = 0, valid_hit = 0, scouting = 0, can_transport = 0;
	// Road search tunables (PathFinder::init @689ec0; saved by walk_data).
	int32_t road_base_val = 55, road_avoid_sea = 100, road_cross_coast = 100, road_enemy = 540, road_noone = 240,
	        road_bad_path = 200, road_river = 60, road_z_max = 600, road_diag_penalty = 8;

	// The pathfinder's own trees (+0x40..+0x50). They are emptied only by kill_lists, so a find_upath that returns
	// early (after valid_ucoord calls, before its search) leaves cached valid answers and block snapshots behind for
	// the next U-level search, exactly like the original. A time-sliced search moves them into the unit.
	std::unique_ptr<SavedSearch, SavedSearchDel> lists;
	// The current lists' block snapshot (blocklist +0x4c), read and filled by collide_here(pathing). Always points
	// into `lists` once a U-level validity test has run; null before that.
	BlockSnapshot* blocklist = nullptr;
};

// ---- entry points (each pushes onto / pops from `stack`, the unit's or a group's Stack<PathData>)

// PathFinder::find_wpath @688fc0 (start = (x, y)); the 4-argument wrapper @688e10 passes the unit's position.
int32_t find_wpath(Game& g, std::vector<PathData>& stack, int32_t x, int32_t y, int32_t who, int32_t o);
int32_t find_wpath(Game& g, std::vector<PathData>& stack, int32_t who, int32_t o);
// PathFinder::find_wpath_army @683730: find_wpath with army = 1. (The original ignores its stack argument and always
// searches into the global `grouppath`; callers pass that stack here.)
int32_t find_wpath_army(Game& g, std::vector<PathData>& stack, int32_t x, int32_t y, int32_t who, int32_t o);
// PathFinder::find_tpath @6897d0 / wrapper @688e60.
int32_t find_tpath(Game& g, std::vector<PathData>& stack, int32_t x, int32_t y, int32_t who, int32_t o);
int32_t find_tpath(Game& g, std::vector<PathData>& stack, int32_t who, int32_t o);
// PathFinder::find_upath wrapper @688eb0 (limit = 500 / max(1, repaths^2), halved when fast) -> @682f30.
int32_t find_upath(Game& g, std::vector<PathData>& stack, int32_t who, int32_t o, bool fast);
// PathFinder::find_upath_restore @688f40: resume the unit's saved search (saving = 1, limit = 300 / ...).
int32_t find_upath_restore(Game& g, std::vector<PathData>& stack, int32_t who, int32_t o);

// PathFinder::astar_path @683770 on the global pathfinder state set up by the entry points.
int32_t astar_path(Game& g, std::vector<PathData>& stack, int32_t step, bool fast);
// PathFinder::calc_cost @684e50.
int32_t calc_cost(Game& g, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t d, int32_t step, int32_t depth,
                  int32_t* transport);
// PathFinder::valid_ucoord @687c80 (Coords; cached in the current lists' valid cache under `key`), valid_tcoord
// @687d60 (here on tcoords), valid_wcoord @687da0 (Coords; goal (gx, gy) in Coords).
bool valid_ucoord(Game& g, Unit& u, int32_t x, int32_t y, uint32_t key);
bool valid_tcoord(Game& g, const Unit& u, int32_t tx, int32_t ty);
bool valid_wcoord(Game& g, const Unit& u, int32_t x, int32_t y, int32_t depth, int32_t gx, int32_t gy);
// PathFinder::kill_lists @687ae0: empties the pathfinder's own trees.
void kill_lists(PathFinder& pf);

// Unit::clear_partial_path @5e3920: frees the unit's saved search (openlist and trees).
void clear_partial_path(Unit& u);

}  // namespace ron::sim
