# UI port: what ronmap's interface implements and how

Companion to [ui-spec.md](ui-spec.md) (the decoded original; port plan in its section 8). This file records what the
port does, the decisions that differ from or go beyond the spec, and what is left. Status: U0 done; U1 steps 8–11, 13
and most of 12 and 14 done (2026-10-12); the scripts' interface and audio (sections 8-9, 2026-10-13).

## 1. Pieces

| Where | What |
|---|---|
| `engine/assets/iface_layout.*` | interface.xml (any GraphicWinXML file): element tree, `resolve_iface_rect` (= `get_attrib_rect_ex`, ui-spec 2.3: anchors, `_inverse`, `_percent`, `center_off`, negative sizes, 16-bit truncation), POSITIONING groups flattened (`iface_layout_children`), IFACETEX pieces, 16-slot IFACEBORDERs, IFACECOLOR, IFACETEXLOAD ids; interface.txt (`InterfaceTxt`: the two bitmap digit sets and any `KEY, TEX, x, y, w, h` line); the per-civ skin code (`iface_skin_code`, ui-spec 1.1) and file swap |
| `engine/assets/strings.*` | internal / translated / string_lookup / typenames tables, `string_hash` (String::generate_hash), `lookup(text, category)`, language suffixes (.7 German, .10 Spanish, .12 French, .16 Italian) |
| `engine/assets/fonts.*` | fonts.xml (CODEPAGE "default", RESOLUTION closest to the screen width, FONT list, LOOKUP tags), `FontsXml::pick` = FontFamily::get(size, style) (exact, then bold-only difference, then nearest size), TrueType metrics as FreeType derives them (LineHeight, Ascent, Descent, FontHeight, hinted integer advances) |
| `engine/assets/cursors.*` | cursors.xml rows (strip cells for negative x / y) |
| `engine/assets/catalog.*` | unit_graphics.xml icon fields (`tex_id`, `tex_col`, `tex_row`, `tex_x`, `tex_y`) |
| `apps/ui-check` → `ron-ui-check` | `tree` / `texloads` print exactly what `re/tools/ifacelayout.mjs` prints; `fonts`, `strings`, and `check` (every window at 1024×768 and 1920×1080, every atlas in all 15 skins, digits, fonts, tables, cursors; ends with `failures: N`) |
| `platform/metal/ui.*` | `UiRenderer`: IFaceRenderManager's API (`add` = add_smooth, `add_sub`, `tile`, `border` = draw_border, `expander` / `vexpander`, `fill`, `frame`, `write_digits` = write), the 9 IFaceRenderType states plus text and a multiply state, `UiFont` glyph atlases, one encode into the post-tonemap overlay pass |
| `apps/map-mac/hud.*` | the HUD panels and their hit tests / clicks (reads a `hud::State`, never the simulation) |
| `apps/map-mac/main.mm` | fills `hud::State` from the simulation views, runs the card's commands, building placement, selection rules, input routing, control groups, edge scroll, zoom, cursors, the minimap background; fills `sim::ScriptInput` and applies the scripts' camera moves, locks and pauses (section 8) |
| `apps/map-mac/script_view.*` | the scenario scripts' presentation (section 8): consumes `sim::script_presentation`, keeps and draws the MessageWin / AnimNotice / ping / popup state |
| `platform/audio/audio.*` | AVFoundation sound layer (section 9): the scenario voice channel, effects, the JukeBox |

Check: `./build/ron-ui-check ~/Games/RiseOfNations check` → `failures: 0`; `ron-ui-check tree --all --w W --h H --file F`
is byte-identical to `node re/tools/ifacelayout.mjs tree --all ...` for interface.xml, topmenu.xml, setupwin.xml and
optionswin.xml at 800×600, 1024×768, 1280×800, 1920×1080 and 2560×1440, except where the JS tool prints raw XML entities
(`Tools &amp; Extras`): the C++ reader decodes them, as MSXML does. `ron-ui-check fonts --w 1024` matches
`fontpreview.mjs list` line for line.

## 2. Decisions

