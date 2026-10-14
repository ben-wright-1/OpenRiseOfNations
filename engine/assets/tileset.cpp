#include "assets/tileset.hpp"

#include <stdexcept>

#include "assets/xml.hpp"

namespace ron {

namespace {

// Texture names resolve against BASEDIR, then "terrain art/", then "art/" (case-insensitively).
std::string resolve_texture(const GameFiles& files, const std::string& basedir, const std::string& name) {
	const std::string n = normalize_game_path(name);
	for (const std::string& dir : {basedir, std::string("terrain art/"), std::string("art/"), std::string()}) {
		std::string candidate = dir + n;
		// Collapse "dir/../x" so the lookup table (which has no ".." entries) can find it.
		for (size_t p; (p = candidate.find("/../")) != std::string::npos;) {
			const size_t prev = candidate.rfind('/', p == 0 ? 0 : p - 1);
			candidate.erase(prev == std::string::npos ? 0 : prev + 1, p + 4 - (prev == std::string::npos ? 0 : prev + 1));
		}
		if (!files.resolve(candidate).empty()) return candidate;
	}
	return std::string();
}

std::vector<TilesetTexture> read_list(const GameFiles& files, const std::string& basedir, const XmlElement* list) {
	std::vector<TilesetTexture> out;
	if (!list) return out;
	for (const XmlElement& base : list->children) {
		if (base.name != "BASE") continue;
		const XmlElement* tex = base.child("TEXTURE");
		TilesetTexture t;
		t.name = base.get("name");
		t.supertile = base.get_int("supertile", 0) != 0;
		if (tex) t.file = resolve_texture(files, basedir, tex->get("name"));
		out.push_back(std::move(t));
	}
	return out;
}

void read_light(const XmlElement* e, float rgb[3]) {
	if (!e) return;
	float a = 1, r = rgb[0], g = rgb[1], b = rgb[2];
	for (const XmlElement& c : e->children) {
		const std::string n = c.get("name");
		// <COLOR name="red">0.01</COLOR>: the value is the element's text.
		const float v = c.get_float("#text", 0.0f);
		if (n == "alpha") a = v;
		if (n == "red") r = v;
		if (n == "green") g = v;
		if (n == "blue") b = v;
	}
	rgb[0] = r * a, rgb[1] = g * a, rgb[2] = b * a;
}

}  // namespace

Tileset load_tileset(const GameFiles& files, const std::string& name) {
	const XmlElement root = parse_xml(files.read_text("Data/tilesets.xml"));
	const XmlElement* found = nullptr;
	for_each_element(root, "TILESET", [&](const XmlElement& e) {
		if (!found && to_lower(e.get("name")) == to_lower(name)) found = &e;
	});
	if (!found) throw std::runtime_error("tileset not found: " + name);
	Tileset t;
	t.name = found->get("name");
	if (const XmlElement* bd = found->child("BASEDIR")) {
		t.basedir = normalize_game_path(bd->get("path"));
		if (!t.basedir.empty() && t.basedir.back() != '/') t.basedir += '/';
	}
	t.baseland = read_list(files, t.basedir, found->child("BASELAND"));
	t.coastal = read_list(files, t.basedir, found->child("COASTAL"));
	t.ocean = read_list(files, t.basedir, found->child("OCEAN"));
	if (const XmlElement* trees = found->child("TREES")) t.treescale = trees->get_float("treescale", 1.0f);
	if (const XmlElement* group = found->child("TERRAINGROUP")) {
		if (const XmlElement* cb = group->child("COAST_BASE")) t.coast_base = cb->get_int("value", 0);
	}
	t.snowcaps = found->get_int("snowcaps", 0) != 0;
	auto river = [&](const char* element) {
		const XmlElement* e = found->child(element);
		const XmlElement* tex = e ? e->child("TEXTURE") : nullptr;
		return tex ? resolve_texture(files, t.basedir, tex->get("file")) : std::string();
	};
	t.river_normal = river("RIVERTEX_NORMAL");
	t.river_delta = river("RIVERTEX_DELTA");
	t.river_halfdelta = river("RIVERTEX_HALFDELTA");
	t.river_water = river("RIVERTEX_WATER");
	if (const XmlElement* mtn = found->child("MOUNTAINS")) {
		if (const XmlElement* b = mtn->child("BASETEX")) t.mountain_base = resolve_texture(files, t.basedir, b->get("file"));
		if (const XmlElement* a = mtn->child("ALPHATEX")) t.mountain_alpha = resolve_texture(files, t.basedir, a->get("file"));
	}
	if (const XmlElement* under = found->child("UNDERLAYS")) {
		for (const XmlElement& tex : under->children) {
			if (tex.name != "TEXTURE") continue;
			const std::string id = to_lower(tex.get("id"));
			if (id == "snowcap") t.snowcap = resolve_texture(files, t.basedir, tex.get("file"));
			if (id == "wheatfarm") t.wheat_underlay = resolve_texture(files, t.basedir, tex.get("file"));
			if (id == "pigfarm") t.animal_underlay = resolve_texture(files, t.basedir, tex.get("file"));
		}
	}
	if (const XmlElement* fog = found->child("FOGTEX")) {
		if (const XmlElement* tex = fog->child("TEXTURE")) t.fog = resolve_texture(files, t.basedir, tex->get("file"));
	}
	if (const XmlElement* farm = found->child("FARMTEX")) {
		if (const XmlElement* tex = farm->child("TEXTURE")) t.farm_wheat = resolve_texture(files, t.basedir, tex->get("file"));
	}
	if (const XmlElement* coral = found->child("CORALTEX")) {
		if (const XmlElement* tex = coral->child("TEXTURE")) t.coral = resolve_texture(files, t.basedir, tex->get("file"));
	}
	if (const XmlElement* l = found->child("LIGHTING")) {
		read_light(l->child("AMBIENT"), t.ambient);
		read_light(l->child("DIFFUSE"), t.diffuse);
	}
	return t;
}

}  // namespace ron
