# Water spec: sea, ocean floor, river water, surf, map border

Status: ✅ geometry, constants, render states and pass order read from `riseofnations.exe` (rise.pdb applied) ·
🟡 a few visual consequences derived from code only (marked; check against a screenshot) · ❓ open questions at the end.

Companion docs: [terrain-spec.md](terrain-spec.md) (land mesh, transitions, camera, draw order),
[lighting-spec.md](lighting-spec.md) (shader lighting, HDR, reflection buffers), [renderstates.md](renderstates.md)
(stage-op encoding, enables bits). Shader behaviour is paraphrased from the user's own `shaders\ps_global.hlsl`.
Software check: `node re/tools/water-preview.mjs <file.scx>` (section 9).

**Field naming.** `TerrainOut` (the global `terrain`, 0x00c0b0f0) embeds `TerrainData` at **+0x40**, so a
`TerrainOut` offset X is the PDB field `TerrainData` X−0x40 (e.g. `+0x4d80` = `min_clip_level`, `+0x46fc` = the
`ocean_verts` stream). Names below are the PDB names where they exist.

---

## 0. What the original actually draws (summary, in draw order)

| # | Pass | What | Where it hides the shelf/deep staircase |
|---|---|---|---|
| 1 | Land buckets (Base Terrain) | land mesh; **vertex alpha fades to 0 between z = −52 and z = −89** (`light_land_vert`); underwater vertex RGB replaced by a grey "floor tint" | shelf (≈ −200) and deep (−273) land is fully transparent |
| 2 | Ocean floor (`render_ocean_floor`) | **opaque flat sheet at z = −101** over every shelf/deep/half-water wcoord (+ a 1-wcoord ring off the top-left map edges), OCEAN texture ×2, two drifting UV layers | replaces both shelf and deep with one uniform floor |
| 3 | Transitions | as terrain-spec B2a; ocean wcoords add CoralSandy + CoralTrans (no depth test, alpha = mask only) | the only shelf/deep difference left is a coral fringe on the deep side |
| 4 | `render_coral` | its RenderHelper is never given faces in EE → draws nothing | – |
| … | roads, rivers (troughs), shadows, mountains, doobers, fog, borders, objects, particles | | |
| 5 | **Sea water** (`render_water`) | per water wcoord a 13-vertex patch at z = 0; vertex RGB = WaterTop→WaterBot screen gradient; alpha from heights, smoothed; deep water ≈ **189/255**, not 245 | |
| 6 | River water (`RiversOut::render_water`) | z = 0 polygons over river troughs, animated `newwater` frames | |
| 7 | Surf (`Surf::draw`) | animated `.mot` strips along stored waterlines, states 47–49 | |
| – | Map border skirts | **built but never drawn in EE**; outside the map is the HDR clear colour (black) | |

Why our renderer shows the problems from the task:

- **Stepped shapes where deep meets shelf**: we draw the shelf's sand and the deep `ocean_floor` texture at full
  alpha under a 96 % opaque sheet. The original fades both out (step 1) and covers them with one floor sheet (step 2).
- **Sawtooth water edge**: our sheet splits each 384² cell along one diagonal with a linear `−h/55` ramp. The original
  uses four triangles fanned around an averaged centre vertex per 384² quad (no diagonal bias), a narrower ramp
  (−55…−25) and a neighbourhood smoothing pass. Measured coastline jaggedness (section 9): 1.009–1.015 original vs
  1.020–1.065 ours.
- **Black outside the map**: that is what EE shows too (skirts are dead code); see section 6.

---

## 1. Sea water mesh

### 1.1 Which wcoords get water — `TerrainOut::coord_contains_water(wx, wy)` @0086a080

False outside the map. Otherwise true if **any** of:

1. `WData.flags & 0x0004` (coast; file flag 0x08), or `land == 2` (deep ocean), or `WData.flags & 0x0100` (half water;
   file flag 0x80);
2. CoordFlags (chunk 99, ORed into `CoordInfo.flags`) `& 0x9` (WATER | OCEAN);
3. CoordFlags `& 0x4` (COASTAL) **and** any of the wcoord's 5×5 height vertices is < 0;
4. CoordFlags `& 0x10` (RIVER).

A water wcoord is always covered by a full 768×768 patch; there is no water outside such wcoords.

### 1.2 Shared vertices — `generate_water` @00861980 → `create_water_verts(px, py, py&1, map)` @0086ceb0

Half-wcoord lattice: `px ∈ {0, 2, …, 2·xs}` (even only), `py ∈ {0 … 2·ys}`; position `(px·384, py·384)`; a
`(2xs+1)×(2ys+1)` int map holds the vertex index of each lattice point (−1 = none). Each call emits up to three
vertices along +x (`x, x+384, x+768`); a vertex shared with the left neighbour is not emitted again.

