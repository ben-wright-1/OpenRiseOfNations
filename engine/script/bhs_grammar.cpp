#include "script/bhs_grammar.hpp"

#include <algorithm>
#include <bitset>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "script/bhs_common.hpp"

namespace ron::bhs {

namespace {

// docs/script-spec.md section 2.1 (nonterminal names are ours; rule numbers are the binary's).
const char* const kGrammar[] = {
    "$accept : program $end",  // 0
    "string_list : STRING_LIT",
    "string_list : string_list STRING_LIT",
    "literal : CONST_NAME",
    "literal : INT_LIT",
    "literal : FLOAT_LIT",
    "literal : CHAR_LIT",
    "literal : string_list",
    "literal : LOCALIZE '(' string_list ')'",
    "var_ref : VAR_NAME",
    "primary : VAR_NAME",  // 10
    "primary : literal",
    "primary : type '(' expr_list ')'",
    "primary : '[' expr_list ']'",
    "primary : '(' expr ')'",
    "primary : ERROR_TOKEN",
    "array_index : postfix '[' expr ']'",
    "field_access : postfix '.' IDENTIFIER",
    "incdec_target : var_ref",
    "incdec_target : array_index",
    "incdec_target : field_access",  // 20
    "postfix : primary",
    "postfix : call",
    "postfix : array_index",
    "postfix : field_access",
    "unary : postfix",
    "unary : unary_op unary",
    "unary : '(' type ')' unary",
    "unary_op : '-'",
    "unary_op : '~'",
    "unary_op : '!'",  // 30
    "pow_expr : unary",
    "pow_expr : pow_expr POW_OP unary",
    "mul_expr : pow_expr",
    "mul_expr : mul_expr '*' pow_expr",
    "mul_expr : mul_expr '/' pow_expr",
    "mul_expr : mul_expr '%' pow_expr",
    "add_expr : mul_expr",
    "add_expr : add_expr '+' mul_expr",
    "add_expr : add_expr '-' mul_expr",
    "shift_expr : add_expr",  // 40
    "shift_expr : shift_expr LEFT_OP add_expr",
    "shift_expr : shift_expr RIGHT_OP add_expr",
    "rel_expr : shift_expr",
    "rel_expr : rel_expr '<' shift_expr",
    "rel_expr : rel_expr '>' shift_expr",
    "rel_expr : rel_expr LE_OP shift_expr",
    "rel_expr : rel_expr GE_OP shift_expr",
    "eq_expr : rel_expr",
    "eq_expr : eq_expr EQ_OP rel_expr",
    "eq_expr : eq_expr NE_OP rel_expr",  // 50
    "bitand_expr : eq_expr",
    "bitand_expr : bitand_expr '&' eq_expr",
    "bitxor_expr : bitand_expr",
    "bitxor_expr : bitxor_expr '^' bitand_expr",
    "bitor_expr : bitxor_expr",
    "bitor_expr : bitor_expr '|' bitxor_expr",
    "and_expr : bitor_expr",
    "and_expr : and_expr AND_OP bitor_expr",
    "or_expr : and_expr",
    "or_expr : or_expr OR_OP and_expr",  // 60
    "cond_expr : or_expr",
    "cond_expr : or_expr '?' expr ':' cond_expr",
    "expr : cond_expr",
    "assign_expr : expr",
    "assign_expr : VAR_NAME assign_op expr",
    "assign_expr : array_index assign_op expr",
    "assign_expr : field_access assign_op expr",
    "assign_expr : VAR_NAME REF_ASSIGN var_ref",
    "assign_expr : incdec_target INC_OP",
    "assign_expr : incdec_target DEC_OP",  // 70
    "assign_expr : INC_OP incdec_target",
    "assign_expr : DEC_OP incdec_target",
    "assign_op : '='",
    "assign_op : MUL_ASSIGN",
    "assign_op : DIV_ASSIGN",
    "assign_op : MOD_ASSIGN",
    "assign_op : ADD_ASSIGN",
    "assign_op : SUB_ASSIGN",
    "assign_op : POW_ASSIGN",
    "assign_op : LEFT_ASSIGN",  // 80
    "assign_op : RIGHT_ASSIGN",
    "assign_op : AND_ASSIGN",
    "assign_op : XOR_ASSIGN",
    "assign_op : OR_ASSIGN",
    "static_kw : STATIC",
    "ref_kw : REF",
    "type : TYPE_NAME",
    "type : ARRAY '<' type '>'",
    "type : type '[' ']'",
    "opt_type : type",  // 90
    "opt_type : VOID",
    "opt_type :",
    "declarator : IDENTIFIER",
    "declarator : declarator '[' ']'",
    "init_declarator : declarator '=' assign_expr",
    "init_declarator : declarator '=' init_declarator",
    "init_declarator : declarator REF_ASSIGN var_ref",
    "init_declarator : declarator REF_ASSIGN init_declarator",
    "declarator_item : init_declarator",
    "declarator_item : declarator",  // 100
    "declarator_item : declarator '(' expr ')'",
    "finished_declarator : declarator_item",
    "declarator_list : finished_declarator",
    "declarator_list : declarator_list ',' finished_declarator",
    "declaration : type declarator_list ';'",
    "declaration : static_kw type declarator_list ';'",
    "declaration : static_kw declarator_list ';'",
    "declaration : init_declarator ';'",
    "struct_decl : struct_def ';'",
    "struct_def : STRUCT IDENTIFIER struct_open member_decls '}'",  // 110
    "struct_def : STRUCT TYPE_NAME struct_open member_decls '}'",
    "member_decls : member_decl ';'",
    "member_decls : member_decls member_decl ';'",
    "member_decl : opt_type member_declarator",
    "member_decl : member_decl ',' member_declarator",
    "member_declarator : declarator",
    "param : type declarator",
    "param : ref_kw type declarator",
    "param : ref_kw declarator",
    "param : declarator",  // 120
    "param_item : param",
    "param_item : param '=' literal",
    "param_list : param_item",
    "param_list : param_list ',' param_item",
    "opt_params : '(' param_list ')'",
    "opt_params : '(' ')'",
    "opt_params :",
    "arg_list : expr",
    "arg_list :",
    "arg_list : arg_list ',' expr",  // 130
    "expr_list : expr",
    "expr_list :",
    "expr_list : expr_list ',' expr",
    "script_item : stmt_or_decl",
    "script_item : trigger_def",
    "script_item : labels_block",
    "script_item : struct_decl",
    "stmt_list : stmt_or_decl",
    "stmt_list : stmt_list stmt_or_decl",
    "stmt_or_decl : statement",  // 140
    "stmt_or_decl : declaration",
    "for_init : declaration",
    "for_init : expr_stmt",
    "statement : expr_stmt",
    "statement : switch_stmt",
    "statement : if_stmt",
    "statement : loop_stmt",
    "statement : jump_stmt",
    "statement : toggle_stmt",
    "case_list : case_clause",  // 150
    "case_list : case_list case_clause",
    "case_clause : case_label stmt_list",
    "case_clause : case_label",
    "case_label : CASE literal ':'",
    "case_label : DEFAULT ':'",
    "script_items : script_item",
    "script_items : script_items script_item",
    "body : block_open opt_script_items block_close",
    "body : statement",
    "opt_script_items : script_items",  // 160
    "opt_script_items :",
    "expr_stmt : ';'",
    "expr_stmt : assign_expr ';'",
    "expr_stmt : error ';'",
    "if_stmt : IF '(' expr ')' body",
    "if_stmt : IF '(' expr ')' body ELSE body",
    "switch_stmt : switch_head block_open case_list block_close",
    "switch_head : SWITCH '(' expr ')'",
    "loop_mark :",
    "loop_stmt : WHILE '(' expr ')' loop_mark body",  // 170
    "loop_stmt : DO loop_mark body WHILE '(' expr ')' ';'",
    "loop_stmt : FOR '(' for_init expr ';' ')' loop_mark body",
    "loop_stmt : FOR '(' for_init expr ';' assign_expr ')' loop_mark body",
    "jump_stmt : CONTINUE ';'",
    "jump_stmt : BREAK ';'",
    "jump_stmt : RETURN ';'",
    "jump_stmt : RETURN expr ';'",
    "trigger_head : TRIGGER IDENTIFIER",
    "trigger_head : TRIGGER",
    "run_once_head : RUN_ONCE",  // 180
    "trigger_def : trigger_head '(' expr ')' body",
    "trigger_def : trigger_head '(' ')' body",
    "trigger_def : run_once_head body",
    "toggle_kw : ENABLE_TRIGGER",
    "toggle_kw : DISABLE_TRIGGER",
    "trigger_ref : IDENTIFIER",
    "trigger_ref : STRING_LIT",
    "toggle_stmt : toggle_kw '(' trigger_ref ')'",
    "toggle_stmt : toggle_kw '(' TRIGGER_NAME ')'",
    "program : file_seq",  // 190
    "program : or_expr EVAL_END",
    "file_seq : file_items",
    "file_seq : file_seq END_INCLUDE file_items",
    "file_items : file_item",
    "file_items : file_items file_item",
    "file_items : error",
    "file_item : script_def",
    "file_item : script_fwd",
    "file_item : labels_block",
    "file_item : struct_decl",  // 200
    "call : postfix '.' IDENTIFIER '(' arg_list ')'",
    "call : postfix '.' SCRIPT_NAME '(' arg_list ')'",
    "call : IDENTIFIER '(' arg_list ')'",
    "call : SCRIPT_NAME '(' arg_list ')'",
    "block_open : '{'",
    "struct_open : '{'",
    "block_close : '}'",
    "script_scope :",
    "script_def : script_head '{' opt_script_items '}'",
    "script_fwd : script_head ';'",  // 210
    "script_fwd : error ';'",
    "script_head : opt_type SCRIPT_KIND IDENTIFIER script_scope opt_params",
    "script_head : opt_type SCRIPT_KIND script_scope opt_params",
    "script_head : opt_type SCRIPT_KIND SCRIPT_NAME script_scope opt_params",
    "labels_block : LABELS '{' label_list '}'",
    "labels_block : LABELS '{' label_list ',' '}'",
    "labels_block : LABELS '{' '}'",
    "label_list : IDENTIFIER",
    "label_list : IDENTIFIER '=' literal",
    "label_list : label_list ',' IDENTIFIER",  // 220
    "label_list : label_list ',' IDENTIFIER '=' literal",
};
constexpr int kNumRules = int(sizeof kGrammar / sizeof kGrammar[0]);

int named_token(const std::string& n) {
	static const std::map<std::string, int> m = {
	    {"IDENTIFIER", T_IDENTIFIER}, {"INT_LIT", T_INT_LIT}, {"FLOAT_LIT", T_FLOAT_LIT}, {"CHAR_LIT", T_CHAR_LIT},
	    {"STRING_LIT", T_STRING_LIT}, {"INC_OP", T_INC_OP}, {"DEC_OP", T_DEC_OP}, {"LEFT_OP", T_LEFT_OP},
	    {"RIGHT_OP", T_RIGHT_OP}, {"LE_OP", T_LE_OP}, {"GE_OP", T_GE_OP}, {"EQ_OP", T_EQ_OP}, {"NE_OP", T_NE_OP},
	    {"POW_OP", T_POW_OP}, {"AND_OP", T_AND_OP}, {"OR_OP", T_OR_OP}, {"REF_ASSIGN", T_REF_ASSIGN},
	    {"MUL_ASSIGN", T_MUL_ASSIGN}, {"DIV_ASSIGN", T_DIV_ASSIGN}, {"MOD_ASSIGN", T_MOD_ASSIGN},
	    {"ADD_ASSIGN", T_ADD_ASSIGN}, {"SUB_ASSIGN", T_SUB_ASSIGN}, {"LEFT_ASSIGN", T_LEFT_ASSIGN},
	    {"RIGHT_ASSIGN", T_RIGHT_ASSIGN}, {"AND_ASSIGN", T_AND_ASSIGN}, {"XOR_ASSIGN", T_XOR_ASSIGN},
	    {"OR_ASSIGN", T_OR_ASSIGN}, {"POW_ASSIGN", T_POW_ASSIGN}, {"STATIC", T_STATIC}, {"REF", T_REF},
	    {"ERROR_TOKEN", T_ERROR_TOKEN}, {"SCRIPT_NAME", T_SCRIPT_NAME}, {"SCRIPT_KIND", T_SCRIPT_KIND},
	    {"VAR_NAME", T_VAR_NAME}, {"CONST_NAME", T_CONST_NAME}, {"TRIGGER_NAME", T_TRIGGER_NAME},
	    {"TYPE_NAME", T_TYPE_NAME}, {"VOID", T_VOID}, {"LOCALIZE", T_LOCALIZE}, {"RUN_ONCE", T_RUN_ONCE},
	    {"TRIGGER", T_TRIGGER}, {"ENABLE_TRIGGER", T_ENABLE_TRIGGER}, {"DISABLE_TRIGGER", T_DISABLE_TRIGGER},
	    {"END_INCLUDE", T_END_INCLUDE}, {"EVAL_END", T_EVAL_END}, {"LABELS", T_LABELS}, {"STRUCT", T_STRUCT},
	    {"ARRAY", T_ARRAY}, {"CASE", T_CASE}, {"DEFAULT", T_DEFAULT}, {"IF", T_IF}, {"ELSE", T_ELSE},
	    {"SWITCH", T_SWITCH}, {"WHILE", T_WHILE}, {"DO", T_DO}, {"FOR", T_FOR}, {"CONTINUE", T_CONTINUE},
	    {"BREAK", T_BREAK}, {"RETURN", T_RETURN}};
	auto it = m.find(n);
	return it == m.end() ? -1 : it->second;
}

using LaSet = std::bitset<128>;

struct Builder {
	// symbols: terminals 0..nt-1 ($end, error, $undefined, then the grammar's tokens), nonterminals nt..
	std::vector<std::string> names;
	std::vector<int> term_tok;  // terminal index -> token code
	int nt = 0;
	std::map<std::string, int> sym;
	struct RuleDef {
		int lhs;
		std::vector<int> rhs;
	};
	std::vector<RuleDef> rules;
	std::vector<bool> nullable;
	std::vector<LaSet> first;  // per nonterminal

