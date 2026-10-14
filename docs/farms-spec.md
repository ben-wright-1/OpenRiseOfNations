# Farms spec: wheat boxes, farm underlays, growth, animal pens

Status key as in [formats.md](formats.md): ✅ verified against real files or by rendering · 🟡 read from code, not yet
verified · ❓ open. Render-state numbers refer to [renderstates.md](renderstates.md). Evidence (functions and addresses)
is at the end. Implementation: `engine/render/farms.{hpp,cpp}`, drawn by `apps/map-mac/main.mm` (`ronmap`).

**Summary.** A farm has **no building sprite**. `WallOut::draw` skips the sprite block for buildings that `is(FARM)`
(type 417, 0x1a1). The `texture`, `spritexoff`, `spriteyoff` and `mask` of `<FARM>` in `building_graphics.xml` are not used
for drawing: `Farms::init` reads `texture` into a throwaway global and keeps only the stalk colour and the two heights.
A farm is drawn as two layers:

1. A **ground underlay** over its 4×4-tile footprint: the tileset's `wheatfarm` texture (or `pigfarm` for an animal pen),
   drawn with the terrain's "Weathering Underlay" state right after the terrain.
2. For wheat farms, **16 open boxes of wheat**, one per tile. Each box is 192×192 units and between 30 and 108 units tall,
   depending on that tile's growth. The boxes are drawn after all objects with the farm's own state "Farm Blend". Their
   texture is the tileset's `FARMTEX`, not `<FARM texture>`, although both are `art/farm_wheat_1.tga` in most tilesets.

Our old renderer drew `farm_wheat_1.tga` upright as a 384×384-unit building sprite. The game lays it out as a field of
low, wide boxes over 768×768 units, which explains the "taller and smaller" look.

---

## 1. Data

| Source | What | Used as |
|---|---|---|
| `building_graphics.xml <FARM stalk_red stalk_green stalk_blue stalk_alpha min_height max_height>` | stock values 166, 216, 188, 173, 10, 36 | stalk tint, and heights `min_height·3 = 30` and `max_height·3 = 108` world units (`Farms::init`) |
| `tilesets.xml <TILESET><FARMTEX><TEXTURE file>` | `art/farm_wheat_1.tga`; snowy tilesets use `farm_snowy_wheat_1.tga`, jungle tilesets `farm_jungle_wheat_1.tga` | the wheat texture (`TileSet::init` → `TileSetData::wheat_texture`, 32-bit, 2 mip levels; `TileSet::get_wheat_texture`) |
| `tilesets.xml <UNDERLAYS><TEXTURE id="wheatfarm">` / `id="pigfarm"` | `common_doobers/farm_wheat.tga` (four dirt rows), `farm_animals.tga` (`farm_j_*` in jungle tilesets) | ground underlays (UNDERLAYS index 1 and 0) |
| rules | FARM is buildtype 417, footprint `X_SIZE = Y_SIZE = 4` | footprint |

**`farm_wheat_1.tga` layout** (128×128, v = 0 at the top row as stored for D3D):

- Top half (v 0…0.5): two 64×64 top-view tiles side by side, at u 0…0.5 and u 0.5…1.
- Bottom half (v 0.5…1): one 128×64 side view of the stalks. The ears are at v = 0.5 and the stalk bases at v = 1.

The `<FARM><DEFAULT><AGE0 mask="4x4 gather S" doobermask=...>` entries are pathing and gather masks, not drawing data.

## 2. Placement and per-farm state (`Farms::add`, `FarmStruct`, 0xC0 bytes) ✅

- **Footprint.** The scenario stores the snapped centre (scenario-objects-spec §3). The size is even, so `cx` and `cy`
  are multiples of 192, which puts the centre on a tile corner. Then `tx0 = cx/192 − 2` and `ty0 = cy/192 − 2`. The footprint
  covers tiles `tx0…tx0+3` × `ty0…ty0+3`, which is `[cx − 384, cx + 384)` on each axis. Cell (x, y) is centred at
  `((tx0+x)·192 + 96, (ty0+y)·192 + 96)`. `FarmsOut::draw` repeats the snap (`div_3_table[x>>6]·192`, minus `size>>1` tiles,
  plus 96). Its loops are hard-coded to 4×4 cells (0…0x300 in steps of 0xC0).