**Even row** `py = 2·wy` (a wcoord edge row), `wx = px/2`:
- `left = contains(wx−1, wy−1) || contains(wx−1, wy)`; `up = contains(wx, wy−1)`; `down = contains(wx, wy)`.
- If neither `up` nor `down`, nothing is emitted. Otherwise record the row start index: for wcoord `(wx, wy−1)` this is
  its **row 2** (`CoordInfo.vert_row2`), for `(wx, wy)` its **row 0** (`vert_row0`); the start is the current count,
  or count−1 when `left` (the left neighbour's last vertex is reused).
- Emit `(x, y)` unless `left`; then, only if `wx < xs`, `(x+384, y)` and `(x+768, y)`.

**Odd row** `py = 2·wy+1` (the wcoord's middle row), `wx = px/2`:
- Only if `contains(wx, wy)`: record `vert_row1` (count, or count−1 if `contains(wx−1, wy)`), emit `(x, y+384)` unless
  the left wcoord has water, then `(x+384, y+384)` and `(x+768, y+384)`.

**Position z**: 0. (The code tries `max(0, find_data_z)` but its bounds test compares `px·384` against `4·xs` and is
only true at the map corner (0,0). Treat water as flat at z = 0.)

**UV0** (`temp_t0`): `(x/768, y/768) · water_t_scale`, `water_t_scale = 1/2^WaterLevel1Scale = 1` → one texture repeat
per wcoord. (A second set with `WaterLevel2Scale` = 0.5/wcoord is built but replaced by fog UVs in 1.4.)

**Raw alpha** (`get_water_fade` @00869fd0, sampled on the height grid, `a0 = WaterColor Solid = 245`):

```
h = height[ty][tx]
if h <= -55:                a = 245
elif h <= min_clip_level:   a = trunc(245 · (h − min_clip_level) / −55)      (min_clip_level = −25)
else:                       a = 0
if a != 0 and TData(tx,ty) has the river bit (0x0800; file mask 0x02):  a >>= 2
```

`min_clip_level` ("OceanWaterMaxHeight", editor range −91…0) defaults to **−25** (TerrainOut ctor @008664e6). Note the
ramp is discontinuous: at h = −55 it jumps from 245 to 133, then falls linearly to 0 at −25.

Sample points: even-row vertices use `(tx, ty) = (4wx + {0,2,4}, 4wy)`. **Quirk:** odd-row vertices also use
`ty = 4·wy` (the wcoord's top edge), not the middle row `4wy+2`. Reproduce it; it shifts the fade half a wcoord.

Vertex colour stored = `(alpha << 24) | 0xFFFFFF` (`WaterColor Invis` 0 → RGB white; `OceanRed/Green/Blue` 255).

### 1.3 Smoothing (end of `generate_water`)

One pass over the lattice in raster order (y outer, x inner), **in place**:

```
for each lattice point k with a vertex:
    own = alpha[k]/255
    avg = mean of own and every existing vertex among the 8 lattice neighbours k±1, k±W, k±W±1   (W = 2xs+1)
    a   = min(own, avg)
    alpha[k] = trunc(a·a·255)          ← squared, written back immediately
```

Because neighbours already visited hold squared values, alpha sinks along the scan to a fixed point: open deep water
ends at **≈ 189** (0.74), not 245 (granicus/tyre2: most common values 189–192). Water touching the top or left edge of a
body keeps up to 235 (only the first rows/columns). Quirks to keep for exactness: the bounds test is against
`(2xs+1)²` (square maps), and `k±1` wraps across rows. RGB channels are averaged the same way (all white here).

### 1.4 Per-frame mesh — `modify_water` @0086e700 (called by `render_water` when the camera or fog changed)

For every visible water wcoord (fog bitmap `& 5`, `CoordInfo.flags & 1`) emit **13 vertices**:

- 0–8: the 3×3 grid from `vert_row0/1/2` (3 consecutive vertices each), row-major, row 0 = smaller y.
- 9–12: the centres of the quads (0,1,3,4), (1,2,4,5), (3,4,6,7), (4,5,7,8): position and UV0 = midpoint of the quad's
  first and last corner; colour = **mean of the four corners** (each channel, alpha included).
- Triangles (template `TerrainOut+0x477c`, built in `one_time_init`), 4 per quad, fanned around the centre:

```
(0,9,3) (0,1,9) (1,4,9) (4,3,9)    (1,10,4) (1,2,10) (2,5,10) (5,4,10)
(3,11,6) (3,4,11) (4,7,11) (7,6,11)    (4,5,12) (5,8,12) (8,7,12) (7,4,12)
```

  Winding matches the land mesh.
- Normal (0,0,1), tangent (1,0,0) for every vertex.
- **RGB = screen gradient** (alpha kept): `t = clamp(((x−x0) + (y−y0))·0.70711 / |B−A|, 0, 1)` where `A = (x0,y0)` and
  `B` are the ground points (z = 0) under the screen's top-left and bottom-left corners
  (`determine_frustum_intersect`). `rgb = lerp(WaterTop (0,159,255), WaterBot (0,111,218), t)`: top of the screen
  lighter. A port can compute `t` in the vertex shader from the same two ground points.
- **UV1 = fog**: `(fog_u(tcoord vertex), 0)` from the terrain fog texcoords; stage 1 samples the tileset
  `fow_fade_texture`.

### 1.5 graphics.txt `#NEW_TERRAIN_WATER` (read in file order by `TerrainOut::one_time_init`)

| Key | Value | Used for |
|---|---|---|
| Frame Delay (ms) | 100 | `water_effect` and `water_ripple_effect` frame time |
| Shallow Color RGB | 26, 92, 93 | nothing found |
| Deep Color RGB | 7, 81, 40 | the (undrawn) land skirt (section 6) |
| WaterColor Invis / Solid | 0 / 245 | vertex alpha base (1.2) |
| WaterColor Top/Bot/SideScreen, RiverColor | 255 / 0 / 35 / 25 | only registered as editor variables |
| OceanAlpha, OceanRed/Green/Blue | 0.95, 255×3 | vertex RGB reset to white; alpha byte untouched |
| OceanFloor{Alpha,Red,Green,Blue}{Top,Bot} | 1.0, 255×3 / 1.0, 64×3 | floor tint (2.3) |
| Water{Red,Green,Blue}{Top,Bot} | (0,159,255) / (0,111,218) | water RGB gradient (1.4) |
| OceanSwimAmp / OceanSwimFreq | 0.2 / 0.02 | **unused** in EE |
| OceanFLoorjitter / OceanFLoorSpeed | 17.0 / 0.0003 | floor UV drift (2.4) |
| OceanFadeHeight | −40 | stored, no reader found |
| OceanFloorScale / WaterLevel1Scale / WaterLevel2Scale | 3 / 0 / 1 | UV scales 1/8, 1, 1/2 per wcoord |
| Num Water Textures, Tex0…31 | 32 × `terrain art\newwater\water00NN.tga` | `water_effect` frames (only 1 loaded when `sys_grade > 2`) |

`#NEW_TERRAIN_CONST`: `coast_depth −101`, `land_height 10` feed the floor height and the land fade (2.1, 2.2).

---

## 2. Underwater terrain and the ocean floor (how the staircase is hidden)

### 2.1 Land vertex alpha — `TerrainOut::light_land_vert` @00866e50 (from `calculate_norms`, height passed in XMM3)

```
if z >= 0: keep alpha (255)
else:
    D = |coast_depth + land_height + 2 + 52| = 37
    d = max(0, −52 − z)
    alpha = trunc((1 − min(d, D)/D) · 255)      → 255 down to z = −52, 0 at z ≤ −89
```

Base Terrain blends SRCALPHA/INVSRCALPHA **with depth write on**, so faded land still writes depth.
Transitions on land use `mask.a × vertex alpha` and fade out the same way.

### 2.2 Underwater land tint — `TerrainOut::modify_ocean_floor` @00873960 (from `CoordInfo::add_geometry`)

Called only for wcoords with `OCEAN_FLOOR` (0x2000, i.e. those that got floor quads in 2.3), each time their
geometry is (re)added. It recolours the floor vertices and every land vertex of the wcoord's 5×5 patch whose alpha
byte is **< 0xDD (221)** (i.e. z < ≈ −57): that vertex gets its RGB (baked lighting) replaced by `min(255, floorTint + 99)` per channel, keeping its alpha.
`floorTint = 0.6 · lerp(OceanFloorTop (255), OceanFloorBot (64), t)` with the same screen `t` as 1.4; 99 =
`OceanVars::trans_bright` (set in `Terrain::init`). With the "Ocean Floor" graphics section off: floor colour
0x77777777 and land RGB 0x808080.

### 2.3 Floor sheet geometry — `TerrainOut::build_ocean_floor` @0086c7f0 (from `generate_land`)

- For every wcoord `(wx, wy)` with `wx, wy ∈ [−1, xs−1] × [−1, ys−1]`: add one quad (2 triangles, land winding)
  covering `[wx·768, (wx+1)·768] × [wy·768, (wy+1)·768]` if the wcoord is outside the map (−1), half water, `land` 1
  or 2, or already flagged `OCEAN_FLOOR (0x2000)`. Out-of-map quads are stored on the clamped wcoord `(max(0,wx),
  max(0,wy))`, which also gets flag 0x2000 — so the whole first row and column get floor quads too, and the floor
  extends **one wcoord past the top (y = 0) and left (x = 0) map edges only**.
- Shared vertices on a `(xs+2)×(ys+2)` lattice; **z = coast_depth + land_height − 10 = −101**; colour 0xFFFFFFFF
  (recoloured per 2.2 to `0.6·lerp(top, bot, t)`, alpha 153, no blending so alpha is irrelevant); normal (0,0,1),
  tangent (1,0,0).
- UV0 = `(gx + 10, gy + 10) / 8`, UV1 = `(xs − gx + 10, ys − gy + 10) / 8` (mirrored), with `(gx, gy)` the lattice
  point in wcoords (−1 … xs) and 1/8 = `1/2^OceanFloorScale`.

### 2.4 Floor animation ("swim") — `modify_ocean_floor_texture` @0086e320

Once per **simulation frame** (`game->frame` changed) and only with the "Ocean Floor" section on:
`j = floor_jitter · 1e−9 · rand(0..1000)` (≤ 1.7e−5), `s = floor_speed = 0.0003`;
UV0 of every floor vertex `−= (s, s + j)`; UV1 `v −= (s − j)` (u fixed). When any coordinate goes negative, +10 is added
to every coordinate of that layer (wrap). So the two mirrored layers drift slowly against each other.

### 2.5 Floor render — `render_ocean_floor` @00854dd0, state "OceanFloor" (`ocean_floor_rs`, TerrainOut+0x333c)

| | |
|---|---|
| Enables | 0x201: depth test LESSEQUAL, **no depth write**, lighting, no blending |
| Stage 0 | colour `MODULATE2X(TEXTURE, DIFFUSE)`, alpha `SELECTARG2` (diffuse); WRAP; tex = tileset OCEAN texture (Dirty: `ocean_floor_5.tga`) on UV0 |
| Stage 1 | colour `MODULATE(TEXTURE, CURRENT)`, alpha current; CLAMP (the D3D11 path keeps it); same texture on UV1 |
| Material | terrain (`prep_terrain_lighting`), lit like Base Terrain |

Result: `rgb = tex(UV0) · tex(UV1) · 2 · floorTint`, then the usual lit/HDR path. Drawn right after the land buckets
and before transitions, so it passes the depth test wherever the land below is deeper than −101 and fails where the
land is above it.

🟡 Consequence: between z ≈ −52 and −101 the land is (partly) transparent and the floor is behind it, so the HDR clear
colour (black) shows through → a **dark rim along the drop-off** under the water. It is in the code; confirm on a
screenshot before deciding whether to reproduce it.

### 2.6 Transitions and coral over the floor

- Ocean (land 2) wcoords next to shelf get the sand overlay drawn with "CoralSandy", then the same faces with the
  tileset `CORALTEX` and "CoralTrans" (TerrainOut+0x1708 / +0x15a0): enables `0x400204` (**no depth test, no depth
  write**, blend, lighting, point sampling), colour `MODULATE(TEXTURE, DIFFUSE)`, **alpha = mask alpha only** (stage 1
  `SELECTARG1(TEXTURE)`), CoralSandy WRAP, CoralTrans CLAMP. They are therefore visible over the floor sheet, while
  the shelf's own base texture is not: a soft turquoise fringe on the deep side with a stepped inner edge along the
  wcoord boundary (subtle: coral ≈ (41,198,212), floor texture ≈ (71,203,237)).
- `render_coral` @00854b40 (state "Coral", +0x1438, alpha = 1 − vertex alpha) draws `scene_coral` (+0x1870). In EE that
  helper is only cleared / `new_data` / `fill_ib` / `fill_stream`ed by `TerrainOut::render`; nothing appends faces
  (no reference in `CoordInfo::add_geometry` or anywhere else), so the pass is empty. Skip it.

---

## 3. "Normal Water" and "Min Water"

Built in `TerrainOut::one_time_init` @0085c150 into `watercontrol` (+0x665c); `render_water` @00852cd0 picks
"Min Water" unless graphics section 23 "Water Reflections" == 2.

| | Normal Water (rs "Ocean") | Min Water (rs "OceanMin") |
|---|---|---|
| Enables | `(3 \| 0x204 \| 0x384000) & ~2` = depth test LESSEQUAL, no depth write, blend, lighting, normal map, animated normals, Torrance, **screen reflections** | same without 0x200000 (no reflections) |
| Blend | SRCALPHA / INVSRCALPHA | same |
| zbias / cull | 0 / CCW | same |
| Stage 0 | colour `SELECTARG2(TEXTURE, DIFFUSE)` = vertex RGB; alpha `SELECTARG2` = vertex alpha; WRAP | same |
| Stage 1 | colour = current; alpha `MODULATE(1 − TEXTURE.a, CURRENT)`; CLAMP | same |
| Textures | t0 = current `newwater` frame (bound, **colour unused**), t1 = tileset `fow_fade_texture` (fog), t2 = `art\waterbump.tga` (normal map, sampled with sampler 0 = WRAP), t3 = `art\waterfoam.tga` (bound, **no shader reads t3**) | same |
| Material (+0x6698) | ambient (0.2,0.2,0.2), diffuse (1,1,1.5), specular (40,40,40), emissive 0, **power 0.3** | same |
| Vertex format | position, normal, diffuse, UV0, UV1, tangent (0x203000d) | same |

0x4000/0x80000/0x100000 need vertex tangents (always present on the D3D11 card). When the local player is an
observer/out of the game (semaphore 0x42 or player flags 0x100/0xD0, not tournament) stage 1's alpha op is switched to
SELECTARG2 for the draw (fog ignored), then restored.

**Pixel colour** (ps_global, h = 5; see lighting-spec §5):

1. `c = saturate(vertex colour)`, `a = vertex.a · (1 − fow_fade.a(UV1))`.
2. Normal: three waterbump samples on UV0, `T = gCurrentTime / 40` (seconds):
   `nA` at `(u·0.1, v·0.05 − 0.7T)`, `nC` at `(u·0.4 − 0.6T, v·0.4 + 0.5T) + 0.5`, `nD` at `(u, v)·0.8 + 0.8T + 0.3`,
   each `rgb·2−1`; `n = normalize(nA + nC + nD)`; `N = n.x·T̂ + n.y·B̂ + n.z·N̂` with view-space T̂ = (1,0,0),
   N̂ = (0,0,1), B̂ = cross(T̂, N̂).
3. ambient `= 5·0.2·gAmbientLight = (0.6, 0.716, 0.745)`; diffuse `= 5/π·(1,1,1.5)·Sun·max(0, N·L)` (Sun at full
   white during this phase).
4. Torrance–Sparrow specular with m = 0.3, specColour 40, Schlick r0 = 0.0209, unclamped (lighting-spec §5).
5. Normal Water only: 10-step screen-space reflection into the **previous frame's** quarter-res HDR colour/depth
   (lighting-spec §5, §8.6), weighted by `saturate(N·V)`.
6. `rgb = c^1.45 · (diffuse + ambient) + specular [+ reflection]`, alpha `a`, blended over the HDR target.

**Recommendation:** start with **Min Water**. It is Normal Water minus the reflection term (which needs the
previous-frame quarter-res colour and depth copies and is the only cross-frame dependency). Order of work: flat
N = (0,0,1) + vertex colour + alpha (gets the look of the coastlines right), then the animated waterbump normals and
Torrance specular (one texture, `gCurrentTime`), then reflections last.

---

## 4. River water

- `RiversOut::render_water` @008883e0 → `RiverSectionOut::render_water` @0088ce80 for each section rendered this frame,
  right after the sea water.
- Geometry (`RiverSectionOut::create_water_plane` @0088b710): polygons at **z = 0** spanning the section's trough
  vertices; vertex colour white, alpha 255 or 0 (open section ends fade, from section flags 0x10/0x40); UV0 =
  `world/768 · water_t_scale`, UV1 = the river stretch/flow coordinates.
- State "RiverWater" (static `water_rs`, `RiverSectionOut::one_time_init` @0088bc20): enables 0x5 (depth test, no
  write, blend; **no lighting, no TL_FAKE_LIGHT**, so it writes LDR values into the HDR target — lighting-spec open
  question 2), stage 0 `MODULATE(TEXTURE, DIFFUSE)` / alpha diffuse, stage 1 colour current / alpha
  `MODULATE(TEXTURE, CURRENT)`, both WRAP; t0 = current `newwater` frame, t1 = tileset `river_water_tex`.
- **This is where the `newwater` frames are visible.** Frame index = `water_effect.cur_texture`, advanced by
  `TextureEffect::increment` @00542500 once per 100 ms of graphic clock (`TerrainOut::render`), wrapping at 32.
- The sea mesh also covers river wcoords (1.1 rule 4), but river tcoords divide its raw alpha by 4 and the squaring
  takes it to ≈ 0, so the sea mesh is practically invisible on rivers.

---

## 5. Surf

### 5.1 Data (`Data/effects_graphics.xml` → `<EFFECTS><SURF wavetime="250" stalltime="75">`, case-insensitive)

- `TEXTURE name file`: 13 named textures (`art\surf_*.tga`).
- `SURFTIMING name` → list of `TIME percent alpha` keys; key k pairs with keyframe k of the `.mot` model.
- `WAVE type`: "Breaker" → shape 0, anything else ("Large") → shape 1; `WAVESECTION model tex0 tex1 renderstate
  world_coord_mapping timing`. `renderstate` ∈ {Wash, Crash, Wave} → render state **47 + index** (renderstates.md).
  Sections are stored per wave **indexed by tex0's texture index** (empty slots skipped); one batch per texture, with
  that texture on stage 0 and its `tex1` mask on stage 1 ("one alpha per main texture").
- `SURFTYPE name dist_from_shore dist_along_shore`, in file order: [0] Breaker 700 / 1000, [1] Wave 320 / 400
  (`dist_along_shore` clamped to ≥ 192).
- Registered tunables (`Surf::registration` @008a09b0): `location_offset` = 24, `world_texx_speed` = (−1/1200, 0),
  `world_texy_speed` = (0, −1/1200), `position_noise` = 0.

### 5.2 `.mot` model (`SurfWaveSection::init` @0089e8f0 via `MOTMethod::load`)

Chunked like BH3 (`u32 size, u16 id, u16 aux`). Root id 0, children:
- id 11: `u32 frames, u32 nIndices, u16 indices[nIndices]` (triangle list, e.g. 10 triangles over 12 vertices);
- `frames` × id 12 containers, each with id 2 `{u32 n, f32 xyz[n]}` (only x, y kept) and id 4 `{u32 n, f32 uv[n]}`.

Frame counts match the timing key counts (breakers 14 = SURFTIMING12, `0wave` 11 = SURFTIMING13). If all frames carry
identical UVs and `world_coord_mapping` is 0, the section uses frame 0's UVs (`world_tex_coords = −1`).

### 5.3 Placement — `Surf::init` @008a0cf0 (from `Terrain::init`)

Source: `TerrainData::sorted_waterline_tcoords` (+0x47c0), i.e. **chunk 90 of the scenario**. It is rebuilt
(`check_and_set_halfland` → `sort_waterline_wcoords`) only for freshly generated terrain (`Terrain::init` param 2 = 0).
`ScenarioRead::import` passes 1 whenever heights are stored (@009b231e), so a scenario with an empty chunk 90 has **no
surf** (granicus, tyre2 and 145 of the 156 `.scx` files a quick scan parsed; 11 have usable lists, e.g. tut1, cuba2, jhelum3; random maps always get surf).

For each waterline with **> 15 tcoords**:
1. Spline through every 4th tcoord `(tx·192, ty·192, 0)` (`Spline::calc_spline`). `n` = the spline normal at a point
   (offset side = sea; tut1's lists always have water on that side, and breakers require it).
2. **Waves**: `t0 = rand(−75, 250)`. Walk the spline in steps of `dist_along[Wave]/2 = 200`: at point P, `Q = P + 320·n`;
   unless Q is inside the map on a river tcoord (and `enter_river_deltas` is 0), add a shape-1 wave at Q with phase t0,
   advance another 200 and add a second wave with phase `((325/2) + t0) % 325 − 75` (half a cycle later).
3. **Breakers**: walk in random steps in [500, 1000]; `Q = P + 700·n`; add a shape-0 wave if Q is in the map and its
   tcoord is water (`TData & 0x30 == 0x20`), phase `rand(−75, 250)`.

`add_wave` @0089d5a0: angle `a = angle(n) + 180°` → `x_axis = (cos a, sin a)` (points back to shore),
`y_axis = (cos(a+90°), sin(a+90°))`; model = random wave of that shape; `time = ((int Qx + int Qy)/24 + phase) % 325 − 75`.

### 5.4 Animation and drawing

- `Surf::inc_time` @008a1a00 (per simulation frame, from `Objects::inc_time`): `time += 1`; at `time ≥ 250` pick a new
  random model of the same shape and set `time = −75` (5 s stall at 15 Hz).
- `Surf::draw` @0089d6f0 (after river water): waves are culled in pairs by the explored-wcoord bitmap; rebuilt when the
  camera, fog or game time changed.
- `SurfWave::draw` @0089df10: skip if `time < 0`; `p = (time·200/3 + graphic_dt) / (250·200/3)` (ms; 200/3 ms per
  simulation frame).
- `SurfWaveSection::draw` @0089e360: find keys k0 ≤ p < k1 in the section's timing (binary search); `f = (p_k1 − p)/(p_k1 − p_k0)`;
  alpha `= trunc(alpha_k1·(1−f) + alpha_k0·f)` for every vertex (colour white); vertex `(vx, vy) = lerp(frame k1, frame
  k0, f)`, world `= Q + vx·x_axis + vy·y_axis`, **z = 0**. UV0: mapping 0 → lerp of the two frames' UVs; −1 → frame 0;
  mapping m ∈ {1, 2} → `(wx/576 + xoff[m−1], wy/576 + yoff[m−1])` with `xoff = frame·world_texx_speed`,
  `yoff = frame·world_texy_speed` (`frame` = simulation frame). UV1 (mask) = frame 0 UVs.
- States 47 Wash / 49 Wave (identical) and 48 Crash: enables 0x20085 (depth test, no write, blend, alpha test ALWAYS,
  TL_FAKE_LIGHT), **zbias 4** (view z − 32), SRCALPHA/INVSRCALPHA; rgb = tex0 (× white), alpha = tex0.a × vertex.a
  × mask.a (Crash: no mask stage); shader: `rgb = c^1.9 · 5`.

---

## 6. Map border and outside the map

- `TerrainOut::generate_skirts` @008603c0 (from `Terrain::init` when the world size changed) builds two meshes:
  - `land_skirt`: four 1536-wide quads around the map (`[−1536, W+1536] × [−1536, 0]`, `[−1536, 0] × [−1536, H+1536]`,
    `[−1536, W+1536] × [H, H+1536]`, `[W, W+1536] × [−1536, H+1536]`, W = 768·xs, H = 768·ys) at `z = sea_depth`
    (−380, set in `TerrainOut::close`; static initial value −1140), colour graphics.txt **Deep Color** (7, 81, 40), alpha 255.
  - `water_skirt`: four quads at z = 0 extending 3072 out and slightly into the map (`[−3072, W+3072] × [−3072, 192]`,
    `[−3072, 192] × …`, `… × [H−384, H+3072]`, `[W−192, W+3072] × …`), colour `DARK_GREY`.
  - State "Water Skirt" (+0x3ff4): lit, no blending, default stage 0, depth test + write.
- **Neither mesh nor the state is used by any draw call in EE** (instruction scans for their stream/IB/state fields
  and absolute addresses find only `generate_skirts`, `clear_tileset_vars`/ctor/dtor and `one_time_init`).
- So outside the map EE shows the HDR clear colour (black → black after tone mapping), except the ocean-floor ring one
  wcoord wide beyond the x = 0 and y = 0 edges (2.3), which has no water over it. Fog of war (`render_fow`) and
  `TerrainBorder::render` (national borders) only cover map wcoords.
- Port: draw nothing (match EE), or, if the camera can see far past the edges, optionally draw the land skirt as an
  opaque Deep Color sheet at z = −380 as a cosmetic fallback (not EE behaviour).

---

## 7. Port recipe (Metal)

1. **Land**: per height vertex alpha from 2.1; recolour vertices with alpha < 221 per 2.2 (screen `t`); keep depth
   write on while blending.
2. **Ocean floor** right after land: mesh 2.3, state 2.5 (depth test, no write, no blend), `rgb = tex0·tex1·2·tint`,
   lit like terrain; per simulation frame drift UVs (2.4) — or in the vertex shader from the frame number.
3. **Transitions**: unchanged, but land overlays must use `mask × vertex alpha`; ocean overlays (CoralSandy/Trans)
   ignore depth and vertex alpha.
4. **Sea water**: build the lattice (1.2), smooth in place (1.3), emit 13-vertex patches (1.4) once per map; compute
   the gradient `t` and `rgb` in the vertex shader from the two ground points of the current camera. State: Min Water
   (section 3). Draw after objects and particles.
5. **River water**, then **surf**, as in sections 4–5.
6. Clear to black; no skirts.

---

## 8. Evidence

| What | Where |
|---|---|
| Lattice, row indices, raw alpha calls, odd-row `ty` quirk, z bounds bug | `TerrainOut::create_water_verts` @0086ceb0 (asm in `re/decomp/water/asm/`) |
| Smoothing (min, squared, in place, 3×3) | `TerrainOut::generate_water` @00861980, asm @008620c2–00862125 |
| Fade function, river ÷4 | `TerrainOut::get_water_fade` @00869fd0 |
| `min_clip_level` = −25, editor name/range | `TerrainOut::TerrainOut` @008664e6; `TerrainOut::registration` @00865930 (internal string "OceanWaterMaxHeight", −91…0; "RenderWater" = `should_render_water`) |
| Water wcoord test | `TerrainOut::coord_contains_water` @0086a080 |
| 13-vertex patch, colour gradient, fog UV1, normals/tangents | `TerrainOut::modify_water` @0086e700; template faces in `TerrainOut::one_time_init` @0085c150 (~0085eff6); `determine_frustum_intersect` @00854770 |
| Techniques, stages, material, graphics.txt order | `TerrainOut::one_time_init` @0085c150 (internal strings "Normal Water", "Ocean", "Min Water"/"OceanMin", "NEW_TERRAIN_WATER", "graphics.txt") |
| Technique choice, textures t0–t3, observer fog override | `TerrainOut::render_water` @00852cd0; waterbump/waterfoam loaded in `Terrain::init` @00850f70 (`water_normal_map`, `water_foam_map`) |
| Shader maths | `shaders\ps_global.hlsl` (`ANIMATE_WATER_NORMALS`, `SPEC_MODEL_TORRANCE`, `HAS_SCREEN_REFLECTIONS`), `vs_global.hlsl` (bitangent) |
| Land fade | `TerrainOut::light_land_vert` @00866e50, caller `calculate_norms` @008710b0 (height in XMM3 @0087175b) |
| Underwater tint, floor colour | `TerrainOut::modify_ocean_floor` @00873960 (from `CoordInfo::add_geometry` @0084bef0); `trans_bright = 99` in `Terrain::init` |
| Floor mesh, z, UVs | `TerrainOut::build_ocean_floor` @0086c7f0 (UVs @0086cb3a–0086cbb5) |
| Floor drift | `TerrainOut::modify_ocean_floor_texture` @0086e320 |
| Floor state and draw | `ocean_floor_rs` setup in `one_time_init`; `TerrainOut::render_ocean_floor` @00854dd0 |
| Coral states; coral pass empty | `one_time_init` ("CoralTrans", "CoralSandy", "Coral"); `render_coral` @00854b40; InsnGrep for +0x1870 → only `TerrainOut::render` clear/fill calls |
| River water | `RiversOut::render_water` @008883e0, `RiverSectionOut::render_water` @0088ce80, `::one_time_init` @0088bc20, `::create_water_plane` @0088b710 |
| Frame animation | `TerrainOut::render` @008558b0 (100 ms), `TextureEffect::increment` @00542500 |
| Surf | `Surf::init_class` @0089f050, `Surf::init` @008a0cf0, `Surf::add_wave` @0089d5a0, `Surf::inc_time` @008a1a00, `Surf::draw` @0089d6f0, `SurfWave::draw` @0089df10, `SurfWaveSection::draw` @0089e360, `SurfWaveSection::init` @0089e8f0, `Surf::registration` @008a09b0; data globals `location_offset` 0x00c062ac, `world_tex{x,y}_speed` 0x00c062b8/0x00c062b0 |
| Waterlines only from chunk 90 for stored-height scenarios | `ScenarioRead::import` @009b1430 (CL = 1 @009b231e), `ScenarioEditor::init_terrain` @009a19d0, `Terrain::init` (param 2), `ScenarioRead::load_sorted_waterline_data_chunk` @009ad8a0 |
| Skirts unused | `TerrainOut::generate_skirts` @008603c0; InsnGrep over all instructions for the stream/IB/face fields (+0x4724…+0x4778) and absolute addresses 0x00c0f814…0x00c0f87f; "Water Skirt" state (+0x3ff4) referenced only in `one_time_init` |
| Draw order | `Scene::render` @008b3270 (… particles → `render_water` → `RiversOut::render_water` → `Surf::draw` → underlays) |
| Field names | `re/analysis/types_terraindata.txt`, `re/analysis/types_water.txt` (DumpTypes) |

Decompiles: `re/decomp/water/` (+ `asm/`). Jobs: `re/jobs/dc_water.txt`, `da_water*.txt`, `ig_*.txt`, `cl_water*.txt`.

---

## 9. Verification — `re/tools/water-preview.mjs`

`node re/tools/water-preview.mjs <file.scx> [--px 32] [--crop x,y,w,h] [--hist]` writes
`re/previews/<map>_water_{alpha,alpha_ours,orig,ours}.png` (top-down; Lambert lighting, no HDR; screen `t` stands in as
`(x+y)/(W+H)`).

| Map | Water wcoords | Vertices / triangles (orig) | Deep alpha after smoothing | Jaggedness orig / ours |
|---|---|---|---|---|
| tut1 (24²) | 200 | 916 / 3200 | 187–190 | 1.015 / 1.065 |
| granicus (60²) | 1352 | 6021 / 21632 | 182–191 | 1.013 / 1.026 |
| tyre2 (60²) | 3005 | 12404 / 48080 | 189–192 | 1.009 / 1.020 |

Jaggedness = length of the alpha 0.2 iso-line divided by that of a box-blurred copy. Visually: `*_water_alpha_ours`
shows diagonal sawteeth along every coast; `*_water_alpha` is smooth (wider, softer ramp). `*_water_orig` shows no
sand/ocean staircase in the sea, only the faint stepped coral fringe and the dark drop-off rim (2.5).

---

## Open questions

1. 🟡 Screenshot check of the dark drop-off rim (2.5) and of the coral fringe; both follow from the code, neither has
   been seen.
2. Deep-water alpha ≈ 0.74 depends on reading the smoothing as in place; the disassembly reads and writes the same
   colour array, but compare the sea's transparency with a screenshot.
3. The floor-sheet ring exists only beyond the x = 0 / y = 0 edges (loop bounds −1 … xs−1). Is that visible in game, or
   does the camera clamp hide it?
4. `Spline::build_normals` side convention was not decoded; the seaward side is inferred from data (tut1) and from the
   breaker water test.
5. Surf timing assumes 200/3 ms per simulation frame (the constant in `SurfWave::draw`); confirm against
   `TurnControl` pacing.
6. The river water polygon layout (which trough vertices, the stretch UV1) is summarised only; see the rivers work in
   `re/decomp/dressing/` for the trough itself.

**Port decision (2026-10-12):** `ronmap` draws the ocean-floor sheet *before* the land rather than after it, because the
code-implied dark rim (open question 1) rendered as a black ring around every coast. With the floor first, faded
underwater land blends over the floor. Revert to the original order if an in-game screenshot shows the rim.
**Port decision (2026-10-12):** the one-wcoord ocean-floor ring past the x = 0 / y = 0 edges (2.3, open question 3)
is not built: the user, who knows the original, saw it as an out-of-place water border around the map.
**Port decision (2026-10-12):** the ocean transition overlays (CoralSandy + CoralTrans, 2.6) render as hard-edged
turquoise bands over deep water in our build, not a faint fringe, so `ronmap` skips them unless `RON_CORAL=1`.
Revisit with an in-game screenshot (the floor sheet's MODULATE2X brightness vs the coral texture is the likely mismatch).