	struct Item {
		int rule, dot;
		bool operator<(const Item& o) const { return rule != o.rule ? rule < o.rule : dot < o.dot; }
		bool operator==(const Item& o) const { return rule == o.rule && dot == o.dot; }
	};
	struct State {
		std::vector<Item> kernel;
		std::map<int, int> trans;  // symbol -> state
		std::vector<LaSet> la;     // per kernel item
	};
	std::vector<State> states;
	std::map<std::vector<Item>, int> state_index;

	bool is_term(int s) const { return s < nt; }

	void parse_grammar() {
		// terminals first: collect every symbol that is a token name or a quoted char
		names = {"$end", "error", "$undefined"};
		term_tok = {0, -1, -2};
		std::vector<std::vector<std::string>> parsed;
		for (int r = 0; r < kNumRules; r++) {
			std::istringstream in(kGrammar[r]);
			std::vector<std::string> w;
			std::string x;
			while (in >> x) w.push_back(x);
			parsed.push_back(w);
			for (size_t i = 2; i < w.size(); i++) {
				const std::string& s = w[i];
				if (s == "$end" || s == "error") continue;
				int tok = -1;
				if (s.size() == 3 && s[0] == '\'') tok = (unsigned char)s[1];
				else tok = named_token(s);
				if (tok >= 0 && !sym.count(s)) {
					sym[s] = int(names.size());
					names.push_back(s);
					term_tok.push_back(tok);
				}
			}
		}
		sym["$end"] = 0;
		sym["error"] = 1;
		nt = int(names.size());
		for (auto& w : parsed) {
			if (!sym.count(w[0])) {
				sym[w[0]] = int(names.size());
				names.push_back(w[0]);
			}
		}
		for (auto& w : parsed) {
			RuleDef rd;
			rd.lhs = sym.at(w[0]);
			for (size_t i = 2; i < w.size(); i++) {
				auto it = sym.find(w[i]);
				if (it == sym.end()) throw std::runtime_error("bhs grammar: unknown symbol " + w[i]);
				rd.rhs.push_back(it->second);
			}
			rules.push_back(rd);
		}
	}

