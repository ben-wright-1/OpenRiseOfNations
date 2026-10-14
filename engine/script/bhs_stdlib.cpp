// MathUtilFuncSet (@a04260..), TriggerUtilFuncSet (@a04440..), StringUtilFuncSet (@a04650..), ArrayUtilFuncSet
// (@a044a0..) and the rand_* bodies (@9e1890..9e1930).
#include "script/bhs_stdlib.hpp"

#include <cmath>
#include <cstring>

#pragma clang fp contract(off)

namespace ron::bhs {

namespace {
constexpr float kPi = 3.14159274101257324f;  // 0x40490fdb

float deg2rad(float d) { return (d * kPi) / 180.0f; }
float rad2deg(float r) { return (r * 180.0f) / kPi; }

// The element types an ArrayUtil value must match: the array's element data type (its template's).
bool element_ok(const Value* arr, const Value* v) {
	if (!arr || !v || !arr->blank) return false;
	return arr->blank->data_type == v->data_type;
}
}  // namespace

U16 parse_format(const U16& fmt, const std::vector<U16>& ps) {
	U16 out;
	size_t i = 0;
	auto pstr = [&](int d) { return d >= 0 && size_t(d) < ps.size() ? ps[size_t(d)] : U16(); };
	while (i < fmt.size()) {
		const size_t d = fmt.find(u'$', i);
		if (d == U16::npos) {
			out += fmt.substr(i);
			break;
		}
		const char16_t nx = d + 1 < fmt.size() ? fmt[d + 1] : 0;
		if (nx == u'$') {
			out += fmt.substr(i, d - i + 1);
			i = d + 2;
			continue;
		}
		if (nx == u'L' && fmt.compare(d, 6, u"$LINK<") == 0) {
			out += fmt.substr(i, d - i + 1);
			i = d + 1;
			continue;
		}
		if (nx == u'<') {  // $<...> gender / plural selection: keep the first form
			const size_t gt = fmt.find(u'>', d);
			out += fmt.substr(i, d - i);
			if (gt == U16::npos) {
				i = d + 2;
				continue;
			}
			const U16 body = fmt.substr(d + 2, gt - d - 2);
			const size_t colon = body.find(u':');
			U16 first = colon == U16::npos ? body : body.substr(colon + 1);
			const size_t c2 = first.find(u':');
			if (c2 != U16::npos) first = first.substr(0, c2);
			out += first;
			i = gt + 1;
			continue;
		}
		size_t k = d + 1;
		while (k < fmt.size() && !(fmt[k] >= u'0' && fmt[k] <= u'9')) k++;
		if (k >= fmt.size()) {  // no index: the rest is copied
			out += fmt.substr(i);
			break;
		}
		out += fmt.substr(i, d - i);
		out += pstr(fmt[k] - u'0');
		i = k + 1;
	}
	return out;
}

void install_stdlib(Runtime& rt, const StdlibHooks& h) {
	// ---- MathUtil (degrees, f32)
	rt.set_native("sin", [](NativeCall& c) { return make_float(std::sin(deg2rad(c.arg_float(0)))); });
	rt.set_native("cos", [](NativeCall& c) { return make_float(std::cos(deg2rad(c.arg_float(0)))); });
	rt.set_native("tan", [](NativeCall& c) { return make_float(std::tan(deg2rad(c.arg_float(0)))); });
	rt.set_native("asin", [](NativeCall& c) { return make_float(rad2deg(std::asin(c.arg_float(0)))); });
	rt.set_native("acos", [](NativeCall& c) { return make_float(rad2deg(std::acos(c.arg_float(0)))); });
	rt.set_native("atan", [](NativeCall& c) { return make_float(rad2deg(std::atan(c.arg_float(0)))); });
	rt.set_native("sqrt", [](NativeCall& c) { return make_float(std::sqrt(c.arg_float(0))); });
	rt.set_native("absl_float", [](NativeCall& c) {
		float f = c.arg_float(0);
		if (f < 0.0f) f = -f;
		return make_float(f);
	});
	rt.set_native("absl_int", [](NativeCall& c) {
		int32_t v = c.arg_int(0);
		if (v < 0) v = int32_t(0u - uint32_t(v));
		return make_int(v);
	});
	rt.set_native("min_val", [](NativeCall& c) {
		const float a = c.arg_float(0), b = c.arg_float(1);
		return make_float(b <= a ? b : a);
	});
	rt.set_native("max_val", [](NativeCall& c) {
		const float a = c.arg_float(0), b = c.arg_float(1);
		return make_float(a <= b ? b : a);
	});
	rt.set_native("rand_int", [h](NativeCall& c) { return make_int(h.rand_get ? h.rand_get(c.arg_int(0), c.arg_int(1)) : 0); });
	rt.set_native("rand_real", [h](NativeCall&) {
		const uint32_t s = h.rand_next ? h.rand_next() : 0;
		const uint32_t bits = (s & 0x7fffffu) | 0x3f800000u;
		float f;
		std::memcpy(&f, &bits, 4);
		return make_float(f - 1.0f);
	});
	rt.set_native("rand_seed", [h](NativeCall& c) {
		const int32_t n = c.arg_int(0);
		// n < 0 reseeds from timeGetTime() in the original (not deterministic); 0 here.
		if (h.rand_set_seed) h.rand_set_seed(n < 0 ? 0u : uint32_t(n));
		return VP();
	});
	rt.set_native("rand_get_seed", [h](NativeCall&) { return make_int(h.rand_seed ? int32_t(h.rand_seed()) : 0); });

	// ---- TriggerUtil (the running script's named triggers)
	auto toggle_all = [](Runtime& r, bool on) {
		RtScript* s = r.current_script();
		if (!s) return;
		for (size_t k = 0; k < s->trigger_names.size(); k++) {
			if (!s->trigger_names[k].empty() && int32_t(k) <= s->trigger_bit_count) s->set_trigger(int32_t(k), on);
		}
	};
	rt.set_native("enable_all_triggers", [toggle_all](NativeCall& c) {
		toggle_all(c.rt, true);
		return VP();
	});
	rt.set_native("disable_all_triggers", [toggle_all](NativeCall& c) {
		toggle_all(c.rt, false);
		return VP();
	});
	rt.set_native("is_trigger_enabled", [](NativeCall& c) {
		RtScript* s = c.rt.current_script();
		const U16 name = c.arg_str(0);
		if (!s || name.empty()) return make_int(-1);
		for (size_t k = 0; k < s->trigger_names.size(); k++) {
			if (ieq(s->trigger_names[k], name)) {
				if (int32_t(k) > s->trigger_bit_count) return make_int(-1);
				return make_int(s->trigger_enabled(int32_t(k)) ? 1 : 0);
			}
		}
		return make_int(-1);
	});

	// ---- StringUtil
	rt.set_native("length", [](NativeCall& c) { return make_int(int32_t(c.arg_str(0).size())); });
	rt.set_native("char_at", [](NativeCall& c) {
		const U16 s = c.arg_str(0);
		const int32_t k = c.arg_int(1);
		return make_int(k < int32_t(s.size()) && k > -1 ? int32_t(s[size_t(k)]) : 0);
	});
	rt.set_native("char_from_int", [](NativeCall& c) { return make_string(U16(1, char16_t(c.arg_int(0)))); });
	rt.set_native("parse", [](NativeCall& c) {
		// argument 0 is the format; up to 10 more are stringified into parse_strings (the rest cleared)
		std::vector<U16> ps(10);
		for (size_t k = 1; k < c.args.size() && k <= 10; k++) ps[k - 1] = c.args[k] ? value_string(c.args[k].get()) : U16();
		return make_string(parse_format(c.arg_str(0), ps));
	});
	rt.set_native("print", [h](NativeCall& c) {
		if (h.debug_print) h.debug_print(c.arg_str(0));
		return VP();
	});
	rt.set_native("print_line", [h](NativeCall& c) {
		if (h.debug_print) h.debug_print(c.arg_str(0) + u"\n");
		return VP();
	});

	// ---- ArrayUtil (elements must have the array's element type, else -1)
	rt.set_native("add", [](NativeCall& c) {  // add(a): appends a blank element
		Value* a = c.args[0].get();
		if (!a || !a->is_array() || !a->blank) return make_int(-1);
		VP e = std::make_shared<Value>(*a->blank);
		e->scope = SC_VAR;
		a->elems.push_back(e);
		return make_int(int32_t(a->elems.size()) - 1);
	}, 0);
	rt.set_native("add", [](NativeCall& c) {  // add(a, v): appends a copy
		Value* a = c.args[0].get();
		if (!a || !a->is_array() || !element_ok(a, c.args[1].get())) return make_int(-1);
		VP e = std::make_shared<Value>(*duplicate(c.args[1]));
		e->scope = SC_VAR;
		a->elems.push_back(e);
		return make_int(int32_t(a->elems.size()) - 1);
	}, 1);
	rt.set_native("remove", [](NativeCall& c) {  // the first equal element; its old index
		Value* a = c.args[0].get();
		if (!a || !a->is_array() || !element_ok(a, c.args[1].get())) return make_int(-1);
		for (size_t k = 0; k < a->elems.size(); k++) {
			const Value* e = a->elems[k].get();
			const Value* v = c.args[1].get();
			bool eq = false;
			if (e && v && e->cls == v->cls) {
				if (e->cls == Value::Int) eq = e->i == v->i;
				else if (e->cls == Value::Float) eq = e->f == v->f;
				else if (e->cls == Value::String) eq = ieq(e->s, v->s);
			}
			if (eq) {
				a->elems.erase(a->elems.begin() + long(k));
				return make_int(int32_t(k));
			}
		}
		return make_int(-1);
	});
	rt.set_native("remove_index", [](NativeCall& c) {  // returns the new length
		Value* a = c.args[0].get();
		const int32_t k = c.arg_int(1);
		if (!a || !a->is_array() || k < 0 || size_t(k) >= a->elems.size()) return make_int(-1);
		a->elems.erase(a->elems.begin() + k);
		return make_int(int32_t(a->elems.size()));
	});
	rt.set_native("insert", [](NativeCall& c) {
		Value* a = c.args[0].get();
		const int32_t k = c.arg_int(2);
		if (!a || !a->is_array() || !element_ok(a, c.args[1].get()) || k < 0 || size_t(k) > a->elems.size())
			return make_int(-1);
		VP e = std::make_shared<Value>(*duplicate(c.args[1]));
		e->scope = SC_VAR;
		a->elems.insert(a->elems.begin() + k, e);
		return make_int(k);
	});
	rt.set_native("find", [](NativeCall& c) {
		Value* a = c.args[0].get();
		const Value* v = c.args[1].get();
		if (!a || !a->is_array() || !element_ok(a, v)) return make_int(-1);
		for (size_t k = 0; k < a->elems.size(); k++) {
			const Value* e = a->elems[k].get();
			if (!e || e->cls != v->cls) continue;
			if ((e->cls == Value::Int && e->i == v->i) || (e->cls == Value::Float && e->f == v->f) ||
			    (e->cls == Value::String && ieq(e->s, v->s)))
				return make_int(int32_t(k));
		}
		return make_int(-1);
	});
	rt.set_native("clear", [](NativeCall& c) {
		if (Value* a = c.args[0].get(); a && a->is_array()) a->elems.clear();
		return VP();
	});
}

}  // namespace ron::bhs
