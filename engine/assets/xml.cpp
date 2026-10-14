#include "assets/xml.hpp"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace ron {

namespace {

std::string lower(std::string s) {
	for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string decode_entities(const std::string& s) {
	if (s.find('&') == std::string::npos) return s;
	static const std::pair<const char*, char> table[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
	std::string out;
	for (size_t i = 0; i < s.size();) {
		bool hit = false;
		for (const auto& [ent, ch] : table) {
			const size_t n = std::char_traits<char>::length(ent);
			if (s.compare(i, n, ent) == 0) {
				out += ch;
				i += n;
				hit = true;
				break;
			}
		}
		if (!hit) out += s[i++];
	}
	return out;
}

struct Tag {
	std::string name;
	std::map<std::string, std::string> attrs;
	std::vector<std::pair<std::string, std::string>> raw_attrs;
	bool closing = false;
	bool self_closing = false;
	std::string text_before;  // character data between the previous markup and this tag
};

class TagScanner {
public:
	explicit TagScanner(const std::string& text) : s_(text) {}

	bool next(Tag& t) {
		for (;;) {
			const size_t lt = s_.find('<', p_);
			if (lt == std::string::npos) return false;
			const size_t text_start = p_;
			if (s_.compare(lt, 4, "<!--") == 0) {
				const size_t e = s_.find("-->", lt + 4);
				p_ = e == std::string::npos ? s_.size() : e + 3;
				continue;
			}
			if (s_.compare(lt, 2, "<?") == 0 || s_.compare(lt, 2, "<!") == 0) {
				const size_t e = s_.find('>', lt);
				p_ = e == std::string::npos ? s_.size() : e + 1;
				continue;
			}
			t = Tag{};
			t.text_before = s_.substr(text_start, lt - text_start);
			size_t q = lt + 1;
			if (q < s_.size() && s_[q] == '/') {
				t.closing = true;
				q++;
			}
			const size_t name_start = q;
			while (q < s_.size() && !space(q) && s_[q] != '>' && s_[q] != '/') q++;
			t.name = s_.substr(name_start, q - name_start);
			while (q < s_.size()) {
				while (q < s_.size() && space(q)) q++;
				if (q >= s_.size()) break;
				if (s_[q] == '>') {
					q++;
					break;
				}
				if (s_[q] == '/') {
					t.self_closing = true;
					q++;
					continue;
				}
				const size_t an = q;
				while (q < s_.size() && s_[q] != '=' && !space(q) && s_[q] != '>' && s_[q] != '/') q++;
				const std::string raw_key = s_.substr(an, q - an);
				const std::string key = lower(raw_key);
				while (q < s_.size() && space(q)) q++;
				if (q < s_.size() && s_[q] == '=') {
					q++;
					while (q < s_.size() && space(q)) q++;
					if (q < s_.size() && (s_[q] == '"' || s_[q] == '\'')) {
						const char quote = s_[q];
						size_t end = s_.find(quote, q + 1);
						if (end == std::string::npos) end = s_.size();
						t.attrs[key] = decode_entities(s_.substr(q + 1, end - q - 1));
						t.raw_attrs.emplace_back(raw_key, t.attrs[key]);
						q = end + 1;
					} else {
						const size_t vs = q;
						while (q < s_.size() && !space(q) && s_[q] != '>') q++;
						t.attrs[key] = s_.substr(vs, q - vs);
						t.raw_attrs.emplace_back(raw_key, t.attrs[key]);
					}
				} else if (key.empty()) {
					q++;  // stray character; skip it
				} else {
					t.attrs[key] = "";
					t.raw_attrs.emplace_back(raw_key, "");
				}
			}
			p_ = q;
			return true;
		}
	}

private:
	bool space(size_t i) const { return std::isspace(static_cast<unsigned char>(s_[i])) != 0; }
	const std::string& s_;
	size_t p_ = 0;
};

}  // namespace

std::string XmlElement::get(const std::string& key, const std::string& fallback) const {
	auto it = attrs.find(key);
	return it == attrs.end() ? fallback : it->second;
}

float XmlElement::get_float(const std::string& key, float fallback) const {
	auto it = attrs.find(key);
	return it == attrs.end() || it->second.empty() ? fallback : float(std::atof(it->second.c_str()));
}

int XmlElement::get_int(const std::string& key, int fallback) const {
	auto it = attrs.find(key);
	return it == attrs.end() || it->second.empty() ? fallback : std::atoi(it->second.c_str());
}

const XmlElement* XmlElement::child(const std::string& n) const {
	for (const XmlElement& c : children) {
		if (c.name == n) return &c;
	}
	return nullptr;
}

XmlElement parse_xml(const std::string& text) {
	XmlElement root;
	std::vector<XmlElement*> stack{&root};
	TagScanner scan(text);
	Tag t;
	while (scan.next(t)) {
		const size_t a = t.text_before.find_first_not_of(" \t\r\n");
		if (a != std::string::npos && stack.size() > 1) {
			const size_t b = t.text_before.find_last_not_of(" \t\r\n");
			stack.back()->attrs["#text"] += decode_entities(t.text_before.substr(a, b - a + 1));
		}
		if (t.closing) {
			for (size_t i = stack.size(); i-- > 1;) {
				if (stack[i]->name == t.name) {
					stack.resize(i);
					break;
				}
			}
			continue;
		}
		XmlElement& parent = *stack.back();
		parent.children.push_back(XmlElement{std::move(t.name), std::move(t.attrs), {}, std::move(t.raw_attrs)});
		if (!t.self_closing) stack.push_back(&parent.children.back());
	}
	return root;
}

}  // namespace ron
