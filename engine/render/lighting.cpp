#include "render/lighting.hpp"

#include "assets/xml.hpp"

namespace ron {

SceneLighting load_scene_lighting(const GameFiles& files) {
	SceneLighting l;
	const XmlElement root = parse_xml(files.read_text("Data/unit_graphics.xml"));
	const XmlElement* units = nullptr;
	for_each_element(root, "UNITS", [&](const XmlElement& e) {
		if (!units) units = &e;
	});
	if (!units) return l;
	const float ai = units->get_float("ambienti", 0.558f), di = units->get_float("diffusei", 2.5f);
	const char* a[3] = {"ambientr", "ambientg", "ambientb"};
	const char* d[3] = {"diffuser", "diffuseg", "diffuseb"};
	for (int i = 0; i < 3; i++) {
		l.ambient_light[i] = units->get_float(a[i], l.ambient_light[i]);
		l.unit_ambient[i] = l.ambient_light[i] * ai;
		l.unit_diffuse[i] = units->get_float(d[i], 1.0f) * di;
	}
	return l;
}

}  // namespace ron
