#include "assets/bha.hpp"

#include <algorithm>

namespace ron {

namespace {

// Key track, chunk id 7: u32 n; n * {f32 dt_seconds, f32 quat[4] (conjugated), f32 trans[3], f32 unused}.
std::vector<AnimKey> read_track(const Bytes& b, const Chunk& c) {
	const uint32_t n = read_u32(b, c.start);
	if (n > 100000 || c.start + 4 + size_t(n) * 36 > c.end) throw FormatError("bad key track");
	std::vector<AnimKey> keys(n);
	uint32_t t = 0;
	for (uint32_t i = 0; i < n; i++) {
		const size_t p = c.start + 4 + size_t(i) * 36;
		// The game accumulates int(dt * 1000) & 0xffff into a running millisecond counter.
		t += uint32_t(int(read_f32(b, p) * 1000.0f)) & 0xffffu;
		keys[i].time_ms = t;
		keys[i].xform.q = {-read_f32(b, p + 4), -read_f32(b, p + 8), -read_f32(b, p + 12), read_f32(b, p + 16)};
		keys[i].xform.t = {read_f32(b, p + 20), read_f32(b, p + 24), read_f32(b, p + 28)};
	}
	return keys;
}

void read_node(const Bytes& b, const Chunk& c, AnimNode& node, uint32_t& duration, int depth) {
	if (depth > 256) throw FormatError("animation tree too deep");
	for (const Chunk& k : read_chunks(b, c.start, c.end)) {
		if (k.id == 7) {
			node.keys = read_track(b, k);
			if (!node.keys.empty()) duration = std::max(duration, node.keys.back().time_ms);
		} else if (k.id == 8) {
			node.children.emplace_back();
			read_node(b, k, node.children.back(), duration, depth + 1);
		}
	}
}

}  // namespace

Animation parse_bha(const Bytes& b) {
	Animation anim;
	for (const Chunk& c : read_top_level(b)) {
		if (c.id != 8) continue;
		anim.roots.emplace_back();
		read_node(b, c, anim.roots.back(), anim.duration_ms, 0);
	}
	return anim;
}

bool sample(const std::vector<AnimKey>& keys, uint32_t ms, uint32_t duration_ms, Xform& out) {
	if (keys.empty()) return false;
	if (keys.size() == 1) {
		out = keys[0].xform;
		return true;
	}
	const uint32_t t = duration_ms ? ms % duration_ms : 0;
	// First key whose time is >= t; interpolate from the key before it.
	auto it = std::lower_bound(keys.begin(), keys.end(), t, [](const AnimKey& k, uint32_t v) { return k.time_ms < v; });
	if (it == keys.end()) it = keys.end() - 1;
	const AnimKey& k2 = *it;
	const AnimKey& k1 = it == keys.begin() ? k2 : *(it - 1);
	const float span = float(k2.time_ms) - float(k1.time_ms);
	float u = span > 0 ? (float(t) - float(k1.time_ms)) / span : 0.0f;
	u = std::clamp(u, 0.0f, 1.0f);
	out.q = slerp(k1.xform.q, k2.xform.q, u);
	out.t = lerp(k1.xform.t, k2.xform.t, u);
	return true;
}

}  // namespace ron
