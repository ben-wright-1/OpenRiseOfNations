// City capture, ownership transfer, capital loss, defeat and victory (docs/capture-spec.md).
//
// The entry points are Build::check_capture (combat.hpp declares it: called from Object::valid_target, the group
// attack code, Object::do_damage and the city's 64-frame poll in Build::process), Cities::capture_city and the
// leader-level elimination chain (City::close -> Leader::lost_a_city -> Leader::defeat -> Leader::defeat_by ->
// Game::check_victory). Every game_random draw happens inside functions other modules own (Objects::init_build's
// gather-tile shuffle, Build::activate's farm animals, Objects::init_unit's Guy::init_real, Unit::close's death
// animation); this module only orders the calls as the original does (capture-spec section 9).
#pragma once

#include <cstdint>

namespace ron::sim {

class Game;
struct Build;
struct Unit;

// Build::swap_team @628210 (with Wall::swap_team @640c00): a copy of the building for leader `who` (Objects::init_build
// preplaced, the state fields copied); the original stays alive (callers activate the copy and close the original).
// Returns the new o or < 0.
int32_t build_swap_team(Game& g, Build& b, int32_t who);
// Unit::swap_team @6118d0: the whole uber chain recreated for `who` (Objects::init_unit), the old chain closed (dt 0).
int32_t unit_swap_team(Game& g, Unit& u, int32_t who);

// Cities::capture_city(new_who, idx, old_who) @733380: city `idx` of `old_who` changes hands. Returns the new city index
// (or -1 when the city object could not be copied).
int32_t cities_capture_city(Game& g, int32_t new_who, int32_t idx, int32_t old_who);
// City::capture @736c40: fills the freshly created record (new_who, new_idx) from the old one.
void city_capture(Game& g, int32_t new_who, int32_t new_idx, int32_t old_who, int32_t old_idx, int32_t new_o);

// Leader::lost_a_city @6dc6d0 (called by City::close).
void leader_lost_a_city(Game& g, int32_t who, bool was_capital, bool own, int32_t attacker);
void leader_lost_capital(Game& g, int32_t who, int32_t who_new);  // Leader::lost_capital @6dc870
void leader_recapture_capital(Game& g, int32_t who);              // Leader::recapture_capital @6d1ba0
// Leader::defeat(type, attacker, flag) @6ecb00 and Leader::defeat_by(attacker, type) @6d1c80. DefeatIndex: 0 conquest,
// 1 capital, 2 capital sudden death, 3 sudden death, 4 musical chairs, 5 another leader's victory, 6 resigned,
// 7 disconnected, 8 Armageddon.
void leader_defeat(Game& g, int32_t who, int32_t type, int32_t attacker, int32_t flag);
void leader_defeat_by(Game& g, int32_t who, int32_t attacker, int32_t type);
// Leader::victory(type, flag) @6ec9b0 (type: 0 conquest, 1 wonders, 2 territory, 4 score, 5 economic, 6 time limit).
void leader_victory(Game& g, int32_t who, int32_t type, int32_t flag);
void game_check_victory(Game& g);  // Game::check_victory @5926b0
void game_defeat_all(Game& g);     // Game::defeat_all @592aa0 (Armageddon)
int32_t game_retake_capital(const Game& g, int32_t who);  // Game::retake_capital @594530 (frames)
// Leaders::process_all steps 4 and 5 (frame-spec 4): Leader::process_elimination @6b8a20 and the three clocks.
void leader_process_elimination(Game& g, int32_t who);
void leader_step_timers(Game& g, int32_t who);
// GameDaemon::process_victory @730ef0 (frame-spec 5 step 2).
void game_daemon_process_victory(Game& g);

// Build::process step 4 of a city object (production-spec 5.6): every 64 frames the nearest enemy land unit within
// 0xc00 tries Build::check_capture; the AI's damaged cities call a free citizen to repair. Returns true when the city
// was captured (Build::process stops for this object).
bool build_capture_poll(Game& g, Build& b);

}  // namespace ron::sim