	void compute_first() {
		const int ns = int(names.size());
		nullable.assign(size_t(ns), false);
		first.assign(size_t(ns), LaSet());
		for (int t = 0; t < nt; t++) first[size_t(t)].set(size_t(t));
		for (bool changed = true; changed;) {
			changed = false;
			for (const RuleDef& r : rules) {
				bool all_null = true;
				LaSet f = first[size_t(r.lhs)];
				for (int s : r.rhs) {
					f |= first[size_t(s)];
					if (!nullable[size_t(s)]) {
						all_null = false;
						break;
					}
				}
				if (f != first[size_t(r.lhs)]) first[size_t(r.lhs)] = f, changed = true;
				if (all_null && !nullable[size_t(r.lhs)]) nullable[size_t(r.lhs)] = true, changed = true;
			}
		}
	}

	std::vector<Item> closure0(const std::vector<Item>& kernel) const {
		std::vector<Item> out = kernel;
		std::set<int> added;
		for (size_t i = 0; i < out.size(); i++) {
			const RuleDef& r = rules[size_t(out[i].rule)];
			if (out[i].dot >= int(r.rhs.size())) continue;
			const int b = r.rhs[size_t(out[i].dot)];
			if (is_term(b) || added.count(b)) continue;
			added.insert(b);
			for (int k = 0; k < int(rules.size()); k++) {
				if (rules[size_t(k)].lhs == b) out.push_back({k, 0});
			}
		}
		return out;
	}