- **`terrain_height[5][5]`**: `TerrainOut::find_data_z` at the 25 tile corners, indexed `[x][y]`. `Farms::add` fills it
  for wheat farms, and `Farms::recalc_heights` refills it when the terrain under a farm changes (called from `Wall::init`).
  A port reads the terrain grid heights at `(tx0+i, ty0+j)`.
- **`percent[4][4]`** (growth 0…1) and **`status[4][4]`** (0 bare, 1 growing, 2 ripe, 3 being harvested), both indexed
  `[x][y]`. `Farms::add` sets them all to 0, so **a farm in a freshly loaded scenario is all bare stubble** (§3.3).
  Scenarios do not store growth.
- **`farm_type`** (+0xBD): 0 wheat, 1 animal pen. Bit 4 means a wheat farm with an ambience sound
  (`GraphicEvents::add_ambience`). `valid` is at +0xBC, and `who`/`o` at +0/+4.

### 2.1 Wheat or animal pen (`Farms::add`, `CityData::count_farms`, `FarmsData::get_nearest_farm_type`) 🟡

`Build::init` first finds the farm's city (`Build::find_city` → `BuildTypeData::get_town` → `ObjectsData::find_city_at`).
That is the nearest city of the same owner within `CityData::get_radius`, which is `CITY_CENTER_RADIUS` (20 tiles)
+ (level − 1)·`CITY_CENTER_POP_RADIUS` (4), plus an Indian tribe bonus, capped at 64 tiles. Level is 1 for Small City,
2 for Large City and 3 for Major City. Then `Farms::add` decides the type:

- **No city:** an animal pen if the object slot `o` is odd, else wheat.
- Otherwise let *n* be the city's other farms and *w* those of them that are wheat (`count_farms` counts finished
  non-animal farms):
  - *n ≠ w* (the city already has a pen): **wheat**.
  - *n = w = 4* (the fifth farm, the other four wheat): **animal pen**.
  - Else the type of the nearest other farm within 1152 units (`find_any_building`, radius 0x480), if there is one.
  - Else *n = w = 3*: **animal pen**.
  - Else **random**: `game_random & 3 == 3` gives a pen (1 in 4), otherwise wheat.

