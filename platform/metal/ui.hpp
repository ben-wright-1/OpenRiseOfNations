// The in-game interface draw layer (docs/ui-spec.md sections 2.5, 3.2, 5): IFaceRenderManager's quad API (add,
// add_smooth, tile_*, draw_border, expanders, bitmap-digit write), the 9 IFaceRenderType states, text from the install's
// TrueType fonts rasterised with CoreText, and one Metal pass drawn after the post-tonemap overlays.
//
// Coordinates are UI pixels with a top-left origin: the original's screen pixels (no UI scaling, section 2.4). The pass
// renders them at `scale` drawable pixels per UI pixel (the window's backing scale), so Retina output is crisp: atlas
// pieces drawn at their own size are point-sampled (exact pixel doubling), stretched ones bilinear, and glyphs are
// rasterised at the scaled size while the layout (advances, cell heights) keeps the 1x metrics of fonts.xml.
//
// Order: every quad carries the window's update_depth; the pass sorts quads by depth (stable, glyphs after quads of the
// same depth, as FontFamily::flush_fonts runs after IFaceRenderManager::flush_all) and draws them back to front. That
// replaces the original's LESSEQUAL depth test (section 5.3) without a depth attachment.
#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/fonts.hpp"
#include "assets/iface_layout.hpp"

namespace ron::metal {

// IFaceRenderType (rise.pdb), plus the text state of D3D11GLFont and an internal multiply state (minimap fog).
enum class UiType : uint8_t {
	Normal = 0,          // opaque, colour = texture x diffuse
	Alpha = 1,           // blend, alpha test > 0, modulate
	ColorAlpha = 2,      // blend SRCALPHASAT / INVSRCALPHA
	Highlight = 3,       // opaque, MODULATE2X
	AlphaHighlight = 4,  // blend, MODULATE2X, alpha = texture
	ColorBlend = 5,      // blend, colour and alpha = diffuse
	Alpha2 = 6,          // = ALPHA (inline #ICONs)
	ColorAlphaMod = 7,   // blend, modulate, no alpha test
	AddFB = 8,           // SRCALPHA / ONE, colour = texture
	Text = 9,            // blend, alpha test >= 8, modulate
	Multiply = 10,       // ZERO / SRCCOLOR (minimap fog)
};

struct UiTexture {
	id<MTLTexture> tex;
	int w = 0, h = 0;
};

// One fonts.xml entry: 1x layout metrics from the TTF tables, glyphs rasterised with CoreText at size x scale.
class UiFont {
public:
	bool init(const GameFiles& files, const FontEntry& entry, float scale, id<MTLDevice> device);
	const FontMetrics& metrics() const { return metrics_; }
	int advance(uint32_t cp) const;  // hinted pixels, plus 1 for outline fonts
	struct Glyph {
		int page = -1;
		int x = 0, y = 0, w = 0, h = 0;  // atlas texels (scaled)
		int top_px = 0;                  // scaled pixels from the cell top to the bitmap top (Ascent - bitmap_top)
	};
	const Glyph& glyph(uint32_t cp);
	const std::deque<UiTexture>& pages() const { return pages_; }
	float scale() const { return scale_; }

private:
	TtfFace face_;
	FontEntry entry_;
	FontMetrics metrics_;
	float scale_ = 1;
	void* ct_font_ = nullptr;  // CTFontRef (retained)
	id<MTLDevice> device_;
	std::deque<UiTexture> pages_;  // deque: quads keep pointers to pages while new ones are added
	std::map<uint32_t, Glyph> glyphs_;
	int pen_x_ = 4, pen_y_ = 4, row_h_ = 0;
	int page_size_ = 256;
};

class UiRenderer {
public:
	bool init(id<MTLDevice> device, id<MTLCommandQueue> queue, MTLPixelFormat color, const GameFiles* files,
	          const IfaceLayout* layout, std::string& error);

	// Per-civilisation skin (section 1.1): TEX_CIV and TEX_LIBRARY_BACK(2) load the file with this code.
	void set_skin(const std::string& code);
	const std::string& skin() const { return skin_; }
	// The atlas of an IFACETEXLOAD id (null for TEX_NONE / unknown / missing files).
	const UiTexture* texture(const std::string& id);
	// Registers a texture under a private id (minimap background, fog), replacing any previous one.
	void set_texture(const std::string& key, id<MTLTexture> tex, int w, int h);
	// A TGA from the install by game-relative path, registered under the id "file:<path>" (returned in `id`), for
	// add_quad. Null when missing.
	const UiTexture* file_texture(const std::string& game_path, std::string* id = nullptr);

	// Starts a frame of ui_w x ui_h UI pixels drawn at `scale` drawable pixels each. Fonts follow fonts.xml's
	// resolution choice for ui_w.
	void begin(int ui_w, int ui_h, float scale);
	int width() const { return w_; }
	int height() const { return h_; }
	// update_depth: +1 per window, +2 per drawing layer (section 5.3); quads take the current value.
	int depth = 0;
	void next_window() { depth += 1; }
	void next_layer() { depth += 2; }