**Font backend: CoreText.** FreeType is not available with the Command Line Tools and the build must not need extra
installs, so glyphs are rasterised with CoreText from the install's own TTF files (`CGFontCreateWithDataProvider`,
8-bit coverage, no font smoothing). The *layout* does not depend on the rasteriser: metrics and advances come from the
TTF tables with FreeType's rounding (`ttf_metrics`, `ttf_advance`), so widths, line heights and cell positions equal the
original's and `fontpreview.mjs`'s. Kept from the decoded backend (ui-spec 3.2): pixel size = fonts.xml size, cell
height = FontHeight, the bitmap's ink box placed at the cell's left edge (left bearing dropped, open question 2), top at
Ascent − bitmap_top, no kerning, coverage as alpha with white RGB, alpha test ≥ 8. Difference: CoreText does not hint,
so stems can differ by a pixel from FreeType's hinted output. Outline (O) fonts are drawn as plain fonts (EE leaves them
empty; ui-spec 10 Q3) — nothing in the HUD asks for them.

**Retina: original pixels, rendered at the backing scale.** The UI is laid out in UI pixels = window points, which play
the role of the original's screen pixels (no UI scaling, ui-spec 2.4; fonts.xml's resolution is chosen from the width in
points). The pass draws them at `backingScaleFactor` drawable pixels each: atlas pieces drawn at their own size are
point-sampled, so they double crisply; stretched pieces are bilinear; glyphs are rasterised at size × scale with the 1×
layout, so text is sharp while metrics stay the original's. Snapshots render at scale 1 (`--ui-scale 2` for a Retina
check). The map keeps its own drawable-pixel camera.

**Order instead of depth.** Every quad carries the window's `update_depth` (+1 per window, +2 per layer, ui-spec 5.3).
`UiRenderer::encode` sorts quads by depth (stable; glyphs after the quads of the same depth, as `flush_fonts` runs after
`flush_all`) and draws back to front in the overlay pass, batching consecutive quads with the same atlas, state and
sampler. This is the LESSEQUAL depth test of the original without a depth attachment (and sidesteps ui-spec 10 Q1).

**Render types.** NORMAL / HIGHLIGHT opaque; ALPHA, ALPHA2, ALPHAHIGHLIGHT blend with alpha test > 0; COLORALPHA with
SRCALPHASAT; COLORBLEND = vertex colour; type 7 blend without test; ADDFB additive with texture colour; MODULATE2X
saturates. Alpha blend factors ONE / ZERO, as d3dgl.

**Simulation boundary.** The HUD reads `GameView`, `FogView`, `LeaderView` and rules types; commands go through
`CommandManager` (`issue_move_to`, `issue_halt`, `issue_build`, `issue_queue`, `issue_unqueue`, `issue_gather_point`).
The placement preview calls `snap_center_who` and `blocked_site` on the live game (no RNG draws in build_place.cpp).
Selection, control groups, build pages and targeting modes are seat-local.

## 3. What is drawn

- **Bar (IFACEMAINBASE, opened mode)**: slot backgrounds (left slot `back` + border only without a selection; right slot
  `back`), `bar` across the top 34 px, clock (`time_back` stretched into `timer_area2`, "Normal: H:MM:SS" in `clock_size`
  with a 1,1 shadow, blinking while paused), frame edges, the bar buttons' textures, the display button (3-slice + three
  arrows). Skin from the viewer's age and nation build style (`IFaceMainBase::age_update`).
- **Top strip**: caption plate (105 + 10 × 21 + 105) with "Nation - Age", border3 around the cap area and help + menu,
  pop cap ("control/pop_cap", red blink at the cap) and city cap ("cities/limit") on HIGHLIGHT plates with digit set 0.
- **Resource panel**: `adjust_rects` geometry (resources open, market and infrastructure collapsed: their bordered
  columns and vertical toggles), commerce-cap box, six rows in good_order with icon, stockpile, "/" and rate.
- **Selection panel**: single object: frame (back, misc_border, misc_tile, joints), team-colour name bar with the name
  (12 B, shadow), owner plate, portrait (icon cell ×2, frame overlay, health bar by health_level), hit points, stats
  (attack / 10, armor, range), buildings: construction or production progress bar and text, current item button with the
  green progress overlay, the rest of the queue, the infinite-queue icon. Several objects: the 60×35 grid, column-major,
  k-shrink with small cells, health bars.
- **Command card**: dividers, joints, `blank_button` in empty slots; icon buttons with the HIGHLIGHT grey factors
  (0x80, hover / latched 0xa4, disabled 0x40) and the frame overlay; research badge. Contents: unit orders (MOVE_TO,
  HALT; citizens BUILD / BUILD_MILITARY / BUILD_WONDER), the build menu pages (every building type with
  `type_avail == 4` and `cat == page`, at GRID_X / GRID_Y, disabled when `can_pay_cost` fails; BACK, NEXT, CANCEL), a
  building's TRAIN / RESEARCH buttons (`build_can_make`: 4 → train at the unit's slot, 2 → its upgrade one row down,
  techs at theirs).
