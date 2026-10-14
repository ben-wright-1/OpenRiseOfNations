// Symbol tables and types of the BigHugeScript compiler (SymTable @9d8c80.., SymType and subclasses @9da0a0..9dbd80)
// and the compiled-image writer (LexerFileEntry::write_file @9c05d0, OpCode::count_code @9c2ed0, SymTable::write
// @9d8da0 and the per-symbol write methods). docs/script-spec.md sections 2.4, 3.5 and 4.
#include <cstring>

#include "script/bhs_compiler_internal.hpp"

namespace ron::bhs {

// ---------------------------------------------------------------- chunk writer

void ChunkWriter::grow(size_t n) {
	for (size_t off : stack_) {
		uint32_t v;
		std::memcpy(&v, data.data() + off, 4);
		v += uint32_t(n);
		std::memcpy(data.data() + off, &v, 4);
	}
}
void ChunkWriter::begin(uint16_t id) {
	if (!stack_.empty()) {  // the parent's aux counts its children
		uint16_t aux;
		std::memcpy(&aux, data.data() + stack_.back() + 6, 2);
		aux++;
		std::memcpy(data.data() + stack_.back() + 6, &aux, 2);
	}
	const size_t off = data.size();
	stack_.push_back(off);
	const uint8_t hdr[8] = {0, 0, 0, 0, uint8_t(id), uint8_t(id >> 8), 0, 0};
	data.insert(data.end(), hdr, hdr + 8);
	grow(8);
}
void ChunkWriter::end() {
	if (!stack_.empty()) stack_.pop_back();
}
void ChunkWriter::bytes(const void* p, size_t n) {
	const uint8_t* b = static_cast<const uint8_t*>(p);
	data.insert(data.end(), b, b + n);
	grow(n);
}
void ChunkWriter::u32(uint32_t v) {
	const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
	bytes(b, 4);
}
void ChunkWriter::f32(float v) {
	uint32_t u;
	std::memcpy(&u, &v, 4);
	u32(u);
}
void ChunkWriter::u8(uint8_t v) { bytes(&v, 1); }
void ChunkWriter::str(const U16& s) {
	u32(uint32_t(s.size()));
	for (char16_t c : s) {
		const uint8_t b[2] = {uint8_t(c), uint8_t(c >> 8)};
		bytes(b, 2);
	}
}

// ---------------------------------------------------------------- symbols

SymType* Compiler::new_sym(SymType::Cls cls) {
	syms.emplace_back();
	syms.back().cls = cls;
	return &syms.back();
}

SymTable* Compiler::new_table() {
	tables.emplace_back();
	return &tables.back();
}

// SymTable::init_root @9d8c80: the five data types, then the three script kinds (StringWrapType, kind 0x121).
void Compiler::init_root() {
	root = SymTable();
	const struct {
		const char16_t* name;
		int id;
	} types[] = {{u"int", TY_INT}, {u"float", TY_FLOAT}, {u"string", TY_STRING}, {u"void", TY_VOID}, {u"bool", TY_BOOL}};
	for (auto& t : types) {
		SymType* s = new_sym(SymType::Cls::Data);
		s->token = t.name;
		s->kind = t.id;
		s->dont_delete = true;
		root.table.push_back(s);
	}
	t_int = root.table[0];
	t_float = root.table[1];
	t_string = root.table[2];
	t_void = root.table[3];
	t_bool = root.table[4];
	for (const char16_t* k : {u"scenario", u"conquest", u"ai"}) root.table.push_back(string_wrap(k));
}

SymType* Compiler::string_wrap(const U16& s) {
	SymType* w = new_sym(SymType::Cls::Wrap);
	w->kind = K_SCRIPT_KIND;
	w->token = s;
	return w;
}

// SymTable::find_by_token @9d8e90 (case-insensitive; at the current file's table also its includes)
SymType* Compiler::find_by_token(SymTable* t, const U16& name, SymTable** found_in) {
	for (; t; t = t->parent) {
		if (cur && t == cur->file_table) {
			if (SymType* s = find_by_token_recurse_include(t, name, found_in)) return s;
			continue;
		}
		for (SymType* s : t->table) {
			if (ieq(s->token, name)) {
				if (found_in) *found_in = t;
				return s;
			}
		}
	}
	return nullptr;
}

SymType* Compiler::find_by_token_recurse_include(SymTable* t, const U16& name, SymTable** found_in) {
	for (SymType* s : t->table) {
		if (ieq(s->token, name)) {
			if (found_in) *found_in = t;
			return s;
		}
	}
	if (t->file_entry) {
		for (FileEntry* inc : t->file_entry->local_includes) {
			if (SymType* s = find_by_token_recurse_include(inc->file_table, name, found_in)) return s;
		}
	}
	return nullptr;
}

SymType* Compiler::find_by_type(SymTable* t, int type_id) {
	for (; t; t = t->parent) {
		if (cur && t == cur->file_table) {
			for (SymType* s : t->table) {
				if (s->kind == type_id) return s;
			}
			if (t->file_entry) {
				for (FileEntry* inc : t->file_entry->local_includes) {
					if (SymType* s = find_by_type_recurse_include(inc->file_table, type_id)) return s;
				}
			}
			continue;
		}
		for (SymType* s : t->table) {
			if (s->kind == type_id) return s;
		}
	}
	return nullptr;
}

SymType* Compiler::find_by_type_recurse_include(SymTable* t, int type_id) {
	for (SymType* s : t->table) {
		if (s->kind == type_id) return s;
	}
	if (t->file_entry) {
		for (FileEntry* inc : t->file_entry->local_includes) {
			if (SymType* s = find_by_type_recurse_include(inc->file_table, type_id)) return s;
		}
	}
	return nullptr;
}

// SymTable::add_child_table @9da010. The original copies the parent's local_script_index into the *parent* (not the
// child), which is why block-level variable names are recorded under a stale script index (section 4.4).
SymTable* Compiler::add_child_table(SymTable* t, int flag) {
	SymTable* c = new_table();
	c->parent = t;
	c->type = (flag == 0 || t->type > 0xffff) ? t->type + 1 : 0x10000;
	t->children.push_back(c);
	if (t->parent) t->local_script_index = t->parent->local_script_index;
	return c;
}

// SymTable::add_const_type @9d9d10: constants are pooled by canonical token, found case-insensitively.
SymType* Compiler::add_const_type(SymTable* t, int type, const U16& text) {
	U16 tok = text;
	SymType* data = nullptr;
	if (type == TY_CHAR || type == TY_BOOL || type == TY_INT) {
		if (type == TY_CHAR) tok = itow_text(text.empty() ? 0 : int32_t(text[0]));
		else if (type == TY_BOOL) tok = text.empty() ? u"0" : u"1";
		tok = itow_text(convert_int(tok));
		SymType* f = find_by_token(t, tok);
		if (f && f->kind == K_CONST) return f;
		data = t_int;
	} else if (type == TY_FLOAT) {
		tok = gcvt_text(get_double(text));
		SymType* f = find_by_token(t, tok);
		if (f && f->kind == K_CONST) return f;
		data = t_float;
	} else if (type == TY_STRING) {
		tok = u"\"" + text;
		SymType* f = find_by_token(t, tok);
		if (f && f->kind == K_CONST) return f;
		data = t_string;
	} else {
		comp_error("Unknown constant type");
	}
	SymType* c = new_sym(SymType::Cls::Const);
	c->token = tok;
	c->of_type = data;
	c->kind = K_CONST;
	c->index = -1;
	t->table.push_back(c);
	return c;
}

// SymTable::add_array_type @9d9440
SymType* Compiler::add_array_type(SymTable* t, SymType* elem) {
	if (elem == t_void) elem = t_int;
	const U16 tok = u"@" + unique_token(elem);
	SymType* f = find_by_token(t, tok);
	if (f && is_array(f)) return f;
	SymType* a = new_sym(SymType::Cls::Array);
	a->token = tok;
	a->of_type = elem;
	a->kind = int(type_hash(tok));
	a->index = -1;
	t->table.push_back(a);
	return a;
}

// SymTable::add_struct_type @9d91f0 (struct NAME { ... })
SymType* Compiler::add_struct_type(SymTable* t, const U16& name, Node* members, SymTable* member_table) {
	SymType* st = new_sym(SymType::Cls::Struct);
	curr_table = curr_table->parent;
	member_table->type = 0x10000;
	if (SymTable* p = member_table->parent) {
		for (size_t i = 0; i < p->children.size(); i++) {
			if (p->children[i] == member_table) {
				p->children.erase(p->children.begin() + long(i));
				break;
			}
		}
	}
	member_table->parent = nullptr;
	init_struct(st, name, members, member_table);
	t->table.push_back(st);
	return st;
}

// SymTable::add_struct_type @9d9300 (struct TYPE_NAME { ... }: a redefinition must match)
SymType* Compiler::add_struct_type_existing(SymTable*, SymType* st, Node* members, SymTable*) {
	curr_table = curr_table->parent;
	if (!is_struct(st)) {
		comp_error("\"" + to_utf8(get_name(st)) + "\" is not a struct");
		return st;
	}
	if (!struct_equals(st, members)) comp_error("Struct \"" + to_utf8(get_name(st)) + "\" redefined differently");
	return st;
}

// StructType::init @9db5a0
void Compiler::init_struct(SymType* st, const U16& name, Node* members, SymTable* member_table) {
	st->token = name;
	st->of_type = nullptr;
	st->kind = 0;
	st->index = -1;
	for (SymType* s : member_table->table) {
		if (s->kind != K_VAR) {
			comp_error("Only variables may be declared in a struct");
			continue;
		}
		s->dont_delete = true;
		st->members.push_back(s);
	}
	ops.emplace_back();
	OpCode* op = &ops.back();
	op->line = uint16_t(cur ? cur->line_count : 0);
	op->count = opcode_serial++;
	op->op = OP_CREATE_STRUCT;
	op->a1 = imm_arg(uint32_t(st->members.size()));
	op->a2 = sym_arg(st);
	op->first = op;
	if (OpCode* m = members ? members->opcode : nullptr) {
		op->prev = m;
		m->next = op;
		op->first = m->first;
	}
	st->code_init = op;
	st->kind = int(type_hash(unique_token(st)));
}

bool Compiler::struct_equals(SymType* st, Node* members) {
	// StructType::equals(SyntaxNode*) compares the member declarations; only reached by `struct TYPE_NAME {...}`,
	// which no shipped script writes. Compare member count and types.
	std::vector<SymType*> decl;
	std::vector<Node*> stack{members};
	while (!stack.empty()) {
		Node* n = stack.back();
		stack.pop_back();
		if (!n) continue;
		if (n->type == N_NON_INIT_DECL && n->sym) decl.push_back(n->sym);
		for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) stack.push_back(*it);
	}
	if (decl.size() != st->members.size()) return false;
	for (size_t i = 0; i < decl.size(); i++) {
		if (!sym_equals(decl[i]->of_type, st->members[i]->of_type)) return false;
	}
	return true;
}

