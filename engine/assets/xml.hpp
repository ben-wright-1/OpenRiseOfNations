// Just enough XML for the game's data files: an element tree with attributes. Comments and declarations are skipped;
// an element's trimmed character data is kept as the pseudo-attribute "#text". Attribute names are lowercased
// (the game's files are inconsistent about case).
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ron {

struct XmlElement {
	std::string name;
	std::map<std::string, std::string> attrs;
	std::vector<XmlElement> children;
	// The attributes as written (original-case names, document order); `attrs` is the lowercased lookup table.
	std::vector<std::pair<std::string, std::string>> raw_attrs;

	// Attribute value, or `fallback` when absent. `key` must be lowercase.
	std::string get(const std::string& key, const std::string& fallback = std::string()) const;
	float get_float(const std::string& key, float fallback = 0.0f) const;
	int get_int(const std::string& key, int fallback = 0) const;
	bool has(const std::string& key) const { return attrs.count(key) != 0; }

	// First direct child named `n`, or null.
	const XmlElement* child(const std::string& n) const;
};

// Parses `text` and returns a synthetic root element (name "") whose children are the document's top-level
// elements. Tolerant: a stray closing tag closes the nearest open element with that name, or is ignored.
XmlElement parse_xml(const std::string& text);

// Depth-first visit of every element named `name` under `root`.
template <typename F>
void for_each_element(const XmlElement& root, const std::string& name, F&& f) {
	for (const XmlElement& c : root.children) {
		if (c.name == name) f(c);
		for_each_element(c, name, f);
	}
}

}  // namespace ron
