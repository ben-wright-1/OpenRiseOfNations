// ron-world: build the simulation world grid of a scenario (engine/sim/world_load.hpp) and print the same summary and
// JSON as re/tools/worlddump.mjs.
// Usage: ron-world <game dir> <file.scx> [--seed n] [--no-objects] [--json out.json]
// The .scx path is used as given when it exists, else relative to the game dir. --seed is game_random when the
// terrain chunk loads (only WData.val depends on it).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/scenario.hpp"
#include "sim/rules.hpp"
#include "sim/world_load.hpp"

namespace {

using namespace ron::sim;

const char* arg_value(int argc, char** argv, const char* key) {
	for (int i = 3; i + 1 < argc; ++i) {
		if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
	}
	return nullptr;
}

bool has_flag(int argc, char** argv, const char* key) {
	for (int i = 3; i < argc; ++i) {
		if (std::strcmp(argv[i], key) == 0) return true;
	}
	return false;
}

std::string hex(uint64_t v) {
	char b[32];
	std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
	return b;
}

template <typename F>
void json_array(std::string& o, const char* key, size_t n, F&& at) {
	o += '"';
	o += key;
	o += "\":[";
	for (size_t i = 0; i < n; i++) {
		if (i) o += ',';
		o += std::to_string(at(i));
	}
	o += ']';
}

// JSON.stringify of worlddump.mjs's object (same key order, no whitespace).
std::string to_json(const WorldBuild& b) {
	const World& w = b.world;
	const auto& d = w.wdata;
	const size_t n = d.size();
	std::string o = "{\"xs\":" + std::to_string(w.xs) + ",\"ys\":" + std::to_string(w.ys) + ",\"tile_xs\":" +
	                std::to_string(w.tile_xs) + ",\"tile_ys\":" + std::to_string(w.tile_ys) + ",\"wdata\":{";
	json_array(o, "flags", n, [&](size_t i) { return int(d[i].flags); });
	o += ',';
	json_array(o, "land", n, [&](size_t i) { return int(d[i].land); });
	o += ',';
	json_array(o, "land_sub", n, [&](size_t i) { return int(d[i].land_sub); });
	o += ',';
	json_array(o, "region", n, [&](size_t i) { return int(d[i].region); });
	o += ',';
	json_array(o, "region2", n, [&](size_t i) { return int(d[i].region2); });
	o += ',';
	json_array(o, "val", n, [&](size_t i) { return int(d[i].val); });
	o += ',';
	json_array(o, "goods", n, [&](size_t i) { return int(d[i].goods); });
	o += ',';
	json_array(o, "blocked", n, [&](size_t i) { return int(d[i].blocked); });
	o += ',';
	json_array(o, "bad", n, [&](size_t i) { return int(d[i].bad); });
	o += ',';
	json_array(o, "solid", n, [&](size_t i) { return int(d[i].solid); });
	o += ',';
	json_array(o, "was_seen", n, [&](size_t i) { return int(d[i].was_seen); });
	o += "},";
	json_array(o, "tdata", w.tdata.size(), [&](size_t i) { return int(w.tdata[i]); });
	o += ",\"regions\":[";
	bool first = true;
	for (const Region& g : b.regions.r) {
		if (!g.size) continue;
		if (!first) o += ',';
		first = false;
		o += "{\"region\":" + std::to_string(g.region) + ",\"size\":" + std::to_string(g.size) + ",\"rank\":" +
		     std::to_string(g.rank) + ",\"coast\":\"" + hex(g.coast) + "\",\"coastal\":\"" + hex(g.coastal) + "\"}";
	}
	o += "]}";
	return o;
}

// worlddump.mjs printSummary.
void print_summary(const std::string& file, const WorldBuild& b) {
	const World& w = b.world;
	auto cnt = [&](uint16_t bit) {
		int n = 0;
		for (uint16_t v : w.tdata) n += (v & bit) == bit;
		return n;
	};
	int water = 0, tree = 0, mountain = 0, cliff = 0, building = 0;
	for (uint16_t v : w.tdata) {
		if ((v & 0x30) == 0x20) water++;
		else if ((v & 0x30) == 0x30) tree++;
		if ((v & 3) == 2) mountain++;
		else if ((v & 3) == 1) cliff++;
		else if ((v & 3) == 3) building++;
	}
	std::printf("%s: %dx%d wcoords, %dx%d tcoords\n", std::filesystem::path(file).filename().string().c_str(), w.xs, w.ys,
	            w.tile_xs, w.tile_ys);
	std::printf("  tcoords: water %d, tree %d, mountain %d, cliff %d, building %d, blocked %d, bad_path %d, gather_edge %d, "
	            "perm_behind %d\n",
	            water, tree, mountain, cliff, building, cnt(tmask::kBlocked), cnt(tmask::kBadPath), cnt(tmask::kGatherEdge),
	            cnt(tmask::kPermBehind));
	std::printf("  mountain stamps placed: %zu; dead mountain spots filled: %zu; compute_all_vals RNG draws: %d\n",
	            b.mountains.size(), b.dead_mount_spots.size(), b.rng_draws);
	std::string line;
	for (const Region& g : b.regions.r) {
		if (!g.size) continue;
		if (!line.empty()) line += ", ";
		line += (g.region < 0x40 ? "L" : "S") + hex(uint64_t(g.region)) + ":" + std::to_string(g.size);
		if (g.region < 0x40 && g.coast) {
			line += " coast[";
			bool first = true;
			for (int k = 0; k < 64; k++) {
				if (!((g.coast >> k) & 1)) continue;
				if (!first) line += ',';
				first = false;
				line += hex(uint64_t(0x40 + k));
			}
			line += ']';
		}
	}
	std::printf("  regions: %s\n", line.c_str());
	int halfland = 0;
	for (const WData& d : w.wdata) halfland += (d.flags & wflag::kHalfLand) != 0;
	std::printf("  half-water wcoords: %d (region2 = adjacent sea)\n", halfland);
	if (b.has_objects) {
		const ObjectStats& o = b.objects;
		std::string unknown;
		for (const std::string& u : o.unknown) unknown += (unknown.empty() ? "" : ", ") + u;
		std::printf("  objects: %d items, %d goods (%d blocking), %d buildings + %d cities (%d re-snapped), %d units on map "
		            "covering %d ucoords%s\n",
		            o.items, o.goods, o.goods_blocking, o.builds, o.cities, o.resnapped, o.units, o.unit_cells,
		            unknown.empty() ? "" : ("; unknown types: " + unknown).c_str());
	}
	long sum_blocked = 0, sum_bad = 0;
	std::map<int, int> solid;
	for (const WData& d : w.wdata) {
		sum_blocked += d.blocked;
		sum_bad += d.bad;
		solid[d.solid]++;
	}
	std::string hist = "[";
	for (const auto& [v, c] : solid) hist += (hist.size() > 1 ? ",[" : "[") + std::to_string(v) + "," + std::to_string(c) + "]";
	hist += "]";
	std::printf("  counters: sum(blocked) %ld (= blocked tcoords %d), sum(bad) %ld (= %d), solid %s\n", sum_blocked,
	            cnt(tmask::kBlocked), sum_bad, cnt(tmask::kBadPath), hist.c_str());
	if (b.mountain_outside || (b.has_objects && b.objects.outside)) {
		std::printf("  note: %d mountain and %d good writes outside the map skipped\n", b.mountain_outside,
		            b.has_objects ? b.objects.outside : 0);
	}
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: %s <game dir> <file.scx> [--seed n] [--no-objects] [--json out.json]\n", argv[0]);
		return 2;
	}
	try {
		const ron::GameFiles files(argv[1]);
		std::string file = argv[2];
		std::error_code ec;
		if (!std::filesystem::exists(file, ec)) {
			const std::string r = files.resolve(file);
			if (r.empty()) throw std::runtime_error("scenario not found: " + file);
			file = r;
		}
		const ron::Scenario sc = ron::parse_scenario(ron::read_file(file));
		const char* seed = arg_value(argc, argv, "--seed");
		const bool objects = !has_flag(argc, argv, "--no-objects");
		Rules rules;
		if (objects) {
			rules = load_rules(files);
			start_game(rules, GameSetup());
		}
		auto b = std::make_unique<WorldBuild>();
		build_world(*b, files, sc, objects ? &rules : nullptr, seed ? uint32_t(std::strtoul(seed, nullptr, 0)) : 0u);
		print_summary(file, *b);
		if (const char* out = arg_value(argc, argv, "--json")) {
			std::ofstream f(out, std::ios::binary);
			f << to_json(*b);
			if (!f) throw std::runtime_error(std::string("cannot write ") + out);
		}
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
