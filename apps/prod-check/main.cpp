// ron-prod-check: the worked examples of docs/production-spec.md section 2.6 computed by engine/sim (TypeData::get_cost
// via Type::pay_cost, ObjectData::train_time, BuildData::construct_time), printed in the format of
// `node re/tools/production-ref.mjs --examples` so the two can be diffed:
//   ./build/ron-prod-check <game dir> | diff - <(node re/tools/production-ref.mjs --examples)
// Each case builds a synthetic leader 0 (Spanish unless stated: no tribe bonus in prices or times) with the counters
// the reference sets; tech bits: every unit and building type, the age techs below the leader's age and the first
// epoch[c] techs of each library line (so has_preq of the bonus types stays false, as in the reference).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "sim/game.hpp"
#include "sim/production.hpp"
#include "sim/rules.hpp"

using namespace ron::sim;

namespace {

constexpr int32_t kSpanish = 9, kTurks = 8, kIndians = 21, kBantu = 3, kGreeks = 5, kChinese = 14, kEgyptians = 7,
                  kMaya = 1;

struct Case {
	int32_t tribe = kSpanish, ages = 0;
	int32_t epoch[4] = {0, 0, 0, 0};
	std::vector<std::pair<int32_t, int32_t>> units, buildings;  // type, count
	std::set<int32_t> unresearched;
	int32_t wonders_all = 0;
	bool pyramids = false;
	int32_t builders = 1;
};

int32_t find_type(const Rules& r, const std::string& name) {
	for (int32_t t = 50; t < 684; t++) {
		const Type& ty = r.types[size_t(t)];
		if (!ty.loaded) continue;
		std::string n = to_utf8(ty.name), w = name;
		for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
		for (char& c : w) c = char(std::tolower(static_cast<unsigned char>(c)));
		if (n == w) return t;
	}
	std::fprintf(stderr, "no type named %s\n", name.c_str());
	std::exit(2);
}

std::unique_ptr<Game> make_game(const Rules& rules, const Case& c) {
	auto g = std::make_unique<Game>();
	g->rules = &rules;
	Leader& l = g->leaders[0];
	l.flags = leaderflag::kValid | leaderflag::kActive | leaderflag::kHuman;
	l.tribe = c.tribe;
	l.age = c.ages;
	for (int32_t k = 0; k < 4; k++) l.epoch[size_t(k)] = c.epoch[k];
	l.science = c.epoch[3];
	l.city_num = 1;
	for (int32_t t = kUnits; t < kEndBuilds; t++) l.set_tech_bit(t, true);
	for (int32_t a = 0; a < c.ages; a++) l.set_tech_bit(kAges + a, true);
	const int32_t bases[4] = {ptype::kMilitaryBase, ptype::kCivic, ptype::kBarter, ptype::kWrittenWord};
	for (int32_t k = 0; k < 4; k++)
		for (int32_t e = 0; e < c.epoch[k]; e++) l.set_tech_bit(bases[k] + e, true);
	for (int32_t t : c.unresearched) l.set_tech_bit(t, false);
	for (const auto& [t, n] : c.units) track_unit_type(*g, 0, t, n, -1);
	for (const auto& [t, n] : c.buildings) l.num_buildings[size_t(t - kBuilds)] = int16_t(n);
	for (int32_t i = 0; i < c.wonders_all; i++) g->unbuilt_wonders[0].push_back({0, 0});
	if (c.pyramids) {
		// A finished Pyramids in a city: a wonder record whose building has city >= 0 (LeaderData::has_wonder).
		find_free(*g, 0, kBuildBase, kBuildLimit, kBuildBase);
		Build& b = *g->build(0, kBuildBase);
		b.is_unit = false;
		b.who = 0;
		b.o = kBuildBase;
		b.type = ptype::kPyramids;
		b.ptype = &g->type(ptype::kPyramids);
		b.flags = objflag::kValid | objflag::kActive;
		b.city = 0;
		Wonder w;
		w.o = kBuildBase;
		w.wonder_flags = 1;
		g->wonder_records[0].push_back(w);
		l.wonder_mark = 1;
	}
	return g;
}

std::string fmt_cost(const int32_t* c) {
	static const char* kRes[6] = {"food", "timber", "wealth", "knowledge", "metal", "oil"};
	std::string s;
	for (int32_t k = 0; k < 6; k++) {
		if (!c[k]) continue;
		if (!s.empty()) s += ", ";
		s += std::to_string(c[k]) + " " + kRes[k];
	}
	return s.empty() ? "0" : s;
}

void row(const Rules& rules, const std::string& label, const std::string& type_name, const Case& c) {
	const int32_t t = find_type(rules, type_name);
	auto g = make_game(rules, c);
	int32_t cost[6] = {0, 0, 0, 0, 0, 0};
	pay_cost(*g, t, 0, -1, -1, 0, cost);  // (the stockpile goes negative: only the price is read)
	std::string time, frames, extra;
	const Leader& l = g->leaders[0];
	if (is_build_type(t) && l.has_tech_bit(t)) {
		Build b;
		b.is_unit = false;
		b.who = 0;
		b.o = -1;
		b.type = t;
		b.ptype = &g->type(t);
		b.constr_time = int32_t(update_construct_time(*g, 0, t));
		const int32_t ct = construct_time(*g, b, 0);
		// Wall::do_construct: builder k of a frame adds max(1, accel_construct / k); done when job_counter >= time.
		const int32_t amount = g->rules->constants.at(0x22c);  // accel_construct
		int32_t per = 0;
		for (int32_t k = 0; k < c.builders; k++) per += std::max(1, amount / (k + 1));
		int32_t jc = 0, f = 0;
		while (jc < ct) {
			jc += per;
			f++;
		}
		time = std::to_string(ct);
		frames = std::to_string(f);
		extra = " (" + std::to_string(c.builders) + " builder" + (c.builders > 1 ? "s" : "") + ", " + std::to_string(per) + "/frame)";
	} else if (is_unit_type(t) || is_tech_type(t) || is_build_type(t)) {
		const int32_t tt = train_time(*g, -1, 0, t);
		// Build::do_queue: the done test precedes the add (accel_train per frame), so ceil(T / a) + 1 frames.
		const int32_t a = g->rules->constants.at(0x228);  // accel_train
		int32_t f = 0;
		if (tt == 1) {
			f = 1;
		} else {
			int32_t jc = 0;
			for (;;) {
				f++;
				if (jc >= tt) break;
				jc = std::min(jc + a, tt);
			}
		}
		time = std::to_string(tt);
		frames = std::to_string(f);
	}
	std::printf("| %s | %s | %s | %s | %s |\n", label.c_str(), fmt_cost(cost).c_str(), time.c_str(), frames.c_str(), extra.c_str());
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <game dir>\n", argv[0]);
		return 2;
	}
	try {
		const ron::GameFiles files(argv[1]);
		Rules rules = load_rules(files);
		start_game(rules, GameSetup());
		std::printf("| Case | Cost | train/construct time (1/100 frame) | frames | note |\n|---|---|---|---|---|\n");
		for (int32_t k : {0, 1, 4, 9, 29}) {
			Case c;
			c.units = {{50, k + 3}};
			row(rules, "Citizen, " + std::to_string(k + 3) + " citizens owned", "Citizen", c);
		}
		{
			Case c;
			c.tribe = kTurks;
			c.units = {{50, 10}};
			row(rules, "Citizen, 10 owned, Turks", "Citizen", c);
		}
		for (int32_t k : {0, 6, 7, 12}) {
			Case c;
			c.ages = 1;
			if (k) c.units = {{52, k}};
			row(rules, "Scholar, " + std::to_string(k) + " owned (Classical)", "Scholar", c);
		}
		for (int32_t k : {0, 1, 4}) {
			Case c;
			c.buildings = {{427, k}};
			row(rules, "Barracks, " + std::to_string(k) + " owned", "Barracks", c);
		}
		{
			Case c;
			c.tribe = kIndians;
			c.buildings = {{427, 4}};
			row(rules, "Barracks, 4 owned, Indians", "Barracks", c);
		}
		{
			Case c;
			c.buildings = {{417, 5}};
			row(rules, "Farm, 5 owned", "Farm", c);
		}
		{
			Case c;
			c.buildings = {{414, 1}};
			row(rules, "Small City, 1 owned", "Small City", c);
		}
		{
			Case c;
			c.tribe = kBantu;
			c.buildings = {{414, 1}};
			row(rules, "Small City, 1 owned, Bantu", "Small City", c);
		}
		{
			Case c;
			c.pyramids = true;
			c.buildings = {{414, 2}};
			row(rules, "Small City, 2 owned, Pyramids", "Small City", c);
		}
		for (int32_t k : {0, 10}) {
			Case c;
			if (k) c.units = {{132, k}};
			row(rules, "Hoplites, " + std::to_string(k) + " barracks units (Ancient)", "Hoplites", c);
		}
		{
			Case c;
			c.ages = 1;
			c.units = {{132, 10}};
			row(rules, "Hoplites, 10 barracks units, Classical", "Hoplites", c);
		}
		{
			Case c;
			c.ages = 1;
			c.epoch[0] = 2;
			c.units = {{132, 10}};
			row(rules, "Hoplites, 10 units, Classical, 2 mil. techs", "Hoplites", c);
		}
		{
			Case c;
			c.ages = 1;
			c.unresearched = {133};
			c.units = {{132, 10}};
			row(rules, "Phalanx upgrade, 10 hoplites", "Phalanx", c);
		}
		{
			Case c;
			row(rules, "Classical Age", "Classical Age", c);
		}
		{
			Case c;
			c.ages = 1;
			c.epoch[3] = 1;
			row(rules, "Medieval Age (1 science)", "Medieval Age", c);
		}
		{
			Case c;
			c.ages = 1;
			row(rules, "Medieval Age (0 science)", "Medieval Age", c);
		}
		{
			Case c;
			c.ages = 1;
			c.epoch[3] = 1;
			row(rules, "Mathematics (library, 1 science)", "Mathematics", c);
		}
		{
			Case c;
			c.ages = 1;
			c.tribe = kChinese;
			c.epoch[3] = 1;
			row(rules, "Mathematics, Chinese", "Mathematics", c);
		}
		{
			Case c;
			row(rules, "Library (building)", "Library", c);
		}
		{
			Case c;
			c.tribe = kGreeks;
			row(rules, "Library, Greeks", "Library", c);
		}
		{
			Case c;
			c.ages = 1;
			row(rules, "Colossus, first wonder", "Colossus", c);
		}
		{
			Case c;
			c.ages = 1;
			c.wonders_all = 4;
			row(rules, "Colossus, 4 wonders owned", "Colossus", c);
		}
		{
			Case c;
			c.ages = 1;
			c.tribe = kEgyptians;
			c.wonders_all = 4;
			row(rules, "Colossus, 4 wonders, Egyptians", "Colossus", c);
		}
		for (int32_t b : {1, 2, 4, 8}) {
			Case c;
			c.builders = b;
			row(rules, "Barracks construction, " + std::to_string(b) + " builder(s)", "Barracks", c);
		}
		{
			Case c;
			c.tribe = kMaya;
			c.builders = 2;
			row(rules, "Barracks construction, 2 builders, Maya", "Barracks", c);
		}
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
