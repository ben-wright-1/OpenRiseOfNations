// Shared helpers of the production modules (not part of the public API).
#pragma once

#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/placement.hpp"
#include "sim/production.hpp"

// A rules.xml constant by name, looked up once per call site (requires `g` in scope). A name missing from the table
// reads -1.
#define K(name)                                                                       \
	([&]() -> int32_t {                                                               \
		static const int off_ = ::ron::sim::constant_offset(name);                   \
		return off_ < 0 ? -1 : (g).rules->constants.at(uint32_t(off_));               \
	}())

namespace ron::sim::prod {

inline int32_t pct(int32_t p, int32_t v) { return ((100 - p) * v) / 100; }  // the common discount form
inline int32_t div100k(int32_t v, int32_t k) { return (v * 100) / (k + 100); }  // "/(k + 100)" speed-ups
inline bool is_t(const Game& g, int32_t t, int32_t x, bool strict = false) {
	return t >= 0 && t < kNumTypes && type_is(g.type(t), x, strict);
}
inline bool tb(const Game& g, int32_t who, int32_t n) { return has_tribe_bonus(g, who, n); }

}  // namespace ron::sim::prod
