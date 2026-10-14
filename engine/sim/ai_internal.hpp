// Shared helpers of the AI modules (not part of the public API).
#pragma once

#include "sim/ai.hpp"
#include "sim/production_internal.hpp"

namespace ron::sim::ai {

inline Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }
inline const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }
inline int32_t rnd_raw(Game& g) { return g.rng.get(0, 0xffff); }  // Random::get(game_random, 0, 0xffff)

// The truncating "/2^k" idiom (x + (x >> 31 & (2^k - 1))) >> k is plain C division by 2^k.
inline int32_t div4(int32_t x) { return x / 4; }
// corner_x / corner_y @adc3e0 / @adc3c0 (compass.obj): self and the four diagonals.
constexpr int8_t corner_x5[5] = {0, -1, 1, 1, -1};
constexpr int8_t corner_y5[5] = {0, -1, -1, 1, 1};

// WorldData::space_at_corner @6b27f0 (tcoord corner; the 4x4 block grid_index_x/y, grid_threes).
int32_t space_at_corner(const Game& g, int32_t tx, int32_t ty, int32_t who, int32_t city_only);
// WorldData::check_building_wcoord @6b26e0: the largest free square (0..4) at the wcoord, probing tcoord corners
// (dx, dy) in [-sx, sx] x [-sy, sy] with dx == 0, dy == 0 or |dx| + |dy| <= sum.
int32_t check_building_wcoord(const Game& g, int32_t wx, int32_t wy, int32_t who, int32_t sx, int32_t sy, int32_t sum,
                              int32_t city_only);
// World::gather_at(out, wx, wy, single) @6b07f0: resources per good (out[6]) of the wcoord (single != 0: its own land
// only) or of its 3x3 neighbourhood.
void gather_at(const Game& g, int32_t* out, int32_t wx, int32_t wy, int32_t single);
bool is_dock_tile(const Game& g, int32_t wx, int32_t wy);  // BuildTypeData::is_dock_tile @636700
int32_t num_coasts(const Game& g, int32_t r);              // Region::num_coasts @680760
int32_t find_fort(const Game& g, int32_t wx, int32_t wy, int32_t who);  // Forts::find_fort @73ebe0
// World.danger[who] at the danger cell of a world position (GameDaemon::calc_danger; zero until it is ported).
int32_t danger_at(const Game& g, int32_t who, int32_t x, int32_t y);
int32_t danger_at_w(const Game& g, int32_t who, int32_t wx, int32_t wy);  // (wy >> 1) * reg_xs + (wx >> 1)
// The order kind of a unit's current action (UnitData::get_action, vslot 0x10 get_type); 0 without one.
int32_t action_kind(const Unit& u);
// get_tregion of a world position (WData.region2 for the water half of a half-land wcoord).
int32_t tregion_at(const Game& g, int32_t x, int32_t y);

}  // namespace ron::sim::ai