So a city usually gets one pen, normally as its fifth farm. **Port:** `place_farms` applies these rules in scenario order.
It uses Euclidean centre distance (the game's `vector_dist` metric is not checked) and takes the random case as wheat,
because the game's synchronised RNG stream cannot be reproduced without the simulation. On granicus this gives 3 pens for
18 farms, one per five-farm city.

## 3. Wheat geometry (`build_box` @008d6140, `FarmsOut::draw` @008d75f0) ✅

### 3.1 The unit box (`build_box`, run once from `Farms::init`)

There are 10 vertices with x, y = ±96 and z = 1 (the wheat top) or 0 (the ground). Six triangles: the top quad, the **+y side**
and the **+x side**. Only the two faces that point toward the camera exist (the camera looks toward −x, −y).

| # | (x, y, z) | corner (col, row) | wind | UV set 0 | UV set 1 |
|---|---|---|---|---|---|
| 0 | (−96, −96, 1) | (0, 0) | yes | (0.5, 0) | (1, 0) |
| 1 | (96, −96, 1) | (1, 0) | yes | (0.5, 0.5) | (1, 0.5) |
| 2 | (−96, 96, 1) | (0, 1) | yes | (0, 0) | (0.5, 0) |
| 3 | (96, 96, 1) | (1, 1) | yes | (0, 0.5) | (0.5, 0.5) |
| 4 | (−96, 96, 1) | (0, 1) | yes | (0, 0.5) | (1, 0.5) |
| 5 | (−96, 96, 0) | (0, 1) | no | (0, 1) | (1, 1) |
| 6 | (96, 96, 1) | (1, 1) | yes | (0.5, 0.5) | (0.5, 0.5) |
| 7 | (96, 96, 0) | (1, 1) | no | (0.5, 1) | (0.5, 1) |
| 8 | (96, −96, 1) | (1, 0) | yes | (1, 0.5) | (0, 0.5) |
| 9 | (96, −96, 0) | (1, 0) | no | (1, 1) | (0, 1) |

Faces: (0,1,2) (2,1,3) top; (4,6,5) (5,6,7) +y side; (6,8,7) (7,8,9) +x side. With the game camera these are
clockwise on screen, which makes them front faces under CULL CCW. The tables are `wheat_box_verts`, `wheat_box_cols`,
`wheat_box_rows`, `wheat_use_wind`, `wheat_box_faces` and `wheat_box_texs[2]`.

- The top maps one 64×64 top-view tile. Set 0 uses the left tile and set 1 the right one. u follows world y and v follows
  world x, which is a rotated mapping.
- The two sides take the 128-wide side strip together: 0…0.5 on the +y face and 0.5…1 on the +x face, so the strip wraps
  around the corner. Set 1 mirrors it.
- **Cell (x, y) uses set `(x + y) & 1`**, a checkerboard.

### 3.2 Per cell, every frame (the detailed path)

For each wheat farm (`farm_type != 1`), rows y = 0…3 run on the outside and cells x = 0…3 on the inside. That is back to front for this
camera. With `g = percent[x][y]`:

- `height = min_h + (max_h − min_h)·g`, from 30 to 108 units.
- Vertex position: `(cell_cx + vx [+ sway·wind.x], cell_cy + vy [+ sway·wind.y], vz·height + terrain_height[x+col][y+row])`.
  The box follows the terrain at its corners, and the sides rise from the ground at each corner.
- **Colour** (vertex diffuse, all 10 vertices; `get_global_color` is an identity copy):
  - `g < 0.25`: the stalk RGB (166, 216, 188), with alpha `(255 − a)·4g + a·build`. Here a = stalk_alpha (173) and build is the
    construction progress `hp/max`, or 1 for a finished farm.
  - `g = 0.25` exactly: the stalk colour with alpha a (the code leaves this case untouched).
  - `0.25 < g ≤ 0.75`: each channel is `c + (255 − c)·2(g − 0.25)` (truncated), with alpha 255. The tint fades to white.
  - `g > 0.75`: WHITE, so ripe wheat shows the texture unmodified.
- **Wind sway** applies to the 7 top vertices only. `phase = (frame + 5y + 17x + tx0 + ty0) & 63` over sim frames,
  `t = phase < 32 ? phase : 64 − phase`, and `sway = (t/16 − 1)·25000·g`. The offset is `sway·wind_vec.xy`.
  `Scene::render` sets `wind_vec = (4, 4, 3)·1.4925374e-5` rotated about z by `wind_angle` (an oscillation clamped to
  ±45°), so the amplitude is at most about 2 world units, less than a pixel. **Port:** wind angle 0.
- The vertex, UV and colour arrays are rebuilt only when the frame time, camera, fog or `recalc_builds` change. The face list is
  appended once per cell with a vertex offset (`append_with_offset`).

**Simple path** (scalability `section_render[12]` = BUILDINGS off; config_defaults has it on for grades A–C): there is no
per-cell growth and no wind. A finished farm draws every cell at `max_h` in WHITE. An unfinished one draws at `min_h` in
the stalk colour. The port implements only the detailed path.

### 3.3 Growth over time (`Farms::inc_time`, per sim frame from `Objects::inc_time`) ✅ decoded

The farm must be finished (Build flag 4) and not an animal pen.

1. Every cell: status 1 adds 0.005 and status 3 subtracts 0.01. At ≥ 1 the cell becomes 1.0 with status 2 (ripe). At ≤ 0 it
   becomes 0.0 with status 0 and counts as **bare**.
2. With *b* bare cells, the chance per mille is 20 for b ≥ 12, `(b − 4)·20/8` for 5 ≤ b ≤ 11, and none below 5.
   `game_random % 1000 < chance` sets one bare cell (picked by `game_random % b`, counted in y-outer, x-inner order) to
   status 1.
3. Farmers act through `Unit::do_gather`: `Farms::grow` adds 0.005 and sets status 1 (2 at full), and `Farms::snip` turns a
   ripe cell (2) into 3, which then shrinks back to bare.

At 15 sim frames a second, a cell grows from bare to ripe in about 13 s. An unattended farm fills until four cells are left bare
(about 20 s per new cell at first). **A freshly loaded scenario therefore shows every farm as low, translucent green-tinted
stubble** (30 units tall, alpha 173 × texture alpha, the texture times (166, 216, 188)/255). The golden fields people
remember are farms that have been worked or left to grow for a minute or more.

**Port:** `farms_inc_time` copies these rules with the game's LCG on its own seed. `ronmap` runs it in real time (15 per
second) and `--farm-time S` runs S seconds before a snapshot.

