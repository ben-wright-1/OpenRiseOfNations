# Terrain dressing spec: mountains, rivers, land doobers, unit team colours

Phase-1 follow-up to [terrain-spec.md](terrain-spec.md) (base land, transitions, water, trees). This file covers the static
dressing a scenario adds on top of the base terrain. Status key as in [formats.md](formats.md): ✅ verified against real
files or by rendering · 🟡 read from code, not yet verified · ❓ open. Render-state numbers refer to the table in
[renderstates.md](renderstates.md). Evidence (functions and addresses) is at the end.

Reference implementation: `re/tools/dressing.mjs` (library, plus a CLI that prints statistics and self-checks).
Software preview through the game camera: `re/tools/dressing-preview.mjs`.

```
node re/tools/dressing.mjs <file.scx>                                   per-stamp / per-river / doober stats and checks
node re/tools/dressing-preview.mjs <file.scx> [--u 12] [--crop wx0,wy0,wx1,wy1] [--topdown] [--layers ...] [--quadrant n]
```

**Port summary.** Every piece of dressing can be built once at load from the scenario chunks and the data files:

| Dressing | Source | Geometry | Drawn with |
|---|---|---|---|
| Mountain overlay | chunk 69 stamps + `effects_graphics.xml` `<MOUNTAINS>` templates + tileset `<MOUNTAINS>` | a subset of the terrain grid vertices (stored heights), with the template's faces and UVs | "Mountain Blend", 2 draws per range |
| Snowcap | the template's highest vertex (medium/large ranges) | a 6×6 (or 8×8) tcoord patch of the terrain grid | "Weathering Underlay" |
| River trough | chunk 92 polylines → B-spline → sections | 27 vertices per section, at fixed depths (not the terrain heights) | `trough_rs` (no depth test) |
| River water | the same sections | one quad per section at z = 0 | `water_rs` |
| Land doobers | chunk 73 (bush/rock) + graphics.txt `#<TILESET>_LAND DOOBERS` | one camera-facing quad per doober | #7 Doober Blend |
| Roads | **not stored in scenarios** (built at runtime from buildings) | — | skip for Phase 1 |

---

## 1. Mountains

### 1.1 Data

- **Stamps:** terrain chunk 69/71, `n × {u8 wx, u8 wy, i32 range_index, u8 num_coords}` (terrain-spec A). The stamp
  location is the **NW corner of wcoord (wx, wy)**: `loc = (768·wx, 768·wy)`. `range_index` indexes the range list below.
  `num_coords` must equal the template's covered-wcoord count (the loader rejects the stamp otherwise). ✅ This matches
  for all 15 stamps on tut1 and granicus.
- **Range templates** (`Mountains::init`): `Data/effects_graphics.xml` `<MOUNTAINS>`. Each `<MOUNTAIN area height>` has
  `TEMPLATE_TEX`, `MAIN_ALPHA_TEX` and `RING_ALPHA_TEX` (`file=` attributes), and range index = element order (0…15
  in stock data: h1–h6, h6_3, m1, m2, m4–m8, m10, s1). `area`: `sm`/`sml` → size 1, `med` → 2, `lg` → 3. `height` is
  read by `MountainRange::init`. TEMPLATE_TEX is loaded 32-bit, MAIN_ALPHA as DXT3, RING_ALPHA 32-bit.
- **Tileset** (`Data/tilesets.xml`, `<MOUNTAINS>`): `BASETEX` (e.g. `t_mountain_plain.tga`), `ALPHATEX`
  (`t_mountain_blend.tga`), plus the derived `<name>_mtrl.tga` material map at high shader quality. `snowcaps="1"` on
  `<TILESET>` enables snowcaps, and the snowcap texture is `<UNDERLAYS><TEXTURE id="snowcap">` (Dirty:
  `t_mountainpeak.tga`).

### 1.2 Template mesh (`MountainRange::init`) ✅

Template image W×H (all stock templates are 256×256). One template pixel is 48 world units, so 4 pixels make one
tcoord and 16 pixels one wcoord. The image centre maps to `loc`.

1. `ox = (W/2) mod 16`, `oy = (H/2) mod 16` (0 for stock data).
2. Bounding box of all pixels with alpha ≠ 0: `minx, miny, maxx, maxy`.
3. **Vertices:** every sample `(x, y)` with `x = ox + 4i`, `y = oy + 4j` and alpha ≠ 0 gives a vertex:
   - `pos = ((x − W/2)·48, (y − H/2)·48, B(x,y)·3·height/255)`, where B is the blue byte (templates are grey).
   - `UV0 = (x/16, y/16)`. The tileset textures repeat once per wcoord and stay world-aligned.
   - `UV1 = ((x − minx)/(maxx − minx), (y − miny)/(maxy − miny))`. This maps the 128² MAIN/RING alpha textures over
     the template's footprint.
