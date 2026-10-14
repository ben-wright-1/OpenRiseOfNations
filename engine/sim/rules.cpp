// Rules loading order (docs/rules-spec.md section 2; port of re/tools/rulesdump.mjs loadRules / startGame).
#include "sim/rules.hpp"

#include <stdexcept>
#include <string>

#include "sim/rules_internal.hpp"

namespace ron::sim {

namespace {

// The element-count asserts of Types::init (type numbering is compile-time: the n-th element is type BASE + n).
std::vector<XNode*> elements(const XDoc& d, const char16_t* wrap, const char16_t* tag, size_t expect, const char* what) {
	const XNode* parent = wrap ? xml::child(d.root, wrap) : d.root;
	std::vector<XNode*> list = xml::children(parent, tag);
	if (list.size() != expect) {
		throw std::runtime_error(std::string(what) + ": " + std::to_string(list.size()) + " elements, expected " +
		                         std::to_string(expect));
	}
	return list;
}

}  // namespace

Rules load_rules(const GameFiles& files, const LoadOptions& opts) {
	Rules r;
	r.types.assign(kNumTypes, Type());
	detail::Loader L(files, opts, r);

	// ---- Game::init_rules_data
	detail::load_constants(L);
	detail::load_categories(L);
	detail::load_tribes(L);
	detail::load_display_names(L);

	// ---- Types::init. Within a pass, types go in index order; passes are file-major.
	const XDoc techrules = read_game_xml(files, "techrules.xml");
	const std::vector<XNode*> techs = elements(techrules, nullptr, u"TECH", 85, "techrules.xml TECH");
	for (int pass = 0; pass < 2; ++pass) {
		for (size_t i = 0; i < techs.size(); ++i) detail::init_tech(L, techs[i], kTechs + int(i), pass);
	}
	detail::load_alternate_names(L);

	const XDoc resources = read_game_xml(files, "resourcerules.xml");
	const std::vector<XNode*> goods = elements(resources, u"RESOURCES", u"RESOURCE", 50, "resourcerules.xml RESOURCE");
	for (int pass = 0; pass < 2; ++pass) {
		for (size_t i = 0; i < goods.size(); ++i) detail::init_good(L, goods[i], kGoods + int(i), pass);
	}

	const XDoc buildingrules = read_game_xml(files, "buildingrules.xml");
	const std::vector<XNode*> builds = elements(buildingrules, nullptr, u"BUILDING", 129, "buildingrules.xml BUILDING");
	for (int pass = 0; pass < 2; ++pass) {
		for (size_t i = 0; i < builds.size(); ++i) {
			if (pass == 0) L.alloc(kBuilds + int(i));
			detail::init_build(L, builds[i], kBuilds + int(i), pass);
		}
	}
	detail::post_buildings(r);
	if (opts.masks) detail::load_masks(L);

	const XDoc unitrules = read_game_xml(files, "unitrules.xml");
	const std::vector<XNode*> units = elements(unitrules, nullptr, u"UNIT", 364, "unitrules.xml UNIT");
	for (int pass = 0; pass < 5; ++pass) {
		int last = 0;
		XNode* last_el = nullptr;
		for (size_t i = 0; i < units.size(); ++i) {
			const int t = kUnits + int(i);
			if (pass == 0) L.alloc(t);
			// From pass 2 on, a unit named like the previous distinct unit is initialised from that unit's element.
			if (pass < 2 || last == 0 || !iequals(r.types[size_t(t)].name, r.types[size_t(last)].name)) {
				last = t;
				last_el = units[i];
			}
			detail::init_unit(L, pass < 2 ? units[i] : last_el, t, pass);
		}
	}
	for (int t = kUnits; t < kEndUnits; ++t) detail::fill_relative_values(r, r.types[size_t(t)]);

	const XDoc itemrules = read_game_xml(files, "itemrules.xml");
	const std::vector<XNode*> items = elements(itemrules, u"ITEMS", u"ITEM", 1, "itemrules.xml ITEM");
	for (int pass = 0; pass < 2; ++pass) detail::init_item(L, items[0], kItem, pass);

	const XDoc craftrules = read_game_xml(files, "craftrules.xml");
	const std::vector<XNode*> crafts = elements(craftrules, nullptr, u"CRAFT", 55, "craftrules.xml CRAFT");
	for (size_t i = 0; i < crafts.size(); ++i) detail::init_spell(L, crafts[i], kSpells + int(i));
	detail::post_spells(r);

	const XDoc rulesxml = read_game_xml(files, "rules.xml");
	const std::vector<XNode*> bonuses = elements(rulesxml, u"TECHBONUSES", u"BONUS", 122, "rules.xml TECHBONUSES/BONUS");
	for (size_t i = 0; i < bonuses.size(); ++i) detail::init_bonus(L, bonuses[i], kBonuses + int(i));

	for (size_t i = 0; i < techs.size(); ++i) detail::set_research(L, techs[i], kTechs + int(i));
	for (int t = kTechs; t < kEndTechs; ++t) detail::compute_ai_values(L, t);
	detail::finalize_init_all(r);

	// Balance::init, the last step of Types::init. It reads ages through ObjectTypeData::get_age, which falls back to
	// get_age_slow while `age` is still -1, i.e. under the GameInfo in effect at load time.
	std::vector<int32_t> ages(kNumTypes);
	for (int t = 0; t < kNumTypes; ++t) {
		const Type& o = r.types[size_t(t)];
		const bool slow = o.age == -1 && (o.kind == Kind::Unit || o.kind == Kind::Build) && !(t >= kGaia && t < kEndGaia);
		ages[size_t(t)] = slow ? detail::get_age_slow(r, o, opts.load_game) : o.age;
	}
	detail::compute_balance(L, ages);

	// ---- Game::init_non_rules_data after Types::init: Lands::init.
	detail::compute_largest_gather(L);
	return r;
}

void start_game(Rules& rules, const GameSetup& game) {
	detail::init_civ_masks(rules);
	detail::finalize_grafting(rules, game);
}

}  // namespace ron::sim
