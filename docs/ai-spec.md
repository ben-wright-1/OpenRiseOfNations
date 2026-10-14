# The computer opponent (AI)

Status: 🟡 specified from the decompiles, with the disassembly checked (✅) wherever the decompiler was ambiguous:
setup and the personality draws, scheduling and the production state machine, the strategy census, sites, the make
list and purchases, the market AI, every production step (`production_ai_setup`, `found_cities`, `research_techs`,
`upgrade_units`, `create_units`, `create_buildings`, `make_stuff`), the produce_* functions and building placement,
armies and target selection, per-unit AI, the town bell and `Leader::diplomacy` (open items in §10.1.12, §12.11,
§16). Nothing is validated against a running game yet (no replay with an AI opponent exists on this Mac).

Evidence: decompiled C and disassembly stay local (gitignored): `re/decomp/ai/` (job `re/jobs/dc_ai_1.txt`, 139
functions), `re/decomp/ai2/` (helpers, `re/jobs/dc_ai_2.txt`), `re/decomp/ai/asm/` (`re/jobs/asm_ai_1.txt`), PDB type
dumps `re/decomp/ai/types_ai*.txt` (`re/jobs/types_ai*.txt`, DumpTypes.java). Older decompiles used:
`re/decomp/script_rt/Setup_build_game@005ac190.c`, `re/decomp/terrain/ScenarioEditor_init_terrain@009a19d0.c`,
`re/decomp/scenario_objects/Build_queue_up@00620f40.c`.

Read first: [script-spec.md](script-spec.md) §6.4 (the AI production scripts and their builtins),
[frame-spec.md](frame-spec.md) §4-§5 (where `strategy_all` and `Armies::process_all` run),
[production-spec.md](production-spec.md) (queues, `can_pay_cost`, `get_cost`, placement, `blocked_site`),
[economy-spec.md](economy-spec.md) (buckets, escrow, markets, `think_peasant`), [combat-spec.md](combat-spec.md)
(acquisition, `action_alarm`), [orders-spec.md](orders-spec.md) (groups and orders).

## 0. Conventions

- Addresses are `riseofnations.exe` VAs, names are PDB names. Pseudocode is paraphrased, never the decompiler text.
- int32 arithmetic, C truncating division (`/` rounds toward zero, also for the compiler's `(x + (x>>31 & m)) >> k`
  idiom) unless noted. **rnd(n)** = `game_random.get(0, 0xFFFF) % n` (the single simulation stream, replay-spec §12;
  the AI code calls `Random::get(game_random, 0, 0xffff)` directly and takes `%` itself). **coin** =
  `get(0, 0xFFFF) & 1` (written `& 0x80000001` in the code; the value is never negative). `internal_random` (chat line
  choice) is presentation only.
- `L` = `leaders.list[who]` (`LeaderData`, global `leaders` @e3a390, stride 0x6eec, 10 entries; the AI loops run over
  0..7). Field names and offsets are from the PDB (`re/decomp/ai/types_ai.txt`). Encrypted fields
  (`LeaderDataEncrypt`, state-walk.md) are written plain here: `bucket[g]`, `income[g]`, `rate[g]`, `resource_cap[g]`,
  `over_cap[g]`, `ages` (current age, 0 = Ancient), `epochs` (library techs held), `epoch[c]` (library level per
  category c).
- Goods: 0 food, 1 timber, 2 wealth, 3 knowledge, 4 metal, 5 oil. Tech category `cat` (TechType +0x14, rules-spec
  §9.3): 0 military, 1 civic, 2 commerce, 3 science / every non-library tech. `epoch[c]` is indexed by that cat.
- TypeIndex names from `re/types/rules_enums.txt` (TypeIndex enum): 50 PEASANTS (citizen), 414 VILLAGE, 417 FARM,
  418 WOODCUTTER, 419 MINE, 420 UNIVERSITY, 421/422 OILWELL/OILPLATFORM, 423..426 GRANARY/LUMBERMILL/SMELTER/REFINERY,
  427 BARRACKS, 428 STABLE, 430 SIEGEFACTORY, 431 FACTORY, 432 DOCK, 435 LIBRARY, 436 MARKET, 437 TEMPLE, 438 SENATE,
  439 TOWER, 443 FORTX, 447 AIRBASE, 526..542 wonders, 544..550 ages, 551..578 library techs, 623..628 governments.
  Tribes: 0 Aztecs, 2 Inca, 3 Bantu, 4 Nubians, 5 Greeks, 6 Romans, 7 Egyptians, 8 Turks, 9 Spanish, 11 British,
  12 Germans, 13 Russians, 14 Chinese, 15 Japanese, 17 Mongols, 19 Lakota, 21 Indians (`TribeIndex`).
- Type virtual slots (re/analysis/vt_types.txt; UnitType / BuildType slots from a local dump `re/decomp/ai/vt_types2.txt` —
  their vtable symbols are named `vftable_for_Type_`, which `DumpVtables.java` (exact name `vftable`) does not match): +0x0c `is_unit_type`, +0x14 `is_build_type`, +0x1c `is_wonder_type`,
  +0x28 `is_tech_type`, +0x34 `is_age_type`, +0x38 `is_epoch_type`, +0x48 `is_spell_type`, +0x60 `is(t, strict)`,
  +0x64 `is_city` for buildings (`BuildTypeData::is_city` = `is(414)`; folded `return 0` on the base Type), +0x78
  `get_cost(g, who, o, city, 0, 1, -1)`, +0x84 `can_pay_cost(who, city, o, total)` (production-spec §2.2.1: the
  number of copies affordable, 10 when nothing is charged); UnitType +0xec `get_age`, +0x10c `is_siege`, +0x114
  `is_missile`, +0x118 `get_wonder_value`; BuildType +0x90 `is_gather_type`, +0xfc `is_fort`, +0x100 `is_tower`.
- Difficulty: **`get_diff()`** = `LeaderData::get_diff` @6ec000: multiplayer (semaphore byte0 & 4) → `L.multi_diff`
  (+0x50); single player → `L.multi_diff` only when `semaphore byte1 & 0x10` (scripted rules, game_rules 10), not
  Conquer the World, and `multi_diff ≥ 0`; otherwise `GameInfo.setup.difficulty`. `DiffIndex`: 0 EASIEST, 1 EASY,
  2 MODERATE, 3 TOUGH, 4 TOUGHER, 5 TOUGHEST. `Leader::set_diff` @6d2470 just stores `multi_diff`.
- "AI leader": `(leader_flags & 0xc) != 4`, i.e. not a HUMAN (4) seat without COOP_SOLO (8). leader_flags2 bits:
  1 NO_CITY_DEFEAT, 2 UNIT_AI_OFF, 4 PROD_AI_OFF, 8 COMBAT_AI_OFF, 0x10 NO_EXPANSION, 0x20 LOCK_TRANSPORT.
- Object lists: units `objects.lists[who][0 .. unit_mark)`, buildings `[2000 .. build_mark)`, walls in a second range;
  "for every building" loops below walk both ranges in index order (`obj_base[1..2]` / marks at objects +0x1ec..).
- Wcoord helpers: `wd(x, y)` = `world.wdata[y·xs + x]` (`WData`, 0x1c bytes: `region` +4, `val` +0xc, `goods` +0xd
  (resource bit mask, bit g), `who` +0xf (territory owner, −1 none), `who2` +0x10). `danger[who][(y>>1)·reg_xs + (x>>1)]`
  (World +0x13c, filled by `GameDaemon::calc_danger` every 200 frames). World `xs`/`ys` (+0/+4), `map` (+0x30, map
  style), `sea_map` (+0x34: 0 land map … ≥3 mostly water).

## 1. Where the AI runs

```
Setup::build_game (load)          Leader::init per VALID leader, in start-slot order        §2
Game::run (once, new games only)  Leaders::init_production_script (compile ai/scripts/*.bhs)  script-spec §6.4
Game::do_frame, step 6 (frame-spec §4-§5; skipped in the editor)
  Leaders::strategy_all @6ed430   for who 0..7 with VALID|ACTIVE (flags & 3 == 3):
      Leader::check_explore        §11
      Leader::plan_strategy        §3 (full pass every 200/ai_speed frames, production steps in between)
      Leader::compute_score(0)     score (frame 0, then when (who + frame) % 10 == 0): unit / building / economy /
                                   upgrade / research parts, territory·1000/land_size; read by diplomacy
                                   (get_team_score) — sim state, not specified here (open question)
      Leader::diplomacy            §10
    then if semaphore byte1 & 2: Game::check_victory
  GameDaemon::process_all          (markets, groups; not AI)
  Armies::process_all @6f3b00      §12 military
Objects::process_all
  Unit::process → Unit::think ...  per-unit AI for AI_CONTROL units (§12.6); peasants/fishermen/merchants:
                                   economy-spec (think_peasant, think_fish, think_merchant, think_caravan)
```

The AI never goes through `CommandManager`: it calls `Group::action_*`, `Build::queue_up`, `Objects::init_build` and
the leader's market functions directly, inside `do_frame`. Every peer runs the same AI code in the same order, so the AI
is part of the lockstep simulation and every `game_random` draw below is on the shared stream.

