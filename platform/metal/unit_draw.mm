#include "unit_draw.hpp"

#include <cmath>
#include <cstdio>

#include "assets/tga.hpp"

namespace ron::metal {

id<MTLTexture> make_texture(id<MTLDevice> device, id<MTLCommandQueue> queue, const Image& img) {
	const bool mips = img.width > 1 || img.height > 1;
	MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                                             width:NSUInteger(img.width)
	                                                                            height:NSUInteger(img.height)
	                                                                         mipmapped:mips];
	d.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> t = [device newTextureWithDescriptor:d];
	[t replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(img.width), NSUInteger(img.height))
	     mipmapLevel:0
	       withBytes:img.rgba.data()
	     bytesPerRow:NSUInteger(img.width) * 4];
	if (mips) {
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit generateMipmapsForTexture:t];
		[blit endEncoding];
		[cb commit];
	}
	return t;
}

void UnitDrawer::init(id<MTLDevice> device, id<MTLCommandQueue> queue, StateCache* states, const GameFiles* files) {
	device_ = device;
	queue_ = queue;
	states_ = states;
	files_ = files;
	units_ = load_unit_catalog(*files);
	const auto anim_names = load_anim_names(*files);
	ammo_ = load_ammo_catalog(*files, anim_names);
	// The FLAG pieces (ScenarioData::draw_components, rally / waypoint markers) are drawn through draw_ammo too.
	for (const AmmoPiece& p : load_ammo_catalog(*files, anim_names, "FLAG").pieces) {
		if (ammo_.find(p.name)) continue;
		ammo_.by_lower.emplace(to_lower(p.name), ammo_.pieces.size());
		ammo_.pieces.push_back(p);
	}
	unit_state_ = find_render_state("Unit Blend");
	Image white;
	white.width = white.height = 1;
	white.rgba = {255, 255, 255, 255};
	white_ = make_texture(device, queue, white);
}

int UnitDrawer::find(const std::string& name) const {
	for (size_t i = 0; i < units_.size(); i++) {
		if (units_[i].name == name) return int(i);
	}
	return -1;
}

id<MTLTexture> UnitDrawer::texture(const std::string& path) {
	auto it = textures_.find(path);
	if (it != textures_.end()) return it->second;
	id<MTLTexture> t = nil;
	const std::string abs = path.empty() ? std::string() : files_->resolve(path);
	if (!abs.empty()) {
		try {
			t = make_texture(device_, queue_, parse_tga(read_file(abs)));
		} catch (const std::exception& e) {
			std::fprintf(stderr, "texture %s: %s\n", path.c_str(), e.what());
		}
	}
	return textures_[path] = t;
}

UnitDrawer::Model* UnitDrawer::model(int unit) {
	auto it = models_.find(unit);
	if (it != models_.end()) return it->second.get();
	auto m = std::make_unique<Model>();
	const UnitGraphic& u = units_[size_t(unit)];
	try {
		const std::string path = files_->resolve(u.model);
		if (path.empty()) throw std::runtime_error("missing " + u.model);
		m->bh3 = std::make_unique<BH3Model>(parse_bh3(read_file(path)));
		m->slots = attach_slots(*m->bh3);
		const auto& ix = m->bh3->meshes[0].indices;
		m->index_count = ix.size();
		if (!ix.empty()) m->indices = [device_ newBufferWithBytes:ix.data() length:ix.size() * 2 options:MTLResourceStorageModeShared];
		m->texture = texture(u.texture);
		if (!u.texture.empty() && !m->texture) m->status = "missing texture " + u.texture;
	} catch (const std::exception& e) {
		m->status = e.what();
		m->bh3.reset();
	}
	Model* raw = m.get();
	models_[unit] = std::move(m);
	return raw;
}

