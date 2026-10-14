# Animation lengths in the simulation

How the original decides, for the guy animation clock (`Guy::set_anim`, `Guy::inc_time`, `Guy::init_real`), whether a
figure has an animation, how many frames it lasts and whether it loops. These values feed the lockstep simulation:
the clock decides when idle variants are re-picked (one `game_random` draw each time), when attacks end and when
walks restart, so they must match the original bit for bit. Port: `engine/sim/anims.hpp` (interface),
`engine/sim/anim_assets.{hpp,cpp}` (loader), `engine/sim/guys.cpp` (`unit_gpiece`, `guy_update_gpiece`, the clock).

Evidence is from the decompiles in `re/decomp/animlen/`, `re/decomp/anim/`, `re/decomp/move_guy/`,
`re/decomp/gpieces/`, `re/decomp/scenario_objects/` (gitignored); the job files are `re/jobs/animlen*.txt`.

## 1. Summary

```
guy ──Guy::update_gpiece──▶ graphics piece (index) ──unit_graphics.xml <UNIT name=…>──▶ AnimationPacket
     (type, owner age & nation, o parity, packed, guy slot)            action_ids[UnitAnim] ──▶ animmgr entry
animmgr entry (anim_graphics.xml, in file order) ──▶ looping (section) , times (ms, .bha first node) ──▶ frames
```

- **Key**: the guy's graphics piece, not the unit type. The piece encodes unit type, unit style (the owner's tribe
  `UNIT_CONTINENT`), age bucket (owner's age), the gender slot (`-FEMALE` for odd `o` civilians, `-PACKED` for packed
  units) and the guy slot (`-CREWn`), with fallbacks (section 2).
- **Action → animation**: the piece's `<UNIT>` element lists `<ANIM name="CHAR_…" file="<anim name>"/>`; the anim
  name is looked up in `anim_graphics.xml` (section 3).
- **Length**: `times` = the cumulative key time (ms) of the last key of the `.bha` file's first node, or of the
  key before it for non-looping animations; `frames = round_half_up(times · 3 / 200)` (15 simulation frames per
  second). Missing entry or file: 3 frames (section 4).
- **Looping**: the anim's section in `anim_graphics.xml` (`LOOPING` → loops; `NONLOOPING`, `BUILDING`, `PATH` → not).

Nothing on this path depends on graphics settings, the renderer, timing or load order (section 6).

## 2. The graphics piece (the key)

### 2.1 When it is computed

`Guy::update_gpiece` @5d8530 stores `GraphicPieces::get_unit_gpiece(...)` in `Guy+0x88`. Callers:
`Guy::init_real` @5db6b0 (first thing, before the idle draw), `Unit::update_gpiece` @5e2920 (guys `[0, guy_mark)`
and the crew `[squad_size, squad_size + crew_size)`), which runs at the end of `Unit::init`'s guy loop, in
`Unit::set_type`, for every unit of a leader in `Leader::gain_tech` @6dcb60 (age advance), in
`Unit::kill_current_order` @5e2cb0 (a merchant finishing a pack cast), twice in `Group::action_move_near` @704990
(when it replaces a unit's order list) and in `Objects::reload_data` @65d9c0 (graphics reload; same inputs, same
result). In `Unit::init` new guys get `guy_num` (Guy+0xa2) before `init_real`, so the first computation already
uses the right slot.

Arguments: the unit's type, the guy's owner `who` and `o`, `guy_num` as the crew index, and "packed": for types
MERCHANT (61), MERCHANTDUTCH (62) and FURTRAPPER (400) `unit_masks & 0x80000` or `UnitData::is_packing` @60aa60 (the
current order is a cast of a pack spell); for all others `unit_masks & 0x80000`. The age offset (`param_7`) and the
"load over time" flag (`param_6`) are 0 here.

