// The passability predicate the pathfinder and movement call: UnitData::invalid_loc @00607c30
// (docs/world-spec.md section 8, docs/movement-spec.md section 8.2) and its PathFinder wrappers.
#pragma once

#include <cstdint>

#include "sim/rules.hpp"
#include "sim/world.hpp"

namespace ron::sim {

// invalid_loc results.
enum : int32_t { kLocValid = 0, kLocOffMap = 1, kLocTerrain = 2, kLocWaterHazard = 3, kLocBlocked = 4 };

constexpr int kAircraftCarrier = 0x15f;  // tested with ObjectTypeData::is(.., strict) for the warship hazard
constexpr uint32_t kUnitMaskTransport = 0x800000;  // UnitData.unit_masks: boards / is a transport
constexpr uint32_t kUnitMask2MoveInForest = 0x4000, kUnitMask2NoTransport = 0x2000;  // UnitData.unit_masks2

// What invalid_loc reads from the moving unit (UnitData and its UnitTypeData).
struct Mover {
	int32_t domain = 0;  // type DomainIndex (+0x218): 0 ground, 1 sea, 2 air
	int32_t attack = 0;  // type attack (+0x1e8): attackers face the warship hazard and the own-building rule
	bool carrier = false;  // is(AIRCRAFT_CARRIER, strict): a warship without attack
	uint32_t type_unit_flags = 0;  // type unit_flags (+0x2b4); 0x10 ("e") makes it a transport
	uint32_t unit_masks = 0, unit_masks2 = 0;
	int32_t who = 0;
	int32_t own_tx = 0, own_ty = 0;  // tcoord of the unit's position: a unit inside a blocked tile may walk out
	bool path_transport = false;     // the top path entry has flag 4 (forces `transp`)
	bool fog_limited = false;        // the owner's leader flag 4 (human): unexplored tiles count as passable with `fog`
};
Mover make_mover(const Type& unit_type, uint32_t unit_masks, uint32_t unit_masks2, int32_t who, int32_t cx, int32_t cy);

// UnitData::can_transport @0046f960
inline bool can_transport(const Mover& u) {
	return ((u.unit_masks & kUnitMaskTransport) && !(u.unit_masks2 & kUnitMask2NoTransport)) || (u.type_unit_flags & 0x10);
}

// The invalid_loc parameters, in the original's order.
struct LocArgs {
	bool wlevel = false;  // p3: wcoord-level query (whole forest/mountain wcoords are out; BLOCKED is not tested)
	bool fog = false;     // p4: honour fog for fog-limited owners
	bool bldg = false;    // p5: attackers may enter other players' buildings
	bool transp = false;  // p6
	bool p7 = false;
};

// Game state outside the grid that invalid_loc consults. Either callback may be null: was_seen null = everything
// explored; building_owner null = no building found (owner -1).
struct LocEnv {
	const void* ctx = nullptr;
	// WorldData::was_seen(fx, fy, who) @006b53f0 (reveal map, allies, seen2).
	bool (*was_seen)(const void* ctx, int32_t fx, int32_t fy, int32_t who) = nullptr;
	// ObjectsData::find_any_building_at(tx, ty): the owner of the building found there, or -1.
	int32_t (*building_owner)(const void* ctx, int32_t tx, int32_t ty) = nullptr;
};

// 0 = valid, else kLoc*.
int32_t invalid_loc(const World& w, const Mover& u, int32_t tx, int32_t ty, const LocArgs& a, const LocEnv& env = LocEnv());

// PathFinder::valid_tcoord @00687d60: !invalid_loc(t, 0, 1, 1, 1, 0).
inline bool valid_tcoord(const World& w, const Mover& u, int32_t tx, int32_t ty, const LocEnv& env = LocEnv()) {
	return invalid_loc(w, u, tx, ty, {false, true, true, true, false}, env) == kLocValid;
}
// The terrain part of PathFinder::valid_ucoord @00687c80 (the unit-collision part is the caller's):
// !invalid_loc(t, 0, 1, 0, 1, 0).
inline bool valid_ucoord_terrain(const World& w, const Mover& u, int32_t tx, int32_t ty, const LocEnv& env = LocEnv()) {
	return invalid_loc(w, u, tx, ty, {false, true, false, true, false}, env) == kLocValid;
}
// PathFinder::valid_wcoord @00687da0 (all positions in Coords): false at the unit's avoid point, else
// invalid_loc(tcoord(c), 1, depth > 1, 0, 1, 0) == 0, with the water hazard (3) forgiven inside the goal's wcoord.
bool valid_wcoord(const World& w, const Mover& u, int32_t cx, int32_t cy, int32_t depth, int32_t goal_cx, int32_t goal_cy,
                  int32_t avoid_cx, int32_t avoid_cy, const LocEnv& env = LocEnv());
// ScenarioFuncSet::map_is_passable @009e4de0: -1 off the map, else 1 when the wcoord has no mountain/forest/cliff.
int32_t map_is_passable(const World& w, int32_t tx, int32_t ty);

}  // namespace ron::sim
