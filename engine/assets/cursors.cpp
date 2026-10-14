#include "assets/cursors.hpp"

#include "assets/xml.hpp"

namespace ron {

std::vector<CursorRow> load_cursors(const GameFiles& files, const std::string& xml) {
	std::vector<CursorRow> rows;
	const XmlElement doc = parse_xml(files.read_text(xml));
	for_each_element(doc, "CURSOR", [&](const XmlElement& e) {
		CursorRow r;
		r.name = e.get("name");
		r.file = normalize_game_path(e.get("file"));
		r.xs = e.get_int("xs");
		r.ys = e.get_int("ys");
		auto cell = [](int v, int s) { return v < 0 ? 1 - (s + 1) * v : v; };
		r.x = cell(e.get_int("x"), r.xs);
		r.y = cell(e.get_int("y"), r.ys);
		r.hot_x = e.get_int("hot_x");
		r.hot_y = e.get_int("hot_y");
		r.gdi_id = e.get_int("gdi_id");
		r.bw_id = e.get_int("bw_id");
		rows.push_back(r);
	});
	return rows;
}

}  // namespace ron