	// LR(1) closure with lookahead sets; bit `hash` (= 127) is the propagation marker.
	static constexpr int kHash = 127;
	std::map<Item, LaSet> closure1(const std::vector<std::pair<Item, LaSet>>& seed) const {
		std::map<Item, LaSet> la;
		std::vector<Item> work;
		for (auto& [it, s] : seed) {
			la[it] |= s;
			work.push_back(it);
		}
		while (!work.empty()) {
			const Item it = work.back();
			work.pop_back();
			const RuleDef& r = rules[size_t(it.rule)];
			if (it.dot >= int(r.rhs.size())) continue;
			const int b = r.rhs[size_t(it.dot)];
			if (is_term(b)) continue;
			LaSet f;
			bool all_null = true;
			for (size_t i = size_t(it.dot) + 1; i < r.rhs.size(); i++) {
				f |= first[size_t(r.rhs[i])];
				if (!nullable[size_t(r.rhs[i])]) {
					all_null = false;
					break;
				}
			}
			if (all_null) f |= la[it];
			for (int k = 0; k < int(rules.size()); k++) {
				if (rules[size_t(k)].lhs != b) continue;
				const Item ni{k, 0};
				LaSet& cur = la[ni];
				const LaSet merged = cur | f;
				if (merged != cur) {
					cur = merged;
					work.push_back(ni);
				}
			}
		}
		return la;
	}

