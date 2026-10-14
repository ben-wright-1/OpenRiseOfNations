# In-game interface (HUD, text, cursors, input): what EE builds and how to rebuild it

Status: ✅ layout format, rect resolution, UI render states/batching/draw order, fonts and text rendering, string tables
and language selection read from `riseofnations.exe` and `d3dgl.dll` and proven by tools · 🟡 per-panel drawing is
summarised from the decompiles (positions are data, contents are code) · open questions in §10.

Companion docs: [lighting-spec.md](lighting-spec.md) (frame passes; the UI is step 11), [renderstates.md](renderstates.md)
(render-state bits → shader), [graphsys-interface.md](graphsys-interface.md) (backend slots: fonts, cursors, input pump),
[fog-spec.md](fog-spec.md) §9.4 (minimap fog), [orders-spec.md](orders-spec.md) / [production-spec.md](production-spec.md)
(what the buttons issue). Everything here paraphrases code; no game text or decompiled code is reproduced.

Tools (all read the user's install, `~/Games/RiseOfNations`, never write to it):

| Tool | Proves |
|---|---|
| `node re/tools/ifacelayout.mjs tree NODE [--w W --h H] [--all]` | the layout grammar and the rect resolver (§2.3) for any screen size |
| `node re/tools/ifacelayout.mjs render NODE [--skin MD3]` / `sheet NODE` | atlas pieces, borders, the generic drawing rules (§2.6) → `re/previews/` |
| `node re/tools/ifacelayout.mjs texloads` | every `IFACETEXLOAD` id → file |
| `node re/tools/fontpreview.mjs list [--w 1280]` / `render "text" ...` / `sample` | fonts.xml resolution choice, FreeType metrics, glyph layout (§3) → PNG |
| `node re/tools/locstrings.mjs hash/int/loc/lookup/cats/check` | the String hash, the three string tables, language suffixes (§3.5) |
| `node re/tools/cursors.mjs` | cursors.xml and the `.cur` files (§7) |

---

## 1. Files that define the interface

| File | Read by | Contents |
|---|---|---|
| `Data/interface.xml` (487 KB) | `IFaceRenderManager::init` @0051e6c0, `GraphicWinXML::init` @00516c80 per window, `IFaceButton::init_data` @007fbbe0 | **Every in-game window**: the HUD (`IFACEMAINBASE`, `IFACERESOURCES`, `IFACESELECTED`, `IFACEOPTIONS`, `IFACEOPTIONSBUILDINGS`, `IFACEMINIMAP`, `IFACELIBRARY`, `IFACEMINILIBRARY`, `IFACEGARRISON`, `IFACEAIRGARRISON`, `IFACEBUTTONCLASS`, `MESSAGEWIN`, `POINTERWIN`, `IFACETIPWIN`, `STATWIN`, `DIPLOWIN`, `ENDGAMEWIN`, `REPLAYWIN`, `POPUP` styles, ...), the scenario editor, lobby/chat windows, and under `IFACERENDERMANAGER` the global texture atlas list (`IFACETEXLOAD`, 170 entries) and shared borders |
| `interface.txt` (install root) | `IFaceRenderManager::setup_text` @0051dd50 via `Text::open("interface", section)` | Legacy BHG text format (§2.7): the bitmap digit fonts `NUMBERS_TEX[0..1][0..9]`, `SLASH/PLUS/MINUS_TEX`, the "heads" (citizen/scholar portraits), bars/tees/crosses, `#IFACEGARRISON` grid, `#IFACEMARKET` arrows |
| `Data/topmenu.xml`, `setupwin.xml`, `optionswin.xml`, `workshopwin.xml`, `aboutbox.xml` | the front-end windows (same GraphicWinXML grammar) | menus, skirmish setup, options (incl. hotkeys page `OptionsWinHotkeys`) |
| `Data/fonts.xml[.lang]` | `FontFamily::init` @00521330 | TrueType font list per resolution and the tag → size table (§3.1) |
| `Data/Fonts/*.ttf` | `d3dgl.dll` FreeType (`FT::FontFace`) | Arial, Courier New, Georgia (regular/bold/italic/bold-italic) |
| `Data/icon_texs.xml` | `Buffer::init_icon_texs` @005112d0 | 17×17 inline icons used as `#ICONn` in text (§3.4) |
| `Data/cursors.xml` (`cursors_uwp.xml` for the Store build) + `art/SC_*.tga`, `art/cursors.tga` | `Cursors::init` @007d0270 | the cursor set (§7); the `.cur` files in the install root are not read at run time |
| `Data/internal_strings.xml`, `translated_strings.xml[.lang]`, `string_lookup.xml[.lang]`, `typenames.xml[.lang]`, `help.xml[.lang]`, ... | `Main::init_string_tables` @00595a10, `StringTable::init` @00a28520, `String::lookup` @00a1ddb0 | all text (§3.5) |
| `Data/effects_graphics.xml` `EFFECTS/MINIMAP` | `WorldMap::init` @008d2160, `WorldMap::init_texture_pings` @008c9960 | minimap blip atlas (`blipmap texture=`, `size`/`xoffset`/`yoffset` per blip size 1..8) and ping effects (attack, wonder, citylost, citywon, waypoint, resource, default) |
| `art/iface_*.tga`, `art/Iface_*_assets.tga`, `art/wrapper_*.tga`, `art/resource_icon.tga`, `art/iface_timers.tga`, `art/iface_alerts.tga` | via `IFACETEXLOAD` ids | the atlases (32-bit TGA) |
| unit/building/tech rules + `unit_graphics.xml` | `GRID_X/GRID_Y` (button cell on the command card), `TEX_ID/TEX_COL/TEX_ROW` (icon page and cell; units carry them per graphic in `unit_graphics.xml` as `tex_id/tex_col/tex_row`) | command-card buttons (§6.4) |

### 1.1 Per-civilisation skins

`TEX_CIV` (`art/iface_AS0_assets.tga` in the XML) and `TEX_LIBRARY_BACK/BACK2` are **replaced at run time** by
`IFaceMainBase::age_update` @00810b10 (called at game start and on every age-up): file = `art\iface_` + code +
`_assets.tga` (library: `_library_full.tga` when the game starts in age 0 and ends in age 7, else
`_library_generic.tga`). The code depends on the console player's age and the tribe's `build_continent` (Tribe +0x64,
rules-spec §10):

| Player age | build_continent → code |
|---|---|
| 0–2 (Ancient–Medieval) | 0,1 → `MD0`; 2,3 → `AS0`; 4,5,7,8 → `SA0`; 6 → `IRO` |
| 3–4 (Gunpowder, Enlightenment) | 0 `MD3`, 1 `NE3`, 2 `AS3`, 3 `KO3`, 4 `SA3`, 5 `TR3`, 6 `IR3`, 7 `USA`, 8 `EIN` |
| 5 (Industrial) | `IND` |
| ≥ 6 (Modern, Information) | `MOD` |

The player's age is read de-obfuscated (`ages ^ 0x62766`, the same XOR the simulation uses). All per-skin atlases share
one layout, so every `IFACETEX` rect works for every skin (`ifacelayout.mjs render NODE --skin MD3`).

---

## 2. Layout format (GraphicWinXML)

### 2.1 Parsing rules

- XML through MSXML (`basic:xml.obj`); element lookups use paths such as `root/IFaceButtonClass` and
  `ROOT/IFACEMINIMAP` against upper-case tags, so **element and attribute names match case-insensitively**. Attribute
  values are trimmed (the file is full of padded values like `id="TEX_UNITS_0     "`). Numbers parse like `atoi`
  (`"92 "`, `" 6"` are fine).
- A window class initialises from **its own element**: `XML::init(file, "IFACERESOURCES")` then
  `GraphicWin::init` → `load_style` @00519f20 → `process_element` for each child (GraphicWinXML::process_element
  @005145a0, GraphicWin::process_element @0051a9e0, IFaceWindow::process_element @00828ca0,
  IFaceButton::process_element @007fc270). Unknown tags are left to the subclass (e.g. `IFaceSelected` reads its own
  rect names).
- Window titles are localised: `title="Close"` is passed through `String::lookup(title, "WINTITLE")` (§3.5).

### 2.2 Element vocabulary

| Element | Meaning (handler) |
|---|---|
| `<CLASSNAME name= ... rect attrs show="0">` | a window of that class (the class decides what the tag is); `show="0"` starts hidden |
| `STYLE` | window style flags: `CHILD`, `TOP`, `BOTTOM`, `MOUSE_THRU` (clicks fall through), `PASS_CONTROLS`, `NO_SET_RENDERTARGET` (draw straight to the screen; every in-game panel), `CLIENTDRAG`, `CLICK_FOCUS`, `NO_TAB_FOCUS`, `NO_BRING_TO_TOP`, `DIALOG`, `CAPTION`/`NOCAPTION`, `HIDDEN`, `KEEPPROPORTIONS`, `SEETHRU`, `SEETHRU_SELECT`, `RESIZE`, `SIZECLIENT`, `CLIENTONLY`, `NCPARENT`, `CLEARCHILD`, `NOFOCUS`, `NOKEYINPUT` (`GraphicWin::load_style`) |
| `WINDOW`, `BUTTON`, `SLIDER`, `COMBO`/`COMBOBOX`, `EDIT`/`EDITBOX`, `LIST`/`LISTBOX`, `LISTBOX2`/`LIST2`, `CHECK`/`CHECKBOX`, `STRINGBOX`, `SCROLL`, `DROPBUTTON`/`DROP` | child controls (GraphicWinXML::process_element creates a `GraphicWinXML`, `Button`, `Slider`, ... and recurses). Attributes: rect, `name`, `title`, `ref_rect="other"` (copy a named IFACERECT's rect, `Window::get_common_attribs` @00a55d40), control-specific (`min`, `max`, `vertical`, `text_limit`, ...) |
| `SPOT` | invisible hot spot: rect + `name`, `type`, `id` (`Window::add_spot`) |
| `IFACERECT name=` | a named rect (resolved like a window rect against the window size) |
| `IFACEPOINT name=` | a point: `left`/`left_inverse` (= w − v)/`center_off_x` (= w/2, value ignored), same for `top`; plus `x`, `y`, `x_percent`, `y_percent` |
| `IFACETEX name= texture=TEX_ID left top width height` | an atlas piece (`IFaceTex::get` @0051fed0: texture id → packet index, rect in texels). `ref="name"` copies another piece |
| `IFACETEXLOAD id= file= pixformat=PF_32 sort=` | register an atlas texture (`IFaceRenderManager::load_tex` @00520500); valid globally once loaded |
| `IFACEBORDER name=` with `IFACETEX corner0..3, edge0..3, extra0..7` | a 16-slot border (`IFaceBorder::get` @0051ff80): corners TL, TR, BR, BL; edges top, right, bottom, left; extras drawn at native size |
| `IFACECOLOR name= red green blue alpha` | a colour (alpha defaults to 255) |
| `IFACESPRITE name= x y`, `STRETCHSPRITE name=` | non-atlas images (sprites) placed or stretched |
| `FONT id/name/size/style` and `FONTCOLOR r g b a` | the window's or control's font (resolved through `FontFamily::get`, §3.1) and text colour |
| `POSITIONING inc_x inc_x_percent inc_y inc_y_percent x x_percent y y_percent center_off_x center_off_y variable_inc` | layout of repeated children (`GraphicWinXML::add_elements` @00516700 / `resize_elements` @00516270): each successive child is offset by the increment |
| `IFACEBUTTONDATA name=IFACEBUTTON_*` | per button kind: `tex` (61×39 icon cell 0), `small_tex` (30×19), `overlay_tex0/1` (`IFaceButton::init_data`: exactly 4 IFACETEX + 10 IFACEBUTTONDATA required) |

### 2.3 Rect resolution (`XMLElement::get_attrib_rect_ex` @00a25ba0)

A rect is `RectTemplate<short>` stored as **(x1, x2, y1, y2)** in pixels, relative to the parent window's client area
(top-level windows: the screen, `GraphSys::get_screen_width/height`). `P` = parent width (x) or height (y). All
percentages are **per mille** (`v·P/1000`, C truncation). Per axis (x shown; y is the same with
top/bottom/height/`y_percent_diff`/`center_off_y`):

1. `w = width`; if 0, `w = (P − x_percent_diff)·percent_width/1000`.
2. **w > 0** (or positive percent): left edge from the first attribute present of `left` (v), `left_inverse` (P − v),
   `left_inverse_percent` (P − v·P/1000), `left_percent` (v·P/1000), `center_off_x` (v + P/2 − w/2, each halved
   separately; for y: v + (P − h)/2), else 0. Right = left + w.
3. **w < 0**: the right edge from `right`, `right_inverse`, `right_inverse_percent`, `right_percent`, `center_off_x`
   (same formulas), else 0; left = right + w (|w| to the left). For y with `center_off_y` the original computes
   bottom = v − (P − h)/2 (reproduced; no shipped element relies on it).
4. **w = 0** (no size): left from `left`/`left_inverse`/`left_inverse_percent`/`left_percent` (default 0), right from
   `right`/`right_inverse`/`right_inverse_percent`/`right_percent` (default 0). `center_off_*` is not consulted.
5. Missing attributes are detected with a sentinel (−0x790b); the result is truncated to 16 bits.

Examples (1024×768, `ifacelayout.mjs tree`): `IFACEMAINBASE left=0 top_inverse=198 percent_width=1000 height=198` →
(0,570)–(1024,768); its `BUTTON stats right_inverse=4 top=10 width=-25 height=19` → (995,10)–(1020,29) inside it.

### 2.4 Resolution and aspect

- **There is no UI scaling.** Every rect is in screen pixels; the layout adapts only through `_inverse`/`percent`
  anchoring. The HUD bar is always 198 px tall and spans the width (`percent_width=1000`); its panels have fixed pixel
  sizes; the right panel stretches (`right_inverse=14`). Higher resolutions show more map, not bigger UI.
- **Fonts** are the only resolution-dependent asset: `fonts.xml` lists `RESOLUTION res=1280` and `res=1024`; the game
  picks the one whose `res` is closest to the screen width (§3.1).
- No aspect-specific variants exist in the data. The design target is 1024×768 (front-end backgrounds are 1024×768;
  `skybox_intro_1440/1920.wmv` are intro movies only).
- Port: keep pixel-exact layout in a "UI points" space. On a Retina Mac, either render the UI at 1 point = 1 pixel of a
  1024..1920-wide logical screen and scale the final image, or (better looking) render at native pixels with an integer
  UI scale applied to all rects and font pixel sizes. The latter changes font metrics; the former is faithful.

### 2.5 The UI draw API (IFaceRenderManager, `bighuge:ifacerendermanager.obj`)

- **Atlases ("packets")**: `iface_render_manager.packets` is a NamedArray of textures, one per `IFACETEXLOAD` id
  (packet 0 = `TEX_NONE`, untextured). Each packet holds, for each of the **9 UI render states × 2 modes**
  (`dyna_mode` 0 static / 1 dynamic), a growing vertex/UV/colour/index list.
- `add(tex, rect, colour, type)` / **`add_smooth(tex, w, h, colour, state, ...)`** @0051b700 (position in registers):
  appends a screen-space quad: corners `(x − 0.5, y − 0.5)` to `(x + w − 0.5, y + h − 0.5)` (D3D9 half-pixel
  convention; **drop the −0.5 on Metal**), `z = (0x1fff − update_depth)/8192`, `w = 1`, per-vertex colour (ARGB
  modulate), UVs = the piece's texel rect over the texture size. `w`/`h` = 0 means the piece's own size (stretching
  otherwise). A static-mode cache (`render_indexing`) re-uses last frame's quad when nothing changed.
- `tile_horiz` / `tile_vert` / `tile_random` @0051f060..0051f2e0: repeat a piece across a span (last copy clipped by
  shrinking its UV rect); `tile_random` tiles a background in both directions.
- `draw_border(rect, corners, edges, extras, colour)` @0051d260: corners at the four corners; top edge tiled from
  corner0's right to corner1's left (inset by extra0/extra1 widths when present), bottom edge between corner3 and
  corner2 (extra5/extra4), left edge tiled vertically between corner0 and corner3 (extra7/extra6), right edge between
  corner1 and corner2 (extra2/extra3); extras last at native size.
- `draw_expander` / `draw_vert_expander` / `draw_2d_expander` (3- and 9-slice stretch), `draw_horiz_divider` /
  `draw_vert_divider` (thick/thin bars with tees and crosses from `interface.txt`), `draw_scroll`, `draw_heads`.
- `write(string, x, y, colour, set)` @0051f660: **bitmap digits** from `interface.txt` (`NUMBERS_TEX[set][0..9]`,
  `SLASH`, `PLUS`, `MINUS`; `char_to_index` accepts only those 13 characters), advancing by each glyph's width;
  `write_width` measures. Used for resource amounts, rates, caps, timers' small numbers.
- `IFaceRenderManager::flush_all` @0051fbe0 (§5.3) submits everything and clears.

### 2.6 Generic window drawing (GraphicWinXML)

`GraphicWinXML::draw_basics(section)` @00518cf0 (default section `misc`) draws, each step bumping
`graphsys->update_depth` by 2 so later layers sit in front:

1. background (`draw_background` @00518e20): piece `<section>_back` (or `back`) tiled over rect `<section>_back` (or the
   whole window); if there is no piece, an `IFACECOLOR` of that name fills the rect;
2. the window border: `IFACEBORDER main` around the client rect (`draw_main_border` @00518fe0);
3. `IFACEBORDER <section>` drawn into every `IFACERECT <section>_border` (`draw_borders` @00519070);
4. textures: the n-th `IFACETEX <section>` stretched into the n-th `IFACERECT <section>` (`draw_textures` @00519260);
5. tiled textures: `<section>_tile` pieces tiled into same-named rects (`draw_tiled_textures` @005193e0);
6. points, sprites, expanders (`draw_points`, `draw_sprites`, `draw_expanders`).

Panels call this, then draw their own content (numbers, icons, text). `ifacelayout.mjs render` reproduces steps 1–5.

### 2.7 `interface.txt` (BHG "Text" format, `basic:text.obj`)

Sections start with `#NAME`; `;` starts a comment; each line is `KEY, item, item, ...`. Items are comma-separated and a
numeric item may carry a trailing word that is ignored (`464 xleft, 137 ytop, 8 xwidth`). `Text::open(file, section)`,
`Text::get(i)`, `Text::item(n)`, `String::convert_int`. `setup_text` reads, in order, 2 × 13 digit pieces, the heads
(`NUM_HEAD_TYPES`, then per type `NUM_HEADS_FOR_TYPE` and the first piece; the rest follow to the right), logo and
background pieces, checks/radios, tees, bars and vertical button pieces, collapse arrows.

---

## 3. Fonts and text

### 3.1 fonts.xml and font selection (`FontFamily`, `bighuge:fontfamily.obj`)

- `FontFamily::init` @00521330 loads `fonts.xml` (language variant per §3.5). Under `FONTS`, the `CODEPAGE name="default"`
  is used (`use_ea_line_breaking` switches `Font::find_line_break_l` to `find_ea_line_breaking_l` for CJK). From its
  `RESOLUTION` children the one with the **smallest |res − screen_width|** wins (ties keep the first; screen width
  1024 if no device yet): 1280 for widths ≥ 1152, else 1024.
- Each `FONT filename name size [tex_size] style` is registered (`FontFamily::add` @00520e40). Style letters
  (`string_to_style` @00521260, any case): **B = 1, I = 2, U = 4, S = 8, O = 16, A = 32**; `?` = −1 (match any style).
  Bold/italic come from separate TTF files; the bits pick the right entry and drive U/S/O/A rendering.
- `LOOKUPS/LOOKUP tag` lists per resolution a size and optional style (e.g. `message_size` 1280:14 B, 1024:12 B).
  Game code asks for fonts by **size + style** (`FontFamily::get(&fonts, 0xe, -1)` = 14 px, any style) or by tag.
  `fontpreview.mjs list` prints both tables.
- A window's `<FONT size="14"/>` resolves the same way; `FONTCOLOR` sets its colour.

### 3.2 Rasterisation (d3dgl.dll: `D3D11GLFont`, `SGLFont`, `FT::FontFace`, `FontRenderManager`)

- FreeType: `FT_New_Face(file)`, unicode charmap, **`FT_Set_Pixel_Sizes(size, size)`** — the fonts.xml `size` is the
  pixel em size, not points. Two faces per font: a metrics face at `size` and a texture face at `tex_size` (= size in
  all shipped entries). Fallback face `Data\Fonts\arial.ttf`.
- Metrics (`FT::FontFace::SetupFace` @10009c00): `LineHeight = size.metrics.height >> 6`, `Ascent = ascender >> 6`,
  `Descent = descender >> 6` (negative), **`FontHeight = Ascent − Descent`**, `MaxHorizontalAdvance`.
  `line_height()`, `get_height()` etc. return these; `internal_leading = 0`.
- Glyphs (`D3D11GLFont::get_character_info` @10029ac0, `FT::GlyphData` @1000aa30): `FT_Load_Glyph(.., 0)` (hinted),
  `FT_Render_Glyph(NORMAL)` (8-bit AA). Per character: `advance = advance.x >> 6` (+ `bitmap_left`, which is 0 after a
  load), `pix_width = metrics.width >> 6`, cell height = `FontHeight`; outline (O) adds +1 advance, +2 width, +2 height.
  Characters with `C1_CNTRL` get an empty cell. **No kerning.**
- Atlas (`FontRenderManager::render_character_info` @1004a910 / `render_pendings` @1004ad60): 256×256 RGBA textures per
  font, glyph cells packed in rows from (4,4) with 1 px gaps; a new texture when full. The coverage byte becomes alpha
  with white RGB. **The bitmap is copied to the cell's left edge (bitmap_left dropped) at row `Ascent − bitmap_top`.**
  Strikethrough (S) writes a 2-px white bar across the middle of the cell. Outline (O) and aliased (A) cells are not
  rendered through FreeType in EE (they stay empty; see §10 Q3).
- `D3D11GLFont::write` @10027a20: `y` is the **bottom** of the line: the quad's top = `y − FontHeight·scale`; x starts at
  the pen and advances by `advance·scale`; quads at `x − 0.5`; `z = (0x1fff − update_depth)/8192`; clipped against the
  buffer clip rect (`add_character_clip`). Colour per vertex = the text colour.
- Text render state (`D3D11GLFont::init_class` @10027180): blend SRCALPHA/INVSRCALPHA, alpha test GREATEREQUAL 8,
  depth LESSEQUAL, colour = texture × diffuse, alpha = texture × diffuse; lighting cleared; the outline state uses alpha
  ref 0x80. Text is batched per font texture and flushed by `FontFamily::flush_fonts` (§5.3).
- `fontpreview.mjs render` reproduces this layout (left bearing dropped, hinted advances, cell = FontHeight) with an
  unhinted rasteriser; the visible consequence is slightly uneven spacing in italics (`Rise o fNations`).

### 3.3 Game-side text (`Buffer`, `bighuge:buffer.obj`)

- `Buffer` (a window's drawing context) keeps 3 fonts, 3 text colours and 3 shadow colours with x/y shadow offsets
  (`set_text_color`, `set_text_color2/3` @0050db40 store colour, shadow colour, shadow dx, dy). A string drawn with a
  shadow is written twice: shadow at +offset first, then the colour.
- `write_l`, `write_cent_l`, `write_right_l`, `wrap*` (word wrap with `find_line_break_l`), `write_strings`,
  `text_width`, `text_height`, `write_multi_font_raw_l` (mixed fonts on a line, max line height of the three).
- **Inline markup** switches between the 3–6 font/colour slots inside one string: `{` slot 1, `{*n` slot n, `[` slot 2,
  `}` / `]` back to slot 0, doubled characters are literals (details and the tooltip slot table in §6.5).
- `write_raw_l` @0050e470 expands `#ICONn` (§3.4) and otherwise passes the run to the backend font.

### 3.4 Inline icons and substitutions

- `#ICON` followed by a decimal index draws `icon_texs[n]` (from `icon_texs.xml`, ordered list of `IFACETEX`: food,
  timber, wealth, knowledge, metal, oil, the infrastructure icons, heads, supply, wonder, bonus, rare, tribute, caps,
  tracks, ...) inline with `add_smooth`, then advances by the icon width + 4. Help and rules text use it.
- `$NUMBER0`, `$NUM0`, `$NAME0`, `$TRIBE0`, `$LEADERNAME0`, `$GOODTYPE0`, `$UNITTYPE0`, ... are filled by the caller
  before drawing (`String::replace`-style substitution in the text producers).

### 3.5 String tables and languages

- **Language**: `LocalizationManager` (Steam's language name → enum, `GetLanguageFromSteamName` @00a42ad0, stored in
  prefs). Five languages; `gLanguageSuffix` (0x00b1ca54): **German `.7`, English `""`, Spanish `.10`, French `.12`,
  Italian `.16`** — the Windows primary language ids. English uses the bare file. The other shipped suffixes (`.4`,
  `.9`, `.17`, `.18`) are not selectable in EE. A localised file `name.xml.7` replaces `name.xml`.
- `internal_strings.xml` → `int_str_array` and `translated_strings.xml` → `loc_str_array`: positional tables
  (`StringTable::init` @00a28520). Code addresses element *i* at byte offset `i·0x14` (sizeof `String`), which is how
  decompiles show them (`locstrings.mjs int 0x1b4b8` → "Unit Blend"; renderstates.md uses the same rule).
- `string_lookup.xml`: `CAT hash=` → `STRING hash= hash_i=`; `String::lookup(text, category)` @00a1ddb0 hashes the
  English text and the category name and returns the localised STRING (falls back to the input). Categories recovered
  by hashing internal strings: `WINTITLE` (585 strings: window and control titles from the layout XMLs), `CATTITLES`,
  `CATNAME`, `CATDESC`, `MISC`, `DROPBUTTON`, `tilesets`, `SCRIPTTEMP`, `CTW_GAME_STYLE/*`, ...
- **Hash** (`String::generate_hash` @00a1b6b0, primes table 0x00b14500, 50 entries): iterate UTF-16 units from last to
  first with `n` = remaining length (len, len−1, ...): `h += primes[i % 50]·c + primes[c % 50]·n` (32-bit wrap); the
  case-insensitive `hash_i` does the same with `towlower(c)`. `locstrings.mjs check`: 7,605/7,630 internal strings and
  1,784/1,986 lookup strings match (the rest are keys whose English text was edited after hashing).
- `typenames.xml` (unit/building/tech display names by hash), `help.xml` (tooltips/help pages, `$STR` keys),
  `rules_string_lookup.xml`, `resourcerules_strings.xml` complete the set.

---

## 4. Input

All gameplay input in the main view goes through `Console` (`console.obj`, the "player console" window that covers the
map); the minimap is `WorldMap`. Both end in `GroupOut::issue_*` / `CommandManager::issue_*` (orders-spec,
replay-spec, production-spec cover what the commands do). Notation: key word = Windows VK | 0x10000 Shift |
0x20000 Ctrl | 0x40000 Alt. Four Controls options in the player profile (`player_profile.prefs`, optionswin.xml,
`OptionsWinControl::do_load` @0082b110) change the mouse model:

| Bit | Option | Effect |
|---|---|---|
| 0x20 | Right-Click Execute | a right click on a pending target spot picks it (off: cancels) |
| 0x200 | One Click Garrison | |
| 0x400 | Left-Click Interface (LCI) | left click issues orders, right click deselects, Alt shows context cursors, no formation drag |
| 0x200000 | Drag-Select Prefer Military | box select drops citizens when military units are in the box |

### 4.1 Mouse in the main view

- **Press** (`Console::down` @007cd8f0, left = 0, right = 1): capture the mouse, convert to terrain (`find_cam_vec` →
  `TerrainOut::convert_mouse_to_terrain`), remember start (world and screen), `drag_stamp = graphic_tick`,
  `clicking = 1`. Right: `dragging |= 2`, seed the formation drag from the selection's formation and spacing. Left:
  `dragging |= 1`; during a right formation drag a left press cycles the formation (`(form + 1) % 5`).
- **Click vs drag** (`on_mouse_move` @007cef20, screen-pixel distance): right 6 px; left 20 px if held more than
  7 graphic ticks, else 64 px (`Console::init` @007cbd90). Hover with no button: `execute_at_cursor(preview = 1)`
  only picks the cursor (§7).
- **Left click** (`left_click` @007c3bc0): pick the nearest visible object whose bounding volume is under the mid
  point of press/release (units and walls). No modifier: select only it. **Shift: add. Ctrl: toggle** (sound
  `S_CONTROL_GROUP_ACTIVATED`). Alt on an own unit in an army: select the army. Empty ground with nothing selected:
  select the resource/herd/ruins under the cursor for information. With LCI and a selection, a click on nothing /
  a non-own object / with Alt issues the order instead.
- **Double click** (`on_left_double_click` → `left_click(1)`): **every on-screen object of the same owner and type**
  (same active state and flag, captains only; units and buildings separately); Shift keeps the old selection;
  Alt+double click on an army member selects the army. Clicks within half the system double-click time after a
  double click are suppressed.
- **Box select** (left drag, `on_left_up` @007cde90): screen rect; units first, buildings only if no unit was boxed
  (Shift adds within the existing category); the player's own objects win, other players' only if none of his are
  boxed; `UnitOut::around_bounding_volume`; selection cap **128**, one owner, never mixed, captains substitute
  non-captains (`SelectGroup::add` @00716c10). Filters: none → (Prefer Military) drop citizens/caravans/merchants/
  fishermen if any military is boxed; Ctrl flips that preference; Ctrl+Shift citizens only; Alt drops civilians and
  non-attackers except siege; Alt+Ctrl siege only; Alt+Shift air only. Empty box: ruins / rare / herd for info.
- **Right click** (`on_right_up` @007cd3c0 → `right_click` @007c4e70): queue position **Shift = QUEUE_LAST (waypoint),
  Alt = QUEUE_FIRST, else QUEUE_NEW**. Pending target spot: pick or cancel (Right-Click Execute). Otherwise, with a
  selection that can act: `execute_at_cursor(0, 0)` (LCI: deselect); with nothing selected: select under the cursor.
- **Right drag**: caravans/merchant fleets → trade route between the cities under start and end (`issue_trade`);
  otherwise a **formation move**: destination = press point, facing = drag direction,
  `issue_move_to(..., MOVE_TO, form, spacing)` (patrol for domain-4 units), `ping_dest`. While dragging,
  `Scene::draw_drag_formations` @008b3e10 previews the slots (`Group::compute_form`) as circles in the player colour;
  the wheel changes spacing ±5 (0..100); arrows/numpad nudge the anchor 0x60 (Shift 0xc0) world units; left press cycles
  the formation.
- **`execute_at_cursor(preview, mode)`** @007c6630, priority with units selected: Ctrl → attack-move
  (`ATTACK_TO`), Ctrl+Alt → plain move, Alt with siege → siege attack; citizens → gather (forest/mine/oil/own gather
  building), build an unfinished own/allied site (`issue_swarm_around` BUILD_AT), repair a damaged building, garrison;
  then garrison/air garrison, patrol, spells (scout, spy, general, commando), trade, guard/follow friendly units,
  merchants/fishermen on rare goods, scouts on ruins, carriers and aircraft, silos; then **attack** an enemy under the
  cursor (`MouseOver::is_over_enemy` / `find_target(SEARCH_NON_FRIENDLY)` → `issue_attack`; "not at war" message via
  `tell_no_war`); fallback **formation move**. Buildings selected: right click sets the **rally point**.
- **Wheel** (`on_mouse_wheel` @007ccfa0): deltas accumulate (1 s window); each notch is a key-map lookup with mouse
  index 1 (up) / 2 (down) → default `ZOOM_IN3` / `ZOOM_OUT3`. Middle button and X1..X8 (indices 3, 4..) also go through
  the key map (`map_click` @007c3b30); bound to PING they start a ping-spline drag.
- **Minimap** (`WorldMap::on_left_down` @008c79b0, `on_right_up` @008c7050): left press/drag moves the camera
  (`moving_cam`, `update_world_map`) or picks a pending target spot; right release issues orders at that world point
  with the same queue modifiers (not with LCI); caravans drag trade routes; middle/X buttons via the key map.

### 4.2 Keyboard and the key map

- `Console::on_key_down` @007cc210: ping drag / formation-drag nudges first; while placing a building, Ctrl or Shift
  marks multi-place; then **`KeyMap::lookup(options.mode, key, 0)`** @007d4780 → `exec_keymap(i)` @007c2ce0
  (Esc with nothing selected becomes the menu); else Alt+digit switches the viewed nation (observers/shared
  control); else control groups (§4.3). `on_key_up` @007cc1a0: releasing Ctrl ends building placement; releasing
  Shift ends waypoint targeting (ATTACK_TO / PATROL / FLEE_TO).
- **Defaults are compiled into the exe**, not in a data file: `KeyMap::close` @007d5de0 fills 461 `KeyMapEntry`
  (0x50 bytes) from static arrays (`default_keys` @00aee4f0, `default_mouse` @00aed630, `default_modes` @00aeec60,
  `default_types` @00aedda0, `default_groups` @00aecd10, `default_dependent` @00aed460) and `keymap_options` @00ae7770
  maps each to an `(OptionIndex, param)` for `Options::exec` @007188c0. Names = PDB enum `KeyMapType` order.
  **`node re/tools/keymap.mjs [--all] [--tsv]`** prints the table from the user's exe (406 of 461 bound).
- Entry: key word, mouse index, mode (`OptionMode` 0 standard, 1 build menu, 4 script IDE), option + param,
  selection context (`type_selected`: −1 always, −2/−4 buildings, −3 units, −5 aircraft, −6 land/sea units, ≥0 a
  TypeIndex in the selection), group bit mask (23 groups in the hotkey options list), `dependent` (modifier-only
  variant of the preceding entry: C cycles cities, Ctrl+C without zoom, Shift+C selects all). Lookup tries the
  current mode, then mode 0; context gating lets one letter mean different things (G garrisons units, trains at a
  barracks). TRAIN entries also match with Shift (batch), QUEUE ignores Ctrl/Shift.
- Display strings: `Data/playerprofile.xml` `ROOT/KEYS/KEY enum=` (localised names only; its `INPUT` children are
  ignored for defaults). `KeyMap::init` @007d5a90 **writes missing entries back into the file** (the port must not touch
  the install). Key names come from `GetKeyNameTextW`.
- Rebinding: `OptionsWinHotkeys` → `KeyMap::assign_key` @007d32d0 (conflict popup on same key/mouse/mode with
  overlapping context). Saved in the player profile `.dat` (CloudXML) under `ROOT/KEYS`: only entries that differ from
  the defaults, `KEY enum= dependent=` with `INPUT key mouse ctrl shift alt` (`KeyMap::save` @007d5720,
  `PlayerProfile::save` @005d1340); loaded only if `keymap_version == 0x3e`.
- Main defaults: F10 menu, F11 options, Pause/P pause, −/= speed, Enter chat (Shift all, Ctrl allies, Alt enemies),
  F9 diplomacy, Insert/X2 ping, Space next message, PgDn/Num3/wheel-up zoom in, PgUp/Num9/wheel-down zoom out,
  Delete disband, G garrison, M move, A attack-move, Z patrol, H halt, B build (then the build-menu letters: F farm,
  W woodcutter, M mine, C city, L library, K barracks, ...), Home all current, Tab cycle research, period idle
  citizens, comma military, letters cycle buildings of a type (Ctrl no zoom, Shift all), R rally point, Q infinite
  queue, Ctrl+letter researches at a building. Arrow-key scrolling is hard-wired (§4.4).

### 4.3 Control groups

18 groups per player in `hotkey_groups` (index `who·18 + n`; digits 0–9 → 0–9, F1–F8 → 10–17). A group is a unit list
or a **screen bookmark** (camera x, y, zoom). **They are simulation state** (HOTKEY command, replay-spec #34;
`CommandPackage::process_hotkey` @009474d0; saved), while the current selection is local.

| Input | Effect |
|---|---|
| Ctrl+N | selection non-empty: `issue_hotkey(n)` (HOTKEY, clear = 0), sound `S_CONTROL_GROUP_FORMED`; empty selection: bookmark the camera (clear = 1, valid = 1, x, y, zoom) |
| Ctrl+Shift+N | erase (clear = 1, valid = 0) |
| N | bookmark → move the camera; unit group → select it locally (`SelectGroups::copy_group` @00716220), unless scripts disabled hotkey selection |
| N again | centre on the group (smooth pan if farther than 0x480 world units, else jump); a third press zooms to level 6 |
| Shift+N | add the group's live members to the selection |
| F1–F8 alone | observers/replays (or empty/foreign group): switch the viewed player |

### 4.4 Camera keys, edge scroll, zoom

- Zoom levels 0..6 (6 closest; `Camera::fixed_zooms`). Zoom in steps +1 to 6 and **zooms toward the cursor** when the
  mouse is over the map or minimap (`Camera::zoom_in(x, y)` @00843f10; repeated steps within 500 ms keep the first
  point). Zoom out steps −1 down to **4** (2 in replays) (`zoom_out` @00843fe0). Both ease (`check_zoom` @00843c30).
  No in-game rotation or tilt keys; tilt follows zoom (`adjust_tilt`).
- **Scrolling** (`Camera::check_scroll` @008440f0, once per rendered frame): edge band `scroll_edge` = **2 px**
  (`def_scroll_edge` @00af0ac4); left/right edges only scroll above the HUD bar top − 40 px (or while dragging);
  arrows / numpad 4,6,8,2 when no Ctrl/Alt and no modal window. Step along the camera heading
  (`scroll_dir` @00842f60) of `min(1, dt_ms/67·0.5) · 63.9936 · speed_option · (0.25 while box-dragging) ·
  scroll_speeds[zoom]`, with speed_option for profile settings 0..12 = 0.33, 0.5, 0.75, 0.9, 1, 1.1, 1.25, 1.5, 1.75, 2,
  3, 4, 5 and `def_scroll_speeds` @00c06260 = 48, 32, 24, 16, 12, 9, 8 (zoom 0..6). Scrolling cancels unit follow.

### 4.5 Building placement preview

Picking a building on the build page sets `options.opt` = BUILD / BUILD_MILITARY / BUILD_WONDER and `selecting_spot`.
Every frame `Options::draw_cursor_follows` @00723c00 (called from `Scene::render`):

1. snap the cursor point with `BuildTypeData::snap_center` (world-spec §6); oil buildings become the oil platform
   (0x1a6) on water when the OILPLATFORM tech is known; civilian, woodcutter and mine types look up the attaching city;
2. **footprint** (`BuildTypeOut::draw_template` @00634250): re-draw the terrain mesh of every footprint tile with
   render state 19 **Template Blend** and a per-tile vertex colour: ordinary building **green** checkerboard
   (`(x+y)&1` picks two greens); gather buildings green where the needed resource is, **yellow** where it is not;
   **orange** for soft blocks (CITY_RADIUS, ONE, ONE_OTHER, FARM, CITY_DISTANCE, FORT_DISTANCE, FORT_CITY_DISTANCE,
   WONDER); **red** for any other `blocked_site` code. A label scroll in the player colour (font `template_size`) gives
   the name, resource levels (`GoodTypeOut::say_level`) and the blocked reason (`BuildTypeOut::say_blocked` @00635c50);
3. **ghost building** (all but farms): `BuildTypeOut::draw` @00634060 with `whom = blocked code`: 0 draws the normal
   passes (#28 Team Color Blend then #26 Building Blend), non-zero draws #25 **Blocked Building Blend**; facing fixed
   (no rotation);
4. range rings: cities/forts white `GENERAL_SELECT_CIRCLE` at the territory radius; defensive buildings in the player
   colour (alpha 0xcf) at their range; type 0x209 at its LOS (alpha 0x4f).

Click (`Options::picked_spot` @00721c40): `GroupData::validate_build`, city limit (message + `S_CITY_LIMIT_REACHED`),
else `S_BUILDING_DROPPED` + `GroupOut::issue_build(x, y, x2, y2, type, queue)` (walls use press and release points).
**Ctrl or Shift held: placement stays active** (Ctrl = QUEUE_FIRST, Shift = QUEUE_LAST); releasing Ctrl ends it.

### 4.6 Input evidence

| Function @ address | Shows |
|---|---|
| `Console::down` @007cd8f0, `on_mouse_move` @007cef20, `init` @007cbd90 | press state, click/drag thresholds, hover preview |
| `Console::left_click` @007c3bc0, `on_left_up` @007cde90, `on_left_double_click` | pick, Shift/Ctrl/Alt rules, double click, box select and filters |
| `Console::right_click` @007c4e70, `on_right_up` @007cd3c0, `execute_at_cursor` @007c6630 | queue modifiers, trade and formation drags, context orders |
| `Console::on_mouse_wheel` @007ccfa0, `map_click` @007c3b30, `on_middle_down` @007cdcc0, `on_extra_*` | mouse buttons through the key map |
| `Console::on_key_down` @007cc210, `on_key_up` @007cc1a0, `exec_keymap` @007c2ce0 | key routing, control groups, dispatch to `Options::exec` @007188c0 |
| `KeyMap::close` @007d5de0, `init` @007d5a90, `lookup` @007d4780, `save` @007d5720, `assign_key` @007d32d0 | default table, lookup gating, profile storage |
| `CommandManager::issue_hotkey` @00941fd0, `CommandPackage::process_hotkey` @009474d0 | control groups in the simulation |
| `Camera::check_scroll` @008440f0, `scroll_dir` @00842f60, `zoom_in` @00843ed0/@00843f10, `zoom_out` @00843fe0 | scroll and zoom |
| `Options::draw_cursor_follows` @00723c00, `BuildTypeOut::draw_template` @00634250, `Options::picked_spot` @00721c40 | placement preview and multi-place |
| `WorldMap::on_left_down` @008c79b0, `on_right_up` @008c7050 | minimap input |

---

## 5. Rendering the UI

### 5.1 Where it sits in the frame

`Game::loop_render` @005917e0 → `Scene::render` @008b3270 (3D into HDR, tone map, bloom, FXAA, then the post-tonemap
extras of lighting-spec step 10: drag box, `ObjectsOut::draw_overlays` (selection circles, health bars), ammo,
shock waves, `describe_mouseover`, `Scene::draw_text` (status/mouse-over text), `AnimNotice::process_all`,
`draw_game_stats`) → `BaseScene::render_end` @00538c40:

1. `Window::update_screen` @00a54460: `FontFamily::start_render`, `update_zorder`, full-screen viewport,
   `update_depth = start_update_depth`; then **every visible window from the bottom of the z-order to the top** runs its
   `on_redraw` (vslot 0x3e4: queues quads and text) followed by `inc_update_depth`. Windows hidden by an ancestor with
   style bit 0x2000000 are skipped.
2. `Scene::post_update_screen` @008b3d00: flush the card, full viewport, **`IFaceRenderManager::flush_all`** (all UI
   quads), **`FontFamily::flush_fonts`** (all text), `ScreenArrowSpline::draw_arrows` (the on-screen pointer arrows),
   scenario highlight circles.
3. `graphsys->end_drawing/end_scene`: the backend draws the **cursor** last (`D3D11GLCursor::render`), then presents.

### 5.2 UI render states (`IFaceRenderManager::setup_render_states` @0051e170)

Nine `RenderState`s (separate from the 50 scene states; defaults per renderstates.md §0: depth test + write, LESSEQUAL):

Indexed by `IFaceRenderType` (rise.pdb); the `add*` calls take the type:

| # | IFaceRenderType | Changes from the default | Seen used by |
|---|---|---|---|
| 0 | NORMAL | zfunc LESSEQUAL only: opaque, depth write, no blend | backgrounds (`draw_background`), opaque fills |
| 1 | ALPHA | + blend SRCALPHA/INVSRCALPHA, alpha test GREATER 0, alpha = MODULATE(texture, diffuse) | **most pieces**: borders, icons, digits, buttons (`add_smooth` default) |
| 2 | COLORALPHA | blend **SRCALPHASAT**/INVSRCALPHA, alpha test GREATER 0, alpha MODULATE | `IFaceButton::on_redraw` |
| 3 | HIGHLIGHT | colour MODULATE2X (opaque) | button and market-button highlight |
| 4 | ALPHAHIGHLIGHT | blend + alpha test, colour MODULATE2X, alpha SELECTARG1(texture) | |
| 5 | COLORBLEND | blend, colour and alpha SELECTARG2 (= vertex colour, texture ignored) | translucent colour fills (`IFaceButton`) |
| 6 | ALPHA2 | copy of ALPHA in its own batch | inline `#ICON`s in text (`Buffer::write_raw_l`), objectives |
| 7 | COLORALPHA_ALPHAMODWITHCOLORALPHA | blend, colour and alpha MODULATE(texture, diffuse), no alpha test | `MessageWin::on_redraw` (fading messages) |
| 8 | ..._ADDFB | additive SRCALPHA/ONE, colour SELECTARG1(texture), alpha MODULATE | |

None sets lighting or TL_FAKE_LIGHT, so the UI is plain LDR into the back buffer. The legacy "Nuke" pixel-shader
situations they register are dead in EE (renderstates.md).

### 5.3 Batching, order and depth

- `flush_all` @0051fbe0: for each packet in `packet_render_order`, for each of the 9 states, for both modes: bind the
  texture (none for packet 0), set the state, submit the packet's triangle list (pre-transformed vertices, `add_tl_prim`
  path), then `clear_all`. Order between batches is therefore by texture, not by window.
- **Visibility between windows comes from depth**: every quad and glyph carries `z = (0x1fff − update_depth)/8192`;
  `update_depth` rises by 1 per window and by 2 per drawing layer inside a window, so later (higher) layers get smaller
  z and win the LESSEQUAL test even though text is flushed after all quads. Alpha-tested edges keep cut-outs from
  writing depth.
- The scene leaves the back buffer's depth populated; the UI z values are ≥ 0.75 (update_depth < 2048). See §10 Q1.
- **Port**: emulate with a UI pass that has its own cleared depth attachment (or sort all UI quads and glyphs by
  update_depth, stable, and draw in one pass with blending; this is equivalent because every layer is either opaque or
  alpha-tested-and-blended). Draw after the post-tonemap overlay pass (`ron::metal::overlay_pass`) in the same LDR
  target, then the cursor.

---

## 6. The HUD

Status: 🟡 from the decompiles (`re/decomp/ui_hud`, `ui_hud2*`) and the layout (`ifacelayout.mjs tree`). Positions are
for 1024×768. Panel contents are code, so this section paraphrases the draw functions. Nothing has been checked
against a running game.

**Clock used everywhere:** `Game::graphic_tick` is a wall-clock counter. `Game::loop_render` @005917e0 adds 1 per 66 ms
(≈ 15.15 Hz) whatever the frame rate. So `graphic_tick & 8` blinks are ≈ 0.53 s on and 0.53 s off, and every
"N ticks" below means N × 66 ms. `AnimNotice` uses `timeGetTime` milliseconds instead.

### 6.1 Frame and window tree

**Ownership.** `IFaceMain` (derived from `IFaceMainBase`) is the HUD object (`MiscAccess::iface_main` /
`iface_current`). Every panel is a member of it. `add_windows` @00809610 sets the parents; they are read from the
disassembly, because the decompile loses them.

| Parent (member) | Rect → screen @1024×768 | Children |
|---|---|---|
| `left_window` (+0x48c) | (0,34)–(316,197) → (0,604)–(316,767) | `iface_options` (IFACEOPTIONS), `iface_options_building` (IFACEOPTIONSBUILDINGS), `iface_market`, `iface_library`. This is the **command card**, bottom-left |
| `mid_window` (+0x8f8) | (316,1)–(648,199) → (316,571)–(648,769) | `iface_minimap` (IFACEMINIMAP 332×196) |
| `right_window` (+0xd64) | `left=655 right_inverse=14`, y 34..195 → (655,604)–(1010,765). It **stretches with screen width** | `iface_selected` (IFACESELECTED), `iface_garrison`, `iface_air_garrison`. This is the **selection** panel |
| HUD itself | IFACEMAINBASE (0,570)–(1024,768): 198 px tall, full width | `iface_diplo`, `iface_diplo_neg` |
| screen (top level) | – | `iface_resources` (top-left), `iface_mini_library` (399,25) 227×51 |

The three slot windows are plain `GraphicWin`s (style 0x1022) created in `IFaceMainBase::init` @00812360 from the
named IFACERECTs.

**Which panel shows.** `IFaceMain::update_visibility` @00809980 decides this every frame while the scene draws the
interface (draw flag `[2] & 0x20`):
- Resources, the mini library and the minimap are always shown.
- In diplomacy mode only `iface_diplo` / `diplo_neg` show.
- With a selection, `iface_selected` and `iface_options` show (the options come from `SelectGroup::make_options`).
- The selected object (group leader, first building, or the player's first library for a library selection) can then
  swap the left slot. This happens only if it is owned by the viewer or an ally, or in observer mode (semaphore & 0x50).

| Type (`typeindex.tsv`) | Left slot | Right slot extra |
|---|---|---|
| 427–434 BARRACKS…SHIPYARD | IFaceOptionsBuildings | IFaceGarrison |
| 447 AIRBASE | IFaceOptionsBuildings | IFaceAirGarrison |
| 435 LIBRARY (finished) | IFaceLibrary | – |
| 436 MARKET | IFaceMarket (an IFaceOptions subclass with buy/sell prices, `calc_market_prices`) | IFaceGarrison |
| 351 AIRCRAFTCARRIER | IFaceOptions | IFaceAirGarrison if it is the only thing selected |
| other with a garrison | IFaceOptions | IFaceGarrison |

**Right-slot layout.** `IFaceMain::position_windows` @00809250:
- With more than one object selected, the selected panel is resized to the right slot's surface minus 8 and moved to
  (4,4). At 1024 that is 347×153.
- Otherwise the panel uses its `main` rect (294×158), or `main_streamlined` (626×158) when the HUD is collapsed. The
  garrison or air-garrison panel then fills the rest: width = slot − selected − `display` button − 8. That is only
  ≈ 42 px at 1024, so the grid shrinks (§6.3). The air garrison switches to `draw_small` below 101 px.

**Opened vs streamlined.** `IFaceMainBase::opened` (+0x470) is toggled by the `display` button
(OPTION_TOGGLE_IFACE_MAIN 317). `IFaceMainBase::update_visibility` @00811d40 handles each mode:

**Opened**
- All windows sit at their XML rects.

**Streamlined**
- The minimap moves to the right end (x = 1010 − 332 = 678).
- The right slot moves to x = 316 and narrows to 362 px.
- The left and right slots are hidden unless `GroupData::show_interface` says the selection needs them.
- The bar buttons are hidden.

**Bar buttons in both modes**
- The buttons below get their visibility bit from `opened`.
- Cannon also needs the cannon-times game option. It is disabled while a cannon time is running or when the player
  has none left (`Leader +0x48`).

**Drawing the bottom bar.** `IFaceMainBase::on_redraw` @00811600, in order:
1. `compute_cap_limits` @0080edf0 sets `pop_limit` to 1 when population ≥ cap. It also sets it when population is
   within 10 of the cap and a finished military trainer has a queued unit that `check_population` refuses.
2. The piece `bar` (TEX_CIV 390,131 122×34, ALPHA) is tiled across the top 34 px of the HUD: across the left slot when
   the slot is visible, and across the whole width when opened.
3. When opened, the clock (`draw_button_row_area`, below).
4. Frame edges, all `tile_random` ALPHA:
   - When the left slot is visible: `left_tile`, a 3 px left edge at y 34..198.
   - When streamlined: `left_tile_only`, a 3 px bottom edge under the left slot, and `streamlined_end_cap` (24×197)
     at the slot's right edge, bottom-aligned.
   - When opened: `right_tile`, a 3 px bottom edge across the HUD and a 3 px right edge.
5. `iface_render_manager.x_offset = y_offset = 0`. From here on everything is drawn **at the top of the screen**:
   `draw_cap_area`, `draw_pop_cap`, `draw_city_cap`, `draw_caption`, `draw_special_resources`.

`on_window_redraw` @00811b60 paints slot backgrounds:
- Left slot, only when none of its panels is visible: `back` (TEX_CIV 0,371 160×141) tiled with NORMAL (opaque), plus
  IFACEBORDER `back`.
- Right slot, when opened: `back` tiled.
- Otherwise each panel paints its own background.

**Top-of-screen strip.** All rects are right-anchored and resolved against the screen width.

| Element (rect @1024) | Drawn by | Content |
|---|---|---|
| caption, centred, 420×43 at y 0 | `draw_caption` @0080f360 | `caption_ends_left` (105) + 10 × `caption_ends_mid` (21) + `caption_ends_right` (105) via `draw_expander`. Text nation name and age, 14 px, colour (212,212,212), centred in y −2..23. Spots: tribe part → OPTION_TRIBE 180, age part → OPTION_AGE 182 |
| `cap_area` (801,0)–(974,25) | `draw_cap_area` @00810370 | Shared IFACEBORDER `border3` around `cap_area` and around help+menu (inset −4); vertical divider at menu.x − 1 |
| `pop_cap` (805,4)–(902,21) | `draw_pop_cap` @0080ff90 | Plate `IFaceResources::draw_back` (§6.2) in (50,50,50); **RED, blinking, while `pop_limit`**. `pop_cap` icon (TEX_COMMON 0,115 29×19) scaled to the rect height (NORMAL). "control/pop_cap" (LeaderData +0x940 / +0x7e4, economy-spec §7.1) in digit set 0, white, centred in the rest. Divider after the icon. Spot OPTION_STATX 176 index 0 |
| `city_cap` (903,4)–(970,21) | `draw_city_cap` @0080fbe0 | Same plate (never red), `city_cap` icon, "cities/limit" (`get_total_cities` / `get_city_limit`). Spot OPTION_STATX index 2 |
| `help_button` (977,4) 21×17 | Button, parent = console | Tex 23×19. OPTION_TOGGLE_HELP 126 |
| `menu_button` (998,4) 22×17 | Button | Tex 22×19. OPTION_MENU 89 |

**Bar buttons.** These are IFACEMAINBASE BUTTONs with ids set in `init`. A click (`on_button_clicked` @008113a0)
plays S_FEEDBACK, counts the click, then calls `Options::exec(id)`. Rects are in bar coordinates; add (0,570) for
screen.

| Button | Rect | Tex | Option |
|---|---|---|---|
| pause | (4,10) 24×19 | TEX_CIV 313,0 | OPTION_PAUSE 153 |
| clock spot `timer_area2` | (32,10)–(232,29) | `time_back` 124×19, stretched | OPTION_TIMER 155 |
| cannon | (235,10) 48×19 | TEX_COMMON 462,216 | OPTION_CANNON_TIME 154 |
| advanced_options | (858,10) 25×19 | TEX_CIV 305,146 | OPTION_TOGGLE_ADVANCED 128 (pref bit 2: the "Advanced Menu" extra unit orders, §6.4) |
| ping / diplo / chat / stats | (886,10) 25 / (914,10) 48 / (965,10) 27 / (995,10) 25, all 19 tall | TEX_CIV 384,0 / 410,0 / 459,0 / 487,0 | OPTION_PING 86 / OPTION_DIPLO 163 / OPTION_CHAT 82 / OPTION_NEXT_DATA 123 |
| display | (1010,34) 11×161 | drawn by `on_button_redraw` @00811020 | OPTION_TOGGLE_IFACE_MAIN 317 |

The display button is a vertical 3-slice:
- Pieces `collapse_button_up0/1/2`, or `collapse_button_down0/1/2` when pressed or disabled.
- Three 4×7 arrows are stacked in the middle: `…arrow_right` when opened, `…arrow_left` when streamlined.

**Clock.** `draw_button_row_area` @0080f7b0 draws in window coordinates.
- Text: the game-speed name (a paused word when paused, or when a non-diplomacy modal window is open), then ": ", then
  `say_timer` (H:MM:SS), then "  (CT: n)" when cannon times are on.
- Style: font `clock_size` (12 B), colour slot 0 white, slot 1 red, both with a 1,1 black shadow. Centred over
  `time_back`, which is stretched into `timer_area2` (NORMAL).
- While paused the text blinks off.
- In a ranked game, for the first 60 ticks it shows an ELO grace-period countdown in green.

**Rare resources.** `draw_special_resources` @0080ef80, screen coordinates, `sort_depths[2]`:
- For each good 7..49 the player owns or holds by conquest (`rare_owned` / `rare_conquest`), the good's icon is drawn.
  The icon comes from TEX_SPECIAL_RESOURCES `art/resource_icon.tga`, with the rect at goodtypes +0x2fc..+0x302.
- Icons are drawn at 50 % alpha (colour 0x80FFFFFF, ALPHA).
- Layout: a column from x = 10, y = resource panel bottom + 20, with 4 px gaps. A new column starts every 8 icons. The
  message list (§6.7) is pushed right by 50 px per column.
- A count is written in digit set 0 when the player has more than one (Leader +0x6c0[g]).
- Each icon has a spot (id g | 0x80000000) for its tooltip.

**Production progress buttons.** `setup_progress_buttons` @0080db10, only with profile pref bit 0x40:
- Pooled IFaceButtons (`IFaceButtonPool`) parented to the screen, stacked down the right edge below the timers.
- Position: x = screen width − w − 12, 6 px gaps, a new column to the left every 7 buttons.
- One button per unit or tech in production, with `percent_done` = progress / `train_time` and `outline_color` = team
  colour. Wonders under construction (`unbuilt_wonders`) follow.
- A click centres the camera on the producer (negative button id → select it + OPTION_CENTER).

**Game timers.** `draw_timers` @0080b060 → `draw_game_timer` @0080a3c0, `sort_depths[11]`:
- **Column:** top-right, right edge at screen width − 10, from y = 45, one timer under another.
- **Kinds:** 0 wonder, 1 capital, 2 armageddon, 3 territory, 4 peace, 5 time limit (6 barbarian when team style is 3),
  7 musical chairs.
- **Icon:** the kind's 42×39 `*_timer` icon (TEX_TIMERS `art/iface_timers.tga`), right-aligned.
- **Background:** `background_timer` (60×26) to the icon's left, vertically centred. For a player's timer it gets a
  team-coloured border: `vert_border_timer` 2×26 at both ends and `horiz_border_timer` 54×3 top and bottom, inset 3.
- **Digits:** `0..9_digit_timer` (11×20) as M:SS, starting at (x+5, y+3), with `colon_timer`. Time is counted in sim
  frames (900 per minute).
- **Digit roll:** a digit about to change cross-fades during the last second. The old digit has alpha (1−f²)·255 and
  the new one the complement, using render type 7.
- **Wonder timer:** also writes the owner's `get_wonder_net` in 18 px, in team colour, left of the icon.
- **Armageddon:** shows a two-digit count between `armageddon_timer_left` and `armageddon_timer_right` instead.
- The time-limit text colour blinks yellow/red below 120 frames and yellow/white below 300.

**Animated notices.** `IFaceMainBase::do_notice` @008107d0 / @0080d8c0 and `AnimNotice` @007b0840..007b1170:
- **Icon:** `do_notice(name, text, colour)` takes an IFACEMAINBASE piece from TEX_ALERTS `art/iface_alerts.tga`, sizes
  63×28 to 63×64. Examples: `age_notice1..7`, `wonder_*_notice`, `city_lost/captured/under_attack_notice`,
  `capital_under_attack_notice`, `victory/defeat_notice`, `cannon_time_notice`, `merchant_notice`.
- **Path:** it starts at native size, centred on the screen. It ends at half size with its top-left at (screen width,
  screen height), so it flies off the bottom-right corner.
- **Timing** (`init` @007b0a10): delay = `message_length`·400 + 500 ms, then a 500 ms move.
- **Text:** 22 px bold, white with a 1,1 black shadow, centred 5 px under the icon. It shows only while the notice is
  waiting.
- **Drawing** (`process` @007b0c50, `sort_depths[11]`): f = clamp((now − start − delay)/500, 0, 1). The rect is
  interpolated from start to end, with a `draw_scroll` plate behind it (covering the text too while f = 0). At f = 1
  the notice is removed (`process_all` @007b06c0, at most 100 at once).

| Function @address | Shows / does |
|---|---|
| `IFaceMain::add_windows` @00809610, `IFaceMainBase::init` @00812360 | slot windows, parents, button ids, `good_order` |
| `IFaceMain::update_visibility` @00809980, `position_windows` @00809250 | which panel shows; right-slot geometry |
| `IFaceMainBase::update_visibility` @00811d40 | opened/streamlined moves, button visibility, cannon enable |
| `IFaceMainBase::on_redraw` @00811600, `on_window_redraw` @00811b60, `on_button_redraw` @00811020 | bar, edges, slot backgrounds, collapse button |
| `draw_caption` @0080f360, `draw_cap_area` @00810370, `draw_pop_cap` @0080ff90, `draw_city_cap` @0080fbe0, `compute_cap_limits` @0080edf0 | top-of-screen strip |
| `draw_button_row_area` @0080f7b0, `say_timer` @007f1b00 | clock |
| `draw_special_resources` @0080ef80, `setup_progress_buttons` @0080db10 | rare-resource column, progress buttons |
| `draw_timers` @0080b060, `draw_game_timer` @0080a3c0 | timer column |
| `do_notice` @008107d0 / @0080d8c0, `AnimNotice::init` @007b0a10 / `process` @007b0c50 | notices |

Port notes:
- Build three slot windows plus two floating panels.
- Set the UI origin to (0,0) before drawing the top-of-screen strip.
- Drive all blinks from a 66 ms wall-clock tick.
- At widths above 1024 only the right slot grows; everything else is fixed pixels (§2.4).

### 6.2 Resource panel (IFaceResources, @00817ba0..0081ba60)

**Window.** A top-level, client-draggable (`CLIENTDRAG`) window at the screen's top-left. The XML says (0,0) 316×126;
`adjust_rects` @00819a90 resizes it from the content.
- **Sections:** market | resources | infrastructure, each with a collapsed rect [0] and an expanded rect [1].
- **`flags`:** bit 0 market, bit 1 resources, bit 2 infra. Persisted in `player_profile.market_open / resource_open /
  infra_open` (`init` @0081ac10).
- Market and infra count as open only together with resources ((flags & 3) == 3, (flags & 6) == 6). Collapsing the
  resources hides both side toggles.

**Rows.** `on_redraw` @0081b3b0 counts the goods passing `LeaderData::type_avail` (observers get all six), plus
`num_market_resources` (without wealth and knowledge).
- **Order:** `IFaceMainBase::good_order` = **food, timber, metal, oil, wealth, knowledge**, skipping goods that are not
  available.
- **Spacing:** row pitch 20 (17 + 3). The first row is at y 25. Section height is 26 + 20·rows: 146 with six goods,
  106 for the market's four.
- **Fully open at 1024:** market (0,0)–(120,106), resources (119,0)–(234,146), infra (233,21)–(316,146). Each section
  starts 1 px left of the previous one's right edge.
- **Section art:** `draw_border` with `corner_tex0..3` (6×6) and `side_tex0..3` (TEX_CIV). `draw_horiz_divider` /
  `draw_vert_divider` (pieces `thick_*`, `thin_*`, `cross`) run between the header and the rows and between rows.

**The plate.** `draw_back` @0081a920 is a 3-slice of `resource_item_tex0/1/2` (TEX_COMMON 364,85: 7+67+7 × 19)
stretched to the rect, drawn with render type **HIGHLIGHT** (texture × colour × 2) in dynamic mode. The normal colour
is (50,50,50). Caps (§6.1) and infra cells use it too.

**Resource section** (`draw_resource_area` @00818cc0):

*Header*
- The toggle button (4,4) 39×17: `resource_button_tex` with `up_arrow_tex` / `down_arrow_tex` centred.
- The **commerce cap** box (47,4)–(111,21), drawn by `draw_cap` @00819370:
  - plate (50,50,50), with `cap_tex` (TEX_COMMON 461,197 21×17) scaled to height − 2;
  - "+N" in digit set 0, green, where N = min over available goods of `resource_cap[k]`/16 (economy-spec
    `LeaderDataEncrypt`; starts from 999·16);
  - if any `over_cap[k]` ≠ 0 the box blinks: RED plate, BLACK text;
  - spot type 1, id 1.

*Rows* (`draw_resource` @00818840), in (4,25)–(111,42) + 20·i:
- **Plate:** (50,50,50). **RED** with black text for 8 ticks after an order failed for lack of this good
  (`console.flash_resource_stamp`, bit `1<<good` of `flash_resource_flags`). **YELLOW** with black text, blinking,
  while `over_cap[good]` ≠ 0.
- **Icon:** `first_good_tex` (TEX_COMMON 106,169 21×19) shifted right by good·25, drawn at x+1.
- **Stockpile:** `bucket[good]` in digit set 0 at x+1+21+7, vertically centred, white (black when highlighted).
- **Rate:** "/" then "+N" with N = (`income[good]` + 8)/16, or "0" with no plus. GREY, or the highlight text colour
  while over cap or flashing. Observers see only the stockpile.
- **Spot:** type 0, id good.

**Market section** (`draw_market_area` @00818f20):
- Vertical toggle at the left, (3,3), 11 px wide.
- Title (17,4)–(116,21) on a plate: buy | sell labels centred in its halves, 14 px, GREY.
- Rows for food, timber, metal, oil, level with the resource rows. Column divider, with `cross` at the row dividers.

**Buttons.** 15 IFaceButtons (`setup_buttons` @00819d10, placed by `adjust_buttons` @00819660):

| Id | Button |
|---|---|
| 0 | market toggle |
| 1 | resource toggle |
| 2 | infra toggle |
| 3–8 | **sell**, at (68,25) 48×17 per row |
| 9–14 | **buy**, at (17,25) 48×17 per row |

**Market buttons** (`draw_market_button` @00817ba0):
- A 3-slice `market_button_tex0x` (8 + tiled 30 + 8, all ×17), switching to `…tex1x` when pressed (content shifted
  +1,+1).
- Render type HIGHLIGHT. Tint **0x007B9B** for sell, **0x006C1E** for buy, ×2/3 when disabled.
- The price in **digit set 1**, centred, white (GREY when disabled).

**Clicks** (`on_button_clicked` @0081b570):
- Buy/sell buttons run OPTION_BUY 57 / OPTION_SELL 60 for the good. Shift gives `*_500`, Alt gives `*_ALL`.
- Toggles run OPTION_TOGGLE_IFACE_MARKET 315 / _RESOURCES 314 / _INFRA 316.
- Enable states come from `update_visibility` @0081ba60.

**Infrastructure section** (`draw_infra_area` @00818270, `draw_infra` @00817f30):
- Vertical toggle at the right, (69,3).
- Rows in (4,4)–(66,21) + 20·i: plate, then an icon from `first_good_cap_tex` (TEX_COMMON 96,149 19×19, stride 20),
  then "workers" (white) "/limit" (GREY). Per good:

  | Good | Workers / limit |
  |---|---|
  | food | farmers / Leader +0x8a4 |
  | timber | woodcutters / +0x8a8 |
  | wealth | caravans / `get_caravan_limit` |
  | knowledge | scholars / +0x8b0 |
  | metal | miners / +0x8b4 |
  | oil | oil workers / +0x8b8 |

- Counts are rebuilt every frame by walking the player's units: citizens whose gather order targets FARM 417 /
  WOODCUTTER 418 / MINE 419 / UNIVERSITY 420 / OILWELL 421–OILPLATFORM 422, scholars inside a building, citizens inside
  a building (counted as oil; inferred: oil platforms), and caravans.
- Spots, type 1: id 4 farmers, 5 woodcutters, 3 caravans, 7 scholars, 6 miners, 8 oil. These are OPTION_STATX tooltip
  indices; the top strip uses 0 pop cap, 1 commerce cap, 2 city cap.

**Toggles** (`on_button_redraw` @0081af70):
- The vertical ones are 3-slices of `top/mid/bottom_vert_button_tex0` (`tex1` when pressed), with `left/right_arrow_tex`
  down the middle.
- Hover: `on_button_move` @0081b720 sets the tooltip option (314/315/316, or BUY/SELL with the good). `on_spot_move`
  @0081b4e0 handles rows (MouseOver type 0x200, t = good) and cells (type 0x80, index).

| Function @address | Shows / does |
|---|---|
| `on_redraw` @0081b3b0, `adjust_rects` @00819a90, `adjust_buttons` @00819660 | row count, section geometry, button placement |
| `draw_resource_area` @00818cc0, `draw_resource` @00818840, `draw_cap` @00819370, `draw_back` @0081a920 | resource rows, cap, plates, flash/over-cap colours |
| `draw_market_area` @00818f20, `draw_market_button` @00817ba0, `on_button_clicked` @0081b570 | market |
| `draw_infra_area` @00818270, `draw_infra` @00817f30 | worker counts |
| `init` @0081ac10, `toggle_market` @00480f50, `update_visibility` @0081ba60 | profile flags, enable state |

Port notes:
- Pure data plus three draw rules: HIGHLIGHT plates, digit strings, dividers.
- The red flash needs `flash_resource_stamp` from the command layer, set when an order fails for lack of resources.

### 6.3 Selection panel (IFaceSelected, @0081bd70..00827b00)

**Window.** IFACESELECTED "main" sits in the right slot at screen (655,604). Positions below are relative to the
panel.
- `update_visibility` @008277c0 sets `multi` when more than one object is selected.
- With one object, it shows the leader (or the first building). With nothing selected, it falls back to the selected
  map good or item.

**Frame** (opened, single selection only). `update_depth` +2 per layer:
1. `back` (TEX_CIV 0,371 160×141) tiled.
2. IFACEBORDER `misc_border` around (0,18)–(129,160) and (133,18)–(292,160).
3. `misc_tile` rules: (6,14)–(290,18), (129,20)–(133,159), (292,0)–(296,158).
4. Five `misc` joint pieces.

Streamlined and multi-selection draw none of this.

**Name line**, rect (0,0)–(292,14):
- **Bar:** filled with the owner's team UNIT colour (LIGHT_GREY for goods and items).
- **Text:** 12 B, white, 1,1 black shadow, centred:
  - unit: `say_name`;
  - building: the type name, or `$BUILDINGTYPE0 -- $CITYNAME1` inside a city;
  - city centre: `$CITYNAME1 -- $BUILDINGTYPE0`.

**Owner plate:** `player_color` (TEX_CIV 386,84 126×46) in (0,18)–(126,64), modulated by the team colour, behind the
portrait. A capital adds `capital_star` (22×21) at (54,71). Tooltips: OPTION_IFACE_SELECTED_STAR (+1 for the plate).

**Portrait:** button `object` (33,22) 60×35, of type UNIT, BUILDING, GOOD or ITEM. It shows the 61×39 icon cell
(§6.4), stretched.
- **Health bar** (`IFaceButton::on_redraw` @007fc730, for object buttons only): inset 3 px left, right and bottom,
  5 px tall. A black COLORBLEND background, then a fill of inner width × hp/max (small icons: 2 px). Shifted +1,+1
  when pressed.
- **Bar colour** from `ObjectData::health_level` @006534b0: ≥ 90 % GREEN; 25–90 % YELLOW; < 25 % RED.
- **Mana bar:** for casters (mana) and decoys (time left), a blue bar overlaps it by 1 px.
- **Infiltrated:** the viewer's infiltrated building gets a 3 px frame in the infiltrator's colour.
- **Click:** OPTION_OBJECT (OPTION_CENTER for goods and items).

**Hit points:** rect (30,60)–(92,73). Text only, and only with profile pref bit 0x2 and visible stats. 10 px, white,
centred: "cur/max", plus "++" when healing in a supply radius, or "+".

**Stats grid** (`draw_stat` @008214d0):
- **When:** opened mode only, and only for the viewer's own, allied or infiltrated objects, objects the viewer can
  currently see, or reveal-all.
- **Cells:** `stat_short` 63×17, columns at x 1 / 65, rows at y 94, 111, 128, 145.
- **Fill:** stats are tried in a fixed order and each takes the next free cell, row-major. When all 8 are taken the
  rest are dropped. Order: decoy, attack, antiair, armor, range, buildings, airbase, splash, garrison, power, craft,
  entrench, cloak, blowncloak, infiltrated, in_supply, out_of_supply, attrition, radius.
- **Craft** takes a whole `stat_long` row (105×17) when both cells are free.
- **Cell art:** left column `stat_left` (TEX_CIV 211,115 47×17) right-aligned with its icon at the left; right column
  `stat_right` (211,97) with its icon at the right; long row `stat_large` (298,128 87×17). Icons are 20×20 TEX_COMMON
  `stat_*`, drawn at the row top (3 px overhang).
- **Value:** centred over the background in a 10 px font, white. There is no colour change for upgrades; bonuses are
  written as "base+bonus".
- **Values:**

  | Stat | Value |
  |---|---|
  | attack / antiair | base/10 + bonus/10 |
  | range | max, or min→max |
  | garrison | inside/limit |
  | power | arrows/shots |
  | airbase | here/limit |
  | buildings | kinds/needed |
  | attrition | 512/attrition, clamped to 1..99 |
  | cloak | "Hide" / "Rev." |
  | infiltrated | "*" |

- **Tooltip:** OPTION_UNIT_INFO 192 with the stat number.

**Foreign owner** (not for the viewer's own; only when there is no progress bar):
- `draw_player` (139,23)–(289,45): the leader name (12 px, white), then a `status` icon (34×19, TEX_COMMON
  256/291/326,0): alliance / peace / war. The icon's spot shows OPTION_DIPLO_STATUS_INFO; clicking it declares war.
- `draw_tribe` (139,98)–(289,115): the nation name (tag `unit_info`). An infiltrated object shows an informer label in the
  infiltrator's colour instead, blinking.

**Text area** (139,125)–(289,158):
- Units: word-wrapped 12 B status lines (current action, trade route income, strengths …).
- Buildings: gatherer heads (`draw_heads`) with "(+rate/rate)", construction percentage, spawn timers, a jammed notice in RED.

**Progress** (`update_progress` @00824d20, `setup_progress_bar_and_button` @00825c60, `draw_progress` @00823080):
- **Text:** the research / construction status with percentage and so on, bottom-aligned at the left of (139,26)–(292,35).
  Colour 0xFFCFCFCF with a black shadow, 12 px; drops to 10 px, then wraps, if wider than 153 px.
- **Warnings:** population / caravan / airbase / silo limit warnings (translated_strings) blink in RED.
- **Bar:** (139,79) 135×9, w = 135·percent. `progress_bar0` (TEX_COMMON 308,236 153×9) covers the first w pixels and
  `progress_bar1` (308,246) the rest, sampled from texel w. Both ALPHA.
- **Progress button:** the first `queue` button (61×39) at (139, 37 + wrap offset). It shows the item being made;
  clicking it cancels the build or spell.

**Queue** (`update_queue` @00824200), for own, allied or infiltrated buildings. Libraries show the shared library
queue.
- Ten 30×32 buttons at x 136+31i, rows y 92 and 125. Only one row when the progress text wraps.
- Each button's id is its queue slot. Click → OPTION_QUEUE 166 (removes the item).
- Identical neighbours merge into one button with a count (digit set 1, bottom-right) when the queue is longer than
  the slots.
- The in-progress item gets a translucent GREEN `percent_done` overlay (alpha 0x80 over the left part).
- **Infinite toggle:** `infinite_queue_on/off` (TEX_COMMON 463,42 / 448,42, 14×14) at (277,80). It blinks to "off"
  while on. Click → OPTION_INFINITE 64.
- `update_carrier_queue` @00824a80 shows one button per queued plane on carriers.
- `disband` (101,55) 21×22 appears only for an own Library or Market → OPTION_DISBAND.

**Streamlined:** only progress and queue are drawn:

| Element | Position | Size |
|---|---|---|
| Current-item button | (38,125) | 61×39 |
| Bar | (103,150) | |
| Text | (103,130) | |
| Queue | (258+33i, 130) | |
| Infinite toggle | (241,150) | |

`check_mouse_accept` passes clicks through except over the queue.

**Multi-selection** (`update_selected` @00823790):
- **Buttons:** pooled IFaceButtons on a grid of 60×35 cells with 2 px spacing: nx = (W+2)/62, ny = (H+2)/37.
- **Group name:** a named control group puts its name in row 0 (Georgia 18 B, white, shadowed).
- **Shrink:** if count > nx·ny, take the smallest k with k²·nx·ny ≥ count, divide cells by k, and use small icons
  (30×19).
- **Group by type:** with pref 0x20000 there is one full-size button per type, with a count.
- **Fill:** column-major. Each button gets a health bar.
- **Click:** OPTION_OBJECT (index into the selection).
- **Background:** none for the panel. Buttons draw grey 0x80 (0xA4 on hover, 0x40 when disabled) with a black 1 px box.

**Garrison panels.**
- `IFaceGarrison` (`update_visibility` @00806170):
  - Sits right of the selected panel, with 50×30 cells and 2 px spacing; the same shrink and group rules apply.
  - Fills column-major when opened, and from the bottom-right when streamlined.
  - With several owners, each button is outlined in its owner's colour (`on_button_redraw` @00805cc0).
  - Click → OPTION_EJECT 170. Right click ejects all of that type.
- `IFaceAirGarrison`: 3 rows (fighter, bomber, helicopter) × 2 columns (ready / fueling), with `meter` headers
  (`small_meter_*` when narrow). Clicks run OPTION_FIGHTERS / BOMBERS / HELIS / ALLAIR.

| Function @address | Shows / does |
|---|---|
| `on_redraw` @00826420 | frame, stat packing, dispatch |
| `draw_name` @008205a0, `draw_unit_name` @00820ff0, `draw_build_name` @00820c30 | name bar, plate, star |
| `draw_player` @0081c2e0, `draw_tribe` @0081c570, `draw_hitpoints` @00821150, `draw_stat` @008214d0 | owner, hp text, stats |
| `draw_unit_text` @0081c9a0, `draw_build_text` @0081f0c0 | text area |
| `draw_progress` @00823080, `update_progress` @00824d20, `setup_progress_bar_and_button` @00825c60 | progress |
| `update_queue` @00824200, `update_carrier_queue` @00824a80 | queue, infinite toggle |
| `update_selected` @00823790, `draw_group_name` @00820b30 | portrait, multi grid |
| `on_button_clicked` @00826e00, `on_button_move` @00827210, `on_spot_move` @008276a0 | options, tooltips |
| `IFaceGarrison::*` @00805960.., `IFaceAirGarrison::*` @007fa9c0.. | garrison grids |

Port notes:
- Visibility and grid packing are pure functions of selection state, so they can be unit-tested without rendering.
- Stat values are TrueType text. Only button counts use `IFaceRenderManager::write` (set 1).

### 6.4 Command card (IFaceOptions + Options)

**Two objects, one class.** `iface_options` reads IFACEOPTIONS and `iface_options_building` reads
IFACEOPTIONSBUILDINGS; `IFaceOptionsBuildings` has no code of its own. Both are 313×161 at (3,0) in the **left** slot,
so the screen origin is (3,604).

**Grid.** `init` @008176f0 reads `num_x_slots` = 5 and `num_y_slots` = 4, then collects the `butt_<x><y>` buttons.
Cells are 61×39 on a 63×41 pitch, columns at x 0/63/126/189/252.

| Layout | Row y | Extras |
|---|---|---|
| IFACEOPTIONS | −1, 40, 81, 122 | none |
| IFACEOPTIONSBUILDINGS | 20, 81, 122, 122 (row 3 overlaps row 2) | header rects `units` (0,0)–(314,18) and `unit_upgrades` (0,61)–(314,79) |

**Panel drawing** (`on_redraw` @00815a50). There is no background of its own: the slot's art shows through.
1. `misc_tile` dividers (TEX_CIV 155,0 28×3 horizontal, 155,8 3×30 vertical) tiled into their rects (horizontal at
   y 38/78/119, vertical at x 60/124/187/249).
2. `misc` junction caps.
3. Building layout only: `draw_expander` headers (units / unit upgrades labels), 12 px, colour (212,212,212), shadow
   (13,22,15).
4. Every slot with no option gets `blank_button` (TEX_CIV 211,20 60×38), stretched and opaque.
5. If `options_disabled` @00817990 returns non-zero, a message is written in 18 px white with a black shadow over rows
   0–2:

   | Code | Message |
   |---|---|
   | 1 | captured, assimilation countdown m:ss |
   | 2 | stranded in contested territory |
   | 3 | stranded in enemy territory |
   | 4 | neutralised |
   | 5 | needs a nearby city |

   Only EJECT_ALL / ALARM / ALL_CLEAR / BACK_TO_WORK / GATHER / DISBAND(_ALL) remain, forced into row 3.

**Option list.** `Options` holds up to 100 entries `{option, object, count, disable, grid_x, grid_y}`, plus `mode` and
`mode_data`.
- `add` @0072c290 merges entries with the same (option, object): the count goes up, and the entry is enabled if any
  contributor allows it.
- `SelectGroup::make_options` @00716320 calls each selected object's `make_options`: generals first, then military,
  then the rest.
- For buildings, `purge` @0072bfd0 keeps only options common to all of them.
- **Placement** (`setup_selected` @008165b0):
  1. Explicit (x,y) first; a slot clash drops the later option silently, and out-of-grid raises an error.
  2. Then x < 0 with a row: the first free slot in that row or below.
  3. Then fully automatic, in reading order.
  4. Unused buttons are hidden and drawn as `blank_button`.
- For another player's selection all buttons are disabled, except EJECT_ALL for team-mates.

**Unit orders** (`UnitOut::make_options` @00615fc0; "A" = only with the Advanced Menu pref bit 0x2):

| Row | Options |
|---|---|
| 0 | MOVE_TO (0,0), ATTACK_TO (1,0), ADVANCE (2,0) A, GUARD (3,0) A, PATROL (4,0) A; citizens ALARM_PEASANT (4,0). Aircraft: PATROL/AIRSTRIKE, ATTACK_GROUND, TRANSFER, RETURN/CANCEL |
| 1 | citizens BUILD (0,1), BUILD_MILITARY (1,1), BUILD_WONDER (2,1), REPAIR (3,1); SPELL at the spell's GRID_X; ATTACK_GROUND (0,1) A; carrier AIRSTRIKE; DISEMBARK (4,1) |
| 2 | AUTO_EXPLORE (0,2), TRANSPORT (1,2), STANCE (2,2) A, FORM (3,2) A |
| 3 | HALT (0,3), GARRISON (1,3), EJECT_ALL (0,3) A, carrier REPEAT/RECALL, DISBAND (4,3) A |

While targeting, only CANCEL (4,3) is shown.

**Building options** (`BuildOut::make_options` @00629ed0):
- TRAIN for each unit at its rules (GRID_X, GRID_Y). Unit rules use row 0, so on military buildings row 0 is units.
- The unit line's next upgrade as RESEARCH at (GRID_X, GRID_Y+1).
- Techs at their GRID_X/GRID_Y.
- Cities: RENAME_CITY (3,0), ALARM/ALL_CLEAR (4,0), CITY_*_GATHER (0..3,2) A.
- BACK_TO_WORK (1,3), EJECT_ALL (2,3), GATHER/CLEAR_GATHER (3,3), building STANCE (0,3) A, DISBAND (4,3) A.
- Airbase SCRAMBLE/REPEAT/RECALL.
- `WallOut::make_options` @00642470 puts DISBAND at (5,0), outside the grid; it raises the error.

**Build menu.** BUILD_OPTIONS mode, `set_mode` @0072bca0.
- BUILD / BUILD_MILITARY / BUILD_WONDER open page 0 civic / 1 military / 2 wonders (`mode_data`).
- Each eligible building type sits at its GRID_X/GRID_Y; the page comes from building-type field +0x14. It is
  disabled when unaffordable, unavailable, or at the city limit.
- BACK (2,3), NEXT (3,3) (page ±1 mod 3, or mod 2 when wonders are off), CANCEL (4,3).
- Choosing a building starts placement (§4.5).

**Button setup** (`setup_button` @00817010):
- **Type:** OPTION, or UNIT / BUILDING / TECH2 when the object is a type (50–413 / 414–542 / 544–628).
- **STANCE / FORM:** EXPLICIT buttons showing the group's most common stance or formation, the n-th IFACETEX of the
  layout's `combat`/`worker`/`caster`/`packer`/`form` lists (TEX_ORDERS_0).
- **Colour:** WHITE, or **ORANGE** (255,160,0) for "on/mixed": mixed stance or formation, AUTO_EXPLORE on, REPEAT on,
  INFINITE on, auto-transport on.
- **Research badge:** flags 0x10 for RESEARCH / UPGRADE.

**Button drawing** (`IFaceButton::on_redraw` @007fc730; kinds from IFACEBUTTONCLASS via `init_data` @007fbbe0).

*Icon source.* The base rect is `data[type].tex` (61×39). The atlas is the item's TEX_ID packet:
- OPTION: the `options.xml` entry for the OptionIndex.
- Building, tech, age: the rules TEX_ID/TEX_COL/TEX_ROW.
- Unit: `unit_graphics.xml` `[player][set]`, where set = owner age 0–2 → 0, 3–4 → 1, 5+ → 2.

*Cell math.* If tex_x and tex_y are ≥ 0, the piece's top-left is (tex_x, tex_y). Otherwise:
- **left = 62·col, top = 40·row, size 61×39**;
- small icons: 31·col, 20·row, 30×19.

Tech and age pieces wider than the button are clipped left, centre or right by `clip`. TEX_NONE or no icon gives a
text button.

*States.* Held down or disabled shifts the source by (−1,−1). The icon quad is stretched to the button with render
type **HIGHLIGHT** (×2), colour = button colour × factor:

| State | Factor (×2 brightness) |
|---|---|
| disabled | 0x40 (×0.5) |
| held | 0x80 (×1.0) |
| hovered / latched | 0xa4 (×1.29) |
| flag 0x40 | warm (0xc2, 0xc2, 0xae) |
| normal | 0x80 |

*Layers drawn on top, in order:*
1. Frame overlay: `overlay_tex0` / `def_overlay_texs0` (TEX_COMMON 364,0 61×39) normally; `…1` (364,40) when held or
   disabled. ALPHA, same colour.
2. Badges, last: research icon (TEX_COMMON 326,80 17×23) at (w−19, h−25); a count in digit set 1, white, right-aligned
   at (w−2, h−digit−2).
3. Progress fill: an ALPHA `percent_color` with alpha 0x80, or a bar with flag 0x2. Not used on the card.

*Text button.* The NONE piece (TEX_COMMON 350,185 79×50) tiled with HIGHLIGHT grey 0x80/0xa4/0x40, a bevel box, and the
option name wrapped to w−6 with a black shadow.

*Not drawn on buttons:* hotkey letters and costs. They appear only in the tooltip (§6.5). `infinity_tex` is never
loaded.

**Input.**
- **Left click:** `exec_index` @007187f0 → `exec` @007188c0. TRAIN/RESEARCH/UPGRADE call `issue_queue_up` (Shift ×5
  on TRAIN).
- **Right click:** only STANCE/FORM, which step backwards (STANCE_BACK / FORM_BACK).
- **Hover:** MouseOver type 2, option index (`on_button_move` @00816570).

| Function @address | Shows / does |
|---|---|
| `IFaceOptions::init` @008176f0, `on_redraw` @00815a50, `options_disabled` @00817990 | grid, dividers, headers, blanks, disabled message |
| `update_visibility` @00816df0, `setup_selected` @008165b0, `setup_button` @00817010 | placement passes, colours, badges |
| `on_button_clicked` @008164f0, `on_button_right_clicked` @008164a0, `on_button_move` @00816570 | input |
| `SelectGroup::make_options` @00716320, `Options::add/purge/grid/member/set_mode/exec` | list and modes |
| `UnitOut::make_options` @00615fc0, `BuildOut::make_options` @00629ed0, `WallOut::make_options` @00642470 | option sets and slots |
| `IFaceButton::on_redraw` @007fc730, `init_data` @007fbbe0, `GroupData::get_stance_option` @0070bab0 | button drawing |

Port notes:
- Rebuild the `Options` list whenever the selection or mode changes, then run the three placement passes.
- Draw the icon (×2 modulate), then the alpha frame, then the badges.

### 6.5 Tooltips (mouse-over help)

**Where.** `Scene::draw_mouseover_text` @008b9500 runs `prepare_mouseover_text` @008b9520, then (with no modal
window) `render_mouseover_text` @008bda80. It draws post-tonemap at `sort_depths[8]`. The box is anchored
**bottom-left, above the HUD**:
- x = 10;
- bottom = screen height − 4 − 198 when the HUD is opened or the selection needs the interface, otherwise screen
  height − 4; 40 px higher in observer mode.
- Width = the widest line, capped at `help_box_width` = screen width / 2 (`Scene::init`).
- Background: `draw_scroll`, the civ-skinned IFACERENDERMANAGER `scroll` 9-slice (TEX_CIV, corners 4×4, padding 8/4),
  tinted BLACK.
- When a modal window is open, `HelpBox::adjust_rect` places it instead.

**Content,** top to bottom:
1. `info_first`, the title, plus the cost and hotkey strings if they fit within `help_box_width` − 5.
2. Otherwise a second line with the cost and hotkey.
3. A blank line (verbosity ≥ 2), then `info_text`, a StringList of lines.
4. `info_last`.

**Cost string.** Built from `resources_req` (`Scene::set_good_required`, called by `Options::describe` @00724460):
- " (" then, at verbosity ≥ 2, "Cost: ".
- Each good as `{*1#ICONn amount}` when affordable or `{*2…}` when not. Verbosity 3 adds the good's name.
- ", " between goods.
- An optional discounts group in slot 3 (verbosity ≥ 2: the discounting tech and building), then ")".
- Icon index n = good + 1 (`icon_texs.xml`, §3.4).

**Hotkey string.** " (" (or " (Hotkey: " at verbosity ≥ 2), then "{key}", an optional suffix (a context qualifier), then ")".

**Markup** (`Buffer::write_multi_font_raw_l` @0050f390, `text_width` @0050ef70). This applies to all Buffer text,
including help.xml and the message window:
- `{` switches to slot 1;
- `{*n` (n = 0…5) switches to slot n;
- `[` switches to slot 2;
- `}` and `]` go back to slot 0;
- doubled characters are literals.

A slot selects the Buffer's font[n] and text_color[n]. Tooltip slots, each with a 1,1 BLACK shadow:

| Slot | Font tag (1024 / 1280) | Colour |
|---|---|---|
| 0 | `helpbox_size1` 12B / 14B | (190,190,190) |
| 1 | `helpbox_size2` 12B / 14B | WHITE (titles, affordable) |
| 2 | `helpbox_size3` 12B / 14B | RED (unaffordable) |
| 3 | `helpbox_size4` 12BI / 14BI | WHITE (discounts) |
| 4 | `helpbox_size5` 12BI / 14BI | DARK_GREEN |

**Delay** (`MouseOver::describe` @007d09b0):
- The text is rebuilt when the hovered thing is unchanged (`same_as_before`) or the mouse has been still for more than
  2 ticks.
- Objects wait 3 ticks after the hover starts.
- So tooltips appear after ≈ 130–200 ms. There is no long timer.
- Nothing shows while the camera scrolls or when profile verbosity is 0 (levels 1–3 add detail).

**Sources.**
- Buttons: `MouseOver::set_option` @007d0830 → `Options::describe`, which calls `add_info_first`, `set_good_required`,
  `add_info_last` and `set_help_type` @008be140.
- Help keys (type 0x4000): `Scene::load_help_key` @008bd7f0 looks up `help.xml` ENTRY `key`, then `key`+`LV`
  (verbosity < 2) or `key`+`MV`, then `key`+`NV` (verbosity < 3) or `key`+`V`, and appends all of them. Entries use
  `{TITLE}: body` markup.
- Other sources: map objects OPTION_MOUSEOVER_OBJECT; panels OPTION_STATX / OPTION_GOOD / OPTION_QUEUE /
  OPTION_UNIT_INFO; land OPTION_LAND; borders `describe_border_info` @007d2420. Script messages are appended until they
  expire.

| Function @address | Shows / does |
|---|---|
| `Scene::prepare_mouseover_text` @008b9520 | line assembly, cost/hotkey strings, fitting |
| `Scene::render_mouseover_text` @008bda80 | box position, fonts, colours, wrap |
| `MouseOver::describe` @007d09b0, `set_option` @007d0830 | refresh/delay rules, dispatch to `Options::describe` |
| `Scene::load_help_key` @008bd7f0, `set_good_required` @008be090, `Scene::init` @008c16e0 | help.xml keys, cost list, colours and width |
| `Buffer::write_multi_font_raw_l` @0050f390 | slot markup |

Port notes:
- Implement the slot markup once in the text layer.
- Rebuild the tooltip every frame from (option, index), as the original does.

### 6.6 Minimap (IFaceMiniMap + WorldMap, `worldmap.obj`)

**Window.** IFaceMiniMap is 332×196 in the mid slot; `WorldMap` is a child on rect `map` (10,26) 317×159, i.e. screen
(326,597) at 1024.
- `on_redraw` @008152e0 draws only the frame `back` (TEX_CIV 173,315 339×197, civ-skinned) at native size, ALPHA, while
  the HUD is open. Its alpha has a diamond hole over the map.
- Buttons, no filters:

  | Button | Option |
  |---|---|
  | `cycle_military` 47×47 | 97 |
  | `cycle_general` 32×32 | 98 |
  | `objectives` 32×32 | 93 |
  | `cycle_peasant` 47×47 | 95, disabled without idle citizens (leader flag 0x80000) |

- Filtering follows the **Stats panel tab** (`player_profile.prefs.stat_mode`: 0 Normal, 1 Military, 2 Economy …
  8 Wonders).

**Layers** (`WorldMap::on_redraw` @008c81f0, gated by pref `EnableMinimap`), in order:
1. background;
2. territory;
3. fog;
4. blips;
5. view rect;
6. pings.

They go straight to `GSGraphicCard` as pre-transformed triangles during `Window::update_screen`, with z = the window's
UI depth, not through IFaceRenderManager. The frame quad, flushed later, fails LESSEQUAL wherever the map was drawn.

**Mapping.** `init_camera` @008cbb10 sets an ortho, top-down camera with yaw 45° (like the main view), giving a 2:1
diamond in the 317×159 rect. World (0,0) is the top corner, (W,0) right, (W,H) bottom, (0,H) left. With u = x − W/2 and
v = y − H/2 (square maps):
- px = w/2·(1 + 1.003·(u − v)/W)
- py = h/2·(1 + 1.003·(u + v)/W)

`MiscAccess::world_to_console` @00583840 rounds half up. `add_blip` uses integer halves and no rounding.

**Background** (`WorldMapBackground::render` @008cfc40). Rendered once per map into **256×256** (`prep_buffers`
@008d3870 falls back to 64):
- Camera: a temporary ortho camera, yaw 0, covering [0,W]². NO_HDR shader path; LOD level 5.
- Passes, in order:
  1. Clear to BLACK.
  2. Ocean gradient quad: tileset `minimap_ocean` at corner (0,0) to `minimap_ocean_bottom` at the opposite corner.
  3. `TerrainOut::render(1)`, mountains, cliffs.
  4. Feature overlays (`render_feature` @008cf710) with per-vertex colours on the 5×5 meshes:
     - trees `tree_minimap_color` 0x425542, opaque, corners skipped;
     - rivers 0x576870;
     - Snowy only: a multiply darken by 0xD4D4D4;
     - mountains 0x857F69 (Snowy 0xD2C9A6), α 0x77, additive.
  5. Copy to the texture with alpha forced to 255.
- On screen: one quad over the diamond, UVs aligned with the world axes, opaque.

**Territory** (`render_borders` @008cc490, `WorldMapFog::fill_colors_who` @008d1070):
- A fog-resolution texture (one texel per half-wcoord) holding the owner's `TeamColor::get_border` colour, for owners
  the viewer can see.
- Alpha 0x50 inside, 0xC8 on edges (any of the 8 neighbours unowned or non-allied; water ignored), 0 when unowned.
- Slot colours: red (255,0,0), blue (0,0,255), pink (188,0,250), green (0,255,0), yellow (255,255,0), cyan (0,255,255),
  white, orange (255,128,0). The relation-colours pref (0x100) uses self BLUE, ally CYAN, neutral WHITE, enemy RED
  instead.
- Alpha test ref 0 (fill visible) on the Normal tab or with the Stats panel hidden; 100 (edges only) otherwise.
- **Fog** multiplies next (ZERO/SRCCOLOR; fog-spec §9.4). It is skipped when the map is revealed.

**Blips** (`render_blips` @008cc700, `add_blip` @008ce500):
- Atlas: `art/blipmap.tga` 32×16 (effects_graphics EFFECTS/MINIMAP `BLIPMAP`). Sizes s = 1, 2, 3, 6, 8; each sprite is
  a black 1 px frame around a white s×s square.
- Styles:
  - 0: the sprite × fill colour; BLACK fill draws a border-colour square with a black core;
  - 1: the sprite in white;
  - 6: an (s+4)² square in the border colour plus an s² square in the fill.
- Render state: z ALWAYS.
- Five batches, drawn 0→4. Batch 4 is hidden on `graphic_tick & 8`.

| Batch | Contents |
|---|---|
| 0 | visible goodies, size 2, GOODY_COLOR (210,210,210) |
| 1 | usable resources: fish size 2 / whales size 3 in (80,160,80); oil size 3 black with a white rim; other rares size 3 in (160,160,255) |
| 2 | buildings in the owner's border colour (WHITE if selected), size 2–3; walls style 1; cities size 6; a city under capture threat also gets a white size-8 ring |
| 3 | units (every frame), size 2; citizens and scholars size 1; own scouts pulse 3/2. Units attacking (inferred) and own buildings under attack (inferred) blink |
| 4 | blinking hints: capture overlay; unworked fish when boats are selected; rares when merchants are selected; oil spots while placing a well or platform |

Unit filter by tab: 1 military only; 2 own and allied economic units (idle ones blink); 4 none; otherwise all.

**View rect** (`get_view_rect` @008c8120, `draw_view_rect` @008c7f70):
- The main view's screen corners (0,0) and (w,h) are cast onto z = 0 (`mouse_to_xy_plane`) and mapped to the
  minimap.
- A 1 px **axis-aligned WHITE box** is drawn between them, clamped 4 px inside the window. It stays axis-aligned
  because both cameras share the 45° yaw.

**Pings** (`render_ping` @008ca6d0, `init_texture_pings` @008c9960, `render_texture_pings` @008c8dc0).
- Source: the `MessageWin` event list.
- Cycle: 8 ticks, p = (8 − t mod 8)/8. Each ping lives (k+1)·duration cycles.
- Texture by event type: −10 attack, −8 wonder, −9 resource, −6 citylost, −7 citywon, −2/−3 waypoint, else `default`.
  These are 64×64 alpha masks.
- Each `PHASE` in EFFECTS/MINIMAP/PING is active for end_pct ≤ p ≤ start_pct:
  - alpha: a triangle wave when `usealpha`;
  - colour: the phase colour, or the event colour;
  - half-size 16 px, × the `usescale` term;
  - rotation (1−p)·360°·`rotate_speed`.
  - Blend SRCALPHA/INVSRCALPHA, no depth.
- Also drawn here:
  - player PING commands: a square of half-size 16·p in the owner's colour with a black shadow;
  - ping splines;
  - rally points of selected own buildings: white 2×2 dots with an "X" at the end.

**Range circles** (`draw_minimap_circle` @008c84a0, `draw_type_circle` @008c89c0):
- Shape: a 2:1 ellipse, one point every 5°, clipped to the diamond.
- Radius: building range·192 (viewer text colour); for airbases and carriers, fighter range (text colour) and bomber
  range (neon colour at half brightness); for silos, missile ranges.
- Drawn for selected own buildings and for the building being placed.

| Function @address | Shows / does |
|---|---|
| `IFaceMiniMap::on_redraw` @008152e0, `init` @00815660, `on_button_clicked` @00815390 | frame, buttons |
| `WorldMap::init` @008d2160 / @008d2cd0, `init_camera` @008cbb10, `compute_fog_destination_verts` @008cbcd0 | sizes, render states, mapping |
| `WorldMapBackground::render` @008cfc40, `setup_camera` @008d0b70, `render_feature` @008cf710 | background texture |
| `render_borders` @008cc490, `fill_colors_who` @008d1070 | territory |
| `render_blips` @008cc700, `add_blip` @008ce500 | blips |
| `get_view_rect` @008c8120, `draw_view_rect` @008c7f70 | view box |
| `render_ping` @008ca6d0, `render_texture_pings` @008c8dc0, `draw_minimap_circle` @008c84a0 | pings, circles |

Port notes:
- Render the 256² background offscreen once per map.
- Each frame, draw the layers as 2D in the UI pass using the closed-form mapping.
- Keep the integer-half blip offsets and the 66 ms blink.

### 6.7 Messages and screen text (MessageWin, `messagewin.obj`; Scene text)

There is **no fade**: lines vanish when they expire. Durations are in 66 ms ticks. `player_profile.prefs.message_length`
(ml) defaults to 5, and the standard duration is ml·45 = 225 ticks ≈ 15 s.

**Message list** (`MessageWin::on_redraw` @007ea640, MESSAGEWIN is a full-screen window).

*Position and visibility*
- Anchor: x = 10 + 50·ceil(n/8), where n is the number of rare goods owned (clears the rare column); y = resource panel
  height + 20.
- Scenario and CtW objective boxes come first and push the list down.
- Hidden while the chat console is open.
- At most the **last 10** live entries are shown.

*Text*
- Lines wrap to the running maximum of min(text width, screen width/3), with line pitch = line height.
- Font `message_size` (12 B; 14 B at 1280).
- Colour 0 = `entry.color`, colour 1 = `entry.color2`, both with a 1,1 BLACK shadow.

*Background*
- One untextured ALPHA quad, colour 0x50000000, covering all the lines with 3 px of padding.

*Review and blinking*
- `scroll_up` / `scroll_down` @007e98f0 / @007e9960 step through history 10 at a time.
- Review mode prefixes each line with its time and widens wrapping to w/2. It ends 224 ticks after the last scroll.
- Entries with ping −5 blink.

*Adding entries* (`add_message` @007e9fb0)
- Default colour LIGHT_GREY (180,180,180).
- Duration ≤ 0 means ml·45; d < −1 means (1−d)·ml·45/2.
- With a location it also calls `add_event` @007e9b70.

*Chat* (`LeaderOut::display_chat` @006d3320)
- Format "Name (Nation): {text}".
- The name is in the sender's `TeamColor::get_text` (observers AQUA (89,186,73)); the text is WHITE through `{}`.
- Lasts 450 ticks.

**Events.**
- `add_event` keeps up to 200 located events. `next_event` @007e9190 pans the camera to them.
- A new event draws a shrinking `ANNOUNCE_CIRCLE` (TRANSPARENT_WHITE, 8 ticks) on the terrain, and feeds minimap pings
  (§6.6).

**Feedback / instructions / scenario message** (top centre):
- **Feedback** (`add_feedback` @007e9ab0):
  - Font `feedback_size` (14 B), WHITE with a black shadow, on a `draw_scroll` plate, `sort_depths[9]`.
  - Width min(text, w/2) + 1, centred. Top at 45 px, or 100 px when the mini library is expanded.
  - Lasts ml·45 ticks. `feedback_color` is stored but unused.
- **Instructions** (`add_instructions` @007e9850) and **scenario messages** (`add_scen_message` @007e8990, scenario
  mode only): the same box, no shadow.
- **Script objectives:** right-aligned at x = w − 10 from y = 60, width (w − 450)/2 − 20.
- **Game over:** the game-over line (with the menu hotkey substituted) in `game_over_message_size` (18 B), centred under the resource
  panel.

**Objective boxes.**
- `draw_scen_objectives` @007e75b0: a scroll plate at the anchor, 12 B white, width ≤ w/3. Check boxes `check_off` /
  `check_on` (24×21, ALPHA2); completed ones wipe in over 15 ticks and are removed 30 ticks later.
- `draw_ctw_objectives` @007e6aa0: the `ctw_objectives` icon (63×66, TEX_WRAPPER_5) toggles a box with an
  `objectives_close` button.

**Text bubbles** (`add_bubble_message` @007e7e20, `TextBubble::draw_update` @007e6820):
- Colour: neon team colour.
- Lifetime ≈ 120 ticks.
- Grow over 12 ticks: scale 0.2→1, alpha 128→255. The rect pops up to 50 px and settles. Fades out at the end.
- Rect ±100 × (−70..+10) px around the projected point, font `big_huge_size` (22 B) scaled, `sort_depths[6]`.

**Scene text.**
- **City names:** `BuildOut::draw_overlays` @0062cb30, pref 0x40000. Font `onscreen_mouseover_size` (12 B), white with a
  shadow, on a scroll plate above the city; capitals get the star in the team colour.
- **Status box** (waiting-for-player, `Scene::set_status_message` @008ba2d0, drawn in `draw_overlays`
  @008b4840): `paused_size` (22 B), centred on the screen on a scroll plate, with an optional white progress bar.
- **Signposts:** `Signposts::draw_all` @00897bd0, zoom 0 only, centred white labels.
- **Developer only:** `draw_game_stats` @008b9e70 (the FPS line, cheat key Alt+Shift+F) and the `control_group_size`
  labels. There are **no control-group numbers over units**. `draw_help_text` @008b3260 is an empty stub.

| Function @address | Shows / does |
|---|---|
| `MessageWin::on_redraw` @007ea640, `add_message` @007e9fb0, `scroll_up/down` @007e98f0/@007e9960 | message list |
| `add_event` @007e9b70, `next_event` @007e9190 | located events, ping source |
| `add_feedback` @007e9ab0, `add_instructions` @007e9850, `add_scen_message` @007e8990 | top-centre text |
| `draw_scen_objectives` @007e75b0, `draw_ctw_objectives` @007e6aa0 | objectives |
| `add_bubble_message` @007e7e20, `TextBubble::draw_update` @007e6820 | bubbles |
| `LeaderOut::display_chat` @006d3320 | chat format and colours |
| `Scene::draw_text` @008b7960, `draw_overlays` @008b4840, `BuildOut::draw_overlays` @0062cb30 | screen text |

Port notes:
- No fade, one translucent quad, every box a scroll plate.
- Bubbles and pings need world→screen projection.
- Skip the developer overlays.

### 6.8 Library and mini library

**IFaceLibrary** (@00806960..00809000; IFACELIBRARY 319×168). It replaces the command card for a finished Library.

*Grid*
- Column 0: category buttons, `cat_age` (34×38) and four `cat` (34×28: military, civic, commerce, science), with
  overlays `age_overlay0/1` and `cat_overlay0/1`.
- Then 7 age columns (CLASSICAL 544 … INFORMATION 550) at x = 45 + 39·i. Each has an `age` button (36×36) over
  `age_underlay`, and `tech0..3` (36×28), one per category.
- `age_progress` (12×35) and `tech_progress` (12×28) bars mark items being researched. `blank_button` fills empty
  cells.

*Behaviour*
- Click → OPTION_RESEARCH (`on_button_clicked` @00807930).
- Enable state comes from `update_visibility` @008081e0.

*Final techs* ("jeopardy" view)
- `jep_back0` (TEX_FINAL_TECHS `art/iface_final_jeopardy.tga` 319×168) with four 74×157 `final_jep` buttons.
- Pieces: `jep_tech_button`, `jep_star`, `jep_overlay[_complete]` 69×69, and `jep_bar` 60×13 with `jep_bar_fill` 54×7
  progress (`on_button_redraw` @00807b00).

**IFaceMiniLibrary** (@00813860..00815200). Top level at (399,25) 227×51, top centre under the caption.

*Contents*
- Background `back` (TEX_LIBRARY_BACK 227×42).
- Buttons: `military_button` (4,1), `civic_button` (44,1), `age_button` (84,1) 59×37, `commerce_button` (147,1),
  `science_button` (187,1). The four category buttons are 36×28. Each shows the next research, or the next age, with
  progress bars.
- Ticks: under each category a row of 2×5 `<cat>_tick` pieces at `<cat>_tick_rect`, one per level reached
  (`LeaderDataEncrypt.epoch[cat]`), 5 px apart (`on_redraw` @00814210).

*Minimising*
- `toggle` @00813860 hides everything except the 99×11 `toggle` tab: `min_button_tex` at (64,0), or `max_button_tex`
  at (64,40) when expanded.
- The state is persisted in `player_profile.mini_lib_open`, and moves the feedback line (§6.7).

| Function @address | Shows / does |
|---|---|
| `IFaceLibrary::setup_buttons` @00806d60, `update_visibility` @008081e0, `on_button_redraw` @00807b00, `draw_button_underlays` @00806960 | grid, overlays, final techs |
| `IFaceMiniLibrary::on_redraw` @00814210, `toggle` @00813860, `update_visibility` @008145a0, `on_button_redraw` @00813f90 | ticks, minimise, buttons |

Port notes: both are grids of standard IFaceButtons (§6.4) over fixed backgrounds. The per-skin `TEX_LIBRARY_BACK(2)`
swap is in §1.1.

### 6.9 Open questions

1. **Text markup.** Does `{`/`[` change the font as well as the colour slot everywhere? Confirmed for
   `write_multi_font_raw_l`, not for every Buffer path. Do `write_cent_l` (rect form) and `Buffer::wrap` centre
   vertically, and do they use the top or the baseline as y? This affects box alignment.
2. **Top strip at other widths.** `cap_area`/`pop_cap` etc. are right-anchored, but the caption is a fixed 420 px and
   the mini library sits at a fixed x = 399. Check overlaps at 800- and 1920-wide screens.
3. **Command card rows.** IFACEOPTIONSBUILDINGS row 3 sits on row 2. Can shipped data fill both? (Explicit slot
   clashes drop the later option, e.g. HALT vs EJECT_ALL at (0,3).)
4. **Command card hover.** What is `Button.passover_delay` (hover brighten delay), and who sets IFaceButton flags 0x40
   (the warm tint)? Do the IFACEBUTTON_AGE `overlay0/1` names (vs `overlay_tex0/1`) mean age overlays never load?
5. **Garrison width at 1024.** The garrison panel is only ≈ 42 px wide there. Confirm in game that it shrinks icons
   (k-shrink) rather than overflowing.
6. **Minimap.** Red-low `Color.rgb` byte order in the territory texture; non-square maps (UVs vs the W×W camera);
   whether the stat-tab blip filter applies with the Stats panel closed (it does in code).
7. **Inferred meanings.** Several are unconfirmed: the attacking/under-attack blink flags (order vslot 0x18, building
   +0x60 & 0x20); the building page category (+0x14) and vfunc +0x1c; type 443 in the research-badge test;
   `is(0x1a4)` queue merging; disband only on Library/Market.
8. **Fonts.** Which face do `get(10, any)` (stat values) and the misspelled `"info_size "` signpost tag resolve to? If
   the latter falls back to the first 12 px entry (Arial 12 OB), signposts would render empty (§10 Q3).
9. **Never-called font tags.** `gamestat_size`, `city_name_size` and `stat_size` are never requested by name. Are they
   leftovers? No code draws control-group numbers over units.
10. **AnimNotice end rect.** It lies entirely off-screen (top-left at (w,h)). Confirm the "fly off the corner" look in
    game.

---

## 7. Cursors

- **`Data/cursors.xml`**: 68 `CURSOR` rows; the row index is the id for `Cursors::set(id)` (id −1 shows row 0, Main).
  Attributes `name`, `file` (a TGA in `art/`), `x y xs ys` (sub-rect; a negative x or y selects a cell of a strip of
  (size+1)-pixel cells: x = 1 − (xs+1)·x, so −1 → 33, −3 → 97, in the 128×128 `art/cursors.tga`), `hot_x hot_y`
  (0,0 on every row: the tip is the top-left pixel), `gdi_id`/`bw_id` (Windows resource ids for the GDI colour and
  black-and-white fallbacks). `cursors_uwp.xml` differs only in `gdi_id`. The `.cur` files in the install root and
  `art/SC_*.cur` are the sources of those exe resources and are not opened at run time.
- `Cursors::init` @007d0270 (from `System::init_graphics`) creates one GraphSys cursor per row (`Cursor::init`
  @00526a30: texture `PF_32` + rect + hotspot + ids); row 0 becomes `Window::def_cursor`. `Cursors::set(id)` @007d06e0
  sets it on the console window; the minimap gets the same cursor only for ids 1, 5, 6, 8, 13, 14, 34–37, 40–42, 61
  (else Main).
- Who sets it: `Console::execute_at_cursor` (hover preview), `Console::on_mouse_move` / `down`, `Options::set_mode` /
  `reset` / `exec` / `picked_spot` and the targeting `Options::do_*` modes. Ids seen in code: 0 Main, 1 Gather Point,
  7 Garrison, 9 Repair, 10 Airstrike, 11 Air Transfer, 16 Build, 19/21/22 Gather Timber/Metal/Oil, 23 Ruins,
  27 NoAttack, 42 Nuke Attack, 43 No Air Transfer, 45–48 crossed-out wood/metal/oil/knowledge, 54 crossed-out
  garrison, 62 No Missile, 63 No Nuke; attack/move/spell ids are computed.
- Backend (`D3D11GLCursor`, d3dgl): `init_class` @10025c30 picks a mode: **custom** (`render` @100252a0 draws the TGA
  rect as a screen quad at mouse − hotspot, SRCALPHA/INVSRCALPHA, alpha test > 0, after all UI), GDI colour
  (`LoadCursorW(gdi_id)`), or GDI black-and-white (scalability flag). `set` @10025150 → `set_custom_cursor` /
  `set_stock_cursor`.
- **Port**: build an `NSCursor` per row from the cropped RGBA (hotspot from the XML) and set it on the Metal view
  (hardware cursor, no lag), or draw it as the last quad of the frame. `node re/tools/cursors.mjs` lists the table;
  `--png <idx|name|file.cur>` writes `re/previews/cursors/*.png`.

---

## 8. Port plan (Metal/AppKit, `apps/map-mac`)

Progress and port decisions (font backend, Retina policy, draw order) are in [ui-port.md](ui-port.md).

Goal of the first milestone: **a playable loop** (select, see resources, build, train, move the camera from the
minimap) with the original look at 1024×768 and wider. Each step is checked by `--snapshot` PNGs at 1024×768 and
1280×800 and, where possible, against the reference tools.

**U0. Foundations (no gameplay dependency)**

1. `engine/ui/layout`: load `interface.xml` with the existing `engine/assets/xml` reader (its lower-cased attribute
   names already give the case-insensitivity of §2.1; element names must be compared case-insensitively too), resolve
   rects exactly as §2.3 (port `resolveRect` and POSITIONING from `ifacelayout.mjs`), keep per-window name → rect /
   piece / border / point / colour tables in file order (the n-th rect pairs with the n-th piece). Parse
   `interface.txt` with the existing `GraphicsTxt` reader (same format; strip the trailing words of numeric items).
   A CLI check (`ron-ui-check`) resolves every window at 1024×768 and 1920×1080 and loads every `IFACETEXLOAD`.
2. `engine/ui/strings`: internal/translated/string_lookup/typenames tables with the language suffix rule and the String
   hash (§3.5); a language setting replaces Steam's.
3. `platform/metal/ui`: one UI pass after `overlay_pass`, into the same LDR drawable. Screen-space quads with
   per-vertex colour and UV, batched per atlas, the 9 `IFaceRenderType` states as pipeline variants (blend factors +
   an alpha-test threshold in the fragment shader; MODULATE2X/SELECTARG variants as shader switches). Ordering: give
   the pass a depth attachment cleared to 1 and write `z = (0x1fff − update_depth)/8192` (or sort by update_depth and
   skip depth). No half-pixel offset on Metal. API mirrors §2.5: `add`, `add_smooth`, `tile_*`, `draw_border`,
   expanders, dividers, bitmap-digit `write`.
4. Text: glyph atlases of 256×256 per font, rules of §3.2 (pixel size = fonts.xml size, cell = Ascent − Descent, no
   left bearing, hinted integer advances, alpha = coverage, alpha test ≥ 8). Rasterise with **FreeType** (vendored,
   identical hinting to the original) or **CoreText** from the install's TTFs (no dependency, slightly different
   hinting); keep the layout rules either way. `fontpreview.mjs` is the reference for metrics and layout.
5. A small window manager: tree, z-order (TOP/BOTTOM/CHILD), visibility, hit testing with MOUSE_THRU, update_depth
   per window and +2 per layer; event routing so the map (`Console`) receives only what the HUD lets through.
6. Cursors: `NSCursor`s from `cursors.xml` rows (§7); the context cursor comes later from `execute_at_cursor` preview.
7. Per-civ skin swap (§1.1) when the console player's age or nation changes.

**U1. Playable loop (needs the production/economy ports for live numbers; draw zeros until then)**

8. HUD frame: `IFACEMAINBASE` bar (top strip buttons as stubs), left/mid/right windows, opened/streamlined layout.
9. Resource panel (`IFACERESOURCES`): six rows (icon, stockpile digits, "/", +rate), cap and flash colours; market and
   infrastructure sections collapsed initially.
10. Selection panel (`IFACESELECTED`): name, owner, portrait icon (atlas cell from `unit_graphics.xml` /
    buildingrules `TEX_*`), hit points, stat rows, multi-selection; production queue + progress bar for buildings.
11. Command card (`IFACEOPTIONS`, 5×4 grid): citizens → build pages (buildings by `GRID_X/GRID_Y`), buildings → train
    and research buttons by `GRID_X/GRID_Y`, unit orders (halt, stance, formation, garrison, disband). Clicks call the
    same `Options::exec` paths the hotkeys use; right click on a train button removes from the queue.
12. Building placement (§4.5): Template Blend footprint colours, ghost with Blocked Building Blend, Ctrl/Shift
    multi-place, `issue_build`.
13. Minimap (`IFACEMINIMAP` + WorldMap): background texture, fog multiply (fog-spec §9.4), territory, blips, white view
    rectangle; left click/drag moves the camera, right click orders.
14. Input completion (§4): click/drag thresholds, Shift/Ctrl/Alt selection rules, double click same type on screen,
    box filters, right-click context order, formation drag preview, control groups (HOTKEY command into the
    simulation), edge scroll and zoom-to-cursor, the key map. The default key table can be read at start-up from the
    user's `riseofnations.exe` exactly as `keymap.mjs` does (or written out by hand from its output).
15. Top-strip game clock and pop cap; pause/menu buttons.

**U2. Later**: tooltips with costs (help.xml, `MouseOver::describe`), messages and notices (`MessageWin`,
`AnimNotice`), mini-library / library (ages and research), market, garrison panels, diplomacy, stats window, chat,
front-end menus (`topmenu.xml`, `setupwin.xml`, `optionswin.xml` use the same layout engine), hotkey rebinding saved to
our own profile file.

---

## 9. Evidence index

Functions not already listed in the tables above.

| Area | Function @ address |
|---|---|
| Layout | `GraphicWinXML::process_element` @005145a0, `resize_element` @00513a30, `add_elements` @00516700, `resize_elements` @00516270, `draw_basics` @00518cf0 … `draw_tiled_textures` @005193e0; `GraphicWin::load_style` @00519f20, `process_element` @0051a9e0; `Window::get_common_attribs` @00a55d40; `XMLElement::get_attrib_rect_ex` @00a25ba0 |
| UI drawing | `IFaceRenderManager::init` @0051e6c0, `setup_render_states` @0051e170, `setup_text` @0051dd50, `load_tex` @00520500, `add_smooth` @0051b700, `draw_border` @0051d260, `tile_*` @0051f060–0051f2e0, `write` @0051f660, `char_to_index` @0051f4a0, `flush_all` @0051fbe0, `clear_all` @0051f8b0; `IFaceTex::get` @0051fed0, `IFaceBorder::get` @0051ff80 |
| Frame order | `Game::loop_render` @005917e0, `Scene::render` @008b3270, `BaseScene::render_end` @00538c40, `Window::update_screen` @00a54460, `Scene::post_update_screen` @008b3d00 |
| Skins | `IFaceMainBase::age_update` @00810b10 |
| Fonts | `FontFamily::init` @00521330, `add` @00520e40, `string_to_style` @00521260, `get` @00522050, `flush_fonts` @005228e0; d3dgl `FT::FontFace::FontFace` @10009990, `SetupFace` @10009c00, `RenderGlyph` @10009e40, `FT::GlyphData` @1000aa30, `D3D11GLFont::init` @10026c10, `init_class` @10027180, `get_character_info` @10029ac0, `add_character` @10029140, `write` @10027a20, `FontRenderManager::render_character_info` @1004a910, `render_pendings` @1004ad60 |
| Text | `Buffer::write_raw_l` @0050e470, `set_text_color2/3` @0050db40/0050db90, `init_icon_texs` @005112d0 |
| Strings | `Main::init_string_tables` @00595a10, `StringTable::init` @00a28520, `String::generate_hash` @00a1b6b0, `String::lookup` @00a1ddb0, `LocalizationManager::GetLanguageSuffix` @00a42830, `GetLanguageFromSteamName` @00a42ad0; tables `gLanguageSuffix` @00b1ca54, `gLanguageSteamNames` @00b1ca40, `primes` @00b14500 |
| Data structures | PDB enums `IFaceRenderType`, `OptionIndex`, `OptionMode`, `KeyMapType` (`re/analysis/types_ui.txt`, regenerate with `DumpTypes.java re/jobs/ui_types.txt`) |

Job files: `re/jobs/ui_*.txt` (decompiles land in the gitignored `re/decomp/ui*/`).

---

## 10. Open questions

1. **Depth before the UI.** The UI relies on LESSEQUAL depth with z ≥ 0.75, but nothing read here clears the back
   buffer's depth after the 3D scene (it shares the HDR target's depth). Either the backend clears it in `end_hdr` /
   the post chain, or scene geometry can occlude UI in theory. The port uses its own cleared depth (§5.3); a GPU
   capture of the original would settle it.
2. **Left bearing dropped in glyphs** (§3.2): the bitmap goes to the cell's left edge and the quad sits at the pen,
   while the advance is the plain hinted advance. This predicts slightly uneven spacing (most visible in italics).
   Confirm against a screenshot before copying it.
3. **Outline (O) and aliased (A) fonts**: `render_pendings` skips FreeType for both, leaving the cells empty, yet
   fonts.xml uses "OB" (Arial 12 at 1024, Georgia 14 at 1280). Either those fonts are never used in EE, or another
   path (the else branch sets a black/white text colour, perhaps a dropped GDI call) draws them. Check in game which
   texts use 12/14 px bold outline.
4. **CODEPAGE**: only `name="default"` is ever selected; the localised fonts.xml files carry other code pages (932, …)
   that EE does not use. Confirm with a German/French install that nothing else changes.
5. **String tables**: 202 `string_lookup.xml` keys do not hash to their English text (edited strings); the original
   then falls back to the input text. Harmless, but means a lookup by hash, not by text.
6. **Retina**: faithful rendering wants 1 UI pixel = 1 screen pixel at the logical resolution; native-resolution UI
   needs integer scaling of rects and font sizes. A product decision.
7. Input details left open by the research: the jump-table cases of `Options::exec` (e.g. `OPTION_LEFT/RIGHT/DOWN`),
   the full `execute_at_cursor` branch list (spells, aircraft, transports), middle-button + wheel (window vslot 0xe4),
   the box-select Alt+Ctrl+Shift filter (`unit_flags & 0x10`), `check_nation_hotkey` conditions, the profile
   directory, and whether a mouse-only rebinding is saved (`KeyMap::save` compares only `key`).
8. Panel-level questions are listed at the end of §6.
