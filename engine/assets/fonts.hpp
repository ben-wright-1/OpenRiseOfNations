// Fonts (docs/ui-spec.md section 3): Data/fonts.xml (FontFamily::init @00521330: the "default" CODEPAGE, the
// RESOLUTION closest to the screen width, FONT entries, LOOKUP tags), font selection by size and style
// (FontFamily::get @00522810) and the TrueType metrics EE derives with FreeType (FT::FontFace::SetupFace @10009c00:
// FT_Set_Pixel_Sizes(size, size), LineHeight / Ascent / Descent / FontHeight, hinted integer advances, no kerning).
// Rasterisation is the platform's job; this reader gives the layout numbers. Reference: re/tools/fontpreview.mjs.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "assets/chunk.hpp"
#include "assets/files.hpp"
#include "assets/strings.hpp"

namespace ron {

// FontFamily::string_to_style @00521260: B = 1, I = 2, U = 4, S = 8, O = 16, A = 32 (any case); "?" = -1.
int font_style_bits(const std::string& style);
enum : int { kFontBold = 1, kFontItalic = 2, kFontUnderline = 4, kFontStrike = 8, kFontOutline = 16, kFontAliased = 32 };

struct FontEntry {
	std::string file;  // game-relative path, e.g. Data/Fonts/georgiab.ttf
	std::string name;  // family name in the XML (Georgia, Arial)
	int size = 0, tex_size = 0;  // pixel em size (FT_Set_Pixel_Sizes), texture face size
	std::string style;
	int bits = 0;
};

struct FontLookupRes {
	int res = 0, size = 0;
	int bits = -1;  // style bits, -1 when the entry has none (any style)
};

struct FontsXml {
	int res = 1024;                    // the chosen RESOLUTION
	bool ea_line_breaking = false;     // CODEPAGE use_ea_line_breaking
	std::vector<FontEntry> fonts;      // of the chosen resolution, in file order
	std::vector<std::pair<std::string, std::vector<FontLookupRes>>> lookups;  // tag -> per resolution, file order

	// FontFamily::get(size, style): exact size+style, then same size differing only in bold, then nearest size with
	// any style. style < 0 means the default style (0). Returns an index into `fonts`, or -1.
	int pick(int size, int style) const;
	// By LOOKUP tag (the entry for the chosen resolution, else the first): index into `fonts`, or -1.
	int pick_tag(const std::string& tag) const;
};

// Picks the RESOLUTION with the smallest |res - screen_width| (ties keep the first).
FontsXml load_fonts_xml(const GameFiles& files, Language lang, int screen_width);

// The parts of a TrueType file the layout needs.
struct TtfFace {
	int units_per_em = 2048, head_flags = 0;
	int ascender = 0, descender = 0, line_gap = 0, advance_width_max = 0;
	int num_hmetrics = 0, num_glyphs = 0;
	Bytes data;
	size_t hmtx = 0, cmap4 = 0;  // table offsets (cmap4: the Windows Unicode BMP format-4 subtable, 0 if none)

	uint16_t glyph_index(uint32_t codepoint) const;
	int advance_units(uint16_t glyph) const;
};
TtfFace parse_ttf(Bytes data);

// FreeType's scaled metrics at a pixel size (26.6 rounded to whole pixels), as SetupFace keeps them.
struct FontMetrics {
	int line_height = 0, font_height = 0, ascent = 0, descent = 0, max_advance = 0;
};
FontMetrics ttf_metrics(const TtfFace& f, int size);
// Hinted advance in whole pixels for a code point at a pixel size.
int ttf_advance(const TtfFace& f, int size, uint32_t codepoint);

}  // namespace ron