// SymTable::add_enum_type @9d9ad0 (labels: unvalued = previous + 1, the first is 1; after a float the same float,
// after a string 1)
SymType* Compiler::add_enum_type(SymTable* t, SymType* prev, const U16& name, SymType* value) {
	SymType* prev_const = (prev && prev->of_type && prev->of_type->kind == K_CONST) ? prev->of_type : nullptr;
	if (!value || value->kind != K_CONST) {
		int type = TY_INT;
		U16 text = u"1";
		if (!value && prev && prev_const) {
			SymType* dt = data_type(prev);
			if (dt == t_int) {
				text = itow_text(const_int(prev_const) + 1);
			} else if (dt == t_float) {
				text = gcvt_text(double(const_float(prev_const)));
				type = TY_FLOAT;
			}
		}
		value = add_const_type(cur->file_table, type, text);
	}
	SymType* e = new_sym(SymType::Cls::Enum);
	e->token = name;
	e->of_type = value;
	e->kind = K_ENUM;
	e->index = -1;
	t->table.push_back(e);
	return e;
}

// SymTable::add_script_type @9d97e0: an unnamed script takes the file name without directory and extension.
SymType* Compiler::add_script_type(SymTable* t, const U16& name, SymType* kind_wrap, SymType* ret, Node* params) {
	U16 nm = name;
	if (nm.empty() && cur) {
		std::string base = cur->input_string;
		const size_t s = base.find_last_of("\\/");
		if (s != std::string::npos) base = base.substr(s + 1);
		const size_t dot = base.rfind('.');
		if (dot != std::string::npos) base = base.substr(0, dot);
		nm = to_u16(base);
	}
	SymType* fn = new_sym(SymType::Cls::Script);
	init_script(fn, nm, ret, kind_wrap ? kind_wrap->token : U16(), params);
	t->table.push_back(fn);
	return fn;
}

