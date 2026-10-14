// The BigHugeScript builtin registrations (docs/script-spec.md section 7.1): the 873 ScriptFuncs of the five function
// sets in global-index order, with their parameter type ids, flags and validation strings. The compiler binds calls
// against this table (ScriptGameInterfaceBase::find_func + ScriptFunc::accepts_params) and the VM dispatches
// CALL_GAME operands through it. Implementations are attached by the runtime host (BuiltinImpl).
#pragma once

#include <cstdint>
#include <vector>

#include "script/bhs_common.hpp"

namespace ron::bhs {

// Type ids of the builtin parameter / return types beyond the simple ones (section 7.1).
constexpr int32_t TY_STRING_ARRAY = 0x20d693;           // "@string"
constexpr int32_t TY_UNIT_GROUP = 0x139fd8d;            // "UnitGroup$int^@int^"
constexpr int32_t TY_CONQUEST_DIPLO_OFFER = 0x1d0655f3; // ConquestDiploOffer's unique token

enum BuiltinSet : int { SET_MATH = 0, SET_TRIGGER = 1, SET_STRING = 2, SET_ARRAY = 3, SET_SCENARIO = 4 };

struct BuiltinParam {
	U16 name;
	int32_t type = 0;
	uint8_t flags = 0;  // 4 = by reference (exact type, the argument must be a variable); 1 = variadic
	U16 validation;     // "active_unit_group" for the group-order overloads (ScenarioFuncSet::validate_param)
};

struct BuiltinFunc {
	int index = 0;     // global index: CALL_GAME operand
	int set = 0;       // BuiltinSet
	U16 name;
	std::string name8;
	int32_t return_type = TY_VOID;
	std::vector<BuiltinParam> params;
	uint8_t flags = 0;  // 1 = variadic (set when a parameter has the varargs type)
	const BuiltinFunc* next_overload = nullptr;  // next same-name registration (ScriptFunc::check_overload)
	bool varargs() const { return (flags & 1) != 0; }
};

class BuiltinRegistry {
public:
	// The standard table (bhs_builtin_table.inc), parsed once.
	static const BuiltinRegistry& standard();
	// First registration with this name, case-insensitive (ScriptGameInterfaceBase::find_func), or null.
	const BuiltinFunc* find(const U16& name) const;
	const BuiltinFunc* at(int index) const { return index >= 0 && index < int(funcs_.size()) ? &funcs_[size_t(index)] : nullptr; }
	int size() const { return int(funcs_.size()); }
	const std::vector<BuiltinFunc>& funcs() const { return funcs_; }

private:
	std::vector<BuiltinFunc> funcs_;
};

// Type name (as the registration table writes it) -> type id; 0 if unknown.
int32_t builtin_type_id(const std::string& name);
const char* type_id_name(int32_t id);  // "int", "string", "UnitGroup", ... ("?" if unknown)

}  // namespace ron::bhs
