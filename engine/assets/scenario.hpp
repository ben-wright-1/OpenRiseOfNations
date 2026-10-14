// Scenario files (.scx/.sce): gzip around a BHG chunk tree (ScenarioRead::import). Only the header and the terrain
// chunk are decoded here; layout and meaning in docs/terrain-spec.md section A.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/chunk.hpp"

namespace ron {

constexpr float kWCoordSize = 768.0f;  // world units per gameplay tile
constexpr float kTCoordSize = 192.0f;  // world units per terrain sub-tile (4 per wcoord)

// Per-wcoord fields as stored in the file (chunk 88/16).
struct WCoordCell {
	uint16_t flags = 0;  // file bits: 0x04 forest, 0x01 rocks, 0x08 coast, 0x20 river, 0x80 half water (shoreline)
	uint8_t land = 0;    // 0 land, 1 shelf water, 2 deep ocean, 3 coast
	uint8_t land_sub = 0;  // land 0: 0 fertile .. 3 arid; picks the BASELAND texture
};

namespace wfile {
constexpr uint16_t kRocks = 0x01, kForest = 0x04, kCoast = 0x08, kRiver = 0x20, kHalfWater = 0x80;
}
namespace tfile {
constexpr uint16_t kRiver = 0x02, kWater = 0x10, kCoastal = 0x20;
}

struct MountainStamp {
	uint8_t wx = 0, wy = 0;
	int32_t range_index = 0;
	uint8_t num_coords = 0;
};

struct Doober {
	int32_t x = 0, y = 0;  // world units
	uint8_t kind = 0;      // 0 bush, 1 rock
};

struct Terrain {
	std::string tileset;  // a <TILESET name=> in Data/tilesets.xml
	int xs = 0, ys = 0;   // wcoords
	std::vector<WCoordCell> wcoords;      // xs * ys, row-major
	std::vector<uint16_t> tcoord_mask;    // 4xs * 4ys, file bits (tfile::)
	std::vector<float> heights;           // (4xs+1) * (4ys+1) vertex heights, world units, +z up, sea level 0
	std::vector<uint16_t> coord_flags;    // xs * ys, eTERRA_COORD_* (chunk 99); may be empty
	std::vector<MountainStamp> mountains;
	std::vector<std::vector<std::array<int32_t, 2>>> rivers;  // wcoord polylines
	std::vector<Doober> doobers;

