# Fog of war and line of sight

Status: ✅ data layout, primitive writers, every `WorldData` query, `Object::update_seen` (stamp shape, ring tables,
projection, detection), `Unit::update_los` / `Wall::update_los`, `GameDaemon::update_all_seen`, ally-mask maintenance,
the per-object `is_seen` family, reveal side effects (all from decompilation, PDB names, cross-checked against the
disassembly where Ghidra lost arguments) · 🟡 renderer fade timing, minimap blend state, the semantics of a few tech bits
(§4.1 note) · ❓ nothing here is validated against a running game (no replay with commands exists yet). Ported to
`engine/sim/fog.cpp` (docs/sim-implementation.md, "Fog of war"); the port's corrections are folded in below (§3.3,
§3.4, §5.2, §6.6, §11).

Companion specs: [world-spec.md](world-spec.md) (grids, `WData`, `FCoord`), [frame-spec.md](frame-spec.md) (frame
order), [movement-spec.md](movement-spec.md) (pathfinder fog terms), [orders-spec.md](orders-spec.md) (`Unit::work`),
[state-spec.md](state-spec.md) / [state-walk.md](state-walk.md) (save and checksum layout),
[scenario-objects-spec.md](scenario-objects-spec.md) (diplomacy chunk 41). Addresses are `riseofnations.exe` VAs.
Decompiles: `re/decomp/fog/` (jobs `re/jobs/dc_fog*.txt`, `da_fog*.txt`, `callers_fog*.txt`, `grep_allymask*.txt`,
`syms_fog.txt`), `re/decomp/fog/render/` (`dc_fog_render.txt`), plus `re/decomp/{move,combat,combat_C,combat_D,
grid_build,diplo,gpieces}/`. Reference tool: `re/tools/los-ref.mjs` (circle/ring tables, stamp shapes).

Conventions: `who` is a leader 0..9 (8 = unused/neutral slot, 9 = gaia); "bit(p)" is `1 << p` truncated to a byte (so
players 8 and 9 contribute nothing to any fog byte). A **fog cell** (`FCoord`) is 384 × 384 world units: `fx =
floor(x / 384)`, computed in the code as `div_3_table[x >> 7]`; the fog grid is `fog_xs = 2·xs` by `fog_ys = 2·ys`
(world-spec §1). Fog cell `(fx, fy)` lies in wcoord `(fx >> 1, fy >> 1)` and tcoords `2fx..2fx+1`. `L[p]` is
`leaders.list[p]` (LeaderData). Object fields are `ObjectData` / `WallData` members (`node re/tools/pdbtypes.mjs`).

---

## 1. Data

### 1.1 World grids (`WorldData`, global `world`)

