// The game's named render states (Scene::init_render_states), as plain data. A RenderState fully describes
// fixed-function-style draw state: blend, depth, alpha test, cull, and two texture-combiner stages.
// Enum values match riseofnations.exe (rise.pdb). Spec and table source: docs/renderstates.md.
#pragma once

#include <cstdint>
#include <string>

namespace ron {

enum class CompareFunc : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class BlendMode : uint8_t {
	None, Zero, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DestAlpha, InvDestAlpha, DestColor, InvDestColor, SrcAlphaSat
};
enum class CullMode : uint8_t { None, CW, CCW };  // D3D9 semantics: CCW culls counter-clockwise triangles
enum class FillMode : uint8_t { None, Point, Wireframe, Solid };
enum class TexAddress : uint8_t { Wrap, Mirror, Clamp, Border, MirrorOnce };
// D3D9 D3DTEXTUREOP numbering minus one.
enum class TextureOp : uint8_t {
	Disable, SelectArg1, SelectArg2, Modulate, Modulate2x, Modulate4x, Add, AddSigned, AddSigned2x, Subtract, AddSmooth,
	BlendDiffuseAlpha, BlendTextureAlpha, BlendFactorAlpha, BlendTextureAlphaPM, BlendCurrentAlpha, PreModulate,
	ModulateAlphaAddColor, ModulateColorAddAlpha, ModulateInvAlphaAddColor, ModulateInvColorAddAlpha
};
// The engine's own argument numbering (not D3DTA_*). On stage 0, Current means Diffuse.
enum class TextureArg : uint8_t { Current, Diffuse, SelectMask, Specular, Temp, Texture, TFactor };

// `enables` bits (decoded from what d3dgl.dll reads; there is no enum in the PDB).
namespace rs_enable {
constexpr uint32_t kDepthTest = 0x1, kDepthWrite = 0x2, kAlphaBlend = 0x4, kStencil = 0x8, kFog = 0x10, kAlphaTest = 0x80,
                   kLighting = 0x200, kTLFakeLight = 0x20000, kPointSampling = 0x400000;
}

struct TextureStage {
	TextureOp color_op = TextureOp::Disable;
	TextureArg color_arg1 = TextureArg::Texture, color_arg2 = TextureArg::Current;
	TextureOp alpha_op = TextureOp::Disable;
	TextureArg alpha_arg1 = TextureArg::Texture, alpha_arg2 = TextureArg::Current;
	uint8_t texcoord = 0;  // which vertex UV set
	TexAddress address_u = TexAddress::Wrap, address_v = TexAddress::Wrap;
};

struct RenderState {
	const char* name = "";
	uint32_t enables = 0;
	BlendMode src_blend = BlendMode::One, dst_blend = BlendMode::Zero;
	CompareFunc alpha_func = CompareFunc::Always;
	uint8_t alpha_ref = 0;
	CompareFunc depth_func = CompareFunc::LessEqual;
	int8_t depth_bias = 0;  // applied in the vertex shader: view-space z pulled toward the camera by 8 x bias
	CullMode cull = CullMode::CCW;
	FillMode fill = FillMode::Solid;
	uint32_t tfactor = 0xFFFFFFFF;  // 0xAABBGGRR (red in the low byte)
	TextureStage stage[2];

	bool has(uint32_t bit) const { return (enables & bit) != 0; }
};

constexpr int kNamedRenderStates = 50;

// Index 0..49, in the game's order (Unit Blend = 0, Ammo Blend = 31, ...).
const RenderState& named_render_state(int index);
// Exact (case-sensitive) name match, as the game's XML lookups do. Null if unknown.
const RenderState* find_render_state(const std::string& name);
int render_state_index(const std::string& name);  // -1 if unknown

}  // namespace ron
