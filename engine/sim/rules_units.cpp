// Type core: constructor defaults, name keys, is/is_slow, units (UnitType::init @0061ab50), relative combat values
// and ObjectType::finalize_init_all (docs/rules-spec.md sections 6 and 7; port of re/tools/rules/types.mjs).
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "sim/rules_internal.hpp"

namespace ron::sim {

Kind kind_of(int t) {
	if (t < kEndGoods) return Kind::Good;
	if (t < kEndGaia) return Kind::Unit;
	if (t < kEndBuilds) return Kind::Build;
	if (t < kTechs) return Kind::Item;
	if (t < kEndTechs) return Kind::Tech;
	if (t < kEndSpells) return Kind::Spell;
	return Kind::Bonus;
}

// Type::Type, ObjectType::ObjectType, UnitType::UnitType, BuildType::BuildType, GoodType::GoodType,
// SpellTypeData::SpellTypeData, TechType::TechType. The struct's member defaults are the Type/ObjectType ones;
// fields a constructor leaves uninitialised are 0 (spec section 15.7).
Type make_type(Kind kind) {
	Type o;
	o.kind = kind;
	if (kind == Kind::Unit) o.relative_value.assign(kNumGraft, 0);
	if (kind == Kind::Tech) o.age = 0;
	return o;
}

namespace detail {

Type& Loader::alloc(int i) {
	Type& o = t(i);
	if (!o.loaded) {
		o = make_type(kind_of(i));
		o.loaded = true;
	}
	return o;
}

int Loader::key(int lo, int hi, const ustr& s) const {
	for (int i = lo; i < hi; ++i) {
		const Type& o = r.types[size_t(i)];
		if (o.loaded && iequals(s, o.type_name)) return i;
	}
	if (iequals(s, u"none")) return TYPE_NONE;
	if (iequals(s, u"disable")) return TYPE_ANY_BUILDING;
	return TYPE_ANY_UNIT;
}

int Loader::object_key(const ustr& s) const {
	const int u = unit_key(s);
	return u != TYPE_ANY_UNIT ? u : build_key(s);
}

int Loader::cat_key(const char* list, const ustr& s) const {
	const CategoryList* c = r.category_list(list);
	return c ? c->find_key(s) : -1;
}

// ---------------------------------------------------------------- predicates

// ObjectTypeData::is_slow @00661ae0. Non-strict: this type, its graft, or (recursively) anything its FROM type is.
// Strict: this type, or its graft when this is a unit and the graft target lacks unit flag 'y'.
bool is_slow(const Rules& r, const Type& o, int t, bool strict) {
	if (o.type == t) return true;
	if (!strict) {
		if (t < 0) return false;
		if (o.graft == t) return true;
		if (o.from >= 0) return is_slow(r, r.types.at(size_t(o.from)), t, false);
		return false;
	}
	if (o.kind != Kind::Unit) return false;
	return o.graft == t && !(r.types.at(size_t(t)).unit_flags & 0x1000000);
}

// ObjectTypeData::is @0065f7d0: the is-lists once built, else is_slow. TypeData::is (techs, crafts, bonuses) is
// plain equality.
bool is(const Rules& r, const Type& o, int t, bool strict) {
	if (!is_object_kind(o.kind)) return o.type == t;
	if (o.type == t) return true;
	const U16Array& list = strict ? o.is_strict_list : o.is_list;
	if (!list.count) return is_slow(r, o, t, strict);
	// (A negative t is never in a list; the original only short-cuts it for the non-strict list.)
	if (t < 0 || t > 0xffff) return false;
	return std::find(list.v.begin(), list.v.end(), uint16_t(t)) != list.v.end();
}

void load_cost(const XNode* el, std::array<int32_t, 6>& dst) {
	ustr s = xml::text(el);
	if (s.empty()) s = xml::get_attrib(el, u"cost");
	if (s.empty()) return;
	dst.fill(0);
	for (const CostToken& k : cost_tokens(s)) dst[size_t(k.res)] = k.amount;
}

// ---------------------------------------------------------------- units

namespace {

inline bool is_age_type(int t) { return t >= kAges && t < kEndAges; }
inline bool has_obj_mask(const Type& o, uint32_t m) { return (uint32_t(o.obj_masks) & m) != 0; }

void unit_pass0(Loader& L, Type& o, const XNode* el) {
	o.name = xml::get_text(el, u"NAME");
	o.type_name = o.name;
	o.display_name = L.lookup_display_name(o.type, o.name);
	o.graph_name = xml::get_text(el, u"GRAPH");
	const ustr tn = xml::get_text(el, u"TYPENAME");
	o.type_name = tn.empty() ? o.name : tn;
}

void unit_pass1(Loader& L, Type& o, const XNode* el) {
	o.where = L.build_key(xml::get_text(el, u"WHERE"));
	o.from = L.unit_key(xml::get_text(el, u"FROM"));
	o.obs = -2;
	o.jump = L.unit_key(xml::get_text(el, u"JUMP"));
	o.tribe_mask = parse_binary(xml::get_text(el, u"TRIBE_MASK"));
	o.graft = L.unit_key(xml::get_text(el, u"GRAFT"));
}

// FROM's `upgrade` points at this unit unless this unit has OBJ_MASK '1' (can't upgrade to it), or it is a unique
// unit grafted onto a mainline unit whose FROM is mainline (unless flag 'h'). Last writer wins.
void link_upgrade(Loader& L, Type& o, int t) {
	if (o.from < 0) return;
	Type& f = L.t(o.from);
	if (!has_obj_mask(o, 0x4000000) && (o.graft < 0 || f.graft >= 0 || (o.unit_flags & 0x80))) f.upgrade = t;
}

// ObjectType::load_support / load_upgrade: only when the element has a COST child (always true here, because
// pass 2 has just created one with get_element(create = 1)).
void load_support(Type& o, const XNode* el) {
	if (!xml::child(el, u"COST")) return;
	ustr s = xml::get_text(el, u"SUPPORT");
	if (s.empty()) s = xml::get_attrib(xml::child(el, u"COST"), u"support");
	o.support = {{-1, -1}};
	o.support_cost = {{0, 0}};
	const std::vector<CostToken> toks = cost_tokens(s);
	for (size_t i = 0; i < toks.size() && i < 2; ++i) {
		o.support[i] = toks[i].res;
		o.support_cost[i] = toks[i].amount;
	}
}

void load_upgrade(Type& o, const XNode* el) {
	if (!xml::child(el, u"COST")) return;
	ustr s = xml::get_text(el, u"UPGRADE");
	if (s.empty()) s = xml::get_attrib(xml::child(el, u"COST"), u"upgrade");
	o.special_upgrade = -1;
	o.special_upgrade_cost = 0;
	const std::vector<CostToken> toks = cost_tokens(s);
	if (!toks.empty()) {
		o.special_upgrade = toks[0].res;
		o.special_upgrade_cost = toks[0].amount;
	}
}

// UnitType::determine_roles @0061c320 (RoleIndex bits, spec section 7.2).
void determine_roles(const Rules& r, Type& o) {
	int32_t role = 0;
	const int t = o.type;
	if (t >= 50 && t <= 53) role = 0x200;
	if (o.domain == 2) {
		role |= 0x1000;
	} else if (o.domain == 0) {
		role |= 0x40000;
		if (is(r, o, 69 /* SCOUT */)) role |= 0x10;
		if (o.cat == 1) role |= 0xc;
	} else {
		role |= 0x80000;
		if (is(r, o, 323 /* BARK */)) role |= 0x10;
	}
	if (o.carry != 0) role |= 0x8000;
	if (o.attack != 0 && o.cat != 5 && o.cat != 4 && !(role & 0x200)) {
		const int32_t before = role;
		role = before | 0x10000;
		if (o.domain == 1) role = before | 0x12000;
		if (o.domain == 2) role |= 0x4000;
		if (o.domain == 0 && o.cat == 0) {
			role |= 0x800;
			role |= is(r, o, 132 /* HOPLITES */) ? 2 : 0x100000;
		}
		role |= o.max_range != 0 ? 0x400 : 1;
	} else {
		role |= 0x100;
	}
	o.role = role;
}

int32_t clamp_1_100(int32_t v) { return std::max(1, std::min(100, v)); }

void unit_pass2(Loader& L, Type& o, XNode* el, int t) {
	const Constants& C = L.r.constants;
	const int32_t block_radius_unit = C.at(Constants::kUnitBlockRadius);
	o.help_name = xml::get_text(el, u"GRAPH");
	o.grid_x = i8(xml::get_text_num(el, u"GRID_X"));
	o.grid_y = i8(xml::get_text_num(el, u"GRID_Y"));
	o.unit_flags = lower_letter_flags(xml::get_text(el, u"FLAGS"));
	o.unit_flags2 = 0;
	o.to_hit = xml::get_text_num(el, u"TO_HIT");
	const int32_t att = xml::get_text_num(el, u"ATTENUATE");
	o.attenuate = att < 0 ? wrap32(-int64_t(att)) : att;
	o.attack = imul(xml::get_text_num(el, u"ATTACK"), 10);
	o.mode = 0;
	o.cat = L.cat_key("unit_cats", xml::get_text(el, u"CAT"));
	// RANGE "min-max": min = convert_int(whole text), max = convert_int(after the first '-'), or min.
	const ustr rng = xml::get_text(el, u"RANGE");
	o.min_range = convert_int(rng);
	const size_t dash = rng.find(u'-');
	const int32_t max = dash == ustr::npos ? o.min_range : convert_int(rng, dash + 1);
	o.max_range = max;
	o.second_max_range = max;
	if (o.unit_flags & 0x400) o.max_range = 0;
	o.splash_area = xml::get_text_num(el, u"SPLASH");
	o.splash_percent = xml::get_text_num(el, u"SPLASH_PERCENT");
	o.ammo_per_att = xml::get_text_num(el, u"AMMO_PER_ATT");
	o.recharge = xml::get_text_num(el, u"RECHARGE");
	o.armor = xml::get_text_num(el, u"ARMOR");
	o.obj_masks = obj_mask(xml::get_text(el, u"OBJ_MASK"));
	const ustr dom = xml::get_text(el, u"DOMAIN");
	o.domain = iequals(dom, u"Sea") ? 1 : iequals(dom, u"Air") ? 2 : 0;
	o.moves = xml::get_text_num(el, u"MOVES");
	o.turn_speed = degrees_to_angle(xml::get_text_num(el, u"TURN_SPEED"));
	o.hits = xml::get_text_num(el, u"HITS");
	o.proj_speed = xml::get_text_num(el, u"PROJ_SPEED");
	if (o.attack == 0) {
		o.fire_proj = (o.unit_flags & 0x200000) ? 1 : 0;
	} else {
		if (o.max_range != 0 && o.proj_speed == 0) o.proj_speed = 200;
		o.fire_proj = 1;
	}
	o.carry = xml::get_text_num(el, u"CARRY");
	o.carry_size = xml::get_text_num(el, u"CARRY_SIZE");
	o.control_cost = xml::get_text_num(el, u"POP");
	o.progression = xml::get_text_num(el, u"PROGRESSION");
	o.job_time = o.res_time = uint32_t(xml::get_text_num(el, u"JOB_TIME"));
	const ustr jet = xml::get_text(el, u"JOB_EXTRA_TIME");
	if (jet.empty()) {
		o.job_extra_time = 0;
	} else {
		// Percent fraction: a * 100 / d.
		const int32_t a = wtoi(jet);
		const size_t k = jet.find(u'/');
		const int32_t d = k == ustr::npos ? 1 : wtoi(jet, k + 1);
		o.job_extra_time = uint32_t(d == 0 ? 0 : cdiv(imul(a, 100), d));
	}
	o.research_premium_time = fraction(xml::get_text(el, u"RESEARCH_PREMIUM_TIME"), 256);
	o.research_premium_cost = fraction(xml::get_text(el, u"RESEARCH_PREMIUM_COST"), 256);
	o.mana = xml::get_text_num(el, u"MANA");
	o.los = xml::get_text_num(el, u"LOS");
	o.science_los = xml::get_text_num(el, u"SCIENCE_LOS");
	o.fly_high = xml::get_text_num(el, u"FLY_HIGH");
	o.fly_low = xml::get_text_num(el, u"FLY_LOW");
	o.guy_spacing = imul(C.at(Constants::kUnitGuySpacing), xml::get_text_num(el, u"GUY_SPACING"));
	o.x_spacing = imul(C.at(Constants::kUnitFormationSpacing), xml::get_text_num(el, u"X_SPACING"));
	o.y_spacing = imul(C.at(Constants::kUnitFormationSpacing), xml::get_text_num(el, u"Y_SPACING"));
	o.x_size = o.y_size = std::min(xml::get_text_num(el, u"CIRCLE_RADIUS"), 10);
	const int32_t br = xml::get_text_num(el, u"BLOCK_RADIUS");
	o.push_circles = clamp_1_100(xml::get_text_num(el, u"PUSH_CIRCLES"));
	int32_t ps = clamp_1_100(xml::get_text_num(el, u"PUSH_SIZE"));
	if (o.push_circles == 1) ps = br;
	o.push_size = imul(block_radius_unit, ps);
	o.target_size = imul(xml::get_text_num(el, u"TARGET_SIZE"), block_radius_unit);
	o.preq[0] = L.tech_key(xml::get_text(el, u"PREQ0"));
	o.preq[1] = L.tech_key(xml::get_text(el, u"PREQ1"));
	load_cost(xml::child_or_create(el, u"COST"), o.costs);
	load_support(o, el);
	load_upgrade(o, el);
	o.squad_size = 1;
	o.uber_size = xml::get_text_num(el, u"UBER_SIZE");
	o.crew_size = xml::get_text_num(el, u"CREW_SIZE");
	// An attacking unit gated by an age alone also needs that age's military epoch.
	if (o.preq[0] >= 0 && o.preq[1] == -1 && is_age_type(o.preq[0]) && o.attack != 0) o.preq[1] = o.preq[0] + 28;
	determine_roles(L.r, o);
	o.new_block_radius = br;
	o.guy_radius = o.block_radius = imul(block_radius_unit, br);
	o.big_radius = iadd(cdiv(imul(o.squad_size - 1, o.guy_spacing), 2), o.guy_radius);
	o.new_big_radius = cdiv(o.big_radius, block_radius_unit);
	o.block_points = br > 1 ? 12 : 8;
	link_upgrade(L, o, t);
	if (o.graft >= 0) {
		const Type& g = L.t(o.graft);
		o.grid_x = g.grid_x;
		o.grid_y = g.grid_y;
		if (o.upgrade < 0) o.upgrade = g.upgrade;
		// Every tribe in this unit's mask gets it in place of the graft target (document order: last writer wins).
		for (int tb = 0; tb < kNumTribes; ++tb) {
			if (uint32_t(o.tribe_mask) & (1u << tb)) L.r.tribes[size_t(tb)].graft[size_t(o.graft - kUnits)] = t;
		}
	}
	if (o.where >= 0 && !(o.unit_flags & 0x8000)) {
		Type& w = L.t(o.where);
		w.build_flags |= 0x80000000u;
		if (o.attack != 0 && !(o.role & 0x100) && !is(L.r, w, 414 /* SMALL_CITY */)) w.build_flags |= 0x40000000u;
	}
	o.base_form = 0;
}

// Pass 3: for each tribe where this unit is neither native nor its own replacement, every other non-native unit
// grafting onto it is redirected to the tribe's replacement. Gaia (402..413) would index past the 352-entry graft
// table; no unit grafts onto gaia, so the reference skips them and so do we.
void unit_pass3(Loader& L, const Type& o, int t) {
	if (t >= kEndUnits) return;
	for (int tb = 0; tb < kNumTribes; ++tb) {
		Tribe& tr = L.r.tribes[size_t(tb)];
		const uint32_t bit = 1u << tb;
		if (tr.graft[size_t(t - kUnits)] == t || (uint32_t(o.tribe_mask) & bit)) continue;
		for (int j = kUnits; j < kEndUnits; ++j) {
			const Type& u = L.r.types[size_t(j)];
			if (j != t && !(uint32_t(u.tribe_mask) & bit) && u.graft == t) tr.graft[size_t(j - kUnits)] = tr.graft[size_t(t - kUnits)];
		}
	}
}

}  // namespace

void init_unit(Loader& L, XNode* el, int t, int pass) {
	Type& o = L.t(t);
	o.military_level = -1;  // set on every pass, before the pass switch
	o.type = t;
	switch (pass) {
	case 0: unit_pass0(L, o, el); break;
	case 1: unit_pass1(L, o, el); break;
	case 2: unit_pass2(L, o, el, t); break;
	case 3: unit_pass3(L, o, t); break;
	case 4: link_upgrade(L, o, t); break;
	default: break;
	}
}

// UnitType::fill_relative_values @0061be50: relative_value[j - 50] for every combat unit j, 8.8 fixed point - 1.0.
void fill_relative_values(Rules& r, Type& o) {
	if (!(o.role & 0x10000)) return;
	const int32_t A = o.obj_masks;
	auto h2 = [](int32_t x) { return cdiv(imul(x, 3), 2); };
	auto half = [](int32_t x) { return cdiv(x, 2); };
	for (int j = kUnits; j < kEndUnits; ++j) {
		const Type& u = r.types[size_t(j)];
		o.relative_value[size_t(j - kUnits)] = 0;
		if (!(u.role & 0x10000)) continue;
		const int32_t D = u.obj_masks;
		int32_t v = 0x100;
		if ((A & 0x1000000) && o.max_range == 0 && (D & 0x90408)) v = 0x180;
		if ((A & 0x20000000) && o.max_range == 0 && ((D & 0x10000) || (D & 0x84000))) v = h2(v);
		if ((A & 0x80) && (D & 0x81800)) v = h2(v);
		if ((A & 0x40000000) && (D & 0x1000)) v = h2(v);
		if (is(r, o, 123) && (D & 0x10108)) v = h2(v);
		if ((A & 0x108) && is(r, u, 123)) v = half(v);
		if ((D & A) & 0x2000) {
			v = h2(v);
			if (is(r, o, 340) && is(r, u, 346)) v = h2(v);
			if (is(r, o, 334) && !is(r, u, 334)) v = h2(v);
			if (!is(r, o, 334) && is(r, u, 334)) v = half(v);
			if (is(r, o, 334)) v = half(v);
		}
		bool l08b;
		if (A & 0x10) {
			if (D & 0x80000) v = h2(v);
			l08b = true;
		} else {
			l08b = is(r, o, 213);
		}
		if (l08b && (D & 0x1010) == 0x1000 && !is(r, u, 213)) v = h2(v);
		const int32_t A22 = A & 0x400000;
		if (A22 && (D & 1) && !(A & 0x20040)) v = cdiv(v, 8);
		if ((A & 0x430000) == 0x20000 && (D & 1)) v = half(v);
		if (A & 0x400) {
			if (D & 0x80) v = h2(v);
			if (D & 0x4000) v = h2(v);
			if (!A22 && (D & 0x10000)) v = half(v);
		}
		if (A & 0x4000) {
			if (D & 0x80) v = cdiv(imul(v, 5), 4);
			if (!A22 && (D & 0x10000)) v = cdiv(imul(v, 2), 3);
		}
		if (A & 0x40) {
			if (D & 0x80) v = imul(v, 2);
			if ((A & 0x20) && (D & 0x1000)) v = cdiv(imul(v, 5), 4);
		}
		if ((A & 0x100000) && (D & 1)) v = h2(v);
		bool skip119 = false, goto213 = true;
		if ((A & 0x10000000) && !(A & 0x8000000) && A < 0) {
			if (D & 0x10000000) v = imul(v, 3);
			else goto213 = false;
		}
		if (goto213 && (D & 0x10000000) && !(D & 0x8000000)) {
			v = half(v);
			if (A < 0) {
				v = imul(v, 3);
				if (A & 0x200000) v = imul(v, 2);
			}
			skip119 = true;
		}
		if (!skip119 && is(r, o, 281)) v = cdiv(v, 3);
		o.relative_value[size_t(j - kUnits)] = i16((v < 0x300 ? (v & 0xffff) : 0x300) - 0x100);
	}
}

// ---------------------------------------------------------------- finalisation

namespace {

// ObjectType::init_is_list @00661dd0: ascending TypeIndex lists of everything is_slow(t, 0) / is_slow(t, 1)
// accepts. SimpleArray growth step 1, so capacity == count; flags byte 0.
void init_is_list(const Rules& r, Type& o) {
	o.is_list = U16Array();
	o.is_strict_list = U16Array();
	U16Array a, s;
	for (int t = 0; t < kNumTypes; ++t) {
		if (is_slow(r, o, t, false)) a.v.push_back(uint16_t(t));
		if (is_slow(r, o, t, true)) s.v.push_back(uint16_t(t));
	}
	for (U16Array* l : {&a, &s}) {
		if (l->v.empty()) continue;
		l->count = l->capacity = int32_t(l->v.size());
		l->block = 1;
		l->flags = 0;
	}
	o.is_list = std::move(a);
	o.is_strict_list = std::move(s);
}

// UnitType::init_final_flags @0061dc70: unit_flags2 bits derived from upgrade lines.
void init_final_flags(const Rules& r, Type& o) {
	auto i = [&](int t) { return is(r, o, t); };
	if (i(265) || i(279) || o.type == 61 || o.type == 62 || o.type == 400 || i(123) || i(317) || i(278)) o.unit_flags2 |= 4;
	if (i(63) || i(352) || i(354) || i(356)) o.unit_flags2 |= 0x40;
	if (i(54)) o.unit_flags2 |= 0x20;
	if (i(59) || i(318)) o.unit_flags2 |= 8;
	if (i(69)) o.unit_flags2 |= 0x10;
	if (i(320) || i(318)) o.unit_flags |= 0x10;
	if (i(123) || i(131)) o.unit_flags2 |= 1;
}

// UnitType::init_spellcasters @0061aae0: a caster when any unit up the FROM chain (or its graft) casts.
void init_spellcasters(const Rules& r, Type& o) {
	int t = o.type;
	for (int guard = 0;; ++guard) {
		if (guard > 100000) throw std::runtime_error("init_spellcasters: FROM cycle at unit " + std::to_string(o.type));
		const Type* u = &r.types.at(size_t(t));
		if (u->unit_flags2 & 2) break;
		if (u->graft >= 0) {
			t = u->graft;
			u = &r.types.at(size_t(t));
			if (u->unit_flags2 & 2) break;
		}
		t = u->from;
		if (t < 0) return;
	}
	o.unit_flags2 |= 2;
}

}  // namespace

// ObjectType::finalize_init_all @0065f4a0: units 50..401, then buildings 414..542.
void finalize_init_all(Rules& r) {
	for (int t = kUnits; t < kEndUnits; ++t) {
		Type& o = r.types[size_t(t)];
		init_is_list(r, o);
		init_final_flags(r, o);
		init_spellcasters(r, o);
	}
	for (int t = kBuilds; t < kEndBuilds; ++t) {
		Type& o = r.types[size_t(t)];
		init_is_list(r, o);
		// BuildType::init_final_flags: farms and oil.
		if (is(r, o, 417) || is(r, o, 421) || is(r, o, 422)) o.build_flags |= 0x10000000u;
	}
}

}  // namespace detail
}  // namespace ron::sim