Gates: `production_ai` does nothing for human seats (`flags & 4` without `& 8`), when `*GameAccess::ai_off` is set
(cheat / debug toggle `Game::action_cheat_ai_toggle`), or with leader_flags2 PROD_AI_OFF (4; scenario leader flag,
e.g. the tutorials' passive AI). `Armies::process_all` skips human seats and leaders with UNIT_AI_OFF | COMBAT_AI_OFF.
`*GameAccess::ai_speed` (int, 1 in normal games; cheats `action_cheat_ai_speed_*`) shortens the strategy period and
multiplies production accelerations (production-spec).

## 2. Setup

### 2.1 Order of the leader inits and the RNG stream

Leader flags (VALID|ACTIVE 3, HUMAN 4, COOP_SOLO 8, tribe, colour) are set from the GameInfo player slots by
`Game::init_rules_and_teams` @589bb0 in `Game::run` (after `Leaders::init` reset all ten leaders with tribe −1, which
returns before the AI part), so the human test inside `Leader::init` already sees the right seats. The AI draws
happen in the real `Leader::init(who, tribe, colour)` call, which depends on the game type (`Setup::build_game`
@5ac190, re/decomp/script_rt; `param_2` = scenario):

**Random map** (`param_2 == 0`):
1. `game_random.seed = GameInfo.seed`, one draw `world.seed = rnd(0xffff) + 1`, `Map::new_map` and the map generator
   (many draws), `Terrain::init`.
2. Start-slot order: `k = 0`; for 8 slots: if the map has `randomize_starts`, `p = get(0,0xffff) & 7` (one draw per
   attempt, rejection until an unused value) else `p = k`; the 8-int table written at `GameInfo + 0x618..` (decompiled
   as `info.player + 0x618`; presumably the `Game.start_list` / `start_index` permutation that
   `LeaderData::get_target` @6da000 reads ❓) receives the permutation.
3. **`Leader::init(who, tribe, who)` for each slot of that permutation whose leader is VALID** @6e3930: the
   personality draws (§2.2, §2.3) interleave leader by leader in permutation order.

**Scenario** (`param_2 ≠ 0`): `game_random.seed = GameInfo.seed` and the `world.seed` draw as above (the header chunk
then overwrites `world.seed` with `map_seed` and sets `world.sea_map = ai_sea_map`, which `random_personality`
reads), then `ScenarioRead::import` @9b1430 dispatches the top-level chunks **in file order**. The leaders chunk
(11) calls `ScenarioRead::load_leader_chunk` @9ac080 → `Leader::init(who, file tribe, who)` for each leader chunk
(id 0x25) in file order (GameInfo tribe when `semaphore byte3 & 4`), then copies the file's government, handicap,
human flag, difficulty, diplomacy (chunk 41), techs and resources over the defaults. The terrain chunk (2) runs
`World::compute_val` (draws) and `ScenarioEditor::init_terrain`, which **reseeds `game_random = GameInfo.seed`**
(replay-spec §12, world-spec §3) before the objects chunk. Every stock file stores the chunks as `11 leaders,
2 terrain, 3 items, 4 goods, 7 cities, 5 objects, …` (scenario-objects-spec §1), so **in scenarios the personality
draws come before the reseed**: they take values from the stream right after the `world.seed` draw (plus anything
`CliffsOut::init_class` draws, none expected ❓) and do **not** shift the simulation stream. A file with the leaders
chunk after the terrain chunk would draw after the reseed instead (none ships). The `build_game` permutation loop is
not run for scenarios. Conquer the World: `ConquestGame::build_conquest_game` (calls `Leader::init`, then
`init_terrain`).

Consequences for a port: random maps — the personality draws are part of the simulation stream (after map
generation); scenarios — run them in the leaders chunk, before the terrain reseed, so the values match while the
stream after the reseed is unaffected. (Loading a save restores the walked `Personality` and the RNG seed instead;
`init_production_script` is not run on load.)

### 2.2 Leader::init: the AI part

After the generic resets (diplomacy, stats, economy keys; economy-spec §5.7), `Leader::init`:
- `escrow_rate[0..5] = 40` if the leader starts with more than 2 cities + villages (also re-applied by every full
  strategy pass, §4.1).
- `prod_script = ""`. Human seat without co-op AI (`flags & 0xc == 4`): skip the rest of this list.
- `prod_script_run = 1` (+0x78c), `random_personality()` (§2.3).
- Player flags from `GameInfo.player[get_player()].flags`: 0x1000 (setup "rusher") → `pers.rush = 1`,
  `pers.raid = 1`; else 0x2000 ("boomer") → `rush = −1`, `raid = −1`, `civilians = 1`; else team style 3 and
  `get_team() == 1` → `rush = 1`, `raid = 1`. (`LeaderData::is_rusher` / `is_boomer` @6d61f0 / @6d6190 read the same
  bits.)
- Script choice (`int_str_array` entries 0x169b8 = `economic`, 0x169cc = `defensive`; script-spec §6.4):
  - patch version ≥ 9: if (`rush < 0` or (`rush == 0` and **coin** = 1)) and not tribe bonus 0x13 (Lakota) →
    `economic`; else `defensive` and `pers.early_army = 1`.
  - patch version < 9: if `rush ≥ 0` and (`rush ≠ 0` or **coin** = 0) and not Lakota → `defensive`, `early_army = 1`;
    else `economic`.
  - The coin is drawn only when `rush == 0` (short-circuit), before `has_tribe_bonus`.
- Then for everyone: `sites` cleared (10 entries), `make_list` initialised (11 entries), `mil_trainers`, `new_rares`,
  `oil_patches` emptied, starting stockpiles (economy-spec §5.7).

`Leaders::init_production_script` @6ed490 (once in `Game::run`) compiles `prod_script_path + prod_script + ".bhs"`
for AI leaders with a script name (script-spec §6.4).

### 2.3 Leader::random_personality @6cfd00

`Personality` (LeaderData +0x6dd4, 24 ints, all in −1..1 unless noted; walked raw in saves):

| off | field | off | field | off | field |
|---|---|---|---|---|---|
| +0x00 | rush (boom_vs_rush) | +0x20 | target | +0x40 | market |
| +0x04 | cities | +0x24 | strategy | +0x44 | scouts |
| +0x08 | upgrades (never set by the draws) | +0x28 | raze (set 0) | +0x48 | civilians |
| +0x0c | arms (−2..2) | +0x2c | spells | +0x4c | early_army (set 0, then by the script choice) |
| +0x10 | army | +0x30 | forts | +0x50 | friendly_human |
| +0x14 | army_size | +0x34 | nukes | +0x54 | alliance_human |
| +0x18 | raid | +0x38 | air | +0x58 | friendly_ai (= friendly_human) |
| +0x1c | invade | +0x3c | naval | +0x5c | alliance_ai (= alliance_human) |

Draws in exact order (`t` = `L.tribe`, `r3` = `rnd(3) − 1`):
1. `early_army = 0`; **`rush = r3`**.
2. Tribe adjustment (at most one extra draw):
   - t ∈ {14 Chinese, 2 Inca, 7 Egyptians, 4 Nubians, 5 Greeks}: if `rush == 1`: draw; if `rnd % 5 ≠ 0` → `rush = −1`.
   - else t ∈ {12 Germans, 8 Turks}: if `rush == 1`: draw; if `% 3 ≠ 0` → `rush = 0`.
   - else t ∈ {0 Aztecs, 3 Bantu, 17 Mongols, 6 Romans, 15 Japanese}: if `rush < 0`: draw; if `% 3 ≠ 0` →
     `rush = (world.sea_map < 3) ? 1 : 0`.
3. Opponents, j = 0..7 in order: if j is VALID|ACTIVE, j ≠ who, not `is_ally(j)`, and j's flags lack 0x10
   (WAS_RANDOM): if j's tribe ∈ {14, 13 Russians, 1 Maya} and `rush == 1`: draw; `& 3 ≠ 0` → `rush = 0`; else if j's
   tribe ∈ {0, 15} and `rush == −1`: draw; `& 1 ≠ 0` → `rush = 0`. (`rush` can change inside the loop, so later
   opponents see the updated value.)
4. If `rush == 1` and `world.sea_map > 2`: draw; `% 3 ≠ 0` → `rush = 0`.
5. **`cities = r3`**.
6. **coin**: 0 → **`arms = rnd(5) − 2`**; 1 → **`arms = r3`**.
7. **`army = r3`**, **`army_size = r3`**, **`raid = r3`**; if t ∈ {17 Mongols, 0 Aztecs}: draw; `% 3 ≠ 0` →
   `raid = 1`. If `setup.rush_rules == 8` → `raid = −1` (no draw).
8. **`invade = r3`**, **`target = r3`**, `raze = 0`, **`strategy = r3`**, **`spells = r3`**.
9. **`forts = r3`**, then if t == 6 (Romans) `forts = 1` (the draw still happens).
10. **`nukes = r3`**; **`air = r3`**, then t == 12 (Germans) → `air = 1`.
11. **`naval`**: `v = rnd(3)`, `naval = v − 1`; if t ∈ {15, 11 British, 9 Spanish} and `naval < 1` → `naval = v`
    (−1 becomes 0, 0 becomes 1).
12. **`market = r3`**; **`scouts = r3`**, t == 9 (Spanish) → `scouts = 1`; **`civilians = r3`**;
    **`friendly_human = r3`**; **`alliance_human = r3`**; `friendly_ai = friendly_human`,
    `alliance_ai = alliance_human`.

Draw count: 1 + (0..1) + (0..#opponents) + (0..1) + 1 + 2 + 3 + (0..1) + 4 + 1 + 2 + 1 + 5 = 20 + conditional draws.
`Leader::init` then may draw the script coin (§2.2) after the player-flag overrides.

Consumers (every read of +0x6dd4..+0x6e33 in the binary, InsnGrep `re/jobs/grep_ai_pers*.txt`, both register-relative
and `leaders`-absolute addressing): `rush` → production script argument (`production_ai`), `create_units`,
`upgrade_units`, `Army::release_mustering`; `army_size` → `create_units`, `upgrade_units`; `raid` →
`research_techs` (government family), `Army::find_target`, written by `process_taunt` (§10.3); `nukes` →
`create_units`, `upgrade_units`; `air` → `create_units`; `early_army` → `Army::find_target`,
`Army::release_mustering`; `friendly_human` → `diplomacy`. **The other 17 fields (cities, upgrades, arms, army,
invade, target, strategy, raze, spells, forts, naval, market, scouts, civilians, alliance_human, friendly_ai,
alliance_ai) are never read** — their draws matter only for the RNG stream and the save image.

### 2.4 Difficulty and handicap

- Difficulty enters through `get_diff()` (§0) in: the bucket clamp (§6.1), `found_cities` (city count vs. humans),
  `research_techs` (age pacing behind humans, library allowance), `upgrade_units` (the rnd draw when diff ≠ 2,
  §6.4), region strategy bits (§4.4), and the military code (§12).
- `L.handicap` (+0x4c) is a gather-rate handicap (economy-spec `get_gather_handicap`), not read by the AI code.
- No XML personality or difficulty table exists: all AI tuning constants are compiled in. The data the AI reads is
  the rules (`TechType.ai[11]` from `compute_ai_values`, costs, type flags), GameInfo setup bytes and the two scripts.

## 3. Scheduling: Leader::plan_strategy @6b9620 and the production state machine

Called every frame for each VALID|ACTIVE leader (also human ones; the census is shared by the HUD and scripts).

```
c = (who·25 + frame) % (200 / ai_speed)          // 200/ai_speed computed as a 64-bit division
if production_step (+0x788) != 0: production_ai(); return            // §3.1, one state per frame
if frame != 0 and c != 0:
    if c % 30 != 0: return
    t = make_list[0].t
    if t is a tech (544..628) and can_pay(0) and !has_tech(t) and !researching(t, -1, 0, 0): make_this(0)
    return
full strategy pass (§4); at its end, unless semaphore byte1 & 2: check_orphaned_buildings (§4.6),
    compute_sites(0) (§5), production_step = 1
```

So the full pass runs at frame 0 and every 200 frames (staggered by 25 per leader); the production states then run on
the following frames (one per frame, while `production_step` ≠ 0, which also suppresses the 30-frame tech retry).

### 3.1 Leader::production_ai @6c1960

```
if human seat (flags & 4 and not & 8) or *ai_off or leader_flags2 & PROD_AI_OFF: production_step = 0; return
effective_pop (+0x9e0) = queued_units() + control (+0x940) + 1
if step == 1 and (prod_script_run == 0 or setup.starting_resources == 8): step = 2
switch step:
 1: run the script (script-spec §6.4): run_script(prod_script, who+1, ref script_step, rush+2, 5);
    on success read the return r and write script_step back: r == 1 → step = 0, return;
    r == 3 or a run failure → prod_script_run = 0; then (r ≠ 1) step++ (→ 2), return
 2: step = 3; production_ai_setup() (§6.1); make_list.clear(); return
 3: step = 4; found_cities() (§6.2)
 4: step = 5; research_techs() (§6.3)
 5: step = 6; upgrade_units() (§6.4)
 6, 9: step++; create_units() (§6.5)
 7: step = 8; create_buildings() (§6.6)
 8: step = 9; if make_stuff() != 0: return; if starting_resources == 8: return; step = 0; return
10: step = 11; create_buildings(); return
11: make_stuff(); step = 0; return
default: step = 0; return
// cases 3, 4, 5, 6, 9 and 7 reach here:
if setup.starting_resources == 8: make_stuff(); make_list.clear()
```

`make_stuff` returns 1 when it bought item 0 (§6.7), so a successful first purchase earns a second round (states 9-11:
more units, more buildings, another purchase). `starting_resources == 8` ("unlimited"? ❓ the setup value whose
production-setup also marks every good "enough", §6.1) makes the AI spend after every step.

`queued_units` @6ce000: Σ over the leader's active buildings, over their queued items (count `queued` +0x82, entries
of 0x14 bytes at `build_queue` +0x8c), of `UnitType.control_cost` (+0x2f0) for unit types with `type_avail ≥ 4`.

## 4. The full strategy pass (census)

The pass recomputes the leader's statistics from scratch. It draws **no** random numbers; its outputs drive every later
step. Order matters only where noted (city fields written by several loops). Step list (`plan_strategy` body):

### 4.1 Resets
1. If cities + villages (`city_num` + `village_num`) > 2: `escrow_rate[0..5] = 40`.
2. For each own city slot 0..city_mark−1 that is VALID: `gatherers = busy = free = 0`, `peasant_dist = 100`.
3. Zero: active, combat, siege, non_siege, sea_combat, defense, attack, naval, air, missile, transports, fishermen,
   idle_fishermen, peasants, scholars, merchants, fighters, bombers, cruise, nuke, caras, free_peasants,
   xport_peasants, gatherers, attacked, full_cities; `filled_gather_slots[0..5]`.
4. `my_team_terr = get_team_terr()`; over other ACTIVE leaders j (j ≠ who, not mutually allied — diplos 2 both
   ways): `other_team_terr = max(team_terr(j))`, `min_other_team_terr = min(...)` (first value initialises).
5. `ally_mask = bit(who)`; scripted rules or CTW → `|= ScenarioData::ally_mask[who]`. If `has_preq(ALLY_LOS)` or
   `setup.reveal_map ≠ 0`: for each ACTIVE j ≠ who: mutual alliance → `ally_mask |= bit(j)`; if `treaties[j] & 1`
   (met) is clear: scan j's buildings and walls (object ranges 1 and 2, from `obj_base`) for the first one that is
   active (vslot +0x0c `is_active`) and `is_seen(who, 0)` (vslot +0x48); found → mark met both ways
   (`treaties[j] |= 1` on both leaders) with a presentation `say_meet`. (fog-spec §8 covers the mask.)
6. `invaders[who]` of every VALID leader's row = 0 (`leaders[j].invaders[who]`, +0x290: "my combat units in j's
   territory").
7. Region arrays, r = 0..126: `reg_active`, `reg_combat`, `reg_attack` = 0; r < 63: `reg_naval`, `reg_transports`
   = 0; r < 64: `reg_defense`, `reg_attacked`, `reg_peasants`, `reg_free_peasants`, `reg_xport_peasants`,
   `reg_gatherers`, `reg_gather_slots`, `reg_land`, `reg_known_rares`, `reg_unpack_merch` = 0,
   `reg_cities[r] = reg_buildings[r][VILLAGE] + [TOWN] + [METROPOLIS] + [FORBIDDENCITY]` (the stored per-region
   building counts).
8. Rares: for each entry of `new_rares` (good object indices): unless (who < 8 and reveal_map ≠ 3 and the good is
   neither revealed (`flags & REVEAL_ENEMY`, or `num_units[FOUCHE 371]` ≠ 0, +0x59e4) nor in a cell seen by
   `ally_mask`): set the
   good's known bit for who and, if the cell's territory owner is −1, who or a mutual ally,
   `reg_known_rares[region]++`.
9. Own cities: `in_port = 0`.

### 4.2 Unit census (own units, slot order)
For each VALID unit u that is a captain (`is_captain`), not a decoy (`unit_masks` +0x68 `& UNIT_DECOY 1` clear) and
whose type has `control_cost` (+0x2f0) ≠ 0:
- Region `R`: on the map: `wd(u).region` or `region2` (the second region of a bridge / dock tile when the tdata bits
  say so); garrisoned: the region of the containing building (for a sea building (domain 1), the first land-or-sea
  cell of its footprint whose region ≥ 64, i.e. a sea region; fallback: the building's own cell).
- `active++`. Strength `s = attack() / 10` (Unit vslot +0x120 `UnitData::attack`), or 10 for non-land domains.
- Combat role (`role & 0x10000`): `attack += s`, `combat++`; `is_siege` (UnitType vslot +0x10c) ? `siege++` :
  `non_siege++`.
- Domain 1 (sea): `naval += s`, `sea_combat++` if s ≠ 0; `carry` (+0x2d4) ≠ 0 → `transports++`; fishing boat
  (`+0x2b8 & 4`) → `fishermen++`, idle (`unit_masks & 0x80000`) → `idle_fishermen++`.
- Domain 2 (air): missiles (`role & 0x8000000`) → `missile++`, then `is(V2ROCKET 313)` → `cruise++` else
  `is(NUCLEARMISSILE 315)` → `nuke++`; else `air++`, `is(BIPLANE 287)` → `fighters++` else `is(BOMBER 304)` →
  `bombers++`.
- A land combat unit (role & 0x10010 == 0x10000) standing in territory of another VALID leader k:
  `leaders[k].invaders[who]++` (+0x290; `dow` +0x270 is only changed by human war declarations).
- Types 52/53 (SCHOLARS): `scholars++`; inside a UNIVERSITY → `filled_gather_slots[3]++`. Types 61/62/400
  (MERCHANT, MERCHANTDUTCH, FURTRAPPER): `merchants++`; not idle → `reg_unpack_merch[R]++`. Else `is(CARA 59)` → `caras++`.
- Citizens (type 50/51): `c = find_city(x, y, SEARCH_FRIENDLY, who, …, 0x200 radius, FILTER_ALL)`; `peasants++`.
  - Off the map (inside something): inside an oil platform (422) → `filled_gather_slots[5]++`; inside a land transport
    whose target is set: count `reg_xport_peasants[region of the transport target]++`, `xport_peasants++`.
  - On the map, R < 64: `reg_peasants[R]++`; if `reg_cities[R] == 0` and the current order is not BUILD_AT (6):
    `reg_xport_peasants[R]++`, `xport_peasants++` (citizens in a region without a city count as "to be shipped").
  - No order, or order kind (UnitOrder vslot 0x10, `OrderIndex`) NONE 0 / EXPLORE_TO 3: `free_peasants++`,
    `reg_free_peasants[R]++` (R < 64), city c: `free++`, `peasant_dist = min(peasant_dist, objects.find_dist / 0x300)` (the
    distance from the citizen to c found by that `find_city`, in wcoords).
  - Else city c: `busy++`; order 7 (GATHER): the gather target's type FARM → `filled_gather_slots[0]++`,
    WOODCUTTER → [1], MINE → [4], OILWELL/OILPLATFORM → [5]; `busy--` on c and `busy++` on the target building's
    city; `gatherers++`; the city `gatherers++` and `peasant_dist` min go to the target building's city when it has
    one (the city index variable is overwritten; port), else to c; `reg_gatherers[R]++`.
    Order BOARD_SHIP 8 or CAST_SPELL 14: `reg_xport_peasants[region of the order target]++`, `xport_peasants++`.
- Region totals, when (land unit and R < 64) or (sea unit and R ≥ 64) or air unit: `reg_active[R]++` (127-entry
  arrays); combat role: `reg_attack[R] += s`, `reg_combat[R]++`; sea units with R > 62: `reg_naval[R % 63] += s` and,
  transports, `reg_transports[R % 63]++`.

### 4.3 Building census
For each VALID building/wall (both ranges, index order): region `R = wd(b).region`.
- Active buildings whose type `is_gather_type` (BuildType vslot 0x90): FARM → slot 0, WOODCUTTER 1, MINE 4,
  UNIVERSITY 3, OILWELL/OILPLATFORM 5: `slots[k] += gather_max` (+0x80).
- Land buildings (domain ≠ 1) in R < 64 with `attack` (+0x1e8) ≠ 0: `d = is(TOWER) ? 1 : (vslot 0xfc is_fort ? 2 : 0)`;
  `defense += d`, `reg_defense[R] += d`.
- Then: `gather_slots[k] = slots[k]` and `gather_slots_high[k] = max(...)` for k ∈ {0,1,3,4,5} (k = 2 wealth is not
  written); highs: `peasant_high`, `scholar_high`, `caravan_high`, `merchant_high`, `army_high` (= combat),
  `city_high`, `village_high`, `population_high` (= pop).
- `resources_controlled` = number of rares owned (bit count of `rare`, cached through the BitMask "valid" flag).

### 4.4 City and region evaluation
For each own VALID city: `city_pop_high = max(city.pop)`; level index from the city building (VILLAGE 1, TOWN 2,
METROPOLIS / FORBIDDENCITY 3); radius `rr = min(64, city_center_radius + (level−1)·city_center_pop_radius
(+ indians_city_radius for tribe bonus 0x15))` (the "scouted − 64" of the decompile is a mistyped address computation)
then `n = (rr + 2) / 4` (circle index); clear the city's
`ocean`, `ocean_filled`, `land`, `filled`, `dock_tile`, `ter[6]`, `space[3]`; `reg_gather_slots[city.reg] +=
City::count_gather_slots()`; city under attack (`city_flags & 2`) → `attacked++`, `reg_attacked[reg]++`.
Then walk the circle offsets `0 .. circle_radius[n+1]−1` (tables @cb7e91 / @cbb0e1) around the city's wcoord; for each
in-bounds cell whose territory owner is −1 or who:
- water cell (flags & 0x100 clear and land type 1 or 2 ❓): `ocean++`; if the cell's region has ≥ 2 coasts or is at least
  `world.size/10` big, and the city has no dock tile yet, and `is_dock_tile` → `dock_tile = 1`.
- land cell with `k + 1 < circle_radius[n]` belonging to the city's region (the decompile's `goody_factor` is the
  WData `region` at +4):
  if the cell is free of `0x70` flags: `land++`; `q = check_building_wcoord(cell, who, 0, 0, 1, 1)`; for sizes
  2..min(q, 4): `space[size−2]++`; `q < 4` → `filled++` and next cell. Cells with q = 4 and impassable (0x70) cells
  call `World::gather_at(cell, single = 1)` and raise `ter[g] = max(ter[g], found)` (port, from the decompile).
- After the walk: `land − filled < 2` → `full_cities++`; `reg_land[reg] += land − filled`.

Diplomacy counts over j = 0..7 (ACTIVE, ≠ who, `treaties[j] & 1` met): `wars++` if either side's `diplos` is 0,
`allies++` if both are 2.

Region strategy (`strategy[64]`, ushort bit flags): for r = 0..63: `reg_wars[r] = reg_allies[r] = reg_neutrals[r] = 0`,
`strategy[r] = 0`;
- `cities_all = Σ_j VALID reg_cities_j[r]`. If `cities_all·50 < region.size`: `strategy[r] = 1` (room to expand),
  else if own `reg_cities[r] ≠ 0`: `strategy[r] = 8`, else next r.
- If own `reg_cities[r] ≠ 0`:
  - for j = 0..7, ACTIVE, ≠ who, met (`treaties[j] & 1`), with `reg_cities_j[r] ≠ 0`:
    - at war (either `diplos` 0): `active_wars++`, `active_wars_with |= bit(j)`, `reg_wars[r]++`; then if
      `leaders[j].attack > attack` and (`r == home_reg` or `reg_cities_j[r] < reg_cities[r]`): bits `|= 4`, `&= ~(8|2)`;
      else `|= 2`, `&= ~4`. Then `get_diff() < 2` → `|= 4`, `&= ~2`; else `starting_resources == 8` → `|= 2`, `&= ~4`;
      else unchanged.
    - mutual alliance → `reg_allies[r]++`; otherwise `reg_neutrals[r]++`.
  - Water: `sea_map < 1` → clear 8; `sea_map == 2` and `strategy & 6 == 0`: for r2 = 1..63, r2 ≠ r, with
    `region[r2].size ≠ 0`, `region[r2].flags & 8` and own `reg_cities[r2] == 0`: if no VALID leader has cities in r2 →
    `|= 8` (continue scanning); `sea_map ≥ 3` and `strategy & 6 == 0` → `|= 8`.
  - Armies (AI leaders, `flags & 0xc ≠ 4`): count own armies (16 slots) with `valid`, `status & 1` (MUSTERING) and
    `reg == r`, plus those with `status & 0x20` (DEFENDING) and `reg == r`; if the sum < 2: over own VALID
    cities c with `c.reg == r` that have no MUSTERING army homed at them (`army.city == c`): `score =
    level·20·(4·(barracks + siege factories + stables) + 1 + docks)` (`count_buildings(…, 0, 1)`), ÷3 when the city is
    under attack, × (`find_army(who, c.x, c.y, −1, …) < 0 ? 12 : find_dist/4 + 6`); the best (`≥`, so ties → later
    city) gets `Armies::init_army(who, c)` (§12.3).

Bits: 1 = expandable, 2 = defend ❓, 4 = threatened/attack ❓, 8 = naval or contested. 🟡 names are guesses; the
consumers are `found_cities`, `create_units`, `create_buildings` and the army code.

### 4.5 End of the pass
Unless `semaphore byte1 & 2` (❓ a special game mode; `Leaders::strategy_all` runs `check_victory` under the same
bit): `check_orphaned_buildings()`, `compute_sites(0)`, `production_step = 1`.

### 4.6 Leader::check_orphaned_buildings @6c9f20 (AI leaders only, no RNG)
A local `Group` is used for orders. For each own building (both ranges, index order) that is VALID:
1. If `build_masks & 1` (PATHING; used here as a "re-check" flag ❓ who sets it): clear it; if the building is not a
   city building (`flags & 0x20` clear) and (`is(TOWER)` or `is(FORTX)` or `is(AIRBASE)` or the type's `attack`
   (+0x1e8) ≠ 0 or `num_buildings[type] > 1`): put it alone in the group, `action_disband(0)`; next building. (This
   also applies to finished buildings.)
2. If not active (an unfinished site), `R` = its region. Scan own units 0..unit_mark−1: citizens (50/51, on the map)
   whose current order (`update_action`) is BUILD_AT (6) on this site: if the citizen stands in R, or the site is not a
   land building (domain ≠ 0) → the site is being built, next building. Otherwise (a builder stuck in another region
   on a land site) `clear_orders()` on it and remember that one was cleared; keep scanning. If one was cleared → next
   building (nothing else this pass).
3. Nobody works on the site:
   - not started (`job_counter` +0x48 == 0) and `danger[who]` at the site > 0 → disband it.
   - else if `danger ≤ 0`: the first own citizen (slot order) in region R that is on the map and idle (no order or
     order kind 0) or gathering (7) gets `action_swarm_around(site, who, QUEUE_NEW, BUILD_AT, 1)` (clears ORPHAN,
     production-spec §3); next building.
   - otherwise (danger > 0 and started, or no such citizen): `build_masks |= ORPHAN (0x2000)` (production-spec: the
     site then decays / is disbanded by `take_damage`).

## 5. Sites: candidate city locations

`Sites` (LeaderData +0x6e34): 10 × `Site {wx, wy, val, reg, dist, rank}` (0x18 bytes). Kept across passes; walked in saves.

### 5.1 Leader::compute_sites(force) @6cc950
```
if frame == 0: zero all 10 sites
else for each site i: if rank < 6 or val < 1: zero it
     else re-evaluate: c = reg_cities[site.reg] ? find_city(near the site centre, SEARCH_FRIENDLY, 0x200) : -1,
          o = c ≥ 0 ? city's building : find_unit(PEASANTS, friendly, 0x200) (nearest citizen);
          compute_site_stats(site.wx, site.wy, c, o, site.reg, &val, &dist, 0, &wx, &wy) → write val, dist, wx, wy
if !force and the leader is a human seat without co-op: return (keeps the re-evaluation only)
for region r = 0..63 with reg_peasants[r] ≠ 0:
    centre = region.coords[0] (first wcoord of the region list, +0x6c), as Coord (·0x300 + 0x180)
    c / o as above for that centre
    step = 2, start = 0
    if region.size > 199:
        k = (world.xs ≥ 2) ? rnd(world.xs) : 0                          // draw A
        step = max(10, size / (world.xs − k/2))
        start = site_mark (+0x7e0) % step
        q = step / 4; site_mark += 3 + (q ≥ 2 ? rnd(q) : 0)             // draw B (always: step ≥ 10)
    for i = start; i < size; i += step:
        (wx, wy) = region.coords[i]; compute_site_stats(wx, wy, c, o, r, &val, &dist, 1, &wx, &wy)
        if some site has the same (wx, wy) and val > 0: continue
        slot = -1; cur = 0
        for j = 0..9: v = sites[j].val
            if not (cur < v and (cur > 0 or val ≤ v)): slot = j; cur = v      // i.e. take j when v ≤ cur, or cur ≤ 0 and v < val
        if slot ≥ 0: sites[slot] = {wx, wy, val, r, dist}      // = the lowest-valued slot below val, ties → later slot
rank[i] = #{ j : sites[j].val ≤ sites[i].val }   (1..10, ties raise the rank)
```
`site_mark` also counts make_stuff passes without a city item (§6.7). Draws: 2 per region of size > 199 with own
citizens, regions in index order. `compute_site_stats` itself draws nothing.

### 5.2 Leader::compute_site_stats(wx, wy, city, o, reg, &val, &dist, scan, &bx, &by) @6cd040
Returns `val = 0` early when: the cell is impassable (tdata flags & 0x100), not `was_seen` by who, owned by another
leader (CTW exception: allowed with `local = 1` when the leader has no cities and the owner is an ally), or ocean.
1. `best = wd.val` (the terrain value, `World::compute_val`); 0 if `VILLAGE.blocked_site(x·0x300, y·0x300)`.
2. Walk the 25 offsets `move_x/move_y[0..24]` (5×5): for in-bounds seen cells: water/impassable → `water++`,
   `bad = max(bad − 1, 0)`; land owned by nobody → `bad += 1` (+2 if the leader has > 2 cities); owned by the team-style-2
   target → `enemy++`, `bad += 2`; owned by a non-ally → `bad += 2`. For own (or allowed allied) land cells with
   `scan`: `forts += find_fort()`; `v = (val + val_E + val_S + val_SE of the 2×2 block)/4 + height/25`
   (`find_tcoord_z`); if `v > best` and `!VILLAGE.blocked_town(cell)`: `best = v`, `(bx, by) = cell`.
3. `best < 1` → return 0. If `sea_map > 2` and the leader has no DOCK (incl. its upgrades): if any of the 40 offsets
   `move_x/y[81..120]` around the original cell is ocean → `best ·= 30`.
4. `v = best·250 / (water + 1)`; 1 city: `v = (min(bad, 9) + 7)·v / 8` if bad ≠ 0; 2 cities: same with min(bad, 4);
   else forts ≠ 0 → `v = v·3/2`. `v = (enemy + 2)·v / 2`.
5. Edge penalty unless map style 0xc or 0x11: with < 3 cities, `x < xs/5` or `x > 4·xs/5` → `/4`; and unless
   `ys/5 ≤ y ≤ 4·ys/5` → `/4`; with ≥ 3 cities the band is 1/10..9/10.
6. `v /= max(1, danger[who] at the cell)`. Territory: owner −1 → `×2`; non-ally → `×4`; ally → `/2`.
7. Unless map style 0x14: own `reg_cities[reg]` 0 → `×2`, 1 → `×2`; `reg_land[reg]` 0 → `×4`; `< 3` → `×4`;
   `reg_land < reg_cities` → `×2`; `reg_land < 2·reg_cities` → `×2`.
8. No cities yet: a friendly citizen within 0 range of the cell (`find_unit(FILTER_TYPE PEASANTS)`) → `v /= objects+0x1fc/0xc00 + 1` ❓;
   unless starting_resources == 8, at least one of the 5 `corner_x/y` cells must have goods bit 2 (wealth ❓) or
   `val = 0` (return).
9. Goods: for g = 0..5 available: cell has good g → `has |= econ[g] & 6`, and `econ[g] & 2` → `×3/2`, `& 4` → `×3/2`;
   else `miss |= econ[g] & 6`. `has == 0` → `×2/3`; `miss & ~has` → `best & 3 == 0 ? /2 : ×3/4`.
10. Distance: `o < 0` → `f = 3`, `dist = 9`; else `f = 1`, and if a city exists: for each own city (slot order):
    `d = vector_dist(cell − city wcoord)`; `d < 5` → `v /= 2`; `d < 8` → `v /= 2`; `f += (cities > 2 ? min(d, 10) :
    d)`; `dist = 2·f²`. ✅ An own city slot that is not VALID or lies in another region than `reg` makes the function
    **return** with `val = 0` (the disassembly jumps to the epilogue): once a leader has cities in two regions, no
    site in a region with one of its cities scores (original behaviour, keep it).
11. `v = f·v`. Unless map style 0xc/0x11, if the leader has 1 city or team style 2: `s = Σ` over other ACTIVE non-allied
    leaders (team style 2 with ≥ 2 cities: only `get_target()`) of `vector_dist(cell, their capital) / num_nations`
    for each of their VALID capital cities (`city_flags & 0x11 == 0x11`); team style 2 → `s ·= 100`; `s ≠ 0` → `v /= s`.
12. `local` (allied territory) → `v /= 10`. Output `val = v`, `dist`.

## 6. The production steps

### 6.0 The make list (MakeList @ LeaderData +0x6ec8)
11 `MakeObject {t, val, escrow, city, up, o, num, cat, wx, wy}` (0x28 bytes). Slots 0..3 hold the four best items
by `val` (0 best); slots 4..10 hold the best item per category (slot index = `cat`: 4 commerce techs/…, 6 towers,
8 science/other techs, 9 civic techs and cities, 10 military techs; units and buildings use their own cat values,
§6.5-6.6). Cleared (t = −1, val = −1, city = −1, o = −1, num = 1, rest 0) in production state 2.

`MakeList::make_me(t, val, escrow, cat, city, up, _, num, wx, wy)` @6c9be0:
```
if val > slot0.val: slot0 = item (o = -1); clear slots 1..3 whose t == t       // slot 0 is overwritten, not shifted
else for i = 1..3:
    if slot[i].val ≤ val: shift slots i..2 down one (slot 3 falls off); clear slots i..3 whose t == t;
                          slot[i] = item; break
    if slot[i].t == t: break                       // a better entry of the same type exists
if slot[cat].val < val: slot[cat] = item          // category slot
```

### 6.1 Step 2: Leader::production_ai_setup @6c83e0
1. `worst_good = best_good = shortages = 0`. `starting_resources == 8`: `econ[0..5] |= 8`; return.
2. Bucket clamp for `get_diff() < 3`: `A = 544 + ages` (next age); if A is an age type: `cap = max(300, max_g
   A.get_cost(g))`, then ×3/2 (diff 0), ×2 (diff 1), ×5/2 (diff 2); else `cap = 11000`. For g = 0..5 with
   `bucket[g] > cap`: `escrow[g] = escrow[g]·cap / bucket[g]`, `bucket[g] = cap`. (Easy AIs cannot hoard.)
3. Rates: for g = 0..5: `econ[g] = 0`; `rate[g] = min(get_mod_resource_cap(g), income[g]) / 16`; for available goods
   track `worst_good` (strict min) and `best_good` (strict max); `rate < 30` → `econ[g] |= 1`, `shortages++`.
4. Need bits per available good (unavailable → `econ[g] = 0`):
   `lo = min(cities·15, cap/32)`, `hi = min(cities·30, (cap/16)·4/5)`; cities > 4: `lo = cap/32`,
   `hi = min(175, (cap/16)·3/4)`; `lo = min(lo, 250)`, `hi = min(hi, 350)`. For g ∉ {food, timber} with cities < 3:
   `lo = 20`, `hi = cities·20`, and clear bit 1 (shortages−−). If `income[g] < cap`: `rate < lo` → `|= 2`,
   `rate < hi` → `|= 4`, and a rate below `hi` is done; otherwise (at cap, or rate ≥ hi) `|= 8` (enough).
   (`cities` = `city_num`, `cap` = `get_mod_resource_cap(g)`.)
5. With ≤ 2 cities, concentrate: bit 4: if food has it and the leader is not Lakota → clear it on goods 1..5; else if
   timber has it → clear it on food and goods 2..5. Bit 2 likewise, but the loser's bit 2 becomes bit 4 instead of
   being dropped.
6. `market_speculation()`.

`get_need(g)` @6cdc80 (used by the creators): `econ & 8` → 0, `& 1` → 4, `& 2` → 3, `& 4` → 2, else 1.

#### Leader::market_speculation @6c8110 (no RNG)
Only with (tribe bonus 4 (Nubians) or `has_preq(BUY_SELL)`) and `has_market()` and no nuke embargo and
starting_resources ≠ 8:
1. For available goods: `escrow[g] > 4000` → `escrow[g] = 2000`. `p = 2` if any bucket < 100, else 1 if any < 200,
   else 0.
2. Sell pass, g = 0..5 except wealth and knowledge: if `bucket[g] − escrow[g] ≥ 2000 / 2^p` and the sell price
   (`calc_market_prices` second output) ≥ `75 / (p + 1)` → `do_sell(g)` once.
3. Buy pass, same goods: buy price `b`: if `bucket[wealth] − escrow[wealth] ≥ b`, `bucket[g] < 2000`, `b ≤ 200`,
   and (`bucket[g] < 500` or `b ≤ 25`) and (`< 200` or `b ≤ 50`) and (`< 100` or `b ≤ 100`) → `do_buy(g)` once.
(An embargo instead calls `tell_embargo`, presentation.)

### 6.2 Step 3: Leader::found_cities @6c7a60
Skipped with leader_flags2 NO_EXPANSION (0x10). Only `t = VILLAGE` is considered (the loop runs 414..414; the
FORBIDDENCITY branch is dead):
1. Need `get_city_limit() > get_total_cities()` (the city limit; for 531 it would skip this check), `type_avail(414)
   == 4` and (`free_peasants` or `gatherers` ≠ 0).
2. Human pacing (non-co-op leaders, when `max_human_cities() > 0` = h): diff 0/1: stop if `total_cities ≥ max(h, 2)`;
   diff 2: stop if `total_cities ≥ max(h + 1, 2)`.
3. For each site i (0..9) with `val > 0`, region r with free or gathering citizens (`reg_free_peasants[r]` or
   `reg_gatherers[r]`), and not (`sea_map ≥ 4`, map style ≠ 0x14, 2 ≤ cities ≤ 3, r == `home_reg`), and
   `!VILLAGE.blocked_site(wx·0x300, wy·0x300)`:
   - `v = site.val`; own cities in r = n: n == 0 → `v ·= 4`; else `v = (reg_pop[r] + 1)·v / (2n)`, and `/2` unless
     `strategy[r] & 1`.
   - If no VILLAGE is queued (`num_queued[VILLAGE]` == 0, +0x5d5e): `×2` if `reg_pop[r] ≤ reg_peasants[r]`; `×2` if
     `n ≤ reg_free_peasants[r]`.
   - Proximity veto: for each VALID leader j that is who or has cities in r: if any of j's city buildings
     (`flags & 0x20`, range 2000..) is within 5 wcoords of the site (octile-style distance:
     `max + min²/(2·max)`, or `(max + 2·min)/2` beyond 60000) → skip the site; a site with `v == 0` is skipped too.
   - `reg_land[r] < n` → `×2`; `reg_land[r] < 2n` → `×3/2`.
   - City count: `lim = epoch[1] (civic) + 1` (+ `bantu_city_limit` for tribe bonus 3 when epoch[1] ≠ 0;
     + `pyramids_city_limit` with PYRAMIDS); `total_cities < lim` → mark "queued a city" and `v ·= 30`.
   - Pop pressure (pop_cap < 200): `effective_pop > 3·pop_cap/4` → `v = ((n5 + 2)·v)/(n5 + 1)` with
     `n5 = (cities + villages)/5`; `effective_pop > pop_cap − 4` → `×3/2`.
   - `strategy[r] & 5 == 1`: with `C = cities + villages`, `P = peasants`: `P ≥ 5C` → `×2`, else `P ≥ 3C` →
     `×3/2`; then `P ≥ 2C`: `C < 4` → `×2`, `C < 8` → `×3/2`.
   - `w = v·10` (negative → 999999); danger at the site > 0 → `/3`; territory owner ≠ who → `/3`.
   - `m = VILLAGE.can_pay_cost(who, −1, −1, 1) < 1 ? 64 : 256`; `make_me(VILLAGE, ((w/256)·m/256) / (num_queued[VILLAGE] + 1),
     escrow 1, cat 9, city −1, up 0, num 1, site.wx, site.wy)`.
4. If something was queued, `make_list[0].t ≠ −1` and `make_stuff()` ≠ 0 → `make_list.clear()`.

`place_city_with_cost` (script builtin) also ends in `found_cities` (script-spec §7).

### 6.3 Step 4: Leader::research_techs @6c6ba0
Draws: up to 2 per government tech (below). Elimination games (`setup.elimination == 1`): only the leader that owns
the capital found by `find_capital` researches.

```
lowest = get_lowest_epoch(); over = #{available g : over_cap[g] ≠ 0}
for t = 544 .. 628 (ages, library, final, plain techs, governments), index order:
    if tech_avail(t) ≤ 3: continue
    w = TechType.where (+0x40); need w ≥ 0 and (num_buildings[w] + get_buildings(w.to, …)) ≠ 0
    B = ((pop·200) / max(1, cities + villages)) · infra_mod (+0x7a4) / 256
    [special mode starting_resources == 7 and starting_technology == 8: MISSILE_SHIELD shortcut, see note]
    ages (544..550), ✅ disassembly:
        knowledge available and cost_k(t) > 4·bucket[3]/3 → skip
        human pacing (non-co-op, max_human_age() = h ≥ 0), a = t − 544:
            diff 0/1: skip if a > h − 1; s = best_human_age_stamp(t) (earliest frame a human reached that age);
                      skip if s ≥ 0 and frame − s < (who + (diff 0 ? 16 : 8))·225
            diff 2:   skip if a > h; s = best_human_age_stamp(t − 1); skip if s ≥ 0 and frame − s < (who + 20)·225
        B ·= (lowest > a) ? 600 : 10
    library techs (551..578):
        knowledge check as above
        if ages < setup.ending_technology and t ≠ THE_ART_OF_WAR (572) and not (cat 3 and tribe 9 Spanish):
            next = 544 + ages; if next is an age type:
                allow = techs_per_age(next); cat 0 and effective_pop ≥ 7·pop_cap/8 → +4; cat 2 and over > 1 → +4
                diff 0/1: research only if epochs < allow + 2 and epoch[cat] < allow
                diff 2:   only if epochs < allow + 6 and epoch[cat] < allow + 1        (diff ≥ 3: no limit)
        epoch[cat] == lowest → B ·= 10; else epoch[cat] > lowest + 1 → skip
        cat ∈ {1, 2} and setup.victory ∈ {8, 9} → ×30; sea_map > 1 and TRANSPORT_BONUS (686) needs t → ×30;
        t ∈ {COINAGE 559, EMPIRE 566} and has CLASSICAL_AGE → ×20
    if cost_k(t) < bucket[3] − 100: B ·= 10
    S = 1 + ai[0]/10 + Σ ai[1..10]                       (TechType.ai[11], rules-spec §9.3)
    S += (team_style ∈ {0, 8, 11}) ? ai[10] : −ai[10]
    full_cities > (cities+villages)/2 → S += 2·ai[5]; else full_cities ≠ 0 → S += ai[5]
    my_team_terr < other_team_terr → S += ai[5] (·2 if also < min_other_team_terr)
    P = active_wars == 0 ? ai[5] + ai[1] : ai[0]/3
    Q = sea_map == 0 ? −ai[2] : ai[2]·sea_map³
    V = (P + Q + S)·B
    cat 0: V /= 10; if pop_cap < 200: effective_pop > 5·pop_cap/6 → ×20, else epoch[0] > 1 → /10
    cat 2: epoch[2] == 0 → ×3; library or age tech: n = #{g ≠ knowledge available : income[g] ≥ 9·cap/10};
           V = n == 0 ? V/10 : V·n²
    else (cat 1, 3) and get_diff() ≥ 4 and cat 3:
           economy lines: AGRICULTURE..FOOD_INDUSTRY (612-614) → g 0, CARPENTRY..PAPERMILL (609-611) → 1,
           METAL_ALLOYS..STEEL (620-622) → 4, LITERACY..INSTITUTIONAL_RESEARCH (615-618) → 3:
           econ[g] & 4 → V = 2·V; & 2 → ·4; & 1 → ·8
           d = epoch[3] − TechType.age (+0x1c8); d > 0 → V ·= (d + 1)·d
    victory == 9: V ·= 3 (age techs: ·9 of the pre-victory value)
    slot = cat 0 → 10, cat 1 → 9, cat 2 → 4, cat 3 → 8
    w == TOWER: skip if get_diff() < 2; V ·= 100; slot = 6
    w == TEMPLE: V ·= 4; TAXATION..INCOME_TAX (589-592) → another ·100
    w == LIBRARY: V = (max(0, epoch[0..3])·V + 1) / (epoch[cat] + 1)
    w == UNIVERSITY: V ·= 1000
    w == SENATE (governments):
        family = (raid < 0 or (raid == 0 and coin = 1)) ? "civil" : "martial"          // coin drawn only if raid == 0
        civil: skip EMPIRE, MONARCHY, SOCIALISM; martial: skip REPUBLIC, DEMOCRACY, CAPITALISM
        V ·= rnd(100); get_gov() < 0 → V ·= 20                                         // second draw
    time factors (✅): a = min(4, (frame − tech_frame)·ai_speed/14400 + 13), b = min(3, (frame −
        tech_cat_frame[cat])·ai_speed/25600 + 17) — both constant (4 and 3) since the stamps are past frames;
        V = (b·((a·V)/2))/2; shortages == 0 → ·2
    missing goods: C = Σ costs[g]; M = Σ over g with !type_avail(g) and !has_preq(g) of 2·costs[g];
        M ≠ 0 → V = (2C − M)·V / (2C); V < 0 → 9999999
    wonder_mod (+0x794) ≠ 0 → V /= 2
    V = check_income(t, 0x400, −1, wonder_mod == 0, −1, 1) · V / 256; V < 0 → 9999999
    make_me(t, V, escrow 1, slot, city −1, up 0, num 1, 0, 0)
```
`tech_frame` / `tech_cat_frame[cat]` (+0x7c4 / +0x7c8) are the frames of the last tech queued (any / per category),
written by `Build::queue_up`. Note the government draws: the coin only when `raid == 0`, the `rnd(100)` for every
government of the allowed family that reaches that point (each passes the building and `tech_avail` tests first).

Special mode: with `starting_resources == 7` and `starting_technology == 8` the decompile tests MISSILE_SHIELD (579)
and directly calls `produce_tech(579, 1)` when affordable, skipping the scoring ❓ (rare setup; port later).

`Leader::check_income(t, k, o, total, city, need, &n)` @6cc800: `n = t.can_pay_cost(who, city, o, total)`; if
`n < need`: return 0 when `total == 0`, else `m = 64`; otherwise `m = 256`. With `total == 0` (and starting_resources
≠ 8), for each available good with `c = get_cost(g, who, o, city) ≠ 0`: `i = income[g]/16`; `i < 1` → `m /= 4`;
else `q = clamp(c / i, 0, 20)`, `m = (100 − q·k/256)·m / 100`. Returns m (the "affordability" factor /256).

### 6.4 Step 5: Leader::upgrade_units @6c6430
Queues unit-line upgrade research (unit types with `type_avail == 2`, i.e. researchable upgrades). Elimination games:
only the capital owner. One draw per evaluated type when `get_diff() ≠ 2`.
```
E_war = max over j ≠ who at war with who (either diplos 0) with bit j in active_wars_with of leaders[j].combat
E_any = the same max over the other warring leaders; E = E_war if non-zero else E_any
for t = 50 .. 401 (unit types), index order:
    if type_avail(t) ≠ 2: continue
    w = t.where; need num_buildings[w] (+ the w.to upgrade line) ≠ 0 and !researching(t)
    age_t = t.age (get_age_slow when −1)
    // walk the line below t: f = t.from (+0x3c; if !tribe_can_type(f), f = f.type_graft(who)), while f ≥ 50:
    //   owned += num_units[f] (+0x56fe-relative, i.e. num_units[f]); if type_avail(f) == 4: buildable = 1 and
    //   age_b = age of the first buildable predecessor; f = f.from (graft rule again)
    special = t.unit_flags (+0x2b4) & 0x20000
    skip unless (owned ≠ 0 or !buildable or special) and !t.is(MERCHANTFLEET 318)
    P = (pop·1000) / max(1, cities + villages)
    a = 4 (time factor, constant, see §6.3); d = age_t − age_b; m = max(1, 5d + 2)
    V = m·(((a·P)/2)·(owned + 2)/2)
    wonder_mod ≠ 0 → V /= 100
    domain 0 → V = V·ground_mod (+0x798)/256; domain 1 → ·sea_mod (+0x7a0)/256;
    domain 2 → V = V·air_mod (+0x79c)/256, and for missiles `is(NUCLEARMISSILE)`:
        V ·= (sea_map/2 + 1)·(nukes + 2); nukes > 0 → ·2; rush < 0 and army_size > 0 → ·2;
        skip the type if any VALID|ACTIVE non-ally has MISSILE_DEFENSE_BONUS or researches MISSILE_SHIELD (579);
        get_diff() > 2 and max_enemy_age(who) < ages → ·100
    C = combat (+0x944): C ≥ 2E → ·4; else C ≥ 3E/2 → ·3; else C > E → ·2
    C ≥ 24 → ·(special or owned ? 4 : 2); 16 ≤ C < 24 → (special or owned) ? ·3 : ·3/2;
        8 ≤ C < 16 and !special and owned == 0 → ·5/4
    clamp V to [0, 9000000] (negative → 9000000)
    if get_diff() ≠ 2:
        u = unit_prod_value(t); diff 0 or ≥ 3 → u = 3u/2
        u = (rnd(3) + 4)·u / 5                                     // DRAW
        diff < 2 → u = −u; clamp u to [−256, 256]; V = (V/256)·(u + 256)
    non-combat type (role & 0x10000 clear): slot 8; active_wars == 0 → V = 5V/4
    combat type: skip under rush_rules 8; slot 7; at war (active_wars ≠ 0): V ·= (owned/3 + 1);
        else wars ≠ 0: V ·= (owned/6 + 6) (✅ disassembly: +6, not +1); then special ? V ·= (d + 1) : V = (d + 2)·V/2
    shortages == 0 → ·2; sea units (domain 1) → ·sea_map²; effective_pop > pop_cap − 4 → ·2
    m2 = t.can_pay_cost(who, −1, −1, 1) < 1 ? 64 : 256
    make_me(t, m2·(V/256), escrow 1, slot, city −1, up 0, num 1)
```
`Leader::unit_prod_value(t)` @6cc580 (combat units without the 0x20000 flag, else 0): over the other leaders j that
are not mutual allies (and, for `get_diff() < 2`, only human leaders), for every combat unit type u they own
(`num_units[u] = n > 0`) that t can engage (same domain; air enemies only if t has the anti-air flag `+0x1e4 &
0x80000000`; a land anti-air t counts only air enemies): `Σ (t.vs[u] − u.vs[t])·min(n, 10)` over `Σ min(n, 10)`, where
`vs[]` is the per-type short array at UnitType +0x2b0 (index = target type) ❓ (rules-spec unit bonus table). Returns
the average (0 without opponents).

### 6.5 Steps 6 and 9: Leader::create_units @6c40a0

Evidence: `re/decomp/ai/Leader_create_units@006c40a0.c` and its disassembly (`re/decomp/ai/asm/`); every constant, branch
order and register-level quirk below was read off the disassembly (✅ marks spots where the decompile was ambiguous or
wrong). Helpers: `re/decomp/ai2/`, `re/decomp/ai_create/` (job `re/jobs/dc_ai_create_1.txt`, `_2.txt`).

**Notation.** `T = unittypes[t]`; `w = T.where` (+0x40, the trainer building type); `have = num_queued[t] +
num_units[t]` (raw counters of t only); `haveL = get_queued(t, num_queued[t]) + get_units(t, 0)` (line-inclusive:
both helpers also add every type u ≠ t whose graft-resolved `from` is t, recursively, i.e. t's later upgrades);
`owned(w) = num_buildings[w] + get_buildings(w.to, num_buildings[w.to])` (w plus its `to` upgrade chain, BuildTypeData
+0x2e0). LeaderData arrays are indexed by full TypeIndex: `num_buildings[t]` = +0x5222 + 2t, `num_units[t]` = +0x56fe +
2t, `num_queued[t]` = +0x5a22 + 2t. `e0` = `epoch[0]` (military library level, encrypted +0xe8). `cap(g)` =
`get_mod_resource_cap(g)`. `/256` is the truncating idiom `(x + (x>>31 & 0xff)) >> 8`; `/4`, `/8`, `/32` likewise.
Role bits (UnitType +0x2c8, PDB enum ROLE_*): 0x10 SCOUT, 0x200 PEASANT, 0x1000 AIR, 0x8000 TRANSPORT, 0x10000
COMBAT, 0x40000 GROUND, 0x80000 SEA. Type predicates (UnitType vtable @b41fd4, dumped from the exe): `is_siege` vslot
0x10c = `unit_flags & 0x20000` (the summary's "special" flag is **siege**), `is_missile` 0x114 = `obj_masks (+0x1e4) &
0x8000000`, `is_caravan` 0x130 = `unit_flags2 & 8`, `get_age` 0xec = `age` (+0x278, `get_age_slow` when −1).
`is_peasant`: type 50/51; `is_scholar`: 52/53; `is_worker`: 50..53; `is_merchant`: 61/62/400. City flags (+4):
1 VALID, 2 UNDER_ATTACK, 8 EVER_ATTACKED, 0x10 CAPITAL, 0x1000 VERY_NEAR_BORDER.

No RNG except the one draw per evaluated combat type described in **Pacing and the draw**.

#### 6.5.1 Per call
```
pt   = max(pop_cap/2, pop_cap − 40)                                   // army-size target
e0   = epoch[0]                                                       // local copy, may grow (CTW quirk, 6.5.6)
d    = get_diff()
dcap = d < 2 ? 2·d + 1 : min(10, 2·d + 2)                              // 1, 3, 6, 8, 10, 10
civ  = merchants + caras + scholars + peasants                         // computed once, never updated
first = 1
for c = 0 .. city_mark−1 (slot order):
    city = cities[who][c]; skip unless city_flags & VALID
    co = city.o (city building index); reg = city.reg (land region)
    army = first of the 16 armies with valid ≠ 0, army.city == c, status & 1; else −1
    slots = count_gather_slots(city, &gs[6], &fs[6])
        // gs[g] = Σ gather_max of the city's gather buildings for good g; fs[g] = Σ (gather_max − num_gatherers);
        // return value = Σ gs over every good except knowledge (universities excluded)
    for t = 50 .. 401 (index order): evaluate(t)                       // 6.5.2
    first = 0                                                          // after the first VALID city, ✅
```
`first` is cleared after the type loop of the first VALID city even if every type was skipped there.

#### 6.5.2 Gates (in this order; any failure skips the type)
1. `effective_pop > pop_cap` (tested per type).
2. `type_avail(t, 1) < 4`.
3. `w < 0`; `owned(w) == 0`; `T.unit_flags & 0x8000`.
4. Trainer kind. **Leader-wide** (`lw = 1`) when `w.is_fort()` (`is(FORTX)`) or (`w.is_military_trainer()` and
   `!w.is(VILLAGE, 0)`): evaluated only while `first` (the first VALID city) or when `T.domain == 1` (ships: every
   city). Otherwise **city-trained** (`lw = 0`): requires `City::could_queue(city, t)` (some building of the city's
   chain can queue t).
5. Count cap `cap` (−1 = none) and `left` (default 5):
   ```
   if T.is_siege():            cap = min(city_num, dcap, e0)
   elif unit_flags & 4:        cap = min(city_num, 6)
   elif unit_flags & 8:        cap = min(city_num, 10)
   else: cap = −1; if role & COMBAT: cap = (d == 1) ? 7 : (d == 0) ? 3 : −1
   for the first three cases: d < 2 → cap = 2; then skip unless non_siege − 5·have − sea_combat ≥ 10   ✅
   if T.is(AAGUN 281): cap = cap / 2                 // C truncation: an uncapped AA gun gets cap 0 (−1/2 = 0) ✅
   if cap > 0: skip if have ≥ cap; left = cap − have
   ```
6. `P = pop·1000 / max(1, city_num)`; `esc = 0`.

#### 6.5.3 Pacing and the draw (every role & COMBAT type, any domain)
```
if role & COMBAT:
    skip if setup.rush_rules == 8
    N = non_siege − sea_combat; dd = get_diff()
    dd == 0: skip unless N ≤ 3·city_num and N < 2·e0 + 3 and N < 10
    dd == 1: skip unless N ≤ 5·city_num and N < (7·(e0 + 1))/2 + 1 and N < 25
    dd == 2: skip unless N ≤ 6·city_num and N < 5·e0 + 1
    (dd ≥ 3: no pacing)
    if wonder_mod ≠ 0: P = P/10
    P = P · (domain 0 ? ground_mod : domain 1 ? sea_mod : air_mod) / 256
    if dd ≠ 2:
        u = unit_prod_value(t)                       // §6.4
        if get_diff() == 0 or ≥ 3: u = (3u)/2
        u = ((rnd(3) + 4)·u)/5                       // DRAW ✅ (Random::get(0,0xffff) % 3)
        if get_diff() < 2: u = −u
        u = clamp(u, −256, 256); P = ((u + 256)·P)/256
```
The draw happens here, before the domain branches, so every combat type that survives the gates and the pacing
draws once per evaluation (land, ship, aircraft and missile alike), even when a later branch skips it. Ships pass
gate 4 in every city, so a ship type can draw once per city. (The pacing bound uses `e0` — the military epoch, not
science.)

**Non-combat pop pressure** (types with role PEASANT clear, `!is_caravan`, role COMBAT clear: merchants, explorers,
spies, heroes, fishermen …): `effective_pop > pop_cap − 4` → `P /= 2`; `> pop_cap − 2` → `P /= 2` again;
`> pop_cap − 1` → skip.

**Desired-count divisor `D`** (used in the tail): 4; `w == STABLE` → 3 (5 with tribe bonus 0x13 Lakota);
`w == SIEGEFACTORY` → 2; `w == DOCK` → 2; `w == BARRACKS` → `3·pop_cap < 25 ? 4 : 3·pop_cap/5`; then `unit_flags & 8`
→ 2; `unit_flags & 4` → 1. The civilian branches below override it with `Dc = 2·pop_cap < 25 ? 4 : 2·pop_cap/5`.

#### 6.5.4 Branches (by role; each sets V, `cat`, `esc` and a batch size `b`)
Branch selection ✅: role & GROUND → (COMBAT → land army; else SCOUT → explorers; else civilians); else role & SEA →
navy; else role & (COMBAT|AIR) == both → air/missiles; else skip.

**Land army** (GROUND|COMBAT), cat 6:
```
skip if rush_rules == 8, or rush_rules < 8 and Leaders::max_age() < rush_rules − 2
E = (semaphore byte2 & 2 (Conquer the World)) ? e0 + 2 : e0
if N < 3·e0 and !is_siege: P ·= 100; esc = 1                    // e0 before the update below
if lw == 0 and !(city_flags & UNDER_ATTACK):
    e0 = E                                                       // stored now (before the frame test)
    skip if frame < 2000 / ai_speed
    pop_cap < 24 → P = P/4; else pop_cap < 36 → P = P/2
// war factor
if reg_wars[reg]:  V = active_wars ? (P = 2P) : 3P/2
elif reg_neutrals[reg] or active_wars: V = 3P/2
elif wars: V = 11P/10
else V = P
s = strategy[reg]
if s & 4 or reg_attacked[reg]: V ·= 2; if lw == 0 and city under attack: V ·= 2
elif s & 2: V = 3V/2
V = ((T.get_age() + 8)·V)/8
C = combat: C < pt/2 → ×6; C < 4pt/5 → ×2; C > 5pt/4 → 2V/3; C > 3pt/2 → /2; C > 2pt → /2
reg_combat[reg] > 2·reg_pop[reg]·(reg_wars[reg] + 1) → /2              (unsigned compare)
if is_siege: non_siege ≥ 3 → ×2; siege < non_siege/8 → ×2
else if siege < dcap: siege < non_siege/20 → /2, else siege < non_siege/10 → 2V/3
if lw == 0 and army < 0 and !(city under attack) and T.domain == 0: V = 3V/4
b = 10 / control_cost; if b < 1 or left < 1: b = 1
else: b = min(b, left); while b > 1 and effective_pop ≥ pop_cap − b − 1: b −= 1
e0 = E
```
(The 5pt/4 bound uses the `/4` idiom; 4pt/5, 3pt/2 truncate.)

**Explorers** (GROUND, SCOUT, not COMBAT), cat 6, b 1: elimination games (`setup.elimination == 1`): skip unless
`find_capital` reports who as the capital owner. Skip if `have > 1`. `V = 10000 / max(1, frame/1000)`; `esc = (have == 0)`.

**Civilians** (GROUND, not COMBAT, not SCOUT): elimination → capital owner only (as above). Skip if `lw == 0`, the city
is under attack and `city_num > 1`. Then:

- **Role PEASANT** (citizens, scholars): skip if `lw ≠ 0`. `B = infra_mod·P/256`.
  - **Citizens** (`is_peasant`), cat 5, `D = Dc`:
    ```
    q = city building.count_queue(PEASANTS, 0)                 // citizens queued in the city centre
    skip unless reg_free_peasants[reg] < reg_cities[reg]                  ✅ (a gate, not a multiplier)
    if city.busy == 0 and city.free == 0 and q == 0: B ·= 30
    nh = city.free + city.busy + q
    if nh < slots − 1:
        room = slots − free − busy − q; useful = 0
        for g = 0..5, g ≠ knowledge, type_avail(g,1) ≠ 0:
            income[g] ≥ cap(g) → room −= fs[g]   else useful += min((cap(g) − income[g])/160, fs[g])
        n = max(0, min(useful, room)); esc = 1; V = n²·B·80; b = n
        if n == 0: skip if city.gatherers or busy or free ≠ 0; skip if q ≠ 0
    else:
        skip unless nh < slots or free + busy ≤ 2;  V = B; b = 1; (esc stays 0)
    skip if free + q ≥ 2 and slots == 0
    victory == 8 → ×10
    reg_gatherers[reg] < reg_gather_slots[reg] → 3V/2; 2·reg_gatherers < reg_gather_slots → ×2
    city.gatherers == 0 → ×2;  free + busy ≤ slots/2 → 3V/2;  reg_peasants[reg] == 0 → ×5
    reg_free_peasants[reg] == 0 → ×2, and ×2 again if type_avail(TOWN) == 4 and reg_land[reg] < 2
    have > pop_cap/2 → /2;  have > 40 → /2
    ```
    `fs[g]` are the city's *free* gather slots (the summary's "a4" is the free array, not the slot count). With
    `n == 0` and an empty city the item is queued with `num = 0` (tail: `min(0, left)`) ❓ harmless?
  - **Scholars** (`is_scholar`), cat 4, `D = Dc`: skip if `starting_resources == 8`, `fs[3] == 0`, or `bucket[3] >
    ((resource_cap[0]/16)·3)/2` (raw encrypted food cap, not `cap(3)` ✅ odd but literal). `u =
    city.get_building(UNIVERSITY)` (first VALID building of that type in the chain); skip if `u < 0` or not VALID.
    `ns = fs[3] − u.count_queue(TYPE, SCHOLARS)`; skip if `ns ≤ 0`. `V = infra_mod·ns·10000/256`;
    `F = filled_gather_slots[3]`, `G = gather_slots[3]`: `F < G` → ×60; `F < 2G/3` → ×10 and `esc = 1`; `F < G` → ×5.
    `b = ns`.
- **Caravans** (`is_caravan`), cat 4, `D = Dc`: skip if `starting_resources == 8`, `T.is(MERCHANTFLEET 318)` or `lw ≠ 0`.
  `V = infra_mod·1000000/256`; `e = econ[2]` (wealth ✅): `e & 1` → ×120, b 3; else `e & 2` → ×60, b 2; else b 1 and
  `e & 4` → ×30. `b = min(b, get_caravan_limit(1) − have)`; skip if `b < 1`; `esc = 1`.
- **Merchants** (`is_merchant`), cat 4, `D = Dc`, b 1: `city.get_building(MARKET)` must exist and be VALID; need
  `has_tech(MATHEMATICS 552)` or `type_avail(KNOWLEDGE)`; need `known_rares − have > 0` and `effective_pop < pop_cap −
  1`. `V = 1000000`, `esc = 1`.
- **Spies** (`unit_flags2 & 0x60` clear, `T.is(SPY 58)`), cat 6, `D = Dc`, b 1: need `lw ≠ 0`, `combat ≥ 20`,
  `get_diff() > 2`. `V = left·2000`; `h = get_units(t,0) + num_queued[t]`; `h == 0` → `esc = 1`, `V ·= 100`; skip if
  `15·h > combat`. (Other non-merchant civilians without the 0x60 bits are skipped.)
- **Heroes / supply** (`unit_flags2 & 0x60`: 0x20 HERO, 0x40 SUPPLY), cat 6, `D = Dc`, b 1: need `lw ≠ 0`, `combat ≥
  8`. `V = combat·left·2000`; `h` as above; `h == 0` → `esc = 1` and, for SUPPLY types, `V ·= 10`; skip if `8·h > combat`.

**Navy** (role SEA, not GROUND):
```
skip if frame < 1000 / ai_speed
dk = find_building(city.x, city.y, SEARCH_FRIENDLY, who, 0x1800, 0, FILTER_BASE_TYPE, DOCK, 0); skip if dk < 0
skip unless dock object is_active (flags & 1)
(tx, ty) = dock.tile_corner(); sr = −1
for x = tx .. tx + dock.type.x_size − 1:                     // outer
    for y = ty .. ty + y_size − 1:                          // inner
        if tdata[y·tile_xs + x] & 0x30 == 0x20 (water ❓): sr = get_tregion(x, y); break
    if sr ≥ 64: break
skip if sr < 63 or Region[sr].size < 20
hL = haveL
if sea_map < 3 and T.is(BARK 323) and reg_combat[sr] ≥ 2·sea_map: skip
skip if role & TRANSPORT                                   // transports are never queued here ✅
```
`reg_combat[sr]` = own combat ships in that sea region (census §4.2).
- **Warships** (role COMBAT), cat 6, b 1:
  ```
  skip if sea_map ≤ 1; sea_map == 2 and !T.is(BARK) → skip
  skip if e0 ≥ 5 and reg_combat[sr] > combat/3;  skip if reg_combat[sr] > combat/2
  over j = 0..7 with leader_flags & 1 and !leaders[j].is_ally(who): k++, S += leaders[j].reg_combat[sr],
      M = max(M, that)                                       // self and mutual allies excluded
  A = k > 0 ? S/k : 0
  skip if reg_combat[sr] > A + 5 or > M + 3
  wx = 192·x + 96, wy = 192·y + 96 (the water tile found)
  if Armies::find_army(who, wx, wy, −1, _, −1) < 0 and (c2 = find_city(wx, wy, SEARCH_FRIENDLY, who, …, FILTER_ALL)) ≥ 0:
      Armies::init_navy(who, c2, sr)
  if sea_map ≤ 2:
      skip if reg_combat[sr] > Region[reg].size/20 and A == 0        // reg = the city's land region
      skip if reg_combat[sr] > 1
      V = sea_map·(A + 1)·((10·P)/(hL + 1))
  else:
      W = (500·P)/(hL + 1)
      if T.is(AIRCRAFTCARRIER 351): skip if hL > 1 or reg_combat[sr] < 8 or reg_combat[sr] < A; esc = 1
      else: esc = (hL < e0 + 3); if A > reg_combat[sr]: W = T.is(BARK) ? W/10 : 10W
      if dock.is_under_attack() (build_masks & 0x20): esc = 1
          T.is(FIRESHIP 336) → V = sea_map·(A + 1)·(100W), done;  else W = W/1000
      V = sea_map·(A + 1)·W
  ```
  `find_city` returns the nearest friendly city of *any* friendly leader, and its index is passed to `init_navy` as
  an own-city index ❓ (original quirk when an ally's city is nearer). Both helpers ignore their garbage `this`/5th
  arguments (find_army's param 5 and find_city's param 5 are unused) ✅.
- **Fishing boats** (non-combat, `T.is(FISHERMEN 317)`, else skip), cat 4, b 1: skip if the dock is under attack;
  skip if `reg_combat[sr] == 0` and `e0 ≥ 3` (the region-size ≥ 20 part is always true here); `F = (100·P)/(hL + 1)`;
  skip if `idle_fishermen ≠ 0`; skip if `hL ≥ epoch[2]` and `hL ≥ 4`. (`econ[0]` bits then scale a dead variable — the
  draw value slot — with no effect ✅.) `reg_combat[sr] < 10` → `F = F/(10 − reg_combat[sr])`; `V = sea_map·F`.
  MERCHANTFLEET and other non-combat ships: skipped.

**Air** (role COMBAT|AIR, not GROUND/SEA), cat 6:
- **Missiles** (`is_missile`), b 1:
  - `T.is(NUCLEARMISSILE 315)`: skip if `get_diff() < 2`. `base = wars ? 400000 : 200000`; `n =
    leaders[who].get_nukes()` (= 0 without tech bit 315, else `get_queued(315) + get_units(315) − nukes_in_flight`);
    skip if `n + game.armageddon ≥ get_armageddon() − 1`. Over j = 0..7 (`leader_flags & 1`, `!leaders[who].is_ally(j)`,
    `!leaders[j].has_preq(MISSILE_DEFENSE_BONUS 693)`, `!leaders[j].researching(MISSILE_SHIELD 579, −1, 0, 0)`): mark
    found and `M = max(M, leaders[j].get_nukes())`. Skip if none found; skip if `n ≥ num_buildings[T.where]`.
    `V = M > n ? (M − n)·base (esc = 1) : base`; `T.can_pay_cost(who, −1, −1, 1) ≠ 0` → `esc = 1`; `n == 0` → ×5;
    `V = (pers.nukes + 2)·V`; `pers.nukes > 0` → ×2; `pers.rush < 0` and `pers.army_size > 0` → ×2.
  - Other missiles (cruise): skip if `leaders[who].has_preq(NUCLEARMISSILE)`. `V = hL ? 600000/(hL + 1) :
    (pers.air > 0 ? 1200000 : 600000)`; `pers.nukes < 0` → ×2; `esc = 0`.
- **Aircraft**: skip if `air ≥ 8·num_buildings[AIRBASE]`. `left = (get_diff() < 2) ? 2 : (== 2 ? 4 : 16)` (overwrites
  left ✅); skip if `hL ≥ left`. War factor and strategy as for the land army (no extra ×2 for an attacked city, `P`
  not written back). `V = (T.attack·V)/10`. `T.is(BOMBER 304)`: if `fighters ≠ 0`: `bombers < fighters/4` → ×2,
  `bombers < fighters/8` → ×2; `fighters ≥ 8` and `bombers < 3` → ×2. Army size: `C < pt/2` → ×2; `< 4pt/5` →
  3V/2; `> 5pt/4` → 2V/3; `> 3pt/2` → /2; `> 2pt` → /2. `reg_combat[reg]` test as land (/2). Batch as land but with
  `b < 1 → 1` only (no `left < 1` test): `b = min(10/control_cost, left)`, reduced while `effective_pop ≥ pop_cap − b − 1`.

#### 6.5.5 Common tail
```
if cap ≥ 0 and !T.is(MACHINEGUN 123): V = 10·left·V; if num_units[t] == 0 and city_num > 2: V ·= 100
num = min(b, left)
if is_worker or is_caravan or is_merchant:
    if civ ≥ 50 and civ > pop_cap/2: V = V/100
    skip if civ ≥ (3·pop_limits[setup.pop_limit].data[0])/5
W = (D·V) / (D + have)
n = 0; m = check_income(t, 0x400, co, esc, −1, num, &n)          // o = city building index, city = −1
if 0 < n < num: m = check_income(t, 0x400, co, esc, −1, n, null); num = n
V = (m·W)/256; V < 0 → 9999999; wonder_mod ≠ 0 → V/2
make_me(t, V, esc, cat, c, up 0, _ 9999999, num, 0, 0)
```
`check_income` (§6.3): with `esc = 0` the income loop scales m; with `esc = 1` m is 256, or 64 when fewer than `need`
copies are affordable. The cap multiplier also hits uncapped AA guns at d ≥ 2 (cap 0 from the halving).

#### 6.5.6 Quirks worth reproducing
- CTW (`semaphore byte2 & 2`): every land-army evaluation that reaches the `lw == 0` block or the batch code stores
  `e0 + 2` back into the local `e0`, so the pacing bounds of later types loosen cumulatively within the call ✅.
- `num` can be 0 for citizens (above). `D` uses raw `have`, not the line-inclusive count.

#### 6.5.7 Fields read
LeaderData: pop_cap, effective_pop, pop, city_num, city_mark, combat, non_siege, sea_combat, siege, air, fighters,
bombers, idle_fishermen, merchants, caras, scholars, peasants, known_rares, wars, active_wars, wonder_mod, ground_mod,
sea_mod, air_mod, infra_mod, econ[0], econ[2], filled_gather_slots[3], gather_slots[3], strategy[], reg_wars[],
reg_neutrals[], reg_attacked[], reg_pop[], reg_combat[], reg_free_peasants[], reg_cities[], reg_gatherers[],
reg_gather_slots[], reg_peasants[], reg_land[], num_units[], num_queued[], num_buildings[], pers.rush / army_size /
nukes / air, multi_diff (via get_diff); encrypted epoch[0], epoch[2], income[g], bucket[3], resource_cap[0].
City: flags, o, reg, free, busy, gatherers. Setup: difficulty, rush_rules (+0xe), elimination (+0x13), victory
(+0x14), starting_resources (+9), pop_limit (+0xd). World: sea_map, tile_xs, tdata. game.frame, game.armageddon,
ai_speed, armies[who], Region.size.

---

### 6.6 Steps 7 and 10: Leader::create_buildings @6c1be0

Evidence: `re/decomp/ai/Leader_create_buildings@006c1be0.c` + disassembly; checked ✅ against the asm throughout.
Build flags (BuildTypeData +0x2c0, PDB enum BUILDTYPE_*): 4 GLOBAL, 0x10 OUTSIDE, 0x40 GATHER, 0x200 ONE (one per
city), 0x8000000 RESEARCH, 0x40000000 MIL_TRAINING, 0x80000000 TRAINING. BuildType vtable @b42b94: 0x64 `is_city`,
0x90 `is_gather_type` (= flags & 0x40), 0xfc `is_fort` (= is(FORTX)), 0x118 `get_wonder_value`. Helpers:
`is_gather_enhancer` = is(GRANARY 423 / LUMBERMILL / SMELTER / REFINERY 426, 0); `get_enhancing_good` (exact type
423→0, 424→1, 425→4, 426→5, else −1); `get_good` (FARM 0, WOODCUTTER 1, MINE 4, UNIVERSITY 3, OILWELL/OILPLATFORM 5,
else −1); `is_defensive` = TOWER, FORTX, AIRBASE or `attack ≠ 0`; `is_training_building` / `is_military_trainer`
test the *basic* type's TRAINING / MIL_TRAINING flag. `city.count_buildings(t, exact, active)` counts VALID buildings
of the city chain (exact type or `is(t,0)`; `active` → `flags & 4`); `city.num_buildings()` counts VALID completed
buildings incl. the centre; `get_reg_buildings(r, t)` = `reg_buildings[r][t]` + its `to` chain (0 for r ≥ 64).
Draws: two per wonder type that reaches the random formula (6.6.4).

#### 6.6.1 Loops and city filter
```
for pass = 0, 1:                                       // 0: non-gather types, 1: GATHER types
  for c = 0 .. city_mark−1 (slot order):
    city VALID; cb = the city building's BuildData
    skip if cb.city ≥ 0 and cities[cb.who][cb.city].race ≠ cb.who, unless (cb.flags & 0x20 CITY) == 0 and
         cb.type has OUTSIDE (0x10)          // a city centre always has CITY, so: captured, unassimilated → skip
    co = city.o; reg = city.reg
    level = city type TOWN 415 → 2; METROPOLIS 416 or FORBIDDENCITY 531 → 3; else 1
    nb = city.num_buildings()
    fl = land − filled;  fo = ocean − ocean_filled
    skip if space[0] == 0 and fo == 0
    gs_total = count_gather_slots(city, &gs[6], &fs[6])          // as in 6.5.1
    for t = 414 .. 542 (index order): evaluate(t)
```
The city type loop runs over buildings incl. wonders 526..542; ages/techs are not reached.

#### 6.6.2 Gates (in order)
1. `t.is_city()` → skip. `(build_flags & 0x40 ≠ 0) ≠ pass` → skip. `type_avail(t,1) < 4` → skip.
2. Elimination (`setup.elimination == 1`): skip unless capital owner == who or `t.is_military_trainer()`.
3. Room: `sz = clamp(max(x_size, y_size) − 2, 0, 2)`, `sp = space[sz]`. Sea building (`domain == 1`): need `fo ≠ 0`,
   and for `is(DOCK)` also `dock_tile ≠ 0`. Land: need `sp ≠ 0`.
4. `cat = 8`, `esc = 0`, `up = 0`, `k = 4`. Upgrade: `from (+0x3c) ≠ −1` and not GLOBAL → `up = 1`, need
   `city.count_buildings(t.where, 1, 1) ≠ 0` (an active base building in this city).
5. Sea types other than DOCK → skip. `rush_rules == 8` and `is(LOOKOUT 521)` → skip. ONE-per-city: skip if
   `city.count_buildings(t, 0, 0) ≠ 0`.
6. UNIVERSITY: need `nb ≥ 3`. LIBRARY: if `owned(LIBRARY) ≠ 0` need `nb ≥ 3` (the first library has no limit).
   SENATE: skip if `owned(SENATE) + get_queued(SENATE) > 0`.
7. Plain types (`!GATHER`, `!is_gather_enhancer`, `attack == 0`, `!is(TEMPLE)`): skip if `owned(t) + get_queued(t) ≥ 3`
   (leader-wide; wonders pass since they are at most 1).
8. `cnt = city.count_buildings(t, 1, 0)`; FARM: skip if `cnt ≥ get_farm_limit()`.

Then pass 1 → gatherers (6.6.3); pass 0 → base value (6.6.4).

#### 6.6.3 Gatherers (pass 1), k = 2
```
skip if starting_resources == 8
g = get_good(t); skip if filled_gather_slots[g] < (3·gather_slots[g])/4
B = ((pop·7000)/max(1, city_num + village_num) + level·3000 + fl·4000)·infra_mod/256
victory == 8 → ×100;  num_buildings[t] == 0 → ×5
UNIVERSITY: num_buildings[UNIV] == 0 or scholars > 3·num_buildings[UNIV] → ×10, else /10
if cb.city_down < 0 (the centre is the city's only building): ×10
else: level == 1, has_tech(TOWN) and nb < city_buildings + 1 → ×2;
      level == 2, has_tech(METROPOLIS) and nb < metro_buildings + 1 → ×2           (Constants +0x14c / +0x150)
city CAPITAL → B/2
cat = 4; found = 0; Mbest = 256
rf = reg_free_peasants[reg]; skip if rf == 0 and pop_cap − control ≤ 1
if rf > 2 and rate[worst_good] < cap(worst_good)/32: esc = 1
for gg = 0..5, only gg == g:
    need type_avail(g), income[g] < cap(g), fs[g] < 2 (fewer than 2 free slots of g in the city)
    oil = 0; m = 256
    if g == 5: oil = (c == 0 and oil_patches.count > gather_slots[5]); B ·= 10
    ter = city.ter[g]
    if g == worst_good:
        need ter ≠ 0 or oil; m = (g == 5) ? 0x800 : (max(3 − goodtypes[g].+0x2ec, ter) + 2)·256
    else:
        need ter ≠ 0 or oil
        g ∈ {food, timber, wealth} and city_num + village_num < 5 → m = 512
        if up == 0: need sp ≥ 2; sp < 3 and !(econ[g] & 2) → m >>= 1; sp < 4 and ter < 2 and g ≠ 5 → m >>= 1
        dv = econ[g] & 8 ? 4 : (econ[g] & 2 ? 2 : 3)
        m = ((dv + max(3 − goodtypes[g].+0x2ec, ter))·m)/dv
    found = 1
    rate[g] < 20·city.pop → ×2;  g == worst_good → ×2;  g == best_good → /2;  econ[g] & 8 → /2
    if ter > 1 or g == 5: econ&2 → ×2; econ&1 → ×2; econ&4 → 3m/2
    g == 0 → ×2
    fb = free + busy: gs_total > 2·fb → 2m/3; gs_total > 3·fb → 2m/3
    if city.free ≠ 0: gs_total < free + gatherers → 4m/3; gs_total == 0 → ×3
    if !(econ[g] & 8) and (ter > 1 or g == 5) and city_num + village_num > 1 and city.count_buildings(t,0,1) == 0: 3m/2
    Mbest = min(Mbest, m)                                     // capped at 256 ✅
skip unless found
V = B·Mbest/256; V < 0 → 9999999
```
`goodtypes[g] +0x2ec` is a GoodType field ❓ (unnamed in the dump). Continue at 6.6.5.

#### 6.6.4 Pass 0 base value and special classes
```
V = level·1000
if reg_buildings[reg][t] + get_queued(t) ≥ reg_cities[reg]: V = V/(reg_cities[reg] + 1)   (unsigned)
TEMPLE (is(TEMPLE, 1)): skip unless city CAPITAL or get_diff() > 2
    V ·= 500; esc = 1; num_buildings[t] == 0 → ×20; city.bordering → ×100
    VERY_NEAR_BORDER → ×2, and ×5 more unless UNDER_ATTACK
LOOKOUT: skip if epoch[3] ≤ 3
    ages < 4: skip if cnt ≠ 0; V ·= 100; esc = 1
    else:     cnt < 2 → ×100, esc = 1;  cnt ≥ 3 → skip
```
**Wonders** (`is_wonder_type`):
```
skip if CTW (semaphore byte2 & 2), city.num_wonders(1) ≠ 0 (any wonder except REDFORT, not counting CITY objects),
     or (starting_resources == 7 and city_num < 2)
ok = 1
for j = 0..7 with leader_flags & 2:
    if leaders[j].is_ally(who) (incl. self): skip the type if leaders[j].num_queued[t] ≠ 0
    elif leaders[j].num_queued[t] ≠ 0:
        for each object o in unbuilt_wonders[j] (list order): if it is(t, 1) and
            (float)job_counter / (float)construct_time(0) > 0.25: ok = 0; break     // next j
skip unless ok
if wonder_mod ≠ 0: W = 100000000; esc = 1
else:
    if get_diff() < 2:
        victory == 6: skip if get_team_wonder_value() ≥ get_enemy_wonder_value() + 1
        else: skip if wonder_mark ≠ 0                                         // easy AIs: one wonder at a time
    ww = Game::wonder_winning(); E = get_enemy_wonder_value() + get_enemy_unbuilt_wonder_value()
    if victory == 6: W = (E + 1)·100000; esc = 1
    elif (ww ≥ 0 and ww ≠ who) or E ≥ wonderwins[setup.wonderwin].data[0]/2 or E > 4: W = 2000000; esc = 1
    else:
        r1 = rnd(1000)                                    // DRAW 1 ✅ (first Random::get, % 1000)
        r2 = rnd(300)                                     // DRAW 2 ✅ (second Random::get, % 300)
        W = (get_team_wonder_value() + 1)·(E + 1)·r2·r1   // int32
    get_diff() == 0 → W/100; == 1 → W/10
V = city.get_level()·t.get_wonder_value()·W                // replaces the level·1000 base
```
The job_counter / construct_time ratio uses unsigned→double→float conversions (CVTDQ2PD + 2³² fix-up, CVTPD2PS)
and a strict `>` against 0.25f ✅.

Non-wonders: `wonder_mod ≠ 0` and `!GATHER` → `V /= 100`.

#### 6.6.5 Common part (all types that got here)
```
victory == 9 and t ∈ {TERRACOTTA 529, STATUEOFLIBERTY 537, SPACEPROGRAM 542} → V/1000
if up == 0:
    pd = city.peasant_dist; pd ≥ 5 → V = V/((pd + 5)/5)
    city.free == 0 and reg_free_peasants[reg] ≠ 0 → 3V/4, then /2 if city.gatherers == 0 and city.busy ≠ 0
    land building (domain ≠ 1) without OUTSIDE:
        skip if fl ≤ 1 and sp < 2;  fl ≥ (7·land)/8 → 4V/3;  fl < 2 and sp < 3 → 3V/4
```
**Gather enhancers** (`is_gather_enhancer`), cat 4: skip if `starting_resources == 8`; victory 8 → ×100; `B =
infra_mod·V/256`; `g = get_enhancing_good(t)`; `n = g ≥ 0 ? gs[g] − fs[g] : 0` (citizens working that good in the
city); `g == 5` → `n = 2·gather_slots[5]`. Skip unless `type_avail(g)`, `income[g] < cap(g)`, (`n ≥ 3` or g == 5),
not (GRANARY and Lakota), (`fl ≥ 4` or g == 5). `V = n²·B`; `econ[g] & 4` → ×2; `& 2` → ×3, esc = 1; `& 1` → ×4,
esc = 1. (Enhancer upgrades have g = −1 and always end in a skip.)

Then the class blocks, in this order (a type can pass through several):

1. **Military trainers** (not RESEARCH, not DOCK, `is_military_trainer`), cat 7: skip if `rush_rules == 8`; skip if
   `cnt ≠ 0` and `up == 0`. `V ·= wars + reg_wars[reg] + reg_neutrals[reg] + 3`. If `get_reg_buildings(reg,t) == 0`
   and `get_queued(t) == 0`: `V ·= 2`, and `×200` more if BARRACKS or STABLE, or SIEGEFACTORY with
   `num_buildings[t] == num_queued[t] == 0`. `num_buildings[t] == num_queued[t] == 0` → esc = 1. `n =
   num_buildings[t] + num_queued[t]`: `n < (ages + 2)/2` → ×100; skip if `starting_resources ≠ 8` and `n > ages`.
   Tribe: BARRACKS & (Aztecs 0 or Romans 6) → ×4; STABLE & Mongols 17 → ×4; SIEGEFACTORY & Turks 8 → ×4; AIRBASE &
   Germans 12 → ×4 (the DOCK & Spanish ×2 test is dead here, docks never enter this block ✅).
2. **Dock** (`is(DOCK)`): skip if `cnt ≠ 0` and `up == 0`; skip if `starting_resources ≠ 8` and `num_queued[t] +
   num_buildings[t] > ages`. `V = sea_mod·V/256`; `owned(t) == 0` and `get_queued(t) == 0` → ×4, and with `sea_map ≥
   3` ×20 more and esc = 1. Skip if `sea_map < 3` and `city_num < 2`. `get_reg_buildings == 0` → ×8; `reg_wars[reg] ==
   0` → (active_wars ? ×4 : ×2), else /2; `V ·= sea_map`. (cat stays 8.)
3. **Research / market block** — (RESEARCH and not UNIVERSITY and not an enhancer) or `is(MARKET)`:
   ```
   if !TEMPLE:
       up == 0: skip if 2·(owned(t) + get_queued(t)) ≥ city_num
       starting_resources ≠ 8: skip if 2·(owned(t) + get_queued(t)) > ages
   if get_reg_buildings(reg, t) == 0:
       m = 10
       TEMPLE: m = 18; V = infra_mod·V/256; my_team_terr ≥ min_other_team_terr → m = 9; econ[2] & 3 → m ·= 2
       MARKET: m += 1; econ[2] & 3 → m ·= 2
       get_queued(t) ≠ 0 → m >>= 1; else if owned(t) == 0: (LIBRARY → m ·= 2); m ·= 2
       city_num == 1 → m >>= 2; == 2 → m >>= 1; > 2 and num_buildings[t] == 0 → m = (city_num·m)/2, esc = 1
       m > 1 → V ·= m
   LIBRARY: owned < 2 and city_num > 3 → V = infra_mod·V·10000/256
   MARKET:  owned < 1 → V = infra_mod·V·10000/256
   SENATE:  skip if city UNDER_ATTACK or city.num_buildings() < 5; V ·= 10; get_highest_epoch() ≥ 3 → ×10
   ```
4. **Tower / fort** (`is(TOWER)` or `is_fort`): `D = defense_mod·V/256`. `rush_rules == 8`: only forts, and skip
   if `my_team_terr > other_team_terr`. `defense_mod ≤ 256`: skip at diff 0; at diff 1 skip if `num_queued[t] +
   num_buildings[t] > ages`. `f = city.num_buildings() − cnt`; `D ·= (f ≥ 4 ? f/2 : 1)`. `city.bordering` → ×10,
   else forts /100. VERY_NEAR_BORDER → ×2 (×10 if not under attack); `!(city_flags & EVER_ATTACKED)` → /2;
   `city.founder ≠ who` → ×3; `is_unassimilated` → 3D/4; `reg_wars[reg] == 0` → /2.
   - FORTX: `my_team_terr < min_other_team_terr` → ×2; `F = 3D/(cnt + 1)`; `owned(t) + num_queued[t] == 0` → esc = 1;
     `cx = xs/2`, `cy = ys/2`, `dx = |cx − div3[city.x>>8]|`, `dy = |cy − div3[city.y>>8]|`;
     `V = ((cx + cy − dx − dy)/10 + 1)·F` (forts favour central cities).
   - TOWER: `V = D/(cnt/2 + 1)`; if `cnt == 0`: diff ≥ 3 → ×10 and esc = 1; CAPITAL → esc = 1.
5. **AIRBASE**: skip if `rush_rules == 8`. `n = get_queued(t) + get_reg_buildings(reg, t)`; `V = n ? V/n : 10V`;
   `reg_wars[reg] == 0` → /2; `n == 0` → esc = 1; `V = air_mod·V/256`.

#### 6.6.6 Common tail
```
reg_free_peasants[reg] == 0 → V = 4V/5
V < 0 → 9999999 (9999971 for wonders)
city UNDER_ATTACK and !is_defensive and (!is_military_trainer or city.count_buildings(t,0,1) ≠ 0) → V/100
q = get_queued(t); rb = get_reg_buildings(reg, t)
if GATHER:
    V = V/(q + 1); esc = 1
    i0 = make_list[0]: if V < i0.val and i0.t ≥ 0 and !can_pay(0) and !(i0.t is a GATHER building type):
        for g = 0..5: if g == get_good(t) and type_avail(g) and
                         i0.t.get_cost(g, who, i0.o, −1, 0, 1, −1) > bucket[g]: V = min(2V, i0.val); break
else:
    DOCK:                 V = V/(4(q + 2cnt) + 1 + rb)
    military trainer:     V = V/(4(cnt + q) + 1 + rb)
    training building:    V = V/(2(rb + 2(q + 2cnt)) + 1)
    TOWER:                V = V/(4(q + cnt) + 1)
    FORTX:                V = V/(4(q + 2cnt) + 1)
    else:                 V = V/(4(owned(t) + 2cnt + q) + 1)
m = check_income(t, k·256, −1, esc, c, 1, null)       // o = −1, city = c (the city index)
make_me(t, (m·V)/256, esc, cat, c, up, _ (garbage ECX, unused), 1, 0, 0)
```
`cat`: 8 default, 4 gatherers and enhancers, 7 military trainers. `up` → `make_this` uses `produce_upgrade`. One
`check_income` call per evaluated type (no RNG).

#### 6.6.7 Fields read
LeaderData: city_mark, city_num, village_num, pop, pop_cap, control, scholars, ages (enc), epoch[3] (enc), rate[g],
income[g], bucket[g] (enc), econ[], worst_good, best_good, gather_slots[], filled_gather_slots[], oil_patches.count,
wonder_mark, wonder_mod, infra_mod, sea_mod, air_mod, defense_mod, wars, active_wars, reg_wars[], reg_neutrals[],
reg_cities[], reg_free_peasants[], reg_buildings[][], num_buildings[], num_queued[], my_team_terr, other_team_terr,
min_other_team_terr, tribe, make_list[0]. City: flags, o, reg, race, founder, bordering, land, filled, ocean,
ocean_filled, dock_tile, space[3], ter[6], pop, free, busy, gatherers, peasant_dist. Setup: elimination,
rush_rules, starting_resources, victory, wonderwin. Constants: city_buildings (+0x14c), metro_buildings (+0x150),
farm limits (get_farm_limit). World: xs, ys, sea_map. unbuilt_wonders[j], GoodType +0x2ec.

---

#### 6.6.8 Draws in 6.5 / 6.6
| Function | Draw | Condition / order |
|---|---|---|
| create_units | `rnd(3)` → `u = ((r + 4)·u)/5` | once per evaluated combat type (role COMBAT, any domain) that passes gates 1-5, rush_rules ≠ 8, the difficulty pacing, with `get_diff() ≠ 2`; cities in slot order, types 50..401; ships may draw once per city |
| create_buildings | `rnd(1000)` then `rnd(300)` | pass 0 only, per city (slot order) and wonder type (526..542): after all gates, no ally queued it, no enemy copy > 25 % built, `wonder_mod == 0`, easy-AI checks passed, victory ≠ 6, no foreign wonder winner, `E < target/2` and `E ≤ 4`. First draw is taken `% 1000`, second `% 300` ✅ |

#### 6.6.9 Open questions (6.5 / 6.6)
- `tdata & 0x30 == 0x20` is assumed to mean "water tile" (navy region scan).
- `find_city` may return an ally's city index, which `init_navy` then treats as own (likely an original bug).
- Citizens can be queued with `num = 0` (n == 0, empty city); check what `produce_unit` does with 0.
- `goodtypes[g] +0x2ec` (gatherer term `3 − v`) is unnamed in the type dump.
- Scholars compare knowledge `bucket[3]` with the raw *food* `resource_cap[0]` — literal, but possibly an original bug.
- The fishing boat's `econ[0]` scaling writes a dead local (no effect); keep it out of the port.

### 6.7 Steps 8 and 11: Leader::make_stuff @6c8af0
```
item0 = make_list[0]; if item0.t == −1 or item0.val == 0: return 0
use_market()                                                               // §6.8, may draw rnd(6)
ok = can_pay(0) (= item0.t.can_pay_cost(who, item0.city, item0.o, item0.escrow) ≥ item0.num)
cost0[g] = item0.t.get_cost(g, who, item0.o, item0.city) for g = 0..5
bought = 0
if ok: bought = (make_this(0) == 0)
keep = ok or is_city(item0.t)
skip_extra = ok or bucket[3] < cost0[3]       // knowledge-short: no shortfall reserve below
drop_all = is(TOWER) or is(FORTX) or is_city or wonder (526..542) or is(UNIVERSITY, strict) or (unit type and not a citizen)
for each slot j = 0..10 with make_list[j].t == item0.t:                    // one draw per matching slot
    if rnd(3) == 0 or drop_all: make_list[j].t = −1                         // DRAW
extra = 0
if !skip_extra: extra = max(0, average over available goods with cost0[g] ≠ 0 of (cost0[g] − bucket[g]))
for i = 1..10:
    it = make_list[i]; skip unless it.t ≥ 0 and it.val ≠ 0
    if i > 3: skip if (it.t, it.city) equals slot 0, 1, 2 or 3 (any match) 
    blocked = some available g with cost0[g] ≠ 0 and c = it.t.get_cost(g, …) ≠ 0 and bucket[g] < cost0[g] + c + extra
    if blocked:
        i == 5 (cat 5): skip if free_peasants or gatherers ≠ 0, else go on (buy anyway) ❓
        i == 4: only a gather-enhancer building (build_flags & 0x40 ❓) for a good whose econ has bit 4
                (FARM → food, WOODCUTTER → timber, MINE → metal, UNIVERSITY → knowledge, oil buildings → oil) goes on
        other i: skip
    if it.t.can_pay_cost(who, it.city, it.o, it.escrow) ≥ it.num:
        if is_city(it.t): keep = 1
        make_this(i)
        drop = is(TOWER) or is(FORTX) or is_city or wonder
        if i < 11: for j = i..10 with make_list[j].t == it.t: if rnd(3) == 0 or drop: make_list[j].t = −1   // DRAWS
if !keep: site_mark++
return bought
```
✅ The item-0 part (`ok`, `keep`, `skip_extra`, `drop_all`, the duplicate draws) is checked against the disassembly
(re/jobs/asm_ai_2.txt).
So item 0 is bought only when fully affordable; lower items are bought when affordable and they do not delay item 0
(their cost does not push any good below item 0's need plus the average shortfall). Each purchase removes duplicates
of the same type with probability 1/3 per slot (always for towers, forts, cities, wonders, universities and, for item
0, non-citizen units). The draw is made for every matching slot even when the removal is certain.

### 6.8 Leader::use_market @6c91c0
```
n = max(1, epoch[2])                                   // commerce library level
need[g] = Σ over make_list[0 .. n−1] with t > 0 and val > 0 of t.get_cost(g, who, o, city)
if !(Nubians or has_preq(BUY_SELL)) or !has_market() or embargo: return
for g = 0..5 except knowledge, available:                         // ✅ disassembly
    traded = 1
    while bucket[g] < need[g] and traded:
        traded = 0
        if g ≠ wealth and bucket[wealth] − buy_price(g) ≥ need[wealth]:
            do_buy(g) once; traded = 1 regardless of the result      // (a failing do_buy would loop)
        else:
            s = rnd(6)                                                  // DRAW, one per selling round
            for k = 0..5: h = (s + k) % 6; skip knowledge, wealth, oil, g, unavailable h
                if bucket[h] − 100 ≥ need[h] and (income[h] ≥ resource_cap[h]/2 or bucket[h] ≥ 200):
                    do_sell(h) once; traded = 1                        // several goods can be sold per round
```
`buy_price` = first output of `calc_market_prices(g)`. The market / BUY_SELL / embargo tests are repeated inside (an
embargo calls `tell_embargo`, presentation).

### 6.9 Leader::make_this(i) @6c94f0 and the produce_* functions
Dispatch on `it = make_list[i]`:
- building type (414..542): `type_avail ≠ 4` → `produce_tech(t, escrow)` (research the building's tech ❓). Else
  `it.up ≠ 0` → `produce_upgrade(t, city, escrow)`; else `is(TOWN 415)` (✅ disassembly: town / metropolis
  upgrades of a city) → `produce_upgrade`; else city types (`is_city`): fail unless the region of (wx, wy) has free or gathering citizens,
  then `produce_city(t, wx, wy, escrow)`; else (a building in a city): fail if the city slot is invalid or its region
  has no free/gathering citizens; a land building (domain 0) without the 'e' flag (0x10) in a city with
  `land − filled < 2` needs `city.space[clamp(max(xs, ys) − 2, 0, 3)] ≥ 2`; then `produce_building(t, city.o, escrow)`.
- tech (544..628) → `produce_tech(t, escrow)`.
- unit (50..413): `type_avail == 4` → `produce_unit(t, city, num, escrow)`, else `produce_tech(t, escrow)` (the unit's
  upgrade research).
- spell (629..683) → `produce_spell(t, city, escrow)`. Anything else → fail (1).
- Success (0): `it.val /= 100`. Failures return non-zero; AI log messages are presentation.

Common building choice ("best trainer"): a candidate building must be a building (Object vslot +0x20), active
(`flags & 4`), not neutralized (vslot +0x180, always 0), `is(w, strict = t is a building type)` where `w` =
`t.where` (+0x40), and `count_queue(1, t) == 0` (vslot +0x190: nothing of that item already queued) where noted.
Score `s` is compared with strict `>` against the best so far (initial −1), so the **first** best in iteration order
wins ties. `danger` = `max(0, danger[who][(y>>1)·reg_xs + (x>>1)])` at the building. `level` = city level (VILLAGE 1,
TOWN 2, METROPOLIS/FORBIDDENCITY 3). City chains are walked from `city.o` through `Build.city_down` (+0x74).

- **`produce_tech(t, escrow)`** @6ca980: return 0 if `has_tech(t)` or `researching(t)`. `w` a military trainer
  (`is_military_trainer`): candidates = `mil_trainers` (LeaderData +0x6e50, slot order), `s = 1000000/(queued + 1)
  /(damage + 1)`; else for each VALID city with `count_buildings(w, is_build(t), 1) ≠ 0`, its chain: `s =
  level·1000000/(queued + 1)`. Both: `count_queue(1, t) == 0` required; unassimilated (`is_unassimilated`) → `s /= 2`;
  `s /= danger + 1`. Best → `Build::queue_up(t, escrow)` (production-spec §6.2); none → 1.
- **`produce_upgrade(t, city, escrow)`** @6cb5d0 (building upgrades, e.g. a better wall/tower type in place): the
  given city's chain, `s = level·1000000/(queued + 1)`, unassimilated /2, /(danger + 1), `count_queue(1, t) == 0`;
  best → `queue_up(t, escrow)`.
- **`produce_spell(t, city, escrow)`** @6ca720: cities (all, or only `city` when ≥ 0) with `count_buildings(w, 0, 1)
  ≠ 0`; chain buildings with `BuildData::could_queue(t)`: `s = level·1000000/(queued + 1)/(danger + 1)`; best →
  `queue_up`.
- **`produce_unit(t, city, num, escrow)`** @6cb9e0, by `w = t.where` (< 0 → 1):
  - `w` not a military trainer and not `is(FORTX)`: the given city's chain (`is(w, …)`, or `w == TOWN` and the
    building `is(VILLAGE)`), active, not neutralized: `s = level·1000000/(queued + 1)/(damage/50 + 1)/(danger + 1)`,
    unassimilated → /2 (applied before the divisions by damage and danger). Best gets `queue_up(t, escrow)` `num` times.
  - `w` is FORTX: the leader's forts list (`fort_mark` +0x428 entries, VALID), active fort buildings with `is(w)`:
    `s = 1000000/(queued + 1)/(damage + 1)`, unassimilated → /5, /(danger + 1); best: `num` queue_ups.
  - military trainer: `n = num`; for a combat unit (`attack` +0x1e8 ≠ 0) that is not a missile (`is_missile`,
    UnitType vslot +0x114) and `get_diff() ≥ 2`: `n = min(7, t.can_pay_cost(who, −1, −1, escrow))`. `n < 1` → return −1. Repeat n times:
    best of `mil_trainers` with `queued < 4` and `is(w)`: `s = 1000000/(queued + 1)/(damage + 1)`, unassimilated /5,
    /(danger + 1); `queue_up(t, escrow)` on it (so the batch spreads over the trainers as their queues grow). No
    trainer on the first round → 1. Return value: the first `queue_up` result.
- **`produce_city(t, wx, wy, escrow)`** @6cb120: `R = wd(wx, wy).region`. Builder = own citizen (types 50/51, valid,
  on the map) in region R whose order kind is NONE 0, GATHER 7 or EXPLORE_TO 3, minimising `vector_dist(citizen − site)` + 24 when
  gathering (strict `<`, slot order). None → 1. **Pays first**: `t.pay_cost(who, −1, −1, escrow, 0)`; then
  `blocked_site(t, wx·0x300, wy·0x300)` → return 1 **without refund** (🟡 an original quirk), else `b =
  Objects::init_build(who, t, wx·0x300, wy·0x300, 0, −1)` (< 0 → 1). Group: with `city_num == 0` every own citizen,
  else the builder; `action_swarm_around(b, who, QUEUE_NEW, BUILD_AT, 1)`. Census fix-up for the builder's city
  (`find_city` near the builder, 0x200): gathering → `city.gatherers−−`, `gatherers−−`, `reg_gatherers[R]−−`; else
  `city.free−−`, `free_peasants−−`, `reg_free_peasants[R]−−`. Return 0.

`vector_dist(dx, dy)` @46cff0 (ECX/EDX): `a = |dx|`, `b = |dy|`, `M = max`, `m = min`: `M == 0` → 0; `m ≥ 60000`
(the larger of the two compared the other way, see the code) → `(m + 2M) >> 1`; else `m²/(2M) + M` (unsigned). The
4-argument overloads @46d060 / @46f020 forward to it.

### 6.10 LeaderData helpers used by the creators (no RNG)
- `get_city_limit` @6d6130: `epoch[1]` (civic level) `+ 1`, `+ bantu_city_limit` for tribe bonus 3 when `epoch[1] ≠ 0`,
  `+ pyramids_city_limit` with PYRAMIDS (526).
- `get_total_cities` @6d6060: `city_mine` (+0x400) + `num_queued[VILLAGE]` + queued items of every other type whose
  `from` (graft-resolved when `!tribe_can_type`) is VILLAGE (types 0..805, only when VILLAGE is a unit-type… vslot
  0x0c test as decompiled ❓), minus 1 with FORBIDDENCITY (531).
- `get_farm_limit` @6db270: `farms_per_city_base` (Egyptians, tribe bonus 7: `egyptian_farms_per_city_base`) +
  `taj_farms` (wonder 539) + `kremlin_farms` (wonder 538) + `olive_farms` (rare bit 33 = byte 4 bit 1 of `rare` or `rare_conquest`).
- `techs_per_age(age_type)` @6d7280: `s = starting_technology` (game_rules 8) or `starting_age()`; `s == 0` and
  `ending_technology > 6` → `4·age + 2` (age = TechType +0x1c8 of the age type); else `n = ending − s + 1` (0 → 0),
  `v = (age − s + 1)·(28/n) − 2`, and `v == 1` with `s == 0`, `age == 0` → 2.
- `get_diff` (§0), `is_rusher` / `is_boomer` (GameInfo player flags 0x1000 / 0x2000 of the leader's player slot),
  `get_target` @6da000 (next VALID|ACTIVE leader after `start_index[who]` in `start_list`, else self).

## 7. AI building placement: Leader::produce_building(t, city_o, escrow) @6e1400

This is where `make_this` and the scripts' `place_*_with_cost` / `place_orphan_building*` builtins end
(script-spec §7). `city_o` is the city building's object index. Returns 0 on success.

### 7.1 Context
1. `cidx = −1`. If the `city_o` building is active and a city building (`flags & 0x20`) with `city` (+0x72) ≥ 0:
   `cidx = city`, allowed iff that city is VALID. Otherwise allowed iff `t.build_flags & 0x10` ('e', may stand
   outside a city). Not allowed → 1.
2. Centre `(cx, cy)` = the `city_o` building's wcoord, `R0` = its region. `rad = get_radius(city building type)`
   (`LeaderData::get_radius` @6db790: `city_center_radius + (level − 1)·city_center_pop_radius` (+
   `indians_city_radius` for tribe bonus 0x15), max 64); circle index `n = (rad + 2)/4`.
3. `outside = frame ≠ 0 and t.build_flags & 0x10`. If outside: `n += 1`; `tower = is(TOWER)`; unless the type has
   the gatherer flag (0x40), is(DOCK) or is a tower: start index `k0 = is(FORTX) ? 1 : circle_radius[3]` (other 'e'
   buildings start beyond ring 3); `fort = is(FORTX)`. Default `k0 = 1`.
4. `gather = build_flags & 0x40`, forced 0 for FARM, UNIVERSITY and MINE (they are scored separately).
5. Clearance arguments for `check_building_wcoord`: DOCK → `(sx, sy, sum) = (4, 4, 8)`; else `sx = xs < 4 ? 5 − xs :
   1`, `sy` likewise, `sum = (xs < 4 or ys < 4) ? sx + sy : 2`.

### 7.2 OILWELL
`t == OILWELL`: walk `oil_patches` (LeaderData +0x6e88) from the last entry to the first (good object indices):
- invalid good → skip. Its cell: water (land class 1/2, not impassable) needs `leader_flags & 0x700` (any
  CAN_TRANSPORT_* flag ❓ used here as "can reach water"), else skip; land/impassable owned by someone else →
  **remove the patch** from the list, skip.
- tdata at the cell centre `(4x+2, 4y+2)`: needs `& 3 != 3` and `& 0x80` clear. If `wd.flags & 2` (an AI "danger
  here" mark ❓): if no enemy within 0xf00 of the cell (`ObjectsData::find(SEARCH_ENEMY)`) clear the mark; skip
  either way. Else: no enemy within 0x600 → `s = 2·xs − vector_dist(patch − centre)`; `s ≥ best` → best = s,
  position = cell origin + OILWELL footprint centre (`x·0x300 + OILWELL.xs·0x60`, same for y).
- Afterwards, if `best > 0` and the chosen cell is water → `t = OILPLATFORM`.

### 7.3 The ring scan (every other type)
`k0 ≥ circle_radius[n]` → 1. `step = 1`, `best = 0`. For `k = k0; k < circle_radius[n]; k += step` (`circle_x/y[k]`
offsets around `(cx, cy)`; `step` can become 3 and stays 3):
1. Cell `(x, y)` in bounds, `wd.flags & 0x4000` clear, `q = check_building_wcoord(x, y, who, sx, sy, sum,
   !outside) > 1` (q = the free square size found). `m = max(xs, ys)`: `m < 5` needs `q ≥ m` and the cell not on
   the map border; `m ≥ 5` needs `frame < 1` or `2 ≤ x < xs−2`, `2 ≤ y < ys−2`.
2. `t == OILPLATFORM` or `buildings_allowed(x, y)` (`wd.flags & 0x78 == 0`); the cell must be ocean exactly when the
   type is a sea building (domain 1).
3. Position `P` = cell origin + footprint centre, plus `(4 − xs)·0xc0/2` when `xs < 4` (and likewise y): the building
   is centred in the wcoord. `blocked_site(t, P, who, −1, &aux)`; if blocked and the type is a DOCK: try the
   sub-positions `dx ∈ [−sx/2, sx/2]`, `dy ∈ [−sy/2, sy/2]` with `|dx| + |dy| ≤ sum` (dx outer, dy inner), `P' =
   cell origin + footprint centre + (dx, dy)·0xc0`, the first unblocked one is used; otherwise skip the cell.
4. Territory owner of the cell must be −1 or who. Score `s = 1000`:
   - unless (outside and starting_resources ≠ 8 and fort): `f = find_friends(t, x, y, cidx, who)` (§7.5).
     `f == 0`: FARM or MINE → `s = 4000/max(1, vector_dist(cell − centre)) + rnd(500)` (**DRAW**); WOODCUTTER at
     frame 0 → `d > 3` → 500 and `d > 4` → halved (250); other types: `d > 4` → 333. `f ≠ 0`: `s = (f + 2)·1000`,
     towers `·(f + 2)` again. Gather enhancers (GRANARY, LUMBERMILL, SMELTER, REFINERY lines) must lie in the same
     town: `get_town(t, P) == city_o`, else skip.
   - else (outside forts): `s = vector_dist(cell − centre)·1000` (far is better), `/2` when tdata bit 0x100 at the
     cell.
   - non-fort towers: if any TOWER (any owner) within 0x600 of the cell → `s /= 8`.
   - forts: `s += danger/4`; cell's `who2` (+0x10) is who, −1 or an ally → `s /= 2`; else team style 2: `s ·= (who2
     == get_target()) ? 16 : 4`, other styles `s ·= 8`; city valid with `bordering` (+0x65) ≠ 0 and `n <
     circle_radius[3]` → `s ·= 10`.
   - `mult = 1`, `extra = 0`:
     - not a gatherer: `s += 255 − wd.val` (prefer poor land).
     - WOODCUTTER: `s ·= aux³` (aux = tree count reported by `blocked_site`); rejected unless `aux > 2` or frame 0.
     - other gatherers: `World::gather_at(x, y)` → counts `c[g]`; for each good with `c[g] ≠ 0`, not a rare
       (GoodType vslot 0x94), available, and equal to `t.get_good()`: found; if `econ[g] & 8` clear:
       `mult += get_need(g)·c[g]`, else `extra += c[g]`; `s ·= c[g]²`. Not found → reject.
     - then `s = (s + extra)·mult` for gatherers.
5. Accept if `s ≥ best` (ties go to the later cell):
   - `wd.flags & 2`: an enemy within 0xf00 → reject; else clear the flag.
   - city VALID and under attack (`city_flags & 2`): look at the 9 cells (3×3 offset tables @adc400 / @adcaf0) around
     the candidate, walk each cell's object chain (`WorldData::get_down`, then `+0x2c/+0x2e` links): an object of an
     enemy leader (< 8, `is_enemy`) → reject and `step = 3`.
   - `!gather` and `best ≠ 0` and not a tower and `k0 > circle_radius[3]` → `step = 3`.
   - `best = s`, `bq = q`, `bestP = P`.
6. `best == 0` after the scan → 1.

### 7.4 Builder, fine position, construction
1. Tile origin of `bestP`: `tx = tile(bestP.x) − xs/2`, `ty` likewise (tile = coord/0xc0 with the odd-size half-tile
   rule). `r = cidx ≥ 0 ? min(64, get_radius(city type)) : 0x1200`.
2. Builder (frame ≠ 0): own citizens (50/51, valid, on the map) with order kind 0, 7 or 6, standing in `R0`;
   `d = vector_dist(citizen − site)`; must be `< bestd` before the bonus; gathering: `+3r/2` if the gathered building's
   good is food, `+r/2` timber, `+r/3` other; building (6): if the target object's vslot +0x2c (is_wonder) skip the
   citizen, else `+2r`. Strict `<` against the running minimum (init 99999999). None → 1.
3. Fine position:
   - DOCK: keep `bestP`.
   - WOODCUTTER: if `4 − xs > 0` and `4 − ys > 0` (small camps): for `k < radius[4 − xs]` (`radius` table):
     `P = ((xs + 2·(move_x[k] + tx))·0x60, (ys + 2·(move_y[k] + ty))·0x60)`, unblocked with the largest `aux`
     (strict `>`); none → 1. Otherwise keep `bestP`.
   - other types: for `tx' = tx .. tx + 1`, `ty' = ty .. ty + 1` (tx' outer): `P = (xs·0x60 + tx'·0xc0, ys·0x60 +
     ty'·0xc0)`; if not blocked: `v = rnd(100)` (**DRAW**, one per unblocked offset); `v ≥ bestv` (init −1) → take P.
     None → 1.
4. Pay (frame ≠ 0): `t.pay_cost(who, −1, cidx, escrow, 0)`. `b = Objects::init_build(who, t, P, 0, −1)`; < 0 → 1
   (paid, not refunded).
5. Wonders: `wonder_mod = 0`; CTW also updates the conquest wonder bit sets.
6. frame ≠ 0: group of the builder; `action_swarm_around(b, who, builder was building (6) ? QUEUE_LAST : QUEUE_NEW,
   BUILD_AT, 1)`. frame 0 (scenario start / instant AI setup): `b->activate(0, p, 0)` (vslot +0x1a8) at once, with
   `p = 0` for CTW wonders, else 1.
7. City bookkeeping: `city.filled++` (+0x64); if `bq > 1`: `space[size−2]−−` (floor 0) for sizes 2..bq. Builder
   census (frame ≠ 0): gathering → `city.gatherers−−`, `gatherers−−`, `reg_gatherers[R0]−−`; else `city.free−−`,
   `free_peasants−−`, `reg_free_peasants[R0]−−`. Return 0.

### 7.5 BuildTypeData::find_friends(t, x, y, cidx, who) @639270
0 for WOODCUTTER and for armed types (`attack ≠ 0`) other than TOWER and LOOKOUT (521). Otherwise over the 8
neighbour wcoords `k = 1..8` (`move_x/move_y[k]`), weight `+1` for odd k and `+2` for even k (where noted): the
own building placed at the neighbour's centre tile (`find_building_placed_at`), in the same city unless 't' is an 'e'
type; then:
- t a gather enhancer: neighbour's good == t's enhancing good → weight.
- t a military trainer: neighbour a military trainer that is not a city building → weight.
- t a TOWER or LOOKOUT: neighbour a gather building (BuildType vslot 0x90) that is not a UNIVERSITY → +2; a wonder
  neighbour → +4 (+8 for LOOKOUT).
- t is(FARM): neighbour is(FARM) or is(GRANARY) → weight.
- other types: neighbours that are gather buildings (non-university), gather enhancers, or non-city military
  trainers do not count; any other non-wonder building → weight.

## 8. Economy AI

There is no central "assign N citizens to food" planner. The AI economy is the sum of:
1. **Production** decides what exists: `create_units` trains citizens per city against the city's gather slots
   (`City::count_gather_slots`) and pop room, `create_buildings` places farms / woodcutters / mines / oil wells /
   universities and their enhancers, `research_techs` boosts the economy lines by `econ[g]` (§6.3), the bucket
   clamp and `market_speculation` / `use_market` trade surpluses (§6.1, §6.8).
2. **Unit-level auto-gather** (economy-spec §4.6, §4.7, ported): AI citizens carry `UNIT_AI_CONTROL` (0x40000, set
   by `Unit::init` for non-human owners; `Unit::set_ai_control` @605200 has no callers, §12.6) and differ from human idle citizens in
   `think_peasant` @5f5760: threshold `T = 1` idle frame (humans: leader option `peasants`), `think_civilian_transport(1)`
   first (§12), `find_gather_spot(−1)` with unlimited range, and AI units in a region without a city may
   `think_scout`. `find_gather_spot` picks the building with the most room below the resource cap
   (`resource_cap − income`) per distance, so allocation follows the caps and the buildings the production AI places.
3. **AI repair**: AI-controlled citizens every 256 frames (`(o + frame + who) % 256 == 0`) on difficulty > 1 repair
   damaged buildings of their city (economy-spec §4.5 step 7).
4. **Fishermen and merchants**: `think_fish` (one `game_random % 60` per candidate cell) and `think_merchant` for
   AI-controlled boats / merchants (economy-spec §4.7); caravans: `think_caravan` (economy-spec §6).
5. **Orphaned sites**: `check_orphaned_buildings` (§4.6) re-assigns idle citizens to unfinished sites or disbands
   them.
6. **Census side effects**: `produce_building` / `produce_city` decrement the free / gatherer counts of the city and
   region they took a builder from, so later purchases in the same pass see fewer free citizens (§6.9, §7.4).

`LEADER_IDLE_PEASANT` (set by `think` for idle citizens, cleared each `Leaders::process_all`) is UI only for the AI.

## 9. (reserved: no separate section; scripts are in script-spec §6.4)

## 10. Diplomacy AI

### 10.1 Leader::diplomacy @6bc950 ✅ (complete, from the disassembly)

Evidence: decompile `re/decomp/ai/Leader_diplomacy@006bc950.c`, disassembly
`re/decomp/ai/asm/Leader_diplomacy@006bc950.asm` (job `re/jobs/asm_ai_diplo.txt`), helpers `re/decomp/ai_diplo/`
(jobs `re/jobs/dc_ai_diplo_1.txt`, `_2.txt`; asm `re/jobs/asm_ai_diplo2.txt`, `_3.txt`). Every branch below was
followed in the disassembly (✅ unless marked); the decompile's `local_1c[0x124d].on_team[k]`,
`local_1c[0x1255].keystate + who·0x5c + 0x9c`, `local_1c[0x1256].info.elo_loss + n`, `…setup_packets[1]._padding_`,
`…player[5].synced_cheated`, `…game_fps_timing[10]`, `…prevFrameKeystate + 0x34/0x3c` are, in order: `J`'s int
fields (`on_team[2]` = who, `[6]` = score), `J.dip[who].agree`, `J`'s encrypted block (`+8` = bucket[wealth], `+0xe8`
epoch[0] …), `J.blacken` (+0x20c), `J.wars` (+0x9ac), `J.got_diplo_message` (+0x2f0), `J.popwin_timer` (+0x444) /
`J.wonderwin_timer` (+0x44c).

#### 10.1.0 Notation

- `L` = `leaders[who]` (this), `J` = `leaders[j]`, `K` = `leaders[k]` (a third party). `Leader` == `LeaderData` at
  offset 0 (stride 0x6eec).
- `ally(a,b)` = `LeaderData::is_ally` (a == b, or `diplos` 2 both ways); `enemy(a,b)` = `is_enemy` (a ≠ b and either
  side's `diplos` is 0); `peace(a,b)` = neither. The code inlines these everywhere; they are equivalent.
- `human(X)` = `X.leader_flags & 4`; `ACTIVE(X)` = flags & 2; `VA(X)` = flags & 3 == 3.
- `neutral(X)` = `LeaderData::is_neutral`: `setup.team_style == 7` and the lobby slot `p = X.get_player()` has
  `player[p].flags & 1` and `player[p].team == 8`. `get_player` (inlined many times): the first slot 0..7 with
  `flags & 1`, `who` byte == X.who and `flags & 0x50 == 0`; else the last slot that matched who; else 0.
- `unlocked` = `team_style ∈ {0, 8, 11}` (= `!Game::teams_locked()`); `open` = `unlocked or (team_style == 7 and
  (neutral(L) or neutral(J)))`. (Several places test `unlocked` and then `team_style == 7 …`; that inner test is dead
  and is not repeated below.)
- `scripted` = `semaphore byte1 & 0x10 or byte2 & 2`; `dset` = `ScenarioData::diplomacy_setting[who]` (uchar,
  scenario-objects / state-walk). `mp` = `semaphore byte0 & 4` (multiplayer).
- Encrypted values written plain: `bucket(X,g)`, `ages(X)`, `e0(X)`, `e1(X)`, `e2(X)` = `X.epoch[0..2]` (military,
  civic, commerce library levels, `+0xe8/+0xec/+0xf0 ^ 0x63187`).
- `diff` = `L.get_diff()` (§0; it preserves ECX, so no stale-register effects). `seed` = `GameInfo.seed`.
- Division: C truncation. **`seed` is reduced unsigned (`DIV`) only for `T` (step C2); everywhere else
  (`% 10`, `% 15`, `& 7` test) it is signed `IDIV`.** Identical for seeds < 2³¹.
- All draws are `game_random.get(0, 0xFFFF)`: `rnd(n)` = `% n`, `coin` = `& 1` (§0). Every `internal_random` call in
  this function (`get(0,7|8|9)`, 18 sites) only picks a chat line for the console player: presentation, not on the
  simulation stream. The chats (`receive_chat`, `MessageWin::add_feedback`, sounds) are omitted below.
- `x ← stamp` means `x = game.frame`.

#### 10.1.1 Entry and the global pass

```
diplomacy():
    if human(L) or semaphore byte1 & 2 or *ai_off: return            // co-op seats (4|8) included
    topScorer = −1; best = 0; nAbove = 0; nNonAllied = 0
    for k = 0..7 with VA(K):                                          // self included
        if !K.is_ally(who): nNonAllied++                              // self is its own ally → not counted
        if K.score > L.score: nAbove++
        if K.score >= best: best = K.score; topScorer = k             // ties → later k; stays −1 if all < 0
    for j = 0..7: evaluate(j)                                         // 10.1.2 …; "next j" = continue
```

#### 10.1.2 Gate and cadence

```
evaluate(j):
    skip unless ACTIVE(J) (flags & 2 only), j ≠ who, L.treaties[j] & 1 (met), !(L.treaties[j] & 4) ❓(bit 4)
    a = L.agendas[j]
    if a & 4: urgent = 1                                              // set by action_respond (10.1.9)
    else:
        P = 0x800
        if L.dip[j].agree ≠ 1:
            if L.tribute_demanded[j] ≠ 0: P = 0x2000
            if L.counteroffer[j] ≠ 0:     P *= 2
        if !(a & 8): P >>= 2
        if (((j + 8·who)·(P >> 6) + frame) & (P − 1)) ≠ 0: next j     // unsigned
        urgent = 0
    L.agendas[j] = a & ~4
```
So a leader evaluates each met opponent every 512 frames (2048 once `agendas & 8` is set, 8192/16384 while a demand or
counter-offer of its own is outstanding), staggered by `(j + 8·who)`.

#### 10.1.3 Per-j quantities (no RNG)

```
C1  Δ = ages(L) − ages(J)
C2  k = is_rusher(L) ? seed % 3 + 4 : is_boomer(L) ? seed % 6 + 8 : seed % 5 + 6          // unsigned
    T = (((seed + who) & 7) + 3k)·300                                  // "patience" frame, 3600 .. 6900+
    (is_rusher / is_boomer = lobby player flags bits 12 / 13 of L's slot, LeaderData @6d61f0 / @6d6190)
C3  mineHeldByJ = #{J's VALID cities (cities list of j, 0..J.city_mark−1, city_flags & 1) with race (+0x5f) == who}
    jHeldByMe   = #{L's VALID cities with race == j}
C4  third parties k = 0..7 with VA(K), k ≠ who, k ≠ j, !ally(K,J), !ally(K,L):
        lostFounded += #{K's VALID cities with founder (+0x60) == who}
        if L.invaders[k] ≥ 15: invadedBy++
C5  region pass. aggression[j] = weak[j] = strong[j] = 0; half = (L.city_num + L.village_num)/2
    for r = 1..63:
        if L.reg_cities[r] == 0 and L.reg_combat[r] == 0: continue
        if J.reg_cities[r] == 0 and J.reg_combat[r] == 0: continue
        aggression[j] = 1                                              // any shared region
        if L.reg_cities[r] ≥ half:
            v = L.reg_combat[r] + 3·L.reg_defense[r]
            if J.reg_cities[r] ≠ 0: v += L.reg_combat[r] >> 1
            if Δ > 0: v = 4v/3  elif Δ < 0: v = 3v/4
            if J.reg_combat[r] > v: weak[j] += (J.reg_combat[r] << 5)/(v + 1); continue
        if J.reg_cities[r] ≠ 0:
            x = L.reg_combat[r]
            if L.reg_cities[r] ≠ 0 and ally(L,J): x −= J.reg_combat[r]
            strong[j] += x
        elif L.reg_cities[r] ≠ 0:
            strong[j] += (L.reg_combat[r] >> 1) − J.reg_combat[r] + (L.reg_defense[r] >> 1)
C6  weak[j] /= max(1, L.dow[who])            // dow[self] is never written (10.1.9): always /1 in practice
C7  free = ∃ k (VA, k ≠ j, enemy(K,J)) with K.weak[j] == 0              // includes k == who (its new weak[j])
    if (weak[j] ≠ 0 or J.attack > 3·L.attack) and !free: aggression[j] = 0
C8  if Δ > 0: strong[j] = 4·strong[j]/3 + 1  elif Δ < 0: strong[j] = 3·strong[j]/4
C9  patience = 0; capTarget = −1; victTarget = −1
    if L.agendas[j] & 0x10 and !enemy(L,J): strong[j] += L.invaders[j]
C10 if human(J):
        strong[j] += diff ≥ 4 ? 2 : diff == 3 ? 1 : 0
        for T ∈ {BARRACKS 427, STABLE 428, SIEGEFACTORY 430, AIRBASE 447}:
            n(X) = X.num_buildings[T−414] + (T.to ≥ 0 ? X.get_buildings(T.to, X.num_buildings[T.to−414]) : 0)
            if n(L) ≠ 0 and n(J) == 0: strong[j]++                    // (BuildType +0x2e0 "to", §6.3)
        if diff ≥ 3: strong[j] += #{k : VA(K), k ≠ who, k ≠ j, enemy(K,J)}
C11 strong[j] <<= 5; if strong[j] ≤ 0: aggression[j] = 0
```
Field meanings: `invaders[k]` (+0x290) = k's land combat units standing in L's territory (census §4.2 writes
`leaders[k].invaders[who]`; `dow` is +0x270, see 10.1.9);
`strong[j]` = L's surplus strength where both are present; `weak[j]` = j's threat to L's core regions.

#### 10.1.4 Hostility rules (aggression[j], in order)

```
H1  b = 0; if L.agendas[j] & 2: L.agendas[j] &= ~2; b = 1            // bit 2: Unit::resolve_block (a non-enemy's
                                                                       // building blocked L's unit)
    if L.agendas[j] & 0x10 and !enemy(L,J) and L.invaders[j] > (is_boomer(L) ? 4 : 1): b = 1
    e = enemy(L,J) ? 1 : 0;  c = e + (L.pers.friendly_human < 0 ? 1 : 0)
    busy = 0
    if L.wars − b > c: aggression = 0; busy = 1                        // wars: census §4.4
    if L.wars ≠ 0 and (invadedBy ≠ 0 or lostFounded ≠ 0): aggression = 0; busy = 1
H2  myTS = L.get_team_score(); maxTS = max over VA k of K.get_team_score() (init 0)
    nEnemies = #{k ≠ who, VA, enemy(L,K)}; nFriendly = #{k ≠ who, VA, !enemy(L,K), !neutral(K)}
    (J.get_team_score() is also called; result unused.) get_team_score @6d6520: team games (semaphore byte0 sign
    bit) → mean score of who and its is_team members, else own score.
H3  if ally(L,J):                                                     // allied j
        aggression = 0
        if Game::is_victory_timer(): next j                             // standard victory and some VA leader has
                                                                        // popwin_timer or wonderwin_timer ≠ 0
        if open:
            n = #{k ∉ {who, j}: VA(K)}, last = the highest such k
            if n == 1:
                if diff ≥ 4 and human(J): aggression = 1
                if leaders[last].get_team_score() < myTS and L.score > J.score: aggression = 1
H4  if team_style == 7 and neutral(L):                                // L is a "neutral" (team 8) seat
        if myTS == maxTS: aggression = 1  elif myTS ≤ 2·maxTS/3: aggression = 0
        if !ally(L,J) and nEnemies == 0: aggression = 1
        if nFriendly == 0: aggression = 0
        if neutral(J): aggression = 0
        goto H6
H5  if ally(L,J): (diff ≤ 2 and human(J)) → aggression = 0
    elif peace(L,J) and human(J):
        if J.dow[who] < 2 and J.blacken < 4 and J.wars > 0:
            diff ≤ 1 → aggression = 0;  diff == 2 and J.wars > 1 → 0;  diff == 3 and J.wars > 2 → 0
        if !busy and J.wars == 0:
            diff ≥ 4: patience = !(is_boomer(L) and e0(L) < 4)
            diff == 3: DRAW D1 = coin
                       patience = (D1 == 0 and !(is_boomer(L) and e0(L) < 4)) or is_rusher(L)
            diff == 2: patience = is_rusher(L)
H6  if diff ≥ 2:
        if topScorer == j and (!ally(L,J) or nAbove < 2)
           and frame ≥ (diff ≥ 3 ? 30 : 45)·900 and nAbove < (human(J) ? 4 : 2): aggression = 1
        if !ally(L,J) and nNonAllied == 1: aggression = 1
H7  if human(J) and mp:
        ally(L,J) → 0;  enemy(L,J) and J.wars > 1 → 0;  peace(L,J) and J.wars ≠ 0 → 0
H8  if mineHeldByJ ≠ 0 and J.blacken < 2 and !is_rusher(L):
        DRAW D2 = rnd(is_boomer(L) ? 2 : 4); if D2 == 0: aggression = 0
H9  if L.hire_stamp[j] ≠ 0 and frame − L.hire_stamp[j] < 13500: aggression = 1
H10 if jHeldByMe ≠ 0 and mineHeldByJ == 0:
        DRAW D3 = rnd(is_boomer(L) ? 2 : 3)
        if D3 == 0 and (L.hire_stamp[j] == 0 or frame − L.hire_stamp[j] > 4500): aggression = 0
H11 for k = 0..7 with VA(K), k ≠ who, k ≠ j:
        if K.hire_who[who] == k and frame − K.hire_stamp[who] < 9000: aggression = 1; break
        if !enemy(J,K) and enemy(L,K) and L.hire_who[k] == j and frame − L.hire_stamp[k] ≤ 9000 and ally(L,J):
            aggression = 1                                           // ally j abandoned our joint war on k
H12 if L.agendas[j] & 0x20: aggression = 1                           // L already declared on j (10.1.5)
    if patience and frame ≥ T: aggression = 1
```
**H11 correction (port, Leader::init decompile)**: `Leader::init` sets every `hire_who[k]` to **−1** (the per-player
reset loop writes `+0x1d4 + 4k = 0xffffffff`, next to the zeroed agendas / stamps), not 0. `action_respond` is still
the only other writer (with the partner, never the target), so `K.hire_who[who] == k` is false until a hire happens;
the "leader 0 hostility before frame 9000" effect described in earlier revisions does not exist.

#### 10.1.5 Team styles, elimination, victory races

```
H13 if !open:                                                         // locked teams, no neutral involved
        aggression = ally(L,J) ? 0 : 1; goto F (10.1.6)
E1  if setup.elimination == 1:
        L.find_capital(&o, &owner, −1, −1)                            // @6eb930, owner of the capital ❓
        if owner ≠ who:
            if owner == j or ally(J, leaders[owner]):  goto DECLARE_CAP
            capTarget = owner; aggression = 0                          // owner < 0 reads J.chat_status[7] ❓
E2  lone = 0
    if human(J) and diff ≥ 3 and topScorer == j and frame ≥ (seed % 10 + 25)·900: lone = (nAbove < 3)
    if setup.victory ≠ 0 and !lone: goto F
    if !ally(L,J) and (lone or J.wonderwin_timer ≠ 0 or J.popwin_timer ≠ 0
                       or J.has_preq(GLOBAL_GOVERNMENT_BONUS 697) or J.researching(GLOBAL_GOVERNMENT 580, −1, 0, 0)):
        goto DECLARE_CAP
    if setup.victory == 0:
        for k = 0..7, VA(K), k ∉ {who, j}, !ally(K,L), !ally(K,J), (K.wonderwin_timer ≠ 0 or K.popwin_timer ≠ 0):
            victTarget = k; if human(K): break                        // else the last such AI
    goto F
DECLARE_CAP:
    if J.dip[who].agree == 1: action_respond(j, L.dip[j].agree == 1 ? 0 : 2)
    if !enemy(L,J) and war_allowed(): action_declare(j, 0, 1, 0)      // inlined war_allowed, identical
    L.agendas[j] |= 0x20; next j
```
`Game::war_allowed` @594670: `rr = setup.rush_rules`; true if rr == 0 or `Game::current_age()` (max `ages` over VALID
leaders) ≥ rr, or (rr ≥ 9 and frame ≥ `rush_rules.list[rr]+0x3c`·900); else false.

#### 10.1.6 Action selection (F)

```
F1  if L.dip[j].agree ≠ 1 and !urgent and aggression == 0 and L.agendas[j] & 8:
        DRAW D4 = rnd(3); if D4 ≠ 0: next j
F2  cleared = 0
    if L.dip[j].agree == 1 or J.dip[who].agree == 2: action_clear_all(j); cleared = 1
    // agree == 2: no writer exists (InsnGrep), only ScenarioFuncSet::offer_rejected reads it ❓
    if aggression ≠ 0 and !(scripted and dset ≠ 0):
        if enemy(L,J): goto R (10.1.7)                                // already at war: only answer proposals
        goto HOSTILE
    // friendly (or scripted with a diplomacy setting)
    if !urgent and L.invaders[j] ≠ 0 and peace(L,J):                 // "get your army out"
        L.agendas[j] |= 0x18; J.got_diplo_message ← stamp
    if cleared and aggression == 0: next j
    goto R

HOSTILE:   (aggression ≠ 0, not at war)
    if !war_allowed() or !open or L.is_tribute_period(j) or frame < T: goto WARN
    if cleared or J.blacken ≥ 4 or patience or (L.hire_stamp[j] ≠ 0 and frame − L.hire_stamp[j] ≤ 13500):
        goto DECLARE
    D = (e0(L) + 1)·(J.blacken + 1)·100
    if bucket(J, wealth) < D: goto DECLARE
    TRIBUTE DEMAND:
        action_clear_all(j)
        W = D; n = 0
        DRAW D5 = rnd(6)
        for i = 0..5:
            g = (D5 + i) % 6
            skip g ∈ {wealth 2, knowledge 3}; need L.type_avail(g,1), J.type_avail(g,1),
                (L.econ[g] & 0xc) == 4, bucket(L,g) < 1000
            n++
            if n == 1 or (DRAW D6 = rnd(n)) == 0:                     // D6 only for the 2nd+ eligible good
                if bucket(J,g) ≥ 50: action_offer(j, g, −min(bucket(J,g), D)); continue
            W += (25·e0(L) + 25)·(J.blacken + 1)
        action_offer(j, wealth, −min(W, bucket(J, wealth)))
        action_respond(j, 1)                                          // L agrees to its own demand
        L.agendas[j] |= 8; L.tribute_demanded[j] = 1; next j
DECLARE:
    if J.dip[who].agree == 1: action_respond(j, L.dip[j].agree == 1 ? 0 : 2)
    if war_allowed():                                                 // always true here
        J.got_diplo_message ← stamp; L.agendas[j] |= 8
        action_declare(j, ally(L,J) ? 1 : 0, 1, 0)                    // an ally is first dropped to peace
    next j
WARN:      (hostile but not allowed / not yet)
    if J.dip[who].agree == 1: action_respond(j, L.dip[j].agree == 1 ? 0 : 2)
    if L.agendas[j] & 0x48 or (J.got_diplo_message ≠ 0 and frame − J.got_diplo_message < 450): next j
    J.got_diplo_message ← stamp; L.agendas[j] |= 0x40; next j       // threat chat
```

#### 10.1.7 Answering j's proposal (R)

Proposal state: `L.dip[j]` and `J.dip[who]` mirror each other (`treaty`, `attacks[]` copied, `offers[]` negated:
`L.dip[j].offers[g] > 0` = L gives). `J.dip[who].agree == 1` = j has agreed (a human's ACCEPT command, economy-spec
§5.6). `LeaderData::any_proposals(j)` @6d5ad0 = `L.dip[j].treaty ≠ −1` or any `offers[g] ≠ 0` or any `attacks[k] ≠ 0`.

```
R   if J.dip[who].agree ≠ 1: goto P (10.1.8)
    if scripted: dset 1 → next j;  2 → action_respond(j, 1), next j;
                 3 → action_respond(j, L.dip[j].agree == 1 ? 0 : 2), next j;  other → continue
    G = L.good_deeds[j]                                               // goodwill
R1  if L.dip[j].treaty == 2:                                          // alliance proposal
        vt = Game::is_victory_timer()
        refuse = unlocked ? L.num_allies() ≠ 0
               : (team_style == 7 and (neutral(L) or neutral(J))) ? 1 − L.num_team_members(1) + L.num_allies() ≠ 0
               : 1
        if refuse or vt: action_respond(j, L.dip[j].agree == 1 ? 0 : 2); next j
        if !human(J) and aggression == 0: action_respond(j, 1); L.counteroffer[j] = 0; next j
        base = (enemy(L,J) ? 400 : 200)·(e0(L) + 1); if mp: base /= 2
        if is_rusher(L): base += 100  elif is_boomer(L) and e0(L) < 4: base −= 100
        if capTarget ≥ 0: base −= 400;  if victTarget ≥ 0: base −= 200
        G −= base
R2  goods, g = 0..5:  gives = 0; short = 0
        x = L.dip[j].offers[g]; if x == 0: continue
        if !L.type_avail(g,1) or !J.type_avail(g,1):
            action_respond(j, L.dip[j].agree == 1 ? 0 : 2); break     // then R3 if proposals remain
        if x > bucket(L,g): short = 1                                 // unscaled x
        if x > 0: gives = 1
        if x > 0 or gives:                                            // x ≤ 0 is scaled only after a positive good
            if frame < 4500: x ·= 4  elif frame < 9000 and weak[j] == 0: x = 3x/2
        if x > 0 and ally(L,J) and x ≤ bucket(L,g)/3
           and (unlocked or L.gift_stamp[j] == 0 or frame − L.gift_stamp[j] ≥ 4500): continue   // free gift
        G −= resource_value(g)·x/256
    if !any_proposals(j): next j
R3  attacks, k = 0..7, k ∉ {who, j}, L.dip[j].attacks[k] ≠ 0:
        if ally(L,K) or ally(J,K) or (enemy(L,J) and L.dip[j].treaty < 1):
            action_respond(j, L.dip[j].agree == 1 ? 0 : 2); goto R4    // no war on own/j's ally; no help while at war
        if enemy(L,K):  v = enemy(J,K) ? 0 : (weak[k] ≠ 0 ? 120 : 20)·(e1(J) + 1)
        else:           v = (25·K.blacken − (mp ? 50 : 100)·(J.blacken ≠ 0 ? 2 : 1))·(e0(K) + 1)
                            + (weak[k] ≠ 0 ? 20 : 0)·(e1(J) + 1)
        (an `ally(L,K)` formula 50·K.blacken − (mp ? 100 : 200)·… is dead: those k were refused above)
        if k == capTarget: v += 100·e1(K)
        if k == victTarget: v += 200
        if ally(L,J) and v < 0: v /= 2
        G += v                                                        // weak[k] here is L.weak[k]
R4  if !any_proposals(j): next j
    c = weak[j] ≠ 0 ? (diff < 2 ? 100 : 50) : 0
    if aggression ≠ 0:
        H = G + e0(J)·c
        if !mp: H −= (e0(L) + 1)·strong[j]
        if is_rusher(L): H −= 50  elif is_boomer(L) and e0(L) < 4: H += diff < 3 ? 100 : 50
        if diff < 2: H += 100
        X = (e0(L) + J.blacken + 1)·(mp ? 50 : 100)·(strong[j] ≠ 0 ? 2 : 1) − H
    else:
        if !J.is_tribute_period(who):
            G += e0(J)·c
            if L.dip[j].treaty == 1:                                  // peace proposal
                if strong[j] ≠ 0 and weak[j] == 0 and J.wars == 1: G −= (e0(L) + 1)·strong[j]
                else:
                    if J.dow[who] < 2: G += diff < 2 ? 200 : diff == 2 ? 100 : 0
                    if J.dow[who] < 3 and weak[j] ≠ 0: G += 100·e0(J) + 100
        X = −G
    if L.counteroffer[j] ≠ 0: X −= 99
R5  if short and !(X > 0 and gives):                                 // L cannot pay what it would give
        action_respond(j, L.dip[j].agree == 1 ? 0 : 2); next j
    if X ≤ 0:                                                         // ACCEPT
        L.counteroffer[j] = 0
        if any_proposals(j):
            action_respond(j, 1)                                      // executes the deal (j already agreed)
            if !J.is_tribute_period(who) and weak[j] ≠ 0: X += 50
            L.good_deeds[j] −= X                                      // good_deeds grows by the surplus
        next j
    if !gives:                                                        // COUNTER (chat only)
        g* = argmax resource_value(g) over g ≠ knowledge with both type_avail, start (64, wealth), ties → later g
        L.counteroffer[j] = 1; clear_agree(J, who); clear_agree(L, j)
        // the chat asks for ((256·X − 1 + rv)/rv + 99)/100·100 of g*; no offer is placed
        next j
    if L.dip[j].treaty ≠ 2 and aggression ≠ 0 and peace(L,J) and war_allowed() and (DRAW D7 = coin) ≠ 0:
        if J.dip[who].agree == 1: action_respond(j, L.dip[j].agree == 1 ? 0 : 2)   // always true here
        action_declare(j, 0, 1, 0); next j                            // "insulting offer": war
    J.got_diplo_message ← stamp; L.counteroffer[j] = 0                 // REJECT
    action_respond(j, L.dip[j].agree == 1 ? 0 : 2); next j
```
`LeaderData::resource_value(g)` @6c18d0: `econ[g] & 2` and `bucket < 1000` → 512; else `econ & 4` and `bucket <
500` → 384; else `econ & 8` or `bucket > 1000` → 170; else 256. The D7 short-circuit order (✅): treaty ≠ 2,
aggression ≠ 0, j ≠ who, `L.diplos[j] ≠ 0`, `J.diplos[who] ≠ 0`, not both 2, `war_allowed()`, then the coin.

#### 10.1.8 The AI's own proposals (P; j has not agreed)

```
P   if scripted and dset == 1: next j
    if any_proposals(j): next j                                      // something is already on the table
P1  if !ally(L,J) and topScorer == j and nAbove ≥ 4 and J.dow[j] < 2  // dow[self]: always 0 (10.1.9)
       and (DRAW D8 = coin) ≠ 0 and human(J) and L.num_allies() == 0 and J.num_allies() == 0 and unlocked:
        J.got_diplo_message ← stamp; action_clear_all(j); action_treaty(j, 2)
        L.agendas[j] |= 8; action_respond(j, 1); next j               // ask the runaway human leader for alliance
P2  if aggression ≠ 0: next j
    if peace(L,J) and !(scripted and dset ≠ 0) and war_allowed()
       and frame ≥ ((7·who + seed) % 15 + 15)·900 and !(human(J) and seed % 8 == 0):
        if !((J.blacken ≥ 2 and capTarget < 0 and victTarget < 0) or L.score ≥ J.score
             or L.num_allies() ≠ 0 or J.num_allies() ≠ 0 or !unlocked):
            best = −1; bv = 0
            for k = 0..7, VA(K), k ∉ {who, j}, !K.is_ally(who), !K.is_ally(j):
                if k == capTarget: best = k; break
                if k == victTarget: v = 9999
                else:
                    if K.score < L.score or K.score < J.score: continue
                    v = (K.score == topScorer) ? 200 : 100            // ✅ quirk: score compared with an index
                    if K.score ≥ 3·L.score/2: v ·= 2
                    if K.score ≥ 2·L.score:   v ·= 2
                    if enemy(L,K): v ·= 3
                    if !enemy(J,K): v = 3v/2
                    if human(K): v ·= 2
                if v > bv: bv = v; best = k
            if best ≥ 0:                                              // alliance + joint attack on best
                J.got_diplo_message ← stamp; action_clear_all(j); action_treaty(j, 2)
                action_attack(j, best, 1); L.agendas[j] |= 8; action_respond(j, 1); next j
P3  if enemy(L,J) and !(scripted and dset ≠ 0):                       // peace offer
        action_clear_all(j); action_treaty(j, 1)
        if weak[j] ≠ 0 and frame ≥ 9000:                              // with tribute
            a = e2(J) + 1 + (Δ < 0 ? −2Δ : 0); rem = max(100, 100·a)
            DRAW D9 = rnd(6)
            for i = 0..5:
                g = (D9 + i) % 6; skip knowledge; need both type_avail(g,1) and bucket(L,g) ≥ 100
                if bucket(L,g) ≥ rem: action_offer(j, g, rem); break
                y = (bucket(L,g)/100)·100; action_offer(j, g, y); rem −= y
                if rem < 100: L.tribe = 100                           // ✅ quirk, see below
        J.got_diplo_message ← stamp; L.agendas[j] |= 8; action_respond(j, 1)
        (falls through to P4; it stops there because they are still at war)
P4  if !(scripted and dset ≠ 0) and peace(L,J) and aggression == 0 and !(L.agendas[j] & 0x48)
       and (J.got_diplo_message == 0 or frame − J.got_diplo_message ≥ 675):
        L.agendas[j] |= 0x40; J.got_diplo_message ← stamp              // friendly chat
    next j
```
**P3 quirk ✅** (`MOV [EDI+0xc],0x64`, EDI = this): when a partial payment leaves less than 100 owed, the code writes
100 into **`L.tribe`** (+0xc), not into a local. The loop then continues and the next good with ≥ 100 offers the
remaining `rem` (possibly 0: `action_offer(j, g, 0)` still clears agreements). Port literally; what `tribe = 100` does
downstream (every `tribes.list[L.tribe]` lookup) is ❓.

Note the P2 `seed % 8 == 0` skip applies to all human j in a game with that seed (one game in eight never sees an AI
propose joint attacks to humans).

#### 10.1.9 What the action_* functions change

All are deterministic and draw nothing; the `IFaceDiploNeg::check_click_stamp` call each starts with is UI only.
`clear_agree(X, Y)` @6d1af0: if `X.dip[Y].agree == 1`, per good refund `min(dows[g], tributes[g])` and, for
`offers[g] > 0`, `min(offers[g], tributes[g])` from `X.tributes` to `X.bucket` (`bucket_add`), then
`clear_dows`; always `agree = any_offer = 0`. `Diplomacy::clear_all`: `treaty = −1`, everything else 0.

- **`action_offer(B, g, n)`** @6d1780: if `n > 0` and `bucket(A,g) < A.dip[B].offers[g] + n`: return (refused).
  `A.dip[B].any_offer = 1`, `clear_agree(A,B)`, `clear_agree(B,A)` (which resets `any_offer` to 0 again),
  `A.dip[B].offers[g] += n`, `B.dip[A].offers[g] −= n`. Negative n = a demand (no check).
- **`action_treaty(B, t)`** @6d1570: `any_offer = 1`, both `clear_agree`, `A.dip[B].treaty = B.dip[A].treaty = t`
  (1 peace, 2 alliance).
- **`action_attack(B, k, v)`** @6d14e0 (= `action_propose_attack` @6d14d0): `any_offer = 1`; if k ≥ 0: both
  `clear_agree`, `A.dip[B].attacks[k] = B.dip[A].attacks[k] = v`.
- **`action_clear_all(B)`** @6d15e0: both `clear_agree`, `Diplomacy::clear_all` on `A.dip[B]` and `B.dip[A]`.
- **`action_respond(B, r)`** @6d03c0: always `A.tribute_demanded[B] = A.counteroffer[B] = 0` first.
  - r = 0 (withdraw): `clear_agree(A, B)` only.
  - r = 2 (reject): both `clear_agree`, both `clear_all`. (A human A rejecting an AI ally's attack request has a
    dead retaliation loop: it tests `B.is_enemy(B)`.)
  - r = 1 (agree), economy-spec §5.6 for the goods: first agreement (`A.dip[B].agree ≠ 1`): per good with both
    available, `held = max(0, A.offers[g])`, `dowcost` = for each VALID k with `A.dip[B].attacks[k] ≠ 0` and
    `!enemy(A,K)`: `DECLARE_WAR(668)+k` cost of g × (ally(A,K) ? 2 : 1) × (human(A) ? 1 : 0) (assigned, last k
    wins; AI leaders pay nothing); if `bucket < held + dowcost` → return (AI silently). Then debit `held` and `dowcost`
    into `tributes` (`bucket`, `escrow` floored at 0), `dows[g] = dowcost`; `A.dip[B].agree = 1`;
    **`B.agendas[A] |= 4`** (B evaluates A urgently, 10.1.2). Any attack target allied to A or B voids the deal (both
    `clear_agree` + `clear_all`). The deal executes now if `B.dip[A].agree == 1`, or if A's side is a pure gift
    (`treaty == −1`, no attacks, B gives nothing); otherwise it waits for B. Alliances also need `1 −
    num_team_members(1) + num_allies() == 0` for both, else voided. Execution: goods both ways through
    `scale_tribute` and `consider_tribute` (§10.2); treaty t ≠ −1 → `set_diplo(A,B,max(t, A.diplos[B]))`,
    `set_diplo(B,A,max(t, B.diplos[A]))` (treaties only upgrade), t == 1 → `made_peace` stamps both ways, neutral
    (team 8) propagation to team members; if `war_allowed()`: per attacked k: `A.hire_stamp[k] = B.hire_stamp[k] =
    frame`, `A.hire_who[k] = B`, `B.hire_who[k] = A`, and each of A, B not yet at war with k calls
    `action_declare(k, 0, !human, 0)`; finally both `clear_all`.
- **`action_declare(t, d, free, force)`** @6dab50 (diplomacy always passes `free = 1, force = 0`):
  ```
  loop:
      if enemy(L,T): return 0
      if d == 0 and force == 0 and ally(L,T): d = 1; continue           // war on an ally → only break the alliance
      if d == 0 and force == 0:
          s = max(T.broke_alliance[who], L.broke_alliance[t])
          if s ≠ 0 and frame − s < constants.ally_to_war_delay: return 1
      if free == 0 and !afford_dow(t, d, &g): return 1
      if d ≠ 0 or war_allowed():
          if free == 0:                                               // human declarations only
              pay_dow(t, d)                                           // DECLARE_WAR(668)+t / CANCEL_ALLIANCE(676)+t costs
              if ally(L,T) and d == 0: pay_dow(t, 0)                  // (only reachable with force ≠ 0)
              if d == 0 and (ally(L,T) or L.invaders[t] < 5): { if L.dow[t] ≠ 0: L.blacken++ }; L.dow[t]++
              if ally(L,T): { if L.dow[t] ≠ 0: L.blacken++ }; L.dow[t]++
          if ally(L,T): L.broke_alliance[t] = frame
          set_diplo(who, t, d)
          for k = 0..7 VALID, k ∉ {who, t}:                           // team mates follow (is_team(·,·,0):
              if K.is_team(who) and !K.is_team(t) and d < K.diplos[t]: K.ally_diplo(t, d)   //  = is_ally, neutrals
              if !K.is_team(who) and K.is_team(t) and d < K.diplos[who]: K.ally_diplo(who, d) //  never count)
          return 0
      if force == 0: return 1
      d = 1
  ```
  So `dow[]` / `blacken` count a **human's** paid declarations (AI declarations are free and never touch them);
  `dow[self]` is never written, which makes C6 and P1's `J.dow[j]` constant (0).
- **`Leader::set_diplo(t, d)`** @6ec6a0 (via declare / respond / `ally_diplo` @6d0120): no-op if unchanged; leaving
  an alliance clears both `ally_mask` bits and calls `eject_my_shit_from_his_ass` both ways (garrison ejection);
  writes `diplos` both ways; entering an alliance sets `ally_mask` bits (ALLY_LOS or `reveal_map`) and, if no VA
  leader remains outside who's and t's alliances, calls `victory(who)` and sets semaphore bit 0x16;
  **`Armies::diplo_change(who)`** re-runs every valid army of who (`Army::process(A, 1)`, §12.2) — which can draw
  `game_random` through `Army::find_target` (§12.8); `scene.recalc_borders = 1`.

#### 10.1.10 game_random draws in Leader::diplomacy (per evaluated j, in this order)

| # | decompile line / VA | Expression | Condition (all ✅) | Effect |
|---|---|---|---|---|
| D1 | 814 / 6bdab3 | coin | H5: peace(L,J), human(J), !busy, diff == 3, J.wars == 0, not (team_style 7 and neutral(L)) | patience |
| D2 | 920 / 6bdc6d | `rnd(boomer ? 2 : 4)` | mineHeldByJ ≠ 0, J.blacken < 2, !is_rusher(L) | 0 → aggression = 0 |
| D3 | 931 / 6bdcd3 | `rnd(boomer ? 2 : 3)` | jHeldByMe ≠ 0 and mineHeldByJ == 0 | 0 (+ hire test) → aggression = 0 |
| D4 | 1217 / 6be45d | `rnd(3)` | reached F; L.dip[j].agree ≠ 1, !urgent, aggression == 0, agendas[j] & 8 | ≠ 0 → stop for j |
| D5 | 1519 / 6be6eb | `rnd(6)` | HOSTILE tribute demand (after `action_clear_all`) | start good |
| D6 | 1536 / 6be78a | `rnd(n)` | per eligible good after the first (n = eligible count so far) | 0 → demand g |
| D7 | 2961 / 6c03e1 | coin | R5: X > 0, gives, treaty ≠ 2, aggression ≠ 0, peace(L,J), war_allowed() | ≠ 0 → declare war |
| D8 | 2135 / 6c0980 | coin | P1: !ally(L,J), topScorer == j, nAbove ≥ 4, J.dow[j] < 2 (before the human / allies / team tests) | alliance offer |
| D9 | 2337 / 6c1009 | `rnd(6)` | P3: enemy(L,J), weak[j] ≠ 0, frame ≥ 9000 (after `action_treaty(j,1)`) | start good |

Per evaluation the possible sequence is `[D1] [D2] [D3] [D4]` and then at most one of `D5 D6*` (hostile, not at war),
`D7` (j has agreed), `[D8] [D9]` (j has not agreed). Exits before F (H3 victory timer, DECLARE_CAP) skip D4…D9.
Nested draws: any `set_diplo` reached through `action_declare` / `action_respond` (execution) / `ally_diplo` runs
`Armies::diplo_change` (Army::find_target draws, §12.8) in the middle of this sequence.
These rows replace S12 in §13.2.

#### 10.1.11 Port notes

- Evaluation order is leaders 0..7 (strategy_all), then j = 0..7; every action mutates both leaders immediately, so
  later evaluations in the same frame see the new `diplos` / `dip` / `agendas`. The census fields read here
  (`wars`, `attack`, `reg_*`, `invaders`, `score`) are from the last full pass (§3, every 200 frames) and from
  `compute_score`; `weak[]`, `strong[]`, `aggression[]` of j are rewritten at each evaluation of j and read by other
  leaders (C7) and by the military code.
- `agendas[j]` bits: 2 blocked by j's building (movement), 4 urgent, 8 "we talked" (slows cadence, enables D4),
  0x10 invaders complaint, 0x20 declared on j (sticky hostility), 0x40 message sent; 1 and 0x10 can also come from
  scenario scripts (`Leader::agenda_on` @6e11c0 has no direct callers ❓).
- `got_diplo_message` lives on **j** (the message recipient) and rate-limits all AIs talking to that leader
  (450 / 675 frames).
- The 10.2 / 10.3 sections are unchanged.

#### 10.1.12 Open questions

1. `treaties[j] & 4` (evaluation skipped): who sets bit 4 (scenario files carry values 4/5)?
2. `J.dip[who].agree == 2` (F2) has no writer in the binary; can it come from a save or a script?
3. `L.tribe = 100` in P3: downstream effect (tribe-indexed tables, `TribeIndex` checks) and whether the game crashes.
4. E1 with `find_capital` returning owner < 0 (reads `J.chat_status[7]` as a diplo value) — when can that happen?
5. The H11 leader-0 quirk makes every AI except leader 0 hostile to everyone except leader 0 for the first 9000
   frames; confirm against a replay with ≥ 3 AI players (tribute demands / declarations before frame 9000).
6. `strong`/`weak`/`aggression` consumers outside diplomacy (military, `Army::find_target`) should be cross-checked
   against the new semantics of C5 (weak = j's threat to L's core regions).

### 10.2 Leader::consider_tribute(from, amount, good) @6d1360 (no RNG)
Called from `Leader::action_respond` (economy-spec §8, tribute handling) when `from` sends `amount` of `good` to
`who`: if `amount > 0`: `leaders[from].gift_stamp[who]` (+0x194) = frame, and if the receiver is human (flags & 4) or
(`amount ≥ 100·leaders[from].blacken` (+0x20c) and `bucket[good] < 3000` and (good == wealth or `econ[good] & 4`)):
`tribute_stamp[from]` (+0x174) = frame. The stamps feed `diplomacy`'s goodwill tests.

### 10.3 Ally requests: Leader::process_taunt(kind, from) @6b8cc0
Scheduled requests from allies (the 8 `incoming_taunt*` slots processed in `Leaders::process_all` step 6 at their due
frame, frame-spec §4) are acted on by AI leaders (`flags & 4` clear, ACTIVE) only from a mutual ally (or self, which
returns): `last_taunt[from] = kind`, `taunt_frame[from] = frame`, then:
- 1..5 (send food / timber / wealth / metal / oil): both have the good (`type_avail == 4`); unless (teams locked and
  not neutral, or `gift_stamp[from]` within 4500 frames) — then a refusal chat — if `bucket[g] > 149`: (unlocked or
  neutral) `gift_stamp[from] = frame`; `action_clear_all(from)`, `action_offer(from, g, bucket[g]/3)`,
  `action_respond(from, 1)` (a tribute of a third of the stockpile).
- 6 ("what do you need"): reply chat naming the good with the smallest bucket (presentation).
- 7: `wonder_mod = 1` (build a wonder). 16: acknowledge only.
- 8 (land army): `ground_mod <<= 5`, `sea_mod /= 16`, `air_mod /= 16`, `infra_mod /= 4`.
- 9 (navy): `ground_mod /= 16`, `sea_mod <<= 5`, `air_mod /= 16`, `infra_mod /= 4` (✅ the creators use
  ground_mod for domain 0, sea_mod +0x7a0 for domain 1, air_mod +0x79c for domain 2).
- 10 (air): `ground_mod /= 16`, `sea_mod /= 16`, `air_mod <<= 5`, `infra_mod /= 4`.
- 11 (economy): ground/sea/air `/= 4`, `infra_mod <<= 2`.
- 12 (attack): ground = sea = air = 0x200, infra = 0x80, defense = 0x80, `pers.raid = 1`.
- 13 (defend): ground = sea = air = 4, infra = 0x200, defense = 0x80, `pers.raid = −2`.
- 14 / 15: `defense_mod = 0x40` / `0x400`.
The mods (+0x794 wonder_mod 0, +0x798 ground, +0x79c air, +0x7a0 sea, +0x7a4 infra, +0x7a8 defense, all 0x100 from
`Leader::init`) scale the creators' scores (`/256`). Acknowledgement chat lines use `internal_random` (presentation).
(Who issues the requests: the taunt / request UI of allied humans through commands, replay-spec; ❓ the exact
command.)

## 11. Exploring statistics and transport permissions

**`Leader::check_explore` @6bc860** (first call of `strategy_all` for each leader, no RNG; it only counts): at frame 0
and when `(who·25 + frame + 12) % (200/ai_speed) == 0`: with `has_preq(EXPLORE_MAP_BONUS)` `explored = world.reg_size`
(+0x2c); else `explored` (+0x9d4) = number of region-grid cells `(rx, ry)` whose seen byte
`seen2[fog_xs·(4ry + 3) + 4rx + 3]` (World +0x160) has bit `who`. Used by `compute_explore_score` and the score.
Actual exploring is unit-level (`think_scout`, §12).

**`Leader::check_transport` @6bc5f0** (not called from the AI pass: from `Leader::gain_tech`, `gain_building`,
`lose_building`, `Build::activate`, `Build::close`): unless leader_flags2 LOCK_TRANSPORT (0x20): with
`has_preq(TRANSPORT_BONUS)` (686) and a DOCK (or its upgrade line) the leader may ship units: level `L = 3`; the
current level from leader_flags (0x100 → 3, 0x200 → 2, 0x400 → 1, else 0); if different: set CAN_TRANSPORT_CIV |
MIL | SCT (0x100 | 0x200 | 0x400) and, for every valid own unit that `can_ever_transport`, unless the leader option
bit 2 (LeaderOptions +0x1c) is clear and the unit `is(SCOUT 69)` (scouts are only shipped with that option), mark the unit
transportable (`unit_masks |= 0x800000`) when `transport_type ≤ L`. Without the tech or the dock and with any of
0x700 set: clear the three flags and `0x800000` on every unit. (Deterministic state; part of the economy/transport
port, listed here because the AI's `oil_patches` and `think_civilian_transport` test those flags.)

## 12. Military AI

Status: 🟡 draft from the decompiles, with the disassembly checked (✅) for `Armies::process_all`, `Army::process`,
`find_target`, `find_muster_spot`, `do_forming`, `march_to_target`, `send_here`, `is_engaged`, `is_moving`,
`engagement`, `find_besieged_city`, `do_transporting` and `Unit::add_to_army`. Not validated against a running game.

Evidence (local, gitignored): `re/decomp/ai/` (job `re/jobs/dc_ai_1.txt`), helpers in `re/decomp/ai_mil/`
(`re/jobs/dc_ai_mil_1.txt`, `dc_ai_mil_2.txt`: `Group::action_siege_attack_to`, `Group::get_num_cap`,
`LeaderData::find_capital`, `Leader::action_ping`, `Unit::set_group`, …), disassembly `re/decomp/ai_mil_asm/`
(`re/jobs/dc_ai_mil_asm.txt`), PDB types `re/decomp/ai_mil/types_army.txt` (`re/jobs/types_ai_mil.txt`), vtables
`re/decomp/ai_mil/vt_group.txt`, `vt_build.txt`, callers `re/decomp/ai_mil/callers.txt`. Older decompiles used:
`re/decomp/econ_trade/ObjectsData_find_city@0065ba90.c`, `re/decomp/grid/Region_go_here@006810f0.c`,
`re/decomp/move/Unit_go_to_unit@005f78c0.c`.

Conventions as §0. In addition:

- `A` = the army being processed (`Army`, 0xa0 bytes; `ArmyData` is at offset 0, the virtual-base table pointer at
  +0x98). `me = A.who`, `Lme = leaders[me]`. `wc(c)` = `div_3_table[c >> 8]` (world coordinate → wcoord, c/768 for
  c ≥ 0). `centre(w)` = `w·768 + 384`.
- `war(a, b)` = `b ≠ leaders[a].who && (leaders[a].diplos[b] == 0 || leaders[b].diplos[a] == 0)` (this is also what
  `LeaderData::is_enemy` @6ebaa0 computes, combat-spec §2.6); `mutual(a, b)` = `b == leaders[a].who ||
  (leaders[a].diplos[b] == 2 && leaders[b].diplos[a] == 2)`. diplos: 0 war, 1 peace, 2 allied.
- `age(p)` = `leaders[p].data_encrypted->epoch[0] ^ 0x63187` (the **military** library level, production-spec
  §1.5; the code calls it nothing, it is the +0xe8 word). `cur_age(p)` = `ages ^ 0x62766` (the current age).
- Object positions are plain (the stored x/y are `^ 0x63637`). `T(o, w)` = `objects.lists[w][o]`; `as_build(T)` = the
  object's vslot 0xac (`return this` for buildings, `return 0` for units — every army target is a building).
  `T.flags & 0x20` = CITY (a city centre, frame-spec §0). Building fields: `damage` (+0x24, ObjectData),
  `wall_flags` (+0x60, 0x20 = under attack = `WallData::is_under_attack`), `city` (+0x72), `city_down` (+0x74, next
  building of the same city), `hits()` = vslot 0x11c.
- City record (`CityData`, `cities.lists[w][i]`): `flags` (+4: 1 VALID, 2 UNDER_ATTACK, 0x10 CAPITAL, 0x40 ALARM,
  0x2000 CROWDED), `o` (+8), `reg` (+0xa), `x`/`y` (+0xc/+0x10), `attack_stamp` (+0x14), `scouted` (+0x4c),
  `who` (+0x5e), `founder` (+0x60), `ocean` (+0x62), `bordering` (+0x65, bit per player), `was_capital_flags` (+0x68,
  bit per player). City-centre types: TOWN 415, METROPOLIS 416, FORBIDDENCITY 531, anything else = village.
  `ctype(C)` = 1 village, 2 town, 3 metropolis/forbidden city.
- Unit types: GENERAL 54, SPY 58, MERCHANT 61, MERCHANTDUTCH 62, SUPPLYWAGON 63, SCOUT 69, HOPLITES 132,
  CATAPHRACT 227, AAGUN 281, FISHERMEN 317, AIRCRAFTCARRIER 351, FOUCHE 371, FURTRAPPER 400, FARM 417.
  `domain` (ObjectTypeData +0x218): 0 land, 1 sea, 2 air. `unit_flags2` (+0x2b8): 2 SPELLCASTER, 0x10 SPECIAL,
  0x20 HERO, 0x40 SUPPLY. `role` (+0x2c8): 0x10 SCOUT, 0x10000 COMBAT. `carry` (+0x2d4).
- Group fields as orders-spec §1.1; `g.cap()` = `Group::get_num_cap` @7145c0 (number of captains; compacts dead
  members first and normalises small groups — **a mutating call**). Army counts use `Army::count(kind, p)` @6f9120 =
  Σ over the army's groups (list order) of `GroupData::count(kind, p, 0)` @711720. `CountIndex`: 2 ATTACK, 4 SIEGE,
  5 CASTERS, 6 DECOYS, 17 TYPE, 19 NON_DECOY_TYPE.
- Combat stance options: 0 AGGRESSIVE, 1 DEFENSIVE, 3 RAID, 5 HOLD_FIRE. QueuePos: 0 FIRST, 1 LAST, 2 NEW.
  Orders: 1 MOVE_TO, 2 ATTACK_TO, 3 EXPLORE_TO, 26 GARRISON. `move_to(g, x, y, q, set_angle, angle, orders)` is
  `Group::action_move_to(x, y, q, set_angle, angle, orders, explicit 1, form −1, width −1, disembark 0)` @70fba0
  (orders-spec §2.4) unless other trailing arguments are given.

### 12.1 Data model

`Armies armies` @c09700 (0xe8 bytes): `lists[8]` = one `PtrArray<Army>` per player (stride 0x1c: vtable +0, `count`
+4, capacity +8, grow +0xc, data +0x10). `Armies::init` @6f3c10 (from `Game::init_data`) gives **every player exactly
16 Army objects** (`valid = 0`); nothing ever adds or removes slots, so every loop below runs `k = 0..15` in slot
order. `Armies::close` frees them. `ArmiesData::find_dist` @cb4bac is a global written by the `find_*army` searches
(state-walk: walked, so part of the checksum).

`ArmyData` (0x98, PDB names):

| Off | Field | Meaning |
|---|---|---|
| 0x00 | i16 `valid` | slot in use |
| 0x02 | i16 `army` | own slot index |
| 0x04 | `status` | `ArmyStatus` bits: 1 MUSTERING, 2 MARCHING, 4 OFFENSE (assault issued), 8 IDLE, 0x10 FORMUP, 0x20 DEFENDING, 0x40 NEED_TRANSPORT, 0x80 FORCE_PROCESS ("new": process now) |
| 0x08 | `reg` | home region (land region; for navies a sea region ≥ 64) |
| 0x0c | `role` | OR of the groups' `role` (normalize) |
| 0x10 | `num_units` | Σ group `num` |
| 0x14 | `num_captains` | Σ `g.cap()` |
| 0x18 | `num_standard` | Σ (`g.cap()` − casters − supply wagons − decoys − AA guns) (normalize) |
| 0x1c | `num_decoys` | Σ decoys |
| 0x20 | `city` | home/muster city index in `cities.lists[who]` (−1 none) |
| 0x24 | `navy` | 1 for a fleet (init_navy) |
| 0x28 | `human_frame` | countdown set by an ally's ping (12.4.10) |
| 0x2c | `hurry` | set by `find_muster_spot` when it musters next to a defender (12.5.2) |
| 0x30/0x34 | `target_o`, `target_who` | target building (city centre or fort) |
| 0x38/0x3c | `x`, `y` | army anchor (world coords; usually the target or home city, y + 0x300) |
| 0x40 | `angle` | copied from `muster_angle` when mustering ends (no reader found) |
| 0x44 | `rally_dist` | set to 0x1200 on a new target (no reader found ❓) |
| 0x48/0x4c | `muster_x`, `muster_y` | muster spot (wcoords) |
| 0x50 | `muster_angle` | binary angle |
| 0x54 | `list[16]` | group slot ids (`groups[]` index), −1 unused |
| 0x94 | i16 `who` | owner |
| 0x96 | i16 `num_groups` | entries used in `list` |

**Save/checksum** (`Army::walk_data` @6f9850, state-walk): tag, the 2-byte `valid`, and only if `valid ≠ 0` the
raw bytes [0x2, 0x98). The group side of the link is `GroupData.army` (+8, the army slot, −1 none), which survives
`push_group` slot reuse (orders-spec §1.4), so stale `army` values in reused groups are part of the contract.

`Army::init(idx, who, city)` @6f9000: `army = idx`, `who`, `city`; with `city ≥ 0`: `reg = C.reg`, `x = C.x`,
`y = C.y + 0x300`, `muster = (wc(x), wc(y))`; with `city < 0`: `reg = x = y = muster_x = muster_y = −1`. Then
`num_groups = 0`, the four counts 0, `human_frame = role = 0`, `valid = 1`, **`status = MUSTERING`**, `angle =
muster_angle = rally_dist = 0`, `target = (−1, −1)`, `navy = hurry = 0`, `list[] = −1`.

`Army::close` @6f8ea0 (if valid): every listed group whose `army` is this slot gets `army = −1` and
`Group::action_halt(0)` (orders-spec §2.2: clears the members' orders); then `valid = 0`, `status = 0`,
`human_frame = 0`, `num_groups = 0` (target, counts and `list` contents are left as they were).

`Object::get_army` @649d70: units only (vslot 0x18); `g = unit.group`; if `g ≥ 0`, `GroupData::member(g, o, who, 1)`
and `g.army ≥ 0`: return `g.army` if `Army::member(armies[who][g.army], g)` (the group id is in `list`), else −1.

### 12.2 Armies::process_all @6f3b00 (frame step 12) and Army::process @6f93d0 ✅

```
Armies::process_all:                                   // after Groups::process, not in the editor (frame-spec §5)
  for who in 0..7:
    L = leaders[who]
    if !(L.flags & VALID) or (L.flags & 0xc) == 4 (human, no co-op) or (L.flags2 & 0xa) (UNIT_AI_OFF|COMBAT_AI_OFF): continue
    for k in 0..15:
      A = armies[who][k]; if !A.valid: continue
      force = A.status & 0x80;  A.status &= ~0x80
      Army::process(A, force)
```

`Army::process(A, force)`:
```
if A.human_frame ≠ 0: A.human_frame -= 1                     // every call, forced or not
if !force:
    k = 2·A.army + 4·A.who
    if (frame − 30 + k) & 127 == 0:                           // "every 128 frames", staggered
        if Lme.flags & DEFEATED (0x40): return
        normalize(A)
        if A.num_captains == 0: return
        if A.num_standard ≠ 0 and count(NON_DECOY_TYPE, GENERAL) ≠ 0: use_generals(A)
        if count(NON_DECOY_TYPE, SPY) ≠ 0:   use_spies(A)
        if count(NON_DECOY_TYPE, SCOUT) ≠ 0: use_scouts(A)
    if (frame + k) & 255 ≠ 0: return                          // main body every 256 frames, staggered
if Lme.flags & DEFEATED: return
normalize(A)
// (1) disband: nothing left to fight with
if A.num_standard < 1 and A.valid and !(A.status & MUSTERING):
    A.city = 0
    if Lme.city_num > 0:
        while !(cities[me][A.city].flags & VALID):           // note: bound is city_num (+0x3f8), not city_mark
            A.city += 1; if A.city ≥ Lme.city_num: close(A); return
        send_here(A, C.x, C.y, MOVE_TO)                       // C = that city
    close(A); return                                          // close halts the groups again (net effect: stop)
// (2) a human ally's ping
if A.human_frame ≠ 0: send_here(A, A.x, A.y, ATTACK_TO); return
// (3) merge a weak army into a strong one in the same region
if A.num_standard < (A.num_captains − A.num_decoys) / 2:
    for k in 0..15:
        B = armies[me][k]
        if B.valid and B.navy == 0 and B.reg == A.reg and B.num_standard > 4
           and (B.num_captains − B.num_decoys) / 2 ≤ B.num_standard:          // A itself can never pass this
            for i = A.num_groups−1 .. 0:  g = A.list[i]; if g ≥ 0:
                for m = g.num−1 .. 0: o = g.list[m]; if units[me][o].flags & VALID: add_unit(B, o)
            close(A); return
// (4) stopped and not fighting: pick the next muster spot
if !is_moving(A) and !is_engaged(A):
    A.status &= ~(MARCHING|FORMUP)
    if A.target_o ≥ 0 and A.target_who ≥ 0:
        if !war(me, A.target_who) or A.navy:
            find_muster_spot(A, A.target_o, A.target_who, 0)            // result ignored
        else:                                                           // creep halfway towards the target
            A.muster_x = (wc(T.x) + A.muster_x) / 2;  A.muster_y = (wc(T.y) + A.muster_y) / 2
        A.status |= MARCHING|FORMUP
if (A.status & 0x18) == A.status: A.status = MARCHING        // only IDLE/FORMUP (or nothing) left
// (5) state handlers, each re-reading status (a handler can switch the state for the next one)
if A.status & MUSTERING:   set_stance(A, 1); do_mustering(A)
if A.status & DEFENDING:   set_stance(A, 1); do_defending(A)
if A.status & MARCHING:    set_stance(A, 0); do_marching(A)
if A.status & FORMUP:      do_forming(A)
elif is_engaged(A):        engagement(A)
if A.status & NEED_TRANSPORT: do_transporting(A)
if A.navy:                 set_stance(A, 3)                 // fleets always RAID
```

Other callers of `Army::process`: `Armies::emergency` and `Armies::diplo_change` (force 1), `Armies::send_navy`
(force 0, so it only runs on the army's own frames), see 12.3.

`set_stance(A, s)` @6f8750: for each listed group (list order) with `num ≠ 0` whose `GroupData::get_stance_type()`
is COMBAT: `Group::action_stance(s)` (orders-spec §2.3; sets `NEW_THINK` on the members and may clear/repath
orders).

`normalize(A)` @6f9b50:
```
role = num_units = num_captains = num_standard = num_decoys = 0
for i = num_groups−1 .. 0: g = list[i]; if g ≥ 0:
    Group::normalize(g)                                         // orders-spec §1.5
    if g.num == 0 or g.buildings: remove_group(A, g); return    // remove_group re-normalises; this call stops here
    num_units += g.num;  c = g.cap();  num_captains += c;  num_decoys += count(g, DECOYS)
    num_standard += c − count(g, CASTERS) − count(g, NON_DECOY_TYPE, SUPPLYWAGON) − count(g, DECOYS)
                      − count(g, NON_DECOY_TYPE, AAGUN)
    role |= g.role
// stable insertion sort of list[1..] by the leader unit's TypeData.cat (+0x14), ascending
for i in 1 .. num_groups−1 with list[i] ≥ 0 and groups[list[i]].num ≠ 0:
    Li = leader(list[i])      // buildings group: list[0]; else GroupData::find_leader; num < 1 → −1 (unreachable here)
    walk j = i−1 down to 0 over entries with num ≠ 0: if cat(leader(list[j])) ≤ cat(Li): stop; else swap list[j], list[i'] (bubble Li down)
```
(The decompile's index bookkeeping in the sort is convoluted; the effect is an insertion sort by unit category so that
the slowest/heaviest categories march last. 🟡 confirm the tie behaviour against the asm before porting.)

`remove_group(A, g)` @6f8b50: only if `groups[g].army == A.army` and `num_groups ≠ 0`: find `g` in `list`, shift
the rest down, `num_groups −= 1`, `groups[g].army = −1`, then `normalize(A)` (recursion). A group whose `army` field
was overwritten (slot reuse) is **not** removed, and `normalize` then returns early with partial counts every time.

### 12.3 Creation, membership and the Armies helpers

**Who creates armies.**
1. `Leader::plan_strategy` (§4.4): per region with fewer than two mustering/defending armies, `init_army(who, c)`
   for the best city.
2. `Leader::create_units` (§6.5): navies, `init_navy(who, city, sea_region)` when `find_army` finds none.
3. `Unit::add_to_army` (below): a land unit with no army nearby founds one at its nearest own city.

`Armies::init_army(who, city)` @6f36a0: the first slot with `valid == 0`; if all 16 are valid, the slot with the
smallest `num_units` (ties → the **later** slot, `≤`) is **re-initialised in place without close** (its groups keep
`army = k`). `Army::init(k, who, city)`; returns k.

`Armies::init_navy(who, city, reg)` @6f31b0: `k = init_army(who, city)`; `navy = 1`, `reg = reg`.

`Unit::add_to_army` @5f7740 (returns the army slot or −1):
```
a = get_army(); if a ≥ 0: return a
if ptype.domain == 1:                                     // ships: only join an existing fleet
    a = find_army(who, x, y, −1, −, −1); if a < 0: return a
else:
    a = (damage == 0) ? find_local_army(who, x, y, −, o) : find_army(who, x, y, 0x2400, −, o)
    if a < 0:
        c = ObjectsData::find_city(x, y, SEARCH_FRIENDLY, who, −, 0x200 (same region), FILTER_ALL, 0, 0)
        if c < 0: return a
        a = init_army(who, c);  goto add                    // (init_army never returns < 0)
normalize(armies[who][a])
if A.num_units ≠ 0: u = get_unit(A, 0); if u ≥ 0 and u is VALID: Unit::go_to_unit(u, who)
add: Army::add_unit(armies[who][a], o); return a
```
`Unit::go_to_unit(u, w)` @5f78c0: sets `UNIT_IN_COMBAT` (4) on this unit's captain and every follower (`o_down`
chain), then if `vector_dist(|u.x − x|, |u.y − y|) > 0x480`: `Unit::go_to(u.x, u.y, 2, 0, 0x300)`.

Callers of `add_to_army`: `Unit::think` (supply wagons and heroes, 12.6), `Unit::think_attack` (most AI combat units
on their think tick, combat-spec §2.3 step 3), `Unit::think_scout` (sea scouts with nothing left to explore),
`Unit::come_out` (freshly trained AI units, one RNG draw, production-spec §6.4.2 H). `think_hero` / `think_supply`
(@5f4720/@5f4730: `add_to_army(); return 1`) have no callers.

`ObjectsData::find_city(x, y, search, who, −, flags, filter, f1, f2)` @65ba90: `last_who = who`, `find_dist =
99,999,999`; for leaders k = 0..7 that are VALID and pass `Search::valid_search(k, search, who)` (FRIENDLY = k == who),
for cities i < `city_mark`: VALID, same region as `wd(wc(x), wc(y))` when `flags & 0x200`, `d = vector_dist(…)` to
the city ≤ `find_dist` (ties → later), filter → best; writes `objects.find_dist`/`last_who`. No radius limit. The 5th
argument is unused (callers pass junk).

`Army::add_unit(A, o)` @6f9f40:
```
if !objects[me][o].is_active (vslot 8): return
if member(A, o, me): return                                  // Army::member(o, who) @6f8de0: o is in one of A's groups
if A.num_groups < 1 or A.list[0] < 0:
    tmp = new Group (clear(−1), id = −1); tmp.add(o, me, 0, 0)
    g = Groups::push_group(me, &tmp, 1); if g < 0: return
    add_group(A, g)
else:
    g = A.list[0]; n0 = groups[g].num; groups[g].add(o, me, 0, 0)    // vslot 0xc
    A.num_units += groups[g].num − n0;  A.num_captains += 1
Unit::set_group(units[me][o], g, 0)        // sets unit.group on the captain and its o_down chain; does NOT remove
                                           // the unit from its old group (Group::normalize drops it later)
```
`Army::add_group(A, g)` @6f8c00: 16 groups max (else a debug report, nothing added); owner must match. `list[num_groups++]
= g.id`, `g.army = A.army`, `num_units += g.num`, `num_captains += g.cap()`, `role |= g.role`.

`ArmyData::get_unit(i)` @6f9df0: the i-th unit over the listed non-building groups in list order (−1 and a debug
report if `i ≥ num_units`). `Army::member(g)` @6f9a50: `g` is in `list`. `Army::find_waiting_unit` @6fa090 has no
callers. `ArmyData::select` @6f8f20 is UI (select the army's units).

**The Armies helpers** (all scan slots 0..15 in order):

| Function | Behaviour |
|---|---|
| `find_army(who, x, y, maxd, −, u)` @6f34f0 | `R = get_tregion(x/192, y/192)`; `find_dist = 99,999,999`; best = valid army with `reg == R`, `d = vector_dist(\|x − A.x\|, \|y − A.y\|)` with `d ≤ find_dist` (ties → later) and (`maxd < 0` or `d ≤ maxd`); if `u ≥ 0`: a supply wagon only joins armies with `count(TYPE, SUPPLYWAGON) < 3`, a hero only armies with `count(TYPE, GENERAL) < 2`. Returns slot or −1 |
| `find_local_army(who, x, y, −, u)` @6f32e0 | as `find_army` but also requires `A.target_who < 0 or == who` and (`get_diff(who) > 2` or `A.status & MUSTERING`); `d = 90,000,000` for an army with `num_units == 0`; same wagon/hero caps; no `maxd` |
| `find_useful_army(who, x, y, …)` @6f2fe0 | valid, `num_captains ≠ 0`, and (`navy == 0` with `L.flags & 0x700` (any CAN_TRANSPORT) or `reg == R`); `d = vector_dist`, ×3 if `reg ≠ R`; score `d / num_captains`, smallest (ties → later). Used by `Leader::action_ping` |
| `find_aggressive_army(who)` @6f2e10 | the first valid army with `num_captains ≥ 2`, not MUSTERING, standing outside its owner's territory (`wd(wc(A.x), wc(A.y)).who ≠ who`); −1 none |
| `find_city(who, city, −)` @6f3160 | first valid MUSTERING army with `A.city == city` (no callers) |
| `num_armies(who, mask, reg)` @6f3200 | valid armies with `status & mask` and (`reg < 0` or `A.reg == reg`) |
| `emergency(who)` @6f3250 | (AI leader, not UNIT/COMBAT_AI_OFF) every valid army: `target = (−1, −1)`, `process(A, 1)`. From `Object::do_damage` (combat-spec §5.3: a hit on an AI capital/city) |
| `diplo_change(who)` @6f30f0 | same gate: every valid army `process(A, 1)`. From `Leader::set_diplo` |
| `leader_defeated(who)` @6f2f90 | every valid army `Army::stop`. From `Leader::defeat` |
| `send_navy(who, o, w, regA, regB)` @6f2c90 | (`L.flags & 5 == 1`, not UNIT/COMBAT_AI_OFF) every valid army with `navy`, `num_captains > 2`, `reg > 63` whose sea region is coastal to both `regA` and `regB` (`Region::is_coast`): `target = (o, w)`, `process(A, 0)`. From `find_target` |
| `update_city(old_o, old_w, new_o, new_w, −)` @6f2d70 | for every VALID non-human-seat leader p, every valid army targeting `(old_o, old_w)`: target = `(new_o, new_w)`; if `new_w == p`: `status |= FORCE_PROCESS`. From `Cities::capture_city` |

`Army::stop` @6f9180: for each group with members: drop the scenario's ignore-orders objects (`kill`), `action_begin`;
unit groups: `form = −1` and, for each valid on-map member that is not an aircraft (domain 2 without unit flag 0x20)
and not entering/exiting: clear `UNIT_MULTIMOVE`, `+0xc0 = 0`, `close_orders(0)`, `clear_partial_path`,
`update_action`, clear `UNIT_AUTO_EXPLORE`. (The same body as `action_halt`, without `close`.)

### 12.4 The state handlers

#### 12.4.1 do_mustering @6f4260
```
r = release_mustering(A)
if r == 0:
    if A.city ≥ 0 and C = cities[me][A.city] is VALID and find_muster_spot(A, C.o, me, 1): A.status |= FORMUP; return
    if (Lme.strategy[A.reg] & 8) and A.num_captains > 7 and (Lme.flags & 0x300): A.status = NEED_TRANSPORT; goto leave
    A.status = MARCHING; goto leave                  // no muster spot: go anyway
elif A.navy == 0:
    s = Lme.strategy[A.reg]
    if (s & 4) and get_diff(me) < 3: A.status = DEFENDING
    elif (s & 8) and (Lme.flags & 0x300): A.status = NEED_TRANSPORT
    else: A.status = MARCHING
else: A.status = MARCHING
leave: A.city = −1; A.x = centre(A.muster_x); A.y = centre(A.muster_y); A.angle = A.muster_angle
```
(`strategy` bits: §4.4. Bit 4 = threatened region, bit 8 = needs a sea crossing; 0x300 = CAN_TRANSPORT_CIV|MIL.)

`release_mustering(A)` @6f87c0 → 1 = go, 0 = keep gathering. `n = A.num_standard`:
```
C = cities[me][A.city]
if C.who ≠ me or !(C.flags & VALID): return 1
if game.info.rush_rules and !Game::war_allowed(): return 0
if Lme.flags & 8: return 0                                    // LEADER_COOP_SOLO bit ❓ (also stops find_target)
if n < 2: return 0
for p in 0..7 VALID with is_ally(me, p) (includes me ❓):
    for every city of p (index < city_mark) with flags & 3 == 3 (under attack):
        if p == me: return 1;  if n > 4: return 1
if n < Lme.city_num: return 0
if n < 5: return 0
if A.navy: return 1
cap = Lme.pop_cap (+0x7e4)
if Lme.effective_pop (+0x9e0) ≥ 7·cap/8: return 1
if n ≥ cap/8: return 1
if n ≥ cap / (2·num_armies(me, MUSTERING, A.reg)): return 1     // num_armies ≥ 1 (A itself)
d = get_diff(me)
if d == 0 and n > 6: return 1;  if d == 1 and n > 10: return 1;  if d == 2 and n > 12: return 1
if pers.rush (+0x6dd4) == −1: return n > 32
if pers.rush == 1:
    if age(me) < 2: return 1;  if age(me) < 3: return n > 15;  if age(me) < 5: return n > 20
elif pers.early_army (+0x6e20) ≠ 0:
    if age(me) < 2: return 1;  if age(me) < 3: return n > 15
return n > 25
```
(All divisions are C truncation; `7·cap/8` and `cap/8` use the round-toward-zero idiom.)

#### 12.4.2 do_defending @6f4070
```
if count(ATTACK) < 5: close(A); return
if is_engaged(A): return
old = A.status; A.status &= ~OFFENSE
if (old & MARCHING) and A.target_o ≥ 0 and A.target_who ≥ 0:
    T = objects[tw][to]
    if T VALID:
        if T is CITY and as_build(T).city ≥ 0 and cities[tw][city].flags & 3 == 3: return    // still besieged: keep going
        A.target = (−1, −1); A.muster = (wc(A.x), wc(A.y)); A.status = A.status & ~0x22 | FORMUP; return
if find_besieged_city(A): A.status |= MARCHING; return
c = ObjectsData::find_city(A.x, A.y, SEARCH_FRIENDLY, me, −, 0x200, FILTER_ALL, 0, 0)
A.target = (−1, −1); A.city = c
if c ≥ 0:
    A.status &= ~0x22; A.x = C.x; A.y = C.y + 0x300; WorldData::restrict(&A.x, &A.y)
    if find_muster_spot(A, C.o, me, 1): A.status |= FORMUP else close(A)
    return
A.muster = (wc(A.x), wc(A.y)); A.status = A.status & ~0x22 | FORMUP
```

`find_besieged_city(A)` @6f58b0 ✅ — choose an own or allied city under attack:
```
best = −99,999,999; bc = −1
for p in 0..7 with leaders[p] VALID and (p == Lme.who or mutual(me, p)):
    for i in 0 .. Lme.city_mark − 1:            // ⚠ bound is the ARMY OWNER's city_mark, index into p's list (bug)
        C = cities[p][i]; if !(C.flags & VALID): continue
        if A.navy == 0: require C.reg == A.reg  else: require Region::is_coast(regions[A.reg], C.reg)
        if !(C.flags & UNDER_ATTACK): continue
        s = 1000·ctype(C)
        if A.navy and ObjectsData::find_unit(C.x, C.y, SEARCH_ENEMY, me, 0x1200, 0, FILTER_DOMAIN, 1, 0, FILTER_ALL, 0, 0) < 0: s >>= 2
        if p == me: s *= 10
        for k in 0..15: B = armies[p][k]       // ⚠ the armies of p, not of me
            if B.valid and (k ≠ A.army or p ≠ me) and B.city == i and B.num_captains > 4: s >>= 2
        s = s / (vector_dist(|A.x − C.x|, |A.y − C.y|) / 768 + 1)
        if s ≥ best: best = s; bp = p; bc = i
if bc < 0: return 0
C = cities[bp][bc]
if A.navy == 0:
    A.muster_angle = find_angle(C.x − centre(A.muster_x), C.y − centre(A.muster_y))     // old muster
    A.muster = (wc(A.x), wc(A.y))
    A.x = C.x; A.y = C.y + 0x300, clamped to [0, xs·768−1] × [0, ys·768−1]
    A.target = (C.o, bp); return 1
find_muster_spot(A, C.o, bp, 0); A.x = centre(A.muster_x); A.y = centre(A.muster_y); return 1
```
The reading past `p`'s own `city_mark` can touch stale or unused entries of `p`'s city list; the port must index the
same storage (keep each leader's city PtrArray contents and capacity semantics) or reproduce the out-of-range reads'
results exactly. 🟡 Check whether `cities.lists[p]` always has capacity ≥ the largest `city_mark`.

#### 12.4.3 do_marching @6f3df0
```
if count(ATTACK) < 1: close(A); return
(o, w) = A.target
if o < 0: goto retarget
B = as_build(objects[w][o])
if B VALID:
    if !(B is CITY):                               // a fort or other building
        if !war(me, w) and !B.is_under_attack(): goto retarget
    elif B.city ≥ 0:
        C = cities[w][B.city]; f = C.flags
        if !(f & VALID): goto retarget
        if A.navy and !war(me, w) and !(f & UNDER_ATTACK): goto retarget
        if !war(me, w) and !(f & UNDER_ATTACK) and !C.is_unassimilated():
            if B.damage < 3·B.hits(0)/4 and (is_ally(me, w) or Lme.weak[w] or !Lme.strong[w]
                                               or is_tribute_period(me, w)): goto retarget
// (an invalid B, or a city centre with city < 0, keeps the target)
if A.status & MARCHING: march_to_target(A)
return
retarget:
A.target = (−1, −1)
if A.status & DEFENDING: A.status = DEFENDING|FORMUP; return
find_target(A)
if A.target_o ≥ 0: A.status |= FORMUP; do_forming(A)
else: A.status = IDLE
```
(`weak`/`strong` are LeaderData +0x250/+0x230 per player, set by the diplomacy AI §10.)

#### 12.4.4 do_forming @6f43c0 ✅ (march in formation to the muster spot)
```
if is_engaged(A): return 0
P = project(A.muster_angle, 192·num_groups, centre(A.muster_x), centre(A.muster_y))   // combat-spec §0: (x+sin, y−cos)
for i in 0 .. num_groups−1: g = list[i]; if g < 0 or groups[g].num == 0: continue
    g.army = A.army
    if A.target_who ≥ 0 and war(me, A.target_who) and A.hurry == 0 and age(me) ≤ 3 and count(SIEGE) == 0:
        set_stance(A, 3)                                         // RAID
    siege = find_building(P, SEARCH_ENEMY, me, 0x300, 0x200, FILTER_COMBAT, 0, 0) ≥ 0 and count(SIEGE) ≠ 0
    plain = !siege and (A.hurry ≠ 0 or (mutual-or-self target city under attack))
        // exactly: if A.hurry → plain; else if target_who ≠ me and !is_ally(me, target_who) → siege path;
        //          else if target_o ≥ 0, as_build(T).city ≥ 0 and that city's flags & 3 == 3 → plain; else siege path
    if plain: set_stance(A, 0); move_to(g, P, QUEUE_NEW, 1, A.muster_angle, ATTACK_TO)
    else:     Group::action_siege_attack_to(g, P, −, −, A.muster_angle)
    P = project(A.muster_angle + 0x80000000, 384, P)            // next group one wcoord further back
return 1
```

`Group::action_siege_attack_to(x, y, −, −, angle)` @70d830 (the two middle arguments are unused; callers leave the
stack slots uninitialised):
```
if buildings group or num < 1: return  (after dropping scenario ignore-orders members)
S = temp group (id and army copied) of the members that are valid, on the map and siege (type vslot 0x10c)
L = S.find_leader (or −1)
if owner not HUMAN:
    if S empty: add the first live on-map supply wagon member; if none, the first live on-map hero member; L = leader
    // the most central member of S:
    best = 0x0fffffff; for each m in S (list order): s = Σ over live on-map group members of (|dx| + |dy|) / 1024
                       (each term truncated); if 0 ≤ s < best: best = s; L = m
if S empty or L < 0: move_to(this, x, y, QUEUE_NEW, 1, angle, ATTACK_TO); return
if owner not HUMAN and region(wd(x, y)) ≠ region of L's cell (region2 if L stands on the water half of a shoreline
   wcoord): move_to(this, x, y, QUEUE_NEW, 1, angle, ATTACK_TO); return
move_to(S, x, y, QUEUE_NEW, 1, angle, ATTACK_TO)
copy S's formation slots (off_x/off_y/curr_x/curr_y/angles) back to the members' entries in this group and call
Unit::replace_form_id(member, k) for each copied member k
Group::action_guard(this, L, who, QUEUE_NEW, 1)                // the rest escorts the siege leader ❓ (members of S too)
```

#### 12.4.5 march_to_target @6f4d80 ✅ (the assault)
```
if A.target_o < 0: return
if !is_engaged(A): A.status = A.status & ~OFFENSE | MARCHING | FORMUP; return      // fall back to forming
if A.status & OFFENSE: return                                                      // assault already ordered
P = project(A.muster_angle + 0x80000000, 768, A.x, A.y)
ang = find_angle(P.x − T.x, P.y − T.y)                    // T = target building position
if A.target_who ≥ 0 and war(me, tw) and A.hurry == 0 and age(me) ≤ 3 and count(SIEGE) == 0: set_stance(A, 3)
siege = find_building(P, SEARCH_ENEMY, me, 0x300, 0x200, FILTER_COMBAT, 0, 0) ≥ 0 and count(SIEGE) ≠ 0
plain = !siege and (A.hurry ≠ 0 or ((tw == me or is_ally(me, tw)) and as_build(T).city ≥ 0
                                    and that city's flags & 3 == 3))
for i in 0 .. num_groups−1: g = list[i]; if g < 0 or groups[g].num == 0: continue
    g.army = A.army
    if T is CITY and T.damage ≥ 9·T.hits(0)/10:            // nearly taken: everybody to the centre
        move_to(g, T.x, T.y, QUEUE_NEW, 1, ang, ATTACK_TO)
    elif plain: move_to(g, P, QUEUE_NEW, 1, ang, ATTACK_TO)
    else:       action_siege_attack_to(g, P, −, −, ang)
    P = project(A.muster_angle + 0x80000000, 384, P)
A.status |= OFFENSE
```

#### 12.4.6 is_engaged, is_moving, engagement, center_of_gravity ✅
`center_of_gravity(A, &wx, &wy)` @6f8a20: mean `(wc(u.x), wc(u.y))` (C truncation) over `get_unit(i)`, i < num_units,
that are VALID, `is_active` and `is_on_map`; no such unit → `(wc(A.x), wc(A.y))`.

`is_engaged(A)` @6f56d0: `normalize(A)`; count units u (get_unit order) that are active, on map, whose current action
`get_type() == ATTACK (10)`, with `vector_dist(|u.x − A.x|, |u.y − A.y|) < 0xc00` and (COG computed lazily on the
first such unit) `vector_dist(|centre(cog) − u|) ≤ 0xc00`. Engaged iff `count ≠ 0 and count > num_units/4`.

`is_moving(A)` @6f5470 (no normalize): for each unit u (get_unit, skipping −1) active, on map, with a current order
(refreshes the order-list cursor cache: `+0xd4 = head.next`, `+0xcc = node order`, `+0xd0 = node metric`):
if the order is not a move (`UnitOrder::is_move`, vslot 0x14) → count it unless `wd(wc(u.x), wc(u.y)).region ==
A.reg`; if it is a move → count it when `vector_dist(|centre(cog) − u|) < 0xf01` (COG lazily). Moving iff
`num_units/6 < count`.

`engagement(A)` @6f5160:
```
(o, w) = (−1, −1)
for i < num_units: u = get_unit(i); same filter as is_engaged (active, on map, ATTACK action, < 0xc00 of A, ≤ 0xc00 of COG):
    (o, w) = target of Unit::update_action(u) (accessor vslot 0x3c, fields +8/+0xc)
    if o ≥ 0 and w ≥ 0 and is_map_unit(objects[w][o]): break
if o < 0 or w < 0: return        // note: without a break, the LAST examined target is used, unit or not
if A.target_who ≥ 0 and war(me, A.target_who) and A.hurry == 0 and age(me) < 4 and count(SIEGE) == 0: set_stance(A, 3)
for each group with GroupData::num_valid ≠ 0: Group::action_attack(o, w, 0, QUEUE_NEW, 0)
```

#### 12.4.7 do_transporting @6f4690 ✅ (pick an overseas destination)
```
if wd(wc(A.x), wc(A.y)).region ≠ A.reg: return 0
best = −1
for r in 1..63, r ≠ A.reg, with regions[r].size > 0:
    s = Lme.strategy[r]; g = Region::go_here(regions[r], me)
    if !((s & 2) or (g & 2) or (g & 4)): continue
    v = regions[r].size · (g & 4 ? 3 : (s & 2 ? 2 : 1))
    v = v / (vector_dist(regions[A.reg].coords[0] − regions[r].coords[0]) / 4 + 1)
    c = −1
    for i < Lme.city_mark: C = own city i; if VALID and C.reg == r and C.flags & UNDER_ATTACK: v *= C.get_level(); c = i
    if v > best: best = v; br = r; bc = c
if best ≤ 0: return 0
A.reg = br
if bc < 0:
    find_target(A)
    if A.target_o < 0: A.reg = wd(wc(A.x), wc(A.y)).region; return 1          // status stays NEED_TRANSPORT
else:
    A.target = (C.o, me); A.city = bc
    if !find_muster_spot(A, C.o, me, 1):
        A.muster = (wc(C.x), wc(C.y)); A.muster_y += (A.muster_y ≥ ys−1) ? −1 : +1
A.status = MARCHING; return 1
```
`Region::go_here(r, who)` @6810f0: bit 1 when the region has flag 8, `who` has neither cities nor citizens there and
no other ACTIVE leader has cities there with `size < 200·cities`; bits 2/4 for each ACTIVE leader p ≠ who at war
with who that has cities in r: 2 if `who` has no city there or `strategy[r] & 4`, else 4.

### 12.5 Target selection

#### 12.5.1 Army::find_target @6f69b0 ✅ (from the disassembly)

Writes `A.target`, muster spot and anchor, or leaves the target unchanged (callers clear it first, except
`do_transporting`). Three RNG sites (12.8).

**Prologue.**
```
wagons = count(NON_DECOY_TYPE, SUPPLYWAGON)
if Lme.flags & 8: return
siege_ok = A.navy ≠ 0 or (2·count(SIEGE) + count(NON_DECOY_TYPE, CATAPHRACT) + count(NON_DECOY_TYPE, HOPLITES) ≥ 4
                          and (age(me) < 3 or !type_avail(me, SUPPLYWAGON, 1) or wagons ≠ 0))
LeaderData::find_capital(me, &cc, &cw, −1, −1)       // first own VALID CAPITAL city, else a city of another player
                                                    // that was my capital (was_capital_flags) ❓ see below
have_cap = cc ≥ 0 and cw == me;  (capx, capy) = have_cap ? (wc(C.x), wc(C.y)) : (0, 0)
// average sea strength of the two camps (LeaderData +0x94c `sea_combat` ❓: census §4.2 counts sea combat units there)
for p in 0..7 VALID: if p == Lme.who or mutual(me, p): fsum += leaders[p].sea_combat; fn++
                     elif war(me, p): esum += leaders[p].sea_combat; en++
friendly = fn ? fsum/fn : 0;  enemy = en ? esum/en : 0
best = −99,999,999; best_o = best_who = −1
for pass in 0..1:
    for j in 0..7: candidate leader j (below)
    if best_o ≥ 0: goto found
return                                              // nothing in either pass
```
`find_capital`'s fallback loop tests `leaders[this.who].flags & 3 == 3` instead of `leaders[j]` (a bug; harmless when
the AI is active).

**Leader filter** (per pass, j in 0..7; "skip" = next j):
```
if !(leaders[j].flags & ACTIVE): skip
win = 0
if j ≠ me:
    if war(me, j):
        if Lme.defense_mod (+0x7a8) > 0x100: skip
        win = (Lme.wonderwin_timer ≠ 0 or Lme.popwin_timer ≠ 0)          // ❓ me's timers (literal)
    elif mutual(me, j):
        if Lme.weak[j] or !Lme.strong[j] or is_tribute_period(me, j) or Lme.defense_mod < 0x100: skip
    else: skip                                                            // peace, not allied
    d = get_diff(me)
    if d ≤ 1:
        if d == 0 and !(leaders[j].flags & HUMAN): skip
        if leaders[j].frame_attacked (+0xa40) + 7200 > frame: skip
        a = find_aggressive_army(me)
        if a ≥ 0: if a ≠ A.army: skip
        else:     if coin: skip                                           // RNG F1
    if d == 1:
        if A.num_captains ≥ Lme.combat (+0x944) / 2: skip
        a = find_aggressive_army(me); if a ≥ 0 and a ≠ A.army: skip
    if d ≥ 2 and !mutual(me, j):
        if (age(me) > 2 or pers.early_army == 0 or get_diff(me) == 2) and A.num_standard < 7 − 2·pers.raid (+0x6dec): skip
    if d == 2:
        fa = leaders[j].frame_attacked
        if fa + 1800 > frame: skip
        if game.info.team_style ≠ 3 and fa + 3600 > frame
           and leaders[j].attacked_by (+0xa44) ≠ me and Lme.attacked_by ≠ j: skip
if game.info.team_style == 2:                          // also for j == me
    t = the first of start_list[(start_index[Lme.who] + 1 + c) mod 8], c = 0..7, whose leader has flags & 3 == 3
        (none → Lme.who)
    if t ≠ j and j ≠ Lme.who and !mutual(me, j): skip   // only the next player in start order (plus allies)
```
(`frame_attacked`/`attacked_by` are written by `take_damage` for easy games and by `find_target` itself on success.)

**City candidates** — for i in 0 .. leaders[j].city_mark−1, `C = cities[j][i]`:
```
if !(C.flags & VALID): next
if A.navy and !(Region::is_coast(regions[A.reg], C.reg) and C.ocean): next
if get_diff(me) ≤ 1 and mutual(me, j) and C.founder ≠ me: next             // (mutual includes j == me)
S = 900 + rnd(200)                                                          // RNG F2
if have_cap and war(me, j):
    if !siege_ok:
        if C.founder ≠ me: next
        if !is_unassimilated(city centre): next
    S += (vector_dist(|wc(C.x) − capx|, |wc(C.y) − capy|) · −50) / world.xs
if Lme.pop_issues (+0x7e8) and war(me, j): S <<= 2
if leaders[j].wonderwin_timer and C.num_wonders(0) and Game::wonder_winning() == j: S *= 10
if war(me, j):                                                  // enemy city
    if team_style ∈ {2, 3}: S *= 5
    if A.navy: S *= 10
    t = ctype(C); raid = pers.raid
    if raid == −1:      S *= t
    elif raid == 1:
        if A.num_captains ≥ 20 and count(SIEGE) ≥ 3 and Lme.score > 4·leaders[j].score/3: S *= C.get_level()
        else: S *= 4 − t
    else:
        if A.num_captains ≥ 20 and count(SIEGE) ≥ 2: S *= C.get_level()
        elif A.num_captains ≥ 10 and count(SIEGE) ≠ 0: (no factor)
        else: S *= 4 − t
else:                                                           // own or allied city (non-war j reached here is mutual)
    if C.flags & UNDER_ATTACK:
        if team_style == 2: S *= 5
    else:
        if A.num_captains ≥ 20 and count(SIEGE) ≥ 2: S = (4 − C.get_level())·S / 4
        elif A.num_captains ≥ 10 and count(SIEGE) ≠ 0: (no factor)
        else: S *= ctype(C)
        if C.flags & CROWDED: S /= 20
// overseas
if C.reg ≠ A.reg and A.navy == 0:
    S /= 2
    if !(Lme.flags & 0x700): next
    if friendly < enemy or friendly < 5:
        S /= 2
        if friendly < enemy − 2 or friendly == 0: S /= 5
// pass / difficulty
if (pass == 0 or get_diff(me) ≥ 3) and mutual(me, j) and !(C.flags & UNDER_ATTACK): S /= 3
if mutual(me, j) and (C.flags & UNDER_ATTACK):
    if j == me: S *= 10
    if C.flags & CAPITAL: S *= 10
elif war(me, j) and (C.flags & CAPITAL):
    if has_preq(me, GLOBAL_GOVERNMENT_BONUS 697): S <<= 2
    elif pers.raid < 0: S = 3·S/2
if C.bordering & bit(me):
    S *= (pers.raid ≤ 0 ? 4 : 2)
    if war(me, j) and get_diff(me) ≥ 3:
        if leaders[j].frame_attacked + 9000 < frame: S *= 3
        if C.attack_stamp + 9000 < frame: S *= 3
if j ≠ me and mutual(me, j):                                 // allies only: the asm tests j == A.who first
    S = 3·S/4
    if !(C.flags & UNDER_ATTACK): S /= 2
if win: S *= 100
B = city centre (objects[j][C.o])
if j == me: if B.damage: S *= 2
elif B.damage: S = (C.bordering & bit(me)) ? 3·S/2 : 5·S/4
if C.was_capital_flags & bit(me): S = 1,000,000            // my old capital (army bonuses below still apply)
elif war(me, j) and A.navy == 0:
    if age(me) > 2 and get_diff(me) ≥ 3 and !is_unassimilated(B):
        if count(SIEGE) == 0 and age(me) ≥ 4: S /= 5
        if wagons < 2:
            if age(me) > 4: S /= 5
            S /= 5
            if wagons < 1: S /= 5
    if leaders[j].flags & HUMAN:
        if get_diff(me) == 4: S = 5·S/4  elif get_diff(me) == 5: S = 4·S/3
// other armies on the same target (k in 0..15)
for k: B2 = armies[me][k]; if !B2.valid or (B2.status & MUSTERING) or B2.target ≠ (C.o, j): continue
    if k == A.army:
        S *= 2
        if war(me, j) or (C.flags & UNDER_ATTACK): S <<= 2
        if j == me and is_unassimilated(B): S <<= 3
        if j == me and 2·B.damage > B.hits(0): S <<= 3
    elif C.flags & UNDER_ATTACK:
        if mutual(me, j) or get_diff(me) ≥ 3 or A.navy: S <<= 2  else: S /= 4
    else: S /= 4
if S < 0: S = 1,000,000                                     // overflow guard
if S ≥ best: best = S; best_who = j; best_o = C.o           // ties → the later candidate
```
All `/` and `·k/m` steps truncate toward zero (`3·S/2` is `(3S − (3S>>31)) >> 1`, `/4` adds 3 when negative, `/5`,
`/20`, `/3` use the reciprocal-multiply idiom). The order of the multiplications matters for overflow and truncation:
follow the list.

**Fort candidates** (after j's cities, same j): only if `count(SIEGE) ≥ 2`, `wagons ≠ 0` and `war(me, j)`:
for f in 0 .. leaders[j].fort_mark−1, `F = forts.lists[j][f]` (`+2` i16 object, `+4` i16 region, `+6` flags bit 0
valid): require VALID and `F.region == A.reg`:
```
S = (900 + rnd(200)) · (cur_age(j) + 1)                    // RNG F3
if as_build(fort).city < 0: S /= 2
for k in 0..15: B2 as above targeting (F.o, j), not mustering:
    if k == A.army: S *= 2  else: S = fort.is_under_attack() ? S << 2 : S/4
d = vector_dist(|A.x − fort.x|, |A.y − fort.y|)            // inlined copy of vector_dist
S = S / (d/768 + 1)
if S ≥ best: best = S; best_o = F.o; best_who = j
```
(City scores have no army-distance term; forts do.)

**Found.**
```
if best_who ≠ me: A.hurry = 0
if get_diff(me) == 0 and leaders[best_who].flags & HUMAN:           // EASIEST: a two-unit token raid
    u0 = get_unit(0); if u0 < 0: return
    tmp = Group (clear, stamp = frame); tmp.add(u0, me, 0, 0)
    u1 = get_unit(num_units − 1); if u1 ≥ 0: tmp.add(u1, me, 0, 0)
    if tmp.num ≤ 0: return
    L = tmp.buildings ? tmp.list[0] : tmp.find_leader(); if L < 0: return
    g = push_group(me, &tmp, 1)
    if leaders[best_who].combat == 0:
        b = find_building(T.x, T.y, SEARCH_FRIENDLY, best_who, −1, 0x200, FILTER_TYPE, FARM, 0)   // a farm of the human
        if b ≥ 0:
            as_build(b).+0x62 |= bit(me)        // per-player byte on the farm ❓ name
            groups[g].action_stance(5)          // HOLD_FIRE
            groups[g].action_attack(b, best_who, 1, QUEUE_NEW, 0)
            move_to(groups[g], L.x, L.y, QUEUE_LAST, 0, 0, MOVE_TO, explicit 0)   // come back afterwards
    else: move_to(groups[g], T.x, T.y, QUEUE_NEW, 0, 0, ATTACK_TO, explicit 0)
    normalize(A); leaders[best_who].frame_attacked = frame; leaders[best_who].attacked_by = me
    return                                                            // the army's own target is NOT set
if A.target ≠ (best_o, best_who): A.rally_dist = 0x1200
A.target = (best_o, best_who)
if war(me, best_who):
    leaders[best_who].frame_attacked = frame; leaders[best_who].attacked_by = me
    if A.navy == 0:
        r1 = wd(wc(A.x), wc(A.y)).region; r2 = wd(wc(T.x), wc(T.y)).region
        if r1 ≠ r2: Armies::send_navy(me, best_o, best_who, r1, r2)        // may run other armies' process()
A.x = T.x; A.y = T.y + 0x300 (both clamped to the map)
if !find_muster_spot(A, best_o, best_who, 0): close(A); return
A.muster_angle += 0x80000000
```

#### 12.5.2 Army::find_muster_spot(A, o, w, wide) @6f5cc0 ✅

Returns 1/0 (see the last line), writes `muster_x/y`, `muster_angle`, `hurry`, and the target city's CROWDED flag.
`(o, w)` is the target building (a city centre for musters at home).
```
A.hurry = 0; ret = 0; best = 0; bi = 999,999; found = 0
// (a) defending a friendly city: muster on its attackers
if (w == Lme.who or mutual(me, w)) and A.navy == 0:
    A.muster = (−1, −1)
    for b = o, then as_build(b).city_down, … while b ≥ 0:                 // every building of that city
        if as_build(b).damage ≠ 0 and as_build(b).wall_flags & 0x20 (under attack):
            e = find_unit(b.x, b.y, SEARCH_ENEMY, me, 0x900, 0x200, FILTER_COMBAT, 0, 0, FILTER_DOMAIN, 0, 0)
            if e < 0: e = find_unit(b.x, b.y, SEARCH_ENEMY, me, 0x1200, 0x200, FILTER_TARGET, b, w, FILTER_DOMAIN, 0, 0)
            if e < 0 and count(SIEGE) ≠ 0: e = find_building(b.x, b.y, SEARCH_ENEMY, me, 0x1200, 0x200, FILTER_COMBAT, 0, 0)
            if e ≥ 0 and objects.last_who ≥ 0:
                E = objects[last_who][e]; A.muster = (wc(E.x), wc(E.y))
                A.muster_angle = find_angle(A.muster_x − wc(T.x), A.muster_y − wc(T.y)); A.hurry = 1; return 1
            if A.muster_x < 0 or A.muster_y < 0: A.muster = (wc(b.x), wc(b.y))
    if A.muster_x < 0 or A.muster_y < 0: A.muster = (wc(T.x), wc(T.y))
    A.muster_y += (A.muster_y < ys − 1) ? 1 : −1
    A.muster_angle = find_angle(A.muster_x − wc(T.x), A.muster_y − wc(T.y))
// (b) spiral search around the army anchor
(cx, cy) = (wc(A.x), wc(A.y))
if !(T is CITY) or A.navy: r0 = 5
elif is_unassimilated(city centre): r0 = 4
else: r0 = C.get_radius()/4 + 1; if count(SIEGE) == 0 and me ≠ w: r0 /= 2
kmax = wide ? 49 : 9;  r1 = min(64, r0 + 2·(3·A.navy + 1))
for i in circle_radius[r0] .. circle_radius[r1] − 1:                    // combat-spec §2.17.1 tables
    c = (cx + circle_x[i], cy + circle_y[i]); if off the map: next i (no early-exit test)
    score = 0
    for k in 0..15, k ≠ A.army: B = armies[me][k]; if B.valid:
        lim = (me == leaders[w].who or mutual(w, me)) ? 4 : 2
        if vector_dist(|c − B.muster|) ≤ lim: goto cell_done                // too close to another army (ret unchanged)
    for k in 0 .. kmax−1:
        n = c + move[k]                      // compass tables: 0 self, 1..8 ring 1, 9..24 ring 2, 25..48 ring 3
        if n off the map: ret = 0; goto cell_done
        if A.navy == 0 and c + 2·move[k] is on the map:
            q = wd(c + 2·move[k]).who
            if q ≠ −1 and q ≠ Lme.who and !mutual(me, q) and q ≠ w: ret = 0; goto cell_done   // foreign territory
        if k ≥ (A.navy ? 25 : 9): continue
        R = wd(n).region
        if R ≠ A.reg and (A.navy or !(Lme.flags & 0x700) or R > 64): ret = 0; goto cell_done
        f = wd(n).flags
        t = f & COAST ? 3 : f & FOREST ? 4 : f & (MOUNTAIN|CLIFF) ? 5 : f & ROCK ? (f & OIL ? 7 : 6)
            : (is_ocean(n) and f & OIL) ? 7 : wd(n).land
        water = !(f & HALFLAND) and wd(n).land ∈ {1, 2}
        if A.navy == 0: if water or t ∈ {3, 4, 5}: ret = 0; goto cell_done
        else:           if !water: ret = 0; goto cell_done
        if f & BUILDING: if A.navy and k ≥ 9: score += 10
        else: score += lands[t].move_rate            // Land record +0x100 (LandData.move_rate ❓ offset base)
        ret = 1
    if ret and score > best: best = score; bx, by = c; bi = i; found = 1
  cell_done:
    if i − bi > 40: ret = 1; break                   // 40 spiral points past the best: stop
if found:
    A.muster = (bx, by); A.muster_angle = find_angle(bx − cx, by − cy)
    if T is CITY and me == w and as_build(T).city ≥ 0 and that city is VALID: clear CROWDED (0x2000)
    return ret                                       // ⚠ the LAST cell's result (or 1 after the 40-point stop)
if A.navy == 0 and T is CITY and (!mutual(me, w) or T.damage ≠ 0):
    A.muster = (wc(C.x), wc(C.y) ± 1)  (+1 unless at the bottom row, then −1)
    A.muster_angle = find_angle(A.muster_x − cx, A.muster_y − cy); return 1
if T is CITY and me == w and as_build(T).city ≥ 0 and (C.flags & 3) == 1: C.flags |= CROWDED
return ret
```
`ret` keeps its value across cells (it is not reset per cell): a cell rejected by the army-proximity test leaves the
previous cell's value. `find_muster_spot` therefore can return 0 even when it stored a spot, and `find_target` then
**closes the army**. Reproduce exactly.

#### 12.5.3 Charge, send_here, the human ping
`Army::charge(A, o, w)` @6f9a90 (from `Unit::fight` for AI siege units and from `Object::do_damage` when a city's
hits run out, combat-spec §5.3): for each group with members: `g.army = A.army`, `move_to(g, T.x, T.y, QUEUE_FIRST,
0, 0, ATTACK_TO)`, `A.target = (o, w)` (set inside the loop, i.e. only when some group has members).

`Army::send_here(A, x, y, order)` @6f98a0 ✅: clamp to `[0, xs·768−1] × [0, ys·768−1]`; if `x ≠ A.x` **and**
`y ≠ A.y`: `A.muster_angle = find_angle(x − A.x, y − A.y)` (a pure horizontal/vertical move keeps the old angle);
`A.x, A.y = x, y`; `A.muster = (wc(x), wc(y))`; then for each group with members: `g.army = A.army`,
`move_to(g, P, QUEUE_NEW, 1, A.muster_angle, order)` with `P` starting at `(x, y)` and stepping
`project(muster_angle + 0x80000000, 384, P)` per group.

`Leader::action_ping(x, y, pinger)` @6d18a0 (PING command, replay-spec): presentation (`receive_ping`) and, when the
pinger is HUMAN and pings for itself: for each VALID non-human leader j ≠ pinger with `is_ally(pinger, j)`,
`leaders[j].last_taunt[pinger] == 0x10` and `frame ≤ leaders[j].taunt_frame[pinger] + 225`:
`a = find_useful_army(j, x, y)`; if found: `send_here(a, x, y, MOVE_TO)`, `a.human_frame = 2700`. (The chat line uses
`internal_random`: presentation.) While `human_frame ≠ 0` every main `process` re-sends the army there with
ATTACK_TO.

#### 12.5.4 use_scouts / use_spies / use_generals (every 128 frames, 12.2)
`use_generals` @6f4c30 / `use_spies` @6f4af0 / `use_scouts` @6f49a0: walk the groups (list order) and members (list
order); the first active member that is a hero / `is(SPY, 0)` / `is_special` (unit_flags2 & 0x10; scouts, explorers,
commandos) for which `Unit::think_spellcaster` returns non-zero ends the walk.

### 12.6 Per-unit AI: Unit::think @5f6e40 for AI units

`UNIT_AI_CONTROL` (0x40000, `unit_masks` +0x68) is set by `Unit::init` for every unit whose owner is not a human seat
without co-op (frame-spec §3). `Unit::set_ai_control(v)` @605200 sets/clears it but has no callers in the exe.
The combat part of `think` (formation members copy the captain; 16-frame stagger; UNIT_AI_OFF; the near-attacker
engagement and the `think_attack` on-tick) is combat-spec §2.2 steps 1–5. Then (`tick` = `idle == 1 or
(o + frame) % 32 == 0`):
```
6.  if !(L.flags & ACTIVE): return                              // (NEW_THINK stays set)
7.  if is_worker and think_peasant(0): done                     // economy-spec
    if is_caravan and think_caravan(0): done                    // economy-spec
    rare collectors of HUMAN leaders, PACKED: on tick, unless AUTO_EXPLORE (merchants excepted): do_gather(…) and
      unpack_merchant(4) → done; failed → UI notice; idle += 1   (human only)
8.  if (L.flags & HUMAN) or *ai_off:
        if L.flags & HUMAN:
            if !(unit_masks & AUTO_EXPLORE):
                if is_special: if tick and think_spellcaster(): done
                    if idle == 1 and (group < 0 or groups[group].num == 1) and unit_masks & UNIT_MULTIMOVE:
                        unit_masks &= ~UNIT_MULTIMOVE    // + "scout idle" notice for the local player (UI)
            else: think_scout(0)
        if !(unit_masks & UNIT_AI_CONTROL): done
9.  if !tick: done
10. if is(FISHERMEN, 0) and think_fish(): done                  // economy-spec (RNG)
11. if (idle == 1 or (o + frame) % 128 == 0) and is_merchant and think_merchant(): done
12. if L.flags & 8: done                                        // COOP_SOLO bit ❓
13. if ptype.carry ≠ 0 and !is(AIRCRAFTCARRIER, 1) and think_carry(): done
14. if is_supply or is_hero: add_to_army(); done
15. if !(ptype.role & ROLE_SCOUT) and !is(SPY, 0): done
16. if get_army() < 0: think_scout(0)
done: flags &= ~NEW_THINK
```

#### 12.6.1 Unit::think_spellcaster @5f27a0
`ok(s)` = `SpellTypeData::is_castable(spelltypes[s], o, who, 0)` and `mana() ≥ mana_burn (+0x96) + spell.cost (+0x1d0)`.
`cast(s, t, tw)` = `add_cast_order(t, tw, x, y, s, QUEUE_FIRST, 0)`, return 1. Spells: BRIBE 629, COUNTERINTEL 631,
CREATE_DECOY 634, AMBUSH 635, FORCED_MARCH 636, SNIPER 641.
```
if L.flags & HUMAN:
    if !is_special or !ok(COUNTERINTEL): return 0
    t = ObjectsData::find(x, y, SEARCH_ALL, who, range(631), …, FILTER_SPELL_TARGET, 631, …); t < 0 → 0; cast(631, t, last_who)
if get_diff(who) < 2 or unit_masks & UNIT_DECOY: return 0
if is(SPY, 0):
    if !is_cloaked: return 0
    if ok(631): t = find(x, y, SEARCH_FRIENDLY, who, 5·range(631), …, FILTER_SPELL_TARGET, 631); t ≥ 0 → cast(631)
    if ok(629): t = find_unit(x, y, SEARCH_ENEMY, who, 5·range(629), 0, FILTER_SPELL_TARGET, 629, who, FILTER_COMBAT, 0, 0); t ≥ 0 → cast(629)
    return 0
if !is_special:
    if !is_hero: return 0
    if !is_moving:
        if any own unit (0..unit_mark) is VALID, a captain and a decoy: return 0
        if mana < cost(634) + mana_burn: return 0;  cast(634, −1, −1)
    else:
        if guys[0].+0x84 == 0: return 0                               // ❓ guy field
        if !(L.flags & LEADER_MARCH_GENERAL 0x8000) and coin:          // RNG S2 (only drawn without MARCH_GENERAL)
            mana check 636; cast(636, −1, −1)
        else:
            q = wd(wc(x), wc(y)).who; if q < 0 or q == who: return 0
            mana check 635; cast(635, −1, −1)
// special units
if ok(641): t = find_unit(x, y, SEARCH_ENEMY, who, range(641), 0, FILTER_SPELL_TARGET, 641, who, FILTER_COMBAT, 0, 0); t ≥ 0 → cast(641)
r = Random::get(0, 0xffff)                                            // RNG S1, always drawn here
if r % 3 ≠ 0 and get_army() < 0: return 0
if !ok(631): return 0
t = find(x, y, SEARCH_ALL, who, range(631), …, FILTER_SPELL_TARGET, 631, …); t < 0 → 0; cast(631, t, last_who)
```
(Exact `ObjectsData::find` argument list: see the decompile; 🟡 not re-derived from the asm.)

#### 12.6.2 Unit::think_scout(region_mode) @5f6010
Auto-exploration for AI scouts, spies, sea units and citizens (and human AUTO_EXPLORE units). `R` = `get_tregion`
of the unit; `(ux, uy)` = its wcoord; `best = 99,999,999`; target tcoord `(tx, ty)`.
```
if domain == 0 and Unit::find_goody_box(): return 1                  // fog-spec §… (no RNG)
if !(L.flags & HUMAN) and is_active and (unit_flags2 & SPELLCASTER) and think_spellcaster(): return 1
// (A) region_mode: a random passable cell of the own region
if region_mode and regions[R].size > 5:
    i = rnd(size)                                                     // RNG T1
    repeat while wd(coords[i]).flags & 0x70:  i = rnd(size); tries += 1; if tries > 15: return 1    // RNG T1'
    target = (4·coords[i].x + 2, 4·coords[i].y + 2); goto issue
// (B) scout enemy cities (AI land units that are not citizens, spies or sea units, and not AUTO_EXPLORE)
home = ObjectsData::find_city(x, y, SEARCH_FRIENDLY, who, −, 0x200, FILTER_ALL) (−1 → 0)
if !AUTO_EXPLORE and type ∉ {50, 51} and !is(SPY) and domain ≠ 1 and L.city_num ≠ 0:
    rings = 12
    for jj in 0..7: j = (who + jj) mod 8
        if !(leaders[j] ACTIVE) or leaders[j].reg_cities[R] == 0 or (j ≠ who and is_ally(j, who)): continue
        for ci in 0 .. leaders[j].city_mark−1: C = cities[j][((j == who ? home : 0) + ci) mod city_mark]
            if !VALID or (domain == 0 and C.reg ≠ R) or C.scouted & bit(who): continue
            step = 1; lim = rings
            if unit_masks & AI_CONTROL: if j == who: step = 2 else: rings = lim = 3
            for r = 1; r < rings; r += step:
                m = frame mod 8;  stride = r/4 + m + 1
                s0 = rnd(circle_radius[r])                             // RNG T2 (circle_radius[r] > 1 always)
                jit = (r/4 + m ≥ 1) ? rnd(stride) : 0                  // RNG T3
                for p = circle_radius[r−1] + jit; p < circle_radius[r]; p += stride:
                    q = circle_radius[r−1] + (s0 + p) mod (circle_radius[r] − circle_radius[r−1])
                    c = (wc(C.x) + circle_x[q], wc(C.y) + circle_y[q])
                    if lim + 1 < r: r = rings + 1; break               // stop two rings past the best ring
                    if off the map: continue
                    eligible = domain == 2 or (wd(c).region == R and (tdata(4c+2) & 0x30 == 0x20) == (domain == 1))
                    if !eligible: continue
                    evals += 1; if evals > 0x600: goto (C)-check
                    q2 = wd(c).who (−1 → who)
                    if (!was_really_seen(2c+1, who) or (q2 == who and wd(c).flags & GOODY)) and !invalid_loc(4c+2):
                        s = 8·vector_dist(|ux − c.x|, |uy − c.y|) + (get(0,0xffff) & 7)      // RNG T4
                        if j ≠ who and (j < 0 or !(Lwho.treaties[j] & 3)): s *= (leaders[j] HUMAN ? 2 : 4)
                        s += (q2 ≠ who ? 4 : 0) + danger[who][cell(c)]
                        if L.city_num == 0 and wd_goods(c) & 2: s /= 2
                        elif L.city_num == 1 and wd_goods(c) & 0x10: s /= 2
                        if s < best and find_unit_ordered(centre(c), who, 0x600, 0x200, FILTER_BASE_TYPE, base_type,
                                                          0, FILTER_NOT_ME, o, who) < 0:
                            best = s; target = 4c + 2; lim = min(lim, r)
            if r == 8 + 4·(j == who): C.scouted |= bit(who)
            rings = 8
            if j ≠ who: break                                          // one foreign city per leader
    (C)-check: if best ≤ 199: goto issue
// (C) explore unexplored cells of the region
spyish = !(L.flags & HUMAN) and (is(SPY) or is_special or domain == 1)
n = max(1, (regions[R].size + 99)/100);  stride = n + frame mod 8
i0 = stride ≤ 1 ? 0 : rnd(stride)                                     // RNG T5
for i = i0; i < size; i += stride: c = coords[i]
    if who < 8 and reveal_map ≠ 3 and !(Lc.flags & REVEAL_ENEMY) and Lc.num_units[FOUCHE 371] == 0
       and !(seen2(2c+1) & Lc.ally_mask) and !invalid_loc(4c+2):
        eligible = domain == 2 or (domain == 0 or inside something ? tdata(4c+2) & 0x30 ≠ 0x20 : is_ocean(c))
        if eligible:
            s = 16·vector_dist(|ux − c.x|, |uy − c.y|) + (get(0,0xffff) & 7)                   // RNG T6
            q2 = wd(c).who (−1 → who)
            if !spyish: s += danger[who][cell(c)] + (q2 ≠ who ? 4 : 0)
            else: if q2 ≥ 0 and !is_ally(who, q2): s /= 4;  if tdata(4c+2) & 0x100 (CITY tile): s /= 2
            if 2s < best and find_unit_ordered(... as above) < 0: best = 2s; target = 4c + 2
if best > 99,999,998:                                                  // nothing to explore
    if domain ≠ 2: regions[R].scouted |= bit(who) (BitMask cache word +0x3c = 0)
    if domain == 1: return add_to_army() ≥ 0
    if type ∈ {50, 51} and !AUTO_EXPLORE and L.reg_cities[R] == 0: return 0
    return think_civilian_transport(0)
issue: tmp = Group with this unit; g = push_group(who, &tmp, 1)
       move_to(groups[g], tx·192 + 96, ty·192 + 96, QUEUE_NEW, 0, 0, EXPLORE_TO, explicit 0); return 1
```
`cell(c)` = `(c.y >> 1)·world.xs2 + (c.x >> 1)` (World +0x24); `tdata(t)` = the tcoord word at `(4c.x+2, 4c.y+2)`;
`seen2` byte at `(2c.y+1)·fog_xs + 2c.x+1`. 🟡 The decompile of (B) is the hardest part of this function to read;
loop-control details (`lim`, `rings`, the "+1 break") should be re-checked against the asm before porting.

#### 12.6.3 Transports: think_carry family (no RNG)
Auto-transports are temporary boats; an empty one removes itself (`die`, vslot 0x158).
- `Unit::think_carry` @5f2ee0: cargo has workers → `think_carry_civilian(1)`; has scouts → `think_carry_civilian(0)`;
  no attack units → `think_carry_idle`; else the military body (inlined copy of `think_carry_military` @5f3260):
  the first passenger's (`inside_down` +0x28 / `+0x3e`) group → army. No army: `eject_contents(0, −1, 0, 1)`; empty
  afterwards → die; else go to the nearest own city (capital distance ÷4), region = its region, target = its centre.
  With army: region = `army.reg`, target = `army.target`. If any of the 9 cells `move[0..8]` around the boat is in
  that region: unload (`eject_contents`), die if empty, return 1. Destination: the target's wcoord if alive, else
  `regions[region].coords[0]`, overridden (when `strategy[region] & 4`) by the last own valid city there (a capital
  wins at once). The order: `close_orders`, then a MOVE_TO built in place (48-unit cell centre, facing
  `find_angle`) pushed onto the order list, `update_action`.
- `think_carry_idle` @5f2f30: nearest own city with a DOCK on a coast of the boat's region →
  `UnitType::find_nearby_spot(city, 0xf00, …)`; a different wcoord → MOVE_TO there.
- `think_carry_civilian(workers)` @5f37c0 and `Unit::think_civilian_transport(flag)` @5f40d0 (citizens/scouts that
  need a crossing, economy-spec §… calls it with 1): score sea-coast cells of candidate regions (`Region::go_here`,
  `Region::coast_here`, danger-weighted distance), start index `(o + frame) mod stride` (deterministic), then
  `action_move_near` / MOVE_TO and `action_eject_all`. 🟡 Only summarised here; specify with the economy AI.

#### 12.6.4 Exploring helpers
- `Unit::do_explore_to(order)` @5f24a0 (EXPLORE_TO executor): `do_move(order)`; every 15 frames (`(o + frame) % 15
  == 0`), if the order is still the current one and the unit is a captain: `find_goody_box()`.
- `Unit::explore_goody` @5f9780 (from `set_new_location` when a non-decoy land unit enters a GOODY wcoord;
  economy/fog, listed for the RNG): clears the GOODY flag and the item terminal; if `frame ≠ 0`: for goods
  g ∈ {0,1,2,4,5} (3 skipped) with `type_avail(g, 1)`: `v = bucket[g] + rnd(25)` (**RNG G1**, one draw per eligible
  g, in g order); smallest v wins (strict, default wealth 2); amount = `epoch[3]·goody_box_age + goody_box` (Spanish
  tribe bonus 9: `spanish_ruins`/`spanish_ruins_base`); `bucket[g] += amount`, `goody_box_resources += amount`.
- `Leader::check_explore` and `Leader::check_transport`: §11.

### 12.7 Group::action_alarm @70ec30 and action_alarm_peasant @6fd980 (town bell)

Not AI-only: the ALARM command (`process_alarm`), `Build::process` (city centres, production-spec §… "alarm reset")
and `Object::take_damage` (AI peasant/building alarm, combat-spec §5.4) call it with a temporary group.
```
action_alarm(G):
  tmp = Group (clear, stamp)
  drop the scenario's ignore-orders objects from G
  if !G.buildings: action_alarm_peasant(G); return
  action_begin; leaders[G.who].flags |= RECOMPUTE (0x2000000)
  rang = cleared = 0
  for each member b (list order), live (vslot 0x10; else a debug report) and CITY with as_build(b).city ≥ 0:
      C = cities[who][city]; if !(C.flags & VALID): continue
      if !(C.flags & ALARM):
          if cleared: continue
          (sound once, UI) rang = 1
          cap = num_inside(centre, 0) used; lim = garrison_limit(centre)
          for each building x of the city (centre.city_down chain) whose type the citizen type can garrison
              (citizen type = 50, or the tribe's graft): used += num_inside(x); lim += garrison_limit(x)
          rad = C.get_radius(); n = (rad + 3)/4; tmp.clear
          for i < circle_radius[n] around wc(C) (in bounds), while used < lim:
              if find_city_at(centre(cell), who, −1, 0) ≠ this city: next
              walk the cell's object list (WData.down/down_who, newest first) while used < lim:
                  objects of other players: skip
                  not (active and on map) → a building of type vslot 0x90 (gatherer building): walk its
                      gather list (+0x70, then unit +0x92) for citizens (50/51) not yet in tmp:
                      tmp.add(u); used += decoy ? 0 : ptype.control_cost (+0x2f0)
                  active on map citizen within vector_dist ≤ rad of … ❓ (args): tmp.add(u); used += control_cost
          if tmp.num: tmp.action_garrison(C.o, who, QUEUE_NEW, 1); C.flags |= ALARM
      elif !rang:                                       // all clear
          (sound once, UI) cleared = 1
          for i < circle_radius[(rad+3)/4] around wc(C): for each own live on-map citizen in the cell's list within
              rad whose current action is GARRISON (26): repath, kill_current_order(0)
          for each building x of the city chain that is ACTIVE (flags & 4; vslot 0x4c for non-Build):
              for each unit inside (+0x28 chain, who +0x3e): if is(MILITIA 66): SpellType::cast_civilian(u, w)
              Object::eject_contents(x, 0, 50 (citizens), 0, 0)
          C.flags &= ~ALARM

action_alarm_peasant(G):                                // unit groups
  action_begin; drop ignore-orders objects
  for each member u (list order) that is a citizen (50/51):
      b = find_building(u.x, u.y, SEARCH_FRIENDLY, who, −1, 0x200, FILTER_CAN_GARRISON, u, who)
      if b < 0: continue
      while the current action is GARRISON: repath(u); kill_current_order(u, 0)
      build a GARRISON order (target b, who, uid = b+0x30, flag +0x14 = 1, order flag 4), push it at the head of u's
      order list, clear_partial_path, cursor = head, update_action
      leaders[who].flags |= RECOMPUTE
```
No RNG in either function. 🟡 The vector_dist arguments in the on-map citizen test were not re-derived from the asm.

### 12.8 Every game_random draw in the military AI

All draws are `Random::get(game_random, 0, 0xffff)` followed by the stated reduction (`% n` on a non-negative value,
or `& 1` / `& 7`). Listed in call order within each function.

| # | Function @ call | Expression | Drawn when | Count |
|---|---|---|---|---|
| F1 | `Army::find_target` @6f6dbb | coin; odd → skip leader j | j ≠ me, ACTIVE, passes the war/ally filter, `get_diff(me) ≤ 1` (EASIEST also needs j HUMAN), `leaders[j].frame_attacked + 7200 ≤ frame`, and `find_aggressive_army(me) < 0` | ≤ 1 per j per pass |
| F2 | `Army::find_target` @6f718a | `900 + rnd(200)` | every city candidate that passed the VALID / navy-coast / easy-founder tests (before the siege_ok/founder test) | 1 per candidate city per pass |
| F3 | `Army::find_target` @6f8026 | `(900 + rnd(200))·(cur_age(j)+1)` | every valid fort of an at-war j in `A.reg`, when the army has ≥ 2 siege and ≥ 1 wagon | 1 per fort per pass |
| S1 | `Unit::think_spellcaster` @5f2bae | `% 3` (non-zero and no army → return 0) | AI special unit (not spy/hero), difficulty ≥ 2, not a decoy, after a failed SNIPER attempt | 1 |
| S2 | `Unit::think_spellcaster` @5f2d24 | coin (odd → FORCED_MARCH) | AI hero that is moving, `guys[0]+0x84 ≠ 0`, leader without MARCH_GENERAL | 1 |
| T1 | `Unit::think_scout` @5f6130, @5f61aa | `rnd(size)`, then up to 16 `rnd(size)` redraws | `region_mode ≠ 0` and region size > 5; redraws while the cell is impassable | 1–17 |
| T2 | `Unit::think_scout` @5f6441 | `rnd(circle_radius[r])` | per ring r of each scouted city | 1 per ring |
| T3 | `Unit::think_scout` @5f6463 | `rnd(r/4 + frame%8 + 1)` | per ring, when `r/4 + frame%8 ≥ 1` | ≤ 1 per ring |
| T4 | `Unit::think_scout` @5f6657 | `& 7` | per evaluated ring cell (passed fog/goody and `invalid_loc`) | 1 per cell |
| T5 | `Unit::think_scout` @5f694c | `rnd(stride)` | region scan, stride > 1 | ≤ 1 |
| T6 | `Unit::think_scout` @5f6ac5 | `& 7` | per evaluated region cell | 1 per cell |
| G1 | `Unit::explore_goody` @5f99f7 | `rnd(25)` | per good g ∈ {0,1,2,4,5} with `type_avail(g,1)`, `frame ≠ 0` | ≤ 5 |
| — | `Unit::come_out` @61a1bb/@61a1d5 | army-join roll | production-spec §6.4.2 H | 1 |
| — | `Unit::think_fish` @5f4c60 | — | economy-spec | — |

No draws in: `Armies::*`, `Army::process` and the other `Army::*` functions, `Unit::add_to_army`, `think_attack`
(combat-spec), `think_carry*`, `think_civilian_transport`, `action_alarm*`, `check_explore`, `check_transport`,
`do_explore_to`. Indirect draws happen through what these functions call: group orders (`action_move_to` →
`action_move_near` → `PathFinder` paths, movement-spec §6–7), `action_attack`, `find_goody_box`, and every order the
units later execute. `Armies::send_navy` and `emergency`/`diplo_change` run other armies' `process` (and therefore
`find_target`) inside the caller.

### 12.9 Iteration-order dependencies

1. Players 0..7 in `process_all`; armies 0..15 per player (slots are reused: `init_army` takes the first free slot,
   then the smallest army, later slot on ties).
2. Within an army: groups in `list` order, which `normalize` re-sorts by unit category every time it runs (it runs at
   the start of every main `process`, in `is_engaged`, `add_to_army`, `find_target`'s EASIEST path). Formation
   positions (`P` stepping) and the order of `action_*` calls (each may `push_group` and path) follow it.
3. Units: `get_unit` order (groups in list order, members in group order) for COG, `is_engaged`, `is_moving`,
   `engagement` (first map-unit target wins); merge transfers back to front.
4. `find_target`: passes 0..1, leaders 0..7, cities `0..city_mark−1`, then that leader's forts `0..fort_mark−1`;
   `≥` keeps the last best; the RNG stream advances per candidate, so candidate filtering order is part of the
   contract.
5. `find_muster_spot`: the spiral order of `circle_x/y`, armies 0..15 for the proximity test, `move[k]` order.
6. Players searched by `ObjectsData::find_city`/`find_unit`/`find_building` in index order (ties → later/first as
   documented there).
7. Other armies' `process` can run in the middle of an army's `find_target` (`send_navy`) and in the middle of
   damage handling (`emergency`, combat-spec §5.3); `Army::charge` changes targets from `fight`/`do_damage` during
   `Objects::process_all`.

### 12.10 Port plan

1. **Data**: `Army`/`Armies` with the exact 0x98-byte `ArmyData` layout for `walk_data` (2-byte `valid`, then
   [2, 0x98) when valid) and `ArmiesData::find_dist`; 16 slots per player from `Armies::init`. Hook into the
   save/checksum walk (state-walk order: `Armies::walk_data` in `WalkDataGame`, `find_dist` in the globals block).
2. **Helpers**: `Army::count`, `normalize` (with the early-return quirk), `remove_group`, `add_group`, `add_unit`,
   `get_unit`, `member`, `center_of_gravity`, `is_engaged`, `is_moving`, `set_stance`, `close`, `stop`,
   `send_here`, `charge`; `Group::get_num_cap` (mutating) and `Group::action_siege_attack_to` (new); `Unit::go_to_unit`;
   `Region::go_here`; confirm `ObjectsData::find_city`, `find_unit`, `find_building`, `find_capital`,
   `is_tribute_period`, `Game::wonder_winning`, `CityData::num_wonders` exist with the documented tie rules.
3. **Armies helpers** (12.3 table) and `Unit::add_to_army`; replace the stubs in `engine/sim/queues.cpp`
   (`come_out` join), `engine/sim/combat_target.cpp` (`think_attack`), `engine/sim/combat_damage.cpp`
   (`emergency`, `charge`), `engine/sim/game.cpp` step 12 (`process_all`).
4. **State machine**: `process` with the 128/256-frame cadence, `do_mustering` + `release_mustering`,
   `do_forming`, `do_marching`, `march_to_target`, `engagement`, `do_defending` + `find_besieged_city` (with the
   city_mark bug), `do_transporting`.
5. **`find_muster_spot`** (return-value quirk) then **`find_target`** (F1–F3); unit-test the scoring with synthetic
   leaders/cities against hand-computed integer results (truncation order).
6. **Per-unit AI** in `Unit::think` after the combat part: dispatch order of 12.6, `think_spellcaster` (S1, S2),
   `use_*`, `think_scout` (T1–T6), the transport family, `explore_goody` (G1), `check_transport`.
7. `action_alarm` / `action_alarm_peasant` (shared with the human ALARM command).
8. Oracle: a replay with an AI opponent (none on this Mac yet, STATUS); until then, a debug log of
   `(frame, who, army, status, target, muster)` after every `Army::process` and of every RNG draw with its call site,
   compared against a Windows run with a breakpoint log on the RNG sites of 12.8.

### 12.11 Open questions

1. ❓ `LeaderData +0x94c` is named `sea_combat` in the PDB, but `find_target` averages it as "strength" for the
   overseas test; the census (§4.2) increments a field there for sea combat units (a count, not a strength).
   Confirm which census field lands at +0x94c.
2. ❓ `win` uses the army owner's own `wonderwin_timer`/`popwin_timer` (literal reading of the asm); check whether
   these timers live on the winning or the losing leader.
3. ❓ `leader_flags & 8` (PDB: LEADER_COOP_SOLO) stops `find_target`, `release_mustering` and part of `Unit::think`;
   scenario-objects-spec maps a different file bit to COOP_SOLO. Which leaders have it in practice?
4. ❓ `normalize`'s group sort: exact tie behaviour and whether entries with `num == 0` are skipped as both pivot and
   comparand (decompile only).
5. ❓ `rally_dist` (+0x44) and `angle` (+0x40) are written but no reader was found (walked state either way).
6. ❓ `LandData` offset base inside `Land` (the muster score reads `Land +0x100`; taken as `move_rate`).
7. ❓ `Group::action_guard` semantics for the siege escort (orders-spec has no entry).
8. ❓ `as_build(farm) +0x62` bit set by the EASIEST token raid; `guys[0] +0x84` in `think_spellcaster`.
9. ⚠ Out-of-range city reads in `find_besieged_city` (12.4.2): the port needs the same backing storage or proof that
   the indices stay below each list's capacity.
10. `Unit::set_group` does not remove a unit from its previous group; in the merge path `close(A)` then halts the old
    groups that still list the moved units (until `Group::normalize` drops them): confirm in a trace.
11. `think_scout` part (B) loop control and `think_carry_civilian`/`think_civilian_transport` are summarised, not
    fully specified.

## 13. Every game_random draw in the AI

All draws are `Random::get(game_random, 0, 0xffff)` taken modulo n by the caller (`rnd(n)`), on the single simulation
stream. Order = the call order below inside one `Game::do_frame`. `internal_random` draws in `Leader::diplomacy`
(chat line choice, `get(0,7..9)`) are presentation and must not touch `game_random`.

### 13.1 Load time (Leader::init: random maps in start-slot order after map generation; scenarios in leader-chunk order before the terrain reseed, §2.1)
| # | Function | Expression | Condition |
|---|---|---|---|
| L1 | `Leader::random_personality` @6cfd00 | `rush = rnd(3) − 1` | every AI leader |
| L2 | same | `rnd % 5` / `% 3` / `% 3` | tribe groups, only if rush == 1 / == 1 / < 0 (§2.3 step 2) |
| L3 | same | `& 3` or `& 1` per opponent | opponent tribe and current rush (§2.3 step 3), j = 0..7 |
| L4 | same | `% 3` | rush == 1 and sea_map > 2 |
| L5 | same | 15 unconditional `rnd(3)` + 1 coin + `rnd(5)` or `rnd(3)` | cities, coin, arms, army, army_size, raid, invade, target, strategy, spells, forts, nukes, air, naval, market, scouts, civilians, friendly_human, alliance_human |
| L6 | same | `% 3` | tribe 17/0 (raid), after the raid draw |
| L7 | `Leader::init` @6e3930 (@6e4eb6 region) | coin | after the player-flag overrides, only if `rush == 0` (script choice) |
Exact sequence in §2.3 (L5/L6 interleave: cities, coin, arms, army, army_size, raid, [L6], invade, target, strategy,
spells, forts, nukes, air, naval, market, scouts, civilians, friendly_human, alliance_human).

### 13.2 Per frame, Leaders::strategy_all (leaders 0..7)
| # | Function | Expression | When |
|---|---|---|---|
| S1 | `compute_sites` @6cc950 | `rnd(world.xs)` then `rnd(step/4)` | full pass end, per region 0..63 with own citizens and size > 199 (2 draws) |
| S2 | `research_techs` @6c6ba0 | coin | per government tech reaching the SENATE branch with `pers.raid == 0` |
| S3 | same | `rnd(100)` | per government tech of the allowed family (after S2) |
| S4 | `upgrade_units` @6c6430 | `rnd(3)` | per evaluated upgrade candidate when `get_diff() ≠ 2` |
| S5 | `create_units` @6c40a0 | `rnd(3)` (`u = (r + 4)·u/5`) | once per evaluated combat type (any domain) that passed the gates, rush_rules and pacing, when `get_diff() ≠ 2`; drawn before the domain branches (§6.5.3) |
| S6 | `create_buildings` @6c1be0 | `rnd(1000)` then `rnd(300)` (first draw `% 1000`, second `% 300`) | per city and wonder type 526..542 that reaches the random wonder value (§6.6.4) |
| S7 | `make_stuff` @6c8af0 | `rnd(3)` per make-list slot whose type equals item 0's | every make_stuff call with a valid item 0 |
| S8 | same | `rnd(3)` per slot j ≥ i with the bought item's type | after each lower-item purchase i |
| S9 | `use_market` @6c91c0 (from make_stuff, and `place_city_with_cost` → found_cities → make_stuff) | `rnd(6)` | per selling round for a good below its make-list need (rounds continue while the previous one traded) |
| S10 | `produce_building` @6e1400 | `rnd(500)` | per FARM/MINE candidate cell without friends that reaches scoring |
| S11 | same | `rnd(100)` | per unblocked fine-position offset (2×2) for non-dock, non-woodcutter types |
| S12 | `Leader::diplomacy` @6bc950 | D1-D9 (coins, `rnd(2/3/4)`, `rnd(6)` + `rnd(n)` reservoirs) | per evaluated opponent, in the order of §10.1.10 |
| S13 | everything reached through the AI's group orders | pathfinding, `action_swarm_around`, `Build::queue_up`, `init_build` (find_gather_tiles) | production-spec / movement-spec draws |

Script runs (production state 1) can draw through `rand_int` / `rand_real` (script-spec §7).

### 13.3 Armies, units (Armies::process_all, Objects::process_all)
See §12.8 (F1-F3 `Army::find_target`, S1-S2 `think_spellcaster`, T1-T6 `think_scout`, G1 `explore_goody`), plus
`Unit::come_out`'s army-join roll (production-spec §6.4.2 H) and `think_fish` (economy-spec §4.7). `Armies::*` and the
other `Army::*` functions draw nothing directly; `send_navy`, `emergency` and `diplo_change` run other armies'
`process` (and so `find_target`) inside their caller.

## 14. Data read by the AI
- Rules: TypeData costs (`get_cost`), `can_pay_cost`, `where` (+0x40), `from`/`to` lines, `age`, `build_flags`
  (0x10 'e', 0x40 gatherer, 0x200 one per city, 0x800 large city), `x_size`/`y_size`, `domain`, `attack`,
  UnitType `role` (+0x2c8: 0x10000 combat, 0x200 …), `unit_flags` (+0x2b4), `control_cost` (+0x2f0), `carry`
  (+0x2d4), the per-type `vs` table (+0x2b0), TechType `cat`, `age`, `ai[11]` (rules-spec §9.3,
  `compute_ai_values`), GoodType rare flag (vslot 0x94).
- Constants: `city_center_radius`, `city_center_pop_radius`, `indians_city_radius`, `bantu_city_limit`,
  `pyramids_city_limit`, `commerce_cap`, `timer_refresh_ratio` (rules-spec §12.6).
- Setup (GameInfo): difficulty, starting_resources (7/8 special), starting_technology, ending_technology,
  rush_rules (8 = no rush), team_style, reveal_map, victory, elimination, map_style, game_rules; player flags
  0x1000 / 0x2000 (rusher / boomer); `seed` (diplomacy uses `seed % k`, not the RNG).
- Scripts: `ai/scripts/economic.bhs`, `defensive.bhs` (+ `aibestbuildlibrary.bhs`), script-spec §6.4.
- No AI personality or difficulty XML exists in the install: `Data/*.xml` has no AI tables (all constants above are
  compiled in).

## 15. Port plan (ordered: what makes the AI visibly play first)

Prerequisites already in `engine/sim`: production (queue_up, init_build, placement, construction, cities), economy
(gather, auto-gather, markets), scripts (production state 1), combat and movement. Each step below should keep
`ron-sim` digests deterministic and be checked against the decompile; an AI replay from the Windows PC is the oracle.

1. **Setup draws** (§2): `random_personality`, the player-flag overrides, the script coin — for scenarios inside the
   leader-chunk load (file order, before the terrain chunk's reseed: values only), for random maps in start-slot
   order in `build_game` after map generation (part of the simulation stream). Even before any AI behaviour exists this fixes the RNG stream offset of
   every game with AI seats. Save/load the `Personality` block (already walked raw).
2. **Scheduling** (§3): `plan_strategy`'s cadence, the 30-frame tech retry, `production_ai` states 0-11 with the
   exact state transitions and the `starting_resources == 8` variant; `effective_pop`.
3. **Census** (§4.1-§4.4): all counters and region arrays (they are also read by scripts and by the score), city
   evaluation (`land`, `filled`, `space[]`, `ter[]`, `dock_tile`), region `strategy[]` bits, army creation calls
   (stub `Armies::init_army` until step 9 but keep the call order).
4. **Make list + make_stuff + make_this + produce_* (§6.0, §6.7, §6.9)** with `use_market` (§6.8): the purchase
   pipeline, including the `rnd(3)` duplicate removal and `rnd(6)` market draws.
5. **production_ai_setup** (§6.1: bucket clamp, `econ[]` bits, market_speculation) and **found_cities** with
   **sites** (§5, §6.2): the AI expands.
6. **produce_building placement** (§7) with `find_friends`, `check_building_wcoord`, the ring scan, builder choice and
   the `rnd(500)` / `rnd(100)` draws; `check_orphaned_buildings` (§4.6). At this point the AI builds a town.
7. **create_buildings** (§6.6) and **create_units** (§6.5): the scoring that fills the make list with buildings and
   units (citizens first: with steps 4-7 the AI gathers, builds and trains). Then **research_techs** (§6.3) and
   **upgrade_units** (§6.4): ages and libraries.
8. Script builtins that end in AI code: `place_*_with_cost` → `produce_building` / `found_cities` (now real).
9. **Armies** (§12): `Armies::init_army` / `find_army` / `add_to_army` from production, `Armies::process_all`,
   `Army::process` states, `find_target` with its draws, march / attack / retreat, `Group::action_alarm` defence, then
   naval / air / transports. The AI attacks.
10. **Per-unit AI** (§12): `Unit::think` branches for AI_CONTROL units (`think_scout`, `think_attack`,
    `think_civilian_transport`, `think_carry*`, `think_spellcaster`, `explore_goody`), AI repair.
11. **Diplomacy** (§10): `Leader::diplomacy` (war declarations under rush rules, peace / alliance / tribute logic,
    answers to human proposals; port the quirks of §10.1 literally; `hire_who` starts at −1, see the H11 correction),
    ally requests (`process_taunt`), `consider_tribute`.
12. Validation: replays with AI opponents (skirmish on several difficulties, a scenario with scripted AI), compare
    per-turn checksums / sync logs; add a JS reference for the census + sites + placement if a divergence needs
    bisecting.

### Port notes (2026-10-13, engine/sim/ai_*.cpp)

Corrections and details found while porting steps 1-8 (evidence in the decompiles named in sim-implementation.md):
- `min_other_team_terr` (4.1 step 4): `min = (min == 0) ? v : min(v, min)` — a 0 minimum is overwritten by the next
  leader's value, not only the first.
- `compute_site_stats` (5.2): the 25-cell loop moves `(x, y)` to the best cell as it goes, and everything after it
  (ocean bonus excepted: it uses the original cell) is evaluated at the moved cell; the 2×2 value is
  `(val + val(x+1,y) + val(x+1,y+1) + val(x,y+1)) >> 2` only when (x+1, y+1) is on the map; the `dist` output is the
  local that the capital loop (step 11) overwrites with the last `vector_dist / num_nations` term (original quirk).
- `produce_city` (6.9): distances are in wcoords (+24 when gathering); the census fix-up tests the order kind of the
  **last** citizen examined in the builder loop, not the builder's (original quirk).
- `make_this` (6.9): the building-room test reads `space[clamp(max(xs, ys) − 2, 0, 3)]`; index 3 reads `ter[0]`.
- `CityData::get_building` needs the building VALID, a building and active; `City::count_gather_slots` sums every
  gather building of the chain (no active test).
- `village_num` / `village_mine` are only written by Leader::init (0): `cities + villages` is `city_num`.
- `home_reg` (+0x9c8) is written only by `Setup::build_cities` and `ConquestGame::build_cities` (InsnGrep): 0 in
  scenarios.
- FilterIndex values: COMBAT 9, TARGET 11, DOMAIN 13 (rules_enums); `Search::valid_filter`'s table index is
  filter − 1. SearchIndexBH: FRIENDLY 1, ENEMY 3. ObjectsData::find_building measures tcoord distances in both
  modes; find_unit / find_city measure world units.
- The production scripts (state 1) block the C++ steps while they return BLOCK_ON_THIS; with the placement builtins
  ported they place buildings and cities themselves (tut5's script founds the second city).

Round 2 (steps 9-11, armies / danger / per-unit AI / navy and air creators / diplomacy / score):
- `create_units` navy (6.5.4): `Armies::find_army` and `ObjectsData::find_city` are **both** called before the
  `find_army < 0 and city ≥ 0` test (find_city's `find_dist` / `last_who` writes happen either way); find_city's
  flags argument is 0 (no region filter).
- `Leader::eject_my_shit_from_his_ass(t)` @6d0220 (from set_diplo when an alliance ends): every unit of who whose
  outermost container (ObjectData::get_inside) belongs to t calls `come_out(0)`; when come_out fails (returns ≠ 0)
  the unit **dies** (vslot 0x158 `die(0, −1, 0)`); when it succeeds an aircraft (domain 2) that is not `is(310)` gets
  `add_strafe_order`.
- `LeaderData::is_team(b, p)` @6ebd30: self → 1; team style 7 and either seat neutral (team 8) → 0; after frame 0
  with p == 0 → `is_ally`; else same lobby team 0..3, and after frame 0 with p ≠ 0 in unlocked styles (0, 8, 11)
  → `is_ally`. `num_team_members(p)` counts self plus VALID (and ACTIVE when p) team members; `num_allies` counts VA
  leaders allied both ways (self excluded).
- `action_declare` (10.1.9): the war-on-an-ally path sets `d = 1` and also clears `force` before looping; afford_dow
  answers "affordable" when the two are already at war (either side's diplos 0) or, for d ≠ 0, not allied both ways.
  `pay_dow` debits bucket and escrow (floored at 0) by the DECLARE_WAR (668) / CANCEL_ALLIANCE (676) + t cost.
- `action_respond` (r = 1) execution order: goods (B → A then A → B per good, each followed by consider_tribute on
  the receiver), then the treaty (`set_diplo` both ways with `max(treaty, current)`, made_peace stamps for t == 1,
  the neutral propagation when either is neutral: a VALID k on B's team but not A's raises `k.diplos[A]` to
  `B.diplos[A]`, and symmetrically), then (war_allowed) the hire stamps / declarations per attacked k, then both
  clear_all. The alliance rule (`1 − num_team_members(1) + num_allies() == 0` for both) is checked before the goods.
- Score (resolves open question 8): `Leader::compute_score(p)` @6ec560 runs when frame 0, p ≠ 0, semaphore byte0 &
  0x40 or `(who + frame) % 10 == 0`: explored := 0; unit score when flags & 0x1800000 (then clear 0x800000);
  building and economy scores; pop := 0; unit-upgrade and research scores when flags & NEW_TECH 0x1000000 (then clear
  it); territory = `territory · 1000 / world.land_size`; if `get_armageddon() ≤ game.armageddon` **every** part
  including the combat score (+0x44) and the total are zeroed; else score (+0x18) = combat + wonders + research +
  upgrades + economy + buildings + units + territory + pop + explored. Parts (Leader +0x1c..+0x44): explored, territory,
  units, attack units (+0x28, units with attack ≠ 0; not in the total), buildings, economy, pop, upgrades, research,
  wonders. Each part is 0 at Armageddon. `get_score_value(p)` (TypeData vslot 0x7c @6673b0) = the six costs summed
  (militia 66-68 count as type 50 when p == 0) × unit / build / spell / tech cost factor by kind; for units with p ≠ 0
  also `× research_premium / 256 × research_premium_cost / 256`. Units 50..401: `n = num_queued + num_units`, `v =
  ((n − 1)·(support_cost[0] + support_cost[1])·n/2 + value(0)·n)/5`. Buildings 414..542: `n = num_buildings +
  num_queued`, `v = value(0)·n + (n − 1)·(support costs)·build_support_factor·n/2`, wonders (526..542) add `v/3` to
  the wonder part, others `v/5`. Economy: per available good `clamp((bucket − Game.starting[g])/20, 0, 100) +
  min(200, income[g]/80)`, plus 25 per owned rare (`rare` BitMask). Research: techs 544..628 held or researching
  (exact 0), not in `tech_at_start`: `value(0)/5`. Upgrades: units 50..401 held or researching (exact 1), not at start,
  not unit_flags 0x80: `value(1)/5`. `Game::get_armageddon` @594020 = `per_team·num_sides + per_nation·num_nations +
  armageddon`, ×3 for starting resources 7, ×2 for 5, 6, 11, 12, at least 100 for 8.

## 16. Open questions
1. `semaphore byte1 & 2`: which game mode sets it (production scheduling and diplomacy are off under it, and
   `strategy_all` runs `check_victory`). No direct `OR byte [..+1],2` writer exists in the binary.
2. The start-slot table written by `build_game` (GameInfo +0x618 as decompiled) versus `Game.start_list` /
   `start_index` read by `get_target`: confirm they are the same permutation.
3. `starting_resources == 8` and `starting_resources == 7 && starting_technology == 8`: which setup options these are
   (the AI special-cases both).
4. `build_masks & 1` (PATHING) as the orphan re-check flag in `check_orphaned_buildings`: who sets it.
5. `citytemplates.xml` is not referenced by any AI function decoded here; find its reader (probably start-city setup).
6. `research_techs`' and `upgrade_units`' time factors are constant because `tech_frame ≤ frame` (a mis-signed ramp
   in the original); check a save where `tech_frame` could exceed `frame` (after a load?) before simplifying.
7. The `compute_site_stats` early return for multi-region leaders (§5.2 step 10) is original behaviour; confirm in a
   replay that the AI still founds cities afterwards (sites in regions without its cities).
8. ~~compute_score~~: specified in the port notes above. Remaining: the team-game bit of `get_team_score`
   (semaphore byte0 bit 7, no writer found) and `Game.num_sides` (Game::init_teams).
9. Diplomacy open points: §10.1.12 (who sets `treaties` bit 4, `agree == 2` writers, `tribe = 100` side effect).
10. Military open points: §12.11.