const Animation* UnitDrawer::animation(int unit, int anim) {
	Model* m = model(unit);
	const auto& anims = units_[size_t(unit)].anims;
	if (!m || anim < 0 || anim >= int(anims.size())) return nullptr;
	auto it = m->anims.find(anim);
	if (it != m->anims.end()) return it->second.get();
	std::unique_ptr<Animation> a;
	const std::string path = anims[size_t(anim)].file.empty() ? std::string() : files_->resolve(anims[size_t(anim)].file);
	if (!path.empty()) {
		try {
			a = std::make_unique<Animation>(parse_bha(read_file(path)));
		} catch (const std::exception& e) {
			m->status = std::string("animation: ") + e.what();
		}
	}
	const Animation* raw = a.get();
	m->anims[anim] = std::move(a);
	return raw;
}

uint32_t UnitDrawer::clip_ms(int unit, int anim) {
	const Animation* a = animation(unit, anim);
	return a ? a->duration_ms : 0;
}

bool UnitDrawer::pose(int unit, int anim, double ms, std::vector<PosedVertex>& out) {
	Model* m = model(unit);
	if (!m || !m->bh3) return false;
	pose_model(*m->bh3, animation(unit, anim), uint32_t(std::max(0.0, ms)), out);
	return true;
}

std::string UnitDrawer::status(int unit) {
	Model* m = model(unit);
	return m ? m->status : std::string();
}

UnitDrawer::Attachment* UnitDrawer::attachment(const std::string& type) {
	auto it = attachments_.find(type);
	if (it != attachments_.end()) return it->second.get();
	std::unique_ptr<Attachment> a;
	if (const AmmoPiece* p = ammo_.find(type)) {
		try {
			a = std::make_unique<Attachment>();
			const std::string mp = files_->resolve(p->model);
			if (mp.empty()) throw std::runtime_error("missing " + p->model);
			a->model = std::make_unique<BH3Model>(parse_bh3(read_file(mp)));
			const auto& ix = a->model->meshes[0].indices;
			a->index_count = ix.size();
			if (!ix.empty()) a->indices = [device_ newBufferWithBytes:ix.data() length:ix.size() * 2 options:MTLResourceStorageModeShared];
			a->texture = texture(p->texture);
			if (!a->texture) a->texture = white_;
			const std::string ap = p->anim.empty() ? std::string() : files_->resolve(p->anim);
			if (!ap.empty()) a->anim = std::make_unique<Animation>(parse_bha(read_file(ap)));
			a->state = find_render_state(p->renderstate);
			if (!a->state) a->state = find_render_state("Ammo Blend");
		} catch (const std::exception& e) {
			std::fprintf(stderr, "attachment %s: %s\n", type.c_str(), e.what());
			a.reset();
		}
	}
	Attachment* raw = a.get();
	attachments_[type] = std::move(a);
	return raw;
}

void UnitDrawer::draw_mesh(id<MTLRenderCommandEncoder> enc, const std::vector<PosedVertex>& verts, id<MTLBuffer> indices,
                           NSUInteger count, id<MTLTexture> tex, const RenderState& rs, const DrawUniforms& du, bool flip_cull) {
	id<MTLBuffer> vb = [device_ newBufferWithBytes:verts.data() length:verts.size() * sizeof(PosedVertex) options:MTLResourceStorageModeShared];
	states_->bind(enc, rs, VertexKind::Posed, false, flip_cull);
	[enc setVertexBuffer:vb offset:0 atIndex:0];
	[enc setVertexBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentBytes:&du length:sizeof du atIndex:1];
	[enc setFragmentTexture:tex atIndex:0];
	[enc setFragmentTexture:tex atIndex:1];
	[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:count indexType:MTLIndexTypeUInt16 indexBuffer:indices indexBufferOffset:0];
}

