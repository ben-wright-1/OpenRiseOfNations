#include "sim/trig.hpp"

#include <cmath>

namespace ron::sim {

namespace {

// trig_init @00a46980: t[i] = (int)(sin(i * 1.570796327 / 255.0) * 65535.0), in double.
struct SineTable {
	int32_t t[256];
	SineTable() {
		for (int i = 0; i < 256; i++) t[i] = int32_t(std::sin(double(i) * 1.570796327 / 255.0) * 65535.0);
	}
};
const SineTable& sine() {
	static const SineTable s;
	return s;
}

inline int32_t imul(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); }  // 32-bit wrapping IMUL

}  // namespace

int32_t vector_dist(int32_t dx, int32_t dy) {
	const uint32_t a = uint32_t(dx < 0 ? -dx : dx), b = uint32_t(dy < 0 ? -dy : dy);
	uint32_t M, m;
	if (int32_t(a) > int32_t(b)) {
		M = a;
		m = b;
	} else {
		M = b;
		m = a;
	}
	if (M == 0) return 0;
	if (m >= 0xea60u) return int32_t((m + 2 * M) >> 1);
	return int32_t((m * m) / (2 * M) + M);
}

uint32_t find_angle(int32_t dx, int32_t dy) {
	const int32_t ny = -dy;
	if (dx == 0) return ny > 0 ? 0u : 0x80000000u;
	if (ny == 0) return dx > 0 ? 0x40000000u : 0xC0000000u;
	const int32_t a = dx < 0 ? -dx : dx, b = ny < 0 ? -ny : ny;
	bool xdom;
	int32_t q;
	if (a > b) {
		xdom = true;
		q = int32_t(uint32_t(b) << 14) / a;
	} else {
		xdom = false;
		q = int32_t(uint32_t(a) << 14) / b;
	}
	int32_t t = 0x1333 - q;
	if (t < 0) t = -t;
	const int32_t c = imul(t, 0xb00) >> 14;
	const uint32_t e = (uint32_t(imul(0x2800 - c, q)) & 0xffffc000u) << 2;
	if (dx > 0) {
		if (ny > 0) return xdom ? 0x40000000u - e : e;
		return xdom ? 0x40000000u + e : 0x80000000u - e;
	}
	if (ny > 0) return xdom ? 0xC0000000u + e : 0u - e;
	return xdom ? 0xC0000000u - e : 0x80000000u + e;
}

int32_t sin_table(uint32_t a, int32_t r) {
	const int32_t* t = sine().t;
	const int32_t rr = int32_t(a) < 0 ? -r : r;
	const bool quad2 = (a & 0x40000000u) != 0;
	const uint32_t i = (a & 0x3fffffffu) >> 22;
	const int32_t f = int32_t(a & 0x3fffffu);
	const int32_t ti = t[i], tn = t[(i + 1) & 0xff];
	const int32_t interp = imul(tn - ti, f) >> 22;
	const int32_t v = quad2 ? interp - ti + 0xffff : interp + ti;
	if (rr < 0xffff) return imul(v, rr) >> 16;
	if (rr < 0xffffff) return imul(v, rr >> 8) >> 8;
	return imul(v, rr >> 16);
}

int32_t sin_a(uint32_t a, int32_t r) {
	if (r == 0) return 0;
	if (int32_t(a) < 0) {
		r = -r;
		a &= 0x7fffffffu;
	}
	return sin_table((a & 0x40000000u) ? 0x7fffffffu - a : a, r);
}

int32_t cos_a(uint32_t a, int32_t r) { return sin_a(a + 0x40000000u, r); }

}  // namespace ron::sim
