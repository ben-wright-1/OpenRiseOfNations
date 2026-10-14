# Terrain spec: drawing a still battlefield

Phase 1 of the port: load a real map and render its terrain. This file is the implementation spec; the evidence
(functions and addresses) is at the end. Status key as in [formats.md](formats.md): ✅ verified against real files ·
🟡 read from code, not yet verified by rendering · ❓ open.

**Shortest path:** load a scenario file (`.scx`), take its terrain chunk, and build the mesh straight from the stored
per-vertex heights. Scenarios store everything the renderer needs already baked (heights with rivers and mountains carved in,
land class per tile, water masks), so no map generator is needed. Saves would also work but go through the DataWalk
serializer, which is a much larger decode. Scenarios are self-contained chunk files: 311 of 311 shipped files decode.

Tool: `re/tools/scxdump.mjs` (decoder, survey, PNG previews, round-trip encoder).

```
node re/tools/scxdump.mjs <file.scx> [--png re/previews/name]   summary (+ land/height/tcoord PNGs)
node re/tools/scxdump.mjs --survey ~/Games/RiseOfNations        one line per scenario
node re/tools/scxdump.mjs --roundtrip ~/Games/RiseOfNations     re-encode every terrain chunk, compare bytes
```

---

## A. Map source: scenario files ✅

### Container

- The whole file is a **gzip** stream (magic `1f 8b`; `File`/`CloudFile` read through zlib). Decompressed, it is one
  BHG chunk tree, the same container as `.BH3` ([formats.md](formats.md)): `u32 size` (incl. header), `u16 id`, `u16 aux`
  (child count for containers). No DataWalk is involved.
- Root: chunk id 0. Its first child is the **header**, whose chunk id doubles as the header version: `0x01`, `0x50`,
  `0x52`, `0x66`, `0x67` or `0x6c`. Shipped files: 275 × `0x6c`, 26 × `0x52`, 8 × `0x67`, 2 × `0x66`.
- Remaining top-level children (any order): 2 terrain, 3 items, 4 goods, 5 objects, 6 orders, 7 cities, 8 triggers,
  9 signposts, 10 types, 11 leaders. Only **2** matters for the terrain; chunks 3–11 (players, units, buildings, goods)
  are in [scenario-objects-spec.md](scenario-objects-spec.md).
- `.scx` and `.sce` exist side by side. 12 of 147 pairs are identical; in the rest the terrain chunk differs too
  (the `.sce` looks like the editor copy). Use `.scx` (❓ confirm which one the game loads for play).

### Header (version `0x6c`, read as an array of u32 `h[i]`, byte offset `4*i`) 🟡

| Index | Field |
|---|---|
| `h[1]` | magic: one of `0x49248753 0x39823792 0x00700732 0x12345678 0x87654321 0xf00dcafe` (else the file is rejected) |
| `h[2..51]` | `wchar_t name[100]` |
| `h[0x34..0x45]` | game settings bytes: team_style, game_rules, map_style, map_size, … , rush_rules |
| `h[0x38]` | world seed (→ `WorldData.seed`) |
| `h[0x46..0x4d]` / `h[0x4e..0x55]` | `camera_init_x[8]` / `camera_init_y[8]` per player, **world units**; `-1` = unset |
| `h[0x56..0x5d]` | `camera_init_zoom[8]` (small integers, e.g. 4 or 5) |
| `h[0x5f]` | pop_limit |

Older versions are shorter and the loader fills defaults (camera fields `-1`, zoom 5).

### Units and grids ✅

| Grid | Size | Cell | Stored as |
|---|---|---|---|
| **WCoord** (gameplay tile) | `xs × ys` (stock: 24…100 square) | 768 world units | `WData`, 28 bytes in memory |
| **TCoord** (terrain sub-tile) | `4xs × 4ys` | 192 world units | `TData`, u16 mask |
| **Height vertices** | `(4xs+1) × (4ys+1)` | one per TCoord corner | f32, world units |

All arrays are row-major, index `y * width + x`. Heights use the same units as x/y: land sits around 0…400, mountain
peaks reach 1500–1900, the ocean floor is a flat **−273**, shelf water is around −200. `TerrainOut::find_data_z`
bilinearly interpolates the vertex grid with a 192-unit cell (it divides by 192 via a lookup table and multiplies by
1/192 = 0.0052083335), splitting each cell into two triangles along its diagonal.

### Terrain chunk (id 2) ✅ (byte-exact round trip on all 311 files)

Children in file order (`ScenarioWrite::save_terrain_chunk` writes them in this order; the reader dispatches on id).
283 files have all twelve; the 28 older ones lack 104.

| Id | Name | Payload |
|---|---|---|
| 81 | tileset | `wchar_t name[100]` (bytes after the NUL are junk). Names a `<TILESET>` in `Data/tilesets.xml` |
| 88 | wcoord3 | children **15** `{u32 count, u32 xs, u32 ys}` and **16** `count × {u16 flags, u8 land, u8 land_sub}` |
| 68 | tcoord2 | children **17** `{u32 count, u32 txs, u32 tys}` and **18** `count × u16 mask` |
| 89 | heights | `f32[(4xs+1)(4ys+1)]`, copied verbatim into `TerrainOut` (+0x4a4) |
| 90 | sorted waterline | `u32 n`, then n × `{u32 len, u32[len]}` (coastline orderings; can be rebuilt) |
| 91 | coord flags (diagonal order) | `u16` CoordFlags per wcoord in `TerrainOut`'s diagonal-list order, `xs*ys − 1` entries. Same values as 99 |
| 99 | wcoord flags | children **100** `{u32 count}` and **101** `count × u16` CoordFlags, row-major per wcoord. **Use this one** |
| 104 | height adjust | children **105** `{u32 size}` and **106** `u8[size]` (signed per-wcoord edit steps; all zero in stock files, and only applied when heights are absent) |
| 69 | mountains | children **70** `{u32 n}` and **71** `n × {u8 wx, u8 wy, u16 pad, i32 range_index, u8 num_coords, u8 pad[3]}` |
| 92 | rivers | **93** `{u32 n}`, then per river **94** `{u32 num_nodes}` followed by num_nodes × **95** `{i32 wx, i32 wy}` (all siblings; aux = child count) |
| 96 | cliffs | **97** `{u32 n}`, then n × **98** `{i32 wx, i32 wy, i32 size, i32 elem_num}`. No shipped scenario has cliffs |
| 73 | doobers | **74** `{u32 n}` and **75** `n × {i32 x, i32 y (world units), u8 kind (0/1), u8 pad[3]}` (pad bytes are uninitialised junk) |

Legacy ids still accepted by the reader but absent from shipped files: 12 (wcoord v1), 14 (tcoord v1), 67 (wcoord2).

