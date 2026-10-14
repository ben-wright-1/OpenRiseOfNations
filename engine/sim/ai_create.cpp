// The AI creators (docs/ai-spec.md 6.5, 6.6): Leader::create_units @6c40a0 and Leader::create_buildings @6c1be0,
// which score every trainable unit type and every placeable building type per city and fill the make list.
//
// Evidence (local, gitignored): re/decomp/ai/Leader_create_units@006c40a0.c, Leader_create_buildings@006c1be0.c and
// their disassembly (re/decomp/ai/asm), helpers in re/decomp/ai2 and re/decomp/ai_create. The spec sections were
// checked against the disassembly; the comments below name the spec steps.
#include <algorithm>

#include "sim/ai_internal.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"

namespace ron::sim {

using namespace ai;

namespace {

int32_t owned_w(const Game& g, int32_t who, int32_t w) {  // num_buildings[w] + its `to` chain
	if (!is_build_type(w)) return 0;
	const Leader& L = LD(g, who);
	int32_t n = L.num_buildings[size_t(w - kBuilds)];
	const int32_t to = g.type(w).to;
	if (to >= 0 && is_build_type(to)) n += get_buildings(g, who, to, L.num_buildings[size_t(to - kBuilds)]);
	return n;
}

int32_t reg_buildings_of(const Game& g, int32_t who, int32_t r, int32_t t) {  // LeaderData::get_reg_buildings @6d5470
	if (r >= 64 || r < 0 || !is_build_type(t)) return 0;
	int32_t n = uint16_t(LD(g, who).reg_buildings[size_t(r)][size_t(t - kBuilds)]);
	const int32_t to = g.type(t).to;
	if (to >= 0) n += reg_buildings_of(g, who, r, to);
	return n;
}

bool is_gather_enhancer(const Game& g, int32_t t) {
	const Type& T = g.type(t);
	return type_is(T, ptype::kGranary) || type_is(T, ptype::kLumberMill) || type_is(T, ptype::kSmelter) ||
	       type_is(T, ptype::kRefinery);
}
int32_t enhancing_good(int32_t t) {  // BuildTypeData::get_enhancing_good (exact types)
	switch (t) {
	case ptype::kGranary: return 0;
	case ptype::kLumberMill: return 1;
	case ptype::kSmelter: return 4;
	case ptype::kRefinery: return 5;
	default: return -1;
	}
}
bool is_defensive(const Game& g, int32_t t) {
	const Type& T = g.type(t);
	return type_is(T, ptype::kTower) || type_is(T, ptype::kFortX) || type_is(T, ptype::kAirbase) || T.attack != 0;
}
bool mil_trainer(const Game& g, int32_t t) { return is_military_trainer(g, t); }
bool is_peasant_t(int32_t t) { return t == 50 || t == 51; }
bool is_scholar_t(int32_t t) { return t == 52 || t == 53; }
bool is_merchant_t(int32_t t) { return t == 61 || t == 62 || t == 400; }
bool is_caravan_t(const Type& T) { return (T.unit_flags2 & 8) != 0; }

}  // namespace

// ---------------------------------------------------------------- create_units (6.5)

void create_units(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	const int32_t pt = std::max(L.pop_cap / 2, L.pop_cap - 40);
	int32_t e0 = L.epoch[0];
	const int32_t d = get_diff(g, who);
	const int32_t dcap = d < 2 ? 2 * d + 1 : std::min(10, 2 * d + 2);
	const int32_t civ = L.merchants + L.caras + L.scholars + L.peasants;
	const CategoryList* pl = g.rules->category_list("pop_limits");
	const int32_t pop_limit0 =
	    pl && size_t(g.info.pop_limit) < pl->items.size() ? pl->items[size_t(g.info.pop_limit)].data[0] : 200;
	bool first = true;
	auto& cities = g.cities[size_t(who)];
	for (int32_t c = 0; c < L.city_mark && size_t(c) < cities.size(); c++) {
		City& C = cities[size_t(c)];
		if (!C.valid()) continue;
		const int32_t co = C.o;
		const int32_t reg = C.reg & 63;
		int32_t army = -1;
		for (int32_t k = 0; k < 16; k++) {
			const Army& A = g.armies[size_t(who)][size_t(k)];
			if (A.valid && A.city == c && (A.status & armystat::kMustering)) {
				army = k;
				break;
			}
		}
		int32_t gs[6], fs[6];
		const int32_t slots = city_count_gather_slots(g, who, c, gs, fs);
		const bool under_attack = (C.city_flags & cityflag::kUnderAttack) != 0;
		for (int32_t t = kUnits; t < 402; t++) {
			// 6.5.2 gates
			if (L.effective_pop > L.pop_cap) continue;
			if (type_avail(g, who, t, 1) < 4) continue;
			const Type& T = g.type(t);
			const int32_t w = T.where;
			if (w < 0 || owned_w(g, who, w) == 0 || (T.unit_flags & 0x8000)) continue;
			const bool lw = type_is(g.type(w), ptype::kFortX) || (mil_trainer(g, w) && !type_is(g.type(w), ptype::kVillage));
			if (lw) {
				if (!first && T.domain != 1) continue;
			} else if (!city_could_queue(g, who, c, t)) {
				continue;
			}
			const int32_t have = L.num_queued[size_t(t)] + L.num_units[size_t(t - kUnits)];
			const int32_t haveL = get_queued(g, who, t, L.num_queued[size_t(t)]) + get_units(g, who, t, 0);
			int32_t cap = -1, left = 5;
			const bool siege = (T.unit_flags & 0x20000) != 0;
			bool capped3 = true;
			if (siege) cap = std::min({L.city_num, dcap, e0});
			else if (T.unit_flags & 4) cap = std::min(L.city_num, 6);
			else if (T.unit_flags & 8) cap = std::min(L.city_num, 10);
			else {
				capped3 = false;
				cap = -1;
				if (T.role & 0x10000) cap = d == 1 ? 7 : d == 0 ? 3 : -1;
			}
			if (capped3) {
				if (d < 2) cap = 2;
				if (!(L.non_siege - 5 * have - L.sea_combat >= 10)) continue;
			}
			if (type_is(T, ptype::kAaGun)) cap = cap / 2;
			if (cap > 0) {
				if (have >= cap) continue;
				left = cap - have;
			}
			int32_t P = (L.pop * 1000) / std::max(1, L.city_num);
			int32_t esc = 0;
			// 6.5.3 pacing and the draw
			const int32_t N = L.non_siege - L.sea_combat;
			if (T.role & 0x10000) {
				if (g.info.rush_rules == 8) continue;
				if (d == 0 && !(N <= 3 * L.city_num && N < 2 * e0 + 3 && N < 10)) continue;
				if (d == 1 && !(N <= 5 * L.city_num && N < (7 * (e0 + 1)) / 2 + 1 && N < 25)) continue;
				if (d == 2 && !(N <= 6 * L.city_num && N < 5 * e0 + 1)) continue;
				if (L.wonder_mod != 0) P /= 10;
				P = P * (T.domain == 0 ? L.ground_mod : T.domain == 1 ? L.sea_mod : L.air_mod) / 256;
				if (d != 2) {
					int32_t u = unit_prod_value(g, who, t);
					if (d == 0 || d >= 3) u = (3 * u) / 2;
					u = ((rnd_raw(g) % 3 + 4) * u) / 5;
					if (d < 2) u = -u;
					u = std::clamp(u, -256, 256);
					P = ((u + 256) * P) / 256;
				}
			}
			if (!(T.role & 0x200) && !is_caravan_t(T) && !(T.role & 0x10000)) {
				if (L.effective_pop > L.pop_cap - 4) P /= 2;
				if (L.effective_pop > L.pop_cap - 2) P /= 2;
				if (L.effective_pop > L.pop_cap - 1) continue;
			}
			int32_t D = 4;
			if (w == ptype::kStable) D = has_tribe_bonus(g, who, 0x13) ? 5 : 3;
			else if (w == ptype::kSiegeFactory) D = 2;
			else if (w == ptype::kDock) D = 2;
			else if (w == ptype::kBarracks) D = 3 * L.pop_cap < 25 ? 4 : (3 * L.pop_cap) / 5;
			if (T.unit_flags & 8) D = 2;
			if (T.unit_flags & 4) D = 1;
			const int32_t Dc = 2 * L.pop_cap < 25 ? 4 : (2 * L.pop_cap) / 5;
			int32_t V = 0, cat = 6, b = 1;
			// 6.5.4 branches
			if (T.role & 0x40000) {
				if (T.role & 0x10000) {  // land army
					if (g.info.rush_rules == 8 || (g.info.rush_rules < 8 && leaders_max_age(g) < g.info.rush_rules - 2))
						continue;
					const int32_t E = g.ctw ? e0 + 2 : e0;
					if (N < 3 * e0 && !siege) {
						P *= 100;
						esc = 1;
					}
					if (!lw && !under_attack) {
						e0 = E;
						if (g.frame < 2000 / g.ai_speed) continue;
						if (L.pop_cap < 24) P /= 4;
						else if (L.pop_cap < 36) P /= 2;
					}
					if (L.reg_wars[size_t(reg)]) V = L.active_wars ? (P = 2 * P) : (3 * P) / 2;
					else if (L.reg_neutrals[size_t(reg)] || L.active_wars) V = (3 * P) / 2;
					else if (L.wars) V = (11 * P) / 10;
					else V = P;
					const int32_t s = L.strategy[size_t(reg)];
					if ((s & 4) || L.reg_attacked[size_t(reg)]) {
						V *= 2;
						if (!lw && under_attack) V *= 2;
					} else if (s & 2) {
						V = (3 * V) / 2;
					}
					V = ((T.age + 8) * V) / 8;
					const int32_t Cc = L.combat;
					if (Cc < pt / 2) V *= 6;
					else if (Cc < (4 * pt) / 5) V *= 2;
					if (Cc > (5 * pt) / 4) V = (2 * V) / 3;
					if (Cc > (3 * pt) / 2) V /= 2;
					if (Cc > 2 * pt) V /= 2;
					if (uint32_t(L.reg_combat[size_t(reg)]) >
					    uint32_t(2 * L.reg_pop[size_t(reg)] * (L.reg_wars[size_t(reg)] + 1)))
						V /= 2;
					if (siege) {
						if (L.non_siege >= 3) V *= 2;
						if (L.siege < L.non_siege / 8) V *= 2;
					} else if (L.siege < dcap) {
						if (L.siege < L.non_siege / 20) V /= 2;
						else if (L.siege < L.non_siege / 10) V = (2 * V) / 3;
					}
					if (!lw && army < 0 && !under_attack && T.domain == 0) V = (3 * V) / 4;
					b = T.control_cost != 0 ? 10 / T.control_cost : 1;
					if (b < 1 || left < 1) {
						b = 1;
					} else {
						b = std::min(b, left);
						while (b > 1 && L.effective_pop >= L.pop_cap - b - 1) b--;
					}
					e0 = E;
				} else if (T.role & 0x10) {  // explorers
					if (g.info.elimination == 1) {
						int32_t cc = -1, cw = -1;
						find_capital(g, who, &cc, &cw, -1, -1);
						if (cw != who) continue;
					}
					if (have > 1) continue;
					V = 10000 / std::max(1, g.frame / 1000);
					esc = have == 0 ? 1 : 0;
					b = 1;
				} else {  // civilians
					if (g.info.elimination == 1) {
						int32_t cc = -1, cw = -1;
						find_capital(g, who, &cc, &cw, -1, -1);
						if (cw != who) continue;
					}
					if (!lw && under_attack && L.city_num > 1) continue;
					D = Dc;
					if (T.role & 0x200) {
						if (lw) continue;
						int32_t B = L.infra_mod * P / 256;
						if (is_peasant_t(t)) {
							cat = 5;
							const Build* cb = g.build(who, co);
							const int32_t q = cb ? build_count_queue(g, *cb, 0, ptype::kPeasants) : 0;
							if (!(L.reg_free_peasants[size_t(reg)] < uint16_t(L.reg_cities[size_t(reg)]))) continue;
							if (C.busy == 0 && C.free == 0 && q == 0) B *= 30;
							const int32_t nh = C.free + C.busy + q;
							if (nh < slots - 1) {
								int32_t room = slots - C.free - C.busy - q, useful = 0;
								for (int32_t k = 0; k < 6; k++) {
									if (k == 3 || type_avail(g, who, k, 1) == 0) continue;
									const int32_t cp = get_mod_resource_cap(g, who, k);
									if (L.income[size_t(k)] >= cp) room -= fs[k];
									else useful += std::min((cp - L.income[size_t(k)]) / 160, fs[k]);
								}
								const int32_t n = std::max(0, std::min(useful, room));
								esc = 1;
								V = n * n * B * 80;
								b = n;
								if (n == 0) {
									if (C.gatherers != 0 || C.busy != 0 || C.free != 0) continue;
									if (q != 0) continue;
								}
							} else {
								if (!(nh < slots || C.free + C.busy <= 2)) continue;
								V = B;
								b = 1;
							}
							if (C.free + q >= 2 && slots == 0) continue;
							if (g.info.victory == 8) V *= 10;
							if (L.reg_gatherers[size_t(reg)] < L.reg_gather_slots[size_t(reg)]) V = (3 * V) / 2;
							if (2 * L.reg_gatherers[size_t(reg)] < L.reg_gather_slots[size_t(reg)]) V *= 2;
							if (C.gatherers == 0) V *= 2;
							if (C.free + C.busy <= slots / 2) V = (3 * V) / 2;
							if (L.reg_peasants[size_t(reg)] == 0) V *= 5;
							if (L.reg_free_peasants[size_t(reg)] == 0) {
								V *= 2;
								if (type_avail(g, who, ptype::kTown, 1) == 4 && L.reg_land[size_t(reg)] < 2) V *= 2;
							}
							if (have > L.pop_cap / 2) V /= 2;
							if (have > 40) V /= 2;
						} else if (is_scholar_t(t)) {
							cat = 4;
							if (g.info.starting_resources == 8 || fs[3] == 0) continue;
							if (L.bucket[3] > ((L.resource_cap[0] / 16) * 3) / 2) continue;  // raw food cap (literal)
							const int32_t uo = city_get_building(g, who, c, ptype::kUniversity);
							const Build* ub = uo >= 0 ? g.build(who, uo) : nullptr;
							if (!ub || !ub->alive()) continue;
							const int32_t ns = fs[3] - build_count_queue(g, *ub, 1, ptype::kScholars);
							if (ns <= 0) continue;
							V = L.infra_mod * ns * 10000 / 256;
							const int32_t F = L.filled_gather_slots[3], G = L.gather_slots[3];
							if (F < G) V *= 60;
							if (F < (2 * G) / 3) {
								V *= 10;
								esc = 1;
							}
							if (F < G) V *= 5;
							b = ns;
						} else {
							continue;
						}
					} else if (is_caravan_t(T)) {
						cat = 4;
						if (g.info.starting_resources == 8 || type_is(T, 318) || lw) continue;
						V = L.infra_mod * 1000000 / 256;
						const int32_t e = L.econ[2];
						if (e & 1) {
							V *= 120;
							b = 3;
						} else if (e & 2) {
							V *= 60;
							b = 2;
						} else {
							b = 1;
							if (e & 4) V *= 30;
						}
						b = std::min(b, get_caravan_limit(g, who, 1) - have);
						if (b < 1) continue;
						esc = 1;
					} else if (is_merchant_t(t)) {
						cat = 4;
						b = 1;
						const int32_t mo = city_get_building(g, who, c, ptype::kMarket);
						const Build* mb = mo >= 0 ? g.build(who, mo) : nullptr;
						if (!mb || !mb->alive()) continue;
						if (!(has_tech(g, who, 552) || type_avail(g, who, 3, 1) != 0)) continue;
						if (!(L.known_rares - have > 0 && L.effective_pop < L.pop_cap - 1)) continue;
						V = 1000000;
						esc = 1;
					} else if (!(T.unit_flags2 & 0x60)) {
						if (!type_is(T, ptype::kSpy)) continue;
						b = 1;
						if (!lw || L.combat < 20 || get_diff(g, who) <= 2) continue;
						V = left * 2000;
						const int32_t h = get_units(g, who, t, 0) + L.num_queued[size_t(t)];
						if (h == 0) {
							esc = 1;
							V *= 100;
						}
						if (15 * h > L.combat) continue;
					} else {
						b = 1;
						if (!lw || L.combat < 8) continue;
						V = L.combat * left * 2000;
						const int32_t h = get_units(g, who, t, 0) + L.num_queued[size_t(t)];
						if (h == 0) {
							esc = 1;
							if (T.unit_flags2 & 0x40) V *= 10;
						}
						if (8 * h > L.combat) continue;
					}
				}
			} else if (T.role & 0x80000) {  // navy
				if (g.frame < 1000 / g.ai_speed) continue;
				const int32_t dk = objects_find_building(g, C.x, C.y, search::kFriendly, who, 0x1800, 0, filter::kBaseType,
				                                         ptype::kDock, 0);
				const Build* dock = dk >= 0 ? g.build(who, dk) : nullptr;
				if (!dock || !(dock->flags & objflag::kActive)) continue;
				// Wall::tile_corner, then the first water tcoord of the dock footprint (x outer, y inner).
				const Type& DT = *dock->ptype;
				const int32_t cxt = tcoord_of(dock->x) * 0xc0 + ((DT.x_size & 1) ? 0x60 : 0);
				const int32_t cyt = tcoord_of(dock->y) * 0xc0 + ((DT.y_size & 1) ? 0x60 : 0);
				const int32_t tx0 = tcoord_of(cxt) - (DT.x_size >> 1), ty0 = tcoord_of(cyt) - (DT.y_size >> 1);
				int32_t sr = -1, fx = 0, fy = 0;
				for (int32_t x = tx0; x < tx0 + DT.x_size; x++) {
					for (int32_t y = ty0; y < ty0 + DT.y_size; y++) {
						if (!g.world.in_t(x, y)) continue;
						if ((g.world.t(x, y) & 0x30) == 0x20) {
							sr = g.world.get_tregion(x, y);
							fx = x;
							fy = y;
							break;
						}
					}
					if (sr >= 64) break;
				}
				if (sr < 63 || sr >= Regions::kNum || g.regions.r[size_t(sr)].size < 20) continue;
				const int32_t hL = haveL;
				const int32_t sea_map = g.world.sea_map;
				const int32_t rc = L.reg_combat[size_t(sr)];
				if (sea_map < 3 && type_is(T, ptype::kBark) && rc >= 2 * sea_map) continue;
				if (T.role & 0x8000) continue;  // transports are never queued here
				const bool dock_attacked = (dock->build_masks & bmask::kUnderAttack) != 0;
				if (T.role & 0x10000) {  // warships
					b = 1;
					if (sea_map <= 1) continue;
					if (sea_map == 2 && !type_is(T, ptype::kBark)) continue;
					if (e0 >= 5 && rc > L.combat / 3) continue;
					if (rc > L.combat / 2) continue;
					int32_t k = 0, S = 0, M = 0;
					for (int32_t j = 0; j < 8; j++) {
						if (!(LD(g, j).flags & leaderflag::kValid) || is_ally(g, j, who)) continue;
						k++;
						const int32_t v = LD(g, j).reg_combat[size_t(sr)];
						S += v;
						M = std::max(M, v);
					}
					const int32_t A = k > 0 ? S / k : 0;
					if (rc > A + 5 || rc > M + 3) continue;
					const int32_t wx = 192 * fx + 96, wy = 192 * fy + 96;
					// (both searches run before the test; find_city answers the nearest friendly city of any friendly
					// leader and its index goes to init_navy as an own-city index: original quirk)
					const int32_t na = armies_find_army(g, who, wx, wy, -1, -1);
					const int32_t c2 = objects_find_city(g, wx, wy, search::kFriendly, who, 0, filter::kAll, 0, 0);
					if (na < 0 && c2 >= 0) armies_init_navy(g, who, c2, sr);
					if (sea_map <= 2) {
						if (rc > g.regions.r[size_t(reg)].size / 20 && A == 0) continue;
						if (rc > 1) continue;
						V = sea_map * (A + 1) * ((10 * P) / (hL + 1));
					} else {
						int32_t W = (500 * P) / (hL + 1);
						bool done = false;
						if (type_is(T, ptype::kCarrier)) {
							if (hL > 1 || rc < 8 || rc < A) continue;
							esc = 1;
						} else {
							esc = hL < e0 + 3 ? 1 : 0;
							if (A > rc) W = type_is(T, ptype::kBark) ? W / 10 : 10 * W;
						}
						if (dock_attacked) {
							esc = 1;
							if (type_is(T, 336)) {  // FIRESHIP
								V = sea_map * (A + 1) * (100 * W);
								done = true;
							} else {
								W = W / 1000;
							}
						}
						if (!done) V = sea_map * (A + 1) * W;
					}
				} else {  // fishing boats
					if (!type_is(T, 317)) continue;  // FISHERMEN (MERCHANTFLEET and other non-combat ships: skipped)
					cat = 4;
					b = 1;
					if (dock_attacked) continue;
					if (rc == 0 && e0 >= 3) continue;
					int32_t F = (100 * P) / (hL + 1);
					if (L.idle_fishermen != 0) continue;
					if (hL >= L.epoch[2] && hL >= 4) continue;
					if (rc < 10) F = F / (10 - rc);
					V = sea_map * F;
				}
			} else if ((T.role & 0x11000) == 0x11000) {  // air and missiles
				cat = 6;
				b = 1;
				if (T.obj_masks & 0x8000000) {  // missiles
					if (type_is(T, ptype::kNuke)) {
						if (get_diff(g, who) < 2) continue;
						const int32_t base = L.wars ? 400000 : 200000;
						const int32_t n = get_nukes(g, who);
						if (n + g.armageddon >= get_armageddon(g) - 1) continue;
						bool found = false;
						int32_t M = 0;
						for (int32_t j = 0; j < 8; j++) {
							if (!(LD(g, j).flags & leaderflag::kValid) || is_ally(g, who, j)) continue;
							if (has_preq(g, j, 693) || researching(g, j, 579, -1, false, false)) continue;
							found = true;
							M = std::max(M, get_nukes(g, j));
						}
						if (!found) continue;
						if (n >= L.num_buildings[size_t(w - kBuilds)]) continue;
						if (M > n) {
							V = (M - n) * base;
							esc = 1;
						} else {
							V = base;
						}
						if (can_pay_cost(g, t, who, -1, -1, 1) != 0) esc = 1;
						if (n == 0) V *= 5;
						V = (L.pers.nukes + 2) * V;
						if (L.pers.nukes > 0) V *= 2;
						if (L.pers.rush < 0 && L.pers.army_size > 0) V *= 2;
					} else {  // cruise missiles
						if (has_preq(g, who, ptype::kNuke)) continue;
						V = haveL ? 600000 / (haveL + 1) : (L.pers.air > 0 ? 1200000 : 600000);
						if (L.pers.nukes < 0) V *= 2;
						esc = 0;
					}
				} else {  // aircraft
					if (L.air >= 8 * L.num_buildings[size_t(ptype::kAirbase - kBuilds)]) continue;
					left = d < 2 ? 2 : d == 2 ? 4 : 16;
					if (haveL >= left) continue;
					int32_t P2 = P;
					if (L.reg_wars[size_t(reg)]) V = L.active_wars ? (P2 = 2 * P2) : (3 * P2) / 2;
					else if (L.reg_neutrals[size_t(reg)] || L.active_wars) V = (3 * P2) / 2;
					else if (L.wars) V = (11 * P2) / 10;
					else V = P2;
					const int32_t s = L.strategy[size_t(reg)];
					if ((s & 4) || L.reg_attacked[size_t(reg)]) V *= 2;
					else if (s & 2) V = (3 * V) / 2;
					V = (T.attack * V) / 10;
					if (type_is(T, ptype::kBomber)) {
						if (L.fighters != 0) {
							if (L.bombers < L.fighters / 4) V *= 2;
							if (L.bombers < L.fighters / 8) V *= 2;
						}
						if (L.fighters >= 8 && L.bombers < 3) V *= 2;
					}
					const int32_t Cc = L.combat;
					if (Cc < pt / 2) V *= 2;
					else if (Cc < (4 * pt) / 5) V = (3 * V) / 2;
					if (Cc > (5 * pt) / 4) V = (2 * V) / 3;
					if (Cc > (3 * pt) / 2) V /= 2;
					if (Cc > 2 * pt) V /= 2;
					if (uint32_t(L.reg_combat[size_t(reg)]) > uint32_t(2 * L.reg_pop[size_t(reg)] * (L.reg_wars[size_t(reg)] + 1)))
						V /= 2;
					b = T.control_cost != 0 ? 10 / T.control_cost : 1;
					if (b < 1) {
						b = 1;
					} else {
						b = std::min(b, left);
						while (b > 1 && L.effective_pop >= L.pop_cap - b - 1) b--;
					}
				}
			} else {
				continue;
			}
			// 6.5.5 common tail
			if (cap >= 0 && !type_is(T, ptype::kMachineGun)) {
				V = 10 * left * V;
				if (L.num_units[size_t(t - kUnits)] == 0 && L.city_num > 2) V *= 100;
			}
			int32_t num = std::min(b, left);
			if (is_peasant_t(t) || is_scholar_t(t) || is_caravan_t(T) || is_merchant_t(t)) {
				if (civ >= 50 && civ > L.pop_cap / 2) V /= 100;
				if (civ >= (3 * pop_limit0) / 5) continue;
			}
			const int32_t W = (D * V) / (D + have);
			int32_t n = 0;
			int32_t m = check_income(g, who, t, 0x400, co, esc, -1, num, &n);
			if (n > 0 && n < num) {
				m = check_income(g, who, t, 0x400, co, esc, -1, n, nullptr);
				num = n;
			}
			V = (m * W) / 256;
			if (V < 0) V = 9999999;
			if (L.wonder_mod != 0) V /= 2;
			make_me(g, who, t, V, esc, cat, c, 0, num, 0, 0);
		}
		first = false;
	}
}

// ---------------------------------------------------------------- create_buildings (6.6)

void create_buildings(Game& g, int32_t who) {
	Leader& L = LD(g, who);
	auto& cities = g.cities[size_t(who)];
	const int32_t d = get_diff(g, who);
	for (int32_t pass = 0; pass < 2; pass++) {
		for (int32_t c = 0; c < L.city_mark && size_t(c) < cities.size(); c++) {
			City& C = cities[size_t(c)];
			if (!C.valid()) continue;
			const Build* cb = g.build(who, C.o);
			if (!cb) continue;
			if (cb->city >= 0 && g.cities[size_t(cb->who)][size_t(cb->city)].race != cb->who &&
			    !(!(cb->flags & objflag::kCity) && (cb->ptype->build_flags & 0x10)))
				continue;
			const int32_t reg = C.reg & 63;
			const int32_t level = cb->type == ptype::kTown ? 2 : (cb->type == ptype::kMetropolis || cb->type == ptype::kForbiddenCity) ? 3 : 1;
			const int32_t nb = city_num_buildings(g, who, c);
			const int32_t fl = int32_t(C.land) - int32_t(C.filled);
			const int32_t fo = int32_t(C.ocean) - int32_t(C.ocean_filled);
			if (C.space[0] == 0 && fo == 0) continue;
			int32_t gs[6], fs[6];
			const int32_t gs_total = city_count_gather_slots(g, who, c, gs, fs);
			const bool attacked = (C.city_flags & cityflag::kUnderAttack) != 0;
			for (int32_t t = kBuilds; t < 543; t++) {
				const Type& T = g.type(t);
				// 6.6.2 gates
				if (type_is(T, ptype::kVillage)) continue;
				if (((T.build_flags & 0x40) != 0) != (pass == 1)) continue;
				if (type_avail(g, who, t, 1) < 4) continue;
				if (g.info.elimination == 1) {
					int32_t cc = -1, cw = -1;
					find_capital(g, who, &cc, &cw, -1, -1);
					if (cw != who && !mil_trainer(g, t)) continue;
				}
				const int32_t sz = std::clamp(std::max(T.x_size, T.y_size) - 2, 0, 2);
				const int32_t sp = C.space[size_t(sz)];
				if (T.domain == 1) {
					if (fo == 0) continue;
					if (type_is(T, ptype::kDock) && C.dock_tile == 0) continue;
				} else if (sp == 0) {
					continue;
				}
				int32_t cat = 8, esc = 0, up = 0, k = 4;
				if (T.from != -1 && !(T.build_flags & 4)) {
					up = 1;
					if (city_count_buildings(g, who, c, T.where, 1, 1) == 0) continue;
				}
				if (T.domain == 1 && !type_is(T, ptype::kDock)) continue;
				if (g.info.rush_rules == 8 && type_is(T, ptype::kLookout)) continue;
				if ((T.build_flags & 0x200) && city_count_buildings(g, who, c, t, 0, 0) != 0) continue;
				if (type_is(T, ptype::kUniversity) && nb < 3) continue;
				if (type_is(T, ptype::kLibrary) && owned_w(g, who, ptype::kLibrary) != 0 && nb < 3) continue;
				if (type_is(T, ptype::kSenate) && owned_w(g, who, ptype::kSenate) + get_queued(g, who, ptype::kSenate, L.num_queued[size_t(ptype::kSenate)]) > 0)
					continue;
				const int32_t q_t = get_queued(g, who, t, L.num_queued[size_t(t)]);
				if (!(T.build_flags & 0x40) && !is_gather_enhancer(g, t) && T.attack == 0 && !type_is(T, ptype::kTemple) &&
				    owned_w(g, who, t) + q_t >= 3)
					continue;
				const int32_t cnt = city_count_buildings(g, who, c, t, 1, 0);
				if (t == ptype::kFarm && cnt >= get_farm_limit(g, who)) continue;
				int32_t V = 0;
				if (pass == 1) {
					// 6.6.3 gatherers
					k = 2;
					if (g.info.starting_resources == 8) continue;
					const int32_t gd = building_good(t);
					if (gd < 0) continue;
					if (L.filled_gather_slots[size_t(gd)] < (3 * L.gather_slots[size_t(gd)]) / 4) continue;
					int32_t B = ((L.pop * 7000) / std::max(1, L.city_num + L.village_num) + level * 3000 + fl * 4000) *
					            L.infra_mod / 256;
					if (g.info.victory == 8) B *= 100;
					if (L.num_buildings[size_t(t - kBuilds)] == 0) B *= 5;
					if (type_is(T, ptype::kUniversity)) {
						const int32_t nu = L.num_buildings[size_t(ptype::kUniversity - kBuilds)];
						if (nu == 0 || L.scholars > 3 * nu) B *= 10;
						else B /= 10;
					}
					if (cb->city_down < 0) {
						B *= 10;
					} else {
						if (level == 1 && has_tech(g, who, ptype::kTown) && nb < K("city_buildings") + 1) B *= 2;
						if (level == 2 && has_tech(g, who, ptype::kMetropolis) && nb < K("metro_buildings") + 1) B *= 2;
					}
					if (C.city_flags & cityflag::kCapital) B /= 2;
					cat = 4;
					bool found = false;
					int32_t Mbest = 256;
					const int32_t rf = L.reg_free_peasants[size_t(reg)];
					if (rf == 0 && L.pop_cap - L.control <= 1) continue;
					const int32_t wg = L.worst_good;
					if (rf > 2 && L.rate[size_t(wg)] < get_mod_resource_cap(g, who, wg) / 32) esc = 1;
					for (int32_t gg = 0; gg < 6; gg++) {
						if (gg != gd) continue;
						if (type_avail(g, who, gd, 1) == 0 || L.income[size_t(gd)] >= get_mod_resource_cap(g, who, gd) ||
						    fs[gd] >= 2)
							continue;
						bool oil = false;
						int32_t m = 256;
						if (gd == 5) {
							oil = c == 0 && int32_t(L.oil_patches.size()) > L.gather_slots[5];
							B *= 10;
						}
						const int32_t ter = C.ter[size_t(gd)];
						const int32_t lg = g.type(gd).largest_gather;
						const int32_t e = L.econ[size_t(gd)];
						if (gd == wg) {
							if (ter == 0 && !oil) continue;
							m = gd == 5 ? 0x800 : (std::max(3 - lg, ter) + 2) * 256;
						} else {
							if (ter == 0 && !oil) continue;
							if ((gd == 0 || gd == 1 || gd == 2) && L.city_num + L.village_num < 5) m = 512;
							if (up == 0) {
								if (sp < 2) continue;
								if (sp < 3 && !(e & 2)) m >>= 1;
								if (sp < 4 && ter < 2 && gd != 5) m >>= 1;
							}
							const int32_t dv = (e & 8) ? 4 : (e & 2) ? 2 : 3;
							m = ((dv + std::max(3 - lg, ter)) * m) / dv;
						}
						found = true;
						if (L.rate[size_t(gd)] < 20 * C.pop) m *= 2;
						if (gd == wg) m *= 2;
						if (gd == L.best_good) m /= 2;
						if (e & 8) m /= 2;
						if (ter > 1 || gd == 5) {
							if (e & 2) m *= 2;
							if (e & 1) m *= 2;
							if (e & 4) m = (3 * m) / 2;
						}
						if (gd == 0) m *= 2;
						const int32_t fb = C.free + C.busy;
						if (gs_total > 2 * fb) m = (2 * m) / 3;
						if (gs_total > 3 * fb) m = (2 * m) / 3;
						if (C.free != 0) {
							if (gs_total < C.free + C.gatherers) m = (4 * m) / 3;
							if (gs_total == 0) m *= 3;
						}
						if (!(e & 8) && (ter > 1 || gd == 5) && L.city_num + L.village_num > 1 &&
						    city_count_buildings(g, who, c, t, 0, 1) == 0)
							m = (3 * m) / 2;
						Mbest = std::min(Mbest, m);
					}
					if (!found) continue;
					V = B * Mbest / 256;
					if (V < 0) V = 9999999;
				} else {
					// 6.6.4 base value and special classes
					V = level * 1000;
					if (uint32_t(L.reg_buildings[size_t(reg)][size_t(t - kBuilds)] + q_t) >= uint32_t(uint16_t(L.reg_cities[size_t(reg)])))
						V = V / (uint16_t(L.reg_cities[size_t(reg)]) + 1);
					if (type_is(T, ptype::kTemple, true)) {
						if (!((C.city_flags & cityflag::kCapital) || d > 2)) continue;
						V *= 500;
						esc = 1;
						if (L.num_buildings[size_t(t - kBuilds)] == 0) V *= 20;
						if (C.bordering) V *= 100;
						if (C.city_flags & cityflag::kVeryNearBorder) {
							V *= 2;
							if (!attacked) V *= 5;
						}
					}
					if (type_is(T, ptype::kLookout)) {
						if (L.epoch[3] <= 3) continue;
						if (L.age < 4) {
							if (cnt != 0) continue;
							V *= 100;
							esc = 1;
						} else {
							if (cnt < 2) {
								V *= 100;
								esc = 1;
							}
							if (cnt >= 3) continue;
						}
					}
					if (is_wonder_type(t)) {
						if (g.ctw || city_num_wonders(g, who, c, 1) != 0 || (g.info.starting_resources == 7 && L.city_num < 2))
							continue;
						bool ok = true, skip = false;
						for (int32_t j = 0; j < 8 && !skip; j++) {
							const Leader& J = LD(g, j);
							if (!(J.flags & leaderflag::kActive)) continue;
							if (is_ally(g, j, who)) {
								if (J.num_queued[size_t(t)] != 0) skip = true;
							} else if (J.num_queued[size_t(t)] != 0) {
								for (const Unbuilt& ub : g.unbuilt_wonders[size_t(j)]) {
									const Build* wb = ub.o >= 0 ? g.build(j, ub.o) : nullptr;
									if (!wb || !type_is(*wb->ptype, t, true)) continue;
									const float r = float(double(uint32_t(wb->job_counter))) /
									                float(double(uint32_t(construct_time(g, *wb, 0))));
									if (r > 0.25f) {
										ok = false;
										break;
									}
								}
							}
						}
						if (skip || !ok) continue;
						int32_t W;
						if (L.wonder_mod != 0) {
							W = 100000000;
							esc = 1;
						} else {
							if (d < 2) {
								if (g.info.victory == 6) {
									if (get_team_wonder_value(g, who) >= get_enemy_wonder_value(g, who) + 1) continue;
								} else if (L.wonder_mark != 0) {
									continue;
								}
							}
							const int32_t ww = wonder_winning(g);
							const int32_t E = get_enemy_wonder_value(g, who) + get_enemy_unbuilt_wonder_value(g, who);
							const CategoryList* wl = g.rules->category_list("wonderwins");
							const int32_t target =
							    wl && size_t(g.info.wonderwin) < wl->items.size() ? wl->items[size_t(g.info.wonderwin)].data[0] : 0;
							if (g.info.victory == 6) {
								W = (E + 1) * 100000;
								esc = 1;
							} else if ((ww >= 0 && ww != who) || E >= target / 2 || E > 4) {
								W = 2000000;
								esc = 1;
							} else {
								const int32_t r1 = rnd_raw(g) % 1000;
								const int32_t r2 = rnd_raw(g) % 300;
								W = int32_t(uint32_t(get_team_wonder_value(g, who) + 1) * uint32_t(E + 1) * uint32_t(r2) *
								            uint32_t(r1));
							}
							if (d == 0) W /= 100;
							else if (d == 1) W /= 10;
						}
						V = int32_t(uint32_t(city_level(g, who, c)) * uint32_t(T.wonder_val) * uint32_t(W));
					} else if (L.wonder_mod != 0 && !(T.build_flags & 0x40)) {
						V /= 100;
					}
				}
				// 6.6.5 common part
				if (g.info.victory == 9 && (t == ptype::kTerraCotta || t == ptype::kLiberty || t == ptype::kSpaceProgram))
					V /= 1000;
				if (up == 0) {
					const int32_t pd = C.peasant_dist;
					if (pd >= 5) V = V / ((pd + 5) / 5);
					if (C.free == 0 && L.reg_free_peasants[size_t(reg)] != 0) {
						V = (3 * V) / 4;
						if (C.gatherers == 0 && C.busy != 0) V /= 2;
					}
					if (T.domain != 1 && !(T.build_flags & 0x10)) {
						if (fl <= 1 && sp < 2) continue;
						if (fl >= (7 * C.land) / 8) V = (4 * V) / 3;
						if (fl < 2 && sp < 3) V = (3 * V) / 4;
					}
				}
				if (is_gather_enhancer(g, t)) {
					cat = 4;
					if (g.info.starting_resources == 8) continue;
					if (g.info.victory == 8) V *= 100;
					const int32_t B = L.infra_mod * V / 256;
					const int32_t gd = enhancing_good(t);
					int32_t n = gd >= 0 ? gs[gd] - fs[gd] : 0;
					if (gd == 5) n = 2 * L.gather_slots[5];
					if (gd < 0 || type_avail(g, who, gd, 1) == 0 || L.income[size_t(gd)] >= get_mod_resource_cap(g, who, gd))
						continue;
					if (!(n >= 3 || gd == 5)) continue;
					if (type_is(T, ptype::kGranary) && has_tribe_bonus(g, who, 0x13)) continue;
					if (!(fl >= 4 || gd == 5)) continue;
					V = n * n * B;
					const int32_t e = L.econ[size_t(gd)];
					if (e & 4) V *= 2;
					if (e & 2) {
						V *= 3;
						esc = 1;
					}
					if (e & 1) {
						V *= 4;
						esc = 1;
					}
				}
				const bool mt = mil_trainer(g, t);
				if (!(T.build_flags & 0x8000000) && !type_is(T, ptype::kDock) && mt) {
					cat = 7;
					if (g.info.rush_rules == 8) continue;
					if (cnt != 0 && up == 0) continue;
					V *= L.wars + L.reg_wars[size_t(reg)] + L.reg_neutrals[size_t(reg)] + 3;
					if (reg_buildings_of(g, who, reg, t) == 0 && q_t == 0) {
						V *= 2;
						if (type_is(T, ptype::kBarracks) || type_is(T, ptype::kStable) ||
						    (type_is(T, ptype::kSiegeFactory) && L.num_buildings[size_t(t - kBuilds)] == 0 && L.num_queued[size_t(t)] == 0))
							V *= 200;
					}
					if (L.num_buildings[size_t(t - kBuilds)] == 0 && L.num_queued[size_t(t)] == 0) esc = 1;
					const int32_t n = L.num_buildings[size_t(t - kBuilds)] + L.num_queued[size_t(t)];
					if (n < (L.age + 2) / 2) V *= 100;
					if (g.info.starting_resources != 8 && n > L.age) continue;
					const int32_t tr = L.tribe;
					if ((type_is(T, ptype::kBarracks) && (tr == 0 || tr == 6)) || (type_is(T, ptype::kStable) && tr == 17) ||
					    (type_is(T, ptype::kSiegeFactory) && tr == 8) || (type_is(T, ptype::kAirbase) && tr == 12))
						V *= 4;
				}
				if (type_is(T, ptype::kDock)) {
					if (cnt != 0 && up == 0) continue;
					if (g.info.starting_resources != 8 && L.num_queued[size_t(t)] + L.num_buildings[size_t(t - kBuilds)] > L.age)
						continue;
					V = L.sea_mod * V / 256;
					if (owned_w(g, who, t) == 0 && q_t == 0) {
						V *= 4;
						if (g.world.sea_map >= 3) {
							V *= 20;
							esc = 1;
						}
					}
					if (g.world.sea_map < 3 && L.city_num < 2) continue;
					if (reg_buildings_of(g, who, reg, t) == 0) V *= 8;
					if (L.reg_wars[size_t(reg)] == 0) V *= L.active_wars ? 4 : 2;
					else V /= 2;
					V *= g.world.sea_map;
				}
				if (((T.build_flags & 0x8000000) && !type_is(T, ptype::kUniversity) && !is_gather_enhancer(g, t)) ||
				    type_is(T, ptype::kMarket)) {
					const bool temple = type_is(T, ptype::kTemple);
					if (!temple) {
						if (up == 0 && 2 * (owned_w(g, who, t) + q_t) >= L.city_num) continue;
						if (g.info.starting_resources != 8 && 2 * (owned_w(g, who, t) + q_t) > L.age) continue;
					}
					if (reg_buildings_of(g, who, reg, t) == 0) {
						int32_t m = 10;
						if (temple) {
							m = 18;
							V = L.infra_mod * V / 256;
							if (L.my_team_terr >= L.min_other_team_terr) m = 9;
							if (L.econ[2] & 3) m *= 2;
						}
						if (type_is(T, ptype::kMarket)) {
							m += 1;
							if (L.econ[2] & 3) m *= 2;
						}
						if (q_t != 0) m >>= 1;
						else if (owned_w(g, who, t) == 0) {
							if (type_is(T, ptype::kLibrary)) m *= 2;
							m *= 2;
						}
						if (L.city_num == 1) m >>= 2;
						else if (L.city_num == 2) m >>= 1;
						else if (L.city_num > 2 && L.num_buildings[size_t(t - kBuilds)] == 0) {
							m = (L.city_num * m) / 2;
							esc = 1;
						}
						if (m > 1) V *= m;
					}
					if (type_is(T, ptype::kLibrary) && owned_w(g, who, t) < 2 && L.city_num > 3) V = L.infra_mod * V * 10000 / 256;
					if (type_is(T, ptype::kMarket) && owned_w(g, who, t) < 1) V = L.infra_mod * V * 10000 / 256;
					if (type_is(T, ptype::kSenate)) {
						if (attacked || city_num_buildings(g, who, c) < 5) continue;
						V *= 10;
						if (get_highest_epoch(g, who) >= 3) V *= 10;
					}
				}
				if (type_is(T, ptype::kTower) || type_is(T, ptype::kFortX)) {
					const bool fortx = type_is(T, ptype::kFortX);
					int32_t D = L.defense_mod * V / 256;
					if (g.info.rush_rules == 8) {
						if (!fortx || L.my_team_terr > L.other_team_terr) continue;
					}
					if (L.defense_mod <= 256) {
						if (d == 0) continue;
						if (d == 1 && L.num_queued[size_t(t)] + L.num_buildings[size_t(t - kBuilds)] > L.age) continue;
					}
					const int32_t f = city_num_buildings(g, who, c) - cnt;
					D *= f >= 4 ? f / 2 : 1;
					if (C.bordering) D *= 10;
					else if (fortx) D /= 100;
					if (C.city_flags & cityflag::kVeryNearBorder) D *= attacked ? 2 : 10;
					if (!(C.city_flags & cityflag::kEverAttacked)) D /= 2;
					if (C.founder != who) D *= 3;
					if (is_unassimilated(g, *cb)) D = (3 * D) / 4;
					if (L.reg_wars[size_t(reg)] == 0) D /= 2;
					if (fortx) {
						if (L.my_team_terr < L.min_other_team_terr) D *= 2;
						const int32_t F = (3 * D) / (cnt + 1);
						if (owned_w(g, who, t) + L.num_queued[size_t(t)] == 0) esc = 1;
						const int32_t cx = g.world.xs / 2, cy = g.world.ys / 2;
						const int32_t dx = std::abs(cx - wcoord_of(C.x)), dy = std::abs(cy - wcoord_of(C.y));
						V = ((cx + cy - dx - dy) / 10 + 1) * F;
					} else {
						V = D / (cnt / 2 + 1);
						if (cnt == 0) {
							if (d >= 3) {
								V *= 10;
								esc = 1;
							}
							if (C.city_flags & cityflag::kCapital) esc = 1;
						}
					}
				}
				if (type_is(T, ptype::kAirbase)) {
					if (g.info.rush_rules == 8) continue;
					const int32_t n = q_t + reg_buildings_of(g, who, reg, t);
					V = n ? V / n : 10 * V;
					if (L.reg_wars[size_t(reg)] == 0) V /= 2;
					if (n == 0) esc = 1;
					V = L.air_mod * V / 256;
				}
				// 6.6.6 common tail
				if (L.reg_free_peasants[size_t(reg)] == 0) V = (4 * V) / 5;
				if (V < 0) V = is_wonder_type(t) ? 9999971 : 9999999;
				if (attacked && !is_defensive(g, t) && (!mt || city_count_buildings(g, who, c, t, 0, 1) != 0)) V /= 100;
				const int32_t q = q_t;
				const int32_t rb = reg_buildings_of(g, who, reg, t);
				if (T.build_flags & 0x40) {
					V = V / (q + 1);
					esc = 1;
					const MakeObject& i0 = L.make_list[0];
					if (V < i0.val && i0.t >= 0 && !can_pay(g, who, 0) && !(is_build_type(i0.t) && (g.type(i0.t).build_flags & 0x40))) {
						const int32_t gd = building_good(t);
						for (int32_t k2 = 0; k2 < 6; k2++) {
							if (k2 == gd && type_avail(g, who, k2, 1) != 0 &&
							    get_cost(g, i0.t, k2, who, i0.o, -1, 0, 1, -1) > L.bucket[size_t(k2)]) {
								V = std::min(2 * V, i0.val);
								break;
							}
						}
					}
				} else if (type_is(T, ptype::kDock)) {
					V = V / (4 * (q + 2 * cnt) + 1 + rb);
				} else if (mt) {
					V = V / (4 * (cnt + q) + 1 + rb);
				} else if (is_training_building(g, t)) {
					V = V / (2 * (rb + 2 * (q + 2 * cnt)) + 1);
				} else if (type_is(T, ptype::kTower)) {
					V = V / (4 * (q + cnt) + 1);
				} else if (type_is(T, ptype::kFortX)) {
					V = V / (4 * (q + 2 * cnt) + 1);
				} else {
					V = V / (4 * (owned_w(g, who, t) + 2 * cnt + q) + 1);
				}
				const int32_t m = check_income(g, who, t, k * 256, -1, esc, c, 1, nullptr);
				make_me(g, who, t, (m * V) / 256, esc, cat, c, up, 1, 0, 0);
			}
		}
	}
}

}  // namespace ron::sim
