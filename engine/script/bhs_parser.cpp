// The BigHugeScript parser: a bison 1.x style LR driver over the tables of bhs_grammar.cpp, with the semantic actions
// of yyparse @9ba430 (one case per rule; children of init_new_node from the disassembly of the action switch,
// docs/script-spec.md section 3.1), and the compile entry point.
#include <stdexcept>

#include "script/bhs_compiler_internal.hpp"
#include "script/bhs_grammar.hpp"

namespace ron::bhs {

Compiler::Compiler(const CompileOptions& o) : opt(o), builtins(o.builtins) {}
Compiler::~Compiler() = default;

void Compiler::comp_error(const std::string& msg, bool warning, int line) {
	if (line < 1) line = cur ? cur->line_count : 0;
	const std::string where = (cur ? cur->input_string : std::string("?")) + "(" + std::to_string(line) + "): ";
	if (warning) {
		warning_count++;
		result.warnings.push_back(where + msg);
	} else {
		err_count++;
		result.errors.push_back(where + msg);
	}
}

bool Compiler::parse() {
	const ParseTables& t = parse_tables();
	std::vector<int> states{0};
	std::vector<YYVal> vs(1);
	int state = 0, yychar = -2, err_status = 0;
	YYVal lval;
	for (long guard = 0; guard < 50000000; guard++) {
		int rule = 0;
		if (!t.needs_lookahead[size_t(state)]) {
			rule = t.default_rule[size_t(state)];
		} else {
			if (yychar == -2) {
				yychar = lex(lval);
				if (opt.on_token) opt.on_token(yychar, lval.sym ? lval.sym->token : lval.str);
			}
			if (yychar <= 0) yychar = 0;
			const int a = t.act(state, t.term(yychar));
			if (a == ParseTables::kAccept) return err_count == 0;
			if (a > 0) {
				if (yychar != 0) yychar = -2;
				states.push_back(a);
				vs.push_back(lval);
				state = a;
				if (err_status) err_status--;
				continue;
			}
			rule = a < 0 ? -a : t.default_rule[size_t(state)];
		}
		if (rule == 0) {
			// syntax error, bison recovery: report once, discard tokens, pop to a state that shifts `error`
			if (err_status == 0) {
				comp_error(std::string("syntax error near ") + tok_name(yychar) +
				           (lval.str.empty() ? "" : " \"" + to_utf8(lval.str) + "\""));
			} else if (err_status == 3) {
				if (yychar == 0) return false;
				yychar = -2;
			}
			err_status = 3;
			for (;;) {
				const int a = t.act(state, t.error_term);
				if (a > 0 && a != ParseTables::kAccept) {
					states.push_back(a);
					vs.push_back(lval);
					state = a;
					break;
				}
				if (states.size() <= 1) return false;
				states.pop_back();
				vs.pop_back();
				state = states.back();
			}
			continue;
		}
		const int len = t.rule_len[size_t(rule)];
		if (opt.on_reduce) opt.on_reduce(rule);
		const size_t base = vs.size() - size_t(len);
		YYVal v = action(rule, vs, base, len);
		states.resize(states.size() - size_t(len));
		vs.resize(vs.size() - size_t(len));
		const int g = t.goto_state(states.back(), t.rule_lhs[size_t(rule)]);
		if (g < 0) throw std::runtime_error("bhs parser: missing goto");
		state = g;
		states.push_back(state);
		vs.push_back(std::move(v));
	}
	throw std::runtime_error("bhs parser did not terminate");
}

// The semantic actions (the case labels of yyparse's switch are the rule numbers).
YYVal Compiler::action(int rule, std::vector<YYVal>& vs, size_t base, int len) {
	auto top = [&](int k) -> YYVal& { return vs[vs.size() - 1 - size_t(k)]; };
	YYVal r = len ? vs[base] : (base < vs.size() ? vs[base] : YYVal());  // bison's default $$ = $1
	SymTable* file = cur ? cur->file_table : nullptr;
	auto node = [&](int type, SymType* sym, std::initializer_list<Node*> c) {
		YYVal o;
		o.node = init_new_node(type, sym, c);
		return o;
	};
	auto reset_decl = [&] {
		curr_scope = 0;
		curr_type = t_void;
	};
	auto append_child = [](Node* list, Node* c) {  // SyntaxNode::add_child_node(c, 0): code appended
		list->children.push_back(c);
		if (!c->opcode) return;  // (the original would dereference null here)
		if (list->opcode) prepend(c->opcode, list->opcode);
		list->opcode = c->opcode;
	};
	auto prepend_child = [](Node* list, Node* c) {  // the list rules that link the new child's code first
		list->children.push_back(c);
		if (c->opcode && list->opcode) prepend(list->opcode, c->opcode);
	};
	switch (rule) {
	case 2: r = top(1); r.str += top(0).str; return r;
	case 4: r = YYVal(); r.sym = add_const_type(file, TY_INT, top(0).str); return r;
	case 5: r = YYVal(); r.sym = add_const_type(file, TY_FLOAT, top(0).str); return r;
	case 6: r = YYVal(); r.sym = add_const_type(file, TY_CHAR, top(0).str); return r;
	case 7: r = YYVal(); r.sym = add_const_type(file, TY_STRING, top(0).str); return r;
	case 8: r = YYVal(); r.sym = add_const_type(file, TY_STRING, top(1).str); return r;  // $S(...): identity
	case 9: case 10: return node(N_VAR, top(0).sym, {});
	case 11: return node(N_CONST, top(0).sym, {});
	case 12: return node(N_STRUCT_INITER, top(3).sym, {top(1).node});
	case 13: return node(N_ARRAY_INITER, nullptr, {top(1).node});
	case 14: return top(1);
	case 15: {
		YYVal o = node(N_ERROR, nullptr, {});
		comp_error("Illegal token \"" + to_utf8(top(0).str) + "\"");
		return o;
	}
	case 16: return node(N_ARRAY_INDEX, nullptr, {top(3).node, top(1).node});
	case 17: return node(N_STRUCT_ACCESS, string_wrap(top(0).str), {top(2).node});
	case 24: return node(N_STRUCT_GET, nullptr, {top(0).node});
	case 26: return node(top(1).i, nullptr, {top(0).node});
	case 27: return node(N_CAST, top(2).sym, {top(0).node});
	case 28: r = YYVal(); r.i = N_UNA_NEGA; return r;
	case 29: r = YYVal(); r.i = N_UNA_TILD; return r;
	case 30: r = YYVal(); r.i = N_UNA_NOT; return r;
	case 32: return node(N_POW_OP, nullptr, {top(2).node, top(0).node});
	case 34: return node(N_MUL, nullptr, {top(2).node, top(0).node});
	case 35: return node(N_DIV, nullptr, {top(2).node, top(0).node});
	case 36: return node(N_MOD, nullptr, {top(2).node, top(0).node});
	case 38: return node(N_ADD, nullptr, {top(2).node, top(0).node});
	case 39: return node(N_SUBT, nullptr, {top(2).node, top(0).node});
	case 41: return node(T_LEFT_OP, nullptr, {top(2).node, top(0).node});
	case 42: return node(T_RIGHT_OP, nullptr, {top(2).node, top(0).node});
	case 44: return node(N_LESS, nullptr, {top(2).node, top(0).node});
	case 45: return node(N_GREA, nullptr, {top(2).node, top(0).node});
	case 46: return node(N_LE_OP, nullptr, {top(2).node, top(0).node});
	case 47: return node(N_GE_OP, nullptr, {top(2).node, top(0).node});
	case 49: return node(N_EQ_OP, nullptr, {top(2).node, top(0).node});
	case 50: return node(N_NE_OP, nullptr, {top(2).node, top(0).node});
	case 52: return node(N_AND_BIT, nullptr, {top(2).node, top(0).node});
	case 54: return node(N_XOR_BIT, nullptr, {top(2).node, top(0).node});
	case 56: return node(N_OR_BIT, nullptr, {top(2).node, top(0).node});
	case 58: return node(N_AND_OP, nullptr, {top(2).node, top(0).node});
	case 60: return node(N_OR_OP, nullptr, {top(2).node, top(0).node});
	case 62: return node(N_COND_EXP, nullptr, {top(4).node, top(2).node, top(0).node});
	case 65: return node(N_ASSIGN_TO_VAR, top(2).sym, {top(0).node});
	case 66: return node(N_ASSIGN_TO_ARRAY_INDEX, nullptr, {top(2).node, top(0).node});
	case 67: return node(N_ASSIGN_TO_STRUCT_FIELD, nullptr, {top(2).node, top(0).node});
	case 68: return node(N_REF_ASSIGN, top(2).sym, {top(0).node});
	case 69: return node(N_INC_OP_POST, nullptr, {top(1).node});
	case 70: return node(N_DEC_OP_POST, nullptr, {top(1).node});
	case 71: return node(N_INC_OP, nullptr, {top(0).node});
	case 72: return node(N_DEC_OP, nullptr, {top(0).node});
	case 73: case 74: case 75: case 76: case 77: case 78: case 79: case 80: case 81: case 82: case 83: case 84: {
		static const int ops[] = {OP_ASSIGN, OP_MUL_ASSIGN, OP_DIV_ASSIGN, OP_MOD_ASSIGN, OP_ADD_ASSIGN, OP_SUB_ASSIGN,
		                          OP_POW_ASSIGN, OP_LEFT_ASSIGN, OP_RIGHT_ASSIGN, OP_AND_ASSIGN, OP_XOR_ASSIGN, OP_OR_ASSIGN};
		last_op = ops[rule - 73];
		r = YYVal();
		r.i = last_op;
		return r;
	}
	case 85: curr_scope = kScopeStatic; r = YYVal(); r.i = kScopeStatic; return r;
	case 86: curr_scope = kScopeRef; r = YYVal(); r.i = kScopeRef; return r;
	case 87: r = top(0); curr_type = r.sym; return r;
	case 88: r = YYVal(); r.sym = add_array_type(file, top(1).sym); curr_type = r.sym; return r;
	case 89: r = YYVal(); r.sym = add_array_type(file, top(2).sym); curr_type = r.sym; return r;
	case 90: case 91: r = top(0); curr_scope = 0; return r;
	case 92: r = YYVal(); r.sym = t_int; curr_scope = 0; curr_type = t_int; return r;
	case 93: r = YYVal(); r.sym = create_var_type(top(0).str, curr_type); return r;
	case 94: r = top(2); r.sym->of_type = add_array_type(file, r.sym->of_type); return r;
	case 95: case 96: return node(N_INIT_DECL, top(2).sym, {top(0).node});
	case 97: case 98: {
		YYVal o = node(N_INIT_DECL, top(2).sym, {top(0).node});
		curr_scope = kScopeRef;
		return o;
	}
	case 100: return node(N_NON_INIT_DECL, top(0).sym, {});
	case 101: return node(N_ARRAY_SIZED_DECL, top(3).sym, {top(1).node});
	case 102: return node(N_FINISH_DECL, nullptr, {top(0).node});
	case 103: case 114: case 123: case 128: case 131: case 138: case 150: case 156: case 194:
		return node(N_GENERIC_LIST, nullptr, {top(0).node});
	case 112: return node(N_GENERIC_LIST, nullptr, {top(1).node});
	case 104: case 124: r = top(2); append_child(r.node, top(0).node); return r;
	case 139: case 151: case 157: case 195: r = top(1); append_child(r.node, top(0).node); return r;
	case 105: case 106: case 107: r = top(1); reset_decl(); return r;
	case 108: {
		YYVal o = node(N_FINISH_DECL, nullptr, {top(1).node});
		reset_decl();
		return o;
	}
	case 109: case 126: case 127: case 162: case 217: return node(N_NOP, nullptr, {});
	case 215: return node(N_NOP, top(1).sym, {});
	case 216: return node(N_NOP, top(2).sym, {});
	case 110: r = YYVal(); r.sym = add_struct_type(file, top(3).str, top(1).node, curr_table); return r;
	case 111: r = YYVal(); r.sym = add_struct_type_existing(file, top(3).sym, top(1).node, curr_table); return r;
	case 113: r = top(2); prepend_child(r.node, top(1).node); return r;
	case 115: case 133: r = top(2); prepend_child(r.node, top(0).node); return r;
	case 116: {
		YYVal o = node(N_NON_INIT_DECL, top(0).sym, {});
		curr_table->table.push_back(top(0).sym);
		return o;
	}
	case 117: case 118: case 119: case 120: return node(N_PARAM_DECL, top(0).sym, {});
	case 121: r = top(0); reset_decl(); return r;
	case 122: {
		YYVal o = node(N_PARAM_DEFAULT, top(0).sym, {top(2).node});
		reset_decl();
		return o;
	}
	case 125: case 158: return top(1);
	case 129: case 132: return node(N_GENERIC_LIST, nullptr, {});
	case 130: r = top(2); r.node->children.push_back(top(0).node); return r;  // arg lists keep their code unlinked
	case 152: {
		YYVal o = node(N_CASE, nullptr, {top(1).node, top(0).node});
		curr_table = curr_table->parent;
		return o;
	}
	case 153: {
		YYVal o = node(N_CASE, nullptr, {top(0).node});
		curr_table = curr_table->parent;
		return o;
	}
	case 154: {
		YYVal o = node(N_CASE_HEADER, top(1).sym, {});
		curr_table = add_child_table(curr_table, 0);
		return o;
	}
	case 155: {
		YYVal o = node(N_CASE_HEADER, nullptr, {});
		curr_table = add_child_table(curr_table, 0);
		return o;
	}
	case 160: return node(N_STMT_BLOCK, nullptr, {top(0).node});
	case 161: return node(N_STMT_BLOCK, nullptr, {});
	case 163: return node(N_STMT, nullptr, {top(1).node});
	case 164: case 196: case 211: return node(N_ERROR, nullptr, {});
	case 165: return node(N_IF, nullptr, {top(2).node, top(0).node});
	case 166: return node(N_IF, nullptr, {top(4).node, top(2).node, top(0).node});
	case 167: return node(N_SWITCH, nullptr, {top(3).node, top(1).node});
	case 168: r = top(1); set_switch_ops(r.node ? r.node->sym : nullptr); return r;
	case 169: set_loop_ops(); return r;
	case 170: return node(N_WHILE, nullptr, {top(3).node, top(0).node});
	case 171: return node(N_DO, nullptr, {top(5).node, top(2).node});
	case 172: return node(N_FOR, nullptr, {top(5).node, top(4).node, top(0).node});
	case 173: return node(N_FOR, nullptr, {top(6).node, top(5).node, top(3).node, top(0).node});
	case 174: return node(N_CONTINUE, nullptr, {});
	case 175: return node(N_BREAK, nullptr, {});
	case 176: return node(N_RETURN, nullptr, {});
	case 177: return node(N_RETURN, nullptr, {top(1).node});
	case 178:
		result.stats.triggers++;
		r = YYVal();
		r.sym = add_trigger_type(cur->script_table, top(0).str);
		return r;
	case 179:
		result.stats.triggers++;
		r = YYVal();
		r.sym = add_trigger_type(cur->script_table, U16());
		return r;
	case 180: r = YYVal(); r.sym = add_trigger_type(cur->script_table, U16()); return r;
	case 181: return node(N_TRIGGER, top(4).sym, {top(2).node, top(0).node});
	case 182: return node(N_TRIGGER, top(3).sym, {top(0).node});
	case 183: return node(N_TRIGGER, top(1).sym, {top(0).node});
	case 184: r = YYVal(); r.i = T_ENABLE_TRIGGER; return r;
	case 185: r = YYVal(); r.i = T_DISABLE_TRIGGER; return r;
	case 188:
		return node(N_TRIGGER_TOGGLE, add_trigger_toggle_name(cur->script_table, top(1).str, top(3).i == T_ENABLE_TRIGGER), {});
	case 189: {
		SymType* trig = (top(1).sym && top(1).sym->kind == K_TRIGGER) ? top(1).sym : nullptr;
		return node(N_TRIGGER_TOGGLE, add_trigger_toggle_sym(cur->script_table, trig, top(3).i == T_ENABLE_TRIGGER), {});
	}
	case 190: r = top(0); master_root = r.node; return r;
	case 191: {
		YYVal o = node(N_EVAL, nullptr, {top(1).node});
		master_root = o.node;
		return o;
	}
	case 192: case 193: return node(N_COMPLETE_FILE, nullptr, {top(0).node});
	case 201: return node(N_FUNC_CALL, string_wrap(top(3).str), {top(1).node, top(5).node});
	case 202: return node(N_FUNC_CALL, top(3).sym, {top(1).node, top(5).node});
	case 203: return node(N_FUNC_CALL, string_wrap(top(3).str), {top(1).node});
	case 204: return node(N_FUNC_CALL, top(3).sym, {top(1).node});
	case 205: curr_table = add_child_table(curr_table, 0); r = YYVal(); return r;
	case 206: curr_table = add_child_table(curr_table, 1); r = YYVal(); return r;
	case 207: curr_table = curr_table->parent; r = YYVal(); return r;
	case 208:
		curr_table = add_child_table(curr_table, 0);
		cur->script_table = curr_table;
		curr_scope = 0;
		curr_type = t_void;
		return YYVal();
	case 209: {
		YYVal o = node(N_SCRIPT_BODY, nullptr, {top(3).node, top(1).node});
		cur->script_table = nullptr;
		return o;
	}
	case 210: return node(N_SCRIPT_DECL, nullptr, {top(1).node});
	case 212: case 213: case 214: {
		result.stats.scripts++;
		SymType* fn;
		if (rule == 212) {
			SymType* kind = (top(3).sym && top(3).sym->kind == K_SCRIPT_KIND) ? top(3).sym : nullptr;
			fn = add_script_type(file, top(2).str, kind, top(4).sym, top(0).node);
		} else if (rule == 213) {
			SymType* kind = (top(2).sym && top(2).sym->kind == K_SCRIPT_KIND) ? top(2).sym : nullptr;
			fn = add_script_type(file, U16(), kind, top(3).sym, top(0).node);
		} else {
			SymType* kind = (top(3).sym && top(3).sym->kind == K_SCRIPT_KIND) ? top(3).sym : nullptr;
			SymType* existing = (top(2).sym && top(2).sym->kind == K_FUNC) ? top(2).sym : nullptr;
			fn = existing ? add_script_type_existing(file, existing, kind, top(4).sym, top(0).node)
			              : add_script_type(file, top(2).str, kind, top(4).sym, top(0).node);
		}
		return node(N_SCRIPT_HEADER, fn, {top(0).node});
	}
	case 218: r = YYVal(); r.sym = add_enum_type(file, nullptr, top(0).str, nullptr); return r;
	case 219: r = YYVal(); r.sym = add_enum_type(file, nullptr, top(2).str, top(0).sym); return r;
	case 220: {
		SymType* prev = (top(2).sym && top(2).sym->kind == K_ENUM) ? top(2).sym : nullptr;
		r = YYVal();
		r.sym = add_enum_type(file, prev, top(0).str, nullptr);
		return r;
	}
	case 221: {
		SymType* prev = (top(4).sym && top(4).sym->kind == K_ENUM) ? top(4).sym : nullptr;
		r = YYVal();
		r.sym = add_enum_type(file, prev, top(2).str, top(0).sym);
		return r;
	}
	default:
		// the pass-through rules: $$ = the last right-hand side value (1, 3, 18-23, 25, 31.., 99, 134-149, 159,
		// 186-187, 197-200); the empty ones keep bison's stale $$.
		if (len) return top(0);
		return r;
	}
}

CompileResult Compiler::run(const std::string& path) {
	include_paths = {".\\scenario\\scriptlibrary\\"};  // prefs default (Lexer::init_once @9c0f90)
	init_root();
	cur = nullptr;
	curr_type = t_int;
	if (!open_file(path, std::string())) {
		result.errors.push_back("can't open " + path);
		return std::move(result);
	}
	curr_table = cur->file_table;
	bool ok = false;
	try {
		ok = parse();
	} catch (const std::exception& e) {
		result.errors.push_back(std::string("internal error: ") + e.what());
	}
	if (!ok) err_count++;
	total_err_count += err_count;
	result.ok = total_err_count == 0 && master_root != nullptr && result.errors.empty();
	for (const CompiledFile& f : result.files) {
		if (!f.ok) result.ok = false;
	}
	return std::move(result);
}

CompileResult compile(const std::string& path, const CompileOptions& opt) {
	Compiler c(opt);
	return c.run(path);
}

}  // namespace ron::bhs
