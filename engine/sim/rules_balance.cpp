// Combat balance table: Balance::init / fill_tables / compute_modifier / type_damage (docs/rules-spec.md section 11;
// port of re/tools/rules/balance.mjs). final[a - 50][d - 50] for attackers and defenders 50..542.
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "sim/rules_internal.hpp"

namespace ron::sim::detail {

namespace {

constexpr int kFirstWonder = 526;
constexpr int kRaw = 399;  // 352 unit names + 5 lines + 2 objects + 8 ages + 32 flags
constexpr int kRLine = 352, kRObj = 357, kRAge = 359, kRFlag = 367;
const char* const kLineStr[] = {"SIEGE", "FORTS", "TOWERS", "CITIES", "OBSPOST"};
const char* const kObjectStr[] = {"BUILDINGS", "UNITS"};
const char* const kObjMaskStr[] = {"ARMORED", "BOMBARD", "CIVILIAN", "MUSKET_INF", "ELEPHANT", "FOOT", "GUN",
                                    "HEAVY_INF", "MODERN_INF", "CARRY_AIR", "FOOT_ARCHER", "LARGE", "MOUNTED", "NAVAL",
                                    "HORSE_ARCHER", "SPARSE", "LIGHT_INF", "ARCHERY", "SIEGE", "WAR_MACHINE",
                                    "ARMORPIERCE", "VEHICLE", "MELEE", "EXPLOSIVE", "HEAVY_CAV", "DETECT", "UNUSED",
                                    "MISSILE", "AIR", "LIGHT_CAV", "PIKE", "ANTI_AIR"};

// (v * k) / 100 with int32 wrap and C truncation.
inline int32_t pct(int32_t v, int32_t k) { return imul(v, k) / 100; }

inline bool is_unit(int t) { return t >= kUnits && t < kBuilds; }  // TypeData::is_unit_type (includes gaia)
inline bool is_gaia(int t) { return t >= kGaia && t < kEndGaia; }
inline bool is_build(int t) { return t >= kBuilds && t < kEndBuilds; }
inline bool is_wonder(int t) { return t >= kFirstWonder && t < kEndBuilds; }

// Balance::lookup_name via lookup_absolute_name: raw index -> balance.xml attribute / ENTRY name. The flag names
// join with internal string #17, which is empty in the shipped table, so they read "Flag_YOBJMASK_HEAVY_CAV" and never
// match balance.xml's "Flag_Y_OBJMASK_HEAVY_CAV": every flag row and column stays 100 (verified against the oracle).
ustr balance_name(int i, const Rules& r) {
	ustr s;
	if (i < kRLine) {
		for (char16_t c : r.types[size_t(kUnits + i)].name) {
			if (c != u'\'') s.push_back(c);
		}
	} else if (i < kRObj) {
		s = from_ascii(kLineStr[i - kRLine]);
	} else if (i < kRAge) {
		s = from_ascii(kObjectStr[i - kRObj]);
	} else if (i < kRFlag) {
		s = from_ascii(("AGE " + std::to_string(i - kRAge)).c_str());
	} else {
		const int b = i - kRFlag, c = 65 + b;
		s = u"Flag ";
		s.push_back(char16_t(c > 90 ? c - 42 : c));  // A..Z then 1..6
		s += from_ascii("OBJMASK_");
		s += from_ascii(kObjMaskStr[b]);
	}
	for (char16_t& c : s) {
		if (c == u' ') c = u'_';
	}
	return s;
}

struct UStrHash {
	size_t operator()(const ustr& s) const { return std::hash<std::u16string>()(s); }
};

// Balance::fill_tables, first half: raw[row * 399 + col] from balance.xml TABLE/ENTRY (row = ENTRY, col = attribute).
std::vector<int16_t> read_raw_table(Loader& L) {
	const XDoc d = read_game_xml(L.files, "balance.xml");
	const XNode* table = xml::child(d.root, u"TABLE");
	std::unordered_map<ustr, const XNode*, UStrHash> by_name;
	if (table) {
		for (const auto& e : table->children) {
			if (e->is_text || e->tag != u"ENTRY") continue;
			if (const ustr* n = e->attr(u"name")) by_name.emplace(*n, e.get());  // first entry wins
		}
	}
	std::vector<ustr> names(kRaw);
	for (int i = 0; i < kRaw; ++i) names[size_t(i)] = balance_name(i, L.r);
	std::vector<int16_t> raw(size_t(kRaw) * kRaw, 100);
	for (int row = 0; row < kRaw; ++row) {
		auto it = by_name.find(names[size_t(row)]);
		if (it == by_name.end()) continue;
		std::unordered_map<ustr, const ustr*, UStrHash> attrs;
		for (const auto& a : it->second->attrs) attrs.emplace(a.first, &a.second);
		for (int c = 0; c < kRaw; ++c) {
			// get_attrib_num(name, 100): empty or missing -> 100.
			auto at = attrs.find(names[size_t(c)]);
			const int32_t v = at == attrs.end() || at->second->empty() ? 100 : convert_int(*at->second);
			raw[size_t(row) * kRaw + size_t(c)] = i16(v);
		}
	}
	return raw;
}

struct Ops {
	const Rules& r;
	const std::vector<int32_t>& ages;
	bool is(const Type& o, int x, bool strict = false) const { return detail::is(r, o, x, strict); }
	bool is_siege(const Type& o) const { return is_unit(o.type) && (o.unit_flags & 0x20000) != 0; }
	bool is_missile(const Type& o) const { return is_unit(o.type) && (o.obj_masks & 0x8000000) != 0; }
	bool is_caravan(const Type& o) const { return is_unit(o.type) && (o.unit_flags2 & 8) != 0; }
	int32_t age(const Type& o) const {
		const int32_t a = ages[size_t(o.type)];
		if (a == -1) throw std::runtime_error("balance: no age for type " + std::to_string(o.type));
		return a;
	}
};

// Balance::return_pack + the raw indices it selects (type, line, object class, age, every obj-mask bit).
std::vector<int> pack_indices(const Type& o, const Ops& ops) {
	int type = -1, line = -1, obj = -1;
	if (is_unit(o.type)) {
		type = o.type - kUnits;
		if (ops.is_siege(o)) line = 0;
		obj = 1;
	} else {
		if (ops.is(o, 443)) line = 1;       // BuildTypeData::is_fort
		else if (ops.is(o, 414)) line = 3;  // is_city
		else if (ops.is(o, 521)) line = 4;
		else if (ops.is(o, 439)) line = 2;
		obj = 0;
	}
	const int32_t age = ops.age(o);
	std::vector<int> out;
	if (type >= 0) out.push_back(type);
	if (line >= 0) out.push_back(line + kRLine);
	if (obj >= 0) out.push_back(obj + kRObj);
	if (age >= 0) out.push_back(age + kRAge);
	for (int b = 0; b < 32; ++b) {
		if ((uint32_t(o.obj_masks) >> b) & 1) out.push_back(b + kRFlag);
	}
	return out;
}

// Balance::type_damage @0057fb50: hard-coded percentage chain, integer and truncating, in this exact order.
int32_t type_damage(const Type& A, const Type& D, const Ops& ops, const Constants& K) {
	auto is = [&](const Type& o, int x, bool strict = false) { return ops.is(o, x, strict); };
	const int32_t am = A.obj_masks, dm = D.obj_masks;
	const int at = A.type, dt = D.type;
	const int32_t ageA = ops.age(A), ageD = ops.age(D);
	int32_t v = 100;
	if (is_unit(at) && ageD < ageA && is_unit(dt)) {
		const int32_t d = ageA - ageD;
		const int32_t k = d == 1   ? K.at(Constants::kOneAgeDown)
		                  : d == 2 ? K.at(Constants::kTwoAgesDown)
		                  : d == 3 ? K.at(Constants::kThreeAgesDown)
		                  : d == 4 ? K.at(Constants::kFourAgesDown)
		                           : K.at(Constants::kFiveAgesDown);
		v = pct(iadd(k, 100), 100);
	}
	if (am & 0x10000) {
		if (dm & 0x400) v = pct(v, 162);
		if (dm & 0x4000) v = pct(v, 152);
	}
	if (am & 0x80) {
		if (!(am & 0x40)) {
			if (dm & 0x1000) v = pct(v, 166);
			if (is_build(dt)) v = pct(v, 130);
			if (dm & 0x400) v = pct(v, 86);
			if (dm & 8) v = pct(v, 75);
			if (dm & 0x100) v = pct(v, 75);
		} else {
			if (dm & 0x1000) v = pct(v, 257);
			if (dm & 0x4000) v = pct(v, 66);
			if (dm & 0x20000000) v = pct(v, 114);
			if (is(D, 216)) v = pct(v, 225);
			if (is(D, 239)) v = pct(v, 132);
			if (dm & 0x100) v = pct(v, 80);
		}
	}
	if (am & 0x400) {
		if (dm & 0x10000) v = pct(v, 65);
		if (dm & 8) v = pct(v, 90);
		if (dm & 0x80) v = pct(v, 253);
		if (dm & 0x4000) v = pct(v, 138);
		if (dm & 0x8000) v = pct(v, 80);
		if (is_build(dt)) v = pct(v, 33);
		if (ops.is_siege(D)) v = pct(v, 75);
	}
	if (am & 0x20000000) {
		if (dm & 0x400) v = pct(v, 114);
		if (dm & 0x10000) v = pct(v, 158);
		if (dm & 8) v = pct(v, 149);
		if (dm & 0x100) v = pct(v, 159);
		if (is(D, 123)) v = pct(v, 130);
		if (is(D, 131)) v = pct(v, 125);
		if (dm & 0x40000) v = pct(v, 122);
		if (dm & 1) v = pct(v, 98);
		if (is_build(dt)) v = pct(v, 33);
		if (ops.is_caravan(D) || dt == 61 || dt == 62 || dt == 400) v = v / 3;
		if (dt == 50 || dt == 51 || dt == 52 || dt == 53 || is(D, 66)) v = v / 2;
	}
	if (am & 0x4000) {
		if (is_build(dt)) v = pct(v, 33);
		if (ops.is_siege(D)) v = pct(v, 75);
		if (dm & 0x20000000) v = pct(v, 57);
		if (dm & 0x400) v = pct(v, 57);
		if (dm & 0x10000) v = pct(v, 66);
		if (dm & 8) v = pct(v, 66);
		if (dm & 0x80) v = pct(v, 160);
		if (am & 0x200000) {
			if (dm & 0x20000000) v = pct(v, 160);
			if (dm & 0x400) v = pct(v, 135);
			if (dm & 0x10000) v = imul(v, 2);
			if (dm & 8) v = imul(v, 2);
			if (dm & 0x80) v = pct(v, 50);
			if (dm & 0x1000000) v = pct(v, 97);
		}
		if (am & 0x40) {
			if (is(D, 123)) v = pct(v, 155);
			if (dm & 1) v = pct(v, 92);
		}
	}
	if (am & 0x1000000) {
		if (dm & 0x400) v = pct(v, 170);
		if (dm & 0x10000) v = pct(v, 179);
		if (dm & 8) v = pct(v, 155);
		if (dm & 0x100) v = pct(v, 170);
		if (is(D, 123)) v = pct(v, 155);
		if (is(D, 131)) v = pct(v, 140);
		if (is_build(dt)) v = pct(v, 33);
	}
	if (am & 8) {
		v = pct(v, 133);
		if (dm & 0x400) v = pct(v, 125);
		if (dm & 0x80) v = pct(v, 180);
		if (dm & 0x4000) v = pct(v, 114);
		if (is_build(dt)) v = pct(v, 66);
		if (ops.is_siege(D)) v = pct(v, 75);
		if (ageA == 4 && (dm & 0x80)) v = pct(v, 110);
	}
	if (am & 1) {
		if (is(A, 239) && (dm & 8)) v = pct(v, 155);
		if (is(A, 239) && (dm & 0x100)) v = pct(v, 185);
		if (is(A, 239) && (dm & 0x1000000)) v = pct(v, 115);
		if (is(A, 239) && (dm & 0x20000000)) v = pct(v, 120);
		if (is(A, 239) && (dm & 0x4000)) v = pct(v, 120);
		if (is(A, 239) && is(D, 123)) v = pct(v, 175);
		if (is(A, 216) && (dm & 0x100)) v = pct(v, 175);
		if (is(A, 216) && (dm & 8)) v = pct(v, 175);
		if (is(A, 216) && is(D, 239)) v = pct(v, 120);
		if (is_build(dt)) v = pct(v, 66);
	}
	if (am & 0x100) {
		if (dm & 0x80) v = pct(v, 260);
		if (is(D, 216)) v = pct(v, 170);
		if (is(D, 239)) v = pct(v, 75);
		if (is(D, 123)) v = pct(v, 66);
		if (dm & 4) v = pct(v, 120);
		if (ops.is_siege(D)) v = pct(v, 75);
		if (is_build(dt)) v = pct(v, 66);
	}
	if (is(A, 123)) {
		if (dm & 0x10008) v = pct(v, 330);
		if (dm & 0x100) v = pct(v, 330);
		if (dm & 0x80) v = pct(v, 330);
		if (dm & 4) v = imul(v, 5);
		if (dm & 1) v = pct(v, 50);
		if (is_build(dt)) v = pct(v, 50);
		if (ops.is_siege(D)) v = pct(v, 75);
	}
	if (is(A, 131, true)) {
		if (dm & 8) v = pct(v, 88);
		if (dm & 0x100) v = pct(v, 80);
		if (dm & 0x1000) v = pct(v, 125);
	}
	if ((am & 0x100000) && (dm & 1)) v = pct(v, 180);
	if (is(A, 66) && (dm & 0x14000)) v = imul(v, 2);
	if ((at == 50 || at == 51 || at == 52 || at == 53) && (dm & 0x14000)) v = imul(v, 2);
	if (am & 0x2000) {
		if (is_build(dt) && !(am & 0x40000)) v = pct(v, 33);
		if (is_unit(dt) && !(dm & 0x10002000) && !ops.is_siege(D)) v = pct(v, 33);
		if (is_unit(dt) && !(dm & 0x2000) && ops.is_siege(D)) v = pct(v, 66);
		if (is(A, 346) && (dm & 0x2000)) v = pct(v, 33);
		if ((is(A, 323) || is(A, 338)) && (dm & 0x2004) == 0x2004) v = imul(v, 2);
	}
	if (is(A, 334) && is(D, 340)) v = pct(v, 310);
	if (is(A, 334) && is(D, 346)) v = ishl(v, 2);
	if (is(A, 334) && is(D, 334)) v = ishl(v, 2);
	if (is(A, 340) && is(D, 323)) v = pct(v, 160);
	if (is_unit(at) && !(am & 0x2000)) {
		if (!(am & 0x40000) && (dm & 0x2000)) v = pct(v, 33);
		if (ops.is_siege(A) && (dm & 0x2000)) v = pct(v, 350);
	}
	if ((is(A, 304) || is(A, 308)) && is_build(dt) && dm < 0) v = pct(v, 25);
	if ((am & 0x10000000) && !ops.is_missile(A) && am < 0) {
		if (is_build(dt) && dm >= 0) v = pct(v, 15);
		if (is_build(dt) && dm < 0) v = pct(v, 85);
		if (is(D, 304)) v = imul(v, 2);
		if (ops.is_siege(D)) v = pct(v, 125);
		if (dm & 0x2000) v = pct(v, 350);
	}
	if (is(A, 310)) {
		if (is(D, 334)) v = ishl(v, 3);
		if (is(D, 239)) v = pct(v, 450);
	}
	if (is_unit(at) && am >= 0 && is(D, 310)) v = pct(v, 25);
	if (is(A, 313) && is_build(dt) && is_wonder(dt)) v = v / 2;
	if (is(D, 63)) {
		if (am & 0x400000) v = pct(v, 180);
		if (is_build(at)) v = imul(v, 14) / 5;
	}
	const int32_t siege_bit = am & 0x40000;
	if (siege_bit && is_build(dt)) v = pct(v, 430);
	if ((am & 2) && is_build(dt)) v = pct(v, 250);
	if (is(D, 447) && (A.domain != 0 || siege_bit)) v = v / 3;
	if (is(A, 88) && (dm & 0x10000)) v = pct(v, 150);
	if (is(A, 174)) {
		if (dm & 0x400) v = pct(v, 125);
		if (dm & 0x4000) v = pct(v, 125);
	}
	if (is(A, 91)) {
		if (dm & 0x1000000) v = pct(v, 115);
		if (dm & 0x20000000) v = pct(v, 115);
		if (dm & 0x4000) v = pct(v, 115);
	}
	if (am & 0x4000) {
		if (is(A, 191)) {
			if (dm & 0x10000) v = pct(v, 135);
			if (dm & 8) v = pct(v, 120);
		}
		if (is(A, 195) && (dm & 0x4000)) v = pct(v, 150);
		if (is(A, 199)) {
			if (dm & 0x10000) v = pct(v, 160);
			if (dm & 8) v = pct(v, 145);
		}
	}
	if ((am & 0x20000000) && is(A, 224)) v = pct(v, 105);
	if (am & 0x400) {
		if (is(A, 177, true) && (dm & 0x80)) v = pct(v, 130);
		if (is(A, 178, true) && (dm & 0x80)) v = pct(v, 130);
		if (is(A, 179, true) && (dm & 0x80)) v = pct(v, 130);
		if (is(A, 174, true) && (dm & 0x400)) v = pct(v, 135);
	}
	if ((am & 0x1000000) && is(A, 232)) v = pct(v, 105);
	if (am & 0x80) {
		if (is(A, 146) && (dm & 0x80)) v = imul(v, 2);
		if (is(A, 160, true)) v = pct(v, 50);
		if (is(A, 149)) {
			if (dm & 0x20000000) v = pct(v, 180);
			if (dm & 0x1000000) v = pct(v, 180);
			if (dm & 0x4000) v = pct(v, 110);
		}
		if (is(A, 151)) v = pct(v, 130);
		if (is(A, 144) && (dm & 0x200000)) v = pct(v, 135);
	}
	if (is(A, 114) && (dm & 0x20)) v = pct(v, 130);
	if (is(A, 130, true) && (dm & 0x20)) v = pct(v, 130);
	for (int t : {258, 259}) {
		if (is(A, t, true)) {
			if (dm & 0x10108) v = pct(v, 115);
			if (is(D, 123)) v = pct(v, 120);
		}
	}
	if (!siege_bit) return v;
	if (is(A, 279) && is_build(dt)) v = pct(v, 135);
	if (is(A, 276) && is_build(dt)) v = pct(v, 110);
	if (is(A, 274) && is_build(dt)) v = pct(v, 110);
	return v;
}

// Balance::compute_modifier @00581cc0.
int32_t compute_modifier(const Type& A, const Type& D, const std::vector<int16_t>& raw, const Ops& ops,
                         const Constants& K) {
	if (is_gaia(A.type) || is_gaia(D.type)) return 100;
	const std::vector<int> rows = pack_indices(A, ops), cols = pack_indices(D, ops);
	int32_t m = 100;
	for (int row : rows) {
		for (int c : cols) m = pct(raw[size_t(row) * kRaw + size_t(c)], m);
	}
	return pct(type_damage(A, D, ops, K), m);
}

}  // namespace

// Balance::fill_tables, second half.
void compute_balance(Loader& L, const std::vector<int32_t>& ages) {
	const std::vector<int16_t> raw = read_raw_table(L);
	const Ops ops{L.r, ages};
	L.r.balance.assign(size_t(kBalanceN) * kBalanceN, 0);
	for (int a = kUnits; a < kEndBuilds; ++a) {
		for (int d = kUnits; d < kEndBuilds; ++d) {
			L.r.balance[size_t(a - kUnits) * kBalanceN + size_t(d - kUnits)] =
			    i16(compute_modifier(L.r.types[size_t(a)], L.r.types[size_t(d)], raw, ops, L.r.constants));
		}
	}
}

}  // namespace ron::sim::detail
