#include "assets/tga.hpp"

#include <algorithm>
#include <string>

namespace ron {

Image parse_tga(const Bytes& b) {
	if (b.size() < 18) throw FormatError("TGA too small");
	const int id_len = b[0], cmap_type = b[1], type = b[2], bpp = b[16], desc = b[17];
	const int w = read_u16(b, 12), h = read_u16(b, 14);
	if (cmap_type != 0 || (type != 2 && type != 10) || (bpp != 24 && bpp != 32)) {
		throw FormatError("unsupported TGA (type " + std::to_string(type) + ", " + std::to_string(bpp) + " bpp)");
	}
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192) throw FormatError("bad TGA dimensions");

	Image img;
	img.width = w;
	img.height = h;
	img.rgba.resize(size_t(w) * h * 4);
	const int px = bpp / 8;
	size_t p = 18 + size_t(id_len);
	const size_t total = size_t(w) * h;

	auto put = [&](size_t i, size_t s) {
		if (s + px > b.size()) throw FormatError("TGA pixel data truncated");
		uint8_t* d = &img.rgba[i * 4];
		d[0] = b[s + 2];
		d[1] = b[s + 1];
		d[2] = b[s];
		d[3] = px == 4 ? b[s + 3] : 255;
	};

	if (type == 2) {
		for (size_t i = 0; i < total; i++, p += px) put(i, p);
	} else {
		size_t i = 0;
		while (i < total) {
			if (p >= b.size()) throw FormatError("TGA RLE data truncated");
			const uint8_t hdr = b[p++];
			const size_t n = std::min<size_t>((hdr & 0x7f) + 1, total - i);
			if (hdr & 0x80) {
				for (size_t k = 0; k < n; k++) put(i++, p);
				p += px;
			} else {
				for (size_t k = 0; k < n; k++, p += px) put(i++, p);
			}
		}
	}

	// Bit 5 of the descriptor set means the rows are already stored top-down.
	if (!(desc & 0x20)) {
		const size_t row = size_t(w) * 4;
		for (int y = 0; y < h / 2; y++) {
			std::swap_ranges(img.rgba.begin() + y * row, img.rgba.begin() + (y + 1) * row,
			                 img.rgba.begin() + (h - 1 - y) * row);
		}
	}
	return img;
}

}  // namespace ron
