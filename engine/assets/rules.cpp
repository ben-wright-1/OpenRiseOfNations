#include "assets/rules.hpp"

#include <cctype>
#include <cstdlib>

#include "assets/xml.hpp"

namespace ron {

namespace {

std::string trim(const std::string& s) {
	const size_t a = s.find_first_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string child_text(const XmlElement& e, const std::string& name) {
	const XmlElement* c = e.child(name);
	return c ? trim(c->get("#text")) : std::string();
}

void read_graphs(const GameFiles& files, const std::string& path, const std::string& element, std::map<std::string, std::string>& out) {
	for_each_element(parse_xml(files.read_text(path)), element, [&](const XmlElement& e) {
		std::string key = child_text(e, "TYPENAME");
		if (key.empty()) key = child_text(e, "NAME");
		const std::string graph = child_text(e, "GRAPH");
		if (!key.empty() && !graph.empty()) out.emplace(key, graph);  // emplace keeps the first
	});
}

std::vector<std::string> read_tribe_list(const GameFiles& files, const XmlElement& tribes, const std::string& base_dir) {
	std::vector<std::string> out;
	for (const XmlElement& t : tribes.children) {
		if (t.name != "TRIBE") continue;
		const std::string f = normalize_game_path(child_text(t, "FILE"));
		// Base-list entries are bare file names in tribes/; overrides carry a path from the game root.
		out.push_back(f.find('/') == std::string::npos ? base_dir + f : f);
	}
	(void)files;
	return out;
}

std::string squash(const std::string& s) {  // lowercase, without spaces and underscores ("Cold War" == "ColdWar")
	std::string r;
	for (char c : s) {
		if (c != ' ' && c != '_') r += char(std::tolower(static_cast<unsigned char>(c)));
	}
	return r;
}

}  // namespace

int Rules::age(const std::vector<std::string>& techs) const {
	int a = 0;
	for (const std::string& t : techs) a += age_techs.count(t) ? 1 : 0;
	return std::min(a, 7);
}

Rules load_rules(const GameFiles& files) {
	Rules r;
	read_graphs(files, "Data/unitrules.xml", "UNIT", r.unit_graph);
	read_graphs(files, "Data/buildingrules.xml", "BUILDING", r.build_graph);
	for_each_element(parse_xml(files.read_text("Data/techrules.xml")), "NAME", [&](const XmlElement& e) {
		const std::string n = trim(e.get("#text"));
		if (n.size() > 4 && n.compare(n.size() - 4, 4, " Age") == 0) r.age_techs.insert(n);
	});
	const XmlElement rules = parse_xml(files.read_text("Data/rules.xml"));
	for_each_element(rules, "TRIBES", [&](const XmlElement& e) {
		if (r.tribe_files.empty()) r.tribe_files = read_tribe_list(files, e, "tribes/");
	});
	return r;
}

std::vector<std::string> conquest_tribe_files(const GameFiles& files, const std::string& scenario_path) {
	const std::string p = normalize_game_path(scenario_path);
	const size_t at = to_lower(p).find("conquest/");
	if (at == std::string::npos) return {};
	const size_t start = at + 9, end = p.find('/', start);
	if (end == std::string::npos) return {};
	const std::string campaign = squash(p.substr(start, end - start));
	for (const std::string& f : files.list_dir("conquest")) {
		const std::string name = f.substr(f.rfind('/') + 1);
		if (name.rfind("ctw_", 0) != 0 || name.size() < 16 || name.compare(name.size() - 11, 11, "_map_01.xml") != 0) continue;
		if (squash(name.substr(4, name.size() - 15)) != campaign) continue;
		std::vector<std::string> out;
		for_each_element(parse_xml(files.read_text(f)), "TRIBES", [&](const XmlElement& e) {
			if (out.empty()) out = read_tribe_list(files, e, "tribes/");
		});
		return out;
	}
	return {};
}

TribeStyle load_tribe_style(const GameFiles& files, const std::string& tribe_file) {
	TribeStyle s;
	if (files.resolve(tribe_file).empty()) return s;
	const XmlElement doc = parse_xml(files.read_text(tribe_file));
	auto num = [&](const char* name, int fallback) {
		int v = fallback;
		for_each_element(doc, name, [&](const XmlElement& e) { v = std::atoi(trim(e.get("#text")).c_str()); });
		return v;  // only the leading integer counts ("0 Med")
	};
	s.build_style = num("BUILD_CONTINENT", 0);
	s.backup_style = num("BACKUP_BUILD_CONTINENT", s.build_style);
	s.unit_style = num("UNIT_CONTINENT", 0);
	return s;
}

const char* build_style_name(int style) {
	static const char* names[] = {"DEFAULT", "NEUROPE", "ASIAN", "KOREAN", "AMERICAN", "ARAB", "IROQUOIS", "COLONIAL", "EINDIAN"};
	return style >= 0 && style < 9 ? names[style] : "DEFAULT";
}

const char* unit_style_name(int style) {
	static const char* names[] = {"DEFAULT", "ARAB", "AMERICAN", "ASIAN", "NA", "INDIA"};
	return style >= 0 && style < 6 ? names[style] : "DEFAULT";
}

}  // namespace ron