// SymTable::add_script_type @9d9920: a header naming an existing script implements the matching declaration or adds
// an overload at the end of the chain.
SymType* Compiler::add_script_type_existing(SymTable* t, SymType* fn, SymType* kind_wrap, SymType* ret, Node* params) {
	const U16 kind = kind_wrap ? kind_wrap->token : U16();
	SymType* last = fn;
	for (SymType* f = fn; f; f = (f->next_overload && f->next_overload->kind == K_FUNC) ? f->next_overload : nullptr) {
		last = f;
		if (script_equals(f, params, kind)) {
			if (!sym_equals(f->return_type, ret)) comp_error("Script \"" + to_utf8(get_name(f)) + "\" redeclared with a different return type");
			f->decl_count++;
			return f;
		}
		if (!f->next_overload) break;
	}
	SymType* n = new_sym(SymType::Cls::Script);
	init_script(n, get_name(fn), ret, kind, params);
	last->next_overload = n;
	t->table.push_back(n);
	return n;
}

// LocalScriptType::init @9dad30
void Compiler::init_script(SymType* fn, const U16& name, SymType* ret, const U16& kind, Node* params) {
	fn->token = name;
	fn->of_type = nullptr;
	fn->kind = K_FUNC;
	fn->index = -1;
	fn->address = nullptr;
	fn->trigger_counter = 0;
	fn->decl_count = 0;
	fn->is_implemented = false;
	fn->usage = 0;
	fn->source_file = cur;
	fn->script_type = kind;
	fn->return_type = ret;
	SymType* last_default = nullptr;
	if (params) {
		for (Node* c : params->children) {
			SymType* s = c->sym;
			SymType* var = nullptr;
			if (s && s->kind == K_CONST) {
				var = (!c->children.empty() && c->children[0]->sym && c->children[0]->sym->kind == K_VAR)
				          ? c->children[0]->sym
				          : nullptr;
				last_default = s;
				fn->param_defaults.push_back(s);
				if (var && var->scope_modifier == kScopeRef) comp_error("A ref parameter can't have a default value");
			} else {
				var = (s && s->kind == K_VAR) ? s : nullptr;
				if (last_default) comp_error("Parameter \"" + to_utf8(var ? var->token : U16()) + "\" needs a default value");
				fn->param_defaults.push_back(nullptr);
			}
			if (var) var->dont_delete = true;
			fn->params.push_back(var);
		}
	}
	fn->sym_table = cur ? cur->file_table : nullptr;
}

