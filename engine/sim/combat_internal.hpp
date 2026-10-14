// Shared helpers of the combat modules (not part of the public API).
#pragma once

#include "sim/combat.hpp"
#include "sim/game.hpp"
#include "sim/placement.hpp"

namespace ron::sim {

// A rules constant by its rules-spec name (resolved once per call site).
#define RON_K(g, name)                                                 \
	([&]() -> int32_t {                                                \
		static const int ron_k_off_ = constant_offset(name);           \
		return ron_k_off_ >= 0 ? (g).rules->constants.at(uint32_t(ron_k_off_)) : 0; \
	}())

namespace combat_detail {

inline Unit* as_unit(Object* ob) { return ob && ob->is_unit ? static_cast<Unit*>(ob) : nullptr; }
inline const Unit* as_unit(const Object* ob) { return ob && ob->is_unit ? static_cast<const Unit*>(ob) : nullptr; }
inline Build* as_build(Object* ob) { return ob && !ob->is_unit ? static_cast<Build*>(ob) : nullptr; }
inline const Build* as_build(const Object* ob) { return ob && !ob->is_unit ? static_cast<const Build*>(ob) : nullptr; }
// The target slot (objects[whom][ox]) when it exists (alive or not).
inline Object* obj_at(Game& g, int32_t ox, int32_t whom) {
	return ox >= 0 && whom >= 0 && whom < kNumLeaders ? g.object(whom, ox) : nullptr;
}
inline const Object* obj_at(const Game& g, int32_t ox, int32_t whom) {
	return ox >= 0 && whom >= 0 && whom < kNumLeaders ? g.object(whom, ox) : nullptr;
}
inline bool alive_at(const Game& g, int32_t ox, int32_t whom) {
	const Object* t = obj_at(g, ox, whom);
	return t && t->alive();
}
// vslot 0x08 is_live_unit / 0x1c is_wall (every building) / 0xbc is_on_map.
inline bool is_live_unit(const Object& ob) { return ob.is_unit && ob.alive(); }
inline bool is_wall(const Object& ob) { return !ob.is_unit; }
inline bool on_map(const Object& ob) { return !ob.is_unit || static_cast<const Unit&>(ob).on_map(); }
inline bool has_objmask(const Object& ob, uint32_t m) { return ob.ptype && (uint32_t(ob.ptype->obj_masks) & m) != 0; }
inline int32_t domain(const Object& ob) { return ob.ptype ? ob.ptype->domain : 0; }
inline uint32_t ucentre(int32_t c) { return uint32_t(ucoord_of(c) * 48 + 24); }
inline int32_t uc(int32_t c) { return ucoord_of(c) * 48 + 24; }
inline uint32_t abs_angle(uint32_t d) { return d > 0x80000000u ? ~d : d; }
// flanking @92cfe0: 0 if d > 0xd5555555, 1 in [0x60000000, 0xa0000000], else 2.
inline int32_t flanking(uint32_t d) {
	if (d > 0xd5555555u) return 0;
	return (d >= 0x60000000u && d <= 0xa0000000u) ? 1 : 2;
}
inline bool unit_ai(const Unit& u) { return (u.unit_masks & umask::kAiControl) != 0; }
inline bool leader_human(const Game& g, int32_t who) {
	return who >= 0 && who < kNumLeaders && (g.leaders[size_t(who)].flags & leaderflag::kHuman) != 0;
}
// is_worker: types 50..53.
inline bool is_worker_type(int32_t t) { return t >= 50 && t <= 53; }

}  // namespace combat_detail
}  // namespace ron::sim
