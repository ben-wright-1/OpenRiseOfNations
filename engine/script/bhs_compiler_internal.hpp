// Internal data structures of the BigHugeScript compiler (engine/script/bhs_*.cpp). They mirror the original's classes
// field by field where the compiled image depends on them (SymType / SymTable / OpCode / SyntaxNode, docs/script-spec.md
// sections 2.4, 3, 4). Memory is owned by the Compiler's arenas for the duration of one compile.
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "script/bhs_builtins.hpp"
#include "script/bhs_compiler.hpp"

namespace ron::bhs {

struct SymTable;
struct OpCode;
struct FileEntry;

// One class for every SymType subclass, tagged by `cls`; the original's virtual slots become functions that switch on it.
struct SymType {
	enum class Cls { Data, Array, Struct, Var, Const, Script, Trigger, Wrap, Enum, Toggle };
	Cls cls = Cls::Data;
	SymType* of_type = nullptr;  // +4: element type (arrays), data type (variables, constants, labels -> ConstType)
	int kind = 0;                // +8 'type': a Kind or, for data types, the type id
	int index = -1;              // +0xc
	U16 token;                   // +0x10
	bool dont_delete = false;
	// VarType
	int scope_modifier = 0;
	// TriggerType: declaration line; TriggerToggle: the resolved trigger and the enable flag
	int line = 0;
	SymType* trigger = nullptr;
	bool is_enable = false;
	// StructType
	std::vector<SymType*> members;
	OpCode* code_init = nullptr;
	// LocalScriptType
	std::vector<SymType*> params, param_defaults;
	SymType* return_type = nullptr;
	int trigger_counter = 0, decl_count = 0;
	bool is_implemented = false;
	int usage = 0;  // 0 unused, 1 linked (called in its own file), 2 included
	OpCode* address = nullptr;
	FileEntry* source_file = nullptr;
	SymTable* sym_table = nullptr;
	U16 script_type;  // "scenario" / "conquest" / "ai"
	SymType* next_overload = nullptr;
};

struct SymTable {
	std::vector<SymType*> table;
	int type = 0;  // 0 root, 1 file, 2.. script and blocks, >= 0x10000 struct bodies
	int local_script_index = 0;
	FileEntry* file_entry = nullptr;
	int var_counter = 0, static_var_counter = 0, script_counter = 0;
	SymTable* parent = nullptr;
	std::vector<SymTable*> children;
};

struct OpArg {
	uint8_t kind = 0;  // 0 none, 1 symbol, 2 jump target, 3 immediate
	SymType* sym = nullptr;
	OpCode* target = nullptr;
	uint32_t imm = 0;
	bool present() const { return kind == 3 || (kind == 1 && sym) || (kind == 2 && target); }
};

// Doubly linked instruction lists, referenced by their tail; tail->first is the head (OpCode::prepend semantics).
struct OpCode {
	uint32_t count = 0;  // creation number, then the byte address (OpCode::count_code)
	uint16_t line = 0;
	int op = 0;
	OpArg a1, a2;
	OpCode* first = nullptr;
	OpCode* next = nullptr;
	OpCode* prev = nullptr;
};

struct Node {
	std::vector<Node*> children;
	SymType* sym = nullptr;
	U16 sym_string;
	int type = 0;
	OpCode* opcode = nullptr;
};

struct FileEntry {
	std::string input_string;  // path as opened (game-relative, '\' separators)
	std::string abs_path;
	std::vector<uint8_t> data;
	size_t pos = 0;
	int line_count = 1;
	std::vector<FileEntry*> local_includes;
	std::vector<U16> linked_files;
	SymTable* file_table = nullptr;
	SymTable* script_table = nullptr;
};

// Parser stack values (the original's yystype union, kept as separate fields).
struct YYVal {
	int i = 0;
	SymType* sym = nullptr;
	Node* node = nullptr;
	U16 str;
};

// Builtin seen as a call candidate.
struct Candidate {
	SymType* script = nullptr;
	const BuiltinFunc* func = nullptr;
};

class ChunkWriter;

class Compiler {
public:
	explicit Compiler(const CompileOptions& opt);
	~Compiler();
	CompileResult run(const std::string& path);

	// ---- shared state (field names follow the PDB)
	const CompileOptions& opt;
	const BuiltinRegistry* builtins = nullptr;
	CompileResult result;

	// Compiler
	int err_count = 0, total_err_count = 0, warning_count = 0;
	SymType* curr_type = nullptr;
	int curr_scope = 0;
	int last_op = 0;
	std::vector<OpCode*> cont_ops, break_ops, case_stack;
	std::vector<int> switch_num_stack;
	std::vector<SymType*> switch_type_stack;
	SymType* cur_script = nullptr;
	Node* master_root = nullptr;
	SymTable* curr_table = nullptr;
	uint32_t op_count = 0;
	uint32_t opcode_serial = 0;  // opcode_safety_list length (creation numbers)

	// Lexer
	SymTable root;
	std::vector<std::string> include_paths;
	std::vector<FileEntry*> file_stack;
	FileEntry* cur = nullptr;
	int start = 1;  // flex start condition: 1 INITIAL, 3 string, 5 comment, 7 line comment, 9 skip to EOF
	int comment_nest = 0;
	U16 crr;
	SymType *t_int = nullptr, *t_float = nullptr, *t_string = nullptr, *t_void = nullptr, *t_bool = nullptr;

	// ---- arenas
	std::deque<SymType> syms;
	std::deque<SymTable> tables;
	std::deque<OpCode> ops;
	std::deque<Node> nodes;
	std::deque<FileEntry> files;

	// ---- errors
	void comp_error(const std::string& msg, bool warning = false, int line = 0);

