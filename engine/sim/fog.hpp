// Fog of war and line of sight (docs/fog-spec.md): the primitive grid writers of world.obj, every WorldData fog query,
// LOS radii (Unit::update_los, Wall::update_los, UnitData::los), the stamp (Object::update_seen, update_local_seen,
// World::reveal_fog, update_seen_ally), the refresh (GameDaemon::update_all_seen), building memory
// (Wall::check_ever_seen), the fog parts of the leader (ally_mask in Leader::plan_strategy, Leader::check_explore,
// Leader::new_rare, Leader::meet) and the per-object is_seen family.
//
// The grids live in World (seen, seen2, seen3, wcoord_seen, WData.was_seen), fog cells are 384 x 384 world units
// (fx = floor(x / 384)), and every "seen by p" query ANDs a grid byte with L[p].ally_mask. No game_random here.
// Stamps happen at the moment of the fog-cell change (never batched): readers later in the same pass see them.
#pragma once

#include <cstdint>

namespace ron::sim {

class Game;
class World;
struct Object;
struct Unit;
struct Build;
struct GoodObj;

// TypeIndex values the fog code tests (re/types/typeindex.tsv).
namespace fogtype {
constexpr int32_t kOil = 5, kFish = 6, kFurs = 21, kWhales = 31, kObsidian = 32, kDogs = 38;
constexpr int32_t kPeasants = 50, kPeasantsKorean = 51, kSpy = 58, kMerchant = 61, kMerchantDutch = 62, kScout = 69;
constexpr int32_t kTheCeo = 357, kPtolemy = 362, kFouche = 371, kFurTrapper = 400;
constexpr int32_t kBarracks = 427, kStable = 428, kAutoplant = 429, kTower = 439, kFort = 443;
constexpr int32_t kWonderFirst = 526, kWonderLast = 542, kColosseum = 530, kLibertyStatue = 537, kEiffelTower = 540,
                  kSpaceProgram = 542;
constexpr int32_t kWrittenWord = 551, kComputerization = 557;  // the science epoch line
constexpr int32_t kAllyLos = 688, kExploreMapBonus = 690, kActivePingBonus = 691, kRevealEnemyBonus = 692;
constexpr int32_t kFortLos2 = 730, kFortLos3 = 731, kFortLos4 = 732, kTroopsLos1 = 745, kTroopsLos3 = 747;
constexpr int32_t kGeneralsUpgrade1 = 773, kGeneralsUpgrade3 = 775, kSpiesUpgrade1 = 776, kSpiesUpgrade3 = 778;
}  // namespace fogtype

// floor(c / 384) (div_3_table[c >> 7]): the fog cell of a coordinate.
inline int32_t fcoord_of(int32_t c) {
	const int32_t v = c >> 7;
	return v >= 0 ? v / 3 : -((2 - v) / 3);
}

// ---------------------------------------------------------------- primitive writers (world.obj, fog-spec 2)
// World::set_seen @6b3c60: seen, seen3 (detect), seen2, WData.was_seen, wcoord_seen |= bit(who) (who < 0: 0xff).
// Returns true iff seen2 changed (the cell is newly explored by that owner).
bool fog_set_seen(World& w, int32_t fx, int32_t fy, int32_t who, bool detect);
// World::set_seen2 @6b4bb0: seen2 |= mask; unless explored_only also seen and wcoord_seen; always WData.was_seen.
bool fog_set_seen2(World& w, int32_t fx, int32_t fy, uint8_t mask, bool explored_only);
// World::set_was_seen @6b41d0: WData.was_seen and seen2 |= bit(who).
void fog_set_was_seen(World& w, int32_t fx, int32_t fy, int32_t who);
// World::clear_seen @6b2250 (seen, wcoord_seen) and the seen3 memset of update_all_seen / clear_seen3 @6b2140.
void fog_clear_seen(World& w);
void fog_clear_seen3(World& w);

// ---------------------------------------------------------------- queries (WorldData, fog-spec 7.1)
// who > 7 always sees; who < 0 (an invalid leader index in the original) is answered as "seen".
bool is_seen(const Game& g, int32_t fx, int32_t fy, int32_t who);           // @6b55c0: currently visible
bool is_really_seen(const Game& g, int32_t fx, int32_t fy, int32_t who);    // @6b42c0: is_seen without reveal_map
bool was_seen(const Game& g, int32_t fx, int32_t fy, int32_t who);          // @6b53f0: explored (territory, techs)
bool was_really_seen(const Game& g, int32_t fx, int32_t fy, int32_t who);   // @6b54f0: actually looked at
bool is_seen_wcoord(const Game& g, int32_t wx, int32_t wy, int32_t who);    // @6b2540 (no simulation callers)
bool was_seen_wcoord(const Game& g, int32_t wx, int32_t wy, int32_t who);   // @6b25f0 (renderer, mouse-over)
bool is_detected(const Game& g, int32_t fx, int32_t fy, int32_t who);       // @6b48c0: seen3 & ally_mask
bool is_detected_by_enemy(const Game& g, int32_t fx, int32_t fy, int32_t who);  // @6b50f0: seen3 & ~ally_mask

// ---------------------------------------------------------------- leader helpers
// LeaderData::has_tribe_bonus @6e1370.
bool has_tribe_bonus(const Game& g, int32_t who, int32_t bonus);
// LeaderData::has_preq @6db810 (production-spec 7.9.2; implemented in techs.cpp).
bool has_preq(const Game& g, int32_t who, int32_t type);
// LeaderData::has_wonder @6ebc10 != 0 (production-spec 5.13.3; cities.cpp).
bool has_wonder(const Game& g, int32_t who, int32_t type);
// Leader::meet @6e1250 -> treaty_on(p, 1): treaties bit 1 both ways (the "met" messages are UI).
void leader_meet(Game& g, int32_t who, int32_t p);
// Leader::new_rare @6d9e70.
void leader_new_rare(Game& g, int32_t who, int32_t good);
// Leader::check_explore @6bc860 (frame 0 and every 200 / ai_speed frames, staggered by who * 25 + 12).
void leader_check_explore(Game& g, int32_t who);
// The fog part of Leader::plan_strategy and Leaders::strategy_all are in sim/ai.hpp (ai_strategy.cpp).
// Leader::init's ally_mask rule (@6e3930): bit(who), plus every slot is_ally with ALLY_LOS or reveal_map != 0.
void leader_init_ally_mask(Game& g, int32_t who);

// ---------------------------------------------------------------- LOS (fog-spec 4)
int32_t unit_update_los(Game& g, Unit& u);         // Unit::update_los @60e4d0 (mylos)
int32_t build_update_los(Game& g, Build& b);       // Wall::update_los @63eeb0
int32_t object_update_los(Game& g, Object& ob);    // vslot +0x160 (gaia animals: Animal::update_los = 0)
int32_t object_los(const Game& g, const Object& ob);  // vslot +0x128: UnitData::los @6100c0 / WallData::los

// ---------------------------------------------------------------- stamping (fog-spec 3, 5, 6)
// The table range one Object::update_seen call stamps (fog-spec 3.2 steps 2-5), as a pure function of the LOS:
// `projected` = the small-LOS ground-unit case (the caller projects the centre 384 units ahead), `detector` = the
// DETECTOR flag. r < 0 (negative LOS) stamps nothing here (the original indexes before the tables).
struct StampRange {
	int32_t r = 0;           // radius in fog cells after the clamps
	bool ring = false;       // ring tables (rings()) instead of circles()
	int32_t start = 0, end = 0, detect_end = 0;  // table indices [start, end); index < detect_end also writes seen3
};
StampRange stamp_range(int32_t los, bool move, bool projected, bool detector);
// Object::update_seen @651b80 (vslot +0x174). move = the rim-only update after a one-fog-cell move.
void object_update_seen(Game& g, Object& ob, bool move);
// vslot +0x164: Unit::update_local_seen @60e410 / Wall::update_local_seen @63ed50.
void object_update_local_seen(Game& g, Object& ob);
// World::reveal_fog @6b3d30: side effects of the first exploration of a fog cell by `who` (o = the stamping object,
// -1 for reveal points).
void reveal_fog(Game& g, int32_t fx, int32_t fy, int32_t who, int32_t o);
// Object::update_seen_ally @6537a0 (starting cities): explored for the owner's share set.
void update_seen_ally(Game& g, Object& ob);
// GameDaemon::update_all_seen @732840: the refresh (frame % 100 == 33, and once before frame 0).
void update_all_seen(Game& g);
// Wall::check_ever_seen @63ce70.
void check_ever_seen(Game& g, Build& b, bool force);
// The wcoord object-list lookups reveal_fog uses: ObjectsData::find_good_at(w, who, 1, 0) @65bec0 (the list's good
// terminal, live and not oil) and find_goody_at(w) @65c040 (the item terminal, live). -1 if none.
int32_t find_good_at_wcoord(const Game& g, int32_t wx, int32_t wy);
int32_t find_goody_at_wcoord(const Game& g, int32_t wx, int32_t wy);
// Unit::get_goody_box @5f7690 (a one-unit group explores to the goody box wcoord).
void unit_get_goody_box(Game& g, Unit& u, int32_t wx, int32_t wy);

// ---------------------------------------------------------------- per-object visibility (fog-spec 7.2)
bool unit_is_cloaked(const Unit& u);                          // UnitData::is_cloaked @60a6a0
bool unit_is_detected(const Game& g, const Unit& u, int32_t who);  // UnitData::is_detected @60a630
// vslot +0x48 is_seen(who, ignore_cloak): UnitData @607a60 (gaia animals: AnimalData @5d8160, non-herd part),
// BuildData @62e1a0 (-> WallData @642bd0).
bool object_is_seen(const Game& g, const Object& ob, int32_t who, bool ignore_cloak);
// vslot +0x17c is_now_seen: BuildData @62e130 -> WallData @642ca0 (units: is_seen with the cloak test).
bool object_is_now_seen(const Game& g, const Object& ob, int32_t who);
bool good_is_seen(const Game& g, const GoodObj& gd, int32_t who);  // GoodData::is_seen @66e4c0
bool item_is_seen(const Game& g, const GoodObj& it, int32_t who);  // ItemData::is_seen @677850

}  // namespace ron::sim
