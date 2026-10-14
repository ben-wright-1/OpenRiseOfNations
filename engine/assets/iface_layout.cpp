#include "assets/iface_layout.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>

#include "assets/graphics_txt.hpp"

namespace ron {

namespace {

std::string trim(const std::string& s) {
	const size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return std::string();
	return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

bool ieq(const std::string& a, const char* b) {
	const size_t n = std::strlen(b);
	if (a.size() != n) return false;
	for (size_t i = 0; i < n; i++) {
		if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
	}
	return true;
}

constexpr int kMissing = -0x790b;  // get_attrib_rect_ex's "attribute absent" sentinel

int attr_or_missing(const XmlElement& e, const char* key) {
	auto it = e.attrs.find(key);
	return it == e.attrs.end() ? kMissing : iface_int(it->second);
}

int per_mille(int v, int p) { return int((int64_t(v) * p) / 1000); }  // C truncation

int16_t s16(int v) { return int16_t(uint16_t(v & 0xFFFF)); }

// The value of a raw (case-preserved) attribute, trimmed; null when absent. Used only by the dump, which mirrors the
// reference tool's exact-case lookups.
const std::string* raw_attr(const XmlElement& e, const char* key) {
	for (const auto& [k, v] : e.raw_attrs) {
		if (k == key) return &v;
	}
	return nullptr;
}

}  // namespace

int iface_int(const std::string& v) { return std::atoi(trim(v).c_str()); }

std::string iface_attr(const XmlElement& e, const std::string& key) {
	auto it = e.attrs.find(key);
	return it == e.attrs.end() ? std::string() : trim(it->second);
}

bool iface_tag_is(const XmlElement& e, const char* tag) { return ieq(e.name, tag); }

// Per axis: lo/hi edges from the first attribute present (section 2.3).
IfaceRect resolve_iface_rect(const XmlElement& e, int pw, int ph) {
	auto first = [&](std::initializer_list<const char*> keys, const char*& hit) {
		for (const char* k : keys) {
			const int v = attr_or_missing(e, k);
			if (v != kMissing || e.attrs.count(k)) {
				hit = k;
				return v == kMissing ? 0 : v;
			}
		}
		hit = nullptr;
		return 0;
	};
	auto edge = [&](const char* k, int v, int P, int size) {
		if (!k) return 0;
		const std::string s = k;
		if (s == "left" || s == "right" || s == "top" || s == "bottom") return v;
		if (s.find("_inverse_percent") != std::string::npos) return P - per_mille(v, P);
		if (s.find("_inverse") != std::string::npos) return P - v;
		if (s.find("_percent") != std::string::npos) return per_mille(v, P);
		(void)size;
		return 0;
	};
	auto axis = [&](const char* sz, const char* pct, const char* diff, const char* lo, const char* hi, const char* center,
	                int P, bool y, int& out_lo, int& out_hi) {
		const int s0 = attr_or_missing(e, sz) == kMissing ? 0 : attr_or_missing(e, sz);
		const int pct_v = s0 == 0 && attr_or_missing(e, pct) != kMissing ? attr_or_missing(e, pct) : 0;
		const int d = attr_or_missing(e, diff) == kMissing ? 0 : attr_or_missing(e, diff);
		const int s = s0 != 0 ? s0 : per_mille(P - d, pct_v);
		const std::string L = lo, H = hi;
		const char* k = nullptr;
		if (s0 > 0 || pct_v > 0) {
			const int v = first({lo, (L + "_inverse").c_str(), (L + "_inverse_percent").c_str(), (L + "_percent").c_str(), center}, k);
			if (k && std::strcmp(k, center) == 0) out_lo = y ? v + (P - s) / 2 : v + (P / 2 - s / 2);
			else out_lo = edge(k, v, P, s);
			out_hi = out_lo + s;
		} else if (s0 < 0 || pct_v < 0) {
			const int v = first({hi, (H + "_inverse").c_str(), (H + "_inverse_percent").c_str(), (H + "_percent").c_str(), center}, k);
			if (k && std::strcmp(k, center) == 0) out_hi = y ? v - (P - s) / 2 : v + (P / 2 - s / 2);
			else out_hi = edge(k, v, P, s);
			out_lo = out_hi + s;
		} else {
			int v = first({lo, (L + "_inverse").c_str(), (L + "_inverse_percent").c_str(), (L + "_percent").c_str()}, k);
			out_lo = edge(k, v, P, 0);
			v = first({hi, (H + "_inverse").c_str(), (H + "_inverse_percent").c_str(), (H + "_percent").c_str()}, k);
			out_hi = edge(k, v, P, 0);
		}
	};
	IfaceRect r;
	axis("width", "percent_width", "x_percent_diff", "left", "right", "center_off_x", pw, false, r.l, r.r);
	axis("height", "percent_height", "y_percent_diff", "top", "bottom", "center_off_y", ph, true, r.t, r.b);
	r.l = s16(r.l), r.r = s16(r.r), r.t = s16(r.t), r.b = s16(r.b);
	return r;
}

bool has_iface_rect(const XmlElement& e) {
	for (const auto& [k, v] : e.raw_attrs) {
		(void)v;
		for (const char* p : {"left", "right", "top", "bottom", "width", "height", "percent_", "center_off"}) {
			if (k.rfind(p, 0) == 0) return true;
		}
		if (k.find("_inverse") != std::string::npos || k.find("_percent") != std::string::npos) return true;
	}
	return false;
}

IfacePiece iface_piece(const XmlElement& e) {
	IfacePiece p;
	p.name = iface_attr(e, "name");
	p.texture = iface_attr(e, "texture");
	p.left = iface_int(iface_attr(e, "left"));
	p.top = iface_int(iface_attr(e, "top"));
	p.width = iface_int(iface_attr(e, "width"));
	p.height = iface_int(iface_attr(e, "height"));
	return p;
}

IfaceBorder iface_border(const XmlElement& e) {
	IfaceBorder b;
	b.name = iface_attr(e, "name");
	for (const XmlElement& c : e.children) {
		if (!iface_tag_is(c, "IFACETEX")) continue;
		const std::string n = to_lower(iface_attr(c, "name"));
		int slot = -1;
		if (n.size() == 7 && n.rfind("corner", 0) == 0) slot = n[6] - '0';
		else if (n.size() == 5 && n.rfind("edge", 0) == 0) slot = 4 + (n[4] - '0');
		else if (n.size() == 6 && n.rfind("extra", 0) == 0) slot = 8 + (n[5] - '0');
		if (slot >= 0 && slot < 16) b.slot[slot] = iface_piece(c);
	}
	return b;
}

uint32_t iface_color(const XmlElement& e) {
	const int r = iface_int(iface_attr(e, "red")), g = iface_int(iface_attr(e, "green")), b = iface_int(iface_attr(e, "blue"));
	const int a = e.has("alpha") ? iface_int(iface_attr(e, "alpha")) : 255;
	return uint32_t(a & 255) << 24 | uint32_t(r & 255) << 16 | uint32_t(g & 255) << 8 | uint32_t(b & 255);
}

IfaceLayout::IfaceLayout(const std::string& xml_text) : doc_(parse_xml(xml_text)) {
	static const XmlElement empty;
	root_ = doc_.children.empty() ? &empty : &doc_.children.front();
	// Depth-first, document order.
	auto visit = [&](const XmlElement& e, auto&& self) -> void {
		if (iface_tag_is(e, "IFACETEXLOAD")) {
			const std::string id = iface_attr(e, "id");
			std::string file = iface_attr(e, "file");
			for (char& c : file) {
				if (c == '\\') c = '/';
			}
			bool found = false;
			for (auto& [k, v] : texloads_) {
				if (k == id) v = file, found = true;
			}
			if (!found) texloads_.emplace_back(id, file);
		}
		for (const XmlElement& c : e.children) self(c, self);
	};
	visit(*root_, visit);
}

const XmlElement* IfaceLayout::find(const std::string& name) const {
	auto rec = [&](const XmlElement& e, auto&& self) -> const XmlElement* {
		if (ieq(e.name, name.c_str()) || iface_attr(e, "name") == name) return &e;
		for (const XmlElement& c : e.children) {
			if (const XmlElement* f = self(c, self)) return f;
		}
		return nullptr;
	};
	return rec(*root_, rec);
}

std::string IfaceLayout::texload_file(const std::string& id) const {
	for (const auto& [k, v] : texloads_) {
		if (k == id) return v;
	}
	return std::string();
}

namespace {

const std::set<std::string> kPieces = {"IFACETEX", "IFACETEXLOAD", "IFACEBORDER", "IFACECOLOR", "IFACEPOINT", "IFACESPRITE",
                                       "STRETCHSPRITE", "FONT", "FONTCOLOR", "STYLE", "CATEGORY", "CATEGORIES", "PARAMS", "CHECK"};

std::string fmt_rect(const IfaceRect& r) {
	std::ostringstream o;
	o << '(' << r.l << ',' << r.t << ")-(" << r.r << ',' << r.b << ") " << r.w() << 'x' << r.h();
	return o.str();
}

std::string json_escape(const std::string& s) {
	std::string o;
	for (char c : s) {
		if (c == '"' || c == '\\') o += '\\';
		o += c;
	}
	return o;
}

// POSITIONING (GraphicWinXML::add_elements @00516700): child i shifts by (x + i*inc_x, y + i*inc_y); *_percent are
// per mille of the window size.
struct Positioning {
	int x = 0, y = 0, ix = 0, iy = 0;
};
Positioning positioning(const XmlElement& e, int pw, int ph) {
	auto v = [&](const char* k, int P) {
		if (e.attrs.count(k)) return int(std::atof(iface_attr(e, k).c_str()));
		const std::string kp = std::string(k) + "_percent";
		if (e.attrs.count(kp)) return int((std::atof(iface_attr(e, kp).c_str()) * P) / 1000);
		return 0;
	};
	return {v("x", pw), v("y", ph), v("inc_x", pw), v("inc_y", ph)};
}

struct Dumper {
	bool all;
	int max_depth;
	std::ostringstream out;

