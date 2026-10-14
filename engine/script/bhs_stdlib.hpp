// The four utility builtin sets of BigHugeScript (docs/script-spec.md section 7.6): MathUtil (0-14), TriggerUtil
// (15-17), StringUtil (18-23), ArrayUtil (24-30). The random numbers come from the host's game_random.
#pragma once

#include <cstdint>
#include <functional>

#include "script/bhs_vm.hpp"

namespace ron::bhs {

struct StdlibHooks {
	// game_random: Random::get(lo, hi) (hi exclusive, no draw when lo == hi), the raw 32-bit step, the seed.
	std::function<int32_t(int32_t, int32_t)> rand_get;
	std::function<uint32_t()> rand_next;
	std::function<void(uint32_t)> rand_set_seed;
	std::function<uint32_t()> rand_seed;
	std::function<void(const U16&)> debug_print;  // print / print_line (OutputDebugStringW only)
};

void install_stdlib(Runtime& rt, const StdlibHooks& hooks);

// String::parse with parse_ignore_num (StringUtilFuncSet::parse): "$..n" takes parse string n (the letters between
// '$' and the digit do not matter), "$$" is a '$', "$LINK<" stays, "$<...>" gender / plural forms pick their first form.
U16 parse_format(const U16& fmt, const std::vector<U16>& parse_strings);

}  // namespace ron::bhs
