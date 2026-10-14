// BigHugeScript (.bhs) shared vocabulary: token codes, symbol kinds, type ids, syntax node types and opcodes, as the
// original's PDB names them (docs/script-spec.md sections 0-4). Used by the compiler, the image reader / disassembler
// and the virtual machine.
#pragma once

#include <cstdint>
#include <string>

namespace ron::bhs {

using U16 = std::u16string;

// ---------------------------------------------------------------- tokens (Lexer::yylex return codes)
enum Tok : int {
	T_EOF = 0,
	T_IDENTIFIER = 0x101, T_INT_LIT, T_FLOAT_LIT, T_CHAR_LIT, T_STRING_LIT, T_INC_OP, T_DEC_OP, T_LEFT_OP, T_RIGHT_OP,
	T_LE_OP, T_GE_OP, T_EQ_OP, T_NE_OP, T_POW_OP, T_AND_OP, T_OR_OP, T_REF_ASSIGN, T_MUL_ASSIGN, T_DIV_ASSIGN,
	T_MOD_ASSIGN, T_ADD_ASSIGN, T_SUB_ASSIGN, T_LEFT_ASSIGN, T_RIGHT_ASSIGN, T_AND_ASSIGN, T_XOR_ASSIGN, T_OR_ASSIGN,
	T_POW_ASSIGN, T_STATIC, T_REF, T_ERROR_TOKEN, T_SCRIPT_NAME, T_SCRIPT_KIND, T_VAR_NAME, T_CONST_NAME,
	T_TRIGGER_NAME, T_TYPE_NAME, T_VOID, T_LOCALIZE,
	T_RUN_ONCE = 0x12a, T_TRIGGER, T_ENABLE_TRIGGER, T_DISABLE_TRIGGER, T_END_INCLUDE, T_EVAL_END, T_LABELS, T_STRUCT,
	T_ARRAY, T_CASE, T_DEFAULT, T_IF, T_ELSE, T_SWITCH, T_WHILE, T_DO, T_FOR,
	T_CONTINUE = 0x13c, T_BREAK, T_RETURN,
};
const char* tok_name(int tok);  // "IDENTIFIER", "';'", "$end" (the names re/tools/bhs-parse.mjs prints)

// ---------------------------------------------------------------- symbol kinds (SymType.type) and type ids
enum Kind : int {
	K_FUNC = 0x120, K_SCRIPT_KIND = 0x121, K_VAR = 0x122, K_CONST = 0x123, K_TRIGGER = 0x124, K_TOGGLE = 0x143,
	K_ENUM = 0x144, K_BUILTIN = 0x145,
};
// Type ids: the case-insensitive String hash of the type's unique token (String::generate_hash, second result).
enum TypeId : int32_t {
	TY_INT = 0x57bad, TY_FLOAT = 0x12f35f, TY_STRING = 0x168174, TY_VOID = 0x84048, TY_BOOL = 0xa6192,
	TY_ARRAY_ANY = 0x153c88,  // "array": builtin parameters accepting any array
	TY_GENERIC = 0x27b2c1,    // "generic": builtin parameters accepting any value
	TY_VARARGS = 0x64ea2a,    // the variadic parameter of `parse`
	TY_CHAR = 0x104,          // SymTable::add_const_type's char-literal selector (becomes an int constant)
};
uint32_t type_hash(const U16& unique_token);  // the type id of a unique token

// Variable scope modifiers (Compiler.curr_scope / VarType.scope_modifier).
constexpr int kScopeStatic = 0x11d, kScopeRef = 0x11e;

// ---------------------------------------------------------------- syntax nodes (SyntaxTypes; token codes reused)
enum NodeType : int {
	N_INC_OP = 0x106, N_DEC_OP = 0x107, N_LE_OP = 0x10a, N_GE_OP = 0x10b, N_EQ_OP = 0x10c, N_NE_OP = 0x10d,
	N_POW_OP = 0x10e, N_AND_OP = 0x10f, N_OR_OP = 0x110, N_REF_ASSIGN = 0x111, N_ERROR = 0x11f, N_SCRIPT_BODY = 0x120,
	N_VAR = 0x122, N_CONST = 0x123, N_TRIGGER = 0x124, N_EVAL = 0x12f, N_CASE = 0x133, N_IF = 0x135, N_SWITCH = 0x137,
	N_WHILE = 0x138, N_DO = 0x139, N_FOR = 0x13a, N_CONTINUE = 0x13c, N_BREAK = 0x13d, N_RETURN = 0x13e,
	N_NOP = 0x140, N_MASTER_ROOT, N_GENERIC_LIST, N_TRIGGER_TOGGLE, N_ENUM_CONST, N_SCRIPT_FUNC, N_ARRAY_INITER,
	N_STRUCT_INITER, N_STRUCT_ACCESS, N_STRUCT_GET, N_ARRAY_INDEX, N_INIT_DECL, N_NON_INIT_DECL, N_ARRAY_SIZED_DECL,
	N_FINISH_DECL, N_PARAM_DECL, N_PARAM_DEFAULT, N_SCRIPT_DECL, N_ASSIGN_TO_VAR, N_ASSIGN_TO_ARRAY_INDEX,
	N_ASSIGN_TO_STRUCT_FIELD, N_MUL, N_DIV, N_MOD, N_ADD, N_SUBT, N_LESS, N_GREA, N_ASSIGN, N_ABS_OP, N_AND_BIT,
	N_XOR_BIT, N_OR_BIT, N_ASSIGN_EXP, N_UNA_NEGA, N_UNA_TILD, N_UNA_NOT, N_INC_OP_POST, N_DEC_OP_POST, N_CAST,
	N_CASE_HEADER, N_COND_EXP, N_STMT, N_STMT_BLOCK, N_SCRIPT_HEADER, N_FUNC_CALL, N_COMPLETE_FILE,
};

// ---------------------------------------------------------------- opcodes (OpCodeTypes)
enum Op : int {
	OP_ASSIGN = 0, OP_EQ_OP, OP_NE_OP, OP_ADD_ASSIGN, OP_ADD, OP_LESS, OP_GREA, OP_LE_OP, OP_GE_OP, OP_UNA_NOT,
	OP_AND_OP, OP_OR_OP, OP_MUL_ASSIGN, OP_DIV_ASSIGN, OP_SUB_ASSIGN, OP_MUL, OP_DIV, OP_SUBT, OP_INC_OP, OP_DEC_OP,
	OP_INC_OP_POST, OP_DEC_OP_POST, OP_UNA_NEGA, OP_POW_OP, OP_POW_ASSIGN, OP_MOD_ASSIGN, OP_LEFT_ASSIGN,
	OP_RIGHT_ASSIGN, OP_AND_ASSIGN, OP_XOR_ASSIGN, OP_OR_ASSIGN, OP_MOD, OP_LEFT_OP, OP_RIGHT_OP, OP_AND_BIT,
	OP_XOR_BIT, OP_OR_BIT, OP_UNA_TILD, OP_PUSH, OP_POP, OP_CREATE_SIMPLE, OP_CREATE_ARRAY, OP_CREATE_ARRAY_DYN,
	OP_CREATE_ARRAY_INITER, OP_CREATE_STRUCT, OP_PUSH_ARRAY_INDEX, OP_CREATE_ARRAY_INDEX, OP_PUSH_STRUCT_FIELD,
	OP_PUSH_ARRAY_LENGTH, OP_SET_ARRAY_LENGTH, OP_INIT_COPY, OP_INIT, OP_CAST, OP_CAST_BOOL, OP_CALL, OP_CALL_INCLUDE,
	OP_CALL_GAME, OP_CALL_GAME_VARIED, OP_CASE, OP_BREAK, OP_BIT_SET, OP_BIT_UNSET, OP_RETURN, OP_JUMP, OP_JUMP_IF,
	OP_JUMP_IF_NOT, OP_JUMP_IF_SC_TRUE, OP_JUMP_IF_SC_FALSE, OP_JUMP_IF_INITED, OP_JUMP_IF_BITSET, OP_MARKER,
	OP_SCRIPT_MARKER, OP_ERROR_TOKEN, NUM_OP_CODES
};
const char* op_name(int op);
// Number of 4-byte operands an encoded instruction carries (0, 1 or 2; MARKER is never encoded).
int op_operand_count(int op);

// Operand flags in encoded instructions (OpArg::write_arg, VirtualMachine::get_value).
constexpr uint32_t kConstFlag = 0x20000000, kStaticFlag = 0x40000000;

// ---------------------------------------------------------------- strings
// Case-insensitive comparison as the original's String operators (_wcsicmp in the C locale: ASCII folding only).
int wcsicmp_c(const U16& a, const U16& b);
inline bool ieq(const U16& a, const U16& b) { return a.size() == b.size() && wcsicmp_c(a, b) == 0; }
U16 to_u16(const std::string& latin1_or_ascii);
std::string to_utf8(const U16& s);

// Numeric text conversions of the original's String class.
int32_t convert_int(const U16& s);         // String::convert_int: 0x / 0b / 0d prefixes, else _wtoi
double get_double(const U16& s);           // String::get_double: wcstod
U16 gcvt_text(double v);                   // String::operator=(double): _gcvt_s(..., 1014 digits)
U16 itow_text(int32_t v);                  // _itow decimal

}  // namespace ron::bhs