	int add_state(std::vector<Item> kernel) {
		std::sort(kernel.begin(), kernel.end());
		auto it = state_index.find(kernel);
		if (it != state_index.end()) return it->second;
		const int id = int(states.size());
		state_index[kernel] = id;
		State s;
		s.kernel = kernel;
		s.la.assign(kernel.size(), LaSet());
		states.push_back(std::move(s));
		return id;
	}

	void build_lr0() {
		add_state({{0, 0}});
		for (size_t si = 0; si < states.size(); si++) {
			const std::vector<Item> cl = closure0(states[si].kernel);
			std::map<int, std::vector<Item>> moves;
			std::vector<int> order;
			for (const Item& it : cl) {
				const RuleDef& r = rules[size_t(it.rule)];
				if (it.dot >= int(r.rhs.size())) continue;
				const int x = r.rhs[size_t(it.dot)];
				if (x == 0) continue;  // never shift $end (accept is an action)
				if (!moves.count(x)) order.push_back(x);
				moves[x].push_back({it.rule, it.dot + 1});
			}
			for (int x : order) {
				const int t = add_state(moves[x]);
				states[si].trans[x] = t;
			}
		}
	}

	int kernel_pos(const State& s, const Item& it) const {
		for (size_t i = 0; i < s.kernel.size(); i++) {
			if (s.kernel[i] == it) return int(i);
		}
		return -1;
	}

	void build_lalr() {
		struct Edge {
			int from_state, from_item, to_state, to_item;
		};
		std::vector<Edge> edges;
		for (int si = 0; si < int(states.size()); si++) {
			const State& s = states[size_t(si)];
			for (int ki = 0; ki < int(s.kernel.size()); ki++) {
				LaSet h;
				h.set(kHash);
				const auto cl = closure1({{s.kernel[size_t(ki)], h}});
				for (auto& [it, la] : cl) {
					const RuleDef& r = rules[size_t(it.rule)];
					if (it.dot >= int(r.rhs.size())) continue;
					const int x = r.rhs[size_t(it.dot)];
					auto tr = s.trans.find(x);
					if (tr == s.trans.end()) continue;
					State& tgt = states[size_t(tr->second)];
					const int pos = kernel_pos(tgt, {it.rule, it.dot + 1});
					LaSet spont = la;
					spont.reset(kHash);
					tgt.la[size_t(pos)] |= spont;
					if (la.test(kHash)) edges.push_back({si, ki, tr->second, pos});
				}
			}
		}
		for (bool changed = true; changed;) {
			changed = false;
			for (const Edge& e : edges) {
				const LaSet& src = states[size_t(e.from_state)].la[size_t(e.from_item)];
				LaSet& dst = states[size_t(e.to_state)].la[size_t(e.to_item)];
				const LaSet m = dst | src;
				if (m != dst) dst = m, changed = true;
			}
		}
	}

