#include "assets/chunk.hpp"

#include <fstream>
#include <iterator>

namespace ron {

std::vector<Chunk> read_chunks(const Bytes& b, size_t start, size_t end) {
	std::vector<Chunk> out;
	size_t p = start;
	while (p + 8 <= end) {
		const uint32_t size = read_u32(b, p);
		if (size < 8 || p + size > end) {
			throw FormatError("bad chunk size at offset " + std::to_string(p));
		}
		out.push_back({read_u16(b, p + 4), read_u16(b, p + 6), p + 8, p + size});
		p += size;
	}
	return out;
}

std::vector<Chunk> read_top_level(const Bytes& b) {
	auto top = read_chunks(b);
	if (top.size() == 1 && top[0].id == 0) return read_chunks(b, top[0].start, top[0].end);
	return top;
}

Bytes read_file(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) throw std::runtime_error("cannot open " + path);
	return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace ron
