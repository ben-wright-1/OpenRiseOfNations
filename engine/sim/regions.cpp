#include "sim/regions.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ron::sim {

namespace {

// Regions are landmasses and water bodies: water = is_ocean (half-water wcoords count as land).
bool water(const WData& d) { return World::is_ocean(d); }

bool bit(uint64_t m, int32_t b) { return ((m >> b) & 1u) != 0; }

}  // namespace

Regions::Regions() {
	for (int32_t i = 0; i < kNum; i++) r[size_t(i)].region = i;
}

void Regions::clear_all(World& w) {
	for (Region& g : r) {
		g.flags = 0;
		g.climate = 0;
		g.fertile = 0;
		g.common_factor = 8;
		g.goody_factor = 8;
		if (g.size != 0) {
			g.size = 0;
			g.coords.clear();
		}
	}
	for (WData& d : w.wdata) d.region = 0;
	land = 0;
	sea = kFirstSea;
}

void Regions::release_recalc(World& w) {
	for (int32_t i = 0; i < kFirstSea; i++) {
		Region& g = r[size_t(i)];
		if (!(g.flags & region_flag::kRecalc)) continue;
		for (int32_t k = 0; k < g.size && size_t(k) < g.coords.size(); k++) w.w(g.coords[size_t(k)].x, g.coords[size_t(k)].y).region = 0;
		g.size = 0;
		g.coords.clear();
	}
}

// Regions::find @00680180: breadth-first, 8-connected, same water-ness, unlabelled cells only; size = cells popped.
void Regions::find(World& w, int32_t x, int32_t y, int32_t id) {
	const bool t = water(w.w(x, y));
	Region& g = r[size_t(id)];
	g.size = 0;
	w.w(x, y).region = int16_t(id);
	std::vector<WCoord> q{{x, y}};
	for (size_t out = 0; out < q.size(); out++) {
		const WCoord c = q[out];
		g.size++;
		for (int k = 1; k <= 8; k++) {
			const int32_t nx = c.x + move_x[k], ny = c.y + move_y[k];
			if (!w.in_w(nx, ny)) continue;
			WData& n = w.w(nx, ny);
			if (water(n) == t && n.region == 0) {
				n.region = int16_t(id);
				q.push_back({nx, ny});
			}
		}
	}
}

void Regions::find_all(World& w) {
	for (int32_t y = 0; y < w.ys; y++) {
		for (int32_t x = 0; x < w.xs; x++) {
			WData& d = w.w(x, y);
			if (d.region != 0) continue;
			const bool t = water(d);
			// Join an adjacent labelled cell of the same kind (never fires in a full rebuild).
			bool joined = false;
			for (int k = 1; k <= 8 && !joined; k++) {
				const int32_t nx = x + move_x[k], ny = y + move_y[k];
				if (!w.in_w(nx, ny)) continue;
				const WData& n = w.w(nx, ny);
				if (water(n) == t && n.region != 0 && n.region != 0x7f) {
					d.region = n.region;
					r[size_t(n.region)].size++;
					joined = true;
				}
			}
			if (joined) continue;
			int32_t id, lo, hi;
			if (!t) {
				id = 1;
				while (id < 64 && r[size_t(id)].size != 0) id++;
				id = std::clamp(id, 0, kLandOverflow - 1);
				lo = 0;
				hi = kLandOverflow - 1;
			} else {
				sea++;
				id = std::clamp(sea, kFirstSea, kSeaOverflow - 1);
				lo = kFirstSea;
				hi = kSeaOverflow - 1;
			}
			find(w, x, y, id);
			r[size_t(id)].climate = 0;
			if (id == hi) {
				// Overflow: fold the smallest region (running minimum below this one's size) into the overflow bucket
				// and give its id to this region. Region 0 / 0x40 are always empty, so they are picked first; for land
				// that relabels the unvisited cells (needs 62 land bodies; no stock map comes close).
				const int32_t S = r[size_t(id)].size;
				int32_t m = -1, min = S;
				for (int32_t k = lo; k < hi; k++) {
					if (r[size_t(k)].size < min) {
						m = k;
						min = r[size_t(k)].size;
					}
				}
				auto relabel = [&](int32_t a, int32_t b) {
					for (WData& c : w.wdata) {
						if (c.region == a) c.region = int16_t(b);
					}
				};
				if (m >= 0) {
					r[size_t(hi + 1)].size += r[size_t(m)].size;
					r[size_t(m)].size = 0;
					relabel(m, hi + 1);
					r[size_t(m)].size += r[size_t(id)].size;
					r[size_t(id)].size = 0;
					relabel(id, m);
				} else {
					r[size_t(hi + 1)].size += S;
					r[size_t(id)].size = 0;
					relabel(id, hi + 1);
				}
			}
		}
	}
	// region2 of half-water wcoords: the region of the first open-water 8-neighbour (x outer, y inner).
	for (int32_t x = 0; x < w.xs; x++) {
		for (int32_t y = 0; y < w.ys; y++) {
			WData& d = w.w(x, y);
			if (!(d.flags & wflag::kHalfLand) || d.region2 != 0) continue;
			for (int k = 1; k <= 8; k++) {
				const int32_t nx = x + move_x[k], ny = y + move_y[k];
				if (!w.in_w(nx, ny)) continue;
				const WData& n = w.w(nx, ny);
				if (water(n)) {
					d.region2 = n.region;
					break;
				}
			}
		}
	}
	set_coastals(w);
	sort_regions();
	rebuild_coords(w);
}

