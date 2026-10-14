# Buildings: drawing spec

Status: ✅ verified against the binary (PDB names) and against the data. The placement rules below put the zbuffer mesh on the
sprite with a median IoU of 0.80 over all 909 buildings that have both (`node re/tools/building-zfit.mjs`), and the 3D add-ons land
on their props in the sprite (`node re/tools/building-preview.mjs <BUILD> out.png --z --tint`; examples are in `re/previews/build_*.png`).
🟡 marks the parts that were inferred rather than traced.

Related specs: camera and tiles in [terrain-spec.md](terrain-spec.md) §B7, render states in [renderstates.md](renderstates.md)
(#25–#29), and add-on/AMMO handling in [attachments-spec.md](attachments-spec.md).

## 1. Model

A building is drawn from four things, all keyed by one `<BUILD>` in `Data/building_graphics.xml`:

| Part | Source | How it is drawn |
|---|---|---|
| Sprite | `texture=` TGA | a 2D screen-space quad with **no depth test or write**, two passes (team colour, then the rest) |
| Depth hull | `zbuffer=` BH3 | 3D, **depth only** (colour alpha 0), after the sprite. Units are depth-tested against it. |
| Add-ons | `<EXISTEVENT subject=…>` → `<AMMO>` in `effects_graphics.xml` | real 3D BH3 models with their own texture and BHA anim (flags, cranes, wheels, laundry) |
| Particles | `<PARTICLEEVENT>` | particle emitters at hull nodes (chimney smoke) |

Buildings are painter-sorted with the units: every object goes into one tree keyed by world `x + y` and is drawn in ascending
order, back to front (`ObjectsOut::sort_all` @00657320, `sort_buildings` @00657200).

## 2. Sprite texture

- **One image per building.** There is no atlas, no frames and no damage states: 340 distinct TGAs, all 32-bit with
  descriptor 0x08 (bottom-up rows, 8 alpha bits). 328 are type 10 (RLE) and 12 are type 2. The engine loads the whole image as
  one sprite (`SpritePacket::set_sprite` @009171c0 → `ImageIO::load`).
- **The alpha channel is a 4-way code**, measured over all 340 textures:

  | alpha | share | meaning | RGB |
  |---|---|---|---|
  | 0 | 46.8 % | empty | (ignore) |
  | 255 | 50.2 % | opaque art | colour |
  | 145 | 1.9 % | **baked ground shadow**, blended at 145/255 | always black |
  | 248 | 1.1 % | **team-colour mask** | grey luminance (≈80–90) |
  | other | <0.1 % | stray edge values | colour |
- Animated parts are **not** in the sprite. They are the 3D add-ons in §5.
- **Snow:** when the tileset has `snowy_buildings`, the loader tries `art/snow/<name>_snow.<ext>` first and keeps it if the
  file exists. There are 345 such files (`GraphicPieces::init_build_data` @008f2f20, internal strings `snow\`, `_snow.`).
- **Discrete scales:** at load the sprite is resampled into copies at {1, 2/3, 1/2}. When the screen width is exactly 1024
  the copies are {0.8, 0.533, 0.4} (`init_build_data`, param "Discrete Levels"; `D3D11GLSprite::init` @10037c80). At draw time
  the copy whose scale is nearest to the current sprite scale *k* is used and stretched to *k*, with linear filtering and
  clamp. A port can use mipmaps or a single linear-filtered texture instead.
- The loader trims fully transparent borders (`Sprite::init_32` @0054c1d0) and adds the trim offset back at draw time. The net
  result is the same as drawing the untrimmed image, so a port can ignore the trim.

## 3. Placement (camera, anchor, scale)

**Camera** (terrain-spec §B7): **orthographic**, yaw 45°, tilt 60°. For a world point P and look-at point L, with *u* world
units per pixel and y pointing down:

```
px = 0.70711·(ΔX − ΔY)/u
py = (0.35355·(ΔX + ΔY) − 0.86603·ΔZ)/u        Δ = P − L
depth (larger = farther) = −(0.61237·ΔX + 0.61237·ΔY + 0.5·ΔZ)
```

Zoom level 6 (the closest) gives **u = 3** on screens at least 1280 wide.
A tile (TCoord) is 192 world units, and +z is up.

**Sprite scale.** `k = fixed_zooms[6]·game_sprite_scale / zoom_distance`, where
`game_sprite_scale = screen_w / max(screen_w, 1280)` (`GraphicPieces::draw_build` @008e7610 build_num/build_denom;
`Game::init_camera` @00590bc0). That makes `k = 1` at zoom 6 and `k = ratio[level]` otherwise. **One sprite texel always
covers 3 world units** along the camera's right and up axes, at every zoom. The building art is authored at zoom 6.

**Anchor.** For a building object at world position (X, Y, Z), with `h` = `halfoffset` (0 or 1):

```
L = (X − 96·h, Y − 96·h, Z)                         // half a tile toward −x,−y; same L for sprite, hull and add-ons
(ax, ay) = round(project(L))                        // MiscAccess::world_to_screen
top-left = (ax − trunc(spritexoff·k), ay − trunc(spriteyoff·k)),  size = (tex_w·k, tex_h·k)
```

- `spritexoff`/`spriteyoff` are the anchor's texel coordinates in the image, with a **top-left origin**. They are not
  offsets from the centre. They overwrite the loader's default anchor (`w/2`, `h − w/4`) right after `RData::load`
  (`init_build_data`: `SpritePacket` +0x10/+0x14 ← attrs `spritexoff`/`spriteyoff`, default −1).
- `halfoffset` shifts the anchor by (−96, −96) world units, which is 22.6 px straight up at zoom 6. It is an art-alignment flag:
  in the data it does not follow footprint parity (`WallOut::draw` @00641930, reached for buildings through @00472370:
  `loc = coord − build_half_tcoord·0x60`; `BuildRDataStruct::build_half_tcoord` ← `halfoffset`). The object coordinates are
  XOR-obfuscated in memory (`^ 0x63637`), which is irrelevant to a port.
- `angle` is always 270 for a building chad, and the chad scale is (1, −1, 1). `build_scale` is 3.0 (a global @00c062d0,
  written only by a console cheat).
- **Equivalent world-space form** (exact, because the camera is orthographic): draw a camera-facing quad centred so that texel
  (spritexoff, spriteyoff) sits on L. Its right axis is (0.7071, −0.7071, 0)·3 per texel and its down axis is
  −(−0.3536, −0.3536, 0.8660)·3 per texel. Turn off depth test and depth write.
- The D3D path subtracts 0.5 px (the D3D9 texel convention) and builds the quad from 6 TL vertices at
  `z = (0x1fff − update_depth)/8192`. Metal needs neither.

## 4. Sprite passes, depth hull, colour

`init_build_map` @008f9450 and `init_zbuild_map` @008f9070 build the per-pass method lists. The hull path is used when
`zbuffer=` is non-empty (`init_build_data`).

| pass | used when | sprite primary → follow-up render state | hull |
|---|---|---|---|
| 0 | normal | #28 Team Color Blend → #26 Building Blend | `PreTransformBatchMethod` (batched, cached across frames) |
| 1 | no live object (`o == −1`: placement ghost, collapsing building) | #28 → #26 | `PreTransformMethod` (immediate) |
| 2 | chad `whom ≠ 0` (highlight/flash) | #25 Blocked Building Blend | `PreTransformMethod` |

`D3D11GLSprite::draw` @10038240 draws the quad with the primary state. If a follow-up state is set, it re-submits the same quad
with that state (`set_render_state` @10037a90 stores both). Per pixel, for a normal building:

1. **Team Color Blend (#28):** keep only texels with `a == 248` (alpha test EQUAL 248, or 247 on SiS cards).
   `rgb = saturate(tex.rgb × team.rgb × 4)`, written opaque (ONE/ZERO).
2. **Building Blend (#26):** keep `a != 248` (alpha test NOTEQUAL `alpha_ref`). `rgb = tex.rgb`, blended with
   SRCALPHA/INVSRCALPHA, so shadows (145) darken the ground and 255 is opaque. `draw_build` sets `alpha_ref = 0xf8` when
   the chad's `transparent == 255`, and **0** while the building is fading. In that case every non-zero texel, team texels
   included (uncoloured), goes through this pass.
3. Both states set `enables 0x20084`: no depth, blend, alpha test and `TL_FAKE_LIGHT`. That last option makes the EE shader apply
   `pow(c, 1.9)·hdr` and `a·gTLAlpha` before tonemapping, so in a non-HDR viewer output the texture colour as is.

**Hull (#29 Building ZBuffer Blend):** depth test LESSEQUAL with depth write on, and colour = tfactor alpha 0 (invisible).
Vertices are pre-baked once at load (`PreTransformMethod::load` @0091dbe0, `update_vert_list` @0091ac60):
`v' = PreTransform(node_world(v))`, with PreTransform = `rotate_z(270)` then scale (1, −1, 1) (param "PreTransform" built in
`init_build_data`). At draw time the world transform is `translate(L)` · scale(sx·3, −sy·3, sz·3) = (3, 3, 3)
(`draw_build`; the batch path `PreTransformBatchMethod::execute` @0091d340 uses `v'·scale.x + L`). With the engine's rotation
convention (rotations by +θ act as −θ, see formats.md), the net mapping is:

```
world = L + (−3·y, −3·x, 3·z)          // (x, y, z) = hull vertex in BH3 model space after its node's rest transform
```

This is a mirror (det < 0). Draw the hull with **culling off**. The same mapping places the add-ons (§5).
Validation: the mapping scores IoU 0.80 against the sprites, against 0.65 for no rotation and 0.59 for an x/y swap. The best u
is 3.0 (0.63 at 2.5, 0.60 at 3.5, 0.21 at 6). Run `node re/tools/building-zfit.mjs --sweep`.

**Team colour** (`draw_build`):

- base = `TeamColor::get_build` @008c2890 of the owner's colour slot (`Leader+0x6928`)
- defaults (`TeamColors::init` @008c3100, `TEAM_COLOR_BUILD_*`), as RGB: 0 (102,0,0), 1 (26,57,172), 2 (128,0,128),
  3 (0,89,0), 4 (98,96,0), 5 (0,92,108), 6 (120,120,120), 7 (112,48,0), 8 (255,255,255)
- `alternatecolors.xml` `<BUILD>` overrides these when it is present or enabled. Note that its TEAM3 is (80,80,80).
- A preference makes self/ally/enemy show as BLUE/CYAN/RED.
- While the building is being captured (`graphic_tick` 1–3), it flashes the capturer's colour.
- A chad with `whom ≠ 0` replaces the team colour with a pulsing colour.
- The result passes through `get_global_color`. The chad's `transparent` becomes the colour alpha, and the colour goes to
  `set_team_color` (sprite diffuse).
- Fog: `draw_build` writes WHITE (seen) or GREY (remembered, not currently visible) into the tfactor of #26. 🟡 Building Blend
  selects the texture colour only, so in EE this has no visible effect.

## 5. Add-ons (`<EXISTEVENT>`) and particles

`init_build_events` @008ea3c0. Each event stores:

- `buildanim`: an index into `BuildAnimNames` (@0040f1b0): 0 BUILD_DEFAULT, 1 BUILD_BUILD, 2 BUILD_EMPTY_IDLE,
  3 BUILD_NUKE_BUILD, 4 BUILD_NUKE_IDLE, 5 BUILD_NUKE_LAUNCH, 6 BUILD_MISSILE_BUILD, 7 BUILD_MISSILE_IDLE,
  8 BUILD_MISSILE_LAUNCH
- `subject`: an `<AMMO>` name (first case-insensitive match, as for units)
- `anim`: the add-on's BHA
- `axis`: a byte, default −1
- `node`: a byte, default 7

The building's current `cur_anim` comes from `WallOut::draw`:

| cur_anim | when |
|---|---|
| 0 | idle |
| 1 | working/producing |
| 2–5 | silo states |

When the stocked unit type lacks flag 0x13b, the silo states get +3, which selects the missile variants instead of the nuke
ones.

Drawing (`execute_graphic_events` @00909a10, building branch at 00909bb4; `draw_ammo` @008e68b0):

- Each event is a top-level chad from a persistent pool (`find_empty_addon` @008e6140) with:
  - `loc = L`, `angle = 270`
  - scale = building scale × 3 = (3, −3, 3)
  - `draw_ammo` transform = rotate_z(angle) · translate · scale, which gives **world = L + (−3y, −3x, 3z)** of the posed
    add-on vertex. That is the same space as the hull, verified visually (`re/previews/build_village0_tint.png`,
    `build_woodcutter0_tint.png`).
- `axis ≠ 0`:
  - Offset: `L += (sx, sy, sz) ∘ translation(rotate_z(270) · hullNode[node].world)`. Here `hullNode` is the zbuffer BH3's
    NodePacket, with the same slot rules as units. Node 7 = "pivot node 3".
  - Orientation: the add-on's angle becomes `45 − wind_angle + 180`, so it faces the wind.
  - Counts: 18 add-on events have axis = 1, all of them granary windmill sails on BUILD_BUILD. The other 444 have axis 0.
  - Check: `re/previews/build_granary0_tint.png` shows the sails on the right silo's top. 🟡 The sign of the wind angle was
    not checked against the game.
- Time: the add-on runs the chad's anim time, modulo the building's `anim=` clip length (`build_anim_time`). The time is frozen
  at 0 while the object is not active (not yet built).
- Render state: the AMMO's (mostly Ammo Blend), depth-tested against the hull. It is gated by a scalability flag
  (`section_render + 0xF`).

`<ANIM anim=BUILD_X file=…>` inside a BUILD binds a BHA to action X of the building's AnimationPacket (`init_build_data`; 7 uses,
all silos). These animate the hull hierarchy that the node offsets read, not the sprite.