- **Minimap**: frame `back`, the background texture (rendered once: terrain, water and trees, top-down 256²) on the
  2:1 diamond, fog multiplied (luminance 0xff / 0x80 / 0x50 / 0), blips (black-framed squares: units 2, citizens and
  scholars 1, buildings 3, cities 6) in team colours, the white view box, the four buttons.
- **Placement**: Template Blend footprint (green checkerboard valid, orange soft blocks, red otherwise), the ghost
  building with the normal passes or Blocked Building Blend.
- **Cursors**: NSCursors from cursors.xml (Main, Move To while targeting, Attack over an enemy with units selected).

## 4. Input (ui-spec 4)

HUD windows (slots, bar buttons, display button, cap area, caption, resource panel) take clicks; the bar strip between
buttons is MOUSE_THRU. Left click vs drag: 20 px once held 7 graphic ticks (462 ms), else 64 px; right 6 px. Click:
select, Shift adds, Ctrl or Cmd toggles; double click: every on-screen object of the same owner and type; box: units
first, buildings only when no unit is boxed, the viewer's own objects win, one owner, 128 max. Right click: move or
attack (units; Shift waypoint, Alt queue first), rally point (own buildings), cancel (placement / target). Card: MOVE_TO
targets the next left click; build buttons start placement (Ctrl / Shift keep placing). Minimap: left press / drag looks,
right click orders. Control groups Ctrl+0..9 / 0..9 / twice to centre / Shift adds / Ctrl+Shift erases; empty selection
bookmarks the camera. Edge scroll: 2 px band (sides only above the HUD top − 40), `scroll_speeds[zoom]` steps. Mouse
wheel and pinch zoom one level toward the cursor (in to 6, out to 4); trackpad scrolling pans.

## 5. Inferred or approximate (check against the game)

- Divider tees (`draw_horiz_divider` / `draw_vert_divider`): positions came in registers; placed at the bar ends,
  centred.
- The blocked ghost's diffuse is red (the decompile shows only the render state).
- Button frame overlay drawn in the button's grey (spec: "same colour"); queue buttons stretch the full 61×39 icon into
  30×32.
- Command card: ATTACK_TO, REPAIR, GARRISON, ALARM are shown disabled (no ATTACK_TO verb; the others not wired); unit
  stances, formations, auto-explore, patrol, disband are not offered yet.
- Rates and the commerce cap read 0 until the economy port exposes them (`hud_resources` in main.mm).
- The clock's speed word is the English "Normal" / "Paused" (the localised names are not looked up yet).
- Unfinished construction sites are drawn as the finished building.

## 6. Not yet

Streamlined (collapsed) HUD mode; market and infrastructure sections; mini library and library panel; tooltips
(help.xml, costs); game messages and notices other than the scripts' (MessageWin is fed by the scripts only), timers;
the territory layer of the minimap, rivers / mountains in its background; formation drag; the full key map (keymap.mjs); localised UI strings beyond the rules' names; HOTKEY command
for control groups (seat-local now).

## 8. Script UI (2026-10-13)

The scenario scripts (`engine/sim/script_ui.hpp`, sim-implementation "Scripts") are played in `ronmap`: the window
reports the local seat's interface to them every frame and draws / plays what they post.

**Input (`fillScriptInput`, before every `Game::do_frame`).**
- `attached = true`; `select_who` / `selected` = the owner and objects of the current selection (units, else buildings).
- The armed command from the card state: MOVE_TO targeting gives `option 9` and `selecting_spot`; an open build page
  gives `build_options`, `mode_data` = the page and `option` = that page's BUILD option (0 / 1 / 2); placing a building
  gives `option` = its page, `option_object` = the type and `selecting_spot`.
- The cursor tile from the ground under the mouse (-1 when the mouse is outside the window); `cursor_over_iface` = below
  the bar's top while the HUD is shown.
- `screen_w` / `screen_h` in UI pixels; `iface_rects` = every IFACERECT of IFACEMAINBASE by lower-case name
  (`left_window`, `mid_window`, `right_window`, ... after the bar's offset; the top-strip ones at the top of the
  screen), plus `minimap` and `butt_XY` (`Hud::named_rects`).
- `objectives_rect` = the objectives box of the last draw. Headless runs lay the script layers out every step, so
  `get_objectives_*` work without frames.
