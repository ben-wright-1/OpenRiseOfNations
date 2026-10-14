#include "sim/rules_xml.hpp"

#include <stdexcept>

namespace ron::sim {

// ---------------------------------------------------------------- strings

ustr from_ascii(const char* s) {
	ustr out;
	for (; *s; ++s) out.push_back(static_cast<char16_t>(static_cast<unsigned char>(*s)));
	return out;
}

std::string to_utf8(const ustr& s) {
	std::string out;
	for (size_t i = 0; i < s.size(); ++i) {
		uint32_t c = s[i];
		if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
			c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
			++i;
		}
		if (c < 0x80) {
			out += char(c);
		} else if (c < 0x800) {
			out += char(0xc0 | (c >> 6));
			out += char(0x80 | (c & 0x3f));
		} else if (c < 0x10000) {
			out += char(0xe0 | (c >> 12));
			out += char(0x80 | ((c >> 6) & 0x3f));
			out += char(0x80 | (c & 0x3f));
		} else {
			out += char(0xf0 | (c >> 18));
			out += char(0x80 | ((c >> 12) & 0x3f));
			out += char(0x80 | ((c >> 6) & 0x3f));
			out += char(0x80 | (c & 0x3f));
		}
	}
	return out;
}

namespace {

void push_code_point(ustr& out, uint32_t cp) {
	if (cp < 0x10000) {
		out.push_back(static_cast<char16_t>(cp));
	} else {
		cp -= 0x10000;
		out.push_back(static_cast<char16_t>(0xd800 + (cp >> 10)));
		out.push_back(static_cast<char16_t>(0xdc00 + (cp & 0x3ff)));
	}
}

}  // namespace

ustr decode_utf8(const uint8_t* p, size_t n) {
	ustr out;
	out.reserve(n);
	uint32_t cp = 0, needed = 0, seen = 0, lower = 0x80, upper = 0xbf;
	for (size_t i = 0; i < n; ++i) {
		const uint32_t b = p[i];
		if (needed == 0) {
			if (b <= 0x7f) {
				out.push_back(static_cast<char16_t>(b));
			} else if (b >= 0xc2 && b <= 0xdf) {
				needed = 1;
				cp = b & 0x1f;
			} else if (b >= 0xe0 && b <= 0xef) {
				if (b == 0xe0) lower = 0xa0;
				if (b == 0xed) upper = 0x9f;
				needed = 2;
				cp = b & 0xf;
			} else if (b >= 0xf0 && b <= 0xf4) {
				if (b == 0xf0) lower = 0x90;
				if (b == 0xf4) upper = 0x8f;
				needed = 3;
				cp = b & 0x7;
			} else {
				out.push_back(0xfffd);
			}
			continue;
		}
		if (b < lower || b > upper) {
			// The broken sequence becomes one U+FFFD and this byte is decoded afresh.
			cp = needed = seen = 0;
			lower = 0x80;
			upper = 0xbf;
			out.push_back(0xfffd);
			--i;
			continue;
		}
		lower = 0x80;
		upper = 0xbf;
		cp = (cp << 6) | (b & 0x3f);
		if (++seen == needed) {
			push_code_point(out, cp);
			cp = needed = seen = 0;
		}
	}
	if (needed) out.push_back(0xfffd);
	return out;
}

ustr decode_text_file(const Bytes& raw) {
	if (raw.size() >= 2 && raw[0] == 0xff && raw[1] == 0xfe) {
		ustr out;
		out.reserve(raw.size() / 2);
		for (size_t i = 0; i + 1 < raw.size(); i += 2) out.push_back(static_cast<char16_t>(raw[i] | (raw[i + 1] << 8)));
		return out;
	}
	return decode_utf8(raw.data(), raw.size());
}

namespace {

inline char16_t lower_c(char16_t c) { return c >= u'A' && c <= u'Z' ? char16_t(c + 32) : c; }
inline char16_t upper_c(char16_t c) { return c >= u'a' && c <= u'z' ? char16_t(c - 32) : c; }

}  // namespace