// LocalScriptType::equals @9dac30 (parameter names and types, defaults, the script kind)
bool Compiler::script_equals(SymType* fn, Node* params, const U16& kind) {
	const size_t np = params ? params->children.size() : 0;
	if (fn->params.size() != np || !ieq(fn->script_type, kind)) return false;
	for (size_t i = 0; i < np; i++) {
		Node* c = params->children[i];
		SymType* s = c->sym;
		SymType* var = nullptr;
		if (s && s->kind == K_CONST) {
			var = (!c->children.empty() && c->children[0]->sym && c->children[0]->sym->kind == K_VAR) ? c->children[0]->sym : nullptr;
			SymType* d = fn->param_defaults[i];
			if (!d) return false;
			if (!sym_equals(s, d)) return false;
		} else {
			var = (s && s->kind == K_VAR) ? s : nullptr;
			if (fn->param_defaults[i]) return false;
		}
		if (!var) return false;
		SymType* p = fn->params[i];
		if (!p || var->kind != K_VAR || var->scope_modifier != p->scope_modifier) return false;
		if (!sym_equals(p, var)) return false;
	}
	return true;
}

// SymTable::add_trigger_type @9d9700
SymType* Compiler::add_trigger_type(SymTable* t, const U16& name) {
	SymType* tr = new_sym(SymType::Cls::Trigger);
	tr->kind = K_TRIGGER;
	tr->index = cur_script ? cur_script->trigger_counter++ : 0;
	tr->token = name;
	tr->line = cur ? cur->line_count : 0;
	if (t) t->table.push_back(tr);
	return tr;
}

