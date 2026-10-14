#include "assets/catalog.hpp"

#include "assets/xml.hpp"

namespace ron {

std::map<std::string, std::string> load_anim_names(const GameFiles& files) {
	std::map<std::string, std::string> names;
	for_each_element(parse_xml(files.read_text("Data/anim_graphics.xml")), "ANIM", [&](const XmlElement& e) {
		if (!e.get("name").empty()) names[e.get("name")] = normalize_game_path(e.get("file"));
	});
	return names;
}

std::vector<UnitGraphic> load_unit_catalog(const GameFiles& files) {
	const auto anim_files = load_anim_names(files);
	std::vector<UnitGraphic> units;
	for_each_element(parse_xml(files.read_text("Data/unit_graphics.xml")), "UNIT", [&](const XmlElement& e) {
		UnitGraphic u;
		u.name = e.get("name");
		u.model = normalize_game_path(e.get("model"));
		if (u.model.empty()) return;
		u.texture = normalize_game_path(e.get("texture"));
		u.scale = e.get_float("scale", 1.0f);
		u.tex_id = e.get("tex_id");
		while (!u.tex_id.empty() && u.tex_id.back() == ' ') u.tex_id.pop_back();
		u.tex_col = e.get_int("tex_col");
		u.tex_row = e.get_int("tex_row");
		u.tex_x = e.get_int("tex_x", -1);
		u.tex_y = e.get_int("tex_y", -1);
		for (const XmlElement& c : e.children) {
			if (c.name == "ANIM") {
				auto it = anim_files.find(c.get("file"));
				u.anims.push_back({c.get("name"), it == anim_files.end() ? std::string() : it->second});
			} else if (c.name == "EXISTEVENT") {
				// Absent times parse as -1 into a u16 (GraphicPieces::init_unit_events), i.e. 0xFFFF.
				ExistEvent ev;
				ev.action = c.get("anim");
				ev.start_ms = uint16_t(c.get_int("starttime", -1));
				ev.end_ms = uint16_t(c.get_int("endtime", -1));
				ev.type = c.get("type");
				ev.node = static_cast<signed char>(c.get_int("node", -1));
				u.exist_events.push_back(std::move(ev));
			}
		}
		units.push_back(std::move(u));
	});
	return units;
}

}  // namespace ron