#### Per-wcoord fields (chunk 88/16)

`land` (LandIndex) — verified against heights and masks on tut1, granicus and europe:

| land | Meaning | Typical height | Notes |
|---|---|---|---|
| 0 | land ("baseland") | 0…400 | `land_sub` = BaseLandSubtype: 0 fertile, 1 fertile→arid, 2 arid→fertile, 3 arid. Picks the texture family |
| 1 | shallow / shelf water | ≈ −200 | with file flag `0x80` it is a **shoreline** tile (about half its TCoords are water, mean height ≈ −55) |
| 2 | deep ocean | −273 flat | CoordFlags always `0x3009` |
| 3 | coast | — | not used in stock files |

`WData.is_ocean` is true for land 1 or 2 unless the half-water bit is set.

File `flags` (bit in file → bit in `WData.flags` after load, per `load_wcoord_chunk3`):

| File bit | WData bit | Meaning |
|---|---|---|
| `0x04` | `0x0020` | forest |
| `0x01` | `0x0008` | rocks (stone) |
| `0x08` | `0x0404` | coast (+ original coast) |
| `0x20` | `0x1000` | river |
| `0x10` | `0x0800` | oil (`FILE_FLAG_OIL` → `FLAG_OIL`; mostly together with rocks `0x01`) |
| `0x40` | `0x0040` | cliff (`FILE_FLAG_CLIFFS` → `FLAG_CLIFF`; 2 wcoords in all stock files) |
| `0x80` | `0x0100` | half water (shoreline) |

`WData.flags & 0x10` (mountains) is not stored; mountains come from chunk 69.

#### Per-tcoord mask (chunk 68/18)

| File bit | TData bit | Meaning |
|---|---|---|
| `0x10` | `0x0020` (as `mask & 0x30 == 0x20`) | water (ocean / sea) |
| `0x20` | `0x0400` | coastal |
| `0x02` | `0x0800` | river |

TData bits not in the file are rebuilt at load: `mask & 0x30 == 0x30` tree, `mask & 3 == 1` cliff, `== 2` mountain,
`== 3` building, `0x08` PERM_BEHIND. The full bit list and the load-time rebuild are in [world-spec.md](world-spec.md). **Trees are not stored per TCoord**; forests are stored per wcoord (flag above) and planted at load.

#### CoordFlags (chunk 99, per wcoord) — `eTERRA_COORD_*`

`1 WATER, 2 MOUNTAIN, 4 COASTAL, 8 OCEAN, 0x10 RIVER, 0x20 BORDERS_COASTAL, 0x40 HALFWATER, 0x80 OCEAN_BORDERS_COASTAL,
0x100 DECO, 0x200 HIGHEST_MOUNTAIN, 0x400 INIT_COAST_AFFECTED, 0x800 HAS_SIGNPOST, 0x1000 BASELAND_OCEAN,
0x2000 OCEAN_FLOOR, 0x4000 SANDY_OCEAN, 0x8000 ORIGINAL_COAST`. Typical: deep ocean `0x3009`, shelf `0x3089`/`0x308d`,
shoreline `0xa08d`, river land `0x11`. These feed the texture and water passes (section B).

### Load sequence (what the game derives) 🟡

1. `load_tileset_chunk`: flush the previous tileset, `TileSet::load_tileset(name)`.
2. `load_wcoord_chunk3`: `World::init(xs, ys)`, fill `WData`, then `TerrainOut::generate_land_lists`.
3. `load_tcoord_chunk2`, heights into a temporary list, waterlines, coord flags, mountains, rivers (`RiverLoadData`
   lists of wcoords), cliffs, doobers (`Doober::add_doober`).
4. `World::compute_all_vals`, `World::adjust_for_blocking`; `Map::make_coastlines` only if no heights were stored.
5. `ScenarioEditor::init_terrain`: rebuild rivers (`RiversOut::start_river` + `River::calculate` per stored polyline),
   regions and territory, `Terrain::init`, then **memcpy the stored heights over the generated ones**, OR chunk 99 into the
   per-wcoord coord info, add cliffs, `TerrainOut::adjust_for_mountains(1)`, `generate_water`, `calculate_norms`,
   `calculate_tangents`. Height-adjust bytes (104) are applied only on the random-map path.

For a still render you can skip 2–5 except: heights (89), land/land_sub (88), water mask (68), CoordFlags (99),
the tileset name (81), and optionally river polylines (92) and mountain stamps (69).

### Verification

- `--survey`: all 311 shipped `.scx`/`.sce` decode; sizes 24² (tutorials) to 100²; 13 tilesets.
- `--roundtrip`: re-encoding chunks 88, 68, 89, 90, 91, 99, 104, 69, 92, 96, 73 reproduces the original bytes in every
  file; the only differences are the three uninitialised padding bytes of doober records (30 files).
- Visual: `re/previews/tut1_land.png`, `tut1_hgray.png`, `granicus_land.png`, `granicus_hgray.png`. The tcoord water mask
  lines up with low heights; rivers are carved troughs in the heightfield and mountain ranges are bright peaks in it.
- Cross-checks per land class (granicus): land 2 → mean height −273, 100 % water tcoords; land 1 → −214, 100 % water;
  land 1 + half-water → −57, 51 % water; land 0 → +323, 0 % water.
- Textured: `node re/tools/terrain-preview.mjs <file.scx>` draws the base pass top-down (texture choice + supertile
  subtiles + Lambert + water plane) → `re/previews/tut1_terrain.png`, `granicus_terrain.png`. They look like the game's
  maps (beaches on shelf tiles, rivers as water-filled troughs, grass/dirt patches from `land_sub`). `--transitions`
  adds the soft transitions (B2a); the mountain overlay is still missing.

**First test map: `scenario/Tutorial1/tut1.scx`** (24×24, tileset Dirty, coast + shelf + deep ocean, 4 mountain
ranges, 137 forest tiles, 231 doobers, no rivers; 97×97 height vertices). Second: `conquest/Alexander/granicus.scx`
(60×60, Dirty, adds 3 rivers and islands). Both use the same 6 textures.

---

## B. Terrain rendering

### B1. Mesh 🟡

- One vertex per TCoord corner: position `(tx·192, ty·192, height[ty·(4xs+1)+tx])`. **+z is up**; heights are raw world
  z. No LOD on land geometry.
- Each TCoord quad is two triangles `(x,y)(x+1,y)(x,y+1)` and `(x,y+1)(x+1,y)(x+1,y+1)`, split along the
  `(x+1,y)–(x,y+1)` diagonal (`find_data_z` interpolates the same way). Cull mode is CCW (❓ check winding against the
  camera handedness below).
