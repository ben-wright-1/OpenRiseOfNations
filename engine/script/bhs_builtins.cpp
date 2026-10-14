#include "script/bhs_builtins.hpp"

#include <stdexcept>
#include <string>

namespace ron::bhs {

namespace {
struct Row {
	const char* set;
	const char* name;
	const char* ret;
	const char* params;
};
const Row kRows[] = {
#include "script/bhs_builtin_table.inc"
};

std::string trim(std::string s) {
	while (!s.empty() && s.front() == ' ') s.erase(s.begin());
	while (!s.empty() && s.back() == ' ') s.pop_back();
	return s;
}
}  // namespace

int32_t builtin_type_id(const std::string& n) {
	if (n == "int") return TY_INT;
	if (n == "float") return TY_FLOAT;
	if (n == "string") return TY_STRING;
	if (n == "void") return TY_VOID;
	if (n == "bool") return TY_BOOL;
	if (n == "array") return TY_ARRAY_ANY;
	if (n == "generic") return TY_GENERIC;
	if (n == "string[]") return TY_STRING_ARRAY;
	if (n == "UnitGroup") return TY_UNIT_GROUP;
	if (n == "ConquestDiploOffer") return TY_CONQUEST_DIPLO_OFFER;
	if (n == "...") return TY_VARARGS;
	return 0;
}

const char* type_id_name(int32_t id) {
	switch (id) {
	case TY_INT: return "int";
	case TY_FLOAT: return "float";
	case TY_STRING: return "string";
	case TY_VOID: return "void";
	case TY_BOOL: return "bool";
	case TY_ARRAY_ANY: return "array";
	case TY_GENERIC: return "generic";
	case TY_STRING_ARRAY: return "string[]";
	case TY_UNIT_GROUP: return "UnitGroup";
	case TY_CONQUEST_DIPLO_OFFER: return "ConquestDiploOffer";
	case TY_VARARGS: return "...";
	default: return "?";
	}
}

static int set_index(const std::string& s) {
	if (s == "MathUtil") return SET_MATH;
	if (s == "TriggerUtil") return SET_TRIGGER;
	if (s == "StringUtil") return SET_STRING;
	if (s == "ArrayUtil") return SET_ARRAY;
	return SET_SCENARIO;
}

const BuiltinRegistry& BuiltinRegistry::standard() {
	static const BuiltinRegistry* reg = [] {
		BuiltinRegistry* rp = new BuiltinRegistry;  // never freed: next_overload points into funcs_
		BuiltinRegistry& r = *rp;
		for (const Row& row : kRows) {
			BuiltinFunc f;
			f.index = int(r.funcs_.size());
			f.set = set_index(row.set);
			f.name8 = row.name;
			f.name = to_u16(row.name);
			f.return_type = builtin_type_id(row.ret);
			if (!f.return_type) throw std::runtime_error(std::string("bhs builtin: unknown return type ") + row.ret);
			const std::string ps = row.params;
			size_t i = 0;
			while (i < ps.size()) {
				size_t j = ps.find(';', i);
				if (j == std::string::npos) j = ps.size();
				std::string p = trim(ps.substr(i, j - i));
				i = j + 1;
				if (p.empty()) continue;
				BuiltinParam bp;
				if (p.rfind("...", 0) == 0) {  // "... params"
					bp.type = TY_VARARGS;
					bp.flags = 1;
					bp.name = to_u16(trim(p.substr(3)));
				} else {
					const size_t sp = p.find(' ');
					const std::string ty = p.substr(0, sp);
					std::string rest = sp == std::string::npos ? "" : trim(p.substr(sp + 1));
					bp.type = builtin_type_id(ty);
					if (!bp.type) throw std::runtime_error("bhs builtin: unknown parameter type " + ty);
					const size_t eq = rest.find("=<");
					if (eq != std::string::npos) {
						const size_t gt = rest.find('>', eq);
						bp.validation = to_u16(rest.substr(eq + 2, gt - eq - 2));
						rest = rest.substr(0, eq);
					}
					const size_t rf = rest.find("[ref]");
					if (rf != std::string::npos) {
						bp.flags |= 4;
						rest = rest.substr(0, rf);
					}
					bp.name = to_u16(rest);
				}
				if (bp.type == TY_VARARGS) f.flags |= 1;
				f.params.push_back(bp);
			}
			r.funcs_.push_back(std::move(f));
		}
		// ScriptFunc::check_overload: each registration links the nearest earlier same-name one to it; the last
		// registration of the whole table is never checked (so it never joins a chain).
		for (size_t k = 1; k < r.funcs_.size(); k++) {
			const size_t prev = k - 1;  // checked when function k is added
			for (size_t j = prev; j-- > 0;) {
				if (ieq(r.funcs_[j].name, r.funcs_[prev].name)) {
					r.funcs_[j].next_overload = &r.funcs_[prev];
					break;
				}
			}
		}
		return rp;
	}();
	return *reg;
}

const BuiltinFunc* BuiltinRegistry::find(const U16& name) const {
	for (const BuiltinFunc& f : funcs_) {
		if (ieq(f.name, name)) return &f;
	}
	return nullptr;
}

}  // namespace ron::bhs