- Camera position, zoom and panning; `on_screen` = the tile centre at terrain height projects inside the drawable.
- `keystate` from key down / up (letters, digits, space, return, escape, tab, arrows, Shift / Ctrl / Alt as Windows
  virtual keys).
- `user_warnings` |= bit 0 when the card queues an item or starts a building the player cannot pay for
  (GroupOut::issue_queue_up / validate_build).
- `sound_state` = 0 while the scenario sound plays, else -1 (the WAV-length model). A sound started inside a frame is
  never reported finished before it was heard, and a slower simulation never waits beyond the real playback.
- `graphic_clock` = the wall clock in ms.
- `popup` = a nested event loop that draws the popup and waits for a button: the script stays suspended inside
  `do_frame`, as in `BasePop::exec`. Headless runs answer button 0.
- Multiplayer: the builtins keep the original's `semaphore byte0 & 4` guards themselves, so the input is filled the
  same way in any game; nothing in ronmap feeds the presentation back into the simulation.

**Presentation (`scriptview::View`, after every `do_frame`; drawn after the HUD).** From the decompiles in
`re/decomp/ui_hud` (MessageWin) and `re/decomp/scriptui_render` (ScreenArrowSpline, TextBubble, Scene::add_ping,
ScenarioData::draw_components, GraphicPieces::draw_highlight_circle, JukeBox, SoundGlobal, BasePop):

| What | How |
|---|---|
| Objectives (`draw_scen_objectives`) | At x = 10, y = resource panel bottom + 20 (the top of the screen while the HUD is hidden). `scenario_objectives` font (12 B), white, width at most w / 3, on the IFACERENDERMANAGER `scroll` 9-slice tinted BLACK and padded 8 / 4. Check boxes `check_off` / `check_on` (MESSAGEWIN, 24 x 21) when any entry came from `add_objective`; text-only entries are indented but unchecked. A completed entry's check wipes in over 15 graphic ticks and the entry goes 30 ticks after completion. The box is `get_objectives_*`'s rectangle. |
| Message list | Under the objectives, `message_size` (12 B): the last 10 live entries, wrapped to the running maximum of min(width, w / 3), on one 0x50000000 quad with 3 px padding, colours with a 1,1 black shadow. `print_game_msg` in game_msg_color for message_length x 45 = 225 ticks; chat as "Name (Nation): " in the sender's TEAM_COLOR_TEXT with the text white, 450 ticks; bubbles in the console's neon colour. |
| Top centre | Feedback (red-tinted plate, white text, black shadow, 225 ticks), else instructions (black plate); then the scenario message (`print_msg`: `scenario_size` 12, red plate, white text, `msg_time` graphic ticks, -1 forever). Centred, width min(text, w / 2) + 1, top at 45. |
| Bubbles (`TextBubble::draw_update`) | 120 ticks: grow over 12 ticks (scale 0.2 to 1, alpha 128 to 255, the rect rising on ((2s - 1)^2 - 1) * 50 px), fade over the last 36; +-100 x (-70 .. +10) px around the projected object, `bubble_size` scaled, neon colour, black shadow. Hidden after `bubble_text_hide`. |
| Notices (AnimNotice) | The TEX_ALERTS icon of IFACEMAINBASE centred at native size; after message_length * 400 + 500 ms it moves in 500 ms to half size at (w, h). 22 px bold white text with a black shadow 5 px under it while waiting; a black scroll plate behind. |
| Arrows (`ScreenArrowSpline::draw`) | The `SPLINEARROW` texture (`spline_arrow.tga`, arrowhead in the first `tex_arrowhead` texels) as a strip diag(screen) * 0.4 * 0.03 px wide (0.4 = ArrowSpline::init_class's set_width_percent), the head over the last tenth of the line (`next_at_dist_reverse` 0.1), RED with alpha abs(8 - frame % 17) * 24 + 58. An arrow with a duration ends that many **game seconds** (Game::tick) after it was added; 0 or negative = until cleared. World arrows (tile coordinates) are projected. |
| Highlight (`IFaceButton::on_redraw`, `draw_highlight_circle`) | A card button whose option (plain option buttons) equals `hilite_option` or whose object (train / research / build buttons) equals `hilite_object` gets `BUTTONHIGHLIGHT` (`tutorial_button_highlight_01.tga`) centred on it, half extents = half the texture, turning 20 degrees per graphic tick. `highlight_option(name)` is resolved through options.xml's ENGLISH_NAME in the renderer (the port's builtin passes the name on). |
| Pings (`Scene::add_ping`) | Only for the console leader (or everyone); each clears the previous ping. On the minimap: the "default" EFFECTS/MINIMAP/PING phase (`AttackAlertCircle1.tga`, half-size 16 px growing x1.5 over the 8-tick cycle, alpha a triangle wave) and the pinging leader's neon square (half-size 16 * p, black shadow) for 180 ticks. On the map: the ANNOUNCE_CIRCLE, a TRANSPARENT_WHITE ring shrinking over 8 ticks. Sound S_PING (`soundglobals.xml` PING, `sounds/ping.wav`). |
| Flags (`ScenarioData::draw_components`) | The ScenarioFlag piece (FLAG `banner_0n`, first_flag_piece + 2) at the tile centre on the terrain, scale 3, angle 45 degrees, leader 0's colour, its Banner Wave clock offset by ((frame + y * 17 + x * 13) mod frames) * 67 ms. |
| Camera (`Camera::pan_to` / `set_loc` / `check_zoom`) | Jumps at once. Pans and zooms step every 33 ms: one zoom level per step toward the target level; pans by max(0x300, distance / 2) per axis, snapping within 0x300. `scroll_lock_flags & 8` blocks the moves. |
| Locks and switches | `scroll_lock_flags & 1`: no edge / arrow-key scrolling and no pan toward the cursor when zooming; `& 4`: no minimap look. `zoom_lock_flags & 1`: no wheel / pinch / key zoom. `ui_hidden`: the HUD is neither drawn nor takes the mouse (the script layers stay). `mouse_selection_disabled`: clicks and boxes select nothing. `hotkey_selection_disabled`: control groups do not recall. `pause_disabled`: P does nothing. `pause_game` / `unpause_game` pause the window's stepping. `clear_selection` clears the selection. |
| Popups (BasePop, POPUPSTYLE "default") | A centred box in `bg_color` (39, 40, 43, 224), the `popup_dialog` font, the `butnormal` / `buthighlight` / `butdown` 3-slices 173 x 32. Layout approximated (calculate_dimensions not ported). |
| Goal dialog (`show_goal_dialog`) | A 480 x 330 box (OBJECTIVESDLG) listing the local leader's `add_goal` goals with check boxes and an OK button; the game waits while it is open; Esc or Return close it. |
| Game over | From the simulation's event log (victory / defeat of the local leader): the `victory_notice` / `defeat_notice` AnimNotice, YOU_WIN / YOU_LOSE, the WIN / LOSE track, and "Victory!" / "Defeat!" with the scenario's victory / defeat message in `game_over_message_size`, centred under the resource panel. |
| Unit scale | `set_unit_scale` multiplies the figure's graphics-piece scale. |

