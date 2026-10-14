// rules.xml CONSTANTS -> struct Constants (Constants::init @00569a90; docs/rules-spec.md section 12;
// port of re/tools/rules/constants.mjs).
#include <cstring>
#include <string>

#include "sim/rules_internal.hpp"

namespace ron::sim {

namespace {

// item:           <NAME value=".."/> through get_attrib_num("value", -1)
// fraction:       String::fraction(value, mul); a missing element or attribute gives 0
// entry_int:      entry<i> attributes through convert_int, missing -> 0
// entry_fraction: entry<i> through String::fraction
// none:           never read (stays at its static zero)
enum class Parse : uint8_t { Item, Fraction, EntryInt, EntryFraction, None };
// Applied to each parsed int: mul48 (UCoord scale), mul10, marine = v - v/15 + 2.
enum class Post : uint8_t { None, Mul48, Mul10, Marine };

struct ConstantField {
	const char* name;
	uint32_t offset;
	int count;
	const char16_t* xml;
	Parse parse;
	int32_t mul;
	Post post;
};

const ConstantField kFields[] = {
#include "sim/rules_constants_table.inc"
};

int32_t post(Post p, int32_t v) {
	switch (p) {
	case Post::Mul48: return imul(v, 48);
	case Post::Mul10: return imul(v, 10);
	case Post::Marine: return iadd(v - cdiv(v, 15), 2);
	case Post::None: break;
	}
	return v;
}

}  // namespace

int constant_offset(const char* name) {
	for (const ConstantField& f : kFields) {
		if (std::strcmp(f.name, name) == 0) return int(f.offset);
	}
	return -1;
}

namespace detail {

void load_constants(Loader& L) {
	const XDoc d = read_game_xml(L.files, "rules.xml");
	const XNode* C = xml::child(d.root, u"CONSTANTS");
	Constants& out = L.r.constants;
	out.v.fill(0);
	for (const ConstantField& f : kFields) {
		// get_element(name, create = 1): a missing element behaves like an empty one.
		const XNode* el = f.xml ? xml::child(C, f.xml) : nullptr;
		for (int i = 0; i < f.count; ++i) {
			int32_t v = 0;
			switch (f.parse) {
			case Parse::Item: v = xml::get_attrib_num(el, u"value", -1); break;
			case Parse::Fraction: v = fraction(xml::get_attrib(el, u"value"), f.mul); break;
			case Parse::EntryInt:
			case Parse::EntryFraction: {
				const ustr s = xml::get_attrib(el, from_ascii(("entry" + std::to_string(i)).c_str()));
				v = f.parse == Parse::EntryInt ? (s.empty() ? 0 : convert_int(s)) : fraction(s, f.mul);
				break;
			}
			case Parse::None: v = 0; break;
			}
			const size_t slot = f.offset / 4 + size_t(i);
			if (slot < out.v.size()) out.v[slot] = post(f.post, v);
		}
	}
}

}  // namespace detail
}  // namespace ron::sim