// Regions::set_coastals @0067fd70
void Regions::set_coastals(const World& w) {
	for (int32_t i = 0; i < kNum - 1; i++) {
		r[size_t(i)].coast = 0;
		r[size_t(i)].coastal = 0;
	}
	for (int32_t y = 0; y < w.ys; y++) {
		for (int32_t x = 0; x < w.xs; x++) {
			const int32_t me = w.w(x, y).region;
			if (me >= kFirstSea) continue;
			for (int k = 1; k <= 8; k++) {
				const int32_t nx = x + move_x[k], ny = y + move_y[k];
				if (!w.in_w(nx, ny)) continue;
				const int32_t s = w.w(nx, ny).region - kFirstSea;
				if (s < 0 || s >= 64) continue;
				r[size_t(me)].coast |= uint64_t(1) << s;
				// Land regions within Chebyshev rings 2 and 3 of this land cell are coastal to each other.
				for (int j = 9; j <= 48; j++) {
					const int32_t mx = x + move_x[j], my = y + move_y[j];
					if (!w.in_w(mx, my)) continue;
					const int32_t L = w.w(mx, my).region;
					if (L < kFirstSea && L != me) {
						r[size_t(me)].coastal |= uint64_t(1) << L;
						r[size_t(L)].coastal |= uint64_t(1) << me;
					}
				}
			}
		}
	}
	for (int32_t me = 0; me < 64; me++) finalize_coastals(me);
}

// Region::finalize_coastals @006805b0: j joins me.coastal when me is coastal to i, i to j, and all three touch a
// common sea; adding a j <= i restarts the scan at j.
void Regions::finalize_coastals(int32_t me) {
	Region& M = r[size_t(me)];
	int32_t i = 0;
	while (i < 64) {
		int32_t restart = -1;
		if (i != me && bit(M.coastal, i)) {
			for (int32_t j = 0; j < 64; j++) {
				if (j == me || j == i) continue;
				if (!(bit(r[size_t(i)].coastal, j) && !bit(M.coastal, j))) continue;
				for (int32_t b = 0; b < 63; b++) {
					if (bit(M.coast, b) && bit(r[size_t(i)].coast, b) && bit(r[size_t(j)].coast, b)) {
						M.coastal |= uint64_t(1) << j;
						r[size_t(j)].coastal |= uint64_t(1) << me;
						if (restart < 0 && j <= i) restart = j;
					}
				}
			}
		}
		i = restart >= 0 ? restart : i + 1;
	}
}

// Regions::sort_regions @0067fb90: rank = position in a stable sort by descending size, land 0..63 and sea
// 0x40..0x7e separately.
void Regions::sort_regions() {
	auto rank = [&](int32_t from, int32_t to) {
		std::vector<int32_t> ids;
		for (int32_t k = from; k < to; k++) ids.push_back(k);
		std::stable_sort(ids.begin(), ids.end(), [&](int32_t a, int32_t b) { return r[size_t(a)].size > r[size_t(b)].size; });
		for (size_t k = 0; k < ids.size(); k++) r[size_t(ids[k])].rank = int32_t(k);
	};
	rank(0, kFirstSea);
	rank(kFirstSea, kNum - 1);
}

// Regions::rebuild_coords @0067f800: raster order (y outer); a count mismatch is the original's assert.
void Regions::rebuild_coords(const World& w) {
	for (Region& g : r) g.coords.clear();
	for (int32_t y = 0; y < w.ys; y++) {
		for (int32_t x = 0; x < w.xs; x++) r[size_t(w.w(x, y).region)].coords.push_back({x, y});
	}
	for (const Region& g : r) {
		if (int32_t(g.coords.size()) != g.size) throw std::runtime_error("Region Size mismatch " + std::to_string(g.region));
	}
}

bool Regions::is_coast(int32_t a, int32_t b) const {
	if (a == b) return true;
	if (a < kFirstSea && b >= kFirstSea) return bit(r[size_t(a)].coast, b - kFirstSea);
	if (a >= kFirstSea && b < kFirstSea) return bit(r[size_t(b)].coast, a - kFirstSea);
	return false;
}

int32_t Regions::get_num_sea() const {
	int32_t n = 0;
	for (int32_t i = kFirstSea; i < kNum; i++) n += r[size_t(i)].size != 0;
	return n;
}

}  // namespace ron::sim
