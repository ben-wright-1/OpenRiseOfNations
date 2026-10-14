// The BigHugeScript code generator: SyntaxNode::eval @9dc710 and its helpers (eval_binary_op @9df890, eval_unary_op
// @9dfb50, eval_unary_assign_op @9df6e0, auto_cast @9dfee0, optimize_pop @9dbdd0, resolve_func_call @9dc030,
// set_op_call_to @9dbee0, eval_struct_direct_access @9dfcb0, resolve_field_func_call @9dc470), docs/script-spec.md
// section 3. Every node builds its instruction list as it is reduced; lists are referenced by their tail and joined
// with OpCode::prepend. Creation order, line numbers (the scanner's line when an instruction is created, or the line
// of a child's instruction where the original passes one) and the list surgery follow the disassembly so the encoded
// image and its line table come out the same.
#include "script/bhs_compiler_internal.hpp"

namespace ron::bhs {

// OpCode::get_new @9c3140: the scanner's current line (MARKER: 0)
OpCode* Compiler::get_new(int op, OpArg a1, OpArg a2) {
	ops.emplace_back();
	OpCode* o = &ops.back();
	if (op == OP_MARKER) o->line = 0;
	else if (cur) o->line = uint16_t(cur->line_count);
	o->count = opcode_serial++;
	o->op = op;
	o->first = o;
	o->a1 = a1;
	o->a2 = a2;
	return o;
}

// OpCode::get_line_new @9c2d50 + OpCode::init @9c2cc0: an explicit line, 0 meaning the scanner's line
OpCode* Compiler::get_line_new(int op, int line, OpArg a1, OpArg a2) {
	ops.emplace_back();
	OpCode* o = &ops.back();
	uint16_t l = uint16_t(line);
	if (l == 0) {
		if (op == OP_MARKER) l = 0;
		else if (cur) l = uint16_t(cur->line_count);
	}
	o->line = l;
	o->count = opcode_serial++;
	o->op = op;
	o->first = o;
	o->a1 = a1;
	o->a2 = a2;
	return o;
}

void Compiler::prepend(OpCode* tail, OpCode* other) {
	if (!other) return;
	tail->first->prev = other;
	other->next = tail->first;
	tail->first = other->first;
}

Node* Compiler::init_new_node(int type, SymType* sym, std::initializer_list<Node*> children) {
	nodes.emplace_back();
	Node* n = &nodes.back();
	n->type = type;
	n->sym = sym;
	n->children.assign(children.begin(), children.end());
	eval(n);
	return n;
}

// SyntaxNode::auto_cast @9dfee0: converts the code of `src` to `dst`'s type. Constants are re-typed at compile time
// (the PUSH operand is replaced), other values get a CAST on the line of their last instruction.
bool Compiler::auto_cast(SymType* src, SymType* dst, OpCode** code) {
	if (!dst || !src) return false;
	SymType* target = data_type(dst);
	const int r = casts_to(src, target);
	if (r == 0) return true;
	if (r != 1) return false;
	if (src->kind == K_CONST) {
		OpCode* c = *code;
		if (!c) return false;
		if (c->a1.kind == 1 && c->a1.sym) c->a1.sym = add_const_type(cur->file_table, target->kind, src->token);
		return true;
	}
	if (!*code) return false;
	OpCode* cast = get_line_new(OP_CAST, (*code)->line, sym_arg(target));
	prepend(cast, *code);
	*code = cast;
	return true;
}

// SyntaxNode::optimize_pop @9dbdd0: a statement's value is dropped
OpCode* Compiler::optimize_pop(OpCode* tail) {
	if (tail->op == OP_PUSH) {
		tail->op = OP_MARKER;
		tail->a1 = OpArg();
		return tail;
	}
	if (tail->op == OP_MARKER && tail->prev && tail->prev->op == OP_PUSH) {
		tail->prev->op = OP_MARKER;
		tail->prev->a1 = OpArg();
		return tail;
	}
	OpCode* pop = get_new(OP_POP);
	pop->first->prev = tail;
	tail->next = pop->first;
	pop->first = tail->first;
	return pop;
}

// Compiler::set_loop_ops @9becd0 / set_switch_ops @9bec20
void Compiler::set_loop_ops() {
	cont_ops.push_back(get_new(OP_MARKER));
	break_ops.push_back(get_new(OP_MARKER));
}
void Compiler::set_switch_ops(SymType* t) {
	break_ops.push_back(get_new(OP_MARKER));
	switch_num_stack.push_back(0);
	switch_type_stack.push_back(data_type(t));
}

void Compiler::eval_binary_op(Node* n, int op, bool compare) {
	Node* a = n->children[0];
	Node* b = n->children[1];
	if (auto_cast(b->sym, a->sym, &b->opcode)) {
		n->sym = data_type(a->sym);
	} else if (auto_cast(a->sym, b->sym, &a->opcode)) {
		n->sym = data_type(b->sym);
	} else {
		comp_error("Can't convert \"" + to_utf8(get_name(b->sym)) + "\" to \"" + to_utf8(get_name(a->sym)) + "\"");
		n->sym = a->sym;
	}
	if (!accepts_op(a->sym, op) || !accepts_op(b->sym, op))
		comp_error(std::string("Can't use \"") + op_name(op) + "\" with type \"" + to_utf8(get_name(n->sym)) + "\"");
	n->opcode = get_new(op);
	prepend(n->opcode, b->opcode);
	prepend(n->opcode, a->opcode);
	if (compare) n->sym = t_bool;
}

void Compiler::eval_unary_op(Node* n, int op) {
	Node* a = n->children[0];
	n->sym = data_type(a->sym);
	if (!accepts_op(n->sym, op))
		comp_error(std::string("Can't use \"") + op_name(op) + "\" with type \"" + to_utf8(get_name(n->sym)) + "\"");
	n->opcode = get_new(op);
	prepend(n->opcode, a->opcode);
	if (op == OP_UNA_NOT) n->sym = t_bool;
}

void Compiler::eval_unary_assign_op(Node* n, int op) {
	Node* a = n->children[0];
	if (a->type != N_VAR) comp_error("Increment and decrement need a variable");
	SymType* t = data_type(a->sym);
	if (!accepts_op(t, op))
		comp_error(std::string("Can't use \"") + op_name(op) + "\" with type \"" + to_utf8(get_name(t)) + "\"");
	if (a->opcode && a->opcode->op == OP_PUSH_ARRAY_INDEX) a->opcode->op = OP_CREATE_ARRAY_INDEX;
	n->sym = t;
	n->opcode = get_new(op);
	prepend(n->opcode, a->opcode);
}

// SyntaxNode::eval_struct_direct_access @9dfcb0: `s.member` on a struct variable gives PUSH_STRUCT_FIELD member
int Compiler::eval_struct_direct_access(Node* n, SymType** member) {
	Node* a = n->children[0];
	SymType* t = data_type(a->sym);
	*member = nullptr;
	const bool var_like = (a->sym && a->sym->kind == K_VAR) || a->type == N_VAR;
	if (!var_like || (!is_struct(t) && !is_array(t))) {
		comp_error("\"" + to_utf8(get_name(a->sym)) + "\" is not a struct variable");
		return 1;
	}
	if (!is_struct(t)) return 0;
	for (SymType* m : t->members) {
		if (ieq(get_name(m), n->sym_string)) {
			if (!m || m->kind != K_VAR) return 0;
			n->opcode = get_new(OP_PUSH_STRUCT_FIELD, sym_arg(m));
			prepend(n->opcode, a->opcode);
			*member = m;
			return 0;
		}
	}
	return 0;
}

// SyntaxNode::resolve_field_func_call @9dc470: `s.field` without such a member calls get_<Struct>_<field>() /
// set_<Struct>_<field>(v) (no shipped script reaches this).
OpCode* Compiler::resolve_field_func_call(Node* n, const char* prefix, bool is_set) {
	Node* acc = n->children[0];  // the field access node: its symbol is the struct type, sym_string the field
	const U16 name = to_u16(prefix) + get_name(acc->sym) + u"_" + acc->sym_string;
	const BuiltinFunc* f = builtins ? builtins->find(name) : nullptr;
	if (!f) {
		comp_error("Unknown field \"" + to_utf8(acc->sym_string) + "\" (no function " + to_utf8(name) + ")");
		return get_new(OP_CALL_GAME, imm_arg(0));
	}
	if (!is_set) {
		if (!f->params.empty()) comp_error("Field function " + to_utf8(name) + " takes parameters");
	} else if (!(f->params.size() == 1 && f->params[0].type == (data_type(acc->sym) ? data_type(acc->sym)->kind : 0))) {
		comp_error("Field function " + to_utf8(name) + " has the wrong parameters");
	}
	return get_new(OP_CALL_GAME, imm_arg(uint32_t(f->index)));
}

// LocalScriptType::accepts_params @9da200 (0 exact, 1 convertible, 2 no). Scripts only call scripts of their kind.
int Compiler::script_accepts_params(SymType* fn, std::vector<Node*>& args) {
	if (args.size() > fn->params.size() || !cur_script || !ieq(cur_script->script_type, fn->script_type)) return 2;
	get_new(OP_ASSIGN);  // the original allocates (and drops) an instruction here
	bool conv = false;
	size_t i = 0;
	for (; i < args.size(); i++) {
		SymType* p = fn->params[i];
		const int r = casts_to(args[i]->sym, p);
		if (p && p->scope_modifier == kScopeRef) {
			if (r != 0) return 2;
			if (!args[i]->sym || args[i]->sym->kind != K_VAR) return 2;
		} else {
			if (r == 2) return 2;
			if (r == 1) conv = true;
		}
	}
	if (i < fn->param_defaults.size() && fn->param_defaults[i] == nullptr) return 2;
	return conv ? 1 : 0;
}

// ScriptFunc::accepts_params @9d4a70
int Compiler::builtin_accepts_params(const BuiltinFunc* f, std::vector<Node*>& args) {
	const size_t na = args.size(), np = f->params.size();
	if (np < na) {
		if (!f->varargs()) return 2;
	} else if (na < np) {
		if ((!f->varargs() || np - na > 1) /* && params[na].default_value empty */) return 2;
	}
	get_new(OP_ASSIGN);  // dropped instruction, as the original
	bool conv = false;
	size_t i = 0;
	for (; i < na; i++) {
		if (i >= np) break;
		const BuiltinParam& p = f->params[i];
		if (p.type == TY_VARARGS) break;
		if (p.type == TY_GENERIC) continue;
		SymType* at = data_type(args[i]->sym);
		int r;
		if (is_array(at)) {
			if (p.type != TY_ARRAY_ANY && p.type != at->kind) return 2;
			r = 0;
		} else if (is_struct(at)) {
			if (p.type != at->kind) return 2;
			r = 0;
		} else {
			SymType* pt = find_by_type(cur->file_table, p.type);
			if (!pt) return 2;
			r = casts_to(args[i]->sym, pt);
		}
		if (p.flags & 4) {
			if (r != 0) return 2;
			if (args[i]->type != N_VAR) return 2;
		} else {
			if (r == 2) return 2;
			if (r == 1) conv = true;
		}
	}
	if (!f->varargs() && i < np) return 2;
	return conv ? 1 : 0;
}

// SyntaxNode::set_op_call_to @9dbee0: arguments are prepended one by one, so argument 0 ends up on top.
void Compiler::set_op_call_to(Node* n, Candidate c, std::vector<Node*>& args, bool cast) {
	if (c.script) {
		SymType* fn = c.script;
		if (cur_script && fn->source_file != cur_script->source_file) {
			const int link = add_link(cur_script->source_file, fn->source_file);
			fn->usage = 2;
			n->opcode = get_new(OP_CALL_INCLUDE, sym_arg(fn), imm_arg(uint32_t(link)));
		} else {
			fn->usage = 1;
			n->opcode = get_new(OP_CALL, sym_arg(fn));
		}
		// LocalScriptType::make_params @9da0a0
		for (size_t i = 0; i < args.size(); i++) {
			if (cast && i < fn->params.size()) auto_cast(args[i]->sym, fn->params[i], &args[i]->opcode);
			if (args[i]->opcode) prepend(n->opcode, args[i]->opcode);
		}
		for (size_t i = args.size(); i < fn->params.size() && i < fn->param_defaults.size(); i++)
			prepend(n->opcode, get_new(OP_PUSH, sym_arg(fn->param_defaults[i])));
		n->sym = fn->return_type;
		return;
	}
	const BuiltinFunc* f = c.func;
	if (f->varargs()) n->opcode = get_new(OP_CALL_GAME_VARIED, imm_arg(uint32_t(f->index)), imm_arg(uint32_t(args.size())));
	else n->opcode = get_new(OP_CALL_GAME, imm_arg(uint32_t(f->index)));
	// ScriptFunc::make_params @9d4880
	for (size_t i = 0; i < args.size(); i++) {
		if (cast && i < f->params.size() && is_simple(args[i]->sym)) {
			SymType* pt = find_by_type(&root, f->params[i].type);
			if (pt) auto_cast(args[i]->sym, pt, &args[i]->opcode);
		}
		if (args[i]->opcode) prepend(n->opcode, args[i]->opcode);
	}
	for (size_t i = args.size(); i < f->params.size(); i++) {
		if (f->params[i].type == TY_VARARGS) continue;
		SymType* d = add_const_type(cur->file_table, f->params[i].type, U16());  // empty default_value
		prepend(n->opcode, get_new(OP_PUSH, sym_arg(d)));
	}
	n->sym = find_by_type(curr_table, f->return_type);
}

// SyntaxNode::resolve_func_call @9dc030: the script overload chain, then the builtin chain; one exact match wins
// (two: ambiguous), else a single convertible match is used with argument casts.
void Compiler::resolve_func_call(Node* n) {
	U16 name;
	SymType* script = (n->sym && n->sym->kind == K_FUNC) ? n->sym : nullptr;
	if (!script) {
		n->sym = nullptr;
		name = n->sym_string;
	} else {
		name = get_name(script);
	}
	const BuiltinFunc* f = builtins ? builtins->find(name) : nullptr;
	std::vector<Node*> args = n->children.empty() ? std::vector<Node*>() : n->children[0]->children;
	Candidate exact;
	std::vector<Candidate> conv;
	int rejected = 0;
	bool ambiguous = false;
	for (SymType* s = script; s;) {
		const int r = script_accepts_params(s, args);
		if (r == 0) {
			if (exact.script || exact.func) {
				ambiguous = true;
				break;
			}
			exact.script = s;
		} else if (r == 1) {
			conv.push_back({s, nullptr});
		} else {
			rejected++;
		}
		s = (s->next_overload && s->next_overload->kind == K_FUNC) ? s->next_overload : nullptr;
	}
	if (!ambiguous) {
		for (const BuiltinFunc* b = f; b; b = b->next_overload) {
			const int r = builtin_accepts_params(b, args);
			if (r == 0) {
				if (exact.script || exact.func) {
					ambiguous = true;
					break;
				}
				exact.func = b;
			} else if (r == 1) {
				conv.push_back({nullptr, b});
			} else {
				rejected++;
			}
		}
	}
	if (ambiguous) {
		comp_error("Ambiguous call to \"" + to_utf8(name) + "\"");
		n->opcode = get_new(OP_ERROR_TOKEN);
		n->sym = t_void;
		return;
	}
	if (exact.script || exact.func) {
		set_op_call_to(n, exact, args, false);
		return;
	}
	if (conv.size() == 1) {
		set_op_call_to(n, conv[0], args, true);
		return;
	}
	if (conv.size() >= 2) comp_error("Ambiguous call to \"" + to_utf8(name) + "\" (" + std::to_string(conv.size()) + " candidates)");
	else if (rejected) comp_error("Wrong arguments for \"" + to_utf8(name) + "\"");
	else comp_error("Unknown function \"" + to_utf8(name) + "\"");
	n->sym = t_void;
	n->opcode = get_new(OP_ERROR_TOKEN);
}

// SyntaxNode::eval @9dc710
void Compiler::eval(Node* n) {
	auto ch = [&](size_t i) { return n->children[i]; };
	switch (n->type) {
	case N_INC_OP: eval_unary_assign_op(n, OP_INC_OP); return;
	case N_DEC_OP: eval_unary_assign_op(n, OP_DEC_OP); return;
	case N_INC_OP_POST: eval_unary_assign_op(n, OP_INC_OP_POST); return;
	case N_DEC_OP_POST: eval_unary_assign_op(n, OP_DEC_OP_POST); return;
	case N_LE_OP: eval_binary_op(n, OP_LE_OP, true); return;
	case N_GE_OP: eval_binary_op(n, OP_GE_OP, true); return;
	case N_EQ_OP: eval_binary_op(n, OP_EQ_OP, true); return;
	case N_NE_OP: eval_binary_op(n, OP_NE_OP, true); return;
	case N_POW_OP: eval_binary_op(n, OP_POW_OP, false); return;
	case N_MUL: eval_binary_op(n, OP_MUL, false); return;
	case N_DIV: eval_binary_op(n, OP_DIV, false); return;
	case N_MOD: eval_binary_op(n, OP_MOD, false); return;
	case N_ADD: eval_binary_op(n, OP_ADD, false); return;
	case N_SUBT: eval_binary_op(n, OP_SUBT, false); return;
	case N_LESS: eval_binary_op(n, OP_LESS, true); return;
	case N_GREA: eval_binary_op(n, OP_GREA, true); return;
	case N_UNA_NEGA: eval_unary_op(n, OP_UNA_NEGA); return;
	case N_UNA_NOT: eval_unary_op(n, OP_UNA_NOT); return;

	case N_AND_OP:
	case N_OR_OP: {
		// a && b: [a] JUMP_IF_SC_FALSE L [b] L: CAST_BOOL; a chain shares one CAST_BOOL
		Node* a = ch(0);
		Node* b = ch(1);
		OpCode* at = a->opcode;
		if (at->op == OP_CAST_BOOL && a->type == n->type) {
			n->opcode = at;
			at->prev->first = at->first;
			n->opcode->prev->next = nullptr;
			a->opcode = n->opcode->prev;
			n->opcode->first = n->opcode;
			n->opcode->next = nullptr;
			n->opcode->prev = nullptr;
			if (b->opcode->op == OP_CAST_BOOL) b->opcode->op = OP_MARKER;
			prepend(n->opcode, b->opcode);
		} else if (b->opcode->op == OP_CAST_BOOL) {
			n->opcode = b->opcode;
		} else {
			n->opcode = get_line_new(OP_CAST_BOOL, b->opcode->line);
			prepend(n->opcode, b->opcode);
		}
		OpCode* j = get_line_new(n->type == N_OR_OP ? OP_JUMP_IF_SC_TRUE : OP_JUMP_IF_SC_FALSE, a->opcode->line,
		                         jump_arg(n->opcode));
		prepend(n->opcode, j);
		if (a->opcode->op == OP_CAST_BOOL) a->opcode->op = OP_MARKER;
		prepend(n->opcode, a->opcode);
		n->sym = t_bool;
		return;
	}

	case N_REF_ASSIGN: {  // x => y: PUSH y INIT x PUSH x
		SymType* var = (n->sym && n->sym->kind == K_VAR) ? n->sym : nullptr;
		SymType* src = ch(0)->sym;
		if (!var || !src || src->kind != K_VAR) comp_error("=> needs two variables");
		else if (var->scope_modifier == kScopeRef) comp_error("Can't rebind a ref parameter");
		else if (var->of_type != src->of_type)
			comp_error("Can't bind \"" + to_utf8(get_name(src->of_type)) + "\" to \"" + to_utf8(get_name(var->of_type)) + "\"");
		n->opcode = get_new(OP_PUSH, sym_arg(n->sym));
		prepend(n->opcode, get_new(OP_INIT, sym_arg(n->sym)));
		prepend(n->opcode, ch(0)->opcode);
		n->sym = t_void;
		return;
	}

	case N_ERROR:
		n->opcode = get_new(OP_ERROR_TOKEN);
		n->sym = t_int;
		return;

	case N_SCRIPT_BODY: {  // SCRIPT_MARKER s, parameters, body, [CREATE_SIMPLE R] RETURN
		SymType* fn = cur_script;
		if (!fn) {
			comp_error("Script body without a header");
			return;
		}
		if (fn->is_implemented) comp_error("Script \"" + to_utf8(get_name(fn)) + "\" already implemented");
		fn->is_implemented = true;
		fn->index = cur->file_table->script_counter;
		if (cur->script_table) cur->script_table->local_script_index = fn->index;
		cur->file_table->script_counter++;
		Node* body = ch(1);
		if (body->opcode && body->opcode->op == OP_RETURN) {
			n->opcode = body->opcode;
		} else {
			n->opcode = get_new(OP_RETURN);
			if (fn->return_type != t_void) {
				if (is_simple(fn->return_type)) {
					prepend(n->opcode, get_new(OP_CREATE_SIMPLE, sym_arg(fn->return_type)));
					comp_error("Script \"" + to_utf8(get_name(fn)) + "\" is missing a return value", true);
				} else {
					comp_error("Script \"" + to_utf8(get_name(fn)) + "\" must return a value");
				}
			}
			prepend(n->opcode, body->opcode);
		}
		n->sym = fn;
		prepend(n->opcode, ch(0)->opcode);
		fn->sym_table = cur->script_table;
		fn->address = ch(0)->opcode ? ch(0)->opcode->first : nullptr;
		cur_script = nullptr;
		curr_table = cur->file_table;
		cur->script_table = nullptr;
		return;
	}

	case N_VAR:
	case N_CONST:
		n->opcode = get_new(OP_PUSH, sym_arg(n->sym));
		return;

	case N_TRIGGER: {  // JUMP_IF_BITSET t, L [c] JUMP_IF_NOT L BIT_SET t [B] L:
		n->opcode = get_new(OP_MARKER);
		SymType* tr = n->sym;
		const int tline = tr ? tr->line : 0;
		if (n->children.size() == 2) {
			prepend(n->opcode, ch(1)->opcode);
			const int cline = ch(0)->opcode ? ch(0)->opcode->first->line : 0;
			prepend(n->opcode, get_line_new(OP_BIT_SET, cline, sym_arg(tr)));
			prepend(n->opcode, get_line_new(OP_JUMP_IF_NOT, cline, jump_arg(n->opcode)));
			prepend(n->opcode, ch(0)->opcode);
		} else {
			prepend(n->opcode, ch(0)->opcode);
			prepend(n->opcode, get_line_new(OP_BIT_SET, tline, sym_arg(tr)));
		}
		prepend(n->opcode, get_line_new(OP_JUMP_IF_BITSET, tline, sym_arg(tr), jump_arg(n->opcode)));
		n->sym = nullptr;
		return;
	}

	case N_EVAL:
	case N_STMT:
		n->opcode = optimize_pop(ch(0)->opcode);
		n->sym = t_void;
		return;

	case N_CASE: {  // the clause's code; the dispatch (CASE k, B or POP JUMP B) goes on case_stack
		n->opcode = n->children.size() == 2 ? ch(1)->opcode : get_new(OP_MARKER);
		OpCode* head = n->opcode->first;
		if (ch(0)->sym) {
			OpCode* c = get_new(OP_CASE, sym_arg(ch(0)->sym), jump_arg(head));
			case_stack.push_back(c);
		} else {
			OpCode* j = get_new(OP_JUMP, jump_arg(head));
			OpCode* p = get_line_new(OP_POP, 1);
			prepend(j, p);
			case_stack.push_back(j);
		}
		return;
	}

	case N_COND_EXP: {  // as if/else with values
		const bool c1 = auto_cast(ch(2)->sym, ch(1)->sym, &ch(2)->opcode);
		bool ok = c1;
		if (!c1) ok = auto_cast(ch(1)->sym, ch(2)->sym, &ch(1)->opcode);
		else n->sym = data_type(ch(1)->sym);
		if (!ok) comp_error("Can't convert \"" + to_utf8(get_name(ch(2)->sym)) + "\" to \"" + to_utf8(get_name(ch(1)->sym)) + "\"");
		else if (!c1) n->sym = data_type(ch(2)->sym);
	}
		[[fallthrough]];
	case N_IF: {  // [c] JUMP_IF_NOT E [A] JUMP L E: [B] L:
		if (casts_to(ch(0)->sym, t_bool) == 2)
			comp_error("Can't evaluate \"" + to_utf8(get_name(data_type(ch(0)->sym))) + "\" as a bool");
		n->opcode = get_new(OP_MARKER);
		OpCode* target = n->opcode;
		if (n->children.size() == 3) target = ch(2)->opcode->first;
		OpCode* jin = get_line_new(OP_JUMP_IF_NOT, ch(0)->opcode->line, jump_arg(target));
		if (n->children.size() == 3) {
			prepend(n->opcode, ch(2)->opcode);
			prepend(n->opcode, get_line_new(OP_JUMP, ch(1)->opcode->first->line, jump_arg(n->opcode)));
		}
		prepend(n->opcode, ch(1)->opcode);
		prepend(n->opcode, jin);
		prepend(n->opcode, ch(0)->opcode);
		return;
	}

	case N_SWITCH: {
		int count = 0;
		if (!switch_num_stack.empty()) {
			count = switch_num_stack.back();
			switch_num_stack.pop_back();
		}
		n->opcode = break_ops.back();
		break_ops.pop_back();
		prepend(n->opcode, ch(1)->opcode);
		const uint16_t hline = ch(0)->opcode ? ch(0)->opcode->first->line : 0;
		if (count) {
			if (case_stack.empty() || case_stack.back()->op != OP_JUMP) {
				OpCode* j = get_line_new(OP_JUMP, hline, jump_arg(n->opcode));
				OpCode* p = get_line_new(OP_POP, hline);
				prepend(j, p);
				prepend(n->opcode, j);
			}
			for (int k = 0; k < count && !case_stack.empty(); k++) {
				OpCode* e = case_stack.back();
				case_stack.pop_back();
				e->line = hline;
				prepend(n->opcode, e);
			}
		}
		prepend(n->opcode, ch(0)->opcode);
		if (!switch_type_stack.empty()) switch_type_stack.pop_back();
		return;
	}

	case N_WHILE: {  // L_cont: [c] JUMP_IF_NOT L_brk [A] JUMP c L_brk:
		if (casts_to(ch(0)->sym, t_bool) == 2)
			comp_error("Can't evaluate \"" + to_utf8(get_name(data_type(ch(0)->sym))) + "\" as a bool");
		n->opcode = break_ops.back();
		break_ops.pop_back();
		prepend(n->opcode, get_new(OP_JUMP, jump_arg(ch(0)->opcode->first)));
		prepend(n->opcode, ch(1)->opcode);
		prepend(n->opcode, get_line_new(OP_JUMP_IF_NOT, ch(0)->opcode->line, jump_arg(n->opcode)));
		prepend(n->opcode, ch(0)->opcode);
		OpCode* cont = cont_ops.back();
		cont_ops.pop_back();
		prepend(n->opcode, cont);
		return;
	}

	case N_DO: {  // [A] L_cont: [c] JUMP_IF A L_brk:
		if (casts_to(ch(1)->sym, t_bool) == 2)
			comp_error("Can't evaluate \"" + to_utf8(get_name(data_type(ch(1)->sym))) + "\" as a bool");
		n->opcode = break_ops.back();
		break_ops.pop_back();
		prepend(n->opcode, get_line_new(OP_JUMP_IF, ch(1)->opcode->line, jump_arg(ch(0)->opcode->first)));
		prepend(n->opcode, ch(1)->opcode);
		OpCode* cont = cont_ops.back();
		cont_ops.pop_back();
		prepend(n->opcode, cont);
		prepend(n->opcode, ch(0)->opcode);
		return;
	}

	case N_FOR: {
		if (casts_to(ch(1)->sym, t_bool) == 2)
			comp_error("Can't evaluate \"" + to_utf8(get_name(data_type(ch(1)->sym))) + "\" as a bool");
		n->opcode = break_ops.back();
		break_ops.pop_back();
		if (n->children.size() == 4) {
			// [i] JUMP C S: [s] POP C: [c] JUMP_IF_NOT L_brk [A] JUMP S L_brk:  (cont_ops is not popped)
			prepend(n->opcode, get_new(OP_JUMP, jump_arg(ch(2)->opcode->first)));
			prepend(n->opcode, ch(3)->opcode);
			prepend(n->opcode, get_line_new(OP_JUMP_IF_NOT, ch(1)->opcode->line, jump_arg(n->opcode)));
			prepend(n->opcode, ch(1)->opcode);
			prepend(n->opcode, get_line_new(OP_POP, ch(2)->opcode->line));
			prepend(n->opcode, ch(2)->opcode);
			prepend(n->opcode, get_line_new(OP_JUMP, ch(0)->opcode->line, jump_arg(ch(1)->opcode->first)));
			prepend(n->opcode, ch(0)->opcode);
		} else {
			// [i] C: [c] JUMP_IF_NOT L_brk [A] JUMP C L_brk:
			prepend(n->opcode, get_line_new(OP_JUMP, ch(2)->opcode->line, jump_arg(ch(1)->opcode->first)));
			prepend(n->opcode, ch(2)->opcode);
			prepend(n->opcode, get_line_new(OP_JUMP_IF_NOT, ch(1)->opcode->line, jump_arg(n->opcode)));
			prepend(n->opcode, ch(1)->opcode);
			prepend(n->opcode, ch(0)->opcode);
		}
		return;
	}

	case N_CONTINUE:
	case N_BREAK: {
		std::vector<OpCode*>& st = n->type == N_BREAK ? break_ops : cont_ops;
		if (!st.empty()) {
			n->opcode = get_new(OP_JUMP, jump_arg(st.back()));
		} else {
			n->opcode = get_new(OP_ERROR_TOKEN);
			comp_error(n->type == N_BREAK ? "break outside a loop or switch" : "continue outside a loop");
		}
		return;
	}

	case N_RETURN: {
		n->opcode = get_new(OP_RETURN);
		SymType* rt = cur_script ? cur_script->return_type : t_void;
		if (n->children.size() == 1) {
			if (!auto_cast(ch(0)->sym, rt, &ch(0)->opcode))
				comp_error("Can't convert \"" + to_utf8(get_name(ch(0)->sym)) + "\" to \"" + to_utf8(get_name(rt)) + "\"");
			prepend(n->opcode, ch(0)->opcode);
		} else if (rt != t_void) {
			comp_error("Script must return a \"" + to_utf8(get_name(rt)) + "\"");
		}
		return;
	}

	case N_NOP:
		n->opcode = get_new(OP_MARKER);
		return;

	case N_MASTER_ROOT:
		n->opcode = ch(0)->opcode;
		return;

	case N_GENERIC_LIST:
		if (n->children.empty()) return;
		n->opcode = ch(0)->opcode;
		n->sym = ch(0)->sym;
		if (n->sym) n->sym = data_type(n->sym);
		return;

	case N_TRIGGER_TOGGLE: {  // enable: BIT_UNSET t, disable: BIT_SET t (by index now, or by name at count_code)
		SymType* tg = (n->sym && n->sym->kind == K_TOGGLE) ? n->sym : nullptr;
		if (!tg) return;
		const int op = tg->is_enable ? OP_BIT_UNSET : OP_BIT_SET;
		if (tg->trigger) n->opcode = get_new(op, imm_arg(uint32_t(tg->trigger->index)));
		else n->opcode = get_new(op, sym_arg(tg));
		n->sym = nullptr;
		return;
	}

	case N_ARRAY_INITER: {  // [en] .. [e0] CREATE_ARRAY_INITER n, T[]
		Node* list = ch(0);
		if (!list->sym) {
			comp_error("Empty array initializer");
			n->sym = t_void;
			return;
		}
		n->sym = data_type(list->sym);
		for (size_t i = 1; i < list->children.size(); i++) {
			if (data_type(list->children[i]->sym) != n->sym) comp_error("Array initializer elements must have the same type");
		}
		n->sym = add_array_type(cur->file_table, n->sym);
		n->opcode = get_new(OP_CREATE_ARRAY_INITER, imm_arg(uint32_t(list->children.size())), sym_arg(n->sym));
		prepend(n->opcode, list->opcode);
		if (list->children.empty()) comp_error("Empty array initializer");
		return;
	}

	case N_STRUCT_INITER: {  // T(e0, ...)
		SymType* st = n->sym;
		Node* list = ch(0);
		if (!is_struct(st)) {
			if (!(is_array(st) && list->children.empty()))
				comp_error("\"" + to_utf8(get_name(st)) + "\" can't be constructed");
			n->opcode = create_opcode(st, lex_line());
			return;
		}
		if (list->children.empty()) {
			n->opcode = create_opcode(st, lex_line());
			return;
		}
		n->opcode = get_new(OP_CREATE_STRUCT, imm_arg(uint32_t(st->members.size())), sym_arg(st));
		prepend(n->opcode, list->opcode);
		if (list->children.size() != st->members.size()) comp_error("Wrong number of struct members");
		for (size_t i = 0; i < list->children.size() && i < st->members.size(); i++) {
			if (data_type(st->members[i]) != data_type(list->children[i]->sym)) comp_error("Struct member type mismatch");
		}
		return;
	}

	case N_STRUCT_ACCESS: {  // `postfix . IDENTIFIER`
		if (n->sym && n->sym->kind == K_SCRIPT_KIND) {
			n->sym_string = n->sym->token;
			n->sym = nullptr;
		}
		SymType* member = nullptr;
		if (!is_array(data_type(ch(0)->sym))) {
			eval_struct_direct_access(n, &member);
			if (member) {
				n->sym = member;
				return;
			}
		}
		n->sym = data_type(ch(0)->sym);
		n->opcode = ch(0)->opcode;
		return;
	}

	case N_STRUCT_GET: {  // read of a field: member, array length, or get_<Struct>_<field>()
		Node* a = ch(0);
		if (a->sym && a->sym->kind == K_VAR) {
			n->sym = a->sym;
			n->opcode = a->opcode;
			return;
		}
		if (is_array(a->sym)) {
			n->opcode = get_new(OP_PUSH_ARRAY_LENGTH);
			prepend(n->opcode, a->opcode);
			n->sym = t_int;
			return;
		}
		n->opcode = resolve_field_func_call(n, "get_", false);
		n->sym = data_type(a->sym);
		return;
	}

	case N_ARRAY_INDEX: {  // [a] [i] PUSH_ARRAY_INDEX (a nested index becomes CREATE_ARRAY_INDEX)
		Node* a = ch(0);
		SymType* t = data_type(a->sym);
		const bool var_like = (a->sym && a->sym->kind == K_VAR) || a->type == N_VAR;
		if (!var_like || !is_array(t)) comp_error("Array [] index may only be used on an array variable");
		if (a->opcode && a->opcode->op == OP_PUSH_ARRAY_INDEX) a->opcode->op = OP_CREATE_ARRAY_INDEX;
		n->opcode = get_new(OP_PUSH_ARRAY_INDEX);
		prepend(n->opcode, ch(1)->opcode);
		prepend(n->opcode, a->opcode);
		n->type = N_VAR;
		n->sym = t ? t->of_type : nullptr;
		return;
	}

	case N_INIT_DECL: {  // T v = e
		SymType* v = n->sym;
		Node* init = ch(0);
		if (v->of_type == t_void) {
			v->of_type = data_type(init->sym);
			if (v->of_type == t_void) comp_error("Can't determine the type of \"" + to_utf8(v->token) + "\"");
		} else if (!auto_cast(init->sym, v, &init->opcode)) {
			comp_error("Can't convert \"" + to_utf8(get_name(init->sym)) + "\" to \"" + to_utf8(get_name(v->of_type)) + "\"");
		}
		if (init->type == N_FINISH_DECL) {
			n->opcode = get_new(OP_PUSH, sym_arg(init->sym));
			prepend(n->opcode, init->opcode);
		} else {
			n->opcode = init->opcode;
		}
		return;
	}

	case N_NON_INIT_DECL: {  // T v: the type's create sequence
		if (n->sym->of_type == t_void) n->sym->of_type = t_int;
		n->opcode = create_opcode(n->sym, lex_line());
		return;
	}

	case N_ARRAY_SIZED_DECL: {  // T v(n): <create elem> [n] CREATE_ARRAY_DYN T[]
		if (n->sym->of_type == t_void) n->sym->of_type = t_int;
		SymType* at = data_type(n->sym);
		SymType* elem = at ? at->of_type : nullptr;
		n->opcode = get_new(OP_CREATE_ARRAY_DYN, sym_arg(at));
		prepend(n->opcode, ch(0)->opcode);
		if (elem) prepend(n->opcode, create_opcode(elem, lex_line()));
		return;
	}

	case N_FINISH_DECL: {  // [code] INIT_COPY v (INIT for ref / no initializer); statics: JUMP_IF_INITED v, L ... L:
		Node* d = ch(0);
		int op = OP_INIT;
		if (d->type == N_INIT_DECL) {
			if (curr_scope == kScopeRef) {
				SymType* src = d->children.empty() ? nullptr : data_type(d->children[0]->sym);
				if (data_type(d->sym) != src)
					comp_error("Can't bind \"" + to_utf8(get_name(src)) + "\" to \"" + to_utf8(get_name(data_type(d->sym))) + "\"");
				op = OP_INIT;
				curr_scope = 0;
			} else {
				op = OP_INIT_COPY;
			}
		}
		OpCode* init = get_new(op, sym_arg(d->sym));
		prepend(init, d->opcode);
		if (!d->sym || d->sym->kind != K_VAR) {
			comp_error("Declaration of a non-variable");
			return;
		}
		if (d->sym->scope_modifier == kScopeStatic) {
			n->opcode = get_new(OP_MARKER);
			prepend(n->opcode, init);
			const int line = d->opcode ? d->opcode->line : 0;
			prepend(n->opcode, get_line_new(OP_JUMP_IF_INITED, line, sym_arg(d->sym), jump_arg(n->opcode)));
		} else {
			n->opcode = init;
		}
		n->sym = d->sym;
		curr_table->table.push_back(d->sym);
		return;
	}

	case N_PARAM_DECL: {  // INIT_COPY p (INIT for ref)
		if (n->sym->of_type == t_void) n->sym->of_type = t_int;
		n->opcode = get_new(curr_scope == kScopeRef ? OP_INIT : OP_INIT_COPY, sym_arg(n->sym));
		if (n->sym->kind != K_VAR) comp_error("Parameter is not a variable");
		curr_table->table.push_back(n->sym);
		return;
	}

	case N_SCRIPT_DECL: {  // a forward declaration: its parameter code and table are dropped
		n->opcode = get_new(OP_MARKER);
		if (cur->script_table) {
			SymTable* st = cur->script_table;
			if (SymTable* p = st->parent) {
				for (size_t i = 0; i < p->children.size(); i++) {
					if (p->children[i] == st) {
						p->children.erase(p->children.begin() + long(i));
						break;
					}
				}
			}
			st->table.clear();
		}
		curr_table = cur->file_table;
		cur->script_table = nullptr;
		cur_script = nullptr;
		return;
	}

	case N_ASSIGN_TO_VAR: {  // [e] PUSH v OP
		Node* e = ch(0);
		if (!accepts_op(n->sym, last_op) || !accepts_op(e->sym, last_op))
			comp_error(std::string("Can't use \"") + op_name(last_op) + "\" with type \"" + to_utf8(get_name(e->sym)) + "\"");
		if (!auto_cast(e->sym, n->sym, &e->opcode))
			comp_error("Can't convert \"" + to_utf8(get_name(e->sym)) + "\" to \"" + to_utf8(get_name(n->sym)) + "\"");
		n->opcode = get_new(last_op);
		prepend(n->opcode, get_new(OP_PUSH, sym_arg(n->sym)));
		prepend(n->opcode, e->opcode);
		return;
	}

	case N_ASSIGN_TO_ARRAY_INDEX: {  // [e] [a] [i] CREATE_ARRAY_INDEX OP
		Node* idx = ch(0);
		Node* e = ch(1);
		if (!accepts_op(idx->sym, last_op) || !accepts_op(e->sym, last_op))
			comp_error(std::string("Can't use \"") + op_name(last_op) + "\" with type \"" + to_utf8(get_name(e->sym)) + "\"");
		if (!auto_cast(e->sym, idx->sym, &e->opcode))
			comp_error("Can't convert \"" + to_utf8(get_name(e->sym)) + "\" to \"" + to_utf8(get_name(idx->sym)) + "\"");
		if (idx->opcode && idx->opcode->op == OP_PUSH_ARRAY_INDEX) idx->opcode->op = OP_CREATE_ARRAY_INDEX;
		n->opcode = get_new(last_op);
		prepend(n->opcode, idx->opcode);
		prepend(n->opcode, e->opcode);
		return;
	}

	case N_ASSIGN_TO_STRUCT_FIELD: {
		Node* f = ch(0);
		Node* e = ch(1);
		if (!accepts_op(f->sym, last_op) || !accepts_op(e->sym, last_op))
			comp_error(std::string("Can't use \"") + op_name(last_op) + "\" with type \"" + to_utf8(get_name(e->sym)) + "\"");
		if (f->sym && f->sym->kind == K_VAR) {  // direct member: [e] [s] PUSH_STRUCT_FIELD OP
			if (!auto_cast(e->sym, f->sym, &e->opcode))
				comp_error("Can't convert \"" + to_utf8(get_name(e->sym)) + "\" to \"" + to_utf8(get_name(f->sym)) + "\"");
			n->opcode = get_new(last_op);
			prepend(n->opcode, f->opcode);
			prepend(n->opcode, e->opcode);
			return;
		}
		if (is_array(f->sym)) {  // a.length = n: [n] [a] SET_ARRAY_LENGTH
			if (!auto_cast(e->sym, t_int, &e->opcode))
				comp_error("Can't convert \"" + to_utf8(get_name(e->sym)) + "\" to \"int\"");
			n->opcode = get_new(OP_SET_ARRAY_LENGTH);
			prepend(n->opcode, f->opcode);
			prepend(n->opcode, e->opcode);
			return;
		}
		// set_<Struct>_<field>(s, e); compound operators wrap a get_ call
		n->opcode = resolve_field_func_call(n, "set_", true);
		if (last_op != 0) {
			prepend(n->opcode, get_new(last_op));
			prepend(n->opcode, resolve_field_func_call(n, "get_", false));
		}
		prepend(n->opcode, e->opcode);
		return;
	}

	case N_CAST: {  // [a] CAST T (constants re-typed, nothing when already T)
		Node* a = ch(0);
		if (auto_cast(a->sym, n->sym, &a->opcode)) {
			n->opcode = a->opcode;
			return;
		}
		if (is_simple(data_type(a->sym)) && is_simple(n->sym)) {
			n->opcode = get_new(OP_CAST, sym_arg(n->sym));
			prepend(n->opcode, a->opcode);
		} else {
			comp_error("Can't cast \"" + to_utf8(get_name(data_type(a->sym))) + "\" to \"" + to_utf8(get_name(n->sym)) + "\"");
		}
		return;
	}

	case N_CASE_HEADER: {
		int count = switch_num_stack.empty() ? 0 : switch_num_stack.back();
		if (n->sym) {
			n->opcode = nullptr;
			SymType* st = switch_type_stack.empty() ? nullptr : switch_type_stack.back();
			if (!auto_cast(n->sym, st, &n->opcode))
				comp_error("Can't convert \"" + to_utf8(get_name(n->sym)) + "\" to \"" + to_utf8(get_name(st)) + "\"");
			bool dup = false;
			for (int k = 0; k < count && k < int(case_stack.size()); k++) {
				OpCode* c = case_stack[case_stack.size() - size_t(count) + size_t(k)];
				if (c->a1.kind == 1 && c->a1.sym == n->sym) dup = true;
			}
			if (dup) comp_error("Duplicate case \"" + to_utf8(get_name(n->sym)) + "\"");
			else if (!case_stack.empty() && case_stack.back()->op == OP_JUMP) comp_error("case after default");
		} else if (count && !case_stack.empty() && case_stack.back()->op == OP_JUMP) {
			comp_error("Multiple defaults");
		}
		if (!switch_num_stack.empty()) switch_num_stack.back()++;
		return;
	}

	case N_STMT_BLOCK:
		if (n->children.size() == 1) n->opcode = ch(0)->opcode;
		else n->opcode = get_new(OP_MARKER);
		return;

	case N_SCRIPT_HEADER:  // SCRIPT_MARKER s, then the parameters' INIT_COPYs
		n->opcode = ch(0)->opcode;
		prepend(n->opcode, get_new(OP_SCRIPT_MARKER, sym_arg(n->sym)));
		cur_script = (n->sym && n->sym->kind == K_FUNC) ? n->sym : nullptr;
		return;

	case N_FUNC_CALL: {
		if (n->sym && n->sym->kind == K_SCRIPT_KIND) {  // IDENTIFIER name wrapped by the parser
			n->sym_string = n->sym->token;
			n->sym = nullptr;
		}
		if (n->children.size() > 1) {  // x.f(args): x becomes argument 0
			Node* obj = ch(1);
			const bool ok = ((obj->sym && obj->sym->kind == K_VAR) || obj->type == N_VAR) && is_complex(obj->sym);
			if (!ok) {
				comp_error("Method calls need an array or struct variable");
				resolve_func_call(n);
				return;
			}
			ch(0)->children.insert(ch(0)->children.begin(), obj);
			n->children.pop_back();
		}
		resolve_func_call(n);
		return;
	}

	case N_COMPLETE_FILE: {
		if (n->children.empty()) {
			n->opcode = get_new(OP_ERROR_TOKEN);
			comp_error("Empty file");
		} else {
			n->opcode = ch(0)->opcode;
		}
		master_root = n;
		if (cur) {
			write_file(cur);
			close_file();
		}
		return;
	}

	default:  // node types without code (bitwise operators, ~, shifts, parameter defaults): keep the child's symbol
		n->sym = (!n->children.empty() && n->children[0]->sym) ? n->children[0]->sym : nullptr;
		return;
	}
}

}  // namespace ron::bhs
