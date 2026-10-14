#include "assets/effects.hpp"

#include "assets/xml.hpp"

namespace ron {

const AmmoPiece* AmmoCatalog::find(const std::string& name) const {
	auto it = by_lower.find(to_lower(name));
	return it == by_lower.end() ? nullptr : &pieces[it->second];
}

AmmoCatalog load_ammo_catalog(const GameFiles& files, const std::map<std::string, std::string>& anim_names, const char* tag) {
	AmmoCatalog cat;
	for_each_element(parse_xml(files.read_text("Data/effects_graphics.xml")), tag, [&](const XmlElement& e) {
		AmmoPiece p;
		p.name = e.get("name");
		p.model = normalize_game_path(e.get("model"));
		p.texture = normalize_game_path(e.get("texture"));
		p.renderstate = e.get("renderstate");
		auto it = anim_names.find(e.get("anim"));
		if (it != anim_names.end()) p.anim = it->second;
		cat.by_lower.emplace(to_lower(p.name), cat.pieces.size());  // emplace keeps the first
		cat.pieces.push_back(std::move(p));
	});
	return cat;
}

}  // namespace ron
