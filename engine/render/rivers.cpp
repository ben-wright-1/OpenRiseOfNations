#include "render/rivers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace ron {

namespace {

using V3 = std::array<float, 3>;

// River parameters (RiversOut::one_time_init console variables).
constexpr float kMaxWidth = 0.75f;          // wcoords
constexpr float kMaxDepth = -60.0f;
constexpr float kSectionsPerWcoord = 1.7f;
constexpr float kProfile[9] = {1, 0.75f, 0.26f, 0, 0, 0, 0.26f, 0.75f, 1};  // bank -> centre -> bank

int tcoord_of(float v) {  // as the exe computes it: floor(floor(trunc(v) / 64) / 3)
	const int iv = int(v);
	return int(std::floor(std::floor(double(iv) / 64.0) / 3.0));
}

struct Lcg {
	uint32_t s;
	int operator()(int lo, int hi) {
		s = s * 0x19660Du + 0x3C6EF35Fu;
		return lo + int((int64_t(s & 0xFFFF) * int64_t(hi - lo)) >> 16);
	}
};

// Fractal::init(xs, ys, smooth, flags, seed) + get_height: midpoint displacement on an (xs+1) x (ys+1) byte grid.
class Fractal {
public:
	Fractal(int xs, int ys, int smooth, int flags, uint32_t seed) : col_(ys + 1), flags_(flags), f_(size_t(xs + 1) * size_t(ys + 1), 0) {
		Lcg rnd{seed};
		const int f1 = flags & 1;
		smooth = std::clamp(smooth, 0, 5);
		auto clamp255 = [](int v) { return uint8_t(std::clamp(v, 0, 255)); };
		for (int L = smooth; L >= 0; L--) {
			const int mask = (1 << (L + 1)) - 1, s = 1 << L, amp = 1 << (7 - smooth + L), span = (1 << (8 - smooth + L)) - 1;
			if (!f1) {
				for (int y = 0; y <= ys; y++) at(xs, y) = at(0, y);
			}
			for (int cx = 0; cx < (xs >> L) + f1; cx++) {
				for (int cy = 0; cy < (ys >> L); cy++) {
					const int X = cx << L, Y = cy << L;
					if (L == smooth) {
						at(X, Y) = uint8_t(rnd(0, 255));
						continue;
					}
					if ((mask & X) == 0) {
						if ((mask & Y) == 0) continue;
						at(X, Y) = clamp255(((at(X, Y + s) + at(X, Y - s) + 1) >> 1) + rnd(0, span) - amp);
					} else if ((mask & Y) == 0) {
						at(X, Y) = clamp255(((at(X + s, Y) + 1 + at(X - s, Y)) >> 1) + rnd(0, span) - amp);
					} else {
						at(X, Y) = clamp255(((at(X - s, Y - s) + at(X + s, Y + s) + at(X - s, Y + s) + at(X + s, Y - s) + 2) >> 2) + rnd(0, span) - amp);
					}
				}
			}
		}
		if (!f1) {
			for (int y = 0; y <= ys; y++) at(xs, y) = at(0, y);
		}
		xinc_ = double(xs) / double(xs - f1 + 1);
	}
	int operator()(int x, int y) const {
		const double fx = xinc_ * (x + 0.5), fy = 1.0 * (y + 0.5);
		const int ix = int(fx), iy = int(fy);
		const double dx = fx - ix, dy = fy - iy;
		double v = (1 - dx) * (1 - dy) * get(ix, iy) + 0.0;
		v += (1 - dy) * dx * get(ix + 1, iy);
		v += (1 - dx) * dy * get(ix, iy + 1);
		v += get(ix + 1, iy + 1) * dy * dx;
		const int i = std::clamp(int(v), 0, 255);
		return (flags_ & 2) ? (i * 100) >> 8 : i;
	}

private:
	uint8_t& at(int x, int y) { return f_[size_t(x) * size_t(col_) + size_t(y)]; }
	double get(int x, int y) const { return f_[size_t(x) * size_t(col_) + size_t(y)]; }
	int col_, flags_;
	double xinc_ = 1;
	std::vector<uint8_t> f_;
};

// Spline type 3: clamped uniform B-spline, Cox-de Boor with the exe's guards, in single precision.
std::vector<V3> b_spline(const std::vector<V3>& P, int degree, int segs) {
	const int n = int(P.size());
	if (n <= degree) degree = n - 1;
	if (segs < degree) segs = degree;
	std::vector<float> knots;
	float k = 0;
	for (int i = 0; i <= n + degree; i++) {
		if (i > degree && i <= n) k += 1;
		knots.push_back(k);
	}
	const int m = int(knots.size());
	const float last = knots[size_t(m - 1)];
	std::function<float(float, int, int)> basis = [&](float t, int i, int d) -> float {
		if (i + d >= m) return 1;
		if (d == 0) return knots[size_t(i)] <= t && t < knots[size_t(i + 1)] ? 1.0f : 0.0f;
		float a = 0;
		const float d1 = t - knots[size_t(i)], w1 = knots[size_t(i + d)] - knots[size_t(i)];
		if (d1 > 0 && w1 > 0) a = basis(t, i, d - 1) * (d1 / w1);
		const float d2 = knots[size_t(i + d + 1)] - t, w2 = knots[size_t(i + d + 1)] - knots[size_t(i + 1)];
		if (d2 > 0 && w2 > 0) a = a + basis(t, i + 1, d - 1) * (d2 / w2);
		return a;
	};
	const float step = (last - knots[0]) / float(segs);
	std::vector<V3> out{P[0]};
	float t = knots[0] + step;
	int span = 0;
	while (last > t) {
		int i = span;
		while (i + 1 < m) {
			if (!(t > knots[size_t(i + 1)]) && knots[size_t(i)] < knots[size_t(i + 1)]) break;
			i++;
		}
		span = i;
		if (span > m - 1) break;
		V3 p{0, 0, 0};
		for (int j = std::max(0, span - degree); j <= span && j < n; j++) {
			const float N = basis(t, j, degree);
			for (int c = 0; c < 3; c++) p[size_t(c)] = p[size_t(c)] + P[size_t(j)][size_t(c)] * N;
		}
		out.push_back(p);
		t = t + step;
	}
	if (last - (t - step) > 0.001f) out.push_back(P[size_t(n - 1)]);
	return out;
}

V3 norm3(V3 v) {
	const float l2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	if (l2 == 0 || l2 == 1) return v;
	const float s = 1.0f / std::sqrt(l2);
	return {v[0] * s, v[1] * s, v[2] * s};
}

float dist3(const V3& a, const V3& b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

// Spline::next_at_dist: walks the evaluated polyline by arc length.
class Walker {
public:
	Walker(const std::vector<V3>& pts, const std::vector<V3>& normals, float length) : pts_(pts), normals_(normals), length_(length) {
		if (pts.empty() || length == 0) acc_ = -1, idx_ = -1;
	}
	bool next(float step, V3& p, V3& n) {
		if (acc_ < 0 || idx_ < 0) return false;
		idx_ += 1;
		target_ += step;
		const float thr = step * 0.33f;
		float s = -1;
		while (idx_ < int(pts_.size()) && target_ > acc_) {
			s = dist3(pts_[size_t(idx_)], pts_[size_t(idx_ - 1)]);
			acc_ += s;
			idx_++;
		}
		idx_ -= 1;
		if (acc_ > target_ && idx_ < int(pts_.size()) && !(length_ - target_ < thr)) {
			if (s < 0) s = dist3(pts_[size_t(idx_)], pts_[size_t(idx_ - 1)]);
			const float f = s == 0 ? 0 : (acc_ - target_) / s;
			for (size_t c = 0; c < 3; c++) {
				p[c] = pts_[size_t(idx_)][c] + (pts_[size_t(idx_ - 1)][c] - pts_[size_t(idx_)][c]) * f;
				n[c] = normals_[size_t(idx_)][c] + (normals_[size_t(idx_ - 1)][c] - normals_[size_t(idx_)][c]) * f;
			}
			n = norm3(n);
			return true;
		}
		p = pts_[size_t(idx_)];
		n = normals_[size_t(idx_)];
		if (idx_ + 1 < int(pts_.size()) && acc_ < length_) return true;
		acc_ = length_;
		idx_ = -1;
		return false;
	}

private:
	const std::vector<V3>& pts_;
	const std::vector<V3>& normals_;
	float length_, acc_ = 0, target_ = 0;
	int idx_ = 0;
};

struct EdgeVertex {
	float x, y, z, u, v;
	uint8_t alpha;
};

std::array<EdgeVertex, 9> edge(const Terrain& t, const V3& P, const V3& N, float width, float v, int flag) {
	std::array<EdgeVertex, 9> out{};
	const float s[2] = {width * N[0] * 2 * 768 * 0.125f, width * N[1] * 2 * 768 * 0.125f};  // N x width x 192
	const float lift = kMaxDepth * 0.25f * -1;
	float x = P[0] - s[0] * 4, y = P[1] - s[1] * 4;
	const uint8_t color = flag ? ((flag & 0x800) ? 0x88 : 0) : 255;
	const float map_w = float(t.xs) * kWCoordSize;
	const int txs = t.xs * 4, tys = t.ys * 4;
	for (int k = 0; k < 9; k++) {
		float z = kMaxDepth - kProfile[k] * kMaxDepth + lift;
		const int tx = tcoord_of(x), ty = tcoord_of(y);
		if (tx >= 0 && ty >= 0 && tx < txs && ty < tys) {
			const bool sea = (t.tcoord_mask[size_t(ty) * size_t(txs) + size_t(tx)] & tfile::kWater) != 0;
			if (sea && z < 0 && std::abs(k - 4) <= 1) z = ((z - 30) + z) * 0.5f;
			out[size_t(k)] = {x, y, z, float(k) * 0.125f, v, color};
		} else {
			out[size_t(k)] = {std::clamp(x, 0.0f, map_w), std::clamp(y, 0.0f, map_w), z, float(k) * 0.125f, v, 0};
		}
		x += s[0];
		y += s[1];
	}
	return out;
}

}  // namespace

RiverMesh build_rivers(const Terrain& t) {
	RiverMesh out;
	if (t.tcoord_mask.empty()) return out;
	const Fractal frac(50, 50, 2, 2, 0);  // River::one_time_init
	for (const auto& nodes : t.rivers) {
		std::vector<V3> P;
		for (const auto& n : nodes) P.push_back({float(n[0]) * kWCoordSize + kWCoordSize / 2, float(n[1]) * kWCoordSize + kWCoordSize / 2, 0});
		if (P.size() < 2) continue;
		float len_w = 0;
		for (size_t i = 1; i < P.size(); i++) {
			len_w += std::sqrt((P[i][1] - P[i - 1][1]) * (P[i][1] - P[i - 1][1]) + (P[i][0] - P[i - 1][0]) * (P[i][0] - P[i - 1][0])) / 768.0f;
		}
		const int segs = int(len_w * kSectionsPerWcoord) & 0xFFFF;
		const std::vector<V3> pts = b_spline(P, 3, segs);
		if (pts.size() < 2) continue;
		// Spline::build_normals (up = +z): side vector (dy, -dx, 0), averaged with the previous segment's.
		std::vector<V3> normals;
		V3 prev{0, 0, 0};
		float length = 0;
		for (size_t j = 1; j < pts.size(); j++) {
			const V3 d{pts[j][0] - pts[j - 1][0], pts[j][1] - pts[j - 1][1], pts[j][2] - pts[j - 1][2]};
			length += std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			const V3 n = norm3({d[1], -d[0], 0});
			normals.push_back(norm3({prev[0] + n[0], prev[1] + n[1], prev[2] + n[2]}));
			prev = n;
		}
		normals.push_back(prev);
		Walker walk(pts, normals, length);
		const float step = 768.0f / kSectionsPerWcoord;
		V3 A, nA, B, nB;
		walk.next(0, A, nA);
		int a0 = 0, a4 = 0, cc = -1, index = 0;
		float prev_w = kMaxWidth;
		bool ocean = false, first = true;
		struct Section {
			std::array<EdgeVertex, 27> verts;
			int index, flags;
			float v0, v1;
		};
		std::vector<Section> sections;
		for (;;) {
			if (++a0 > 49) {
				a4++, a0 = 0;
				if (a4 > 49) a4 = 0;
			}
			const float w0 = prev_w;
			float w = ocean ? std::min(kMaxWidth + kMaxWidth, w0 * 1.5f) : (float(frac(a0, a4)) * 0.005f + 0.5f) * kMaxWidth;
			prev_w = w;
			const bool more = walk.next(step, B, nB);
			float v0 = (index & 1) ? 0.5f : 0.0f, v1 = (index & 1) ? 1.0f : 0.5f;
			if (ocean && (cc & 1)) v0 = (index & 1) ? 0.0f : 0.5f, v1 = (index & 1) ? 0.5f : 1.0f;
			int flags = more ? 0 : 0x544;
			if (ocean && index == cc + 1) flags |= 0x204;
			if (first) flags |= 0x11, first = false;
			if (!ocean) {
				const int mx = tcoord_of((A[0] + B[0]) * 0.5f), my = tcoord_of((A[1] + B[1]) * 0.5f);
				const int txs = t.xs * 4, tys = t.ys * 4;
				if (mx >= 0 && my >= 0 && mx < txs && my < tys && (t.tcoord_mask[size_t(my) * size_t(txs) + size_t(mx)] & tfile::kWater)) {
					ocean = true;
					cc = index;
					if (index & 1) v0 = 0, v1 = 0.5f;
					w *= 1.5f;
					prev_w = w;
				}
			} else if (cc + 1 < index) {
				flags |= 7;
			}
			// Three cross-sections: start, middle (side vector not renormalised: narrower on bends), end.
			const V3 mid{(A[0] + B[0]) * 0.5f, (A[1] + B[1]) * 0.5f, (A[2] + B[2]) * 0.5f};
			const V3 n_mid{(nA[0] + nB[0]) * 0.5f, (nA[1] + nB[1]) * 0.5f, (nA[2] + nB[2]) * 0.5f};
			const int mid_flag = (flags & 6) == 4 ? (flags & 2) | 0x200 : flags & 2;
			Section sec{};
			sec.index = index, sec.flags = flags, sec.v0 = v0, sec.v1 = v1;
			const auto e0 = edge(t, A, nA, w0, v0, flags & 1), e1 = edge(t, mid, n_mid, (w0 + w) * 0.5f, (v0 + v1) * 0.5f, mid_flag),
			           e2 = edge(t, B, nB, w, v1, flags & 4);
			std::copy(e0.begin(), e0.end(), sec.verts.begin());
			std::copy(e1.begin(), e1.end(), sec.verts.begin() + 9);
			std::copy(e2.begin(), e2.end(), sec.verts.begin() + 18);
			sections.push_back(sec);
			A = B, nA = nB;
			index++;
			if (!more) break;
		}
		for (const Section& s : sections) {
			const int tex = cc != -1 && cc <= s.index ? ((cc & 1) ? 2 : 1) : 0;
			auto& verts = out.trough[tex];
			auto& idx = out.trough_indices[tex];
			const uint32_t base = uint32_t(verts.size());
			for (const EdgeVertex& e : s.verts) verts.push_back({e.x, e.y, e.z, 0, 0, 1, e.u, e.v, 255, 255, 255, e.alpha});
			for (int row = 0; row < 18; row += 9) {
				for (int k = 0; k < 8; k++) {
					const uint32_t a = base + uint32_t(row + k);
					idx.insert(idx.end(), {a, a + 1, a + 9, a + 9, a + 1, a + 10});
				}
			}
			// The water quad: start k = 1 and 7, end k = 1 and 7, at z = 0.
			const uint8_t al0 = (s.flags & 0x10) ? 0 : 255, al1 = (s.flags & 0x40) ? 0 : 255;
			const EdgeVertex* q[4] = {&s.verts[1], &s.verts[7], &s.verts[19], &s.verts[25]};
			const float qu[4] = {0, 1, 0, 1}, qv[4] = {s.v0, s.v0, s.v1, s.v1};
			const uint8_t qa[4] = {al0, al0, al1, al1};
			const uint32_t wb = uint32_t(out.water.size());
			for (int i = 0; i < 4; i++) {
				ColorVertex2 v{};
				v.base = {q[i]->x, q[i]->y, 0, 0, 0, 1, q[i]->x / kWCoordSize, q[i]->y / kWCoordSize, 255, 255, 255, qa[i]};
				v.u1 = qu[i], v.v1 = qv[i];
				out.water.push_back(v);
			}
			out.water_indices.insert(out.water_indices.end(), {wb, wb + 1, wb + 2, wb + 2, wb + 1, wb + 3});
		}
	}
	return out;
}

}  // namespace ron