| Field | Off | Size | Meaning | Set by | Cleared by |
|---|---|---|---|---|---|
| `seen` | +0x15c | `fog_size` u8 | **currently seen**: bit p = some object of p stamped this cell since the last refresh | `World::set_seen`, `World::set_seen2(…, now)` | `World::clear_seen` (every refresh, §5.2) |
| `seen2` | +0x160 | `fog_size` u8 | **explored** (ever actually seen) | `set_seen`, `set_seen2`, `set_was_seen`, `Wall::start`, `update_seen_ally`, nuke, frame-0 ally share | never in play (`clear_seen2` only from `World::wipe`, the editor) |
| `seen3` | +0x164 | `fog_size` u8 | **detected**: bit p = a detector of p (or a script) covered the cell since the last refresh; reveals cloaked units | `set_seen(…, detect = 1)` | memset in `update_all_seen` |
| `wcoord_seen` | +0x168 | `size` u8 | currently seen, per wcoord (OR of its 4 fog cells' stamps) | `set_seen`, `set_seen2(…, now)` | `clear_seen` |
| `WData.was_seen` | wdata +0x14 | per wcoord | explored, per wcoord | every `seen2` writer except the frame-0 ally share (§5.2) | never |

There are **no reference counts**: "currently seen" is a sticky OR that is wiped and rebuilt from scratch every 100
frames (§5.2). Between refreshes a moving unit only adds cells (it stamps the rim of its new circle, §3.4), so the trail
of a moving unit stays "seen" until the next refresh. `wcoord_seen` has no simulation reader (only
`WorldData::is_seen_wcoord` @6b2540, which has no callers), but it is saved and checksummed.

All five arrays are saved and checksummed: `World::walk_data` part 5 (`WData` bytes 0..0x14, so `was_seen` included),
part 6 (`tdata`, `seen`, `seen2`, `seen3`), part 7 (`wcoord_seen`), checksum part 12 (state-walk.md, World). The debug
verifier `CheckSums::check_seen` @9370f0 hashes one cell of `seen`, `seen2`, `seen3`.

### 1.2 Per object

| Field | Where | Meaning |
|---|---|---|
| `mylos` (i8) | ObjectData +0x3c | LOS in tcoords, cached by `update_los` (§4); read by `los()` (vslot +0x128) |
| `visible` (i8) | ObjectData +0x40 | bit p = this object is revealed to p because it attacked/cast at p (§6.4). Units: cleared every 32 frames (staggered by `o`) in `Unit::work` unless `flags & ATTACKING`; buildings: same in `Build::do_attack` |
| `infiltrated` (i8) | ObjectData +0x3a | bit p = a spy of p infiltrated this building: p sees everything the building sees (§3.3) and the building itself |
| `flags & DETECTOR` (0x40) | SubObject +0x8 | set by `Object::init` @647750 when the type has obj-mask 0x2000000; this object's stamps also write `seen3` |
| `ever_seen`, `ever_seen_completed` (u8) | WallData +0x62, +0x63 | per-player memory of a building / of its finished state (§6.1) |
| `ever_seen` (u8) | GoodData +0x20, ItemData +0x20 | rare resources and goody items once discovered (§3.5) |
| `los_x`, `los_y` | UnitData +0x78/+0x7c | set to the unit position by `Unit::init`; no fog reader found (❓ AI) |
| `seen` mask (u8) | graphic event +0x16 (0x18-byte entries) | OR of the players that have seen the event's cell (§9.5) |

Goods and items are not in the per-leader object lists: `Objects::init_good` @653f30 / `init_item` @653e00 take the
first free slot of the global `goods` / `items` arrays (in load order: items chunk 3, then goods chunk 4) and make it
the **terminal of the wcoord's object list**: `WData.down = −2` (good, not for oil) or `−3` (item), `WData.down_who` =
the array index (a good overwrites an item on the same wcoord). Objects linked later are head-inserted above it, so
`ObjectsData::find_good_at(w, …, p5 = 0)` @65bec0 and `find_goody_at(w)` @65c040 walk the list to its negative end.
`Objects +0x154` is the highest good index + 1 (the bound of the linear good searches).

### 1.3 Per leader and global settings

- **`L[p].ally_mask`** (LeaderData +0x6929, u8): the players whose vision p shares, always including p. Every
  "seen by p" query ANDs the grid byte with `L[p].ally_mask`; stamps only ever write the stamping object's own bit.
  Maintenance in §6.6. (Saved: it is the last byte of the LeaderData raw block, state-walk.md.)
- `L[p].leader_flags` (PDB `LeaderFlagIndex`): **0x800 LEADER_REVEAL_ENEMY** (whole map seen), **0x1000
  LEADER_EXPLORE_MAP** (whole map explored), **0x2000 LEADER_ACTIVE_PING** (everything inside allied territory seen).
- `L[p].num_units[FOUCHE − 50]` (LeaderData +0x59e4; FOUCHE = TypeIndex 371, the Napoleon-campaign hero whose help text is
  "Reveals all enemies"): while p owns one, every query answers "seen" exactly as with REVEAL_ENEMY.
- `GameInfo.reveal_map` (setup byte +0x24; `rules.xml` category `revealmaps`): **0 No Ally LOS, 1 Normal, 2 Explored,
  3 All Visible**.
- `ScenarioData::ally_mask[8]` (scripted vision grants, `add_visibility` / `remove_visibility`) and
  `ScenarioData::reveal_points[8]` (per player `{tx, ty, radius}` in tcoords, `add_reveal_point`). Both saved.
- `GameDaemon.busy` is set to 4 by every refresh (decremented in `GameDaemon::process_all`, frame-spec §5).

---

## 2. Primitive writers (`world.obj`)

Index `i = fy·fog_xs + fx`, `w = (fy >> 1)·xs + (fx >> 1)`.

- **`World::set_seen(fx, fy, who, detect)`** @6b3c60: `b = who < 0 ? 0xff : bit(who)`. `seen[i] |= b`; if `detect`,
  `seen3[i] |= b`; `seen2[i] |= b`; `WData[w].was_seen |= b`; `wcoord_seen[w] |= b`. Returns **1 iff `seen2[i]`
  changed** (the cell is newly explored by that owner). (Renderer hook: `WorldMapFog::add_update(fx, fy)` when `seen` or
  `seen2` changed.)
- **`World::set_seen2(fx, fy, mask, explored_only)`** @6b4bb0: `seen2[i] |= mask`; unless `explored_only`: `seen[i] |=
  mask`, `wcoord_seen[w] |= mask`; always `WData[w].was_seen |= mask`. Returns 1 if `seen2` or `seen` changed (only the
  renderer uses it). Never touches `seen3`.
- **`World::set_was_seen(fx, fy, who)`** @6b41d0: `WData[w].was_seen |= bit(who)`, `seen2[i] |= bit(who)` (no callers in
  the simulation; `Wall::start` inlines the same thing).
- **`World::clear_seen`** @6b2250: zero `seen` and `wcoord_seen` (renderer: an update per previously-seen cell).
  `clear_seen3` @6b2140 / the inline memset in `update_all_seen`: zero `seen3`. `clear_seen2` @6b2160: zero `seen2`
  (`World::wipe` only). `World::adjust_fog` @6b76a0 is empty.
- `World::get_seen` / `get_was_seen` (@6b4290 / @6b1ba0): raw `seen[i]` / `seen2[i]` (no simulation callers).

---

## 3. Stamping: `Object::update_seen(move)` @651b80 (vslot +0x174)

### 3.1 Tables

- **Circle tables** `circle_x/circle_y/circle_radius` (`circle_init` @006817f0, world-spec §2.4): ring r = offsets whose
  approximate distance equals r; "within r" is entries `[0, circle_radius[r])`. **The table is capped at 0x3249
  points**: the cap is reached inside ring 64, so `circle_radius[64] = 12873` and ring 64 holds only its first 300
  points (`node re/tools/los-ref.mjs tables`). `engine/sim/compass.cpp` applies the cap (`circle_init` @006817f0 fills
  `circle_radius[r..64]` with the count and returns once 0x3249 points exist).
- **Ring tables** `ring_x/ring_y/ring_radius[33]` (`ring_init` @00681920): ring 0 = the centre; ring r (1..32) = the
  circle ring r **plus gap fillers**: for each original point of the ring (in table order) count its 4-neighbours
  (Manhattan distance 1) among the ring's entries so far, +1 if the point lies on an axis; when the count is below 2,
  append each of its N, E, S, W neighbours (orthog order) whose circle distance is `< r`. Appended points are searched as
  neighbours by later points but are not examined themselves; duplicates are possible. `ring_radius[r]` = cumulative
  count. The fillers make the rim 4-connected so that a unit moving one fog cell leaves no unstamped holes. Totals: 4781
  entries; per-ring filler counts are printed by `los-ref.mjs tables`.

### 3.2 Algorithm

`move` = 0 stamps the whole disc; `move` = 1 is the incremental update after a one-cell move (rim only).

1. **Visible-wonder test**: `vw = is_wonder() && !(flags & CITY) && !type.is_fort() && get_build().is_started()`.
   (`BuildData::is_wonder` @472320: TypeIndex 526..542; units and walls: a folded `return 0`. `BuildTypeData::is_fort`
   @472ba0: `is(FORT 443)`. `UnitTypeData::is_siege` @470460, used in §4: `unit_flags & 0x20000`.)
2. `los = los()` (vslot +0x128, §4.2). If 0, return. `r = (los·192)/384` (= `los/2` truncated), capped at 64.
3. **Centre and index range**:
   - `r < 4` and the object is a ground unit (`is_unit()`, `type.domain == 0`) that is not a packer (`unit_flags2 & 4`)
     and not a decoy (`unit_masks & UNIT_DECOY`): the centre is **projected 384 units ahead** along the unit's facing
     (`project(x, y, angle, 0x180)`: `x + sin(angle)·384`, `y − cos(angle)·384`, the sin table of movement-spec §1). `start
     = 0`, or with `move`: `start = circle_radius[0] = 1` (everything but the centre cell).
   - other `r < 4`: centre = own fog cell; `start = 0`, or with `move` and `r ≥ 2`: `start = circle_radius[r − 1]` (ring
     r only).
   - `r ≥ 4`: centre = own fog cell; `start = 0`; with `move`: `r = min(r, 32)`, `start = ring_radius[max(r−1, 0)]` and
     the **ring tables** are used.
4. **Detection bound** (only if `flags & DETECTOR`): ring case: `r = min(r, 31)` (this also changes the end below),
   `det = ring_radius[r]`; circle case: `det = circle_radius[r]`. Otherwise `det = 0`.
5. **End**: without `move`: `start = 0`, `end = circle_radius[r]` (circle tables, up to radius 64); with `move`: `end =
   ring_radius[r]` (ring case) or `circle_radius[r]`. Quirk: a detector with `r = 32` doing a ring update ends at
   `ring_radius[31] = start` and stamps nothing.
6. If not `move` and (`visible != 0` or `vw`): `update_local_seen()` (vslot +0x164, §3.3).
7. For each table index `k` in `[start, end)`: `(cx, cy) = centre + (X[k], Y[k])`; skip cells off the fog grid (the
   per-cell test is skipped when the whole `±r` box is inside). `new = World::set_seen(cx, cy, owner, k < det)`; if
   `new`, `World::reveal_fog(cx, cy, owner, o)` (§3.5). If `infiltrated != 0`: `World::set_seen2(cx, cy, infiltrated,
   0)` (the spies' players see the area as well, currently and explored).

The owner bit for players 8 and 9 truncates to 0, so gaia and slot 8 never mark anything (animals have LOS 0 anyway).
No `game_random` anywhere in §2–§5.

Shapes: `node re/tools/los-ref.mjs show <los> [full|move] [--project] [--detector]` prints the stamped cells (e.g.
LOS 14 = r 7: 185 cells full, a 68-cell 4-connected rim on a move).

### 3.3 `update_local_seen` (vslot +0x164)

- **`Unit::update_local_seen`** @60e410: if `visible != 0`, over `[0, circle_radius[type.x_size])` around the unit's own
  fog cell: `set_seen2(cell, visible, 0)`. (`x_size` = the rules `CIRCLE_RADIUS`, ≤ 10, used here directly as a radius in
  fog cells.) So an attacker or caster is revealed, with its surroundings, to the players it attacked.
- **`Wall::update_local_seen`** @63ed50 (buildings and walls): let `w = is_wonder && !(flags & CITY) && !is_fort`
  (the §3.2 test **without** the started part). `mask = 0xff` if `w` and started, else `ever_seen | visible |
  bit(owner)`; for tcoords `tx ∈ [corner_x − 1, corner_x + x_size]`, `ty ∈ [corner_y − 1, corner_y + y_size]` on the
  tile grid: `set_seen2((tx >> 1, ty >> 1), mask, explored_only = !w)`. A started wonder is therefore **currently
  visible to everyone** (footprint plus a one-tile border); other buildings just become explored for the players that
  remember them. (An unstarted wonder would write `seen` with the memory mask; no caller reaches it.)

### 3.4 `World::reveal_fog(fx, fy, who, o)` @6b3d30 (first exploration of a cell by `who`)

In order, with `t` = the tcoord `(2fx+1, 2fy+1)` and `w` its wcoord:
1. If `TData[t] & RESOURCE (0x200)` and `ObjectsData::find_good_at(w, who, 1, 0)` (the wcoord list's good terminal,
   §1.2, live and not oil; with p4 = 1 it returns the index without the `type_avail` test) finds a live good `g`:
   `Leader::new_rare(who, g)` (for AI-controlled owners: appends `g` to `new_rares` of every VALID, AI-controlled
   ally-or-self leader for whom the good's type is available, skipping fish (6) and whales (31) and duplicates), then
   `goods[g].ever_seen |= bit(who)`. (A "found rare resource" message for the local player is UI only.)
2. If `WData[w].flags & OIL`: the first live good of any type (lowest index in `[0, good_mark)`) whose position is in
   `w` is appended to `L[who].oil_patches` unless already present.
3. If `WData[w].flags & GOODY (0x8000)`: unless the wcoord is open ocean (not HALFLAND and `land ∈ {1, 2}`),
   `goody = find_goody_at(w)` and `items[goody].ever_seen |= bit(who)`. Then, if `o ≥ 0` and the stamping object is a unit
   with `unit_masks & UNIT_AUTO_EXPLORE`, **`Unit::get_goody_box(w)`**: a one-unit Group is pushed (`Groups::push_group`)
   and `Group::action_move_to(w·768 + 384, …, QUEUE_FIRST, EXPLORE_TO)` is issued. This is an order-system side effect of
   fog and depends on iteration order (§5.3).

---

## 4. LOS radius

### 4.1 `Unit::update_los` @60e4d0 (vslot +0x160)

Computes `mylos` (i8 arithmetic) in this order; later steps see earlier ones:
1. `mylos = L[owner] VALID ? type.los : 0`; if 0, return 0 (no bonuses).
2. Furs (rare bit FURS in `L.rare` or `L.rare_conquest`; rare bit index = TypeIndex − 6): `+ constants.furs_los`.
3. Dogs rare and the unit `is(SCOUT line 69)`: `+ dogs_scout_los`.
4. Citizens (type 50 PEASANTS or 51 PEASANTSKOREAN): `+2` if `L.tech` has bit 66, `+2` for bit 67, `+2` if bit 68 or
   `starting_town == 0` (nomad start). Bits 66..68 are TypeIndex MILITIA, MINUTEMAN, PARTISAN (the tech bitmask is
   indexed by TypeIndex; ❓ why the militia line drives this, see §11).
5. `+ science · type.science_los` with `science` = the low byte of the decrypted science epoch
   (`LeaderDataEncrypt.epoch[3]`, byte read `^ 0x87`).
6. Packers (`type.unit_flags2 & 4`: catapult-likes, merchants, fishermen…) that are packed (`UNIT_PACKED`) or packing
   (`UnitData::is_packing`): `mylos = min(mylos, 4)` (values above 3 become 4). Otherwise MERCHANT (61),
   MERCHANTDUTCH (62), FURTRAPPER (400): `mylos = science + 4` (replaces everything so far).
7. Units trained at BARRACKS / STABLE / AUTOPLANT (`type.where` 427/428/429): `+ n · troops_upgrade_los` with `n` =
   number of TROOPS_LOS_1..3 bonuses held (`has_preq`); and if the type has obj-mask HORSE_ARCHER (0x4000) or FOOT_ARCHER
   (0x400) and the Obsidian rare: `+ obsidian_archers_range`.
8. Spanish (tribe bonus 9) and `is(SCOUT)`: `+ spanish_los`.
9. Iroquois (tribe bonus 18): citizens, or barracks units that are not scouts: `+ iroquois_los`.
10. Siege (`type.is_siege`): Turks (8) `+ turk_siege_los`; Statue of Liberty (537) `+ liberty_siege_range`; Eiffel Tower
    (540) `+ eiffel_siege_range` (`LeaderData::has_wonder`).
11. `is(SPY 58)`: `+ n · spy_upgrade_los`, `n` = SPIES_UPGRADE_1..3 held.
12. Heroes (`type.unit_flags2 & 0x20`): `+ n · general_upgrade_los`, `n` = GENERALS_UPGRADE_1..3 held.
13. Decoys (`unit_masks & UNIT_DECOY`): `mylos = 1`.

Tribe bonus k = the tribe whose `tribe` field is k (rules.xml TRIBES order: 4 Nubians, 8 Turks, 9 Spanish, 18 Iroquois,
19 Lakota); `has_tribe_bonus` @6e1370 also honours CTW racial-power cards and is off with `GameInfo.flags & 4` or a
nomad leader without a city.

**`UnitData::los`** @6100c0 (vslot +0x128) returns `mylos` plus two live terms: `+ ptolemy_los_bonus` if the owner has a
PTOLEMY (362) hero, `type.role & 0x420`, and `ObjectData::has_general(0, PTOLEMY) ≥ 0`; `+ theceo_unit_los` if the owner
has THECEO (357), the unit is not siege and `has_general(0, THECEO) ≥ 0`.

`Animal::update_los` @5d7d50 = 0. `Object::update_los` @646fe0 (other object kinds) = `VALID ? type.los : 0`.

### 4.2 `Wall::update_los` @63eeb0 (buildings and walls)

- not started → 0; started, not active and not a wonder → 0; a started, unfinished wonder → `mylos = 1`, then the last
  step.
- active: `mylos = (VALID ? type.los : 0) + science · type.science_los`. Forts (`type.is_fort`): `+
  fort_upgrade_los[lvl − 1]`, `lvl = get_fort_los()` (FORTLOS4 → 4, FORTLOS3 → 3, FORTLOS2 → 2, else 1). Otherwise
  towers (`is(TOWER 439)`): `+ tower_fort_los[lvl − 1]`, `lvl = get_tower_fort_los()` (tech bit 68 → 4, 67 → 3, 66 → 2,
  else 1). Forts and towers with the Colosseum (530): `+ colosseum_fort_range`. Furs: `+ furs_los`.
- finally `mylos += type.x_size / 2` (the footprint half-size: building LOS is measured from the centre).
- `WallData::los` = `mylos` (no live terms).

### 4.3 When LOS is recomputed

`update_los` is called by `Unit::init` (before the first stamp), `Build::activate`, `Wall::init`, and by
`Leader::calc_unit_stats` (every alive unit) / `calc_wall_stats` (every alive building and wall), which run from
`Leaders::process_all` when `LEADER_CALC_UNIT_STATS` / `LEADER_CALC_WALL_STATS` is set (`gain_tech` sets them), and from
the script `set_type_line_of_sight`. A changed LOS is **not** stamped immediately: it takes effect at the object's next
`update_seen` (its next fog-cell change or the next refresh).

---

## 5. When stamping happens

### 5.1 Triggers of `update_seen`

| Caller | Call | When |
|---|---|---|
| `Unit::init` @612100 | `update_seen(0)` | creation (after `update_hits/los/speed/armor`), even for units created inside a building (stamps at the building centre) |
| `Unit::set_new_location(x, y, teleport, …)` @5f8d20 | `update_seen(teleport == 0)` | only when the unit is on the map (`inside_up < 0`) and its fog cell changes (old `tcoord >> 1` vs new `floor(x/384)`). Normal moves are rim updates; teleports (`come_out`, placement) stamp the full disc |
| `Build::activate` @623e20 | `update_seen(0)` | building finished |
| `Wall::activate` @63e4b0 | `update_seen(0)` | walls only (not `is_build`) |
| `GameDaemon::update_all_seen` | `update_seen(0)` | every refresh (§5.2) |

### 5.2 Refresh: `GameDaemon::update_all_seen` @732840

Called from `GameDaemon::process_all` when **`frame % 100 == 33`** (frame-spec §5 step 3, before `calc_markets`), from
`Game::run` once before the first frame (after `calc_danger`), and from `Build::close` (a Space Program owner loses
REVEAL_ENEMY), scripts `set_explored_show_buildings`, `add_visibility`, `remove_visibility`. With `reveal_map == 3` it
does nothing at all. Otherwise:
1. `busy = 4`; `clear_seen` (seen, wcoord_seen); zero `seen3`.
2. For leaders 0..7 that are VALID, buildings `[2000, build_mark)` then walls `[3000, wall_mark)`: a valid, active one →
   `update_seen(0)`; otherwise a valid, started wonder → `update_local_seen()` only.
3. For leaders 0..7 that are VALID, units `[0, unit_mark)`: valid and on the map → `update_seen(0)`. (Units inside
   buildings or transports give no vision; gaia and slot 8 are never visited.)
4. With scripted rules or CTW (`semaphore` byte1 & 0x10 or byte2 & 2): for each VALID leader p, each reveal point
   `{tx, ty, rad}` **of `ScenarioData::reveal_points[0]`** (the list of scenario player 1, for every p: the loop never
   indexes the array by p, asm @732a99..732bcc; entries are 16 bytes, `tx, ty, rad` at +4/+8/+0xc): `r =
   clamp(rad·192/384, 0, 64)`, over `[0, circle_radius[r])` around fog cell `(tx >> 1, ty >> 1)`: `set_seen(cell, p, 0)`
   and `reveal_fog(cell, p, −1)` when newly explored.
5. **Only at frame 0**: for leaders i = 0..7 that are VALID: `m = bit(i) | bits of j ≠ i` that are mutually allied with i
   (diplomacy 2 both ways) or in `L[i].ally_mask`. If `m` is not already covered by the union of earlier masks, and
   (`GameInfo.starting_resources` (+0x21) ≠ 0 or `reveal_map ≠ 0`, confirmed in the asm @732c97): every fog cell with
   `seen2 & m` gets `seen2 |= m` (allies start with each other's explored area). Then add `m` to the union.

### 5.3 Place in the frame and order dependence

- The refresh runs in step 11 of `Game::do_frame` (GameDaemon), before `Objects::process_all`; incremental stamps run
  inside `Objects::process_all` (units in the rotating player order of frame-spec §6, buildings 0..7) as units move, and
  inside command processing (placement, `come_out`).
- Grid bits are ORs, so the final bytes do not depend on order. **Readers do**: a unit processed later in the pass sees
  the cells stamped by units that moved earlier in the same frame. A port must stamp at the moment of the fog-cell
  change, never batch at the end of the frame.
- Order-dependent side effects: `reveal_fog` appends to `new_rares` and `oil_patches` (list order), pushes goody-box
  groups and orders (Group slot allocation, order queues), and only the first discoverer gets the "newly explored" return
  for its own bit. `Wall::check_ever_seen` calls `Leader::meet` (§6.1) in building order.

---

## 6. Other writers

### 6.1 Building memory: `Wall::check_ever_seen(force)` @63ce70

Runs from `Wall::process` when `frame ≠ 0 && (frame & 7) == owner` (each player's buildings every 8 frames), from
`Wall::init` and `Wall::start`, and with `force = 1` from the territory check below. If `ever_seen` or
`ever_seen_completed` lacks some bit of `S` (`S = game.everyone_mask` if started, else `L[owner].ally_mask`): over the
footprint tcoords, `ever_seen |= seen[fog cell] & (started ? 0xff : L[owner].ally_mask)` and, if active,
`ever_seen_completed |= seen[cell]`. If `ever_seen` changed (or `force`): for every VALID leader p ≠ owner with
`ever_seen & L[p].ally_mask` newly non-zero: if `L[owner].treaties[p] & 1` is clear, `Leader::meet(owner, p, x, y)`; and
if any such p, `update_local_seen()`.

`Wall::start` @63e810 marks the footprint explored for the owner (`WData.was_seen` and `seen2`, once per fog cell).
`Wall::activate` ORs `L[owner].ally_mask` into `ever_seen_completed`.

**ACTIVE_PING territory reveal** (`Wall::process`, every 32 frames staggered by `o`, during the territory-attrition
check, only when war is allowed): if the building stands in the territory of a non-allied leader e (docks: an adjacent
enemy via `check_enemy_adjacent`) who has LEADER_ACTIVE_PING and lacks the bit, `ever_seen |= bit(e)` and
`check_ever_seen(1)`.

### 6.2 Starting cities: `Object::update_seen_ally` @6537a0

Called by `Setup::build_cities`, `ConquestGame::build_cities`, `ScenarioRead::load_city_chunk`. For objects that are not
buildings-or-walls or are active: `r = los()`, or for a city centre `r = LeaderData::get_radius(city type) · 2 / 4`
(signed, rounding toward zero); cap 64. If the object `is_build`, `ever_seen |= bit(j)` for each VALID j in the share set
below. Then over `[0, circle_radius[r])` (note: for non-cities `r` is the LOS in tcoords used directly as a fog radius)
around the object's fog cell, for each VALID j in the share set: `WData.was_seen |= bit(j)`, `seen2 |= bit(j)` (explored
only). Share set: j = owner, or mutually allied, or (single player and both human), or (`game_rules == 8` and the
owner's team is 0).

### 6.3 Scripts (`ScenarioFuncSet`, scenario VM)

- `set_explored(p, tx, ty, rad)` @9e44b0: tiles in the box (x inclusive, y exclusive at the high end) with float distance
  `< rad`: `set_seen((tx >> 1, ty >> 1), p, 1)` (current + detect + explored); `p = 0` → every VALID leader.
- `set_explored(p)` @9e46f0: sets **LEADER_REVEAL_ENEMY** for p (sic). `set_explored_show_buildings(p)` @a03300: sets
  LEADER_EXPLORE_MAP, ORs bit(p) into `ever_seen` and `ever_seen_completed` of every live building of every VALID leader,
  then refreshes.
- `set_seen(p, who, o)` @9e4860: the building's footprint plus 2 tiles: `set_seen(cell, p, 1)`.
- `add_reveal_point` / `clear_reveal_points`; `add_visibility(p, q)` / `remove_visibility(p, q)` (set/clear bit q in
  `ScenarioData::ally_mask[p]` and in `L[p].ally_mask`, then refresh); `set_type_line_of_sight(type, los)` (sets
  `type.los` for every matching type, clamped 0..64, then `calc_wall_stats` + `calc_unit_stats` for ACTIVE leaders).
- Readers: `is_seen`, `object_visible`, `get_percent_explored`, `num_rare_resources_seen`.

### 6.4 Revealing attackers

- `Unit::set_attacking(p)` @5ff5b0 (the unit hits player p): diplomacy bookkeeping, `flags |= ATTACKING`; if `visible`
  lacks bit(p): set it, and if p does not currently see the attacker's fog cell (`WorldData::is_seen`),
  `update_local_seen()`. `Unit::do_cast` does the same for spell targets.
- `Build::do_attack` @6228f0: every 32 frames (staggered) clears `visible` unless ATTACKING; when it fires at player t,
  sets bit(t) and calls `update_local_seen()` if t had not seen the building (`ever_seen`).

### 6.5 Others

- Nuke impact (`Ammo::do_damage` @678060): tcoords within `circle_radius[clamp(splash + 1, 0, 64)]` of the impact tile
  (tcoord offsets, mapped to fog cells by `>> 1`): `set_seen2(cell, 0xff, 0)` (everyone sees it).
- Herd animals (`Animal::work` @5d7330, every 16 frames staggered by `o`): for each player p in 0..7 with `is_seen(cell,
  p)`, the herd's good gets `ever_seen |= bit(p)`. Quirk: the loop tests VALID on the animal's owner (gaia), not on p.
- `Leader::gain_tech` @6dcb60: the preq of bonus ALLY_LOS (Mathematics) → bit of every VALID ally into `ally_mask`; of
  EXPLORE_MAP_BONUS (Electronics) → LEADER_EXPLORE_MAP; of ACTIVE_PING_BONUS (Computerization) → LEADER_ACTIVE_PING; of
  REVEAL_ENEMY_BONUS → REVEAL_ENEMY. REVEAL_ENEMY_BONUS's preq is "disable", which the loader stores as −2
  (TYPE_ANY_BUILDING) and `LeaderData::has_tech` never holds, so only the Space Program (Build::activate, `is(542)`)
  and scripts set REVEAL_ENEMY. (`has_tech` @6e0c80: TYPE_NONE and anything below 50 are held; a unit needs its preqs
  and its bit, a building only its preqs, a tech its bit.)
  `Leader::init` @6e3930: EXPLORE_MAP / REVEAL_ENEMY (also the Space Program wonder 542) / ACTIVE_PING (Computerization)
  from bonuses already held.

### 6.6 `ally_mask` maintenance (order matters)

1. `Leader::init` @6e3930: `bit(who)`; for each slot j with `is_ally(j)` and (`has_preq(ALLY_LOS)` or `reveal_map ≠ 0`):
   `|= bit(j)`; later, the same rule again over VALID leaders. Scenario diplomacy (chunk 41) is written **after** this,
   so the initial mask reflects `Leader::init`'s default diplomacy, not the file's.
2. **`Leader::plan_strategy`** @6b9620, at frame 0 and every `200 / ai_speed` frames when `(who·25 + frame) % (200 /
   ai_speed) == 0`, for every VALID|ACTIVE leader (human ones too) unless `LeaderData+0x788` (production-AI mode):
   `ally_mask = bit(who)`, `| ScenarioData::ally_mask[who]` with scripted rules or CTW, and if `has_preq(ALLY_LOS)` or
   `reveal_map ≠ 0`: `|= bit(j)` for every ACTIVE j ≠ who that is mutually allied. This is where the scenario's diplomacy
   first reaches the mask (frame 0). In the same loop (same condition), for each ACTIVE j ≠ who not yet met
   (`treaties[j] & 1` clear) the leader **meets** j as soon as one of j's valid buildings (then walls) passes
   `is_seen(who, 0)`: `treaties` bit 1 both ways (the meet messages are UI), then the next j. (plan_strategy's off-cycle
   frames only run the AI's age-research check.)
3. `Leader::set_diplo` @6ec6a0: leaving an alliance clears both bits; entering one sets each side's bit when that side
   has ALLY_LOS or `reveal_map ≠ 0`.
4. `Leader::fix_tech_flags` @6d2480 (from `lose_tech`, the editor's test run): `bit(who)` plus allies if `has_tech(ALLY_LOS
   preq)` (no `reveal_map` clause).
5. `gain_tech` (§6.5) and `add/remove_visibility` (§6.3).

---

## 7. Queries

### 7.1 Grid queries (`WorldData`)

Common prefix "**unless**": return 1 if `who > 7`, or the reveal-map condition, or `L[who]` has REVEAL_ENEMY, or owns a
FOUCHE. `A = L[who].ally_mask`.

| Query | Address | Reveal-map condition | Rule after the prefix |
|---|---|---|---|
| `is_seen(fx, fy, who)` | 6b55c0 | `reveal_map == 3` | ACTIVE_PING and `WData.who` ≥ 0 allied with who → 1; else `seen & A` |
| `is_really_seen` | 6b42c0 | none | same as `is_seen` without the reveal-map test (used by `BuildTypeData::in_enemy_territory`) |
| `is_seen_wcoord(wx, wy, who)` | 6b2540 | `reveal_map == 3` | ACTIVE_PING territory test (via `get_whose`); else `wcoord_seen & A` (no callers) |
| `was_seen(fx, fy, who)` | 6b53f0 | `reveal_map > 1` | also 1 with EXPLORE_MAP; if `WData.who` = an ally a ≥ 0 and `L[a].reg_cities[region]` or `reg_forts[region]` ≠ 0 (an allied city or fort in that land region) → 1; else `seen2 & A` |
| `was_seen_wcoord(wx, wy, who)` | 6b25f0 | `reveal_map > 1` | as `was_seen` on `WData.was_seen & A` (renderer and mouse-over only) |
| `was_really_seen(fx, fy, who)` | 6b54f0 | `reveal_map == 3` | `seen2 & A` (no EXPLORE_MAP, no territory rule) |
| `is_detected(fx, fy, who)` | 6b48c0 | — | `seen3 & A` (no prefix) |
| `is_detected_by_enemy(fx, fy, who)` | 6b50f0 | — | `seen3 & ~A` (UI only) |

So: **was_seen** = "explored" for gameplay (territory and techs count), **was_really_seen** = "actually looked at"
(rare resources, fish, pathfinder cost), **is_seen** = "currently visible".

### 7.2 Per-object `is_seen(who, ignore_cloak)` (vslot +0x48) and friends

| Class | Address | Rule |
|---|---|---|
| `SubObjectData` (base) | 661f10 | owner → 1; else `was_seen(own cell)` |
| `UnitData` | 607a60 | unless `ignore_cloak`: a cloaked unit (below) without `UNIT_BLOW_COVER` (0x1000) that is not `is_detected(who)` → 0. Then owner or `reveal_map == 3` → 1; `WorldData::is_seen(own cell)` → 1; else `visible & bit(who)` |
| `WallData` | 642bd0 | owner → 1; need (started, or the owner's bit in `L[who].ally_mask`) **and** (`reveal_map == 3`, or `ever_seen & L[who].ally_mask`, or REVEAL_ENEMY/FOUCHE, or a CITY in single player with both human) |
| `BuildData` | 62e1a0 | `visible & bit(who)` → 1; `infiltrated & bit(who)` (who ≠ owner) → 1; else `WallData::is_seen` |
| `WallData::is_now_seen` (vslot +0x17c) | 642ca0 | owner → 1; unstarted and the owner not in `L[who].ally_mask` → 0; `reveal_map == 3` or (CITY, single player, human owner) → 1; else `WorldData::is_seen(centre cell)` |
| `BuildData::is_now_seen` | 62e130 | visible / infiltrated as above, else `WallData::is_now_seen` |
| `GoodData` | 66e4c0 | `ever_seen & A` → 1; fish (6), whales (31) or `who < 0` → `was_really_seen`; oil (5) → `was_seen`; other goods: Spanish (9) or Nubians (4) → `was_seen`, else `was_really_seen` |
| `ItemData` (ruins) | 677850 | `ever_seen & A` → 1; Spanish and the item's wcoord owned by who → `was_seen`; else `is_seen` |
| `AnimalData` | 5d8160 | `reveal_map == 3` → 1; non-herd: attached to an object (`ox ≥ 0`) → that object's `is_seen`, else `was_seen(cell)`; herd: herd good's `ever_seen` bit → 1; not fish/whale and Spanish/Nubian → `was_seen`, else `was_really_seen` |
| `CityData` | 739560 | the city building's `is_seen(who, 0)` |

**Cloak** (`UnitData::is_cloaked` @60a6a0): `UNIT_UNDER_COVER` (0x800), type flag 'o' (`unit_flags & 0x4000`),
`UNIT2_PASSIVE_CLOAK` (0x8000), or type flag 's' stealth (`unit_flags & 0x40000`) while the unit has no current order.
`UnitData::is_detected(who)` @60a630: owner → 1; else `seen3 & L[who].ally_mask` at the unit's fog cell.
`Unit::process_cloak` (every 16 frames) maintains the cloak masks (passive cloak for Iroquois infantry in own/allied
territory; cloaking generals); its particle emission is UI.

---

## 8. Readers that affect the simulation

- **Targeting**: `ObjectData::valid_target_const` @6472c0 (vslot +0x138, reached from `Object::valid_target` and so from
  `Object::find_nearby_target`, `Unit::find_melee_target`, `find_new_target`, `think`) requires
  `target.is_seen(attacker.owner, 0)`. `Unit::check_target_path` @5e22d0, `Group::action_attack` @712490 and
  `GameDaemon::do_danger` @732390 (danger halved for unseen units) call `is_seen` too. Because "seen" persists until the
  next refresh, a target stays valid for up to 100 frames after it left all LOS.
- **Pathfinding** (movement-spec §6.1, §8): `find_wpath` @688fc0 "fog branch" for human owners (goals in another tregion
  or unexplored are dropped; marching only toward explored goals; implemented in `engine/sim/pathfinder.cpp`);
  `UnitData::invalid_loc` @607c30 (`fog` argument, human owners: a tile whose 4 fog cells are all unexplored by
  `was_seen` is passable); `PathFinder::calc_cost` W level (`was_really_seen`); `calc_road_cost` (`was_seen`, cost ×2).
- **Building placement**: `BuildTypeData::blocked_site` / `blocked_tcoord` / `blocked_location` / `snap_center`
  (`was_seen`, `was_really_seen`; world-spec §6), `in_enemy_territory` (`is_really_seen`).
- **AI**: `Leader::check_explore` @6bc860 (explored count at `LeaderData+0x9d4`: one sample per 1536-unit region cell,
  `seen2` at fog cell `(4i+3, 4k+3)`, own bit only; `= reg_size` with EXPLORE_MAP; frame 0 and every `200/ai_speed`
  frames staggered by `who·25 + 12`), `Leader::plan_strategy` (`is_seen` of enemy buildings), `compute_site_stats`
  (`was_seen`), `Unit::think_scout` @5f6010 (unexplored targets from `seen2 & ally_mask`, plus `game_random`),
  `Unit::find_goody_box` @5f2540 (`was_seen`).
- **Decoys** (`Unit::process`): a decoy on the map whose fog cell is detected by an enemy (`seen3 & ~ally_mask`) suffers
  attrition (frame-spec §9 corrected accordingly; it used to say "a tile the owner cannot see"; confirmed in
  `Unit::process` @610bc0: the seen3 byte ANDed with the complement of the owner's ally_mask).
- `game_random`: **none** in the fog code itself; consumers (scouts, goody-box group moves, AI) draw as usual.

---

## 9. Renderer

The renderer reads the simulation state for the local seat (`console.who`) and never writes it (the only fog writes
from "Out" code are the render caches below).

### 9.1 Fog levels

Per fog cell, for the local player: **3** visible (`is_seen`), **2** explored and actually seen (`was_really_seen`),
**1** explored only (`was_seen`: territory, EXPLORE_MAP, `reveal_map ≥ 2`), **0** unexplored. `WorldMapFog` keeps them in
`fog_lookup`, a `(fog_xs + 2) × (fog_ys + 2)` byte array whose border replicates the edge cells
(`WorldMapFog::set_fog_lookup` @8d1300). Updates are incremental (`add_update` queue filled by `set_seen`/`set_seen2`/
`clear_seen`) or full when `fog.flags & 2` (set on reveal changes, techs, scripts, and when the set of players whose
borders can be seen changes: `WorldMapFog::fill_colors` @8d17b0).

### 9.2 Terrain fog overlay

Per wcoord a `FogTransition` holds 25 texcoords for a 5 × 5 vertex grid (192-unit spacing). Each value is
`fog_tcoord_lookup[s]` with `s` the sum of the levels of the four fog cells around the vertex (corner vertices), twice
the sum of the two cells on an edge, or four times the cell at a cell centre (`TerrainVis::fill_fog_texcoords` @8b19f0),
i.e. a bilinear sample of `level/3` with texel centres at fog-cell centres. `fog_tcoord_lookup[13]` @af2df4 ≈ `s/12`
(hex: 0, 0x3d2a9931, 0x3e2a9931, 0x3e7fe5ca, 0x3eaa9931, 0x3ed53f7d, 0x3effe5ca, 0x3f15460b, 0x3f2a9931, 0x3f3fec57,
0x3f553f7d, 0x3f6a92a3, 0x3f800000; entry 1 is 1/24, not 1/12). The value is the u coordinate into the tileset's
`FOGTEX` (tilesets.xml; `art/fow_fade.tga`, 256 × 1 RGBA): black with alpha falling from 255 (u = 0) through 175 (1/3),
76 (2/3) to 0 from u ≈ 0.83 (texels ≥ 211 are white, alpha 0). So: unexplored black, explored-only about 69 % dark,
seen-but-not-visible about 30 % dark, visible clear, with smooth gradients across cell edges. `art/fow_fade_dark.tga` is a
darker variant. Values fade from the old to the new texcoords over render frames (`TerrainOut::inc_fog_time` @86d8e0:
per-vertex step `(new − old) · X / 30000`, at least 0.001 in magnitude, X = the Scene's first field; snaps when the
scalability flag is off). Metal port: an R8 texture of `level/3` per fog cell (clamp to edge, linear filter), sampled at
`(x/384, y/384)`, mapped through `fow_fade.tga`, drawn as a black alpha overlay on terrain, water and terrain dressing.

Not fog of war: `TerrainOut::set_visible / clear_visible / is_visible` (camera frustum culling), `TerrainOut::
wcoord_is_seen` (stub, returns 0), and the `master_fog / current_fog / scene_fog` distance fog (lighting-spec).

### 9.3 Objects

- **Enemy units**: drawn only if `is_seen(console.who, 1)` (cloak ignored here; `GuyOut::draw` checks `is_detected` for the
  cloaked look). Observers and defeated players (`semaphore byte0 & 0x42`, or the seat's player flags 0x100 / 0xd0
  outside tournament rules) see everything and get no minimap fog.
- **Buildings**: drawn if `BuildData::is_seen` (remembered via `ever_seen`); tinted **WHITE** when currently seen
  (`is_now_seen`, or for units `is_seen`) and **GREY** when only remembered (`GraphicPieces::draw_build` @8e7610). The
  sorter (`ObjectsOut::sort_all` @657320) uses `is_now_seen` for universities (type 420).
- Trees (`RTreeData::adjust_tree_colors`): GREY when the cell is seen, DARK_GREY (or −0x3f per channel) when not; units
  drawn in an unseen cell get DARK_GREY; goods (`draw_good`), deaths, ammo, doobers, grass, river doobers, signposts and
  particles are skipped or dimmed by `is_seen` / `was_seen` of their cell.

### 9.4 Minimap

`WorldMapFog::fill_colors_fog` @8d1420 writes the fog texture (`fog_xs × fog_ys`): luminance 0xff (level 3), 0x80 (2),
0x50 (1), 0 (0), grey RGB. `WorldMap::render_fog` @8cc1f0 draws it as one quad over the map with `fog_render_state`
(🟡 blend not decoded; multiplying the map by the luminance reproduces the look). Territory colours come from
`fill_colors_who` (only for players whose borders `LeaderData::can_see_borders` allows). Blips: units that pass
`is_seen`.

### 9.5 Graphic events and sounds

`GraphicEvents::process` @8e50a0 (frame-spec step 15) every 32 frames ORs `seen[event cell]` into each event's mask and,
for each newly added bit i, also `L[i].ally_mask`. It is saved but not checksummed and nothing in the simulation reads it;
`GraphicEventsOut::play_sound_if_seen` and the effect drawing use it. A port may treat it as presentation (keep it if
byte-exact saves matter).

---

## 10. Port plan

Done (engine/sim/fog.cpp, 2026-10-12); kept for reference. What remains stubbed is listed in
docs/sim-implementation.md ("Fog of war").

Files are under `engine/sim/` unless noted; the world arrays already exist (`world.cpp`: `seen`, `seen2`, `seen3`,
`wcoord_seen`, `WData.was_seen`).

1. **`compass.{hpp,cpp}`**: apply the 0x3249 cap in `make_circles` (ring 64 truncated; `radius[64] = 12873`) and add
   `Rings` (`ring_init`, 33 radii, 4781 entries). Check both against `node re/tools/los-ref.mjs dump circle|ring` (hashes
   in `los-ref.mjs tables`).
2. **`fog.{hpp,cpp}`** (new): `set_seen`, `set_seen2`, `clear_seen`, `clear_seen3`; the queries of §7.1 taking `Game&`
   (leader flags, `ally_mask`, `num_units[FOUCHE]`, `reg_cities/reg_forts`, `reveal_map`, territory `WData.who`); replace
   the `was_seen` / `was_really_seen` stubs in `game.cpp`.
3. **Leader state**: `ally_mask` (Leader::init rule; plan_strategy refresh at frame 0 and every `200/ai_speed` frames
   staggered; set_diplo; fix_tech_flags; gain_tech ALLY_LOS), flags 0x800/0x1000/0x2000, `oil_patches`, `new_rares`,
   `ScenarioData::ally_mask` and `reveal_points`.
4. **LOS** (`objects.cpp` or `los.cpp`): `unit_update_los` (§4.1, constants from the rules image), `UnitData::los` live
   terms, `wall_update_los` (§4.2), Animal = 0; call sites of §4.3.
5. **Stamp**: `object_update_seen(obj, move)` exactly as §3.2 (projection with the existing `sin_a`/`cos_a`), unit and wall
   `update_local_seen`, `reveal_fog` (rares, oil, goody items, `get_goody_box` → group EXPLORE_TO move when groups allow;
   until then log it as a known deviation).
6. **Hooks**: `unit_init` (after LOS), `unit_set_new_location` (`guys.cpp:335` TODO), building activation, `wall_start`
   (add the `seen2` write next to `was_seen` in `placement.cpp`), `update_seen_ally` at scenario city load,
   `GameDaemon::process_all` (`frame % 100 == 33`, before `calc_markets`) and the pre-frame-0 call in `Game::load`
   (with the frame-0 ally share), `busy = 4`.
7. **Object memory**: `visible` (set_attacking/do_cast/Build::do_attack set it; the 32-frame clears in `work` and
   `do_attack`), `infiltrated`, `ever_seen(_completed)` with `check_ever_seen` in `Wall::process` and `Leader::meet`,
   Good/Item `ever_seen`.
8. **Readers**: per-object `is_seen` family (§7.2) behind one `object_is_seen(game, who, o, viewer, ignore_cloak)`;
   wire into `valid_target_const` when combat lands, `invalid_loc` (already uses `was_seen`), `calc_cost`,
   `calc_road_cost`, placement.
9. **Digest** (`apps/sim` FNV): add `seen`, `seen2`, `seen3`, `wcoord_seen`, `WData.was_seen`, per object `mylos`,
   `visible`, `infiltrated`, `ever_seen`, `ever_seen_completed`, per leader `ally_mask`, the three leader flags,
   `oil_patches`, `new_rares`, `GameDaemon.busy`.
10. **View API** (`view.{hpp,cpp}`): per fog cell level 0..3 for a chosen seat, per object `seen_now` / `remembered`
    flags; the renderer builds §9.2's texture and §9.3's tints from it (Metal: `platform/metal`, `apps/viewer-mac`).
11. **Tests**: `los-ref.mjs` shapes vs C++; a harness check that a stationary unit's area survives a refresh and a
    moving unit's trail disappears at the next `frame % 100 == 33`; later, CHECK_SUMS part 12 against a multiplayer
    replay.

## 11. Open questions

1. Tech bits 66..68 (TypeIndex MILITIA, MINUTEMAN, PARTISAN) drive the citizens' LOS bonus and `get_tower_fort_los`.
   The code is unambiguous; whether these are really "militia line researched" or the type slots are reused as tech
   flags was not traced.
2. Science epoch: `epoch[3]` read through its low byte with key 0x87 is taken as the science level from the field name
   `science_los`. Supporting evidence (port): `has_wonder` compares `epoch[1] ^ 0x63187` with `ctw_wonder_min_civics`,
   so the epochs run in the reverse of the tech table's line order (techs 572..578 military, 565..571 civic, 558..564
   commerce, 551..557 science), making `epoch[3]` the science line. The port uses the number of techs 551..557 held.
3. `UnitData::los` Ptolemy term tests `type.role & 0x420`; the role bit names are unknown.
4. Renderer fade: the Scene field `X` in the fade step (§9.2) and the minimap `fog_render_state` blend.
5. `Object::update_seen_ally` uses the raw LOS (tcoords) as a fog-cell radius for non-city buildings, twice the usual
   size; confirm with a scenario start screenshot. (For cities it uses `LeaderData::get_radius` @6db790 of the city's
   building type: `city_center_radius + (size − 1)·city_center_pop_radius (+ indians_city_radius with tribe bonus 21)`,
   capped at 64, size 2 for type 415, 3 for 416 and 531, else 1.)
6. Units created inside a building stamp at the building centre in `Unit::init` (no on-map test); harmless but visible
   in the grid until the next refresh.
7. `los_x/los_y` (UnitData +0x78) are set at init; no reader was found in the fog code.

## Evidence

| Function | Address | Used for |
|---|---|---|
| `World::set_seen / set_seen2 / set_was_seen / clear_seen / clear_seen2 / clear_seen3 / reveal_fog / get_seen / get_was_seen / adjust_fog` | 6b3c60 / 6b4bb0 / 6b41d0 / 6b2250 / 6b2160 / 6b2140 / 6b3d30 / 6b4290 / 6b1ba0 / 6b76a0 | §2, §3.4 |
| `WorldData::is_seen / is_really_seen / is_seen_wcoord / was_seen / was_seen_wcoord / was_really_seen / is_detected / is_detected_by_enemy` | 6b55c0 / 6b42c0 / 6b2540 / 6b53f0 / 6b25f0 / 6b54f0 / 6b48c0 / 6b50f0 | §7.1 |
| `Object::update_seen / update_seen_ally / update_los`, `ObjectData::los / is_jammed` | 651b80 / 6537a0 / 646fe0, 6450b0 / 653660 | §3, §4, §6.2 |
| `Unit::update_los / update_local_seen / set_new_location / init / set_attacking / do_cast / process_cloak / get_goody_box / think_scout / find_goody_box`, `UnitData::los / is_seen / is_cloaked / is_detected` | 60e4d0 / 60e410 / 5f8d20 / 612100 / 5ff5b0 / 5ebfe0 / 5e1c20 / 5f7690 / 5f6010 / 5f2540, 6100c0 / 607a60 / 60a6a0 / 60a630 | §3–§8 |
| `Wall::update_los / update_local_seen / check_ever_seen / start / activate / process`, `WallData::is_seen / is_now_seen`, `BuildData::is_seen / is_now_seen`, `Build::activate / close / do_attack` | 63eeb0 / 63ed50 / 63ce70 / 63e810 / 63e4b0 / 640450, 642bd0 / 642ca0, 62e1a0 / 62e130, 623e20 / 628980 / 6228f0 | §4.2, §6, §7.2 |
| `GameDaemon::update_all_seen / process_all`, `Game::run` | 732840 / 732700, 584590 | §5 |
| `circle_init / ring_init / project`; tables `circle_radius` cbe330, `ring_radius` cb7e00, `fog_tcoord_lookup` af2df4 | 6817f0 / 681920 / 92cf40 | §3.1, §9.2 |
| `Leader::init / plan_strategy / set_diplo / fix_tech_flags / gain_tech / calc_unit_stats / calc_wall_stats / meet / new_rare / check_explore`, `LeaderData::is_ally / has_preq / has_tech / has_tribe_bonus / has_wonder / get_fort_los / get_tower_fort_los / get_troops_los_upgrade / get_spy_upgrade / get_general_upgrade / can_see_borders` | 6e3930 / 6b9620 / 6ec6a0 / 6d2480 / 6dcb60 / – / – / 6e1250 / 6d9e70 / 6bc860, 6edb50 / 6db810 / – / 6e1370 / 6ebc10 / 6daf30 / 6daf80 / 6e1110 / 6e1090 / 6e0830 / 6d5f60 | §4, §6.6, §8 |
| `GoodData / ItemData / AnimalData / CityData / SubObjectData ::is_seen`, `Animal::work / update_los` | 66e4c0 / 677850 / 5d8160 / 739560 / 661f10, 5d7330 / 5d7d50 | §6.5, §7.2 |
| `ObjectData::valid_target_const`, `Object::valid_target / find_nearby_target`, `Unit::check_target_path`, `Group::action_attack`, `GameDaemon::do_danger`, `Ammo::do_damage` | 6472c0, 648ba0 / 648da0, 5e22d0, 712490, 732390, 678060 | §6.5, §8 |
| `ScenarioFuncSet::set_explored ×2 / set_explored_show_buildings / set_seen / add_visibility / remove_visibility / add_reveal_point / set_type_line_of_sight` | 9e44b0, 9e46f0 / a03300 / 9e4860 / 9fc710 / 9fc7b0 / 9e4750 / a00550 | §6.3 |
| `GraphicEvents::process`, `WorldMapFog::add_update / fill_colors / fill_colors_fog / set_fog_lookup`, `WorldMap::render_fog`, `TerrainVis::fill_fog_texcoords`, `TerrainOut::inc_fog_time / invalidate_fog_cache(_wcoord)`, `GraphicPieces::draw_build / draw_unit / draw_good`, `RTreeData::adjust_tree_colors`, `ObjectsOut::sort_all`, `TileSet::get_fog_alpha_texture` | 8e50a0, 8d2760 / 8d17b0 / 8d1420 / 8d1300, 8cc1f0, 8b19f0, 86d8e0 / 866980, 8e7610 / 8e7da0 / 8e9080, 9236f0, 657320, 87ab60 | §9 |
| Enums `LeaderFlagIndex`, `UnitMask`, `UnitMask2`, `TypeIndex`; classes `WorldData`, `ObjectData`, `UnitData`, `WallData`, `LeaderData`, `Constants`, `GameInfo`; virtual slots from the PDB method lists | `rise.pdb` | field and slot names |
| Game data: `rules.xml` (`revealmaps`, TECHBONUSES preqs, TRIBES order), `help.xml` (FOUCHE), `tilesets.xml` (FOGTEX), `art/fow_fade*.tga` | user install | §1.3, §4, §9.2 |
| (port, `re/decomp/fog2/`, jobs `re/jobs/dc_fogport1.txt`, `dc_fogport2.txt`) `Objects::init_good / init_item`, `ObjectsData::find_good_at (WCoord) / find_goody_at (WCoord)`, `BuildData::is_wonder`, `BuildTypeData::is_fort`, `UnitTypeData::is_siege`, `LeaderData::get_team / is_team / type_eligible`, `Leader::treaty_on`, `Leaders::strategy_all`; `Leader::plan_strategy` (`re/decomp/grid_regions/`), `circle_init` (`re/decomp/grid_misc/`), `Unit::process` (`re/decomp/combat_D/`) | 653f30 / 653e00, 65bec0 / 65c040, 472320, 472ba0, 470460, 6ec040 / 6ebd30 / 6dbd10, 6e1190, 6ed430; 6b9620, 6817f0, 610bc0 | §1.2, §3.2, §5.2, §6.6, §8, §11 |
