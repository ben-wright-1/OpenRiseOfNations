// Buildings (TypeIndex 414..542): buildingrules.xml BUILDING, building_graphics.xml mask keys and masks.txt
// footprints (BuildType::init @00632340 and the Types::init steps around it; docs/rules-spec.md section 8;
// port of re/tools/rules/buildings.mjs).
#include <stdexcept>
#include <string>

#include "sim/rules_internal.hpp"

namespace ron::sim::detail {

namespace {

constexpr uint32_t kGlobalUpgrade = 1u << 2;    // BUILD_FLAGS 'c': the predecessor's upgrade points here
constexpr uint32_t kUpgradeChain = 0x4000000u;  // has a FROM, or is some building's FROM

// BuildTypeData::basic_type @00639970: follow FROM to the root.
int basic_type(Loader& L, int t) {
	for (int guard = 0; guard < 1000; ++guard) {
		const int f = L.t(t).from;
		if (f < 0) return t;
		t = f;
	}
	throw std::runtime_error("FROM cycle at building " + to_utf8(L.t(t).name));
}

}  // namespace

void init_build(Loader& L, XNode* el, int t, int pass) {
	Type& o = L.t(t);
	if (pass == 0) {
		o.abbrev.clear();  // BuildType::close
		o.desc.clear();
	}
	o.type = t;
	if (pass == 0) {
		const ustr name = xml::get_text(el, u"NAME");
		L.set_name(o, name);
		o.graph_name = xml::get_text(el, u"GRAPH");
		o.help_name = xml::get_text(el, u"GRAPH");
		const ustr tn = xml::get_text(el, u"TYPENAME");
		o.type_name = tn.empty() ? o.name : tn;
		o.upgrade = -1;
		o.to = -1;
		return;
	}
	o.attack = 0;  // both overwritten below
	o.armor = 2;
	o.cat = L.cat_key("build_cats", xml::get_text(el, u"CAT"));
	o.grid_x = i8(xml::get_text_num(el, u"GRID_X"));
	o.grid_y = i8(xml::get_text_num(el, u"GRID_Y"));
	o.tex.id = xml::get_text(el, u"TEX_ID");
	o.tex.col = i8(xml::get_text_num(el, u"TEX_COL"));
	o.tex.row = i8(xml::get_text_num(el, u"TEX_ROW"));
	o.tex.x = i16(xml::get_text_num(el, u"TEX_X"));
	o.tex.y = i16(xml::get_text_num(el, u"TEX_Y"));
	o.tex.clip = i8(xml::get_text_num(el, u"TEX_CLIP"));
	o.hits = xml::get_text_num(el, u"HITS");
	o.town_hits = xml::get_text_num(el, u"TOWN_HITS");
	o.min_city_size = 0;
	o.misery_rate = xml::get_text_num(el, u"MISERY");
	o.to_hit = xml::get_text_num(el, u"TO_HIT");
	o.attenuate = xml::get_text_num(el, u"ATTENUATE");
	o.attack = imul(xml::get_text_num(el, u"ATTACK"), 10);
	const ustr range = xml::get_text(el, u"RANGE");
	o.min_range = convert_int(range);
	const size_t dash = range.find(u'-');
	o.max_range = dash == ustr::npos ? o.min_range : convert_int(range, dash + 1);
	o.splash_area = xml::get_text_num(el, u"SPLASH_AREA");
	o.splash_percent = xml::get_text_num(el, u"SPLASH_PERCENT");
	o.ammo_per_att = xml::get_text_num(el, u"AMMO_PER_ATT");
	o.proj_speed = xml::get_text_num(el, u"PROJ_SPEED");
	o.most_shots = xml::get_text_num(el, u"MOST_SHOTS");
	o.garrison_max = xml::get_text_num(el, u"GARRISON_MAX");
	o.base_arrows = xml::get_text_num(el, u"BASE_ARROWS");
	o.wonder_val = xml::get_text_num(el, u"WONDER_VAL");
	o.recharge = xml::get_text_num(el, u"RECHARGE");
	o.armor = xml::get_text_num(el, u"ARMOR");
	o.fly_high = xml::get_text_num(el, u"FLY_HIGH");
	o.fly_low = xml::get_text_num(el, u"FLY_LOW");
	o.obj_masks = int32_t(upper_letter_flags(xml::get_text(el, u"OBJ_MASKS")));
	o.x_size = xml::get_text_num(el, u"X_SIZE");
	o.y_size = xml::get_text_num(el, u"Y_SIZE");
	o.los = xml::get_text_num(el, u"LOS");
	o.science_los = xml::get_text_num(el, u"SCIENCE_LOS");
	o.build_flags = lower_letter_flags(xml::get_text(el, u"BUILD_FLAGS"));
	o.job_time = o.res_time = uint32_t(xml::get_text_num(el, u"JOB_TIME"));
	o.preq[0] = L.tech_key(xml::get_text(el, u"PREQ0"));
	o.preq[1] = L.tech_key(xml::get_text(el, u"PREQ1"));
	o.preq[2] = L.tech_key(xml::get_text(el, u"PREQ2"));
	o.from = L.build_key(xml::get_text(el, u"FROM"));
	// The predecessor learns its successor unless this building has OBJ_MASKS '1' (can't upgrade to it).
	if ((uint32_t(o.obj_masks) & 0x4000000u) == 0 && o.from >= 0) {
		Type& p = L.t(o.from);
		p.to = t;
		if (o.build_flags & kGlobalUpgrade) p.upgrade = t;
	}
	o.support[0] = L.good_key(xml::get_text(el, u"SUPPORT0"));
	o.support_cost[0] = xml::get_text_num(el, u"SUPPORTVALUE0");
	o.support[1] = L.good_key(xml::get_text(el, u"SUPPORT1"));
	o.support_cost[1] = xml::get_text_num(el, u"SUPPORTVALUE1");
	load_cost(xml::child_or_create(el, u"COST"), o.costs);
	o.plunder_value = xml::get_text_num(el, u"PLUNDER");
	o.plunder_good = L.good_key(xml::get_text(el, u"PLUNDER_GOOD"));
	o.behind_height = xml::get_text_num(el, u"BEHIND_HEIGHT");
	o.tribe_mask = parse_binary(xml::get_text(el, u"TRIBE_MASK"));
	o.where = o.from < 0 || basic_type(L, t) == t ? -1 : o.from;
	o.obs = L.tech_key(xml::get_text(el, u"OBSOLETE"));
	o.jump = L.build_key(xml::get_text(el, u"JUMP"));
	if (o.jump >= 0) o.upgrade = o.jump;
}

// Types::init post-loop: domain (BuildType::set_domain inlined; 0 land, 1 sea only, 2 land and sea) and the
// upgrade-chain flag on both ends of every FROM link.
void post_buildings(Rules& r) {
	for (int t = kBuilds; t < kEndBuilds; ++t) {
		Type& o = r.types[size_t(t)];
		o.domain = (o.build_flags & 2) ? int32_t(o.build_flags & 1) + 1 : 0;
		if (o.from >= 0) {
			r.types[size_t(o.from)].build_flags |= kUpgradeChain;
			o.build_flags |= kUpgradeChain;
		}
	}
}

// ---------------------------------------------------------------- masks.txt (Text::open / get / item)
namespace {

// JS toLowerCase over the Latin-1 range masks.txt can hold.
ustr lower_latin1(ustr s) {
	for (char16_t& c : s) {
		if ((c >= u'A' && c <= u'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) c = char16_t(c + 32);
	}
	return s;
}

class MaskText {
public:
	explicit MaskText(const Bytes& raw) {
		// fgetws in text mode: CRLF -> LF; lines split on LF.
		ustr cur;
		for (size_t i = 0; i < raw.size(); ++i) {
			if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
			if (raw[i] == '\n') {
				lines_.push_back(std::move(cur));
				cur.clear();
			} else {
				cur.push_back(char16_t(raw[i]));  // latin1
			}
		}
		lines_.push_back(std::move(cur));
	}

	// Text::open(file, key): rescan from the top for the line "#key" (case-insensitive, trailing spaces ignored).
	// An empty key opens at the top of the file.
	bool open(const ustr& key) {
		pos_ = 0;
		if (key.empty()) return true;
		const ustr want = lower_latin1(u"#" + key);
		while (pos_ < lines_.size()) {
			ustr l = lines_[pos_++];
			while (!l.empty() && l.back() == u' ') l.pop_back();
			if (lower_latin1(l) == want) {
				item_mark_ = l;
				return true;
			}
		}
		return false;
	}

	// Text::get: the next line that is not blank and not a ';' comment. A '#' line (the next section) or EOF gives "".
	ustr get() {
		while (pos_ < lines_.size()) {
			ustr l = lines_[pos_++];
			if (!l.empty() && l[0] == u'#') return ustr();
			size_t a = 0;
			while (a < l.size() && l[a] == u' ') ++a;
			l.erase(0, a);
			if (!l.empty() && l[0] == u';') continue;
			while (!l.empty() && l.back() == u' ') l.pop_back();
			if (!l.empty()) {
				item_mark_ = l;
				return l;
			}
		}
		return ustr();
	}

	// Text::item: up to the next ',' trimmed of spaces; an empty field repeats the previous item.
	ustr item() {
		const size_t k = item_mark_.find(u',');
		const size_t n = k == ustr::npos ? item_mark_.size() : k;
		if (n) {
			ustr s = item_mark_.substr(0, n);
			size_t a = 0, b = s.size();
			while (a < b && s[a] == u' ') ++a;
			while (b > a && s[b - 1] == u' ') --b;
			item_buffer_ = s.substr(a, b - a);
		}
		item_mark_ = item_mark_.substr(k == ustr::npos ? n : n + 1);
		return item_buffer_;
	}

private:
	std::vector<ustr> lines_;
	size_t pos_ = 0;
	ustr item_mark_, item_buffer_;
};

constexpr int64_t kMaxMaskCells = 1 << 20;

bool read_header(Loader& L, MaskText& txt, const Type& o, const char* what, int32_t& w, int32_t& h) {
	txt.get();
	w = convert_int(txt.item());
	h = convert_int(txt.item());
	if (h != o.y_size || w != o.x_size) {
		L.r.warnings.push_back(std::string(what) + " size " + std::to_string(w) + "x" + std::to_string(h) + " != " +
		                       std::to_string(o.x_size) + "x" + std::to_string(o.y_size) + " for " + to_utf8(o.name));
	}
	if (int64_t(w) * int64_t(h) > kMaxMaskCells || h > kMaxMaskCells) {
		L.r.warnings.push_back(std::string(what) + " too large for " + to_utf8(o.name));
		return false;
	}
	return true;
}

// BuildType::init_build_mask @006310b0: "W, H" then H rows of W integers stored as (u8).
void read_build_mask(Loader& L, MaskText& txt, Type& o) {
	int32_t w, h;
	if (!read_header(L, txt, o, "mask", w, h)) return;
	BuildMask m;
	m.present = true;
	m.w = w;
	m.h = h;
	m.data.assign(size_t(w > 0 && h > 0 ? int64_t(w) * h : 0), 0);
	for (int32_t row = 0; row < h; ++row) {
		txt.get();
		for (int32_t c = 0; c < w; ++c) m.data[size_t(row) * size_t(w) + size_t(c)] = uint8_t(convert_int(txt.item()));
	}
	o.mask = std::move(m);
}

// BuildType::init_doober_mask @00630cd0: the same grid of words (0, up, down, left, right, random). The cell value
// is dir << 16 | flag << 8 | index into the GraphicPieces doober names, which the rules loader does not have, so
// cells stay -1 and the words are kept (as in the JS reference).
void read_doober_mask(Loader& L, MaskText& txt, Type& o) {
	int32_t w, h;
	if (!read_header(L, txt, o, "doober mask", w, h)) return;
	DooberMask m;
	m.present = true;
	m.w = w;
	m.h = h;
	m.data.assign(size_t(w > 0 && h > 0 ? int64_t(w) * h : 0), -1);
	for (int32_t row = 0; row < h; ++row) {
		txt.get();
		for (int32_t c = 0; c < w; ++c) m.words.push_back(txt.item());
	}
	o.doober_mask = std::move(m);
}

}  // namespace

// Types::init after the post-loop: "<GRAPH>-Default-Age0" in building_graphics.xml gives the mask keys
// (BuildType::init_masks), then masks.txt sections. Not part of the rules image.
void load_masks(Loader& L) {
	const XDoc g = read_game_xml(L.files, "building_graphics.xml");
	const std::vector<XNode*> builds = xml::children(xml::child(g.root, u"Buildings"), u"build");
	for (int t = kBuilds; t < kEndBuilds; ++t) {
		Type& o = L.t(t);
		const ustr key = to_upper(o.graph_name + u"-Default-Age0");
		const XNode* node = nullptr;
		for (const XNode* b : builds) {
			if (iequals(xml::get_attrib(b, u"name"), key)) {
				node = b;
				break;
			}
		}
		if (!node) {
			L.r.warnings.push_back("Failed to locate basic building graphic key: " + to_utf8(o.graph_name));
			continue;
		}
		o.mask_name = xml::get_attrib(node, u"mask");
		o.doober_name = xml::get_attrib(node, u"doobermask");
	}
	const std::string path = L.files.resolve("masks.txt");
	if (path.empty()) throw std::runtime_error("missing masks.txt under " + L.files.root());
	MaskText txt(read_file(path));
	for (int t = kBuilds; t < kEndBuilds; ++t) {
		Type& o = L.t(t);
		// An empty key opens at the top of the file, as in the game.
		if (txt.open(o.mask_name)) read_build_mask(L, txt, o);
		if (txt.open(o.doober_name)) read_doober_mask(L, txt, o);
	}
}

}  // namespace ron::sim::detail