4. **Faces:** for every sample cell whose four corners all exist, add triangles `(tl, tr, bl)` and `(bl, tr, br)`. This
   is the same diagonal as the terrain mesh.
5. **Covered wcoords:** the distinct `(x/16 − (W/2−ox)/16, y/16 − (H/2−oy)/16)` of each face's top-left sample,
   relative to the stamp wcoord, in face order. The count is the `num_coords` check.
6. **Peak:** the first vertex in scan order (rows, then columns) with the strictly largest z. Its tcoord offset is
   `floor(x/192), floor(y/192)`.
7. **Far LOD ("mid"):** a second vertex/face set on an 8-pixel (2-tcoord) grid, covering every 2×2 tcoord block that
   holds at least one complete cell. A vertex copies the template vertex at that sample if one exists, else gets
   z = 0. Faces sit on complete 8-pixel cells, same diagonal. The UVs use the same formulas.

### 1.3 Placement on a scenario (`adjust_for_mountains(1)`, `fill_mountain_data`) ✅

**Answer to terrain-spec open question 5: with stored heights the game does not add anything.**
`ScenarioEditor::init_terrain` calls `adjust_for_mountains(1)`. With that flag, `fill_mountain_data` sets each
mountain vertex's z to the **stored terrain height** at its grid vertex and leaves the height array untouched. Only the
random-map path (flag 0) adds template z to the heights, and writes it back for the near LOD.

For each covered wcoord (stamp + offset) and each of its 5×5 grid vertices `(tx, ty)`, look up the template vertex at
tcoord offset `(tx − 4·wx, ty − 4·wy)`. If there is one and it has not been emitted yet, emit
`{pos = (192·tx, 192·ty, height[ty][tx]), color 0xFFFFFFFF, UV0, UV1}`. Faces are remapped through the emitted
indices. So the overlay is exactly **a subset of the terrain vertices**: same positions, same heights, same
triangulation. Each face is listed on the covered wcoord that holds its first vertex (`CoordInfo::mountain_faces`, or
`mountain_faces_lod` for the far set), which is used for visibility culling only. The game also sets CoordInfo flag 2
(mountain) and `mountain_number = range index` on every covered wcoord.

Normals come from the mountain mesh itself (`RenderHelper::generate_normals` / `generate_tangents` on the merged
mountain vertex buffer). Inside the footprint they equal the terrain normals. The vertex format is
position + normal + diffuse + 2 UVs (+ tangent with normal maps).

### 1.4 Rendering (`TerrainOut::render_mountains`) 🟡 (verified visually in the preview)

There are 16 render helpers, one per range index (`TerrainOut+0x868`, so at most 16 templates). Each frame, the visible
wcoords append their face lists. **Near LOD** (zoom 5–6, unless `scalability.sys_grade > 2`) uses `mountain_faces`. Zoom
0–4 uses the far set. For each range helper that has faces:

| | Draw 1 | Draw 2 (`repeat_last_prim`, same geometry) |
|---|---|---|
| stage 0 (UV0) | tileset `ALPHATEX` | tileset `BASETEX` |
| stage 1 (UV1) | range `RING_ALPHA_TEX` | range `MAIN_ALPHA_TEX` |
| stage 2 | — | `BASETEX` `_mtrl` (high shader quality only) |

Render state **"Mountain Blend"** (a `MountainsOut` member, not in the 50-state table): `enables 0x287` (depth test
LESSEQUAL with write, blend, alpha test, lighting) plus `0x1c000` (normal/spec/gloss maps) only at the highest
shader quality. Alpha test GREATER 0, blend SRCALPHA/INVSRCALPHA, cull CCW, identity world transform.
- stage 0: colour `MODULATE(TEXTURE, DIFFUSE)`, alpha `SELECTARG1(TEXTURE)`, WRAP
- stage 1: colour `ADDSIGNED(TEXTURE, CURRENT)`, alpha `MODULATE(TEXTURE, CURRENT)`, CLAMP
- tfactor is set to `get_global_color(WHITE)` every frame (unused by these ops)

