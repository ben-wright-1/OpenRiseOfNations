// Terrain tilesets: Data/tilesets.xml <TILESET>. Texture lists per land class, plus terrain lighting.
// See docs/terrain-spec.md section B2/B3.
#pragma once

#include <string>
#include <vector>

#include "assets/files.hpp"

namespace ron {

struct TilesetTexture {
	std::string name;      // BASE name, e.g. "grass"
	std::string file;      // resolved relative path, or empty if not found
	bool supertile = false;  // a 512^2 image cut 3x3 over 3x3 wcoords
};

struct Tileset {
	std::string name;
	std::string basedir;                   // BASEDIR path, normalized
	std::vector<TilesetTexture> baseland;  // indexed by WCoordCell::land_sub
	std::vector<TilesetTexture> coastal;   // land 1
	std::vector<TilesetTexture> ocean;     // land 2
	float ambient[3] = {0, 0, 0};          // LIGHTING/AMBIENT rgb x alpha
	float diffuse[3] = {1, 1, 1};          // LIGHTING/DIFFUSE rgb x alpha
	float treescale = 1.0f;                // TREES treescale
	int coast_base = 0;                    // TERRAINGROUP/COAST_BASE: the BASELAND sub that sand sits on
	std::string coral;                     // CORALTEX texture (ocean transitions), may be empty
	std::string mountain_base, mountain_alpha;  // MOUNTAINS BASETEX / ALPHATEX
	std::string snowcap;                   // UNDERLAYS <TEXTURE id="snowcap">
	std::string wheat_underlay, animal_underlay;  // UNDERLAYS <TEXTURE id="wheatfarm"> / id="pigfarm" (farm ground)
	std::string fog;                       // FOGTEX/TEXTURE: the fog-of-war fade ramp (art/fow_fade.tga, 256 x 1)
	std::string farm_wheat;                // FARMTEX/TEXTURE: the wheat stalk texture of farms (TileSet::get_wheat_texture)
	bool snowcaps = false;                 // <TILESET snowcaps="1">
	std::string river_normal, river_delta, river_halfdelta, river_water;  // RIVERTEX_* textures
};

// Loads the tileset named `name` (case-insensitive). Throws std::runtime_error if absent.
Tileset load_tileset(const GameFiles& files, const std::string& name);

}  // namespace ron
