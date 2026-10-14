// XML access and value parsing exactly as Rise of Nations' rules loader sees it (docs/rules-spec.md sections 4 and 5).
// The game reads its rules through MSXML wrapped by basic:xml.obj and parses values with basic:str.obj; this is a
// port of re/tools/rulesxml.mjs, which reproduces those semantics. It is separate from assets/xml.hpp on purpose:
// the rules loader needs case-sensitive tags and attributes, MSXML text (descendant text, trimmed at the ends),
// UTF-16 strings (names are hashed and walked as UTF-16) and a mutable tree (get_element(create=1) appends).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "assets/chunk.hpp"
#include "assets/files.hpp"

namespace ron::sim {

using ustr = std::u16string;

// ---------------------------------------------------------------- integer helpers (int32 wrap-around, C division)
inline int32_t wrap32(int64_t v) { return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v))); }
inline int32_t imul(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
inline int32_t iadd(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
inline int32_t ishl(int32_t a, int n) { return static_cast<int32_t>(static_cast<uint32_t>(a) << n); }
// C truncating division on int32 operands. INT_MIN / -1 wraps; a zero divisor gives 0 (what the JS reference yields;
// the original would fault, and no stock value reaches it).
inline int32_t cdiv(int32_t a, int32_t b) { return b == 0 ? 0 : wrap32(static_cast<int64_t>(a) / b); }
inline int8_t i8(int32_t v) { return static_cast<int8_t>(static_cast<uint8_t>(static_cast<uint32_t>(v))); }
inline int16_t i16(int32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(v))); }

// ---------------------------------------------------------------- strings
ustr from_ascii(const char* s);
std::string to_utf8(const ustr& s);
// WHATWG UTF-8 decoding (invalid sequences become U+FFFD), as Node's Buffer.toString('utf8') does.
ustr decode_utf8(const uint8_t* p, size_t n);
// A rules file as text: UTF-16LE when it starts with FF FE, else UTF-8.
ustr decode_text_file(const Bytes& raw);

// _wcsicmp equality and case mapping in the "C" locale: only A-Z / a-z fold.
bool iequals(const ustr& a, const ustr& b);
ustr to_upper(ustr s);
ustr to_lower(ustr s);
int wcsicmp(const ustr& a, const ustr& b);

// ---------------------------------------------------------------- minimal mutable DOM
struct XNode {
	bool is_text = false;
	ustr tag;   // elements
	ustr text;  // text and CDATA nodes
	std::vector<std::pair<ustr, ustr>> attrs;  // document order; a repeated name overwrites
	std::vector<std::unique_ptr<XNode>> children;
	XNode* parent = nullptr;

	const ustr* attr(const ustr& name) const;
};

struct XDoc {
	std::unique_ptr<XNode> doc;  // the #document node
	XNode* root = nullptr;        // its first element (null for an empty document)
};

XDoc parse_xml(const ustr& src);

// A data file from the install. A bare name is Data/<name>; a name with a directory part is relative to the install
// root (XML::init). A non-empty `lang_suffix` (".7", ...) prefers <file><suffix> when it exists.
// Returns an empty XDoc (root null) for a missing file when `required` is false, otherwise throws.
XDoc read_game_xml(const GameFiles& files, const std::string& name, const std::string& lang_suffix = std::string(),
                   bool required = true);

namespace xml {

// IXMLDOMNode::get_text with preserveWhiteSpace off: descendant text and CDATA, white space trimmed at the ends
// (unless an ancestor has xml:space="preserve").
ustr text(const XNode* el);
// XMLNode::get_element(name, create = 0, exact): first child element with the (upper-cased unless exact) tag.
XNode* child(const XNode* el, const ustr& name, bool exact = false);
// XMLNode::get_elements(name): element children with the tag, document order.
std::vector<XNode*> children(const XNode* el, const ustr& name, bool exact = false);
// get_element(name, create = 1): a missing child is created empty and appended, as the game does.
XNode* child_or_create(XNode* el, const ustr& name);
// XMLElement::get_text(name): text of the first child `name`, "" when absent.
ustr get_text(const XNode* el, const ustr& name);
// XMLElement::get_text_num(name, def): def when the text is empty, else String::convert_int.
int32_t get_text_num(const XNode* el, const ustr& name, int32_t def = -1);
ustr get_attrib(const XNode* el, const ustr& name, const ustr& def = ustr());
int32_t get_attrib_num(const XNode* el, const ustr& name, int32_t def = -1);

}  // namespace xml

// ---------------------------------------------------------------- value parsers (basic:str.obj, arith.obj)
// All take a start position so callers can parse from inside a string as the original does.
int32_t wtoi(const ustr& s, size_t p = 0);         // MSVC _wtoi: saturates at INT_MIN/INT_MAX
int32_t parse_binary(const ustr& s, size_t p = 0);  // String::binary, int32 wrap
int32_t parse_hex(const ustr& s, size_t p = 0);     // String::hex, int32 wrap
int32_t convert_int(const ustr& s, size_t p = 0);   // String::convert_int: 0b / 0x / 0d prefixes, else _wtoi
int32_t fraction(const ustr& s, int32_t mul);       // String::fraction: _wtoi(s) * mul / _wtoi(after '/')
int32_t degrees_to_angle(int32_t d);                 // 360 degrees = 2^32

// Cost-list token: "<int><letter>" (t timber 1, g wealth 2, k knowledge 3, m metal 4, o oil 5, else food 0).
struct CostToken {
	int32_t amount;
	int32_t res;
};
std::vector<CostToken> cost_tokens(const ustr& s);  // tokens with a non-zero amount, in order

// Unit OBJ_MASK: upper-cased, 'A'..'Z' -> bits 0..25, '1'..'9' -> bit (c - 0x17) & 31, anything else ignored.
int32_t obj_mask(const ustr& s);
// Lower-case letter flags (unit/craft FLAGS, building BUILD_FLAGS): every character sets a bit,
// '1'..'9' -> c - 0x17, anything else -> c - 'a', shift taken mod 32.
uint32_t lower_letter_flags(const ustr& s);
// Building OBJ_MASKS: the same rule over the upper-cased string with base 'A' (non-letters are not ignored).
uint32_t upper_letter_flags(const ustr& s);

}  // namespace ron::sim
