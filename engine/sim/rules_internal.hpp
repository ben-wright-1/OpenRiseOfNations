// Loader internals shared by the sim/rules_*.cpp files (not part of the public API).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "sim/rules.hpp"

namespace ron::sim::detail {

// What Types::init and friends see while loading: the tables being filled plus the lookups built before them.
struct Loader {
	const GameFiles& files;
	LoadOptions opts;
	Rules& r;
	// typenames.xml: section tag -> (hash attribute text -> display name), first match wins.
	std::map<ustr, std::map<ustr, ustr>> display_names;

	Loader(const GameFiles& f, const LoadOptions& o, Rules& rules) : files(f), opts(o), r(rules) {}

	Type& t(int i) { return r.types.at(size_t(i)); }
	// Allocates type i with its class's constructor defaults (Types::init creates each type before its first pass).
	Type& alloc(int i);

	// Types::unit_key / build_key / tech_key / good_key: first case-insensitive type_name match over the class
	// range (gaia excluded for units), then "none" -> TYPE_NONE, "disable" -> TYPE_ANY_BUILDING, else TYPE_ANY_UNIT.
	int key(int lo, int hi, const ustr& s) const;
	int unit_key(const ustr& s) const { return key(kUnits, kEndUnits, s); }
	int build_key(const ustr& s) const { return key(kBuilds, kEndBuilds, s); }
	int tech_key(const ustr& s) const { return key(kTechs, kEndTechs, s); }
	int good_key(const ustr& s) const { return key(kGoods, kEndGoods, s); }
	int object_key(const ustr& s) const;  // Types::object_key: unit_key, else build_key
	int cat_key(const char* list, const ustr& s) const;

	// Type::lookup_display_name: typenames.xml <SECTION>/STRING[@hash = hash(name)], else the name itself.
	ustr lookup_display_name(int type, const ustr& name) const;
	// Type::set_name.
	void set_name(Type& o, const ustr& name);
};

// ---- predicates (types.mjs / tribes.mjs)
// ObjectTypeData::is_slow and ObjectTypeData::is (TypeData::is for non-object types is plain equality).
bool is_slow(const Rules& r, const Type& o, int t, bool strict);
bool is(const Rules& r, const Type& o, int t, bool strict = false);
// TypeData::get_preq with no player. unit_hi is the end of the "is unit" range the caller's copy of the function uses
// (the tech AI pass treats 50..401 as units, finalize_grafting and the balance ages 50..413).
int get_preq(const Rules& r, const Type& o, int i, const GameSetup& g, int unit_hi);
int get_age_slow(const Rules& r, const Type& o, const GameSetup& g);           // ObjectTypeData::get_age_slow
int get_military_level_slow(const Rules& r, const Type& o, const GameSetup& g);  // UnitTypeData::get_military_level_slow

// Type::load_cost: text of el (or its cost= attribute); when non-empty, dst is zeroed and each token stored.
void load_cost(const XNode* el, std::array<int32_t, 6>& dst);

// ---- tribes, categories, names (rules_tribes.cpp)
uint32_t string_hash(const ustr& s);              // String::generate_hash, case-sensitive part
uint32_t string_hash_insensitive(const ustr& s);  // the folded part
void load_categories(Loader& L);
void load_tribes(Loader& L);
void load_display_names(Loader& L);
void init_civ_masks(Rules& r);
void finalize_grafting(Rules& r, const GameSetup& g);

// ---- constants (rules_constants.cpp)
void load_constants(Loader& L);

// ---- units and finalisation (rules_units.cpp)
void init_unit(Loader& L, XNode* el, int t, int pass);
void fill_relative_values(Rules& r, Type& o);
void finalize_init_all(Rules& r);

// ---- buildings (rules_buildings.cpp)
void init_build(Loader& L, XNode* el, int t, int pass);
void post_buildings(Rules& r);
void load_masks(Loader& L);

// ---- techs, goods, item, crafts, bonuses (rules_techs.cpp)
void init_tech(Loader& L, XNode* el, int t, int pass);
void load_alternate_names(Loader& L);
void init_good(Loader& L, XNode* el, int t, int pass);
void init_item(Loader& L, XNode* el, int t, int pass);
void init_spell(Loader& L, XNode* el, int t);
void post_spells(Rules& r);
void init_bonus(Loader& L, XNode* el, int t);
void set_research(Loader& L, XNode* el, int t);
void compute_ai_values(Loader& L, int t);
void compute_largest_gather(Loader& L);

// ---- balance (rules_balance.cpp)
// `ages`: each type's age as Balance::init reads it (get_age_slow while age is still -1).
void compute_balance(Loader& L, const std::vector<int32_t>& ages);

}  // namespace ron::sim::detail
