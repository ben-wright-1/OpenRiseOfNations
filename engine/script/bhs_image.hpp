// Compiled BigHugeScript images (docs/script-spec.md section 4.4): the chunk reader (ScriptFile::read_script_chunk
// @9c5440 and its load_* helpers) and a disassembler for sections 4.1/4.2.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assets/chunk.hpp"
#include "script/bhs_common.hpp"

namespace ron::bhs {

class BuiltinRegistry;

struct ImageScript {
	int index = 0;
	U16 name;
	int32_t offset = 0, script_type = 0, return_type = 0, trigger_count = 0;
	std::vector<int32_t> params;     // parameter type ids
	std::vector<uint8_t> refs;       // 1 = ref parameter
	std::vector<U16> trigger_names;  // by trigger index (chunk 5)
	std::vector<U16> var_names, static_var_names;  // by variable index (chunk 8)
};

struct ImageConst {
	int32_t type = 0;  // TY_INT, TY_FLOAT, TY_STRING
	int32_t i = 0;
	float f = 0;
	U16 s;
};

struct ScriptImage {
	std::vector<U16> links;          // chunk 6: linked file names (CALL_INCLUDE link operand -> file)
	std::vector<int32_t> lines;      // chunk 7: first op address per source line (-1 none)
	std::vector<ImageScript> scripts;
	std::vector<ImageConst> consts;  // chunk 3, in pool order
	std::vector<U16> structs;        // chunk 9
	std::vector<uint8_t> code;       // chunk 4
	std::vector<int> chunk_order;    // chunk ids in file order (diagnostics)
};

// Parses an image; returns false (and sets *err) on malformed data.
bool read_image(const Bytes& image, ScriptImage& out, std::string* err = nullptr);

struct DisasmStats {
	int instructions = 0;
	std::vector<int> per_script;  // instruction count per script (by script index order of offsets)
	int bad_jumps = 0;            // jump operands that do not land on an instruction start
};

// One line per instruction: "addr  OP operands ; comment". `builtins` names CALL_GAME targets when given.
std::string disassemble(const ScriptImage& img, const BuiltinRegistry* builtins, DisasmStats* stats = nullptr);

}  // namespace ron::bhs
