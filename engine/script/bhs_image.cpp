#include "script/bhs_image.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "script/bhs_builtins.hpp"

namespace ron::bhs {

namespace {
struct Reader {
	const Bytes& b;
	size_t p, end;
	bool ok = true;
	int32_t i32() {
		if (p + 4 > end) {
			ok = false;
			return 0;
		}
		int32_t v;
		std::memcpy(&v, b.data() + p, 4);
		p += 4;
		return v;
	}
	float f32() {
		const int32_t v = i32();
		float f;
		std::memcpy(&f, &v, 4);
		return f;
	}
	U16 str() {  // String::read: i32 length + UTF-16 units
		const int32_t n = i32();
		U16 s;
		if (n < 0 || p + size_t(n) * 2 > end) {
			ok = false;
			return s;
		}
		for (int32_t k = 0; k < n; k++) s += char16_t(b[p + size_t(2 * k)] | (b[p + size_t(2 * k) + 1] << 8));
		p += size_t(n) * 2;
		return s;
	}
};
}  // namespace

bool read_image(const Bytes& image, ScriptImage& out, std::string* err) {
	out = ScriptImage();
	try {
		const std::vector<Chunk> top = read_top_level(image);
		for (const Chunk& c : top) {
			out.chunk_order.push_back(c.id);
			Reader r{image, c.start, c.end};
			switch (c.id) {
			case 2: {  // ScriptFile::load_script_info @9c51c0
				ImageScript s;
				s.index = r.i32();
				s.name = r.str();
				s.offset = r.i32();
				s.script_type = r.i32();
				s.return_type = r.i32();
				s.trigger_count = r.i32();
				const int32_t np = r.i32();
				for (int32_t k = 0; k < np && r.ok; k++) {
					s.params.push_back(r.i32());
					s.refs.push_back(uint8_t(r.i32()));
				}
				s.trigger_names.assign(size_t(std::max(0, s.trigger_count)), U16());
				if (s.index >= int(out.scripts.size())) out.scripts.resize(size_t(s.index) + 1);
				out.scripts[size_t(s.index)] = s;
				break;
			}
			case 3: {  // load_const @9c4fb0
				ImageConst k;
				k.type = r.i32();
				if (k.type == TY_INT) k.i = r.i32();
				else if (k.type == TY_FLOAT) k.f = r.f32();
				else if (k.type == TY_STRING) k.s = r.str();
				out.consts.push_back(k);
				break;
			}
			case 4: out.code.assign(image.begin() + long(c.start), image.begin() + long(c.end)); break;
			case 5: {  // load_trigger @9c4f50
				const int32_t si = r.i32(), ti = r.i32();
				const U16 name = r.str();
				if (si >= 0 && si < int(out.scripts.size()) && ti >= 0 && ti < int(out.scripts[size_t(si)].trigger_names.size()))
					out.scripts[size_t(si)].trigger_names[size_t(ti)] = name;
				break;
			}
			case 6: {  // load_links @9c5120
				const int32_t n = r.i32();
				for (int32_t k = 0; k < n && r.ok; k++) out.links.push_back(r.str());
				break;
			}
			case 7: {  // load_line_info @9c4ec0
				const int32_t n = r.i32();
				for (int32_t k = 0; k < n && r.ok; k++) out.lines.push_back(r.i32());
				break;
			}
			case 8: {  // load_variable @9c4e30
				const int32_t si = r.i32();
				const uint32_t vi = uint32_t(r.i32());
				const U16 name = r.str();
				if (si >= 0 && si < int(out.scripts.size())) {
					ImageScript& s = out.scripts[size_t(si)];
					std::vector<U16>& v = (vi & kStaticFlag) ? s.static_var_names : s.var_names;
					const size_t idx = vi & ~kStaticFlag;
					if (idx < 100000) {
						if (v.size() <= idx) v.resize(idx + 1);
						v[idx] = name;
					}
				}
				break;
			}
			case 9: out.structs.push_back(r.str()); break;
			default: break;
			}
			if (!r.ok) throw FormatError("truncated chunk " + std::to_string(c.id));
		}
	} catch (const std::exception& e) {
		if (err) *err = e.what();
		return false;
	}
	return true;
}

static std::string esc(const U16& s) {
	std::string o = "\"";
	for (char16_t c : s) {
		if (c == '"' || c == '\\') o += '\\', o += char(c);
		else if (c == '\n') o += "\\n";
		else if (c == '\r') o += "\\r";
		else if (c == '\t') o += "\\t";
		else if (c < 0x20 || c >= 0x7f) {
			char b[8];
			std::snprintf(b, sizeof b, "\\u%04x", unsigned(c));
			o += b;
		} else {
			o += char(c);
		}
	}
	return o + "\"";
}

std::string disassemble(const ScriptImage& img, const BuiltinRegistry* builtins, DisasmStats* stats) {
	std::string out;
	char buf[512];
	std::map<int32_t, const ImageScript*> entry;
	for (const ImageScript& s : img.scripts) entry[s.offset] = &s;
	// pass 1: instruction starts
	std::set<int32_t> starts;
	for (size_t p = 0; p < img.code.size();) {
		starts.insert(int32_t(p));
		const int op = img.code[p];
		p += 1 + size_t(op_operand_count(op)) * 4;
	}
	std::vector<std::pair<int32_t, int>> script_starts;
	for (auto& [off, s] : entry) script_starts.push_back({off, 0});
	int count = 0, bad = 0;
	const ImageScript* cur = nullptr;
	for (size_t p = 0; p < img.code.size();) {
		auto it = entry.find(int32_t(p));
		if (it != entry.end()) {
			cur = it->second;
			std::snprintf(buf, sizeof buf, "\n; script %d %s (offset %d, return %s, %d triggers, %zu params)\n",
			              cur->index, to_utf8(cur->name).c_str(), cur->offset, type_id_name(cur->return_type),
			              cur->trigger_count, cur->params.size());
			out += buf;
		}
		const int op = img.code[p];
		const int n = op_operand_count(op);
		int32_t a[2] = {0, 0};
		for (int k = 0; k < n; k++) {
			if (p + 1 + size_t(4 * k) + 4 <= img.code.size()) std::memcpy(&a[k], img.code.data() + p + 1 + 4 * k, 4);
		}
		std::snprintf(buf, sizeof buf, "%6zu  %-20s", p, op_name(op));
		std::string line = buf;
		std::string comment;
		auto var_text = [&](int32_t v) {
			const uint32_t u = uint32_t(v);
			if (u & kConstFlag) {
				const size_t ci = u & ~kConstFlag;
				std::string t = "const#" + std::to_string(ci);
				if (ci < img.consts.size()) {
					const ImageConst& c = img.consts[ci];
					if (c.type == TY_INT) t += "=" + std::to_string(c.i);
					else if (c.type == TY_FLOAT) {
						std::snprintf(buf, sizeof buf, "=%gf", double(c.f));
						t += buf;
					} else t += "=" + esc(c.s);
				}
				return t;
			}
			if (u & kStaticFlag) {
				const size_t si = u & ~kStaticFlag;
				std::string t = "static#" + std::to_string(si);
				if (cur && si < cur->static_var_names.size() && !cur->static_var_names[si].empty())
					t += "(" + to_utf8(cur->static_var_names[si]) + ")";
				return t;
			}
			std::string t = "var#" + std::to_string(v);
			if (cur && v >= 0 && size_t(v) < cur->var_names.size() && !cur->var_names[size_t(v)].empty())
				t += "(" + to_utf8(cur->var_names[size_t(v)]) + ")";
			return t;
		};
		auto jump_text = [&](int32_t t) {
			if (!starts.count(t) && t != int32_t(img.code.size())) bad++;
			return "@" + std::to_string(t);
		};
		switch (op) {
		case OP_PUSH: case OP_INIT_COPY: case OP_INIT: line += var_text(a[0]); break;
		case OP_CREATE_SIMPLE: case OP_CREATE_ARRAY: case OP_CREATE_ARRAY_DYN: case OP_CAST:
			std::snprintf(buf, sizeof buf, "type 0x%x %s", uint32_t(a[0]), type_id_name(a[0]));
			line += buf;
			break;
		case OP_CREATE_ARRAY_INITER: case OP_CREATE_STRUCT:
			std::snprintf(buf, sizeof buf, "%d, type 0x%x", a[0], uint32_t(a[1]));
			line += buf;
			break;
		case OP_PUSH_STRUCT_FIELD: line += "field " + std::to_string(a[0]); break;
		case OP_CALL: case OP_SCRIPT_MARKER: {
			line += "script " + std::to_string(a[0]);
			for (const ImageScript& s : img.scripts) {
				if (s.index == a[0] && !s.name.empty()) comment = to_utf8(s.name);
			}
			break;
		}
		case OP_CALL_INCLUDE:
			line += "script " + std::to_string(a[0]) + ", link " + std::to_string(a[1]);
			if (a[1] >= 0 && size_t(a[1]) < img.links.size()) comment = to_utf8(img.links[size_t(a[1])]);
			break;
		case OP_CALL_GAME: case OP_CALL_GAME_VARIED:
			line += "func " + std::to_string(a[0]);
			if (op == OP_CALL_GAME_VARIED) line += ", " + std::to_string(a[1]) + " args";
			if (builtins && builtins->at(a[0])) comment = builtins->at(a[0])->name8;
			break;
		case OP_CASE: line += var_text(a[0]) + ", " + jump_text(a[1]); break;
		case OP_BIT_SET: case OP_BIT_UNSET:
			line += "trigger " + std::to_string(a[0]);
			if (cur && a[0] >= 0 && size_t(a[0]) < cur->trigger_names.size()) comment = to_utf8(cur->trigger_names[size_t(a[0])]);
			break;
		case OP_JUMP: case OP_JUMP_IF: case OP_JUMP_IF_NOT: case OP_JUMP_IF_SC_TRUE: case OP_JUMP_IF_SC_FALSE:
			line += jump_text(a[0]);
			break;
		case OP_JUMP_IF_INITED: line += var_text(a[0]) + ", " + jump_text(a[1]); break;
		case OP_JUMP_IF_BITSET:
			line += "trigger " + std::to_string(a[0]) + ", " + jump_text(a[1]);
			if (cur && a[0] >= 0 && size_t(a[0]) < cur->trigger_names.size()) comment = to_utf8(cur->trigger_names[size_t(a[0])]);
			break;
		default: break;
		}
		if (!comment.empty()) line += "  ; " + comment;
		out += line + "\n";
		count++;
		p += 1 + size_t(n) * 4;
	}
	if (stats) {
		stats->instructions = count;
		stats->bad_jumps = bad;
		stats->per_script.clear();
		std::vector<int32_t> offs;
		for (auto& [off, s] : entry) offs.push_back(off);
		for (size_t k = 0; k < offs.size(); k++) {
			const int32_t lo = offs[k], hi = k + 1 < offs.size() ? offs[k + 1] : int32_t(img.code.size());
			int c = 0;
			for (int32_t s : starts) c += s >= lo && s < hi;
			stats->per_script.push_back(c);
		}
	}
	return out;
}

}  // namespace ron::bhs