Per pixel: `rgb = tex0.rgb × diffuse + tex1.rgb − 0.5`, `a = tex0.a × tex1.a`, blended over the terrain. The
MAIN/RING textures carry the baked shading in RGB (grey around 180, so they mostly lighten) and the footprint mask in
alpha. RING is slightly larger than MAIN, so the rim shows `ALPHATEX` (the cracked "blend" rock with its own alpha) and
the core shows `BASETEX`. Diffuse is the white vertex colour with lighting on (`ENABLE_LIGHTING`), so the EE shader
lights it per pixel like the terrain. Because the vertices coincide with the terrain, LESSEQUAL passes.

**Port recipe:** build one vertex/index buffer per range at load, from the terrain heights. Draw it twice in the
order above, with depth LESSEQUAL and depth write on, after the river troughs and before the snowcaps and doobers (§6).

### 1.5 Snowcaps (`CoordInfo::add_geometry`, `TerrainOut::add_weather_underlay`) ✅ placement / 🟡 look

- **Who gets one:** a stamp of size 2 or 3 (med/lg; never `sm`) when the tileset has `snowcaps="1"`. It goes on the
  covered wcoord that contains the absolute peak tcoord `P = (4·wx + peak_tx, 4·wy + peak_ty)`. That wcoord gets
  CoordInfo flag 0x200 (`HIGHEST_MOUNTAIN`), `snowcap_offsets = (P.x & 3) << 4 | (P.y & 3)` and `snowcap_size = 6`.
- **Patch:** `x0 = P.x − 3`, `y0 = P.y − 3` (that is `4·xw + offset − 2 − size/2 + 2`), each clamped to
  `[0, 4·xs − 6]` (both axes use the map width). It is drawn as an underlay with alpha 0.5, variant mode 1, aligned. An
  odd `x0` or `y0` is decremented and the patch grows to the next even size (6 → 8). The far LOD steps 2 tcoords.
- **Geometry:** the terrain's own grid vertices over `[x0, x0+w] × [y0, y0+h]` (`get_indexed_mesh_unaligned`), with the
  vertex colour alpha set to 0.5·255.