- Normals: per vertex, sum of the unnormalised face normals `(−Δz_x, −Δz_y, 192)` of adjacent triangles, then normalise
  (`calculate_norms`). Tangents likewise (`calculate_tangents`), only needed for the `_mtrl` normal maps.
- The game groups geometry by wcoord (a 5×5-vertex patch per wcoord, `CoordInfo::add_geometry`) into one draw
  bucket per texture, and walks the xs+ys−1 diagonals far to near. A port can just build one index list per bucket.
- Border **skirts** (1536-wide quads at z = −380, graphics.txt "Deep Color") are built by `generate_skirts` but **never
  drawn in EE**; outside the map is black. See [water-spec.md](water-spec.md) §6.
- Vertex format used by the game: position, normal, diffuse colour, UV0, UV1 (+ tangent with normal mapping).

### B2. Texture choice per wcoord ✅ (rule verified visually)

Tileset = `Data/tilesets.xml` `<TILESET name=…>` matching chunk 81. Relevant elements: `BASEDIR`, `BASELAND`,
`COASTAL`, `OCEAN` (each a list of `<BASE name supertile>` + `<TEXTURE name>`), `TERRAINGROUP/COAST_BASE`, `LIGHTING`.
Texture names resolve against `BASEDIR`, then `./terrain art/`, then `./art/`. **Paths must be resolved case-insensitively**
(XML says `dirty`, `Terrain Art`, `T_Flat_…`; disk differs).

| land | Texture list | Index |
|---|---|---|
| 0 | `BASELAND` (Dirty: grass, grass_dirt, dirt_grass, dirt; 512², `supertile="1"`) | `land_sub` (0…3) |
| 1 | `COASTAL` (Dirty: `sand.tga`, 256²) | 0 |
| 2 | `OCEAN` (Dirty: `ocean_floor_5.tga`) | 0 |

- **Supertiles:** a `supertile="1"` 512² image is cut into 3×3 subtiles of `floor(512/3) = 170` px. Wcoord (wx, wy) uses
  subtile `(wx mod 3, wy mod 3)`, so one image spans 3×3 wcoords seamlessly (`TextureEffect::load_as_supertile`,
  `get_subtile`). Equivalent for a port: UV = `((wx mod 3) + u)/3, ((wy mod 3) + v)/3` into the full image, u,v ∈ [0,1]
  per wcoord (but the game samples only the 510 px of the 3 subtiles, and mips each subtile separately).
- Non-supertile textures repeat once per wcoord: UV = `(tx/4, ty/4)`, wrapping. No random rotation on base land.
- Draw buckets: `convert_land_tex_to_linear` numbers them (land type, subtype, subtile): Dirty → 0–35 baseland
  (4 × 9 subtiles), 36 coastal, 37 ocean. Texture LOD copy = `min(2, 6 − zoom_level)`.
- `<name>_mtrl.tga` (derived from the texture name, never listed in XML) is the material map: RG = tangent-space normal
  XY, B = specular, A = gloss. Only used at the highest shader quality; skip for Phase 1.
- Unreferenced legacy files in `terrain art/` root: the single-letter `g/gs/m/p/r/f/a/s/t/w/ta/rt.tga` (+`_mtrl`), the root
  `ocean_floor_5`, `base_*` (fully transparent weathering bases). Ignore them.

Soft edges between these textures come from a second pass of alpha-masked overlay patches: see B2a.

### B2a. Transitions ✅ (algorithm decoded from code; reproduced in `re/tools/transitions.mjs`, seams verified)

A second pass of **one overlay patch per wcoord at most**. The patch reuses the wcoord's own 5×5-vertex geometry, is
textured with the next-lower texture in the chain and is alpha-masked by one of 14 mask files. Everything is derived
from `land`/`land_sub` only (forest, river, mountain and half-water flags play no part), once at load
(`Terrain::init` → `clear_nodes`, `create_nodes`, `mark_nodes`, `create_transition_renderables`) and again after editor or
terraform changes (`Terrain::spot_update_land*`).

The reference implementation is `re/tools/transitions.mjs`, which mirrors the exe step by step, quirks included. Port
it 1:1 rather than "fixing" it, or the mask choice and so the look will differ.

#### T1. Texture order

`is_combo_acceptable(a, b)` returns 1 when texture `a = (land, sub)` is **directly below** `b`, 2 when it is equal or
directly above, and 0 otherwise:

| b (the wcoord) | a is directly below b when |
|---|---|
| land L, sub s (same land) | a = (L, s−1) |
| coastal (land 1) | a = baseland sub `COAST_BASE` (Dirty: 3, dirt) |
| ocean (land 2) | a is **any** non-ocean texture |
| first of a list (L, 0), L ≥ 1 | a = (L−1, last); in practice already covered by the two rows above |

Any other pair is not adjacent in the chain. Such pairs get a hard edge, for example grass next to dirt_grass, or grass
next to sand.

The overlay texture of a wcoord (`get_acceptable_neighbor`) is (L, s−1) if s > 0; (0, `COAST_BASE`) for coastal; (1,
last coastal) for ocean; and itself for baseland sub 0, which never gets a patch anyway. It uses the same supertile
subtile `(wx mod 3, wy mod 3)` and draw bucket (`convert_land_tex_to_linear`) as a base wcoord of that texture would.
In map mode (`tilesets.is_map_mode`) the `MAPTEXTURE` list replaces `TEXTURE`.

So there is only ever **one layer**. Neighbours more than one step lower are ignored. An ocean tile next to both grass
and sand still gets a single sand overlay.

#### T2. Edge nodes

Two `u32` arrays hold three 3-bit groups per wcoord edge:

- **EX(x, y)**: the west edge of wcoord (x, y), at index `(ys+1)·y + x` (`TerrainOut+0x499c`).
- **EY(x, y)**: the north edge of wcoord (x, y), at index `(xs+1)·x + y` (`+0x49a0`).

The east edge of (x, y) is EX(x+1, y) and the south edge is EY(x, y+1). The index strides only match the allocations
`(xs+1)·ys` and `(ys+1)·xs` on square maps (all 156 stock `.scx` are square). Edges on the east and south map border
are never written, so they stay 0.

| Bits | Group | Meaning for the mask sample |
|---|---|---|
| 0–2 | FILL | must show the wcoord's own texture (mask alpha 0) |
| 3–5 | CONNECT | the mask must be transparent somewhere on this edge |
| 6–8 | BLOCK | must show the lower texture (mask alpha 255) |

Within a group, bit 2 is the end with the lower coordinate (W or N), bit 1 the middle and bit 0 the higher end (E or S).
`set_edge(e, v, 6)` ORs `v<<6` into the edge **and clears `v<<3`**: blocking a sample removes its connect bit.

