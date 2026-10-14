// ron-script: the BigHugeScript compiler / VM test tool (docs/script-spec.md, docs/sim-implementation.md "Scripts").
//
//   ron-script --parse-all <game>             compile every shipped .bhs as a main file (same counts as
//                                              node re/tools/bhs-parse.mjs --all)
//   ron-script --compile-all <game>           compile all, check every image (re-read, jumps on instruction starts,
//                                              instruction counts per script); --dump DIR writes the images
//   ron-script --compile <game> <file.bhs> [-o out.bhc]   compile one file (game-relative path); images in load order
//   ron-script --disasm <game> <file.bhs>     compile and disassemble every image of the compile
//   ron-script --disasm-image <image file>    disassemble a stored image
//   ron-script --trace <game> <file.bhs>      tokens read and reductions, one per line (compare with bhs-parse.mjs)
//   ron-script --trace-all <game>             the same for every shipped file, each preceded by "FILE <path>"
//   ron-script --run <game> <file.bhs> <script> [int args...]   compile and run a script with stub builtins
//   ron-script --selftest                     VM value-rule checks
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "assets/chunk.hpp"
#include "assets/files.hpp"
#include "script/bhs_builtins.hpp"
#include "script/bhs_compiler.hpp"
#include "script/bhs_grammar.hpp"
#include "script/bhs_image.hpp"
#include "script/bhs_stdlib.hpp"
#include "script/bhs_vm.hpp"

namespace fs = std::filesystem;
using namespace ron;
using namespace ron::bhs;

