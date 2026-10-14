# Rules data model — implementation spec

Status: ✅ `re/tools/rulesdump.mjs --replay re/analysis/sample_replay.bin` loads the stock XML and reproduces that
replay's 1,024,221-byte rules image **byte-identically** (all 806 types, 848 constants, 493×493 balance cells,
24 tribes). "Oracle" below means that replay image. Unwalked fields (strings, masks, UI data) are reversed from
code but cannot be checked this way.
Reference implementation: `re/tools/rulesdump.mjs` (loader, CLI), `re/tools/rulesxml.mjs` (XML access and value
parsers), `re/tools/rulesimage.mjs` (rules-image codec). All names below are the original PDB names.

This document describes every table the simulation reads from the rules XML, how the original loader fills it, and
how to prove a reimplementation produces the same bytes. Addresses are `riseofnations.exe` VAs (image base 0x400000).

## 1. Key facts

- **The rules image is the oracle.** `Game::walk_rules_data` @00589550 serialises every simulation-relevant rules
  field as raw bytes. `RecordGame::write_rules` @00953630 stores that image in every `.rcx` replay, right after the
  game has been set up (`Game::run` @00584590, only when the game was not started from a save). During playback
  `RecordGame::read_rules` @00953610 loads it **over** the XML-loaded tables. So:
  - replay playback must take the walked fields from the replay, not from XML, exactly like the original;
  - a new-game loader is correct when its tables, serialised the same way, equal the replay's image byte for byte.
  `re/tools/rulesdump.mjs --replay <file.rcx>` does that comparison. The format is in §3.
- **Type numbering is compile-time.** `TypeIndex` (PDB enum, `re/types/typeindex.tsv`) fixes every type's index.
  The loader assumes the *n*-th XML element of a section is type `BASE + n` and asserts the element counts.
  A reimplementation must keep XML document order. Saves, replays, commands and scripts all use these indices.
- **Everything is integer.** No rules value the simulation reads is a float (no float parser is used by any
  rules loader). Fractions are parsed into fixed point
  (`String::fraction`: `a*M/b`, C truncating division). Angles are 32-bit binary angles. See §13.
- **Some fields are finalised per game.** `Types::finalize_grafting` @00669840 (called from `Game::init_data`
  @0058dca0) edits `tribe_mask` and caches `age` and `military_level`, and its results depend on game settings
  (starting/ending age, a GameInfo flag) through `TypeData::get_preq` @00668700. The replay image captures the
  values for that game.

## 2. Load order

`Main::init_common_data` → `Game::init_common_data` @0058e750 → `Game::init_rules_data` @0058ec60 then
`Game::init_non_rules_data` @0058fda0. Per game: `Game::run` → `GameMod::load` (mod overlay) →
`Game::init_rules_and_teams` @00589bb0 → `Game::init` / `Game::init_data` (→ `Types::finalize_grafting`) →
`RecordGame::write_rules` or `read_rules`.

1. **`Game::init_rules_data`** (rules.xml):
   1. `Constants::init` @00569a90 — `ROOT/CONSTANTS` (§12).
   2. `Categories::init` for `tech_cats`, `unit_cats`, `build_cats` (`building_cats`), `stances_combat`,
      `stances_worker`, `stances_caster`, `stances_packer`, `agename_cats` (§10).
   3. `Tribes::init` @006ef430 — `ROOT/TRIBES` (§10).
   4. About 30 more `Categories` lists used by the game-setup UI and GameInfo (treaties, game styles, game rules,
      map styles, difficulties, map sizes, …), `Map::init_map_file_names`, the coord lookup array.
2. **`Game::init_non_rules_data`** → **`Types::init`** @00669cc0. Each type's parser is the virtual
   `Type::init(XMLElement*, TypeIndex, pass)` (vtable slot 0xa4). Some files are read in several passes so that
   later passes can resolve names of types created by earlier ones:

   | # | File / element | Types | Passes | Count assert |
   |---|---|---|---|---|
   | 1 | `techrules.xml` `ROOT/TECH` | 544..628 | 0, 1 | 85 = NUM_TECHTYPES |
   | 2 | `alternatenames.xml` `ALTERNATENAMES/EPOCHTYPE/STRING` | tech `alternate_name[8]` of 551..578 | – | 28 epochs × 8 |
   | 3 | `resourcerules.xml` `RESOURCES/RESOURCE` | 0..49 | 0, 1 | 50 = NUM_GOODTYPES |
   | 4 | `buildingrules.xml` `BUILDING` | 414..542 | 0, 1 then post-loop | 129 = NUM_BUILDTYPES |
   | 5 | install-root `masks.txt` | building footprint masks | – | – |
   | 6 | `unitrules.xml` `UNIT` | 50..413 (incl. 12 gaia) | 0..4 | 364 |
   | 7 | – | `UnitType::fill_relative_values` for 50..413 | – | – |
   | 8 | `itemrules.xml` `ITEMS/ITEM` | 543 | 0, 1 | 1 = NUM_ITEMTYPES |
   | 9 | `craftrules.xml` `CRAFT` | 629..683 | 0 | 55 = NUM_CRAFTTYPES |
   | 10 | `rules.xml` `TECHBONUSES/BONUS` | 684..805 | 0 | 122 = NUM_BONUSTYPES |
   | 11 | `techrules.xml` `TECH` again | `TechType::set_research`, then `compute_ai_values` | – | 85 |
   | 12 | – | `ObjectType::finalize_init_all` (is-lists, final flags) | – | – |
   | 13 | sound tables, name hashes, `Balance::init` (§11) | | | |

   A count mismatch raises an error dialog and aborts the load. Within a pass, types are processed in index order.
3. **Per game**: `Types::finalize_grafting` (§7.6). Scenario `types` chunks and scripts can also edit types at run
   time (`Type::backup/restore` keep a copy); those are game state, not rules loading.

Every rules file is opened with `XML::init` @00a279e0, which prefers a language variant `name.xml.<suffix>` when it
exists (English suffix "9"). Only `typenames.xml`, `alternatenames.xml`, `rules_string_lookup.xml` and
`resourcerules_strings.xml` have variants, and they only affect display strings.

## 3. Rules image format (`Game::walk_rules_data` @00589550)

`DataWalk::walk_function(begin, end)` writes or reads the raw bytes of `[begin, end)` (little-endian, x86 layout).
`walk_test(label)` writes one byte: the low byte of the label's case-insensitive string hash (`SaveGame::walk_test`
@0043d840). A `String` is walked as `u32 length` + `length` UTF-16 code units (`String::walk_data` @00a1b2d0). A
`SimpleArray<u16>` is walked as `i32 count`, then if `count != 0`: `i32 capacity`, `u16 block`, `u8 flags & 0xbf`,
`count × u16` (`SimpleArray<unsigned short>::walk_data` @00476610). In checksum mode (`DataWalk.checksum != 0`)
strings are skipped; replays and saves include them.

```
u8   0x92                                   walk_test("Game Rules")
806 × type record, index order              Types::walk_rules_data @00669800 → vtable slot 0xc4
u8[0xd40]  Constants [0x000, 0xd40)          848 × int32 (§12)
u8[4]      Constants [0x804, 0x808)          mongol_three_mil_cavalry, a second time
i16[493][493] Balance final_balance_table    row-major (§11)
24 × { u8 0x8f                              walk_test("Tribe rules")
       u8[0x18] Tribe [0x54, 0x6c)          tribe, barbarian, people, backup_build_continent, build_continent, unit_continent
       i32[352] Tribe::graft }               (text_substitute at 0x6c is not walked)
```

Type records (field lists with names and types: `re/tools/rulesimage.mjs`):

| Class (vtable 0xc4) | Types | Content |
|---|---|---|
| `Type::walk_rules_data` @00663190 | bonuses 684..805 | TypeData `[0x04, 0x5e)` (90 bytes), `display_name` string |
| `ObjectType::walk_rules_data` @0065fba0 | item 543 | Type part, ObjectTypeData `[0x1e4, 0x27c)` (152 bytes), `is_list`, `is_strict_list` |
| `GoodType::walk_rules_data` @0066fab0 | 0..49 | ObjectType part, GoodTypeData `[0x2b4, 0x2f8)` (68 bytes) |
| `UnitType::walk_rules_data` @0061d190 | 50..413 | ObjectType part, UnitTypeData `[0x2b4,0x2cc)` `[0x2d4,0x2dc)` `[0x2dc,0x2e0)` `[0x2e0,0x5d4)` (792 bytes; `fire_proj` 0x2cc and `second_max_range` 0x2d0 are skipped) |
| `BuildType::walk_rules_data` @00631f50 | 414..542 | ObjectType part, BuildTypeData `[0x2b4, 0x2e5)` (49 bytes) |
| `TechType::walk_rules_data` @0066d5c0 | 544..628 | Type part, TechTypeData `[0x1c8, 0x1e3)` (27 bytes), `alternate_name[8]` strings |
| `SpellType::walk_rules_data` @00675400 | 629..683 | Type part, SpellTypeData `[0x1c8, 0x1f8)` (48 bytes) |

The sample replay `re/analysis/sample_replay.bin` holds the image at offset 0x486, 1,024,221 bytes, and
`rulesimage.mjs` re-encodes it byte-identically. `Game::dump_rules_data` / `restore_rules_data` write and read the
same image to `<app dir>\StateBackup\state.dat`. The install-root `rules.dat` (gzip) starts with the same 0x92
marker but has an older type layout (fewer unit types, no strings); it looks like a stale leftover. The string
"rules.dat" is in internal_strings.xml, but no instruction references its table offset.

Fields a reimplementation keeps but that are **not** in the image (so the replay cannot check them): all strings
other than `display_name` / `alternate_name`, `fire_proj`, `second_max_range`, `random_rare_drop`, building
footprint masks, `Tribe::text_substitute`, categories, sound tables. They come from XML in both new games and replays.

## 4. XML reader (basic:xml.obj)

The game uses MSXML (`CoCreateInstance(DOMDocument2)`, `validateOnParse = false`, `resolveExternals = true`,
`preserveWhiteSpace` left false). No rules file has a DOCTYPE, so nothing is defaulted from a DTD. Semantics the
loader relies on (implemented in `rulesxml.mjs`):

- `XMLNode::get_element(name, create, exact)` @00a28090: the name is **upper-cased** unless `exact`, then
  `selectSingleNode(name)` → the first child element with that exact (case-sensitive) tag. So the lower-case names
  in the code (`y_size`, `cost`) match upper-case tags, and a tag written in lower case in the data is never found.
  With `create = 1` a missing child is created empty and appended (this matters: see `COST` in §7).
- `XMLNode::get_elements(name)` @00a27720: `selectNodes` on the upper-cased name, element nodes only, document order.
- `XMLElement::get_text(name)` @00a271b0: MSXML `text` of the first child `name` (concatenated descendant text and
  CDATA, leading/trailing white space trimmed because white space is not preserved); `""` when the child is
  missing. With no name, the element's own text.
- `XMLElement::get_text_num(name, def)` @00a25620: `def` when the text is empty, else `String::number`.
- `XMLElement::get_attrib(name)` / `get_attrib_num(name, def)` @00a27900 / @00a27580: attribute value (not
  trimmed); `get_attrib_num` returns `def` for an empty or missing attribute.
- Name comparison (`String::operator==` @00a1f140): equal length and `_wcsicmp` (ASCII case-insensitive). If both
  strings already carry hashes (flag 2) only the case-insensitive hashes are compared — not the case during loading,
  because XML strings are not hashed. `Types::init` hashes every type's `name` at the end.

## 5. Value parsers (basic:str.obj, arith.obj)

| Parser | Function | Semantics |
|---|---|---|
| number | `String::convert_int` @00a15fc0 (via `String::number` @00a1d210) | `"0b…"` → binary, `"0x…"` → hex, `"0d…"` → decimal after the prefix, else MSVC `_wtoi`: skip white space, optional sign, leading digits; trailing text ignored (`"0-10rng"` → 0, `"25%"` → 25); saturates at INT_MIN/INT_MAX. |
| binary | `String::binary` @00a1b080 | from the start, while `'0'`/`'1'`: `v = 2v + bit` (int32 wrap); most significant digit first. TRIBE_MASK's rightmost digit is tribe 0 (AZTECS). |
| hex | `String::hex` @00a16060 | while hex digit: `v = 16v + digit`. |
| fraction(M) | `String::fraction` @00a1d110 | `a = _wtoi(s)`; `d = _wtoi(text after the first '/')`, or 1 if there is no '/'; `d == 0` → 0; else `(a*M)/d` with int32 multiply and C (truncate toward zero) division. M = 256 gives 8.8 fixed point, 100 gives percent. |
| cost list | `Type::load_cost` @00663ba0, `ObjectType::load_support` @00661160, `load_upgrade` @006615b0 | tokens separated by `/`; each token is `convert_int(token)` followed by a resource letter found by skipping the token's leading digits: `t` timber 1, `g` wealth 2, `k` knowledge 3, `m` metal 4, `o` oil 5, anything else (`f`, space, end) food 0. Zero amounts are skipped. |
| angle | `degrees_to_angle` @00a28eb0 | integer degrees → 32-bit binary angle (2^32 = 360°), computed piecewise: `(d/90)*2^30`, then the 45°, 30°, 15°, 5° and 1° remainders with constants 0x20000000, 0x15555555 (=3×0x71c71c7), 0xaaaaaaa, 3 per 5°, 0xb60b60 per degree. All int32 with wrap. `rulesxml.mjs degreesToAngle` reproduces it. |
| upper-letter mask | inline (`OBJ_MASK`/`OBJ_MASKS`) | upper-cased; `'A'..'Z'` → bits 0..25, `'1'..'6'` → bits 26..31 (`c - 0x17`); other characters ignored. |
| lower-letter flags | inline (unit `FLAGS`) | lower-cased; `'1'..'9'` → bit `c - 0x17`; **every other character** → bit `(c - 'a') & 31` (so a space sets bit 31 and `'0'` sets bit 15). The data contains no such characters, but a port must keep the rule. |

## 6. Types: numbering and the common parts

### 6.1 Numbering (`TypeIndex`)

| Range | Count | Class | Source |
|---|---|---|---|
| 0..49 | 50 | GoodType (0..5 the six common resources FOOD..OIL, 6..49 rares) | resourcerules.xml |
| 50..401 | 352 | UnitType (352..357 government heroes) | unitrules.xml |
| 402..413 | 12 | UnitType, gaia animals (BIRD …) | unitrules.xml (same list) |
| 414..542 | 129 | BuildType (526..542 wonders) | buildingrules.xml |
| 543 | 1 | ItemType (goody box) | itemrules.xml |
| 544..550 | 7 | TechType, ages | techrules.xml |
| 551..578 | 28 | TechType, library epochs: science 551, commerce 558, civic 565, military 572 (7 each) | techrules.xml |
| 579..582 | 4 | TechType, "final" techs | techrules.xml |
| 583..622 | 40 | TechType, plain techs | techrules.xml |
| 623..628 | 6 | TechType, governments | techrules.xml |
| 629..683 | 55 | SpellType (crafts / powers) | craftrules.xml |
| 684..805 | 122 | BonusType (tech bonuses) | rules.xml TECHBONUSES |

Every index has an enum name (`re/types/typeindex.tsv`, e.g. 50 PEASANTS, 123 MACHINEGUN, 414 SMALL_CITY). Special
values: `TYPE_NONE` −1, `TYPE_ANY_BUILDING` / `TYPE_NOT_AVAIL` −2, `TYPE_ANY_UNIT` / `TYPE_NOT_SPECIFIED` −3,
`TYPE_TRAINING_BUILDING` −4, `TYPE_ANY_AIR_UNIT` −5, `TYPE_NON_AIR_UNIT` −6.

**Name keys** (`Types::unit_key` @006694a0, `build_key` @00669330, `tech_key` @00669610, `good_key` @006691c0):
scan the class range in index order comparing the text with each type's **`type_name`** (§4 comparison; the first
match wins), then `"none"` → −1, `"disable"` → −2, otherwise an error report and −3. Ranges: units 50..401 (gaia
excluded), buildings 414..542, techs 544..628, goods 0..49. `Types::object_key` tries units then buildings.
`type_name` is the XML TYPENAME when present, else NAME; for units several entries share a name (the Korean
citizen is also "Citizen"), so the first one is what keys resolve to.

### 6.2 Object layout

Every concrete type is one heap object (`malloc` + placement constructor in `Types::init`; memory is not zeroed).
Offsets are identical in every class: `TypeData` 0x00..0xec, `TypeOut` (UI) 0xf0..0x1c8, then a second vtable at
0x1c8 for object types, `ObjectTypeData` 0x1e4..0x2b4, then the class part from 0x2b4 (`TechTypeData` and
`SpellTypeData` start at 0x1c8 instead). A port does not need the layout, only the values, but the image (§3) is
defined by it.

### 6.3 TypeData (every type)

| Off | Field | Type | Set by | Default (constructor `Type::Type` @00663010) |
|---|---|---|---|---|
| 0x04 | type | TypeIndex | every init pass | 0 |
| 0x08 | job_time | u32 frames (queues advance 100 per frame in 1/100-frame units; production-spec §2) | units JOB_TIME, techs, buildings, spells | 0 |
| 0x0c | res_time | u32 | as job_time (copy) | 0 |
| 0x10 | tribe_mask | bitmask, bit *n* = tribe *n* | TRIBE_MASK (binary) | −1 (all) |
| 0x14 | cat | category index | CAT via the class's Categories list | 0 |
| 0x18 | costs[6] | food, timber, wealth, knowledge, metal, oil | COST (cost list) | 0 |
| 0x30 | preq[3] | TypeIndex | PREQ0..2 (tech_key) | −1 |
| 0x3c | from | TypeIndex | FROM | −1 |
| 0x40 | where | TypeIndex | WHERE (build_key) | −1 |
| 0x44 | upgrade | TypeIndex | derived (§7.4) | −1 |
| 0x48 | jump | TypeIndex | JUMP | −1 |
| 0x4c | obs | TypeIndex | OBS; units force −2 | −2 |
| 0x50 | show[2] | TypeIndex | – | −2, −2 |
| 0x58 | modified | int | – | 0 |
| 0x5c | grid_x, grid_y | i8 | GRID_X / GRID_Y (button position) | 0 |
| 0x60.. | name, display_name (0x74), graph_name (0x88), help_name (0x9c), type_name (0xb0), abbrev (0xc4), desc (0xd8) | String | §10.3 | "" |

`Type::set_name` @00663ae0 sets `name`, sets `type_name = name`, and sets `display_name = lookup_display_name(name)`
(typenames.xml, §10.3).

### 6.4 ObjectTypeData (goods, units, buildings, item)

Defaults from `ObjectType::ObjectType` @0065f880: `hits`, `armor`, `domain`, `x_spacing`, `y_spacing`, `abil`,
`graft`, `special_upgrade`, `support[0..1]`, `age` = −1; everything else 0.

| Off | Field | Units meaning (see §7 for parsing) |
|---|---|---|
| 0x1e4 | obj_masks | OBJ_MASK letters (§5): A armored … Z detect, 1 can't-upgrade-to, 2 missile, 3 air, 4 light cav, 5 pike, 6 anti-air |
| 0x1e8 | attack | ATTACK × 10 |
| 0x1ec | to_hit | % |
| 0x1f0 | attenuate | % per tcoord, absolute value |
| 0x1f4 | recharge | ticks |
| 0x1f8 / 0x1fc | min_range / max_range | tcoords |
| 0x200 / 0x204 | splash_area / splash_percent | tcoords / % |
| 0x208 | ammo_per_att | |
| 0x20c | proj_speed | |
| 0x210 / 0x214 | hits / armor | |
| 0x218 | domain | 0 ground, 1 sea, 2 air (`DomainIndex`) |
| 0x21c / 0x220 | los / science_los | tcoords |
| 0x224 / 0x228 / 0x22c | guy_spacing / x_spacing / y_spacing | world units (× constants) |
| 0x230 | abil | (unused by loaders; −1) |
| 0x234 / 0x238 | x_size / y_size | units: selection circle radius; goods/buildings: footprint |
| 0x23c / 0x240 / 0x244 | guy_radius / block_radius / big_radius | world units |
| 0x248 / 0x24c | new_block_radius / new_big_radius | in `unit_block_radius` steps |
| 0x250 / 0x254 | fly_high / fly_low | % |
| 0x258 | block_points | 8 or 12 |
| 0x25c | graft | TypeIndex of the mainline unit a unique unit replaces |
| 0x260 / 0x264 | special_upgrade / special_upgrade_cost | resource index / amount (UPGRADE) |
| 0x268 / 0x270 | support[2] / support_cost[2] | resource index / amount (SUPPORT, ramping cost) |
| 0x278 | age | cached by `finalize_grafting` (§7.6) |
| 0x27c / 0x298 | is_list / is_strict_list | `SimpleArray<u16>` (§7.5) |

## 7. Units (`UnitType::init` @0061ab50, unitrules.xml)

364 `UNIT` elements → types 50..413. Five passes over the whole list. In passes 2..4, a unit whose `name` equals
the name of the most recent unit that was *not* such a duplicate is initialised from **that earlier unit's element**
(the Korean citizen and scholar get the standard citizen's/scholar's stats; only NAME, GRAPH, TYPENAME and the pass-1
links come from their own element). Before the pass switch, every pass sets `military_level = -1`.
Constructor defaults (`UnitType::UnitType` @0061d270): `cat`, `attack`, `mode`, `moves`, `second_max_range` = 0,
`relative_value[]` zeroed; the remaining UnitTypeData fields are uninitialised until pass 2 writes them.

### 7.1 Pass 0 and pass 1

| Pass | XML | Field | Rule |
|---|---|---|---|
| 0 | NAME | name, type_name, display_name | `Type::set_name` |
| 0 | GRAPH | graph_name | text (key into unit_graphics.xml) |
| 0 | TYPENAME | type_name | text, or `name` when empty |
| 1 | WHERE | where | build_key |
| 1 | FROM | from | unit_key |
| 1 | – | obs | −2 |
| 1 | JUMP | jump | unit_key ("disable" → −2) |
| 1 | TRIBE_MASK | tribe_mask | binary, rightmost digit = tribe 0 |
| 1 | GRAFT | graft | unit_key |

### 7.2 Pass 2 (all stats), in execution order

`num(X)` = `get_text_num(X, -1)` (−1 when the element is missing or empty). `C.` = Constants (§12).

| XML | Field | Rule / units |
|---|---|---|
| GRAPH | help_name | text |
| GRID_X, GRID_Y | grid_x, grid_y | (i8) num |
| FLAGS | unit_flags (0x2b4), unit_flags2 (0x2b8) = 0 | lower-letter flags (§5). a terrain-light, b cart, c/d AI support rarity, e (set by code: 0x10), f helicopter, g attacks sideways, h no research needed, i infantry family, j available without predecessors, k melee **and** ranged, l garrison town/tower, m garrison fort, n dies when attacking, o cloaked, p not built at buildings, q broadside, r siege line, s stealth, t tank, u submarine, v fires while moving, w strafes, x blocks when dead, y unique unit, z rocks when attacking, 1 respawning government hero (bit 26) |
| TO_HIT | to_hit | num, % |
| ATTENUATE | attenuate | abs(num) |
| ATTACK | attack | num × 10; `mode` = 0 |
| CAT | cat | `Categories::find_key(unit_cats)`: Foot 0, Mounted 1, Mech 2, Artillery 3, Command 4, Civilian 5, Sail 6, Naval 7, Air 8; unknown → −1 |
| RANGE | min_range, max_range, second_max_range | `"<min>-<max>rng"`: min = convert_int(text); if there is a `-`, max = convert_int(after it), else max = min. `second_max_range = max`. If flag k: `max_range = 0` (melee range, the ranged range stays in `second_max_range`). Units: tcoords |
| SPLASH, SPLASH_PERCENT, AMMO_PER_ATT, RECHARGE, ARMOR | splash_area, splash_percent, ammo_per_att, recharge, armor | num |
| OBJ_MASK | obj_masks | upper-letter mask (§5) |
| DOMAIN | domain | case-insensitive `"Sea"` → 1, `"Air"` → 2, anything else → 0 |
| MOVES | moves | num (speed, scaled by `C.unit_move_speed` at use) |
| TURN_SPEED | turn_speed | `degrees_to_angle(num)` |
| HITS | hits | num |
| PROJ_SPEED | proj_speed | num; if attack ≠ 0 and max_range ≠ 0 and proj_speed = 0 → 200 (error report) |
| – | fire_proj (not walked) | attack ≠ 0 → 1; attack = 0 → 1 if flag v else 0 |
| CARRY, CARRY_SIZE | carry, carry_size | num |
| POP | control_cost | num (pop-cap units) |
| PROGRESSION | progression | num (0..3, ramp formula) |
| JOB_TIME | job_time, res_time | num, ticks |
| JOB_EXTRA_TIME | job_extra_time | `"a/b…"`: empty → 0; else `_wtoi(a)*100 / _wtoi(b)` (b = 1 without '/', 0 if b = 0) — percent of JOB_TIME added per existing unit |
| RESEARCH_PREMIUM_TIME / _COST | research_premium_time / _cost | fraction × 256 (8.8) |
| MANA | mana | num |
| LOS, SCIENCE_LOS, FLY_HIGH, FLY_LOW | los, science_los, fly_high, fly_low | num |
| GUY_SPACING | guy_spacing | `C.unit_guy_spacing × num` |
| X_SPACING, Y_SPACING | x_spacing, y_spacing | `C.unit_formation_spacing × num` |
| CIRCLE_RADIUS | x_size = y_size | min(num, 10) |
| BLOCK_RADIUS | (br) | num, used below |
| PUSH_CIRCLES | push_circles | clamp(num, 1, 100) |
| PUSH_SIZE | push_size | `C.unit_block_radius × (push_circles == 1 ? br : clamp(num, 1, 100))` |
| TARGET_SIZE | target_size | `num × C.unit_block_radius` |
| PREQ0, PREQ1 | preq[0], preq[1] | tech_key |
| COST | costs[6] | `get_element("COST", create=1)` then `load_cost` (§5). Text of COST, or its `cost=` attribute if the text is empty; costs are zeroed only when the string is non-empty. The created element means load_support/load_upgrade below always run |
| SUPPORT | support[2], support_cost[2] | `load_support`: (needs a COST child) text of SUPPORT or COST's `support=`; first two non-zero tokens → (resource, amount); defaults −1/0 |
| UPGRADE | special_upgrade, special_upgrade_cost | `load_upgrade`: first non-zero token → (resource, amount); default −1/0. Every stock unit has `<UPGRADE/>` |
| – | squad_size | 1 |
| UBER_SIZE, CREW_SIZE | uber_size, crew_size | num |

Then, still in pass 2:
1. **Age-scaled second prerequisite**: if `preq[0] ≥ 0`, `preq[1] == -1`, `preq[0]` is an age (544..550) and
   attack ≠ 0, then `preq[1] = preq[0] + 28` (the military library tech of that age, 572..578).
2. **Roles** (`UnitType::determine_roles` @0061c320) → `role` bitmask (`ROLE_*`: MELEE 1, ANTI_CAV 2, MOUNTED 4,
   CAVALRY 8, SCOUT 0x10, SIEGE 0x20, GENERAL 0x40, SPY 0x80, NONCOMBAT 0x100, PEASANT 0x200, RANGED 0x400,
   FOOT 0x800, AIR 0x1000, NAVAL 0x2000, BOMBER 0x4000, TRANSPORT 0x8000, COMBAT 0x10000, DEFENSE 0x20000,
   GROUND 0x40000, SEA 0x80000, INFANTRY 0x100000):
   - types 50..53 (citizens, scholars) start with PEASANT; domain air → AIR; ground → GROUND, plus SCOUT if
     `is(SCOUT)`, plus MOUNTED|CAVALRY (0xc) if cat = Mounted; sea → SEA, plus SCOUT if `is(BARK)`;
   - carry ≠ 0 → TRANSPORT;
   - if attack ≠ 0 and cat is not Civilian/Command and not PEASANT: COMBAT; sea → also NAVAL (note: this assignment
     replaces the bits set so far with `previous | COMBAT | NAVAL`, which is the same set); air → BOMBER; ground and
     cat Foot → FOOT plus (`is(HOPLITES)` ? ANTI_CAV : INFANTRY); then RANGED if max_range ≠ 0 else MELEE;
     otherwise NONCOMBAT.
3. **Size**: `new_block_radius = br`, `guy_radius = block_radius = C.unit_block_radius × br`,
   `big_radius = ((squad_size-1) × guy_spacing)/2 + guy_radius`, `new_big_radius = big_radius / C.unit_block_radius`,
   `block_points = br > 1 ? 12 : 8`.
