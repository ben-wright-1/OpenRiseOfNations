// ron-econ-check: the worked examples of docs/economy-spec.md section 10 computed by engine/sim (the formula layer of
// BuildTypeData::calc_gather, the city and leader tails of the income pipeline, Leader::calc_resource_caps and the
// per-frame stockpile arithmetic of Leader::do_gather), printed in the format of
// `node re/tools/gather-ref.mjs --examples` so the two can be diffed:
//   ./build/ron-econ-check <game dir> | diff - <(node re/tools/gather-ref.mjs --examples)
// The map scans (footprint tiles, forest W-cells, deposits, access tiles) are inputs here, as in the reference.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "sim/economy.hpp"
#include "sim/game.hpp"
#include "sim/production.hpp"
#include "sim/rules.hpp"

using namespace ron::sim;

namespace {

const char* kGood[6] = {"food", "timber", "wealth", "knowledge", "metal", "oil"};
constexpr int32_t kLand = 0, kForest = 4, kOil = 7;

int32_t KC(const Game& g, const char* name, int32_t i = 0) {
	const int off = constant_offset(name);
	return off < 0 ? 0 : g.rules->constants.at(uint32_t(off + 4 * i));
}

// Number.prototype.toFixed(2) of v / 16 (ties pick the larger magnitude, as the JS reference).
std::string fixed2(int32_t v) {
	const bool neg = v < 0;
	const int64_t a = neg ? -int64_t(v) : int64_t(v);
	const int64_t n = (2 * a * 100 + 16) / 32;  // round(a * 100 / 16), ties up
	char buf[64];
	std::snprintf(buf, sizeof buf, "%s%lld.%02lld", neg ? "-" : "", (long long)(n / 100), (long long)(n % 100));
	return buf;
}

std::string fmt(const std::array<int32_t, 6>& out) {
	std::string s;
	for (int32_t k = 0; k < 6; k++) {
		if (out[size_t(k)] == 0) continue;
		if (!s.empty()) s += ", ";
		s += std::string(kGood[k]) + " " + std::to_string(out[size_t(k)]) + " (" + fixed2(out[size_t(k)]) + "/450f)";
	}
	return s.empty() ? "0" : s;
}

// The raw land makes of 16 footprint tiles of one land (the first `rivers` on a river), as the flat path scans them.
GatherFormulaIn flat_tiles(const Game& g, int32_t land, int32_t rivers) {
	GatherFormulaIn in;
	const auto& L = g.rules->lands[size_t(land)];
	for (int32_t i = 0; i < 16; i++) {
		for (size_t m = 0; m < 4 && m < L.size(); m++) {
			if (L[m].good < 0) continue;
			in.raw[size_t(L[m].good)] += i < rivers ? L[m].num * KC(g, "river_resource_value") : L[m].num;
		}
	}
	return in;
}

// The radius path's W-cell scan: `cells` W-cells of one land (16 x num per counted make of the building's good).
GatherFormulaIn forest_cells(const Game& g, int32_t land, int32_t cells, int32_t good) {
	GatherFormulaIn in;
	const auto& L = g.rules->lands[size_t(land)];
	for (int32_t c = 0; c < cells; c++) {
		for (size_t m = 0; m < 4 && m < L.size(); m++) {
			if (L[m].good < 0 || L[m].good != good || L[m].num == 0) continue;
			in.raw[size_t(L[m].good)] += 16 * L[m].num;
		}
	}
	return in;
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
		auto gp = std::make_unique<Game>();
		Game& g = *gp;
		g.rules = &rules;
		for (Leader& l : g.leaders) l.tribe = -1;  // a plain nation: no tribe bonus
		const int32_t peasant_rate = KC(g, "peasant_rate"), oil_rate = KC(g, "oil_rate");

		std::printf("E1 farm, 16 Land tiles, 1 farmer, no granary:\n");
		GatherFormulaIn in = flat_tiles(g, kLand, 0);
		in.gatherers = 1;
		GatherCalc r = gather_flat(g, ptype::kFarm, in);
		std::printf("   per farmer %d, out %s, slots %d\n", r.per, fmt(r.out).c_str(), r.max);

		std::printf("E2 same farm, 4 river tiles, granary level 3 (100%%):\n");
		in = flat_tiles(g, kLand, 4);
		in.gatherers = 1;
		in.enh = KC(g, "granary_bonus", 2);
		r = gather_flat(g, ptype::kFarm, in);
		std::printf("   raw food %d, per = trunc256(20*%d) = 200, x200%% = %d; out %s\n", 16 + 4, peasant_rate, r.per,
		            fmt(r.out).c_str());

		std::printf("E3 oil well on 16 Oil tiles, 1 worker:\n");
		in = flat_tiles(g, kOil, 0);
		in.gatherers = 1;
		r = gather_flat(g, ptype::kOilWell, in);
		std::printf("   per = trunc256(16*%d) = %d; out %s\n", oil_rate, r.per, fmt(r.out).c_str());

		std::printf("E4 woodcutter: 5 forest W-cells claimed, no rivers, 7 woodcutters, 40 access tiles, lumber mill level 2 "
		            "(50%%):\n");
		in = forest_cells(g, kForest, 5, 1);
		in.gatherers = 7;
		in.access = 40;
		in.enh = KC(g, "lumbermill_bonus", 1);
		r = gather_radius_path(g, ptype::kWoodcutter, in);
		std::printf("   slots %d, per %d, out %s (2 of the 7 do not count)\n", r.max, r.per, fmt(r.out).c_str());

		std::printf("E5 woodcutter as French (+1 slot), same site:\n");
		in = forest_cells(g, kForest, 5, 1);
		in.gatherers = 7;
		in.access = 40;
		in.tribe = 10;
		r = gather_radius_path(g, ptype::kWoodcutter, in);
		std::printf("   slots %d, out %s\n", r.max, fmt(r.out).c_str());

		std::printf("E6 mine on a small mountain (150 tiles), 6 of 8 solid W-cells claimable, 5 miners, smelter level 1 "
		            "(50%%):\n");
		const int32_t slots = mine_slots(g, 150, 8, 6);
		in = GatherFormulaIn();
		in.slots = slots;
		in.gatherers = 5;
		in.enh = KC(g, "smelter_bonus", 0);
		r = gather_mine(g, in);
		std::printf("   slots = %d*6/8 = %d; per %d; out %s\n", KC(g, "mtn_small_gather"), slots, r.per, fmt(r.out).c_str());

		std::printf("E7 university, 4 scholars, university level 1 and 3:\n");
		in = GatherFormulaIn();
		in.gatherers = 4;
		in.univ_level = 1;
		r = gather_university(g, in);
		std::printf("   L1 per %d, out %s\n", r.per, fmt(r.out).c_str());
		in.univ_level = 3;
		r = gather_university(g, in);
		std::printf("   L3 per %d, out %s\n", r.per, fmt(r.out).c_str());

		std::printf("E8 a capital with the E1 farm (x3) + E4 woodcutter, market and university:\n");
		in = flat_tiles(g, kLand, 0);
		in.gatherers = 1;
		const GatherCalc farm = gather_flat(g, ptype::kFarm, in);
		in = forest_cells(g, kForest, 5, 1);
		in.gatherers = 5;
		in.access = 40;
		const GatherCalc wood = gather_radius_path(g, ptype::kWoodcutter, in);
		std::array<int32_t, 6> city{};
		for (int32_t k = 0; k < 6; k++) city[size_t(k)] = 3 * farm.out[size_t(k)] + wood.out[size_t(k)];
		CityTailIn ci;
		ci.market = true;
		ci.university = true;
		ci.num_buildings = 6;
		city_income_tail(g, ci, city.data());
		std::printf("   city %s\n", fmt(city).c_str());

		std::printf("E9 leader total: basic_gather 0 + that city; 2 refineries (none apply: no oil); territory 600 of 12000 "
		            "tiles, taxation level 2:\n");
		std::array<int32_t, 6> tot = city;
		LeaderTailIn li;
		li.refineries = 2;
		li.total_land = 12000;
		li.territory = 600;
		li.taxation = 2;
		leader_income_tail(g, li, tot.data());
		std::printf("   %s\n", fmt(tot).c_str());

		std::printf("E10 commerce caps at commerce level 1, plain nation, no wonders:\n");
		g.leaders[0].epoch[2] = 1;
		leader_calc_resource_caps(g, 0);
		std::printf("   caps (x16)");
		for (int32_t k = 0; k < 7; k++) std::printf("%s %d", k ? "," : "", g.leaders[0].resource_cap[size_t(k)]);
		std::printf("\n");
		const int32_t cap0 = g.leaders[0].resource_cap[0];

		std::printf("E11 do_gather over 450 frames for food rate 690 (43.125/450f), cap 1600:\n");
		GatherFrameState st;
		st.bucket = 200;
		int32_t got = 0;
		for (int32_t f = 1; f <= 450; f++) got += gather_frame(g, st, 0, 690, cap0, 0, 0, false, 0, false);
		std::printf("   gained %d (690*450/7200 = %g), leftover %d, bucket %d\n", got, 690.0 * 450 / 7200, st.leftover, st.bucket);

		std::printf("E12 same with rate 2000 against cap 1600 (capped, over_cap = 1):\n");
		GatherFrameState st2;
		got = 0;
		for (int32_t f = 1; f <= 450; f++) got += gather_frame(g, st2, 0, 2000, cap0, 0, 0, false, 0, false);
		std::printf("   gained %d (1600*450/7200 = 100), over_cap %d, income %d\n", got, st2.over_cap, st2.income);
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	return 0;
}