#### T3. Marking

`mark_nodes` runs `mark_trans_flags(x, y)` row by row (y outer). Neighbours go through the `move_x/move_y[1..8]` order
NW, N, NE, E, SE, S, SW, W (+y = south), skipping off-map ones. "Lower" means `is_combo_acceptable(nb, own) == 1`.

1. Count the diagonal neighbours that are lower and the orthogonal ones that are not.
2. On the **first** lower orthogonal neighbour only (`add_unique` refuses the second): append the wcoord to the
   transition list. Set CONNECT = 7 on its W and N edges, and on its E/S edges if x+1 < xs / y+1 < ys. Call
   `check_for_tendrils`. If that returns 0, BLOCK = 7 the edge shared with this neighbour.
3. After the loop, if no orthogonal neighbour is lower but some diagonal one is: append the wcoord and OR `0x38` into
   EX(x, y), EX(x+1, y), EX(x, y) and EX(x, y+1). That is **all four in the vertical-edge array**; the last two were
   presumably meant for EY. Then call `check_for_tendrils`.

`check_for_tendrils(x, y)` first takes lower flags for all 8 neighbours. It always calls `neighbor_count` and drops the
result. Then:

- **Corners.** A corner is handled only when both orthogonal neighbours touching it are not lower: N and W for NW,
  N and E for NE, S and W for SW, S and E for SE. If the diagonal neighbour at that corner is not lower, it ORs FILL into
  the two edge ends meeting at the corner. If it is lower, it BLOCKs them:

  | Corner | Edge ends written |
  |---|---|
  | NW | EX(x,y) bit 2, EY(x,y) bit 2 |
  | NE | EX(x+1,y) bit 2, EY(x,y) bit 0 |
  | SW | EX(x,y) bit 0, EY(x,y+1) bit 2 |
  | SE | EX(x+1,y) bit 0, EY(x,y+1) bit 0 |

- **Strips.** If N and S are lower but W and E are not: BLOCK 5 (both ends) on EX(x,y) and EX(x+1,y), and return 1.
  If W and E are lower but N and S are not: BLOCK 5 on EY(x,y) and EY(x,y+1), and return 1.
- **Peninsula.** If exactly 3 orthogonal neighbours are lower, take the open side:
  - BLOCK 5 on that edge, and OR FILL bit 1 (the middle) into it.
  - OR `0x38` (CONNECT) into the two side edges of the wcoord beyond it. For an open north side these are EX(x, y−1) and
    EX(x+1, y−1); the other three sides are analogous.
  - Return 1.
- Otherwise return 0.

Every write is skipped when its coordinate is off the map.

#### T4. Per patch: choose the mask

This happens in `create_transition_renderables`, for each listed wcoord in list order, after all marking is done.

1. Read the three layers as 12-bit codes with `build_trans_flags`:
   `code = top<<9 | right<<6 | rev3(bottom)<<3 | rev3(left)`, where top = EY(x,y), right = EX(x+1,y),
   bottom = EY(x,y+1), left = EX(x,y), and `rev3 = {0,4,2,6,1,5,3,7}`.

   Read MSB first, the code walks **clockwise from the NW corner** with y pointing down: N edge W→E (bits 11–9),
   E edge N→S (8–6), S edge E→W (5–3), W edge S→N (2–0). Each corner appears twice: NW = bits 11/0, NE = 9/8,
   SE = 6/5, SW = 3/2. This is the bit order of the file names, so character 0 = bit 11.
2. `check_blocks(block)`. Go through N, E, S, W, skipping off-map neighbours:
   - If the group is already full (7) and the wcoord is directly below that neighbour, clear the group.
   - Otherwise, if the neighbour is directly below the wcoord, set the group to full.

   Then sync the corner pairs in this order: `800→1, 200→100, 100→200, 40→20, 20→40, 8→4, 4→8, 1→800`.
3. `check_connects(connect, block)`. Go through N, E, S, W:
   - If the group is 0 and the neighbour is lower: `connect |= ~(block & group)`. This is a full 32-bit NOT (sic), so
     it sets every other group as well.
   - Otherwise, if the neighbour's texture differs from the wcoord's: clear the group.

   Off-map neighbours are skipped.
4. `check_fills(fill, block)`. For each in-map orthogonal neighbour, clear the FILL group if the neighbour's texture
   differs. Then clear the FILL bits that are also BLOCK bits.
5. If `block == 0`, run `check_for_diagonal(block, fill)`. Off-map neighbours read as (land 0, sub 0) here.
   - First, four tests compare two orthogonal neighbours that are equal to each other but differ from the wcoord.
     Each match *assigns* BLOCK and ORs FILL, in this order:

     | Equal pair | BLOCK = | FILL \|= |
     |---|---|---|
     | N = E | 0xfc0 | 0xc |
     | N = W | 0xe07 | 0x60 |
     | S = E | 0x1f8 | 0x801 |
     | S = W | 0x3f | 0x300 |

   - If all four match, both are reset (BLOCK = 0, FILL = the original FILL).
   - If none matches, test the diagonals instead. When the diagonal is directly lower than the wcoord:

     | Lower diagonal | BLOCK = | FILL \|= |
     |---|---|---|
     | NW | 0x801 | 0x1fc |
     | NE | 0x300 | 0x3f |
     | SW | 0xc | 0xfc0 |
     | SE | 0x60 | 0xe07 |

     Again each match assigns BLOCK, so the last match wins.
6. Build `required` from FILL. Let `I = required | connect | block`, re-evaluated after each step:
   - If `I & 0xe00 == 0`, OR in `0x400` (call this t).
   - If `I & 0x1c0 == 0`, OR in `0x80` (r).
   - If `I & 0x38 == 0`, OR in `0x10` (b).
   - If `I & 7 == 0`, OR in `2` (l).
   - Corners: t∧l → `0x801`, t∧r → `0x300`, b∧l → **`0x9`**, b∧r → **`0x30`**. The last two are the exe's values; the
     symmetric ones would be 0xc and 0x60.
7. `Transitions::find_map(x, y, block, connect, required)`. See T5.

#### T5. Mask library and `find_map`

