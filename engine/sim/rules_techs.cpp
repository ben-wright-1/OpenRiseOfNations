// Techs, goods, the item, crafts and bonuses (TechType::init @0066c7b0, set_research, compute_ai_values,
// GoodType::init @0066ea20, compute_largest_gather, ItemType::init, SpellType::init @00674a80, BonusType::init
// @00670280; docs/rules-spec.md section 9; port of re/tools/rules/techs.mjs).
#include <algorithm>
#include <stdexcept>
#include <string>

#include "sim/rules_internal.hpp"

namespace ron::sim::detail {

namespace {

// Building TypeIndex constants the AI weighting uses.
constexpr int kVillage = 414, kTown = 415, kDock = 432, kTemple = 437, kFortX = 443;

inline bool is_epoch(int t) { return t >= kEpochs && t < kEndEpochs; }
inline bool is_age(int t) { return t >= kAges && t < kEndAges; }
inline bool is_unit(int t) { return t >= kUnits && t < kEndUnits; }  // TypeData::is_unit_type (gaia excluded)
inline bool is_build(int t) { return t >= kBuilds && t < kEndBuilds; }
inline bool is_tech(int t) { return t >= kTechs && t < kEndTechs; }

// Virtual num_preq: Tech/BuildTypeData 3; Unit/Good/BonusTypeData 2; TypeData (crafts, the item) 1.
int num_preq(int t) {
	if (is_tech(t) || is_build(t)) return 3;
	if (t < kEndGoods || (t >= kUnits && t < kBuilds) || t >= kBonuses) return 2;
	return 1;
}

// ObjectTypeData::is(p, 0) while the is-lists are still empty, i.e. is_slow; plain equality for other classes.
bool is_a(const Rules& r, const Type& o, int p) {
	return is_object_kind(o.kind) ? is_slow(r, o, p, false) : o.type == p;
}

// The tech AI pass sees the GameInfo in effect at load time and treats 50..401 as units.
int preq_at_load(const Loader& L, const Type& o, int i) { return get_preq(L.r, o, i, L.opts.load_game, kEndUnits); }

// Type::find_preq @00668380: any effective preq slot equals x.
bool find_preq(const Loader& L, const Type& o, int x) {
	const int n = num_preq(o.type);
	for (int i = 0; i < n; ++i) {
		if (preq_at_load(L, o, i) == x) return true;
	}
	return false;
}

// TechType::add_preq_ai @0066c350: add `amount` to ai[idx] of every tech preq, recursing while depth != 0
// (depth -1 walks the whole preq tree).
void add_preq_ai(Loader& L, const Type& o, int idx, int amount, int depth) {
	for (int i = 0; i < 3; ++i) {
		const int p = preq_at_load(L, o, i);
		if (p < 0 || !is_tech(p)) continue;
		Type& tp = L.t(p);
		tp.ai[size_t(idx)] = i16(tp.ai[size_t(idx)] + amount);
		if (depth != 0) add_preq_ai(L, tp, idx, amount, depth - 1);
	}
}

}  // namespace

// ---------------------------------------------------------------- techs
void init_tech(Loader& L, XNode* el, int t, int pass) {
	Type& o = L.alloc(t);
	o.type = t;
	if (pass == 0) {
		L.set_name(o, xml::get_text(el, u"NAME"));
		o.type_name = o.name;
		return;
	}
	o.where = -1;
	o.grid_x = i8(xml::get_text_num(el, u"GRID_X", -1));
	o.grid_y = i8(xml::get_text_num(el, u"GRID_Y", -1));
	o.tex.id = xml::get_text(el, u"TEX_ID");
	o.tex.col = i8(xml::get_text_num(el, u"TEX_COL", -1));
	o.tex.row = i8(xml::get_text_num(el, u"TEX_ROW", -1));
	o.tex.x = i16(xml::get_text_num(el, u"TEX_X", -1));
	o.tex.y = i16(xml::get_text_num(el, u"TEX_Y", -1));
	o.tex.clip = i8(xml::get_text_num(el, u"TEX_CLIP", -1));
	o.preq[0] = L.tech_key(xml::get_text(el, u"PREQ0"));
	o.preq[1] = L.tech_key(xml::get_text(el, u"PREQ1"));
	o.preq[2] = L.tech_key(xml::get_text(el, u"PREQ2"));
	o.age = xml::get_text_num(el, u"AGE", -1);
	o.res_time = o.job_time = uint32_t(xml::get_text_num(el, u"JOB_TIME", -1));
	load_cost(xml::child_or_create(el, u"COST"), o.costs);
	o.tribe_mask = parse_binary(xml::get_text(el, u"TRIBE_MASK"));
	o.show[0] = L.tech_key(xml::get_text(el, u"SHOW0"));
	o.show[1] = L.tech_key(xml::get_text(el, u"SHOW1"));
	o.ai.fill(0);
	o.leader_off = 0;
}

// Types::init: ALTERNATENAMES/EPOCHTYPE (28, document order -> epochs 551..578), 8 STRINGs each (one per age).
void load_alternate_names(Loader& L) {
	const XDoc d = read_game_xml(L.files, "alternatenames.xml", L.opts.lang_suffix);
	const std::vector<XNode*> eps = xml::children(xml::child(d.root, u"ALTERNATENAMES"), u"EPOCHTYPE");
	if (eps.size() != 28) throw std::runtime_error("alternatenames.xml: " + std::to_string(eps.size()) + " EPOCHTYPEs, expected 28");
	for (size_t i = 0; i < eps.size(); ++i) {
		const std::vector<XNode*> s = xml::children(eps[i], u"STRING");
		if (s.size() != 8) throw std::runtime_error("alternatenames.xml: EPOCHTYPE " + std::to_string(i) + " has " +
		                                            std::to_string(s.size()) + " STRINGs, expected 8");
		Type& o = L.t(kEpochs + int(i));
		for (size_t k = 0; k < 8; ++k) o.alternate_name[k] = xml::text(s[k]);
	}
}

// TechType::set_research @0066cba0 (second pass over techrules.xml, after the bonuses).
void set_research(Loader& L, XNode* el, int t) {
	Type& o = L.t(t);
	o.where = L.build_key(xml::get_text(el, u"WHERE"));
	for (int b = kBuilds; b < kEndBuilds; ++b) {
		Type& bt = L.t(b);
		if (bt.loaded && is_a(L.r, bt, o.where)) bt.build_flags |= 0x8000000u;  // "researches techs"
	}
	if (is_epoch(t)) {
		const int q = (t - kEpochs) / 7;  // 0 science, 1 commerce, 2 civic, 3 military
		o.cat = q == 0 ? 3 : q == 1 ? 2 : q == 2 ? 1 : 0;
	} else {
		o.cat = 3;
	}
}

// TechType::compute_ai_values @0066cdc0, for every tech in order after set_research.
void compute_ai_values(Loader& L, int t) {
	Rules& r = L.r;
	Type& o = L.t(t);
	auto add = [&](int k, int v) { o.ai[size_t(k)] = i16(o.ai[size_t(k)] + v); };
	// 1. the tech itself
	if (is_epoch(t)) {
		if (o.cat == 3) {
			add(4, 1);
			add(10, 1);
		} else if (o.cat == 1) {
			add(5, 1);
		} else if (o.cat == 2) {
			add(1, 1);
		} else if (o.cat == 0) {
			add(0, 1);
		}
	} else if (o.where == kTemple) {
		add(5, 2);
	} else if (o.where == kFortX) {
		add(5, 1);
		add(0, 1);
	}
	// 2. units that need it
	for (int u = kUnits; u < kEndUnits; ++u) {
		const Type& ut = r.types[size_t(u)];
		if (!find_preq(L, ut, t)) continue;
		if (ut.role & 0x10000) add(0, 1);
		if (ut.role & 0x200) {
			add(1, 1);
			add(9, 1);
		}
		if (ut.domain == 0) {
			if (ut.role & 0x10000) add(8, 1);
		} else {
			if (ut.carry != 0) add(3, 1);
			if (ut.domain == 1) add(2, 1);
			else add(6, 1);
		}
	}
	// 3. buildings that need it
	for (int b = kBuilds; b < kEndBuilds; ++b) {
		const Type& bt = r.types[size_t(b)];
		if (!find_preq(L, bt, t)) continue;
		const int s = bt.from < 0 ? 2 : 1;  // base buildings weigh double
		if (is_a(r, bt, kVillage)) {
			add(1, 1);
			if (b == kTown) {
				add(5, 1);
				add(1, 1);
				add_preq_ai(L, o, 1, 1, 0);
			}
		}
		if (bt.from < 0) {
			if (bt.build_flags & 0x40) {
				add(1, 2 * s);
				add(9, 2 * s);
			}
			if (bt.build_flags & 0x8000000) {
				add(1, 2 * s);
				add(4, 2 * s);
				if (b == 0x1a7 || b == 0x1a8 || b == 0x1aa || b == 0x1a9) add(9, 2 * s);
			}
			if (is_a(r, bt, kDock)) {
				add(2, 1);
				add_preq_ai(L, o, 2, 1, -1);
			}
		}
		if (is_a(r, bt, kDock)) {
			add(2, 1);
			add_preq_ai(L, o, 2, 1, 2);
		}
		int f = b;  // BuildTypeData::fundamental_type
		while (r.types[size_t(f)].from >= 0) f = r.types[size_t(f)].from;
		const Type& ft = r.types[size_t(f)];
		if (ft.build_flags & 0x40) {
			add(1, s);
			add(9, s);
		} else if (ft.build_flags & 0x8000000) {
			add(1, s);
			add(4, s);
		} else if (is_a(r, ft, 0x1b7) || is_a(r, ft, 0x1bb) || is_a(r, ft, 0x1bf) || ft.attack != 0) {
			add(0, s);
		} else if (is_a(r, ft, 0x1a7) || is_a(r, ft, 0x1a8) || is_a(r, ft, 0x1a9) || is_a(r, ft, 0x1aa)) {
			add(1, 8);
			add(9, 8);
		}
		if (ft.build_flags & 0x80000000u) {  // trains units
			for (int u = kUnits; u < kEndUnits; ++u) {
				const Type& ut = r.types[size_t(u)];
				if (ut.where != b) continue;
				if (ut.role & 0x180c0) add(0, s);
				else add(1, 1);
			}
		}
	}
	// 4. techs that need it
	for (int k = kTechs; k < kEndTechs; ++k) {
		if (!find_preq(L, r.types[size_t(k)], t)) continue;
		add(4, 1);
		if (is_epoch(k)) {
			add(4, 1);
			add(0, 1);
		}
		if (is_age(k)) {
			add(4, 2);
			add(0, 2);
		}
	}
	// 5. goods, crafts, bonuses that need it
	for (int g = 0; g < kEndGoods; ++g) {
		if (!find_preq(L, r.types[size_t(g)], t)) continue;
		const int v = g < kNumBasicGoods ? 4 : 1;
		add(9, v);
		add(1, v);
	}
	for (int s = kSpells; s < kEndSpells; ++s) {
		if (!find_preq(L, r.types[size_t(s)], t)) continue;
		add(0, 4);
		add(1, 2);
	}
	for (int k = kBonuses; k < kEndBonuses; ++k) {
		if (!find_preq(L, r.types[size_t(k)], t)) continue;
		if (k == 684 /* FOREIGN_TRADE */) add(1, 1);
		add(1, 1);
		add(4, 2);
	}
}

// ---------------------------------------------------------------- goods
void init_good(Loader& L, XNode* el, int t, int pass) {
	Type& o = L.alloc(t);
	if (pass == 0) {
		o.type = t;
		L.set_name(o, xml::get_text(el, u"NAME"));
		o.graph_name = o.help_name = o.type_name = o.name;
		o.abbrev = xml::get_text(el, u"ABBREV");
		o.x_size = std::max(1, xml::get_text_num(el, u"X_SIZE", -1));
		o.y_size = std::max(1, xml::get_text_num(el, u"Y_SIZE", -1));
		o.block = std::max(0, xml::get_text_num(el, u"BLOCK", -1));
		o.exclude_ctw = std::max(0, xml::get_text_num(el, u"EXCLUDE_CTW", -1));
		o.random_rare_drop = std::max(0, xml::get_text_num(el, u"RANDOM_RARE_DROP", -1));
		o.preq[0] = L.tech_key(xml::get_text(el, u"PREQ0"));
		o.preq[1] = L.tech_key(xml::get_text(el, u"PREQ1"));
		o.obs = L.tech_key(xml::get_text(el, u"OBS"));
		return;
	}
	if (t < kNumBasicGoods) {
		auto gr = [&](const char16_t* n) { return L.good_key(xml::get_text(el, n)); };
		auto fr = [&](const char16_t* n) { return fraction(xml::get_text(el, n), 0x100); };
		o.undiscovered_cost_good = gr(u"UNDISC_COST_GOOD");
		o.undiscovered_cost_rate = fr(u"UNDISC_COST_RATE");
		o.obsolete_cost_good = gr(u"OBS_COST_GOOD");
		o.obsolete_cost_rate = fr(u"OBS_COST_RATE");
		o.undiscovered_support_good = gr(u"UNDISC_SUPPORT_GOOD");
		o.undiscovered_support_rate = fr(u"UNDISC_SUPPORT_RATE");
		o.obsolete_support_good = gr(u"OBS_SUPPORT_GOOD");
		o.obsolete_support_rate = fr(u"OBS_SUPPORT_RATE");
		o.obsolete_production_good = gr(u"OBS_PROD_GOOD");
		o.obsolete_production_rate = fr(u"OBS_PROD_RATE");
		o.bonus_type = {{-1, -1}};
		o.bonus_num = {{0, 0}};
	} else {
		o.bonus_type[0] = L.good_key(xml::get_text(el, u"BONUS_TYPE0"));
		o.bonus_num[0] = xml::get_text_num(el, u"BONUS_NUM0", -1);
		o.bonus_type[1] = L.good_key(xml::get_text(el, u"BONUS_TYPE1"));
		o.bonus_num[1] = xml::get_text_num(el, u"BONUS_NUM1", -1);
	}
	o.largest_gather = 0;
	o.desc = xml::get_text(el, u"DESC");           // the game then runs String::lookup (interface text)
	o.abbrev = xml::get_text(el, u"DESC_ABBREV");
}

// GoodType::compute_largest_gather (Lands::init, after Types::init) for the 6 basic goods: the largest MAKE num
// over the 9 lands except 1 and 2 (first 4 MAKEs each), clamped to [1, 2].
void compute_largest_gather(Loader& L) {
	const XDoc d = read_game_xml(L.files, "rules.xml");
	const std::vector<XNode*> lands = xml::children(xml::child(d.root, u"LANDS"), u"LAND");
	if (lands.size() != 9) throw std::runtime_error("rules.xml: " + std::to_string(lands.size()) + " LANDs, expected 9");
	L.r.lands.clear();
	for (XNode* l : lands) {
		std::vector<LandMake> makes;
		for (XNode* m : xml::children(l, u"MAKE")) {
			makes.push_back({xml::get_attrib_num(m, u"num", -1), L.good_key(xml::get_attrib(m, u"type"))});
		}
		L.r.lands.push_back(std::move(makes));
	}
	for (int g = 0; g < kNumBasicGoods; ++g) {
		int32_t v = 0;
		for (size_t i = 0; i < L.r.lands.size(); ++i) {
			if (i == 1 || i == 2) continue;
			const std::vector<LandMake>& makes = L.r.lands[i];
			for (size_t k = 0; k < makes.size() && k < 4; ++k) {
				if (makes[k].good == g && makes[k].num > v) v = makes[k].num;
			}
		}
		L.t(g).largest_gather = v < 1 ? 1 : v > 2 ? 2 : v;
	}
}

// ---------------------------------------------------------------- item (pass 1 is a no-op)
void init_item(Loader& L, XNode* el, int t, int pass) {
	Type& o = L.alloc(t);
	if (pass != 0) return;
	o.type = t;
	L.set_name(o, xml::get_text(el, u"NAME"));
	o.graph_name = o.type_name = o.name;
	o.abbrev = xml::get_text(el, u"ABBREV");
}

// ---------------------------------------------------------------- crafts
void init_spell(Loader& L, XNode* el, int t) {
	Type& o = L.alloc(t);
	o.use.fill(0);
	o.type = t;
	const ustr name = xml::get_text(el, u"NAME");
	L.set_name(o, name);
	o.graph_name = xml::get_text(el, u"GRAPH");
	o.help_name = xml::get_text(el, u"GRAPH");
	const ustr tn = xml::get_text(el, u"TYPENAME");
	o.type_name = tn.empty() ? name : tn;
	o.grid_x = i8(xml::get_text_num(el, u"GRID_X", -1));
	o.grid_y = i8(xml::get_text_num(el, u"GRID_Y", -1));
	o.grid_x2 = i8(xml::get_text_num(el, u"GRID_X2", -1));
	o.grid_y2 = i8(xml::get_text_num(el, u"GRID_Y2", -1));
	o.from = L.object_key(xml::get_text(el, u"FROM"));
	o.from2 = L.object_key(xml::get_text(el, u"FROM2"));
	o.preq[0] = L.tech_key(xml::get_text(el, u"PREQ0"));
	o.job_time = uint32_t(xml::get_text_num(el, u"JOB_TIME", -1));
	o.res_time = uint32_t(xml::get_text_num(el, u"RES_TIME", -1));
	o.duration = imul(xml::get_text_num(el, u"DURATION", -1), 15);
	o.duration_upgrade = imul(xml::get_text_num(el, u"DURATION_UPGRADE", -1), 15);
	o.mana = xml::get_text_num(el, u"MANA", -1);
	o.spell_range = imul(xml::get_text_num(el, u"SPELL_RANGE", -1), 0xc0);
	o.spell_flags = lower_letter_flags(xml::get_text(el, u"FLAGS"));
	load_cost(xml::child_or_create(el, u"COST"), o.costs);
	load_cost(xml::child_or_create(el, u"COST2"), o.use);
	o.tribe_mask = parse_binary(xml::get_text(el, u"TRIBE_MASK"));
	// FROM marks its caster (units: unit_flags2 bit 1; buildings: build flag 0x20000000) and gives the craft's
	// where. FROM2 is only looked at when FROM is set.
	if (o.from < 0) {
		o.where = -1;
	} else {
		if (is_unit(o.from)) {
			Type& u = L.t(o.from);
			u.unit_flags2 |= 2;
			o.where = u.where;
		} else if (is_build(o.from)) {
			L.t(o.from).build_flags |= 0x20000000u;
			o.where = o.from;
		}
		if (o.from2 >= 0) {
			if (is_unit(o.from2)) L.t(o.from2).unit_flags2 |= 2;
			else if (is_build(o.from2)) L.t(o.from2).build_flags |= 0x20000000u;
		}
	}
	o.upgrade = -1;
}

// Types::init: crafts 669..683 (the DECLARE_WAR and CANCEL_ALLIANCE variants) copy DECLARE_WAR's costs.
void post_spells(Rules& r) {
	for (int t = kDeclareWar + 1; t < kEndSpells; ++t) r.types[size_t(t)].costs = r.types[size_t(kDeclareWar)].costs;
}

// ---------------------------------------------------------------- bonuses
void init_bonus(Loader& L, XNode* el, int t) {
	Type& o = L.alloc(t);
	XNode* preq = xml::child_or_create(el, u"PREQ");
	o.type = t;
	o.desc = xml::get_text(el, u"DESC");
	o.preq[0] = L.tech_key(xml::get_attrib(preq, u"preq0"));
	// DESPOTISM_2/3, REPUBLIC_2/3, MONARCHY_2, DEMOCRACY_2 require the previous level.
	const bool with_prev = t == 795 || t == 796 || t == 798 || t == 799 || t == 801 || t == 803;
	o.preq[1] = with_prev ? t - 1 : -1;
}

}  // namespace ron::sim::detail
