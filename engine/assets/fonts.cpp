#include "assets/fonts.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>

#include "assets/xml.hpp"

namespace ron {

namespace {

uint16_t be16(const Bytes& b, size_t p) { return p + 2 <= b.size() ? uint16_t(b[p] << 8 | b[p + 1]) : 0; }
int16_t sbe16(const Bytes& b, size_t p) { return int16_t(be16(b, p)); }
uint32_t be32(const Bytes& b, size_t p) { return uint32_t(be16(b, p)) << 16 | be16(b, p + 2); }

std::string trimmed(const std::string& s) {
	const size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return std::string();
	return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

}  // namespace

int font_style_bits(const std::string& style) {
	if (trimmed(style) == "?") return -1;
	int bits = 0;
	for (char c : style) {
		switch (std::toupper(static_cast<unsigned char>(c))) {
			case 'B': bits |= 1; break;
			case 'I': bits |= 2; break;
			case 'U': bits |= 4; break;
			case 'S': bits |= 8; break;
			case 'O': bits |= 16; break;
			case 'A': bits |= 32; break;
			default: break;
		}
	}
	return bits;
}

int FontsXml::pick(int size, int style) const {
	if (style < 0) style = 0;  // def_style
	// Passes 1, 2, 3, 5 of FontFamily::get (0 and 4 are skipped): the closest size wins within the first pass that
	// matches anything.
	for (int pass : {1, 2, 3, 5}) {
		int best = -1, best_d = 999999;
		for (size_t i = 0; i < fonts.size(); i++) {
			const FontEntry& f = fonts[i];
			if (pass <= 3 && f.size != size) continue;
			if (pass <= 1 && f.bits != style) continue;
			if (pass <= 2 && ((f.bits ^ style) & ~0x20) != 0) continue;
			if (pass <= 3 && ((f.bits ^ style) & ~0x01) != 0) continue;
			const int d = std::abs(size - f.size);
			if (d < best_d) best = int(i), best_d = d;
		}
		if (best >= 0) return best;
	}
	return -1;
}

int FontsXml::pick_tag(const std::string& tag) const {
	for (const auto& [t, list] : lookups) {
		if (t != tag || list.empty()) continue;
		const FontLookupRes* hit = &list.front();
		for (const FontLookupRes& r : list) {
			if (r.res == res) hit = &r;
		}
		return pick(hit->size, hit->bits);
	}
	return -1;
}

FontsXml load_fonts_xml(const GameFiles& files, Language lang, int screen_width) {
	FontsXml out;
	const XmlElement doc = parse_xml(read_localised(files, "fonts.xml", lang));
	const XmlElement* root = doc.children.empty() ? nullptr : &doc.children.front();
	if (!root) return out;
	const XmlElement* fonts = root->child("FONTS");
	const XmlElement* cp = nullptr;
	if (fonts) {
		for (const XmlElement& c : fonts->children) {
			if (c.name == "CODEPAGE" && to_lower(trimmed(c.get("name"))) == "default") {
				cp = &c;
				break;
			}
		}
	}
	if (cp) {
		out.ea_line_breaking = cp->get_int("use_ea_line_breaking") != 0;
		const XmlElement* best = nullptr;
		int best_d = 1 << 30;
		for (const XmlElement& r : cp->children) {
			if (r.name != "RESOLUTION") continue;
			const int d = std::abs(r.get_int("res") - screen_width);
			if (d < best_d) best_d = d, best = &r;
		}
		if (best) {
			out.res = best->get_int("res");
			for (const XmlElement& f : best->children) {
				if (f.name != "FONT") continue;
				FontEntry e;
				e.file = normalize_game_path(trimmed(f.get("filename")));
				e.name = trimmed(f.get("name"));
				e.size = f.get_int("size");
				e.tex_size = f.has("tex_size") ? f.get_int("tex_size") : e.size;
				e.style = trimmed(f.get("style"));
				e.bits = font_style_bits(e.style);
				out.fonts.push_back(e);
			}
		}
	}
	if (const XmlElement* lk = root->child("LOOKUPS")) {
		for (const XmlElement& l : lk->children) {
			if (l.name != "LOOKUP") continue;
			std::vector<FontLookupRes> list;
			for (const XmlElement& r : l.children) {
				if (r.name != "RESOLUTION") continue;
				list.push_back({r.get_int("res"), r.get_int("size"), r.has("style") ? font_style_bits(r.get("style")) : -1});
			}
			out.lookups.emplace_back(trimmed(l.get("tag")), list);
		}
	}
	return out;
}

TtfFace parse_ttf(Bytes data) {
	TtfFace f;
	f.data = std::move(data);
	const Bytes& b = f.data;
	const int n = be16(b, 4);
	size_t head = 0, hhea = 0, maxp = 0, cmap = 0;
	for (int i = 0; i < n; i++) {
		const size_t o = 12 + size_t(i) * 16;
		const std::string tag(reinterpret_cast<const char*>(b.data() + o), 4);
		const size_t off = be32(b, o + 8);
		if (tag == "head") head = off;
		else if (tag == "hhea") hhea = off;
		else if (tag == "maxp") maxp = off;
		else if (tag == "hmtx") f.hmtx = off;
		else if (tag == "cmap") cmap = off;
	}
	if (!head || !hhea) throw FormatError("not a TrueType font");
	f.units_per_em = be16(b, head + 18);
	f.head_flags = be16(b, head + 16);
	f.ascender = sbe16(b, hhea + 4);
	f.descender = sbe16(b, hhea + 6);
	f.line_gap = sbe16(b, hhea + 8);
	f.advance_width_max = be16(b, hhea + 10);
	f.num_hmetrics = be16(b, hhea + 34);
	f.num_glyphs = maxp ? be16(b, maxp + 4) : 0;
	if (cmap) {
		const int count = be16(b, cmap + 2);
		for (int i = 0; i < count; i++) {
			const int pid = be16(b, cmap + 4 + size_t(i) * 8), eid = be16(b, cmap + 6 + size_t(i) * 8);
			const size_t sub = cmap + be32(b, cmap + 8 + size_t(i) * 8);
			if (pid == 3 && eid == 1 && be16(b, sub) == 4) f.cmap4 = sub;
		}
	}
	return f;
}

uint16_t TtfFace::glyph_index(uint32_t c) const {
	if (!cmap4 || c > 0xFFFF) return 0;
	const Bytes& b = data;
	const size_t sub = cmap4;
	const size_t segx2 = be16(b, sub + 6), ends = sub + 14, starts = ends + segx2 + 2, deltas = starts + segx2, ranges = deltas + segx2;
	for (size_t s = 0; s < segx2; s += 2) {
		if (c > be16(b, ends + s)) continue;
		const uint32_t start = be16(b, starts + s);
		if (c < start) return 0;
		const int delta = sbe16(b, deltas + s);
		const size_t ro = be16(b, ranges + s);
		if (!ro) return uint16_t((c + uint32_t(delta)) & 0xFFFF);
		const uint16_t g = be16(b, ranges + s + ro + (c - start) * 2);
		return g ? uint16_t((g + delta) & 0xFFFF) : 0;
	}
	return 0;
}

int TtfFace::advance_units(uint16_t g) const {
	if (!hmtx || num_hmetrics <= 0) return 0;
	const int i = g < num_hmetrics ? g : num_hmetrics - 1;
	return be16(data, hmtx + size_t(i) * 4);
}

FontMetrics ttf_metrics(const TtfFace& f, int size) {
	const double s = double(size) / f.units_per_em;
	// TrueType faces with head.flags bit 3 (integer ppem) round ascender / descender; others ceil / floor.
	const bool int_ppem = (f.head_flags & 8) != 0;
	FontMetrics m;
	m.ascent = int(int_ppem ? std::lround(f.ascender * s) : std::ceil(f.ascender * s));
	m.descent = int(int_ppem ? std::lround(f.descender * s) : std::floor(f.descender * s));
	m.line_height = int(std::lround((f.ascender - f.descender + f.line_gap) * s));
	m.font_height = m.ascent - m.descent;
	m.max_advance = int(std::lround(f.advance_width_max * s));
	return m;
}

int ttf_advance(const TtfFace& f, int size, uint32_t codepoint) {
	return int(std::lround(f.advance_units(f.glyph_index(codepoint)) * double(size) / f.units_per_em));
}

}  // namespace ron
