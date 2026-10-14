#include "script/bhs_common.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "assets/strings.hpp"

namespace ron::bhs {

const char* tok_name(int tok) {
	static char buf[8];
	if (tok == 0) return "$end";
	if (tok > 0 && tok < 256) {
		std::snprintf(buf, sizeof buf, "'%c'", char(tok));
		return buf;
	}
	switch (tok) {
	case T_IDENTIFIER: return "IDENTIFIER";
	case T_INT_LIT: return "INT_LIT";
	case T_FLOAT_LIT: return "FLOAT_LIT";
	case T_CHAR_LIT: return "CHAR_LIT";
	case T_STRING_LIT: return "STRING_LIT";
	case T_INC_OP: return "INC_OP";
	case T_DEC_OP: return "DEC_OP";
	case T_LEFT_OP: return "LEFT_OP";
	case T_RIGHT_OP: return "RIGHT_OP";
	case T_LE_OP: return "LE_OP";
	case T_GE_OP: return "GE_OP";
	case T_EQ_OP: return "EQ_OP";
	case T_NE_OP: return "NE_OP";
	case T_POW_OP: return "POW_OP";
	case T_AND_OP: return "AND_OP";
	case T_OR_OP: return "OR_OP";
	case T_REF_ASSIGN: return "REF_ASSIGN";
	case T_MUL_ASSIGN: return "MUL_ASSIGN";
	case T_DIV_ASSIGN: return "DIV_ASSIGN";
	case T_MOD_ASSIGN: return "MOD_ASSIGN";
	case T_ADD_ASSIGN: return "ADD_ASSIGN";
	case T_SUB_ASSIGN: return "SUB_ASSIGN";
	case T_LEFT_ASSIGN: return "LEFT_ASSIGN";
	case T_RIGHT_ASSIGN: return "RIGHT_ASSIGN";
	case T_AND_ASSIGN: return "AND_ASSIGN";
	case T_XOR_ASSIGN: return "XOR_ASSIGN";
	case T_OR_ASSIGN: return "OR_ASSIGN";
	case T_POW_ASSIGN: return "POW_ASSIGN";
	case T_STATIC: return "STATIC";
	case T_REF: return "REF";
	case T_ERROR_TOKEN: return "ERROR_TOKEN";
	case T_SCRIPT_NAME: return "SCRIPT_NAME";
	case T_SCRIPT_KIND: return "SCRIPT_KIND";
	case T_VAR_NAME: return "VAR_NAME";
	case T_CONST_NAME: return "CONST_NAME";
	case T_TRIGGER_NAME: return "TRIGGER_NAME";
	case T_TYPE_NAME: return "TYPE_NAME";
	case T_VOID: return "VOID";
	case T_LOCALIZE: return "LOCALIZE";
	case T_RUN_ONCE: return "RUN_ONCE";
	case T_TRIGGER: return "TRIGGER";
	case T_ENABLE_TRIGGER: return "ENABLE_TRIGGER";
	case T_DISABLE_TRIGGER: return "DISABLE_TRIGGER";
	case T_END_INCLUDE: return "END_INCLUDE";
	case T_EVAL_END: return "EVAL_END";
	case T_LABELS: return "LABELS";
	case T_STRUCT: return "STRUCT";
	case T_ARRAY: return "ARRAY";
	case T_CASE: return "CASE";
	case T_DEFAULT: return "DEFAULT";
	case T_IF: return "IF";
	case T_ELSE: return "ELSE";
	case T_SWITCH: return "SWITCH";
	case T_WHILE: return "WHILE";
	case T_DO: return "DO";
	case T_FOR: return "FOR";
	case T_CONTINUE: return "CONTINUE";
	case T_BREAK: return "BREAK";
	case T_RETURN: return "RETURN";
	default: break;
	}
	std::snprintf(buf, sizeof buf, "0x%x", tok);
	return buf;
}

const char* op_name(int op) {
	static const char* const names[NUM_OP_CODES] = {
	    "ASSIGN", "EQ_OP", "NE_OP", "ADD_ASSIGN", "ADD", "LESS", "GREA", "LE_OP", "GE_OP", "UNA_NOT", "AND_OP", "OR_OP",
	    "MUL_ASSIGN", "DIV_ASSIGN", "SUB_ASSIGN", "MUL", "DIV", "SUBT", "INC_OP", "DEC_OP", "INC_OP_POST",
	    "DEC_OP_POST", "UNA_NEGA", "POW_OP", "POW_ASSIGN", "MOD_ASSIGN", "LEFT_ASSIGN", "RIGHT_ASSIGN", "AND_ASSIGN",
	    "XOR_ASSIGN", "OR_ASSIGN", "MOD", "LEFT_OP", "RIGHT_OP", "AND_BIT", "XOR_BIT", "OR_BIT", "UNA_TILD", "PUSH",
	    "POP", "CREATE_SIMPLE", "CREATE_ARRAY", "CREATE_ARRAY_DYN", "CREATE_ARRAY_INITER", "CREATE_STRUCT",
	    "PUSH_ARRAY_INDEX", "CREATE_ARRAY_INDEX", "PUSH_STRUCT_FIELD", "PUSH_ARRAY_LENGTH", "SET_ARRAY_LENGTH",
	    "INIT_COPY", "INIT", "CAST", "CAST_BOOL", "CALL", "CALL_INCLUDE", "CALL_GAME", "CALL_GAME_VARIED", "CASE",
	    "BREAK", "BIT_SET", "BIT_UNSET", "RETURN", "JUMP", "JUMP_IF", "JUMP_IF_NOT", "JUMP_IF_SC_TRUE",
	    "JUMP_IF_SC_FALSE", "JUMP_IF_INITED", "JUMP_IF_BITSET", "MARKER", "SCRIPT_MARKER", "ERROR_TOKEN"};
	return op >= 0 && op < NUM_OP_CODES ? names[op] : "?";
}

int op_operand_count(int op) {
	switch (op) {
	case OP_PUSH: case OP_CREATE_SIMPLE: case OP_CREATE_ARRAY: case OP_CREATE_ARRAY_DYN: case OP_PUSH_STRUCT_FIELD:
	case OP_INIT_COPY: case OP_INIT: case OP_CAST: case OP_CALL: case OP_CALL_GAME: case OP_BIT_SET: case OP_BIT_UNSET:
	case OP_JUMP: case OP_JUMP_IF: case OP_JUMP_IF_NOT: case OP_JUMP_IF_SC_TRUE: case OP_JUMP_IF_SC_FALSE:
	case OP_SCRIPT_MARKER:
		return 1;
	case OP_CREATE_ARRAY_INITER: case OP_CREATE_STRUCT: case OP_CALL_INCLUDE: case OP_CALL_GAME_VARIED: case OP_CASE:
	case OP_JUMP_IF_INITED: case OP_JUMP_IF_BITSET:
		return 2;
	default:
		return 0;
	}
}

uint32_t type_hash(const U16& unique_token) { return string_hash16(unique_token).hash_i; }

static inline char16_t fold(char16_t c) { return (c >= 'A' && c <= 'Z') ? char16_t(c + 32) : c; }

int wcsicmp_c(const U16& a, const U16& b) {
	const size_t n = a.size() < b.size() ? a.size() : b.size();
	for (size_t i = 0; i < n; i++) {
		const int ca = fold(a[i]), cb = fold(b[i]);
		if (ca != cb) return ca - cb;
	}
	if (a.size() == b.size()) return 0;
	return a.size() < b.size() ? -int(fold(b[n])) : int(fold(a[n]));
}

U16 to_u16(const std::string& s) {
	U16 out;
	out.reserve(s.size());
	for (unsigned char c : s) out += char16_t(c);
	return out;
}

std::string to_utf8(const U16& s) {
	std::string out;
	for (char16_t c : s) {
		if (c < 0x80) {
			out += char(c);
		} else if (c < 0x800) {
			out += char(0xC0 | (c >> 6));
			out += char(0x80 | (c & 0x3F));
		} else {
			out += char(0xE0 | (c >> 12));
			out += char(0x80 | ((c >> 6) & 0x3F));
			out += char(0x80 | (c & 0x3F));
		}
	}
	return out;
}

// _wtoi as the UCRT implements it (strtol semantics on a 32-bit long: leading blanks, sign, decimal digits, clamped).
static int32_t wtoi(const U16& s, size_t i) {
	while (i < s.size() && (s[i] == ' ' || (s[i] >= 9 && s[i] <= 13))) i++;
	bool neg = false;
	if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
	int64_t v = 0;
	bool over = false;
	for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
		v = v * 10 + (s[i] - '0');
		if (v > int64_t(0x80000000)) over = true, v = int64_t(0x80000000);
	}
	if (neg) v = -v;
	if (over || v > INT32_MAX) return neg ? INT32_MIN : INT32_MAX;
	if (v < INT32_MIN) return INT32_MIN;
	return int32_t(v);
}

