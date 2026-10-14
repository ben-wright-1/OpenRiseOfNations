// .BH3 mesh + node hierarchy (BH3Method::load_bh3 / load_hier in riseofnations.exe). See docs/formats.md.
#pragma once

#include <string>
#include <vector>

#include "assets/chunk.hpp"
#include "assets/mathx.hpp"

namespace ron {

struct BH3Node {
	std::string name;
	uint32_t first_vertex = 0;  // this node rigidly owns vertices [first_vertex, first_vertex + vertex_count)
	uint32_t vertex_count = 0;
	Xform rest;                 // parent-to-local transform
	std::vector<BH3Node> children;
};

struct BH3Mesh {
	std::vector<float> positions;   // xyz per vertex, in the owning node's local space
	std::vector<float> normals;     // xyz per vertex
	std::vector<float> uvs;         // uv per vertex (top-left texture origin)
	std::vector<uint16_t> indices;  // triangle list
	size_t vertex_count() const { return positions.size() / 3; }
};

struct BH3Model {
	std::vector<BH3Mesh> meshes;  // node vertex ranges index meshes[0]
	BH3Node root;
};

BH3Model parse_bh3(const Bytes& bytes);

}  // namespace ron
