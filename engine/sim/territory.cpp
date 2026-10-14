// Territory influence (docs/production-spec.md section 5.10): World::compute_reg_territory @6b0bb0 and
// World::compute_all_territory @6b5700.
//
// Every land wcoord is owned by the source (city or fort) with the smallest "influence cost"
//   cost = (territory_den * d' * 256) / (strength * multiplier + territory_num)
// where d' is the octagonal tcoord distance from the wcoord centre to the source (shrunk near the source) and
// strength sums the source's bonuses; a source only competes within its range (world territory limits) and only when
// cost <= territory_base * 256. WData.who / who2 get the best and second-best owners, reg_terr counts the wcoords.
// Plain int32 C arithmetic (wrapping products, truncating division), sources visited in the original's order.
#include <algorithm>

#include "sim/territory.hpp"
#include "sim/fog.hpp"
#include "sim/production_internal.hpp"
#include "sim/rules_xml.hpp"

namespace ron::sim {

namespace {

constexpr int32_t kTempleBorders2 = 712, kTempleBorders3 = 713, kTempleBorders4 = 714;
constexpr int32_t kFortBorders2 = 721, kFortBorders3 = 722, kFortBorders4 = 723;
constexpr int32_t kTribeRomans = 6, kTribeRussians = 13;
constexpr int32_t kCtwMissionariesCard = 32;  // LeaderData +0x6916 = num_bonus_cards[32]

// Element i of a constants array (rules constants are int32 by byte offset).
int32_t kat(const Game& g, int off, int32_t i) {
	return off < 0 ? -1 : g.rules->constants.at(uint32_t(off + 4 * i));
}
// (imul: the wrapping int32 product of sim/rules_xml.hpp.)
int32_t iabs(int32_t v) {
	const uint32_t s = uint32_t(v >> 31);
	return int32_t((uint32_t(v) ^ s) - s);
}

// The octagonal distance approximation of the territory pass (unsigned, as compiled): max + min^2 / (2 max), with a
// cheaper (min + 2 max) / 2 once min reaches 60000.
uint32_t oct_dist(uint32_t dx, uint32_t dy) {
	if (int32_t(dy) < int32_t(dx)) {
		if (dx == 0) return 0;
		if (dy < 60000) return (dy * dy) / (dx * 2) + dx;
		return (dy + dx * 2) >> 1;
	}
	if (dy == 0) return 0;
	if (dx < 60000) return (dx * dx) / (dy * 2) + dy;
	return (dx + dy * 2) >> 1;
}

// Distances close to the source count for less (signed int arithmetic).
int32_t shrink(int32_t d) {
	if (d < 13) d = imul(d, 2) / 3;
	if (d < 9) d = imul(d, 2) / 3;
	if (d < 5) d = d / 2;
	return d;
}

// LeaderData::get_handicap @6da740, multiplayer branch only (the territory pass calls it only for human seats with
// semaphore byte0 & 4): the lobby handicap's rules category data[0] (handicaps entry +0x3c).
int32_t get_handicap_mp(const Game& g, int32_t who) {
	const int32_t h = g.leaders[size_t(who)].handicap;
	const CategoryList* cl = g.rules ? g.rules->category_list("handicaps") : nullptr;
	if (!cl || h < 0 || size_t(h) >= cl->items.size()) return 0;
	return cl->items[size_t(h)].data[0];
}

// Per-leader terms computed once per call (the original's eight stack arrays).
struct LeaderTerms {
	int32_t civic_terr = 0;    // civic_upgrade_terr[civic epoch]
	int32_t city_range = 0;    // base city range: limit + civic * limit_civic (+ gems / wonder / Russian steps)
	int32_t bonus = 0;         // strength bonus of every source: gems, Colosseum, Eiffel, Russians, MP handicap
	int32_t temple_terr = 0;   // temple strength (TEMPLEBORDERS level, Tikal, CTW missionaries)
	int32_t temple_level = 0;  // 1..4: range steps a temple adds to a city
	int32_t fort_terr = 0;     // fort strength
	int32_t fort_level = 0;    // range steps of a fort (FORTBORDERS level - 1, Romans more)
	int32_t fort_range = 0;    // fort_level * limit_city + city_range
};

int32_t object_type_of(const Game& g, int32_t who, int32_t o) {
	const Object* ob = g.object(who, o);
	return ob ? ob->type : -1;
}
bool object_is(const Game& g, int32_t who, int32_t o, int32_t t) {
	const Object* ob = g.object(who, o);
	return ob && ob->ptype && type_is(*ob->ptype, t, false);  // vslot 0xb8 is(t, 0): BuildType::is for buildings
}

}  // namespace

int32_t compute_reg_territory(Game& g, int32_t r, int32_t full) {
	Region& rg = g.regions.r[size_t(r)];
	World& w = g.world;
	if (full) rg.borders = 0;

	// Cursor 0: a fresh pass. Clear this region's counters and every city's bordering mask.
	if (rg.borders == 0) {
		for (int32_t p = 0; p < 8; p++) {
			Leader& L = g.leaders[size_t(p)];
			if (!(L.flags & leaderflag::kValid)) continue;
			L.reg_terr[size_t(r)] = 0;
			L.reg_known_rares[size_t(r)] = 0;
			auto& cl = g.cities[size_t(p)];
			for (int32_t c = 0; c < L.city_mark && size_t(c) < cl.size(); c++) {
				if (cl[size_t(c)].city_flags & cityflag::kValid) cl[size_t(c)].bordering = 0;
			}
		}
		rg.flags &= ~region_flag::kAnybodyHere;
	}
	if (rg.size <= rg.borders) return 0;
	if (g.check_borders_budget > 255) return 1;

	// The world territory limits (World::init sets both triples from territory_limit_base / _civic / _city): the
	// player triple for regions flagged PLAYER (a start region), the colonized one elsewhere.
	const int32_t lim_base = K("territory_limit_base"), lim_civic = K("territory_limit_civic"),
	              lim_city = K("territory_limit_city");

	static const int o_temple = constant_offset("temple_upgrade_terr");  // = fort_upgrade_terr[level + 3]
	static const int o_fort = constant_offset("fort_upgrade_terr");
	static const int o_civic = constant_offset("civic_upgrade_terr");
	static const int o_city_level = constant_offset("city_level_territory_bonus");

	std::array<LeaderTerms, 8> T{};
	for (int32_t p = 0; p < 8; p++) {
		const Leader& L = g.leaders[size_t(p)];
		LeaderTerms& t = T[size_t(p)];
		if (!(L.flags & leaderflag::kValid)) continue;  // all terms 0

		// Temples.
		const int32_t tl = has_preq(g, p, kTempleBorders4)   ? 4
		                   : has_preq(g, p, kTempleBorders3) ? 3
		                   : has_preq(g, p, kTempleBorders2) ? 2
		                                                     : 1;
		t.temple_level = tl;
		t.temple_terr = kat(g, o_temple, tl - 1);
		if (has_wonder(g, p, ptype::kTikal)) t.temple_terr = imul(K("tikal_temple_hp") + 100, t.temple_terr) / 100;
		if (g.ctw && L.num_bonus_cards[size_t(kCtwMissionariesCard)] != 0)
			t.temple_terr = imul(K("ctw_missionaries_bonus") + 100, t.temple_terr) / 100;

		// Forts.
		const int32_t fl = has_preq(g, p, kFortBorders4)   ? 4
		                   : has_preq(g, p, kFortBorders3) ? 3
		                   : has_preq(g, p, kFortBorders2) ? 2
		                                                   : 1;
		t.fort_level = fl - 1;
		t.fort_terr = kat(g, o_fort, fl - 1);
		if (has_wonder(g, p, ptype::kColosseum)) t.fort_terr += K("colosseum_fort_borders");
		if (has_tribe_bonus(g, p, kTribeRomans)) {
			const int32_t rb = K("roman_fort_borders");
			t.fort_terr += rb;
			t.fort_level = (rb + 1) / 2 + (fl - 1);
		}

		// Civic epoch, gems, Colosseum, Eiffel Tower, Russians, multiplayer handicap.
		const int32_t civic = L.epoch[1];
		t.bonus = 0;
		t.civic_terr = kat(g, o_civic, civic);
		const int32_t base_range = lim_base + imul(civic, lim_civic);
		t.city_range = base_range;
		const bool russian = has_tribe_bonus(g, p, kTribeRussians);
		if ((L.rare[2] & 0x80) || (L.rare_conquest[2] & 0x80)) {  // GEMS (type 29): rare bit 23
			int32_t v = K("gems_territory_bonus");
			if (russian) v /= 2;
			t.bonus = v;
			t.city_range = lim_civic + base_range;
		}
		if (has_wonder(g, p, ptype::kColosseum)) {
			int32_t v = K("colosseum_territory_bonus");
			t.bonus += russian ? v / 2 : v;
			if (russian) v = 1;
			t.city_range += imul((v + 1) / 2, lim_civic);
		}
		if (has_wonder(g, p, ptype::kEiffel)) {
			int32_t v = K("eiffel_tower_territory_bonus");
			t.bonus += russian ? v / 2 : v;
			if (russian) v = 1;
			t.city_range += imul((v + 1) / 2, lim_civic);
		}
		if (russian) {
			const int32_t rb = K("russian_borders");
			t.bonus += imul(K("russian_borders_per_age"), civic) + rb;
			t.city_range += imul((rb != 0 ? 1 : 0) + civic, lim_civic);
		}
		const int32_t h = ((L.flags & leaderflag::kHuman) && g.multiplayer) ? get_handicap_mp(g, p) : 0;
		t.bonus += (h + 15) / 25;
		t.fort_range = imul(t.fort_level, lim_city) + t.city_range;
	}

	const int32_t base_cost = imul(K("territory_base"), 256);
	const int32_t den = K("territory_den"), num = K("territory_num");
	const int32_t city_mult = K("city_territory_multiplier"), fort_mult = K("fort_territory_multiplier");
	const int32_t capital_bonus = K("capital_territory_bonus");

	while (rg.borders < rg.size) {
		// One wcoord per step; a negative budget (compute_all_territory) is unlimited and never counts.
		const int32_t budget = g.check_borders_budget;
		if (budget >= 0) {
			g.check_borders_budget = budget + 1;
			if (budget >= 256) break;
		}
		const WCoord wc = rg.coords[size_t(rg.borders)];
		rg.borders++;
		const int32_t wx = wc.x, wy = wc.y;
		// (Borders: the border-line renderer's per-wcoord cell gets the old owner here; presentation, skipped.)
		const int32_t tcx = wx * 4 + 2, tcy = wy * 4 + 2;  // the wcoord's centre tcoord

		int32_t best = 999999999, second = 999999999;
		int32_t best_who = -1, second_who = -1, best_city = -1;

		for (int32_t j = 0; j < 8; j++) {
			const int32_t who = (j + wx) % 8;  // start with leader wx mod 8 (C remainder)
			const Leader& L = g.leaders[size_t(who)];
			if (!(L.flags & leaderflag::kValid)) continue;
			const LeaderTerms& t = T[size_t(who)];

			// Cities.
			auto& cl = g.cities[size_t(who)];
			for (int32_t c = 0; c < L.city_mark && size_t(c) < cl.size(); c++) {
				City& ct = cl[size_t(c)];
				if (!(ct.city_flags & cityflag::kValid)) continue;
				const uint32_t dx = uint32_t(iabs(tcx - tcoord_of(ct.x)));
				const uint32_t dy = uint32_t(iabs(tcy - tcoord_of(ct.y)));
				int32_t d = int32_t(oct_dist(dx, dy));
				const int32_t level = city_get_level(object_type_of(g, ct.who, ct.o));
				const bool temple = (ct.city_flags & cityflag::kHasTemple) != 0;
				const int32_t range =
				    imul(level - 1, lim_city) + (temple ? imul(t.temple_level, lim_city) : 0) + t.city_range;
				if (d > range) continue;
				d = shrink(d);
				// The strength terms come from the leader the city is assimilated to (race).
				const int32_t race = ct.race;
				int32_t strength, bonus, temple_terr;
				if (race == who) {
					strength = t.civic_terr, bonus = t.bonus, temple_terr = t.temple_terr;
				} else if (race >= 0 && race < 8) {
					strength = T[size_t(race)].civic_terr, bonus = T[size_t(race)].bonus,
					temple_terr = T[size_t(race)].temple_terr;
				} else {
					strength = bonus = temple_terr = 0;  // (no such leader; the original reads past its arrays)
				}
				if (temple) strength += temple_terr;
				const int32_t level_bonus =
				    ((ct.city_flags & cityflag::kOriginalCapital) || object_is(g, who, ct.o, ptype::kForbiddenCity))
				        ? capital_bonus
				        : kat(g, o_city_level, level - 1);
				const int32_t div = imul(strength + level_bonus + bonus, city_mult) + num;
				if (div == 0) continue;  // (the original faults)
				const int32_t cost = imul(imul(den, d), 256) / div;
				const int32_t tag = race == who ? who : -2;  // -2: an unassimilated city claims for nobody
				if (cost > base_cost || cost >= best) {
					if (cost < second) {
						second = cost;
						second_who = tag;
					}
				} else {
					if (best_who != -1) {
						second = best;
						second_who = best_who;
					}
					best = cost;
					best_city = c;
					best_who = tag;
				}
			}

			// Forts.
			auto& fl = g.forts[size_t(who)];
			for (int32_t f = 0; f < L.fort_mark && size_t(f) < fl.size(); f++) {
				const Fort& ft = fl[size_t(f)];
				if (!(ft.fort_flags & 1)) continue;
				const Object* ob = g.object(who, ft.o);
				if (!ob) continue;
				const uint32_t dx = uint32_t(iabs(tcx - tcoord_of(ob->x)));
				const uint32_t dy = uint32_t(iabs(tcy - tcoord_of(ob->y)));
				int32_t d = int32_t(oct_dist(dx, dy));
				if (d > t.fort_range) continue;
				d = shrink(d);
				int32_t strength = t.fort_terr + t.civic_terr;
				if (object_is(g, who, ft.o, ptype::kRedFort)) strength += 4;
				const int32_t div = imul(strength + t.bonus, fort_mult) + num;
				if (div == 0) continue;  // (the original faults)
				const int32_t cost = imul(imul(den, d), 256) / div;
				if (cost > base_cost || cost >= best) {
					if (cost < second) {
						second = cost;
						second_who = who;
					}
				} else {
					// Unlike the city branch, the old best always moves down (even "none").
					second = best;
					best = cost;
					second_who = best_who;
					best_city = -1;
					best_who = who;
				}
			}
		}

		// (Borders::save_last_who @8835e0: the border-line renderer's dirty cells; presentation, skipped.)
		WData& wd = w.w(wx, wy);
		wd.who = int8_t(best_who);
		wd.who2 = int8_t(second_who);
		if (best_who < 0) continue;

		// A city at a hostile border: both players' bits in its bordering mask, VERY_NEAR_BORDER within 4 wcoords.
		if (best_city >= 0 && second_who >= 0) {
			const Leader& B = g.leaders[size_t(best_who)];
			const Leader& S = g.leaders[size_t(second_who)];
			if (second_who != best_who && (B.diplos(second_who) == 0 || S.diplos(best_who) == 0)) {
				City& ct = g.cities[size_t(best_who)][size_t(best_city)];
				ct.bordering = uint8_t(ct.bordering | (1u << (uint32_t(best_who) & 31)));
				ct.bordering = uint8_t(ct.bordering | (1u << (uint32_t(second_who) & 31)));
				if (iabs(wy - wcoord_of(ct.y)) + iabs(wx - wcoord_of(ct.x)) < 5) ct.city_flags |= cityflag::kVeryNearBorder;
			}
		}
		Leader& O = g.leaders[size_t(best_who)];
		O.reg_terr[size_t(r)] = uint16_t(O.reg_terr[size_t(r)] + 1);
		rg.flags |= region_flag::kAnybodyHere;

		// The first live good on this wcoord: oil joins the owner's oil_patches; another good is a rare resource
		// the owner learns about if the wcoord is explored by it (or it sees everything).
		for (int32_t gi = 0; gi < g.good_mark && size_t(gi) < g.goods.size(); gi++) {
			GoodObj& gd = g.goods[size_t(gi)];
			if (!gd.alive() || wcoord_of(gd.x) != wx || wcoord_of(gd.y) != wy) continue;
			if (gd.type == ptype::kOil) {
				std::vector<int32_t>& op = O.oil_patches;
				if (std::find(op.begin(), op.end(), gi) == op.end()) op.push_back(gi);
			} else {
				const size_t fi = size_t((wy * 2 + 1) * w.fog_xs + 1 + wx * 2);  // the wcoord's centre fog cell
				if (best_who > 7 || g.info.reveal_map == 3 || (O.flags & leaderflag::kRevealEnemy) ||
				    O.num_units[size_t(fogtype::kFouche - kUnits)] != 0 || (w.seen2[fi] & O.ally_mask) != 0) {
					leader_new_rare(g, best_who, gi);
					gd.ever_seen = uint8_t(gd.ever_seen | (1u << (uint32_t(best_who) & 31)));
				}
			}
			break;
		}
	}

	if (rg.borders >= rg.size) {
		for (int32_t p = 0; p < 8; p++) {
			Leader& L = g.leaders[size_t(p)];
			if (L.flags & leaderflag::kValid) L.flags |= leaderflag::kRecompute;
		}
		rg.flags |= region_flag::kAnybodyHere | region_flag::kGatherVerify;
		// (Borders::add_border_region / the border texture refresh and Region +0x30, its renderer slot: presentation.)
	}
	return (rg.borders >= rg.size ? 1 : 0) + 1;
}

void compute_all_territory(Game& g) {
	for (int32_t p = 0; p < 8; p++) {
		Leader& L = g.leaders[size_t(p)];
		if (L.flags & leaderflag::kValid) L.reg_terr.fill(0);
	}
	g.check_borders_budget = -1;  // unlimited
	for (int32_t r = 0; r < 64; r++) {
		Region& rg = g.regions.r[size_t(r)];
		if (rg.size == 0) continue;
		rg.borders = 0;
		compute_reg_territory(g, r, 1);
	}
	// (fog.flags |= 2: the fog renderer's refresh request; presentation.)
	for (int32_t r = 64; r < 128; r++) {
		const Region& rg = g.regions.r[size_t(r)];
		for (int32_t i = 0; i < rg.size && size_t(i) < rg.coords.size(); i++) {
			WData& wd = g.world.w(rg.coords[size_t(i)].x, rg.coords[size_t(i)].y);
			wd.who = -1;
			wd.who2 = -1;
		}
	}
	for (int32_t p = 0; p < 8; p++) {
		Leader& L = g.leaders[size_t(p)];
		if (!(L.flags & leaderflag::kValid)) continue;
		L.territory = 0;
		for (int32_t r = 0; r < 64; r++) {
			if (g.regions.r[size_t(r)].size != 0) L.territory += int32_t(L.reg_terr[size_t(r)]);
		}
		L.territory_high = std::max(L.territory_high, L.territory);
	}
	g.check_borders_budget = 0;
}

}  // namespace ron::sim