Port notes and approximations (check against the game):
- The objectives box sizes text-only entries for their drawn spacing (+5 per entry); the decompile sizes them
  wrap_height + 1 (Buffer::wrap's return convention is not decoded).
- The ScenarioFlag (`banner_0n.BH3`) poses with non-finite, unnormalised normals, which light to NaN;
  `UnitDrawer::draw_ammo` renormalises posed normals (+z fallback). Open: the file's data or the BH3 reader. The banner
  is single-sided and faces the camera with the units' mirrored basis only when the cull mode is not flipped (checked
  by snapshot); its team colour does not show because the texture's alpha is 255 everywhere.
- Objectives that the original's renderer deletes 30 ticks after completion stay in `ScriptPresentation::objectives`
  (the sim keeps them), so `play_objective_sound("")` can differ when completed entries come first.
- `goal_dialog_shown` (ScenarioData::objective_dlg_viewed) is not set by the dialog (ScriptInput has no field for it).
- `display_results` / `display_aging_results` / `save_results` (kVictoryScreen) are informational: their high-score
  lists are objective texts. `take_screenshot` is logged, not written. `use_basic_menu` and `speed_control_disable`
  change nothing visible (the card has no advanced-only options and ronmap no speed control yet). Unit acknowledgement
  and selection sounds (soundtypes.xml) are not played.
- The rare-goods column that pushes MessageWin right (50 px per column) and the mini library's +55 px for the top
  centre text are not drawn yet, so both anchors use their base values.
- The pan cadence (one check_zoom step per 33 ms) and the ANNOUNCE_CIRCLE size are assumptions.