	void line(int depth, const std::string& s) { out << std::string(size_t(depth) * 2, ' ') << s << '\n'; }

	static std::string nm(const XmlElement& e) {
		const std::string* n = raw_attr(e, "name");
		if (n && !trim(*n).empty()) return " \"" + trim(*n) + "\"";
		const std::string* id = raw_attr(e, "id");
		if (id && !trim(*id).empty()) return " id=" + trim(*id);
		return std::string();
	}

	static std::string kv(const XmlElement& e, bool skip_name_id) {
		std::string s;
		bool first = true;
		for (const auto& [k, v] : e.raw_attrs) {
			if (skip_name_id && (k == "name" || k == "id")) continue;
			if (!first) s += ' ';
			s += k + "=" + trim(v);
			first = false;
		}
		return s;
	}

	void tree(const XmlElement& e, int pw, int ph, int depth, int ox = 0, int oy = 0, int px = 0, int py = 0) {
		if (depth > max_depth) return;
		const std::string tag = e.name;
		auto raw = [&](const char* k) {
			const std::string* v = raw_attr(e, k);
			return v ? trim(*v) : std::string("undefined");
		};
		if (tag == "IFACETEX") {
			if (all) line(depth, "IFACETEX" + nm(e) + " " + raw("texture") + " [" + raw("left") + "," + raw("top") + " " + raw("width") + "x" + raw("height") + "]");
			return;
		}
		if (kPieces.count(tag) && tag != "IFACEBORDER" && tag != "IFACEPOINT") {
			if (all || tag == "STYLE" || tag == "IFACETEXLOAD") line(depth, tag + nm(e) + " " + kv(e, true));
			return;
		}
		if (tag == "IFACEBORDER") {
			std::string slots;
			for (const XmlElement& c : e.children) {
				if (c.name != "IFACETEX") continue;
				if (!slots.empty()) slots += ',';
				const std::string* n = raw_attr(c, "name");
				slots += n ? trim(*n) : std::string();
			}
			line(depth, "IFACEBORDER" + nm(e) + " [" + slots + "]");
			return;
		}
		if (tag == "IFACEPOINT") {
			std::string j = "{";
			bool first = true;
			for (const auto& [k, v] : e.raw_attrs) {
				if (!first) j += ',';
				j += "\"" + json_escape(k) + "\":\"" + json_escape(trim(v)) + "\"";
				first = false;
			}
			line(depth, "IFACEPOINT" + nm(e) + " " + j + "}");
			return;
		}
		if (tag == "POSITIONING") {
			const Positioning p = positioning(e, pw, ph);
			line(depth, "POSITIONING " + kv(e, false));
			int i = 0;
			for (const XmlElement& c : e.children) {
				const bool shift = !kPieces.count(c.name) || c.name == "POSITIONING";
				tree(c, pw, ph, depth + 1, ox, oy, px + p.x + i * p.ix, py + p.y + i * p.iy);
				if (shift) i++;
			}
			return;
		}
		if (has_iface_rect(e)) {
			const IfaceRect r = resolve_iface_rect(e, pw, ph).offset(px, py);
			const IfaceRect abs = r.offset(ox, oy);
			const std::string* show = raw_attr(e, "show");
			line(depth, tag + nm(e) + " " + fmt_rect(r) + "  screen " + fmt_rect(abs) + (show && trim(*show) == "0" ? " hidden" : ""));
			const bool win = tag != "IFACERECT";
			for (const XmlElement& c : e.children) {
				tree(c, win ? r.w() : pw, win ? r.h() : ph, depth + 1, win ? abs.l : ox, win ? abs.t : oy);
			}
		} else {
			line(depth, tag + nm(e));
			for (const XmlElement& c : e.children) tree(c, pw, ph, depth + 1, ox, oy, px, py);
		}
	}
};

}  // namespace

std::string IfaceLayout::dump_tree(const XmlElement& e, int w, int h, bool all, int max_depth) const {
	Dumper d{all, max_depth, {}};
	d.tree(e, w, h, 0);
	return d.out.str();
}

std::vector<IfaceLaidOut> iface_layout_children(const XmlElement& win, int pw, int ph) {
	std::vector<IfaceLaidOut> out;
	auto group = [&](const XmlElement& pos, int px, int py, auto&& self) -> void {
		const Positioning p = positioning(pos, pw, ph);
		int i = 0;
		for (const XmlElement& k : pos.children) {
			const bool shift = !kPieces.count(k.name) || k.name == "POSITIONING";
			const int ox = px + p.x + i * p.ix, oy = py + p.y + i * p.iy;
			if (k.name == "POSITIONING") self(k, ox, oy, self);
			else if (!kPieces.count(k.name) && has_iface_rect(k)) out.push_back({&k, resolve_iface_rect(k, pw, ph).offset(ox, oy)});
			if (shift) i++;
		}
	};
	for (const XmlElement& c : win.children) {
		if (c.name == "POSITIONING") group(c, 0, 0, group);
		else if (!kPieces.count(c.name) && has_iface_rect(c)) out.push_back({&c, resolve_iface_rect(c, pw, ph)});
	}
	return out;
}

bool iface_find_rect(const XmlElement& win, const char* tag, const std::string& name, int pw, int ph, IfaceRect& out, int nth) {
	for (const IfaceLaidOut& l : iface_layout_children(win, pw, ph)) {
		if (iface_tag_is(*l.el, tag) && iface_attr(*l.el, "name") == name && nth-- == 0) {
			out = l.rect;
			return true;
		}
	}
	return false;
}

const XmlElement* iface_child(const XmlElement& win, const char* tag, const std::string& name, int nth) {
	for (const XmlElement& c : win.children) {
		if (iface_tag_is(c, tag) && iface_attr(c, "name") == name && nth-- == 0) return &c;
	}
	return nullptr;
}

std::vector<const XmlElement*> iface_children(const XmlElement& win, const char* tag, const std::string& name) {
	std::vector<const XmlElement*> out;
	for (const XmlElement& c : win.children) {
		if (iface_tag_is(c, tag) && (name.empty() || iface_attr(c, "name") == name)) out.push_back(&c);
	}
	return out;
}

IfacePiece iface_find_piece(const XmlElement& win, const std::string& name, int nth) {
	const XmlElement* e = iface_child(win, "IFACETEX", name, nth);
	return e ? iface_piece(*e) : IfacePiece{};
}

IfaceBorder iface_find_border(const XmlElement& win, const std::string& name) {
	const XmlElement* e = iface_child(win, "IFACEBORDER", name);
	return e ? iface_border(*e) : IfaceBorder{};
}

// ---------------------------------------------------------------- interface.txt

InterfaceTxt::InterfaceTxt(const std::string& text) {
	std::string section;
	size_t p = 0;
	while (p < text.size()) {
		size_t e = text.find('\n', p);
		if (e == std::string::npos) e = text.size();
		std::string line = trim(text.substr(p, e - p));
		p = e + 1;
		if (line.empty() || line[0] == ';') continue;
		if (line[0] == '#') {
			section = trim(line.substr(1));
			const size_t sp = section.find_first_of(" \t/");
			if (sp != std::string::npos) section = section.substr(0, sp);
			continue;
		}
		std::vector<std::string> f = split_fields(line);
		if (f.empty()) continue;
		lines_.push_back({section, f[0], std::vector<std::string>(f.begin() + 1, f.end())});
	}
	for (int set = 0; set < 2; set++) {
		for (int i = 0; i < 10; i++) digit[set][i] = piece("IFACERENDERMANAGER", "NUMBERS_TEX[" + std::to_string(set) + "][" + std::to_string(i) + "]");
		digit[set][10] = piece("IFACERENDERMANAGER", "SLASH_TEX[" + std::to_string(set) + "]");
		digit[set][11] = piece("IFACERENDERMANAGER", "PLUS_TEX[" + std::to_string(set) + "]");
		digit[set][12] = piece("IFACERENDERMANAGER", "MINUS_TEX[" + std::to_string(set) + "]");
	}
}

IfacePiece InterfaceTxt::piece(const std::string& section, const std::string& key) const {
	for (const Line& l : lines_) {
		if (l.section != section || l.key != key || l.items.size() < 5) continue;
		IfacePiece p;
		p.name = key;
		p.texture = l.items[0];
		p.left = iface_int(l.items[1]);
		p.top = iface_int(l.items[2]);
		p.width = iface_int(l.items[3]);
		p.height = iface_int(l.items[4]);
		return p;
	}
	return IfacePiece{};
}

InterfaceTxt load_interface_txt(const GameFiles& files) { return InterfaceTxt(files.read_text("interface.txt")); }

int iface_digit_index(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c == '/') return 10;
	if (c == '+') return 11;
	if (c == '-') return 12;
	return -1;
}

// ---------------------------------------------------------------- skins

std::string iface_skin_code(int age, int bc) {
	if (age <= 2) {
		if (bc == 0 || bc == 1) return "MD0";
		if (bc == 2 || bc == 3) return "AS0";
		if (bc == 6) return "IRO";
		return "SA0";  // 4, 5, 7, 8
	}
	if (age <= 4) {
		static const char* const k[9] = {"MD3", "NE3", "AS3", "KO3", "SA3", "TR3", "IR3", "USA", "EIN"};
		return bc >= 0 && bc < 9 ? k[bc] : "MD3";
	}
	return age == 5 ? "IND" : "MOD";
}

std::string iface_skin_file(const std::string& file, const std::string& code) {
	const std::string lower = to_lower(file);
	const size_t p = lower.find("iface_as0_");
	if (p == std::string::npos) return file;
	return file.substr(0, p + 6) + code + file.substr(p + 9);
}

}  // namespace ron
