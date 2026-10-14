#include "assets/strings.hpp"

#include <cstdlib>

#include "assets/xml.hpp"

namespace ron {

namespace {

// basic:str.obj primes[50] (0x00b14500)
const uint32_t kPrimes[50] = {127,   811,   1597,  2131,  2749,  4759,  5527,  5953,  8117,  9539,  10273, 10753, 11159,
                              12301, 13217, 14207, 15413, 17681, 18661, 19013, 21089, 22051, 25111, 25801, 27457, 28057,
                              29581, 30809, 32611, 34469, 36067, 37511, 38723, 40093, 41983, 43321, 45083, 47431, 49667,
                              50767, 53453, 55469, 57193, 59369, 61987, 65071, 73421, 77849, 84223, 89009};

// towlower for the characters the tables use (ASCII and Latin-1).
char16_t lower16(char16_t c) {
	if (c >= 'A' && c <= 'Z') return char16_t(c + 32);
	if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return char16_t(c + 32);
	return c;
}

void append_utf8(std::string& out, uint32_t cp) {
	if (cp < 0x80) {
		out += char(cp);
	} else if (cp < 0x800) {
		out += char(0xC0 | (cp >> 6));
		out += char(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += char(0xE0 | (cp >> 12));
		out += char(0x80 | ((cp >> 6) & 0x3F));
		out += char(0x80 | (cp & 0x3F));
	} else {
		out += char(0xF0 | (cp >> 18));
		out += char(0x80 | ((cp >> 12) & 0x3F));
		out += char(0x80 | ((cp >> 6) & 0x3F));
		out += char(0x80 | (cp & 0x3F));
	}
}

// Numeric character references (&#N; / &#xH;); the XML reader decodes only the five named entities.
std::string decode_numeric(const std::string& s) {
	if (s.find("&#") == std::string::npos) return s;
	std::string out;
	for (size_t i = 0; i < s.size();) {
		if (s.compare(i, 2, "&#") == 0) {
			const size_t semi = s.find(';', i);
			if (semi != std::string::npos && semi - i < 12) {
				const bool hex = s[i + 2] == 'x' || s[i + 2] == 'X';
				const std::string num = s.substr(i + (hex ? 3 : 2), semi - i - (hex ? 3 : 2));
				append_utf8(out, uint32_t(std::strtoul(num.c_str(), nullptr, hex ? 16 : 10)));
				i = semi + 1;
				continue;
			}
		}
		out += s[i++];
	}
	return out;
}

std::string text_of(const XmlElement& e) { return decode_numeric(e.get("#text")); }

}  // namespace

const char* language_suffix(Language l) {
	switch (l) {
		case Language::German: return ".7";
		case Language::Spanish: return ".10";
		case Language::French: return ".12";
		case Language::Italian: return ".16";
		default: return "";
	}
}

Language parse_language(const std::string& s) {
	const std::string l = to_lower(s);
	if (l == "de" || l == "german" || l == ".7") return Language::German;
	if (l == "es" || l == "spanish" || l == ".10") return Language::Spanish;
	if (l == "fr" || l == "french" || l == ".12") return Language::French;
	if (l == "it" || l == "italian" || l == ".16") return Language::Italian;
	return Language::English;
}

std::u32string utf8_to_utf32(const std::string& s) {
	std::u32string out;
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		uint32_t cp = c;
		int n = 0;
		if (c >= 0xF0) cp = c & 0x07, n = 3;
		else if (c >= 0xE0) cp = c & 0x0F, n = 2;
		else if (c >= 0xC0) cp = c & 0x1F, n = 1;
		i++;
		for (int k = 0; k < n && i < s.size(); k++, i++) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
		out += char32_t(cp);
	}
	return out;
}

std::u16string utf8_to_utf16(const std::string& s) {
	std::u16string out;
	for (char32_t cp : utf8_to_utf32(s)) {
		if (cp >= 0x10000) {
			cp -= 0x10000;
			out += char16_t(0xD800 + (cp >> 10));
			out += char16_t(0xDC00 + (cp & 0x3FF));
		} else {
			out += char16_t(cp);
		}
	}
	return out;
}

StringHash string_hash(const std::string& utf8) { return string_hash16(utf8_to_utf16(utf8)); }

StringHash string_hash16(const std::u16string& s) {
	uint32_t h = 0, hi = 0, n = uint32_t(s.size());
	for (size_t u = s.size(); u-- > 0;) {
		const uint32_t c = s[u], l = lower16(s[u]);
		h += kPrimes[u % 50] * c + kPrimes[c % 50] * n;
		hi += kPrimes[l % 50] * n + l * kPrimes[u % 50];
		n--;
	}
	return {h, hi};
}

std::string read_localised(const GameFiles& files, const std::string& name, Language lang) {
	const std::string loc = "Data/" + name + language_suffix(lang);
	if (!files.resolve(loc).empty()) return files.read_text(loc);
	return files.read_text("Data/" + name);
}

StringTables::StringTables(const GameFiles& files, Language lang) {
	auto table = [&](const std::string& name, std::vector<std::string>& out) {
		const XmlElement doc = parse_xml(read_localised(files, name, lang));
		for_each_element(doc, "STRING", [&](const XmlElement& e) { out.push_back(text_of(e)); });
	};
	table("internal_strings.xml", internal_);
	table("translated_strings.xml", translated_);
	{
		const XmlElement doc = parse_xml(read_localised(files, "string_lookup.xml", lang));
		for_each_element(doc, "CAT", [&](const XmlElement& cat) {
			std::vector<Entry>& list = cats_[uint32_t(std::strtoul(cat.get("hash").c_str(), nullptr, 10))];
			for (const XmlElement& s : cat.children) {
				if (s.name != "STRING") continue;
				list.push_back({uint32_t(std::strtoul(s.get("hash", "4294967295").c_str(), nullptr, 10)),
				                uint32_t(std::strtoul(s.get("hash_i", "4294967295").c_str(), nullptr, 10)), text_of(s)});
			}
		});
	}
	{
		const XmlElement doc = parse_xml(read_localised(files, "typenames.xml", lang));
		for_each_element(doc, "STRING", [&](const XmlElement& e) {
			if (e.has("hash")) typenames_[uint32_t(std::strtoul(e.get("hash").c_str(), nullptr, 10))] = text_of(e);
		});
	}
}

std::string StringTables::internal_at_offset(uint32_t offset) const {
	const size_t i = offset / 0x14;
	return offset % 0x14 == 0 && i < internal_.size() ? internal_[i] : std::string();
}

std::string StringTables::translated_at_offset(uint32_t offset) const {
	const size_t i = offset / 0x14;
	return offset % 0x14 == 0 && i < translated_.size() ? translated_[i] : std::string();
}

std::string StringTables::lookup(const std::string& text, const std::string& category) const {
	auto it = cats_.find(string_hash(category).hash);
	if (it == cats_.end()) return text;
	const StringHash h = string_hash(text);
	for (const Entry& e : it->second) {
		if (e.hash == h.hash) return e.text;
	}
	for (const Entry& e : it->second) {
		if (e.hash_i == h.hash_i) return e.text;
	}
	return text;
}

std::string StringTables::type_name(const std::string& english) const {
	auto it = typenames_.find(string_hash(english).hash);
	return it == typenames_.end() ? std::string() : it->second;
}

std::map<uint32_t, std::string> StringTables::category_names() const {
	std::map<uint32_t, std::string> names;
	for (const std::string& s : internal_) names.emplace(string_hash(s).hash, s);
	return names;
}

}  // namespace ron