// SymTable::add_trigger_toggle @9d9580 (by name; resolved now if declared, else at count_code)
SymType* Compiler::add_trigger_toggle_name(SymTable* t, const U16& name, bool enable) {
	SymType* f = cur ? find_by_token(cur->script_table, name) : nullptr;
	SymType* tg = new_sym(SymType::Cls::Toggle);
	tg->token = u"?" + name;
	tg->trigger = (f && f->kind == K_TRIGGER) ? f : nullptr;
	tg->kind = K_TOGGLE;
	tg->index = -1;
	tg->is_enable = enable;
	if (t) t->table.push_back(tg);
	return tg;
}

// SymTable::add_trigger_toggle @9d9650 (a TRIGGER_NAME)
SymType* Compiler::add_trigger_toggle_sym(SymTable* t, SymType* trig, bool enable) {
	SymType* tg = new_sym(SymType::Cls::Toggle);
	tg->token = u"?" + (trig ? trig->token : U16());
	tg->trigger = (trig && trig->kind == K_TRIGGER) ? trig : nullptr;
	tg->kind = K_TOGGLE;
	tg->index = -1;
	tg->is_enable = enable;
	if (t) t->table.push_back(tg);
	return tg;
}

// SymTable::create_var_type @9d9160 + VarType::init @9da870: index from the struct table, the script's static
// counter or its local counter, allocated when the declarator reduces.
SymType* Compiler::create_var_type(const U16& name, SymType* type) {
	SymType* v = new_sym(SymType::Cls::Var);
	v->token = name;
	v->of_type = type ? type : curr_type;
	v->kind = K_VAR;
	v->index = -1;
	v->scope_modifier = curr_scope;
	if (curr_table && curr_table->type > 0xffff) {
		v->index = curr_table->var_counter++;
	} else if (cur && cur->script_table) {
		if (curr_scope == kScopeStatic) v->index = cur->script_table->static_var_counter++;
		else v->index = cur->script_table->var_counter++;
	}
	return v;
}

// ---------------------------------------------------------------- SymType virtual slots

SymType* Compiler::data_type(SymType* s) {
	if (!s) return nullptr;
	switch (s->cls) {
	case SymType::Cls::Var: case SymType::Cls::Const: return s->of_type;
	case SymType::Cls::Enum: return s->of_type ? s->of_type->of_type : nullptr;
	default: return s;
	}
}

