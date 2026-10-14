// BHG chunk container, as read by ChunkRead in riseofnations.exe.
// Header: u32 size (including the 8-byte header), u16 id, u16 aux. Little-endian.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace ron {

using Bytes = std::vector<uint8_t>;

struct Chunk {
	uint16_t id = 0;
	uint16_t aux = 0;   // child count for tree nodes, otherwise version/flags
	size_t start = 0;   // payload start (after the header)
	size_t end = 0;     // payload end
};

struct FormatError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

// Bounds-checked little-endian reads.
inline uint32_t read_u32(const Bytes& b, size_t p) {
	if (p + 4 > b.size()) throw FormatError("read past end of file");
	uint32_t v;
	std::memcpy(&v, b.data() + p, 4);
	return v;
}
inline uint16_t read_u16(const Bytes& b, size_t p) {
	if (p + 2 > b.size()) throw FormatError("read past end of file");
	uint16_t v;
	std::memcpy(&v, b.data() + p, 2);
	return v;
}
inline float read_f32(const Bytes& b, size_t p) {
	if (p + 4 > b.size()) throw FormatError("read past end of file");
	float v;
	std::memcpy(&v, b.data() + p, 4);
	return v;
}

// Split [start, end) into consecutive chunks. Throws FormatError on malformed sizes.
std::vector<Chunk> read_chunks(const Bytes& b, size_t start, size_t end);
inline std::vector<Chunk> read_chunks(const Bytes& b) { return read_chunks(b, 0, b.size()); }

// The children of a file's single id-0 root chunk (or the top-level list if there is no root wrapper).
std::vector<Chunk> read_top_level(const Bytes& b);

Bytes read_file(const std::string& path);

}  // namespace ron
