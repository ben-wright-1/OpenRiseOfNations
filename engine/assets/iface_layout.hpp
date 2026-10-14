// In-game interface layout (Data/interface.xml and the other GraphicWinXML files), docs/ui-spec.md section 2:
// the element tree, the rect resolver (XMLElement::get_attrib_rect_ex @00a25ba0), IFACETEX atlas pieces, 16-slot
// IFACEBORDERs, IFACETEXLOAD atlas ids, interface.txt (bitmap digits) and the per-civilisation skin swap
// (IFaceMainBase::age_update @00810b10). Portable; the reference is re/tools/ifacelayout.mjs, which
// `ron-ui-check tree` reproduces line for line.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "assets/files.hpp"
#include "assets/xml.hpp"

namespace ron {

// RectTemplate<short> {x1, x2, y1, y2}: pixels relative to the parent's client area.
struct IfaceRect {
	int l = 0, r = 0, t = 0, b = 0;
	int w() const { return r - l; }
	int h() const { return b - t; }
	bool contains(int x, int y) const { return x >= l && x < r && y >= t && y < b; }
	IfaceRect offset(int dx, int dy) const { return {l + dx, r + dx, t + dy, b + dy}; }
};

// An attribute value as the game reads numbers (atoi on the trimmed text: "92 ", "72,42FS" and " 6" all parse).
int iface_int(const std::string& v);
// The trimmed value of attribute `key` (lowercase), or "" when absent.
std::string iface_attr(const XmlElement& e, const std::string& key);

// get_attrib_rect_ex: resolves an element's rect attributes against a parent of pw x ph pixels (section 2.3).
IfaceRect resolve_iface_rect(const XmlElement& e, int pw, int ph);
// True if the element carries any rect attribute (the reference tool's test for "draw as a rect").
bool has_iface_rect(const XmlElement& e);

// IFACETEX: an atlas piece. texture = the IFACETEXLOAD id (e.g. TEX_CIV); rect in texels.
struct IfacePiece {
	std::string name, texture;
	int left = 0, top = 0, width = 0, height = 0;
	bool valid() const { return !texture.empty(); }
};
IfacePiece iface_piece(const XmlElement& e);

// IFACEBORDER slots: 0..3 corners (TL, TR, BR, BL), 4..7 edges (top, right, bottom, left), 8..15 extra0..7.
struct IfaceBorder {
	std::string name;
	IfacePiece slot[16];
};
IfaceBorder iface_border(const XmlElement& e);

// IFACECOLOR (alpha defaults to 255), as 0xAARRGGBB.
uint32_t iface_color(const XmlElement& e);

// Case-insensitive element-name compare (MSXML lookups go through upper-case paths).
bool iface_tag_is(const XmlElement& e, const char* tag);

class IfaceLayout {
public:
	explicit IfaceLayout(const std::string& xml_text);
	IfaceLayout(const IfaceLayout&) = delete;
	IfaceLayout& operator=(const IfaceLayout&) = delete;

	// The document's first top-level element (ROOT).
	const XmlElement& root() const { return *root_; }
	// Depth-first: the first element whose tag (case-insensitive) or name attribute equals `name`.
	const XmlElement* find(const std::string& name) const;
	// IFACETEXLOAD ids -> files ('/' separated, trimmed), in first-seen order (a repeated id keeps its slot, last file wins).
	const std::vector<std::pair<std::string, std::string>>& texloads() const { return texloads_; }
	std::string texload_file(const std::string& id) const;
	// The element tree as `node re/tools/ifacelayout.mjs tree` prints it (without the "# file at WxH" header).
	std::string dump_tree(const XmlElement& e, int w, int h, bool all, int max_depth = 99) const;

private:
	XmlElement doc_;
	const XmlElement* root_ = nullptr;
	std::vector<std::pair<std::string, std::string>> texloads_;
};

// Children of a window element, in file order.
const XmlElement* iface_child(const XmlElement& win, const char* tag, const std::string& name, int nth = 0);
std::vector<const XmlElement*> iface_children(const XmlElement& win, const char* tag, const std::string& name);
// The child windows / rects of `win` (client size pw x ph) resolved, with POSITIONING groups flattened (each element
// of a group shifted by x + i * inc_x, y + i * inc_y; GraphicWinXML::add_elements). Pieces are skipped.
struct IfaceLaidOut {
	const XmlElement* el;
	IfaceRect rect;  // relative to the window's client area
};
std::vector<IfaceLaidOut> iface_layout_children(const XmlElement& win, int pw, int ph);
// The rect of the n-th child (window, BUTTON, IFACERECT, ...) with this tag and name, flattened as above.
bool iface_find_rect(const XmlElement& win, const char* tag, const std::string& name, int pw, int ph, IfaceRect& out, int nth = 0);

// The n-th IFACETEX / IFACERECT named `name` directly under `win` (an invalid piece / null if absent).
IfacePiece iface_find_piece(const XmlElement& win, const std::string& name, int nth = 0);
// The IFACEBORDER named `name` directly under `win` (empty slots if absent).
IfaceBorder iface_find_border(const XmlElement& win, const std::string& name);

// ---------------------------------------------------------------- interface.txt (section 2.7)
// Sections "#NAME"; ';' comments; "KEY, item, item": numeric items may carry a trailing word ("464 xleft").
class InterfaceTxt {
public:
	explicit InterfaceTxt(const std::string& text);
	// Pieces of IFaceRenderManager::setup_text: NUMBERS_TEX[set][0..9], then SLASH, PLUS, MINUS (char_to_index order).
	IfacePiece digit[2][13];
	// "KEY, TEX_ID, x, y, w, h" lines of a section by key (first match), invalid if absent.
	IfacePiece piece(const std::string& section, const std::string& key) const;

private:
	struct Line {
		std::string section, key;
		std::vector<std::string> items;
	};
	std::vector<Line> lines_;
};
InterfaceTxt load_interface_txt(const GameFiles& files);

// IFaceRenderManager::char_to_index: '0'..'9' -> 0..9, '/' 10, '+' 11, '-' 12, else -1.
int iface_digit_index(char c);

// ---------------------------------------------------------------- per-civilisation skins (section 1.1)
// The skin code for the console player's age (0 Ancient .. 7 Information) and the tribe's build_continent.
std::string iface_skin_code(int age, int build_continent);
// A TEX_CIV / TEX_LIBRARY_BACK file with the skin code swapped in: art/iface_AS0_assets.tga -> art/iface_MD3_assets.tga.
std::string iface_skin_file(const std::string& file, const std::string& code);

}  // namespace ron