`update_gpiece` also sets the **crew track offsets** (`Guy+0x54/+0x58`, `track_dx/dy`, used by `Guy::set_angle` /
`set_new_location` / `do_turn` to place trailing crew): for `guy_num != 0` and a loaded piece,
`track_dx = cvttss2si((trackoffsetx · guy_scale) · scale)`, `track_dy = cvttss2si((guy_scale · trackoffsety) · scale)`
(single precision, @5d8633..5d8672), else 0. `guy_scale` @c06244 is 4.8; `scale` is the piece's `scale` attribute
(< 0.1 or absent → 1); `trackoffsetx/y` are read by `GraphicPieces::init_unit_data` @8f4290 only for crew pieces
(`piece / per_crew > 0`) of unit types (default 0). `init_real` zeroes the offsets after its own `update_gpiece`,
so in `Unit::init` they come from the `Unit::update_gpiece` that follows.

### 2.2 Piece index layout

`GraphicPieces::init_piece_ranges` @8f70e0: unit pieces start at 0; `num_unit_pieces` 0x160 (one per type
50..401), `per_age` 0x840 (6 styles), `per_gender` 0x18c0 (3 age buckets), `per_crew` 0x3180 (2 gender slots),
`total_num_unit_pieces` 0xc606 (4 crew slots + 6); building pieces follow; birds/gaia animals at 0xea8d (0x24
pieces, 3 per type 402..413).

```
unit piece = (type − 50) + style·0x160 + bucket·0x840 + gender·0x18c0 + crew·0x3180
```

### 2.3 Selection (`GraphicPieces::get_unit_gpiece` @90c030)

- **Gaia animals** (`type − 402 < 12`, unsigned): `k = (GameInfo.seed + o) mod 3` (unsigned); try
  `0xea8d + (type − 402)·3 + k`, then `k − 1` … 0; the first loaded piece wins ("NO GAIAN GPIECE" otherwise).
- Otherwise, for an owner (`who != −1`): age bucket `b = age < 5 ? age / 3 : 2` from the leader's current age
  (stored XOR 0x62766); style `s` = the tribe's `UNIT_CONTINENT` (Tribe+0x68).
  - **Merchant specials** (types 61, 62, 400, not packed): with build style `bs` (Tribe+0x64), `bs == 5` (ARAB)
    forces `s = 1` (ARAB). If `(b == 1 or (b == 0 and bs == 6)) and (bs ∈ {1, 3} or bs ≥ 6)`, try the special piece
    `total − 6` (KOREAN 3), `total − 5` (NEUROPE 1), `total − 4 / −3` (IROQUOIS 6, AGE0 / AGE3), `total − 2`
    (COLONIAL 7), `total − 1` (EINDIAN 8); if loaded, done.
  - **Gender slot** when packed, or when the type lacks UnitType flag 4 (`unit_flags2 & 4`) and `o` is odd: try
    `(b..0, s, gender 1)`, then `(b..0, style 0, gender 1)`.
  - Then `(b..0, s, gender 0)`, then `(b..0, style 0, gender 0)`.
  - Nothing loaded: piece 0 ("NO DEFAULT UNIT GPIECE").
  The crew index is the same in every candidate (no fallback to slot 0).
- "Loaded" = `GraphicPieces::verify_load` left `data_pieces[piece] != 0`: `init_unit_data` found the `<UNIT>`
  element and the model file exists (`file_exist` before `RData::load`; a tileset child element such as `<ARCTIC>`
  may override model and texture).

### 2.4 Piece → `<UNIT>` name (`GraphicPieces::unit_node_from_gpiece` @901c30)

`<GRAPH>-<STYLE>-AGE<0|3|5>[-FEMALE|-PACKED][-CREW<n>]`, matched case-insensitively against `UNITS/UNIT@name`
in `unit_graphics.xml` (first match):

- type = `piece mod 0x160 + 50` (`get_type` @902540); GRAPH = the type's graph name (TypeData+0x88).
- style = `(piece mod per_age) / 0x160` (`cont_from_gpiece` @907990, > 5 invalid), named by
  `say_unit_art_style_name` (0 DEFAULT, 1 ARAB, 2 AMERICAN, 3 ASIAN, 4 NA, 5 INDIA; empty → DEFAULT), uppercased.
