// BigHugeScript compiler (docs/script-spec.md sections 1-4): scanner, LALR(1) parser with the original's symbol-table
// actions, the code generator that builds OpCode lists bottom-up as each reduction happens (SyntaxNode::eval), and the
// compiled-image writer (LexerFileEntry::write_file). One compile produces one image per file: included files first,
// each written when its COMPLETE_FILE reduces, exactly as the original loads them into ScriptFile::script_files.
//
// Port decisions (see docs/sim-implementation.md, "Scripts"):
// - The scanner is hand-written from the flex patterns (section 1.2); the parser runs LALR(1) tables that this code
//   builds at start-up from the grammar of section 2.1 (rule numbers are the original's). Nothing is read from the
//   executable and no game-derived table is embedded.
// - Localisation ($S(...), parse("...")) is the identity: the English scen_script_loc.xml text equals its key.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "assets/chunk.hpp"
#include "script/bhs_common.hpp"

namespace ron::bhs {

class BuiltinRegistry;

struct CompiledFile {
	std::string path;   // the file's input_string (game-relative path as the lexer opened it)
	Bytes image;        // chunk image (section 4.4); empty when the file had errors
	bool ok = false;
};

struct CompileOptions {
	// Resolves a game-relative path ('\' or '/', any case) to a readable file; "" if absent.
	std::function<std::string(const std::string&)> resolve;
	const BuiltinRegistry* builtins = nullptr;  // required for calls (registration order = CALL_GAME operands)
	// Debug traces (used by ron-script to compare with re/tools/bhs-parse.mjs).
	std::function<void(int tok, const U16& text)> on_token;  // every token the parser reads
	std::function<void(int rule)> on_reduce;                 // every reduction (rule number)
	std::function<void(const std::string& file)> on_include;
};

struct CompileStats {
	int scripts = 0;          // script heads reduced (rules 212-214), all files
	int triggers = 0;         // `trigger` heads (rules 178-179)
	int include_openings = 0; // include files opened
};

struct CompileResult {
	bool ok = false;                     // parse succeeded and no file had errors
	std::vector<CompiledFile> files;     // in load order (includes first, main file last)
	std::vector<std::string> errors, warnings;
	CompileStats stats;
};

// Compiles `path` (game-relative). The main script function of an unnamed `scenario { }` takes the file name.
CompileResult compile(const std::string& path, const CompileOptions& opt);

// The grammar rule names (lhs : rhs) for diagnostics.
std::string grammar_rule_text(int rule);
int grammar_rule_count();

}  // namespace ron::bhs
