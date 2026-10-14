#include "render/renderstate.hpp"

namespace ron {

namespace {

using O = TextureOp;
using A = TextureArg;
using X = TexAddress;
using B = BlendMode;
using C = CompareFunc;

// Generated from re/iface/renderstates.json by re/tools/gen-renderstate-table.mjs.
const RenderState kStates[kNamedRenderStates] = {
#include "render/renderstate_table.inc"
};

}  // namespace

const RenderState& named_render_state(int index) { return kStates[index]; }

int render_state_index(const std::string& name) {
	for (int i = 0; i < kNamedRenderStates; i++) {
		if (name == kStates[i].name) return i;
	}
	return -1;
}

const RenderState* find_render_state(const std::string& name) {
	const int i = render_state_index(name);
	return i < 0 ? nullptr : &kStates[i];
}

}  // namespace ron
