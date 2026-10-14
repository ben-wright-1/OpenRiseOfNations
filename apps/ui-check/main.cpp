// ron-ui-check: checks the portable interface readers (engine/assets: iface_layout, strings, fonts, cursors) against
// the user's install and prints what they read, in the reference tools' formats.
//
//   ron-ui-check [game-dir] tree [NODE] [--w 1024 --h 768] [--all] [--depth N] [--file Data/interface.xml]
//        = node re/tools/ifacelayout.mjs tree ... (byte-identical output)
//   ron-ui-check [game-dir] texloads                 = node re/tools/ifacelayout.mjs texloads
//   ron-ui-check [game-dir] fonts [--w 1024]         fonts.xml choice + metrics (cf. fontpreview.mjs list)
//   ron-ui-check [game-dir] strings hash TEXT | int OFF | loc OFF | lookup TEXT [CAT] | check [--lang de]
//   ron-ui-check [game-dir] check                    resolves every window at 1024x768 and 1920x1080, loads every
//        atlas (all skins), interface.txt digits, fonts, string tables and cursors; ends with "failures: N".
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "assets/chunk.hpp"
#include "assets/cursors.hpp"
#include "assets/files.hpp"
#include "assets/fonts.hpp"
#include "assets/iface_layout.hpp"
#include "assets/strings.hpp"
#include "assets/tga.hpp"

using namespace ron;