int32_t convert_int(const U16& s) {
	if (s.empty()) return 0;
	if (s[0] == '0' && s.size() > 1) {
		switch (s[1]) {
		case 'B': case 'b': {  // String::binary
			uint32_t v = 0;
			for (size_t i = 2; i < s.size() && (s[i] == '0' || s[i] == '1'); i++) v = v * 2 + uint32_t(s[i] - '0');
			return int32_t(v);
		}
		case 'X': case 'x': {  // String::hex
			uint32_t v = 0;
			for (size_t i = 2; i < s.size(); i++) {
				const char16_t c = s[i];
				uint32_t d;
				if (c >= '0' && c <= '9') d = c - '0';
				else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
				else break;
				v = v * 16 + d;
			}
			return int32_t(v);
		}
		case 'D': case 'd': return wtoi(s, 2);
		default: break;
		}
	}
	return wtoi(s, 0);
}

double get_double(const U16& s) {
	std::string a;
	for (char16_t c : s) a += (c < 0x80) ? char(c) : '?';
	return std::strtod(a.c_str(), nullptr);
}

// _gcvt_s(buf, 0x400, v, 1014) (UCRT): Fortran G rule on the decimal exponent, exact digits, trailing zeros after the
// decimal point stripped (the point itself stays: 2.0 -> "2."). The exponent form keeps the legacy three-digit
// exponent (TODO(script-spec 10.2): confirm with strings from a save).
U16 gcvt_text(double v) {
	const int precision = 1014;
	std::vector<char> buf(1400);
	int decpt = 0;
	if (v != 0 && std::isfinite(v)) {
		std::snprintf(buf.data(), buf.size(), "%.*e", precision - 1, v);
		const char* e = std::strchr(buf.data(), 'e');
		decpt = e ? std::atoi(e + 1) + 1 : 0;
	}
	const int magnitude = decpt - 1;
	std::string out;
	if (!std::isfinite(v)) {
		std::snprintf(buf.data(), buf.size(), "%g", v);
		out = buf.data();
	} else if (magnitude < -1 || magnitude > precision - 1) {
		std::snprintf(buf.data(), buf.size(), "%.*e", precision - 1, v);
		out = buf.data();
		const size_t e = out.find('e');
		if (e != std::string::npos) {
			std::string mant = out.substr(0, e), ex = out.substr(e + 1);
			const char sign = (ex[0] == '-' || ex[0] == '+') ? ex[0] : '+';
			std::string digits = (ex[0] == '-' || ex[0] == '+') ? ex.substr(1) : ex;
			while (digits.size() < 3) digits = "0" + digits;
			out = mant + "e" + sign + digits;
		}
	} else {
		std::snprintf(buf.data(), buf.size(), "%.*f", precision - decpt, v);
		out = buf.data();
	}
	const size_t dot = out.find('.');
	if (dot != std::string::npos && dot + 1 < out.size()) {
		size_t e = out.find('e', dot);
		if (e == std::string::npos) e = out.size();
		size_t z = e;
		while (z > dot + 1 && out[z - 1] == '0') z--;
		out = out.substr(0, z) + out.substr(e);
	}
	return to_u16(out);
}

U16 itow_text(int32_t v) { return to_u16(std::to_string(v)); }

}  // namespace ron::bhs