	void emit(ParseTables& t) {
		t.nstates = int(states.size());
		t.nterms = nt;
		t.nnonterms = int(names.size()) - nt;
		t.action.assign(size_t(t.nstates) * size_t(t.nterms), 0);
		t.go.assign(size_t(t.nstates) * size_t(t.nnonterms), -1);
		t.default_rule.assign(size_t(t.nstates), 0);
		t.needs_lookahead.assign(size_t(t.nstates), 1);
		for (const RuleDef& r : rules) {
			t.rule_lhs.push_back(r.lhs - nt);
			t.rule_len.push_back(int(r.rhs.size()));
		}
		for (int r = 0; r < kNumRules; r++) t.rule_text.push_back(kGrammar[r]);
		t.tok_to_term.assign(0x200, t.undef_term);
		for (int i = 0; i < nt; i++) {
			if (term_tok[size_t(i)] >= 0) t.tok_to_term[size_t(term_tok[size_t(i)])] = i;
		}
		for (int si = 0; si < t.nstates; si++) {
			const State& s = states[size_t(si)];
			int* row = &t.action[size_t(si) * size_t(t.nterms)];
			bool term_shift = false;
			for (auto& [x, to] : s.trans) {
				if (is_term(x)) {
					row[x] = to;
					term_shift = true;
				} else {
					t.go[size_t(si) * size_t(t.nnonterms) + size_t(x - nt)] = to;
				}
			}
			// reductions from the LR(1) closure of the kernel with its lookaheads
			std::vector<std::pair<Item, LaSet>> seed;
			for (size_t i = 0; i < s.kernel.size(); i++) seed.push_back({s.kernel[i], s.la[i]});
			const auto cl = closure1(seed);
			std::set<int> reduce_rules;
			bool accept = false;
			for (auto& [it, la] : cl) {
				const RuleDef& r = rules[size_t(it.rule)];
				if (it.rule == 0 && it.dot == 1) accept = true;
				if (it.dot != int(r.rhs.size()) || it.rule == 0) continue;
				reduce_rules.insert(it.rule);
				for (int a = 0; a < nt; a++) {
					if (!la.test(size_t(a))) continue;
					int& cell = row[a];
					if (cell > 0) {
						t.conflicts_sr++;  // shift wins
					} else if (cell < 0) {
						t.conflicts_rr++;
						if (it.rule < -cell) cell = -it.rule;
					} else {
						cell = -it.rule;
					}
				}
			}
			if (accept) row[0] = ParseTables::kAccept;
			// bison: yypact is YYFLAG when the row has no non-default entries
			std::set<int> used;
			for (int a = 0; a < nt; a++) {
				if (row[a] < 0) used.insert(-row[a]);
			}
			const bool error_shift = row[t.error_term] > 0;
			if (!term_shift && !accept && reduce_rules.size() == 1) {
				t.needs_lookahead[size_t(si)] = 0;
				t.default_rule[size_t(si)] = *reduce_rules.begin();
			} else if (!error_shift && !used.empty()) {
				// the most frequent reduction becomes the default (first maximum in rule order)
				int best = 0, best_count = 0;
				for (int r : used) {
					int c = 0;
					for (int a = 0; a < nt; a++) c += row[a] == -r;
					if (c > best_count) best = r, best_count = c;
				}
				t.default_rule[size_t(si)] = best;
			}
		}
	}
};

}  // namespace

int ParseTables::term(int tok) const {
	if (tok <= 0) return 0;
	if (tok >= int(tok_to_term.size())) return undef_term;
	return tok_to_term[size_t(tok)];
}

const ParseTables& parse_tables() {
	static const ParseTables* tables = [] {
		Builder b;
		b.parse_grammar();
		b.compute_first();
		b.build_lr0();
		b.build_lalr();
		auto* t = new ParseTables;
		b.emit(*t);
		return t;
	}();
	return *tables;
}

std::string grammar_rule_text(int rule) { return rule >= 0 && rule < kNumRules ? kGrammar[rule] : "?"; }
int grammar_rule_count() { return kNumRules; }

}  // namespace ron::bhs