int Compiler::casts_to(SymType* s, SymType* target) {
	if (!s) return 2;
	switch (s->cls) {
	case SymType::Cls::Var: case SymType::Cls::Const: return casts_to(s->of_type, target);
	case SymType::Cls::Array: case SymType::Cls::Struct:  // ArrayType::casts_to: identical type only
		return (target && data_type(target) == s) ? 0 : 2;
	default: {
		SymType* t = data_type(target);
		if (s == t) return 0;
		if (!t) return 2;
		if ((t->kind == TY_STRING && (s->kind == TY_INT || s->kind == TY_FLOAT)) || (t->kind == TY_FLOAT && s->kind == TY_INT))
			return 1;
		if (t->kind != TY_BOOL || s->kind != TY_INT) return 2;
		return 0;  // int -> bool is free
	}
	}
}

bool Compiler::accepts_op(SymType* s, int op) {
	if (!s) return false;
	if (s->cls == SymType::Cls::Var || s->cls == SymType::Cls::Const) return accepts_op(s->of_type, op);
	const uint32_t u = uint32_t(op);
	switch (s->kind) {
	case TY_BOOL: case TY_INT: return u < 0x26;
	case TY_VOID: return false;
	case TY_FLOAT: return u < 0x19;
	case TY_STRING: return u < 0xc;
	default: return u < 3;
	}
}

// SymType::equals @9dbc50 (VarType::equals adds the scope modifier)
bool Compiler::sym_equals(SymType* a, SymType* b) {
	if (!a || !b) return a == b;
	if (a->cls == SymType::Cls::Var && (b->kind != K_VAR || b->scope_modifier != a->scope_modifier)) return false;
	if (a->kind != b->kind || !ieq(a->token, b->token)) return false;
	if (a->of_type) return sym_equals(a->of_type, b->of_type);
	return true;
}

U16 Compiler::get_name(SymType* s) {
	if (!s) return U16();
	switch (s->cls) {
	case SymType::Cls::Const:
		if (s->of_type && s->of_type->kind == TY_STRING) return s->token.empty() ? U16() : s->token.substr(1);
		return s->token;
	case SymType::Cls::Array: return get_name(s->of_type) + u"[]";
	case SymType::Cls::Toggle: return s->token.empty() ? U16() : s->token.substr(1);
	default: return s->token;
	}
}

U16 Compiler::unique_token(SymType* s) {
	if (!s) return U16();
	switch (s->cls) {
	case SymType::Cls::Array: return u"@" + unique_token(s->of_type);
	case SymType::Cls::Struct: {
		U16 r = s->token + u"$";
		for (SymType* m : s->members) r += unique_token(m->of_type) + u"^";
		return r;
	}
	default: return s->of_type ? s->token + unique_token(s->of_type) : s->token;
	}
}

// get_create_opcode (SymType @9dbb70, ArrayType @9da540, StructType @9db370)
OpCode* Compiler::create_opcode(SymType* s, int line) {
	if (s->cls == SymType::Cls::Var || s->cls == SymType::Cls::Const) return create_opcode(s->of_type, line);
	if (s->cls == SymType::Cls::Array) {
		OpCode* op = get_line_new(OP_CREATE_ARRAY, line, sym_arg(s));
		if (OpCode* e = create_opcode(s->of_type, line)) prepend(op, e);
		return op;
	}
	if (s->cls == SymType::Cls::Struct) {
		OpCode* tail = s->code_init;
		if (!tail) return get_line_new(OP_CREATE_SIMPLE, line, sym_arg(s));
		OpCode* out = get_line_new(tail->op, line, tail->a1, tail->a2);
		for (OpCode* p = tail->prev; p; p = p->prev) {
			OpCode* c = get_line_new(p->op, line, p->a1, p->a2);
			prepend(out, c);
		}
		return out;
	}
	return get_line_new(OP_CREATE_SIMPLE, line, sym_arg(s));
}

bool Compiler::is_simple(SymType* s) {
	SymType* d = data_type(s);
	return !is_array(d) && !is_struct(d);
}
bool Compiler::is_complex(SymType* s) { return !is_simple(s); }

