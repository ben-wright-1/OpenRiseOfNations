// Farms: a 4x4-tile field of wheat boxes over a "wheatfarm" ground underlay, or an animal pen ("pigfarm" underlay plus
// five pigs/chickens). There is no building sprite. Spec and evidence: docs/farms-spec.md (Farms / FarmsOut, farms.obj).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/scenario.hpp"
#include "render/vertex.hpp"

namespace ron {

// building_graphics.xml <FARM> attributes (Farms::init). Heights are the attribute x 3, in world units.
struct FarmParams {
	uint8_t stalk[4] = {166, 216, 188, 173};  // stalk_red/green/blue/alpha: the tint of young wheat
	float min_height = 30, max_height = 108;   // min_height x 3, max_height x 3
};
FarmParams load_farm_params(const GameFiles& files);

enum class FarmType : uint8_t { Wheat = 0, Animal = 1 };

// Per-cell growth status (FarmStruct::status): 0 bare, 1 growing, 2 ripe, 3 being harvested.
enum : uint8_t { kWheatBare = 0, kWheatGrowing = 1, kWheatRipe = 2, kWheatSnipped = 3 };

// FarmStruct. Cells and corners are indexed [x][y], x and y growing with the world axes.
struct Farm {
	float cx = 0, cy = 0;     // footprint centre (snapped to a tile corner: farms are 4x4)
	int tx0 = 0, ty0 = 0;     // the NW tile of the footprint: (cx / 192 - 2, cy / 192 - 2)
	int who = 0;              // owner
	FarmType type = FarmType::Wheat;
	float build = 1.0f;       // construction progress (hit points / max); 1 = complete
	float corner_z[5][5] = {};  // terrain height at the 5x5 tile corners (Farms::add / recalc_heights)
	float percent[4][4] = {};   // growth 0..1 per cell; a freshly built farm is all 0
	uint8_t status[4][4] = {};
};

// A farm building from the scenario: centre, owner and object slot (the slot breaks the tie for farms without a city).
struct FarmSite {
	float cx = 0, cy = 0;
	int who = 0, slot = 0;
};
struct CitySite {
	float cx = 0, cy = 0;
	int who = 0;
	int level = 1;  // 1 Small City (VILLAGE), 2 Large City (CITY), 3 Major City (METROPOLIS)
};

// Farms::add for each site, in order: corner heights from the terrain, growth 0, and the wheat/animal choice.
// The game's random fallback (a 1-in-4 animal pen for a city's first isolated farm) is replaced by wheat.
std::vector<Farm> place_farms(const Terrain& t, const std::vector<FarmSite>& sites, const std::vector<CitySite>& cities);

// Farms::inc_time: one simulation frame (66.7 ms) of unattended growth. Bare cells start growing at random (the game
// uses its synchronised game_random; `seed` is our own stream), growing cells gain 0.005 per frame.
void farms_inc_time(std::vector<Farm>& farms, uint32_t& seed);

// The wheat of every wheat farm (FarmsOut::draw, the detailed path): one open box per cell (top + the +y and +x sides),
// world-space positions, "Farm Blend" colours and UVs into the tileset's FARMTEX. `frame` is the simulation frame
// (wind sway phase). Triangles are in painter's order (back to front per farm) for the depth-writing state.
struct WheatMesh {
	std::vector<ColorVertex> vertices;
	std::vector<uint32_t> indices;
};
void build_wheat(const std::vector<Farm>& farms, const FarmParams& p, uint32_t frame, WheatMesh& out);

// Ground underlays (WallOut::draw_weathering -> TerrainOut::add_weather_underlay(tx0, ty0, 4, 4, ...)): the 5x5 terrain
// grid vertices of the footprint, UV0 = UV1 = (i/4, j/4). Drawn with "Weathering Underlay"; `animal` selects the farms.
struct UnderlayMesh {
	std::vector<ColorVertex2> vertices;
	std::vector<uint32_t> indices;
};
void build_farm_underlays(const Terrain& t, const std::vector<Farm>& farms, FarmType type, UnderlayMesh& out);

// Farms::add_animals: five farm animals per animal pen, owned by nature, within +-192 units of the centre. The game
// draws pig or chicken 50/50 from game_random; here a stream seeded by the farm position.
struct FarmAnimal {
	std::string type;  // "FARMPIG" or "FARMCHICKEN" (unit graphics <TYPE>-TYPE0/1)
	float x = 0, y = 0, angle_deg = 0;
	int variant = 0;
};
std::vector<FarmAnimal> farm_animals(const Farm& f);

}  // namespace ron