	// ---- lexer (bhs_lexer.cpp)
	int lex(YYVal& v);
	int resolve_token(const U16& text, YYVal& v);
	bool open_file(const std::string& name, const std::string& includer_dir);
	void close_file();
	FileEntry* new_file_entry(const std::string& input_string, const std::string& abs);

	// ---- parser (bhs_parser.cpp)
	bool parse();
	YYVal action(int rule, std::vector<YYVal>& vs, size_t base, int len);

	// ---- symbols (bhs_symbols.cpp)
	SymType* new_sym(SymType::Cls cls);
	SymTable* new_table();
	void init_root();
	SymType* find_by_token(SymTable* t, const U16& name, SymTable** found_in = nullptr);
	SymType* find_by_token_recurse_include(SymTable* t, const U16& name, SymTable** found_in);
	SymType* find_by_type(SymTable* t, int type_id);
	SymType* find_by_type_recurse_include(SymTable* t, int type_id);
	SymTable* add_child_table(SymTable* t, int flag);
	SymType* add_const_type(SymTable* t, int type, const U16& text);
	SymType* add_array_type(SymTable* t, SymType* elem);
	SymType* add_struct_type(SymTable* t, const U16& name, Node* members, SymTable* member_table);
	SymType* add_struct_type_existing(SymTable* t, SymType* st, Node* members, SymTable* member_table);
	SymType* add_enum_type(SymTable* t, SymType* prev, const U16& name, SymType* value);
	SymType* add_script_type(SymTable* t, const U16& name, SymType* kind_wrap, SymType* ret, Node* params);
	SymType* add_script_type_existing(SymTable* t, SymType* fn, SymType* kind_wrap, SymType* ret, Node* params);
	SymType* add_trigger_type(SymTable* t, const U16& name);
	SymType* add_trigger_toggle_name(SymTable* t, const U16& name, bool enable);
	SymType* add_trigger_toggle_sym(SymTable* t, SymType* trig, bool enable);
	SymType* create_var_type(const U16& name, SymType* type);
	SymType* string_wrap(const U16& s);
	void init_script(SymType* fn, const U16& name, SymType* ret, const U16& kind, Node* params);
	bool script_equals(SymType* fn, Node* params, const U16& kind);
	void init_struct(SymType* st, const U16& name, Node* members, SymTable* member_table);
	bool struct_equals(SymType* st, Node* members);

	// SymType virtual slots
	SymType* data_type(SymType* s);
	static bool is_array(const SymType* s) { return s && s->cls == SymType::Cls::Array; }
	static bool is_struct(const SymType* s) { return s && s->cls == SymType::Cls::Struct; }
	int casts_to(SymType* s, SymType* target);
	bool accepts_op(SymType* s, int op);
	bool sym_equals(SymType* a, SymType* b);
	U16 get_name(SymType* s);
	U16 unique_token(SymType* s);
	OpCode* create_opcode(SymType* s, int line);
	bool is_simple(SymType* s);
	bool is_complex(SymType* s);
	int32_t const_int(SymType* c);
	float const_float(SymType* c);

	// ---- code generation (bhs_codegen.cpp)
	OpCode* get_new(int op, OpArg a1 = {}, OpArg a2 = {});
	OpCode* get_line_new(int op, int line, OpArg a1 = {}, OpArg a2 = {});
	static void prepend(OpCode* tail, OpCode* other);
	Node* init_new_node(int type, SymType* sym, std::initializer_list<Node*> children);
	void eval(Node* n);
	void eval_binary_op(Node* n, int op, bool compare);
	void eval_unary_op(Node* n, int op);
	void eval_unary_assign_op(Node* n, int op);
	int eval_struct_direct_access(Node* n, SymType** member);
	OpCode* resolve_field_func_call(Node* n, const char* fmt, bool is_set);
	void resolve_func_call(Node* n);
	void set_op_call_to(Node* n, Candidate c, std::vector<Node*>& args, bool cast);
	int script_accepts_params(SymType* fn, std::vector<Node*>& args);
	int builtin_accepts_params(const BuiltinFunc* f, std::vector<Node*>& args);
	bool auto_cast(SymType* src, SymType* dst, OpCode** code);
	OpCode* optimize_pop(OpCode* tail);
	void set_loop_ops();
	void set_switch_ops(SymType* t);
	static OpArg sym_arg(SymType* s) { OpArg a; a.kind = 1; a.sym = s; return a; }
	static OpArg jump_arg(OpCode* t) { OpArg a; a.kind = 2; a.target = t; return a; }
	static OpArg imm_arg(uint32_t v) { OpArg a; a.kind = 3; a.imm = v; return a; }
	int lex_line() const;

	// ---- image (bhs_symbols.cpp)
	void write_file(FileEntry* fe);
	void count_code(OpCode* head, ChunkWriter& w);
	void resolve_trigger_toggle(OpCode* op);
	void write_symtable(SymTable* t, ChunkWriter& w);
	void write_sym(SymType* s, ChunkWriter& w, int script_index);
	int add_link(FileEntry* from, FileEntry* to);
};

// BHG chunk writer (ChunkWrite): u32 size including the 8-byte header, u16 id, u16 aux = number of child chunks.
class ChunkWriter {
public:
	std::vector<uint8_t> data;
	void begin(uint16_t id);
	void end();
	void u32(uint32_t v);
	void i32(int32_t v) { u32(uint32_t(v)); }
	void f32(float v);
	void u8(uint8_t v);
	void bytes(const void* p, size_t n);
	void str(const U16& s);  // String::write: i32 length + UTF-16 units
private:
	std::vector<size_t> stack_;
	void grow(size_t n);
};

}  // namespace ron::bhs
