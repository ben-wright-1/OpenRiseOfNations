// Localised string tables (docs/ui-spec.md section 3.5): internal_strings.xml (int_str_array), translated_strings.xml
// (loc_str_array), string_lookup.xml (String::lookup by category), typenames.xml, and the String hash
// (String::generate_hash @00a1b6b0). Reference: re/tools/locstrings.mjs.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "assets/files.hpp"

namespace ron {

// The five languages EE can select (LocalizationManager) and their file suffixes (gLanguageSuffix @00b1ca54):
// a localised "name.xml.7" replaces "name.xml"; English uses the bare file.
enum class Language { German, English, Spanish, French, Italian };
const char* language_suffix(Language l);
// "en", "de", "es", "fr", "it" (or the suffix itself, ".7") -> Language; English for anything else.
Language parse_language(const std::string& s);

struct StringHash {
	uint32_t hash = 0, hash_i = 0;  // case-sensitive and towlower variants
};
// String::generate_hash over the UTF-16 units of a UTF-8 string.
StringHash string_hash(const std::string& utf8);
// The same over UTF-16 units (BigHugeScript type ids are the case-insensitive variant, docs/script-spec.md 0).
StringHash string_hash16(const std::u16string& s);

class StringTables {
public:
	StringTables() = default;
	StringTables(const GameFiles& files, Language lang);

	// Positional tables: compiled code addresses element i at byte offset i * 0x14 (sizeof String).
	const std::vector<std::string>& internal() const { return internal_; }
	const std::vector<std::string>& translated() const { return translated_; }
	std::string internal_at_offset(uint32_t offset) const;
	std::string translated_at_offset(uint32_t offset) const;
	// String::lookup @00a1ddb0: the localised text for English `text` in `category`, or `text` itself.
	std::string lookup(const std::string& text, const std::string& category) const;
	// typenames.xml: display name by the hash of the English name (or "" if absent).
	std::string type_name(const std::string& english) const;
	// Category hash -> name, recovered by hashing every internal string (diagnostics).
	std::map<uint32_t, std::string> category_names() const;
	size_t category_count() const { return cats_.size(); }

	struct Entry {
		uint32_t hash = 0, hash_i = 0;
		std::string text;
	};
	const std::map<uint32_t, std::vector<Entry>>& categories() const { return cats_; }

private:
	std::vector<std::string> internal_, translated_;
	std::map<uint32_t, std::vector<Entry>> cats_;
	std::map<uint32_t, std::string> typenames_;
};

// The XML text of a localised data file: Data/<name><suffix> if present, else Data/<name>.
std::string read_localised(const GameFiles& files, const std::string& name, Language lang);

// UTF-8 <-> UTF-16 helpers used by the hash and the text layer.
std::u16string utf8_to_utf16(const std::string& s);
std::u32string utf8_to_utf32(const std::string& s);

}  // namespace ron
