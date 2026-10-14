// AI building placement (docs/ai-spec.md 7): Leader::produce_building @6e1400 (context, oil wells, the ring scan with
// its scoring and the rnd(500) draw, builder choice, fine position with the rnd(100) draws, payment, init_build and the
// swarm order) and BuildTypeData::find_friends @639270.
//
// Evidence (local, gitignored): re/decomp/ai/Leader_produce_building@006e1400.c and its disassembly
// (re/decomp/ai/asm), re/decomp/ai2/BuildTypeData_find_friends@00639270.c.
#include <algorithm>

#include "sim/ai_internal.hpp"
#include "sim/economy.hpp"
#include "sim/orders.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace ai;

namespace {

bool is_gather_enhancer_t(const Game& g, int32_t t) {
	const Type& T = g.type(t);
	return type_is(T, ptype::kGranary) || type_is(T, ptype::kLumberMill) || type_is(T, ptype::kSmelter) ||
	       type_is(T, ptype::kRefinery);
}
int32_t enhancing_good_t(int32_t t) {
	switch (t) {
	case ptype::kGranary: return 0;
	case ptype::kLumberMill: return 1;
	case ptype::kSmelter: return 4;
	case ptype::kRefinery: return 5;
	default: return -1;
	}
}

// An enemy object of `who` in the 3x3 wcoords around (x, y) (the city-under-attack test of 7.3 step 5).
bool enemy_near(const Game& g, int32_t who, int32_t x, int32_t y) {
	for (int32_t k = 0; k < 9; k++) {
		const int32_t cx = x + move_x[k], cy = y + move_y[k];
		if (!g.world.in_w(cx, cy)) continue;
		const WData& d = g.world.w(cx, cy);
		int32_t co = d.down, cw = d.down_who;
		while (co >= 0) {
			if (cw < 8 && is_enemy(g, who, cw)) return true;
			const Object* ob = g.object(cw, co);
			if (!ob) break;
			co = ob->down;
			cw = ob->down_who;
		}
	}
	return false;
}

bool any_enemy_within(const Game& g, int32_t who, int32_t x, int32_t y, int32_t r) {
	// ObjectsData::find(x, y, SEARCH_ENEMY, who, r, ...): units first, then buildings (TODO(spec): ObjectsData::find's
	// exact order; only the existence is used here).
	if (objects_find_unit(g, x, y, search::kEnemy, who, r, 0, filter::kAll, 0, 0, filter::kAll, 0, 0) >= 0) return true;
	return objects_find_building(g, x, y, search::kEnemy, who, r, 0, filter::kAll, 0, 0) >= 0;
}

}  // namespace

// ---------------------------------------------------------------- find_friends (7.5)

int32_t find_friends(const Game& g, int32_t t, int32_t x, int32_t y, int32_t cidx, int32_t who) {
	const Type& T = g.type(t);
	if (t == ptype::kWoodcutter) return 0;
	const bool tower = type_is(T, ptype::kTower), lookout = type_is(T, ptype::kLookout);
	if (T.attack != 0 && !tower && !lookout) return 0;
	const bool outside = (T.build_flags & 0x10) != 0;
	const bool enh = is_gather_enhancer_t(g, t), mt = is_military_trainer(g, t), farm = type_is(T, ptype::kFarm);
	int32_t f = 0;
	for (int32_t k = 1; k <= 8; k++) {
		const int32_t nx = x + move_x[k], ny = y + move_y[k];
		if (!g.world.in_w(nx, ny)) continue;
		int32_t bw = -1;
		const int32_t bo = find_building_placed_at(g, wcoord_tcentre(nx), wcoord_tcentre(ny), who, -1, -1, &bw);
		if (bo < 0 || bw != who) continue;
		const Build* b = g.build(who, bo);
		if (!b) continue;
		if (!outside && b->city != cidx) continue;
		const int32_t wgt = (k & 1) ? 1 : 2;
		const Type& N = *b->ptype;
		if (enh) {
			if (building_good(b->type) == enhancing_good_t(t) && building_good(b->type) >= 0) f += wgt;
		} else if (mt) {
			if (is_military_trainer(g, b->type) && !(b->flags & objflag::kCity)) f += wgt;
		} else if (tower || lookout) {
			if (is_gather_type(N) && !type_is(N, ptype::kUniversity)) f += 2;
			if (is_wonder_type(b->type)) f += lookout ? 8 : 4;
		} else if (farm) {
			if (type_is(N, ptype::kFarm) || type_is(N, ptype::kGranary)) f += wgt;
		} else {
			const bool skip = (is_gather_type(N) && !type_is(N, ptype::kUniversity)) || is_gather_enhancer_t(g, b->type) ||
			                  (is_military_trainer(g, b->type) && !(b->flags & objflag::kCity)) || is_wonder_type(b->type);
			if (!skip) f += wgt;
		}
	}
	return f;
}