- age = `((piece mod per_gender) / per_age) · 3`, 6 → 5.
- gender slot (`piece mod per_crew ≥ per_gender`) is `-PACKED` when the type's role has the attacker bit 0x10000
  (UnitType+0x2c8), or `TypeData::is_merchant` @42dbd0 (61, 62, 400), or the type is FISHERMEN (317); else `-FEMALE`.
  (So the shipped `ASSAULTMARINES-…-FEMALE` graphics are unreachable.)
- crew = `piece / per_crew`; `-CREW<n>` when n > 0.
- The six specials are `MERCHANT-KOREAN-AGE3`, `-NEUROPE-AGE3`, `-IROQUOIS-AGE0`, `-IROQUOIS-AGE3`,
  `-COLONIAL-AGE3`, `-EINDIAN-AGE3`; gaia pieces are `<GRAPH>-TYPE<k>`.

Every unit in the 156 stock scenarios resolves to an existing piece (`ron-sim --anims`).

## 3. Actions and the animation manager

- **Entries** (`GraphicPieces::init_anims_pool` @8fca40 → `AnimMgr::add` @53ac00): `anim_graphics.xml`
  `ROOT/ANIMATIONS/LOOPING/ANIM` (looping = 1), then `NONLOOPING`, `BUILDING`, `PATH` (looping = 0), in file order;
  entry index = position (2073 stock entries). `GraphicPieces.anim_names` keeps the names.
  `GraphicPieces::decipher_animation` @8face0 returns the first entry whose name equals the given one
  (case-insensitive), −1 if none ("Caravan3 Wagon Walk" appears twice; the first wins).
- **Packet** (`init_unit_data`, the anim block after the model loads): `AnimationPacket::init_animation` @918650
  sets actions 0..37 to −1, then for every `<ANIM>` child of the `<UNIT>` with a non-empty `file`: the action =
  index of `name` in `UnitAnimNames` (initializer @40ee70, same order as the `UnitAnim` enum, `CHAR_DEFAULT` …
  `CHAR_FARM`; unknown names report an error and use 38), the entry = `decipher_animation(file)` (no match: error,
  skipped); `init_animation(action, entry)` overwrites earlier duplicates. (A buggy extra `set_action_id` for
  "attack" rows indexes `UnitAnimCat` by the element counter; it writes the same value, harmless.)
- **Queries** on a packet and action (all `BasicRenderPackets.obj`):
  `get_animobj` @918be0 = `animmgr.actions[entry]` after `force_load` (non-null iff the `.bha` loaded);
  `get_anim_time` @918c40 = `animmgr.times[entry]`, 200 without an entry; `get_game_frames` @918cc0 =
  `animmgr.frames[entry]`, 3 without an entry; `is_looping` @918d20 = `animmgr.loopings[entry]`, 0 without.
  `Guy::set_anim` inlines the frames lookup for `end_time` (@5db507, 3 without an entry) and the "has" test;
  `Guy::inc_time` @5d9e10 inlines `is_looping` (loops → `set_anim(same)`, else DEFAULT / WALK for ATTACKWALK /
  the attack-queue path).

## 4. Lengths (`AnimMgr::force_load` @53ade0, `AnimObj::load_hier` @54b700)

`force_load` reads the entry's file once (later calls return early because the name string is cleared; the
incremental loader `AnimFileLoader::post_load` @53a4e0 calls it with the prefetched bytes, same result):

1. Chunk walk: id 0 descends, the **first id-8 chunk** is the root node (others are skipped), id 10 is skipped;
   any other id spins until "Invalid Animation File". No root or no file → `actions[i] = null`, `times = 200`.
2. `load_hier`: the node's first child chunk is its key track: `u32 n` (> 100000: "Illegal number of nodes"), then
   `n` keys of 36 bytes; key time accumulates `cvttss2si(dt · 1000.0f) & 0xffff` (MULSS, single precision) and is
   stored as u16. Redundant keys are then compressed, but the last two key times always survive.
