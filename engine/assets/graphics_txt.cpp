#include "assets/graphics_txt.hpp"

namespace ron {

namespace {

std::string trim(const std::string& s) {
	const size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return std::string();
	return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

}  // namespace

GraphicsTxt::GraphicsTxt(const std::string& text) {
	std::vector<std::string>* current = nullptr;
	size_t p = 0;
	while (p < text.size()) {
		size_t e = text.find('\n', p);
		if (e == std::string::npos) e = text.size();
		const std::string line = trim(text.substr(p, e - p));
		p = e + 1;
		if (line.empty() || line[0] == ';') continue;  // blank or comment
		if (line[0] == '#') {
			current = &sections_[to_lower(trim(line.substr(1)))];
		} else if (current) {
			current->push_back(line);
		}
	}
}

const std::vector<std::string>* GraphicsTxt::section(const std::string& name) const {
	auto it = sections_.find(to_lower(name));
	return it == sections_.end() ? nullptr : &it->second;
}

GraphicsTxt load_graphics_txt(const GameFiles& files) { return GraphicsTxt(files.read_text("graphics.txt")); }

std::vector<std::string> split_fields(const std::string& line) {
	std::vector<std::string> out;
	size_t p = 0;
	for (;;) {
		const size_t c = line.find(',', p);
		out.push_back(trim(line.substr(p, c == std::string::npos ? std::string::npos : c - p)));
		if (c == std::string::npos) break;
		p = c + 1;
	}
	return out;
}

}  // namespace ron