## 4. Render states ✅

**"Farm Blend"** is a `RenderState` that belongs to the global `farms` object (at `farms + 0x70`) and is not in the 50-entry table.
`Farms::init` starts from the `RenderState::init` defaults and sets:

| Field | Value |
|---|---|
| enables | 0x3 \| 0x20084 = **0x20087**: depth test and write, blend, alpha test, TL_FAKE_LIGHT |
| blend | SRCALPHA / INVSRCALPHA |
| alpha test | the default ALWAYS 0, so nothing is discarded. Fully transparent texels still write depth |
| depth | LESSEQUAL, write on; cull CCW |
| stage 0 | colour MODULATE(TEXTURE, DIFFUSE), alpha MODULATE(TEXTURE, DIFFUSE) (colorop[0] = alphaop[0] = 3) |
| stage 1 | disabled |
| filters | mag/min/mip POINT (ignored by the D3D11 backend) |
| pixel-shader situation | ("Nuke", `ps_luminance_CtcAtc`), legacy and dead in EE |

The draw sets the texture to stage 0 = `cur_tileset->wheat_texture` and stage 1 = none, uses an identity world transform and a
triangle list with no normals, and binds an index buffer it creates on first use.

**Underlay:** `TerrainOut::add_weather_underlay(tx0, ty0, 4, 4, name, alpha, …, mode 0, step 1, aligned 0)` is drawn by
`render_weathering(0, 3)` with **"Weathering Underlay"** (`TerrainOut + 0x4bf4`, terrain-dressing-spec §1.5): no depth
test, blended, fake-lit, rgb = tex(UV0) × diffuse, a = tex(UV1).a. Mode 0 means no quadrant atlas, so **UV0 = UV1 = (i/4, j/4)**
over the footprint's 5×5 grid vertices, with u along x. `alpha` is 1 for wheat farms. For pens it is the construction fade
(opaque below 84 %, fading out by 88 %), but stage 1 overwrites the vertex alpha anyway.

## 5. Draw order (`Scene::render`) ✅

`TerrainOut::render` → `Doober::draw_weathering` → **`ObjectsOut::draw_weathering`** (collects the farm underlays via
`WallOut::draw_weathering`) → **`render_weathering(0, 3)`** → roads → nuke underlays → river troughs → … → mountains,
snowcaps → doobers → … → objects (`draw_all_zoomed_in` / `pop_all_zoomed_in`) → `draw_ammo` → **`FarmsOut::draw`**
(if `draw_flags[1] & 0x10`) → `GrassClumps::render` → particles → sea and river water → surf.

The wheat is drawn after every unit and building. Because it writes depth, units standing in a field are hidden by the
wheat in front of them and show above it. Farms are visited in the render-object tree's order, which the port takes to be
x + y like the other objects (🟡). Within a farm the order is back to front.

## 6. Animal pens (`Farms::add_animals`) 🟡

These are drawn only as the `pigfarm` underlay, with no boxes. When a pen's building is finished, five animals are created for
player 9 (nature) at `centre + (rand % 384 − 192)` on each axis: a `Farm Pig` (graph FARMPIG) or `Farm Chicken` (FARMCHICKEN),
50/50 from `game_random`. The unit graphics have TYPE0/TYPE1 variants (pig / black pig, hen / rooster). **Port:** a
position-seeded stream picks the type, variant, offset and facing. The game's variant selection and initial facing
are not traced.

## Port recipe

1. Load `FarmParams` from `<FARM>`, and the tileset's `FARMTEX`, `wheatfarm` and `pigfarm` textures.
2. `place_farms(terrain, farm sites in scenario order, cities)` gives the corner heights, type and zero growth.
3. Once: build the underlay meshes (`build_farm_underlays`). Draw them with Weathering Underlay right after the terrain
   (before river troughs). Place the pen animals as units.