3. `times = key_times[last]`; for a non-looping entry with a non-zero last time, the last key time is replaced by
   the one before it (`times = key_times[last − 1]`; the AnimObj is modified, so the renderer also stops there).
4. `frames = cvttss2si(f)`, plus 1 if `f − frames ≥ 0.5`, with `f = float(times · 3) / 200.0f` (CVTDQ2PD,
   CVTPD2PS, DIVSS). For `times < 65536` this equals `(times · 3 + 100) / 200` in integers.

Stock data: all 2065 present files have a first node with ≥ 2 keys and give ≥ 1 frame; 8 entries name missing
files (3 of them used by units: they have no AnimObj and last 3 frames).

Examples (`ron-sim … tut1.scx --anims`): Hoplite AGE0 DEFAULT 33*, IDLE1 72*, WALK 15*, JOG 12*, ATTACK1-3 32,
deaths 14–30; Light Horse WALK 11*, ATTACK 25; Slingers IDLE1 250* (16.7 s).

## 5. Uses in the simulation

- `Guy::init_real`: after `update_gpiece` and the idle draw, the drawn variant is kept only if the packet
  `get_animobj`s it (no packet: "No Animation Packet", variant kept). Flag 8 (turn animations) = packet has more
  than 22 actions and `get_animobj(TURN_RIGHT)`.
- `Guy::set_anim`: every "has" test (TURN, GROUP_IDLE2, idle/attack/walk variants) is `get_animobj`; `end_time` =
  frames; the walk-variant switch rescales `cur_time · T / T` with `T = get_anim_time(cur_anim)` (both calls read
  the old animation; 0 → 1).
- `Guy::inc_time`: when `end_time ≤ cur_time`, a guy whose piece did not load hits "DO NOT IGNORE. FATAL. No
  Animation Packet" (returns when ignored); otherwise loop / non-loop handling by `is_looping`.

## 6. Lockstep notes

- Inputs: unit type, owner age and tribe, `o`, `guy_num`, the packed mask, GameInfo.seed (gaia) and the install's
  XML and `.bha` files. None depend on graphics options: `init_unit_data` only sets a render flag from
  `graphsys->vertex_process_mode`; texture/detail settings do not change which pieces load or what the packets hold.
  Lazy and incremental loading give the same values in any order.
- Machine-dependent only through bad data or tools: a `.bha` whose first node has one key and is non-looping (or
  no keys) makes `force_load` read before the key array (heap contents; none in stock data; the port uses the last
  key, or treats no keys as missing); a modified install (missing/changed `.bha` or XML) desyncs, as in the
  original; the developer console can rewrite `guy_scale` (@7d6e93, @7d6f8f, @7d6ff7, console command parser),
  changing the crew track offsets.
- Floating point: key times and frames are single-precision IEEE operations with truncation; the port compiles them
  with `fp contract(off)` and reproduces them exactly (the frames formula is also exact in integers).

## 7. Port notes and open questions

1. `has_packet` is taken as "the piece loaded": every unit `<UNIT>` with a model is assumed to get an
   AnimationPacket from `RData::load` (packets[0x13]); not traced.
2. Tileset model overrides (`<ARCTIC>`, `<SNOWY>`, … children) are not considered for "loaded": the port checks the
   base model only (all stock models exist).
3. Not ported yet (the callers do not exist in the sim): `UnitData::is_packing` (merchant pack casts; taken as
   false), the `update_gpiece` calls in `Unit::set_type`, `Leader::gain_tech`, `kill_current_order` (merchant
   pack) and `Group::action_move_near` (`groups.cpp` must call `unit_update_gpiece` where it replaces a unit's
   order list), and the age offset `param_7` (0 for every simulation caller seen).
4. The leader age comes from the scenario's age techs (count of known techs 544..550); the sim has no tech effects
   yet. Conquest tribe overrides are not applied to the sim's tribes (the renderer's battlefield uses them).
5. The turret flag 0x100 in `init_real` (pivot restrictions keyed by `get_type(gpiece)`) is still a TODO.