`Transitions::init` reads `./terrain art/transitions/*.tga` (the folder's `.png` copies and `shores/` are not used) in
FileSearch order, which is name order. Each file name is a 12-bit binary code. Code `0xeff` would be skipped; no
such file exists. Each file is loaded as DXT3 with 4 mips.

Stock set (14 files): `000000000000 001110011100 010000000000 010000000010 010000010000 010010010000
010010010010 011110011110 110000000011 110000010011 110000011111 110010010011 110010011111 110011111111`.
13 of them are used by the stock scenarios; `010010010010` is never picked.

Each file stores 8 orientation codes (`AlphaInfo` +4…+0x20). Write a code as groups (N, E, S, W) = (d, c, b, a), where
d holds bits 9–11, and let R = rev3.

| o | Groups (N, E, S, W) | Transform |
|---|---|---|
| 0 | d, c, b, a | as stored |
| 1 | a, d, c, b | rotated 90° clockwise (y down) |
| 2 | b, a, d, c | rotated 180° |
| 3 | c, b, a, d | rotated 270° clockwise |
| 4 | Rd, Ra, Rb, Rc | mirrored west↔east |
| 5…7 | — | mirror, then rotate 90° / 180° / 270° clockwise |

The image (top-left origin, i.e. the TGA after normalising its bottom-up rows) shows code bit 11 at its top-left.
Alpha 255 = lower texture, matching code bits = 0. Alpha 0 = own texture, matching code bits = 1. Each 128² file is a
2×2 atlas of four 64² variants of the same code.

`find_map` keeps, per file, the orientations whose code `v` passes all of these:

- `block & v == 0`
- `v & required == required`
- for each group (N, E, S, W): if `connect` has a bit in it, `v` has a bit in it
- unless block, connect and required are all 0, in which case only `v == 0` (file `000000000000`) qualifies

Files with at least one surviving orientation become candidates, in file order. Let h = `texture_fractal(x, y)` (0…99).
Pick with single-precision floats: `i = trunc(h · (n/100))`, and if `h · (n/100) ≥ n − 0.5` then `i = n − 1`.
Apply this once to choose the file among n candidates, and again **with the same h** to choose the orientation among
that file's survivors. A null result logs and draws no patch; this never happens on the 156 stock `.scx`
(182,638 patches).

**Quadrant:** `f = h · 0.04f`; if `f ≥ 3.5` then `f = 3`; `q = trunc(f)`. That gives 0…24 → q0, 25…49 → q1,
50…74 → q2 and 75…99 → q3. The UV offset is `(0.5 if q ∈ {1,2} else 0, 0.5 if q ∈ {2,3} else 0)`. File, orientation
and quadrant all come from one h, so they are correlated.

#### T6. `texture_fractal`

`TerrainData::texture_fractal` is a global `Fractal`. `TerrainOut::refresh_data` initialises it with
`Fractal::init(xs+1, ys+1, smooth 0, –, flags 2, seed = WorldData.seed)`, but **only when `world.size` differs from
`last_map_size`**. `TerrainOut::close` resets `last_map_size` to 0. The seed is the scenario header's `map_seed`
(`h[0x38]`): 0 in 155 of 156 stock `.scx` files and 2596 in one.

- **Grid:** `F[x][y]` for x ∈ 0…xs+1, y ∈ 0…ys+1, starting at 0. With `smooth = 0` there is a single pass: for x in
  0…xs, then y in 0…ys, set `F[x][y] = Random::get(0, 255)`. That is
  `seed = seed·0x19660D + 0x3C6EF35F; ((seed & 0xFFFF)·255) >> 16`. Afterwards column xs+1 = column 0.
- **`get_height(x, y)`**, all in doubles:
  1. `fx = (x+0.5)·(xs+1)/(xs+2)`, `fy = y + 0.5`, ix/iy = truncations, dx/dy = fractions.
  2. Bilinear sum in exactly this order: `(1−dx)(1−dy)·F[ix][iy] + 0 + (1−dy)·dx·F[ix+1][iy] + (1−dx)·dy·F[ix][iy+1] +
     F[ix+1][iy+1]·(dy·dx)`.
  3. Truncate and clamp to 0…255, then return `v·100 >> 8` (0…99).

The same fractal also drives decorations, weathering, `RoadsPieces::find_map` and `mark_coast_vert_influence`.

#### T7. Geometry and UVs

- **Positions:** the wcoord's 5×5 vertices (`get_indexed_mesh(WCoord)`), the same positions and normals as the base pass.
- **UV0** (overlay texture): `(col/4, row/4)` for vertex (col, row) of the patch (`TerrainOut+0x4a28`). That is the
  base pass's mapping, into the overlay texture's subtile, or the whole image for non-supertile textures.
- **UV1** (mask): take (u, v) = UV0, apply the orientation transform, then multiply by 0.5 and add the quadrant offset.
  The transform (`rotate_tex_coords`) rotates about (0.5, 0.5) and is derived from the exe's input flips and angles:

  | o | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
  |---|---|---|---|---|---|---|---|---|
  | (u′, v′) | (u, v) | (v, 1−u) | (1−u, 1−v) | (1−v, u) | (1−u, v) | (1−v, 1−u) | (u, 1−v) | (v, u) |

  The exe gets there from (1−u, 1−v) for o 0–3 or (u, 1−v) for o 4–7, rotated by `Transform::rotate_z` with
  180/270/0/90° for o 0–3 and 180/90/0/270° for o 4–7. `Point::rotate` applies `x′ = x·cos + y·sin`, `y′ = −x·sin + y·cos`.

#### T8. Render state and order

**Draw order:** land buckets → ocean floor → **transitions** (only when the console variable `showtrans > 0.5`) → the
`render_coral` pass → … (B6).

Transition batches loop over land-texture buckets (outer) and masks (inner). Each batch binds:

| Stage | Texture |
|---|---|
| 0 | the bucket's land texture |
| 1 | the mask |
| 2 | the `_mtrl` map, at high shader quality only |

| | State "Transitions" (`TerrainOut+0x442c`, all other buckets) | States "CoralSandy" + "CoralTrans" (bucket `oceanindex`) |
|---|---|---|
| Used for | every overlay except sand | overlays whose texture is the coastal sand, i.e. ocean wcoords |
| enables | 0x205: depth test LESSEQUAL, no depth write, blend, lighting (+0x1c000 normal/spec/gloss maps at high quality) | 0x400206: **no depth test**, blend, lighting, point sampling |
| zbias | 0 | — |
| Blend | SRCALPHA / INVSRCALPHA | SRCALPHA / INVSRCALPHA |
| Colour | `tex0 × diffuse` (stage 1 SEL2 = current) | `tex0 × diffuse` |
| Alpha | `diffuse.a × mask.a` (stage 0 SEL2 diffuse, stage 1 MODULATE) | `mask.a` only |
| Addressing | CLAMP on both stages | CoralSandy default WRAP; CoralTrans CLAMP |
| Mipmap bias | −1.5 on the mask stage | — |

`oceanindex` is `convert_land_tex_to_linear(COASTAL, 0)`. The ocean case draws twice with the same index buffer: first
the sand with "CoralSandy", then stage 0 is rebound to the tileset's `CORALTEX` (Dirty: `tilesets/coralblend.tga`,
turquoise) and the batch is drawn again with "CoralTrans". So around coasts the sea floor fades to coral colour; the sand
overlay is fully covered wherever the mask is opaque.

