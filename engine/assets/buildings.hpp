// Building graphics catalog: Data/building_graphics.xml <BUILD>. Buildings are pre-rendered sprites with a depth-only
// hull mesh and animated 3D add-ons. Drawing spec: docs/buildings-spec.md.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "assets/files.hpp"

namespace ron {

// The building's animation states (BuildAnimNames). Add-on events are filtered by the current one.
enum class BuildAnim { Default, Build, EmptyIdle, NukeBuild, NukeIdle, NukeLaunch, MissileBuild, MissileIdle, MissileLaunch };

struct BuildAddon {
	BuildAnim anim = BuildAnim::Default;  // buildanim=
	std::string subject;                  // AMMO name
	std::string anim_file;                // the add-on's own clip (anim_graphics.xml name resolved), may be empty
	int axis = -1;                        // != 0: offset to hull slot `node` and face the wind
	int node = 7;
};

struct BuildGraphic {
	std::string name;     // e.g. VILLAGE-DEFAULT-AGE0
	std::string texture;  // relative .tga path (may be empty: farms)
	std::string zbuffer;  // relative .bh3 path of the depth hull (may be empty)
	int spritexoff = 0, spriteyoff = 0;  // anchor texel in the image, top-left origin
	bool halfoffset = false;             // anchor shifted by (-96, -96) world units
	int constructsize = 0;
	std::vector<BuildAddon> addons;
};

struct BuildCatalog {
	std::vector<BuildGraphic> builds;
	std::map<std::string, size_t> by_lower;  // lowercase name -> index

	const BuildGraphic* find(const std::string& name) const;
	// get_build_gpiece's fallback: <type>-<style>-AGE<a>, a-1 .. 0; then the backup style; then DEFAULT; then
	// DEFAULT-AGE0. Styles are e.g. DEFAULT, NEUROPE, ASIAN, ARAB.
	const BuildGraphic* find_variant(const std::string& type, const std::string& style, int age,
	                                 const std::string& backup_style = std::string()) const;
};

BuildCatalog load_build_catalog(const GameFiles& files, const std::map<std::string, std::string>& anim_names);

// building_graphics.xml <CONSTRUCTION texture spritexoff spriteyoff [model]>: the construction underlay sprites, in file
// order (1x1..7x7, 7x7 for 8x8, 9x5, dock, 3x6; docs/buildings-spec.md section 6).
struct ConstructionSprite {
	std::string texture, model;  // normalized relative paths
	int spritexoff = 0, spriteyoff = 0;
};
std::vector<ConstructionSprite> load_construction_sprites(const GameFiles& files);

// effects_graphics.xml <SCAFFOLD> models (per-age sets of equal size, in file order) and <SCAFFOLD_TIMING> lists (by
// building size - 1; the last for Senate-type buildings): pieces drawn while start <= progress <= end.
struct ScaffoldPiece {
	std::string name, model, texture, renderstate, anim;  // anim: anim_graphics.xml name (resolved path in anim_file)
	std::string anim_file;
};
struct ScaffoldTiming {
	std::string name;  // a piece name of the age set (case-insensitive)
	float start = 0, end = 1;
	float x = 0, y = 0, z = 0, angle = 0;
};
struct ScaffoldSet {
	std::vector<ScaffoldPiece> pieces;  // all ages: pieces_per_age per set (ages 0-2, 3-4, 5+)
	int pieces_per_age = 0;
	std::vector<std::vector<ScaffoldTiming>> timings;
	// The piece of the age set `age_set` (0..2) named `name`, or null.
	const ScaffoldPiece* find(int age_set, const std::string& name) const;
};
ScaffoldSet load_scaffolds(const GameFiles& files, const std::map<std::string, std::string>& anim_names);

}  // namespace ron
