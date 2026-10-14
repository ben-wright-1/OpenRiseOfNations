// Scene lighting and post-processing constants of the original frame (docs/lighting-spec.md).
#pragma once

#include "assets/files.hpp"

namespace ron {

// The one light, "Sun" (Game::init_light): travel direction in world space, +z up.
constexpr float kSunDir[3] = {-0.70710678f, 0.0f, -0.70710678f};
constexpr float kHDRScale = 5.0f;  // ps_global's hdrMultiplier outside the minimap

// From Data/unit_graphics.xml <UNITS ambientr.. ambienti diffuser.. diffusei>.
struct SceneLighting {
	float ambient_light[3] = {0.6f, 0.716f, 0.745f};  // gAmbientLight (rgb, no intensity)
	float unit_ambient[3] = {0.3348f, 0.3995f, 0.4157f};  // unit material ambient: rgb x ambienti
	float unit_diffuse[3] = {2.5f, 2.5f, 2.5f};           // unit material diffuse: rgb x diffusei
};
SceneLighting load_scene_lighting(const GameFiles& files);

// D3D11ToneMapper defaults (no data file overrides them; only nukes change exposure, saturation and tint).
struct ToneMapSettings {
	float exposure = 1.0f;
	float white_point = 3.5f;
	float threshold = 0.6f;      // bloom threshold
	float min_luminance = 1.4f;
	float saturation = 1.0f;
	float bloom_strength = 6.0f;
	float bloom_tightness = 12.5f;
	float tint[4] = {0, 0, 0, 0};  // rgb, weight
};

}  // namespace ron
