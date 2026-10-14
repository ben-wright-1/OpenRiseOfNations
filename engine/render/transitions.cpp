#include "render/transitions.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <utility>

namespace ron {

namespace {

// move_x / move_y: NW N NE E SE S SW W, +y = south.
constexpr int kMoveX[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
constexpr int kMoveY[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
constexpr uint32_t kRev3[8] = {0, 4, 2, 6, 1, 5, 3, 7};  // bit_reversal

std::array<uint32_t, 8> orientation_codes(uint32_t c) {
	const uint32_t a = c & 7, b = (c >> 3) & 7, cc = (c >> 6) & 7, d = (c >> 9) & 7;
	auto g = [](uint32_t t, uint32_t r, uint32_t bo, uint32_t l) { return (t << 9) | (r << 6) | (bo << 3) | l; };
	const uint32_t Ra = kRev3[a], Rb = kRev3[b], Rc = kRev3[cc], Rd = kRev3[d];
	return {g(d, cc, b, a), g(a, d, cc, b), g(b, a, d, cc), g(cc, b, a, d),
	        g(Rd, Ra, Rb, Rc), g(Rc, Rd, Ra, Rb), g(Rb, Rc, Rd, Ra), g(Ra, Rb, Rc, Rd)};
}

// Fractal::init(xs+1, ys+1, smooth 0, flags 2, seed) + Fractal::get_height: h(wx, wy) in 0..99.
class TextureFractal {
public:
	TextureFractal(int world_xs, int world_ys, uint32_t seed) : xs_(world_xs + 1), ys_(world_ys + 1), col_(ys_ + 1) {
		f_.assign(size_t(xs_ + 1) * size_t(col_), 0);
		uint32_t s = seed;
		for (int x = 0; x < xs_; x++) {
			for (int y = 0; y < ys_; y++) {
				s = s * 0x19660Du + 0x3C6EF35Fu;
				f_[size_t(x) * size_t(col_) + size_t(y)] = uint8_t(((s & 0xFFFF) * 255u) >> 16);
			}
		}
		for (int y = 0; y <= ys_; y++) f_[size_t(xs_) * size_t(col_) + size_t(y)] = f_[size_t(y)];  // column xs = column 0
		xinc_ = double(xs_) / double(xs_ + 1);
	}
	int operator()(int x, int y) const {
		const double fx = xinc_ * (x + 0.5), fy = 1.0 * (y + 0.5);
		const int ix = int(fx), iy = int(fy);
		const double dx = fx - ix, dy = fy - iy;
		auto at = [&](int a, int b) { return double(f_[size_t(a) * size_t(col_) + size_t(b)]); };
		double v = (1 - dx) * (1 - dy) * at(ix, iy) + 0.0;
		v += (1 - dy) * dx * at(ix + 1, iy);
		v += (1 - dx) * dy * at(ix, iy + 1);
		v += at(ix + 1, iy + 1) * (dy * dx);
		const int i = std::clamp(int(v), 0, 255);
		return (i * 100) >> 8;
	}

private:
	int xs_, ys_, col_;
	double xinc_ = 1;
	std::vector<uint8_t> f_;
};

// Transitions::find_map's index choice, in single precision.
int pick(int h, int n) {
	float v = float(h) * (float(n) / 100.0f);
	if (v >= float(n) - 0.5f) v = float(n - 1);
	return int(v);
}

}  // namespace

std::vector<TransitionMask> load_transition_masks(const GameFiles& files) {
	std::vector<TransitionMask> out;
	for (const std::string& rel : files.list_dir("terrain art/transitions")) {  // lowercase, sorted: case-insensitive order
		const std::string name = rel.substr(rel.rfind('/') + 1);
		if (name.size() < 5 || name.compare(name.size() - 4, 4, ".tga") != 0) continue;
		const std::string bits = name.substr(0, name.size() - 4);
		if (bits.empty() || bits.find_first_not_of("01") != std::string::npos) continue;
		const uint32_t code = uint32_t(std::strtoul(bits.c_str(), nullptr, 2));
		if (code == 0xEFF) continue;  // Transitions::init skips 111011111111
		out.push_back({rel, code, orientation_codes(code)});
	}
	return out;
}

void transition_mask_uv(float u, float v, int orient, int quad, float& mu, float& mv) {
	float a, b;
	switch (orient) {
		case 0: a = u, b = v; break;
		case 1: a = v, b = 1 - u; break;
		case 2: a = 1 - u, b = 1 - v; break;
		case 3: a = 1 - v, b = u; break;
		case 4: a = 1 - u, b = v; break;
		case 5: a = 1 - v, b = 1 - u; break;
		case 6: a = u, b = 1 - v; break;
		default: a = v, b = u; break;
	}
	mu = a * 0.5f + (quad == 1 || quad == 2 ? 0.5f : 0.0f);
	mv = b * 0.5f + (quad >= 2 ? 0.5f : 0.0f);
}

std::vector<TransitionPatch> build_transitions(const TransitionInput& in) {
	const Terrain& t = *in.terrain;
	const int xs = t.xs, ys = t.ys;
	const std::vector<TransitionMask>& masks = *in.masks;
	const TextureFractal frac(xs, ys, in.seed);
	auto inb = [&](int x, int y) { return x >= 0 && y >= 0 && x < xs && y < ys; };
	auto L = [&](int x, int y) { return int(t.cell(x, y).land); };
	auto S = [&](int x, int y) { return int(t.cell(x, y).land_sub); };

	// TileSet::is_combo_acceptable(l1, s1, l2, s2): 1 = (l1, s1) is directly below (l2, s2), 2 = same or directly above.
	auto combo = [&](int p1, int p2, int p3, int p4) -> int {
		if (p1 == p3) {
			if (std::abs(p4 - p2) < 2) return (p4 <= p2 ? 1 : 0) + 1;
			return 0;
		}
		if (p1 == 0) {
			if (p3 == 1) return p2 == in.coast_base ? 1 : 0;
		} else if (p1 == 1 && p3 == 0) {
			return p4 == in.coast_base ? 2 : 0;
		}
		if (p3 == 2) return 1;
		if (p3 < p1) {
			if (p4 == in.num_tiles[p3] - 1) return p2 != 0 ? 0 : 2;
		} else if (p4 == 0) {
			return p2 == in.num_tiles[p1] - 1 ? 1 : 0;
		}
		return 0;
	};

	// Edge node arrays, with the exe's index formulas. Bits 0-2 fill, 3-5 connect, 6-8 block.
	const size_t n = size_t(std::max(xs, ys) + 2) * size_t(std::max(xs, ys) + 2);
	std::vector<uint32_t> EX(n, 0), EY(n, 0);
	auto ix = [&](int x, int y) { return size_t((ys + 1) * y + x); };  // vertical edge, west side of (x, y)
	auto iy = [&](int x, int y) { return size_t((xs + 1) * x + y); };  // horizontal edge, north side of (x, y)
	auto setX = [&](int x, int y, uint32_t v, int sh) {
		uint32_t& e = EX[ix(x, y)];
		e |= v << sh;
		if (sh == 6) e &= ~(v << 3);
	};
	auto setY = [&](int x, int y, uint32_t v, int sh) {
		uint32_t& e = EY[iy(x, y)];
		e |= v << sh;
		if (sh == 6) e &= ~(v << 3);
	};

	std::vector<int> list;
	std::vector<uint8_t> listed(size_t(xs) * size_t(ys), 0);

	auto tendrils = [&](int x, int y, int l, int s) -> int {
		int lw[8];
		for (int k = 0; k < 8; k++) {
			const int nx = x + kMoveX[k], ny = y + kMoveY[k];
			lw[k] = inb(nx, ny) && combo(L(nx, ny), S(nx, ny), l, s) == 1 ? 1 : 0;
		}
		const int NW = lw[0], N = lw[1], NE = lw[2], E = lw[3], SE = lw[4], Sx = lw[5], SW = lw[6], W = lw[7];
		if (!N) {
			if (!W) {
				if (!NW) {
					if (inb(x, y)) EX[ix(x, y)] |= 4;
					if (inb(x, y)) EY[iy(x, y)] |= 4;
				} else {
					if (inb(x, y)) setX(x, y, 4, 6);
					if (inb(x, y)) setY(x, y, 4, 6);
				}
			}
			if (!E) {
				if (!NE) {
					if (inb(x + 1, y)) EX[ix(x + 1, y)] |= 4;
					if (inb(x, y)) EY[iy(x, y)] |= 1;
				} else {
					if (inb(x + 1, y)) setX(x + 1, y, 4, 6);
					if (inb(x, y)) setY(x, y, 1, 6);
				}
			}
		}
		if (!Sx) {
			if (!W) {
				if (!SW) {
					if (inb(x, y)) EX[ix(x, y)] |= 1;
					if (inb(x, y + 1)) EY[iy(x, y + 1)] |= 4;
				} else {
					if (inb(x, y)) setX(x, y, 1, 6);
					if (inb(x, y + 1)) setY(x, y + 1, 4, 6);
				}
			}
			if (!E) {
				if (!SE) {
					if (inb(x + 1, y)) EX[ix(x + 1, y)] |= 1;
					if (inb(x, y + 1)) EY[iy(x, y + 1)] |= 1;
				} else {
					if (inb(x + 1, y)) setX(x + 1, y, 1, 6);
					if (inb(x, y + 1)) setY(x, y + 1, 1, 6);
				}
			}
		}
		int ret = 0;
		if (N && Sx && !W && !E) {  // one-wide north-south strip
			if (inb(x, y)) setX(x, y, 5, 6);
			if (inb(x + 1, y)) setX(x + 1, y, 5, 6);
			ret = 1;
		} else if (W && E && !N && !Sx) {
			if (inb(x, y)) setY(x, y, 5, 6);
			if (inb(x, y + 1)) setY(x, y + 1, 5, 6);
			ret = 1;
		}
		if (N + Sx + E + W == 3) {  // peninsula
			if (!N) {
				if (inb(x, y)) setY(x, y, 5, 6);
				if (inb(x, y)) EY[iy(x, y)] |= 2;
				if (inb(x, y - 1)) EX[ix(x, y - 1)] |= 0x38;
				if (inb(x + 1, y - 1)) EX[ix(x + 1, y - 1)] |= 0x38;
			} else if (!E) {
				if (inb(x + 1, y)) setX(x + 1, y, 5, 6);
				if (inb(x + 1, y)) EX[ix(x + 1, y)] |= 2;
				if (inb(x + 1, y)) EY[iy(x + 1, y)] |= 0x38;
				if (inb(x + 1, y + 1)) EY[iy(x + 1, y + 1)] |= 0x38;
			} else if (!Sx) {
				if (inb(x, y + 1)) setY(x, y + 1, 5, 6);
				if (inb(x, y + 1)) EY[iy(x, y + 1)] |= 2;
				if (inb(x, y + 1)) EX[ix(x, y + 1)] |= 0x38;
				if (inb(x + 1, y + 1)) EX[ix(x + 1, y + 1)] |= 0x38;
			} else {
				if (inb(x, y)) setX(x, y, 5, 6);
				if (inb(x, y)) EX[ix(x, y)] |= 2;
				if (inb(x - 1, y)) EY[iy(x - 1, y)] |= 0x38;
				if (inb(x - 1, y + 1)) EY[iy(x - 1, y + 1)] |= 0x38;
			}
			ret = 1;
		}
		return ret;
	};

	auto mark = [&](int x, int y) {  // TerrainOut::mark_trans_flags
		const int l = L(x, y), s = S(x, y);
		const size_t me = size_t(y) * size_t(xs) + size_t(x);
		int not_lower = 0, orth = 0, diag = 0;
		for (int k = 0; k < 8; k++) {
			const int dx = kMoveX[k], dy = kMoveY[k], nx = x + dx, ny = y + dy;
			if (!inb(nx, ny)) continue;
			const int c = combo(L(nx, ny), S(nx, ny), l, s);
			if (std::abs(dx) + std::abs(dy) == 2) {
				if (c == 1) diag++;
				continue;
			}
			orth++;
			if (c != 1) {
				not_lower++;
				continue;
			}
			if (listed[me]) continue;  // only the first lower neighbour gets here
			listed[me] = 1;
			list.push_back(int(me));
			setX(x, y, 7, 3);
			if (inb(x + 1, y)) setX(x + 1, y, 7, 3);
			setY(x, y, 7, 3);
			if (inb(x, y + 1)) setY(x, y + 1, 7, 3);
			if (tendrils(x, y, l, s) == 0) {
				if (dx && inb(x + (dx == 1 ? 1 : 0), y)) setX(x + (dx == 1 ? 1 : 0), y, 7, 6);
				if (dy && inb(x, y + (dy == 1 ? 1 : 0))) setY(x, y + (dy == 1 ? 1 : 0), 7, 6);
			}
		}
		if (not_lower == orth && diag != 0) {  // only diagonal lower neighbours
			if (!listed[me]) {
				listed[me] = 1;
				list.push_back(int(me));
				// The exe writes all four to the edge_x array (the last two look meant for edge_y).
				EX[ix(x, y)] |= 0x38;
				if (inb(x + 1, y)) EX[ix(x + 1, y)] |= 0x38;
				EX[ix(x, y)] |= 0x38;
				if (inb(x, y + 1)) EX[ix(x, y + 1)] |= 0x38;
			}
			tendrils(x, y, l, s);
		}
	};

	auto build = [&](int x, int y, int sh) -> uint32_t {  // TerrainOut::build_trans_flags
		auto g = [&](uint32_t v) { return (v >> sh) & 7; };
		return (((g(EY[iy(x, y)]) << 3 | g(EX[ix(x + 1, y)])) << 3 | kRev3[g(EY[iy(x, y + 1)])]) << 3) | kRev3[g(EX[ix(x, y)])];
	};
	struct Side {
		int dx, dy;
		uint32_t m;
	};
	static const Side kSides[4] = {{0, -1, 0xE00}, {1, 0, 0x1C0}, {0, 1, 0x38}, {-1, 0, 7}};  // N E S W

	auto check_blocks = [&](int x, int y, uint32_t b) {
		const int l = L(x, y), s = S(x, y);
		for (const Side& sd : kSides) {
			const int nx = x + sd.dx, ny = y + sd.dy;
			if (!inb(nx, ny)) continue;
			if ((b & sd.m) == sd.m) {
				if (combo(l, s, L(nx, ny), S(nx, ny)) == 1) b &= ~sd.m;
			} else if (combo(L(nx, ny), S(nx, ny), l, s) == 1) {
				b |= sd.m;
			}
		}
		if (b & 0x800) b |= 1;
		if (b & 0x200) b |= 0x100;
		if (b & 0x100) b |= 0x200;
		if (b & 0x40) b |= 0x20;
		if (b & 0x20) b |= 0x40;
		if (b & 8) b |= 4;
		if (b & 4) b |= 8;
		if (b & 1) b |= 0x800;
		return b;
	};
	auto check_connects = [&](int x, int y, uint32_t c, uint32_t b) {
		const int l = L(x, y), s = S(x, y);
		for (const Side& sd : kSides) {
			const int nx = x + sd.dx, ny = y + sd.dy;
			if (!inb(nx, ny)) continue;
			if ((c & sd.m) == 0) {
				if (combo(L(nx, ny), S(nx, ny), l, s) == 1) c = c | ~(b & sd.m);  // sic: a full 32-bit NOT
			} else if (L(nx, ny) != l || S(nx, ny) != s) {
				c &= ~sd.m;
			}
		}
		return c;
	};
	auto check_fills = [&](int x, int y, uint32_t f, uint32_t b) {
		const int l = L(x, y), s = S(x, y);
		for (const Side& sd : kSides) {
			const int nx = x + sd.dx, ny = y + sd.dy;
			if (!inb(nx, ny)) continue;
			if ((f & sd.m) && (L(nx, ny) != l || S(nx, ny) != s)) f &= ~sd.m;
			if (b & sd.m) f &= ~(b & sd.m);
		}
		return f;
	};
	auto check_diagonal = [&](int x, int y, uint32_t& b, uint32_t& f) {  // only called with b == 0
		const int l = L(x, y), s = S(x, y);
		const uint32_t f0 = f;
		auto at = [&](int nx, int ny) { return inb(nx, ny) ? std::make_pair(L(nx, ny), S(nx, ny)) : std::make_pair(0, 0); };
		const auto nN = at(x, y - 1), nE = at(x + 1, y), nS = at(x, y + 1), nW = at(x - 1, y), own = std::make_pair(l, s);
		const bool A = nN == nE && nN != own, B = nN == nW && nN != own;
		const bool C = nS == nE && nS != own, D = nS == nW && nS != own;
		if (A) b = 0xFC0, f |= 0xC;
		if (B) b = 0xE07, f |= 0x60;
		if (C) b = 0x1F8, f |= 0x801;
		if (D) b = 0x3F, f |= 0x300;
		if (A) {
			if (B && C && D) b = 0, f = f0;
		} else if (!B && !C && !D) {
			auto low = [&](int nx, int ny) { return inb(nx, ny) && combo(L(nx, ny), S(nx, ny), l, s) == 1; };
			if (low(x - 1, y - 1)) b = 0x801, f |= 0x1FC;
			if (low(x + 1, y - 1)) b = 0x300, f |= 0x3F;
			if (low(x - 1, y + 1)) b = 0xC, f |= 0xFC0;
			if (low(x + 1, y + 1)) b = 0x60, f |= 0xE07;
		}
	};

	for (int y = 0; y < ys; y++) {
		for (int x = 0; x < xs; x++) mark(x, y);  // TerrainOut::mark_nodes
	}

	std::vector<TransitionPatch> patches;
	for (int i : list) {  // TerrainOut::create_transition_renderables
		const int x = i % xs, y = i / xs, l = L(x, y), s = S(x, y);
		uint32_t block = build(x, y, 6), connect = build(x, y, 3), fill = build(x, y, 0);
		block = check_blocks(x, y, block);
		connect = check_connects(x, y, connect, block);
		fill = check_fills(x, y, fill, block);
		if (block == 0) check_diagonal(x, y, block, fill);
		uint32_t req = fill;
		const bool top = ((req | connect | block) & 0xE00) == 0;
		if (top) req |= 0x400;
		const bool right = ((req | connect | block) & 0x1C0) == 0;
		if (right) req |= 0x80;
		const bool bottom = ((req | connect | block) & 0x38) == 0;
		if (bottom) req |= 0x10;
		const bool left = ((req | connect | block) & 7) == 0;
		if (left) req |= 2;
		if (top) {
			if (left) req |= 0x801;
			if (right) req |= 0x300;
		}
		if (bottom) {
			if (left) req |= 9;  // sic: 9 and 0x30, not 0xc and 0x60
			if (right) req |= 0x30;
		}

		TransitionPatch p;
		p.x = x, p.y = y;
		// The overlay texture (TileSet::get_acceptable_neighbor).
		if (s != 0) p.over_land = l, p.over_sub = s - 1;
		else if (l == 0) p.over_land = 0, p.over_sub = 0;
		else if (l == 1) p.over_land = 0, p.over_sub = in.coast_base;
		else p.over_land = l - 1, p.over_sub = in.num_tiles[l - 1] - 1;

		// Transitions::find_map.
		struct Candidate {
			int mask;
			std::vector<int> orients;
		};
		std::vector<Candidate> cands;
		for (size_t m = 0; m < masks.size(); m++) {
			Candidate c{int(m), {}};
			for (int o = 0; o < 8; o++) {
				const uint32_t v = masks[m].codes[size_t(o)];
				if ((block & v) != 0 || (v & req) != req) continue;
				if ((connect & 7) && !(v & 7)) continue;
				if ((connect & 0x1C0) && !(v & 0x1C0)) continue;
				if ((connect & 0xE00) && !(v & 0xE00)) continue;
				if ((connect & 0x38) && !(v & 0x38)) continue;
				if (!block && !connect && !req && v != 0) continue;
				c.orients.push_back(o);
			}
			if (!c.orients.empty()) cands.push_back(std::move(c));
		}
		if (!cands.empty()) {
			const int h = frac(x, y);
			const Candidate& c = cands[size_t(pick(h, int(cands.size())))];
			p.mask = c.mask;
			p.orient = c.orients[size_t(pick(h, int(c.orients.size())))];
			float qf = float(h) * 0.04f;
			if (qf >= 3.5f) qf = 3;
			p.quad = int(qf);
		}
		patches.push_back(p);
	}
	return patches;
}

}  // namespace ron