	int vxs() const { return xs * 4 + 1; }
	int vys() const { return ys * 4 + 1; }
	float height(int tx, int ty) const { return heights[size_t(ty) * size_t(vxs()) + size_t(tx)]; }
	const WCoordCell& cell(int wx, int wy) const { return wcoords[size_t(wy) * size_t(xs) + size_t(wx)]; }
	bool in_bounds(int wx, int wy) const { return wx >= 0 && wy >= 0 && wx < xs && wy < ys; }
	// Ground height at world (x, y): each 192-unit cell is two triangles split along its (x+1,y)-(x,y+1) diagonal
	// (TerrainOut::find_data_z). Clamped to the map.
	float sample_height(float x, float y) const;
};

// Players (chunk 11): one per active slot. docs/scenario-objects-spec.md section 7.
struct ScenarioPlayer {
	int who = 0;
	int tribe = 0;         // index into the 24-entry tribe list (rules.xml, or the conquest campaign's override)
	int color = 0;         // team colour slot 0..8
	uint32_t leader_flags = 0;  // file bits (section 7)
	int32_t is_human = 0;       // 0 = AI, non-zero (4) = human
	std::string name;
	std::vector<std::string> techs;  // known techs; the age is the number of age techs among them
	// Chunk 41 {42 count, 43 count x {i32 who, i32 diplo, i32 treaty}} -> Leader diplos[who] / treaties[who]
	// (docs/scenario-objects-spec.md section 7; diplo 0 war, 1 peace, 2 allied).
	struct Diplo {
		int32_t who = 0, diplo = 0, treaty = 0;
	};
	std::vector<Diplo> diplomacy;
	bool has_diplomacy = false;  // chunk 41 present
	// Production (ScenarioRead::load_leader_chunk; docs/production-spec.md 7.9.7): record +0xc (LeaderData +0x14),
	// +0xe0 handicap, +0xe8 difficulty (record 107 only); chunk 76 {77 count, 78 count x wchar[256]} type names fed to
	// Leader::gain_tech like the techs; chunk 44 {45 count, 46 count x {i32 good, i32 amount}} stockpile overwrites.
	int32_t gov = 0, handicap = 0, difficulty = 0;
	std::vector<std::string> types;
	struct Stock {
		int32_t good = 0, amount = 0;
	};
	std::vector<Stock> stockpile;
	std::vector<uint16_t> child_order;  // the player chunk's children in file order (38 techs, 76 types, 41, 44)
};

// Units (chunk 5/25 -> 83). Positions in world units; facing is a 32-bit binary angle.
struct ScenarioUnit {
	int who = 0, o = 0;
	uint32_t angle = 0;   // deg = angle * 360 / 2^32; faces (sin a, -cos a)
	uint32_t masks = 0;   // file bits: 8 crew slot (not created), 0x10 packed
	std::string type;     // unit TYPENAME
	int32_t cx = 0, cy = 0;
	int32_t inside = -1;  // container's object index; >= 0: garrisoned or carried (not drawn)
	// Simulation fields (UnitPropertiesChunk4; docs/scenario-objects-spec.md section 4.1).
	int o_up = -1, o_down = -1;  // uber chain (batch units)
	uint32_t dest_angle = 0;
	int32_t tolerance = 0;
	int16_t mana_burn = 0;
	int8_t form = 0, stance = 0;
	int32_t damage = 0;
	int32_t inside_who = -1;
	int32_t seq = 0;  // creation order: cities (chunk 7) and objects (chunk 5) in file order
};

// Buildings (5/26 -> 84) and cities (7/27 -> 30). Positions are footprint centres, snapped.
struct ScenarioBuilding {
	int who = 0, o = 0;
	std::string type;  // building TYPENAME ("Small City" for cities)
	int32_t cx = 0, cy = 0;
	bool active = true;  // false: construction site
	bool city = false;
	std::string city_name;
	int32_t seq = 0;     // creation order (as ScenarioUnit::seq)
	int32_t damage = 0;  // applied when active
	int32_t gather_x = 0, gather_y = 0;
	uint8_t ever_seen = 0;
	uint32_t city_flags = 0;  // city record +0x1c: bit 1 capital, bit 2 (+0x4000)
	int32_t founder = -1;     // city record +0x20
	// Chunk 85: {33 {i32 count, i32 first_in_progress}, 35 count x wchar[256]} production queue type names.
	std::vector<std::string> queue;
	int32_t first_in_progress = 0;
};

// Goods (4/24) and items (3/22): resources and ruins.
struct ScenarioGood {
	std::string type;
	int32_t cx = 0, cy = 0;
};

struct Scenario {
	int header_version = 0;
	std::string name;
	std::array<int32_t, 8> camera_x{}, camera_y{}, camera_zoom{};  // per player; -1 = unset
	uint32_t world_seed = 0;  // header h[0x38] (WorldData.seed); seeds the transition noise
	int32_t reveal_map = -1;  // header 0x6c +0xf4 (GameInfo revealmaps: 0 none, 1 normal, 2 explored, 3 all); -1 absent
	// Game settings of the header (docs/scenario-objects-spec.md section 2), when present (has_settings).
	bool has_settings = false;
	int32_t team_style = 0, game_rules = 0, start_tech = 0, end_tech = 7, elimination = 0, victory = 0, rush_rules = 0;
	int32_t score_goal = 0, time_limit = 0, wonderwin = 0, popwin = 0, econwin = 0;  // 0x100..0x110
	int32_t pop_limit = -1;  // -1: absent
	int32_t map_style = 0;   // 0xd8 (GameInfo.map_style)
	int32_t ai_sea_map = 0;  // 0x178 (v0x6c) / 0x124 (v0x67): World.sea_map; 0 in older headers
	uint32_t scenario_flags = 0;  // 0xe4: 4 DISABLE_UNIQUE_UNITS (GameInfo.flags 8), 8 DISABLE_NATION_POWERS (flags 4)
	Terrain terrain;
	std::vector<ScenarioPlayer> players;
	std::vector<ScenarioUnit> units;
	std::vector<ScenarioBuilding> buildings;  // including cities
	std::vector<ScenarioGood> goods, items;

	const ScenarioPlayer* player(int who) const {
		for (const ScenarioPlayer& p : players) {
			if (p.who == who) return &p;
		}
		return nullptr;
	}
};

// Decompresses (if gzip) and decodes. Throws FormatError on malformed data.
Scenario parse_scenario(const Bytes& file);

// gzip / zlib inflate.
Bytes gunzip(const Bytes& in);

}  // namespace ron