bool UnitDrawer::draw_ammo(id<MTLRenderCommandEncoder> enc, const std::string& type, double ms, simd_float4x4 world,
                           bool flip_cull, simd_float4 team) {
	Attachment* a = attachment(type);
	if (!a || a->index_count == 0) return false;
	const double dur = a->anim ? double(a->anim->duration_ms) : 0.0;
	pose_model(*a->model, a->anim.get(), uint32_t(dur > 0 ? std::fmod(std::max(0.0, ms), dur) : 0.0), attach_posed_);
	// banner_0n.BH3 (the ScenarioFlag) poses with non-finite and unnormalised normals, which light to NaN and draw
	// nothing: renormalise, falling back to +z (open question: the file's normal data or the BH3 reader).
	for (PosedVertex& v : attach_posed_) {
		const float l = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
		if (std::isfinite(l) && l > 1e-6f) v.nx /= l, v.ny /= l, v.nz /= l;
		else v.nx = 0, v.ny = 0, v.nz = 1;
	}
	DrawUniforms ad = draw_uniforms(*a->state, world);
	ad.tfactor = simd_make_float4(1, 1, 1, 1);
	const bool tinted = team.x != 1 || team.y != 1 || team.z != 1;
	if (tinted && std::string(a->state->name) == "Ammo Team Blend") ad.diffuse = team;  // as for attachments
	draw_mesh(enc, attach_posed_, a->indices, a->index_count, a->texture, *a->state, ad, flip_cull);
	return true;
}

void UnitDrawer::draw(id<MTLRenderCommandEncoder> enc, int unit, int anim, double ms, simd_float4x4 world, simd_float4 team,
                      bool flip_cull) {
	Model* m = model(unit);
	if (!m || !m->bh3 || m->index_count == 0) return;
	const Animation* clip = animation(unit, anim);
	const double t_raw = std::max(0.0, ms);
	pose_model(*m->bh3, clip, uint32_t(t_raw), posed_, &node_world_);

	// Body: "Unit Blend". tfactor carries the team colour (where texture alpha is low) and the unit's opacity.
	DrawUniforms du = draw_uniforms(*unit_state_, world);
	du.tfactor = team;
	du.mat_ambient = unit_ambient_;
	du.mat_diffuse = unit_diffuse_;
	draw_mesh(enc, posed_, m->indices, m->index_count, m->texture ? m->texture : white_, *unit_state_, du, flip_cull);

	// Attachments active at this action and time (docs/attachments-spec.md).
	const UnitGraphic& u = units_[size_t(unit)];
	if (anim < 0 || anim >= int(u.anims.size())) return;
	const std::string& action = u.anims[size_t(anim)].action;
	const double dur = clip ? double(clip->duration_ms) : 0.0;
	const double t = dur > 0 ? std::fmod(t_raw, dur) : t_raw;
	for (const ExistEvent& ev : u.exist_events) {
		if (ev.action != action || t < ev.start_ms || t > ev.end_ms) continue;
		Xform where;  // node -1: the unit's model origin
		if (ev.node >= 0) {
			if (ev.node >= kAttachSlots || m->slots[size_t(ev.node)] < 0) continue;  // the game skips these too
			const size_t n = size_t(m->slots[size_t(ev.node)]);
			if (n >= node_world_.size()) continue;
			where = node_world_[n];
		}
		Attachment* a = attachment(ev.type);
		if (!a || a->index_count == 0) continue;
		// The attachment's own clip runs on the parent's action time, wrapped to its own length.
		pose_model(*a->model, a->anim.get(), uint32_t(t), attach_posed_);
		transform_vertices(where, attach_posed_);
		// draw_ammo: diffuse is white, or the team colour for "Ammo Team Blend"; tfactor = (1, 1, 1, unit alpha).
		DrawUniforms ad = draw_uniforms(*a->state, world);
		ad.tfactor = simd_make_float4(1, 1, 1, team.w);
		if (std::string(a->state->name) == "Ammo Team Blend") ad.diffuse = team;
		draw_mesh(enc, attach_posed_, a->indices, a->index_count, a->texture, *a->state, ad, flip_cull);
	}
}

}  // namespace ron::metal
