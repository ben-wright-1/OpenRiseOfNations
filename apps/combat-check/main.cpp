// ron-combat-check: the damage core of engine/sim (ObjectData::get_damage and Object::do_damage's scaling,
// docs/combat-spec.md section 5) over every attacker 50..401 x target 50..542 pair, in the TSV format of
// `node re/tools/combat-ref.mjs --dump` (plain situation) or `--dump-situations` (the SITUATIONS list), so the two
// can be compared byte for byte.
//
// Usage: ron-combat-check <game dir> [--dump out.tsv] [--situations out.tsv] [--compare ref.tsv]
//   --dump        writes the plain-situation table
//   --situations  writes the flagged-situation table
//   --compare     compares the table just written with a reference file (exit 1 on the first difference)
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sim/combat.hpp"
#include "sim/rules.hpp"

namespace {

using namespace ron::sim;

const char* arg_value(int argc, char** argv, const char* key) {
	for (int i = 2; i + 1 < argc; ++i)
		if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
	return nullptr;
}

// The situations of combat-ref.mjs SITUATIONS (same names, same order).
struct Situation {
	const char* name;
	void (*apply)(DamageSituation&);
};
const Situation kSituations[] = {
    {"splash", [](DamageSituation& s) { s.splash = true; }},
    {"overkill", [](DamageSituation& s) { s.overkill = true; }},
    {"flank_side", [](DamageSituation& s) { s.has_angles = true, s.target_angle = 0, s.attack_angle = 0x40000000u; }},
    {"flank_rear",
     [](DamageSituation& s) { s.has_angles = true, s.target_angle = 0x40000000u, s.attack_angle = 0x40000000u; }},
    {"packed", [](DamageSituation& s) { s.target_packed = true; }},
    {"moving", [](DamageSituation& s) { s.target_moving = true; }},
    {"river", [](DamageSituation& s) { s.target_in_river = true; }},
    {"rocky", [](DamageSituation& s) { s.target_on_rocky = true; }},
    {"entrenched",
     [](DamageSituation& s) { s.target_entrenched = true, s.trench_angle = 0x80000000u, s.attack_angle = 0; }},
    {"decoy", [](DamageSituation& s) { s.target_decoy = true; }},
    {"height", [](DamageSituation& s) { s.height_diff = 400; }},
    {"unfinished", [](DamageSituation& s) { s.target_active = false; }},
    {"unfriendly", [](DamageSituation& s) { s.target_in_unfriendly_territory = true; }},
    {"own_land", [](DamageSituation& s) { s.target_in_attacker_territory = true; }},
};

std::string row(const Rules& r, int a, int d, const DamageSituation& base, const char* prefix, bool with_balance) {
	const Type& att = r.types[size_t(a)];
	const Type& def = r.types[size_t(d)];
	DamageSituation s = base;
	s.attack = att.attack;
	s.armor = def.armor;
	int32_t death = 0;
	const int32_t dmg = get_damage_core(r, att, def, s, &death);
	const HitScale h = scale_damage(att, true, dmg, 0x100, att.max_range != 0 && att.ammo_per_att > 0);
	std::ostringstream o;
	if (prefix) o << prefix << '\t';
	o << a << '\t' << d << '\t';
	if (with_balance) o << r.balance_at(a, d) << '\t';
	o << dmg << '\t' << death << '\t' << h.hp << '\t' << h.frac << '\n';
	return o.str();
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <game dir> [--dump out.tsv] [--situations out.tsv] [--compare ref.tsv]\n",
		             argv[0]);
		return 2;
	}
	try {
		const ron::GameFiles files(argv[1]);
		Rules rules = load_rules(files);
		start_game(rules, GameSetup());
		const char* dump = arg_value(argc, argv, "--dump");
		const char* sits = arg_value(argc, argv, "--situations");
		const char* cmp = arg_value(argc, argv, "--compare");
		std::string out;
		size_t rows = 0;
		if (sits) {
			out = "situation\tattacker\ttarget\tget_damage\tdeath\thp\tfrac\n";
			for (const Situation& st : kSituations) {
				DamageSituation s;
				st.apply(s);
				for (int a = 50; a < 402; a++)
					for (int d = 50; d < 543; d++) {
						if (d >= 402 && d < 414) continue;
						out += row(rules, a, d, s, st.name, false);
						rows++;
					}
			}
		} else {
			out = "attacker\ttarget\tbalance\tget_damage\tdeath\thp\tfrac\n";
			for (int a = 50; a < 402; a++)
				for (int d = 50; d < 543; d++) {
					if (d >= 402 && d < 414) continue;
					out += row(rules, a, d, DamageSituation(), nullptr, true);
					rows++;
				}
		}
		const char* path = sits ? sits : dump;
		if (path) {
			std::ofstream f(path, std::ios::binary);
			f << out;
			std::printf("%zu rows -> %s\n", rows, path);
		}
		if (cmp) {
			std::ifstream f(cmp, std::ios::binary);
			std::stringstream ss;
			ss << f.rdbuf();
			const std::string ref = ss.str();
			if (ref == out) {
				std::printf("compare %s: identical (%zu rows)\n", cmp, rows);
				return 0;
			}
			// First differing line.
			std::istringstream a(out), b(ref);
			std::string la, lb;
			for (size_t n = 1;; n++) {
				const bool ea = !std::getline(a, la), eb = !std::getline(b, lb);
				if (ea && eb) break;
				if (ea || eb || la != lb) {
					std::printf("compare %s: line %zu differs\n  ours: %s\n  ref:  %s\n", cmp, n, la.c_str(), lb.c_str());
					return 1;
				}
			}
			std::printf("compare %s: differs (line endings?)\n", cmp);
			return 1;
		}
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
