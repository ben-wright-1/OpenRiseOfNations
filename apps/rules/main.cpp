// ron-rules: load the game's rules tables from the XML (engine/sim/rules.hpp), report counts, write the rules image
// and compare it with the image a replay recorded.
// Usage: ron-rules <game dir> [--image out.bin] [--replay file.rcx|decompressed.bin]
//                  [--start-tech N] [--end-tech N] [--game-rules N] [--flag8]
// The game options are the per-game setup finalize_grafting sees (docs/rules-spec.md section 7.6); a replay must be
// compared under its own game's setup. Exit code 1 when the replay's image differs.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "sim/rules.hpp"

namespace {

const char* arg_value(int argc, char** argv, const char* key) {
	for (int i = 2; i + 1 < argc; ++i) {
		if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
	}
	return nullptr;
}

bool has_flag(int argc, char** argv, const char* key) {
	for (int i = 2; i < argc; ++i) {
		if (std::strcmp(argv[i], key) == 0) return true;
	}
	return false;
}

int64_t read_le(const ron::Bytes& b, size_t p, size_t n) {
	uint64_t u = 0;
	for (size_t i = 0; i < n; ++i) u |= uint64_t(b[p + i]) << (8 * i);
	if (n < 8 && (u >> (8 * n - 1)) & 1) u |= ~uint64_t(0) << (8 * n);  // sign-extend
	return int64_t(u);
}

void count_types(const ron::sim::Rules& r, int counts[7]) {
	for (int i = 0; i < 7; ++i) counts[i] = 0;
	for (const ron::sim::Type& t : r.types) counts[int(t.kind)]++;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr,
		             "usage: %s <game dir> [--image out.bin] [--replay file.rcx|decompressed.bin]\n"
		             "       [--start-tech N] [--end-tech N] [--game-rules N] [--flag8]\n",
		             argv[0]);
		return 2;
	}
	try {
		const auto t0 = std::chrono::steady_clock::now();
		const ron::GameFiles files(argv[1]);
		ron::sim::GameSetup game;
		if (const char* v = arg_value(argc, argv, "--start-tech")) game.starting_technology = game.starting_technology2 = std::atoi(v);
		if (const char* v = arg_value(argc, argv, "--end-tech")) game.ending_technology = std::atoi(v);
		if (const char* v = arg_value(argc, argv, "--game-rules")) game.game_rules = std::atoi(v);
		game.flag8 = has_flag(argc, argv, "--flag8");

		ron::sim::Rules rules = ron::sim::load_rules(files);
		ron::sim::start_game(rules, game);
		std::vector<ron::sim::ImageField> layout;
		const ron::Bytes image = ron::sim::write_image(rules, &layout);
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

		int c[7];
		count_types(rules, c);
		std::printf("loaded %s: %zu types (%d goods, %d units, %d buildings, %d item, %d techs, %d crafts, %d bonuses), "
		            "%d constants, %dx%d balance, %zu tribes, %zu category lists; image %zu bytes (%.2fs)\n",
		            argv[1], rules.types.size(), c[0], c[1], c[2], c[3], c[4], c[5], c[6], ron::sim::kNumConstants,
		            ron::sim::kBalanceN, ron::sim::kBalanceN, rules.tribes.size(), rules.categories.size(), image.size(),
		            secs);
		for (const std::string& w : rules.warnings) std::printf("warning: %s\n", w.c_str());

		if (const char* out = arg_value(argc, argv, "--image")) {
			std::ofstream f(out, std::ios::binary);
			f.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
			if (!f) throw std::runtime_error(std::string("can't write ") + out);
		}

		if (const char* replay = arg_value(argc, argv, "--replay")) {
			const ron::Bytes b = ron::sim::read_replay(replay);
			ron::sim::ReplayRules rr;
			if (!ron::sim::find_image(b, rr)) throw std::runtime_error(std::string("no rules image found in ") + replay);
			const size_t theirs_len = rr.end - rr.offset;
			// The decoded replay tables (what playback uses) must re-encode to the same bytes.
			const ron::Bytes again = ron::sim::write_image(rr.rules);
			const bool round_trip = again.size() == theirs_len &&
			                        std::equal(again.begin(), again.end(), b.begin() + std::ptrdiff_t(rr.offset));
			std::printf("replay image decodes and re-encodes %s\n", round_trip ? "identically" : "DIFFERENTLY");
			size_t diff = 0;
			const size_t n = std::min(image.size(), theirs_len);
			while (diff < n && image[diff] == b[rr.offset + diff]) ++diff;
			if (diff == n && image.size() == theirs_len) {
				std::printf("replay image at 0x%zx, %zu bytes: BYTE-IDENTICAL\n", rr.offset, theirs_len);
				return round_trip ? 0 : 1;
			}
			size_t differing = 0;
			for (size_t i = 0; i < n; ++i) differing += image[i] != b[rr.offset + i];
			std::printf("replay image at 0x%zx, %zu bytes (ours %zu): differs; first at image offset 0x%zx, %zu differing "
			            "bytes in the common length\n",
			            rr.offset, theirs_len, image.size(), diff, differing);
			for (const ron::sim::ImageField& f : layout) {
				if (diff < f.offset || diff >= f.offset + f.size) continue;
				std::printf("  %s: %s", f.where.c_str(), f.field);
				if (f.index >= 0) std::printf("[%d]", f.index);
				if (f.size <= 8 && f.offset + f.size <= theirs_len) {
					std::printf(" ours %lld replay %lld", (long long)read_le(image, f.offset, f.size),
					            (long long)read_le(b, rr.offset + f.offset, f.size));
				}
				std::printf(" (field at image offset 0x%zx)\n", f.offset);
				break;
			}
			return 1;
		}
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