// ---------------------------------------------------------------- produce_building (7.1-7.4)

int32_t produce_building(Game& g, int32_t who, int32_t t, int32_t city_o, int32_t escrow) {
	Leader& L = LD(g, who);
	const World& w = g.world;
	Build* cb = g.build(who, city_o);
	if (!cb) return 1;
	// 7.1 context
	int32_t cidx = -1;
	bool allowed;
	if ((cb->flags & objflag::kActive) && (cb->flags & objflag::kCity) && cb->city >= 0) {
		cidx = cb->city;
		allowed = g.cities[size_t(who)][size_t(cidx)].valid();
	} else {
		allowed = (g.type(t).build_flags & 0x10) != 0;
	}
	if (!allowed) return 1;
	int32_t tt = t;
	const Type* T = &g.type(tt);
	const int32_t cx = wcoord_of(cb->x), cy = wcoord_of(cb->y);
	const int32_t R0 = w.w(cx, cy).region;
	const int32_t rad = get_radius(g, who, cb->type);
	const Circles& CI = circles();
	int32_t n = (rad + 2) / 4;
	const bool outside = g.frame != 0 && (T->build_flags & 0x10);
	bool tower = false, fort = false;
	int32_t k0 = 1;
	if (outside) {
		n += 1;
		tower = type_is(*T, ptype::kTower);
		if (!(T->build_flags & 0x40) && !type_is(*T, ptype::kDock) && !tower) {
			k0 = type_is(*T, ptype::kFortX) ? 1 : CI.radius[3];
			fort = type_is(*T, ptype::kFortX);
		}
	}
	n = std::min(n, 64);
	bool gather = (T->build_flags & 0x40) != 0;
	if (tt == ptype::kFarm || tt == ptype::kUniversity || tt == ptype::kMine) gather = false;
	const int32_t xs = T->x_size, ys = T->y_size;
	int32_t sx, sy, sum;
	if (type_is(*T, ptype::kDock)) {
		sx = sy = 4;
		sum = 8;
	} else {
		sx = xs < 4 ? 5 - xs : 1;
		sy = ys < 4 ? 5 - ys : 1;
		sum = (xs < 4 || ys < 4) ? sx + sy : 2;
	}
	int32_t best = 0, bq = 0, bpx = 0, bpy = 0;
	if (tt == ptype::kOilWell) {
		// 7.2 oil wells: the discovered oil patches, last to first.
		int32_t bwx = -1, bwy = -1;
		for (int32_t i = int32_t(L.oil_patches.size()) - 1; i >= 0; i--) {
			const int32_t gi = L.oil_patches[size_t(i)];
			if (gi < 0 || size_t(gi) >= g.goods.size() || !g.goods[size_t(gi)].alive()) continue;
			const GoodObj& gd = g.goods[size_t(gi)];
			const int32_t px = wcoord_of(gd.x), py = wcoord_of(gd.y);
			if (!w.in_w(px, py)) continue;
			const WData& d = w.w(px, py);
			const bool water = World::is_ocean(d) && !(d.flags & 0x100);
			if (water) {
				if (!(L.flags & 0x700)) continue;
			} else if (d.who >= 0 && d.who != who) {
				L.oil_patches.erase(L.oil_patches.begin() + i);
				continue;
			}
			const uint16_t tm = w.t(4 * px + 2, 4 * py + 2);
			if ((tm & 3) == 3 || (tm & 0x80)) continue;
			if (d.flags & 2) {
				if (!any_enemy_within(g, who, px * 0x300 + 0x180, py * 0x300 + 0x180, 0xf00))
					g.world.w(px, py).flags = uint16_t(g.world.w(px, py).flags & ~2);
				continue;
			}
			if (any_enemy_within(g, who, px * 0x300 + 0x180, py * 0x300 + 0x180, 0x600)) continue;
			const int32_t s = 2 * w.xs - vector_dist(px - cx, py - cy);
			if (s >= best) {
				best = s;
				bwx = px;
				bwy = py;
				bpx = px * 0x300 + g.type(ptype::kOilWell).x_size * 0x60;
				bpy = py * 0x300 + g.type(ptype::kOilWell).y_size * 0x60;
			}
		}
		if (best > 0 && bwx >= 0 && World::is_ocean(w.w(bwx, bwy))) {
			tt = ptype::kOilPlatform;
			T = &g.type(tt);
		}
		if (best <= 0) return 1;
	} else {
		// 7.3 the ring scan
		if (k0 >= CI.radius[size_t(n)]) return 1;
		int32_t step = 1;
		const bool sea = T->domain == 1;
		for (int32_t k = k0; k < CI.radius[size_t(n)]; k += step) {
			const int32_t x = cx + CI.x[size_t(k)], y = cy + CI.y[size_t(k)];
			if (!w.in_w(x, y) || (w.w(x, y).flags & 0x4000)) continue;
			const int32_t q = check_building_wcoord(g, x, y, who, sx, sy, sum, outside ? 0 : 1);
			if (q < 2) continue;
			const int32_t m = std::max(xs, ys);
			if (m < 5) {
				if (q < m || x == 0 || y == 0 || x == w.xs - 1 || y == w.ys - 1) continue;
			} else if (!(g.frame < 1 || (x >= 2 && x < w.xs - 2 && y >= 2 && y < w.ys - 2))) {
				continue;
			}
			if (!(tt == ptype::kOilPlatform || w.buildings_allowed(x, y))) continue;
			if (World::is_ocean(w.w(x, y)) != sea) continue;
			int32_t px = x * 0x300 + xs * 0x60, py = y * 0x300 + ys * 0x60;
			if (xs < 4) px += (4 - xs) * 0xc0 / 2;
			if (ys < 4) py += (4 - ys) * 0xc0 / 2;
			int32_t aux = 0;
			if (blocked_site(g, tt, px, py, who, -1, &aux) != 0) {
				if (!type_is(*T, ptype::kDock)) continue;
				bool found = false;
				for (int32_t dx = -sx / 2; dx <= sx / 2 && !found; dx++) {
					for (int32_t dy = -sy / 2; dy <= sy / 2; dy++) {
						if (std::abs(dx) + std::abs(dy) > sum) continue;
						const int32_t qx = x * 0x300 + xs * 0x60 + dx * 0xc0, qy = y * 0x300 + ys * 0x60 + dy * 0xc0;
						if (blocked_site(g, tt, qx, qy, who, -1, &aux) == 0) {
							px = qx;
							py = qy;
							found = true;
							break;
						}
					}
				}
				if (!found) continue;
			}
			const int32_t owner = w.w(x, y).who;
			if (owner != -1 && owner != who) continue;
			int32_t s = 1000;
			// The candidate distance from the city centre, in tcoords (disassembly @6e2040).
			const int32_t dt = vector_dist(4 * x + 2 - tcoord_of(cb->x), 4 * y + 2 - tcoord_of(cb->y));
			if (!(outside && g.info.starting_resources != 8 && fort)) {
				const int32_t f = find_friends(g, tt, x, y, cidx, who);
				if (f == 0) {
					if (tt == ptype::kFarm || tt == ptype::kMine) {
						s = 4000 / std::max(1, dt) + rnd_raw(g) % 500;  // S10
					} else if (tt == ptype::kWoodcutter && g.frame == 0) {
						if (dt > 3) s = 500;
						if (dt > 4) s >>= 1;
					} else if (dt > 4) {
						s = 333;
					}
				} else {
					s = (f + 2) * 1000;
					if (tower) s *= f + 2;
				}
				if (is_gather_enhancer_t(g, tt) && get_town(g, tt, tcoord_of(px), tcoord_of(py), who) != city_o) continue;
			} else {
				s = dt * 1000;
				if (w.t(4 * x + 2, 4 * y + 2) & 0x100) s /= 2;
			}
			if (!type_is(*T, ptype::kFortX)) {
				if (tower &&
				    objects_find_building(g, x * 0x300 + 0x180, y * 0x300 + 0x180, search::kAll, -1, 0x600, 0, filter::kType,
				                          ptype::kTower, 0) >= 0)
					s /= 8;
			} else {
				s += danger_at_w(g, who, x, y) / 4;
				const int32_t o2 = w.w(x, y).who2;
				if (o2 == who || o2 == -1 || is_ally(g, who, o2)) s /= 2;
				else if (g.info.team_style == 2) s *= get_target(g, who) == o2 ? 16 : 4;
				else s *= 8;
				if (cidx >= 0 && g.cities[size_t(who)][size_t(cidx)].bordering != 0 && n < CI.radius[3]) s *= 10;
			}
			int32_t mult = 1, extra = 0;
			if (!gather) {
				s += 255 - w.w(x, y).val;
			} else if (tt == ptype::kWoodcutter) {
				s *= aux * aux * aux;
				if (!(aux > 2 || g.frame == 0)) continue;
				s = (s + extra) * mult;
			} else {
				int32_t c[6];
				gather_at(g, c, x, y, 0);
				bool found = false;
				for (int32_t gd = 0; gd < 6; gd++) {
					if (c[gd] == 0 || type_avail(g, who, gd, 1) == 0 || gd != building_good(tt)) continue;
					found = true;
					if (!(L.econ[size_t(gd)] & 8)) mult += get_need(g, who, gd) * c[gd];
					else extra += c[gd];
					s *= c[gd] * c[gd];
				}
				if (!found) continue;
				s = (s + extra) * mult;
			}
			// 7.3 step 5: accept
			if (s < best) continue;
			if (w.w(x, y).flags & 2) {
				if (any_enemy_within(g, who, x * 0x300 + 0x180, y * 0x300 + 0x180, 0xf00)) continue;
				g.world.w(x, y).flags = uint16_t(g.world.w(x, y).flags & ~2);
			}
			if (cidx >= 0 && (g.cities[size_t(who)][size_t(cidx)].city_flags & cityflag::kUnderAttack) &&
			    enemy_near(g, who, x, y)) {
				step = 3;
				continue;
			}
			if (!gather && best != 0 && !tower && CI.radius[3] < k0) step = 3;
			best = s;
			bq = q;
			bpx = px;
			bpy = py;
		}
		if (best == 0) return 1;
	}
	// 7.4 builder, fine position, construction
	const int32_t txs = T->x_size, tys = T->y_size;
	const int32_t tx = tcoord_of(bpx) - txs / 2, ty = tcoord_of(bpy) - tys / 2;
	const int32_t r = cidx >= 0 ? std::min(64, get_radius(g, who, cb->type)) : 0x1200;
	int32_t builder = -1, builder_kind = 0;
	if (g.frame != 0) {
		int32_t bd = 99999999;
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			Unit* u = g.unit(who, o);
			if (!u || !u->alive() || !u->on_map() || (u->type != 50 && u->type != 51)) continue;
			const Order* a = get_action(*u);
			const int32_t k = a ? int32_t(a->type) : 0;
			if (k != 0 && k != kGather && k != kBuildAt) continue;
			if (w.w(wcoord_of(u->x), wcoord_of(u->y)).region != R0) continue;
			int32_t d = vector_dist(u->x - bpx, u->y - bpy);
			if (!(d < bd)) continue;
			if (k == kGather) {
				const Object* gb = a->ox >= 0 ? g.object(a->whom, a->ox) : nullptr;
				const int32_t gd = gb ? building_good(gb->type) : -1;
				d += gd == 0 ? (3 * r) / 2 : gd == 1 ? r / 2 : r / 3;
			} else if (k == kBuildAt) {
				const Object* tb = a->ox >= 0 ? g.object(a->whom, a->ox) : nullptr;
				if (tb && !tb->is_unit && is_wonder_type(tb->type)) continue;
				d += 2 * r;
			}
			if (d < bd) {
				bd = d;
				builder = o;
				builder_kind = k;
			}
		}
		if (builder < 0) return 1;
	}
	int32_t P_x = bpx, P_y = bpy;
	if (type_is(*T, ptype::kDock)) {
		// keep bestP
	} else if (tt == ptype::kWoodcutter) {
		if (4 - txs > 0 && 4 - tys > 0) {
			int32_t baux = -1;
			bool found = false;
			for (int32_t k = 0; k < kMoveRadius[4 - txs]; k++) {
				const int32_t qx = (txs + 2 * (move_x[k] + tx)) * 0x60, qy = (tys + 2 * (move_y[k] + ty)) * 0x60;
				int32_t aux = 0;
				if (blocked_site(g, tt, qx, qy, who, -1, &aux) != 0) continue;
				if (aux > baux) {
					baux = aux;
					P_x = qx;
					P_y = qy;
					found = true;
				}
			}
			if (!found) return 1;
		}
	} else {
		int32_t bv = -1;
		bool found = false;
		for (int32_t x2 = tx; x2 <= tx + 1; x2++) {
			for (int32_t y2 = ty; y2 <= ty + 1; y2++) {
				const int32_t qx = txs * 0x60 + x2 * 0xc0, qy = tys * 0x60 + y2 * 0xc0;
				if (blocked_site(g, tt, qx, qy, who, -1, nullptr) != 0) continue;
				const int32_t v = rnd_raw(g) % 100;  // S11
				if (v >= bv) {
					bv = v;
					P_x = qx;
					P_y = qy;
					found = true;
				}
			}
		}
		if (!found) return 1;
	}
	if (g.frame != 0) pay_cost(g, tt, who, -1, cidx, escrow, nullptr);
	const int32_t b = init_build_site(g, who, tt, P_x, P_y, false, -1);
	if (b < 0) return 1;  // (paid, not refunded)
	if (is_wonder_type(tt)) L.wonder_mod = 0;
	if (g.frame != 0) {
		Group gr;
		group_clear(gr, -1);
		group_add(g, gr, builder, who, 0, 0);
		group_action_swarm_around(g, gr, b, who, builder_kind == kBuildAt ? kQueueLast : kQueueNew, kBuildAt, true);
	} else {
		build_activate_full(g, *g.build(who, b), false, true, false);  // activate(0, p = 1, 0)
	}
	if (cidx >= 0) {
		City& C = g.cities[size_t(who)][size_t(cidx)];
		C.filled++;
		if (bq > 1) {
			for (int32_t s = 2; s <= bq && s <= 4; s++) {
				if (C.space[size_t(s - 2)] > 0) C.space[size_t(s - 2)]--;
			}
		}
		if (g.frame != 0) {
			if (builder_kind == kGather) {
				C.gatherers--;
				L.gatherers--;
				L.reg_gatherers[size_t(R0 & 63)]--;
			} else {
				C.free--;
				L.free_peasants--;
				L.reg_free_peasants[size_t(R0 & 63)]--;
			}
		}
	}
	return 0;
}

}  // namespace ron::sim
