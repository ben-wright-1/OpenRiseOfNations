#include "assets/buildings.hpp"

#include "assets/xml.hpp"

namespace ron {

namespace {

BuildAnim build_anim(const std::string& name) {
	static const char* names[] = {"BUILD_DEFAULT",    "BUILD_BUILD",         "BUILD_EMPTY_IDLE",
	                              "BUILD_NUKE_BUILD", "BUILD_NUKE_IDLE",     "BUILD_NUKE_LAUNCH",
	                              "BUILD_MISSILE_BUILD", "BUILD_MISSILE_IDLE", "BUILD_MISSILE_LAUNCH"};
	for (int i = 0; i < 9; i++) {
		if (name == names[i]) return BuildAnim(i);
	}
	return BuildAnim::Default;
}

}  // namespace

const BuildGraphic* BuildCatalog::find(const std::string& name) const {
	auto it = by_lower.find(to_lower(name));
	return it == by_lower.end() ? nullptr : &builds[it->second];
}

const BuildGraphic* BuildCatalog::find_variant(const std::string& type, const std::string& style, int age,
                                               const std::string& backup_style) const {
	for (const std::string& s : {style, backup_style, std::string("DEFAULT")}) {
		if (s.empty()) continue;
		for (int a = age; a >= 0; a--) {
			if (const BuildGraphic* g = find(type + "-" + s + "-AGE" + std::to_string(a))) return g;
		}
	}
	return find(type + "-DEFAULT-AGE0");
}

BuildCatalog load_build_catalog(const GameFiles& files, const std::map<std::string, std::string>& anim_names) {
	BuildCatalog cat;
	for_each_element(parse_xml(files.read_text("Data/building_graphics.xml")), "BUILD", [&](const XmlElement& e) {
		BuildGraphic g;
		g.name = e.get("name");
		if (g.name.empty()) return;
		g.texture = normalize_game_path(e.get("texture"));
		g.zbuffer = normalize_game_path(e.get("zbuffer"));
		g.spritexoff = e.get_int("spritexoff", 0);
		g.spriteyoff = e.get_int("spriteyoff", 0);
		g.halfoffset = e.get_int("halfoffset", 0) != 0;
		g.constructsize = e.get_int("constructsize", 0);
		for (const XmlElement& c : e.children) {
			if (c.name != "EXISTEVENT") continue;
			BuildAddon a;
			a.anim = build_anim(c.get("buildanim"));
			a.subject = c.get("subject");
			auto it = anim_names.find(c.get("anim"));
			if (it != anim_names.end()) a.anim_file = it->second;
			a.axis = static_cast<signed char>(c.get_int("axis", -1));
			a.node = static_cast<signed char>(c.get_int("node", 7));
			g.addons.push_back(std::move(a));
		}
		cat.by_lower.emplace(to_lower(g.name), cat.builds.size());
		cat.builds.push_back(std::move(g));
	});
	// Farms are a separate <FARM texture=...><DEFAULT><AGE0 spritexoff ...>, registered here under the name "FARM". The game
	// draws no farm sprite (docs/farms-spec.md): only the texture is used, as a fallback for the tileset's FARMTEX.
	for_each_element(parse_xml(files.read_text("Data/building_graphics.xml")), "FARM", [&](const XmlElement& e) {
		BuildGraphic g;
		g.name = "FARM";
		g.texture = normalize_game_path(e.get("texture"));
		const XmlElement* def = e.child("DEFAULT");
		const XmlElement* age = def ? def->child("AGE0") : nullptr;
		if (age) {
			g.spritexoff = age->get_int("spritexoff", 0);
			g.spriteyoff = age->get_int("spriteyoff", 0);
			g.halfoffset = age->get_int("halfoffset", 0) != 0;
		}
		cat.by_lower.emplace("farm", cat.builds.size());
		cat.builds.push_back(std::move(g));
	});
	return cat;
}

std::vector<ConstructionSprite> load_construction_sprites(const GameFiles& files) {
	std::vector<ConstructionSprite> out;
	for_each_element(parse_xml(files.read_text("Data/building_graphics.xml")), "CONSTRUCTION", [&](const XmlElement& e) {
		ConstructionSprite c;
		c.texture = normalize_game_path(e.get("texture"));
		c.model = normalize_game_path(e.get("model"));
		c.spritexoff = e.get_int("spritexoff");
		c.spriteyoff = e.get_int("spriteyoff");
		out.push_back(c);
	});
	return out;
}

const ScaffoldPiece* ScaffoldSet::find(int age_set, const std::string& name) const {
	if (pieces_per_age <= 0) return nullptr;
	const size_t first = size_t(age_set) * size_t(pieces_per_age);
	for (size_t i = first; i < first + size_t(pieces_per_age) && i < pieces.size(); i++) {
		if (to_lower(pieces[i].name) == to_lower(name)) return &pieces[i];
	}
	return nullptr;
}

ScaffoldSet load_scaffolds(const GameFiles& files, const std::map<std::string, std::string>& anim_names) {
	ScaffoldSet set;
	const XmlElement doc = parse_xml(files.read_text("Data/effects_graphics.xml"));
	for_each_element(doc, "SCAFFOLD", [&](const XmlElement& e) {
		ScaffoldPiece p;
		p.name = e.get("name");
		p.model = normalize_game_path(e.get("model"));
		p.texture = normalize_game_path(e.get("texture"));
		p.renderstate = e.get("renderstate");
		p.anim = e.get("anim");
		auto it = anim_names.find(p.anim);
		if (it != anim_names.end()) p.anim_file = it->second;
		set.pieces.push_back(p);
	});
	// Three age sets (num_scaffold_pieces_per_age): ages 0-2, 3-4, 5+.
	set.pieces_per_age = int(set.pieces.size() / 3);
	for_each_element(doc, "SCAFFOLD_TIMING", [&](const XmlElement& e) {
		std::vector<ScaffoldTiming> list;
		for (const XmlElement& t : e.children) {
			if (to_lower(t.name) != "timing") continue;
			ScaffoldTiming s;
			s.name = t.get("name");
			s.start = t.get_float("start_percent");
			s.end = t.get_float("end_percent", 1.0f);
			s.x = t.get_float("x"), s.y = t.get_float("y"), s.z = t.get_float("z"), s.angle = t.get_float("angle");
			list.push_back(s);
		}
		set.timings.push_back(list);
	});
	return set;
}

}  // namespace ron