- **UVs:** `UV0 = (i/w, j/h)`, the whole snowcap image once. `UV1 = (u0 + i·0.5/w, v0 + j·0.5/h)`, one quadrant of the
  image's **2×2 alpha atlas**. The quadrant is `q = trunc(pool·0.04)` (`q = 3` if that is ≥ 3.5), with
  u0 = 0.5 for q ∈ {1, 3} and v0 = 0.5 for q ∈ {2, 3}. `pool = random_map_pool[(4·xs+1)·x0 + y0]` (pre-alignment x0/y0)
  is a byte table that `Terrain::init` fills from the **non-synchronised `internal_random`**, so the game's choice is
  effectively random per session. A port may pick any deterministic q (the preview's `--quadrant`).
- **State "Weathering Underlay"** (`TerrainOut+0x4bf4`): `enables 0x20004` (no depth test, blend, TL_FAKE_LIGHT),
  SRCALPHA/INVSRCALPHA. Stage 0 colour `MODULATE(TEXTURE, DIFFUSE)`. Stage 1 colour passes through, alpha
  `SELECTARG1(TEXTURE)`. All stages CLAMP. The snowcap texture is bound to both stages, so `rgb = tex(UV0) × diffuse`,
  `a = tex(UV1).a`. The vertex alpha 0.5 is overwritten by stage 1. Drawn by `render_weathering(4, 4)` right after
  `render_mountains` (UNDERLAYS index 4 = `snowcap` in stock tilesets).

---

## 2. Rivers

Scenarios store heights with the river beds already carved (terrain-spec B4). The game rebuilds the river meshes from
the stored polylines at load (`ScenarioEditor::init_terrain` → `RiversOut::start_river`, `add_spline_point` per node,
then `River::calculate(heights_stored = 1)`). With stored heights there is **no** `ensure_ocean_end` and **no**
terraforming. The meshes alone matter.

### 2.1 Parameters (`RiversOut::one_time_init`, console variables of "Rivers") ✅

| Name | Value | Use |
|---|---|---|
| max_width(WCoords) | 0.75 | river width scale |
| max_depth(3Dunits) | −60 | trough depth |
| section/wcoord | 1.7 | spline segments per wcoord of polyline, and section length 768/1.7 ≈ 451.8 |
| river speed / riv wake speed | 1.0 / 0.005 | wake and river-doober animation only |
| (byte +0xf8) | 1 | adds `−0.25·max_depth = +15` to every trough vertex |

`rivers.xml` is opened but nothing is read from it.

### 2.2 Spline ✅ (reproduced; see §7)

1. **Control points:** node `(wx, wy)` → `(768·wx + 384, 768·wy + 384, 0)`. Nodes may lie off the map (e.g. `(49, −1)`)
   and may repeat.
2. **Polyline length L** in wcoords: the sum of segment lengths / 768, in float.
3. **Spline type 3, "General B-Spline (Basis)":** degree 3 (lowered to n−1 if n ≤ 3), `segs = trunc(L·1.7)` (at least
   the degree). Knots are clamped uniform: degree+1 zeros, then 1, 2, …, n−degree, with the last value repeated
   degree+1 times.
4. **Evaluation:** `step = (last − first)/segs` in float. Output P0 first. Then for `t = step, 2·step, …` while
   `t < last`, find the first span i with `t ≤ knot[i+1]` and `knot[i] < knot[i+1]`, and output
   `Σ_{j=max(0,i−3)}^{i} N_{j,3}(t)·P_j`. Finally append P_{n−1} if `last − (t − step) > 0.001`.
   - Cox–de Boor with the exe's guards: `N_{j,0} = 1` iff `knot[j] ≤ t < knot[j+1]`. A term is added only when both
     its numerator and denominator are > 0. `N` returns 1.0 if `j + d ≥ knotCount` (never reached here).
5. **Normals** (`build_normals`, flag 0x20 = up +z): segment side vector `normalize(dy, −dx, 0)`. Point j (j ≥ 1) gets
   `normalize(side_j + side_{j+1})`, point 0 gets `side_1`, and the last point gets the last side vector.
   Spline length = the sum of point-to-point distances.

### 2.3 Sections (`RiverOut::create_sections`) ✅

The spline is walked by arc length (`Spline::next_at_dist`): the first call (step 0) returns P0, and each later call
advances `768/1.7` units, interpolating position and normal linearly (normal renormalised) inside the polyline
segment. When fewer than `0.33·step` units remain, the walker snaps to the last point and returns "end". Each call
yields point B; the previous point is A. One **section** = A→B.

Per section, in this order:

- **Width w** (in wcoords): a per-river fractal counter `(a, b)` starts at (0, 0); `a++`, and when it passes 49 it resets
  to 0 and `b++` (b wraps after 49). Before the ocean, `w = (fractal(a, b)·0.005 + 0.5)·0.75`, using
  `Fractal::init(50, 50, smooth 2, flags 2, seed 0)` (identical for every river; midpoint displacement, see
  `makeFractal`), so w ∈ [0.375, 0.746]. After the ocean, `w = min(1.5, previous·1.5)` (delta widening).
  The section's start width is the previous section's end width (0.75 for the first section).
- **v range:** even sections v ∈ [0, 0.5], odd sections [0.5, 1]. Once the ocean is reached at an odd section index cc,
  the parity is swapped for all later sections.
- **Ocean test (until the first hit):** the tcoord of the midpoint of A and B has TData water (chunk 68 file bit 0x10).
  On a hit, cc = this index. If the index is odd, v = [0, 0.5]. w and the stored width are ×1.5.
- **Flags:** 0x11 on the first section; 0x544 on the last (walker end); after the ocean, 0x204 on section cc+1 and 7 on
  every later section.

### 2.4 Trough mesh (`create_trough_and_water`, `create_trough_edge`) ✅

There are three cross-sections ("edges") of 9 vertices each:

| Edge | Centre | Side vector N | Width | v | Fade flag |
|---|---|---|---|---|---|
| start | A | normal at A | w_start | v0 | `flags & 1` |
| middle | (A+B)/2 | (N_A + N_B)/2, **not renormalised** (narrower on bends) | (w_start + w_end)/2 | (v0+v1)/2 | `(flags & 6) == 4 ? (flags & 2) \| 0x200 : flags & 2` |
| end | B | normal at B | w_end | v1 | `flags & 4` |

- **Vertex k = 0…8:** `pos = centre + (k − 4)·s` with `s = N·w·192`, so the total width is `1536·w` units (k = 0 is
  on the side N points away from).
- **z** (fixed, not the terrain): `z_k = −60 + 60·prof_k + 15` with `prof = {1, .75, .26, 0, 0, 0, .26, .75, 1}`. That
  is +15 at the banks, 0 at k = 1/7, −29.4 at k = 2/6 and −45 in the middle. Where the vertex's tcoord is sea water
  and |k − 4| ≤ 1: `z = z − 15` (−60).
- **UV** `(k/8, v)`, WRAP. **Normal** (0, 0, 1). **Tangent** (1, 0, 0).
- **Colour:** 0xFFFFFFFF if the edge's fade flag is 0, else 0x00FFFFFF (0x88FFFFFF if the flag has 0x800, which never
  happens). Effect: the source end fades in, the river fades out at its last section, and everything past the ocean
  section is invisible.