int32_t Compiler::const_int(SymType* c) { return convert_int(get_name(c)); }
float Compiler::const_float(SymType* c) { return float(get_double(get_name(c))); }

// ---------------------------------------------------------------- image writer

// LexerFileEntry::add_link @9c04a0
int Compiler::add_link(FileEntry* from, FileEntry* to) {
	const U16 name = to_u16(to->input_string);
	for (size_t i = 0; i < from->linked_files.size(); i++) {
		if (ieq(from->linked_files[i], name)) return int(i);
	}
	from->linked_files.push_back(name);
	return int(from->linked_files.size()) - 1;
}

// OpCode::resolve_trigger_toggle @9c2b50
void Compiler::resolve_trigger_toggle(OpCode* op) {
	SymType* tg = op->a1.sym;
	const U16 name = get_name(tg);
	if (!name.empty() && cur_script && cur_script->sym_table) {
		SymType* s = find_by_token(cur_script->sym_table, name);
		if (s && s->kind == K_TRIGGER) {
			tg->trigger = s;
			op->a1.kind = 3;
			op->a1.imm = uint32_t(s->index);
			return;
		}
	}
	comp_error("Trigger \"" + to_utf8(name) + "\" not found", false, op->line);
	op->a1.kind = 3;
	op->a1.imm = 0x7ffffffa;
}

// OpCode::count_code @9c2ed0: byte addresses, the monotone line fix-up and the per-line first-op table (chunk 7).
void Compiler::count_code(OpCode* head, ChunkWriter& w) {
	op_count = 0;
	cur_script = nullptr;
	const int nlines = cur ? cur->line_count : 0;
	std::vector<int32_t> table(size_t(nlines > 0 ? nlines : 0), -1);
	uint16_t prev = 0;
	for (OpCode* op = head; op; op = op->next) {
		op->count = op_count;
		if (op->line < prev) op->line = prev;
		if (op->op == OP_MARKER) continue;
		if (op->op == OP_SCRIPT_MARKER && op->a1.kind == 1 && op->a1.sym && op->a1.sym->kind == K_FUNC) cur_script = op->a1.sym;
		if (nlines != 0 && op->line >= 1 && op->line <= nlines && table[size_t(op->line - 1)] == -1)
			table[size_t(op->line - 1)] = int32_t(op_count);
		op_count += 1;
		prev = op->line;
		if (op->a1.kind == 1 && op->a1.sym && op->a1.sym->kind == K_TOGGLE) resolve_trigger_toggle(op);
		if (op->a1.present() && op->a1.kind >= 1 && op->a1.kind <= 3) op_count += 4;
		if (op->a2.present() && op->a2.kind >= 1 && op->a2.kind <= 3) op_count += 4;
	}
	if (cur_script) {  // OpCode::write_lines
		w.begin(7);
		w.u32(uint32_t(table.size()));
		for (int32_t v : table) w.i32(v);
		w.end();
	}
}

