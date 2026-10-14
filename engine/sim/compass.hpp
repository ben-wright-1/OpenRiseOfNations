// Neighbour tables of compass.obj and the coordinate types (docs/world-spec.md sections 1 and 2.4).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ron::sim {

// ---------------------------------------------------------------- coordinates
// Every grid is a floor division of world units (Coord): div_3_table[x >> k] is floor((x >> k) / 3) for negative x too.
constexpr int32_t kWCoordUnits = 768, kTCoordUnits = 192, kUCoordUnits = 48;

inline int32_t floor_div3(int32_t v) { return v >= 0 ? v / 3 : -((2 - v) / 3); }
inline int32_t wcoord_of(int32_t c) { return floor_div3(c >> 8); }  // 768 units
inline int32_t tcoord_of(int32_t c) { return floor_div3(c >> 6); }  // 192 units
inline int32_t ucoord_of(int32_t c) { return floor_div3(c >> 4); }  // 48 units
inline int32_t regcell_of(int32_t c) { return floor_div3(c >> 9); }  // 1536 units (danger cells)
// Grid to grid: shifts (arithmetic, so floor too).
inline int32_t tcoord_to_wcoord(int32_t t) { return t >> 2; }
inline int32_t ucoord_to_tcoord(int32_t u) { return u >> 2; }
inline int32_t ucoord_to_wcoord(int32_t u) { return u >> 4; }
inline int32_t tcoord_to_fcoord(int32_t t) { return t >> 1; }
// WCoord::get_tcorner (NW tcoord), WCoord -> TCoord (the centre tcoord), the wcoord centre in Coords.
inline int32_t wcoord_tcorner(int32_t w) { return 4 * w; }
inline int32_t wcoord_tcentre(int32_t w) { return 4 * w + 2; }
inline int32_t wcoord_centre(int32_t w) { return kWCoordUnits * w + kWCoordUnits / 2; }
// WCoord::traverse_x/y: the i-th (0..15) tcoord of a wcoord, row-major with x fastest. Every "16 tcoords of a
// wcoord" loop of the game uses this order.
inline int32_t traverse_x(int32_t w, int i) { return 4 * w + (i & 3); }
inline int32_t traverse_y(int32_t w, int i) { return 4 * w + (i >> 2); }

struct WCoord {
	int32_t x = 0, y = 0;
};
struct TCoord {
	int32_t x = 0, y = 0;
};
struct UCoord {
	int32_t x = 0, y = 0;
};
inline WCoord wcoord_at(int32_t cx, int32_t cy) { return {wcoord_of(cx), wcoord_of(cy)}; }
inline TCoord tcoord_at(int32_t cx, int32_t cy) { return {tcoord_of(cx), tcoord_of(cy)}; }
inline UCoord ucoord_at(int32_t cx, int32_t cy) { return {ucoord_of(cx), ucoord_of(cy)}; }

// ---------------------------------------------------------------- tables
// move_x/move_y: a square spiral, index 0 = self, 1..8 = NW N NE E SE S SW W (y grows southwards), 9..24 ring 2 in
// the game's irregular order, 25..48 ring 3 from (-3,-3) clockwise, ... up to ring 10. kMoveRadius[r] = (2r+1)^2.
constexpr int kNumMoves = 441;
extern const int8_t move_x[kNumMoves];
extern const int8_t move_y[kNumMoves];
constexpr int32_t kMoveRadius[11] = {1, 9, 25, 49, 81, 121, 169, 225, 289, 361, 441};

// orthog_x/orthog_y: index 0 = self, 1..4 = N E S W.
constexpr int8_t orthog_x[5] = {0, 0, 1, 0, -1};
constexpr int8_t orthog_y[5] = {0, -1, 0, 1, 0};

// circle_init @006817f0: integer "circles" for r = 0..64. Ring r is [radius[r-1], radius[r]); "within r" is
// [0, radius[r]). The tables hold at most kCircleCap points: the cap is reached inside ring 64, which keeps only its
// first 300 points (radius[64] = 12873; docs/fog-spec.md section 3.1).
constexpr int32_t kCircleCap = 0x3249;
struct Circles {
	std::vector<int8_t> x, y;
	std::array<int32_t, 65> radius{};
};
const Circles& circles();

// ring_init @00681920: the rim tables of the fog stamp's incremental update. Ring 0 = the centre; ring r (1..32) =
// circle ring r plus gap fillers that make the rim 4-connected (docs/fog-spec.md section 3.1). Ring r is
// [radius[r-1], radius[r]); 4781 entries in all.
struct Rings {
	std::vector<int8_t> x, y;
	std::array<int32_t, 33> radius{};
};
const Rings& rings();

}  // namespace ron::sim
