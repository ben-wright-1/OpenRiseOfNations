// Draws animated 3D unit graphics (body + EXISTEVENT attachments) with the game's render states. Shared by the unit
// viewer and the map renderer. Objective-C++; include from .mm files only. See docs/attachments-spec.md.
#pragma once

#import <Metal/Metal.h>
#include <simd/simd.h>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/bh3.hpp"
#include "assets/bha.hpp"
#include "assets/catalog.hpp"
#include "assets/effects.hpp"
#include "assets/pose.hpp"
#include "assets/tga.hpp"
#include "renderer.hpp"

namespace ron::metal {

class UnitDrawer {
public:
	// Loads the unit and effects catalogs. Throws on missing data files.
	void init(id<MTLDevice> device, id<MTLCommandQueue> queue, StateCache* states, const GameFiles* files);

	const std::vector<UnitGraphic>& units() const { return units_; }
	// The lit body's material (unit_graphics.xml <UNITS>): rgb x intensity. Attachments are fake-lit and ignore it.
	void set_unit_material(simd_float4 ambient, simd_float4 diffuse) { unit_ambient_ = ambient, unit_diffuse_ = diffuse; }
	int find(const std::string& name) const;  // exact name, -1 if absent

	// Clip length of animation `anim` of `unit` in ms (0 if none or missing).
	uint32_t clip_ms(int unit, int anim);
	// Model-space pose of the body (for framing). Returns false if the model failed to load.
	bool pose(int unit, int anim, double ms, std::vector<PosedVertex>& out);
	// Last load error for `unit`, or "".
	std::string status(int unit);

	// Draws `unit` playing `anim` (index into its anims, -1 for none) at `ms` into the current encoder. `world` maps
	// model space to world space; `flip_cull` must be set when it mirrors (det < 0) relative to the view.
	void draw(id<MTLRenderCommandEncoder> enc, int unit, int anim, double ms, simd_float4x4 world, simd_float4 team,
	          bool flip_cull = false);
	// Draws the effects_graphics.xml AMMO piece `type` (a projectile in flight or stuck in the ground) posed at `ms` of
	// its own clip. Returns false if the piece has no model.
	// `team` is the diffuse of "Ammo Team Blend" pieces (flags).
	bool draw_ammo(id<MTLRenderCommandEncoder> enc, const std::string& type, double ms, simd_float4x4 world,
	               bool flip_cull = false, simd_float4 team = simd_make_float4(1, 1, 1, 1));

private:
	struct Model {
		std::unique_ptr<BH3Model> bh3;
		id<MTLTexture> texture;
		id<MTLBuffer> indices;
		NSUInteger index_count = 0;
		std::array<int, kAttachSlots> slots{};
		std::map<int, std::unique_ptr<Animation>> anims;  // by anim index; null if missing
		std::string status;
	};
	struct Attachment {
		std::unique_ptr<BH3Model> model;
		std::unique_ptr<Animation> anim;
		id<MTLTexture> texture;
		id<MTLBuffer> indices;
		NSUInteger index_count = 0;
		const RenderState* state = nullptr;
	};

	Model* model(int unit);
	const Animation* animation(int unit, int anim);
	Attachment* attachment(const std::string& type);
	id<MTLTexture> texture(const std::string& path);
	void draw_mesh(id<MTLRenderCommandEncoder> enc, const std::vector<PosedVertex>& verts, id<MTLBuffer> indices,
	               NSUInteger count, id<MTLTexture> tex, const RenderState& rs, const DrawUniforms& du, bool flip_cull);

	id<MTLDevice> device_;
	id<MTLCommandQueue> queue_;
	StateCache* states_ = nullptr;
	const GameFiles* files_ = nullptr;
	std::vector<UnitGraphic> units_;
	AmmoCatalog ammo_;
	const RenderState* unit_state_ = nullptr;
	simd_float4 unit_ambient_ = {0.3348f, 0.3995f, 0.4157f, 1}, unit_diffuse_ = {2.5f, 2.5f, 2.5f, 1};
	id<MTLTexture> white_;
	std::map<int, std::unique_ptr<Model>> models_;
	std::map<std::string, std::unique_ptr<Attachment>> attachments_;
	std::map<std::string, id<MTLTexture>> textures_;
	std::vector<PosedVertex> posed_, attach_posed_;
	std::vector<Xform> node_world_;
};

// Uploads an RGBA image with a mip chain.
id<MTLTexture> make_texture(id<MTLDevice> device, id<MTLCommandQueue> queue, const Image& img);

}  // namespace ron::metal