void Compiler::write_sym(SymType* s, ChunkWriter& w, int script_index) {
	switch (s->cls) {
	case SymType::Cls::Script:  // LocalScriptType::write @9da900
		if (!s->address) {
			if (s->usage == 0) comp_error("Script \"" + to_utf8(s->token) + "\" declared but never defined", true);
			else if (s->usage == 1) comp_error("Script \"" + to_utf8(s->token) + "\" used but never defined");
			return;
		}
		w.begin(2);
		w.i32(s->index);
		w.str(s->token);
		w.u32(s->address->count);
		w.i32(0);
		w.i32(s->return_type ? s->return_type->kind : TY_VOID);
		w.i32(s->trigger_counter);
		w.i32(int32_t(s->params.size()));
		for (SymType* p : s->params) {
			SymType* d = p ? data_type(p) : nullptr;
			w.i32(d ? d->kind : 0);
			w.i32(p && p->scope_modifier == kScopeRef ? 1 : 0);
		}
		w.end();
		s->address = nullptr;
		s->usage = 2;
		return;
	case SymType::Cls::Const:  // ConstType::write @9db920 (assigns the pool index)
		w.begin(3);
		w.i32(s->of_type ? s->of_type->kind : 0);
		s->index = root.var_counter++;
		if (s->of_type == t_string) w.str(get_name(s));
		else if (s->of_type == t_int || s->of_type == t_bool) w.i32(const_int(s));
		else if (s->of_type == t_float) w.f32(const_float(s));
		w.end();
		return;
	case SymType::Cls::Var:  // VarType::write @9da7c0 (marks statics in the index for good)
		w.begin(8);
		if (s->scope_modifier == kScopeStatic) s->index |= int(kStaticFlag);
		w.i32(script_index);
		w.i32(s->index);
		w.str(s->token);
		w.end();
		return;
	case SymType::Cls::Trigger:  // TriggerType::write @9da300
		w.begin(5);
		w.i32(script_index);
		w.i32(s->index);
		w.str(s->token);
		w.end();
		return;
	case SymType::Cls::Struct:  // StructType::write @9db230
		w.begin(9);
		w.str(s->token);
		w.end();
		return;
	default: return;
	}
}

// SymTable::write @9d8da0
void Compiler::write_symtable(SymTable* t, ChunkWriter& w) {
	if (t->type == 1) {
		if (t->file_entry) {
			for (FileEntry* inc : t->file_entry->local_includes) write_symtable(inc->file_table, w);
		}
		for (SymType* s : t->table) {
			if (s->kind == K_FUNC || s->kind == K_CONST || is_struct(s)) write_sym(s, w, -1);
		}
	} else if (t->type > 1 && t->type < 0x10000) {
		for (SymType* s : t->table) {
			if (s->kind == K_VAR || s->kind == K_TRIGGER) write_sym(s, w, t->local_script_index);
		}
	}
	for (SymTable* c : t->children) write_symtable(c, w);
}

// LexerFileEntry::write_file @9c05d0
void Compiler::write_file(FileEntry* fe) {
	CompiledFile out;
	out.path = fe->input_string;
	if (err_count == 0) {
		ChunkWriter w;
		w.begin(0);
		w.begin(6);  // write_includes
		w.u32(uint32_t(fe->linked_files.size()));
		for (const U16& s : fe->linked_files) w.str(s);
		w.end();
		if (!master_root || !master_root->opcode || !master_root->opcode->first) {
			err_count++;
		} else {
			root.var_counter = 0;
			OpCode* head = master_root->opcode->first;
			count_code(head, w);
			write_symtable(fe->file_table, w);
			w.begin(4);  // OpCode::write_code
			for (OpCode* op = head; op; op = op->next) {
				if (op->op == OP_ERROR_TOKEN) {
					comp_error("Error token in compiled code", false, op->line);
					continue;
				}
				if (op->op == OP_MARKER) continue;
				w.u8(uint8_t(op->op));
				for (const OpArg* a : {&op->a1, &op->a2}) {
					if (a->kind == 1 && a->sym) {
						switch (a->sym->kind) {
						case K_FUNC: case K_VAR: case K_TRIGGER: w.i32(a->sym->index); break;
						case K_CONST: w.u32(uint32_t(a->sym->index) | kConstFlag); break;
						default: w.i32(a->sym->kind); break;
						}
					} else if (a->kind == 2 && a->target) {
						w.u32(a->target->count);
					} else if (a->kind == 3) {
						w.u32(a->imm);
					}
				}
			}
			w.end();
			w.end();  // root
			// SymTable::clear_children: the file's script and block tables are released after writing, so an
			// includer's image re-lists the include's constants and structs but not its variables or triggers.
			fe->file_table->children.clear();
			if (err_count == 0) {
				out.image = std::move(w.data);
				out.ok = true;
			}
		}
	}
	result.files.push_back(std::move(out));
}

}  // namespace ron::bhs
