// BigHugeScript values, script files and the virtual machine (docs/script-spec.md sections 4.2, 5, 6.1).
#include "script/bhs_vm.hpp"

#include <cmath>
#include <cstring>

#include "script/bhs_image.hpp"

#pragma clang fp contract(off)

namespace ron::bhs {

// ---------------------------------------------------------------- values

VP make_int(int32_t v, uint8_t scope) {
	auto p = std::make_shared<Value>();
	p->cls = Value::Int;
	p->data_type = TY_INT;
	p->i = v;
	p->scope = scope;
	return p;
}
VP make_float(float v, uint8_t scope) {
	auto p = std::make_shared<Value>();
	p->cls = Value::Float;
	p->data_type = TY_FLOAT;
	p->f = v;
	p->scope = scope;
	return p;
}
VP make_string(const U16& v, uint8_t scope) {
	auto p = std::make_shared<Value>();
	p->cls = Value::String;
	p->data_type = TY_STRING;
	p->s = v;
	p->scope = scope;
	return p;
}

VP duplicate(const VP& v) {
	if (!v) return v;
	if (v->scope == SC_TEMP) return v;
	auto p = std::make_shared<Value>();
	p->cls = v->cls;
	p->data_type = v->data_type;
	p->scope = SC_TEMP;
	p->i = v->i;
	p->f = v->f;
	p->s = v->s;
	if (v->cls == Value::Array || v->cls == Value::Object) {
		for (const VP& e : v->elems) {
			VP c = duplicate(e);
			if (c && e) c->scope = e->scope;
			p->elems.push_back(c);
		}
		p->blank = v->blank;  // shared template (ref_count++)
	}
	return p;
}

// cvttss2si: truncation; NaN and out-of-range give 0x80000000
static int32_t cvtt(float f) {
	if (!(f >= -2147483648.0f && f < 2147483648.0f)) return INT32_MIN;
	return int32_t(f);
}

int32_t value_int(const Value* v) {
	if (!v) return 0;
	switch (v->cls) {
	case Value::Int: return v->i;
	case Value::Float: return cvtt(v->f);
	case Value::String: return convert_int(v->s);
	default: return 0;
	}
}
float value_float(const Value* v) {
	if (!v) return 0;
	switch (v->cls) {
	case Value::Int: return float(v->i);
	case Value::Float: return v->f;
	case Value::String: return float(get_double(v->s));
	default: return 0;
	}
}
U16 value_string(const Value* v) {
	if (!v) return U16();
	switch (v->cls) {
	case Value::Int: return itow_text(v->i);
	case Value::Float: return gcvt_text(double(v->f));
	case Value::String: return v->s;
	default: return U16();
	}
}
bool value_is_false(const Value* v) {
	if (!v) return true;
	switch (v->cls) {
	case Value::Int: return v->i < 1;
	case Value::Float: return v->f <= 0.0f;
	case Value::String: return v->s.empty();
	default: return false;
	}
}

// ---------------------------------------------------------------- scripts and files

bool RtScript::trigger_enabled(int32_t n) const {
	if (n > trigger_bit_count) return true;  // Script::is_trigger_enabled returns -1 (non-zero)
	if (n < 0) return true;
	const size_t byte = size_t(n >> 3);
	return byte < trigger_bits.size() ? ((trigger_bits[byte] >> (n & 7)) & 1) != 0 : true;
}
void RtScript::set_trigger(int32_t n, bool on) {
	if (n > trigger_bit_count || n < 0) return;
	const size_t byte = size_t(n >> 3);
	if (byte >= trigger_bits.size()) return;
	if (on) trigger_bits[byte] = uint8_t(trigger_bits[byte] | (1u << (n & 7)));
	else trigger_bits[byte] = uint8_t(trigger_bits[byte] & ~(1u << (n & 7)));
}

Runtime::Runtime(const BuiltinRegistry& reg) : registry(reg) {
	natives.resize(size_t(reg.size()));
	unported_calls.assign(size_t(reg.size()), 0);
}

int Runtime::load_image(const std::string& source, const Bytes& image, std::string* err) {
	ScriptImage img;
	if (!read_image(image, img, err)) return -1;
	auto f = std::make_unique<RtFile>();
	f->source_file = source;
	f->code = img.code;
	f->line_to_op = img.lines;
	f->linked_file_names = img.links;
	for (const ImageConst& c : img.consts) {  // load_const: CONST-scoped values
		VP v = c.type == TY_INT ? make_int(c.i, SC_CONST) : c.type == TY_FLOAT ? make_float(c.f, SC_CONST) : make_string(c.s, SC_CONST);
		f->const_pool.push_back(v);
	}
	for (const ImageScript& s : img.scripts) {  // load_script_info: trigger bits all enabled
		RtScript r;
		r.name = s.name;
		r.offset = s.offset;
		r.script_type = s.script_type;
		r.return_type = s.return_type;
		r.params = s.params;
		r.refs = s.refs;
		r.trigger_bit_count = s.trigger_count;
		r.trigger_bits.assign(size_t((s.trigger_count + 7) / 8 + 1), 0xff);
		r.trigger_names = s.trigger_names;
		r.var_names = s.var_names;
		r.static_var_names = s.static_var_names;
		f->scripts.push_back(std::move(r));
	}
	const U16 key = to_u16(source);
	int slot = -1, free_slot = -1;
	for (int k = int(files.size()) - 1; k >= 0; k--) {
		if (files[size_t(k)] && ieq(to_u16(files[size_t(k)]->source_file), key)) {
			slot = k;
			break;
		}
		if (!files[size_t(k)]) free_slot = k;
	}
	if (slot < 0) slot = free_slot;
	if (slot < 0) {
		slot = int(files.size());
		files.push_back(nullptr);
	}
	files[size_t(slot)] = std::move(f);
	RtFile& rf = *files[size_t(slot)];
	for (const U16& ln : rf.linked_file_names) {  // ScriptFile::init: links resolved by path
		int idx = -1;
		for (size_t k = 0; k < files.size(); k++) {
			if (files[k] && ieq(to_u16(files[k]->source_file), ln)) idx = int(k);
		}
		if (idx < 0) run_time_error("Linked file " + to_utf8(ln) + " not loaded");
		rf.linked_files.push_back(idx);
	}
	return slot;
}

RtScript* Runtime::find_script(const U16& name, int* file_index) {
	for (int k = int(files.size()) - 1; k >= 0; k--) {
		if (!files[size_t(k)]) continue;
		for (RtScript& s : files[size_t(k)]->scripts) {
			if (ieq(s.name, name)) {
				if (file_index) *file_index = k;
				return &s;
			}
		}
	}
	return nullptr;
}

VP Runtime::err_return(int32_t type) {
	switch (type) {
	case TY_INT: case TY_BOOL: return make_int(-1);
	case TY_FLOAT: return make_float(-1.0f);
	case TY_STRING: return make_string(U16());
	default: return nullptr;
	}
}

void Runtime::set_native(const std::string& name, Native fn, int overload) {
	int k = 0;
	for (const BuiltinFunc& f : registry.funcs()) {
		if (f.name8 == name) {
			if (k++ == overload) {
				natives[size_t(f.index)] = std::move(fn);
				return;
			}
		}
	}
	if (log) log("set_native: no builtin '" + name + "' overload " + std::to_string(overload));
}

// ---------------------------------------------------------------- the machine

void Runtime::run_time_error(const std::string& msg) {
	std::string where;
	if (!frames_.empty()) {
		Frame& f = frames_.back();
		f.flags |= 4;
		if (f.script) where = to_utf8(f.script->name) + " @" + std::to_string(f.bip) + ": ";
	}
	err_count++;
	status_ = 3;
	if (log) log("script error: " + where + msg);
}

RtScript* Runtime::current_script() { return frames_.empty() ? nullptr : frames_.back().script; }
int Runtime::current_file() {
	if (frames_.empty()) return -1;
	for (size_t k = 0; k < files.size(); k++) {
		if (files[k].get() == frames_.back().file) return int(k);
	}
	return -1;
}

void Runtime::clear() {
	stack_.clear();
	err_count = 0;
	bytecodes_executed = 0;
	return_value = nullptr;
	frames_.clear();
	status_ = 2;
}

VP Runtime::pop() {
	if (stack_.empty()) return nullptr;
	VP v = std::move(stack_.back());
	stack_.pop_back();
	return v;
}

int Runtime::run_script(const U16& name, const std::vector<VP>& args) {
	clear();
	int fi = -1;
	RtScript* s = find_script(name, &fi);
	if (!s) {
		run_time_error("Script " + to_utf8(name) + " not found");
		return kNotFound;
	}
	// check_params: same count, exact types, argument 0 on top
	if (s->params.size() != args.size()) {
		run_time_error("Script " + to_utf8(name) + " takes " + std::to_string(s->params.size()) + " arguments");
		stack_.clear();
		return kBadArgs;
	}
	const size_t base = stack_.size();
	stack_.resize(base + args.size());
	for (size_t k = 0; k < args.size(); k++) {
		stack_[base + args.size() - 1 - k] = args[k];
		if (!args[k] || args[k]->data_type != s->params[k]) {
			run_time_error("Script " + to_utf8(name) + ": argument " + std::to_string(k) + " has the wrong type");
			stack_.clear();
			return kBadArgs;
		}
	}
	Frame f;
	f.file = files[size_t(fi)].get();
	f.script = s;
	f.bip = s->offset;
	f.flags = s->return_type == TY_VOID ? 8 : 0;
	f.expected = base + ((f.flags & 8) ? 0 : 1);
	frames_.push_back(std::move(f));
	return exec();
}

int Runtime::exec() {
	status_ = 0;
	for (;;) {
		// RunTimeEnv::check_vm_running
		if (frames_.empty()) {
			status_ = 3;
			break;
		}
		Frame& f = frames_.back();
		if ((f.flags & 4) || status_ == 3) {
			status_ = 3;
			break;
		}
		if (f.flags & 2) {
			status_ = 4;
			break;
		}
		if (bytecodes_executed >= 0x1000000 || frames_.size() >= 0x10000) {
			run_time_error("Script ran too long (" + std::to_string(frames_.size()) + " frames, " +
			               std::to_string(bytecodes_executed) + " instructions)");
			status_ = 3;
			break;
		}
		if (f.bip < 0 || size_t(f.bip) >= f.file->code.size()) {
			run_time_error("Instruction pointer out of the code");
			continue;
		}
		const int op = f.file->code[size_t(f.bip++)];
		execute_next(f, op);
		bytecodes_executed++;
	}
	if (status_ == 4) {  // RunTimeEnv::finish
		return_value = stack_.empty() ? nullptr : pop();
		frames_.clear();
		stack_.clear();
		return kOk;
	}
	const VP keep;
	clear();
	return kError;
}

VP Runtime::get_value(Frame& f, uint32_t operand) {
	if (operand & kConstFlag) {
		const size_t k = operand & ~kConstFlag;
		if (k < f.file->const_pool.size()) return f.file->const_pool[k];
	} else if (operand & kStaticFlag) {
		const size_t k = operand & ~kStaticFlag;
		if (k < f.script->static_vars.size()) return f.script->static_vars[k];
	} else if (operand < f.vars.size()) {
		return f.vars[operand];
	}
	run_time_error("Variable not initialized");
	return nullptr;
}

// VirtualMachine::set_value: a variable adopts a temporary, or shares a variable's object
void Runtime::set_value(Frame& f, uint32_t operand, VP v) {
	std::vector<VP>* slots = &f.vars;
	if (operand & kStaticFlag) {
		operand &= ~kStaticFlag;
		slots = &f.script->static_vars;
	}
	if (operand > 0xfffff) {
		run_time_error("Variable index out of range");
		return;
	}
	if (slots->size() <= operand) slots->resize(size_t(operand) + 1);
	if (v && v->scope != SC_VAR) v->scope = SC_VAR;
	(*slots)[operand] = std::move(v);
}

VP Runtime::init_simple(const Value* src, int32_t type) {
	switch (type) {
	case TY_INT: return make_int(src ? value_int(src) : 0);
	case TY_FLOAT: return make_float(src ? value_float(src) : 0.f);
	case TY_STRING: return make_string(src ? value_string(src) : U16());
	default:
		run_time_error("Can't create a value of type " + std::string(type_id_name(type)));
		return make_int(0);
	}
}

static int32_t idiv(int32_t a, int32_t b) { return (a == INT32_MIN && b == -1) ? INT32_MIN : a / b; }
static int32_t imod(int32_t a, int32_t b) { return (a == INT32_MIN && b == -1) ? 0 : a % b; }

// ScriptInt / ScriptFloat / ScriptString / ScriptArray / ScriptObject ::do_operator (@9d7760, @9d7040, @9d6b10,
// @9d5d80, @9d65d0). The assign family changes `a` and returns it; everything else returns a new temporary.
VP Runtime::do_operator(const VP& a, int op, const VP& b) {
	if (!a) {
		run_time_error("Operator on an uninitialized value");
		return make_int(0);
	}
	if (b && b->data_type != a->data_type) {
		run_time_error(std::string("Can't use \"") + op_name(op) + "\" on types \"" + type_id_name(a->data_type) +
		               "\" and \"" + type_id_name(b->data_type) + "\"");
		return a->cls == Value::Float ? nullptr : a;
	}
	const Value* B = b.get();
	switch (a->cls) {
	case Value::Int: {
		const int32_t x = a->i, y = B ? B->i : 0;
		switch (op) {
		case OP_ASSIGN: a->i = y; return a;
		case OP_EQ_OP: return make_int(x == y);
		case OP_NE_OP: return make_int(x != y);
		case OP_ADD_ASSIGN: a->i = int32_t(uint32_t(x) + uint32_t(y)); return a;
		case OP_ADD: return make_int(int32_t(uint32_t(y) + uint32_t(x)));
		case OP_LESS: return make_int(x < y);
		case OP_GREA: return make_int(y < x);
		case OP_LE_OP: return make_int(x <= y);
		case OP_GE_OP: return make_int(y <= x);
		case OP_UNA_NOT: return make_int(x < 1);
		case OP_AND_OP: case OP_OR_OP: return make_int(0 < x);
		case OP_MUL_ASSIGN: a->i = int32_t(uint32_t(y) * uint32_t(x)); return a;
		case OP_DIV_ASSIGN:
			if (y) {
				a->i = idiv(x, y);
				return a;
			}
			run_time_error("Can't Divide 0");
			return make_int(0);
		case OP_SUB_ASSIGN: a->i = int32_t(uint32_t(x) - uint32_t(y)); return a;
		case OP_MUL: return make_int(int32_t(uint32_t(y) * uint32_t(x)));
		case OP_DIV:
			if (y) return make_int(idiv(x, y));
			run_time_error("Can't Divide 0");
			return make_int(0);
		case OP_SUBT: return make_int(int32_t(uint32_t(x) - uint32_t(y)));
		case OP_INC_OP: case OP_INC_OP_POST: a->i = int32_t(uint32_t(x) + 1u); return a;
		case OP_DEC_OP: case OP_DEC_OP_POST: a->i = int32_t(uint32_t(x) - 1u); return a;
		case OP_UNA_NEGA: return make_int(int32_t(0u - uint32_t(x)));
		case OP_POW_OP: return make_int(cvtt(std::pow(float(x), float(y))));
		case OP_POW_ASSIGN: a->i = cvtt(std::pow(float(x), float(y))); return a;
		case OP_MOD_ASSIGN:
			if (y) {
				a->i = imod(x, y);
				return a;
			}
			run_time_error("Can't Divide 0");
			return make_int(0);
		case OP_LEFT_ASSIGN: a->i = int32_t(uint32_t(x) << (y & 31)); return a;
		case OP_RIGHT_ASSIGN: a->i = x >> (y & 31); return a;
		case OP_AND_ASSIGN: a->i = x & y; return a;
		case OP_XOR_ASSIGN: a->i = x ^ y; return a;
		case OP_OR_ASSIGN: a->i = x | y; return a;
		case OP_MOD:
			if (y) return make_int(imod(x, y));
			run_time_error("Can't Divide 0");
			return make_int(0);
		case OP_LEFT_OP: return make_int(int32_t(uint32_t(x) << (y & 31)));
		case OP_RIGHT_OP: return make_int(x >> (y & 31));
		case OP_AND_BIT: return make_int(y & x);
		case OP_XOR_BIT: return make_int(y ^ x);
		case OP_OR_BIT: return make_int(y | x);
		case OP_UNA_TILD: return make_int(~x);
		default:
			run_time_error(std::string("Bad int operator ") + op_name(op));
			return make_int(0);
		}
	}
	case Value::Float: {
		const float x = a->f, y = B ? B->f : 0.f;
		switch (op) {
		case OP_ASSIGN: a->f = y; return a;
		case OP_EQ_OP: return make_int(x == y);
		case OP_NE_OP: return make_int(x != y);
		case OP_ADD_ASSIGN: a->f = y + x; return a;
		case OP_ADD: return make_float(y + x);
		case OP_LESS: return make_int(!(y < x) && !(y == x));
		case OP_GREA: return make_int(!(x < y) && !(x == y));
		case OP_LE_OP: return make_int(x <= y);
		case OP_GE_OP: return make_int(y <= x);
		case OP_UNA_NOT: return make_int(x <= 0.0f);
		case OP_AND_OP: return make_int(!(x < 0.0f) && !(x == 0.0f));
		case OP_OR_OP: return make_int(0.0f < x ? 1 : 0);
		case OP_MUL_ASSIGN: a->f = y * x; return a;
		case OP_DIV_ASSIGN:
			if (y != 0.0f) {
				a->f = x / y;
				return a;
			}
			run_time_error("Can't Divide 0");
			return make_float(0);
		case OP_SUB_ASSIGN: a->f = x - y; return a;
		case OP_MUL: return make_float(y * x);
		case OP_DIV:
			if (y != 0.0f) return make_float(x / y);
			run_time_error("Can't Divide 0");
			return make_float(0);
		case OP_SUBT: return make_float(x - y);
		case OP_INC_OP: case OP_INC_OP_POST: a->f = x + 1.0f; return a;
		case OP_DEC_OP: case OP_DEC_OP_POST: a->f = x - 1.0f; return a;
		case OP_UNA_NEGA: return make_float(-x);
		case OP_POW_OP: return make_float(float(cvtt(std::pow(x, y))));
		case OP_POW_ASSIGN: a->f = float(cvtt(std::pow(x, y))); return a;
		default:
			run_time_error(std::string("Bad float operator ") + op_name(op));
			return make_float(0);
		}
	}
	case Value::String: {
		const U16 empty;
		const U16& y = B ? B->s : empty;
		switch (op) {
		case OP_ASSIGN: a->s = y; return a;
		case OP_EQ_OP: return make_int(ieq(a->s, y));
		case OP_NE_OP: return make_int(!ieq(a->s, y));
		case OP_ADD_ASSIGN: a->s += y; return a;
		case OP_ADD: return make_string(a->s + y);
		case OP_LESS: return make_int(wcsicmp_c(a->s, y) < 0);
		case OP_GREA: return make_int(wcsicmp_c(a->s, y) > 0);
		case OP_LE_OP: return make_int(wcsicmp_c(a->s, y) < 0 || ieq(a->s, y));
		case OP_GE_OP: return make_int(wcsicmp_c(a->s, y) > 0 || ieq(a->s, y));
		case OP_UNA_NOT: return make_int(a->s.empty());
		case OP_AND_OP: return make_int(!a->s.empty() && !y.empty());
		case OP_OR_OP: return make_int(!(a->s.empty() && y.empty()));
		default:
			run_time_error(std::string("Bad string operator ") + op_name(op));
			return make_string(U16());
		}
	}
	case Value::Array:
	case Value::Object: {
		if (a->cls == Value::Object && !B) {
			run_time_error("Struct operator without a value");
			return a;
		}
		if (a->cls == Value::Object && B && B->elems.size() != a->elems.size()) {
			run_time_error("Struct operator on different structs");
			return a;
		}
		switch (op) {
		case OP_ASSIGN:
			if (a->cls == Value::Array) {
				a->elems.clear();
				if (B) {
					for (const VP& e : B->elems) {
						VP c = duplicate(e);
						if (c) {
							if (c == e) {  // a temporary element would be shared; copy it
								c = std::make_shared<Value>(*e);
							}
							c->scope = SC_VAR;
						}
						a->elems.push_back(c);
					}
				}
			} else {
				for (size_t k = 0; k < a->elems.size(); k++) do_operator(a->elems[k], OP_ASSIGN, B->elems[k]);
			}
			return a;
		case OP_EQ_OP:
			if (!B) return make_int(1);
			for (size_t k = 0; k < a->elems.size(); k++) {
				if (k >= B->elems.size()) return make_int(0);
				VP r = do_operator(a->elems[k], OP_EQ_OP, B->elems[k]);
				if (!r || value_is_false(r.get())) return r ? r : make_int(0);
			}
			return make_int(1);
		case OP_NE_OP:
			if (!B) return make_int(1);
			for (size_t k = 0; k < a->elems.size(); k++) {
				if (k >= B->elems.size()) return make_int(1);
				VP r = do_operator(a->elems[k], OP_NE_OP, B->elems[k]);
				if (r && value_is_false(r.get())) {  // (sic) an equal pair answers "different"
					r->i = 1;
					return r;
				}
			}
			return make_int(0);
		default:
			run_time_error(std::string("Bad operator on an array or struct: ") + op_name(op));
			return a;
		}
	}
	}
	return a;
}

void Runtime::call_script(int link, int script) {
	Frame& cur = frames_.back();
	RtFile* file = cur.file;
	if (link >= int(file->linked_files.size())) {
		run_time_error("Bad link index");
		return;
	}
	if (link >= 0) {
		const int idx = file->linked_files[size_t(link)];
		if (idx < 0 || idx >= int(files.size()) || !files[size_t(idx)]) {
			run_time_error("Linked file not loaded");
			return;
		}
		file = files[size_t(idx)].get();
	}
	if (script < 0 || script >= int(file->scripts.size())) {
		run_time_error("Bad script index");
		return;
	}
	RtScript* s = &file->scripts[size_t(script)];
	Frame f;
	f.file = file;
	f.script = s;
	f.bip = s->offset;
	f.flags = s->return_type == TY_VOID ? 8 : 0;
	const size_t np = s->params.size();
	f.expected = (stack_.size() >= np ? stack_.size() - np : 0) + ((f.flags & 8) ? 0 : 1);
	frames_.push_back(std::move(f));
}

void Runtime::close_frame() {
	if (frames_.back().expected != stack_.size()) run_time_error("Stack size mismatch at return");
	if (frames_.size() > 1) frames_.pop_back();
}

// VirtualMachine::call_func + ScriptFuncSet::call_func: parameters checked last to first (argument i is stack slot
// top - i), then the implementation; the arguments are popped and a non-void result pushed.
void Runtime::call_func(Frame&, int nargs, int index) {
	const BuiltinFunc* fn = registry.at(index);
	if (!fn) {
		run_time_error("Bad builtin index " + std::to_string(index));
		return;
	}
	if (nargs < 0) nargs = int(fn->params.size());
	const size_t len = stack_.size();
	const size_t base = len >= size_t(nargs) ? len - size_t(nargs) : 0;
	bool rejected = false;
	for (int i = int(fn->params.size()) - 1; i >= 0; i--) {
		const BuiltinParam& p = fn->params[size_t(i)];
		if (p.type == TY_VARARGS) continue;
		const VP& v = size_t(i) < len ? stack_[len - 1 - size_t(i)] : stack_.front();
		if (!v) {
			run_time_error("Missing parameter " + to_utf8(p.name) + " of " + fn->name8);
			rejected = true;
			break;
		}
		if (p.type != v->data_type && p.type != TY_GENERIC && !(p.type == TY_ARRAY_ANY && v->is_array())) {
			run_time_error(fn->name8 + ": parameter " + to_utf8(p.name) + " is " + type_id_name(v->data_type) + ", not " +
			               type_id_name(p.type));
			rejected = true;
			break;
		}
	}
	VP result;
	if (rejected) {
		result = err_return(fn->return_type);
	} else {
		std::vector<VP> args;
		const size_t n = fn->varargs() ? size_t(nargs) : fn->params.size();
		for (size_t k = 0; k < n && k < len; k++) args.push_back(stack_[len - 1 - k]);
		const Native& nat = natives[size_t(index)];
		if (nat) {
			NativeCall c{*this, *fn, args};
			result = nat(c);
		} else {
			if (unported_calls[size_t(index)]++ == 0 && log) log("unported builtin: " + fn->name8);
			switch (fn->return_type) {
			case TY_INT: result = make_int(0); break;
			case TY_FLOAT: result = make_float(0); break;
			case TY_STRING: result = make_string(U16()); break;
			default: break;
			}
		}
		if (!result && fn->return_type != TY_VOID) result = err_return(fn->return_type);
		if (result && result->scope != SC_TEMP && fn->return_type != TY_VOID && result->cls <= Value::String)
			result = duplicate(result);  // simple results are new values (ScriptInt / ScriptFloat / ScriptString)
	}
	stack_.resize(base);  // ScriptParamStack::close
	if (fn->return_type != TY_VOID) stack_.push_back(result);
	if (stack_.size() != base + (fn->return_type != TY_VOID ? 1 : 0)) run_time_error("Builtin stack mismatch");
}

static int32_t rd32(const std::vector<uint8_t>& code, int32_t at) {
	int32_t v = 0;
	if (at >= 0 && size_t(at) + 4 <= code.size()) std::memcpy(&v, code.data() + at, 4);
	return v;
}

// VirtualMachine::execute_next @9e0840
void Runtime::execute_next(Frame& f, int op) {
	const std::vector<uint8_t>& code = f.file->code;
	auto operand = [&](int k) { return rd32(code, f.bip + 4 * k); };
	switch (op) {
	case OP_ASSIGN: case OP_ADD_ASSIGN: case OP_MUL_ASSIGN: case OP_DIV_ASSIGN: case OP_SUB_ASSIGN: case OP_POW_ASSIGN:
	case OP_MOD_ASSIGN: case OP_LEFT_ASSIGN: case OP_RIGHT_ASSIGN: case OP_AND_ASSIGN: case OP_XOR_ASSIGN:
	case OP_OR_ASSIGN: {
		VP target = pop();
		VP value = pop();
		do_operator(target, op, value);
		stack_.push_back(target);
		return;
	}
	case OP_EQ_OP: case OP_NE_OP: case OP_ADD: case OP_LESS: case OP_GREA: case OP_LE_OP: case OP_GE_OP: case OP_AND_OP:
	case OP_OR_OP: case OP_MUL: case OP_DIV: case OP_SUBT: case OP_POW_OP: case OP_MOD: case OP_LEFT_OP: case OP_RIGHT_OP:
	case OP_AND_BIT: case OP_XOR_BIT: case OP_OR_BIT: {
		VP b = pop();
		VP a = pop();
		stack_.push_back(do_operator(a, op, b));
		return;
	}
	case OP_UNA_NOT: case OP_UNA_NEGA: case OP_UNA_TILD: {
		VP a = pop();
		stack_.push_back(do_operator(a, op, nullptr));
		return;
	}
	case OP_INC_OP: case OP_DEC_OP: {
		VP t = pop();
		do_operator(t, op, nullptr);
		if (size_t(f.bip) < code.size() && code[size_t(f.bip)] == OP_POP) {
			f.bip++;
			return;
		}
		stack_.push_back(t);
		return;
	}
	case OP_INC_OP_POST: case OP_DEC_OP_POST: {
		VP t = pop();
		if (size_t(f.bip) < code.size() && code[size_t(f.bip)] == OP_POP) {
			do_operator(t, op, nullptr);
			f.bip++;
			return;
		}
		VP old = duplicate(t);
		do_operator(t, op, nullptr);
		stack_.push_back(old);
		return;
	}
	case OP_PUSH: {
		VP v = get_value(f, uint32_t(operand(0)));
		f.bip += 4;
		stack_.push_back(v);
		return;
	}
	case OP_POP: pop(); return;
	case OP_CREATE_SIMPLE: {
		VP v = init_simple(nullptr, operand(0));
		f.bip += 4;
		stack_.push_back(v);
		return;
	}
	case OP_CREATE_ARRAY: case OP_CREATE_ARRAY_DYN: {
		int32_t n = 0;
		if (op == OP_CREATE_ARRAY_DYN) {
			VP nv = pop();
			n = value_int(nv.get());
		}
		VP elem = pop();
		auto arr = std::make_shared<Value>();
		arr->cls = Value::Array;
		arr->data_type = operand(0);
		if (!elem) {
			run_time_error("Array without an element type");
		} else {
			VP b = duplicate(elem);
			if (b == elem) b = std::make_shared<Value>(*elem);
			b->scope = SC_VAR;
			arr->blank = b;
			for (int32_t k = 0; k < n; k++) {
				VP c = std::make_shared<Value>(*duplicate(elem));
				c->scope = SC_VAR;
				arr->elems.push_back(c);
			}
		}
		f.bip += 4;
		stack_.push_back(arr);
		return;
	}
	case OP_CREATE_ARRAY_INITER: case OP_CREATE_STRUCT: {
		const int32_t n = operand(0);
		auto v = std::make_shared<Value>();
		v->cls = op == OP_CREATE_STRUCT ? Value::Object : Value::Array;
		v->data_type = operand(1);
		for (int32_t k = 0; k < n; k++) {
			VP e = duplicate(pop());
			if (e) e->scope = SC_VAR;
			v->elems.push_back(e);
		}
		if (op == OP_CREATE_ARRAY_INITER) {
			if (n != 0 && v->elems[0]) {
				VP b = std::make_shared<Value>(*v->elems[0]);  // a cleared copy of element 0
				b->i = 0;
				b->f = 0;
				b->s.clear();
				b->elems.clear();
				b->scope = SC_VAR;
				v->blank = b;
			} else {
				run_time_error("Empty array initializer");
			}
		}
		f.bip += 8;
		stack_.push_back(v);
		return;
	}
	case OP_PUSH_ARRAY_INDEX: case OP_CREATE_ARRAY_INDEX: {
		VP iv = pop();
		VP av = pop();
		const int32_t idx = value_int(iv.get());
		Value* arr = (av && av->cls == Value::Array) ? av.get() : nullptr;
		if (!arr) {
			run_time_error("Not an array");
		} else if (idx < 0) {
			run_time_error("Negative array index " + std::to_string(idx));
		} else if (op == OP_PUSH_ARRAY_INDEX) {
			if (size_t(idx) < arr->elems.size()) stack_.push_back(arr->elems[size_t(idx)]);
			else run_time_error("Array index " + std::to_string(idx) + " out of range " + std::to_string(arr->elems.size()));
		} else {
			if (arr->elems.size() <= size_t(idx)) {  // ScriptArray::resize_array (grow with copies of the template)
				if (!arr->blank) {
					run_time_error("Array without a template");
					return;
				}
				while (arr->elems.size() <= size_t(idx)) {
					VP c = std::make_shared<Value>(*duplicate(arr->blank));
					c->scope = SC_VAR;
					arr->elems.push_back(c);
				}
			}
			stack_.push_back(arr->elems[size_t(idx)]);
		}
		return;
	}
	case OP_PUSH_STRUCT_FIELD: {
		VP s = pop();
		const int32_t k = operand(0);
		if (!s || s->cls != Value::Object) run_time_error("Not a struct");
		else if (k >= 0 && size_t(k) < s->elems.size()) stack_.push_back(s->elems[size_t(k)]);
		else run_time_error("Struct field out of range");
		f.bip += 4;
		return;
	}
	case OP_PUSH_ARRAY_LENGTH: {
		VP a = pop();
		if (!a || a->cls != Value::Array) {
			run_time_error("Not an array");
			stack_.push_back(make_int(0));
		} else {
			stack_.push_back(make_int(int32_t(a->elems.size())));
		}
		return;
	}
	case OP_SET_ARRAY_LENGTH: {
		VP a = pop();
		VP n = pop();
		if (a && a->cls == Value::Array) {
			const int32_t len = value_int(n.get());
			if (len < int32_t(a->elems.size())) {
				a->elems.resize(size_t(std::max(len, 0)));
			} else if (a->blank) {
				while (int32_t(a->elems.size()) < len) {
					VP c = std::make_shared<Value>(*duplicate(a->blank));
					c->scope = SC_VAR;
					a->elems.push_back(c);
				}
			} else {
				run_time_error("Array without a template");
			}
		} else {
			run_time_error("Not an array");
		}
		stack_.push_back(n);
		return;
	}
	case OP_INIT_COPY: {
		VP v = duplicate(pop());
		set_value(f, uint32_t(operand(0)), v);
		f.bip += 4;
		return;
	}
	case OP_INIT: {
		VP v = pop();
		set_value(f, uint32_t(operand(0)), v);
		f.bip += 4;
		return;
	}
	case OP_CAST: {
		VP x = pop();
		VP r = init_simple(x.get(), operand(0));
		f.bip += 4;
		stack_.push_back(r);
		return;
	}
	case OP_CAST_BOOL: {
		VP x = pop();
		stack_.push_back(make_int(value_is_false(x.get()) ? 0 : 1));
		return;
	}
	case OP_CALL: {
		const int32_t s = operand(0);
		f.bip += 4;  // (the original advances the caller's pointer after pushing the callee's frame)
		call_script(-1, s);
		return;
	}
	case OP_CALL_INCLUDE: {
		const int32_t s = operand(0), link = operand(1);
		f.bip += 8;
		call_script(link, s);
		return;
	}
	case OP_CALL_GAME: {
		const int32_t fi = operand(0);
		f.bip += 4;
		call_func(f, -1, fi);
		return;
	}
	case OP_CALL_GAME_VARIED: {
		const int32_t fi = operand(0), n = operand(1);
		f.bip += 8;
		call_func(f, n, fi);
		return;
	}
	case OP_CASE: {
		const uint32_t k = uint32_t(operand(0)) & ~kConstFlag;
		VP top = stack_.empty() ? nullptr : stack_.back();
		VP c = k < f.file->const_pool.size() ? f.file->const_pool[k] : nullptr;
		VP r = do_operator(top, OP_EQ_OP, c);
		if (r && !value_is_false(r.get())) {
			f.bip = operand(1);
			pop();
		} else {
			f.bip += 8;
		}
		return;
	}
	case OP_BIT_SET: f.script->set_trigger(operand(0), false); f.bip += 4; return;
	case OP_BIT_UNSET: f.script->set_trigger(operand(0), true); f.bip += 4; return;
	case OP_RETURN:
		if (!(f.flags & 8) && !stack_.empty()) stack_.back() = duplicate(stack_.back());
		f.flags |= 2;
		close_frame();
		return;
	case OP_JUMP: f.bip = operand(0); return;
	case OP_JUMP_IF: case OP_JUMP_IF_NOT: {
		VP x = pop();
		const bool truth = x && !value_is_false(x.get());
		if (truth == (op == OP_JUMP_IF)) f.bip = operand(0);
		else f.bip += 4;
		return;
	}
	case OP_JUMP_IF_SC_TRUE: {
		VP x = pop();
		if (!x) {
			f.bip += 4;
			return;
		}
		if (value_is_false(x.get())) {
			f.bip += 4;
			return;
		}
		stack_.push_back(x);
		f.bip = operand(0);
		return;
	}
	case OP_JUMP_IF_SC_FALSE: {
		VP x = pop();
		if (x && !value_is_false(x.get())) {
			f.bip += 4;
			return;
		}
		stack_.push_back(x);
		f.bip = operand(0);
		return;
	}
	case OP_JUMP_IF_INITED: {
		const uint32_t k = uint32_t(operand(0)) & ~kStaticFlag;
		if (k < f.script->static_vars.size() && f.script->static_vars[k]) f.bip = operand(1);
		else f.bip += 8;
		return;
	}
	case OP_JUMP_IF_BITSET: {
		if (f.script->trigger_enabled(operand(0))) f.bip += 8;
		else f.bip = operand(1);
		return;
	}
	case OP_SCRIPT_MARKER: f.bip += 4; return;
	case OP_MARKER: return;
	default:
		run_time_error("Bad opcode " + std::to_string(op));
		return;
	}
}

}  // namespace ron::bhs