	// add_smooth: the piece stretched into (x, y, w, h); w or h = 0 means the piece's own size.
	void add(const IfacePiece& p, float x, float y, float w = 0, float h = 0, uint32_t argb = 0xFFFFFFFF, UiType t = UiType::Alpha);
	// A sub-rectangle of a piece (texels relative to the piece), e.g. a partly filled progress bar.
	void add_sub(const IfacePiece& p, float sx, float sy, float sw, float sh, float x, float y, float w, float h,
	             uint32_t argb = 0xFFFFFFFF, UiType t = UiType::Alpha);
	// Arbitrary textured quad (corners TL, TR, BR, BL) with texel UVs on `tex_id` ("" = untextured).
	void add_quad(const std::string& tex_id, const float xy[8], const float uv[8], uint32_t argb, UiType t, bool point = false);
	// Untextured rectangle (packet 0, TEX_NONE).
	void fill(float x, float y, float w, float h, uint32_t argb, UiType t = UiType::ColorBlend);
	// 1-pixel frame.
	void frame(float x, float y, float w, float h, uint32_t argb);
	// tile_horiz / tile_vert / tile_random: repeat the piece over the rect, clipping the last copy's UVs.
	void tile(const IfacePiece& p, const IfaceRect& r, uint32_t argb = 0xFFFFFFFF, UiType t = UiType::Alpha);
	// draw_border @0051d260.
	void border(const IfaceBorder& b, const IfaceRect& r, uint32_t argb = 0xFFFFFFFF, UiType t = UiType::Alpha);
	// draw_expander: left and right at native width, the middle stretched (horizontal 3-slice); vertical variant.
	void expander(const IfacePiece& l, const IfacePiece& m, const IfacePiece& r, const IfaceRect& rc, uint32_t argb = 0xFFFFFFFF,
	              UiType t = UiType::Alpha);
	void vexpander(const IfacePiece& top, const IfacePiece& mid, const IfacePiece& bot, const IfaceRect& rc,
	               uint32_t argb = 0xFFFFFFFF, UiType t = UiType::Alpha);

	// IFaceRenderManager::write: bitmap digits from interface.txt (set 0 or 1), top-left at (x, y). Returns the width.
	int write_digits(const std::string& s, float x, float y, uint32_t argb, int set);
	int digits_width(const std::string& s, int set) const;
	int digits_height(int set) const;

	// Fonts: FontFamily::get(size, style) / by LOOKUP tag. Returns a handle (or -1).
	int font(int size, int style = -1);
	int font_tag(const std::string& tag);
	const FontMetrics* font_metrics(int f) const;
	int text_width(int f, const std::string& utf8);
	// Writes a line with its cell top at y (the original passes the bottom: top = y - FontHeight). A shadow colour with
	// alpha != 0 draws the string first at (+sdx, +sdy), as Buffer does.
	void text(int f, const std::string& utf8, float x, float y, uint32_t argb, uint32_t shadow = 0, int sdx = 1, int sdy = 1);
	// The same line scaled by `s` about (x, y) (TextBubble's Font::set_scale).
	void text_scaled(int f, const std::string& utf8, float x, float y, float s, uint32_t argb, uint32_t shadow = 0);
	// Centred in a rect (horizontally; vertically on the FontHeight cell).
	void text_centered(int f, const std::string& utf8, const IfaceRect& r, uint32_t argb, uint32_t shadow = 0);

	// Draws everything queued since begin() into `enc` (a pass over the drawable, no depth) and clears the queue.
	void encode(id<MTLRenderCommandEncoder> enc);

	const InterfaceTxt& interface_txt() const { return *itxt_; }
	const FontsXml& fonts_xml() const { return fonts_xml_; }

private:
	struct Vert {
		float x, y, u, v;
		uint32_t argb;
	};
	struct Quad {
		int depth;
		uint32_t seq;
		bool text;
		const UiTexture* tex;
		UiType type;
		bool point;
		Vert v[4];
	};
	void push(const UiTexture* tex, const float xy[8], const float uv_texels[8], uint32_t argb, UiType t, bool point, bool text);
	void push_rect(const UiTexture* tex, float sx, float sy, float sw, float sh, float x, float y, float w, float h, uint32_t argb,
	               UiType t, bool text = false);
	id<MTLRenderPipelineState> pipeline(UiType t);

	id<MTLDevice> device_;
	id<MTLCommandQueue> queue_;
	const GameFiles* files_ = nullptr;
	const IfaceLayout* layout_ = nullptr;
	std::unique_ptr<InterfaceTxt> itxt_;
	std::string skin_ = "AS0";
	std::map<std::string, UiTexture> textures_;        // by IFACETEXLOAD id (+ skin) / private id
	std::map<std::string, UiTexture> file_textures_;   // by lowercase file
	UiTexture white_;
	id<MTLRenderPipelineState> pipes_[5];  // none, alpha, alphasat, add, multiply
	id<MTLSamplerState> linear_, nearest_;
	FontsXml fonts_xml_;
	int fonts_res_width_ = -1;
	float fonts_scale_ = 0;
	std::vector<std::unique_ptr<UiFont>> fonts_;  // parallel to fonts_xml_.fonts (lazily created)
	std::vector<Quad> quads_;
	uint32_t seq_ = 0;
	int w_ = 1024, h_ = 768;
	float scale_ = 1;
};

// 0xAARRGGBB helpers.
constexpr uint32_t argb(int a, int r, int g, int b) {
	return uint32_t(a & 255) << 24 | uint32_t(r & 255) << 16 | uint32_t(g & 255) << 8 | uint32_t(b & 255);
}
constexpr uint32_t rgb(int r, int g, int b) { return argb(255, r, g, b); }

}  // namespace ron::metal