4. Each sim frame: `farms_inc_time`. Each render frame: `build_wheat(farms, params, frame)`. Draw it with Farm Blend after
   the objects and before water.

Snapshot check: `ronmap … granicus.scx --zoom 6 --at 6300,19400 --snapshot a.png` (stubble at t = 0) and the same with
`--farm-time 120` (mostly ripe fields with bare patches).

## Open questions

1. **Random pens.** For a city's first farm with no farm within 1152 units, the 1-in-4 pen comes from `game_random`.
   It is replaced by wheat.
2. **City distance metric** (`vector_dist`) and the +100 penalty in `find_city_at` for limited building types were not
   checked. Farms near the edge of a city's radius could pick the other rule branch.
3. **Wind angle**: `Scene::render`'s sin/cos oscillation is garbled in the decompile. It moves the wheat by at most about
   2 units, so it is ignored.
4. **Render-object order between farms** (the tree at `objects + 0x248`) is assumed to be x + y. It only matters where the
   transparent texels of one farm's boxes overlap another farm, because they write depth.
5. Farmers' `grow`/`snip` timing (`Unit::do_gather`) is not modelled. A port with simulation should call them from the
   gather job.

## Evidence

| What | Where (riseofnations.exe, rise.pdb names) |
|---|---|
| Box geometry, UV sets, wind mask | `build_box` @008d6140 (farms.obj), called from `Farms::init` @008d9310 |
| XML attributes, ×3 heights, "Farm Blend" fields, `farms + 0x70` RenderState | `Farms::init` @008d9310 (internal strings 0xd0d4 "Farm", 0xd0e8–0xd14c stalk_*/min_height/max_height, 0xd160 "Farm Blend") |
| Per-cell draw, colour ramp, wind, texture and state binding, simple path | `FarmsOut::draw` @008d75f0 (`section_render[12]` gate; `cur_tileset + 0x9b8` = `wheat_texture`; GSGraphicCard slots 0x20 set_texture, 0x24 set_render_state(`farms + 0x70`), 0x2c prim type, 0x34/0x4c/0x48 vert/uv/colour arrays, 0x60 index buffer, 0x80 draw) |
| FarmStruct layout, zero init | `FarmStruct::FarmStruct` @008d9890, `Farms::add` @008d8a40 (memset of percent/status) |
| Type decision | `Farms::add`, `CityData::count_farms` @007368c0, `FarmsData::get_nearest_farm_type` @008d73a0, `Build::find_city` @00628180, `BuildTypeData::get_town` @00639b90, `ObjectsData::find_city_at` @0065b870, `CityData::get_radius` @00738410, `LeaderData::get_radius` @006db790 |
| Corner heights | `Farms::add`, `Farms::recalc_heights` @008d88a0 (`TerrainOut::find_data_z`) |
| Growth | `Farms::inc_time` @008d8600 (from `Objects::inc_time` @0065db70), `Farms::grow` @008d91c0, `Farms::snip` @008d9240, `FarmsData::get_wheat_status` @008d9190 |
| No farm sprite | `WallOut::draw` @00641930 (`is(0x1a1)` skips the sprite block) |
| Underlay | `WallOut::draw_weathering` @0063dde0 ("wheatfarm"/"pigfarm", internal strings 0x22cf4/0x22d08), `ObjectsOut::draw_weathering` @00654a60, `TerrainOut::add_weather_underlay` @00874d00 (asm), `TerrainOut::render_weathering` @00852f70 |
| FARMTEX | `TileSet::init` @0087b370 ("FARMTEX/TEXTURE" → `wheat_texture`, PF_32, 2 mips), `TileSet::get_wheat_texture` @00878190 |
| Wind vector | `Scene::render` @008b3270 |
| Draw order | `Scene::render` @008b3270 |
| Animals | `Farms::add_animals` @008d8f30 / @008d92d0 |
| Scalability sections | `Data/config_defaults.xml` `<SECTION>` order (12 = BUILDINGS; 15 = AMMO matches attachments-spec's `section_render + 0xF`) |