- **Off-map vertices** (tcoord outside the map): x, y clamped to `[0, 768·xs]` and colour 0.
- **Faces:** 2 rows × 8 quads, `(i, i+1, i+9)` and `(i+9, i+1, i+10)`.

**Texture** (`RiverOut::render`): sections before cc use `RIVERTEX_NORMAL`. Sections ≥ cc use `RIVERTEX_DELTA` if cc is
even, else `RIVERTEX_HALFDELTA`. The images run bank to bank in u (u = 0 and 1 are fully transparent banks) and along
the flow in v. One image spans two sections.

**State `trough_rs`** (static in RiverSection.obj): `enables 0x204`: **no depth test**, blend, lighting.
Colour `MODULATE(TEXTURE, DIFFUSE)`, alpha `MODULATE(TEXTURE, DIFFUSE)`, WRAP, SRCALPHA/INVSRCALPHA, identity
transform. It is drawn per visible section (start or end centre wcoord visible) by `RiversOut::render`, between roads
and the underlays (§6). It has no UV animation.

### 2.5 River water quad (`create_water_plane`, `render_water`) 🟡

- One quad per section from trough vertices start k = 1, start k = 7, end k = 1, end k = 7, at **z = 0**.
  Faces (0, 1, 2) and (2, 1, 3).
- Colour: start pair 0 if `flags & 0x10`, else 0xFFFFFFFF. End pair 0 if `flags & 0x40`, else 0xFFFFFFFF.
- UV0 = `(x, y)/768 · water_t_scale` (`TerrainOut+0x5624` = 1, one repeat per wcoord; [water-spec.md](water-spec.md) §1).
  UV1 = (0, v0), (1, v0), (0, v1), (1, v1).
- Stage 0 = the current `terrain art/newwater/waterNNNN.tga` frame (32 frames, 100 ms each; water-spec §4), stage 1 = `RIVERTEX_WATER`
  (`d_wateralpha_real.tga`: white, alpha ≤ 51, so at most 20 % opacity).
- **State `water_rs`:** `enables 0x5` (depth test LESSEQUAL, no write, blend). Stage 0 colour
  `MODULATE(TEXTURE, DIFFUSE)`, alpha `SELECTARG2` (= diffuse.a). Stage 1 colour passes through, alpha
  `MODULATE(TEXTURE, CURRENT)`. WRAP, SRCALPHA/INVSRCALPHA.
- Drawn after the sea water (`RiversOut::render_water`) for the sections drawn this frame. The sea plane at z = 0 already
  fills the carved bed (river tcoords get a quarter of the sea alpha, terrain-spec B4), so this quad is only a faint sheen.

