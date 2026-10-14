// Building the simulation grid from a scenario (docs/world-spec.md section 3; ScenarioRead::import @009b1430):
// terrain chunk -> mountains -> compute_all_vals -> adjust_for_blocking (regions inside) -> init_terrain's region
// rebuild -> items, goods, units, buildings, cities. Port of re/tools/worlddump.mjs buildWorld; ron-world dumps the same
// JSON.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/scenario.hpp"
#include "sim/mountains.hpp"
#include "sim/random.hpp"
#include "sim/regions.hpp"
#include "sim/rules.hpp"
#include "sim/world.hpp"

namespace ron::sim {

// MAKE goods of each rules <LANDS> entry (Lands::init @0067e730; the first 4 MAKEs, TYPE_NONE = -1), indexed by
// the gather-land index.
using LandGoods = std::array<std::array<int32_t, 4>, land::kNumLands>;
LandGoods land_goods(const Rules& rules);
// The stock rules.xml table (land 0: Knowledge, Food; 4 Forest: Timber; 5 Mountains: Metal; 7 Oil: Oil; 8 Cliffs:
// Metal; the rest nothing), for building a grid without loading the rules.
extern const LandGoods kStockLandGoods;

struct PlacedMountain {
	int32_t wx = 0, wy = 0, range = 0;
	std::vector<Offset> tcoords;  // the range's mountain tcoords, relative to (4wx, 4wy), template order
	std::vector<Offset> wcoords;  // the range's mountain (solid) wcoords, relative to (wx, wy)
};

// ScenarioRead::load_wcoord_chunk3 / load_tcoord_chunk2: World::init, then the file bits mapped to WData / TData.
// Throws std::runtime_error when the tcoord grid does not match the wcoord size.
void load_terrain(World& w, const Terrain& t);
// Mountains::add_mountain (loader arguments: no validation radii) + set_behind_flags for every chunk-69 stamp; a
// stamp whose num_coords differs from its template's face-wcoord count, or with an invalid range, is skipped.
// Returns the stamps placed. Writes outside the map (the original indexes without bounds checks) are skipped and
// counted in *outside.
std::vector<PlacedMountain> add_mountains(World& w, const Terrain& t, MountainTemplates& templates, int32_t* outside);

// World::compute_all_vals @006b3790 (x outer, y inner) -> compute_val @006b0350: WData.val and goods. One
// Random::get(0, 0xffff) per non-water wcoord; returns the number of draws.
int32_t compute_all_vals(World& w, Random& rng, const LandGoods& lands, const Regions& regions);

// World::adjust_for_blocking @006b3230 (forests, halo, regions, dead mountain spots, edge BAD_PATH, forest seams,
// GATHER_EDGE). `dead_spots` (optional) receives the wcoords eliminate_dead_mount_spots filled.
void adjust_for_blocking(World& w, Regions& regions, const std::vector<PlacedMountain>& mountains,
                         std::vector<WCoord>* dead_spots = nullptr);
// Terrain::eliminate_dead_mount_spots @0084fd90.
void eliminate_dead_mount_spots(World& w, std::vector<WCoord>* filled = nullptr);
// World::set_edge_flags @006b0240: GATHER_EDGE on mountain tcoords (stamp order), cliffs (none), forest tcoords.
void set_edge_flags(World& w, const std::vector<PlacedMountain>& mountains);

struct ObjectStats {
	int32_t items = 0, goods = 0, goods_blocking = 0, builds = 0, cities = 0, resnapped = 0, units = 0, unit_cells = 0;
	int32_t outside = 0;                // good tcoords off the map (skipped)
	std::vector<std::string> unknown;   // type names with no rules type, first-seen order, unique
};

// Items, goods, units (CollBlock footprints, guy 0 at the unit position), buildings then cities, in file order.
// Type names resolve like Types::get_type_index: case-insensitive TYPENAME (NAME for goods), first match wins.
ObjectStats place_objects(World& w, const Scenario& s, const Rules& rules);

struct WorldBuild {
	World world;
	Regions regions;
	std::vector<PlacedMountain> mountains;
	std::vector<WCoord> dead_mount_spots;
	int32_t rng_draws = 0;
	int32_t mountain_outside = 0;
	bool has_objects = false;
	ObjectStats objects;
};

// The whole load path. `seed`: game_random when the terrain chunk loads (only WData.val depends on it; worlddump
// --seed). `rules` null: terrain only (stock <LANDS> table, no objects).
void build_world(WorldBuild& out, const GameFiles& files, const Scenario& s, const Rules* rules, uint32_t seed);

}  // namespace ron::sim