Harness: `--select WHO:o1,..@FRAME` (as a click), `--option N[:MENU[:TYPE]]@FRAME` (the card's option N, then build
page MENU, then building TYPE's button), `--cursor X,Y@FRAME`, `--look X,Y@FRAME`, `--zoom-at LEVEL@FRAME`,
`--script-log`; `RON_POPUP_TEST="text"` leaves a popup open for a snapshot, `RON_POPUP_KEEP=1` keeps the scripts' popups
open in headless runs.

Checked by reading snapshots:
- tut1: selecting Boadicea completes `Select_Boadicea`, adds the flag and the arrow to `right_window`; the Move button
  gets the highlight; the scroll objective completes from `--look` (is_on_screen), the zoom objective from `--zoom-at`;
  the reminder arrow points at the objectives box.
- tut2: `--select 0:0@900 --option 1@1500` completes `select_citizen` at 900 and `military_menu` at 1500 (military
  build page open, the Lookout highlighted); pings show the announce ring and the minimap ping; script pans move the
  camera to the citizen.
- ceaging1: the text-only objective list and the defeat at 3:00 (notice, game-over line). cehotkey1: the hidden HUD
  and the red scenario-message plate. tut5: the first game message. The popup's look via `RON_POPUP_TEST`.

## 9. Audio

`platform/audio/audio.*` uses AVFoundation's `AVAudioPlayer` (build-mac.sh links `-framework AVFoundation`; nothing to
install).
- **Voice**: ScenarioData::sound. `play_sound` / `play_objective_sound` resolve `<scenario dir>\<file>` through the
  install; one sound at a time; `stop_sound` stops it; the playback state feeds `sound_finished` (section 8).
- **Effects**: one-shots from `soundglobals.xml` KEY to `soundfiles.xml` tag (S_PING, YOU_WIN, YOU_LOSE), a random entry
  when a key lists several (SoundGlobal::play).
- **Music** (the JukeBox): tracks per mood from `Data/sound.xml` TRACKS / TRIBE "default" / AGE 0 / MOOD
  (BATTLE_VICTORY = `winning`, BATTLE_DEFEAT = `losing`, ECONOMIC = `economic`); the starting mood is ECONOMIC; a
  random track, never the same one twice in a row; the next starts 1000 ms (JukeBox::timing) after one ends;
  `set_music_mood` switches at once only when the playing track began more than 30 s ago (JukeBox::set_next_mood),
  with a cross-fade over min(5000 ms, the old track's remainder) (JukeBox::play), else the next track takes the new
  mood; the WIN / LOSE track at game over. `set_music_volume` has an empty body in the original.
- Volumes: effects and voice 1.0, music 0.6 (the profile sliders are not read); `RON_SFX_VOL`, `RON_VOICE_VOL`,
  `RON_MUSIC_VOL` (0..1), `RON_MUTE=1`, `RON_NO_MUSIC=1`. Snapshots and harness runs play nothing: the window starts
  the audio.

Not done / to confirm: the tribe-specific first track of a non-scenario game (JukeBox::shuffle's switch on the
console's tribe when frame < 11), audio ducking, the profile's volumes, positional effects (SoundGlobal::play with
coordinates), unit acknowledgements.

## 7. Snapshots

```
./build/ronmap ~/Games/RiseOfNations scenario/Tutorial1/tut1.scx --select 0:2 --snapshot tut1.png --size 1024x768
./build/ronmap ... scenario/Tutorial1/tut1.scx --select-build 0:2002 --at 3936,5088 --size 1280x800 ...
./build/ronmap ... "scenario/Chess Exercise/ceraiding1.scx" --play 1 --select 1:10,11,12 --build-page 0 --at 4536,32568 ...
./build/ronmap ... "scenario/Chess Exercise/ceraiding1.scx" --play 1 --select 1:10,11,12,13,14 --place 427@8600,31200 --place-click --frames 150 ...
./build/ronmap ... "scenario/Chess Exercise/ceraiding1.scx" --select-build 0:2000 --queue '0:2000@50*3' --frames 60 --at 29472,20448 ...
./build/ronmap ... --ui-scale 2 --size 2048x1536 ...   # Retina check
./build/ronmap ... scenario/Tutorial1/tut1.scx --frames 420 --select 0:1@300 --size 1024x768 ...   # arrow, flag
./build/ronmap ... scenario/Tutorial2/tut2.scx --frames 1600 --select 0:0@900 --option 1@1500 --script-log ...   # highlight
./build/ronmap ... "scenario/Chess Exercise/ceaging1.scx" --frames 2720 ...   # defeat notice, text-only objectives
```