Not covered here (dynamic): wake polygons (`update_wake_polys`) and `RiverDoobers` (anchor/clumpy foam sprites, states
RiverWake #44 / River Anchor Blend #46, drawn after the surf).

---

## 3. Land doobers (chunk 73) ✅ placement / 🟡 size

### 3.1 Pieces

- **Names:** graphics.txt `#LAND DOOBERS`, first field of each line (`GraphicPieces::preload_doober_names`). Chunk-73
  kind 0 → `"bush"`, kind 1 → `"rock"`. The match is case-insensitive (`String::operator==` uses `_wcsicmp`).
- **Art:** `#<tileset name>_LAND DOOBERS` (e.g. `#DIRTY_LAND DOOBERS`), falling back to `#LAND DOOBERS`
  (`GraphicPieces::fill_doobers`). Lines are in the same order. Fields (comma-separated, trimmed):
  `name, file, <rows> rows, <cols> cols, BillBoard|Sprite, <c> centering, <mode>`.
  - `Billboard` (case-insensitive) → rmap 0xe: `ZSpriteAccumMethod` + render state **#7 Doober Blend**. Otherwise
    → rmap 0xf: `SpriteAccumMethod` + #35 Interface Blend.
  - Any non-empty 7th field (`1 Random`, `Selected`) sets RData flag 8: cell chosen from the position hash below.
  - Dirty bush/rock: 128² atlases, 4×4 cells of 32², BillBoard, centering 0.
- Each name owns two consecutive pieces. Static doobers use the first one.

### 3.2 Per doober (`Doober::draw`, `GraphicPieces::draw_doober`, `ZSpriteAccumMethod::execute`)

- **Skipped** when its wcoord is not visible, when the tcoord has `TData & 3 == 3` (❓ meaning), or when it is not seen
  by the viewing player (`doober_seen` bit; all bits set at load). Drawn in chunk order, without sorting.
- **Position:** `x' = x + c·192`, `y' = y + c·192`. `z = trunc(find_data_z(x, y))`, an int.
- **Atlas cell** (float32 arithmetic):
  - `row = trunc(y'·13.117 + x'·27.53)`
  - `col = trunc(|x'·13.932 − y'·17.57|)`
  - `r = row mod rows`, `c = col mod cols`. Cell width = `texW / rows` and cell height = `texH / cols`, as the exe has
    it (rows divides the width). The quad's u spans `[c·cellW, (c+1)·cellW]/texW` and v spans
    `[r·cellH, (r+1)·cellH]/texH` (image top = v 0).
- **Quad** (RData scale 1): `hw = cellW·sprite_x_width/2`, `h = cellH·sprite_z_height`. The vertices are, in order:
  1. `(x'−hw, y'+hw, z+h)`, uv (u0, v0)
  2. `(x'+hw, y'−hw, z+h)`, uv (u1, v0)
  3. `(x'−hw, y'+hw, z)`, uv (u0, v1)
  4. `(x'+hw, y'−hw, z)`, uv (u1, v1)

  Faces (0, 1, 2) and (2, 1, 3). It is a vertical quad in the camera's right/up plane with its bottom edge on the
  ground.
- **Scale:** `Scene::render` sets `sprite_x_width = zoomRatio·768 / Δpx(screen x of (0,768,0) and (768,0,0))` and
  `sprite_z_height = zoomRatio·(−768) / Δpy(screen y of z = 768)` (integer pixel distances). For the fixed camera these
  are zoom-independent in world units: **hw = 1.0607·cellW** (3 world units per texel along the screen-right axis) and
  **h = 3.4641·cellH** (2√3 per texel). One texel is one pixel at zoom 6. A 32² bush is about 96 × 111 units.
- **Colour:** `(transparent << 24 | 0xFFFFFF) & WHITE` = opaque white.
- **State #7 Doober Blend:** `0x20085`: depth LESSEQUAL without write, blend, alpha test GREATER 0, TL_FAKE_LIGHT.
  `MODULATE(TEXTURE, DIFFUSE)` for colour and alpha, WRAP. Drawn after mountains, snowcaps and the second shadow pass
  (§6), flushed per piece by `flush_doobers`.

---

## 4. Roads ❌ not stored

Scenario terrain chunks have no road data (terrain-spec A: ids 81–106). Roads (`World::set_road_at`, `Roads::add_roads`,
`RoadsOut::build_render_data`, `ROADTEX`) are generated at runtime from buildings and cities (`BuildType::place_roads`,
`City::regen_roads`). They belong to the buildings/cities phase, so this spec skips them.

---

## 5. Unit team colours ✅

`TeamColors::init` fills `team_colors.list[9]` from rise.pdb statics (RGB, red in the low byte of the stored dword; the
values decode to the building table already in buildings-spec §4, which cross-checks the decoding):

| Slot | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|---|
| Name | RED | BLUE | PINK | GREEN | YELLOW | CYAN | WHITE | ORANGE | (WHITE) |
| `TEAM_COLOR_UNIT_*` | 255,0,0 | 34,82,255 | 255,0,255 | 0,208,0 | 255,255,0 | 0,255,255 | 255,255,255 | 255,105,0 | 255,255,255 |
| `TEAM_COLOR_UNIT_TINT_*` | 127,47,47 | 47,47,159 | 127,63,127 | 63,127,63 | 127,127,63 | 63,127,127 | 191,191,191 | 127,95,47 | 255,255,255 |
| `TEAM_COLOR_BUILD_*` | 102,0,0 | 26,57,172 | 128,0,128 | 0,89,0 | 98,96,0 | 0,92,108 | 120,120,120 | 112,48,0 | 255,255,255 |

- `Data/alternatecolors.xml` (`<UNIT><TEAMn>`, …) replaces the table when the preference `UseAltColors` is set or a
  mod ships the file. Stock alternate UNIT colours differ only in slot 3: (150,150,150).
- **`TeamColor::get_unit`:** if not in a game, or preference bit 0x100 ("relation colours") is off, return the slot
  colour. Otherwise return BLUE for the viewer's own units, CYAN for allies, WHITE for non-enemies and RED for enemies
  (same scheme as `get_build`).
- The slot is the owner's `Leader+0x6928`. While a unit is being converted (`graphic_tick` 1–3), the converter's colour
  is used.
- **`get_global_color`** (@0049f380) is an **identity copy** in EE: it returns its argument unchanged.
- **Use** (`GraphicPieces::draw_unit`): the colour, with alpha = the chad's transparency (capped at 0xBF for one
  ghost-like case), becomes the **tfactor** of render states #0 Unit Blend, #2, #3, #4 and #5.
  - Unit Blend stage 0 is `BLENDTEXTUREALPHA(TEXTURE, TFACTOR)`, so the team colour shows where texture alpha is low.
  - #1 Unit Behind Building gets a pulse colour instead (`get_pulse_color`).
  - The lighting colour (WHITE, or DARK_GREY when not currently seen) goes through `get_global_color` into the unit's
    RData diffuse.