bool iequals(const ustr& a, const ustr& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (lower_c(a[i]) != lower_c(b[i])) return false;
	}
	return true;
}

ustr to_upper(ustr s) {
	for (char16_t& c : s) c = upper_c(c);
	return s;
}

ustr to_lower(ustr s) {
	for (char16_t& c : s) c = lower_c(c);
	return s;
}

int wcsicmp(const ustr& a, const ustr& b) {
	const size_t n = a.size() < b.size() ? a.size() : b.size();
	for (size_t i = 0; i < n; ++i) {
		const int d = int(lower_c(a[i])) - int(lower_c(b[i]));
		if (d) return d;
	}
	return int(a.size()) - int(b.size());
}

// ---------------------------------------------------------------- DOM

const ustr* XNode::attr(const ustr& name) const {
	for (const auto& a : attrs) {
		if (a.first == name) return &a.second;
	}
	return nullptr;
}

namespace {

// JavaScript's \s (and String.prototype.trim) set; the reference parser tokenises with it.
bool js_space(char16_t c) {
	return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xa0 || c == 0x1680 ||
	       (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 ||
	       c == 0xfeff;
}

bool is_hex(char16_t c) { return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F'); }
int hex_val(char16_t c) { return c <= u'9' ? c - u'0' : (c | 0x20) - u'a' + 10; }

bool starts_at(const ustr& s, size_t p, const char16_t* lit) {
	for (size_t i = 0; lit[i]; ++i) {
		if (p + i >= s.size() || s[p + i] != lit[i]) return false;
	}
	return true;
}

// &amp; &lt; &gt; &quot; &apos; and numeric references; anything else stays literal.
ustr decode_entities(const ustr& s) {
	if (s.find(u'&') == ustr::npos) return s;
	static const struct {
		const char16_t* name;
		char16_t ch;
	} named[] = {{u"amp;", u'&'}, {u"lt;", u'<'}, {u"gt;", u'>'}, {u"quot;", u'"'}, {u"apos;", u'\''}};
	ustr out;
	for (size_t i = 0; i < s.size();) {
		if (s[i] != u'&') {
			out.push_back(s[i++]);
			continue;
		}
		size_t p = i + 1;
		if (p < s.size() && s[p] == u'#') {
			const bool hex = p + 1 < s.size() && s[p + 1] == u'x';
			size_t q = p + (hex ? 2 : 1);
			const size_t digits = q;
			uint64_t v = 0;
			while (q < s.size() && (hex ? is_hex(s[q]) : (s[q] >= u'0' && s[q] <= u'9'))) {
				v = v * (hex ? 16 : 10) + uint64_t(hex ? hex_val(s[q]) : s[q] - u'0');
				if (v > 0x10ffff) v = 0x110000;
				++q;
			}
			if (q > digits && q < s.size() && s[q] == u';') {
				if (v > 0x10ffff) throw std::runtime_error("xml: invalid character reference");
				push_code_point(out, uint32_t(v));
				i = q + 1;
				continue;
			}
		} else {
			bool hit = false;
			for (const auto& e : named) {
				if (starts_at(s, p, e.name)) {
					out.push_back(e.ch);
					i = p + std::char_traits<char16_t>::length(e.name);
					hit = true;
					break;
				}
			}
			if (hit) continue;
		}
		out.push_back(s[i++]);
	}
	return out;
}

ustr js_trim(const ustr& s) {
	size_t a = 0, b = s.size();
	while (a < b && js_space(s[a])) ++a;
	while (b > a && js_space(s[b - 1])) --b;
	return s.substr(a, b - a);
}

std::unique_ptr<XNode> text_node(ustr t) {
	auto n = std::make_unique<XNode>();
	n->is_text = true;
	n->text = std::move(t);
	return n;
}

}  // namespace

XDoc parse_xml(const ustr& input) {
	const ustr& src0 = input;
	const size_t skip = !src0.empty() && src0[0] == 0xfeff ? 1 : 0;
	const ustr src = src0.substr(skip);
	const size_t n = src.size();
	auto at = [&](size_t i) -> char16_t { return i < n ? src[i] : char16_t(0); };
	auto need = [](size_t pos, const char* what) {
		if (pos == ustr::npos) throw std::runtime_error(std::string("xml: unterminated ") + what);
		return pos;
	};

	XDoc out;
	out.doc = std::make_unique<XNode>();
	out.doc->tag = u"#document";
	XNode* cur = out.doc.get();
	size_t i = 0;
	while (i < n) {
		const size_t lt = src.find(u'<', i);
		const size_t text_end = lt == ustr::npos ? n : lt;
		if (text_end > i) {
			auto t = text_node(decode_entities(src.substr(i, text_end - i)));
			t->parent = cur;
			cur->children.push_back(std::move(t));
		}
		if (lt == ustr::npos) break;
		if (starts_at(src, lt, u"<!--")) {
			i = need(src.find(u"-->", lt + 4), "comment") + 3;
			continue;
		}
		if (starts_at(src, lt, u"<![CDATA[")) {
			const size_t e = need(src.find(u"]]>", lt + 9), "CDATA");
			auto t = text_node(src.substr(lt + 9, e - lt - 9));
			t->parent = cur;
			cur->children.push_back(std::move(t));
			i = e + 3;
			continue;
		}
		if (starts_at(src, lt, u"<?")) {
			i = need(src.find(u"?>", lt + 2), "declaration") + 2;
			continue;
		}
		if (starts_at(src, lt, u"<!")) {
			i = need(src.find(u'>', lt + 2), "declaration") + 1;
			continue;
		}
		if (at(lt + 1) == u'/') {
			const size_t e = need(src.find(u'>', lt), "end tag");
			const ustr tag = js_trim(src.substr(lt + 2, e - lt - 2));
			if (cur->tag != tag || !cur->parent) {
				throw std::runtime_error("xml: mismatched </" + to_utf8(tag) + "> (open <" + to_utf8(cur->tag) + ">)");
			}
			cur = cur->parent;
			i = e + 1;
			continue;
		}
		// start tag
		size_t j = lt + 1;
		while (j < n && !(js_space(src[j]) || src[j] == u'/' || src[j] == u'>')) ++j;
		auto el = std::make_unique<XNode>();
		el->tag = src.substr(lt + 1, j - lt - 1);
		el->parent = cur;
		for (;;) {
			while (j < n && js_space(src[j])) ++j;
			if (at(j) == u'/' && at(j + 1) == u'>') {
				j += 2;
				cur->children.push_back(std::move(el));
				break;
			}
			if (at(j) == u'>') {
				j += 1;
				XNode* raw = el.get();
				cur->children.push_back(std::move(el));
				cur = raw;
				break;
			}
			size_t k = j;
			while (k < n && !(js_space(src[k]) || src[k] == u'=')) ++k;
			if (k >= n) throw std::runtime_error("xml: unterminated start tag");
			const ustr name = src.substr(j, k - j);
			while (k < n && js_space(src[k])) ++k;
			if (at(k) != u'=') throw std::runtime_error("xml: bad attribute");
			++k;
			while (k < n && js_space(src[k])) ++k;
			if (k >= n) throw std::runtime_error("xml: unterminated attribute");
			const char16_t q = src[k];
			const size_t e = need(src.find(q, k + 1), "attribute value");
			ustr value = decode_entities(src.substr(k + 1, e - k - 1));
			bool replaced = false;
			for (auto& a : el->attrs) {
				if (a.first == name) {
					a.second = std::move(value);
					replaced = true;
					break;
				}
			}
			if (!replaced) el->attrs.emplace_back(name, std::move(value));
			j = e + 1;
		}
		i = j;
	}
	for (const auto& c : out.doc->children) {
		if (!c->is_text) {
			out.root = c.get();
			break;
		}
	}
	return out;
}

XDoc read_game_xml(const GameFiles& files, const std::string& name, const std::string& lang_suffix, bool required) {
	const bool has_dir = name.find_first_of("/\\") != std::string::npos;
	const std::string rel = has_dir ? name : "Data/" + name;
	std::string path = files.resolve(rel);
	if (path.empty()) {
		if (!required) return XDoc{};
		throw std::runtime_error("missing " + rel + " under " + files.root());
	}
	if (!lang_suffix.empty()) {
		const std::string alt = files.resolve(rel + lang_suffix);
		if (!alt.empty()) path = alt;
	}
	return parse_xml(decode_text_file(read_file(path)));
}

namespace xml {

namespace {

void raw_text(const XNode* el, ustr& out) {
	for (const auto& c : el->children) {
		if (c->is_text) out += c->text;
		else raw_text(c.get(), out);
	}
}

bool preserves(const XNode* el) {
	for (const XNode* e = el; e; e = e->parent) {
		const ustr* v = e->attr(u"xml:space");
		if (v && !v->empty()) return *v == u"preserve";
	}
	return false;
}

inline bool ws(char16_t c) { return c == u' ' || c == u'\t' || c == u'\r' || c == u'\n'; }

}  // namespace

ustr text(const XNode* el) {
	if (!el) return ustr();
	ustr s;
	raw_text(el, s);
	if (preserves(el)) return s;
	size_t a = 0, b = s.size();
	while (a < b && ws(s[a])) ++a;
	while (b > a && ws(s[b - 1])) --b;
	return s.substr(a, b - a);
}

XNode* child(const XNode* el, const ustr& name, bool exact) {
	if (!el) return nullptr;
	const ustr t = exact ? name : to_upper(name);
	for (const auto& c : el->children) {
		if (!c->is_text && c->tag == t) return c.get();
	}
	return nullptr;
}

std::vector<XNode*> children(const XNode* el, const ustr& name, bool exact) {
	std::vector<XNode*> out;
	if (!el) return out;
	const ustr t = exact ? name : to_upper(name);
	for (const auto& c : el->children) {
		if (!c->is_text && c->tag == t) out.push_back(c.get());
	}
	return out;
}

XNode* child_or_create(XNode* el, const ustr& name) {
	if (XNode* c = child(el, name)) return c;
	auto c = std::make_unique<XNode>();
	c->tag = to_upper(name);
	c->parent = el;
	XNode* raw = c.get();
	el->children.push_back(std::move(c));
	return raw;
}

ustr get_text(const XNode* el, const ustr& name) { return text(child(el, name)); }

int32_t get_text_num(const XNode* el, const ustr& name, int32_t def) {
	const ustr s = get_text(el, name);
	return s.empty() ? def : convert_int(s);
}

ustr get_attrib(const XNode* el, const ustr& name, const ustr& def) {
	if (!el) return def;
	const ustr* v = el->attr(name);
	return v ? *v : def;
}

int32_t get_attrib_num(const XNode* el, const ustr& name, int32_t def) {
	const ustr s = get_attrib(el, name);
	return s.empty() ? def : convert_int(s);
}

}  // namespace xml

// ---------------------------------------------------------------- value parsers

namespace {

inline bool is_digit(char16_t c) { return c >= u'0' && c <= u'9'; }
inline bool iswspace_c(char16_t c) { return c == u' ' || (c >= u'\t' && c <= u'\r'); }

// Iterate a string by code point, yielding the first UTF-16 unit of each (JS `for (const c of s)` + charCodeAt(0)).
template <typename F>
void for_each_code_point(const ustr& s, F&& f) {
	for (size_t i = 0; i < s.size(); ++i) {
		const char16_t c = s[i];
		if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) ++i;
		f(static_cast<uint32_t>(c));
	}
}

}  // namespace

int32_t wtoi(const ustr& s, size_t p) {
	while (p < s.size() && iswspace_c(s[p])) ++p;
	bool neg = false;
	if (p < s.size() && (s[p] == u'+' || s[p] == u'-')) {
		neg = s[p] == u'-';
		++p;
	}
	int64_t v = 0;
	while (p < s.size() && is_digit(s[p])) {
		v = v * 10 + (s[p] - u'0');
		if (v > (int64_t(1) << 31)) v = int64_t(1) << 31;
		++p;
	}
	v = neg ? -v : v;
	return v > 0x7fffffff ? 0x7fffffff : int32_t(v);
}

int32_t parse_binary(const ustr& s, size_t p) {
	uint32_t v = 0;
	for (; p < s.size() && (s[p] == u'0' || s[p] == u'1'); ++p) v = v * 2 + uint32_t(s[p] - u'0');
	return static_cast<int32_t>(v);
}

int32_t parse_hex(const ustr& s, size_t p) {
	uint32_t v = 0;
	for (; p < s.size() && is_hex(s[p]); ++p) v = v * 16 + uint32_t(hex_val(s[p]));
	return static_cast<int32_t>(v);
}

int32_t convert_int(const ustr& s, size_t p) {
	if (p < s.size() && s[p] == u'0' && p + 1 < s.size()) {
		const char16_t c = s[p + 1];
		if (c == u'b' || c == u'B') return parse_binary(s, p + 2);
		if (c == u'x' || c == u'X') return parse_hex(s, p + 2);
		if (c == u'd' || c == u'D') return wtoi(s, p + 2);
	}
	return wtoi(s, p);
}

int32_t fraction(const ustr& s, int32_t mul) {
	const int32_t a = wtoi(s);
	const size_t k = s.find(u'/');
	const int32_t d = k == ustr::npos ? 1 : wtoi(s, k + 1);
	return d == 0 ? 0 : cdiv(imul(a, mul), d);
}

int32_t degrees_to_angle(int32_t d) {
	const int32_t r90 = d % 90, q90 = d / 90, q45 = r90 / 45, r45 = r90 % 45;
	const int32_t q30 = r45 / 30, r30 = r45 % 30, q15 = r30 / 15, r15 = r30 % 15, q5 = r15 / 5;
	int32_t v = imul(iadd(imul(q30, 0x71c71c7), q5), 3);
	v = iadd(v, imul(iadd(q45, imul(q90, 2)), 0x20000000));
	v = iadd(v, imul(q15, 0xaaaaaaa));
	return iadd(v, imul(r15, 0xb60b60));
}

std::vector<CostToken> cost_tokens(const ustr& s) {
	std::vector<CostToken> out;
	size_t p = 0;
	while (p < s.size()) {
		const int32_t amount = convert_int(s, p);
		if (amount != 0) {
			size_t q = p;
			while (q < s.size() && is_digit(s[q])) ++q;
			int32_t res = 0;
			if (q < s.size()) {
				switch (s[q]) {
				case u't': res = 1; break;
				case u'g': res = 2; break;
				case u'k': res = 3; break;
				case u'm': res = 4; break;
				case u'o': res = 5; break;
				default: break;
				}
			}
			out.push_back({amount, res});
		}
		const size_t k = s.find(u'/', p);
		if (k == ustr::npos) break;
		p = k + 1;
	}
	return out;
}

int32_t obj_mask(const ustr& s) {
	uint32_t m = 0;
	for_each_code_point(to_upper(s), [&](uint32_t k) {
		if (k >= 0x31 && k <= 0x39) m |= 1u << ((k - 0x17) & 31);
		else if (k >= 0x41 && k <= 0x5a) m |= 1u << (k - 0x41);
	});
	return static_cast<int32_t>(m);
}

uint32_t lower_letter_flags(const ustr& s) {
	uint32_t m = 0;
	for_each_code_point(to_lower(s), [&](uint32_t k) { m |= 1u << ((k >= 0x31 && k <= 0x39 ? k - 0x17 : k - 0x61) & 31); });
	return m;
}

uint32_t upper_letter_flags(const ustr& s) {
	uint32_t m = 0;
	for_each_code_point(to_upper(s), [&](uint32_t k) { m |= 1u << ((k >= 0x31 && k <= 0x39 ? k - 0x17 : k - 0x41) & 31); });
	return m;
}

}  // namespace ron::sim
