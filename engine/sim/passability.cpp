#include "sim/passability.hpp"

#include "sim/placement.hpp"

namespace ron::sim {

Mover make_mover(const Type& ut, uint32_t unit_masks, uint32_t unit_masks2, int32_t who, int32_t cx, int32_t cy) {
	Mover m;
	m.domain = ut.domain;
	m.attack = ut.attack;
	m.carrier = type_is(ut, kAircraftCarrier, true);
	m.type_unit_flags = ut.unit_flags;
	m.unit_masks = unit_masks;
	m.unit_masks2 = unit_masks2;
	m.who = who;
	m.own_tx = tcoord_of(cx);
	m.own_ty = tcoord_of(cy);
	return m;
}

int32_t invalid_loc(const World& w, const Mover& u, int32_t tx, int32_t ty, const LocArgs& a, const LocEnv& env) {
	const bool transp = a.transp || u.path_transport;
	if (!w.in_t(tx, ty)) return kLocOffMap;
	const int32_t wx = tx >> 2, wy = ty >> 2;
	// Fog-limited owners treat a tile whose four fog cells were never seen as passable.
	if (a.fog && u.fog_limited && env.was_seen) {
		const int32_t fx = wx * 2, fy = wy * 2;
		if (!env.was_seen(env.ctx, fx, fy, u.who) && !env.was_seen(env.ctx, fx, fy + 1, u.who) &&
		    !env.was_seen(env.ctx, fx + 1, fy, u.who) && !env.was_seen(env.ctx, fx + 1, fy + 1, u.who)) {
			return kLocValid;
		}
	}
	const uint16_t m = w.t(tx, ty);
	const uint16_t medium = m & tmask::kMedium;
	const bool forest_walker = (u.unit_masks2 & kUnitMask2MoveInForest) != 0;
	if (u.domain == 1) {
		if (medium == tmask::kWater) {
			// Warships (attackers, and the carrier) avoid BAD_PATH and COASTAL water.
			if ((u.attack != 0 || u.carrier) && (m & (tmask::kBadPath | tmask::kCoastal))) return kLocWaterHazard;
		} else {
			if (!transp && !a.p7) return kLocTerrain;
			if (!can_transport(u)) return kLocTerrain;
			if (w.w(wx, wy).flags & wflag::kImpassable) return kLocTerrain;
		}
	} else if (u.domain == 0) {
		const uint16_t wf = w.w(wx, wy).flags;
		if ((wf & wflag::kImpassable) && (!(wf & wflag::kForest) || !forest_walker) && a.wlevel && transp) return kLocTerrain;
		const uint16_t low = m & tmask::kLowField;
		if (medium == tmask::kTree || low == tmask::kMountain || low == tmask::kCliff) {
			if (medium != tmask::kTree || !forest_walker) return kLocTerrain;
		}
		if (((!transp && !a.p7) || !(u.unit_masks & kUnitMaskTransport)) && medium == tmask::kWater) return kLocTerrain;
	} else if (u.domain == 2) {
		return kLocValid;
	}
	// BLOCKED (trees excused for forest walkers), unless the unit itself stands on a blocked tile.
	if (!a.wlevel && (m & tmask::kBlocked) && (medium != tmask::kTree || !forest_walker) &&
	    !(w.in_t(u.own_tx, u.own_ty) && (w.t(u.own_tx, u.own_ty) & tmask::kBlocked))) {
		if (u.attack != 0 && a.bldg && w.is_built_at(tx, ty)) {
			const int32_t owner = env.building_owner ? env.building_owner(env.ctx, tx, ty) : -1;
			return owner == u.who ? kLocBlocked : kLocValid;  // other players' buildings are attack targets
		}
		return kLocBlocked;
	}
	return kLocValid;
}

bool valid_wcoord(const World& w, const Mover& u, int32_t cx, int32_t cy, int32_t depth, int32_t goal_cx, int32_t goal_cy,
                  int32_t avoid_cx, int32_t avoid_cy, const LocEnv& env) {
	if (cx == avoid_cx && cy == avoid_cy) return false;
	int32_t r = invalid_loc(w, u, tcoord_of(cx), tcoord_of(cy), {true, depth > 1, false, true, false}, env);
	if (r == kLocWaterHazard && wcoord_of(cx) == wcoord_of(goal_cx) && wcoord_of(cy) == wcoord_of(goal_cy)) r = kLocValid;
	return r == kLocValid;
}

int32_t map_is_passable(const World& w, int32_t tx, int32_t ty) {
	if (!w.in_t(tx, ty)) return -1;
	return (w.w(tx >> 2, ty >> 2).flags & wflag::kImpassable) == 0 ? 1 : 0;
}

}  // namespace ron::sim
