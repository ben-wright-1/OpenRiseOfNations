// Territory (docs/production-spec.md section 5.10): the per-region influence pass that writes WData.who / who2 and the
// leaders' reg_terr counters from cities and forts. Driven incrementally by GameDaemon::check_borders (cities.cpp,
// at most 256 wcoords per frame) and in one go by World::compute_all_territory at setup.
#pragma once

#include <cstdint>

namespace ron::sim {

class Game;

// World::compute_reg_territory(region, full) @6b0bb0. Processes land region `r` from its cursor Region.borders,
// one wcoord per step, while the per-frame budget (Game::check_borders_budget = GameDaemon.borders) allows; a
// negative budget is unlimited. `full` restarts the region (cursor 0). Returns 0 when the region was already finished
// on entry, 1 when the budget ran out first, 2 when the region is finished now.
int32_t compute_reg_territory(Game& g, int32_t r, int32_t full);

// World::compute_all_territory @6b5700: every land region from scratch with an unlimited budget, sea wcoords
// cleared to no owner, then each leader's territory / territory_high totals.
void compute_all_territory(Game& g);

}  // namespace ron::sim
