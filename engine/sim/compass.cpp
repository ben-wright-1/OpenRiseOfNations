#include "sim/compass.hpp"

namespace ron::sim {

#include "sim/compass_table.inc"

namespace {

// The approximate distance circle_init and ring_init use (unsigned integer division).
uint32_t circle_dist(uint32_t ax, uint32_t ay) {
	if (ay < ax) return ay < 60000 ? ay * ay / (ax * 2) + ax : (ay + ax * 2) >> 1;
	if (ay == 0) return 0;
	return ax < 60000 ? ax * ax / (ay * 2) + ay : (ax + ay * 2) >> 1;
}

uint32_t abs32(int32_t v) { return uint32_t(v < 0 ? -v : v); }

// circle_init @006817f0: x outer (-r..r), y inner. Once the table holds kCircleCap points the current and every later
// radius get the count and the build stops (inside ring 64).
Circles make_circles() {
	Circles c;
	c.x.reserve(kCircleCap);
	c.y.reserve(kCircleCap);
	for (int r = 0; r <= 64; r++) {
		for (int x = -r; x <= r; x++) {
			for (int y = -r; y <= r; y++) {
				if (circle_dist(abs32(x), abs32(y)) != uint32_t(r)) continue;
				c.x.push_back(int8_t(x));
				c.y.push_back(int8_t(y));
				if (int32_t(c.x.size()) >= kCircleCap) {
					for (int k = r; k <= 64; k++) c.radius[size_t(k)] = int32_t(c.x.size());
					return c;
				}
			}
		}
		c.radius[size_t(r)] = int32_t(c.x.size());
	}
	return c;
}

// ring_init @00681920. For each ring r = 1..32: copy circle ring r, then examine its original points in order: count
// the 4-neighbours (Manhattan distance 1) among every entry of the ring so far (originals and fillers appended up to
// now), +1 for a point on an axis; below 2, append each N/E/S/W neighbour (orthog order) whose circle distance is < r.
// Fillers are searched as neighbours by later points but not examined; duplicates are kept.
Rings make_rings(const Circles& c) {
	Rings g;
	g.x.push_back(0);
	g.y.push_back(0);
	g.radius[0] = 1;
	for (int32_t r = 1; r <= 32; r++) {
		const int32_t ring_start = g.radius[size_t(r - 1)];
		for (int32_t i = c.radius[size_t(r - 1)]; i < c.radius[size_t(r)]; i++) {
			g.x.push_back(c.x[size_t(i)]);
			g.y.push_back(c.y[size_t(i)]);
		}
		const int32_t orig_end = int32_t(g.x.size());
		for (int32_t i = ring_start; i < orig_end; i++) {
			const int32_t px = g.x[size_t(i)], py = g.y[size_t(i)];
			int32_t n = 0;
			for (int32_t j = ring_start; j < int32_t(g.x.size()); j++) {
				if (int32_t(abs32(py - g.y[size_t(j)]) + abs32(px - g.x[size_t(j)])) == 1) n++;
			}
			if (px == 0 || py == 0) n++;
			if (n >= 2) continue;
			for (int k = 1; k <= 4; k++) {
				const int32_t nx = px + orthog_x[k], ny = py + orthog_y[k];
				if (int32_t(circle_dist(abs32(nx), abs32(ny))) < r) {
					g.x.push_back(int8_t(nx));
					g.y.push_back(int8_t(ny));
				}
			}
		}
		g.radius[size_t(r)] = int32_t(g.x.size());
	}
	return g;
}

}  // namespace

const Circles& circles() {
	static const Circles c = make_circles();
	return c;
}

const Rings& rings() {
	static const Rings g = make_rings(circles());
	return g;
}

}  // namespace ron::sim
