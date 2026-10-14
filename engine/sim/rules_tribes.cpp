// Tribes, categories, names/localisation, effective prerequisites and the game-start rules changes
// (docs/rules-spec.md section 10; port of re/tools/rules/tribes.mjs).
#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>

#include "sim/rules_internal.hpp"

namespace ron::sim {

int CategoryList::find_key(const ustr& k) const {
	for (size_t i = 0; i < items.size(); ++i) {
		if (iequals(items[i].key, k)) return int(i);
	}
	return -1;
}

const CategoryList* Rules::category_list(const std::string& var) const {
	for (const CategoryList& c : categories) {
		if (c.var == var) return &c;
	}
	return nullptr;
}

namespace detail {

// ---------------------------------------------------------------- String::generate_hash
namespace {

// ?primes@@3PAKA (50 entries) from the exe's .rdata.
const uint32_t kPrimes[50] = {127,   811,   1597,  2131,  2749,  4759,  5527,  5953,  8117,  9539,  10273, 10753, 11159,
                              12301, 13217, 14207, 15413, 17681, 18661, 19013, 21089, 22051, 25111, 25801, 27457, 28057,
                              29581, 30809, 32611, 34469, 36067, 37511, 38723, 40093, 41983, 43321, 45083, 47431, 49667,
                              50767, 53453, 55469, 57193, 59369, 61987, 65071, 73421, 77849, 84223, 89009};

// Walks the UTF-16 units from last to first; n is the 1-based position. towlower folds only A-Z ("C" locale).
//   h  += primes[i % 50] * c + primes[c % 50] * n
//   hi += primes[lc % 50] * n + lc * primes[i % 50]      (uint32 wrap-around)
void generate_hash(const ustr& s, uint32_t& h, uint32_t& hi) {
	h = hi = 0;
	for (size_t k = s.size(); k-- > 0;) {
		const uint32_t c = s[k], lc = c >= 65 && c <= 90 ? c + 32 : c, n = uint32_t(k + 1), p = kPrimes[k % 50];
		h += p * c + kPrimes[c % 50] * n;
		hi += kPrimes[lc % 50] * n + lc * p;
	}
}

}  // namespace

uint32_t string_hash(const ustr& s) {
	uint32_t h, hi;
	generate_hash(s, h, hi);
	return h;
}

uint32_t string_hash_insensitive(const ustr& s) {
	uint32_t h, hi;
	generate_hash(s, h, hi);
	return hi;
}

namespace {

ustr trim_right_spaces(ustr s) {
	while (!s.empty() && s.back() == u' ') s.pop_back();
	return s;
}

// ---------------------------------------------------------------- String::set_string_lookup / String::lookup
struct LookupEntry {
	uint32_t h, hi;
	ustr text;
};
using LookupTable = std::map<uint32_t, std::vector<LookupEntry>>;

// Root children <CAT hash=H> with <STRING hash= hash_i=>text</STRING>; the first CAT with a hash wins. The hash
// attributes go through convert_int, so values above INT_MAX saturate (as in the game).
LookupTable load_string_lookup(const Loader& L) {
	LookupTable cats;
	const XDoc d = read_game_xml(L.files, "string_lookup.xml", L.opts.lang_suffix, false);
	for (XNode* cat : xml::children(d.root, u"CAT", true)) {
		const uint32_t h = uint32_t(xml::get_attrib_num(cat, u"hash", -1));
		std::vector<LookupEntry> list;
		for (XNode* s : xml::children(cat, u"STRING", true)) {
			list.push_back({uint32_t(xml::get_attrib_num(s, u"hash", -1)), uint32_t(xml::get_attrib_num(s, u"hash_i", -1)),
			                xml::text(s)});
		}
		cats.emplace(h, std::move(list));  // emplace keeps the first
	}
	return cats;
}

ustr string_lookup(const LookupTable& table, const ustr& str, const ustr& cat) {
	uint32_t h, hi;
	generate_hash(str, h, hi);
	if (h == 0) return str;
	auto it = table.find(string_hash(cat));
	if (it != table.end()) {
		for (const LookupEntry& e : it->second) {
			if (e.h == h && e.hi == hi) return e.text;
		}
	}
	return str;
}

// ---------------------------------------------------------------- Categories::init
// Game::init_rules_data order: global name, expected count, rules.xml CATEGORIES id.
struct CategorySpec {
	const char* var;
	int count;
	const char16_t* id;
};
const CategorySpec kCategoryLists[] = {
	{"tech_cats", 4, u"tech_cats"}, {"unit_cats", 9, u"unit_cats"}, {"build_cats", 3, u"building_cats"},
	{"stances_combat", 6, u"combatstances"}, {"stances_worker", 4, u"workerstances"},
	{"stances_caster", 2, u"casterstances"}, {"stances_packer", 2, u"packerstances"}, {"agename_cats", 8, u"agenameshort"},
	{"treaties_cats", 3, u"treaties"}, {"team_styles", 14, u"gamestyles"}, {"game_rules", 12, u"gamerules"},
	{"map_styles", 23, u"mapstyles"}, {"diffs", 6, u"difficulties"}, {"map_sizes", 7, u"mapsizes"},
	{"victories", 11, u"victories"}, {"eliminations", 4, u"eliminations"}, {"starting_towns", 4, u"startingtowns"},
	{"starting_resources", 13, u"startingresources"}, {"starting_resources2", 5, u"defenderresources"},
	{"tech_costs", 9, u"techcosts"}, {"starting_technologies", 10, u"startingtechs"},
	{"starting_technologies2", 5, u"defendertechs"}, {"ending_technologies", 8, u"endingtechs"},
	{"reveal_maps", 4, u"revealmaps"}, {"pop_limits", 6, u"poplimits"}, {"rush_rules", 15, u"rushrules"},
	{"cannon_times", 5, u"cannontimes"}, {"handicaps", 21, u"handicaps"}, {"player_slots", 12, u"playerslots"},
	{"number_of_players", 8, u"numberofplayers"}, {"max_observers", 7, u"maxobservers"}, {"game_speeds", 5, u"gamespeeds"},
	{"scores", 12, u"scores"}, {"time_limits", 9, u"timelimits"}, {"chairs", 8, u"chairs"},
	{"wonderwins", 13, u"wonderwins"}, {"popwins", 13, u"popwins"}, {"econwins", 9, u"econwins"},
	{"victorytypes", 7, u"victorytypes"}, {"gameinfo_flags", 12, u"gameinfo_flags"},
	{"mods", -1, u"mods"},  // GameMods overload: whatever the file has (mod entries are appended at run time)
};

CategoryList load_category_list(const XNode* rules, const CategorySpec& spec, const LookupTable& table) {
	XNode* sec = nullptr;
	for (XNode* c : xml::children(rules, u"CATEGORIES", true)) {
		if (xml::get_attrib(c, u"id") == spec.id) {
			sec = c;
			break;
		}
	}
	// Categories::init returns failure and init_rules_data aborts.
	if (!sec) throw std::runtime_error(std::string("rules.xml: no CATEGORIES id=") + spec.var);
	const std::vector<XNode*> items = xml::children(sec, u"category");  // upper-cased: CATEGORY
	CategoryList list;
	list.var = spec.var;
	list.id = spec.id;
	// A count differing from the file is reported and the expected count is used (missing items stay empty).
	const size_t n = spec.count < 0 ? items.size() : size_t(spec.count);
	list.mismatch = spec.count >= 0 && size_t(spec.count) != items.size();
	for (size_t i = 0; i < n; ++i) {
		const XNode* e = i < items.size() ? items[i] : nullptr;
		Category c;
		c.name = string_lookup(table, xml::get_attrib(e, u"name"), u"CATNAME");
		c.key = xml::get_attrib(e, u"key");
		if (c.key.empty()) c.key = c.name;  // the key defaults to the (looked-up) display name
		c.desc = string_lookup(table, xml::get_attrib(e, u"desc"), u"CATDESC");
		const std::vector<XNode*> data = xml::children(e, u"data");
		for (size_t k = 0; k < data.size() && k < 6; ++k) c.data[k] = convert_int(xml::text(data[k]));
		c.alpha_order = int32_t(i);
		list.items.push_back(std::move(c));
	}
	// alpha_order: exchange sort of indices by _wcsicmp(name) > 0 (empty names sort first).
	std::vector<size_t> idx(n);
	for (size_t i = 0; i < n; ++i) idx[i] = i;
	for (size_t i = 0; i < n; ++i) {
		for (size_t j = i + 1; j < n; ++j) {
			const ustr& a = list.items[idx[i]].name;
			const ustr& b = list.items[idx[j]].name;
			const bool swap = a.empty() || b.empty() ? !a.empty() : wcsicmp(a, b) > 0;
			if (swap) std::swap(idx[i], idx[j]);
		}
	}
	for (size_t k = 0; k < n; ++k) list.items[idx[k]].alpha_order = int32_t(k);
	list.title = string_lookup(table, xml::get_attrib(sec, u"title"), u"CATTITLES");
	list.list_name = xml::get_attrib(sec, u"name");
	if (list.list_name.empty()) list.list_name = spec.id;
	return list;
}

}  // namespace

void load_categories(Loader& L) {
	const XDoc rules = read_game_xml(L.files, "rules.xml", L.opts.lang_suffix);
	const LookupTable table = load_string_lookup(L);  // Main::main set_string_lookup(EMPTY) -> string_lookup.xml
	for (const CategorySpec& spec : kCategoryLists) {
		L.r.categories.push_back(load_category_list(rules.root, spec, table));
		if (L.r.categories.back().mismatch) L.r.warnings.push_back(std::string("Category data mismatch: ") + spec.var);
	}
}

// ---------------------------------------------------------------- Tribes::init (no mod, first load)
void load_tribes(Loader& L) {
	const XDoc rules = read_game_xml(L.files, "rules.xml", L.opts.lang_suffix);
	const std::vector<XNode*> els = xml::children(xml::child(rules.root, u"TRIBES", true), u"TRIBE", true);
	if (els.size() != size_t(kNumTribes)) throw std::runtime_error("Number of tribes in rules.xml does not equal NUM_TRIBES");
	L.r.tribes.assign(kNumTribes, Tribe());
	for (int i = 0; i < kNumTribes; ++i) {
		Tribe& t = L.r.tribes[size_t(i)];
		t.eng_name = trim_right_spaces(xml::get_text(els[size_t(i)], u"KEY"));
		t.file = trim_right_spaces(u"TRIBES\\" + xml::get_text(els[size_t(i)], u"FILE"));
		const XDoc f = read_game_xml(L.files, to_utf8(t.file), L.opts.lang_suffix, false);
		if (!f.root) throw std::runtime_error("Can't Open Nation File " + to_utf8(t.file));
		t.name = trim_right_spaces(xml::get_attrib(f.root, u"name"));
		t.unit_continent = xml::get_text_num(f.root, u"UNIT_CONTINENT", -1);
		t.build_continent = xml::get_text_num(f.root, u"BUILD_CONTINENT", -1);
		t.backup_build_continent = xml::get_text_num(f.root, u"BACKUP_BUILD_CONTINENT", -1);
		t.text_substitute = xml::get_attrib_num(f.root, u"substitute", 0);
		t.barbarian = 0;
		t.tribe = i;
		t.people = i / 3;
		for (int k = 0; k < kNumGraft; ++k) t.graft[size_t(k)] = kUnits + k;
	}
}

// ---------------------------------------------------------------- display names
void load_display_names(Loader& L) {
	const XDoc d = read_game_xml(L.files, "typenames.xml", L.opts.lang_suffix, false);
	if (!d.root) return;
	for (const auto& sec : d.root->children) {
		if (sec->is_text) continue;
		std::map<ustr, ustr> m;
		for (XNode* s : xml::children(sec.get(), u"STRING", true)) m.emplace(xml::get_attrib(s, u"hash"), xml::text(s));
		L.display_names.emplace(sec->tag, std::move(m));
	}
}

namespace {

// typenames.xml section of a TypeIndex, in the order the virtual is_* tests run.
const char16_t* type_names_section(int t) {
	if (t >= 414 && t <= 542) return u"BUILDS";
	if (t >= 50 && t <= 413) return u"UNITS";
	if (t >= 629 && t <= 683) return u"SPELLS";
	if (t == 543) return u"ITEMS";
	if (t < 50) return u"GOODS";
	if (t >= 544 && t <= 550) return u"AGES";
	if (t >= 551 && t <= 578) return u"TECHS";
	if (t >= 684 && t <= 805) return u"BONUSES";
	return u"MISC";  // 579..628
}

}  // namespace

ustr Loader::lookup_display_name(int type, const ustr& name) const {
	auto sec = display_names.find(type_names_section(type));
	if (sec == display_names.end()) return name;
	auto it = sec->second.find(from_ascii(std::to_string(string_hash(name)).c_str()));
	return it == sec->second.end() || it->second.empty() ? name : it->second;
}

void Loader::set_name(Type& o, const ustr& n) {
	o.name = n;
	o.type_name = n;
	o.display_name = lookup_display_name(o.type, n);
}

// ---------------------------------------------------------------- TypeData::get_preq and the slow caches
namespace {

inline bool is_tech(int t) { return t >= kTechs && t < kEndTechs; }
inline bool is_age(int t) { return t >= kAges && t < kEndAges; }
inline bool is_epoch(int t) { return t >= kEpochs && t < kEndEpochs; }

}  // namespace

int get_preq(const Rules& r, const Type& o, int i, const GameSetup& g, int unit_hi) {
	const std::vector<Type>& T = r.types;
	if (i < 0 || i > 2) return TYPE_NONE;
	if (i == 0) return o.preq[0];
	if (i == 2) {
		const int p = o.preq[2];
		if (p >= 0) {
			if (is_epoch(p)) {
				const int q = get_preq(r, o, 1, g, unit_hi);
				if (q >= 0 && is_epoch(q) && T[size_t(p)].cat == T[size_t(q)].cat && p <= q) return TYPE_NONE;
			}
			if (is_age(p) && o.where == 420 /* UNIVERSITY */ && g.ending_technology < T[size_t(p)].age + 1) return TYPE_NONE;
		}
		return p;
	}
	// Slot 1. (The iroquois early-government rule needs a player and never applies here.)
	const int p = o.preq[1];
	const int s = g.game_rules == 8 ? std::min(g.starting_technology, g.starting_technology2) : g.starting_technology;
	if (p >= 0 && (s != 0 || g.ending_technology < 7)) {
		const bool unit = o.type >= kUnits && o.type < unit_hi;
		if (is_epoch(p) && unit && T[size_t(p)].cat == 0) {
			// Military epochs are rescaled to the game's age span.
			const int32_t age = T[size_t(p)].age;
			if (age < s) return TYPE_NONE;
			const int32_t span = g.ending_technology - s + 1;
			// span 0 would divide by zero in the original; the JS reference's integer path yields 0 there.
			const int32_t v = span == 0 ? 0 : iadd(imul(28 / span, age - s + 1), 3);
			const int32_t q = cdiv(v, 4) + 571;
			return q > 571 ? (q < 579 ? q : 578) : 572;
		}
		if (s != 0 && is_epoch(p)) {
			const Type& tp = T[size_t(p)];
			const int32_t age = tp.age;
			if (s <= age && 7 - s > 0) {
				const int32_t base = tp.cat == 1 ? 565 : tp.cat == 2 ? 558 : tp.cat == 3 ? 551 : 572;
				int32_t q = cdiv((age - s + 1) * 7, 7 - s) - 1 + base;
				if (o.preq[1] < q) q = o.preq[1];
				const int p2 = o.preq[2];
				if (p2 < 0 || !is_epoch(p2) || T.at(size_t(q)).cat != T[size_t(p2)].cat || p2 <= q) return q;
			}
			return TYPE_NONE;
		}
	}
	return p;
}

int get_age_slow(const Rules& r, const Type& o, const GameSetup& g) {
	const int n = o.type >= kBuilds ? 3 : 2;  // num_preq: buildings 3, units 2
	for (int i = 0; i < n; ++i) {
		const int p = get_preq(r, o, i, g, kEndGaia);
		if (p >= 0 && is_tech(p)) return is_age(p) ? r.types[size_t(p)].age + 1 : r.types[size_t(p)].age;
	}
	return 0;
}

int get_military_level_slow(const Rules& r, const Type& o, const GameSetup& g) {
	auto mil = [&](int p) { return p >= 0 && is_epoch(p) && r.types[size_t(p)].cat == 0; };
	int p = o.preq[0];
	if (!mil(p)) p = get_preq(r, o, 1, g, kEndGaia);
	return mil(p) ? p - 571 /* INTERNATIONAL_LAW */ : 0;
}

// ---------------------------------------------------------------- game start
// Types::init_civ_masks / BuildType::init_civ_masks: its building_graphics probe tests an XMLElement that is never
// assigned, so every mask ends up 0.
void init_civ_masks(Rules& r) {
	for (int t = kBuilds; t < kEndBuilds; ++t) r.types[size_t(t)].civ_graph_mask = 0;
}

// Types::finalize_grafting @00669840.
void finalize_grafting(Rules& r, const GameSetup& g) {
	int32_t barb = 0;
	for (int t = 0; t < kNumTribes; ++t) {
		if (r.tribes[size_t(t)].barbarian) barb |= int32_t(1u << t);
	}
	for (int u = kUnits; u < kEndUnits; ++u) {
		const Type& o = r.types[size_t(u)];
		const int gr = o.graft;
		if (gr != u && gr >= 0 && (o.unit_flags & 0x1000000)) {
			Type& tg = r.types[size_t(gr)];
			if (!g.flag8) tg.tribe_mask = (tg.tribe_mask & ~o.tribe_mask) | barb;
			else tg.tribe_mask |= o.tribe_mask;
		}
	}
	for (int u = kUnits; u < kEndUnits; ++u) {
		Type& o = r.types[size_t(u)];
		o.age = get_age_slow(r, o, g);
		o.military_level = get_military_level_slow(r, o, g);
	}
	for (int b = kBuilds; b < kEndBuilds; ++b) r.types[size_t(b)].age = get_age_slow(r, r.types[size_t(b)], g);
}

}  // namespace detail
}  // namespace ron::sim