Diffuse is the baked vertex lighting, so alpha fades with the vertex alpha under water. Filters in the states are POINT,
but the D3D11 backend ignores them (`renderstates.md`). The pixel-shader situation names are "Transitions" and "Coral".

**Port recipe:**

1. Draw the base pass.
2. Draw each patch's 5×5 grid again with depth test LESSEQUAL and depth write off.
3. Output `rgb = overlayTex(UV0) × lighting` and `a = mask(UV1).a × vertexAlpha`, blended SRCALPHA/INVSRCALPHA.
4. For ocean patches, use the coral texture in a second draw (no depth test, `a = mask.a`).

#### T9. Related passes (not decoded in detail)

- **Coral** (`render_coral`, state "Coral" at `+0x1438`): draws the RenderHelper at `+0x1870`, which nothing fills in
  EE (only cleared / `fill_*`), so the pass is empty (water-spec §2.6).
- **Ocean floor** (`build_ocean_floor`, `render_ocean_floor`): a flat 768-unit grid at `coast_depth + land_height − 10`
  = −101 under shelf/deep/half-water wcoords (plus one wcoord past the x = 0 and y = 0 edges), opaque, depth test
  without write. Full description in water-spec §2.
- **Decorations** (`build_decorations`, `render_decorations`, state "Decorations"): forest-floor patches of tileset
  `TREEDECO/BASE` (Dirty: `T_Flat_Land_Grass_Thick_01.tga`). Each is masked by an `ALPHAS` texture of size 1, 2 or 4
  wcoords, chosen with `texture_fractal` from forest neighbour counts. They set CoordFlags `0x100` (DECO).
- **Weathering** (`Terrain::add_weathering`, `render_weathering`, "Weathering" / "Weathering Underlay"): ground wear
  under building footprints larger than 2 tcoords. It uses `texture_fractal(tx>>2, ty>>2)` for its quadrant/rotation,
  plus the tileset's weathering base/alpha buckets. These are object-driven, not part of the static terrain.

#### T10. Verification

- **Preview:** `node re/tools/terrain-preview.mjs <file.scx> --transitions` (or `--alpha` for the mask alpha alone,
  `--crop x,y,w,h --px 64` to zoom) → `re/previews/tut1_trans.png`, `granicus_trans.png`, `*_transalpha.png`. The
  staircase edges of the base pass become soft, irregular boundaries that do not tile.
- **Patch counts:** tut1 lists 257 of 576 wcoords and granicus 1573 of 3600. Every listed wcoord finds a mask.
- **Seam test.** Sample 32 points just inside both sides of every wcoord edge, and compare the dominant visible texture
  (overlay if mask alpha ≥ 128, else own).

  | Configuration | tut1 | granicus | Chain-adjacent edges |
  |---|---|---|---|
  | Base pass only | 23.5 % | 23.1 % | 100 % |
  | With transitions | 0.63 % | 0.59 % | 0.47 % / 0.12 % |
  | Mask image flipped vertically | — | — | 26 % |
  | Rotation direction swapped | — | — | 50 % |

  Base-pass-only mismatches are all on chain-adjacent edges. The remaining mismatches with transitions are mostly
  non-adjacent pairs (hard edges by design). The two perturbed rows confirm both the code semantics and the UV
  transform in T7.

### B3. Lighting 🟡

- One directional light, travel direction `(−1, 0, −1)/√2` (light comes from +x and above at 45°), `Game::init_light`.
- Terrain material from the tileset `LIGHTING`: `AMBIENT` and `DIFFUSE` RGB, each with an `alpha` entry acting as an
  intensity multiplier (Dirty: ambient (0.01, 0.03, 0.18) × 2.0, diffuse (1, 1, 1) × 1.36).
- Baked per vertex (`light_vert`): `rgb = min(1, ambient + diffuse · diffuse.a · max(0, N·(−dir)))`; vertex alpha
  fades for z < 0 (underwater).
- The EE pixel shader (`shaders/ps_global.hlsl`) lights again per pixel: `pow(tex, 1.45)·(diffuse + ambient) +
  specular`, ×5 HDR, then `ps_tonemap.hlsl`. From the code, **both are applied**: `CoordInfo::add_geometry` puts the baked
  colour in the vertex stream and Base Terrain modulates the texture by it before shader lighting (details and constants
  in [lighting-spec.md](lighting-spec.md) §7; still to be confirmed with a screenshot). **Phase 1: `colour = tex × baked
  Lambert` is enough**; match the HDR/tonemap look later.
- No terrain shadows.

### B4. Water ✅ → [water-spec.md](water-spec.md)

- Sea: z = 0 patches of 13 vertices per water wcoord (3×3 at 384 spacing + 4 quad centres), alpha from heights
  (245 at h ≤ −55, ramp to 0 at `min_clip_level` = −25, river tcoords ÷4), then an in-place min/square smoothing pass
  (deep water ends near 189/255). RGB = WaterTop→WaterBot by screen position. "Min Water"/"Normal Water" states.
- Underwater land fades out (vertex alpha 255 at z ≥ −52 → 0 at z ≤ −89) and an opaque ocean-floor sheet at z = −101
  (OCEAN texture ×2, two drifting UV layers) covers shelf and deep water; this is what hides the shelf/deep staircase.
- River water, surf and the (unused) border skirts: water-spec §4–6.

### B5. Mountains and cliffs 🟡

- **Mountains** are heightfield + overlay, not models. Chunk 69 stamps reference mountain range templates; their heights
  are already in chunk 89 (tut1 peaks reach 1486 at the 4 stamps). `render_mountains` draws a second textured mesh over
  the same 192-unit grid (`MOUNTAINS/BASETEX`, then `ALPHATEX` + mtrl), with a LOD face list for far zoom; snowcaps via
  underlays. `adjust_for_mountains(1)` does **not** re-add template heights: the overlay takes the stored heights.
  Full spec (templates, UVs, the two draws, LOD, snowcaps): [terrain-dressing-spec.md](terrain-dressing-spec.md) §1.
- **Cliffs**: `Data/Cliffs.xml` → TGA height templates (`terrain art/cliffs/cliff_<8|16|32>_<orient>_<x>.tga`,
  height = byte/255 × 480, alpha 0 = untouched) + `CLIFFTEX` overlay. No shipped scenario uses cliffs: skip.

### B6. Draw order (`Scene::render`, for later phases)

