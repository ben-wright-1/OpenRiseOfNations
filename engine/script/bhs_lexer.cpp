// The BigHugeScript scanner (docs/script-spec.md section 1): the flex patterns of BigHugeScript.l re-expressed by hand,
// longest match first and the earlier rule on ties, with the actions of Lexer::yylex @9bc8e0 (LEXER_COMPILE), include
// files (Lexer::start_include_file @9c1960, open_file @9bff30, close_file @9bfe20) and identifier classification
// against the live symbol tables (Lexer::resolve_token @9c1460). Only "\r\n" is a line break.
#include <cstring>

#include "assets/chunk.hpp"
#include "script/bhs_compiler_internal.hpp"

namespace ron::bhs {

namespace {
bool is_ident_start(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool is_ident(int c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(int c) { return c >= '0' && c <= '9'; }
bool is_xdigit(int c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// Lexer::append_esc @9c15a0
char16_t unescape(int c) {
	switch (c) {
	case 'f': return u'\f';
	case 'n': return u'\n';
	case 'r': return u'\r';
	case 't': return u'\t';
	case 'v': return u'\v';
	default: return char16_t(c);  // \" \' \\ and any other char stand for themselves
	}
}

struct Keyword {
	const char* text;
	int tok;
};
const Keyword kKeywords[] = {
    {"break", T_BREAK},       {"case", T_CASE},         {"continue", T_CONTINUE},
    {"default", T_DEFAULT},   {"return", T_RETURN},     {"do", T_DO},
    {"else", T_ELSE},         {"for", T_FOR},           {"if", T_IF},
    {"switch", T_SWITCH},     {"while", T_WHILE},       {"static", T_STATIC},
    {"ref", T_REF},           {"struct", T_STRUCT},     {"array", T_ARRAY},
    {"Array", T_ARRAY},       {"trigger", T_TRIGGER},   {"run_once", T_RUN_ONCE},
    {"enable_trigger", T_ENABLE_TRIGGER}, {"disable_trigger", T_DISABLE_TRIGGER}, {"labels", T_LABELS},
};

std::string directory_of(const std::string& p) {
	const size_t s = p.find_last_of("\\/");
	return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}
}  // namespace

int Compiler::lex_line() const { return cur ? cur->line_count : 0; }

FileEntry* Compiler::new_file_entry(const std::string& input_string, const std::string& abs) {
	files.emplace_back();
	FileEntry* fe = &files.back();
	fe->input_string = input_string;
	fe->abs_path = abs;
	fe->data = read_file(abs);
	return fe;
}

// Lexer::open_file @9bff30: the includer's directory + name, then the name as given (content-relative), then each
// include path + name. LexerFileEntry::init_buffer @9c0b30 resets the declaration state and gives the file its table.
bool Compiler::open_file(const std::string& name, const std::string& includer_dir) {
	if (name.empty()) return false;
	std::string tried, abs;
	auto attempt = [&](const std::string& p) {
		if (!abs.empty()) return;
		const std::string a = opt.resolve(p);
		if (!a.empty()) abs = a, tried = p;
	};
	if (cur) attempt(includer_dir + name);
	attempt(name);
	for (const std::string& ip : include_paths) attempt(ip + name);
	if (abs.empty()) return false;
	FileEntry* fe = new_file_entry(tried, abs);
	// init_buffer
	fe->line_count = 1;
	total_err_count += err_count;
	err_count = 0;
	last_op = 0;
	curr_scope = 0;
	curr_type = t_int;
	master_root = nullptr;
	cur_script = nullptr;
	op_count = 0;
	SymTable* ft = new_table();
	ft->parent = &root;
	ft->type = root.type + 1;
	root.children.push_back(ft);
	fe->file_table = ft;
	ft->file_entry = fe;
	fe->script_table = nullptr;
	if (cur) cur->local_includes.push_back(fe);
	// set_curr_buffer
	if (cur) file_stack.push_back(cur);
	cur = fe;
	start = 1;
	curr_table = fe->file_table;
	if (opt.on_include) opt.on_include(tried);
	return true;
}

// Lexer::close_file @9bfe20
void Compiler::close_file() {
	if (file_stack.empty()) return;
	cur = file_stack.back();
	file_stack.pop_back();
	start = 1;
	curr_table = cur->file_table;
}

// Lexer::resolve_token @9c1460 (LEXER_COMPILE)
int Compiler::resolve_token(const U16& text, YYVal& v) {
	SymTable* found_in = nullptr;
	SymType* s = find_by_token(curr_table, text, &found_in);
	if (s) {
		const int k = s->kind;
		v.sym = s;
		if (k == K_CONST) return T_CONST_NAME;
		if (k == K_FUNC || k == K_SCRIPT_KIND || k == K_VAR || k == K_TRIGGER) {
			if (curr_table->type < 0x10000 || curr_table == found_in) return k;  // SCRIPT_NAME .. TRIGGER_NAME
			v.sym = nullptr;
			v.str = text;
			return T_IDENTIFIER;
		}
		if (k == K_ENUM) {
			v.sym = (s->of_type && s->of_type->kind == K_CONST) ? s->of_type : nullptr;
			return T_CONST_NAME;
		}
		if (k == TY_VOID) return T_VOID;
		return T_TYPE_NAME;
	}
	v.str = text;
	return T_IDENTIFIER;
}

int Compiler::lex(YYVal& v) {
	v = YYVal();
	for (;;) {
		const std::vector<uint8_t>& d = cur->data;
		size_t& p = cur->pos;
		const size_t n = d.size();
		auto at = [&](size_t i) -> int { return i < n ? d[i] : -1; };
		const bool eof = p >= n;

		if (start == 9) {  // skip to end of file after an error (Lexer::lex_change)
			if (!eof) {
				p = n;
				continue;
			}
			start = 1;
			return file_stack.empty() ? 0 : T_END_INCLUDE;
		}
		if (start == 3) {  // string
			if (eof) {
				comp_error("end of file in string constant");
				start = 1;
				v.str = crr;
				return T_STRING_LIT;
			}
			const int c = d[p];
			if (c == '\r' && at(p + 1) == '\n') {  // rule 98
				p += 2;
				cur->line_count++;
				comp_error("newline in string constant");
				start = 1;
				v.str = crr;
				return T_STRING_LIT;
			}
			if (c == '\\' && p + 1 < n && d[p + 1] != '\n') {  // rule 99
				crr += unescape(d[p + 1]);
				p += 2;
				continue;
			}
			if (c == '"') {  // rule 100
				p++;
				start = 1;
				v.str = crr;
				return T_STRING_LIT;
			}
			if (c == '\n') {  // a bare \n is skipped everywhere
				p++;
				continue;
			}
			crr += char16_t(c);  // rules 101/102 (text, a lone \r, a backslash before \n)
			p++;
			continue;
		}
		if (start == 5) {  // block comment
			if (eof) {
				comp_error("end of file in comment");
				start = 1;
				continue;
			}
			const int c = d[p];
			if (c == '\r' && at(p + 1) == '\n') {
				cur->line_count++;
				p += 2;
			} else if (c == '/' && at(p + 1) == '*') {
				if (comment_nest < 16) comment_nest++;
				p += 2;
			} else if (c == '*' && at(p + 1) == '/') {
				if (comment_nest) comment_nest--;
				if (!comment_nest) start = 1;
				p += 2;
			} else {
				p++;
			}
			continue;
		}
		if (start == 7) {  // line comment
			if (eof) {
				start = 1;
				return file_stack.empty() ? 0 : T_END_INCLUDE;
			}
			if (d[p] == '\r' && at(p + 1) == '\n') {
				cur->line_count++;
				p += 2;
				start = 1;
			} else {
				p++;
			}
			continue;
		}

		// INITIAL
		if (eof) return file_stack.empty() ? 0 : T_END_INCLUDE;
		const size_t s0 = p;
		const int c = d[p];
		auto text = [&](size_t e) {
			U16 t;
			for (size_t i = s0; i < e; i++) t += char16_t(d[i]);
			return t;
		};
		auto single = [&](int tok) {
			p++;
			v.str = text(p);
			return tok;
		};
		if (c == '\r') {
			if (at(p + 1) == '\n') {  // rule 33
				cur->line_count++;
				p += 2;
				continue;
			}
			return single(T_ERROR_TOKEN);
		}
		if (c == '\n' || c == ' ' || c == '\t' || c == '\f') {  // rules 110, 84
			p++;
			continue;
		}
		if (is_ident_start(c)) {
			size_t e = p;
			while (e < n && is_ident(d[e])) e++;
			const U16 word = text(e);
			// rule 22: include + exactly one blank + "name"
			if (word == u"include" && at(e) == ' ' && at(e + 1) == '"') {
				size_t q = e + 2;
				while (q < n && d[q] != '"' && d[q] != '\r' && d[q] != '\n') q++;
				if (q < n && d[q] == '"') {
					std::string name(reinterpret_cast<const char*>(d.data() + e + 2), q - (e + 2));
					p = q + 1;
					if (file_stack.size() >= 16) {
						comp_error("include files nested too deeply");
					} else {
						const std::string dir = directory_of(cur->input_string);
						if (open_file(name, dir)) {
							result.stats.include_openings++;
						} else {
							comp_error("Can't open include file \"" + name + "\"");
						}
					}
					continue;
				}
			}
			p = e;
			v.str = word;
			for (const Keyword& k : kKeywords) {
				if (word == to_u16(k.text)) return k.tok;
			}
			if (word == u"false") {  // rules 23/24: there is no boolean literal
				v.str = u"0";
				return T_INT_LIT;
			}
			if (word == u"true") {
				v.str = u"1";
				return T_INT_LIT;
			}
			// rule 25 `parse`: marks the next string for localisation (identity here), then resolves like rule 26
			return resolve_token(word, v);
		}
		if (is_digit(c) || (c == '.' && is_digit(at(p + 1)))) {
			size_t e = p;
			if (c == '0' && (at(p + 1) == 'x' || at(p + 1) == 'X') && is_xdigit(at(p + 2))) {  // rule 27
				e = p + 2;
				while (e < n && is_xdigit(d[e])) e++;
				p = e;
				v.str = text(e);
				return T_INT_LIT;
			}
			if (c == '0' && (at(p + 1) == 'b' || at(p + 1) == 'B') && (at(p + 2) == '0' || at(p + 2) == '1')) {
				e = p + 2;  // rule 28
				while (e < n && (d[e] == '0' || d[e] == '1')) e++;
				p = e;
				v.str = text(e);
				return T_INT_LIT;
			}
			while (e < n && is_digit(d[e])) e++;
			auto exponent = [&](size_t q) -> size_t {  // [eE][+-]?[0-9]+ at q; returns end or q
				if (q < n && (d[q] == 'e' || d[q] == 'E')) {
					size_t r = q + 1;
					if (r < n && (d[r] == '+' || d[r] == '-')) r++;
					if (r < n && is_digit(d[r])) {
						while (r < n && is_digit(d[r])) r++;
						return r;
					}
				}
				return q;
			};
			if (at(e) == '.') {  // rules 31/32
				size_t f = e + 1;
				while (f < n && is_digit(d[f])) f++;
				f = exponent(f);
				p = f;
				v.str = text(f);
				return T_FLOAT_LIT;
			}
			const size_t x = exponent(e);
			p = x;
			v.str = text(x);
			return T_INT_LIT;  // rules 29/30 (digits with an exponent are INT_LIT too)
		}
		switch (c) {
		case '"':  // rule 37
			p++;
			crr.clear();
			start = 3;
			continue;
		case '\'':
			if (at(p + 1) == '\\' && at(p + 2) >= 0 && at(p + 2) != '\n' && at(p + 3) == '\'') {  // rule 35
				v.str = U16(1, unescape(d[p + 2]));
				p += 4;
				return T_CHAR_LIT;
			}
			if (at(p + 1) >= 0 && at(p + 1) != '\n' && at(p + 2) == '\'') {  // rule 34
				v.str = U16(1, char16_t(d[p + 1]));
				p += 3;
				return T_CHAR_LIT;
			}
			return single(T_ERROR_TOKEN);
		case '/':
			if (at(p + 1) == '/') {  // rule 36
				p += 2;
				start = 7;
				continue;
			}
			if (at(p + 1) == '*') {  // rule 38
				p += 2;
				if (comment_nest < 16) comment_nest++;
				start = 5;
				continue;
			}
			if (at(p + 1) == '=') {
				p += 2;
				return T_DIV_ASSIGN;
			}
			return single('/');
		case '>':
			if (at(p + 1) == '>' && at(p + 2) == '=') return p += 3, T_RIGHT_ASSIGN;
			if (at(p + 1) == '>') return p += 2, T_RIGHT_OP;
			if (at(p + 1) == '=') return p += 2, T_GE_OP;
			return single('>');
		case '<':
			if (at(p + 1) == '<' && at(p + 2) == '=') return p += 3, T_LEFT_ASSIGN;
			if (at(p + 1) == '<') return p += 2, T_LEFT_OP;
			if (at(p + 1) == '=') return p += 2, T_LE_OP;
			return single('<');
		case '+':
			if (at(p + 1) == '=') return p += 2, T_ADD_ASSIGN;
			if (at(p + 1) == '+') return p += 2, T_INC_OP;
			return single('+');
		case '-':
			if (at(p + 1) == '=') return p += 2, T_SUB_ASSIGN;
			if (at(p + 1) == '-') return p += 2, T_DEC_OP;
			return single('-');
		case '*':
			if (at(p + 1) == '=') return p += 2, T_MUL_ASSIGN;
			return single('*');
		case '%':
			if (at(p + 1) == '=') return p += 2, T_MOD_ASSIGN;
			return single('%');
		case '&':  // '&' and "&&" are both the logical AND_OP
			if (at(p + 1) == '&') return p += 2, T_AND_OP;
			return single(T_AND_OP);
		case '|':
			if (at(p + 1) == '|') return p += 2, T_OR_OP;
			return single(T_OR_OP);
		case '^':  // power
			if (at(p + 1) == '=') return p += 2, T_POW_ASSIGN;
			return single(T_POW_OP);
		case '=':
			if (at(p + 1) == ' ' && at(p + 2) == 'r' && at(p + 3) == 'e' && at(p + 4) == 'f') {  // rule 81 "= ref"
				p += 5;
				return T_REF_ASSIGN;
			}
			if (at(p + 1) == '=') return p += 2, T_EQ_OP;
			if (at(p + 1) == '>') return p += 2, T_REF_ASSIGN;  // rule 80
			return single('=');
		case '!':
			if (at(p + 1) == '=') return p += 2, T_NE_OP;
			return single('!');
		case '$':
			if (at(p + 1) == 'S') return p += 2, T_LOCALIZE;  // rule 82
			return single(T_ERROR_TOKEN);
		case '~':
			if (at(p + 1) == '~') {  // rule 83: EVAL_END only in eval mode
				p += 2;
				continue;
			}
			return single(T_ERROR_TOKEN);
		case ';': case '{': case '}': case ',': case ':': case '(': case ')': case '[': case ']': case '.': case '?':
			return single(c);
		default:
			return single(T_ERROR_TOKEN);  // rule 109
		}
	}
}

}  // namespace ron::bhs