`<PARTICLEEVENT buildanim particle_type starttime endtime node axis>`: an emitter at hull slot `node` (smoke nodes = 8–11).
🟡 The axis semantics were not decoded here.

## 6. Construction and variants

**Progress** `p = hitpoints / max` (`WallOut::draw`):

- **Building sprite:** hidden below 0.84, fades in over 0.84–0.88 (`transparent = 255 − 255·min(1, 25·(0.88 − p))`), and is
  opaque from 0.88. Forced construction counts as p = 0.
- **Construction underlay** (`WallOut::draw_underlays` @006410a0): opaque below 0.84 and fades out over the same window (the
  mirror image). Its sprite is `get_construct_gpiece` @00907840:
  - `CONSTRUCTION[constructsize − 1]`. With `constructsize = 0`, use the build type's size (BuildType+0x238).
  - Type flag +0x218 == 1 (dock) → entry 9. Flag 0x1b6 (Senate) → entry 10.
  - The 11 `<CONSTRUCTION texture spritexoff spriteyoff [model]>` entries are 1×1…7×7, 7×7 again for 8×8, 9×5, dock and 3×6.
    They are drawn like a building sprite at `angle 270`.
- **Scaffold** (`draw_scaffold` @00907420, called from `WallOut::draw_overlays` @00641600 while started and not complete):
  - `effects_graphics.xml` `<SCAFFOLD_TIMING>` list number `size − 1` (list 9 for flag-0x1b6 types) holds
    `<TIMING name start_percent end_percent x y z angle>` entries.
  - Each piece is drawn while `start ≤ p ≤ end` as a 3D `<SCAFFOLD>` model (Scaffold Blend) at
    `coord − 96h + (x, y, z)`, with that angle and scale (3, −3, 3).
  - The model set depends on the owner's age: 0–2, 3–4 or 5+ (`num_scaffold_pieces_per_age`).