Land buckets → ocean floor → transitions (B2a T8; ocean patches add a coral layer) → coral → decorations / weathering →
roads → rivers → underlays/shadows → mountains, cliffs → doobers → fog of war → borders → objects, particles → **water** →
river water → surf.
Land state: stage 0 `TEXTURE × DIFFUSE`, alpha = vertex alpha, wrap, blend SRCALPHA/INVSRCALPHA, z test+write.

### B7. Camera ✅ numbers from disassembly, 🟡 not yet compared with a screenshot

- **Orthographic**, yaw 45°, tilt 60° from vertical (view 30° above the horizon), so a wcoord is an exact 2:1 diamond.
  World +z up; viewed from above +x is right and +y is down (D3D left-handed world).
- Eye direction from the look-at point L: `(0.6124, 0.6124, 0.5)`; right `(0.7071, −0.7071, 0)`; up
  `(−0.3536, −0.3536, 0.8660)`; forward `(−0.6124, −0.6124, −0.5)`. The camera looks at z = 0, not the terrain height.
- Pixel mapping (Δ = P − L, u = world units per pixel, origin at view centre, y down):
  `px = 0.70711·(Δx − Δy)/u`, `py = (0.35355·(Δx + Δy) − 0.86603·Δz)/u`.
- Zoom levels 0…6 (6 = closest): `u = 3·max(screen_w, 1280)/screen_w / ratio[level]`,
  `ratio = {0.08, 0.1, 0.25, 1/3, 0.5, 2/3, 1.0}` → for screens ≥ 1280 wide, 37.5, 30, 12, 9, 6, 4.5, 3 units/pixel.
  The game starts at level 5 (4.5 units/px). Building sprites are authored at level 6 (3 units per sprite pixel).
- Projection matrix (`D3D11GLSys::set_projection_parallel`): horizontal "fov" 45° sets the ortho width at the eye
  distance: half-width = tan(22.5°)·D, D = eye height = d/2. near = D − 3840, far = distance to the plane z = −10 along
  the frustum edge. For a port: build a standard ortho matrix of width `screen_w·u`, height `screen_h·u`.
- Initial position: scenario header `camera_init_x/y[player]` (world units) and `camera_init_zoom` (< 7 = level), else
  centre on the player's first unit at level 5. Clamp the look-at point to `[0, xs·768] × [0, ys·768]` when scrolling.
- Scroll speed per level `{48, 32, 24, 16, 12, 9, 8}` × dt.

---

## C. Trees and doobers 🟡 (preview `re/previews/trees_tut1.png`)

- Gameplay tree TCoords (TData `&0x30 == 0x30`) are rebuilt deterministically at load (`World::adjust_for_blocking`):
  all 16 tcoords of a forest wcoord, minus the east column / south row when that neighbour is neither forest nor mountain.
  Not needed for drawing.
- **Visual trees** (`ObjectsOut::sort_trees`, `ObjectsData::place_tree`): 5 per forest wcoord, from a position-seeded
  LCG: `seed = (int)(cx·7.0f + cy)` with `c = (768wx+384, 768wy+384)`; `Random::get(lo,hi)`:
  `seed = seed·0x19660D + 0x3C6EF35F; lo + (((seed & 0xFFFF)·(hi−lo)) >> 16)`. Reach per direction is 144 if that
  neighbour is forest else 96 (W, N, E, S). Offsets from c, each `r()` one RNG draw:
  k=1: x += −W + r(−W,0), y += S + r(0,S); k=2: x += r(−W,E), y += r(−N,S); k=3: x += E + r(0,E), y += −N + r(−N,0);
  k=4: x += E + r(0,E), y += S + r(0,S); k=0: x += −W + r(−W,0), y += −N + r(−N,0), then falls through into the k=1 step
  (missing `break` in the original, 4 draws). Tree 2 is skipped in low-end sprite mode. z = `find_data_z(x, y)`.
- Models: `graphics.txt` sections `<Tileset>_Tree_Models0` (BH3 meshes, ~50 tris, z-up, ~130 units tall; 24 for Dirty)
  and `<Tileset>_Tree_Textures0` (7 textures: trunk, bottom×2, middle×3, top; plus a shadow decal). Model index
  `(int)(x+y) mod count`; scale `(s, −s, s)` with `s = treescale × 3`; no rotation. Sub-meshes bind to texture slots by
  node name. Shadow: a black 192×192 ground decal per tree.
- Doobers: kind 0 = "bush", kind 1 = "rock"; 4×4-cell billboard atlases from `graphics.txt` `#LAND DOOBERS`
  (per-tileset override `#<Tileset>_LAND DOOBERS`), at world (x, y) + centering·192, z from the terrain. Cell hash, quad
  size and render state: [terrain-dressing-spec.md](terrain-dressing-spec.md) §3. River troughs: same file, §2.

---

## Open questions

1. Which of `.scx` / `.sce` the game loads for play (they differ in 135 of 147 pairs).
2. Double lighting: does the EE backend multiply the baked vertex colour *and* apply shader lighting, or is vertex colour
   white? Decides whether a port bakes or shades. Compare with an in-game screenshot.
3. Transitions (B2a), residual questions:
   - Confirm that `ScenarioRead` copies header `map_seed` into `WorldData.seed` before `refresh_data`. The seed only
     changes which variant, orientation and quadrant get picked, never where patches go.
   - The fractal is only rebuilt when the map size changes; a port should seed it per load.
   - ~~Who fills the `render_coral` RenderHelper (`+0x1870`)?~~ Nobody in EE (water-spec §2.6).
   - Whether the patch vertices are refreshed after `ScenarioEditor::init_terrain` memcpys the stored heights over the
     generated ones. A port should simply reuse the final mesh.
4. ~~Water: `min_clip_level`, `newwater/` frames~~ — answered in water-spec (−25; frames used by river water only).
5. ~~`adjust_for_mountains(1)` after loading stored heights: idempotent or additive?~~ Idempotent: the flag makes
   `fill_mountain_data` read the stored heights (terrain-dressing-spec §1.3).
6. Texture filtering: render states ask for point mip filtering; the D3D11 backend may override.
7. Cull winding vs camera handedness (left-handed world, +y down the screen).
8. ~~Unknown wcoord file flags `0x10` and `0x40`; meaning of `TData & 3 == 3`~~ — oil, cliff, and building footprint
   (PDB enums; all WData/TData bits are in [world-spec.md](world-spec.md) §2).
9. UI scaling: `scaled_screen_xs` / `natural_ratio` on Retina screens decide units-per-pixel.

---

## Evidence

All addresses in `riseofnations.exe` (base 0x00400000) unless noted. Decompiled output (local only): `re/decomp/scenario/`,
`terrain/`, `world/`, `tiletex/`, `transitions/`, `terrainmesh/`, `camera/`, `trees/`. Type layouts: `re/types/*.txt` (`DumpTypes.java`).