int main(int argc, char** argv) {
	std::vector<std::string> args(argv + 1, argv + argc);
	auto opt = [&](const char* name, const std::string& def) {
		for (size_t i = 0; i + 1 < args.size(); i++) {
			if (args[i] == name) {
				std::string v = args[i + 1];
				args.erase(args.begin() + long(i), args.begin() + long(i) + 2);
				return v;
			}
		}
		return def;
	};
	auto flag = [&](const char* name) {
		for (size_t i = 0; i < args.size(); i++) {
			if (args[i] == name) {
				args.erase(args.begin() + long(i));
				return true;
			}
		}
		return false;
	};
	const int W = std::atoi(opt("--w", "1024").c_str()), H = std::atoi(opt("--h", "768").c_str());
	const int depth = std::atoi(opt("--depth", "99").c_str());
	const std::string file = opt("--file", "Data/interface.xml");
	const Language lang = parse_language(opt("--lang", "en"));
	const bool all = flag("--all");
	std::string game = std::string(getenv("HOME") ? getenv("HOME") : "") + "/Games/RiseOfNations";
	if (!args.empty() && args[0].find('/') != std::string::npos) {
		game = args[0];
		args.erase(args.begin());
	}
	const std::string cmd = args.empty() ? "check" : args[0];
	try {
		GameFiles files(game);
		if (cmd == "tree" || cmd == "texloads") {
			IfaceLayout layout(files.read_text(file));
			if (cmd == "texloads") {
				for (const auto& [id, f] : layout.texloads()) std::printf("%s\t%s\n", id.c_str(), f.c_str());
				return 0;
			}
			const XmlElement* e = args.size() > 1 ? layout.find(args[1]) : &layout.root();
			if (!e) {
				std::fprintf(stderr, "no node %s\n", args[1].c_str());
				return 1;
			}
			std::printf("# %s at %dx%d\n%s", file.c_str(), W, H, layout.dump_tree(*e, W, H, all, depth).c_str());
			return 0;
		}
		if (cmd == "fonts") {
			const FontsXml fx = load_fonts_xml(files, lang, W);
			std::printf("fonts.xml: CODEPAGE default, RESOLUTION %d (screen width %d), use_ea_line_breaking=%d\n", fx.res, W,
			            fx.ea_line_breaking ? 1 : 0);
			for (const FontEntry& f : fx.fonts) {
				const TtfFace face = parse_ttf(read_file(files.resolve(f.file)));
				const FontMetrics m = ttf_metrics(face, f.size);
				std::printf("%-8s %2d %-4s bits=0x%02x %-24s line=%d fontH=%d asc=%d desc=%d maxAdv=%d\n", f.name.c_str(), f.size,
				            f.style.empty() ? "-" : f.style.c_str(), f.bits, f.file.c_str(), m.line_height, m.font_height, m.ascent,
				            m.descent, m.max_advance);
			}
			std::printf("LOOKUPS (tag -> font at this resolution):\n");
			for (const auto& [tag, list] : fx.lookups) {
				const int i = fx.pick_tag(tag);
				std::printf("  %-24s %s\n", tag.c_str(),
				            i < 0 ? "-" : (fx.fonts[size_t(i)].name + " " + std::to_string(fx.fonts[size_t(i)].size) + " " + fx.fonts[size_t(i)].style).c_str());
			}
			for (int s : {10, 12, 14, 18}) {
				const int i = fx.pick(s, -1);
				std::printf("get(%d, any) -> %s %d %s\n", s, fx.fonts[size_t(i)].name.c_str(), fx.fonts[size_t(i)].size, fx.fonts[size_t(i)].style.c_str());
			}
			return 0;
		}
		if (cmd == "strings") {
			const std::string sub = args.size() > 1 ? args[1] : "check";
			const std::string a1 = args.size() > 2 ? args[2] : "", a2 = args.size() > 3 ? args[3] : "";
			if (sub == "hash") {
				const StringHash h = string_hash(a1);
				std::printf("{\"text\":\"%s\",\"hash\":%u,\"hash_i\":%u}\n", a1.c_str(), h.hash, h.hash_i);
				return 0;
			}
			const StringTables st(files, lang);
			if (sub == "int" || sub == "loc") {
				const uint32_t off = a1.rfind("0x", 0) == 0 ? uint32_t(std::strtoul(a1.c_str(), nullptr, 16)) : uint32_t(std::atoi(a1.c_str())) * 0x14;
				std::printf("#%u @0x%x: \"%s\"\n", off / 0x14, off, (sub == "int" ? st.internal_at_offset(off) : st.translated_at_offset(off)).c_str());
			} else if (sub == "lookup") {
				std::printf("%s\n", st.lookup(a1, a2.empty() ? "WINTITLE" : a2).c_str());
			} else {
				size_t ok = 0, bad = 0;
				for (const auto& [cat, list] : st.categories()) {
					for (const auto& e : list) (e.hash == string_hash(e.text).hash ? ok : bad)++;
				}
				std::printf("internal %zu, translated %zu, %zu categories; string_lookup.xml (English text = key): %zu matches, %zu mismatches\n",
				            st.internal().size(), st.translated().size(), st.category_count(), ok, bad);
			}
			return 0;
		}
		// check
		int failures = 0;
		IfaceLayout layout(files.read_text("Data/interface.xml"));
		for (auto [w, h] : {std::pair<int, int>{1024, 768}, {1920, 1080}}) {
			const std::string dump = layout.dump_tree(layout.root(), w, h, true);
			size_t lines = 0;
			for (char c : dump) lines += c == '\n';
			std::printf("interface.xml at %dx%d: %zu elements\n", w, h, lines);
		}
		{
			const XmlElement* base = layout.find("IFACEMAINBASE");
			const IfaceRect r = base ? resolve_iface_rect(*base, 1024, 768) : IfaceRect{};
			std::printf("IFACEMAINBASE @1024x768: (%d,%d)-(%d,%d)\n", r.l, r.t, r.r, r.b);
			if (r.l != 0 || r.t != 570 || r.r != 1024 || r.b != 768) failures++, std::printf("  FAIL: expected (0,570)-(1024,768)\n");
		}
		size_t atlases = 0;
		std::set<std::string> seen;
		auto load_tga = [&](const std::string& f) {
			if (!seen.insert(to_lower(f)).second) return;
			const std::string path = files.resolve(f);
			if (path.empty()) {
				// The trial-only art is absent from the Steam depot; report, but it is never used in game.
				std::printf("  missing atlas %s\n", f.c_str());
				if (f.find("trial") == std::string::npos) failures++;
				return;
			}
			const Image img = parse_tga(read_file(path));
			if (img.width <= 0) failures++, std::printf("  FAIL: %s did not decode\n", f.c_str());
			atlases++;
		};
		for (const auto& [id, f] : layout.texloads()) load_tga(f);
		for (const char* code : {"MD0", "AS0", "SA0", "IRO", "MD3", "NE3", "AS3", "KO3", "SA3", "TR3", "IR3", "USA", "EIN", "IND", "MOD"}) {
			load_tga(iface_skin_file(layout.texload_file("TEX_CIV"), code));
		}
		std::printf("atlases: %zu loaded (%zu IFACETEXLOAD ids, 15 TEX_CIV skins)\n", atlases, layout.texloads().size());
		const InterfaceTxt itxt = load_interface_txt(files);
		for (int s = 0; s < 2; s++) {
			for (int i = 0; i < 13; i++) {
				if (!itxt.digit[s][i].valid()) failures++, std::printf("  FAIL: interface.txt digit [%d][%d]\n", s, i);
			}
		}
		std::printf("interface.txt: digit sets 0/1 (13 pieces each), set 0 '0' = %s %d,%d %dx%d\n", itxt.digit[0][0].texture.c_str(),
		            itxt.digit[0][0].left, itxt.digit[0][0].top, itxt.digit[0][0].width, itxt.digit[0][0].height);
		for (int w : {1024, 1280}) {
			const FontsXml fx = load_fonts_xml(files, lang, w);
			for (const FontEntry& f : fx.fonts) {
				if (files.resolve(f.file).empty()) failures++, std::printf("  FAIL: font %s missing\n", f.file.c_str());
				else parse_ttf(read_file(files.resolve(f.file)));
			}
			std::printf("fonts.xml @%d: RESOLUTION %d, %zu fonts, %zu lookups\n", w, fx.res, fx.fonts.size(), fx.lookups.size());
		}
		const StringTables st(files, lang);
		size_t ok = 0, bad = 0;
		for (const auto& [cat, list] : st.categories()) {
			for (const auto& e : list) (e.hash == string_hash(e.text).hash ? ok : bad)++;
		}
		std::printf("strings: %zu internal, %zu translated, %zu lookup categories (%zu/%zu keys hash to their text)\n",
		            st.internal().size(), st.translated().size(), st.category_count(), ok, ok + bad);
		if (st.lookup("Close", "WINTITLE").empty()) failures++;
		const std::vector<CursorRow> cursors = load_cursors(files);
		for (const CursorRow& c : cursors) {
			if (files.resolve(c.file).empty()) failures++, std::printf("  FAIL: cursor %s missing\n", c.file.c_str());
		}
		std::printf("cursors: %zu rows\n", cursors.size());
		std::printf("failures: %d\n", failures);
		return failures ? 1 : 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
