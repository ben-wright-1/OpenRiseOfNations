// The BigHugeScript grammar (docs/script-spec.md section 2.1, rule numbers = the original bison rule numbers, which
// are the case labels of the parser's action switch) and the LALR(1) automaton built from it at start-up.
//
// The driver must reproduce bison 1.x's timing of lookahead reads, because the scanner classifies identifiers against
// symbol tables that the reductions fill: a state whose only action is one reduction (bison's yypact == YYFLAG) reduces
// without reading a token; every other state reads the lookahead first. Shift/reduce conflicts resolve to shift and
// reduce/reduce conflicts to the lower rule, as bison does without precedence declarations.
#pragma once

#include <string>
#include <vector>

namespace ron::bhs {

struct ParseTables {
	static constexpr int kAccept = 0x7fffffff;
	int nstates = 0, nterms = 0, nnonterms = 0;
	std::vector<int> action;           // nstates * nterms: > 0 shift to state, < 0 reduce rule, 0 none, kAccept
	std::vector<int> go;               // nstates * nnonterms: goto state (-1 none)
	std::vector<int> default_rule;     // per state: bison's yydefact (0 = error)
	std::vector<char> needs_lookahead; // per state: false when bison's yypact is YYFLAG
	std::vector<int> rule_lhs, rule_len;  // per rule (index 0 = $accept)
	std::vector<std::string> rule_text;
	int conflicts_sr = 0, conflicts_rr = 0;
	int error_term = 1, undef_term = 2;

	int term(int tok) const;  // token code -> terminal index (undef_term for codes the grammar does not use)
	int act(int state, int t) const { return action[size_t(state) * size_t(nterms) + size_t(t)]; }
	int goto_state(int state, int nonterm) const { return go[size_t(state) * size_t(nnonterms) + size_t(nonterm)]; }

	std::vector<int> tok_to_term;  // indexed by token code (0..0x1ff)
};

const ParseTables& parse_tables();

}  // namespace ron::bhs