namespace {

std::vector<std::string> all_scripts(const std::string& game) {
	std::vector<std::string> out;
	for (auto& e : fs::recursive_directory_iterator(game)) {
		if (!e.is_regular_file()) continue;
		std::string n = e.path().filename().string();
		std::string lower = n;
		for (char& c : lower) c = char(std::tolower((unsigned char)c));
		if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".bhs") out.push_back(e.path().string());
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::string relative_to(const std::string& game, const std::string& p) {
	std::string g = fs::path(game).lexically_normal().string();
	std::string a = fs::path(p).lexically_normal().string();
	if (!g.empty() && g.back() != '/') g += '/';
	return a.rfind(g, 0) == 0 ? a.substr(g.size()) : a;
}

struct Env {
	GameFiles files;
	explicit Env(const std::string& game) : files(game) {}
	CompileOptions options() {
		CompileOptions o;
		o.builtins = &BuiltinRegistry::standard();
		o.resolve = [this](const std::string& p) -> std::string {
			if (!p.empty() && p[0] == '/' && fs::exists(p)) return p;
			return files.resolve(p);
		};
		return o;
	}
};

std::string tok_text(int tok, const U16& v) {
	std::string s = tok_name(tok);
	if (tok == T_IDENTIFIER || tok == T_INT_LIT || tok == T_FLOAT_LIT || tok == T_CHAR_LIT || tok == T_STRING_LIT) {
		std::string j = "\"";
		for (char16_t c : v) {
			if (c == '"' || c == '\\') j += '\\', j += char(c);
			else if (c == '\n') j += "\\n";
			else if (c == '\r') j += "\\r";
			else if (c == '\t') j += "\\t";
			else if (c < 0x20 || c >= 0x7f) {
				char b[8];
				std::snprintf(b, sizeof b, "\\u%04x", unsigned(c));
				j += b;
			} else j += char(c);
		}
		s += " " + j + "\"";
	}
	return s;
}

// Data/trigger_template.bhs is the editor's "insert trigger" fragment ($NAMEn placeholders): parsed inside
// "scenario { }" with the placeholders substituted, as bhs-parse.mjs does.
std::string fragment_file(const std::string& abs) {
	Bytes b = read_file(abs);
	std::string s(b.begin(), b.end()), o = "scenario {\r\n";
	for (size_t i = 0; i < s.size();) {
		if (s[i] == '$' && i + 1 < s.size() && std::isupper((unsigned char)s[i + 1])) {
			size_t j = i + 1;
			while (j < s.size() && std::isupper((unsigned char)s[j])) j++;
			size_t k = j;
			while (k < s.size() && std::isdigit((unsigned char)s[k])) k++;
			if (k > j) {
				o += "placeholder";
				i = k;
				continue;
			}
		}
		o += s[i++];
	}
	o += "\r\n}\r\n";
	const char* tmp = std::getenv("TMPDIR");
	std::string dir = std::string(tmp ? tmp : "/tmp") + "/ron-script-frag";
	fs::create_directories(dir);
	std::string path = dir + "/frag.bhs";
	std::ofstream(path, std::ios::binary) << o;
	return path;
}

int parse_all(const std::string& game, bool verbose) {
	Env env(game);
	int failed = 0, files = 0;
	CompileStats total;
	for (const std::string& f : all_scripts(game)) {
		const std::string rel = relative_to(game, f);
		CompileResult r = compile(rel, env.options());
		if (!r.ok && rel.find("trigger_template.bhs") != std::string::npos) {
			CompileResult r2 = compile(fragment_file(f), env.options());
			if (r2.ok) {
				std::printf("ok   %s (fragment: parsed inside \"scenario { }\" with placeholders substituted)\n", rel.c_str());
				r = std::move(r2);
			}
		}
		files++;
		total.scripts += r.stats.scripts;
		total.triggers += r.stats.triggers;
		total.include_openings += r.stats.include_openings;
		if (!r.ok) {
			failed++;
			std::printf("FAIL %s\n", rel.c_str());
			for (size_t i = 0; i < r.errors.size() && i < 5; i++) std::printf("  %s\n", r.errors[i].c_str());
		} else if (verbose) {
			std::printf("ok   %s (%d scripts, %d triggers, %d includes)\n", rel.c_str(), r.stats.scripts, r.stats.triggers,
			            r.stats.include_openings);
		}
	}
	std::printf("%d files: %d parsed without error, %d failed; %d scripts, %d triggers, %d include openings\n", files,
	            files - failed, failed, total.scripts, total.triggers, total.include_openings);
	return failed ? 1 : 0;
}

void trace_file(Env& env, const std::string& rel, std::FILE* out) {
	CompileOptions o = env.options();
	o.on_token = [&](int tok, const U16& v) { std::fprintf(out, "T %s\n", tok_text(tok, v).c_str()); };
	o.on_reduce = [&](int rule) { std::fprintf(out, "R %d\n", rule); };
	o.on_include = [&](const std::string& f) { std::fprintf(out, "I %s\n", f.c_str()); };
	CompileResult r = compile(rel, o);
	for (const std::string& e : r.errors) std::fprintf(out, "E %s\n", e.c_str());
}

int compile_all(const std::string& game, const std::string& dump_dir) {
	Env env(game);
	int failed = 0, files = 0, images = 0, instructions = 0, bad_jumps = 0, reread_fail = 0;
	size_t bytes = 0;
	for (const std::string& f : all_scripts(game)) {
		const std::string rel = relative_to(game, f);
		if (rel.find("trigger_template.bhs") != std::string::npos) continue;  // a fragment, not a compilation unit
		files++;
		CompileResult r = compile(rel, env.options());
		if (!r.ok) {
			failed++;
			std::printf("FAIL %s\n", rel.c_str());
			for (size_t i = 0; i < r.errors.size() && i < 5; i++) std::printf("  %s\n", r.errors[i].c_str());
			continue;
		}
		for (const CompiledFile& cf : r.files) {
			images++;
			bytes += cf.image.size();
			ScriptImage img;
			std::string err;
			if (!read_image(cf.image, img, &err)) {
				reread_fail++;
				std::printf("BAD IMAGE %s (%s): %s\n", cf.path.c_str(), rel.c_str(), err.c_str());
				continue;
			}
			DisasmStats st;
			disassemble(img, &BuiltinRegistry::standard(), &st);
			instructions += st.instructions;
			bad_jumps += st.bad_jumps;
			if (st.bad_jumps) std::printf("BAD JUMPS %s: %d\n", cf.path.c_str(), st.bad_jumps);
		}
		if (!dump_dir.empty()) {
			const CompiledFile& mainf = r.files.back();
			std::string name = rel;
			for (char& c : name) {
				if (c == '/' || c == '\\' || c == ' ') c = '_';
			}
			std::ofstream(dump_dir + "/" + name + ".bhc", std::ios::binary)
			    .write(reinterpret_cast<const char*>(mainf.image.data()), std::streamsize(mainf.image.size()));
		}
	}
	std::printf("%d files: %d compiled, %d failed; %d images (%zu bytes), %d instructions, %d bad jump targets, "
	            "%d unreadable images\n",
	            files, files - failed, failed, images, bytes, instructions, bad_jumps, reread_fail);
	return (failed || bad_jumps || reread_fail) ? 1 : 0;
}

// ---------------------------------------------------------------- VM self-test

struct Rng {
	uint32_t seed = 12345;
	uint32_t next() { return seed = seed * 0x19660Du + 0x3C6EF35Fu; }
	int32_t get(int32_t lo, int32_t hi) {
		if (lo == hi) return lo;
		if (lo > hi) std::swap(lo, hi);
		next();
		return lo + int32_t((int64_t(seed & 0xFFFFu) * int64_t(hi - lo)) >> 16);
	}
};

StdlibHooks hooks_for(Rng& rng) {
	StdlibHooks h;
	h.rand_get = [&rng](int32_t lo, int32_t hi) { return rng.get(lo, hi); };
	h.rand_next = [&rng] { return rng.next(); };
	h.rand_set_seed = [&rng](uint32_t s) { rng.seed = s; };
	h.rand_seed = [&rng] { return rng.seed; };
	h.debug_print = [](const U16& s) { std::fputs(to_utf8(s).c_str(), stderr); };
	return h;
}

// Compiles `src` (LF line ends become CRLF) into a runtime; returns false on compile errors.
bool load_source(Runtime& rt, const std::string& src, std::vector<std::string>* errors) {
	std::string text;
	for (char c : src) {
		if (c == '\n') text += "\r\n";
		else text += c;
	}
	const char* tmp = std::getenv("TMPDIR");
	const std::string dir = std::string(tmp ? tmp : "/tmp") + "/ron-script-test";
	fs::create_directories(dir);
	const std::string path = dir + "/test.bhs";
	std::ofstream(path, std::ios::binary) << text;
	CompileOptions o;
	o.builtins = &BuiltinRegistry::standard();
	o.resolve = [](const std::string& p) -> std::string { return fs::exists(p) ? p : std::string(); };
	CompileResult r = compile(path, o);
	if (errors) *errors = r.errors;
	if (!r.ok) return false;
	for (const CompiledFile& f : r.files) rt.load_image(f.path, f.image);
	return true;
}

int selftest() {
	struct Case {
		const char* name;
		const char* src;
		const char* script;
		int runs;      // run the script this many times; the last result is checked
		int expect;    // expected int result (status kOk), or the status when expect_status
		bool expect_status;
	};
	const Case cases[] = {
	    {"int arithmetic", "int scenario t { return 7 / 2 + (-7) / 2 * 10 + 7 % 3; }", "t", 1, 3 - 30 + 1, false},
	    {"negative is false", "int scenario t { if (-1) return 1; if (0) return 2; return 3; }", "t", 1, 3, false},
	    {"case-insensitive strings", "int scenario t { string a = \"ABC\"; if (a == \"abc\") return 1; return 0; }", "t", 1, 1, false},
	    {"string order", "int scenario t { if (\"apple\" < \"Banana\") return 1; return 0; }", "t", 1, 1, false},
	    {"float to string", "int scenario t { float f = 2.0; string s = \"x\" + f; if (s == \"x2.\") return 1; return 0; }", "t", 1, 1, false},
	    {"float 0.1 exact", "int scenario t { float f = 0.5; f = f / 5; string s = \"\" + f; if (s == \"0.100000001490116119384765625\") return 1; return length(s); }", "t", 1, 1, false},
	    {"array grow", "int scenario t { int[] a = [1, 2, 3]; a[5] = 9; return a.length * 10 + a[5] + a[4]; }", "t", 1, 69, false},
	    {"array method", "int scenario t { int[] a = [1, 2, 3]; a.add(4); return a.length * 10 + find(a, 3); }", "t", 1, 42, false},
	    {"struct", "struct S { int a; int b; };\nint scenario t { S v; v.b = 5; v.a = 2; return v.a * 10 + v.b; }", "t", 1, 25, false},
	    {"loops", "int scenario t { int n = 0; for (int i = 0; i < 10; i++) n += i; int j = 0; while (j < 5) j++; do { j--; } while (j > 2); return n * 10 + j; }", "t", 1, 452, false},
	    {"switch", "int scenario t { int r = 0; for (int i = 0; i < 4; i++) { switch (i) { case 0: r += 1; break; case 1: r += 10; case 2: r += 100; break; default: r += 1000; } } return r; }", "t", 1, 1211, false},
	    {"statics", "int scenario t { static int n = 0; n++; return n; }", "t", 3, 3, false},
	    {"run_once and triggers", "int scenario t { static int r = 0; run_once { r += 1; enable_trigger(\"b\"); } trigger a (r > 0) { r += 10; } trigger b () { r += 100; } return r; }", "t", 2, 111, false},
	    {"ref parameter", "int scenario inc(ref int x) { x++; return 0; }\nint scenario t { int v = 4; inc(v); inc(v); return v; }", "t", 1, 6, false},
	    {"short circuit", "int scenario f() { static int calls = 0; calls++; return calls; }\nint scenario t { int r = 0; if (0 && f()) r = 1; if (1 || f()) r += 2; return r * 10 + f(); }", "t", 1, 21, false},
	    {"power", "int scenario t { return 2 ^ 10; }", "t", 1, 1024, false},
	    {"labels", "labels { A, B = 5, C }\nint scenario t { return A * 100 + B * 10 + C; }", "t", 1, 156, false},
	    {"constant pooling", "int scenario t { string u = \"Hello\"; string v = \"hello\"; return char_at(v, 0); }", "t", 1, 'H', false},
	    {"parse", "int scenario t { string s = parse(\"$NUM0 and $STRING1\", 5, \"x\"); if (s == \"5 and x\") return 1; return 0; }", "t", 1, 1, false},
	    {"implicit declarations", "int scenario t { x = 5; y = x * 3; return y; }", "t", 1, 15, false},
	    {"division by zero", "int scenario t { static int before = 0; before = 7; int z = 0; return 1 / z; }", "t", 1, Runtime::kError, true},
	    {"error keeps side effects", "int scenario t { static int before = 0; before++; int z = 0; if (before > 1) return before; return 1 / z; }", "t", 2, 2, false},
	    {"instruction limit", "int scenario t { while (1) { } return 0; }", "t", 1, Runtime::kError, true},
	    {"rand_int draws", "int scenario t { rand_seed(1); int a = rand_int(0, 100); int b = rand_int(5, 5); return a * 1000 + b; }", "t", 1, -1, false},
	    {"string concat int", "int scenario t { string s = \"n=\" + 42; if (s == \"n=42\") return 1; return 0; }", "t", 1, 1, false},
	    {"nested arrays", "int scenario t { int[][] m; m[2][3] = 7; return m.length * 10 + m[2].length + m[2][3] * 100; }", "t", 1, 734, false},
	};
	int bad = 0;
	for (const Case& c : cases) {
		Runtime rt;
		Rng rng;
		install_stdlib(rt, hooks_for(rng));
		std::vector<std::string> errs;
		std::string log;
		rt.log = [&log](const std::string& m) { log += m + "; "; };
		if (!load_source(rt, c.src, &errs)) {
			std::printf("FAIL %-28s compile: %s\n", c.name, errs.empty() ? "?" : errs[0].c_str());
			bad++;
			continue;
		}
		int status = 0;
		for (int k = 0; k < c.runs; k++) status = rt.run_script(to_u16(c.script), {});
		int got = c.expect_status ? status : (status == Runtime::kOk ? rt.ret_int() : -999999);
		int want = c.expect;
		if (std::string(c.name) == "rand_int draws") {  // seed 1: one draw for (0,100), none for (5,5)
			uint32_t s = 1u * 0x19660Du + 0x3C6EF35Fu;
			want = int32_t(((int64_t(s & 0xFFFFu) * 100) >> 16)) * 1000 + 5;
		}
		const bool ok = got == want;
		if (!ok) bad++;
		std::printf("%s %-28s got %d want %d%s%s\n", ok ? "ok  " : "FAIL", c.name, got, want, log.empty() ? "" : "  | ",
		            log.c_str());
	}
	std::printf("%s\n", bad ? "selftest FAILED" : "selftest OK");
	return bad ? 1 : 0;
}

int run_file(const std::string& game, const std::string& file, const std::string& script, const std::vector<std::string>& args) {
	Env env(game);
	CompileResult r = compile(file, env.options());
	for (const std::string& e : r.errors) std::printf("error: %s\n", e.c_str());
	if (!r.ok) return 1;
	Runtime rt;
	Rng rng;
	install_stdlib(rt, hooks_for(rng));
	rt.log = [](const std::string& m) { std::printf("%s\n", m.c_str()); };
	for (const CompiledFile& f : r.files) rt.load_image(f.path, f.image);
	std::vector<VP> a;
	for (const std::string& s : args) a.push_back(make_int(std::atoi(s.c_str())));
	const int st = rt.run_script(to_u16(script), a);
	std::printf("status %d, return %s, %u instructions\n", st,
	            rt.return_value ? to_utf8(value_string(rt.return_value.get())).c_str() : "(none)", rt.bytecodes_executed);
	return st;
}

// Runs every parameterless script of every shipped file three times with stub builtins (scenario-like) and the AI
// production scripts with (who, step, boom_vs_rush, 5): no VM error may occur except through stubbed builtins.
int run_all(const std::string& game) {
	Env env(game);
	int runs = 0, errors = 0, files = 0;
	uint64_t instructions = 0;
	std::map<std::string, int> messages;
	for (const std::string& f : all_scripts(game)) {
		const std::string rel = relative_to(game, f);
		if (rel.find("trigger_template.bhs") != std::string::npos) continue;
		CompileResult r = compile(rel, env.options());
		if (!r.ok) continue;
		files++;
		Runtime rt;
		Rng rng;
		install_stdlib(rt, hooks_for(rng));
		std::string last;
		rt.log = [&](const std::string& m) {
			if (m.rfind("unported", 0) != 0) last = m;
		};
		for (const CompiledFile& cf : r.files) rt.load_image(cf.path, cf.image);
		RtFile* mainf = rt.files.back().get();
		std::vector<RtScript> scripts = mainf->scripts;
		for (const RtScript& s : scripts) {
			std::vector<VP> args;
			bool ok = true;
			for (size_t k = 0; k < s.params.size(); k++) {
				if (s.params[k] == TY_INT) args.push_back(make_int(k == 0 ? 1 : (k == 2 ? 2 : (k == 3 ? 5 : 0)), s.refs[k] ? SC_VAR : SC_TEMP));
				else if (s.params[k] == TY_STRING) args.push_back(make_string(u"Romans"));
				else ok = false;
			}
			if (!ok) continue;
			for (int k = 0; k < 3; k++) {
				last.clear();
				const int st = rt.run_script(s.name, args);
				runs++;
				instructions += rt.bytecodes_executed;
				if (st != Runtime::kOk) {
					errors++;
					messages[rel + ": " + to_utf8(s.name) + ": " + last]++;
					break;
				}
			}
		}
	}
	for (auto& [m, n] : messages) std::printf("ERROR %s\n", m.c_str());
	std::printf("%d files, %d runs, %d failed, %llu instructions\n", files, runs, errors, (unsigned long long)instructions);
	return 0;
}

int usage() {
	std::fprintf(stderr,
	             "usage: ron-script --parse-all <game> | --compile-all <game> [--dump DIR] | --compile <game> <file> [-o out] |\n"
	             "       --disasm <game> <file> | --disasm-image <image> | --trace <game> <file> | --trace-all <game>\n");
	return 2;
}

}  // namespace

int main(int argc, char** argv) {
	std::vector<std::string> a(argv + 1, argv + argc);
	if (a.empty()) return usage();
	try {
		const std::string mode = a[0];
		auto opt = [&](const std::string& k) -> std::string {
			for (size_t i = 0; i + 1 < a.size(); i++) {
				if (a[i] == k) return a[i + 1];
			}
			return "";
		};
		const bool verbose = std::find(a.begin(), a.end(), "--verbose") != a.end();
		if (mode == "--parse-all" && a.size() >= 2) return parse_all(a[1], verbose);
		if (mode == "--compile-all" && a.size() >= 2) return compile_all(a[1], opt("--dump"));
		if (mode == "--trace" && a.size() >= 3) {
			Env env(a[1]);
			trace_file(env, a[2], stdout);
			return 0;
		}
		if (mode == "--trace-all" && a.size() >= 2) {
			Env env(a[1]);
			for (const std::string& f : all_scripts(a[1])) {
				const std::string rel = relative_to(a[1], f);
				std::printf("FILE %s\n", rel.c_str());
				trace_file(env, rel, stdout);
			}
			return 0;
		}
		if ((mode == "--compile" || mode == "--disasm") && a.size() >= 3) {
			Env env(a[1]);
			CompileResult r = compile(a[2], env.options());
			for (const std::string& e : r.errors) std::printf("error: %s\n", e.c_str());
			for (const std::string& w : r.warnings) std::printf("warning: %s\n", w.c_str());
			for (const CompiledFile& cf : r.files) {
				std::printf("%s %s: %zu bytes\n", cf.ok ? "image" : "FAILED", cf.path.c_str(), cf.image.size());
				if (mode == "--disasm" && cf.ok) {
					ScriptImage img;
					std::string err;
					if (!read_image(cf.image, img, &err)) {
						std::printf("  unreadable: %s\n", err.c_str());
						continue;
					}
					std::printf("; links %zu, lines %zu, scripts %zu, constants %zu, structs %zu, code %zu bytes\n",
					            img.links.size(), img.lines.size(), img.scripts.size(), img.consts.size(),
					            img.structs.size(), img.code.size());
					std::fputs(disassemble(img, &BuiltinRegistry::standard()).c_str(), stdout);
				}
			}
			const std::string out = opt("-o");
			if (!out.empty() && !r.files.empty())
				std::ofstream(out, std::ios::binary)
				    .write(reinterpret_cast<const char*>(r.files.back().image.data()), std::streamsize(r.files.back().image.size()));
			std::printf("%s: %d scripts, %d triggers, %d includes\n", r.ok ? "ok" : "FAILED", r.stats.scripts, r.stats.triggers,
			            r.stats.include_openings);
			return r.ok ? 0 : 1;
		}
		if (mode == "--disasm-image" && a.size() >= 2) {
			ScriptImage img;
			std::string err;
			if (!read_image(read_file(a[1]), img, &err)) {
				std::fprintf(stderr, "unreadable: %s\n", err.c_str());
				return 1;
			}
			std::fputs(disassemble(img, &BuiltinRegistry::standard()).c_str(), stdout);
			return 0;
		}
		if (mode == "--selftest") return selftest();
		if (mode == "--run-all" && a.size() >= 2) return run_all(a[1]);
		if (mode == "--run" && a.size() >= 4) return run_file(a[1], a[2], a[3], std::vector<std::string>(a.begin() + 4, a.end()));
		if (mode == "--grammar") {
			const ParseTables& t = parse_tables();
			std::printf("%d states, %d terminals, %d nonterminals, %d shift/reduce and %d reduce/reduce conflicts\n",
			            t.nstates, t.nterms, t.nnonterms, t.conflicts_sr, t.conflicts_rr);
			return 0;
		}
	} catch (const std::exception& e) {
		std::fprintf(stderr, "ron-script: %s\n", e.what());
		return 1;
	}
	return usage();
}