**Port (`ronmap`, `encodeConstructionOf` / `encodeScaffoldOf`):** progress from the sim's `BuildView` (unstarted = 0).
Port decisions to confirm against a screenshot: scaffold `TIMING` offsets and models use the add-ons' building-space
mapping `(x, y, z) → (−3y, −3x, 3z)` after rotating the model by `angle` (this places the pieces on the footprint edges
and the crane outside a corner, which looks right); dock / Senate underlay and timing entries are chosen by graphic name
(the type flags are not loaded in the simulation's rules yet); the sprite fade uses vertex alpha.

**Variant selection** (`get_build_gpiece` @0090bce0, `build_node_from_gpiece` @008fd550):

- Graphic slot = `first_build_piece + per_age·age + num_build_pieces·style + (buildtype − 0x19e)`.
- The XML name is `<TYPE>-<STYLE>-AGE<n>` with ages 0–7.
- Style ids (rules.xml art styles; tribe `<BUILD_CONTINENT>` / `<BACKUP_BUILD_CONTINENT>`): 0 DEFAULT (Med), 1 NEUROPE,
  2 ASIAN, 3 KOREAN, 4 AMERICAN, 5 ARAB (Turk), 6 IROQUOIS, 7 COLONIAL, 8 EINDIAN.
- Lookup order: the tribe's BUILD style at age a, a−1, …, 0; then the BACKUP style a…0; then DEFAULT a…0; and finally
  DEFAULT-AGE0. The data has AGE0/3/4/5/6 only, so for example a BARRACKS at age 2 resolves to AGE0.
- 🟡 `scale_gpiece` @008ea0c0 computes the age argument from the obfuscated leader age (`^ 0x62766`) and the building's stored
  age byte. On high-detail settings it uses the viewing player. The exact ±1 offsets were not pinned down.

## 7. Implementation recipe (viewer and map)

1. **Load.**
   - Parse `<BUILD>` (texture, zbuffer, spritexoff/yoff, halfoffset, anim, constructsize, EXISTEVENT/PARTICLEEVENT/ANIM),
     `<CONSTRUCTION>`, `<AMMO>`, `<SCAFFOLD>` and `<SCAFFOLD_TIMING>`.
   - Upload the TGA flipped to a top-left origin. Keep the raw alpha codes (sample with nearest alpha or test with a ±0.5/255
     tolerance, because linear filtering blends 248 with its neighbours; the original filters too, so edge texels may miss the
     EQUAL test).
2. **Per frame,** in painter order (ascending x + y):
   1. Sprite quad, depth off, at the anchor from §3.
   2. Fragment shader: `a ≈ 248` → `saturate(tex·team·4)`, opaque; otherwise `tex` with alpha blending, alpha × fade.
   3. Hull: depth write on, colour writes off, culling off, mapping `L + (−3y, −3x, 3z)`.
   4. Add-ons: 3D with depth test and the same mapping.
   5. Units after all of this, depth-tested.
3. **Viewer without a world:** put L at the origin, use the orthographic camera from §3 with u = 3/k, and draw the sprite with
   its anchor at the projected origin. `re/tools/building-preview.mjs` is a reference software renderer for exactly this.

## Evidence (riseofnations.exe / d3dgl.dll, PDB names)

- `GraphicPieces::init_build_data` @008f2f20:
  - Attribute names come from `internal_strings.xml` (index = offset/20): `texture` #136, `zbuffer` #3850,
    `constructsize` #3851, `spritexoff` #3852, `spriteyoff` #3853, `halfoffset` #3854.
  - RData params "Sprite", "BH3 Name", "TintColor", "PreTransform" (rotate_z(270) + scale(1,−1,1)), "Discrete Levels".
  - Map choice `rmaps[4]` (with zbuffer) or `rmaps[3]`.
- `SpriteMethod::execute` @0091ca30:
  - `set_team_color(build_sprite_color)` (GSSprite vtable +0x54) and `set_screen_clip_pairs` (+0x70; clips the sinking ruin
    when `build_z_sink > 1`).
  - `set_sprite_scale(num, num, den)` (GraphSys +0xF4) and `set_render_state(rs, rs2)` (+0x50).
  - `Sprite::draw(x − trunc(xoff·k), y − trunc(yoff·k))`.
- `D3D11GLSprite::draw` @10038240 / `draw_quad` @10038a70:
  - Package choice is the nearest `|scale − num/den|`; quad size = texel size·k.
  - The primary state is drawn, then the follow-up state.
- Render states: `Scene::init_render_states` @008ba450 (#25 "Blocked Building Blend", #26 "Building Blend", #28 "Team Color
  Blend", #29 "Building ZBuffer Blend"). Enable bits: `D3D11GLGraphicCard::set_depth_stencil_state` @1002b5f0 (bit 0 depth
  test, bits 0+1 depth write), `set_blend_state` @1002b900 (bit 2), `set_shader_options` @1002db30 (0x80 alpha test, 0x20000
  TL_FAKE_LIGHT).
- `GraphicPieces::draw_build` @008e7610 (team colour, fog tfactor, `alpha_ref` switch, transform, pass choice) and
  `GraphicPieces::draw` @0090b2d0 (gpiece type 1 → `draw_build`; units whose RData has flag 2 are sprite units drawn by
  `draw_build` at angle 270).
- Ordering: `ObjectsOut::sort_all` @00657320 keys the RenderObject tree by `(x ^ k) + (y ^ k)`.
- Data checks:
  - TGA headers and alpha histogram: a scratch scan of all 340 building TGAs.
  - Hull fit: `re/tools/building-zfit.mjs`.
  - Visual check: `re/tools/building-preview.mjs`, which wrote `re/previews/build_village0{,_z,_tint}.png`,
    `build_woodcutter0{,_tint}.png`, `build_barracks_neurope0_z.png` and `build_granary0_{tint,wind90}.png`.

## Open questions

1. Fog of war on building sprites in EE: the GREY tfactor on #26 looks dead (the stage selects the texture). Fog may instead
   come from the fog overlay. Check this against a screenshot.
2. The fade-alpha path: how chad `transparent` reaches the pixel under SELECTARG1 (`gTLAlpha` material, or the diffuse alpha in
   the D3D11 sprite path) was not traced.
3. The ±1 age offsets in `scale_gpiece`, and which player's style is shown when graphics detail is high.
4. The construction underlay position uses locals the decompiler did not name. It is assumed to be the same L (with halfoffset).
5. The "Unit Behind Building Blend" (#1: depth GREATER, no write, alpha 0xAF) is presumably the ghost of a unit hidden by a
   hull. Its call site was not found.
6. PARTICLEEVENT `axis` (values −2/1/2/3) and the ruin-sinking clip (`build_z_sink`, `cur_fall_build_z_sink`) were not decoded.
