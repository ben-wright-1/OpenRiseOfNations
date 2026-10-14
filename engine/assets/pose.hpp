// Rigid "skinning" of a BH3 model: each node owns a vertex range and moves it by its world transform,
// where world = parent.world * rest * anim (HierObj::update_transforms in riseofnations.exe).
#pragma once

#include <array>
#include <vector>

#include "assets/bh3.hpp"
#include "assets/bha.hpp"

namespace ron {

struct PosedVertex {
	float px, py, pz;
	float nx, ny, nz;
	float u, v;
};
static_assert(sizeof(PosedVertex) == 32, "PosedVertex layout is shared with GPU shaders");

// Poses meshes[0] at `ms` (anim may be null for the rest pose). `out` is resized to the vertex count.
// If `node_world` is given it receives every node's world transform, in depth-first pre-order (file order).
// Nodes named "pivot node N" keep an identity local transform: the game skips their keys (turrets aim them).
void pose_model(const BH3Model& model, const Animation* anim, uint32_t ms, std::vector<PosedVertex>& out,
                std::vector<Xform>* node_world = nullptr);

// Attach slots (NodePacket, filled by BH3Method::load_hier): slot -> node index in depth-first pre-order, or -1.
// "ammo node i" -> i, "pivot node i" -> i+4, "smoke node i" -> i+8, "node i" -> i (exact, case-insensitive);
// later nodes win; slot 13 starts as the root and "Bip01 Spine" sets 12 and 13. EXISTEVENT/RELEASEEVENT node= index these.
constexpr int kAttachSlots = 14;
std::array<int, kAttachSlots> attach_slots(const BH3Model& model);

// Applies `x` to positions and normals.
void transform_vertices(const Xform& x, std::vector<PosedVertex>& v);

// Bounding sphere of posed vertices.
void bounding_sphere(const std::vector<PosedVertex>& v, Vec3& center, float& radius);

}  // namespace ron
