// Object queries around a point (Objects.obj, search.obj, gamemath.obj) and UnitType::find_nearby_spot: the searches
// the group-move code (relocating formation slots), Objects::init_unit (followers) and Unit::find_path use.
// docs/movement-spec.md section 11.1 and docs/sim-implementation.md.
#pragma once

#include <cstdint>

namespace ron::sim {

class Game;
struct Type;

// FilterIndex
enum FilterIndex : int32_t {
	kFilterAll = 0, kFilterType = 1, kFilterBaseType = 2, kFilterNotMe = 3, kFilterNotMyUber = 4, kFilterCanCollide = 5
};

// project(Coord, Coord, angle, r, &x, &y) @0092cf40: (x + sin(angle, r), y - cos(angle, r)).
void project(int32_t x, int32_t y, uint32_t angle, int32_t r, int32_t* ox, int32_t* oy);

// ObjectsData::find_unit_with_radius @00659890: the unit (o of leader *who_out) within `radius` of (x, y) whose
// distance minus its big_radius is smallest (an overlapping one is returned at once); -1 for none. `search` is the
// SearchIndexBH relation tested against the units' leaders (< 0: all). Scans the circle tables around the wcoord
// when there are at least as many units as circle cells, otherwise every unit of leaders 0..7 in slot order.
int32_t find_unit_with_radius(const Game& g, int32_t x, int32_t y, int32_t search, int32_t radius, int32_t filter,
                              int32_t o, int32_t who, int32_t* who_out = nullptr);
// ObjectsData::find_unit_ordered_with_radius @00658ef0: the same over leader `search`'s units that have a move-family
// current order, measured from (x, y) to the units themselves.
int32_t find_unit_ordered_with_radius(const Game& g, int32_t x, int32_t y, int32_t search, int32_t radius,
                                      int32_t filter, int32_t o, int32_t who);
// Objects::find_collision @0065b1b0: unit (o, who) would overlap another unit's footprint at (x, y).
int32_t find_collision(Game& g, int32_t x, int32_t y, int32_t o, int32_t who, int32_t p5);
// Objects::find_ordered_collision @0065b440: ... another unit's order target (orders_x/orders_y).
int32_t find_ordered_collision(const Game& g, int32_t x, int32_t y, int32_t o, int32_t who);

// UnitType::find_nearby_spot @0061de70: the first free spot for a unit of type `t` on rings of radius r0..r1 (step
// `step`, <= 0: (r1 - r0)/8) around (x, y), probing 32 directions per ring alternating around `angle`. Returns 0 and the
// spot (a ucoord centre) in (*ox, *oy), or 1 and (x, y) when nothing was found. Arguments as the original's
// (r1 = 0 with r0 > 0, or r1 < 0: a default from the type's big_radius; `nocheck`: skip the object tests; `uber`:
// leave room for the whole uber group; `bo, bwho`: a building the spot must not be inside; `region` >= 0: same tregion).
int32_t find_nearby_spot(Game& g, const Type& t, int32_t x, int32_t y, int32_t* ox, int32_t* oy, int32_t r0, int32_t r1,
                         int32_t step, uint32_t angle, int32_t filter, int32_t o, int32_t who, int32_t nocheck,
                         int32_t uber, int32_t bo, int32_t bwho, int32_t region);

}  // namespace ron::sim