---

## 6. Draw order (`Scene::render`, the parts relevant here)

`TerrainOut::render` (land, ocean floor, transitions, coral) → `Doober::draw_weathering` → object weathering →
decorations + `render_weathering(0, 3)` (farm/rough/oil underlays) → `render_roads` → nuke underlays →
**`RiversOut::render` (troughs)** → `Underlays::draw` / object underlays → shadows pass 0 → **`render_mountains`**,
`CliffsOut::render(0)`, **`render_weathering(4, 4)` (snowcaps)** → shadows pass 1 → **`Doober::draw` + `flush_doobers`**
→ fog of war → borders → cliffs pass 1 → objects (zoomed in) → … → sea water (`TerrainOut::render_water`) →
**`RiversOut::render_water`** → surf → underlays again → `RiverDoobers::render` → particles.

---

## 7. Verification

- `node re/tools/dressing.mjs <scx>`:
  - **Mountains:** the covered-wcoord count equals chunk 69's `num_coords` for all 4 (tut1) and 11 (granicus) stamps.
    The stored heights at the mountain vertices correlate with the template z at r = 0.85–0.99 for the true placement.
    A 4-tcoord shift gives 0.11–0.48 and an x/y swap 0.23–0.68.
  - **Rivers** (granicus, 3 rivers, 60/84/28 sections, oceans reached at sections −/75/24): on in-map cross-sections
    before the ocean, the carved terrain minimum lies within ±1 of the trough centre on 438/472. The mean |terrain z −
    trough z| is 11.5 units. Perturbed parameters score worse: max_width 0.5/1.0 → 15.6/21.9, max_depth −80 → 18.0,
    no bank lift → 20.7, and a 192-unit offset → 34.5. The residual matches the averaging and smoothing the generator
    applied when it carved the bed.
  - **Doobers:** tut1 231 (135 bush, 96 rock) and granicus 379, all resolved to Dirty atlas cells.
- `node re/tools/dressing-preview.mjs`:
  - `re/previews/tut1_dressing.png` (u = 12) and `tut1_dressing_crop.png` / `tut1_snowcap_crop.png` (zoom 5 and 6):
    mountain overlays with rim/core texturing on the stamped peaks, and the snowcap patch centred on the
    heightfield's summit.
  - `granicus_dressing.png` / `granicus_dressing_crop.png`: troughs follow the carved valleys, fade at the banks and
    widen into deltas at the coast.

---

## Open questions

1. Snowcap atlas quadrant: random per session (`random_map_pool` from `internal_random`). Confirm in-game that it
   varies, then choose a deterministic hash for the port.