4. **Upgrade link** (§7.4).
5. **Graft** (if `graft ≥ 0`): copy grid_x/grid_y from the graft target; if `upgrade < 0` take the target's
   `upgrade`; for every tribe bit in `tribe_mask`: `tribes[tb].graft[graft-50] = this type`.
6. **Production building flags** (if `where ≥ 0` and not flag p): building `where` gets
   `BUILDTYPE_TRAINING` (0x80000000); if also attack ≠ 0, not NONCOMBAT, and the building is not a SMALL_CITY
   (`is(414)`), `BUILDTYPE_MIL_TRAINING` (0x40000000).
7. `base_form = 0`.

### 7.3 Pass 3 and pass 4

- Pass 3 (tribe grafts for unique lines): for each tribe *tb* whose graft table does **not** map this unit to
  itself and whose bit is not in this unit's `tribe_mask`: every other unit *j* (50..401) that also lacks the bit
  and has `graft == this` gets `tribes[tb].graft[j-50] = tribes[tb].graft[this-50]`.
- Pass 4: the upgrade link of §7.4 again (all grafts are now known).

### 7.4 Upgrade link

If `from ≥ 0`: unless this unit has OBJ_MASK `1` (can't upgrade to this), and if this unit is not a unique (graft
< 0) **or** its FROM unit is itself a unique (FROM's graft ≥ 0) **or** this unit has flag h, then
`types[from].upgrade = this`. Units are processed in index order, so the last qualifying unit wins.

### 7.5 Finalisation in `Types::init`

1. `UnitType::fill_relative_values` @0061be50 for 50..401, right after pass 4 (§7.7).
2. `ObjectType::finalize_init_all` @0065f4a0 (after spells, bonuses and techs are loaded):
   - units 50..401 (not gaia): rebuild `is_list` / `is_strict_list`, then `init_final_flags`, then
     `init_spellcasters`;
   - buildings 414..542: rebuild the lists; if the building `is` FARM (417), OILWELL (421) or OILPLATFORM (422):
     `build_flags |= 0x10000000` (`BUILDTYPE_FLAT_GATHER`).
   Goods, gaia and the item keep empty lists.

**is-lists** (`ObjectType::init_is_list` @00661dd0): `is_list` = every TypeIndex *t* (ascending, 0..805) with
`is_slow(t, 0)`; `is_strict_list` = every *t* with `is_slow(t, 1)`. `ObjectTypeData::is_slow` @00661ae0:
- non-strict: *t* is this type, or this type's `graft`, or (recursively) anything its `from` type is — i.e. the type
  itself, its upgrade-line ancestors, and their graft targets;
- strict: *t* is this type, or (units only) *t* is the graft target and the target lacks flag y.
The arrays are `SimpleArray<u16>` grown one element at a time: `capacity = count`, `block = 1`, `flags = 0`.
`ObjectTypeData::is(t, strict)` @0065f7d0 (vtable 0x60) answers from these lists once they exist; `TypeData::is` for
non-object types is plain equality.

**Final flags** (`UnitType::init_final_flags` @0061dc70), `unit_flags2` bits:
4 if is CATAPULT, FLAMINGARROW, MACHINEGUN, FISHERMEN or KATYUSHA, or the type is MERCHANT (61), MERCHANTDUTCH (62),
FURTRAPPER (400); 0x40 if is SUPPLYWAGON, THEDESPOT, THEMONARCH or THECITIZEN; 0x20 if is GENERAL; 8 if is CARA or
MERCHANTFLEET; 0x10 if is SCOUT; 1 if is MACHINEGUN or FLAMETHROWER; and `unit_flags |= 0x10` (the "e" flag) if is
TRANSPORTBARGE or MERCHANTFLEET.

**Spell casters** (`UnitType::init_spellcasters` @0061aae0): walk from this unit along `from` (checking each unit
and its graft target); if any has `unit_flags2 & 2`, set bit 2 here. Bit 2 itself comes from the craft loader
(§9.6: a craft's FROM/FROM2 unit gets `unit_flags2 |= 2`).

### 7.6 Per-game finalisation (`Types::finalize_grafting` @00669840)

Run by `Game::init_data` for every game (so it is in the replay image, but not in a plain XML load):
1. `barb` = mask of tribes with `barbarian ≠ 0`.
2. For each unit *j* in 50..401 with flag y and `graft ≥ 0`, `graft ≠ j`: unless GameInfo flag 8 is set,
   `types[graft].tribe_mask = (tribe_mask & ~types[j].tribe_mask) | barb`; with flag 8,
   `types[graft].tribe_mask |= types[j].tribe_mask`.
3. Units 50..401: `age = get_age_slow()`, `military_level = get_military_level_slow()`; buildings 414..542:
   `age = get_age_slow()`.

`ObjectTypeData::get_age_slow` @00661a00: for each prerequisite slot (units have 2, `UnitTypeData::num_preq`), the
first `get_preq(i)` that is a tech gives the age: the tech's `age`, plus one if it is an age tech; none → 0.
`UnitTypeData::get_military_level_slow` @0061d4d0: p = `preq[0]`; if that is not a Military-category library epoch
(551..578, tech `cat` 0), p = `get_preq(1)`; the level is `p − 571` (1..7) when p is such a tech, else 0.
`TypeData::get_preq` @00668700 depends on the game setup: preq[1] is re-scaled when the game does not span the
default ages (starting technology > 0 or ending technology < 7; a colour-column tech is mapped to the
corresponding tier of the shortened span) and dropped for the Iroquois early-government bonus; preq[2] is dropped
when it is in the same library column as preq[1] and not more advanced, or when it is an age beyond the game's
ending age for University-produced types. With the default setup it returns preq[i] unchanged. The full rules
are in §9.3 ("Effective preqs") and §10.1; `rules/tribes.mjs` implements them.

### 7.7 Relative combat values (`UnitType::fill_relative_values` @0061be50)

For a unit with `role & COMBAT`: for every unit *j* in 50..401, `relative_value[j-50]` (i16) = 0 if *j* is not
COMBAT; otherwise an 8.8 multiplier *v* (start 0x100) adjusted by the attacker mask `A` (this unit's obj_masks)
and defender mask `D` (*j*'s), then stored as `min(v, 0x300) − 0x100` (the low 16 bits of *v* when below 0x300).
All arithmetic is signed int32 with C truncating division; `×1.5` = `v*3/2`, `/2` = `v/2`. In order:

| # | Condition | Effect |
|---|---|---|
| 1 | A∋Y(heavy cav, bit 24), max_range = 0, D ∩ 0x90408 | v = 0x180 |
| 2 | A∋4(light cav, bit 29), max_range = 0, D ∩ 0x10000 or D ∩ 0x84000 | ×1.5 |
| 3 | A∋H(bit 7), D ∩ 0x81800 | ×1.5 |
| 4 | A∋5(pike, bit 30), D∋M | ×1.5 |
| 5 | this is MACHINEGUN, D ∩ 0x10108 | ×1.5 |
| 6 | A ∩ 0x108, *j* is MACHINEGUN | /2 |
| 7 | both naval (bit 13) | ×1.5; then ×1.5 if this is TRIREME and *j* is BOMBARDSHIP; ×1.5 if this is FIRERAFT and *j* is not; /2 if this is not FIRERAFT and *j* is; /2 if this is FIRERAFT |
| 8 | A∋E: ×1.5 if D bit 19; then (also when this is CAMEL) ×1.5 if `D & 0x1010 == 0x1000` and *j* is not CAMEL | |
| 9 | A∋W(bit 22), D∋A(bit 0), A ∩ 0x20040 = 0 | v/8 |
| 10 | `A & 0x430000 == 0x20000`, D∋A | /2 |
| 11 | A∋K(bit 10): ×1.5 if D bit 7; ×1.5 if D bit 14; /2 if A lacks W and D bit 16 | |
| 12 | A∋O(bit 14): v×5/4 if D bit 7; v×2/3 if A lacks W and D bit 16 | |
| 13 | A∋G(bit 6): ×2 if D bit 7; v×5/4 if A∋F and D∋M | |
| 14 | A∋U(bit 20), D∋A | ×1.5 |
| 15 | air rules: if A has bit 28 (air), is not a missile (bit 27) and has bit 31 (anti-air): ×3 if D is air, else go to 16 without the next step. Then if D is air and not a missile: v/2, and if A has bit 31: ×3, and if also A bit 21: ×2; skip 16 | |
| 16 | this is AAGUN | v/3 |

(`is` here is the is-list predicate of §7.5; the lists are not built yet at this point, so the `is_slow` form is
used, which gives the same answers.) Gaia units (402..413) are never computed and non-combat units keep an all-zero table (constructor memset).

## 8. Buildings (`BuildType::init` @00632340, buildingrules.xml)

Source: `Data/buildingrules.xml`, root `<BUILDINGS>`, 129 `<BUILDING>` elements (element *i* is type 414+*i*;
wonders are 526..542). Implementation: `re/tools/rules/buildings.mjs`.

### 8.1 Where it runs (Types::init @00669cc0)

1. Load `buildingrules.xml`, `get_elements("BUILDING")`; the count must be 0x81 (129), otherwise the error
   "Number of buildtypes in buildingrules.xml doesn't equal NUM_BUILDTYPES!".
2. For pass 0, then pass 1: for t = 414..542, allocate BuildType (first pass only) and call the virtual
   `Type::init(el, t, pass)` (slot 0xa4) = **BuildType::init @00632340**. Buildings get only passes 0 and 1.
   No other caller re-inits buildings (Type::init @00470400 is just the vtable thunk).
3. **Post-loop** over 414..542 (inlined BuildType::set_domain + upgrade-chain flag), see section 4.
4. **Masks**: graphic key lookup + `BuildType::init_masks`, then masks.txt reads (section 6).
5. Then unitrules (which writes build flags into buildings, section 5), items, crafts, bonuses, techs again.

Prerequisites: techs (544..628) and goods (0..49) are loaded (pass 0 at least) so `tech_key`/`good_key` work.
Units do not exist yet; building keys resolve only against buildings (414..542).

### 8.2 Pass 0 (names only)

| Step | Field (offset) | Rule |
|---|---|---|
| close | abbrev 0xc4, desc 0xd8 | BuildType::close @00632060 empties both strings |
| type | type 0x04 | = t (written on every pass) |
| NAME | name 0x60, type_name 0xb0, display_name 0x74 | Type::set_name @00663ae0: name = text; type_name = name; display_name = virtual lookup (slot 0xb0) of name (typenames) |
| GRAPH | graph_name 0x88 **and** help_name 0x9c | get_text (both from GRAPH) |
| TYPENAME | type_name 0xb0 | text, or name when empty |
| - | upgrade 0x44 = -1, to 0x2e0 = -1 | reset so pass 1 can fill them from children |

### 8.3 Pass 1 (everything else), in source order

"num" = `XMLElement::get_text_num(tag, -1)`: -1 if the text is empty, else `String::convert_int` (0b/0x/0d
prefixes, else `_wtoi`). All 51 tags are present in the shipped file, so defaults only matter for mods.

| XML | Field (offset) | Parse / default | Notes |
|---|---|---|---|
| - | attack 0x1e8 = 0, armor 0x214 = 2 | placeholders | both overwritten below |
| CAT | cat 0x14 | `Categories::find_key(build_cats, text)`: case-insensitive match on CATEGORY `key` in rules.xml `<CATEGORIES id="building_cats">` (E=0, M=1, W=2); -1 + error if unknown | menu only |
| GRID_X / GRID_Y | grid_x 0x5c / grid_y 0x5d | num, truncated to signed char | build-menu button |
| TEX_ID | tex_id 0xf0 (TypeOut, not walked) | texture-page index by name in the render manager's list, 0 if empty/missing; small_tex_id 0x108 = same lookup of TEX_ID+"_HALF" | UI |
| TEX_COL/ROW/X/Y/CLIP | tex_col 0x120, tex_row 0x138 (char), tex_x 0x150, tex_y 0x180 (short), tex_clip 0x1b0 (char) | num, truncated; slot [0][0] | UI, not walked |
| HITS | hits 0x210 | num | |
| TOWN_HITS | town_hits 0x2b4 | num | legacy |
| - | min_city_size 0x2b8 = 0 | | |
| MISERY | misery_rate 0x2bc | num | legacy per designer; wonders have negative values |
| TO_HIT | to_hit 0x1ec | num | |
| ATTENUATE | attenuate 0x1f0 | num | |
| ATTACK | attack 0x1e8 | num x 10 (int32 multiply; missing -> -10) | |
| RANGE | min_range 0x1f8, max_range 0x1fc | min = convert_int(whole text); max = convert_int(text after the first '-'), or min if no '-' | "0-10rng" -> 0, 10 (trailing letters ignored by _wtoi) |
| SPLASH_AREA, SPLASH_PERCENT, AMMO_PER_ATT, PROJ_SPEED | 0x200, 0x204, 0x208, 0x20c | num | |
| MOST_SHOTS, GARRISON_MAX, BASE_ARROWS, WONDER_VAL | 0x2c4, 0x2c8, 0x2cc, 0x2d0 | num | |
| RECHARGE | recharge 0x1f4 | num | |
| ARMOR | armor 0x214 | num | |
| FLY_HIGH, FLY_LOW | 0x250, 0x254 | num | |
| OBJ_MASKS | obj_masks 0x1e4 | start 0; upper-case; for **every** character c: bit = (c in '1'..'9' ? c-0x17 : c-'A') mod 32 | not the `rulesxml.objMask` rule: non-letters are not skipped (space -> bit 31, ',' -> bit 11). Data uses only R,G,X,B,J,Z,6,1 |
| X_SIZE, Y_SIZE | x_size 0x234, y_size 0x238 | num | footprint in tiles; must equal the mask dims |
| LOS, SCIENCE_LOS | 0x21c, 0x220 | num | |
| BUILD_FLAGS | build_flags 0x2c0 | start 0; lower-case; every char: bit = (c in '1'..'9' ? c-0x17 : c-'a') mod 32 (= `rulesxml.unitFlags`) | section 4 |
| JOB_TIME | job_time 0x08 **and** res_time 0x0c | num, stored as u32 | frames |
| PREQ0..2 | preq[0..2] 0x30 | `Types::tech_key` | "none" -> -1, "disable" -> -2, unknown -> -3 (+error) |
| FROM | from 0x3c | `Types::build_key` | |
| (FROM link) | parent.to 0x2e0, parent.upgrade 0x44 | if this building's obj_masks lacks 0x4000000 (OBJ_MASKS '1' = "can't upgrade to this type") and from >= 0: types[from].to = t; and if build_flags & 4 ('c', global upgrade): types[from].upgrade = t | last child in index order wins |
| SUPPORT0/1 | support[0..1] 0x268 | `Types::good_key` | |
| SUPPORTVALUE0/1 | support_cost[0..1] 0x270 | empty -> -1 else convert_int | |
| COST | costs[6] 0x18 | `Type::load_cost @00663ba0` on child COST (created if missing): text, else attribute `cost`; if still empty costs untouched (ctor 0); else zero all six, split on '/', each token: n = convert_int; if n != 0, skip digits, letter t=1 g=2 k=3 m=4 o=5 else 0 (food); costs[res] = n (overwrite) | stored raw ("5t" -> 5); the designer's "x10" is applied elsewhere |
| PLUNDER | plunder_value 0x2d4 | empty -> -1 else convert_int | |
| PLUNDER_GOOD | plunder_good 0x2d8 | good_key | |
| BEHIND_HEIGHT | behind_height 0x2dc | empty -> -1 else convert_int | graphics occlusion |
| TRIBE_MASK | tribe_mask 0x10 | `String::binary` from the start, no prefix (24 chars, first char = bit 23); empty -> 0 | |
| - | where 0x40 | from < 0 -> -1; else (basic_type(this) == this ? -1 : from); basic_type @00639970 follows FROM to the root, so in practice where = from | |
| OBSOLETE | obs 0x4c | tech_key | "disable" -> -2 everywhere |
| JUMP | jump 0x48 | build_key; if >= 0 then upgrade = jump | all "none" |

Fields BuildType::init never writes keep the constructor values (Type/ObjectType/BuildType ctors, see
`newType` in re/tools/rules/types.mjs): show[2] = -2, modified 0, guy_spacing 0, x/y_spacing -1, abil -1, all radius fields 0,
block_points 0, graft -1, special_upgrade -1, special_upgrade_cost 0, age -1 (until section 5), domain -1
(until the post-loop), civ_graph_mask 0. **Strings mask_name 0x2e8 / doober_name 0x318 are set in section 6.**

BuildTypeOut::set_parse_data @00633650 is UI only: it fills the help-text `parse_numbers`/`parse_strings` for
city, farm, granary/lumbermill/smelter, refinery, market, temple and each wonder from Constants and the
viewing player's state. No rules data.

### 8.4 BUILD_FLAGS bits and the Types::init post-loop

Letters (designer comments in the file; bit = letter - 'a'):
a 0 land, b 1 sea, c 2 global upgrade, d 3 unused, e 4 outside city radius, f 5 non-friendly territory,
g 6 resource gatherer, h 7 on roads (legacy), i 8 addon placed freely (legacy), j 9 max one per city,
k 10 on wall (legacy), l 11 large city only (legacy), m 12 nubbed city building (legacy), n 13 not captured
with city. Digits '1'..'9' would be bits 26..34 mod 32 (none used).

Post-loop (Types::init, after pass 1, t = 414..542 ascending, one loop):
- domain 0x218 = (flags & 2) ? (flags & 1) + 1 : 0, i.e. 0 land, 1 sea only, 2 land+sea
  (= BuildType::set_domain @00633390, inlined).
- if from >= 0: types[from].build_flags |= 0x4000000 and this.build_flags |= 0x4000000 ("in an upgrade chain";
  set regardless of OBJ_MASKS '1').

Derived high bits written later by other loaders (needed to match the oracle):

| Bit | Writer (address) | Rule |
|---|---|---|
| 0x80000000 | UnitType::init pass 2 @0061ab50 (end) | for every UNIT element 50..413 (gaia 402..413 included: they are trained "at" Large City): if where >= 0 and !(unit_flags & 0x8000): buildtypes[where] gets it |
| 0x40000000 | same | additionally unit attack 0x1e8 != 0 and !(role 0x2c8 & 0x100) and !buildtypes[where].is(VILLAGE 414) |
| 0x20000000 | SpellType::init @00674a80 (craftrules) | building that is a spell/craft's FROM (0x3c) or FROM2 (0x1dc), when in 414..542 |
| 0x08000000 | TechType::set_research @0066cba0 (second techrules pass) | for tech WHERE w: every building b with b.is(w) |
| 0x10000000 | BuildType::init_final_flags @00632070, inlined in ObjectType::finalize_init_all @0065f4a0 and finalize_init @0065f640 | is(FARM 417) or is(OILWELL 421) or is(OILPLATFORM 422) |

`is(x)` for a building = ObjectTypeData::is @0065f7d0 (scans is_list when built, else falls back to
ObjectTypeData::is_slow @00661ae0): type == x, or graft == x, or types[from].is(x) recursively. Before
finalize_init_all the lists are empty, so the fallback gives the same answer.

### 8.5 Finalize-time fields (owned by later steps, specified here for buildings)

- **is_list / is_strict_list** (ObjectType::init_is_list @00661dd0, from finalize_init_all): is_list = all x in
  0..805 ascending with is_slow(x, 0) = this type plus its FROM chain (graft is -1). is_strict_list = [type]
  (is_slow(x, 1) is false for non-units). SimpleArray block 1, so capacity == count, flags 0.
- **age 0x278** (Types::finalize_grafting @00669840, called from Game::init_data after Types::init_civ_masks):
  ObjectTypeData::get_age_slow @00661a00 over the 3 preqs via TypeData::get_preq @00668700: the first preq that
  is a tech gives techtypes[p].age (+1 if p is an age tech 544..550); none -> 0. get_preq(1/2) depends on the
  game setup (start/end age for Library "color techs", leader bonus 0x12); `buildAgeApprox` ignores that and
  matches the oracle game.
- **civ_graph_mask 0x2e4**: Types::init_civ_masks @00669a60 (Game::init_data) sets 0 for every building; the
  rest of that function is unreachable. BuildType::init_civ_masks @006320c0 has no caller. Always 0.

### 8.6 Masks (not in the rules image, but simulation inputs)

#### Mask names
For each building (Types::init, after the post-loop): key = upper(graph_name + "-Default-" + "Age" + "0").
GraphicPieces::build_node_from_gpiece(String) @00905050 builds (once) a list of `building_graphics.xml`
ROOT/BUILDINGS/BUILD elements and their `name` attributes, then returns the first whose name equals the key
(case-insensitive). Not found -> error "Failed to locate basic building graphic key: <GRAPH>" and Types::init
fails. BuildType::init_masks @00632280: mask_name = attribute `mask`, doober_name = attribute `doobermask`
(default "").

#### masks.txt
`masks.txt` in the install root (Text::open @00a431a0 appends ".txt" and prepends the content directory).
Read with the C runtime in text mode (CRLF -> LF). Per building, after the name loop, the file is reopened
(rescan from the top) for each lookup:
- **Text::open(key)**: find the first line equal, case-insensitively and after trimming trailing spaces, to
  "#" + key (no leading-space trim). Not found -> nothing loaded (no error). Empty key -> success at file top.
- **Text::get(0)** @00a43530: next line, skipping blank lines and lines whose first non-space char is ';'.
  A line starting with '#' or EOF returns "" (and leaves the item cursor where it was). Leading and trailing
  spaces are stripped.
- **Text::item** @00a43850: field up to the next ',' trimmed of spaces; an empty field keeps the previous
  value. `item_number` = convert_int(item).

**BuildType::init_build_mask @006310b0**: header line "W, H"; error (non-fatal) if H != y_size or W != x_size;
then H lines of W integers; mask[row * W + col] = (u8) value. The Array<u8> at 0x2fc is sized W*H. Section
lines after the first H rows (e.g. the second 4x4 block in "4x4 gather E/N/S/W/NW") are ignored.

**BuildType::init_doober_mask @00630cd0**: same layout into Array<int> 0x32c, words instead of numbers:
"0" -> -1; up/down -> BuildNSCorner dir 1/2; left/right -> BuildEWCorner dir 1/2 (flag 1); random ->
BuildDeathFiller (dir 0, flag 0); value = dir << 16 | flag << 8 | index into the global `doober_names` list;
-1 if not found. Decoration placement (doobers); not used by the rules image.

Shipped data: all 129 buildings resolve a mask and a doober mask, and every mask's dimensions equal
X_SIZE x Y_SIZE (no size errors). Mask names used: 2x2 solid (51), 4x4 solid (24), 3x3 solid (15),
7x7/8x8 extra space (8 each), 5x5 extra space (6), 6x6 extra space (5), 5x5 solid (4), 4x4 extra space (2),
and one each of 4x4 gather S (farm), 2x2 gather, 4x4 oil, 4x7 extra space, 5x9 solid, 3x3 extra space.

#### Semantics (BuildType::mask_me @006312a0)
Called when a building is placed or removed (add flag param_4). The footprint origin is the building's
centre tile minus (x_size >> 1, y_size >> 1) (odd sizes centre on the tile middle). For every cell
(x in 0..x_size-1 outer, y in 0..y_size-1 inner) inside the world: clear tile bits 0x40/0x80; on add set
the low two tile bits (|= 3) and, for a non-city that is a gatherer ('g') or has 'e' and is not
is(UNIVERSITY 420), remove the road there (set_road_at(..., 0)); on remove clear the low bits if both set.
Then **cell value == 1 -> World::set_blocked_at(cell, add)**; any other value -> on add, explicitly
unblocked, and a road is laid if the building is a city or connects_to_roads. Mask index = x_size * y + x
(the stored row-major mask with W = x_size). Docks (is DOCK 432) also mark nearby shore tiles as bad path
(and remask docks on removal); cities (is VILLAGE) call Wall::mask_city; roads are regenerated (place_roads).
The tile of the centre also gets bit 0x4000 set/cleared in the world's coarse grid. **So the build mask is a simulation input (blocking/pathing)** and must be loaded
exactly; the doober mask is visual.

### 8.7 Determinism notes

- Process passes strictly in index order (pass 0 for all, then pass 1 for all): `to`/`upgrade` of a parent
  are written by its children's pass 1 (last child wins), and JUMP overrides `upgrade` only if processed
  later than the child that set it.
- All arithmetic is int32 (attack x10 wraps, binary() wraps); flags are u32 bit sets with shift mod 32.
- String compares for keys are case-insensitive (`String::operator==`), XML tag lookups are case-sensitive
  upper case (rulesxml).
- The 0x4000000/0x80000000/0x40000000/0x20000000/0x08000000/0x10000000 bits must be applied by their owning
  steps in Types::init order (units -> crafts -> techs set_research -> finalize_init_all).

### 8.8 Open questions

- doober_names (GraphicPieces global) source/order not reversed; only matters for the visual doober mask.
- Text::get's "" return at '#'/EOF reads `"time_limit"+0xb` / `"score_goal"+0xb` (adjacent literal); assumed "".
- String::find(wchar) start/direction arguments are garbled in the decompile; assumed first '-' (RANGE never
  has a leading '-').
- `String::number` on a NULL string (absent RANGE) assumed 0.

## 9. Techs, goods, item, crafts, bonuses

Implementation: `re/tools/rules/techs.mjs`.

TypeIndex ranges: goods 0..49 (basic goods 0..5 = Food, Timber, Wealth, Knowledge, Metal, Oil), item 543,
techs 544..628 (ages 544..550, epoch techs 551..578 in four lines of 7: science 551, commerce 558, civic 565,
military 572; finals 579..582; plain techs 583..622; governments 623..628), spells 629..683 (668 DECLARE_WAR,
669..675 DECLARE_WAR1..7, 676 CANCEL_ALLIANCE, 677..683 CANCEL_ALLIANCE1..7), bonuses 684..805.

### 9.1 Load order (Types::init @00669cc0)

1. `techrules.xml` → every `TECH` (must be exactly 85; document order → 544+i). Pass 0 over all, then pass 1
   over all (TechType::init, vtable slot 0xa4). Allocation happens on pass 0.
2. `alternatenames.xml` (language variant preferred) → `ALTERNATENAMES/EPOCHTYPE` (exactly 28) → techs 551..578.
3. `resourcerules.xml` → `RESOURCES/RESOURCE` (exactly 50) → goods 0..49, passes 0 and 1 (GoodType::init).
   `String::set_string_lookup` brackets the loop (resourcerules_strings.xml, UI strings only).
4. buildings (passes 0,1 + post loop), masks, units (passes 0..4), `UnitType::fill_relative_values` (other parts).
5. `itemrules.xml` → `ITEMS/ITEM` (exactly 1) → 543, passes 0 and 1 (ItemType::init).
6. `craftrules.xml` → `CRAFT` (exactly 55) → 629..683, single pass (SpellType::init). After each spell `t > 668`
   is parsed: `costs[0..5]` of t = `costs[0..5]` of 668 (DECLARE_WAR). Implemented as `postSpells` after the
   loop (same result: 668 is parsed before 669).
7. `rules.xml` → `TECHBONUSES/BONUS` (exactly 122) → 684..805, single pass (BonusType::init), inside a
   set_string_lookup bracket (rules_string_lookup.xml, UI only).
8. `techrules.xml` → `TECH` again (exactly 85): `TechType::set_research` for 544..628 in order, then
   `TechType::compute_ai_values` for 544..628 in order.
9. `ObjectType::finalize_init_all` (other parts), sound tables, name hashes, `Balance::init`.

A count mismatch is reported through `Error::report` and aborts the load. Types::init itself runs from
`Game::init_non_rules_data` (program start) and `GameMod::initExistingData` (mod load/unload), not per game.

### 9.2 Common parse rules

* `getText(el, NAME)`: text of first child (upper-cased tag), trimmed; `''` if absent.
  `getTextNum(el, NAME, -1)`: `-1` if empty, else `String::convert_int` (0b/0x/0d prefixes, else `_wtoi`).
* key lookups (Types::tech_key @00669610, build_key @00669330, unit_key @006694a0, good_key @006691c0):
  case-insensitive match on `type_name` over the class range (techs 544..628, buildings 414..542, units
  50..401 i.e. gaia excluded, goods 0..49), then `none` → -1 (TYPE_NONE), `disable` → -2 (TYPE_ANY_BUILDING),
  anything else (incl. empty) → -3 (TYPE_ANY_UNIT) with an error popup (unit_key/build_key silent when quiet).
* Types::object_key @00668ae0: unit_key(quiet); if -3 then build_key (error if still -3).
* Type::load_cost @00663ba0 (dst = `costs` unless given): text of the element; if empty, its `cost` attribute;
  if both empty, **dst is left unchanged**. Otherwise dst[0..5] = 0, then for each `/`-separated token whose
  `convert_int` is non-zero: skip the decimal digits, the next char picks the slot (t 1 timber, g 2 wealth,
  k 3 knowledge, m 4 metal, o 5 oil, anything else incl. f/w/end 0 food), dst[slot] = amount (last wins).
  The element is fetched with create=1, so a missing COST behaves like an empty one. (`X.costTokens` is correct.)
* TRIBE_MASK: `String::binary` (MSB first, '0'/'1' only, int32 wrap). SpellType::init inlines the same loop.
* char fields (grid_x, grid_y, grid_x2, grid_y2): low byte of the int (sign-extended int8 in the walk);
  job_time/res_time are `ulong` (so -1 reads as 0xffffffff).
* Type::set_name @00663ae0: name = s; type_name = s; display_name = lookup_display_name(s) (typenames.xml by
  hash; UI). String::lookup / set_string_lookup only change UI strings (desc, abbrev).

### 9.3 TechType (TechType::init @0066c7b0, 544..628)

Walked: TypeData [0x04,0x5e) + display_name + TechTypeData [0x1c8,0x1e3) + alternate_name[8].

| XML (TECH) | field (offset) | rule | default if missing |
|---|---|---|---|
| — | type (+0x04) | TypeIndex, set on both passes | |
| NAME | name, type_name, display_name (+0x60/+0xb0/+0x74) | pass 0, set_name; type_name = name again | '' |
| GRID_X, GRID_Y | grid_x, grid_y (+0x5c,+0x5d) | int8 | -1 |
| TEX_ID/COL/ROW/X/Y/CLIP | TypeOut tex_* (+0xf0..) | icon placement, UI only | 0 / -1 |
| PREQ0..2 | preq[0..2] (+0x30..) | tech_key | -3 |
| AGE | age (+0x1c8) | int | -1 |
| JOB_TIME | job_time AND res_time (+0x08,+0x0c) | int stored as ulong | 0xffffffff |
| COST | costs[6] (+0x18) | load_cost | unchanged (0) |
| TRIBE_MASK | tribe_mask (+0x10) | binary | 0 |
| SHOW0, SHOW1 | show[0..1] (+0x50) | tech_key | -3 |
| — | where (+0x40) | set to -1 in pass 1, then set_research | |
| — | ai[11] (+0x1cc), leader_off (+0x1e2) | zeroed in pass 1 | |
| WHERE | where | **set_research** (build_key) | -3 |
| — | cat (+0x14) | **set_research** (epoch line) | |

Constructor (TechType @004773a0 → Type::Type @00663010): costs 0, preq -1, from/where/upgrade/jump -1, obs -2,
show -2, tribe_mask -1, modified 0, cat 0; age/ai/leader_off uninitialised until pass 1. from, upgrade, jump,
obs, modified stay at constructor values (oracle: -1,-1,-1,-2,0).

`leader_off` is a per-age "disabled" bit set (TechType::set_disabled @0066c310: bit n for n < 8); nothing in
the load sets it, it is 0 in the oracle.

#### alternate_name (Types::init)
`ALTERNATENAMES/EPOCHTYPE[i]` (i = 0..27, document order; `name` attribute read but unused) → tech 551+i;
it must have exactly 8 `STRING` children → `alternate_name[0..7]` (one per age, Classical..Information plus the
8th). Other techs keep 8 empty strings. Walked (not in checksum mode), English in the oracle (`.9` file).

#### set_research (TechType::set_research @0066cba0)
For each TECH element i (tech 544+i), in order:
1. `where = build_key(WHERE)` (a `PREQ` child is fetched with create=1 and ignored).
2. For every building b in 414..542: if `b.is(where, 0)` then `b.build_flags |= 0x08000000` (building researches
   techs). `is(p,0)` with the is_list still empty is `ObjectTypeData::is_slow` @00661ae0:
   `type == p || (p >= 0 && (graft == p || (from >= 0 && types[from].is(p,0))))`. (Equals the oracle is_list.)
3. `cat`: epoch techs 551..578 by line `q = (t-551)/7`: science q0 → 3, commerce q1 → 2, civic q2 → 1,
   military q3 → 0. Every other tech → 3.

#### Effective preqs (TypeData::get_preq @00668700, Type::find_preq @00668380)
`num_preq` (vtable slot 1): techs/buildings 3, units/goods/bonuses 2, spells (TypeData) 1.
Slot i of type o (leader = -1 during loading):
* i = 0: `preq[0]`.
* i = 2: p = preq[2]; if p ≥ 0: (a) p epoch and q = slot1 ≥ 0 epoch with `cat[p] == cat[q]` and p ≤ q → -1;
  (b) p an age and `o.where == UNIVERSITY (420)` and `ending_technology < age[p] + 1` → -1. Else p.
* i = 1: p = preq[1]. Let start = starting_technology (min with starting_technology2 when game_rules == 8),
  end = ending_technology. Only if p ≥ 0 and (start ≠ 0 or end < 7):
  - p epoch, o a unit (50..401), cat[p] == 0 (military line): if age[p] < start → -1; else
    `v = trunc(28/(end-start+1))*(age[p]-start+1) + 3`, result `trunc(v/4) + 571` clamped to [572, 578].
  - else if start ≠ 0 and p epoch: if start ≤ age[p] and 7-start > 0: base by cat[p] (1 → 565, 2 → 558,
    3 → 551, else 572), `q = trunc((age[p]-start+1)*7/(7-start)) - 1 + base`, q = min(q, preq[1]); result q
    unless preq[2] is an epoch of the same cat with preq[2] > q, then -1. Otherwise -1.
  - (get_preq with a leader ≥ 0 also drops a government-age preq for leaders with tribe bonus 0x12 when
    `iroquois_govs_early`; not reachable while loading.)
`find_preq(x)` = some slot in 0..num_preq-1 equals x.

**Determinism note:** these slots read `GameAccessConst::gamec->info` *at load time*. The oracle matches
start = 0 with end ≥ 6 (raw preqs). With starting_technology 1 or 2 the AI values of 6-7 techs change, so a
reimplementation must load rules with start = 0 / end ≥ 7 (what the game had in this capture), or reproduce
whatever GameInfo the original has at startup (open question below).

#### AI values (TechType::compute_ai_values @0066cdc0, TechType::add_preq_ai @0066c350)
`ai` is int16[11] (wrapping adds). T = this tech. All additions; order-independent.
1. Self: epoch tech by cat: 3 → ai4+1, ai10+1; 1 → ai5+1; 2 → ai1+1; 0 → ai0+1. Non-epoch: where == TEMPLE
   (437) → ai5+2; where == FORTX (443) → ai5+1 and ai0+1.
2. Units u in 50..401 with `u.find_preq(T)`: role&0x10000 → ai0+1; role&0x200 → ai1+1, ai9+1;
   domain == 0: role&0x10000 → ai8+1; domain ≠ 0: carry ≠ 0 → ai3+1, then domain == 1 → ai2+1 else ai6+1.
3. Buildings b in 414..542 with `b.find_preq(T)`; s = (b.from < 0) ? 2 : 1:
   * b.is(VILLAGE 414): ai1+1; if b == TOWN (415): ai5+1, ai1+1, add_preq_ai(1, +1, depth 0).
   * if b.from < 0: build_flags&0x40 → ai1+2s, ai9+2s; build_flags&0x08000000 → ai1+2s, ai4+2s and, for
     b in 423..426 (GRANARY, LUMBERMILL, SMELTER, REFINERY), ai9+2s; b.is(DOCK 432) → ai2+1,
     add_preq_ai(2, +1, depth -1 = whole tree).
   * b.is(DOCK 432) → ai2+1, add_preq_ai(2, +1, depth 2) (so base docks count twice).
   * F = fundamental type of b (follow `from` to the root). F.build_flags&0x40 → ai1+s, ai9+s; else
     F.build_flags&0x08000000 → ai1+s, ai4+s; else F.is(439) or F.is(443) or F.is(447) or F.attack ≠ 0 →
     ai0+s; else F.is(any of 423..426) → ai1+8, ai9+8.
   * if F.build_flags bit 31 (trains units): every unit u in 50..401 with `u.where == b`:
     role&0x180c0 → ai0+s else ai1+1.
4. Techs k in 544..628 with `k.find_preq(T)`: ai4+1; k epoch → ai4+1, ai0+1; k age → ai4+2, ai0+2.
5. Goods g in 0..49 with find_preq: v = g < 6 ? 4 : 1 → ai9+v, ai1+v.
   Spells 629..683 (slot 0 only): ai0+4, ai1+2. Bonuses 684..805: ai1+1 (+1 more for 684 FOREIGN_TRADE), ai4+2.
add_preq_ai(idx, n, depth): for slots 0..2 of this tech, p = get_preq; if p is a tech: `ai[idx]` of p += n and,
if depth ≠ 0, recurse into p with depth-1.
Unit fields read: where (+0x40), domain (+0x218), role (+0x2c8), carry (+0x2d4). Building fields: from (+0x3c),
graft, attack (+0x1e8), build_flags (+0x2c0).

`TechType::is_ultimate_preq` @0066c410 is a runtime query (Leader::lose_tech, scenario gain/lose tech, hotkey
strings), recursive find over the same effective preq slots; not part of loading.

### 9.4 GoodType (GoodType::init @0066ea20, 0..49)

Pass 0 (calls the virtual close first: Type::close frees desc/abbrev; GoodType::close is empty):

| XML (RESOURCE) | field (offset) | rule | default |
|---|---|---|---|
| NAME | name, type_name, display_name; graph_name, help_name, type_name = name | set_name | '' |
| ABBREV | abbrev (+0xc4) | text (UI) | '' |
| X_SIZE, Y_SIZE | x_size (+0x234), y_size (+0x238) | max(1, num) | 1 |
| BLOCK | block (+0x2f0) | max(0, num) | 0 |
| EXCLUDE_CTW | exclude_ctw (+0x2f4) | max(0, num) | 0 |
| RANDOM_RARE_DROP | random_rare_drop (+0x2f8, not walked) | max(0, num) | 0 |
| PREQ0, PREQ1 | preq[0], preq[1] | tech_key | -3 |
| OBS | obs (+0x4c) | tech_key | -3 |
| TEX_*, TEX2_ID/X/Y/XS/YS | TypeOut / tex2_* (+0x2fc..0x302) | UI only | |

Pass 1, basic goods (t < 6): `*_GOOD` via good_key (errors loudly), `*_RATE` via `String::fraction(s, 256)`
(`a*256/b`, truncating; no '/' → b = 1; b = 0 → 0):
UNDISC_COST_GOOD/RATE → undiscovered_cost_good/rate (+0x2b4/+0x2c8); OBS_COST → obsolete_cost (+0x2b8/+0x2cc);
UNDISC_SUPPORT → undiscovered_support (+0x2bc/+0x2d0); OBS_SUPPORT → obsolete_support (+0x2c0/+0x2d4);
OBS_PROD → obsolete_production (+0x2c4/+0x2d8). Then bonus_type = [-1,-1], bonus_num = [0,0].
Pass 1, special goods (t ≥ 6): for i in 0,1: bonus_type[i] = good_key(`BONUS_TYPE<i>`), bonus_num[i] =
num(`BONUS_NUM<i>`, -1). The 10 undiscovered/obsolete fields stay 0 (constructor).
Pass 1, all: largest_gather = 0; desc = DESC then String::lookup(…, "DESC"); abbrev = DESC_ABBREV then lookup
(UI only; note abbrev is overwritten by DESC_ABBREV).
Constructor (GoodType @0066ff80 + ObjectType): GoodTypeData fields 0; ObjectType defaults as in rulesdump.

#### largest_gather (GoodType::compute_largest_gather @0066e920)
Not part of Types::init: `Lands::init` @0067e730 (Game::init_non_rules_data after Types::init, and GameMod
reload) loads `rules.xml LANDS/LAND` (exactly 9; each exactly 4 `MAKE num=".." type=".."`, type → good_key)
and then calls it for goods 0..5 only. largest_gather = max `num` over MAKEs whose good is this good, over
land indices 0 and 3..8 (indices 1 Sandy and 2 Ocean are skipped), then clamped to [1, 2]. Goods 6..49 keep 0.
`computeLargestGather(ctx)` reproduces this.

### 9.5 ItemType (ItemType::init @00677bc0, 543)
Pass 0: virtual close (no-op), type, set_name(NAME), graph_name = type_name = name, abbrev = ABBREV, TEX_* (UI).
Pass 1: nothing. All other walked fields are ObjectType constructor defaults (x_size/y_size stay 0).

### 9.6 SpellType / crafts (SpellType::init @00674a80, 629..683)

Constructor (SpellTypeData @006749b0): spell fields 0, from2 = 0, use[] 0, grid_x2/y2 0; Type defaults.

| XML (CRAFT) | field (offset) | rule | default |
|---|---|---|---|
| — | use[6] (+0x1e0) | zeroed first | |
| NAME | name, type_name, display_name | name; display_name = lookup(name) | '' |
| GRAPH | graph_name, help_name | text | '' |
| TYPENAME | type_name (+0xb0) | text, or name when empty | name |
| TEX_* | UI | | |
| GRID_X, GRID_Y, GRID_X2, GRID_Y2 | grid_x, grid_y, grid_x2 (+0x1f8), grid_y2 (+0x1f9) | int8 | -1 |
| FROM | from (+0x3c) | object_key (unit, else building) | -3 |
| FROM2 | from2 (+0x1dc) | object_key | -3 |
| PREQ0 | preq[0] | tech_key | -3 |
| JOB_TIME | job_time | num, ulong (cast time, 1/15 s) | 0xffffffff |
| RES_TIME | res_time | num, ulong | 0xffffffff |
| DURATION | duration (+0x1d4) | num × 15 | -15 |
| DURATION_UPGRADE | duration_upgrade (+0x1d8) | num × 15 | -15 |
| MANA | mana (+0x1d0) | num | -1 |
| SPELL_RANGE | spell_range (+0x1cc) | num × 192 (0xc0, tile → fine units) | -192 |
| FLAGS | spell_flags (+0x1c8) | lower-case; per char: '1'..'9' → bit (c-0x17)&31, else bit (c-'a')&31 | 0 |
| COST | costs[6] | load_cost (research cost) | unchanged |
| COST2 | use[6] | load_cost (casting cost) | unchanged (0) |
| TRIBE_MASK | tribe_mask | binary | 0 |
| — | upgrade (+0x44) | always -1 | |

Flag letters (craftrules.xml header): a must be researched, b targets units, c buildings, d an area,
e friendly, f enemy, g usable without cancelling orders, h allied, i vehicles only, j non-vehicles only,
k at-peace objects, l doesn't blow cloak, m affected by "use craft as group".

FROM/FROM2 side effects and `where`:
* from < 0 → where = -1, and FROM2 is **not** examined.
* from a unit (50..401): unit.unit_flags2 |= 2 (spellcaster), where = unit.where.
  from a building (414..542): building.build_flags |= 0x20000000, where = from.
  Then if from2 ≥ 0: a unit → unit_flags2 |= 2; a building → build_flags |= 0x20000000 (no where change).
Post-step: costs of 669..683 = costs of 668.

### 9.7 BonusType (BonusType::init @00670280, 684..805)
type = t; desc = DESC then String::lookup(desc, "BONUSDESC") (UI). `PREQ` child (create=1): preq[0] =
tech_key(attribute `preq0`). preq[1] = t-1 for DESPOTISM_2 795, DESPOTISM_3 796, REPUBLIC_2 798, REPUBLIC_3 799,
MONARCHY_2 801, DEMOCRACY_2 803; else -1. No name: display_name stays '' (matches oracle). Everything else is
Type constructor defaults (tribe_mask -1, obs -2, show -2).

### 9.8 Side effects on other types (owned by other parts)
* buildings: build_flags |= 0x08000000 (set_research), |= 0x20000000 (spell FROM/FROM2).
* units: unit_flags2 |= 2 (spell FROM/FROM2); finalize's UnitType::init_spellcasters @0061aae0 later
  propagates bit 2 along graft/from chains (63 more units in the oracle).

## 10. Tribes, categories, names, game-start changes

Implementation: `re/tools/rules/tribes.mjs` (tribes, categories, string hash, display names, grafting,
`TypeData::get_preq`, `get_age_slow`, `get_military_level_slow`, `finalize_grafting`).

### 10.1 Tribes

#### Records
The global `tribes` (class Tribes) is an `ObjectArray<Tribe>` with **25** entries (Tribes::Tribes @006f12c0).
Entries 0..23 are the nations; entry 24 only gets name "Random" (Tribes::init writes `list[24].name`). The rules
image walks entries 0..23 (Game::walk_rules_data @00589550 -> Tribe::walk_rules_data @006f1270: bytes
[0x54,0x6c) = tribe, barbarian, people, backup_build_continent, build_continent, unit_continent, then graft[352]).
`text_substitute` (0x6c) is not walked.

Tribe layout (size 0x5f0): file String 0x04, name 0x18, eng_name 0x2c, old_name 0x40, tribe 0x54, barbarian 0x58,
people 0x5c, backup_build_continent 0x60, build_continent 0x64, unit_continent 0x68, text_substitute 0x6c,
graft TypeIndex[352] 0x70 (index k = unit type 50+k). Constructor (Tribe::Tribe @006f13a0): scalars 0, graft all
TYPE_NONE (-1).

#### Tribes::init(int keepGrafts, XMLElement *mod) @006ef430
Called from Game::init_rules_data @0058ec60 with (0, null), after the `agename_cats` category list and before
`treaties`. Also from Game::end_game_close @00594aa0 with (1, null), from GameMod::initExistingData (mod element),
and from Conquest setup paths.
1. Open `rules.xml`, element `TRIBES` (the `TRIBES_TRIAL_VERSION` block is not used). Take its `TRIBE` children;
   there must be exactly 24 ("Number of tribes in rules.xml does not equal NUM_TRIBES", returns 1).
2. For i = 0..23: `eng_name` = text of `KEY`, trailing spaces trimmed (this is the key Tribes::find matches);
   `file` = `"TRIBES\" + FILE text`, trailing spaces trimmed. With a mod element the mod's i-th TRIBE `FILE` is
   used verbatim unless it equals "default".
3. For i = 0..23 open `file` (path with a directory part, so relative to the install root: `<install>/tribes/aztecs.xml`,
   language suffix applies; "Can't Open Nation File" and return 1 on failure). Root element `<TRIBE name="...">`:
   - name = attribute `name` (trailing spaces trimmed)
   - unit_continent, build_continent, backup_build_continent = get_text_num of `UNIT_CONTINENT`, `BUILD_CONTINENT`,
     `BACKUP_BUILD_CONTINENT`, default -1. The texts are like `2 American`, so convert_int/_wtoi gives the leading number.
   - barbarian = 0; text_substitute = attribute `substitute` as a number, default 0
   - tribe = i; people = i / 3 (integer)
   - if keepGrafts == 0: graft[k] = 50 + k for all 352 slots (identity). With keepGrafts != 0 the grafts stay.
   Write order verified in the disassembly: 0x68, 0x64, 0x60, 0x58 = 0, 0x6c, 0x54, 0x5c.
4. `list[24].name = "Random"`.

So before unit loading every tribe has the identity graft table and the continents from its nation file.

Related, no effect on the rules image of a normal game:
- Tribes::find @006ef2d0: case-insensitive equality of `eng_name`; -1 if none.
- Tribe::init @006f0230 (tribe = i, people = i/3, identity grafts) is the same reset, used elsewhere.
- Tribe::set_barbarian @006eff70 and Tribe::set_substitute_tribe @006efda0 are only called from
  ConquestGame::setup_scenario. set_barbarian sets barbarian = 1 and reloads continents and name from a barbarian
  nation file (`TRIBES/` + conquest barbarian file, fallback `TRIBES/BARBARIANS.xml`); set_substitute_tribe sets
  barbarian = 0, reloads from the given file and sets text_substitute = 1. Both copy name to old_name first.
- Tribe::parse @006f0560 only fills help-text parameters from per-civ constants (no table writes).

#### Unit-side grafting (writes tribe graft tables)
Inputs per unit (UnitType::init pass 1 @0061ab50): `tribe_mask` = String::binary(TRIBE_MASK) (24 characters, most
significant first, so tribe t is bit t = character 23-t); `graft` = Types::unit_key(GRAFT) (`none` -> -1).

- **End of pass 2** (same function, after the role and radius code): if graft g >= 0, for every tribe t with bit t set
  in the unit's tribe_mask, `tribes[t].graft[g-50] = unit`. Units run in document order (types 50..413), so a later
  unit overwrites an earlier one for the same tribe and slot.
- **Pass 3**: for each unit u (document order) and each tribe t (0..23): if `tribes[t].graft[u-50] != u` and bit t
  is clear in u.tribe_mask, then for every unit v in 50..401 with v != u, bit t clear in v.tribe_mask and
  v.graft == u: `tribes[t].graft[v-50] = tribes[t].graft[u-50]`. (This redirects grafts onto a unit that the tribe
  itself replaces.) For gaia u (402..413) the read `graft[u-50]` runs past the array into the next Tribe; nothing
  grafts onto gaia, so it has no effect and the implementation skips it.
- All units finish pass 2 before pass 3 starts (Types::init @00669cc0 runs pass-major).

#### Game start: Types::init_civ_masks @00669a60 then Types::finalize_grafting @00669840
Both are called at the end of Game::init_data @0058dca0 (every game start, from Game::init @0058c480), and
finalize_grafting also from WalkDataGame::walk_data @005a2360 (loading a save). They run after the rules image
captured by RecordGame::write_rules, so their effects are in the oracle.

init_civ_masks: for buildings 414..542, `civ_graph_mask = 0`. The building_graphics.xml probing in
BuildType::init_civ_masks @006320c0 tests an XMLElement that is never assigned, so no bit is ever set.

finalize_grafting:
1. barbMask = OR of (1 << t) for tribes with barbarian != 0 (0 outside Conquest).
2. For u = 50..401 in order: g = u.graft; if g != u, g >= 0 and `u.unit_flags & 0x1000000`:
   if `(game->info.flags & 8) == 0`: `types[g].tribe_mask = (types[g].tribe_mask & ~u.tribe_mask) | barbMask`;
   else `types[g].tribe_mask |= u.tribe_mask`. (Oracle: flag 8 clear.)
3. For u = 50..401: `age = get_age_slow()` (vtable 0xe8 = ObjectTypeData::get_age_slow @00661a00) and
   `military_level = UnitTypeData::get_military_level_slow @0061d4d0`. For buildings 414..542: `age = get_age_slow()`.
   (UnitType::init sets military_level = -1 on every pass; this is where the real value comes from.)

get_age_slow: for i in 0 .. num_preq-1 (UnitTypeData::num_preq = 2, BuildTypeData::num_preq = 3, default 1):
p = get_preq(i, -1); if p is a tech (544..628) return `techs[p].age + 1` if p is an age (544..550), else
`techs[p].age`. If none, 0.

get_military_level_slow: p = preq[0]; if p is not a military epoch (551..578 with cat 0) then p = get_preq(1, -1);
return p - 571 if p is a military epoch, else 0.

TypeData::get_preq(i, player) @00668700, as used here with player = -1. Let s = starting_technology, or
min(starting_technology2, starting_technology) when game_rules == 8; e = ending_technology.
- i = 0: preq[0]. i > 2: -1.
- i = 2: p = preq[2]. If p is an epoch, p1 = get_preq(1) is an epoch with the same `cat` and p <= p1: -1.
  If p is an age, where == UNIVERSITY (420) and e < techs[p].age + 1: -1. Else p.
- i = 1: p = preq[1] (the Iroquois early-government branch needs a player and is skipped). If p >= 0 and
  (s != 0 or e < 7):
  - if p is an epoch, this is a unit (50..413) and techs[p].cat == 0: if age(p) < s then -1; else
    q = (28 / (e - s + 1)) * (age(p) - s + 1) + 3, q = q / 4 + 571 (C division); return 572 if q <= 571, q if q < 579, else 578.
  - else if s != 0 and p is an epoch: if s <= age(p) and 7 - s > 0: base = 565/558/551/572 for cat 1/2/3/other,
    q = ((age(p) - s + 1) * 7) / (7 - s) - 1 + base; q = min(q, preq[1]); return q unless preq[2] is an epoch with the
    same cat as q and preq[2] > q (then -1). Otherwise -1.
  - Else p.

**Game-setting dependence:** age and military_level depend on the start age, end age and game rule 8. With
start age 0 and end age >= 7 (the oracle game), get_preq(1) is just preq[1]. A replay with a different start or
end age gets different age/military_level values in its rules image.

### 10.2 Categories

Category (0x58 bytes): name String 0x00, key 0x14, desc 0x28, data int[6] 0x3c, alpha_order uchar 0x54.
The constructor (@00435330) zeroes data[]; alpha_order is set by init.

Categories::init(count, rulesRoot, id) @006f21d0:
- If count > 0 the list is sized to `count` first. Find the first `CATEGORIES` element whose `id` attribute equals id
  (String==, case-insensitive). None: return 1, and Game::init_rules_data then fails.
- list.name = attribute `name`, else id; list.title = lookup(attribute `title`, "CATTITLES").
- Children `CATEGORY` (get_elements upper-cases the name). If count < 0 the list takes the child count. If count is
  not the child count, the game reports "Category data mismatch" and still reads `count` entries.
- Per entry: name = lookup(attr `name`, "CATNAME"). Names are not trimmed (unit_cats names carry trailing spaces).
  key = attr `key`, or the looked-up name if that is empty. desc = lookup(attr `desc`, "CATDESC").
  data[k] = convert_int(text of the k-th `DATA` child), at most 6; missing entries stay 0.
- alpha_order: start with the identity order and exchange-sort it (for i < j: swap when name[i] is non-null and
  name[j] null, or `_wcsicmp(name[i], name[j]) > 0`). Then `alpha_order[order[k]] = k`.
- find_key @006f20e0: first index whose key String== s (case-insensitive); otherwise Error::key and -1.
  Type `cat` fields store this index (verified for units and buildings).
- find_data @006f1e30: first index with data[0] == value (the first argument is ignored); -1 and an error if none.
- The GameMods overload @006f1f20 initialises the `mods` list from `id="mods"` and then appends one entry per
  installed mod (name = mod name, key = its 1-based number as text, desc = mod description, alpha_order = number).

Lists in Game::init_rules_data order (global, count, id). Tribes::init runs after agename_cats.

| global | n | id | global | n | id |
|---|---|---|---|---|---|
| tech_cats | 4 | tech_cats | tech_costs | 9 | techcosts |
| unit_cats | 9 | unit_cats | starting_technologies | 10 | startingtechs |
| build_cats | 3 | building_cats | starting_technologies2 | 5 | defendertechs |
| stances_combat | 6 | combatstances | ending_technologies | 8 | endingtechs |
| stances_worker | 4 | workerstances | reveal_maps | 4 | revealmaps |
| stances_caster | 2 | casterstances | pop_limits | 6 | poplimits |
| stances_packer | 2 | packerstances | rush_rules | 15 | rushrules |
| agename_cats | 8 | agenameshort | cannon_times | 5 | cannontimes |
| treaties_cats | 3 | treaties | handicaps | 21 | handicaps |
| team_styles | 14 | gamestyles | player_slots | 12 | playerslots |
| game_rules | 12 | gamerules | number_of_players | 8 | numberofplayers |
| map_styles | 23 | mapstyles | max_observers | 7 | maxobservers |
| diffs | 6 | difficulties | game_speeds | 5 | gamespeeds |
| map_sizes | 7 | mapsizes | scores | 12 | scores |
| victories | 11 | victories | time_limits | 9 | timelimits |
| eliminations | 4 | eliminations | chairs | 8 | chairs |
| starting_towns | 4 | startingtowns | wonderwins / popwins | 13 / 13 | wonderwins / popwins |
| starting_resources | 13 | startingresources | econwins | 9 | econwins |
| starting_resources2 | 5 | defenderresources | victorytypes | 7 | victorytypes |
| | | | gameinfo_flags | 12 | gameinfo_flags |

All counts match the shipped rules.xml. unit_cats keys: Foot, Mounted, Mech, Artillery, Command, Civilian, Sail,
Naval, Air (indices 0..8); build_cats E, M, W; tech_cats Military, Civic, Commerce, Science.

### 10.3 Names and localisation

#### String hash (String::generate_hash @00a1b6b0)
`primes[50]` is at @00b14500 (`?primes@@3PAKA`, .rdata; `num_primes` = 50 @00b144f8). The values are in tribes.mjs.
Walk the UTF-16 units from last to first, with i = index and n = i + 1, all uint32:
`h += primes[i%50]*c + primes[c%50]*n`; `hi += primes[towlower(c)%50]*n + towlower(c)*primes[i%50]`.
h is `hash_value` (case-sensitive) and hi is `hash_value_insensitive`. A null or empty String hashes to 0.
towlower is the "C"-locale one (A-Z only). String== with both hashes cached compares hash_value_insensitive.

#### Display names (Type::set_name @00663ae0, Type::lookup_display_name @006677f0)
set_name(n): name = n, type_name = n, display_name = vcall 0xb0(n) = lookup_display_name for every type class
(checked in the vtables of Unit/Build/Tech/Good/Spell/Item/BonusType). Units, buildings and spells then override
type_name with TYPENAME. Spells call the virtual directly (SpellType::init @00674a80).
lookup_display_name(n): XPath `<SECTION>/STRING[@hash="<hash_value(n)>"]` under ROOT of `typenames.xml` (cached
in `types.names_xml`, else opened each time). The section comes from the TypeData::is_* range tests, in this order:
414..542 BUILDS, 50..413 UNITS, 629..683 SPELLS, 543 ITEMS, 0..49 GOODS, 544..550 AGES, 551..578 (epochs) TECHS,
684..805 BONUSES, otherwise (579..628) MISC.
If a non-empty text is found it is the display name. Otherwise the result is n itself: the game appends
`<STRING hash=.. needed="1">n</STRING>` and tries to save typenames.xml, with an error if the file is read-only.
Bonuses have no NAME, so their display name is "".

#### String::set_string_lookup @00a19980 / String::lookup @00a1ddb0
- set_string_lookup(file): an empty name means `string_lookup.xml`. Loads ROOT's `CAT hash=` elements, each holding
  `STRING hash= hash_i=` entries.
- lookup(s, cat): a 0 hash (empty s) returns s. Otherwise find the CAT whose hash == hash(cat), then the first
  entry with hash == hash(s) and hash_i == hash_i(s), and return its text. If not found, return s; the dev path
  adds the string and saves the file.
- Active table: Main::main sets the default (string_lookup.xml), so the Categories (CATTITLES/CATNAME/CATDESC) use
  it. Types::init switches to `resourcerules_strings.xml` for the RESOURCE passes and to `rules_string_lookup.xml`
  for TECHBONUSES/BONUS, then back to the default. Lands::init and Forms::init use `rules_string_lookup.xml` and
  then reset (call-site disassembly in re/decomp/rules_tribes/asm/ssl_sites.asm).
- English texts are identical to their keys, so for English every lookup is the identity.

#### Language suffix (XML::init @00a279e0)
The file name is `data\<name>` when it has no directory part, otherwise it is taken as-is relative to the content
root (the tribe files). If `gLanguageSuffix[GetLanguage()]` (@00b1ca54) is non-empty and `<file><suffix>` opens,
that file is used. LOC_NUM_LANGUAGES = 5: 0 German ".7", **1 English "" (no suffix)**, 2 Spanish ".10",
3 French ".12", 4 Italian ".16". The `.4/.9/.17/.18` files are not used by this build; `.9` is byte-identical to
the base file. The root element is the document's first element; an optional second argument selects a child.
Suffixes apply to every file loaded through XML::init, including tribes/*.xml, typenames, alternatenames,
string_lookup, rules_string_lookup and resourcerules_strings.

### 10.4 Game-start changes vs a pure XML load

Game::run @00584590 order: init_rules_and_teams -> init_console -> init_type_backup -> Game::init (-> init_data)
-> ... -> RecordGame::write_rules (rules image, when recording and not loading a save).

- **Game::init_rules_and_teams @00589bb0** and its callees init_random_colors, Map::set_map @006a1340,
  init_starting_resources @0058a500, Leaders::init @006ed850, LeaderOptions::init @006f1d40, init_teams @0058ae70,
  init_handicaps @0058abf0, init_tribes @0058a600: **none write types, constants, tribes or balance.** They resolve
  GameInfo settings (defaults, a random start age when starting_technology == 9, a random resource setting when it
  is 12), the map, the per-player starting goods (Game::starting), leader flags, teams, handicaps and random
  civilisations (leader tribe 24 = any of the 24, 25..28 = one of four fixed civ groups, drawn with game_random). They do consume `game_random`.
- **Game::init @0058c480**: if `reload_rules` (set by Game::end_game_close) -> Game::restore_rules_data @005890b0
  reloads the rules image from `<appdir>\StateBackup\state.dat`. That file is written by Game::dump_rules_data,
  called from Main::init_common_data right after the XML load, so it holds the pure-XML state.
- **Game::init_data @0058dca0** (both branches): **Constants::init runs again** (constants are re-read from
  rules.xml at every game start), then Heroes/Specials/Supplies/Caravans/Cities/Wonders/Forts/Docks/OilWells/
  Objects/Herds/OptionInfo/PathFinder/Armies::init, then **Types::init_civ_masks** and **Types::finalize_grafting**
  (section 1). These change the image: building civ_graph_mask = 0, graft-target tribe_mask, and unit/building
  `age` and unit `military_level`, which depend on the game settings.
- Game::init_non_rules_data @0058fda0 (part of the initial load, not game start): Types::init, then Lands::init
  @0067e730, which calls GoodType::compute_largest_gather for goods 0..5 (good `largest_gather`), and Forms::init.
- Game::end_game_close: Tribes::init(1) (names and continents re-read, grafts kept), reload_rules = 1,
  restore_type_backup.
- Not verified: Setup::build_game and ScenarioFuncSet::init inside Game::init (scenario setup) and scenario
  scripts, which may change rules. In Conquest, set_barbarian/set_substitute_tribe change tribes.

## 11. Combat balance table (`Balance::init` @00582d20, balance.xml)

Implementation: `re/tools/rules/balance.mjs`.

### 11.1 What it is

- `combat_table` is a global `Balance` (game.obj, @00c12bf0: a vbtable pointer at +0, then `short final_balance_table[493][493]`
  at 0xc12bf4). `Balance::walk_rules_data` @00582cc0 walks it row-major as 493*493 int16, which is the rules image /
  replay order. `Balance::log_data` @00582bd0 logs it under "COMBATTABLE".
- Row = **attacker** TypeIndex - 50, column = **defender** TypeIndex - 50, for types 50..542 (units 50..401, gaia
  402..413, buildings 414..542 incl. wonders 526..542). The item (543) and anything above are not covered.
- Value = damage percent: 100 = normal, 33 = a third, 2574 = 25.74x. Range in the shipped data: 5..2574, with 367
  distinct values. Gaia rows and columns are always 100.
- Built once by `Balance::init` @00582d20, which is the last step of `Types::init` @00669cc0 (callers: Game::init_non_rules_data
  @0058fda0 and GameMod::initExistingData @005aa3e0). For a replay or saved game the table is loaded from the rules image.

### 11.2 How the simulation reads it

- `Balance::return_modifier(att, def)` @00581ca0 returns `final_balance_table[att-50][def-50]`. The compiler folded the -50
  into the base address (it appears as `s_SteamWorkshopTagLinks+0x22c` = 0xc06afc). This function has no callers.
- The only real reader is **`ObjectData::get_damage` @00644130**, which inlines the same read with
  `MOVSX word [type*2 + 0xc06afc]` at 0x0064418e. The attacker is this object's type and the defender is the target object's type.
  The first step there is `damage = (attack_value * table[att][def]) / 100` (int, truncating). Many runtime modifiers follow
  (flanking, height, river, entrenchment, garrison, tribe bonuses and others), and those are outside this area. Callers of get_damage are
  `Object::do_damage` @0064a480 and `Object::compare_target` @0064e5c0 (AI target choice).
- **Every cell matters for lockstep**: get_damage is part of the simulation, and compare_target feeds AI decisions.

### 11.3 Inputs

`computeBalance(dataDir, info)` in `re/tools/rules/balance.mjs` returns `number[493][493]` (int16 values).

`info.types[t]` for t in 50..542 needs these fields, all as loaded by Types::init (ctx.types works unchanged):

| field | used for |
|---|---|
| `type` | the type's own index |
| `name` | internal `<NAME>` (Type::name, TypeData+0x60) of units 50..401; builds the XML lookup names |
| `obj_masks` | ObjectTypeData+0x1e4; the flag rows/cols (dead, see below) and most of type_damage |
| `age` | ObjectTypeData+0x278; must not be -1 for 50..542 except gaia (true in the data; otherwise get_age_slow is needed, see open questions) |
| `domain` | ObjectTypeData+0x218 (attacker, one rule) |
| `is_list`, `is_strict_list` | `{count, v[]}` from ObjectType::init_is_list; used by ObjectTypeData::is |
| `graft`, `from` | only for the is_slow fallback (never reached with stock data) |
| `unit_flags`, `unit_flags2` | units only: is_siege = unit_flags & 0x20000, is_caravan = unit_flags2 & 8 |

`info.constants`: `one_age_down`, `two_ages_down`, `three_ages_down`, `four_ages_down`, `five_ages_down` (Constants +0x70..+0x80;
rules.xml ONE_AGE_DOWN.. FIVE_AGES_DOWN = 15/20/50/60/70).

`rulesdump.mjs` passes the loaded types with `age` filled by `get_age_slow` under the load-time GameInfo (that is
what `ObjectTypeData::get_age` returns while the cached `age` is still −1 during `Types::init`).

### 11.4 Step 1: the raw 399x399 table from `Data/balance.xml` (Balance::fill_tables @005823f0, first half)

File: `<ROOT><TABLE><ENTRY name="X" A="n" B="n" .../>...</TABLE></ROOT>`, about 1.6 MB with 291 ENTRY elements and 291 attributes each.
`Balance::init` opens it with `XML::init("balance.xml")` and gets `TABLE` with get_element(…, create=1, exact=0).

**Raw indices 0..398** (`Balance::lookup_absolute_name` @00582910 → `Balance::lookup_name` @005829b0):

| raw index | kind | name before mangling |
|---|---|---|
| 0..351 | BALANCE_NAME | `unittypes[50+i].name` with every `'` removed (String::purge) |
| 352..356 | BALANCE_LINE | SIEGE, FORTS, TOWERS, CITIES, OBSPOST |
| 357..358 | BALANCE_OBJECT | BUILDINGS, UNITS |
| 359..366 | BALANCE_AGE | "AGE 0".."AGE 7" |
| 367..398 | flag | "Flag " + letter + ISTR#17 + "OBJMASK_xxx" (bit 0..31) |

Afterwards every space becomes `_` (String::replace). So "Caesar's Legions" becomes `Caesars_Legions`, "Man o' War" becomes `Man_o_War`,
and "AGE 3" becomes `AGE_3`. The flag letter is 'A'+bit, and when that passes 'Z' it becomes the character minus 42, so bits 26..31 get '1'..'6'.
The OBJMASK order (bit 0..31) is ARMORED, BOMBARD, CIVILIAN, MUSKET_INF, ELEPHANT, FOOT, GUN, HEAVY_INF, MODERN_INF,
CARRY_AIR, FOOT_ARCHER, LARGE, MOUNTED, NAVAL, HORSE_ARCHER, SPARSE, LIGHT_INF, ARCHERY, SIEGE, WAR_MACHINE, ARMORPIERCE,
VEHICLE, MELEE, EXPLOSIVE, HEAVY_CAV, DETECT, UNUSED, MISSILE, AIR, LIGHT_CAV, PIKE, ANTI_AIR (string array set up in Balance::init).

**Quirk (verified):** the separator ISTR#17 (internal string offset 0x154) is an empty `<STRING/>` in internal_strings.xml.
The game therefore looks for `Flag_YOBJMASK_HEAVY_CAV` while the file says `Flag_Y_OBJMASK_HEAVY_CAV`, so **all 32 flag rows
and columns stay 100**. Applying the file's flag cells gives 27,918 mismatches, and leaving them out gives 0.

**Fill:** `raw` is 399*399 int16 (malloc 0x4dbc2), `raw[row*399 + col]`. For each row r it does
`TABLE.selectSingleNode('ENTRY[@name="' + name(r) + '"]')` (get_element create=0, exact=1, so the match is case sensitive and the first one wins).
For each col c it sets `raw = 100`, and if the entry exists, `raw = (short)get_attrib_num(name(c), 100)`. A missing or empty attribute gives 100
(XMLElement::get_attrib_num @00a27580, String::number). Rows with no ENTRY are all 100.
The data has these consequences:
- Several unit types share a NAME (e.g. 6 "Tank", 6 "Main Battle Tank", 3 "General"). Each such row and column gets the same entry.
- 51 unit names have no ENTRY, and the ENTRY/attribute names Pathfinder, Pioneer, Ranger and Marines match no unit (dead).
- 348 non-100 cells are effective. All values are plain integers.
- A Prefs-gated debug branch that re-saves the XML (Prefs key at ISTR 0xf78) has no effect on the table.

### 11.5 Step 2: final cell (Balance::compute_modifier @00581cc0)

For att, def in 50..542 (outer loop over att, inner over def, both from BASE_UNITTYPES, fill_tables second half):

1. If att or def is gaia (402..413, TypeData::is_gaia_type), the cell is **100**. type_damage is not applied.
2. `pack(t)` (Balance::return_pack @005821c0) gives {type, line, obj, age, flags}, each starting at -1 (flags 0):
   - unit (50..413): type = t-50; line = 0 (SIEGE) if is_siege (unit_flags & 0x20000); obj = 1 (UNITS).
   - building: line = 1 if is(443 FORTX) [is_fort], else 3 if is(414 VILLAGE) [is_city], else 4 if is(521 LOOKOUT),
     else 2 if is(439 TOWER), else -1; obj = 0 (BUILDINGS).
   - age = get_age() (the age field, or get_age_slow when it is -1); flags = obj_masks.
3. The index lists are, in this order: type (if ≥0), line+352, obj+357, age+359 (if ≥0), then 367+bit for each set bit of flags,
   bit 0→31. The rows come from att and the cols from def.
4. `m = 100; for r in rows: for c in cols: m = (raw[r*399+c] * m) / 100` (int32, C truncation toward zero, row-major order;
   the order matters because each step truncates).
5. `cell = (short)((type_damage(att, def) * m) / 100)`.

### 11.6 Step 3: Balance::type_damage @0057fb50 (hard-coded, no data file)

v starts at 100. Every step is `v = (v*k)/100` (written `*k`) in int32 with truncation unless shown otherwise. The order is exactly as
listed, and `balance.mjs::typeDamage` follows it line by line. `A` is the attacker and `D` the defender; `a.X`/`d.X` test obj_masks bits;
`is(T,n)` is ObjectTypeData::is non-strict and `is!(T,n)` is strict; `bld(D)` means D is 414..542; `unit(T)` means 50..413; `siege(T)` means unit && unit_flags&0x20000;
`missile(A)` means unit && MISSILE bit; `caravan(D)` means unit && unit_flags2&8. The bit names are FOOT_ARCHER=0x400, LIGHT_INF=0x10000, HORSE_ARCHER=0x4000,
HEAVY_INF=0x80, GUN=0x40, MOUNTED=0x1000, LIGHT_CAV=0x20000000, MUSKET_INF=8, MODERN_INF=0x100, SPARSE=0x8000, SIEGE=0x40000,
ARMORED=1, VEHICLE=0x200000, HEAVY_CAV=0x1000000, CIVILIAN=4, NAVAL=0x2000, AIR=0x10000000, ANTI_AIR=0x80000000,
ARMORPIERCE=0x100000, MELEE=0x400000, BOMBARD=2, FOOT=0x20.

1. Age: if unit(A) && unit(D) && age(D) < age(A), then v = 100 + {1:one,2:two,3:three,4:four,≥5:five}_age(s)_down (the binary computes it as `((k+100)*100)/100`).
2. a.LIGHT_INF: d.FOOT_ARCHER*162, d.HORSE_ARCHER*152.
3. a.HEAVY_INF: if not a.GUN: d.MOUNTED*166, bld(D)*130, d.FOOT_ARCHER*86, d.MUSKET_INF*75, d.MODERN_INF*75.
   If a.GUN: d.MOUNTED*257, d.HORSE_ARCHER*66, d.LIGHT_CAV*114, is(D,216 ARMOREDCAR)*225, is(D,239 LIGHTTANK)*132, d.MODERN_INF*80.
4. a.FOOT_ARCHER: d.LIGHT_INF*65, d.MUSKET_INF*90, d.HEAVY_INF*253, d.HORSE_ARCHER*138, d.SPARSE*80, bld(D)*33, siege(D)*75.
5. a.LIGHT_CAV: d.FOOT_ARCHER*114, d.LIGHT_INF*158, d.MUSKET_INF*149, d.MODERN_INF*159, is(D,123 MACHINEGUN)*130,
   is(D,131 FLAMETHROWER)*125, d.SIEGE*122, d.ARMORED*98, bld(D)*33. Then v/=3 if caravan(D) or D∈{61,62,400};
   then v/=2 if D∈{50..53} or is(D,66 MILITIA).
6. a.HORSE_ARCHER: bld(D)*33, siege(D)*75, d.LIGHT_CAV*57, d.FOOT_ARCHER*57, d.LIGHT_INF*66, d.MUSKET_INF*66, d.HEAVY_INF*160.
   If also a.VEHICLE: d.LIGHT_CAV*160, d.FOOT_ARCHER*135, d.LIGHT_INF ×2, d.MUSKET_INF ×2, d.HEAVY_INF*50, d.HEAVY_CAV*97.
   If also a.GUN: is(D,123)*155, d.ARMORED*92.
7. a.HEAVY_CAV: d.FOOT_ARCHER*170, d.LIGHT_INF*179, d.MUSKET_INF*155, d.MODERN_INF*170, is(D,123)*155, is(D,131)*140, bld(D)*33.
8. a.MUSKET_INF: *133 (always), d.FOOT_ARCHER*125, d.HEAVY_INF*180, d.HORSE_ARCHER*114, bld(D)*66, siege(D)*75,
   and if age(A)==4 && d.HEAVY_INF, *110.
9. a.ARMORED: with is(A,239): d.MUSKET_INF*155, d.MODERN_INF*185, d.HEAVY_CAV*115, d.LIGHT_CAV*120, d.HORSE_ARCHER*120,
   is(D,123)*175. With is(A,216): d.MODERN_INF*175, d.MUSKET_INF*175, is(D,239)*120. Then bld(D)*66.
10. a.MODERN_INF: d.HEAVY_INF*260, is(D,216)*170, is(D,239)*75, is(D,123)*66, d.CIVILIAN*120, siege(D)*75, bld(D)*66.
11. is(A,123): d.(MUSKET_INF|LIGHT_INF)*330, d.MODERN_INF*330, d.HEAVY_INF*330, d.CIVILIAN ×5, d.ARMORED*50, bld(D)*50, siege(D)*75.
12. is!(A,131): d.MUSKET_INF*88, d.MODERN_INF*80, d.MOUNTED*125.
13. a.ARMORPIERCE && d.ARMORED: *180.
14. is(A,66) && d.(LIGHT_INF|HORSE_ARCHER): ×2. Then A∈{50..53} && d.(LIGHT_INF|HORSE_ARCHER): ×2.
15. a.NAVAL: bld(D) && !a.SIEGE *33; unit(D) && !d.(NAVAL|AIR) && !siege(D) *33; unit(D) && !d.NAVAL && siege(D) *66;
    is(A,346 BOMBARDSHIP) && d.NAVAL *33; (is(A,323 BARK) or is(A,338 SUB)) && d.NAVAL && d.CIVILIAN ×2.
16. is(A,334 FIRERAFT): is(D,340 TRIREME)*310, is(D,346) <<2, is(D,334) <<2. Then is(A,340) && is(D,323)*160.
17. unit(A) && !a.NAVAL: (!a.SIEGE && d.NAVAL)*33, then (siege(A) && d.NAVAL)*350.
18. (is(A,304 BOMBER) or is(A,308 FIGHTERBOMBER)) && bld(D) && d.ANTI_AIR: *25.
19. a.AIR && !missile(A) && a.ANTI_AIR: bld(D)&&!d.ANTI_AIR *15; bld(D)&&d.ANTI_AIR *85; is(D,304) ×2; siege(D)*125; d.NAVAL*350.
20. is(A,310 HELICOPTER): is(D,334) <<3, is(D,239)*450. Then unit(A) && !a.ANTI_AIR && is(D,310): *25.
21. is(A,313 V2ROCKET) && D is a wonder (526..542): v/=2.
22. is(D,63 SUPPLYWAGON): a.MELEE*180, then if bld(A), v = (v*14)/5.
23. a.SIEGE && bld(D): *430. a.BOMBARD && bld(D): *250.
24. is(D,447 AIRBASE) && (A.domain != 0 || a.SIEGE): v/=3.
25. Attacker specials: is(A,88)&&d.LIGHT_INF*150; is(A,174): d.FOOT_ARCHER*125, d.HORSE_ARCHER*125;
    is(A,91): d.HEAVY_CAV*115, d.LIGHT_CAV*115, d.HORSE_ARCHER*115.
26. a.HORSE_ARCHER: is(A,191): d.LIGHT_INF*135, d.MUSKET_INF*120; is(A,195)&&d.HORSE_ARCHER*150; is(A,199): d.LIGHT_INF*160, d.MUSKET_INF*145.
27. a.LIGHT_CAV && is(A,224)*105.
28. a.FOOT_ARCHER: is!(A,177)/is!(A,178)/is!(A,179) each with d.HEAVY_INF *130; is!(A,174) && d.FOOT_ARCHER *135.
29. a.HEAVY_CAV && is(A,232)*105.
30. a.HEAVY_INF: is(A,146)&&d.HEAVY_INF ×2; is!(A,160)*50; is(A,149): d.LIGHT_CAV*180, d.HEAVY_CAV*180, d.HORSE_ARCHER*110;
    is(A,151)*130; is(A,144)&&d.VEHICLE*135.
31. is(A,114)&&d.FOOT*130; is!(A,130)&&d.FOOT*130.
32. For n in 258, 259: if is!(A,n): d.(MUSKET_INF|MODERN_INF|LIGHT_INF)*115, is(D,123)*120.
33. If a.SIEGE, finish with: is(A,279)&&bld(D)*135, is(A,276)&&bld(D)*110, is(A,274)&&bld(D)*110.

Here "×2", "×5", "<<2" and "<<3" are plain int32 multiplies with no /100, and "v/=n" is truncating division.
Helper semantics (vtables read from the exe):
- vslot 0x60 is ObjectTypeData::is @0065f7d0. It returns true when own type == n. Otherwise it checks membership in is_list (non-strict, n ≥ 0) or
  is_strict_list (strict), and falls back to is_slow @00661ae0 when the list is empty.
- vslot 0x10c is UnitTypeData::is_siege @00470460, 0x114 is UnitTypeData::is_missile @0061d430, and 0x130 is UnitTypeData::is_caravan @00470420.
  For BuildType these slots are a folded `return 0` (@0041bff0).
- BuildType vslots 0xfc and 0x64 are is_fort @00472ba0 = is(443) and is_city @00472bc0 = is(414).

### 11.7 Determinism

- **No floating point anywhere.** Everything is int32 multiply then `idiv` (truncation toward zero), and the result is stored as int16. In the
  shipped data no intermediate value comes near overflow: the largest type_damage and the largest cell are both 2574, and every value is positive.
  For bit-exactness, still use int32 wrapping multiply (`Math.imul` / `int32_t`), C division, and `(int16_t)` on store.
- Order matters at two levels: the row×col product loop (each step truncates), and the type_damage rule order above.
- The hard-coded TypeIndex constants assume the stock unitrules/buildingrules document order, as the original does.
- The result depends only on rules data, never on game setup, so the oracle captured at game start equals the table built at load.

### 11.8 Other files

- `balancerules.txt` (install root) is the INI file for the **UnitBalance** dev tool (`UnitBalance::init` @009b7390 references
  ISTR "balancerules"). It does not affect the table.
- `counterchart.txt` is a human-readable counter chart and no code references it.
- `Data/paramtypes.xml` is not used by Balance.

### 11.9 Open questions

- get_age_slow (@00661a00, takes the age from the first tech prerequisite: an age tech gives its age+1, any other tech its age) is not
  implemented and throws. With the stock data every type 50..542 except gaia has age >= 0, so it is never reached. A mod that leaves AGE unset would need it.
- is_slow (@00661ae0) is implemented from its disassembly but never exercised: non-strict checks own type, then graft == x, then recurses
  into types[from] (TypeData+0x3c) when from >= 0. Strict works for units only, as graft == x && !(types[x].unit_flags & 0x1000000).
- The string data for ISTR#17 has a hash (42787), which suggests the original was not empty (probably "_" or " "). The shipped
  internal_strings.xml has it empty, and the oracle confirms the empty behaviour.

## 12. Constants (`Constants::init` @00569a90, rules.xml `CONSTANTS`)

Struct `Constants` (0xd68 bytes, global `constants` @00e38b88, reached through `GameAccess::constants` @00c061f0):
848 int32 fields at 0x000..0xd3c, then `XMLElement curr_element` at 0xd40 (0x28 bytes, the cursor used while loading).
Only the 848 ints are walked into the rules image / checksums (`Constants::walk_data` @0057f910,
`Game::walk_rules_data` @00589550, `CheckSums::check_constants` @00936cb0 walk `[this, &curr_element)`).
`Constants::walk_data` additionally walks `[0x804, 0x808)` (`mongol_three_mil_cavalry`) a second time — this is the
`constantsDup` int in `re/tools/rulesimage.mjs`.

Implementation: `re/tools/rules/constants.mjs` (`CONSTANT_FIELDS` is this table in machine-readable form).

### 12.1 Where it is called

`Game::init_rules_data` @0058ec60 opens `rules.xml` (for the CATEGORIES lists) and as its first load step calls
`Constants::init` @00569a90, which opens `rules.xml` **again** with its own `XML` object, selects `ROOT/CONSTANTS`
(`get_element("CONSTANTS", create=1)`), stores it in `curr_element`, and reads every field in one straight-line
function (no loops other than the fixed-size `entryN` arrays). It always returns 0. It runs before `Types::init`, which
depends on it (e.g. `UnitType::init` @0061ab50 multiplies block sizes by `unit_block_radius`; `Type::sum_rules_cost`
@006680e0 uses `unit/build/tech/spell_cost_factor`).

Load order inside `Game::init_rules_data` (each step aborts the chain on failure or `quit`; splash work strings and
`NetDaemon::process_all` between steps when called with param_1 != 0):

1. `Constants::init` — rules.xml `CONSTANTS`.
2. `Categories::init` (@006f21d0: finds the `CATEGORIES` element whose `id` attribute matches, then its `CATEGORY`
   children) for, in order: `tech_cats`(4) `tech_cats`, `unit_cats`(9) `unit_cats`, `build_cats`(3) `building_cats`,
   `stances_combat`(6) `combatstances`, `stances_worker`(4) `workerstances`, `stances_caster`(2) `casterstances`,
   `stances_packer`(2) `packerstances`, `agename_cats`(8) `agenameshort`.
3. `Tribes::init(&tribes, 0, null)` @006ef430 — rules.xml `TRIBES/TRIBE` (KEY, FILE -> `TRIBES\<file>`).
4. `Categories::init`: `treaties_cats`(3) `treaties`, `team_styles`(14) `gamestyles`, `game_rules`(12) `gamerules`.
5. `Map::init_map_file_names()`.
6. `Categories::init`: `map_styles`(23) `mapstyles`, `diffs`(6) `difficulties`, `map_sizes`(7) `mapsizes`; then
   `close_coord_lookup_array()` + `init_coord_lookup_array()` (depends on map sizes).
7. `Categories::init`: `victories`(11), `eliminations`(4), `starting_towns`(4) `startingtowns`, `starting_resources`(13)
   `startingresources`, `starting_resources2`(5) `defenderresources`, `tech_costs`(9) `techcosts`,
   `starting_technologies`(10) `startingtechs`, `starting_technologies2`(5) `defendertechs`, `ending_technologies`(8)
   `endingtechs`, `reveal_maps`(4) `revealmaps`, `pop_limits`(6) `poplimits`, `rush_rules`(15) `rushrules`,
   `cannon_times`(5) `cannontimes`, `handicaps`(21), `player_slots`(12) `playerslots`, `number_of_players`(8)
   `numberofplayers`, `max_observers`(7) `maxobservers`, `game_speeds`(5) `gamespeeds`, `scores`(12),
   `time_limits`(9) `timelimits`, `chairs`(8), `wonderwins`(13), `popwins`(13), `econwins`(9), `victorytypes`(7),
   `gameinfo_flags`(12).
   (Names from step 7 on after `startingresources`/`defenderresources` are unresolved string-table offsets in the
   decompile; the ids above are matched by the declared count and document order of rules.xml `CATEGORIES id=`.)
   The second number is the max count passed to `Categories::init`.

`TECHBONUSES`, `FORMATIONS`, `LANDS` are not read here (TECHBONUSES is read by `Types::init`).

### 12.2 Parse semantics

Element lookup: `XMLNode::get_element(name, create=1, exact=0)` — name upper-cased (so the lower-case names in the
code match the upper-case XML tags), first matching child of `CONSTANTS`, a missing element is created empty.
Duplicated XML elements (the five `CTW_*` lines appear twice) resolve to the first one; both copies are identical.

| rule | function | semantics | when missing / empty |
|---|---|---|---|
| item | `Constants::get_item` @0057fa60 | `XMLElement::get_attrib_num("value", -1)` @00a27580: if the attribute text is non-empty, `String::number` → `String::convert_int` @00a15fc0 (`0b`/`0x`/`0d` prefixes, else `_wtoi`: leading digits only, trailing text such as `"% bonus"`, `"tiles"`, `"frames"` ignored). `"25%"` gives 25 (no percent scaling). | **-1** |
| frac×M | `Constants::get_fraction` @0057f950 | `String::fraction(value, M)` @00a1d110: `a = _wtoi(s)`, `d = _wtoi(after first '/')` or 1 if no '/'; result `(a*M)/d` (int32 multiply, C truncating division); `d == 0` → 0. | **0** |
| entries int | inline loop | `<NAME entry0=".." entry1=".." …/>`: attribute `"entry"+i` for i in 0..count-1, `String::convert_int` (via `String::num` @0042d9e0). Extra entries in XML are ignored (KOREAN_CITIZENS has entry8, only 8 read). | **0** per entry (null string → convert_int returns 0) |
| entries frac×256 | inline loop | same, but `String::fraction(entry_i, 256)` (only `scholar_rate`). | 0 |

Multipliers used: ×192 = distances in 1/192 tile (XML gives tiles, e.g. `3/2 tile` → 288); ×256 = 8.8 fixed-point
ratios/rates (`2/3` → 170, `1/1` → 256); ×100 = percent (`unit_rate_base`, `unit_rate_progression`, `accel_*`).

Post-processing (all int32, applied right after the read, also to the missing-default):
- `unit_block_radius = item * 48` (0x30; XML "1 UCoord" → 48). Missing → -48.
- `caravan_attack_bonus = item * 10` ("2 attack" → 20). Missing → -10.
- `americans_marine_entrench = v - v/15 + 2` (C division; magic 0x88888889 in the disassembly; "5 seconds" → 7).
  Missing → 1. Looks like a botched seconds→frames conversion, but it is what the binary does.

No other derived fields; nothing is clamped. No float math anywhere in the loader.

### 12.3 Quirks (must be reproduced)

- `capital_plunder` and `capital_plunder_assassin` are both read from the **same** string-table entry (offset 0x9b14,
  annotated as `capital_plunder_assassin`); the XML `CAPITAL_PLUNDER` element is never read. Both XML values are 500,
  so the oracle cannot distinguish; implement as both reading `CAPITAL_PLUNDER_ASSASSIN`.
- Fields whose element name is absent from the shipped rules.xml → `-1` (oracle confirms): `taj_caravan`
  (XML has `TAJ_CARAVAN_LIMIT`), `kremlin_spy_instant`, `german_light_cavalry`, `russian_uber_spies`,
  `ally_to_war_delay`, `ally_to_war_grace`.
- XML elements never read: `CAPITAL_PLUNDER`, `TAJ_CARAVAN_LIMIT`, `JAPANESE_AIRCRAFT_CARRIERS_SPEED`
  (code reads `JAPANESE_CARRIER_SPEED`), `KOREAN_START_CITIZEN`, `KOREAN_FREE_CITIZEN`.
- `aztec_move_speed` (0x574) is never read by `Constants::init` and has no other writer found → stays 0 (static
  storage; the constructor @0042dbf0 only clears `curr_element`). Oracle: 0.
- Some fields are read twice from the same element (harmless): `gems_territory_bonus`, `diamonds_commerce`,
  `mtn_{tiny,small,medium,large,huge}_{size,gather}`.
- The decompile of the TEMPLE_UPGRADE_TERR/HP/RANGE loops seems to reuse the previous element; the disassembly
  shows the element copy is inlined, so each array reads its own element (oracle confirms).
- Read order differs slightly from struct order (e.g. `CITY_UPGRADE_TERR` → `city_level_territory_bonus` is read after
  `civic_upgrade_terr`, `GEMS_TERRITORY_BONUS` early); irrelevant except for the create-on-missing side effect.

### 12.4 Writers other than Constants::init

`FieldWriteScan` over all 848 offsets via `GameAccess::constants` plus an absolute-address `InsnGrep`
(re/decomp/constants_writes/) found one real runtime writer:
`ScenarioFuncSet::set_age_diff_penalty` @00a01520 — scenario script function; clamps the age difference to 1..5 and
overwrites `one_age_down`…`five_ages_down` (used by `Balance::type_damage` @0057fb50). This changes simulation state
mid-game, so it must go through the deterministic scenario path. (`Leader::calc_resource_caps`/`calc_gather` hits
are false positives: they write Leader data.) No writer was found in `Game::init_rules_and_teams` either; the oracle
(captured after it) equals the pure XML load for all 848 ints.

### 12.5 Determinism notes

Pure integer parsing; `_wtoi` saturates on overflow (MSVC CRT), `fraction` multiplies in int32 (wraps) then divides
with truncation toward zero. Element lookup is case-sensitive on the upper-cased name (MSXML XPath). Arrays are
fixed length; missing entries are 0, not -1 (different from scalars).

### 12.6 Field table

`rule` as above; `def` = value used when the element/attribute is missing (before post-processing); `value` = value
from the shipped rules.xml (= oracle). XML note = start of the designer's `value` text (units/meaning).

| off | field | XML element | rule | def | value | XML note |
|---|---|---|---|---|---|---|
| 0x000 | unit_formation_spacing | UNIT_FORMATION_SPACING | frac×192 | 0 | 12 | 1/16 tile (calibration for unit spacing in formations) |
| 0x004 | unit_move_speed | UNIT_MOVE_SPEED | frac×192 | 0 | 1 | 1/192 tile (granularity for unit movement speeds) |
| 0x008 | unit_turn_speed | UNIT_TURN_SPEED | frac×256 | 0 | 256 | 1/1 rate (master control for unit turn speed) |
| 0x00c | unit_pack_turn_bonus | UNIT_PACK_TURN_BONUS | item | -1 | 2 | 2x (units turn faster when packed) |
| 0x010 | unit_block_radius | UNIT_BLOCK_RADIUS | item then ×48 | -1 | 48 | 1 UCoord (master calibration for unit blocking) |
| 0x014 | unit_guy_spacing | UNIT_GUY_SPACING | frac×192 | 0 | 12 | 1/16 tile (calibration for spacing within a foot unit) |
| 0x018 | unit_respond_range | UNIT_RESPOND_RANGE | item | -1 | 12 | 12 tiles: distance units normally look for enemies to attack, or wh... |
| 0x01c | unit_defensive_respond_range | UNIT_DEFENSIVE_RESPOND_RANGE | item | -1 | 4 | 4 tiles (This seems to keep people from blindly wandering into as m... |
| 0x020 | unit_guard_respond_range | UNIT_GUARD_RESPOND_RANGE | item | -1 | 8 | 8 tiles (BR 1/16/2003 -- figured this should be smaller than 12) |
| 0x024 | unit_build_respond_range | UNIT_BUILD_RESPOND_RANGE | item | -1 | 12 | 12 tiles |
| 0x028 | unit_gather_respond_range | UNIT_GATHER_RESPOND_RANGE | item | -1 | 32 | 32 tiles |
| 0x02c | aircraft_respond_range | AIRCRAFT_RESPOND_RANGE | item | -1 | 10 | 10 tiles (BR 1/16/2003 -- made this smaller so that planes don't go... |
| 0x030 | bomber_respond_range | BOMBER_RESPOND_RANGE | item | -1 | 12 | 12 tiles  |
| 0x034 | ship_defensive_respond_range | SHIP_DEFENSIVE_RESPOND_RANGE | item | -1 | 8 | 8 tiles (Ships need a better respond range so fire ships won't just... |
| 0x038 | target_radius | TARGET_RADIUS | frac×192 | 0 | 96 | 1/2 tile (calibration for target sizes) |
| 0x03c | military_transport_bonus | MILITARY_TRANSPORT_BONUS | item | -1 | 0 | 0 speed bonus for transports per military tech |
| 0x040 | range_inaccuracy | RANGE_INACCURACY | frac×256 | 0 | 2 | 1/100 -percent per # tiles |
| 0x044 | height_increment | HEIGHT_INCREMENT | item | -1 | 200 | 200 z (whatever that means, I don't think it's in TCoords) |
| 0x048 | height_bonus | HEIGHT_BONUS | item | -1 | 10 | 10% per increment |
| 0x04c | flank_bonus | FLANK_BONUS | item | -1 | 50 | 50% per level of flank (max bonus is twice this number) |
| 0x050 | cavalry_flank_bonus | CAVALRY_FLANK_BONUS | item | -1 | 40 | 40% (of base flank bonus) |
| 0x054 | vehicle_flank_bonus | VEHICLE_FLANK_BONUS | item | -1 | 33 | 33% (of base flank bonus) |
| 0x058 | rocky_modifier | ROCKY_MODIFIER | frac×256 | 0 | 170 | 2/3 (light infantry in rocks) |
| 0x05c | overkill_frames | OVERKILL_FRAMES | item | -1 | 30 | 30 (unit hit twice or more w/in this time period receives attenuate... |
| 0x060 | overkill_damage | OVERKILL_DAMAGE | frac×256 | 0 | 85 | 1/3 |
| 0x064 | entrenchment_modifier | ENTRENCHMENT_MODIFIER | frac×256 | 0 | 170 | 2/3 |
| 0x068 | river_modifier | RIVER_MODIFIER | frac×256 | 0 | 512 | 2/1 (units take more damage in rivers) |
| 0x06c | recapture_city_modifier | RECAPTURE_CITY_MODIFIER | frac×256 | 0 | 512 | 2/1 |
| 0x070 | one_age_down | ONE_AGE_DOWN | item | -1 | 15 | 15% |
| 0x074 | two_ages_down | TWO_AGES_DOWN | item | -1 | 20 | 20% |
| 0x078 | three_ages_down | THREE_AGES_DOWN | item | -1 | 50 | 50% |
| 0x07c | four_ages_down | FOUR_AGES_DOWN | item | -1 | 60 | 60% |
| 0x080 | five_ages_down | FIVE_AGES_DOWN | item | -1 | 70 | 70% |
| 0x084 | unit_train_distance | UNIT_TRAIN_DISTANCE | frac×192 | 0 | 288 | 3/2 tile |
| 0x088 | unit_train_max_distance | UNIT_TRAIN_MAX_DISTANCE | frac×192 | 0 | 480 | 5/2 tile |
| 0x08c | boat_train_distance | BOAT_TRAIN_DISTANCE | frac×192 | 0 | 288 | 3/2 tile |
| 0x090 | boat_train_max_distance | BOAT_TRAIN_MAX_DISTANCE | frac×192 | 0 | 1536 | 8 tile |
| 0x094 | boat_garrison_max_distance | BOAT_GARRISON_MAX_DISTANCE | frac×192 | 0 | 576 | 3 tile |
| 0x098 | unit_board_distance | UNIT_BOARD_DISTANCE | frac×192 | 0 | 576 | 3/1 tile |
| 0x09c | unit_disembark_distance | UNIT_DISEMBARK_DISTANCE | frac×192 | 0 | 576 | 3/1 tile |
| 0x0a0 | air_unit_mana_recharge | AIR_UNIT_MANA_RECHARGE | item | -1 | 2 | 2 craft per frame |
| 0x0a4 | max_aircraft_per_carrier | MAX_AIRCRAFT_PER_CARRIER | item | -1 | 7 | 7 |
| 0x0a8 | max_aircraft_per_airbase | MAX_AIRCRAFT_PER_AIRBASE | item | -1 | 10 | 10 |
| 0x0ac | dock_radius | DOCK_RADIUS | item | -1 | 12 | 12 tiles |
| 0x0b0 | mine_radius | MINE_RADIUS | item | -1 | 6 | 6 tiles |
| 0x0b4 | woodcutter_radius | WOODCUTTER_RADIUS | item | -1 | 8 | 8 tiles |
| 0x0b8 | fort_upgrade_terr[4] | FORT_UPGRADE_TERR | entries int | 0 | 2,4,6,9 | 2 / 4 / 6 / 9 |
| 0x0c8 | temple_upgrade_terr[5] | TEMPLE_UPGRADE_TERR | entries int | 0 | 2,4,6,9,12 | 2 / 4 / 6 / 9 / 12 |
| 0x0dc | civic_upgrade_terr[8] | CIVIC_UPGRADE_TERR | entries int | 0 | 0,1,2,4,6,8,11,14 | 0 / 1 / 2 / 4 / 6 / 8 / 11 / 14 |
| 0x0fc | capital_territory_bonus | CAPITAL_TERRITORY_BONUS | item | -1 | 6 | 6 |
| 0x100 | city_level_territory_bonus[3] | CITY_UPGRADE_TERR | entries int | 0 | 0,3,6 | 0 / 3 / 6 |
| 0x10c | fort_territory_multiplier | FORT_TERRITORY_MULTIPLIER | item | -1 | 4 | 4 |
| 0x110 | city_territory_multiplier | CITY_TERRITORY_MULTIPLIER | item | -1 | 4 | 4 |
| 0x114 | territory_base | TERRITORY_BASE | item | -1 | 24 | 24 tiles |
| 0x118 | territory_limit_base | TERRITORY_LIMIT_BASE | item | -1 | 44 | 44 tiles |
| 0x11c | territory_limit_civic | TERRITORY_LIMIT_CIVIC | item | -1 | 4 | 4 tiles |
| 0x120 | territory_limit_city | TERRITORY_LIMIT_CITY | item | -1 | 4 | 4 tiles |
| 0x124 | territory_den | TERRITORY_DEN | item | -1 | 5 | 5 |
| 0x128 | territory_num | TERRITORY_NUM | item | -1 | 11 | 11 (Numerator + (CityorFortMultiplier * BorderBonuses))/Denominator |
| 0x12c | city_center_radius | CITY_CENTER_RADIUS | item | -1 | 20 | 20 tiles |
| 0x130 | city_center_pop_radius | CITY_CENTER_POP_RADIUS | item | -1 | 4 | 4 tiles |
| 0x134 | city_capture_radius | CITY_CAPTURE_RADIUS | item | -1 | 10 | 10 tiles |
| 0x138 | city_spacing | CITY_SPACING | item | -1 | 24 | 24 Tiles |
| 0x13c | relax_city_spacing | RELAX_CITY_SPACING | item | -1 | 6 | 6 Tiles |
| 0x140 | first_city_near_coast | FIRST_CITY_NEAR_COAST | item | -1 | 16 | 16 Tiles |
| 0x144 | fort_spacing | FORT_SPACING | item | -1 | 12 | 12 Tiles |
| 0x148 | fort_to_enemy_city_spacing | FORT_TO_ENEMY_CITY_SPACING | item | -1 | 32 | 32 Tiles |
| 0x14c | city_buildings | CITY_BUILDINGS | item | -1 | 5 | 5 |
| 0x150 | metro_buildings | METRO_BUILDINGS | item | -1 | 9 | 9 |
| 0x154 | building_hp_upgrade | BUILDING_HP_UPGRADE | item | -1 | 10 | 10% per level |
| 0x158 | temple_upgrade_hp[5] | TEMPLE_UPGRADE_HP | entries int | 0 | 25,50,100,150,200 | 25% / 50% / 100% / 150% / 200% |
| 0x16c | temple_upgrade_range[5] | TEMPLE_UPGRADE_RANGE | entries int | 0 | 1,2,3,4,5 | 1 / 2 / 3 / 4 / 5 |
| 0x180 | fort_upgrade_range[5] | FORT_UPGRADE_RANGE | entries int | 0 | 0,1,2,3,4 | 0 / 1 / 2 / 3 / 4 |
| 0x194 | fort_upgrade_los[5] | FORT_UPGRADE_LOS | entries int | 0 | 0,2,4,6,8 | 0 / 2 / 4 / 6 / 8 |
| 0x1a8 | tower_fort_range[4] | TOWER_FORT_RANGE | entries int | 0 | 0,1,2,3 | 0 / 1 / 2 / 3 |
| 0x1b8 | tower_fort_los[4] | TOWER_FORT_LOS | entries int | 0 | 0,2,4,6 | 0 / 2 / 4 / 6 |
| 0x1c8 | attrition_upgrade[4] | ATTRITION_UPGRADE | entries int | 0 | 25,50,75,100 | 25% (decrease from Forage) / 50% (total net decrease from Supply) /... |
| 0x1d8 | attrition_improved[4] | ATTRITION_IMPROVED | entries int | 0 | 1,2,4,8 | 1 / 2 / 4 / 8 |
| 0x1e8 | siege_attrition | SIEGE_ATTRITION | item | -1 | 50 | 50% reduction |
| 0x1ec | militia_attrition | MILITIA_ATTRITION | item | -1 | 300 | 300% increase |
| 0x1f0 | attrition_aged_up | ATTRITION_AGED_UP | item | -1 | 25 | 25% increase |
| 0x1f4 | troops_upgrade_los | TROOPS_UPGRADE_LOS | item | -1 | 2 | 2 |
| 0x1f8 | spy_upgrade_los | SPY_UPGRADE_LOS | item | -1 | 2 | 2 |
| 0x1fc | spy_bribe_upgrade_range | SPY_BRIBE_UPGRADE_RANGE | item | -1 | 2 | 2 |
| 0x200 | spy_informer_upgrade_range | SPY_INFORMER_UPGRADE_RANGE | item | -1 | 3 | 3 |
| 0x204 | spy_upgrade_hp[4] | SPY_UPGRADE_HP | entries int | 0 | 15,45,90,150 | 15 / 45 / 90 / 150 |
| 0x214 | general_upgrade_los | GENERAL_UPGRADE_LOS | item | -1 | 2 | 2 |
| 0x218 | spy_general_cost | SPY_GENERAL_COST | item | -1 | 50 | 50% with bonus |
| 0x21c | jam_unit_radar_prob | JAM_UNIT_RADAR_PROB | item | -1 | 50 | 50% |
| 0x220 | unit_rate_base | UNIT_RATE_BASE | frac×100 | 0 | 120 | 6/5 base rate (See BR before adjusting) |
| 0x224 | unit_rate_progression | UNIT_RATE_PROGRESSION | frac×100 | 0 | 75 | 3/4 progression (See BR before adjusting) |
| 0x228 | accel_train | ACCEL_TRAIN | frac×100 | 0 | 100 | 1/1 speed (Please don't use except on your own machine for testing;... |
| 0x22c | accel_construct | ACCEL_CONSTRUCT | frac×100 | 0 | 100 | 1/1 speed (Please don't use except on your own machine for testing;... |
| 0x230 | accel_research | ACCEL_RESEARCH | frac×100 | 0 | 100 | 1/1 speed (Please don't use except on your own machine for testing;... |
| 0x234 | starting_goods[6] | STARTING_GOODS | entries int | 0 | 200,200,100,100,100,100 | 200food / 200timb / 100gd / 100know / 100met / 100oil |
| 0x24c | basic_gather[6] | BASIC_GATHER | entries int | 0 | 0,0,0,0,0,0 | 0food / 0timb / 0gd / 0know / 0met / 0oil |
| 0x264 | city_gather[6] | CITY_GATHER | entries int | 0 | 10,10,0,0,0,0 | 10food / 10timb / 0gd / 0know / 0met / 0oil |
| 0x27c | gather_rate | GATHER_RATE | item | -1 | 450 | 450 frames |
| 0x280 | peasant_rate | PEASANT_RATE | frac×256 | 0 | 2560 | 10 resources |
| 0x284 | scholar_rate[6] | SCHOLAR_RATE | entries frac×256 | 0 | 1280,1792,2560,3840,5120,6400 | 5 / 7 / 10 / 15 / 20 / 25 |
| 0x29c | oil_rate | OIL_RATE | frac×256 | 0 | 8960 | 35 oil |
| 0x2a0 | farms_per_city_base | FARMS_PER_CITY_BASE | item | -1 | 5 | 5 |
| 0x2a4 | farms_per_city_level | FARMS_PER_CITY_LEVEL | item | -1 | 0 | 0 |
| 0x2a8 | fishermen_bonus[5] | FISHERMEN_BONUS | entries int | 0 | 0,50,100,200,200 | 0% / 50% / 100% / 200% / 200% |
| 0x2bc | granary_bonus[5] | GRANARY_BONUS | entries int | 0 | 20,50,100,200,250 | 20% / 50% / 100% / 200% / 250% |
| 0x2d0 | lumbermill_bonus[5] | LUMBERMILL_BONUS | entries int | 0 | 20,50,100,200,250 | 20% / 50% / 100% / 200% / 250% |
| 0x2e4 | smelter_bonus[5] | SMELTER_BONUS | entries int | 0 | 50,100,150,200,250 | 50% / 100% / 150% / 200% / 250% |
| 0x2f8 | refinery_bonus | REFINERY_BONUS | item | -1 | 33 | 33% per refinery |
| 0x2fc | base_tribute | BASE_TRIBUTE | item | -1 | 51 | 51% of gift gets through |
| 0x300 | commerce_tribute | COMMERCE_TRIBUTE | item | -1 | 7 | 7% additional gift gets through |
| 0x304 | city_plunder_per_level | CITY_PLUNDER_PER_LEVEL | item | -1 | 100 | 100 |
| 0x308 | capital_plunder | CAPITAL_PLUNDER_ASSASSIN | item | -1 | 500 | 500 (times number of players eliminated so far) |
| 0x30c | capital_plunder_assassin | CAPITAL_PLUNDER_ASSASSIN | item | -1 | 500 | 500 (times number of players eliminated so far) |
| 0x310 | merchants_bonus[5] | MERCHANTS_BONUS | entries int | 0 | 100,120,150,200,300 | 100% / 120% / 150% / 200% / 300% |
| 0x324 | village_taxes | VILLAGE_TAXES | item | -1 | 0 | 0 bonus wealth |
| 0x328 | building_taxes | BUILDING_TAXES | item | -1 | 0 | 0 bonus wealth (per building in village/city) |
| 0x32c | market_taxes | MARKET_TAXES | item | -1 | 10 | 10 bonus wealth (+ amount above for just being a building) |
| 0x330 | temple_taxes | TEMPLE_TAXES | item | -1 | 0 | 0 bonus wealth (+ amount above for just being a building) |
| 0x334 | territory_taxes[5] | TERRITORY_TAXES | entries int | 0 | 0,50,100,200,300 | 0% / 50% / 100% / 200% / 300% |
| 0x348 | village_literacy | VILLAGE_LITERACY | item | -1 | 0 | 0 bonus knowledge |
| 0x34c | university_literacy | UNIVERSITY_LITERACY | item | -1 | 10 | 10 bonus knowledge |
| 0x350 | library_literacy | LIBRARY_LITERACY | item | -1 | 0 | 0 bonus knowledge |
| 0x354 | unit_cost_factor | UNIT_COST_FACTOR | item | -1 | 10 | 10 resources |
| 0x358 | build_cost_factor | BUILD_COST_FACTOR | item | -1 | 10 | 10 resources |
| 0x35c | tech_cost_factor | TECH_COST_FACTOR | item | -1 | 10 | 10 resources |
| 0x360 | spell_cost_factor | SPELL_COST_FACTOR | item | -1 | 10 | 10 resources |
| 0x364 | tech_science_discount | TECH_SCIENCE_DISCOUNT | item | -1 | 10 | 10 percent |
| 0x368 | tech_science_speedup | TECH_SCIENCE_SPEEDUP | item | -1 | 10 | 10 percent |
| 0x36c | tech_age_behind_discount | TECH_AGE_BEHIND_DISCOUNT | item | -1 | 10 | 10 percent |
| 0x370 | tech_age_behind_knowledge_discount | TECH_AGE_BEHIND_KNOWLEDGE_DISCOUNT | item | -1 | 20 | 20 percent |
| 0x374 | tech_color_behind_discount | TECH_COLOR_BEHIND_DISCOUNT | item | -1 | 10 | 10 percent |
| 0x378 | tech_color_behind_knowledge_discount | TECH_COLOR_BEHIND_KNOWLEDGE_DISCOUNT | item | -1 | 20 | 20 percent |
| 0x37c | build_support_factor | BUILD_SUPPORT_FACTOR | item | -1 | 1 | 1 resources |
| 0x380 | disband_city_rate | DISBAND_CITY_RATE | item | -1 | 400 | 400% normal raze time for a building |
| 0x384 | disband_senate_rate | DISBAND_SENATE_RATE | item | -1 | 1600 | 1600% normal raze time for a building |
| 0x388 | military_upgrade_discount | MILITARY_UPGRADE_DISCOUNT | item | -1 | 10 | 10% per level ahead |
| 0x38c | military_unit_discount | MILITARY_UNIT_DISCOUNT | item | -1 | 5 | 5% per level ahead |
| 0x390 | unit_refit_max_cost | UNIT_REFIT_MAX_COST | item | -1 | 40 | 40 delta per resource type per unit |
| 0x394 | unit_scholar_ramp_max | UNIT_SCHOLAR_RAMP_MAX | item | -1 | 2000 | 2000% additional ramping cost max |
| 0x398 | unit_worker_ramp_max | UNIT_WORKER_RAMP_MAX | item | -1 | 500 | 500% additional ramping cost max |
| 0x39c | unit_other_civilian_ramp_max | UNIT_OTHER_CIVILIAN_RAMP_MAX | item | -1 | 200 | 200% additional ramping cost max |
| 0x3a0 | unit_military_ramp_max | UNIT_MILITARY_RAMP_MAX | item | -1 | 125 | 125% additional ramping cost max |
| 0x3a4 | research_premium | RESEARCH_PREMIUM | frac×256 | 0 | 256 | 1/1 times base cost (Please don't use except on your own machine fo... |
| 0x3a8 | research_tick_premium | RESEARCH_TICK_PREMIUM | frac×256 | 0 | 256 | 1/1 times base time (Please don't use except on your own machine fo... |
| 0x3ac | ramp_final | RAMP_FINAL | item | -1 | 50 | 50% additional knowledge cost per final tech already researched |
| 0x3b0 | plunder | PLUNDER | item | -1 | 100 | 100% of raze value |
| 0x3b4 | caravan_plunder | CARAVAN_PLUNDER | item | -1 | 50 | 50 |
| 0x3b8 | general_plunder | GENERAL_PLUNDER | item | -1 | 100 | 100 |
| 0x3bc | capital_build_time | CAPITAL_BUILD_TIME | item | -1 | 300 | 300% of normal city build time (this is for nomad games only) |
| 0x3c0 | min_pop_limit | MIN_POP_LIMIT | item | -1 | 0 | 0 pop cap |
| 0x3c4 | pop_cap[8] | POP_CAP | entries int | 0 | 25,50,75,100,125,150,175,200 | 25 / 50 / 75 / 100 / 125 / 150 / 175 / 200 pop cap |
| 0x3e4 | military_pop | MILITARY_POP | item | -1 | 0 | 0 pop cap |
| 0x3e8 | village_pop | VILLAGE_POP | item | -1 | 0 | 0 pop cap |
| 0x3ec | granary_pop | GRANARY_POP | item | -1 | 0 | 0 pop cap |
| 0x3f0 | max_pop_limit | MAX_POP_LIMIT | item | -1 | 300 | 300 pop cap |
| 0x3f4 | commando_min_damage | COMMANDO_MIN_DAMAGE | item | -1 | 200 | 200 |
| 0x3f8 | special_min_damage | SPECIAL_MIN_DAMAGE | item | -1 | 400 | 400 |
| 0x3fc | elite_min_damage | ELITE_MIN_DAMAGE | item | -1 | 800 | 800 |
| 0x400 | commerce_cap[8] | COMMERCE_CAP | entries int | 0 | 70,100,150,200,260,320,400,500 | 70 / 100 / 150 / 200 / 260 / 320 / 400 / 500 commerce cap |
| 0x420 | lumber_commerce | LUMBER_COMMERCE | item | -1 | 10 | 10 commerce cap |
| 0x424 | global_prosperity | GLOBAL_PROSPERITY | item | -1 | 25 | 25% |
| 0x428 | pyramids_commerce | PYRAMIDS_COMMERCE | item | -1 | 50 | 50 commerce cap |
| 0x42c | pyramids_city_limit | PYRAMIDS_CITY_LIMIT | item | -1 | 1 | 1 city |
| 0x430 | pyramids_food | PYRAMIDS_FOOD | item | -1 | 20 | 20% increase |
| 0x434 | pyramids_city_discount | PYRAMIDS_CITY_DISCOUNT | item | -1 | 33 | 33% cheaper |
| 0x438 | colossus_wealth | COLOSSUS_WEALTH | item | -1 | 30 | 30% increase |
| 0x43c | colossus_pop_cap | COLOSSUS_POP_CAP | item | -1 | 50 | 50 |
| 0x440 | colossus_caravan | COLOSSUS_CARAVAN | item | -1 | 0 | 0 |
| 0x444 | colossus_commerce | COLOSSUS_COMMERCE | item | -1 | 50 | 50 commerce cap |
| 0x448 | hanging_gardens_knowledge | HANGING_GARDENS_KNOWLEDGE | item | -1 | 50 | 50 |
| 0x44c | hanging_gardens_build_time | HANGING_GARDENS_BUILD_TIME | item | -1 | 0 | 0% faster construction times |
| 0x450 | hanging_gardens_upgrades | HANGING_GARDENS_UPGRADES | item | -1 | 0 | 0 (off) for enhancer upgrades free |
| 0x454 | hanging_gardens_industry_discount | HANGING_GARDENS_INDUSTRY_DISCOUNT | item | -1 | 66 | 66% cheaper enhancer upgrades |
| 0x458 | terra_cotta_attack | TERRA_COTTA_ATTACK | item | -1 | 0 | 0 |
| 0x45c | terra_cotta_range | TERRA_COTTA_RANGE | item | -1 | 0 | 0 |
| 0x460 | terra_cotta_cost | TERRA_COTTA_COST | item | -1 | 0 | 0% reduction |
| 0x464 | terra_cotta_free | TERRA_COTTA_FREE | item | -1 | 30 | 30 seconds |
| 0x468 | terra_cotta_delay | TERRA_COTTA_DELAY | item | -1 | 7 | 7 fifteenths of a second (but 7 == "half a second") |
| 0x46c | colosseum_territory_bonus | COLOSSEUM_TERRITORY_BONUS | item | -1 | 3 | 3 |
| 0x470 | colosseum_attrition | COLOSSEUM_ATTRITION | item | -1 | 50 | 50% increase to attrition caused |
| 0x474 | colosseum_fort_range | COLOSSEUM_FORT_RANGE | item | -1 | 0 | 0 |
| 0x478 | colosseum_fort_borders | COLOSSEUM_FORT_BORDERS | item | -1 | 0 | 0 |
| 0x47c | colosseum_fort_upgrades | COLOSSEUM_FORT_UPGRADES | item | -1 | 0 | 0 |
| 0x480 | colosseum_fort_cost | COLOSSEUM_FORT_COST | item | -1 | 20 | 20% cheaper |
| 0x484 | colosseum_barracks_cost | COLOSSEUM_BARRACKS_COST | item | -1 | 0 | 0% cheaper |
| 0x488 | forbidden_city_gather | FORBIDDEN_CITY_GATHER | item | -1 | 25 | 25% bonus |
| 0x48c | forbidden_city_base_gather | FORBIDDEN_CITY_BASE_GATHER | item | -1 | 50 | 50 resources |
| 0x490 | tikal_timber | TIKAL_TIMBER | item | -1 | 50 | 50% income bonus |
| 0x494 | tikal_timber_commerce | TIKAL_TIMBER_COMMERCE | item | -1 | 100 | 100 commerce cap bonus |
| 0x498 | tikal_temple_borders | TIKAL_TEMPLE_BORDERS | item | -1 | 50 | 50% |
| 0x49c | tikal_temple_range | TIKAL_TEMPLE_RANGE | item | -1 | 50 | 50% |
| 0x4a0 | tikal_temple_hp | TIKAL_TEMPLE_HP | item | -1 | 50 | 50% |
| 0x4a4 | tikal_temple_cost | TIKAL_TEMPLE_COST | item | -1 | 0 | 0% cost reduction |
| 0x4a8 | tikal_temple_upgrades | TIKAL_TEMPLE_UPGRADES | item | -1 | 0 | 0 (1 = enable; 0 = disable) |
| 0x4ac | porcelain_ships | PORCELAIN_SHIPS | item | -1 | 50 | 50% creation speed |
| 0x4b0 | porcelain_rare | PORCELAIN_RARE | item | -1 | 200 | 200% income |
| 0x4b4 | porcelain_market | PORCELAIN_MARKET | item | -1 | 300 | 300% bonus wealth |
| 0x4b8 | red_fort_heal | RED_FORT_HEAL | item | -1 | 500 | 500% faster |
| 0x4bc | red_fort_fortification | RED_FORT_FORTIFICATION | item | -1 | 1 | 1 |
| 0x4c0 | red_fort_tactics | RED_FORT_TACTICS | item | -1 | 1 | 1 |
| 0x4c4 | red_fort_air_defense | RED_FORT_AIR_DEFENSE | item | -1 | 33 | 33% less damage |
| 0x4c8 | red_fort_fort_hps | RED_FORT_FORT_HPS | item | -1 | 33 | 33% extra hit points for all other forts |
| 0x4cc | angkor_research | ANGKOR_RESEARCH | item | -1 | 0 | 0%  |
| 0x4d0 | angkor_metal | ANGKOR_METAL | item | -1 | 50 | 50% income bonus |
| 0x4d4 | angkor_metal_commerce | ANGKOR_METAL_COMMERCE | item | -1 | 100 | 100 commerce cap bonus |
| 0x4d8 | angkor_ships | ANGKOR_SHIPS | item | -1 | 25 | 25% cost reduction |
| 0x4dc | versailles_knowledge_costs | VERSAILLES_KNOWLEDGE_COSTS | item | -1 | 50 | 50% decrease |
| 0x4e0 | versailles_units_move | VERSAILLES_UNITS_MOVE | item | -1 | 25 | 25% faster |
| 0x4e4 | versailles_building_speed | VERSAILLES_BUILDING_SPEED | item | -1 | 0 | 0% faster |
| 0x4e8 | versailles_supply_heal_rate | VERSAILLES_SUPPLY_HEAL_RATE | item | -1 | 20 | 20 frames |
| 0x4ec | versailles_generals_no_knowledge | VERSAILLES_GENERALS_NO_KNOWLEDGE | item | -1 | 0 | 0 (1 = enable, 0 = disable) |
| 0x4f0 | versailles_affects_university_upgrades | VERSAILLES_AFFECTS_UNIVERSITY_UPGRADES | item | -1 | 0 | 0 (0 = no; 1 = research discount affects university upgrades) |
| 0x4f4 | versailles_affects_misc_upgrades | VERSAILLES_AFFECTS_MISC_UPGRADES | item | -1 | 0 | 0 (0 = no; 1 = research discount affects non-Library tech other tha... |
| 0x4f8 | taj_building_hp | TAJ_BUILDING_HP | item | -1 | 100 | 100% bonus |
| 0x4fc | taj_wealth | TAJ_WEALTH | item | -1 | 100 | 100% income bonus |
| 0x500 | taj_wealth_commerce | TAJ_WEALTH_COMMERCE | item | -1 | 300 | 300 commerce cap bonus |
| 0x504 | taj_caravan | TAJ_CARAVAN | item | -1 | -1 | **missing in XML** |
| 0x508 | taj_farms | TAJ_FARMS | item | -1 | 0 | 0 extra |
| 0x50c | kremlin_commerce | KREMLIN_COMMERCE | item | -1 | 200 | 200 commerce cap bonus |
| 0x510 | kremlin_attrition | KREMLIN_ATTRITION | item | -1 | 100 | 100% increase to attrition caused |
| 0x514 | kremlin_spy | KREMLIN_SPY | item | -1 | 30 | 30 (0 = disable; non-zero = how many seconds to respawn) |
| 0x518 | kremlin_farms | KREMLIN_FARMS | item | -1 | 0 | 0 extra |
| 0x51c | kremlin_spy_instant | KREMLIN_SPY_INSTANT | item | -1 | -1 | **missing in XML** |
| 0x520 | liberty_units | LIBERTY_UNITS | item | -1 | 0 | 0% faster creation |
| 0x524 | liberty_attrition | LIBERTY_ATTRITION | item | -1 | 100 | 100% reduction of attrition received |
| 0x528 | liberty_siege_range | LIBERTY_SIEGE_RANGE | item | -1 | 0 | 0 |
| 0x52c | liberty_bombers | LIBERTY_BOMBERS | item | -1 | 33 | 33% cheaper |
| 0x530 | liberty_missiles | LIBERTY_MISSILES | item | -1 | 0 | 0% cheaper |
| 0x534 | liberty_free_upgrades | LIBERTY_FREE_UPGRADES | item | -1 | 1 | 1 (1 = enable; 0 = disable) |
| 0x538 | liberty_air_defense | LIBERTY_AIR_DEFENSE | item | -1 | 33 | 33% cheaper |
| 0x53c | eiffel_tower_territory_bonus | EIFFEL_TOWER_TERRITORY_BONUS | item | -1 | 6 | 6 |
| 0x540 | eiffel_oil | EIFFEL_OIL | item | -1 | 100 | 100% income bonus |
| 0x544 | eiffel_oil_commerce | EIFFEL_OIL_COMMERCE | item | -1 | 200 | 200 commerce cap bonus |
| 0x548 | eiffel_siege_range | EIFFEL_SIEGE_RANGE | item | -1 | 0 | 0 |
| 0x54c | super_buy | SUPER_BUY | item | -1 | 125 | 125 is maximum buy price |
| 0x550 | super_sell | SUPER_SELL | item | -1 | 50 | 50 is minimum sell price |
| 0x554 | super_missiles | SUPER_MISSILES | item | -1 | 0 | 0% cost increase to enemy missiles |
| 0x558 | super_immune | SUPER_IMMUNE | item | -1 | 0 | 0 (1 = enable; 0 = disable) |
| 0x55c | space_air_range | SPACE_AIR_RANGE | item | -1 | 0 | 0% bonus |
| 0x560 | space_air_attack | SPACE_AIR_ATTACK | item | -1 | 0 | 0% bonus |
| 0x564 | space_air_speed | SPACE_AIR_SPEED | item | -1 | 100 | 100% bonus |
| 0x568 | space_air_cost | SPACE_AIR_COST | item | -1 | 50 | 50% cheaper |
| 0x56c | aztec_slay_bonus | AZTEC_SLAY_BONUS | item | -1 | 15 | 15 resources |
| 0x570 | aztec_slay_cap | AZTEC_SLAY_CAP | item | -1 | 60 | 60 resources |
| 0x574 | aztec_move_speed | — | not read | 0 | 0 |  |
| 0x578 | aztec_plunder | AZTEC_PLUNDER | item | -1 | 100 | 100% |
| 0x57c | aztec_barracks_light | AZTEC_BARRACKS_LIGHT | item | -1 | 1 | 1 |
| 0x580 | aztec_max_light | AZTEC_MAX_LIGHT | item | -1 | 3 | 3 |
| 0x584 | aztec_free_military | AZTEC_FREE_MILITARY | item | -1 | 1 | 1 |
| 0x588 | maya_garrison_arrows | MAYA_GARRISON_ARROWS | item | -1 | 1 | 1 |
| 0x58c | maya_building_hp | MAYA_BUILDING_HP | item | -1 | 25 | 25% bonus |
| 0x590 | maya_building_speed | MAYA_BUILDING_SPEED | item | -1 | 20 | 20% bonus |
| 0x594 | maya_building_timber | MAYA_BUILDING_TIMBER | item | -1 | 25 | 25% reduction |
| 0x598 | inca_wealth_cap | INCA_WEALTH_CAP | item | -1 | 33 | 33% bonus |
| 0x59c | inca_wealth_per_miner | INCA_WEALTH_PER_MINER | item | -1 | 10 | 10 |
| 0x5a0 | inca_refund | INCA_REFUND | item | -1 | 25 | 25% |
| 0x5a4 | bantu_city_limit | BANTU_CITY_LIMIT | item | -1 | 1 | 1 |
| 0x5a8 | bantu_pop_cap | BANTU_POP_CAP | item | -1 | 100 | 100% |
| 0x5ac | bantu_unit_upgrades | BANTU_UNIT_UPGRADES | item | -1 | 1 | 1 (1 = enabled, 0 = disabled) |
| 0x5b0 | bantu_final_pop_cap | BANTU_FINAL_POP_CAP | item | -1 | 25 | 25% |
| 0x5b4 | bantu_units_move | BANTU_UNITS_MOVE | item | -1 | 25 | 25% faster |
| 0x5b8 | bantu_city_cost | BANTU_CITY_COST | item | -1 | 75 | 75% cheaper |
| 0x5bc | nubian_rare | NUBIAN_RARE | item | -1 | 50 | 50% resource bonus |
| 0x5c0 | nubian_caravan_limit | NUBIAN_CARAVAN_LIMIT | item | -1 | 1 | 1 |
| 0x5c4 | nubian_free_caravan | NUBIAN_FREE_CARAVAN | item | -1 | 0 | 0 (1 = enabled, 0 = disabled) |
| 0x5c8 | nubian_merchants | NUBIAN_MERCHANTS | item | -1 | 10 | 10% cheaper |
| 0x5cc | nubian_hit_points | NUBIAN_HIT_POINTS | item | -1 | 50 | 50% more |
| 0x5d0 | nubian_market_prices | NUBIAN_MARKET_PRICES | item | -1 | 20 | 20 |
| 0x5d4 | greek_research_speed | GREEK_RESEARCH_SPEED | item | -1 | 100 | 100% bonus |
| 0x5d8 | greek_research_cost | GREEK_RESEARCH_COST | item | -1 | 10 | 10% cost reduction |
| 0x5dc | greek_knowledge_early | GREEK_KNOWLEDGE_EARLY | item | -1 | 1 | 1 (1 = enabled, 0 = disabled) |
| 0x5e0 | greek_university_early | GREEK_UNIVERSITY_EARLY | item | -1 | 2 | 2 (2 = start with one, 1 = enabled, 0 = disabled) |
| 0x5e4 | greek_start_scholars | GREEK_START_SCHOLARS | item | -1 | 0 | 0 |
| 0x5e8 | greek_age_knowledge | GREEK_AGE_KNOWLEDGE | item | -1 | 0 | 0% of food cost of all converted |
| 0x5ec | greek_classical | GREEK_CLASSICAL | item | -1 | 0 | 0% of food cost converted |
| 0x5f0 | greek_library_cost | GREEK_LIBRARY_COST | item | -1 | 66 | 66% discount |
| 0x5f4 | greek_scholars_fast | GREEK_SCHOLARS_FAST | item | -1 | 0 | 0% faster |
| 0x5f8 | greek_delay_knowledge | GREEK_DELAY_KNOWLEDGE | item | -1 | 1 | 1 (0 = get normal starting knowledge; 1 = only get starting knowled... |
| 0x5fc | roman_fort_hp | ROMAN_FORT_HP | item | -1 | 0 | 0% bonus |
| 0x600 | roman_fort_range | ROMAN_FORT_RANGE | item | -1 | 0 | 0 |
| 0x604 | roman_fort_borders | ROMAN_FORT_BORDERS | item | -1 | 3 | 3 |
| 0x608 | roman_fort_early | ROMAN_FORT_EARLY | item | -1 | 1 | 1 (1 = enabled, 0 = disabled) |
| 0x60c | roman_fort_cost | ROMAN_FORT_COST | item | -1 | 25 | 25% cost reduction |
| 0x610 | roman_fort_speed | ROMAN_FORT_SPEED | item | -1 | 50 | 50% faster |
| 0x614 | roman_fort_upgrades | ROMAN_FORT_UPGRADES | item | -1 | 1 | 1 |
| 0x618 | roman_barracks_legion | ROMAN_BARRACKS_LEGION | item | -1 | 1 | 1 |
| 0x61c | roman_max_legion | ROMAN_MAX_LEGION | item | -1 | 3 | 3 |
| 0x620 | roman_city_gather | ROMAN_CITY_GATHER | item | -1 | 10 | 10 |
| 0x624 | roman_age_for_1_legion | ROMAN_AGE_FOR_1_LEGION | item | -1 | 1 | 1 |
| 0x628 | roman_age_for_2_legions | ROMAN_AGE_FOR_2_LEGIONS | item | -1 | 3 | 3 |
| 0x62c | roman_age_for_3_legions | ROMAN_AGE_FOR_3_LEGIONS | item | -1 | 5 | 5 |
| 0x630 | roman_legion_cheap | ROMAN_LEGION_CHEAP | item | -1 | 10 | 10% cost reduction |
| 0x634 | roman_legion_fast | ROMAN_LEGION_FAST | item | -1 | 10 | 10% faster creation speed |
| 0x638 | roman_free_military | ROMAN_FREE_MILITARY | item | -1 | 1 | 1 |
| 0x63c | egyptian_wonders | EGYPTIAN_WONDERS | item | -1 | 25 | 25% cost reduction |
| 0x640 | egyptian_granary_early | EGYPTIAN_GRANARY_EARLY | item | -1 | 2 | 2 (2 = enabled and start with one, 1 = enabled, 0 = disabled) |
| 0x644 | egyptian_granary_upgrades | EGYPTIAN_GRANARY_UPGRADES | item | -1 | 1 | 1 (1 = enabled, 0 = disabled) |
| 0x648 | egyptian_scholars_food | EGYPTIAN_SCHOLARS_FOOD | item | -1 | 0 | 0 (1 = enabled, 0 = disabled) |
| 0x64c | egyptian_wonders_early | EGYPTIAN_WONDERS_EARLY | item | -1 | 1 | 1 (1 = enabled, 0 = disabled) |
| 0x650 | egyptian_farms_per_city_base | EGYPTIAN_FARMS_PER_CITY_BASE | item | -1 | 7 | 7 |
| 0x654 | egyptian_food_commerce | EGYPTIAN_FOOD_COMMERCE | item | -1 | 10 | 10% |
| 0x658 | egyptian_farm_wealth | EGYPTIAN_FARM_WEALTH | item | -1 | 2 | 2 |
| 0x65c | turk_siege_range | TURK_SIEGE_RANGE | item | -1 | 3 | 3 |
| 0x660 | turk_siege_los | TURK_SIEGE_LOS | item | -1 | 3 | 3 |
| 0x664 | turk_assimilate | TURK_ASSIMILATE | item | -1 | 200 | 200% faster |
| 0x668 | turk_free_siege | TURK_FREE_SIEGE | item | -1 | 2 | 2 |
| 0x66c | turk_free_siege_upgrades | TURK_FREE_SIEGE_UPGRADES | item | -1 | 1 | 1 |
| 0x670 | turk_pack | TURK_PACK | item | -1 | 100 | 100% of normal time |
| 0x674 | turk_military_cheap | TURK_MILITARY_CHEAP | item | -1 | 33 | 33% reduction |
| 0x678 | turk_citizen | TURK_CITIZEN | item | -1 | 33 | 33% cheaper |
| 0x67c | spanish_los | SPANISH_LOS | item | -1 | 0 | 0 |
| 0x680 | spanish_extra_scout | SPANISH_EXTRA_SCOUT | item | -1 | 1 | 1 |
| 0x684 | spanish_free_trireme | SPANISH_FREE_TRIREME | item | -1 | 1 | 1 |
| 0x688 | spanish_ruins_base | SPANISH_RUINS_BASE | item | -1 | 30 | 30 |
| 0x68c | spanish_ruins | SPANISH_RUINS | item | -1 | 26 | 26 |
| 0x690 | spanish_commandos | SPANISH_COMMANDOS | item | -1 | 1 | 1 |
| 0x694 | spanish_scout_upgrades | SPANISH_SCOUT_UPGRADES | item | -1 | 1 | 1 |
| 0x698 | french_supply_heal_rate | FRENCH_SUPPLY_HEAL_RATE | item | -1 | 20 | 20 frames |
| 0x69c | french_special_craft | FRENCH_SPECIAL_CRAFT | item | -1 | 0 | 0% bonus |
| 0x6a0 | french_special_cost | FRENCH_SPECIAL_COST | item | -1 | 0 | 0% reduction |
| 0x6a4 | french_special_speed | FRENCH_SPECIAL_SPEED | item | -1 | 0 | 0% faster creation |
| 0x6a8 | french_siege_cost | FRENCH_SIEGE_COST | item | -1 | 15 | 15% cost reduction |
| 0x6ac | french_siege_speed | FRENCH_SIEGE_SPEED | item | -1 | 25 | 25% faster |
| 0x6b0 | french_free_general | FRENCH_FREE_GENERAL | item | -1 | 1 | 1 (1 = enabled, 0 = disabled) |
| 0x6b4 | french_free_supply | FRENCH_FREE_SUPPLY | item | -1 | 1 | 1 (1 = enabled, 0 = disabled) |
| 0x6b8 | french_entrench | FRENCH_ENTRENCH | item | -1 | 100 | 100% of usual time. |
| 0x6bc | french_siege_move | FRENCH_SIEGE_MOVE | item | -1 | 20 | 20% faster |
| 0x6c0 | french_lumbermill_early | FRENCH_LUMBERMILL_EARLY | item | -1 | 2 | 2 |
| 0x6c4 | french_lumbermill_upgrades | FRENCH_LUMBERMILL_UPGRADES | item | -1 | 1 | 1 |
| 0x6c8 | french_woodies | FRENCH_WOODIES | item | -1 | 1 | 1 |
| 0x6cc | french_timber_commerce | FRENCH_TIMBER_COMMERCE | item | -1 | 10 | 10% bonus |
| 0x6d0 | british_commerce | BRITISH_COMMERCE | item | -1 | 25 | 25% bonus |
| 0x6d4 | british_ship_speed | BRITISH_SHIP_SPEED | item | -1 | 33 | 33% faster creation |
| 0x6d8 | british_archer_speed | BRITISH_ARCHER_SPEED | item | -1 | 0 | 0% faster creation |
| 0x6dc | british_archer_upgrades | BRITISH_ARCHER_UPGRADES | item | -1 | 1 | 1 (1 = enabled, 0 = disabled) |
| 0x6e0 | british_aa_cost | BRITISH_AA_COST | item | -1 | 25 | 25% cost reduction |
| 0x6e4 | british_aa_speed | BRITISH_AA_SPEED | item | -1 | 33 | 33% faster creation |
| 0x6e8 | british_free_ship | BRITISH_FREE_SHIP | item | -1 | 0 | 0 |
| 0x6ec | british_taxation | BRITISH_TAXATION | item | -1 | 100 | 100% increase |
| 0x6f0 | british_tower_range | BRITISH_TOWER_RANGE | item | -1 | 2 | 2 range increase |
| 0x6f4 | british_age_for_1_archer | BRITISH_AGE_FOR_1_ARCHER | item | -1 | 0 | 0 |
| 0x6f8 | british_age_for_2_archers | BRITISH_AGE_FOR_2_ARCHERS | item | -1 | 2 | 2 |
| 0x6fc | british_age_for_3_archers | BRITISH_AGE_FOR_3_ARCHERS | item | -1 | 3 | 3 |
| 0x700 | british_taxation_discount | BRITISH_TAXATION_DISCOUNT | item | -1 | 50 | 50% cheaper |
| 0x704 | british_free_fishermen | BRITISH_FREE_FISHERMEN | item | -1 | 0 | 0 |
| 0x708 | german_submarine_cost | GERMAN_SUBMARINE_COST | item | -1 | 25 | 25% cost reduction |
| 0x70c | german_submarine_speed | GERMAN_SUBMARINE_SPEED | item | -1 | 33 | 33% faster creation |
| 0x710 | german_air_speed | GERMAN_AIR_SPEED | item | -1 | 33 | 33% faster creation |
| 0x714 | german_metal_early | GERMAN_METAL_EARLY | item | -1 | 0 | 0 (1 = enabled, 0 = disabled) |
| 0x718 | german_metal_upgrades | GERMAN_METAL_UPGRADES | item | -1 | 0 | 0 (1 = enabled, 0 = disabled) |
| 0x71c | german_free_fighter | GERMAN_FREE_FIGHTER | item | -1 | 2 | 2  |
| 0x720 | german_free_bomber | GERMAN_FREE_BOMBER | item | -1 | 0 | 0 |
| 0x724 | german_mine_cost | GERMAN_MINE_COST | item | -1 | 0 | 0% cheaper |
| 0x728 | german_miners | GERMAN_MINERS | item | -1 | 0 | 0 |
| 0x72c | german_heavy_infantry | GERMAN_HEAVY_INFANTRY | item | -1 | 0 | 0 (1 = enabled; 0 = disabled) |
| 0x730 | german_light_cavalry | GERMAN_LIGHT_CAVALRY | item | -1 | -1 | **missing in XML** |
| 0x734 | german_industry_upgrades | GERMAN_INDUSTRY_UPGRADES | item | -1 | 0 | 0 (1 = enabled; 0 = disabled) (this is the completely free version) |
| 0x738 | german_industry_discount | GERMAN_INDUSTRY_DISCOUNT | item | -1 | 50 | 50% discount |
| 0x73c | german_completion_bonus | GERMAN_COMPLETION_BONUS | item | -1 | 50 | 50% extra resources for building completion bonuses |
| 0x740 | german_buildings_early | GERMAN_BUILDINGS_EARLY | item | -1 | 1 | 1 (1 = enabled; 0 = disabled) |
| 0x744 | german_industry_early | GERMAN_INDUSTRY_EARLY | item | -1 | 1 | 1 (1 = enabled; 0 = disabled) |
| 0x748 | german_city_gather | GERMAN_CITY_GATHER | item | -1 | 5 | 5 |
| 0x74c | russian_attrition | RUSSIAN_ATTRITION | item | -1 | 100 | 100% bonus |
| 0x750 | russian_attrition_upgrades | RUSSIAN_ATTRITION_UPGRADES | item | -1 | 1 | 1 |
| 0x754 | russian_oil | RUSSIAN_OIL | item | -1 | 20 | 20% bonus |
| 0x758 | russian_communism | RUSSIAN_COMMUNISM | item | -1 | 0 | 0 |
| 0x75c | russian_uber_spies | RUSSIAN_UBER_SPIES | item | -1 | -1 | **missing in XML** |
| 0x760 | russian_borders | RUSSIAN_BORDERS | item | -1 | 0 | 0 |
| 0x764 | russian_borders_per_age | RUSSIAN_BORDERS_PER_AGE | item | -1 | 1 | 1 |
| 0x768 | russian_spy_cost | RUSSIAN_SPY_COST | item | -1 | 50 | 50% cheaper |
| 0x76c | russian_cossack_damage | RUSSIAN_COSSACK_DAMAGE | item | -1 | 25 | 25% bonus |
| 0x770 | russian_plunder_steal | RUSSIAN_PLUNDER_STEAL | item | -1 | 1 | 1 |
| 0x774 | russian_free_civic | RUSSIAN_FREE_CIVIC | item | -1 | 1 | 1 |
| 0x778 | chinese_knowledge_upgrades | CHINESE_KNOWLEDGE_UPGRADES | item | -1 | 0 | 0 (1 = enabled, 0 = disabled) |
| 0x77c | chinese_citizen_instant | CHINESE_CITIZEN_INSTANT | item | -1 | 1 | 1 |
| 0x780 | chinese_citizen_creation | CHINESE_CITIZEN_CREATION | item | -1 | 100 | 100% faster |
| 0x784 | chinese_large_city | CHINESE_LARGE_CITY | item | -1 | 1 | 1 (1 = enabled, 0 = disabled) |
| 0x788 | chinese_large_cities | CHINESE_LARGE_CITIES | item | -1 | 1 | 1 (1 = enabled, 0 = disabled) |
| 0x78c | chinese_herbal_lore | CHINESE_HERBAL_LORE | item | -1 | 1 | 1 |
| 0x790 | chinese_science_discount | CHINESE_SCIENCE_DISCOUNT | item | -1 | 20 | 20% |
| 0x794 | japanese_damage | JAPANESE_DAMAGE | item | -1 | -5 | -5% bonus vs. buildings (negative is per age) |
| 0x798 | japanese_barracks_cost | JAPANESE_BARRACKS_COST | item | -1 | -7 | -7% cost reduction (negative is per age) |
| 0x79c | japanese_barracks_speed | JAPANESE_BARRACKS_SPEED | item | -1 | -10 | -10% faster creation (negative is per age) |
| 0x7a0 | japanese_ship_cost | JAPANESE_SHIP_COST | item | -1 | 10 | 10% discount |
| 0x7a4 | japanese_carrier_speed | JAPANESE_CARRIER_SPEED | item | -1 | 33 | 33% faster creation |
| 0x7a8 | japanese_farms_cheaper | JAPANESE_FARMS_CHEAPER | item | -1 | 50 | 50% cheaper |
| 0x7ac | japanese_fishing_boats | JAPANESE_FISHING_BOATS | item | -1 | 25 | 25% increase |
| 0x7b0 | korean_citizens[8] | KOREAN_CITIZENS | entries int | 0 | 1,3,5,5,5,5,5,5 | 1 / 3 / 5 / 5 / 5 / 5 / 5 / 5 / 5 |
| 0x7d0 | korean_temple_upgrades | KOREAN_TEMPLE_UPGRADES | item | -1 | 2 | 2 (1 = enable, 0 = disable, 2 = enable and start with one) |
| 0x7d4 | korean_temple_tax_upgrades | KOREAN_TEMPLE_TAX_UPGRADES | item | -1 | 0 | 0 (1 = enable, 0 = disable) |
| 0x7d8 | korean_militia_upgrades | KOREAN_MILITIA_UPGRADES | item | -1 | 1 | 1 |
| 0x7dc | korean_build_under_fire | KOREAN_BUILD_UNDER_FIRE | item | -1 | 1 | 1 |
| 0x7e0 | korean_repair | KOREAN_REPAIR | item | -1 | 50 | 50% less total time |
| 0x7e4 | korean_tower_cost | KOREAN_TOWER_COST | item | -1 | 33 | 33% cheaper |
| 0x7e8 | mongol_stable_cost | MONGOL_STABLE_COST | item | -1 | 10 | 10% cost reduction |
| 0x7ec | mongol_stable_speed | MONGOL_STABLE_SPEED | item | -1 | 20 | 20% faster creation |
| 0x7f0 | mongol_attrition | MONGOL_ATTRITION | item | -1 | 50 | 50% reduction |
| 0x7f4 | mongol_free_cavalry | MONGOL_FREE_CAVALRY | item | -1 | 2 | 2 |
| 0x7f8 | mongol_free_forage | MONGOL_FREE_FORAGE | item | -1 | 1 | 1 |
| 0x7fc | mongol_nomadic_food | MONGOL_NOMADIC_FOOD | item | -1 | 1 | 1 |
| 0x800 | mongol_start_cavalry | MONGOL_START_CAVALRY | item | -1 | 1 | 1 |
| 0x804 | mongol_three_mil_cavalry | MONGOL_THREE_MIL_CAVALRY | item | -1 | 3 | 3 |
| 0x808 | iroquois_los | IROQUOIS_LOS | item | -1 | 0 | 0 |
| 0x80c | iroquois_food | IROQUOIS_FOOD | item | -1 | 2 | 2 |
| 0x810 | iroquois_senate | IROQUOIS_SENATE | item | -1 | 0 | 0 |
| 0x814 | iroquois_heal_rate[4] | IROQUOIS_HEAL_RATE | entries int | 0 | 20,15,10,5 | 20 frames / 15 frames / 10 frames / 5 frames |
| 0x824 | iroquois_free_scout | IROQUOIS_FREE_SCOUT | item | -1 | 1 | 1 |
| 0x828 | iroquois_scout_upgrades | IROQUOIS_SCOUT_UPGRADES | item | -1 | 1 | 1 |
| 0x82c | iroquois_govs_early | IROQUOIS_GOVS_EARLY | item | -1 | 0 | 0 age early |
| 0x830 | iroquois_extra_hits | IROQUOIS_EXTRA_HITS | item | -1 | 5 | 5% hp bonus to combat foot units |
| 0x834 | iroquois_quick_senate | IROQUOIS_QUICK_SENATE | item | -1 | 1 | 1 |
| 0x838 | iroquois_spear_speed[4] | IROQUOIS_SPEAR_SPEED | entries int | 0 | 1,1,1,1 | 1 / 1 / 1 / 1 |
| 0x848 | lakota_food | LAKOTA_FOOD | item | -1 | 4 | 4 |
| 0x84c | lakota_herbal_lore | LAKOTA_HERBAL_LORE | item | -1 | 1 | 1 |
| 0x850 | lakota_raze_speed | LAKOTA_RAZE_SPEED | item | -1 | 100 | 100% of normal amount of time |
| 0x854 | lakota_raze_price | LAKOTA_RAZE_PRICE | item | -1 | 0 | 0 |
| 0x858 | lakota_cav_upgrades | LAKOTA_CAV_UPGRADES | item | -1 | 1 | 1 |
| 0x85c | lakota_cav_dmg_bonus | LAKOTA_CAV_DMG_BONUS | frac×256 | 0 | 85 | 1/3 resources per dmg point |
| 0x860 | americans_free_scholar | AMERICANS_FREE_SCHOLAR | item | -1 | 1 | 1 |
| 0x864 | americans_free_bomber | AMERICANS_FREE_BOMBER | item | -1 | 1 | 1 |
| 0x868 | americans_aircraft | AMERICANS_AIRCRAFT | item | -1 | 20 | 20% |
| 0x86c | americans_marine_speed_bonus | AMERICANS_MARINE_SPEED_BONUS | item | -1 | 2 | 2 speed bonus for marine transports per military tech |
| 0x870 | americans_marine_hp_bonus | AMERICANS_MARINE_HP_BONUS | item | -1 | 70 | 70 extra hitpoints for marine transports |
| 0x874 | americans_marine_entrench | AMERICANS_MARINE_ENTRENCH | item then v−v/15+2 | -1 | 7 | 5 seconds of idling |
| 0x878 | americans_starting_farms | AMERICANS_STARTING_FARMS | item | -1 | 3 | 3 farms at start of game |
| 0x87c | americans_free_science | AMERICANS_FREE_SCIENCE | item | -1 | 0 | 0 |
| 0x880 | americans_science_discount | AMERICANS_SCIENCE_DISCOUNT | item | -1 | 0 | 0% discount on science line |
| 0x884 | americans_upgrade_discount | AMERICANS_UPGRADE_DISCOUNT | item | -1 | 25 | 25% discount on military upgrades |
| 0x888 | americans_barracks_gather | AMERICANS_BARRACKS_GATHER | item | -1 | 2 | 2 each of food, timber, metal, and gold |
| 0x88c | indians_city_radius | INDIANS_CITY_RADIUS | item | -1 | 4 | 4 |
| 0x890 | indians_caravan | INDIANS_CARAVAN | item | -1 | 15 | 15 |
| 0x894 | indians_elephant_cost | INDIANS_ELEPHANT_COST | item | -1 | 15 | 15% (percent cheaper) |
| 0x898 | indians_elephant_upgrades | INDIANS_ELEPHANT_UPGRADES | item | -1 | 1 | 1 |
| 0x89c | dutch_free_commerce | DUTCH_FREE_COMMERCE | item | -1 | 1 | 1 |
| 0x8a0 | dutch_commerce_discount | DUTCH_COMMERCE_DISCOUNT | item | -1 | 10 | 10% |
| 0x8a4 | dutch_fort_placement | DUTCH_FORT_PLACEMENT | item | -1 | 0 | 0 |
| 0x8a8 | dutch_interest | DUTCH_INTEREST | item | -1 | 5 | 5% |
| 0x8ac | dutch_interest_cap | DUTCH_INTEREST_CAP | item | -1 | 50 | 50 over econ cap |
| 0x8b0 | dutch_free_light_ship | DUTCH_FREE_LIGHT_SHIP | item | -1 | 1 | 1 |
| 0x8b4 | dutch_fort_speed | DUTCH_FORT_SPEED | item | -1 | 0 | 0% faster |
| 0x8b8 | dutch_attack_bonus | DUTCH_ATTACK_BONUS | item | -1 | 1 | 1 (+1 to attack and armor for dutch merchants, caravans, and wagons... |
| 0x8bc | dutch_hp_bonus | DUTCH_HP_BONUS | item | -1 | 10 | 10 (+10 hps to dutch armed civilians per age) |
| 0x8c0 | dutch_ship_discount | DUTCH_SHIP_DISCOUNT | item | -1 | 10 | 10% cheaper ships |
| 0x8c4 | persians_despotism | PERSIANS_DESPOTISM | item | -1 | 0 | 0 |
| 0x8c8 | persians_bonus_food | PERSIANS_BONUS_FOOD | item | -1 | 50 | 50% |
| 0x8cc | persians_civic_discount | PERSIANS_CIVIC_DISCOUNT | item | -1 | 30 | 30% |
| 0x8d0 | persians_taxation | PERSIANS_TAXATION | item | -1 | 1 | 1 |
| 0x8d4 | relics_research_speed | RELICS_RESEARCH_SPEED | item | -1 | 33 | 33% |
| 0x8d8 | wine_unit_upgrades | WINE_UNIT_UPGRADES | item | -1 | 20 | 20% |
| 0x8dc | papyrus_science_military | PAPYRUS_SCIENCE_MILITARY | item | -1 | 25 | 25% |
| 0x8e0 | dye_civic_commerce | DYE_CIVIC_COMMERCE | item | -1 | 25 | 25% |
| 0x8e4 | silk_commerce | SILK_COMMERCE | item | -1 | 25 | 25% |
| 0x8e8 | furs_military | FURS_MILITARY | item | -1 | 25 | 25% |
| 0x8ec | silk_caravan | SILK_CARAVAN | item | -1 | 0 | 0 |
| 0x8f0 | spice_caravan_income | SPICE_CARAVAN_INCOME | item | -1 | 20 | 20% |
| 0x8f4 | salt_barracks_cost | SALT_BARRACKS_COST | item | -1 | 15 | 15% |
| 0x8f8 | horses_stable_cost | HORSES_STABLE_COST | item | -1 | 15 | 15% |
| 0x8fc | rubber_autoplant_cost | RUBBER_AUTOPLANT_COST | item | -1 | 15 | 15% |
| 0x900 | sulphur_siege_cost | SULPHUR_SIEGE_COST | item | -1 | 20 | 20% |
| 0x904 | amber_market | AMBER_MARKET | item | -1 | 10 | 10 |
| 0x908 | silver_age_cost | SILVER_AGE_COST | item | -1 | 15 | 15% |
| 0x90c | tobacco_building_speed | TOBACCO_BUILDING_SPEED | item | -1 | 10 | 10% |
| 0x910 | marble_wonder_cost | MARBLE_WONDER_COST | item | -1 | 10 | 10% |
| 0x914 | furs_los | FURS_LOS | item | -1 | 0 | 0 |
| 0x918 | cotton_creation_speed | COTTON_CREATION_SPEED | item | -1 | 25 | 25% |
| 0x91c | bison_granary_cost | BISON_GRANARY_COST | item | -1 | 33 | 33% |
| 0x920 | copper_factory_hp | COPPER_FACTORY_HP | item | -1 | 20 | 20% |
| 0x924 | peacocks_pop | PEACOCKS_POP | item | -1 | 10 | 10% |
| 0x928 | sugar_food_cost | SUGAR_FOOD_COST | item | -1 | 10 | 10% |
| 0x92c | diamonds_commerce | DIAMONDS_COMMERCE | item | -1 | 10 | 10 |
| 0x930 | gems_territory_bonus | GEMS_TERRITORY_BONUS | item | -1 | 2 | 2 |
| 0x934 | aluminum_air_cost | ALUMINUM_AIR_COST | item | -1 | 15 | 15% |
| 0x938 | aluminum_air_speed | ALUMINUM_AIR_SPEED | item | -1 | 25 | 25% |
| 0x93c | whales_ships_move | WHALES_SHIPS_MOVE | item | -1 | 20 | 20% |
| 0x940 | obsidian_archers_range | OBSIDIAN_ARCHERS_RANGE | item | -1 | 0 | 0 |
| 0x944 | obsidian_archers_attack | OBSIDIAN_ARCHERS_ATTACK | item | -1 | 1 | 1 |
| 0x948 | wool_citizens_cost | WOOL_CITIZENS_COST | item | -1 | 15 | 15% |
| 0x94c | wool_citizens_speed | WOOL_CITIZENS_SPEED | item | -1 | 33 | 33% |
| 0x950 | coal_timber_cost | COAL_TIMBER_COST | item | -1 | 25 | 25% |
| 0x954 | uranium_nuke_cost | URANIUM_NUKE_COST | item | -1 | 5 | 5% |
| 0x958 | uranium_nuke_speed | URANIUM_NUKE_SPEED | item | -1 | 10 | 10% |
| 0x95c | titanium_attrition | TITANIUM_ATTRITION | item | -1 | 50 | 50% |
| 0x960 | cattle_citizen_armor | CATTLE_CITIZEN_ARMOR | item | -1 | 1 | 1 |
| 0x964 | dogs_scout_los | DOGS_SCOUT_LOS | item | -1 | 3 | 3 |
| 0x968 | olive_farms | OLIVE_FARMS | item | -1 | 1 | 1 |
| 0x96c | gypsum_cost | GYPSUM_COST | item | -1 | 10 | 10% |
| 0x970 | beaver_free_scout_merch | BEAVER_FREE_SCOUT_MERCH | item | -1 | 1 | 1 |
| 0x974 | gold_wealth_cost | GOLD_WEALTH_COST | item | -1 | 10 | 10% |
| 0x978 | rum_naval_range | RUM_NAVAL_RANGE | item | -1 | 1 | 1 |
| 0x97c | coffee_income_bonus | COFFEE_INCOME_BONUS | item | -1 | 10 | 10% |
| 0x980 | bananas_hp_bonus | BANANAS_HP_BONUS | item | -1 | 10 | 10% |
| 0x984 | salmon_naval_cost | SALMON_NAVAL_COST | item | -1 | 20 | 20% |
| 0x988 | iron_metal_cost | IRON_METAL_COST | item | -1 | 10 | 10% |
| 0x98c | maize_ramping_bonus | MAIZE_RAMPING_BONUS | item | -1 | 50 | 50% |
| 0x990 | incense_temple_research | INCENSE_TEMPLE_RESEARCH | item | -1 | 25 | 25% |
| 0x994 | despotism_military_cheaper | DESPOTISM_MILITARY_CHEAPER | item | -1 | 15 | 15% |
| 0x998 | despotism_military_cheaper2 | DESPOTISM_MILITARY_CHEAPER2 | item | -1 | 15 | 15% |
| 0x99c | despotism_military_cheaper3 | DESPOTISM_MILITARY_CHEAPER3 | item | -1 | 15 | 15% |
| 0x9a0 | republic_commerce_bonus | REPUBLIC_COMMERCE_BONUS | item | -1 | 50 | 50 |
| 0x9a4 | republic_commerce_bonus2 | REPUBLIC_COMMERCE_BONUS2 | item | -1 | 50 | 50 |
| 0x9a8 | republic_commerce_bonus3 | REPUBLIC_COMMERCE_BONUS3 | item | -1 | 50 | 50 |
| 0x9ac | monarchy_unit_prod | MONARCHY_UNIT_PROD | item | -1 | 25 | 25% (stable units cheaper and faster) |
| 0x9b0 | monarchy_unit_prod2 | MONARCHY_UNIT_PROD2 | item | -1 | 25 | 25% |
| 0x9b4 | democracy_tech_bonus | DEMOCRACY_TECH_BONUS | item | -1 | 20 | 20% |
| 0x9b8 | democracy_tech_bonus2 | DEMOCRACY_TECH_BONUS2 | item | -1 | 20 | 20% |
| 0x9bc | socialism_unit_bonus | SOCIALISM_UNIT_BONUS | item | -1 | 25 | 25% (factory, airbase, dock units cheaper and faster) |
| 0x9c0 | capitalism_oil_prod | CAPITALISM_OIL_PROD | item | -1 | 100 | 100 income |
| 0x9c4 | capitalism_oil_gift | CAPITALISM_OIL_GIFT | item | -1 | 500 | 500 oil gift |
| 0x9c8 | gov_hero_respawn | GOV_HERO_RESPAWN | item | -1 | 30 | 30 seconds |
| 0x9cc | mtn_tiny_size | MTN_TINY_SIZE | item | -1 | 100 | 100 tiles |
| 0x9d0 | mtn_small_size | MTN_SMALL_SIZE | item | -1 | 210 | 210 tiles |
| 0x9d4 | mtn_medium_size | MTN_MEDIUM_SIZE | item | -1 | 275 | 275 tiles |
| 0x9d8 | mtn_large_size | MTN_LARGE_SIZE | item | -1 | 400 | 400 tiles |
| 0x9dc | mtn_huge_size | MTN_HUGE_SIZE | item | -1 | 1000 | 1000 tiles |
| 0x9e0 | mtn_tiny_gather | MTN_TINY_GATHER | item | -1 | 3 | 3 citizens |
| 0x9e4 | mtn_small_gather | MTN_SMALL_GATHER | item | -1 | 5 | 5 citizens |
| 0x9e8 | mtn_medium_gather | MTN_MEDIUM_GATHER | item | -1 | 6 | 6 citizens |
| 0x9ec | mtn_large_gather | MTN_LARGE_GATHER | item | -1 | 8 | 8 citizens |
| 0x9f0 | mtn_huge_gather | MTN_HUGE_GATHER | item | -1 | 10 | 10 citizens |
| 0x9f4 | min_continent_spacing | MIN_CONTINENT_SPACING | item | -1 | 3 | 3 wcoords including coasts |
| 0x9f8 | ctw_starting_tribute | CTW_STARTING_TRIBUTE | item | -1 | 25 | 25 |
| 0x9fc | ctw_no_attack_bonus | CTW_NO_ATTACK_BONUS | item | -1 | 25 | 25 |
| 0xa00 | ctw_no_attack_bonus_cap | CTW_NO_ATTACK_BONUS_CAP | item | -1 | 100 | 100 |
| 0xa04 | ctw_trib_no_attack | CTW_TRIB_NO_ATTACK | item | -1 | 3 | 3 |
| 0xa08 | ctw_continent_bonus | CTW_CONTINENT_BONUS | item | -1 | 10 | 10 |
| 0xa0c | ctw_attrition | CTW_ATTRITION | item | -1 | 50 | 50% bonus |
| 0xa10 | ctw_base_war_cost | CTW_BASE_WAR_COST | item | -1 | 20 | 20 tribute |
| 0xa14 | ctw_war_cost_stance_time_x | CTW_WAR_COST_STANCE_TIME_X | item | -1 | 1 | 1 tribute |
| 0xa18 | ctw_base_peace_cost | CTW_BASE_PEACE_COST | item | -1 | 20 | 20 tribute |
| 0xa1c | ctw_peace_cost_att_x | CTW_PEACE_COST_ATT_X | item | -1 | 1 | 1 tribute |
| 0xa20 | ctw_base_build_cost | CTW_BASE_BUILD_COST | item | -1 | 20 | 20 tribute |
| 0xa24 | ctw_build_cost_time_held_x | CTW_BUILD_COST_TIME_HELD_X | item | -1 | 10 | 10 x |
| 0xa28 | ctw_base_card_cost | CTW_BASE_CARD_COST | item | -1 | 30 | 30 tribute |
| 0xa2c | ctw_card_cost_num_purchased_x | CTW_CARD_COST_NUM_PURCHASED_X | item | -1 | 40 | 40 x |
| 0xa30 | ctw_card_cost_ramp_cap | CTW_CARD_COST_RAMP_CAP | item | -1 | 110 | 110 |
| 0xa34 | ctw_base_territory_cost | CTW_BASE_TERRITORY_COST | item | -1 | 75 | 75 tribute |
| 0xa38 | ctw_territory_cost_att_x | CTW_TERRITORY_COST_ATT_X | item | -1 | 2 | 2 |
| 0xa3c | ctw_territory_cost_str_x | CTW_TERRITORY_COST_STR_X | item | -1 | 25 | 25 |
| 0xa40 | ctw_territory_min_emp_size | CTW_TERRITORY_MIN_EMP_SIZE | item | -1 | 4 | 4 |
| 0xa44 | ctw_territory_max_att | CTW_TERRITORY_MAX_ATT | item | -1 | 17 | 17 |
| 0xa48 | ctw_max_attitude | CTW_MAX_ATTITUDE | item | -1 | 20 | 20 |
| 0xa4c | ctw_start_attitude | CTW_START_ATTITUDE | item | -1 | 10 | 10 |
| 0xa50 | ctw_num_opp_for_larger_map | CTW_NUM_OPP_FOR_LARGER_MAP | item | -1 | 2 | 2 |
| 0xa54 | ctw_min_keep_alliance | CTW_MIN_KEEP_ALLIANCE | item | -1 | 4 | 4 |
| 0xa58 | ctw_break_alliance_prob_x_att | CTW_BREAK_ALLIANCE_PROB_X_ATT | item | -1 | 1 | 1 |
| 0xa5c | ctw_capital_starting_infra | CTW_CAPITAL_STARTING_INFRA | item | -1 | 2 | 2 |
| 0xa60 | ctw_nomad_starting_res_x | CTW_NOMAD_STARTING_RES_X | item | -1 | 3 | 3 |
| 0xa64 | ctw_max_infra_special_scen | CTW_MAX_INFRA_SPECIAL_SCEN | item | -1 | 3 | 3 |
| 0xa68 | ctw_max_treachery_infra | CTW_MAX_TREACHERY_INFRA | item | -1 | 3 | 3 |
| 0xa6c | ctw_age_cost | CTW_AGE_COST | item | -1 | 50 | 50 |
| 0xa70 | ctw_eureka_techs | CTW_EUREKA_TECHS | item | -1 | 2 | 2 |
| 0xa74 | ctw_eureka_all_techs | CTW_EUREKA_ALL_TECHS | item | -1 | 1 | 1 |
| 0xa78 | ctw_max_bribe_att | CTW_MAX_BRIBE_ATT | item | -1 | 15 | 15 |
| 0xa7c | ctw_cit_per_extra_city | CTW_CIT_PER_EXTRA_CITY | item | -1 | 10 | 10 |
| 0xa80 | ctw_ai_barb_chance | CTW_AI_BARB_CHANCE | item | -1 | 95 | 95 |
| 0xa84 | ctw_ai_stronger_chance | CTW_AI_STRONGER_CHANCE | item | -1 | 80 | 80 |
| 0xa88 | ctw_additional_ally_inc | CTW_ADDITIONAL_ALLY_INC | item | -1 | 100 | 100 |
| 0xa8c | ctw_break_bribe_x | CTW_BREAK_BRIBE_X | item | -1 | 3 | 3 |
| 0xa90 | ctw_ally_war_cost_x | CTW_ALLY_WAR_COST_X | item | -1 | 2 | 2 |
| 0xa94 | ctw_ai_all_cards_num | CTW_AI_ALL_CARDS_NUM | item | -1 | 4 | 4 |
| 0xa98 | ctw_ai_all_cards_prob | CTW_AI_ALL_CARDS_PROB | item | -1 | 25 | 25 |
| 0xa9c | ctw_ai_tac_card_prob | CTW_AI_TAC_CARD_PROB | item | -1 | 25 | 25 |
| 0xaa0 | ctw_ai_tac_card_ally_prob | CTW_AI_TAC_CARD_ALLY_PROB | item | -1 | 50 | 50 |
| 0xaa4 | ctw_bribe_trib_min | CTW_BRIBE_TRIB_MIN | item | -1 | 10 | 10 |
| 0xaa8 | ctw_bribe_tough_hate_acc_prob | CTW_BRIBE_TOUGH_HATE_ACC_PROB | item | -1 | 35 | 35 |
| 0xaac | ctw_bribe_tough_acc_prob | CTW_BRIBE_TOUGH_ACC_PROB | item | -1 | 50 | 50 |
| 0xab0 | ctw_bribe_acc_prob | CTW_BRIBE_ACC_PROB | item | -1 | 100 | 100 |
| 0xab4 | ctw_demand_trib_tough_prob | CTW_DEMAND_TRIB_TOUGH_PROB | item | -1 | 50 | 50 |
| 0xab8 | ctw_demand_trib_prob | CTW_DEMAND_TRIB_PROB | item | -1 | 35 | 35 |
| 0xabc | ctw_overrun_peace_prob | CTW_OVERRUN_PEACE_PROB | item | -1 | 50 | 50 |
| 0xac0 | ctw_overrun_easiest_prob | CTW_OVERRUN_EASIEST_PROB | item | -1 | 0 | 0 |
| 0xac4 | ctw_overrun_easy_prob | CTW_OVERRUN_EASY_PROB | item | -1 | 10 | 10 |
| 0xac8 | ctw_overrun_moderate_prob | CTW_OVERRUN_MODERATE_PROB | item | -1 | 20 | 20 |
| 0xacc | ctw_overrun_tough_prob | CTW_OVERRUN_TOUGH_PROB | item | -1 | 100 | 100 |
| 0xad0 | ctw_def_victory_bonus | CTW_DEF_VICTORY_BONUS | item | -1 | 50 | 50 |
| 0xad4 | ctw_wonder_min_civics | CTW_WONDER_MIN_CIVICS | item | -1 | 4 | 4 |
| 0xad8 | ctw_conquest_time_limit | CTW_CONQUEST_TIME_LIMIT | item | -1 | 90 | 90 |
| 0xadc | ctw_great_thinker_discount | CTW_GREAT_THINKER_DISCOUNT | item | -1 | 25 | 25 |
| 0xae0 | ctw_great_thinker_time | CTW_GREAT_THINKER_TIME | item | -1 | 25 | 25 |
| 0xae4 | ctw_market_bonus_buy | CTW_MARKET_BONUS_BUY | item | -1 | 25 | 25 |
| 0xae8 | ctw_market_bonus_sell | CTW_MARKET_BONUS_SELL | item | -1 | 20 | 20 |
| 0xaec | ctw_heal_bonus | CTW_HEAL_BONUS | item | -1 | 100 | 100 |
| 0xaf0 | ctw_prod_rate_bonus | CTW_PROD_RATE_BONUS | item | -1 | 5 | 5 |
| 0xaf4 | ctw_max_wonder_cards | CTW_MAX_WONDER_CARDS | item | -1 | 1 | 1 |
| 0xaf8 | ctw_num_bonus_elephants | CTW_NUM_BONUS_ELEPHANTS | item | -1 | 6 | 6 |
| 0xafc | ctw_num_bonus_spies | CTW_NUM_BONUS_SPIES | item | -1 | 3 | 3 |
| 0xb00 | ctw_colony_blockade_turns | CTW_COLONY_BLOCKADE_TURNS | item | -1 | 2 | 2 |
| 0xb04 | ctw_steal_trib_amt | CTW_STEAL_TRIB_AMT | item | -1 | 100 | 100 |
| 0xb08 | ctw_missionaries_bonus | CTW_MISSIONARIES_BONUS | item | -1 | 25 | 25 |
| 0xb0c | ctw_num_bonus_citizens | CTW_NUM_BONUS_CITIZENS | item | -1 | 5 | 5 |
| 0xb10 | ctw_nuke_base | CTW_NUKE_BASE | item | -1 | 75 | 75 |
| 0xb14 | ctw_nuke_ramp | CTW_NUKE_RAMP | item | -1 | 35 | 35 |
| 0xb18 | ctw_nuke_launch_turns | CTW_NUKE_LAUNCH_TURNS | item | -1 | 2 | 2 |
| 0xb1c | ctw_espionage_cost | CTW_ESPIONAGE_COST | item | -1 | 20 | 20 |
| 0xb20 | ctw_nuclear_enemy_war_cost | CTW_NUCLEAR_ENEMY_WAR_COST | item | -1 | 450 | 450 |
| 0xb24 | ctw_nuclear_war_age_discount | CTW_NUCLEAR_WAR_AGE_DISCOUNT | item | -1 | 100 | 100 |
| 0xb28 | ctw_defcon_war_discount | CTW_DEFCON_WAR_DISCOUNT | item | -1 | 25 | 25 |
| 0xb2c | ctw_terr_income_base | CTW_TERR_INCOME_BASE | item | -1 | 1 | 1 |
| 0xb30 | ctw_terr_income_x | CTW_TERR_INCOME_X | item | -1 | 1 | 1 |
| 0xb34 | ctw_ally_failure_comp | CTW_ALLY_FAILURE_COMP | item | -1 | 25 | 25 |
| 0xb38 | ctw_parmenio_cav | CTW_PARMENIO_CAV | item | -1 | 5 | 5 |
| 0xb3c | ctw_memnon_infra | CTW_MEMNON_INFRA | item | -1 | 4 | 4 |
| 0xb40 | ctw_spitamenes_archers | CTW_SPITAMENES_ARCHERS | item | -1 | 5 | 5 |
| 0xb44 | alexander_attack | ALEXANDER_ATTACK | item | -1 | 1 | 1 |
| 0xb48 | alexander_heavy_inf_attack | ALEXANDER_HEAVY_INF_ATTACK | item | -1 | 2 | 2 |
| 0xb4c | alexander_forced_march_speed | ALEXANDER_FORCED_MARCH_SPEED | frac×256 | 0 | 384 | 3/2 |
| 0xb50 | parmenio_heavy_cav_attack | PARMENIO_HEAVY_CAV_ATTACK | item | -1 | 1 | 1 |
| 0xb54 | parmenio_radius_adjust | PARMENIO_RADIUS_ADJUST | frac×256 | 0 | 384 | 3/2 |
| 0xb58 | parmenio_ambush_time | PARMENIO_AMBUSH_TIME | frac×256 | 0 | 768 | 3/1 |
| 0xb5c | parmenio_ambush_cost | PARMENIO_AMBUSH_COST | frac×256 | 0 | 256 | 1/1 |
| 0xb60 | ptolemy_range_bonus | PTOLEMY_RANGE_BONUS | item | -1 | 1 | 1 |
| 0xb64 | ptolemy_los_bonus | PTOLEMY_LOS_BONUS | item | -1 | 2 | 2 |
| 0xb68 | ptolemy_siege_attack | PTOLEMY_SIEGE_ATTACK | item | -1 | 1 | 1 |
| 0xb6c | ptolemy_armor | PTOLEMY_ARMOR | item | -1 | 1 | 1 |
| 0xb70 | darius_armor | DARIUS_ARMOR | item | -1 | 1 | 1 |
| 0xb74 | spitamenes_horse_archer_attack | SPITAMENES_HORSE_ARCHER_ATTACK | item | -1 | 1 | 1 |
| 0xb78 | spitamenes_cav_speed | SPITAMENES_CAV_SPEED | frac×256 | 0 | 307 | 12/10 |
| 0xb7c | porus_elephant_speed | PORUS_ELEPHANT_SPEED | frac×256 | 0 | 307 | 12/10 |
| 0xb80 | porus_decoys | PORUS_DECOYS | frac×256 | 0 | 512 | 2/1 |
| 0xb84 | memnon_regen_rate | MEMNON_REGEN_RATE | frac×256 | 0 | 512 | 2/1 |
| 0xb88 | memnon_merc_attack | MEMNON_MERC_ATTACK | item | -1 | 5 | 5 |
| 0xb8c | memnon_merc_armor | MEMNON_MERC_ARMOR | item | -1 | 4 | 4 |
| 0xb90 | antipater_heal_rate | ANTIPATER_HEAL_RATE | item | -1 | 20 | 20 frames |
| 0xb94 | antipater_garrison_attack_bonus | ANTIPATER_GARRISON_ATTACK_BONUS | item | -1 | 3 | 3 |
| 0xb98 | antipater_entrench_bonus | ANTIPATER_ENTRENCH_BONUS | frac×256 | 0 | 204 | 80/100 |
| 0xb9c | antipater_entrench_rate | ANTIPATER_ENTRENCH_RATE | frac×256 | 0 | 128 | 1/2 |
| 0xba0 | schwarzenberg_commandos | SCHWARZENBERG_COMMANDOS | item | -1 | 4 | 4 |
| 0xba4 | napoleon_siege_attack | NAPOLEON_SIEGE_ATTACK | item | -1 | 3 | 3 |
| 0xba8 | napoleon_siege_range | NAPOLEON_SIEGE_RANGE | item | -1 | 1 | 1 |
| 0xbac | napoleon_pack | NAPOLEON_PACK | item | -1 | 50 | 50% of normal |
| 0xbb0 | napoleon_siege_speed | NAPOLEON_SIEGE_SPEED | item | -1 | 150 | 150% of normal |
| 0xbb4 | wellington_radius | WELLINGTON_RADIUS | item | -1 | 200 | 200% of normal |
| 0xbb8 | wellington_defense | WELLINGTON_DEFENSE | item | -1 | 1 | 1 |
| 0xbbc | wellington_siege_attack | WELLINGTON_SIEGE_ATTACK | item | -1 | 1 | 1 |
| 0xbc0 | charles_speed | CHARLES_SPEED | item | -1 | 120 | 120% faster |
| 0xbc4 | charles_unit_cost | CHARLES_UNIT_COST | item | -1 | 90 | 90% of normal |
| 0xbc8 | djezzar_attack | DJEZZAR_ATTACK | item | -1 | 1 | 1 |
| 0xbcc | blucher_cav_attack | BLUCHER_CAV_ATTACK | item | -1 | 2 | 2 |
| 0xbd0 | blucher_cav_armor | BLUCHER_CAV_ARMOR | item | -1 | 2 | 2 |
| 0xbd4 | blucher_cav_speed | BLUCHER_CAV_SPEED | item | -1 | 120 | 120% of normal |
| 0xbd8 | kutosov_decoys | KUTOSOV_DECOYS | item | -1 | 300 | 300% of normal |
| 0xbdc | kutosov_radius | KUTOSOV_RADIUS | item | -1 | 300 | 300% of normal |
| 0xbe0 | tower_garrison_upgrade | TOWER_GARRISON_UPGRADE | item | -1 | 2 | 2 |
| 0xbe4 | fort_garrison_upgrade | FORT_GARRISON_UPGRADE | item | -1 | 5 | 5 |
| 0xbe8 | unit_heal_rate[4] | UNIT_HEAL_RATE | entries int | 0 | 20,15,10,5 | 20 frames / 15 frames / 10 frames / 5 frames |
| 0xbf8 | supply_heal_rate | SUPPLY_HEAL_RATE | item | -1 | 0 | 0 frames (0 means don't heal at all) |
| 0xbfc | civilian_heal_rate | CIVILIAN_HEAL_RATE | item | -1 | 45 | 45 frames (0 means don't heal at all) |
| 0xc00 | aircraft_heal_rate[4] | AIRCRAFT_HEAL_RATE | entries int | 0 | 20,16,12,8 | 20 frames / 16 frames / 12 frames / 8 frames |
| 0xc10 | city_heal_rate | CITY_HEAL_RATE | item | -1 | 4 | 4 frames |
| 0xc14 | bombing_mana_cost | BOMBING_MANA_COST | item | -1 | 0 | 0 mana |
| 0xc18 | sam_site_reload_chance | SAM_SITE_RELOAD_CHANCE | item | -1 | 10 | 10 percent |
| 0xc1c | sam_site_reload_time | SAM_SITE_RELOAD_TIME | item | -1 | 50 | 50 |
| 0xc20 | artillery_under_attack_fires_slowly | ARTILLERY_UNDER_ATTACK_FIRES_SLOWLY | item | -1 | 1 | 1 (0 = fires normal speed, 1 = fires as if out of supply) |
| 0xc24 | goody_box | GOODY_BOX | item | -1 | 25 | 25 resources |
| 0xc28 | goody_box_age | GOODY_BOX_AGE | item | -1 | 25 | 25 resources / age |
| 0xc2c | food_bonus_for_farm | FOOD_BONUS_FOR_FARM | item | -1 | 20 | 20 |
| 0xc30 | oil_bonus_for_well | OIL_BONUS_FOR_WELL | item | -1 | 50 | 50 |
| 0xc34 | knowledge_bonus_for_university | KNOWLEDGE_BONUS_FOR_UNIVERSITY | item | -1 | 25 | 25 |
| 0xc38 | timber_bonus_per_wood_slot | TIMBER_BONUS_PER_WOOD_SLOT | item | -1 | 5 | 5 |
| 0xc3c | metal_bonus_per_mine_slot | METAL_BONUS_PER_MINE_SLOT | item | -1 | 5 | 5 |
| 0xc40 | river_resource_value | RIVER_RESOURCE_VALUE | item | -1 | 2 | 2 times basic rate (does this do anything anymore?) |
| 0xc44 | town_hp_bonus | TOWN_HP_BONUS | item | -1 | 2 | 2 |
| 0xc48 | senate_hp_bonus | SENATE_HP_BONUS | item | -1 | 35 | 35% |
| 0xc4c | senate_armor_bonus | SENATE_ARMOR_BONUS | item | -1 | 1 | 1 armor per city level |
| 0xc50 | forced_march_speed | FORCED_MARCH_SPEED | item | -1 | 42 | 42m |
| 0xc54 | general_speed_bonus | GENERAL_SPEED_BONUS | item | -1 | 3 | 3m |
| 0xc58 | general_radius | GENERAL_RADIUS | item | -1 | 6 | 6 tiles |
| 0xc5c | general_building_range | GENERAL_BUILDING_RANGE | item | -1 | 1 | 1 tile |
| 0xc60 | general_building_attack | GENERAL_BUILDING_ATTACK | item | -1 | 1 | 1 |
| 0xc64 | general_rally_armor | GENERAL_RALLY_ARMOR | item | -1 | 2 | 2 armor |
| 0xc68 | mil_patriot_radius_bonus | MIL_PATRIOT_RADIUS_BONUS | item | -1 | 3 | 3 tiles |
| 0xc6c | econ_patriot_radius_bonus | ECON_PATRIOT_RADIUS_BONUS | item | -1 | 1 | 1 tile |
| 0xc70 | thedespot_plunder | THEDESPOT_PLUNDER | item | -1 | 100 | 100% |
| 0xc74 | thedespot_unit_plunder | THEDESPOT_UNIT_PLUNDER | item | -1 | 15 | 15 resources |
| 0xc78 | thesenator_build_armor | THESENATOR_BUILD_ARMOR | item | -1 | 5 | 5 extra armor |
| 0xc7c | themonarch_cav_armor | THEMONARCH_CAV_ARMOR | item | -1 | 2 | 2 bonus armor |
| 0xc80 | themonarch_cav_attack | THEMONARCH_CAV_ATTACK | item | -1 | 2 | 2 bonus attack |
| 0xc84 | thepresident_unit_build_speed | THEPRESIDENT_UNIT_BUILD_SPEED | item | -1 | 100 | 100% |
| 0xc88 | thepresident_militia_attack_bonus | THEPRESIDENT_MILITIA_ATTACK_BONUS | item | -1 | 0 | 0 (disabled) |
| 0xc8c | thepresident_militia_armor_bonus | THEPRESIDENT_MILITIA_ARMOR_BONUS | item | -1 | 0 | 0 (disabled) |
| 0xc90 | thepresident_building_speed | THEPRESIDENT_BUILDING_SPEED | item | -1 | 33 | 33% |
| 0xc94 | thecitizen_attack_bonus | THECITIZEN_ATTACK_BONUS | item | -1 | 2 | 2 |
| 0xc98 | thecitizen_armor_bonus | THECITIZEN_ARMOR_BONUS | item | -1 | 2 | 2 |
| 0xc9c | thecitizen_assimilation_speed | THECITIZEN_ASSIMILATION_SPEED | item | -1 | 4 | 4 (4 times faster) |
| 0xca0 | theceo_production_bonus | THECEO_PRODUCTION_BONUS | item | -1 | 50 | 50% better in one city |
| 0xca4 | theceo_unit_range | THECEO_UNIT_RANGE | item | -1 | 2 | 2 tile bonus |
| 0xca8 | theceo_unit_los | THECEO_UNIT_LOS | item | -1 | 2 | 2 tile bonus |
| 0xcac | supply_radius | SUPPLY_RADIUS | item | -1 | 14 | 14 tiles |
| 0xcb0 | supply_radius_upgrade | SUPPLY_RADIUS_UPGRADE | item | -1 | 2 | 2 tiles |
| 0xcb4 | supply_hp_upgrade[4] | SUPPLY_HP_UPGRADE | entries int | 0 | 0,20,40,60 | 0 / 20 / 40 / 60 |
| 0xcc4 | truck_radius | TRUCK_RADIUS | item | -1 | 14 | 14 tiles |
| 0xcc8 | siege_out_of_supply_reload | SIEGE_OUT_OF_SUPPLY_RELOAD | frac×256 | 0 | 384 | 3/2 normal delay |
| 0xccc | artillery_out_of_supply_reload | ARTILLERY_OUT_OF_SUPPLY_RELOAD | frac×256 | 0 | 512 | 2/1 normal delay |
| 0xcd0 | decoy_time | DECOY_TIME | item | -1 | 2500 | 2500 frames |
| 0xcd4 | caravan_attack_bonus | CARAVAN_ATTACK_BONUS | item then ×10 | -1 | 20 | 2 attack |
| 0xcd8 | market_basement | MARKET_BASEMENT | item | -1 | 10 | 10 |
| 0xcdc | market_equilibrium | MARKET_EQUILIBRIUM | item | -1 | 65 | 65 |
| 0xce0 | market_min_variance | MARKET_MIN_VARIANCE | item | -1 | 2 | 2 |
| 0xce4 | market_min_trend | MARKET_MIN_TREND | item | -1 | 8 | 8 cycles |
| 0xce8 | market_trend_range | MARKET_TREND_RANGE | item | -1 | 16 | 16 cycles |
| 0xcec | market_cycle_rate | MARKET_CYCLE_RATE | item | -1 | 1 | 1 frames |
| 0xcf0 | market_supply_demand | MARKET_SUPPLY_DEMAND | item | -1 | 3 | 3 +/- to sell price (double to buy price) |
| 0xcf4 | paradrop_range | PARADROP_RANGE | item | -1 | 64 | 64 tiles |
| 0xcf8 | ally_to_war_delay | ALLY_TO_WAR_DELAY | item | -1 | -1 | **missing in XML** |
| 0xcfc | ally_to_war_grace | ALLY_TO_WAR_GRACE | item | -1 | -1 | **missing in XML** |
| 0xd00 | timer_refresh_ratio | TIMER_REFRESH_RATIO | item | -1 | 5 | 5 seconds with timer off removes 1 second from clock |
| 0xd04 | retake_capital | RETAKE_CAPITAL | item | -1 | 3600 | 3600 frames |
| 0xd08 | wonder_timer | WONDER_TIMER | item | -1 | 4500 | 4500 frames |
| 0xd0c | wonder_age | WONDER_AGE | item | -1 | 0 | 0 frames |
| 0xd10 | popwin_timer | POPWIN_TIMER | item | -1 | 3600 | 3600 frames |
| 0xd14 | armageddon | ARMAGEDDON | item | -1 | 4 | 4 nukes |
| 0xd18 | armageddon_per_nation | ARMAGEDDON_PER_NATION | item | -1 | 1 | 1 nukes |
| 0xd1c | armageddon_per_team | ARMAGEDDON_PER_TEAM | item | -1 | 2 | 2 nukes |
| 0xd20 | nuke_embargo_base | NUKE_EMBARGO_BASE | item | -1 | 900 | 900 frames |
| 0xd24 | nuke_embargo_nation | NUKE_EMBARGO_NATION | item | -1 | 900 | 900 frames |
| 0xd28 | nuke_embargo_world | NUKE_EMBARGO_WORLD | item | -1 | 0 | 0 frames |
| 0xd2c | reassimilation | REASSIMILATION | item | -1 | 300 | 300% normal rate (of my own cities I've recaptured) |
| 0xd30 | assimilation_timer | ASSIMILATION_TIMER | item | -1 | 2000 | 2000 frames |
| 0xd34 | peace_attrition | PEACE_ATTRITION | item | -1 | 8 | 8 frames -> this is the special border-violation attrition when you... |
| 0xd38 | assassin_attrition | ASSASSIN_ATTRITION | item | -1 | 8 | 8 frames -> this is the special assassin attrition |
| 0xd3c | attrition | ATTRITION | item | -1 | 48 | 48 frames -> this is the baseline level for "regular" attrition |

## 13. Determinism notes (all areas)

- **Order.** Type index = XML document order; never sort or de-duplicate. Passes run pass-major (every element
  of a file for pass 0, then pass 1, …). Several fields are "last writer wins" across types: `upgrade` of a unit's
  FROM (§7.4), a building parent's `to`/`upgrade` (§8.3), tribe graft slots (§7.2 step 5), building
  `build_flags` high bits (§8.4). Name keys return the **first** match.
- **Integer semantics.** int32 wrap-around everywhere, C truncating division (`cdiv`), signed shifts in
  `fill_relative_values`, int16 truncation of balance cells and `relative_value`, int8 truncation of grid
  positions, u32 for `job_time`/`res_time`. `_wtoi` saturates; `binary`/`hex` wrap.
- **Character rules.** Tag lookup is case-sensitive on the upper-cased name; value comparison is ASCII
  case-insensitive (`_wcsicmp`, "C" locale). Unit `FLAGS`, building `OBJ_MASKS`/`BUILD_FLAGS` and craft `FLAGS`
  map *every* character to a bit (`& 31`); unit `OBJ_MASK` ignores non-alphanumerics. Text is trimmed only at
  the ends (MSXML `text`); attributes are not trimmed.
- **Game-setting dependence.** `TypeData::get_preq` reads GameInfo (starting/ending technology, game rule 8).
  It is used (a) during `Types::init` for the tech AI values and the balance ages — with whatever GameInfo is in
  effect at program start — and (b) at every game start for `age`/`military_level` (§7.6). `finalize_grafting`
  also reads GameInfo flag 8. A port must feed the same values; for replays, take the walked tables from the
  replay (the original does, §1).
- **No hidden randomness.** No rules loader calls a random generator. (`Game::init_rules_and_teams` draws from
  `game_random` for random civs/start ages but does not write rules tables, §10.4.)
- **Runtime writers.** `ScenarioFuncSet::set_age_diff_penalty` @00a01520 rewrites five Constants mid-game
  (§12.4); scenario `types` chunks and scripts edit types (with `Type::backup/restore`). Those belong to the
  simulation, not the loader.
- **String hashes.** After loading, `Types::init` hashes every `name`; later name comparisons between two
  hashed strings compare only the case-insensitive 32-bit hashes (§4), so a hash collision would be treated as
  equal — reproduce the hash (§10.3) rather than comparing text.
- **Mods.** `GameMod::load` / `GameMod::initExistingData` re-run `Types::init`, `Tribes::init` and
  `Lands::init` over the mod's overlay files with the same rules; mod files can change counts only if they
  keep the asserts.

## 14. Reference tool and verification

`re/tools/rulesdump.mjs` (entry point) with `re/tools/rules/*.mjs`:

| Module | Content |
|---|---|
| `rulesxml.mjs` | MSXML-like DOM access (§4) and value parsers (§5) |
| `rulesimage.mjs` | rules-image codec (§3), replay scanner (`findImage`) |
| `rules/types.mjs` | constructor defaults, name keys, units (§7), `fill_relative_values`, is-lists, `finalize_init_all` |
| `rules/constants.mjs` | `CONSTANT_FIELDS` + `loadConstants` (§12) |
| `rules/tribes.mjs` | tribes, categories, string hash, display names, `get_preq`, `finalize_grafting` (§10) |
| `rules/buildings.mjs` | buildings and masks (§8) |
| `rules/techs.mjs` | techs, goods, item, crafts, bonuses, AI values, `largest_gather` (§9) |
| `rules/balance.mjs` | combat balance table (§11) |

```
node re/tools/rulesdump.mjs                                   # load + summary (asserts every element count)
node re/tools/rulesdump.mjs --json rules.json --image rules.bin
node re/tools/rulesdump.mjs --replay game.rcx                 # byte compare + per-field diff
node re/tools/rulesdump.mjs --replay game.rcx --start-tech 2  # per-game settings for §7.6
node re/tools/rulesdump.mjs --replay-json game.rcx image.json # decode a replay's image only
```

The loader throws on any count mismatch (85 techs, 28 epoch name sets × 8, 50 resources, 129 buildings,
364 units, 1 item, 55 crafts, 122 bonuses, 24 tribes, 9 lands), mirroring the compile-time asserts in
`Types::init`, `Tribes::init` and `Lands::init`, and warns on a Categories count mismatch (§10.2: the game
reports "Category data mismatch" and continues). The stock data passes every check.
The JSON holds every walked and non-walked field (names, graph names, mask names, footprint masks, categories)
for generating C++ tables or fixtures. Run time ≈ 0.5 s.

### C++ implementation

`engine/sim/` is a 1:1 C++17 port of the reference tool (namespace `ron::sim`, separate from the renderer's
`ron::Rules` in `engine/assets/rules.hpp`):

| File | Ports |
|---|---|
| `rules.hpp` | public API: `Type` (all classes in one struct, walked fields in image order), `Tribe`, `CategoryList`, `Constants`, `Rules`, `GameSetup`; `load_rules`, `start_game`, `write_image`, `read_image`, `find_image`, `read_replay` |
| `rules_xml.hpp/.cpp` | `rulesxml.mjs`: mutable UTF-16 DOM with the reference's parsing, MSXML text, upper-cased tag lookup, value parsers |
| `rules.cpp` | `rulesdump.mjs` `loadRules` / `startGame` (load order, count asserts, balance ages) |
| `rules_units.cpp` | `rules/types.mjs` |
| `rules_buildings.cpp` | `rules/buildings.mjs` (incl. masks.txt) |
| `rules_techs.cpp` | `rules/techs.mjs` |
| `rules_tribes.cpp` | `rules/tribes.mjs` (string hash, categories, display names, `get_preq`, `finalize_grafting`) |
| `rules_balance.cpp` | `rules/balance.mjs` |
| `rules_constants.cpp` + `rules_constants_table.inc` | `rules/constants.mjs` (the table is generated from `CONSTANT_FIELDS`) |
| `rules_image.cpp` | `rulesimage.mjs`: one templated walk drives the writer and the reader, like `DataWalk` |

`build/ron-rules <game dir> [--image out.bin] [--replay file.rcx|decompressed.bin] [--start-tech N] [--end-tech N]
[--game-rules N] [--flag8]` (`apps/rules/main.cpp`) loads the tables, writes the image and compares it with a
replay's (BYTE-IDENTICAL, or the first differing field and offset; it also checks that the decoded replay image
re-encodes identically). The sample replay is byte-identical, and `--image` matches `rulesdump.mjs --image` for the
default setup and for several start/end-age, game-rule-8 and flag-8 setups. `ron-check` prints a `rules:` summary line.
Deviations from the JS, all on paths the stock data never reaches: integer products wrap as int32 (the JS multiplies
doubles first); a zero divisor in `cdiv` gives 0, and in the military-epoch rescaling of `get_preq` it uses the
`tribes.mjs` integer path for both callers; out-of-range indices throw; doober mask cells stay -1 (the GraphicPieces
doober names are not loaded, as in the JS).

Suggested C++ test: load the stock XML, serialise with the §3 layout, compare with the image in each available
`.rcx` (scan for the 0x92 marker followed by type 0, as `findImage` does); replays from non-default game setups
exercise §7.6.

## 15. Open questions

1. **GameInfo at program start.** The tech AI values and balance ages are computed under whatever GameInfo the
   game holds when `Types::init` runs (also after a mod reload). The sample matches defaults; if saved
   preferences can carry a non-default starting technology into that moment, machines could load different
   tables. Needs a check of `GameInfo` initialisation order in `Main::main` / `Game::init_common_data`.
2. **GameInfo flag 8** (finalize_grafting): which game option it is. Take it from the replay's GameInfo.
3. Only one replay (default setup, no scenario, no mods) was available as an oracle. Replays with other start/end
   ages, scenarios (`Setup::build_game`, scenario `types` chunk) and Conquest (barbarian / substitute tribes)
   are untested.
4. Constants: whether string-table entry 0x9b14 is `CAPITAL_PLUNDER` or `CAPITAL_PLUNDER_ASSASSIN` (both read
   the same entry; both XML values are 500) (§12.3).
5. Balance flag rows: internal string #17 is empty in the shipped table, which makes every obj-mask row/column of
   balance.xml dead data; it carries a hash suggesting the original text was not empty (§11).
6. Buildings: source/order of the global `doober_names` list (visual only); `Text::get` return at `#`/EOF
   assumed empty (§8.8).
7. Uninitialised constructor fields: several UnitTypeData/TechTypeData fields are only set by `init`; all are
   written for the stock data, but a mod missing elements could leave heap garbage in the original (walked into
   replays), which a port cannot reproduce.
8. `ObjectTypeData::get_age_slow` for a type with no tech prerequisite returns 0; gaia types never get an age
   (they stay −1), consistent with the oracle.

## 16. Evidence index

Decompilation for this document: `re/decomp/rules`, `rules2`…`rules6`, `rules6asm`, `rules_asm`,
`build_rules`, `build_rules_asm`, `techs`, `rules_tribes`, `balance`, `constants_disasm`, `constants_writes`;
job files `re/jobs/dc_rules*.txt`, `build_*`, `techs_*`, `rt_*`, `balance_*`, `constants_*`. PDB layouts:
`re/types/rules_structs.txt`, `rules_paths.txt`, `constants_layout.txt`, `typeindex.tsv`, `rules_enums.txt`.

| Function | Address | Role |
|---|---|---|
| Game::init_common_data | 0058e750 | calls init_rules_data, init_non_rules_data, dump_rules_data |
| Game::init_rules_data | 0058ec60 | Constants, Categories, Tribes, map names |
| Game::init_non_rules_data | 0058fda0 | Types::init, Lands::init, Forms::init |
| Types::init | 00669cc0 | all type files, passes, asserts, Balance::init |
| Game::init_data | 0058dca0 | per game: Constants::init, init_civ_masks, finalize_grafting |
| Types::finalize_grafting | 00669840 | tribe masks, age, military_level |
| Game::run | 00584590 | write_rules / read_rules |
| Game::walk_rules_data | 00589550 | rules image |
| Types/Type/ObjectType/UnitType/BuildType/GoodType/TechType/SpellType::walk_rules_data | 00669800 / 00663190 / 0065fba0 / 0061d190 / 00631f50 / 0066fab0 / 0066d5c0 / 00675400 | image layout |
| SaveGame::walk_function / walk_test | 0043d730 / 0043d840 | raw bytes, marker byte |
| XML::init, XMLNode::get_element(s), XMLElement::get_text(_num), get_attrib(_num) | 00a279e0, 00a28090, 00a27720, 00a271b0, 00a25620, 00a27900, 00a27580 | XML access |
| String::convert_int, binary, hex, fraction, operator==, ignore, generate_hash | 00a15fc0, 00a1b080, 00a16060, 00a1d110, 00a1f140, 00a1ba80, 00a1b6b0 | value parsing |
| degrees_to_angle | 00a28eb0 | TURN_SPEED |
| Type::Type, ObjectType::ObjectType, UnitType::UnitType, BuildType::BuildType, GoodType::GoodType, SpellTypeData::SpellTypeData | 00663010, 0065f880, 0061d270, 006334d0, 0066ff80, 006749b0 | defaults |
| Types::unit_key, build_key, tech_key, good_key, object_key | 006694a0, 00669330, 00669610, 006691c0, 00668ae0 | name keys |
| Type::set_name, lookup_display_name, load_cost | 00663ae0, 006677f0, 00663ba0 | names, costs |
| ObjectType::load_support, load_upgrade, init_is_list, finalize_init_all | 00661160, 006615b0, 00661dd0, 0065f4a0 | object parts |
| ObjectTypeData::is, is_slow, get_age_slow; TypeData::get_preq | 0065f7d0, 00661ae0, 00661a00, 00668700 | predicates |
| UnitType::init, determine_roles, fill_relative_values, init_final_flags, init_spellcasters; UnitTypeData::get_military_level_slow | 0061ab50, 0061c320, 0061be50, 0061dc70, 0061aae0, 0061d4d0 | units |
| BuildType::init, init_masks, init_build_mask, init_doober_mask, mask_me | 00632340, 00632280, 006310b0, 00630cd0, 006312a0 | buildings |
| TechType::init, set_research, compute_ai_values, add_preq_ai | 0066c7b0, 0066cba0, 0066cdc0, 0066c350 | techs |
| GoodType::init, compute_largest_gather; Lands::init | 0066ea20, 0066e920, 0067e730 | goods |
| ItemType::init, SpellType::init, BonusType::init | 00677bc0, 00674a80, 00670280 | item, crafts, bonuses |
| Tribes::init, Categories::init, find_key | 006ef430, 006f21d0, 006f20e0 | tribes, categories |
| Balance::init, fill_tables, compute_modifier, return_pack, type_damage; ObjectData::get_damage | 00582d20, 005823f0, 00581cc0, 005821c0, 0057fb50, 00644130 | balance |
| Constants::init, get_item, get_fraction | 00569a90, 0057fa60, 0057f950 | constants |