| Topic | Functions |
|---|---|
| Container, header | `ScenarioRead::import` 009b1430 (gzip read, header versions, magic check, camera_init_*), `ChunkRead::*` |
| Terrain chunk | `ScenarioRead::load_terrain_chunk` 009aedf0 (child dispatch), `ScenarioWrite::save_terrain_chunk` 009a88d0 (order, bit mapping) |
| WCoord / TCoord | `load_wcoord_chunk3` 009ae370, `load_wcoord_chunk2` 009ae670, `load_wcoord_chunk` 009aeba0, `load_tcoord_chunk2` 009ae1f0; `World::init` 006b76f0; types `WData`, `TData`, `WCoordDataChunk2`, `LandIndex`, `BaseLandSubtype`, `CoordFlags` |
| Flag meanings | `WorldData::is_ocean` 006b4830, `is_coast` 006b3020, `is_forest` 006b5560, `is_rocks` 006b4380, `is_tocean` 0046fb10, `is_tree_at` 0046f930, `is_river` 0046d390, `is_cliff_at` 0046f8c0, `World::set_waterhalf` 006b1c90, `set_river_at` 006b1f80 |
| Heights | `load_heights_chunk` 009ad990 ((4xs+1)(4ys+1) floats), `ScenarioEditor::init_terrain` 009a19d0 (memcpy into `TerrainOut`+0x4a4), `TerrainOut::find_data_z` 00866560 (192-unit cells), `find_tcoord_z` 008544a0 |
| Other terrain children | `load_sorted_waterline_data_chunk` 009ad8a0, `load_coord_flag_data` 009ad830, `load_wcoord_flag_data` 009ad6c0, `load_terrain_height_adjust_chunk` 009abcd0, `load_mountains_chunk` 009adf40, `load_rivers_chunk` 009adbf0, `load_cliffs_chunk` 009ada20, `load_doobers_chunk` 009ae920, `load_tileset_chunk` 009aed90 |
| Mesh | `Terrain::init` 00850f70, `TerrainOut::generate_land` 0085f8d0, `generate_land_lists` 0085f6a0, `get_indexed_mesh` 00863c50, `get_mesh` 008643c0, `calculate_norms` 008710b0, `calculate_tangents` 008717a0, `generate_skirts` 008603c0, `CoordInfo::add_geometry` 0084bef0, `remap_wcoord_to_terraincoord` 00854600 |
| Textures | `TileSet::init` 0087b370, `load_land_data` 008767d0, `load_tileset` 0087b020, `fixup_path_to_file` 00877d90, `convert_land_tex_to_linear` 0087ace0, `get_land_textures_by_index` 0087a9a0, `get_acceptable_neighbor` 0087a700, `is_combo_acceptable` 0087abe0, `TextureEffect::load_as_supertile` 00542640, `get_subtile` 005425d0 |
| Transitions | `Terrain::init` 00850f70 (calls `clear_nodes` 0085bd50, `create_nodes` 0085bc70, `mark_nodes` 0085bc20, `create_transition_renderables` 00862c60), `TerrainOut::mark_trans_flags` 008625a0, `check_for_tendrils` 00859eb0, `neighbor_count` 0085bb30, `set_edge_x/y` 00859500/00859410, `build_trans_flags` 00864160, `check_blocks` 0085b3e0, `check_connects` 0085b790, `check_fills` 0085b2c0, `check_for_diagonal` 008595f0, `rotate_tex_coords` 00863a50, `setup_transition_renderables` 0086a7b0 (buckets per mask × land texture), `TileSet::is_combo_acceptable` 0087abe0, `get_acceptable_neighbor` 0087a700, `Transitions::init` 0087eda0, `Transitions::find_map` 0087f2a0, `ArrayBase<int>::add_unique` 0047adb0; tables `move_x/move_y` 00adcaf0/00adc400, `bit_reversal` 00af1d58, switch table 008638f0 |
| Transition maths | `Fractal::init` 006aa2d0, `Fractal::get_height` 006aa870, `TerrainOut::refresh_data` 00870050 (fractal init, `last_map_size` 00cbe584), `Random::get` 00a39d70, `Transform<float>::rotate_z` 00421c50, `Quat<float>::set` 00420c70, `Point<float>::rotate` 00420b10; UV table fill in `one_time_init` at 0085f226 |
| Transition render | `TerrainOut::render` 008558b0 (transition loop after `render_ocean_floor`, `oceanindex`), `one_time_init` 0085c150 (states "Transitions" +0x442c, "CoralSandy" +0x1708, "CoralTrans" +0x15a0, "Coral" +0x1438), `render_coral` 00854b40, `build_ocean_floor` 0086c7f0, `build_decorations` 008722d0, `Terrain::add_weathering` 00850120; decompiles in `re/decomp/transitions/` (+`asm/`) |
| Lighting | `Game::init_light` 00590ef0, `TerrainOut::prep_terrain_lighting` 0085bfd0, `light_vert` 00866f90, `light_land_vert` 00866e50, `GraphicPieces::init_lighting` 00904a30 |
| Render | `TerrainOut::one_time_init` 0085c150 (render states), `TerrainOut::render` 008558b0, `render_ocean_floor` 00854dd0, `render_mountains` 008551f0, `Scene::render` 008b3270 |
| Water | `create_water_verts` 0086ceb0, `modify_water` 0086e700, `get_water_fade` 00869fd0, `render_water` 00852cd0, `generate_water` 00861980 |
| Mountains, cliffs | `fill_mountain_data` 00869380, `adjust_for_mountains` 008703c0, `CliffsOut::init_templates` 008a71a0, `CliffsOut::render` 008a6df0 |
| Camera | `Game::init_camera` 00590bc0, `Camera::update_transforms` 008430a0, `BaseCamera::update_transforms` 00537cc0, `Camera::compute_clip_planes` 00842950, `compute_distance` 00842c90, `set_auto_tilt` 00844690, `loc_bounds` 00842ff0, `Game::zoom_to_first_unit` 0058db30; d3dgl `D3D11GLSys::set_projection_parallel` 10020830 |
| Trees, doobers | `World::adjust_for_blocking` 006b3230, `ObjectsOut::sort_trees` 00656c70, `ObjectsData::place_tree` 00658530, `make_tree_mask` 00658880, `Random::get` 00a39d70, `GraphicPieces::init_tree_data` 008eeb30, `draw_tree` 00905a10, `draw_tree_shadow` 00905910, `TreeTransformMethod::make_index_table` 00920df0, `Doober::draw` 00844bd0 |