2. ~~River water texture and UV scale~~: answered in water-spec §4 (`newwater` frames, `water_t_scale` = 1).
3. River wakes and RiverDoobers (foam/anchor sprites; RiverWake #44, River Anchor Blend #46) were not decoded. Neither
   was who uses #20 River Blend.
4. `TData & 3 == 3` (hides doobers): which tcoords get it at load.
5. "Selected" doober pieces (building rubble) also get the position-hash flag. Check whether `who/whom` selection is
   meant for them (not needed for static doobers).
6. Mountain lighting: the exact `generate_normals` weighting (area-weighted assumed). It only matters on the overlay
   border.
7. The snowcap clamp uses `4·xs` for both axes (harmless on square maps).

---

## Evidence

All addresses in `riseofnations.exe`. Local decompiles and disassembly are in `re/decomp/dressing/` (and `asm/`). To read
internal-string offsets, use `node re/tools/annotate-intstr.mjs <file.c>` (prints an annotated copy).

| Topic | Functions / data |
|---|---|
| Mountain templates | `Mountains::init` 0089ad70 (`effects_graphics.xml` MOUNTAINS/MOUNTAIN, TEMPLATE_TEX/MAIN_ALPHA_TEX/RING_ALPHA_TEX, area sm/sml/med/lg; "Mountain Blend" state fields), `Mountains::add_range` 008992b0, `MountainRange::init` 008998b0 (vertex/UV/face/LOD/peak construction; asm for the UV and z arithmetic), types `MountainRangeData` / `MountainRangeOut` (re/types/terrain.txt) |
| Placement | `ScenarioRead::load_mountains_chunk` 009adf40 (range index and num_coords checks), `Mountains::add_mountain` 0089c2e0 (loc = 768·wcoord), `TerrainOut::adjust_for_mountains` 008703c0 (flag 1 path, CoordInfo flags 2/0x200, snowcap offsets/size, per-wcoord face lists), `TerrainOut::fill_mountain_data` 00869380 (grid matching, stored vs added z), `ScenarioEditor::init_terrain` 009a19d0 |
| Mountain render | `TerrainOut::render_mountains` 008551f0 (16 helpers, 2 draws, textures per stage), `TerrainOut::render` 008558b0 (LOD rule `zoom < 5 or sys_grade > 2`; mountain helpers, normals and tangents), `CoordInfo::add_geometry` 0084bef0 (face lists, snowcap call), `TileSet::get_mountain_texture` 0087ab30 |
| Snowcap | `TerrainOut::add_weather_underlay` 00874d00 (alignment, quadrant, UV0/UV1, vertex alpha; asm), `TerrainOut::render_weathering` 00852f70 (both stages = underlay texture, state +0x4bf4), `TerrainOut::one_time_init` 0085c150 ("Weathering Underlay" fields), `Terrain::init` 00850f70 (`random_map_pool` from `internal_random`) |
| Spline | `RiverOut::add_spline_point` 00885a40, `River::calculate` 008858e0, `Spline::clear` 00914410, `calc_spline` 00912f00, `clamp_knots` 00910be0, `generate_bspline` 00911820, `calc_bspline_basis` 009116e0, `build_normals` 00911f60, `next_at_dist` 00912b70 (asm for returns and thresholds), `Spline::init_class` 009144b0 (type names) |
| River sections | `RiverOut::create_sections` 00884e00 (step, widths, fractal counters, ocean test, flags, v ranges; asm), `RiversOut::one_time_init` 00888510 and `registration` 00889370 (parameters), `River::init` 00883e30, `River::one_time_init` 00883d30 (Fractal 50×50, smooth 2, seed 0), `RiversOut::start_river` 00888f70 (width = max_width), `Fractal::init` 006aa2d0, `Fractal::get_height` 006aa870, `init_coord_lookup_array` 00681db0 (`div_3_table` = floor division) |
| Trough / water | `RiverSection::init` 0088c360, `RiverSection::create_trough_and_water` 00889f20, `create_trough_edge` 00889660 (profile table at 0xb69870/0xb69ac0, colours, clamping; asm), `RiverSectionOut::create_water_plane` 0088b710, `one_time_init` 0088bc20 (`trough_rs`, `water_rs`), `RiverOut::render` 00884c30 (texture choice), `RiverSectionOut::render` 0088cc40, `render_water` 0088ce80, `RiversOut::render` 00888470, `render_water` 008883e0, `terraform_from_river_vert` 0088b360 (generation path only) |
| Doobers | `ScenarioRead::load_doobers_chunk` 009ae920 (bush/rock), `Doober::add_doober` 00846520, `Doober::draw` 00844bd0, `GraphicPieces::preload_doober_names` 0090d990, `fill_doobers` 0090da80, `init_doober_data` 008f29d0 (fields, flag 8), `init_doober_map` 008f8a90 (rmaps 0xe/0xf, states #7/#35), `draw` (case 0xd) / `draw_doober` 008e70f0 (centering, hash constants 27.53/13.117/13.932/17.57), `ZSpriteAccumMethod::execute` 0091e350 (cell, quad, UVs; asm), `Scene::render` 008b3270 (`sprite_x_width` / `sprite_z_height`), `RData::RData` 0091a340 (scale 1), `String::operator==` 00a1f140, `Text::item` 00a43850 |
| Team colours | `TeamColors::init` 008c3100, `TeamColor::get_unit` 008c2db0, `get_build` 008c2890, statics `TEAM_COLOR_UNIT_*` 00c8ce04… / `TEAM_COLOR_UNIT_TINT_*` 00c8cf24… (DumpSymbols), `get_global_color` 0049f380, `GraphicPieces::draw_unit` 008e7da0 (tfactor writes) |
| Draw order | `Scene::render` 008b3270 |
| Roads | `World::set_road_at` 006b43b0, `BuildType::place_roads` 0063c580, `City::regen_roads` 00738aa0, `RoadsOut::build_render_data` 00894890; terrain chunk list in `ScenarioRead::load_terrain_chunk` 009aedf0 |
